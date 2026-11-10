// Regression tests for the 2026-11-10 02:15 security review, clinical findings (R-054, R-056).
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/gsps.hpp"
#include "clinical/roi.hpp"
#include "clinical/seg.hpp"
#include "image/render.hpp"
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

// rows x cols 8-bit MONOCHROME2 image, all zero.
Dataset blank_image(std::uint16_t rows, std::uint16_t cols) {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.100");
    (void)d.set_uint16(siaara::tags::Rows, rows);
    (void)d.set_uint16(siaara::tags::Columns, cols);
    (void)d.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)d.set_uint16(siaara::tags::BitsAllocated, 8);
    (void)d.set_uint16(siaara::tags::BitsStored, 8);
    (void)d.set_uint16(siaara::tags::HighBit, 7);
    (void)d.set_uint16(siaara::tags::PixelRepresentation, 0);
    put(d, siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    const std::vector<std::uint8_t> px(std::size_t{rows} * cols, 0);
    (void)d.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    return d;
}

PresentationState ps_with(std::vector<GraphicObject> objs) {
    PresentationState ps;
    ps.kind = PresentationKind::Grayscale;
    GraphicAnnotation a;
    a.layer = "L";
    a.graphics = std::move(objs);
    ps.annotations.push_back(std::move(a));
    return ps;
}

GraphicObject object(GraphicType t, std::vector<Point2> pts, bool filled = false) {
    GraphicObject g;
    g.type = t;
    g.points = std::move(pts);
    g.filled = filled;
    return g;
}

std::size_t drawn(const PsRendered& r) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < r.rgba.size(); i += 4)
        if (r.rgba[i] == 255 && r.rgba[i + 1] == 255 && r.rgba[i + 2] == 0) ++n;
    return n;
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

TEST_CASE("R-056: a 2,000-point POLYLINE far outside an 8x8 image is clipped, not stepped") {
    const Dataset img_ds = blank_image(8, 8);
    auto img = image::Image::open(img_ds, "1.2.840.10008.1.2.1");
    REQUIRE(img);
    std::vector<Point2> pts;
    for (int i = 0; i < 2000; ++i) pts.push_back({i % 2 ? 4.0e6 : -4.0e6, i % 4 < 2 ? 4.0 : 5.0});
    const PresentationState ps = ps_with({object(GraphicType::Polyline, pts)});
    const auto t0 = std::chrono::steady_clock::now();
    auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100");
    const double secs = seconds_since(t0);
    REQUIRE(r);
    CHECK(secs < 1.0);
    CHECK(drawn(*r) >= 8u);  // the horizontal crossings are still drawn
}

TEST_CASE("R-056: drawing work is budgeted per render") {
    const Dataset img_ds = blank_image(64, 64);
    auto img = image::Image::open(img_ds, "1.2.840.10008.1.2.1");
    REQUIRE(img);
    std::vector<GraphicObject> objs;
    for (int i = 0; i < 200; ++i) objs.push_back(object(GraphicType::Polyline, {{0.0, 0.0}, {64.0, 64.0}}));
    PsRenderOptions o;
    o.max_draw_work = 1000;
    auto r = render_with_presentation_state(*img, ps_with(objs), 0, "1.2.3.100", o);
    REQUIRE(!r);
    CHECK(r.error().code == Errc::LimitExceeded);
    o.max_draw_work = 0;  // default budget: fine
    CHECK(render_with_presentation_state(*img, ps_with(objs), 0, "1.2.3.100", o));
}

TEST_CASE("R-056: polygon fill is an edge-table scan and matches the ROI centre rule") {
    // Many-vertex zigzag polygon over a tall canvas: rows x vertices was the old cost.
    const Dataset img_ds = blank_image(8000, 40);
    auto img = image::Image::open(img_ds, "1.2.840.10008.1.2.1");
    REQUIRE(img);
    std::vector<Point2> zig;
    for (int i = 0; i < 65000; ++i) zig.push_back({i % 2 ? 30.0 : 10.0, 1.0 + i * (7998.0 / 65000.0)});
    zig.push_back({0.5, 7999.0});
    zig.push_back({0.5, 1.0});
    const auto t0 = std::chrono::steady_clock::now();
    auto r = render_with_presentation_state(*img, ps_with({object(GraphicType::Polygon, zig, true)}), 0, "1.2.3.100");
    REQUIRE(r);
    CHECK(seconds_since(t0) < 2.0);
    // Exactness against roi_mask on simple shapes (pixel centres, even-odd).
    const Dataset small_ds = blank_image(20, 20);
    auto small = image::Image::open(small_ds, "1.2.840.10008.1.2.1");
    REQUIRE(small);
    const std::vector<std::vector<Point2>> polys = {
        {{2.2, 3.1}, {17.5, 4.0}, {9.0, 18.7}},
        {{1, 1}, {19, 1}, {19, 19}, {1, 19}, {10, 10}},
        {{-5, -5}, {30, 2}, {4, 40}},
    };
    for (const auto& poly : polys) {
        PsRenderOptions o;
        auto rr = render_with_presentation_state(*small, ps_with({object(GraphicType::Polygon, poly, true)}), 0, "1.2.3.100", o);
        REQUIRE(rr);
        auto m = roi_mask(20, 20, Roi::polygon(poly));
        REQUIRE(m);
        // Every centre-inside pixel is drawn (the outline adds pixels, never removes them).
        for (std::size_t i = 0; i < m->size(); ++i)
            if ((*m)[i] > 0.0f) CHECK(rr->rgba[4 * i] == 255 && rr->rgba[4 * i + 2] == 0);
    }
}

TEST_CASE("R-056: graphic point caps per object and per presentation state") {
    auto ps_ds = [](std::size_t objects, std::size_t points) {
        Dataset d;
        put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.11.1");
        std::vector<Dataset> gos;
        for (std::size_t k = 0; k < objects; ++k) {
            Dataset g;
            put(g, ct::GraphicAnnotationUnits, VR::CS, "PIXEL");
            put(g, ct::GraphicType, VR::CS, "POLYLINE");
            (void)g.set_uint16(ct::GraphicDimensions, 2);
            std::vector<std::uint8_t> fl(points * 8, 0);
            (void)g.set_bytes(ct::GraphicData, VR::FL, Bytes::copy_of(fl.data(), fl.size()));
            gos.push_back(std::move(g));
        }
        Dataset a;
        put(a, ct::GraphicLayer, VR::CS, "L");
        a.put(make_sequence(ct::GraphicObjectSequence, gos));
        d.put(make_sequence(ct::GraphicAnnotationSequence, {a}));
        return d;
    };
    {
        auto lenient = read_presentation_state(ps_ds(1, 70000), true);
        REQUIRE(lenient);
        CHECK(lenient->annotations.empty() || lenient->annotations[0].graphics.empty());
        CHECK(!lenient->warnings.empty());
        auto strict = read_presentation_state(ps_ds(1, 70000), false);
        REQUIRE(!strict);
        CHECK(strict.error().code == Errc::LimitExceeded);
    }
    {
        auto r = read_presentation_state(ps_ds(40, 60000), true);  // 2.4M points in all
        REQUIRE(!r);
        CHECK(r.error().code == Errc::LimitExceeded);
    }
    CHECK(read_presentation_state(ps_ds(4, 60000), true));
}
