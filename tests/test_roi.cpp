// Clinical layer: ROI statistics, masks, line profiles and calibrated measurements
// (src/clinical/roi.*) on synthetic images with exact answers.
#include <cmath>
#include <limits>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/geometry.hpp"
#include "clinical/roi.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

constexpr double kPi = 3.14159265358979323846;
bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// 10 x 10 image, value = column + 10 x row.
std::vector<float> ramp(std::uint32_t rows = 10, std::uint32_t cols = 10) {
    std::vector<float> v(static_cast<std::size_t>(rows) * cols);
    for (std::uint32_t r = 0; r < rows; ++r)
        for (std::uint32_t c = 0; c < cols; ++c) v[r * cols + c] = static_cast<float>(c + 10 * r);
    return v;
}

PixelSpacingInfo spacing(double row, double col) {
    PixelSpacingInfo s;
    s.row = row;
    s.column = col;
    s.source = SpacingSource::PixelSpacing;
    return s;
}

}  // namespace

TEST_CASE("roi: rectangle statistics (centre rule)") {
    const auto img = ramp();
    // Corners (2,3)-(5,7): columns 2..4, rows 3..6 -> 12 pixels.
    const PixelSpacingInfo sp = spacing(0.5, 2.0);
    auto s = roi_stats(img, 10, 10, Roi::rectangle(5, 7, 2, 3), &sp);
    REQUIRE(s.is_ok());
    CHECK(s->count == 12.0);
    CHECK_EQ(s->pixels, 12u);
    // mean = mean(col) + 10 mean(row) = 3 + 45 = 48; sum = 576; min 2 + 30, max 4 + 60.
    CHECK(near(s->mean, 48.0));
    CHECK(near(s->sum, 576.0));
    CHECK(s->min == 32.0 && s->max == 64.0);
    // variance = var(col) + 100 var(row) = 2/3 + 100 x 1.25 = 125.666...; sample: x 12 / 11.
    CHECK(near(s->sd, std::sqrt(2.0 / 3.0 + 125.0)));
    CHECK(near(s->sd_sample, std::sqrt((2.0 / 3.0 + 125.0) * 12.0 / 11.0)));
    CHECK(near(s->area_px, 12.0));
    CHECK(s->area_mm2 && near(*s->area_mm2, 12.0));  // 12 px x 0.5 mm x 2 mm
    CHECK(s->spacing_source == SpacingSource::PixelSpacing);
}

TEST_CASE("roi: partial-pixel weighting is exact for polygons") {
    const auto img = ramp();
    RoiOptions o;
    o.partial_pixels = true;
    // (2.5,3)-(5,7): column 2 half covered, columns 3, 4 fully, rows 3..6.
    auto s = roi_stats(img, 10, 10, Roi::rectangle(2.5, 3, 5, 7), nullptr, o);
    REQUIRE(s.is_ok());
    CHECK(near(s->count, 10.0));
    CHECK_EQ(s->pixels, 12u);
    // weighted mean of columns: (0.5 x 2 + 3 + 4) / 2.5 = 3.2; rows mean 4.5 -> 48.2
    CHECK(near(s->mean, 3.2 + 45.0));
    CHECK(!s->area_mm2.has_value());
    // Triangle (0,0),(4,0),(0,4): area 8; centre rule picks c + r < 3 -> 6 pixels.
    const Roi tri = Roi::polygon({{0, 0}, {4, 0}, {0, 4}});
    CHECK(near(tri.area(), 8.0));
    auto c = roi_stats(img, 10, 10, tri);
    REQUIRE(c.is_ok());
    CHECK(c->count == 6.0);
    auto p = roi_stats(img, 10, 10, tri, nullptr, o);
    REQUIRE(p.is_ok());
    CHECK(near(p->count, 8.0, 1e-12));
    // Concave L shape, clockwise vertex order.
    const Roi ell = Roi::freehand({{1, 1}, {1, 5}, {3, 5}, {3, 3}, {5, 3}, {5, 1}});
    CHECK(near(ell.area(), 12.0));
    auto l = roi_stats(img, 10, 10, ell, nullptr, o);
    CHECK(l.is_ok() && near(l->count, 12.0, 1e-12));
    auto lc = roi_stats(img, 10, 10, ell);
    CHECK(lc.is_ok() && lc->count == 12.0);  // integer vertices: centre rule is exact
}

TEST_CASE("roi: circle and ellipse") {
    const auto img = ramp(20, 20);
    // Circle (10,10) r = 2: centres at offsets +-0.5, +-1.5 except the four (1.5,1.5) corners -> 12.
    auto c = roi_stats(img, 20, 20, Roi::circle(10, 10, 2));
    REQUIRE(c.is_ok());
    CHECK(c->count == 12.0);
    CHECK(near(c->area_px, 4.0 * kPi));
    RoiOptions o;
    o.partial_pixels = true;
    auto cp = roi_stats(img, 20, 20, Roi::circle(10, 10, 2), nullptr, o);
    REQUIRE(cp.is_ok());
    CHECK(near(cp->count, 4.0 * kPi, 1e-9));  // equal-area 1024-gon
    // Symmetric about the centre: mean = value at (9.5, 9.5) = 9.5 + 10 x 9.5 = 104.5
    CHECK(near(cp->mean, 104.5, 1e-9));
    // An ellipse rotated by 90 degrees equals the one with swapped axes.
    auto e1 = roi_mask(20, 20, Roi::ellipse(10, 10, 6, 3, kPi / 2));
    auto e2 = roi_mask(20, 20, Roi::ellipse(10, 10, 3, 6, 0));
    REQUIRE(e1.is_ok() && e2.is_ok());
    CHECK(*e1 == *e2);
    auto ep = roi_stats(img, 20, 20, Roi::ellipse(10, 10, 6, 3, 0.3), nullptr, o);
    CHECK(ep.is_ok() && near(ep->count, kPi * 18.0, 1e-9));
    // Clipped by the image: a circle around a corner keeps only the inside quarter.
    auto q = roi_stats(img, 20, 20, Roi::circle(0, 0, 4), nullptr, o);
    CHECK(q.is_ok() && near(q->count, kPi * 4.0, 1e-9));
}

TEST_CASE("roi: NaN values, empty and degenerate shapes, size mismatch") {
    auto img = ramp();
    img[3 * 10 + 2] = std::numeric_limits<float>::quiet_NaN();
    auto s = roi_stats(img, 10, 10, Roi::rectangle(2, 3, 5, 7));
    CHECK(s.is_ok() && s->count == 11.0);
    auto out = roi_stats(img, 10, 10, Roi::rectangle(20, 20, 30, 30));
    CHECK(out.is_ok() && out->count == 0.0 && out->pixels == 0);
    CHECK(!roi_stats(img, 10, 10, Roi::rectangle(1, 1, 1, 5)).is_ok());
    CHECK(!roi_stats(img, 10, 10, Roi::circle(1, 1, 0)).is_ok());
    CHECK(!roi_stats(img, 10, 10, Roi::polygon({{0, 0}, {1, 1}})).is_ok());
    CHECK(!roi_stats(img, 10, 10, Roi::polygon({{0, 0}, {1, 1}, {2, 2}})).is_ok());  // collinear
    CHECK(!roi_stats(img, 10, 10, Roi::ellipse(1, 1, std::numeric_limits<double>::infinity(), 1)).is_ok());
    CHECK(!roi_stats(img, 9, 10, Roi::rectangle(0, 0, 2, 2)).is_ok());
    // Far away / huge coordinates: no crash, clipped.
    RoiOptions o;
    o.partial_pixels = true;
    auto big = roi_stats(img, 10, 10, Roi::polygon({{-1e9, -1e9}, {1e9, -1e9}, {0, 1e9}}), nullptr, o);
    CHECK(big.is_ok() && big->pixels > 0);
}

TEST_CASE("roi: line profile") {
    const auto img = ramp();
    PlaneGeometry g;
    g.spacing = spacing(1.0, 0.5);
    // Row 5 centres from column 0 to 9: values 50 .. 59, 10 samples, distance 0 .. 4.5 mm.
    auto p = line_profile(img, 10, 10, {0.5, 5.5}, {9.5, 5.5}, &g);
    REQUIRE(p.samples.size() == 10);
    CHECK(p.mm);
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(near(p.samples[i].value, 50.0 + static_cast<double>(i), 1e-6));
        CHECK(near(p.samples[i].distance, 0.5 * static_cast<double>(i)));
    }
    // Bilinear between centres: (2.0, 5.5) is halfway between columns 1 and 2.
    auto q = line_profile(img, 10, 10, {2.0, 5.5}, {2.0, 6.0}, nullptr, 2);
    REQUIRE(q.samples.size() == 2);
    CHECK(near(q.samples[0].value, 51.5, 1e-6));
    CHECK(near(q.samples[1].value, 56.5, 1e-6));
    CHECK(!q.mm);
    auto r = line_profile(img, 10, 10, {-5, 0}, {5, 0}, nullptr, 3);
    CHECK(std::isnan(r.samples[0].value));
    CHECK(line_profile(img, 9, 10, {0, 0}, {1, 1}).samples.empty());
}

TEST_CASE("roi: distance, angle and Cobb angle") {
    PlaneGeometry px;
    auto d0 = measure_distance({0, 0}, {3, 4}, px);
    CHECK(d0.space == MeasureSpace::Pixels && near(d0.value, 5.0));
    PlaneGeometry sp;
    sp.spacing = spacing(0.5, 0.8);
    // dx = 3 x 0.8 = 2.4 mm, dy = 4 x 0.5 = 2 mm.
    auto d1 = measure_distance({0, 0}, {3, 4}, sp);
    CHECK(d1.space == MeasureSpace::Spacing && near(d1.value, std::sqrt(2.4 * 2.4 + 4.0)));
    // Patient space on an oblique plane gives the same in-plane distance.
    PlaneGeometry pt;
    pt.spacing = sp.spacing;
    pt.has_position = pt.has_orientation = true;
    pt.position = {5, 6, 7};
    pt.row_cosine = Vec3{1, 1, 0}.normalized();
    pt.column_cosine = {0, 0, -1};
    pt.normal = pt.row_cosine.cross(pt.column_cosine);
    pt.rows = pt.columns = 10;
    auto d2 = measure_distance({0, 0}, {3, 4}, pt);
    CHECK(d2.space == MeasureSpace::Patient && near(d2.value, d1.value, 1e-12));
    // Right angle at the vertex in mm: (4,0)-(0,0)-(0,4) is 90 degrees in pixels and in mm
    // (axes stay perpendicular), but (4,4)-(0,0)-(4,0) is 45 degrees only with square pixels.
    CHECK(near(measure_angle({4, 0}, {0, 0}, {0, 4}, sp).value, 90.0, 1e-12));
    CHECK(near(measure_angle({4, 4}, {0, 0}, {4, 0}, px).value, 45.0, 1e-12));
    CHECK(near(measure_angle({4, 4}, {0, 0}, {4, 0}, sp).value, std::atan2(4 * 0.5, 4 * 0.8) * 180 / kPi, 1e-12));
    // Cobb: lines at +15 and -15 degrees -> 30; direction of either line does not matter.
    const double t = std::tan(15 * kPi / 180);
    auto cobb = measure_cobb({0, 0}, {10, 10 * t}, {10, 10 * t}, {0, 20 * t}, px);
    CHECK(near(cobb.value, 30.0, 1e-9));
    auto cobb2 = measure_cobb({10, 10 * t}, {0, 0}, {10, 10 * t}, {0, 20 * t}, px);
    CHECK(near(cobb2.value, 30.0, 1e-9));
    CHECK(near(measure_cobb({0, 0}, {1, 0}, {0, 0}, {1, 0}, px).value, 0.0));
}

TEST_CASE("roi: C API statistics on a decoded frame") {
    DicomFile f;
    Dataset& d = f.dataset;
    (void)d.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    (void)d.set_string(siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.4.6");
    (void)d.set_uint16(siaara::tags::Rows, 10);
    (void)d.set_uint16(siaara::tags::Columns, 10);
    (void)d.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)d.set_uint16(siaara::tags::BitsAllocated, 16);
    (void)d.set_uint16(siaara::tags::BitsStored, 16);
    (void)d.set_uint16(siaara::tags::HighBit, 15);
    (void)d.set_uint16(siaara::tags::PixelRepresentation, 0);
    (void)d.set_string(siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)d.set_string(siaara::tags::RescaleSlope, VR::DS, "2");
    (void)d.set_string(siaara::tags::RescaleIntercept, VR::DS, "-10");
    (void)d.set_string(ct::PixelSpacing, VR::DS, "0.5\\2");
    std::vector<std::uint8_t> px(200);
    for (int i = 0; i < 100; ++i) store_le16(px.data() + 2 * i, static_cast<std::uint16_t>(i % 10 + 10 * (i / 10)));
    (void)d.set_bytes(siaara::tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &file) == SIAARA_OK);
    siaara_roi_stats st{};
    st.struct_size = sizeof st;
    const double rect[4] = {2, 3, 5, 7};
    CHECK_EQ(siaara_roi_stats_compute(file, 0, SIAARA_ROI_RECTANGLE, rect, 4, 0, &st), SIAARA_OK);
    CHECK(st.count == 12.0);
    CHECK(near(st.mean, 48.0 * 2 - 10));  // modality values
    CHECK(st.has_area_mm2 == 1 && near(st.area_mm2, 12.0));
    const double tri[6] = {0, 0, 4, 0, 0, 4};
    CHECK_EQ(siaara_roi_stats_compute(file, 0, SIAARA_ROI_POLYGON, tri, 6, 1, &st), SIAARA_OK);
    CHECK(near(st.count, 8.0, 1e-12));
    CHECK_EQ(siaara_roi_stats_compute(file, 0, SIAARA_ROI_POLYGON, tri, 5, 1, &st), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_roi_stats_compute(file, 0, 99, tri, 6, 1, &st), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_roi_stats_compute(file, 3, SIAARA_ROI_RECTANGLE, rect, 4, 0, &st), SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file_free(file);
}
