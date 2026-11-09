// Regression tests for the 2026-11-10 02:15 security review, clinical findings (R-054, R-056).
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/seg.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }

// A tiny SEG whose Number of Frames claims `frames`.
Dataset claimed_seg(std::string_view type, std::uint32_t frames, int segments, std::size_t pixel_bytes) {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.66.4");
    put(d, ct::SegmentationType, VR::CS, type);
    (void)d.set_uint16(siaara::tags::Rows, 2);
    (void)d.set_uint16(siaara::tags::Columns, 2);
    (void)d.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)d.set_uint16(siaara::tags::BitsAllocated, 8);
    (void)d.set_uint16(siaara::tags::BitsStored, 8);
    (void)d.set_uint16(siaara::tags::HighBit, 7);
    put(d, siaara::tags::NumberOfFrames, VR::IS, std::to_string(frames));
    std::vector<Dataset> segs;
    for (int i = 1; i <= segments; ++i) {
        Dataset s;
        (void)s.set_uint16(ct::SegmentNumber, static_cast<std::uint16_t>(i));
        put(s, ct::SegmentLabel, VR::LO, "S");
        segs.push_back(std::move(s));
    }
    d.put(make_sequence(ct::SegmentSequence, segs));
    if (pixel_bytes) {
        const std::vector<std::uint8_t> px(pixel_bytes, 1);
        (void)d.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    }
    return d;
}

}  // namespace

TEST_CASE("R-054: LABELMAP claiming 1M frames x 200 segments is limited to the pixel data") {
    const Dataset d = claimed_seg("LABELMAP", 1000000, 200, 8);  // 8 bytes = two 2x2 frames
    const auto t0 = std::chrono::steady_clock::now();
    auto r = read_segmentation(d, true);
    const double secs = seconds_since(t0);
    REQUIRE(r);
    CHECK_EQ(r->number_of_frames, 2u);
    CHECK_EQ(r->declared_number_of_frames, 1000000u);
    CHECK_EQ(r->frames.size(), 2u);
    REQUIRE(r->segments.size() == 200u);
    for (const Segment& s : r->segments) CHECK(s.frames.empty());  // no per-segment lists for LABELMAP
    CHECK_EQ(r->frame_count(r->segments[199]), 2u);
    bool flagged = false;
    for (const auto& w : r->warnings)
        if (w.find("Number of Frames 1000000") != std::string::npos) flagged = true;
    CHECK(flagged);
    CHECK(secs < 2.0);
    // Strict mode refuses the inconsistent claim.
    auto strict = read_segmentation(d, false);
    REQUIRE(!strict);
    CHECK(strict.error().code == Errc::Malformed);
}

TEST_CASE("R-054: BINARY frames are limited to the Per-Frame Functional Groups items") {
    Dataset d = claimed_seg("BINARY", 1u << 24, 2, 0);
    std::vector<Dataset> per;
    for (int f = 0; f < 3; ++f) {
        Dataset si;
        (void)si.set_uint16(ct::ReferencedSegmentNumber, static_cast<std::uint16_t>(1 + f % 2));
        Dataset it;
        it.put(make_sequence(ct::SegmentIdentificationSequence, {si}));
        per.push_back(std::move(it));
    }
    d.put(make_sequence(0x52009230, per));
    const auto t0 = std::chrono::steady_clock::now();
    auto r = read_segmentation(d, true);
    REQUIRE(r);
    CHECK(seconds_since(t0) < 2.0);
    CHECK_EQ(r->number_of_frames, 3u);
    REQUIRE(r->segment(1) && r->segment(2));
    CHECK((r->segment(1)->frames == std::vector<std::uint32_t>{0, 2}));
    CHECK((r->segment(2)->frames == std::vector<std::uint32_t>{1}));
    CHECK(!read_segmentation(d, false));
}

TEST_CASE("R-054: a frame table beyond the budget is LimitExceeded, not allocated") {
    // No per-frame items and no pixel data (read without pixels): only the budget bounds it.
    const Dataset d = claimed_seg("LABELMAP", 1u << 24, 1, 0);
    auto r = read_segmentation(d, true);
    REQUIRE(!r);
    CHECK(r.error().code == Errc::LimitExceeded);
    // A plausible count without per-frame items still reads.
    CHECK(read_segmentation(claimed_seg("LABELMAP", 1000, 1, 0), true));
}

TEST_CASE("R-054: many segments are indexed in constant time") {
    Dataset d = claimed_seg("BINARY", 1, 0, 4);
    std::vector<Dataset> segs;
    for (int i = 1; i <= 65535; ++i) {
        Dataset s;
        (void)s.set_uint16(ct::SegmentNumber, static_cast<std::uint16_t>(i));
        segs.push_back(std::move(s));
    }
    for (int i = 0; i < 20000; ++i) {  // duplicates: each was a linear search
        Dataset s;
        (void)s.set_uint16(ct::SegmentNumber, static_cast<std::uint16_t>(65535 - i));
        segs.push_back(std::move(s));
    }
    d.put(make_sequence(ct::SegmentSequence, segs));
    const auto t0 = std::chrono::steady_clock::now();
    auto r = read_segmentation(d, true);
    REQUIRE(r);
    CHECK(seconds_since(t0) < 5.0);
    CHECK_EQ(r->segments.size(), 65535u);
}
