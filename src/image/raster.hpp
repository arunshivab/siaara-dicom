// SIAARA DICOM Toolkit - shared 2D rasterisation helpers (internal): segment clipping, safe
// double -> integer conversion, a work budget, and an edge-table scan-line polygon walk.
// Used by overlay/shutter masks and GSPS annotation drawing (R-056, R-083, R-084).
// Liang-Barsky clipping: Liang & Barsky, "A New Concept and Method for Line Clipping", ACM TOG 3(1), 1984.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace siaara::image::raster {

// `v` clamped to [lo, hi] in double first, then converted: never overflows (UBSan, R-083).
// NaN gives `lo`. lo and hi must be exactly representable integers within int64 range.
inline std::int64_t clamp_to_i64(double v, double lo, double hi) noexcept {
    if (!(v >= lo)) return static_cast<std::int64_t>(lo);  // also NaN
    if (v > hi) return static_cast<std::int64_t>(hi);
    return static_cast<std::int64_t>(v);
}

// Clips segment (x0,y0)-(x1,y1) to the box [xlo,xhi] x [ylo,yhi] (Liang-Barsky). False when it
// lies outside (or is not finite). A segment wholly inside is returned unchanged.
inline bool clip_segment(double& x0, double& y0, double& x1, double& y1, double xlo, double xhi, double ylo,
                         double yhi) noexcept {
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return false;
    double t0 = 0, t1 = 1;
    const double dx = x1 - x0, dy = y1 - y0;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x0 - xlo, xhi - x0, y0 - ylo, yhi - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    if (t0 == 0 && t1 == 1) return true;
    const double nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
    x1 = x0 + t1 * dx;
    y1 = y0 + t1 * dy;
    x0 = nx0;
    y0 = ny0;
    return true;
}

// Units of work (points, line steps, scan-line edge visits, pixels written) a rasterisation may
// spend; once spent, charge() fails and stays failed.
struct WorkBudget {
    std::uint64_t remaining = 0;
    bool exceeded = false;
    bool charge(std::uint64_t n) noexcept {
        if (exceeded) return false;
        if (n > remaining) {
            exceeded = true;
            remaining = 0;
            return false;
        }
        remaining -= n;
        return true;
    }
};

// A polygon edge; its crossing with scan line y is ax + (y - ay) * (bx - ax) / (by - ay), and it
// crosses lines with min(ay, by) <= y < max(ay, by) (half-open, so shared vertices count once).
struct Edge {
    double ax, ay, bx, by;
};

// Even-odd scan-line walk with an edge table: for each row r in [r0, r1] that the polygon spans,
// y = r + y_offset, fn(r, xs) gets the sorted crossings. Cost is the edges plus the active edges
// of each visited row (not rows x vertices), charged to `budget`; false when it ran out.
// Non-finite edges are ignored.
template <class Fn>
bool scan_polygon(std::vector<Edge> edges, std::int64_t r0, std::int64_t r1, double y_offset, WorkBudget& budget, Fn&& fn) {
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [](const Edge& e) {
                                   return !(std::isfinite(e.ax) && std::isfinite(e.ay) && std::isfinite(e.bx) &&
                                            std::isfinite(e.by)) ||
                                          e.ay == e.by;
                               }),
                edges.end());
    if (edges.empty() || r1 < r0) return true;
    if (!budget.charge(edges.size())) return false;
    auto ymin = [](const Edge& e) { return std::min(e.ay, e.by); };
    auto ymax = [](const Edge& e) { return std::max(e.ay, e.by); };
    std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) { return ymin(a) < ymin(b); });
    double lo = ymin(edges.front()), hi = lo;
    for (const Edge& e : edges) hi = std::max(hi, ymax(e));
    // First row with y >= lo, last row with y < hi.
    const double rlo = std::max(static_cast<double>(r0), std::ceil(lo - y_offset));
    const double rhi = std::min(static_cast<double>(r1), std::ceil(hi - y_offset) - 1.0);
    if (!(rlo <= rhi)) return true;
    std::vector<const Edge*> active;
    std::vector<double> xs;
    std::size_t next = 0;
    for (auto r = static_cast<std::int64_t>(rlo); r <= static_cast<std::int64_t>(rhi); ++r) {
        const double y = static_cast<double>(r) + y_offset;
        while (next < edges.size() && ymin(edges[next]) <= y) active.push_back(&edges[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge* e) { return ymax(*e) <= y; }),
                     active.end());
        if (active.empty()) {
            if (next >= edges.size()) break;
            // Jump to the row of the next edge.
            const double jump = std::ceil(ymin(edges[next]) - y_offset) - 1.0;
            if (jump > static_cast<double>(r)) r = static_cast<std::int64_t>(std::min(jump, rhi));
            continue;
        }
        if (!budget.charge(active.size() + 1)) return false;
        xs.clear();
        for (const Edge* e : active) xs.push_back(e->ax + (y - e->ay) * (e->bx - e->ax) / (e->by - e->ay));
        std::sort(xs.begin(), xs.end());
        fn(r, xs);
    }
    return true;
}

}  // namespace siaara::image::raster
