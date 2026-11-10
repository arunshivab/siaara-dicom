// Clinical layer: line profiles and calibrated measurements (src/clinical/measure.*) on synthetic
// images with exact answers.
#include <cmath>
#include <limits>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/geometry.hpp"
#include "clinical/measure.hpp"
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

TEST_CASE("measure: line profile") {
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

TEST_CASE("measure: distance, angle and Cobb angle") {
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
