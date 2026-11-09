// 3D view tests (src/volume/resample.*, render3d.*): MPR, slabs, curved MPR, ray casting against
// slow reference implementations written here, resampling, SIMD differential, fusion, picking,
// robustness and the C API.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include "siaara/siaara.h"
#include "test.hpp"
#include "volume/geometry.hpp"
#include "volume/render3d.hpp"
#include "volume/resample.hpp"
#include "volume/volume.hpp"

using namespace siaara;
using namespace siaara::volume;

namespace {

using Fn = std::function<float(std::uint32_t, std::uint32_t, std::uint32_t)>;

std::shared_ptr<Volume> make_volume(std::array<std::uint32_t, 3> dims, std::array<double, 3> spacing, Vec3 origin,
                                    std::array<Vec3, 3> axes, VoxelType type, const Fn& f) {
    Grid g;
    g.dims = dims;
    g.spacing = spacing;
    g.origin = origin;
    g.axes = axes;
    auto r = Volume::allocate(g, type);
    if (!r) return nullptr;
    auto v = std::make_shared<Volume>(std::move(r).value());
    for (std::uint32_t k = 0; k < dims[2]; ++k)
        for (std::uint32_t j = 0; j < dims[1]; ++j)
            for (std::uint32_t i = 0; i < dims[0]; ++i) v->set(i, j, k, f(i, j, k));
    v->update_statistics(2);
    return v;
}

std::array<Vec3, 3> rotated_axes(double a, double b) {
    // Rotation about z by a, then about x by b.
    const Vec3 x{std::cos(a), std::sin(a), 0}, y{-std::sin(a), std::cos(a), 0}, z{0, 0, 1};
    auto rx = [&](Vec3 v) { return Vec3{v.x, v.y * std::cos(b) - v.z * std::sin(b), v.y * std::sin(b) + v.z * std::cos(b)}; };
    return {rx(x), rx(y), rx(z)};
}

std::array<Vec3, 3> identity_axes() { return {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}; }

// ---- reference implementations (independent of src/volume)
struct Ref {
    const Volume& v;
    Vec3 to_index(Vec3 p) const {
        // Solve M * idx = p - origin by Cramer's rule.
        const Grid& g = v.grid();
        const Vec3 a = g.axes[0] * g.spacing[0], b = g.axes[1] * g.spacing[1], c = g.axes[2] * g.spacing[2];
        const Vec3 d = p - g.origin;
        const double det = dot(a, cross(b, c));
        return {dot(d, cross(b, c)) / det, dot(a, cross(d, c)) / det, dot(a, cross(b, d)) / det};
    }
    bool inside(Vec3 i) const {
        const auto& d = v.grid().dims;
        return i.x >= -0.5 && i.x <= d[0] - 0.5 && i.y >= -0.5 && i.y <= d[1] - 0.5 && i.z >= -0.5 && i.z <= d[2] - 0.5;
    }
    double vox(long long i, long long j, long long k) const {
        const auto& d = v.grid().dims;
        auto cl = [](long long x, long long n) { return x < 0 ? 0 : (x >= n ? n - 1 : x); };
        return v.at(static_cast<std::uint32_t>(cl(i, d[0])), static_cast<std::uint32_t>(cl(j, d[1])),
                    static_cast<std::uint32_t>(cl(k, d[2])));
    }
    double trilinear(Vec3 p) const {
        const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
        const double ax = p.x - fx, ay = p.y - fy, az = p.z - fz;
        const auto x = static_cast<long long>(fx), y = static_cast<long long>(fy), z = static_cast<long long>(fz);
        double s = 0;
        for (int c = 0; c < 8; ++c) {
            const double w = ((c & 1) ? ax : 1 - ax) * ((c & 2) ? ay : 1 - ay) * ((c & 4) ? az : 1 - az);
            s += w * vox(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1));
        }
        return s;
    }
    double nearest(Vec3 p) const {
        return vox(static_cast<long long>(std::floor(p.x + 0.5)), static_cast<long long>(std::floor(p.y + 0.5)),
                   static_cast<long long>(std::floor(p.z + 0.5)));
    }
};

Vec3 screen_point(const View& vw, double u, double v) {
    return vw.plane().point + vw.right() * ((u - (vw.width() - 1) / 2.0) * vw.pixel_spacing()) +
           vw.down() * ((v - (vw.height() - 1) / 2.0) * vw.pixel_spacing());
}

// Ray sample parameters t = k * step inside the volume footprint (reference clipping by brute force
// over a generous range, then the footprint test of every sample).
std::vector<double> ray_ts(const View& vw, const Ref& ref, Vec3 o, double step) {
    std::vector<double> ts;
    const Grid& g = ref.v.grid();
    double diag = 0;
    for (int a = 0; a < 3; ++a) diag += (g.dims[static_cast<std::size_t>(a)] + 1) * g.spacing[static_cast<std::size_t>(a)];
    const double c = dot(g.index_to_patient({(g.dims[0] - 1) / 2.0, (g.dims[1] - 1) / 2.0, (g.dims[2] - 1) / 2.0}) - o, vw.normal());
    const auto k0 = static_cast<long long>(std::floor((c - diag) / step)), k1 = static_cast<long long>(std::ceil((c + diag) / step));
    for (long long k = k0; k <= k1; ++k) {
        const double t = static_cast<double>(k) * step;
        if (ref.inside(ref.to_index(o + vw.normal() * t))) ts.push_back(t);
    }
    return ts;
}

double max_abs_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}

std::vector<std::uint8_t> render(const View& v) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(v.width()) * v.height() * 4);
    Status s = v.render_rgba8(out);
    if (!s) std::fprintf(stderr, "  render failed: %s\n", s.message.c_str());
    return out;
}

std::vector<float> values(const View& v, float outside = -12345.0f) {
    std::vector<float> out(static_cast<std::size_t>(v.width()) * v.height());
    Status s = v.render_values(out, outside);
    if (!s) std::fprintf(stderr, "  render_values failed: %s\n", s.message.c_str());
    return out;
}

const Vec3 kOrigin{-31.5, 12.25, -80.0};

}  // namespace

TEST_CASE("render3d: MPR nearest on grid planes returns exact voxel values") {
    for (VoxelType type : {VoxelType::Int16, VoxelType::Float32}) {
        auto vol = make_volume({11, 9, 7}, {1.5, 1.5, 1.5}, kOrigin, rotated_axes(0.4, 0.2), type,
                               [](std::uint32_t i, std::uint32_t j, std::uint32_t k) { return static_cast<float>((i * 31 + j * 17 + k * 7) % 97); });
        REQUIRE(vol);
        const std::array<std::uint32_t, 3> d = vol->grid().dims;
        for (int axis = 0; axis < 3; ++axis) {
            const std::uint32_t w = axis == 0 ? d[1] : d[0];
            const std::uint32_t h = axis == 2 ? d[1] : d[2];
            auto v = View::create(vol, w, h);
            REQUIRE(v.is_ok());
            REQUIRE(v->set_plane(grid_plane(vol->grid(), axis, 2.0)).is_ok());
            for (Interpolation m : {Interpolation::Nearest, Interpolation::Linear, Interpolation::CubicCatmullRom}) {
                REQUIRE(v->set_mode(ViewMode::Mpr, m).is_ok());
                auto out = values(*v);
                bool exact = true;
                for (std::uint32_t y = 0; y < h; ++y)
                    for (std::uint32_t x = 0; x < w; ++x) {
                        float e = 0;
                        if (axis == 2) e = vol->at(x, y, 2);
                        if (axis == 1) e = vol->at(x, 2, d[2] - 1 - y);
                        if (axis == 0) e = vol->at(2, x, d[2] - 1 - y);
                        if (std::fabs(out[static_cast<std::size_t>(y) * w + x] - e) > 1e-3f) exact = false;
                    }
                CHECK(exact);
            }
        }
    }
}

TEST_CASE("render3d: oblique MPR matches the analytic ramp (linear and cubic) and nearest reference") {
    const double a = 2.0, b = -3.0, c = 5.0;
    auto ramp = make_volume({24, 20, 16}, {1.0, 1.25, 2.0}, kOrigin, rotated_axes(0.3, -0.25), VoxelType::Float32,
                            [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) { return static_cast<float>(a * i + b * j + c * k); });
    REQUIRE(ramp);
    auto v = View::create(ramp, 64, 48);
    REQUIRE(v.is_ok());
    Plane p = standard_plane(ramp->grid(), StandardOrientation::Axial);
    p.normal = {0.3, -0.2, 0.93};
    p.up = {0.1, -1, 0.2};
    p.pixel_spacing = 0.45;
    REQUIRE(v->set_plane(p).is_ok());
    const Ref ref{*ramp};
    for (Interpolation m : {Interpolation::Linear, Interpolation::CubicCatmullRom, Interpolation::CubicBSpline}) {
        REQUIRE(v->set_mode(ViewMode::Mpr, m).is_ok());
        auto out = values(*v);
        int checked = 0;
        double worst = 0;
        for (std::uint32_t y = 0; y < 48; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const Vec3 idx = ref.to_index(screen_point(*v, x, y));
                // Away from the clamped border (cubic needs two voxels).
                if (idx.x < 2 || idx.y < 2 || idx.z < 2 || idx.x > 21 || idx.y > 17 || idx.z > 13) continue;
                ++checked;
                worst = std::max(worst, std::fabs(out[y * 64u + x] - (a * idx.x + b * idx.y + c * idx.z)));
            }
        CHECK(checked > 200);
        CHECK(worst < 2e-3);
    }
    // Checkerboard, nearest, against the reference everywhere (inside or not).
    auto checker = make_volume({16, 16, 16}, {1, 1, 1}, kOrigin, rotated_axes(0.1, 0.5), VoxelType::Int16,
                               [](std::uint32_t i, std::uint32_t j, std::uint32_t k) { return ((i / 2 + j / 2 + k / 2) % 2) ? 100.0f : -50.0f; });
    auto vc = View::create(checker, 40, 40);
    REQUIRE(vc.is_ok());
    Plane q = standard_plane(checker->grid(), StandardOrientation::Coronal);
    q.normal = {0.2, 0.9, 0.3};
    q.pixel_spacing = 0.53;
    REQUIRE(vc->set_plane(q).is_ok());
    REQUIRE(vc->set_mode(ViewMode::Mpr, Interpolation::Nearest).is_ok());
    auto out = values(*vc, -999.0f);
    const Ref rc{*checker};
    int mismatches = 0, inside = 0;
    for (std::uint32_t y = 0; y < 40; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const Vec3 idx = rc.to_index(screen_point(*vc, x, y));
            const double e = rc.inside(idx) ? rc.nearest(idx) : -999.0;
            inside += rc.inside(idx);
            // Points within 1e-9 of a rounding boundary may legitimately differ.
            const double fr = std::fabs(idx.x - std::floor(idx.x) - 0.5) + 0 * idx.y;
            if (out[y * 40u + x] != static_cast<float>(e) && fr > 1e-6) ++mismatches;
        }
    CHECK(inside > 100);
    CHECK(mismatches == 0);
}

TEST_CASE("render3d: thick slab MIP, MinIP and average") {
    auto vol = make_volume({21, 21, 21}, {1.5, 1.5, 1.5}, kOrigin, identity_axes(), VoxelType::Int16,
                           [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
                               if (i == 10 && j == 10 && k == 11) return 1000.0f;
                               if (i == 4 && j == 4 && k == 9) return -500.0f;
                               return 10.0f;
                           });
    auto v = View::create(vol, 21, 21);
    REQUIRE(v.is_ok());
    REQUIRE(v->set_plane(grid_plane(vol->grid(), 2, 10.0)).is_ok());  // bright voxel 1.5 mm off-plane
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Nearest).is_ok());
    CHECK(values(*v)[10 * 21 + 10] == 10.0f);
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Nearest, SlabMode::Mip, 6.0).is_ok());
    auto mip = values(*v);
    CHECK(mip[10 * 21 + 10] == 1000.0f);
    CHECK(mip[0] == 10.0f);
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Nearest, SlabMode::MinIp, 6.0).is_ok());
    CHECK(values(*v)[4 * 21 + 4] == -500.0f);
    // A thin slab misses the voxel 3 mm away.
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Nearest, SlabMode::Mip, 1.0).is_ok());
    CHECK(values(*v)[10 * 21 + 10] == 10.0f);
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Linear, SlabMode::Average, 6.0).is_ok());
    CHECK(std::fabs(values(*v)[15 * 21 + 15] - 10.0f) < 1e-4f);
    // Pick on a slab MIP finds the bright voxel's position.
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Nearest, SlabMode::Mip, 6.0).is_ok());
    auto pk = v->pick(10, 10);
    REQUIRE(pk.is_ok());
    CHECK(pk->value == 1000.0);
    CHECK(std::fabs(pk->index.z - 11.0) <= 0.5);
}

TEST_CASE("render3d: ray-cast MIP equals the maximum along the ray") {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-100.0f, 100.0f);
    auto vol = make_volume({14, 12, 10}, {1.0, 1.2, 1.6}, kOrigin, rotated_axes(0.6, 0.35), VoxelType::Float32,
                           [&](std::uint32_t, std::uint32_t, std::uint32_t) { return u(rng); });
    auto v = View::create(vol, 24, 20);
    REQUIRE(v.is_ok());
    Plane p = standard_plane(vol->grid(), StandardOrientation::Sagittal);
    p.normal = {-0.8, 0.3, 0.5};
    p.pixel_spacing = 0.9;
    REQUIRE(v->set_plane(p).is_ok());
    const Ref ref{*vol};
    for (ViewMode mode : {ViewMode::VolumeMip, ViewMode::VolumeMinIp}) {
        REQUIRE(v->set_mode(mode).is_ok());
        auto fast = values(*v, -9999.0f);
        RenderSettings rs;
        rs.empty_space_skipping = false;
        REQUIRE(v->set_render_settings(rs).is_ok());
        auto noskip = values(*v, -9999.0f);
        REQUIRE(v->set_render_settings(RenderSettings{}).is_ok());
        CHECK(fast == noskip);
        const double step = v->sample_distance(Quality::Final);
        double worst = 0;
        int hits = 0;
        for (std::uint32_t y = 0; y < 20; ++y)
            for (std::uint32_t x = 0; x < 24; ++x) {
                const Vec3 o = screen_point(*v, x, y);
                double best = mode == ViewMode::VolumeMip ? -1e30 : 1e30;
                bool any = false;
                for (double t : ray_ts(*v, ref, o, step)) {
                    const double s = ref.trilinear(ref.to_index(o + v->normal() * t));
                    best = mode == ViewMode::VolumeMip ? std::max(best, s) : std::min(best, s);
                    any = true;
                }
                const float got = fast[y * 24u + x];
                if (!any) {
                    if (got != -9999.0f) worst = 1e9;
                    continue;
                }
                ++hits;
                worst = std::max(worst, std::fabs(got - best));
            }
        CHECK(hits > 100);
        CHECK(worst < 1e-3);
    }
    // MIP pick returns the location of the maximum.
    REQUIRE(v->set_mode(ViewMode::VolumeMip).is_ok());
    auto pk = v->pick(12, 10);
    REQUIRE(pk.is_ok());
    CHECK(pk->hit);
    CHECK(std::fabs(ref.trilinear(pk->index) - pk->value) < 1e-3);
}

namespace {

struct RefTf {
    std::vector<ValuePoint> op;
    std::vector<ColorPoint> col;
    double eval_op(double x) const { return evaluate_piecewise(op, x); }
    double eval_col(double x, int ch) const {
        auto g = [ch](const ColorPoint& p) { return ch == 0 ? p.r : (ch == 1 ? p.g : p.b); };
        if (x <= col.front().value) return g(col.front());
        if (x >= col.back().value) return g(col.back());
        for (std::size_t i = 1; i < col.size(); ++i)
            if (x <= col[i].value) {
                const double f = (x - col[i - 1].value) / (col[i].value - col[i - 1].value);
                return g(col[i - 1]) + (g(col[i]) - g(col[i - 1])) * f;
            }
        return g(col.back());
    }
};

// Slow front-to-back compositing reference, with optional headlight Phong shading.
std::vector<std::uint8_t> reference_vr(const View& v, const Volume& vol, const TransferFunction& tf, double step, double ert) {
    const Ref ref{vol};
    const RefTf rt{tf.opacity, tf.color};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(v.width()) * v.height() * 4);
    const Grid& g = vol.grid();
    for (std::uint32_t y = 0; y < v.height(); ++y)
        for (std::uint32_t x = 0; x < v.width(); ++x) {
            const Vec3 o = screen_point(v, x, y);
            double C[3] = {0, 0, 0}, A = 0;
            for (double t : ray_ts(v, ref, o, step)) {
                const Vec3 idx = ref.to_index(o + v.normal() * t);
                const double s = ref.trilinear(idx);
                double a = std::clamp(rt.eval_op(s), 0.0, 1.0);
                a = 1.0 - std::pow(1.0 - a, step / tf.unit_distance);
                if (a <= 0) continue;
                double col[3] = {rt.eval_col(s, 0), rt.eval_col(s, 1), rt.eval_col(s, 2)};
                if (tf.shading) {
                    // Gradient in patient space by central differences along the index axes.
                    const double gi[3] = {(ref.trilinear(idx + Vec3{1, 0, 0}) - ref.trilinear(idx - Vec3{1, 0, 0})) / 2,
                                          (ref.trilinear(idx + Vec3{0, 1, 0}) - ref.trilinear(idx - Vec3{0, 1, 0})) / 2,
                                          (ref.trilinear(idx + Vec3{0, 0, 1}) - ref.trilinear(idx - Vec3{0, 0, 1})) / 2};
                    // grad_P = M^-T grad_idx with M = [a b c]: solve M^T G = gi.
                    const Vec3 ca = g.axes[0] * g.spacing[0], cb = g.axes[1] * g.spacing[1], cc = g.axes[2] * g.spacing[2];
                    const double det = dot(ca, cross(cb, cc));
                    const Vec3 G = (cross(cb, cc) * gi[0] + cross(cc, ca) * gi[1] + cross(ca, cb) * gi[2]) * (1.0 / det);
                    const double gm = length(G);
                    if (gm > 1e-12) {
                        const double ndl = std::fabs(dot(G, v.normal())) / gm;
                        for (double& cv : col)
                            cv = std::clamp(cv * (tf.ambient + tf.diffuse * ndl) + tf.specular * std::pow(ndl, tf.shininess), 0.0, 1.0);
                    }
                }
                const double w = (1 - A) * a;
                for (int ch = 0; ch < 3; ++ch) C[ch] += w * col[ch];
                A += w;
                if (A >= ert) break;
            }
            std::uint8_t* px = &out[(static_cast<std::size_t>(y) * v.width() + x) * 4];
            for (int ch = 0; ch < 3; ++ch) px[ch] = static_cast<std::uint8_t>(std::clamp(C[ch], 0.0, 1.0) * 255.0 + 0.5);
            px[3] = 255;
        }
    return out;
}

}  // namespace

TEST_CASE("render3d: volume rendering matches the slow reference; skipping and termination") {
    // Smooth blob: a sphere with soft edge inside a zero background (so empty-space skipping has work).
    auto vol = make_volume({24, 22, 20}, {1.0, 1.0, 1.4}, kOrigin, rotated_axes(0.25, 0.4), VoxelType::Int16,
                           [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
                               const double r = std::sqrt((i - 11.5) * (i - 11.5) + (j - 10.0) * (j - 10.0) + (k - 9.0) * (k - 9.0) * 1.96);
                               return static_cast<float>(std::clamp(1000.0 * (8.0 - r) / 3.0, 0.0, 1000.0));
                           });
    auto v = View::create(vol, 32, 28);
    REQUIRE(v.is_ok());
    Plane p = standard_plane(vol->grid(), StandardOrientation::Coronal);
    p.normal = {0.3, 0.9, -0.2};
    p.pixel_spacing = 1.0;
    REQUIRE(v->set_plane(p).is_ok());
    TransferFunction tf;
    tf.opacity = {{100, 0.0}, {600, 0.3}, {1000, 0.8}};
    tf.color = {{0, 1.0, 0.2, 0.1}, {500, 0.9, 0.8, 0.3}, {1000, 1.0, 1.0, 1.0}};
    tf.shading = false;
    tf.unit_distance = 2.0;
    RenderSettings rs;
    rs.early_termination = 2.0;  // never
    REQUIRE(v->set_render_settings(rs).is_ok());
    REQUIRE(v->set_transfer_function(tf).is_ok());
    REQUIRE(v->set_mode(ViewMode::VolumeRender).is_ok());
    const double step = v->sample_distance(Quality::Final);
    auto fast = render(*v);
    auto ref = reference_vr(*v, *vol, tf, step, 2.0);
    CHECK(max_abs_diff(fast, ref) <= 3.0);
    long long nonbg = 0;
    for (std::size_t i = 0; i < fast.size(); i += 4) nonbg += fast[i] > 0;
    CHECK(nonbg > 50);
    // Shaded.
    tf.shading = true;
    REQUIRE(v->set_transfer_function(tf).is_ok());
    CHECK(max_abs_diff(render(*v), reference_vr(*v, *vol, tf, step, 2.0)) <= 4.0);
    // Empty-space skipping changes nothing.
    auto skip = render(*v);
    rs.empty_space_skipping = false;
    REQUIRE(v->set_render_settings(rs).is_ok());
    CHECK(render(*v) == skip);
    // Early termination at 0.99 stays close; fully opaque rays end early.
    rs.empty_space_skipping = true;
    rs.early_termination = 0.99;
    REQUIRE(v->set_render_settings(rs).is_ok());
    CHECK(max_abs_diff(render(*v), skip) <= 4.0);
    // Threads do not change the image.
    rs.threads = 1;
    REQUIRE(v->set_render_settings(rs).is_ok());
    auto one = render(*v);
    rs.threads = 4;
    REQUIRE(v->set_render_settings(rs).is_ok());
    CHECK(render(*v) == one);
    // Interactive quality is close to final on average.
    v->set_quality(Quality::Interactive);
    auto inter = render(*v);
    double mean = 0;
    for (std::size_t i = 0; i < inter.size(); ++i) mean += std::fabs(static_cast<double>(inter[i]) - one[i]);
    CHECK(mean / static_cast<double>(inter.size()) < 12.0);
    v->set_quality(Quality::Final);
    // Gradient opacity: a factor of 0 everywhere makes the image background.
    tf.gradient_opacity = {{0, 0.0}, {1e9, 0.0}};
    REQUIRE(v->set_transfer_function(tf).is_ok());
    auto blank = render(*v);
    CHECK(std::all_of(blank.begin(), blank.end(), [n = 0](std::uint8_t c) mutable { return (n++ % 4 == 3) ? c == 255 : c == 0; }));
    // VR pick hits the blob surface.
    tf.gradient_opacity.clear();
    REQUIRE(v->set_transfer_function(tf).is_ok());
    auto pk = v->pick(15.5, 13.5);
    REQUIRE(pk.is_ok());
    CHECK(pk->hit);
    CHECK(pk->inside);
    CHECK(pk->value > 100.0);
    auto miss = v->pick(0, 0);
    REQUIRE(miss.is_ok());
    CHECK(!miss->hit);
}

TEST_CASE("render3d: resampling identity is exact; SIMD kernel matches the scalar reference") {
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(-1000.0f, 1000.0f);
    auto vol = make_volume({17, 13, 11}, {0.9, 1.1, 2.0}, kOrigin, rotated_axes(0.2, 0.7), VoxelType::Float32,
                           [&](std::uint32_t, std::uint32_t, std::uint32_t) { return u(rng); });
    for (Interpolation m : {Interpolation::Nearest, Interpolation::Linear, Interpolation::CubicCatmullRom}) {
        ResampleOptions o;
        o.interpolation = m;
        auto r = resample(*vol, vol->grid(), o);
        REQUIRE(r.is_ok());
        CHECK(std::memcmp(r->data(), vol->data(), static_cast<std::size_t>(vol->byte_size())) == 0);
    }
    // Shift by whole voxels: exact for nearest/linear/cubic in the overlap; outside value elsewhere.
    Grid sh = vol->grid();
    sh.origin = vol->grid().index_to_patient({2, 1, 3});
    ResampleOptions o;
    o.outside_value = -7.0;
    auto r = resample(*vol, sh, o);
    REQUIRE(r.is_ok());
    CHECK(r->at(0, 0, 0) == vol->at(2, 1, 3));
    CHECK(r->at(14, 11, 7) == vol->at(16, 12, 10));
    CHECK(r->at(16, 12, 10) == -7.0f);
    // NaN as the outside value is kept for float32 output (pre-resampled fusion layers rely on it).
    o.outside_value = std::numeric_limits<double>::quiet_NaN();
    auto rn = resample(*vol, sh, o);
    REQUIRE(rn.is_ok());
    CHECK(std::isnan(rn->at(16, 12, 10)));
    CHECK(rn->at(0, 0, 0) == vol->at(2, 1, 3));
    CHECK(std::isfinite(rn->value_min) && std::isfinite(rn->value_max));
    // int16 source onto a rotated finer grid: SIMD vs scalar.
    auto vi = make_volume({33, 29, 21}, {1, 1, 1}, kOrigin, identity_axes(), VoxelType::Int16,
                          [&](std::uint32_t, std::uint32_t, std::uint32_t) { return std::nearbyint(u(rng)); });
    for (const auto& src : {vol, vi}) {
        Grid t;
        t.dims = {40, 37, 23};
        t.spacing = {0.7, 0.8, 0.9};
        t.axes = rotated_axes(-0.3, 0.15);
        t.origin = src->grid().index_to_patient({-1.3, 0.4, 0.2});
        ResampleOptions lo;
        lo.interpolation = Interpolation::Linear;
        set_simd_enabled(true);
        auto a = resample(*src, t, lo);
        set_simd_enabled(false);
        auto b = resample(*src, t, lo);
        set_simd_enabled(true);
        REQUIRE(a.is_ok() && b.is_ok());
        double worst = 0;
        long long inside = 0;
        for (std::size_t i = 0; i < a->voxel_count(); ++i) {
            worst = std::max(worst, std::fabs(static_cast<double>(a->f32() ? a->f32()[i] : a->i16()[i]) -
                                              static_cast<double>(b->f32() ? b->f32()[i] : b->i16()[i])));
            inside += (a->f32() ? a->f32()[i] : a->i16()[i]) != static_cast<float>(src->value_min);
        }
        CHECK(worst <= 1e-3);
        CHECK(inside > 1000);
        // Reference trilinear for a few voxels.
        const Ref ref{*src};
        double rw = 0;
        for (std::uint32_t k = 0; k < 23; k += 5)
            for (std::uint32_t j = 0; j < 37; j += 6)
                for (std::uint32_t i = 0; i < 40; i += 7) {
                    const Vec3 idx = ref.to_index(t.index_to_patient({1.0 * i, 1.0 * j, 1.0 * k}));
                    const double e = ref.inside(idx) ? ref.trilinear(idx) : src->value_min;
                    double got = b->at(i, j, k);
                    if (src->type() == VoxelType::Int16) rw = std::max(rw, std::fabs(got - e) - 0.5);
                    else rw = std::max(rw, std::fabs(got - e));
                }
        CHECK(rw < 1e-2);
    }
    CHECK(simd_available() == (sizeof(void*) == 8 || simd_available()));
    auto iso = isotropic_grid(vol->grid(), 0.5);
    REQUIRE(iso.is_ok());
    CHECK(iso->dims[0] == 29 && iso->dims[2] == 41);
    CHECK(!isotropic_grid(vol->grid(), 0.0).is_ok());
}

TEST_CASE("render3d: fusion of two volumes with different geometries") {
    // Base: CT-like ramp on a 1 mm grid. Overlay: PET-like ramp on a coarser rotated grid, both
    // defined by the same patient-space function, so the overlay value at a pick is analytic.
    auto fpat = [](Vec3 p) { return 3.0 * p.x - 2.0 * p.y + 0.5 * p.z + 400.0; };
    Grid bg;
    bg.dims = {30, 30, 20};
    bg.origin = kOrigin;
    auto base = make_volume(bg.dims, {1, 1, 1}, kOrigin, identity_axes(), VoxelType::Int16,
                            [](std::uint32_t i, std::uint32_t j, std::uint32_t) { return static_cast<float>(i * 10 + j); });
    const auto oaxes = rotated_axes(0.5, 0.3);
    const Vec3 oorigin = kOrigin + Vec3{-3, -4, -2};
    Grid og;
    og.dims = {20, 20, 14};
    og.spacing = {2.0, 2.0, 2.5};
    og.axes = oaxes;
    og.origin = oorigin;
    auto overlay = make_volume(og.dims, og.spacing, oorigin, oaxes, VoxelType::Float32,
                               [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
                                   return static_cast<float>(fpat(og.index_to_patient({1.0 * i, 1.0 * j, 1.0 * k})));
                               });
    REQUIRE(base && overlay);
    auto v = View::create(base, 30, 30);
    REQUIRE(v.is_ok());
    REQUIRE(v->set_plane(grid_plane(base->grid(), 2, 9.0)).is_ok());
    LayerStyle st;
    st.colormap = ColorMap::HotIron;
    st.opacity = 0.4;
    st.window_center = 250;
    st.window_width = 200;
    REQUIRE(v->set_fusion_layer(overlay, st).is_ok());
    REQUIRE(v->set_window(0, 150, 300).is_ok());
    auto pk = v->pick(12, 14);
    REQUIRE(pk.is_ok());
    CHECK(pk->overlay_inside);
    CHECK(std::fabs(pk->overlay_value - fpat(pk->patient)) < 1e-2);
    CHECK(pk->value == 134.0);
    CHECK(pk->overlay_value > 200 && pk->overlay_value < 340);  // inside the overlay window
    // Pixel colour: base gray blended with the overlay colour.
    auto img = render(*v);
    const std::uint8_t* px = &img[(14u * 30 + 12) * 4];
    const double tb = std::clamp((134.0 - (150 - 0.5)) / 299.0 + 0.5, 0.0, 1.0);
    const auto gb = static_cast<int>(tb * 255 + 0.5);
    const double to = std::clamp((pk->overlay_value - (250 - 0.5)) / 199.0 + 0.5, 0.0, 1.0);
    const auto go = static_cast<int>(to * 255 + 0.5);
    std::array<std::uint8_t, 3> pal{};
    {
        // Hot Iron palette entry through a separate one-layer view rendering a known gray.
        auto flat = make_volume({1, 1, 1}, {1, 1, 1}, {}, identity_axes(), VoxelType::Float32,
                                [&](std::uint32_t, std::uint32_t, std::uint32_t) { return static_cast<float>(go); });
        auto fv = View::create(flat, 1, 1);
        REQUIRE(fv.is_ok());
        REQUIRE(fv->set_colormap(0, ColorMap::HotIron).is_ok());
        REQUIRE(fv->set_window(0, 128.0, 256.0).is_ok());  // LINEAR: gray = value exactly
        REQUIRE(fv->set_plane(grid_plane(flat->grid(), 2, 0)).is_ok());
        auto one = render(*fv);
        pal = {one[0], one[1], one[2]};
    }
    for (int ch = 0; ch < 3; ++ch) CHECK(std::abs(px[ch] - static_cast<int>(gb * 0.6 + pal[static_cast<std::size_t>(ch)] * 0.4 + 0.5)) <= 2);
    // Pre-resampled overlay gives (nearly) the same image.
    REQUIRE(v->set_fusion_layer(overlay, st, true).is_ok());
    CHECK(max_abs_diff(render(*v), img) <= 2.0);
    // Outside the overlay: base only.
    REQUIRE(v->set_fusion_layer(nullptr, st).is_ok());
    CHECK(!v->pick(12, 14)->overlay_inside);
    CHECK(v->set_window(1, 0, 10).code == Errc::InvalidArgument);
}

TEST_CASE("render3d: pick and project round trip") {
    auto vol = make_volume({20, 18, 16}, {0.8, 0.8, 1.2}, kOrigin, rotated_axes(0.7, 0.1), VoxelType::Float32,
                           [](std::uint32_t i, std::uint32_t j, std::uint32_t k) { return static_cast<float>(i + 100 * j + 10000 * k); });
    auto v = View::create(vol, 50, 40);
    REQUIRE(v.is_ok());
    Plane p = standard_plane(vol->grid(), StandardOrientation::Sagittal);
    p.normal = {-0.7, 0.2, 0.5};
    p.up = {0, 0.3, 1};
    p.pixel_spacing = 0.37;
    REQUIRE(v->set_plane(p).is_ok());
    REQUIRE(v->set_mode(ViewMode::Mpr, Interpolation::Linear).is_ok());
    auto vals = values(*v, -1.0f);
    int checked = 0;
    for (double y : {3.0, 17.25, 33.5})
        for (double x : {5.0, 21.75, 44.0}) {
            auto pk = v->pick(x, y);
            REQUIRE(pk.is_ok());
            CHECK(pk->hit);
            auto back = v->project(pk->patient);
            REQUIRE(back.is_ok());
            CHECK(std::fabs(back->x - x) < 1e-9 && std::fabs(back->y - y) < 1e-9 && std::fabs(back->z) < 1e-9);
            const Vec3 again = vol->grid().index_to_patient(pk->index);
            CHECK(std::fabs(again.x - pk->patient.x) < 1e-9 && std::fabs(again.z - pk->patient.z) < 1e-9);
            if (pk->inside && x == std::floor(x) && y == std::floor(y)) {
                CHECK(std::fabs(vals[static_cast<std::size_t>(y) * 50 + static_cast<std::size_t>(x)] - pk->value) < 1e-2);
                ++checked;
            }
        }
    CHECK(checked > 0);
    // Crosshair sync: a point picked in one view projects onto the other view's plane through it.
    auto v2 = View::create(vol, 50, 40);
    REQUIRE(v2.is_ok());
    const auto pk = v->pick(21.75, 17.25);
    Plane q = standard_plane(vol->grid(), StandardOrientation::Axial);
    q.point = pk->patient;
    q.pixel_spacing = 0.5;
    REQUIRE(v2->set_plane(q).is_ok());
    auto s2 = v2->project(pk->patient);
    REQUIRE(s2.is_ok());
    CHECK(std::fabs(s2->x - 24.5) < 1e-9 && std::fabs(s2->y - 19.5) < 1e-9);
    auto pk2 = v2->pick(s2->x, s2->y);
    CHECK(std::fabs(pk2->patient.x - pk->patient.x) < 1e-9 && std::fabs(pk2->patient.y - pk->patient.y) < 1e-9);
}

TEST_CASE("render3d: curved MPR (straightened and stretched)") {
    // Ramp along x only (patient space = index space scaled by 1 mm).
    auto vol = make_volume({40, 30, 20}, {1, 1, 1}, {0, 0, 0}, identity_axes(), VoxelType::Float32,
                           [](std::uint32_t i, std::uint32_t j, std::uint32_t k) { return static_cast<float>(i + 1000 * j + 0 * k); });
    auto v = View::create(vol, 30, 11);
    REQUIRE(v.is_ok());
    Plane p;
    p.point = {20, 15, 10};
    p.normal = {0, 0, 1};
    p.up = {0, -1, 0};  // down = +y
    p.pixel_spacing = 1.0;
    REQUIRE(v->set_plane(p).is_ok());
    const std::vector<Vec3> path{{5, 15, 10}, {20, 15, 10}, {32, 15, 10}};
    REQUIRE(v->set_curve(path, false).is_ok());
    REQUIRE(v->set_mode(ViewMode::CurvedStraightened, Interpolation::Linear).is_ok());
    auto out = values(*v, -1.0f);
    // Column u is at x = 5 + u (path length 27 -> columns 0..27), row r at y = 15 + (r - 5).
    CHECK(std::fabs(out[5 * 30 + 0] - (5 + 15000)) < 1e-3);
    CHECK(std::fabs(out[5 * 30 + 27] - (32 + 15000)) < 1e-3);
    CHECK(std::fabs(out[8 * 30 + 10] - (15 + 18000)) < 1e-3);
    CHECK(out[5 * 30 + 28] == -1.0f);
    // The spline through collinear, evenly spaced points: still the straight line.
    const std::vector<Vec3> even{{5, 15, 10}, {15, 15, 10}, {25, 15, 10}};
    REQUIRE(v->set_curve(even, true).is_ok());
    auto sp = values(*v, -1.0f);
    CHECK(std::fabs(sp[5 * 30 + 12] - (17 + 15000)) < 1e-2);
    // An L-shaped path, stretched with down = +y: the leg along y has no projected length, so the
    // panorama is only the x extent, and the rows sample along y.
    const std::vector<Vec3> ell{{5, 10, 10}, {15, 10, 10}, {15, 20, 10}, {25, 20, 10}};
    REQUIRE(v->set_curve(ell, false).is_ok());
    REQUIRE(v->set_mode(ViewMode::CurvedStretched, Interpolation::Linear).is_ok());
    auto st = values(*v, -1.0f);
    CHECK(std::fabs(st[5 * 30 + 3] - (8 + 10000)) < 1e-3);
    CHECK(std::fabs(st[5 * 30 + 15] - (20 + 20000)) < 1e-3);
    CHECK(st[5 * 30 + 21] == -1.0f);
    // Straightened on the same path keeps the 3D arc length (10 + 10 + 10 = 30 columns).
    REQUIRE(v->set_mode(ViewMode::CurvedStraightened, Interpolation::Linear).is_ok());
    auto s3 = values(*v, -1.0f);
    CHECK(std::fabs(s3[5 * 30 + 15] - (15 + 15000)) < 1e-3);
    CHECK(s3[5 * 30 + 29] != -1.0f);
    auto pk = v->pick(15, 5);
    REQUIRE(pk.is_ok());
    CHECK(std::fabs(pk->patient.x - 15) < 1e-9 && std::fabs(pk->patient.y - 15) < 1e-9);
    CHECK(v->project({1, 2, 3}).error().code == Errc::Unsupported);
    auto img = render(*v);
    CHECK(img.size() == 30u * 11u * 4u);
    CHECK(v->set_curve(std::vector<Vec3>{{1, 2, 3}}, false).code == Errc::InvalidArgument);
}

TEST_CASE("render3d: robustness (degenerate inputs, NaN, bad arguments)") {
    auto one = make_volume({8, 8, 1}, {1, 1, 1}, kOrigin, identity_axes(), VoxelType::Int16,
                           [](std::uint32_t i, std::uint32_t, std::uint32_t) { return static_cast<float>(i); });
    REQUIRE(one);
    auto v = View::create(one, 16, 16);
    REQUIRE(v.is_ok());
    for (ViewMode m : {ViewMode::Mpr, ViewMode::VolumeRender, ViewMode::VolumeMip, ViewMode::VolumeMinIp}) {
        REQUIRE(v->set_mode(m).is_ok());
        auto img = render(*v);
        CHECK(img[3] == 255);
        v->set_quality(Quality::Interactive);
        (void)render(*v);
        v->set_quality(Quality::Final);
    }
    // Volumes with NaN voxels: outputs stay finite / valid.
    auto nanv = make_volume({6, 6, 6}, {1, 1, 1}, kOrigin, identity_axes(), VoxelType::Float32,
                            [](std::uint32_t i, std::uint32_t, std::uint32_t) { return i == 2 ? std::numeric_limits<float>::quiet_NaN() : 5.0f; });
    CHECK(nanv->value_min == 5.0 && nanv->value_max == 5.0);
    auto vn = View::create(nanv, 12, 12);
    REQUIRE(vn.is_ok());
    for (ViewMode m : {ViewMode::Mpr, ViewMode::VolumeRender, ViewMode::VolumeMip}) {
        REQUIRE(vn->set_mode(m).is_ok());
        (void)render(*vn);
        auto pk = vn->pick(6, 6);
        REQUIRE(pk.is_ok());
        CHECK(std::isfinite(pk->value));
    }
    Plane bad;
    bad.normal = {0, 0, 1};
    bad.up = {0, 0, 2};
    CHECK(v->set_plane(bad).code == Errc::InvalidArgument);
    bad.up = {0, 1, 0};
    bad.point = {std::nan(""), 0, 0};
    CHECK(v->set_plane(bad).code == Errc::InvalidArgument);
    bad.point = {};
    bad.normal = {};
    CHECK(v->set_plane(bad).code == Errc::InvalidArgument);
    CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Linear, SlabMode::Mip, -1).code == Errc::InvalidArgument);
    CHECK(v->set_size(0, 4).code == Errc::InvalidArgument);
    CHECK(!View::create(nullptr, 4, 4).is_ok());
    TransferFunction tf;
    tf.opacity = {{10, 0.5}, {5, 0.2}};
    CHECK(v->set_transfer_function(tf).code == Errc::InvalidArgument);
    tf.opacity = {{5, 2.0}};
    CHECK(v->set_transfer_function(tf).code == Errc::InvalidArgument);
    std::vector<std::uint8_t> small(10);
    CHECK(v->render_rgba8(small).code == Errc::BufferTooSmall);
    CHECK(!v->pick(std::nan(""), 0).is_ok());
    RenderSettings rs;
    rs.interactive_downsample = 0;
    CHECK(v->set_render_settings(rs).code == Errc::InvalidArgument);
    // A pick far outside the volume is not inside.
    REQUIRE(v->set_mode(ViewMode::Mpr).is_ok());
    auto far = v->pick(-1000, -1000);
    REQUIRE(far.is_ok());
    CHECK(!far->inside);
}

TEST_CASE("render3d: C API argument checks") {
    siaara_view* view = nullptr;
    CHECK(siaara_view_create(nullptr, 4, 4, &view) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_view_render_rgba8(nullptr, nullptr, 0, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_pick_result pr{};
    pr.struct_size = sizeof pr;
    CHECK(siaara_view_pick(nullptr, 0, 0, &pr) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_volume_info(nullptr, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_view_free(nullptr);
}
