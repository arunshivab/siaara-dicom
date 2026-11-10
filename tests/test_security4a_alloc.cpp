// Security review 2026-11-11, R-123: the named data paths answer size queries without building
// the result, decode straight into the caller's buffer, and never let std::bad_alloc escape.
// Uses the allocation guard (alloc_guard.hpp), so this file is left out of TSan builds.
#include <cstdint>
#include <cstring>
#include <vector>

#include "alloc_guard.hpp"
#include "core/dataset.hpp"
#include "modules/data.hpp"
#include "modules/modules.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr std::size_t kSmall = 256 * 1024;  // far below any of the results (8 MB each)
constexpr std::uint32_t kSamples = 1'000'000;

const siaara_dataset* handle(const Dataset& ds) { return reinterpret_cast<const siaara_dataset*>(&ds); }

Dataset big_waveform() {
    Dataset ds;
    Element seq;
    seq.tag = 0x54000100;
    seq.vr = VR::SQ;
    Dataset& g = ds.put(std::move(seq)).items.emplace_back();
    (void)g.set_uint16(0x003A0005, 1);
    (void)g.set_uint32(0x003A0010, kSamples);
    (void)g.set_uint16(0x54001004, 16);
    (void)g.set_string(0x54001006, VR::CS, "SS");
    Element ch;
    ch.tag = 0x003A0200;
    ch.vr = VR::SQ;
    Dataset& c = g.put(std::move(ch)).items.emplace_back();
    (void)c.set_string(0x003A0210, VR::DS, "0.5");
    std::vector<std::uint8_t> data(std::size_t{kSamples} * 2, 0x02);
    (void)g.set_bytes(0x54001010, VR::OW, Bytes::copy_of(data.data(), data.size()));
    return ds;
}

}  // namespace

TEST_CASE("sec4a R-123: size queries and buffer fills of named data allocate nothing of the result's size") {
    Dataset wf = big_waveform();
    Dataset num;
    {
        std::vector<double> fd(kSamples, 0.25);
        (void)num.set_bytes(0x00189313, VR::FD, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(fd.data()), fd.size() * 8));
    }
    std::vector<int64_t> raw(kSamples);
    std::vector<double> dbl(kSamples);
    const char* path = "(0018,9313)";
    {
        siaara_test::AllocGuard guard(kSmall);
        size_t count = 0;
        // Size queries: answered from the value count.
        REQUIRE(siaara_waveform_raw_samples(handle(wf), 0, 0, nullptr, 0, &count) == SIAARA_OK);
        CHECK(count == kSamples);
        REQUIRE(siaara_waveform_samples(handle(wf), 0, 0, 1, nullptr, 0, &count) == SIAARA_OK);
        CHECK(count == kSamples);
        REQUIRE(siaara_waveform_samples(handle(wf), 0, 0, 0, nullptr, 0, &count) == SIAARA_OK);
        CHECK(count == kSamples);
        REQUIRE(siaara_get_doubles_by_path(handle(num), path, std::strlen(path), nullptr, 0, &count) == SIAARA_OK);
        CHECK(count == kSamples);
        // Fills: decoded into the caller's buffer, no intermediate vector.
        REQUIRE(siaara_waveform_raw_samples(handle(wf), 0, 0, raw.data(), raw.size(), &count) == SIAARA_OK);
        REQUIRE(siaara_waveform_samples(handle(wf), 0, 0, 1, dbl.data(), dbl.size(), &count) == SIAARA_OK);
        CHECK(dbl[kSamples - 1] == 0x0202 * 0.5);
        REQUIRE(siaara_waveform_samples(handle(wf), 0, 0, 0, dbl.data(), dbl.size(), &count) == SIAARA_OK);
        CHECK(dbl[kSamples - 1] == 0x0202);
        REQUIRE(siaara_get_doubles_by_path(handle(num), path, std::strlen(path), dbl.data(), dbl.size(), &count) == SIAARA_OK);
        CHECK(dbl[kSamples - 1] == 0.25);
        CHECK(!guard.refused());
        CHECK(guard.peak() < kSmall);
    }
    CHECK(raw[kSamples - 1] == 0x0202);
}

TEST_CASE("sec4a R-123: the C++ vector forms report OutOfMemory instead of throwing std::bad_alloc") {
    Dataset wf = big_waveform();
    Dataset num;
    {
        std::vector<double> fd(kSamples, 0.25);
        (void)num.set_bytes(0x00189313, VR::FD, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(fd.data()), fd.size() * 8));
    }
    Errc raw = Errc::Ok, scaled = Errc::Ok, path = Errc::Ok;
    bool refused = false;
    {
        siaara_test::AllocGuard guard(kSmall);
        // Each result needs 8 MB: the guard refuses it. Before the fix this escaped as an exception.
        auto r = modules::waveform_raw(wf, 0, 0);
        raw = r.is_ok() ? Errc::Ok : r.error().code;
        auto s = modules::waveform_scaled(wf, 0, 0);
        scaled = s.is_ok() ? Errc::Ok : s.error().code;
        auto p = modules::doubles_by_path(num, "(0018,9313)");
        path = p.is_ok() ? Errc::Ok : p.error().code;
        refused = guard.refused();
    }
    CHECK(refused);
    CHECK(raw == Errc::OutOfMemory);
    CHECK(scaled == Errc::OutOfMemory);
    CHECK(path == Errc::OutOfMemory);
}
