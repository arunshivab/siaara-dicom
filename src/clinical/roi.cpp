// SIAARA DICOM Toolkit - ROI statistics and measurements (see roi.hpp).
#include "clinical/roi.hpp"

#include "image/raster.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace siaara::clinical {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kEllipseVertices = 1024;

bool finite(double v) noexcept { return std::isfinite(v); }

double shoelace(const std::vector<Point2>& v) noexcept {
    double a = 0.0;
    const std::size_t n = v.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) a += v[j].x * v[i].y - v[i].x * v[j].y;
    return 0.5 * a;
}

// Polygon with the same area as the ellipse (vertices on an ellipse scaled by
// sqrt(2 pi / (N sin(2 pi / N)))).
std::vector<Point2> ellipse_polygon(double cx, double cy, double rx, double ry, double angle) {
    const double n = static_cast<double>(kEllipseVertices);
    const double k = std::sqrt(2.0 * kPi / (n * std::sin(2.0 * kPi / n)));
    const double ca = std::cos(angle), sa = std::sin(angle);
    std::vector<Point2> v(kEllipseVertices);
    for (std::size_t i = 0; i < kEllipseVertices; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / n;
        const double ex = rx * k * std::cos(t), ey = ry * k * std::sin(t);
        v[i] = {cx + ex * ca - ey * sa, cy + ex * sa + ey * ca};
    }
    return v;
}

std::vector<Point2> as_polygon(const Roi& roi) {
    switch (roi.shape) {
        case RoiShape::Rectangle: {
            const double xa = std::min(roi.x0, roi.x1), xb = std::max(roi.x0, roi.x1);
            const double ya = std::min(roi.y0, roi.y1), yb = std::max(roi.y0, roi.y1);
            return {{xa, ya}, {xb, ya}, {xb, yb}, {xa, yb}};
        }
        case RoiShape::Circle: return ellipse_polygon(roi.cx, roi.cy, roi.rx, roi.rx, 0.0);
        case RoiShape::Ellipse: return ellipse_polygon(roi.cx, roi.cy, roi.rx, roi.ry, roi.angle);
        case RoiShape::Polygon:
        case RoiShape::Freehand: return roi.vertices;
    }
    return {};
}

// Sutherland-Hodgman clip of a polygon against x >= lo (axis 0) etc.
template <class Inside, class Cut>
void clip_edge(const std::vector<Point2>& in, std::vector<Point2>& out, Inside inside, Cut cut) {
    out.clear();
    const std::size_t n = in.size();
    if (n == 0) return;
    for (std::size_t i = 0; i < n; ++i) {
        const Point2& cur = in[i];
        const Point2& prev = in[(i + n - 1) % n];
        const bool ci = inside(cur), pi = inside(prev);
        if (ci) {
            if (!pi) out.push_back(cut(prev, cur));
            out.push_back(cur);
        } else if (pi) {
            out.push_back(cut(prev, cur));
        }
    }
}

double clipped_area(const std::vector<Point2>& poly, double x0, double y0, double x1, double y1,
                    std::vector<Point2>& a, std::vector<Point2>& b) {
    auto cut_x = [](double x) {
        return [x](const Point2& p, const Point2& q) {
            const double t = (x - p.x) / (q.x - p.x);
            return Point2{x, p.y + t * (q.y - p.y)};
        };
    };
    auto cut_y = [](double y) {
        return [y](const Point2& p, const Point2& q) {
            const double t = (y - p.y) / (q.y - p.y);
            return Point2{p.x + t * (q.x - p.x), y};
        };
    };
    clip_edge(poly, a, [x0](const Point2& p) { return p.x >= x0; }, cut_x(x0));
    clip_edge(a, b, [x1](const Point2& p) { return p.x <= x1; }, cut_x(x1));
    clip_edge(b, a, [y0](const Point2& p) { return p.y >= y0; }, cut_y(y0));
    clip_edge(a, b, [y1](const Point2& p) { return p.y <= y1; }, cut_y(y1));
    if (b.size() < 3) return 0.0;
    return std::fabs(shoelace(b));
}

Status validate(const Roi& roi) {
    switch (roi.shape) {
        case RoiShape::Rectangle:
            if (!finite(roi.x0) || !finite(roi.y0) || !finite(roi.x1) || !finite(roi.y1) || roi.x0 == roi.x1 || roi.y0 == roi.y1)
                return Status(Errc::InvalidArgument, "degenerate rectangle");
            return Status::ok();
        case RoiShape::Circle:
            if (!finite(roi.cx) || !finite(roi.cy) || !finite(roi.rx) || !(roi.rx > 0.0))
                return Status(Errc::InvalidArgument, "degenerate circle");
            return Status::ok();
        case RoiShape::Ellipse:
            if (!finite(roi.cx) || !finite(roi.cy) || !finite(roi.angle) || !finite(roi.rx) || !finite(roi.ry) ||
                !(roi.rx > 0.0) || !(roi.ry > 0.0))
                return Status(Errc::InvalidArgument, "degenerate ellipse");
            return Status::ok();
        case RoiShape::Polygon:
        case RoiShape::Freehand:
            if (roi.vertices.size() < 3) return Status(Errc::InvalidArgument, "polygon needs at least 3 vertices");
            if (roi.vertices.size() > (1u << 24)) return Status(Errc::LimitExceeded, "too many polygon vertices");
            for (const auto& p : roi.vertices)
                if (!finite(p.x) || !finite(p.y)) return Status(Errc::InvalidArgument, "non-finite vertex");
            if (std::fabs(shoelace(roi.vertices)) <= 0.0) return Status(Errc::InvalidArgument, "polygon has no area");
            return Status::ok();
    }
    return Status(Errc::InvalidArgument, "unknown shape");
}

struct Box {
    std::int64_t c0 = 0, r0 = 0, c1 = -1, r1 = -1;  // inclusive pixel range
    bool empty() const noexcept { return c1 < c0 || r1 < r0; }
};

Box bounds(const std::vector<Point2>& poly, std::uint32_t rows, std::uint32_t cols) {
    double xa = std::numeric_limits<double>::infinity(), ya = xa, xb = -xa, yb = -xa;
    for (const auto& p : poly) {
        xa = std::min(xa, p.x);
        xb = std::max(xb, p.x);
        ya = std::min(ya, p.y);
        yb = std::max(yb, p.y);
    }
    Box b;
    auto clampc = [](double v, double hi) { return v < 0.0 ? 0.0 : (v > hi ? hi : v); };
    b.c0 = static_cast<std::int64_t>(std::floor(clampc(xa, cols)));
    b.c1 = static_cast<std::int64_t>(std::floor(clampc(xb, cols)));
    b.r0 = static_cast<std::int64_t>(std::floor(clampc(ya, rows)));
    b.r1 = static_cast<std::int64_t>(std::floor(clampc(yb, rows)));
    b.c1 = std::min<std::int64_t>(b.c1, static_cast<std::int64_t>(cols) - 1);
    b.r1 = std::min<std::int64_t>(b.r1, static_cast<std::int64_t>(rows) - 1);
    return b;
}

// Calls fn(index, weight) for every pixel with a non-zero weight.
template <class Fn>
Status for_each_weight(std::uint32_t rows, std::uint32_t cols, const Roi& roi, const RoiOptions& opt, Fn&& fn) {
    if (Status s = validate(roi); !s) return s;
    if (rows == 0 || cols == 0) return Status::ok();
    // Exact centre tests for the analytic shapes in centre mode.
    if (!opt.partial_pixels && (roi.shape == RoiShape::Ellipse || roi.shape == RoiShape::Circle)) {
        const double rx = roi.rx, ry = roi.shape == RoiShape::Circle ? roi.rx : roi.ry;
        const double ang = roi.shape == RoiShape::Circle ? 0.0 : roi.angle;
        const double ca = std::cos(ang), sa = std::sin(ang);
        const double ext = std::max(rx, ry);
        const std::vector<Point2> box{{roi.cx - ext, roi.cy - ext}, {roi.cx + ext, roi.cy + ext}};
        const Box b = bounds(box, rows, cols);
        for (std::int64_t r = b.r0; r <= b.r1 && !b.empty(); ++r)
            for (std::int64_t c = b.c0; c <= b.c1; ++c) {
                const double dx = static_cast<double>(c) + 0.5 - roi.cx, dy = static_cast<double>(r) + 0.5 - roi.cy;
                const double u = (dx * ca + dy * sa) / rx, v = (-dx * sa + dy * ca) / ry;
                if (u * u + v * v <= 1.0)
                    fn(static_cast<std::size_t>(r) * cols + static_cast<std::size_t>(c), 1.0);
            }
        return Status::ok();
    }
    const std::vector<Point2> poly = as_polygon(roi);
    const Box b = bounds(poly, rows, cols);
    if (b.empty()) return Status::ok();
    const std::size_t bw = static_cast<std::size_t>(b.c1 - b.c0 + 1), bh = static_cast<std::size_t>(b.r1 - b.r0 + 1);
    // Centre classification by scan lines (even-odd rule, half-open edges).
    std::vector<std::uint8_t> inside(bw * bh, 0);
    std::vector<double> xs;
    const std::size_t n = poly.size();
    for (std::int64_t r = b.r0; r <= b.r1; ++r) {
        const double y = static_cast<double>(r) + 0.5;
        xs.clear();
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Point2& p = poly[j];
            const Point2& q = poly[i];
            if ((p.y > y) != (q.y > y)) xs.push_back(p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            // Clamped in double first: a far-away vertex must not overflow the cast (R-083).
            const double lim = static_cast<double>(cols) + 1.0;
            std::int64_t ca = image::raster::clamp_to_i64(std::ceil(xs[k] - 0.5), -1.0, lim);
            std::int64_t cb = image::raster::clamp_to_i64(std::ceil(xs[k + 1] - 0.5), -1.0, lim) - 1;
            ca = std::max(ca, b.c0);
            cb = std::min(cb, b.c1);
            for (std::int64_t c = ca; c <= cb; ++c)
                inside[static_cast<std::size_t>(r - b.r0) * bw + static_cast<std::size_t>(c - b.c0)] = 1;
        }
    }
    if (!opt.partial_pixels) {
        for (std::size_t r = 0; r < bh; ++r)
            for (std::size_t c = 0; c < bw; ++c)
                if (inside[r * bw + c])
                    fn((static_cast<std::size_t>(b.r0) + r) * cols + static_cast<std::size_t>(b.c0) + c, 1.0);
        return Status::ok();
    }
    // Pixels crossed by an edge get their exact covered area; the others are fully in or out.
    std::vector<std::uint8_t> edge(bw * bh, 0);
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point2& p = poly[j];
        const Point2& q = poly[i];
        const double ya = std::min(p.y, q.y), yb = std::max(p.y, q.y);
        std::int64_t ra = std::max<std::int64_t>(b.r0, image::raster::clamp_to_i64(std::floor(ya), -1.0, rows + 1.0));
        std::int64_t rb = std::min<std::int64_t>(b.r1, image::raster::clamp_to_i64(std::floor(yb), -1.0, rows + 1.0));
        for (std::int64_t r = ra; r <= rb; ++r) {
            double s0 = std::max(ya, static_cast<double>(r)), s1 = std::min(yb, static_cast<double>(r + 1));
            double xa, xb;
            if (q.y == p.y) {
                xa = std::min(p.x, q.x);
                xb = std::max(p.x, q.x);
            } else {
                const double t0 = (s0 - p.y) / (q.y - p.y), t1 = (s1 - p.y) / (q.y - p.y);
                xa = p.x + t0 * (q.x - p.x);
                xb = p.x + t1 * (q.x - p.x);
                if (xa > xb) std::swap(xa, xb);
            }
            std::int64_t ca = std::max<std::int64_t>(b.c0, image::raster::clamp_to_i64(std::floor(xa), -1.0, cols + 1.0));
            std::int64_t cb = std::min<std::int64_t>(b.c1, image::raster::clamp_to_i64(std::floor(xb), -1.0, cols + 1.0));
            for (std::int64_t c = ca; c <= cb; ++c)
                edge[static_cast<std::size_t>(r - b.r0) * bw + static_cast<std::size_t>(c - b.c0)] = 1;
        }
    }
    std::vector<Point2> ta, tb;
    for (std::size_t r = 0; r < bh; ++r)
        for (std::size_t c = 0; c < bw; ++c) {
            const std::size_t k = r * bw + c;
            const double x0 = static_cast<double>(b.c0) + static_cast<double>(c), y0 = static_cast<double>(b.r0) + static_cast<double>(r);
            double w;
            if (edge[k]) w = std::min(1.0, clipped_area(poly, x0, y0, x0 + 1.0, y0 + 1.0, ta, tb));
            else w = inside[k] ? 1.0 : 0.0;
            if (w > 0.0) fn((static_cast<std::size_t>(b.r0) + r) * cols + static_cast<std::size_t>(b.c0) + c, w);
        }
    return Status::ok();
}

}  // namespace

Roi Roi::rectangle(double x0, double y0, double x1, double y1) {
    Roi r;
    r.shape = RoiShape::Rectangle;
    r.x0 = x0, r.y0 = y0, r.x1 = x1, r.y1 = y1;
    return r;
}
Roi Roi::circle(double cx, double cy, double radius) {
    Roi r;
    r.shape = RoiShape::Circle;
    r.cx = cx, r.cy = cy, r.rx = radius, r.ry = radius;
    return r;
}
Roi Roi::ellipse(double cx, double cy, double rx, double ry, double angle) {
    Roi r;
    r.shape = RoiShape::Ellipse;
    r.cx = cx, r.cy = cy, r.rx = rx, r.ry = ry, r.angle = angle;
    return r;
}
Roi Roi::polygon(std::vector<Point2> v) {
    Roi r;
    r.shape = RoiShape::Polygon;
    r.vertices = std::move(v);
    return r;
}
Roi Roi::freehand(std::vector<Point2> v) {
    Roi r;
    r.shape = RoiShape::Freehand;
    r.vertices = std::move(v);
    return r;
}

double Roi::area() const noexcept {
    switch (shape) {
        case RoiShape::Rectangle: return std::fabs((x1 - x0) * (y1 - y0));
        case RoiShape::Circle: return kPi * rx * rx;
        case RoiShape::Ellipse: return kPi * rx * ry;
        case RoiShape::Polygon:
        case RoiShape::Freehand: return vertices.size() < 3 ? 0.0 : std::fabs(shoelace(vertices));
    }
    return 0.0;
}

Result<RoiStats> roi_stats(std::span<const float> values, std::uint32_t rows, std::uint32_t columns, const Roi& roi,
                           const PixelSpacingInfo* spacing, const RoiOptions& opt) {
    if (values.size() != static_cast<std::size_t>(rows) * columns)
        return Status(Errc::InvalidArgument, "values size does not match rows x columns");
    std::vector<std::pair<std::size_t, double>> w;
    Status s = for_each_weight(rows, columns, roi, opt, [&](std::size_t i, double wt) {
        if (!std::isnan(values[i])) w.emplace_back(i, wt);
    });
    if (!s) return s;
    RoiStats st;
    st.area_px = roi.area();
    if (spacing && spacing->valid()) {
        st.area_mm2 = st.area_px * spacing->row * spacing->column;
        st.spacing_source = spacing->source;
        st.spacing_at_detector = spacing->at_detector;
    }
    if (w.empty()) return st;
    double sw = 0.0, sum = 0.0;
    st.min = std::numeric_limits<double>::infinity();
    st.max = -st.min;
    for (const auto& [i, wt] : w) {
        const double v = values[i];
        sw += wt;
        sum += wt * v;
        st.min = std::min(st.min, v);
        st.max = std::max(st.max, v);
    }
    const double mean = sum / sw;
    double ss = 0.0;
    for (const auto& [i, wt] : w) {
        const double d = static_cast<double>(values[i]) - mean;
        ss += wt * d * d;
    }
    st.count = sw;
    st.sum = sum;
    st.mean = mean;
    st.sd = std::sqrt(ss / sw);
    st.sd_sample = sw > 1.0 ? std::sqrt(ss / (sw - 1.0)) : 0.0;
    st.pixels = w.size();
    return st;
}

Result<std::vector<float>> roi_mask(std::uint32_t rows, std::uint32_t columns, const Roi& roi, const RoiOptions& opt) {
    std::vector<float> m(static_cast<std::size_t>(rows) * columns, 0.0f);
    Status s = for_each_weight(rows, columns, roi, opt, [&](std::size_t i, double wt) { m[i] = static_cast<float>(wt); });
    if (!s) return s;
    return m;
}

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
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    std::uint32_t n = samples;
    if (n == 0) n = static_cast<std::uint32_t>(std::min(1e6, std::ceil(len))) + 1;
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
        if (s.x < 0.0 || s.y < 0.0 || s.x > columns || s.y > rows) {
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
