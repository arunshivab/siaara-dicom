// Heap bounds of the grouping and of the DICOMDIR reader from memory, measured with the allocation
// guard (alloc_guard.hpp): a 10,000-file grouping stays within its budgets, a grouping whose text
// budget is reached stops growing, and a DICOMDIR at max_records is read within a fixed heap.
// Uses the allocation guard, so this file is left out of TSan builds.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "alloc_guard.hpp"
#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "grouping_synth.hpp"
#include "siaara/siaara.h"
#include "store/index.hpp"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr std::size_t kMiB = std::size_t{1} << 20;

// One CT file whose SOP Instance UID ends in a fixed-width counter, and where that counter is.
struct Template {
    std::vector<std::uint8_t> bytes;
    std::size_t counter_at = 0;
};

Template make_template() {
    store_synth::Spec s;
    s.sop_uid = "1.2.826.0.1.8.100000";  // a counter of 6 digits that never starts with 0 (a strict UID)
    s.series_desc = "Axial 2.5 mm";
    const std::vector<std::uint8_t> b = grouping_synth::bytes_of(store_synth::make_file(s));
    Template t;
    t.bytes = b;
    const std::string needle = s.sop_uid;
    for (std::size_t i = 0; i + needle.size() <= b.size(); ++i)
        if (std::memcmp(b.data() + i, needle.data(), needle.size()) == 0) {
            t.counter_at = i + needle.size() - 6;
            break;
        }
    return t;
}

void set_counter(Template& t, unsigned n) {
    char d[7];
    std::snprintf(d, sizeof d, "1%05u", n % 100000u);
    std::memcpy(t.bytes.data() + t.counter_at, d, 6);
}

}  // namespace

TEST_CASE("grouping heap: 10,000 files stay within the budgets; a reached text budget stops growth") {
    Template t = make_template();
    REQUIRE(t.counter_at > 0);
    {
        siaara_test::AllocGuard guard(48 * kMiB);
        grouping_synth::Grouping g;
        for (unsigned i = 0; i < 10000; ++i) {
            set_counter(t, i);
            CHECK_EQ(g.add("zip/CT/IM" + std::to_string(i), t.bytes), SIAARA_OK);
        }
        // one more: past max_files
        set_counter(t, 10000);
        CHECK_EQ(g.add("zip/CT/IM10000", t.bytes), SIAARA_OK);
        const siaara_grouping_summary s = g.result();
        CHECK_EQ(g.last(), SIAARA_OK);
        CHECK_EQ(s.instances, 10000u);
        CHECK_EQ(s.files, 10001u);
        CHECK_EQ(s.skipped, 1u);
        CHECK_EQ(s.budget_reached, static_cast<uint32_t>(SIAARA_GLIMIT_MAX_FILES));
        CHECK(s.result_bytes <= SIAARA_GROUPING_DEFAULT_MAX_RESULT_BYTES);
        CHECK(s.total_bytes <= SIAARA_GROUPING_DEFAULT_MAX_TOTAL_BYTES);
        CHECK_EQ(s.series, 1u);
        CHECK(!guard.refused());
        std::printf("  10,000 files: heap peak %.1f MiB, text kept %.1f MiB\n", static_cast<double>(guard.peak()) / kMiB,
                    static_cast<double>(s.result_bytes) / kMiB);
    }
    {
        siaara_grouping_options o = grouping_synth::options();
        o.max_result_bytes = 256 * 1024;
        siaara_test::AllocGuard guard(8 * kMiB);
        grouping_synth::Grouping g(&o);
        for (unsigned i = 0; i < 10000; ++i) {
            set_counter(t, i);
            CHECK_EQ(g.add("zip/CT/IM" + std::to_string(i), t.bytes), SIAARA_OK);
        }
        const siaara_grouping_summary s = g.result();
        CHECK(s.result_bytes <= o.max_result_bytes);
        CHECK_EQ(s.budget_reached, static_cast<uint32_t>(SIAARA_GLIMIT_RESULT_BYTES));
        CHECK(s.instances < 10000u);
        CHECK_EQ(s.instances + s.skipped, 10000u);
        CHECK(!guard.refused());
        std::printf("  text budget reached after %llu files: heap peak %.1f MiB\n", static_cast<unsigned long long>(s.instances),
                    static_cast<double>(guard.peak()) / kMiB);
    }
}

namespace {

// A DICOMDIR of `n` IMAGE records in sequence order (no offsets: read in sequence order).
std::vector<std::uint8_t> dicomdir_of(std::size_t n) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, store::kMediaStorageDirectoryUid);
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, "1.2.3.4.5");
    (void)f.dataset.set_uint32(0x00041200, 0);
    Element seq;
    seq.tag = 0x00041220;
    seq.vr = VR::SQ;
    Dataset im;
    (void)im.set_uint32(0x00041400, 0);
    (void)im.set_uint16(0x00041410, 0xFFFF);
    (void)im.set_uint32(0x00041420, 0);
    (void)im.set_string(0x00041430, VR::CS, "IMAGE");
    (void)im.set_string(0x00041500, VR::CS, "DICOM\\IM");
    for (std::size_t i = 0; i < n; ++i) seq.items.push_back(im);
    f.dataset.put(std::move(seq));
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return out;
}

int32_t SIAARA_CALL count_records(void* user, const siaara_dicomdir_entry*, const siaara_index_record*, const char*) {
    ++*static_cast<std::size_t*>(user);
    return 0;
}

}  // namespace

TEST_CASE("grouping heap: a DICOMDIR at max_records is read within a fixed heap; past it is refused") {
    constexpr std::size_t kRecords = 20000;  // 6 elements each: within the untrusted 200,000-element limit
    const std::vector<std::uint8_t> dd = dicomdir_of(kRecords);
    siaara_dicomdir_read_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.max_records = kRecords;
    {
        siaara_test::AllocGuard guard(64 * kMiB);
        std::size_t n = 0;
        CHECK_EQ(siaara_dicomdir_read_memory(dd.data(), dd.size(), &o, count_records, &n, nullptr), SIAARA_OK);
        CHECK_EQ(n, kRecords);
        CHECK(!guard.refused());
        std::printf("  DICOMDIR of %zu records: heap peak %.1f MiB\n", kRecords, static_cast<double>(guard.peak()) / kMiB);
    }
    o.max_records = kRecords - 1;
    std::size_t n = 0;
    CHECK_EQ(siaara_dicomdir_read_memory(dd.data(), dd.size(), &o, count_records, &n, nullptr), SIAARA_ERR_LIMIT_EXCEEDED);
    CHECK_EQ(n, 0u);  // refused before any record is reported
    // With the default max_records (100,000) the untrusted element limit comes first: 40,000
    // records (240,000 elements) are refused cleanly.
    const std::vector<std::uint8_t> big = dicomdir_of(40000);
    {
        siaara_test::AllocGuard guard(96 * kMiB);
        CHECK_EQ(siaara_dicomdir_read_memory(big.data(), big.size(), nullptr, count_records, &n, nullptr), SIAARA_ERR_LIMIT_EXCEEDED);
        CHECK(!guard.refused());
    }
}
