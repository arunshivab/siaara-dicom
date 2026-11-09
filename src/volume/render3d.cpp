// SIAARA DICOM Toolkit - 3D views (see render3d.hpp).
#include "volume/render3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "gpu/gpu.hpp"
#include "image/color.hpp"
#include "volume/parallel.hpp"
#include "volume/resample.hpp"

namespace siaara::volume {

namespace {

using detail::VoxelAccess;
using Rgb = std::array<std::uint8_t, 3>;
using ColorTable = std::array<Rgb, 256>;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr std::uint32_t kTfEntries = 4096;
constexpr std::uint32_t kTile = 32;

ColorTable make_colormap(ColorMap m) {
    ColorTable t{};
    for (int g = 0; g < 256; ++g) {
        const auto v = static_cast<std::uint8_t>(g);
        switch (m) {
            case ColorMap::Gray: t[static_cast<std::size_t>(g)] = {v, v, v}; break;
            case ColorMap::InverseGray: {
                const auto iv = static_cast<std::uint8_t>(255 - g);
                t[static_cast<std::size_t>(g)] = {iv, iv, iv};
                break;
            }
            default: {
                image::StandardPalette p = image::StandardPalette::HotIron;
                if (m == ColorMap::Pet) p = image::StandardPalette::Pet;
                if (m == ColorMap::HotMetalBlue) p = image::StandardPalette::HotMetalBlue;
                if (m == ColorMap::Pet20Step) p = image::StandardPalette::Pet20Step;
                const image::PaletteLut& lut = image::standard_palette(p);
                const std::uint32_t n = lut.entries ? lut.entries : 1u;
                const std::uint32_t e = (static_cast<std::uint32_t>(g) * (n - 1u) + 127u) / 255u;
                t[static_cast<std::size_t>(g)] = {lut.to8(lut.red[e]), lut.to8(lut.green[e]), lut.to8(lut.blue[e])};
            }
        }
    }
    return t;
}

const ColorTable& colormap(ColorMap m) {
    static const ColorTable maps[6] = {make_colormap(ColorMap::Gray),   make_colormap(ColorMap::InverseGray),
                                       make_colormap(ColorMap::HotIron), make_colormap(ColorMap::Pet),
                                       make_colormap(ColorMap::HotMetalBlue), make_colormap(ColorMap::Pet20Step)};
    return maps[static_cast<int>(m)];
}

// Window as PS3.3 C.11.2.1.2 LINEAR (width > 1) or C.11.2.1.3.2 LINEAR_EXACT (width <= 1), to 0..1.
struct WinMap {
    double a = 0.0, b = 0.0;
    double t(double x) const noexcept {
        const double v = x * a + b;
        return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;  // NaN -> 0
    }
    bool below(double x) const noexcept { return !(x * a + b > 0.0); }
    std::uint8_t gray(double x) const noexcept { return static_cast<std::uint8_t>(std::lround(t(x) * 255.0)); }  // t: 0..1
};

WinMap make_window(const LayerStyle& s, const Volume& v) {
    double c = s.window_center, w = s.window_width;
    if (!std::isfinite(c) || !std::isfinite(w) || !(w > 0.0)) {
        c = 0.5 * (v.value_min + v.value_max);
        w = v.value_max - v.value_min;
        if (!(w > 0.0)) w = 1.0;
    }
    WinMap m;
    if (w > 1.0) {
        m.a = 1.0 / (w - 1.0);
        m.b = 0.5 - (c - 0.5) / (w - 1.0);
    } else {
        m.a = 1.0 / w;
        m.b = 0.5 - c / w;
    }
    return m;
}

// One sampled layer: the volume, its index transform and the voxel accessor of its type.
struct Layer {
    const Volume* vol = nullptr;
    GridTransform t;
    VoxelAccess<std::int16_t> ai;
    VoxelAccess<float> af;
    bool i16 = false;

    Layer() = default;
    Layer(const Volume& v, const GridTransform& tr) : vol(&v), t(tr), ai(v.i16(), v.grid()), af(v.f32(), v.grid()),
                                                      i16(v.type() == VoxelType::Int16) {}
    void row(Interpolation m, Vec3 s, Vec3 d, std::size_t n, float* out) const noexcept {
        if (i16) detail::sample_row(ai, m, s.x, s.y, s.z, d.x, d.y, d.z, n, kNaN, out);
        else detail::sample_row(af, m, s.x, s.y, s.z, d.x, d.y, d.z, n, kNaN, out);
    }
    float at(Interpolation m, Vec3 p) const noexcept {
        if (i16) return ai.inside(p.x, p.y, p.z) ? detail::sample(ai, m, p.x, p.y, p.z) : kNaN;
        return af.inside(p.x, p.y, p.z) ? detail::sample(af, m, p.x, p.y, p.z) : kNaN;
    }
    float linear(Vec3 p) const noexcept {
        return i16 ? detail::sample_linear_fast(ai, p.x, p.y, p.z) : detail::sample_linear_fast(af, p.x, p.y, p.z);
    }
    bool inside(Vec3 p) const noexcept { return ai.inside(p.x, p.y, p.z); }
};

// Ray / box [-0.5, n - 0.5]^3 in index space: idx(t) = s + t * d. False when missed.
bool clip_ray(Vec3 s, Vec3 d, const Grid& g, double& t0, double& t1) noexcept {
    t0 = -std::numeric_limits<double>::infinity();
    t1 = std::numeric_limits<double>::infinity();
    for (int a = 0; a < 3; ++a) {
        const double lo = -0.5, hi = g.dims[static_cast<std::size_t>(a)] - 0.5;
        if (std::fabs(d[a]) < 1e-300) {
            if (s[a] < lo || s[a] > hi) return false;
            continue;
        }
        double ta = (lo - s[a]) / d[a], tb = (hi - s[a]) / d[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    return t0 <= t1 && std::isfinite(t0) && std::isfinite(t1);
}

std::uint8_t to8(double v) noexcept {
    const double c = v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
    return static_cast<std::uint8_t>(std::lround(c * 255.0));
}

double eval_color(const std::vector<ColorPoint>& pts, double x, int ch) noexcept {
    auto get = [ch](const ColorPoint& p) { return ch == 0 ? p.r : (ch == 1 ? p.g : p.b); };
    if (pts.empty()) return 1.0;
    if (!(x > pts.front().value)) return get(pts.front());
    if (!(x < pts.back().value)) return get(pts.back());
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (x <= pts[i].value) {
            const double span = pts[i].value - pts[i - 1].value;
            const double f = span > 0.0 ? (x - pts[i - 1].value) / span : 1.0;
            return get(pts[i - 1]) + (get(pts[i]) - get(pts[i - 1])) * f;
        }
    }
    return get(pts.back());
}

// Precomputed curved path: per output column, the path point and the offset direction.
struct CurveColumns {
    std::vector<char> valid;
    std::vector<Vec3> c, l;
};

}  // namespace

namespace detail {
void window_coefficients(const LayerStyle& s, const Volume& v, double& a, double& b) noexcept {
    const WinMap m = make_window(s, v);
    a = m.a;
    b = m.b;
}
std::array<std::uint8_t, 3> colormap_rgb(ColorMap m, std::uint32_t entry) noexcept {
    return colormap(m)[std::min(entry, 255u)];
}
}  // namespace detail

double evaluate_piecewise(std::span<const ValuePoint> pts, double x) noexcept {
    if (pts.empty()) return 0.0;
    if (!(x > pts.front().value)) return pts.front().y;
    if (!(x < pts.back().value)) return pts.back().y;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (x <= pts[i].value) {
            const double span = pts[i].value - pts[i - 1].value;
            const double f = span > 0.0 ? (x - pts[i - 1].value) / span : 1.0;
            return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * f;
        }
    }
    return pts.back().y;
}

// ---------------------------------------------------------------- planes
Plane standard_plane(const Grid& g, StandardOrientation o) {
    Plane p;
    p.point = g.index_to_patient({(g.dims[0] - 1.0) / 2.0, (g.dims[1] - 1.0) / 2.0, (g.dims[2] - 1.0) / 2.0});
    switch (o) {
        case StandardOrientation::Axial: p.normal = {0, 0, 1}; p.up = {0, -1, 0}; break;
        case StandardOrientation::Coronal: p.normal = {0, 1, 0}; p.up = {0, 0, 1}; break;
        case StandardOrientation::Sagittal: p.normal = {-1, 0, 0}; p.up = {0, 0, 1}; break;
    }
    p.pixel_spacing = 0.0;
    return p;
}

Plane grid_plane(const Grid& g, int axis, double index) {
    Plane p;
    Vec3 c{(g.dims[0] - 1.0) / 2.0, (g.dims[1] - 1.0) / 2.0, (g.dims[2] - 1.0) / 2.0};
    Vec3 right, down;
    double ps = 1.0;
    if (axis == 0) {
        c.x = index;
        right = g.axes[1];
        down = -g.axes[2];
        ps = std::min(g.spacing[1], g.spacing[2]);
    } else if (axis == 1) {
        c.y = index;
        right = g.axes[0];
        down = -g.axes[2];
        ps = std::min(g.spacing[0], g.spacing[2]);
    } else {
        c.z = index;
        right = g.axes[0];
        down = g.axes[1];
        ps = std::min(g.spacing[0], g.spacing[1]);
    }
    p.point = g.index_to_patient(c);
    p.normal = normalized(cross(right, down)).value_or(Vec3{0, 0, 1});
    p.up = -down;
    p.pixel_spacing = ps;
    return p;
}

// ---------------------------------------------------------------- View setup
Result<View> View::create(std::shared_ptr<const Volume> base, std::uint32_t width, std::uint32_t height) {
    if (!base || base->voxel_count() == 0) return Status(Errc::InvalidArgument, "view: no volume");
    auto t = GridTransform::make(base->grid());
    if (!t) return Status(Errc::InvalidArgument, "view: volume grid is degenerate");
    View v;
    v.base_ = std::move(base);
    v.bt_ = *t;
    if (Status s = v.set_size(width, height); !s) return s;
    v.plane_ = grid_plane(v.base_->grid(), 2, (v.base_->grid().dims[2] - 1.0) / 2.0);
    v.plane_.pixel_spacing = 0.0;
    if (Status s = v.update_plane(); !s) return s;
    TransferFunction tf;
    tf.opacity = {{v.base_->value_min, 0.0}, {v.base_->value_max, 0.5}};
    if (Status s = v.set_transfer_function(tf); !s) return s;
    return v;
}

Status View::set_size(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return Status(Errc::InvalidArgument, "view: size must be 1..16384 pixels");
    w_ = width;
    h_ = height;
    if (base_) return update_plane();
    return Status::ok();
}

Status View::set_plane(const Plane& p) {
    if (!finite(p.point) || !finite(p.normal) || !finite(p.up) || !std::isfinite(p.pixel_spacing) || p.pixel_spacing < 0.0)
        return Status(Errc::InvalidArgument, "view: plane values must be finite");
    const Plane old = plane_;
    plane_ = p;
    if (Status s = update_plane(); !s) {
        plane_ = old;
        (void)update_plane();
        return s;
    }
    return Status::ok();
}

Status View::update_plane() {
    auto n = normalized(plane_.normal);
    if (!n) return Status(Errc::InvalidArgument, "view: normal must not be zero");
    auto up = normalized(plane_.up - *n * dot(plane_.up, *n));
    if (!up) return Status(Errc::InvalidArgument, "view: up must not be parallel to the normal");
    n_ = *n;
    right_ = cross(n_, *up);
    down_ = -*up;
    if (plane_.pixel_spacing > 0.0) {
        ps_ = plane_.pixel_spacing;
    } else {
        // Fit: the volume box (voxel footprints) projected around the plane point.
        const Grid& g = base_->grid();
        double er = 0.0, ed = 0.0;
        for (int c = 0; c < 8; ++c) {
            const Vec3 idx{(c & 1) ? g.dims[0] - 0.5 : -0.5, (c & 2) ? g.dims[1] - 0.5 : -0.5, (c & 4) ? g.dims[2] - 0.5 : -0.5};
            const Vec3 d = g.index_to_patient(idx) - plane_.point;
            er = std::max(er, std::fabs(dot(d, right_)));
            ed = std::max(ed, std::fabs(dot(d, down_)));
        }
        ps_ = std::max(2.0 * er / w_, 2.0 * ed / h_);
        if (!(ps_ > 0.0) || !std::isfinite(ps_)) ps_ = 1.0;
    }
    return Status::ok();
}

Status View::set_mode(ViewMode mode, Interpolation interp, SlabMode slab, double slab_thickness) {
    if (!std::isfinite(slab_thickness) || slab_thickness < 0.0) return Status(Errc::InvalidArgument, "view: bad slab thickness");
    if (static_cast<int>(mode) > static_cast<int>(ViewMode::VolumeMinIp) ||
        static_cast<int>(interp) > static_cast<int>(Interpolation::CubicBSpline) ||
        static_cast<int>(slab) > static_cast<int>(SlabMode::Average))
        return Status(Errc::InvalidArgument, "view: unknown mode");
    mode_ = mode;
    interp_ = interp;
    slab_ = slab;
    slab_thickness_ = slab_thickness;
    if (mode == ViewMode::VolumeRender || mode == ViewMode::VolumeMip || mode == ViewMode::VolumeMinIp)
        return prepare_volume_rendering();
    return Status::ok();
}

namespace {
double min_spacing(const Grid& g) noexcept { return std::min({g.spacing[0], g.spacing[1], g.spacing[2]}); }
// The smallest sample distance accepted: 1/64 of the smallest voxel spacing (R-061).
constexpr double kMinSampleFraction = 1.0 / 64.0;
}  // namespace

Status View::set_render_settings(const RenderSettings& s) {
    if (base_ && s.sample_distance > 0.0 && s.sample_distance < min_spacing(base_->grid()) * kMinSampleFraction)
        return Status(Errc::InvalidArgument, "view: sample distance below 1/64 of the smallest voxel spacing");
    if (!std::isfinite(s.sample_distance) || s.sample_distance < 0.0 || !std::isfinite(s.interactive_sample_factor) ||
        !(s.interactive_sample_factor >= 1.0) || s.interactive_downsample == 0 || s.interactive_downsample > 16 ||
        !std::isfinite(s.early_termination) || !(s.early_termination > 0.0) || s.brick_size < 2 || s.brick_size > 256)
        return Status(Errc::InvalidArgument, "view: bad render settings");
    const bool rebrick = s.brick_size != rs_.brick_size;
    rs_ = s;
    if (rebrick) {
        bricks_.reset();
        brick_empty_.clear();
    }
    prepare_tf_alpha();
    if (bricks_ || rebrick) {
        if (mode_ == ViewMode::VolumeRender || mode_ == ViewMode::VolumeMip || mode_ == ViewMode::VolumeMinIp)
            return prepare_volume_rendering();
    }
    return Status::ok();
}

Status View::set_window(int layer, double center, double width) {
    if (!std::isfinite(center) || !std::isfinite(width) || width < 0.0) return Status(Errc::InvalidArgument, "view: bad window");
    if (layer == 0) {
        base_style_.window_center = center;
        base_style_.window_width = width;
        return Status::ok();
    }
    if (layer == 1 && overlay_) {
        overlay_->style.window_center = center;
        overlay_->style.window_width = width;
        return Status::ok();
    }
    return Status(Errc::InvalidArgument, "view: no such layer");
}

Status View::set_colormap(int layer, ColorMap map) {
    if (static_cast<int>(map) > static_cast<int>(ColorMap::Pet20Step)) return Status(Errc::InvalidArgument, "view: unknown colour map");
    if (layer == 0) {
        base_style_.colormap = map;
        return Status::ok();
    }
    if (layer == 1 && overlay_) {
        overlay_->style.colormap = map;
        return Status::ok();
    }
    return Status(Errc::InvalidArgument, "view: no such layer");
}

Status View::set_fusion_layer(std::shared_ptr<const Volume> overlay, const LayerStyle& style, bool preresample) {
    if (!overlay) {
        overlay_.reset();
        return Status::ok();
    }
    if (!std::isfinite(style.opacity) || style.opacity < 0.0 || style.opacity > 1.0 || !std::isfinite(style.window_center) ||
        !std::isfinite(style.window_width) || style.window_width < 0.0 ||
        static_cast<int>(style.colormap) > static_cast<int>(ColorMap::Pet20Step))
        return Status(Errc::InvalidArgument, "view: bad fusion layer style");
    if (overlay->voxel_count() == 0) return Status(Errc::InvalidArgument, "view: empty overlay volume");
    if (preresample) {
        ResampleOptions ro;
        ro.interpolation = Interpolation::Linear;
        ro.type = VoxelTypeRequest::Float32;
        ro.threads = rs_.threads;
        // Outside the overlay: its minimum (NaN would leak into neighbouring trilinear samples
        // through zero weights). Statistics come from the source.
        ro.outside_value = overlay->value_min;
        auto r = resample(*overlay, base_->grid(), ro);
        if (!r) return r.take_error();
        auto res = std::make_shared<Volume>(std::move(r).value());
        res->value_min = overlay->value_min;
        res->value_max = overlay->value_max;
        overlay = std::move(res);
    }
    auto t = GridTransform::make(overlay->grid());
    if (!t) return Status(Errc::InvalidArgument, "view: overlay grid is degenerate");
    overlay_ = Overlay{std::move(overlay), *t, style};
    return Status::ok();
}

Status View::set_curve(std::span<const Vec3> points, bool spline) {
    if (points.size() < 2) return Status(Errc::InvalidArgument, "view: a curve needs at least two points");
    if (points.size() > 100000) return Status(Errc::LimitExceeded, "view: too many curve points");
    for (const Vec3& p : points)
        if (!finite(p)) return Status(Errc::InvalidArgument, "view: curve points must be finite");
    try {
        curve_.assign(points.begin(), points.end());
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "view: out of memory");
    }
    curve_spline_ = spline;
    return Status::ok();
}

double View::sample_distance(Quality q) const noexcept {
    const Grid& g = base_->grid();
    double d = rs_.sample_distance > 0.0 ? rs_.sample_distance : 0.5 * min_spacing(g);
    d = std::max(d, min_spacing(g) * kMinSampleFraction);  // also if the volume changed since (R-061)
    if (q == Quality::Interactive) d *= rs_.interactive_sample_factor;
    return d;
}

// R-061: refuses a frame whose rays would take more samples than the budget, before rendering.
Status View::check_frame_samples(Quality q) const {
    const Grid& g = base_->grid();
    double diag2 = 0;
    for (int k = 0; k < 3; ++k) {
        const double e = static_cast<double>(g.dims[static_cast<std::size_t>(k)]) * g.spacing[static_cast<std::size_t>(k)];
        diag2 += e * e;
    }
    const double per_ray = std::sqrt(diag2) / sample_distance(q) + 2.0;
    const double samples = per_ray * static_cast<double>(w_) * static_cast<double>(h_);
    const std::uint64_t cap = rs_.max_frame_samples ? rs_.max_frame_samples : (std::uint64_t{1} << 32);
    if (!(samples <= static_cast<double>(cap)))
        return Status(Errc::LimitExceeded, "view: the frame would take more ray samples than the budget");
    return Status::ok();
}

Status View::set_transfer_function(const TransferFunction& tf) {
    auto sorted = [](const auto& v) {
        for (std::size_t i = 1; i < v.size(); ++i)
            if (!(v[i].value >= v[i - 1].value)) return false;
        return true;
    };
    for (const auto& p : tf.opacity)
        if (!std::isfinite(p.value) || !std::isfinite(p.y) || p.y < 0.0 || p.y > 1.0)
            return Status(Errc::InvalidArgument, "view: opacity points must be finite, in 0..1");
    for (const auto& p : tf.gradient_opacity)
        if (!std::isfinite(p.value) || !std::isfinite(p.y) || p.y < 0.0 || p.y > 1.0)
            return Status(Errc::InvalidArgument, "view: gradient opacity points must be finite, in 0..1");
    for (const auto& p : tf.color)
        if (!std::isfinite(p.value) || !std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
            return Status(Errc::InvalidArgument, "view: colour points must be finite");
    if (!sorted(tf.opacity) || !sorted(tf.color) || !sorted(tf.gradient_opacity))
        return Status(Errc::InvalidArgument, "view: transfer function points must be sorted by value");
    if (!std::isfinite(tf.unit_distance) || !(tf.unit_distance > 0.0) || !std::isfinite(tf.ambient) ||
        !std::isfinite(tf.diffuse) || !std::isfinite(tf.specular) || !std::isfinite(tf.shininess) || tf.shininess < 0.0)
        return Status(Errc::InvalidArgument, "view: bad shading parameters");
    TfTable t;
    try {
        tf_ = tf;
        double lo = base_->value_min, hi = base_->value_max;
        if (!tf.opacity.empty()) {
            lo = std::min(lo, tf.opacity.front().value);
            hi = std::max(hi, tf.opacity.back().value);
        }
        t.lo = lo;
        t.scale = hi > lo ? (kTfEntries - 1) / (hi - lo) : 0.0;
        t.r.resize(kTfEntries);
        t.g.resize(kTfEntries);
        t.b.resize(kTfEntries);
        t.a_final.resize(kTfEntries);
        t.a_inter.resize(kTfEntries);
        t.nonzero_prefix.assign(kTfEntries + 1, 0);
        std::vector<float> a(kTfEntries);
        for (std::uint32_t e = 0; e < kTfEntries; ++e) {
            const double v = t.scale > 0.0 ? lo + e / t.scale : lo;
            a[e] = static_cast<float>(std::clamp(evaluate_piecewise(tf.opacity, v), 0.0, 1.0));
            t.r[e] = static_cast<float>(std::clamp(eval_color(tf.color, v, 0), 0.0, 1.0));
            t.g[e] = static_cast<float>(std::clamp(eval_color(tf.color, v, 1), 0.0, 1.0));
            t.b[e] = static_cast<float>(std::clamp(eval_color(tf.color, v, 2), 0.0, 1.0));
            t.nonzero_prefix[e + 1] = t.nonzero_prefix[e] + (a[e] > 0.0f ? 1u : 0u);
        }
        t.a_final = a;  // corrected in prepare_tf_alpha
        t.a_inter = std::move(a);
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "view: out of memory");
    }
    tft_ = std::move(t);
    prepare_tf_alpha();
    brick_empty_.clear();
    if (bricks_) return prepare_volume_rendering();
    return Status::ok();
}

void View::prepare_tf_alpha() {
    if (tft_.a_final.empty() || !base_) return;
    // Opacity correction for the sample distance: alpha' = 1 - (1 - alpha)^(d / unit_distance).
    const double ef = sample_distance(Quality::Final) / tf_.unit_distance;
    const double ei = sample_distance(Quality::Interactive) / tf_.unit_distance;
    for (std::uint32_t e = 0; e < kTfEntries; ++e) {
        const double scale = tft_.scale;
        const double v = scale > 0.0 ? tft_.lo + e / scale : tft_.lo;
        const double a = std::clamp(evaluate_piecewise(tf_.opacity, v), 0.0, 1.0);
        tft_.a_final[e] = static_cast<float>(a >= 1.0 ? 1.0 : 1.0 - std::pow(1.0 - a, ef));
        tft_.a_inter[e] = static_cast<float>(a >= 1.0 ? 1.0 : 1.0 - std::pow(1.0 - a, ei));
    }
}

Status View::prepare_volume_rendering() {
    if (!bricks_) {
        auto b = std::make_shared<Bricks>();
        const Grid& g = base_->grid();
        const std::uint32_t B = rs_.brick_size;
        b->size = B;
        for (std::size_t a = 0; a < 3; ++a) b->nb[a] = (g.dims[a] + B - 1) / B;
        const std::size_t count = static_cast<std::size_t>(b->nb[0]) * b->nb[1] * b->nb[2];
        try {
            b->mn.resize(count);
            b->mx.resize(count);
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "view: out of memory");
        }
        const Volume& vol = *base_;
        Bricks& br = *b;
        detail::parallel_for(br.nb[2], rs_.threads, [&](std::uint64_t bz64, unsigned) noexcept {
            const auto bz = static_cast<std::uint32_t>(bz64);
            const std::uint32_t k0 = bz * B, k1 = std::min((bz + 1) * B, g.dims[2] - 1);
            for (std::uint32_t by = 0; by < br.nb[1]; ++by) {
                const std::uint32_t j0 = by * B, j1 = std::min((by + 1) * B, g.dims[1] - 1);
                for (std::uint32_t bx = 0; bx < br.nb[0]; ++bx) {
                    const std::uint32_t i0 = bx * B, i1 = std::min((bx + 1) * B, g.dims[0] - 1);
                    float mn = std::numeric_limits<float>::infinity(), mx = -mn;
                    for (std::uint32_t k = k0; k <= k1; ++k)
                        for (std::uint32_t j = j0; j <= j1; ++j) {
                            const std::size_t row = vol.index(0, j, k);
                            if (vol.type() == VoxelType::Int16) {
                                const std::int16_t* p = vol.i16() + row;
                                std::int16_t a = p[i0], c = p[i0];
                                for (std::uint32_t i = i0; i <= i1; ++i) {
                                    a = std::min(a, p[i]);
                                    c = std::max(c, p[i]);
                                }
                                mn = std::min(mn, static_cast<float>(a));
                                mx = std::max(mx, static_cast<float>(c));
                            } else {
                                const float* p = vol.f32() + row;
                                for (std::uint32_t i = i0; i <= i1; ++i) {
                                    if (std::isnan(p[i])) continue;
                                    mn = std::min(mn, p[i]);
                                    mx = std::max(mx, p[i]);
                                }
                            }
                        }
                    br.mn[br.index(bx, by, bz)] = mn;
                    br.mx[br.index(bx, by, bz)] = mx;
                }
            }
        });
        bricks_ = std::move(b);
    }
    if (brick_empty_.empty()) {
        const Bricks& br = *bricks_;
        try {
            brick_empty_.assign(br.mn.size(), 0);
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "view: out of memory");
        }
        for (std::size_t i = 0; i < br.mn.size(); ++i) {
            if (!(br.mn[i] <= br.mx[i])) {
                brick_empty_[i] = 1;  // no finite voxel
                continue;
            }
            auto entry = [&](double v) {
                const double e = (v - tft_.lo) * tft_.scale + 0.5;
                return static_cast<std::int64_t>(std::clamp(e, 0.0, static_cast<double>(kTfEntries - 1)));
            };
            // One entry of slack on both sides: trilinear results may differ from the brick
            // extremes by a rounding error.
            const std::int64_t e0 = std::max<std::int64_t>(0, entry(br.mn[i]) - 1);
            const std::int64_t e1 = std::min<std::int64_t>(kTfEntries - 1, entry(br.mx[i]) + 1);
            brick_empty_[i] = tft_.nonzero_prefix[static_cast<std::size_t>(e1 + 1)] == tft_.nonzero_prefix[static_cast<std::size_t>(e0)];
        }
    }
    return Status::ok();
}

// ---------------------------------------------------------------- rendering
struct RenderAccess {
    const View& v;
    Layer base;
    std::optional<Layer> ov;
    WinMap wb, wo;
    const ColorTable* mb = nullptr;
    const ColorTable* mo = nullptr;
    double cx = 0, cy = 0;  // screen centre
    std::vector<double> slab_offsets;

    static RenderAccess make(const View& view);

    Vec3 screen_point(double u, double vv) const noexcept {
        return v.plane_.point + v.right_ * ((u - cx) * v.ps_) + v.down_ * ((vv - cy) * v.ps_);
    }
    // Planar MPR (with slab) values for `count` pixels from column u0 on row vv.
    void mpr_row(const Layer& L, double u0, double vv, std::size_t count, float* out, float* tmp) const noexcept;
    void compose(float bval, float oval, std::uint8_t* px) const noexcept;
    bool curve_columns(std::uint32_t width, CurveColumns& cc) const;

    struct RayResult {
        double r = 0, g = 0, b = 0, a = 0;
        bool any = false;
        double best = 0.0;  // MIP / MinIP value
        double pick_t = 0.0;
        bool picked = false;
    };
    RayResult cast(double u, double vv, Quality q, bool for_pick) const noexcept;
};

RenderAccess RenderAccess::make(const View& view) {
    RenderAccess r{view, Layer(*view.base_, view.bt_), std::nullopt, {}, {}, nullptr, nullptr, 0, 0, {}};
    if (view.overlay_) r.ov.emplace(*view.overlay_->vol, view.overlay_->t);
    r.wb = make_window(view.base_style_, *view.base_);
    r.mb = &colormap(view.base_style_.colormap);
    if (view.overlay_) {
        r.wo = make_window(view.overlay_->style, *view.overlay_->vol);
        r.mo = &colormap(view.overlay_->style.colormap);
    }
    r.cx = (view.w_ - 1.0) / 2.0;
    r.cy = (view.h_ - 1.0) / 2.0;
    if (view.slab_ != SlabMode::None && view.slab_thickness_ > 0.0 && view.mode_ == ViewMode::Mpr) {
        const double step = view.sample_distance(Quality::Final);
        const auto n = static_cast<std::size_t>(std::max(2.0, std::min(4096.0, std::ceil(view.slab_thickness_ / step) + 1.0)));
        r.slab_offsets.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            r.slab_offsets[i] = -0.5 * view.slab_thickness_ + view.slab_thickness_ * static_cast<double>(i) / static_cast<double>(n - 1);
    }
    return r;
}

void RenderAccess::mpr_row(const Layer& L, double u0, double vv, std::size_t count, float* out, float* tmp) const noexcept {
    const Vec3 s = L.t.index(screen_point(u0, vv));
    const Vec3 d = L.t.index_direction(v.right_ * v.ps_);
    if (slab_offsets.empty()) {
        L.row(v.interp_, s, d, count, out);
        return;
    }
    const Vec3 dn = L.t.index_direction(v.n_);
    const SlabMode mode = v.slab_;
    for (std::size_t i = 0; i < count; ++i) out[i] = kNaN;
    // tmp holds count values per slab sample; counts for the average are kept in the sign-free
    // second half of tmp (count floats).
    float* cnt = tmp + count;
    if (mode == SlabMode::Average)
        for (std::size_t i = 0; i < count; ++i) cnt[i] = 0.0f;
    for (double off : slab_offsets) {
        L.row(v.interp_, s + dn * off, d, count, tmp);
        for (std::size_t i = 0; i < count; ++i) {
            const float x = tmp[i];
            if (std::isnan(x)) continue;
            float& o = out[i];
            switch (mode) {
                case SlabMode::Mip: o = std::isnan(o) ? x : std::max(o, x); break;
                case SlabMode::MinIp: o = std::isnan(o) ? x : std::min(o, x); break;
                default:
                    o = std::isnan(o) ? x : o + x;
                    cnt[i] += 1.0f;
                    break;
            }
        }
    }
    if (mode == SlabMode::Average)
        for (std::size_t i = 0; i < count; ++i)
            if (cnt[i] > 0.0f) out[i] /= cnt[i];
}

void RenderAccess::compose(float bval, float oval, std::uint8_t* px) const noexcept {
    double r, g, b;
    if (std::isnan(bval)) {
        r = v.rs_.background[0];
        g = v.rs_.background[1];
        b = v.rs_.background[2];
    } else {
        const Rgb& c = (*mb)[wb.gray(bval)];
        r = c[0];
        g = c[1];
        b = c[2];
    }
    // ov is set exactly when the view has an overlay; both are tested so neither is assumed.
    if (ov && v.overlay_ && !std::isnan(oval) && !(v.overlay_->style.transparent_below_window && wo.below(oval))) {
        const Rgb& c = (*mo)[wo.gray(oval)];
        const double a = v.overlay_->style.opacity;
        r = r * (1.0 - a) + c[0] * a;
        g = g * (1.0 - a) + c[1] * a;
        b = b * (1.0 - a) + c[2] * a;
    }
    // Colour-map and background components are 0..255, and so is any blend of them.
    px[0] = static_cast<std::uint8_t>(std::lround(r));
    px[1] = static_cast<std::uint8_t>(std::lround(g));
    px[2] = static_cast<std::uint8_t>(std::lround(b));
    px[3] = 255;
}

bool RenderAccess::curve_columns(std::uint32_t width, CurveColumns& cc) const {
    const std::vector<Vec3>& pts = v.curve_;
    cc.valid.assign(width, 0);
    cc.c.assign(width, Vec3{});
    cc.l.assign(width, Vec3{});
    if (pts.size() < 2) return false;
    std::vector<Vec3> dense;
    if (v.curve_spline_ && pts.size() >= 3) {
        constexpr int kSub = 32;
        dense.reserve((pts.size() - 1) * kSub + 1);
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec3 p0 = pts[i == 0 ? 0 : i - 1], p1 = pts[i], p2 = pts[i + 1], p3 = pts[std::min(i + 2, pts.size() - 1)];
            for (int s = 0; s < kSub; ++s) {
                const double t = s / static_cast<double>(kSub), t2 = t * t, t3 = t2 * t;
                // Uniform Catmull-Rom segment through p1 (t = 0) and p2 (t = 1).
                dense.push_back(0.5 * ((2.0 * p1) + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                                       (3.0 * p1 - p0 - 3.0 * p2 + p3) * t3));
            }
        }
        dense.push_back(pts.back());
    } else {
        dense = pts;
    }
    const bool stretched = v.mode_ == ViewMode::CurvedStretched;
    const Vec3 d = v.down_;
    std::vector<double> len(dense.size(), 0.0);
    for (std::size_t i = 1; i < dense.size(); ++i) {
        Vec3 delta = dense[i] - dense[i - 1];
        if (stretched) delta = delta - d * dot(delta, d);
        len[i] = len[i - 1] + length(delta);
    }
    if (!(len.back() > 0.0)) return false;
    Vec3 last_l = d;
    for (std::uint32_t u = 0; u < width; ++u) {
        const double s = u * v.ps_;
        if (s > len.back() + 1e-9) break;
        std::size_t seg = static_cast<std::size_t>(std::upper_bound(len.begin(), len.end(), s) - len.begin());
        if (seg >= len.size()) seg = len.size() - 1;
        if (seg == 0) seg = 1;
        while (seg > 1 && !(len[seg] > len[seg - 1])) --seg;
        const double span = len[seg] - len[seg - 1];
        const double f = span > 0.0 ? std::clamp((s - len[seg - 1]) / span, 0.0, 1.0) : 0.0;
        cc.c[u] = dense[seg - 1] + (dense[seg] - dense[seg - 1]) * f;
        if (stretched) {
            cc.l[u] = d;
        } else {
            Vec3 l = last_l;
            if (auto tan = normalized(dense[seg] - dense[seg - 1])) {
                if (auto ll = normalized(d - *tan * dot(d, *tan))) l = *ll;
            }
            cc.l[u] = l;
            last_l = l;
        }
        cc.valid[u] = 1;
    }
    return true;
}

RenderAccess::RayResult RenderAccess::cast(double u, double vv, Quality q, bool for_pick) const noexcept {
    RayResult res;
    const Grid& g = base.vol->grid();
    const Vec3 s = base.t.index(screen_point(u, vv));
    const Vec3 dir = base.t.index_direction(v.n_);  // index units per mm
    double t0 = 0, t1 = 0;
    if (!clip_ray(s, dir, g, t0, t1)) return res;
    const double step = v.sample_distance(q);
    const double kf0 = std::ceil(t0 / step), kf1 = std::floor(t1 / step);
    if (!(kf1 >= kf0) || kf1 - kf0 > 1e7) return res;
    const auto k0 = static_cast<std::int64_t>(kf0), k1 = static_cast<std::int64_t>(kf1);
    const View::Bricks* br = v.bricks_.get();
    const bool skip = v.rs_.empty_space_skipping && br != nullptr;
    const double B = br ? static_cast<double>(br->size) : 8.0;
    const double invB = 1.0 / B;
    auto brick_of = [&](Vec3 p, std::uint32_t idx[3]) {
        for (int a = 0; a < 3; ++a) {
            const double f = p[a] * invB;  // truncation == floor for f >= 0; negatives clamp to 0
            const std::uint32_t last = br->nb[static_cast<std::size_t>(a)] - 1u;
            idx[a] = f <= 0.0 ? 0u : (f >= last ? last : static_cast<std::uint32_t>(f));
        }
    };
    // Parameter where the ray leaves brick idx (edge bricks extend to infinity outwards).
    auto brick_exit = [&](const std::uint32_t idx[3], Vec3 p, double t) {
        double te = std::numeric_limits<double>::infinity();
        for (int a = 0; a < 3; ++a) {
            const double da = dir[a];
            if (std::fabs(da) < 1e-300) continue;
            const auto ua = static_cast<std::size_t>(a);
            const double lo = idx[a] == 0 ? -1e30 : idx[a] * B;
            const double hi = idx[a] + 1 == br->nb[ua] ? 1e30 : (idx[a] + 1) * B;
            const double ta = t + ((da > 0 ? hi : lo) - p[a]) / da;
            te = std::min(te, ta);
        }
        return te;
    };
    const ViewMode mode = v.mode_;
    if (mode == ViewMode::VolumeMip || mode == ViewMode::VolumeMinIp) {
        const bool mip = mode == ViewMode::VolumeMip;
        double best = mip ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        double best_t = 0.0;
        for (std::int64_t k = k0; k <= k1;) {
            const double t = static_cast<double>(k) * step;
            const Vec3 p = s + dir * t;
            if (skip && res.any) {
                std::uint32_t bi[3];
                brick_of(p, bi);
                const std::size_t b = br->index(bi[0], bi[1], bi[2]);
                const bool useless = !(br->mn[b] <= br->mx[b]) || (mip ? br->mx[b] <= best : br->mn[b] >= best);
                if (useless) {
                    const double te = brick_exit(bi, p, t);
                    const double nk = std::ceil(te / step);
                    k = std::max(k + 1, nk > static_cast<double>(k1) ? k1 + 1 : static_cast<std::int64_t>(nk));
                    continue;
                }
            }
            const double x = base.linear(p);
            if (!std::isnan(x) && (mip ? x > best : x < best)) {
                best = x;
                best_t = t;
                res.any = true;
            }
            ++k;
        }
        res.best = best;
        res.pick_t = best_t;
        res.picked = res.any;
        return res;
    }
    // Composite.
    const auto& tf = v.tf_;
    const auto& tt = v.tft_;
    const std::vector<float>& alpha = q == Quality::Final ? tt.a_final : tt.a_inter;
    const bool grad_needed = tf.shading || !tf.gradient_opacity.empty();
    const Mat3 gi = base.t.to_index.transposed();  // patient gradient = to_index^T * index gradient
    const double ert = v.rs_.early_termination;
    double C[3] = {0, 0, 0}, A = 0.0;
    for (std::int64_t k = k0; k <= k1;) {
        const double t = static_cast<double>(k) * step;
        const Vec3 p = s + dir * t;
        if (skip) {
            std::uint32_t bi[3];
            brick_of(p, bi);
            if (v.brick_empty_[br->index(bi[0], bi[1], bi[2])]) {
                const double te = brick_exit(bi, p, t);
                const double nk = std::ceil(te / step);
                k = std::max(k + 1, nk > static_cast<double>(k1) ? k1 + 1 : static_cast<std::int64_t>(nk));
                continue;
            }
        }
        ++k;
        const double x = base.linear(p);
        if (std::isnan(x)) continue;
        const double ef = (x - tt.lo) * tt.scale + 0.5;
        const auto e = static_cast<std::size_t>(std::clamp(ef, 0.0, static_cast<double>(kTfEntries - 1)));
        double a = alpha[e];
        if (!(a > 0.0)) continue;
        double col[3] = {tt.r[e], tt.g[e], tt.b[e]};
        if (grad_needed) {
            const Vec3 gx{(base.linear(p + Vec3{1, 0, 0}) - base.linear(p - Vec3{1, 0, 0})) * 0.5,
                          (base.linear(p + Vec3{0, 1, 0}) - base.linear(p - Vec3{0, 1, 0})) * 0.5,
                          (base.linear(p + Vec3{0, 0, 1}) - base.linear(p - Vec3{0, 0, 1})) * 0.5};
            const Vec3 G = gi * gx;
            const double gm = length(G);
            if (!tf.gradient_opacity.empty()) {
                a *= std::clamp(evaluate_piecewise(tf.gradient_opacity, gm), 0.0, 1.0);
                if (!(a > 0.0)) continue;
            }
            if (tf.shading && gm > 1e-12 && std::isfinite(gm)) {
                const double ndl = std::fabs(dot(G, v.n_)) / gm;  // headlight: light along -normal
                const double spec = tf.specular * std::pow(ndl, tf.shininess);
                const double lit = tf.ambient + tf.diffuse * ndl;
                for (double& c : col) c = std::clamp(c * lit + spec, 0.0, 1.0);
            }
        }
        const double w = (1.0 - A) * a;
        C[0] += w * col[0];
        C[1] += w * col[1];
        C[2] += w * col[2];
        A += w;
        if (for_pick && !res.picked && A >= 0.5) {
            res.picked = true;
            res.pick_t = t;
        }
        if (A >= ert) break;
    }
    res.r = C[0];
    res.g = C[1];
    res.b = C[2];
    res.a = A;
    res.any = A > 0.0;
    return res;
}

Status View::set_backend(RenderBackend b) {
    if (static_cast<int>(b) > static_cast<int>(RenderBackend::Gpu)) return Status(Errc::InvalidArgument, "view: unknown backend");
    if (b == RenderBackend::Gpu && !gpu::acquire())
        return Status(Errc::Unsupported, "view: no GPU device is available (" + gpu::unavailable_reason() + ")");
    if (b != RenderBackend::Cpu && !gpu_) {
        try {
            gpu_ = gpu::make_view_state();
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "view: out of memory");
        }
    }
    backend_ = b;
    return Status::ok();
}

RenderBackend View::last_backend() const noexcept {
    return gpu_ && gpu::last_render_was_gpu(*gpu_) ? RenderBackend::Gpu : RenderBackend::Cpu;
}

Status View::render_rgba8(std::span<std::uint8_t> out) const {
    if (!base_) return Status(Errc::InvalidArgument, "view: not initialised");
    const std::uint64_t need = std::uint64_t{w_} * h_ * 4u;
    if (out.size() < need) return Status(Errc::BufferTooSmall, "view: output buffer too small");
    // GPU first (Auto / Gpu); any refusal or failure falls through to the CPU renderer below.
    if (gpu_) {
        if (backend_ != RenderBackend::Cpu && gpu::render_view(*this, *gpu_, out, backend_ == RenderBackend::Gpu))
            return Status::ok();
        gpu::note_cpu_render(*gpu_);
    }
    const bool vr = mode_ == ViewMode::VolumeRender || mode_ == ViewMode::VolumeMip || mode_ == ViewMode::VolumeMinIp;
    if (vr && (!bricks_ || brick_empty_.size() != bricks_->mn.size()))
        return Status(Errc::Internal, "view: volume rendering not prepared");
    if (vr)
        if (Status s = check_frame_samples(Quality::Final); !s) return s;
    try {
        const RenderAccess ra = RenderAccess::make(*this);
        const unsigned workers = detail::resolve_threads(rs_.threads);
        if (!vr) {
            CurveColumns cc;
            const bool curved = mode_ == ViewMode::CurvedStraightened || mode_ == ViewMode::CurvedStretched;
            if (curved) ra.curve_columns(w_, cc);
            const std::uint32_t tx = (w_ + kTile - 1) / kTile, ty = (h_ + kTile - 1) / kTile;
            std::vector<std::vector<float>> buf(workers, std::vector<float>(std::size_t{kTile} * 6));
            detail::parallel_for(std::uint64_t{tx} * ty, workers, [&](std::uint64_t tile, unsigned wk) noexcept {
                const std::uint32_t x0 = static_cast<std::uint32_t>(tile % tx) * kTile, y0 = static_cast<std::uint32_t>(tile / tx) * kTile;
                const std::uint32_t x1 = std::min(x0 + kTile, w_), y1 = std::min(y0 + kTile, h_);
                const std::size_t n = x1 - x0;
                float* bv = buf[wk].data();
                float* ovv = bv + kTile;
                float* tmp = ovv + kTile;
                for (std::uint32_t y = y0; y < y1; ++y) {
                    if (curved) {
                        const double off = (y - ra.cy) * ps_;
                        for (std::uint32_t x = x0; x < x1; ++x) {
                            const std::size_t i = x - x0;
                            bv[i] = ovv[i] = kNaN;
                            if (!cc.valid[x]) continue;
                            const Vec3 p = cc.c[x] + cc.l[x] * off;
                            bv[i] = ra.base.at(interp_, ra.base.t.index(p));
                            if (ra.ov) ovv[i] = ra.ov->at(interp_, ra.ov->t.index(p));
                        }
                    } else {
                        ra.mpr_row(ra.base, x0, y, n, bv, tmp);
                        if (ra.ov) ra.mpr_row(*ra.ov, x0, y, n, ovv, tmp);
                    }
                    std::uint8_t* px = out.data() + (static_cast<std::size_t>(y) * w_ + x0) * 4u;
                    for (std::size_t i = 0; i < n; ++i) ra.compose(bv[i], ra.ov ? ovv[i] : kNaN, px + 4 * i);
                }
            });
            return Status::ok();
        }
        // Volume rendering: at reduced resolution for interactive quality.
        const std::uint32_t f = quality_ == Quality::Interactive ? rs_.interactive_downsample : 1u;
        const std::uint32_t rw = std::max(1u, (w_ + f - 1) / f), rh = std::max(1u, (h_ + f - 1) / f);
        std::vector<std::uint8_t> low;
        std::uint8_t* target = out.data();
        if (f > 1) {
            low.resize(static_cast<std::size_t>(rw) * rh * 4u);
            target = low.data();
        }
        const double sx = static_cast<double>(w_) / rw, sy = static_cast<double>(h_) / rh;
        const std::uint32_t tx = (rw + kTile - 1) / kTile, ty = (rh + kTile - 1) / kTile;
        const auto& bg = rs_.background;
        detail::parallel_for(std::uint64_t{tx} * ty, workers, [&](std::uint64_t tile, unsigned) noexcept {
            const std::uint32_t x0 = static_cast<std::uint32_t>(tile % tx) * kTile, y0 = static_cast<std::uint32_t>(tile / tx) * kTile;
            const std::uint32_t x1 = std::min(x0 + kTile, rw), y1 = std::min(y0 + kTile, rh);
            for (std::uint32_t y = y0; y < y1; ++y)
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const double u = (x + 0.5) * sx - 0.5, vv = (y + 0.5) * sy - 0.5;
                    const auto rr = ra.cast(u, vv, quality_, false);
                    std::uint8_t* px = target + (static_cast<std::size_t>(y) * rw + x) * 4u;
                    if (mode_ == ViewMode::VolumeRender) {
                        const double rest = 1.0 - rr.a;
                        px[0] = to8(rr.r + rest * bg[0] / 255.0);
                        px[1] = to8(rr.g + rest * bg[1] / 255.0);
                        px[2] = to8(rr.b + rest * bg[2] / 255.0);
                        px[3] = 255;
                    } else {
                        ra.compose(rr.any ? static_cast<float>(rr.best) : kNaN, kNaN, px);
                    }
                }
        });
        if (f > 1) {
            // Bilinear upsampling to the output size.
            detail::parallel_for(h_, workers, [&](std::uint64_t y64, unsigned) noexcept {
                const double fy = std::clamp((static_cast<double>(y64) + 0.5) / sy - 0.5, 0.0, rh - 1.0);
                const auto y0 = static_cast<std::uint32_t>(fy);
                const std::uint32_t y1 = std::min(y0 + 1, rh - 1);
                const double ay = fy - y0;
                for (std::uint32_t x = 0; x < w_; ++x) {
                    const double fx = std::clamp((x + 0.5) / sx - 0.5, 0.0, rw - 1.0);
                    const auto x0 = static_cast<std::uint32_t>(fx);
                    const std::uint32_t x1 = std::min(x0 + 1, rw - 1);
                    const double ax = fx - x0;
                    std::uint8_t* px = out.data() + (static_cast<std::size_t>(y64) * w_ + x) * 4u;
                    for (int c = 0; c < 4; ++c) {
                        auto at = [&](std::uint32_t xx, std::uint32_t yy) {
                            return static_cast<double>(low[(static_cast<std::size_t>(yy) * rw + xx) * 4u + static_cast<std::size_t>(c)]);
                        };
                        const double top = at(x0, y0) * (1 - ax) + at(x1, y0) * ax;
                        const double bot = at(x0, y1) * (1 - ax) + at(x1, y1) * ax;
                        px[c] = static_cast<std::uint8_t>(std::lround(top * (1 - ay) + bot * ay));  // 0..255
                    }
                }
            });
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "view: out of memory");
    }
    return Status::ok();
}

Status View::render_values(std::span<float> out, float outside) const {
    if (!base_) return Status(Errc::InvalidArgument, "view: not initialised");
    if (out.size() < std::uint64_t{w_} * h_) return Status(Errc::BufferTooSmall, "view: output buffer too small");
    if (mode_ == ViewMode::VolumeRender) return Status(Errc::Unsupported, "view: composite volume rendering has no values");
    const bool vr = mode_ == ViewMode::VolumeMip || mode_ == ViewMode::VolumeMinIp;
    if (vr && !bricks_) return Status(Errc::Internal, "view: volume rendering not prepared");
    if (vr)
        if (Status s = check_frame_samples(Quality::Final); !s) return s;
    try {
        const RenderAccess ra = RenderAccess::make(*this);
        const unsigned workers = detail::resolve_threads(rs_.threads);
        CurveColumns cc;
        const bool curved = mode_ == ViewMode::CurvedStraightened || mode_ == ViewMode::CurvedStretched;
        if (curved) ra.curve_columns(w_, cc);
        std::vector<std::vector<float>> buf(workers, std::vector<float>(static_cast<std::size_t>(w_) * 2));
        detail::parallel_for(h_, workers, [&](std::uint64_t y64, unsigned wk) noexcept {
            const auto y = static_cast<std::uint32_t>(y64);
            float* row = out.data() + static_cast<std::size_t>(y) * w_;
            if (vr) {
                for (std::uint32_t x = 0; x < w_; ++x) {
                    const auto rr = ra.cast(x, y, Quality::Final, false);
                    row[x] = rr.any ? static_cast<float>(rr.best) : outside;
                }
                return;
            }
            if (curved) {
                const double off = (y - ra.cy) * ps_;
                for (std::uint32_t x = 0; x < w_; ++x)
                    row[x] = cc.valid[x] ? ra.base.at(interp_, ra.base.t.index(cc.c[x] + cc.l[x] * off)) : kNaN;
            } else {
                ra.mpr_row(ra.base, 0.0, y, w_, row, buf[wk].data());
            }
            for (std::uint32_t x = 0; x < w_; ++x)
                if (std::isnan(row[x])) row[x] = outside;
        });
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "view: out of memory");
    }
    return Status::ok();
}

Result<PickResult> View::pick(double x, double y) const {
    if (!base_) return Status(Errc::InvalidArgument, "view: not initialised");
    if (!std::isfinite(x) || !std::isfinite(y)) return Status(Errc::InvalidArgument, "view: pick point must be finite");
    PickResult r;
    try {
        const RenderAccess ra = RenderAccess::make(*this);
        Vec3 p;
        double value = std::numeric_limits<double>::quiet_NaN();
        bool have_value = false;
        switch (mode_) {
            case ViewMode::Mpr: {
                p = ra.screen_point(x, y);
                if (!ra.slab_offsets.empty() && slab_ != SlabMode::Average) {
                    // Slab MIP / MinIP: the location of the extreme sample.
                    double best = std::numeric_limits<double>::quiet_NaN();
                    Vec3 bp = p;
                    for (double off : ra.slab_offsets) {
                        const Vec3 q = p + n_ * off;
                        const double val = ra.base.at(interp_, ra.base.t.index(q));
                        if (std::isnan(val)) continue;
                        if (std::isnan(best) || (slab_ == SlabMode::Mip ? val > best : val < best)) {
                            best = val;
                            bp = q;
                        }
                    }
                    p = bp;
                    value = best;
                    have_value = !std::isnan(best);
                } else if (!ra.slab_offsets.empty()) {
                    float o[1];
                    std::vector<float> t2(2);
                    ra.mpr_row(ra.base, x, y, 1, o, t2.data());
                    value = o[0];
                    have_value = !std::isnan(o[0]);
                }
                r.hit = true;
                break;
            }
            case ViewMode::CurvedStraightened:
            case ViewMode::CurvedStretched: {
                const auto u = static_cast<std::int64_t>(std::floor(x + 0.5));
                if (u < 0 || u >= static_cast<std::int64_t>(w_)) return r;
                CurveColumns cc;
                ra.curve_columns(w_, cc);
                if (!cc.valid[static_cast<std::size_t>(u)]) return r;
                // Interpolate between neighbouring columns for fractional x.
                const auto uu = static_cast<std::size_t>(u);
                Vec3 c = cc.c[uu], l = cc.l[uu];
                const double fx = x - static_cast<double>(u);
                const std::size_t un = fx >= 0 ? uu + 1 : (uu > 0 ? uu - 1 : uu);
                if (un < w_ && cc.valid[un]) {
                    c = c + (cc.c[un] - c) * std::fabs(fx);
                    l = normalized(l + (cc.l[un] - l) * std::fabs(fx)).value_or(l);
                }
                p = c + l * ((y - ra.cy) * ps_);
                r.hit = true;
                break;
            }
            default: {
                const auto rr = ra.cast(x, y, Quality::Final, true);
                p = ra.screen_point(x, y);
                if (rr.picked) {
                    p = p + n_ * rr.pick_t;
                    r.hit = true;
                    if (mode_ != ViewMode::VolumeRender) {
                        value = rr.best;
                        have_value = true;
                    }
                }
                break;
            }
        }
        r.patient = p;
        r.index = ra.base.t.index(p);
        r.inside = ra.base.inside(r.index);
        if (!have_value && r.inside) {
            const Interpolation m = (mode_ == ViewMode::VolumeRender || mode_ == ViewMode::VolumeMip || mode_ == ViewMode::VolumeMinIp)
                                        ? Interpolation::Linear
                                        : interp_;
            value = ra.base.at(m, r.index);
            have_value = !std::isnan(value);
        }
        r.value = have_value ? value : 0.0;
        if (ra.ov) {
            const float ovv = ra.ov->at(Interpolation::Linear, ra.ov->t.index(p));
            r.overlay_inside = !std::isnan(ovv);
            r.overlay_value = r.overlay_inside ? ovv : 0.0;
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "view: out of memory");
    }
    return r;
}

Result<Vec3> View::project(Vec3 patient) const {
    if (mode_ == ViewMode::CurvedStraightened || mode_ == ViewMode::CurvedStretched)
        return Status(Errc::Unsupported, "view: projection is defined for planar views only");
    if (!finite(patient)) return Status(Errc::InvalidArgument, "view: point must be finite");
    const Vec3 d = patient - plane_.point;
    return Vec3{dot(d, right_) / ps_ + (w_ - 1.0) / 2.0, dot(d, down_) / ps_ + (h_ - 1.0) / 2.0, dot(d, n_)};
}

}  // namespace siaara::volume
