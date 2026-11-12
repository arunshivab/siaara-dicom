// The DICOMDIR reader from memory (siaara_dicomdir_read_memory): the same records as the path
// reader, the record links (cycles, shared children, depth, links into a record), the record and
// size limits, File IDs as text (well formed or rejected), and the options struct that grew (R-081).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "crosscheck/store_synth.hpp"
#include "dicomdir_synth.hpp"
#include "siaara/siaara.h"
#include "store/dicomdir.hpp"
#include "test.hpp"

using namespace siaara;
using namespace dicomdir_synth;
using store_synth::Spec;
namespace fs = std::filesystem;


TEST_CASE("dicomdir memory: from bytes equals the path reader (same keys, File ID text instead of a path)") {
    store_synth::TempDir td("siaara-ddmem");
    make_media(td / "src", td / "m");
    const fs::path dd = td / "m" / "DICOMDIR";
    const std::vector<std::uint8_t> bytes = store_synth::read_bytes(dd);
    REQUIRE(!bytes.empty());
    std::vector<DirRec> by_path, by_mem;
    siaara_dicomdir_summary sp, sm;
    REQUIRE(read_path(dd, by_path, &sp) == SIAARA_OK);
    REQUIRE(read_mem(bytes, by_mem, &sm) == SIAARA_OK);
    REQUIRE(by_path.size() == by_mem.size());
    REQUIRE(by_path.size() == 2u + 4u + 8u + 16u);
    for (std::size_t i = 0; i < by_path.size(); ++i) {
        const DirRec& a = by_path[i];
        const DirRec& b = by_mem[i];
        CHECK(a.index == b.index && a.depth == b.depth && a.parent == b.parent && a.offset == b.offset);
        CHECK(a.type == b.type && a.patient_id == b.patient_id && a.sop_uid == b.sop_uid);
        CHECK_EQ(b.file_status, SIAARA_OK);
        if (a.type == "IMAGE") {
            // The path reader resolves the File ID; from bytes it is the File ID as text.
            CHECK(b.file_text.rfind("DICOM/P", 0) == 0);
            CHECK(b.file_text.find('\\') == std::string::npos);
            const fs::path resolved = fs::path(a.file_text);
            CHECK(fs::equivalent(resolved, td / "m" / fs::path(b.file_text)));
        } else {
            CHECK(b.file_text.empty());
        }
    }
    CHECK_EQ(sm.records, sp.records);
    CHECK_EQ(sm.files, 16u);
    CHECK_EQ(sm.missing_files, 0u);
    CHECK_EQ(sm.rejected_paths, 0u);
    // The C++ reader: the same, and the File ID components as stored.
    std::size_t n = 0;
    auto r = store::read_dicomdir_memory(bytes.data(), bytes.size(), [&](const store::DicomdirRecord& rec) {
        if (!rec.file_id.empty()) {
            CHECK_EQ(rec.file_id.size(), 5u);
            ++n;
        }
        return true;
    });
    CHECK(r.is_ok());
    CHECK_EQ(n, 16u);
}

TEST_CASE("dicomdir memory: options struct grows (R-081); the path reader keeps its behaviour") {
    store_synth::TempDir td("siaara-ddopt");
    make_media(td / "src", td / "m", 1);
    const fs::path dd = td / "m" / "DICOMDIR";
    const std::vector<std::uint8_t> bytes = store_synth::read_bytes(dd);
    std::vector<DirRec> v;
    // The first published version (20 bytes): accepted by both readers; the new fields keep their defaults.
    siaara_dicomdir_read_options o = dd_options();
    o.struct_size = 20;
    CHECK_EQ(read_path(dd, v, nullptr, &o), SIAARA_OK);
    CHECK_EQ(v.size(), 15u);
    CHECK_EQ(read_mem(bytes, v, nullptr, &o), SIAARA_OK);
    CHECK_EQ(v.size(), 15u);
    o.struct_size = 16;
    CHECK_EQ(read_path(dd, v, nullptr, &o), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(read_mem(bytes, v, nullptr, &o), SIAARA_ERR_INVALID_ARGUMENT);
    // A larger struct_size (a newer caller) is accepted too.
    struct Bigger {
        siaara_dicomdir_read_options o;
        uint64_t later;
    } big;
    std::memset(&big, 0, sizeof big);
    big.o = dd_options();
    big.o.struct_size = sizeof big;
    big.later = 12345;
    CHECK_EQ(read_mem(bytes, v, nullptr, &big.o), SIAARA_OK);
    // Path reader: no record or size limit unless asked (0 = no limit), and with them asked for.
    o = dd_options();
    o.max_records = 14;
    CHECK_EQ(read_path(dd, v, nullptr, &o), SIAARA_ERR_LIMIT_EXCEEDED);
    o.max_records = 15;
    CHECK_EQ(read_path(dd, v, nullptr, &o), SIAARA_OK);
    o.max_records = 0;
    o.max_bytes = bytes.size() - 1;
    CHECK_EQ(read_path(dd, v, nullptr, &o), SIAARA_ERR_LIMIT_EXCEEDED);
    o.max_bytes = bytes.size();
    CHECK_EQ(read_path(dd, v, nullptr, &o), SIAARA_OK);
    // From bytes: the same limits; the defaults when 0.
    o = dd_options();
    o.max_records = 14;
    CHECK_EQ(read_mem(bytes, v, nullptr, &o), SIAARA_ERR_LIMIT_EXCEEDED);
    o.max_records = 15;
    CHECK_EQ(read_mem(bytes, v, nullptr, &o), SIAARA_OK);
    o.max_bytes = bytes.size() - 1;
    CHECK_EQ(read_mem(bytes, v, nullptr, &o), SIAARA_ERR_LIMIT_EXCEEDED);
    static_assert(SIAARA_DICOMDIR_MEMORY_DEFAULT_MAX_RECORDS == store::kDicomdirMemoryDefaultMaxRecords, "default");
    static_assert(SIAARA_DICOMDIR_MEMORY_DEFAULT_MAX_BYTES == store::kDicomdirMemoryDefaultMaxBytes, "default");
    // Not Part 10, empty, NULL.
    const std::vector<std::uint8_t> raw(bytes.begin() + 132, bytes.end());
    CHECK_EQ(read_mem(raw, v), SIAARA_ERR_NOT_DICOM);
    CHECK_EQ(siaara_dicomdir_read_memory(nullptr, 0, nullptr, nullptr, nullptr, nullptr), SIAARA_ERR_NOT_DICOM);
    CHECK_EQ(siaara_dicomdir_read_memory(nullptr, 5, nullptr, nullptr, nullptr, nullptr), SIAARA_ERR_INVALID_ARGUMENT);
}

TEST_CASE("dicomdir memory: cycles, a child shared by two parents, deep nesting, links into a record") {
    store_synth::TempDir td("siaara-ddlinks");
    make_media(td / "src", td / "m");
    const fs::path dd = td / "m" / "DICOMDIR";
    const std::vector<std::uint8_t> orig = store_synth::read_bytes(dd);
    std::vector<DirRec> recs;
    REQUIRE(read_mem(orig, recs) == SIAARA_OK);
    std::vector<const DirRec*> patients, studies;
    for (const auto& r : recs) {
        if (r.type == "PATIENT") patients.push_back(&r);
        if (r.type == "STUDY") studies.push_back(&r);
    }
    REQUIRE(patients.size() == 2);
    // record layout (explicit VR LE): (0004,1400) next at +16, (0004,1420) lower at +38
    CHECK_EQ(load_le32(orig.data() + patients[0]->offset + 8), 0x14000004u);
    std::vector<DirRec> v;
    siaara_dicomdir_summary s;
    {  // a cycle: the first patient's next points at itself
        auto b = orig;
        patch32(b, patients[0]->offset + 16, static_cast<std::uint32_t>(patients[0]->offset));
        CHECK_EQ(read_mem(b, v, &s), SIAARA_OK);
        CHECK_EQ(std::count_if(v.begin(), v.end(), [](const DirRec& r) { return r.type == "PATIENT"; }), 1);
        CHECK(s.warnings > 0);
        CHECK(s.unreachable > 0);
    }
    {  // a child shared by two parents: the second patient's lower points at the first patient's study
        auto b = orig;
        patch32(b, patients[1]->offset + 38, static_cast<std::uint32_t>(studies[0]->offset));
        CHECK_EQ(read_mem(b, v, &s), SIAARA_OK);
        std::set<uint32_t> seen;
        for (const auto& r : v) CHECK(seen.insert(r.index).second);  // nothing visited twice
        CHECK(s.warnings > 0);
    }
    {  // a link into the middle of a record is refused
        auto b = orig;
        patch32(b, patients[1]->offset + 38, static_cast<std::uint32_t>(studies[2]->offset + 2));
        CHECK_EQ(read_mem(b, v, &s), SIAARA_OK);
        CHECK(s.unreachable >= 7u);
        CHECK(s.warnings > 0);
    }
    {  // deep nesting: every record's lower link points at the next record in sequence order
        auto b = orig;
        std::vector<std::uint64_t> offs;
        for (const auto& r : recs) offs.push_back(r.offset);
        std::sort(offs.begin(), offs.end());
        for (std::size_t i = 0; i < offs.size(); ++i) {
            patch32(b, offs[i] + 16, 0);
            patch32(b, offs[i] + 38, i + 1 < offs.size() ? static_cast<std::uint32_t>(offs[i + 1]) : 0u);
        }
        siaara_dicomdir_read_options o = dd_options();
        CHECK_EQ(read_mem(b, v, &s, &o), SIAARA_OK);
        CHECK_EQ(v.size(), 16u);  // depth 0 .. 15: the default max_depth (16) cuts the rest
        for (const auto& r : v) CHECK(r.depth < 16u);
        o.max_depth = 4;
        CHECK_EQ(read_mem(b, v, &s, &o), SIAARA_OK);
        CHECK_EQ(v.size(), 4u);
        // every visit counts against max_records
        o.max_depth = 0;
        o.max_records = 10;
        CHECK_EQ(read_mem(b, v, &s, &o), SIAARA_ERR_LIMIT_EXCEEDED);
    }
    {  // the path reader is not changed by the shared walk: the same cycle gives the same result
        auto b = orig;
        patch32(b, patients[0]->offset + 16, static_cast<std::uint32_t>(patients[0]->offset));
        store_synth::write_bytes(dd, b);
        std::vector<DirRec> w;
        CHECK_EQ(read_path(dd, w, &s), SIAARA_OK);
        CHECK_EQ(read_mem(b, v), SIAARA_OK);
        CHECK_EQ(w.size(), v.size());
    }
}

TEST_CASE("dicomdir memory: File IDs from bytes - well formed or rejected, never resolved") {
    // A DICOMDIR built by hand, records in sequence order (root offset 0).
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, store::kMediaStorageDirectoryUid);
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, "1.2.3.4.5");
    (void)f.dataset.set_uint32(0x00041200, 0);
    Element seq;
    seq.tag = 0x00041220;
    seq.vr = VR::SQ;
    const std::vector<std::pair<std::string, bool>> ids = {
        {"DICOM\\IM1", true},          {"A_B\\C9", true},         {"X1\\X2\\X3\\X4\\X5\\X6\\X7\\X8", true},
        {"..\\..\\ETC\\PASSWD", false}, {"\\ABS\\IM", false},      {"C:\\WINDOWS\\X", false},
        {"X1\\X2\\X3\\X4\\X5\\X6\\X7\\X8\\X9", false},             {"dicom\\im1", false},
        {"DICOM\\IM 1", false},        {"DICOM/IM1", false},      {"DICOM\\IM.1", false},
        {std::string("A\0B", 3), false}, {"DICOM\\\\IM", false},  {"DICOM\\ABCDEFGHI", false},
        {"ABCDEFGH\\X", true},
    };
    for (const auto& [id, good] : ids) {
        Dataset im;
        (void)im.set_uint32(0x00041400, 0);
        (void)im.set_uint16(0x00041410, 0xFFFF);
        (void)im.set_uint32(0x00041420, 0);
        (void)im.set_string(0x00041430, VR::CS, "IMAGE");
        (void)im.set_bytes(0x00041500, VR::CS, Bytes::from_string(id.size() % 2 ? id + " " : id));
        seq.items.push_back(im);
        (void)good;
    }
    f.dataset.put(std::move(seq));
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    std::vector<DirRec> v;
    siaara_dicomdir_summary s;
    REQUIRE(read_mem(bytes, v, &s) == SIAARA_OK);
    REQUIRE(v.size() == ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if ((v[i].file_status == SIAARA_OK) != ids[i].second) {
            std::fprintf(stderr, "  File ID %zu\n", i);
            CHECK((v[i].file_status == SIAARA_OK) == ids[i].second);
        }
        if (!ids[i].second) CHECK_EQ(v[i].file_status, SIAARA_ERR_REJECTED);
    }
    CHECK(v[0].file_text == "DICOM/IM1");
    // A rejected File ID's text is the value exactly as stored: '\\' separators, padding kept.
    CHECK(v[3].file_text == "..\\..\\ETC\\PASSWD");
    CHECK(v[7].file_text == "dicom\\im1 ");
    CHECK_EQ(s.files, 4u);
    CHECK_EQ(s.rejected_paths, static_cast<uint32_t>(ids.size() - 4));
    CHECK_EQ(s.missing_files, 0u);
    CHECK(store::file_id_well_formed({"A"}));
    CHECK(!store::file_id_well_formed({}));
    CHECK(!store::file_id_well_formed({""}));
}


TEST_CASE("dicomdir memory: R-222 a value longer than max_value_bytes is left out, never cut; inherited keys are shared, not copied") {
    // A PATIENT record with a 4 MB Patient ID and 3,000 IMAGE records under it (sequence order).
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, store::kMediaStorageDirectoryUid);
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, "1.2.3.4.5");
    (void)f.dataset.set_uint32(0x00041200, 0);
    Element seq;
    seq.tag = 0x00041220;
    seq.vr = VR::SQ;
    auto rec = [](const char* type) {
        Dataset d;
        (void)d.set_uint32(0x00041400, 0);
        (void)d.set_uint16(0x00041410, 0xFFFF);
        (void)d.set_uint32(0x00041420, 0);
        (void)d.set_string(0x00041430, VR::CS, type);
        return d;
    };
    const std::string huge(4u << 20, 'P');
    Dataset p = rec("PATIENT");
    (void)p.set_bytes(0x00100020, VR::LO, Bytes::from_string(huge));
    seq.items.push_back(p);
    constexpr std::size_t kImages = 3000;
    for (std::size_t i = 0; i < kImages; ++i) {
        Dataset im = rec("IMAGE");
        (void)im.set_string(0x00041500, VR::CS, "DICOM\\IM");
        seq.items.push_back(im);
    }
    f.dataset.put(std::move(seq));
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    struct Count {
        std::size_t records = 0, long_id = 0;
    };
    auto on = [](void* user, const siaara_dicomdir_entry*, const siaara_index_record* k, const char*) -> int32_t {
        auto* c = static_cast<Count*>(user);
        ++c->records;
        if (k->str_length[SIAARA_IDX_PATIENT_ID] == (4u << 20)) ++c->long_id;
        return 0;
    };
    siaara_dicomdir_summary sum;
    std::memset(&sum, 0, sizeof sum);
    sum.struct_size = sizeof sum;
    {  // the default cap (64 KiB): the 4 MB value is left out and reported
        Count c;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_EQ(siaara_dicomdir_read_memory(bytes.data(), bytes.size(), nullptr, on, &c, &sum), SIAARA_OK);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK_EQ(c.records, kImages + 1);
        CHECK_EQ(c.long_id, 0u);
        CHECK(sum.warnings >= 1u);
        CHECK(s < 10.0);
        std::printf("  4 MB value, %zu records, default cap: %.2f s\n", kImages + 1, s);
    }
    {  // a cap above it: the value is kept and every record inherits it - by reference, so it is fast
        siaara_dicomdir_read_options o = dd_options();
        o.max_value_bytes = 8u << 20;
        Count c;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_EQ(siaara_dicomdir_read_memory(bytes.data(), bytes.size(), &o, on, &c, &sum), SIAARA_OK);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK_EQ(c.records, kImages + 1);
        CHECK_EQ(c.long_id, kImages + 1);
        CHECK(s < 10.0);
        std::printf("  4 MB value inherited by %zu records: %.2f s\n", kImages, s);
    }
    {  // C++: the keys of a record reference its parent's; the value is never cut
        store::DicomdirReadOptions o;
        o.max_value_bytes = 1024;
        std::size_t n = 0;
        auto r = store::read_dicomdir_memory(bytes.data(), bytes.size(), [&](const store::DicomdirRecord& d) {
            CHECK(d.keys[store::Str::PatientId].empty());
            if (d.parent >= 0) CHECK(d.keys.node->parent() != nullptr);
            ++n;
            return true;
        }, o);
        CHECK(r.is_ok());
        CHECK_EQ(n, kImages + 1);
    }
    // The path reader: no cap by default (its behaviour before), the cap when asked.
    store_synth::TempDir td("siaara-ddcap");
    const fs::path dd = td / "DICOMDIR";
    store_synth::write_bytes(dd, bytes);
    std::size_t kept = 0;
    auto r = store::read_dicomdir(dd.string(), [&](const store::DicomdirRecord& d) {
        if (d.keys[store::Str::PatientId].size() == huge.size()) ++kept;
        return true;
    });
    CHECK(r.is_ok());
    CHECK_EQ(kept, kImages + 1);
}

TEST_CASE("dicomdir memory: testing P3-2 - not a Media Storage Directory is refused; a cut record is dropped; an incomplete read warns") {
    store_synth::TempDir td("siaara-ddp32");
    make_media(td / "src", td / "m", 1);
    const std::vector<std::uint8_t> orig = store_synth::read_bytes(td / "m" / "DICOMDIR");
    std::vector<DirRec> all;
    REQUIRE(read_mem(orig, all) == SIAARA_OK);
    REQUIRE(all.size() == 15u);
    {  // another Media Storage SOP Class
        auto b = orig;
        const std::string cls(store::kMediaStorageDirectoryUid);
        auto it = std::search(b.begin(), b.end(), cls.begin(), cls.end());
        REQUIRE(it != b.end());
        *(it + static_cast<std::ptrdiff_t>(cls.size()) - 2) = '9';  // 1.2.840.10008.1.3.10 -> 1.2.840.10008.1.3.90
        std::vector<DirRec> v;
        CHECK_EQ(read_mem(b, v), SIAARA_ERR_REJECTED);
        CHECK(v.empty());
    }
    {  // cut inside the last record: it is dropped, the rest is read, warnings say so
        std::uint64_t last = 0;
        for (const auto& r : all) last = std::max(last, r.offset);
        std::vector<std::uint8_t> b(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(last + 40));
        std::vector<DirRec> v;
        siaara_dicomdir_summary s;
        CHECK_EQ(read_mem(b, v, &s), SIAARA_OK);
        CHECK(v.size() < all.size());
        for (const auto& r : v) CHECK(r.offset != last);
        CHECK(s.warnings >= 2u);  // the incomplete read, the dropped record
    }
}
