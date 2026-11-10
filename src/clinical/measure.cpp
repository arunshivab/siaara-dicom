// SIAARA DICOM Toolkit - line profiles and calibrated measurements (see measure.hpp).
#include "clinical/measure.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace siaara::clinical {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool finite(double v) noexcept { return std::isfinite(v); }

}  // namespace

// ---------------------------------------------------------------- measurements
namespace {
struct Scaled {
    Vec3 v;
    MeasureSpace space;
};
Scaled to_space(Point2 p, const PlaneGeometry& g) {
    if (g.complete()) return {g.pixel_to_patient(p.x - 0.5, p.y - 0.5), MeasureSpace::Patient};
    if (g.spacing.valid()) return {{p.x * g.spacing.column, p.y * g.spacing.row, 0.0}, MeasureSpace::Spacing};
    return {{p.x, p.y, 0.0}, MeasureSpace::Pixels};
}
Measurement base(const PlaneGeometry& g, MeasureSpace s) {
    Measurement m;
    m.space = s;
    if (s != MeasureSpace::Pixels) {
        m.spacing_source = g.spacing.source;
        m.at_detector = g.spacing.at_detector;
    }
    return m;
}
double angle_between(const Vec3& a, const Vec3& b) noexcept {
    const double na = a.norm(), nb = b.norm();
    if (na == 0.0 || nb == 0.0) return 0.0;
    // atan2 of |a x b| and a.b is accurate near 0 and 180 degrees.
    return std::atan2(a.cross(b).norm(), a.dot(b)) * 180.0 / kPi;
}
}  // namespace

Measurement measure_distance(Point2 a, Point2 b, const PlaneGeometry& g) {
    const Scaled pa = to_space(a, g), pb = to_space(b, g);
    Measurement m = base(g, pa.space);
    m.value = (pb.v - pa.v).norm();
    return m;
}

Measurement measure_angle(Point2 a, Point2 vertex, Point2 c, const PlaneGeometry& g) {
    const Scaled pa = to_space(a, g), pv = to_space(vertex, g), pc = to_space(c, g);
    Measurement m = base(g, pv.space);
    m.value = angle_between(pa.v - pv.v, pc.v - pv.v);
    return m;
}

Measurement measure_cobb(Point2 a0, Point2 a1, Point2 b0, Point2 b1, const PlaneGeometry& g) {
    const Scaled p0 = to_space(a0, g), p1 = to_space(a1, g), q0 = to_space(b0, g), q1 = to_space(b1, g);
    Measurement m = base(g, p0.space);
    double ang = angle_between(p1.v - p0.v, q1.v - q0.v);
    if (ang > 90.0) ang = 180.0 - ang;  // lines are undirected
    m.value = ang;
    return m;
}

LineProfile line_profile(std::span<const float> values, std::uint32_t rows, std::uint32_t columns, Point2 a, Point2 b,
                         const PlaneGeometry* geometry, std::uint32_t samples) {
    LineProfile out;
    if (values.size() != static_cast<std::size_t>(rows) * columns || rows == 0 || columns == 0) return out;
    if (!finite(a.x) || !finite(a.y) || !finite(b.x) || !finite(b.y)) return out;
    // R-140: end points far outside any image (the differences below would overflow to infinity
    // near 1e308, then NaN) and sample counts no caller needs are refused: an empty profile.
    for (const double c : {a.x, a.y, b.x, b.y})
        if (std::fabs(c) > kMaxProfileCoordinate) return out;
    if (samples > kMaxProfileSamples) return out;
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    std::uint32_t n = samples;
    if (n == 0) n = static_cast<std::uint32_t>(std::min(static_cast<double>(kMaxProfileSamples - 1), std::ceil(len))) + 1;
    n = std::max<std::uint32_t>(n, 2);
    PlaneGeometry px;  // pixel-only fallback
    const PlaneGeometry& g = geometry ? *geometry : px;
    out.mm = g.complete() || g.spacing.valid();
    out.samples.reserve(n);
    auto at = [&](std::int64_t c, std::int64_t r) { return static_cast<double>(values[static_cast<std::size_t>(r) * columns + static_cast<std::size_t>(c)]); };
    for (std::uint32_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        ProfileSample s;
        s.x = a.x + t * (b.x - a.x);
        s.y = a.y + t * (b.y - a.y);
        s.distance = measure_distance(a, {s.x, s.y}, g).value;
        // Written so that a NaN coordinate is outside (it is never cast to an integer).
        if (!(s.x >= 0.0 && s.y >= 0.0 && s.x <= columns && s.y <= rows)) {
            s.value = std::numeric_limits<double>::quiet_NaN();
        } else {
            const double fx = std::clamp(s.x - 0.5, 0.0, static_cast<double>(columns - 1));
            const double fy = std::clamp(s.y - 0.5, 0.0, static_cast<double>(rows - 1));
            const std::int64_t c0 = static_cast<std::int64_t>(std::floor(fx)), r0 = static_cast<std::int64_t>(std::floor(fy));
            const std::int64_t c1 = std::min<std::int64_t>(c0 + 1, columns - 1), r1 = std::min<std::int64_t>(r0 + 1, rows - 1);
            const double u = fx - static_cast<double>(c0), v = fy - static_cast<double>(r0);
            s.value = (1 - u) * (1 - v) * at(c0, r0) + u * (1 - v) * at(c1, r0) + (1 - u) * v * at(c0, r1) + u * v * at(c1, r1);
        }
        out.samples.push_back(s);
    }
    return out;
}

}  // namespace siaara::clinical
