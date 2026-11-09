// SIAARA DICOM Toolkit - GPU rendering of a volume::View: packs the view's state into the kernel
// parameters and tables (gpu_params.hpp), runs the backend and reads the image back (see gpu.hpp).
//
// Everything that is per frame and not per pixel is computed here in double precision, from the
// same View members and helpers the CPU renderer uses (screen geometry, index transforms, window
// coefficients, colour maps, slab offsets, the transfer-function tables with the CPU's opacity
// correction, the brick grid and its empty flags), so the kernels only repeat the per-pixel work.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

#include "gpu/gpu.hpp"
#include "gpu/gpu_params.hpp"
#include "volume/render3d.hpp"

namespace siaara::gpu {

struct ViewGpu {
    std::mutex m;
    std::shared_ptr<Backend> backend;
    std::unique_ptr<Session> session;
    std::shared_ptr<const Texture> base_tex, ov_tex;
    std::weak_ptr<const volume::Volume> base_vol, ov_vol;
    std::vector<std::uint32_t> params, data;
    std::vector<std::uint8_t> low;
    // int16 colour LUT cache per layer: key (a, b, colour map), 65536 packed entries.
    struct Lut {
        bool valid = false;
        double a = 0.0, b = 0.0;
        volume::ColorMap map = volume::ColorMap::Gray;
        std::vector<std::uint32_t> words;
    } lut[2];
    std::atomic<bool> last_gpu{false};
    std::string reason;
};

std::shared_ptr<ViewGpu> make_view_state() { return std::make_shared<ViewGpu>(); }

bool last_render_was_gpu(const ViewGpu& s) noexcept { return s.last_gpu.load(); }
void note_cpu_render(ViewGpu& s) noexcept { s.last_gpu.store(false); }

std::string last_fallback_reason(const ViewGpu& s) {
    std::lock_guard<std::mutex> lock(const_cast<ViewGpu&>(s).m);
    return s.reason;
}

}  // namespace siaara::gpu

namespace siaara::volume {

namespace P = gpu::param;

struct GpuAccess {
    static Status render(const View& v, gpu::ViewGpu& g, std::span<std::uint8_t> out, bool allow_cpu);

private:
    struct Packer {
        std::vector<std::uint32_t>& p;
        std::vector<std::uint32_t>& d;
        bool finite = true;
        // Trilinear from 8 texel fetches regardless of the texture's hardware filter (MPR family).
        bool exact_filter = false;
        void u(std::uint32_t w, std::uint32_t x) { p[w] = x; }
        void f(std::uint32_t w, double x) {
            const auto fx = static_cast<float>(x);
            if (!std::isfinite(fx)) finite = false;
            std::memcpy(&p[w], &fx, 4);
        }
        void v3(std::uint32_t w, Vec3 a) {
            f(w, a.x);
            f(w + 1, a.y);
            f(w + 2, a.z);
        }
        // Appends n words to the data buffer and returns their offset.
        std::uint32_t alloc(std::size_t n) {
            const std::size_t at = d.size();
            d.resize(at + n, 0u);
            return static_cast<std::uint32_t>(at);
        }
        std::uint32_t floats(const float* x, std::size_t n) {
            const std::uint32_t at = alloc(n);
            std::memcpy(d.data() + at, x, n * 4u);
            return at;
        }
    };

    static Status layer(const View& v, Packer& pk, std::uint32_t L, const Volume& vol, const GridTransform& t,
                        const LayerStyle& style, const gpu::Texture& tex, gpu::ViewGpu::Lut& cache, bool colors, Vec3 s0,
                        Vec3 du, Vec3 dv);
};

namespace {

std::uint32_t pack_rgb(const std::array<std::uint8_t, 3>& c, bool below) {
    return std::uint32_t{c[0]} | (std::uint32_t{c[1]} << 8) | (std::uint32_t{c[2]} << 16) | (below ? (1u << 24) : 0u);
}

}  // namespace

Status GpuAccess::layer(const View& v, Packer& pk, std::uint32_t L, const Volume& vol, const GridTransform& t,
                        const LayerStyle& style, const gpu::Texture& tex, gpu::ViewGpu::Lut& cache, bool colors, Vec3 s0,
                        Vec3 du, Vec3 dv) {
    const Grid& g = vol.grid();
    pk.v3(L + P::LS0, t.index(s0));
    pk.v3(L + P::LDU, t.index_direction(du));
    pk.v3(L + P::LDV, t.index_direction(dv));
    pk.v3(L + P::LDN, t.index_direction(v.n_));
    for (std::uint32_t a = 0; a < 3; ++a) {
        pk.f(L + P::LDimsF + a, g.dims[a]);
        pk.f(L + P::LEdge + a, g.dims[a] - 0.5);
        pk.u(L + P::LDimsU + a, g.dims[a]);
    }
    pk.f(L + P::LScale, tex.encoding.scale);
    pk.f(L + P::LBias, tex.encoding.bias);
    std::uint32_t fl = 0;
    if (!tex.encoding.hardware_filter || pk.exact_filter || gpu::testing::manual_filtering_forced()) fl |= P::kLayerManual;
    if (tex.encoding.integer) fl |= P::kLayerInteger;
    pk.u(L + P::LFlags, fl);
    double a = 0.0, b = 0.0;
    detail::window_coefficients(style, vol, a, b);
    pk.f(L + P::LWinA, a);
    pk.f(L + P::LWinB, b);
    pk.u(L + P::LLut, P::kNone);
    pk.u(L + P::LCmap, P::kNone);
    pk.f(L + P::LOpacity, style.opacity);
    pk.u(L + P::LTransparent, style.transparent_below_window ? 1u : 0u);
    if (!colors) return Status::ok();
    const std::uint32_t cm = pk.alloc(256);
    for (std::uint32_t e = 0; e < 256; ++e) pk.d[cm + e] = pack_rgb(detail::colormap_rgb(style.colormap, e), false);
    pk.u(L + P::LCmap, cm);
    if (vol.type() == VoxelType::Int16) {
        // Exact colours of every int16 value, computed like the CPU's compose() (WinMap::gray / below).
        if (!cache.valid || cache.a != a || cache.b != b || cache.map != style.colormap) {
            cache.words.resize(65536);
            for (std::int32_t x = -32768; x <= 32767; ++x) {
                const double w = x * a + b;
                const double tt = w > 0.0 ? (w < 1.0 ? w : 1.0) : 0.0;
                const auto gray = static_cast<std::uint8_t>(tt * 255.0 + 0.5);
                cache.words[static_cast<std::size_t>(x + 32768)] = pack_rgb(detail::colormap_rgb(style.colormap, gray), !(w > 0.0));
            }
            cache.valid = true;
            cache.a = a;
            cache.b = b;
            cache.map = style.colormap;
        }
        const std::uint32_t at = pk.alloc(65536);
        std::memcpy(pk.d.data() + at, cache.words.data(), 65536u * 4u);
        pk.u(L + P::LLut, at);
    }
    (void)v;
    return Status::ok();
}

Status GpuAccess::render(const View& v, gpu::ViewGpu& g, std::span<std::uint8_t> out, bool allow_cpu) {
    const bool vr = v.mode_ == ViewMode::VolumeRender || v.mode_ == ViewMode::VolumeMip || v.mode_ == ViewMode::VolumeMinIp;
    if (v.mode_ == ViewMode::CurvedStraightened || v.mode_ == ViewMode::CurvedStretched)
        return Status(Errc::Unsupported, "gpu: curved MPR is rendered on the CPU");
    if (!vr && v.interp_ != Interpolation::Nearest && v.interp_ != Interpolation::Linear)
        return Status(Errc::Unsupported, "gpu: cubic interpolation is rendered on the CPU");
    if (vr && (!v.bricks_ || v.brick_empty_.size() != v.bricks_->mn.size()))
        return Status(Errc::Unsupported, "gpu: volume rendering not prepared");

    std::shared_ptr<gpu::Backend> backend = gpu::acquire();
    if (!backend) return Status(Errc::Unsupported, "gpu: " + gpu::unavailable_reason());
    if (backend->info().type == gpu::DeviceType::Cpu && !allow_cpu)
        return Status(Errc::Unsupported, "gpu: only a software (CPU-type) device is available; Auto uses hardware GPUs only");
    if (g.backend != backend) {
        g.session.reset();
        g.base_tex.reset();
        g.ov_tex.reset();
        g.base_vol.reset();
        g.ov_vol.reset();
        g.backend = backend;
    }
    if (!g.session) {
        auto s = backend->session();
        if (!s) return s.take_error();
        g.session = std::move(s).value();
    }
    if (!g.base_tex || g.base_vol.lock() != v.base_) {
        g.base_tex.reset();
        auto t = backend->texture(v.base_);
        if (!t) return t.take_error();
        g.base_tex = std::move(t).value();
        g.base_vol = v.base_;
    }
    const bool with_overlay = !vr && v.overlay_.has_value();
    if (with_overlay) {
        if (!g.ov_tex || g.ov_vol.lock() != v.overlay_->vol) {
            g.ov_tex.reset();
            auto t = backend->texture(v.overlay_->vol);
            if (!t) return t.take_error();
            g.ov_tex = std::move(t).value();
            g.ov_vol = v.overlay_->vol;
        }
    }

    // ---- parameters
    g.params.assign(P::Count, 0u);
    g.data.clear();
    Packer pk{g.params, g.data};
    const std::uint32_t f = vr && v.quality_ == Quality::Interactive ? v.rs_.interactive_downsample : 1u;
    const std::uint32_t rw = std::max(1u, (v.w_ + f - 1) / f), rh = std::max(1u, (v.h_ + f - 1) / f);
    const double sx = static_cast<double>(v.w_) / rw, sy = static_cast<double>(v.h_) / rh;
    const double cx = (v.w_ - 1.0) / 2.0, cy = (v.h_ - 1.0) / 2.0;
    auto screen_point = [&](double u, double vv) { return v.plane_.point + v.right_ * ((u - cx) * v.ps_) + v.down_ * ((vv - cy) * v.ps_); };
    pk.u(P::Width, rw);
    pk.u(P::Height, rh);
    pk.f(P::BgR, v.rs_.background[0]);
    pk.f(P::BgG, v.rs_.background[1]);
    pk.f(P::BgB, v.rs_.background[2]);
    std::uint32_t flags = 0;
    P::Kernel kernel = P::KernelMpr;
    if (!vr) {
        // MPR, slabs and fusion take one trilinear sample per pixel (per slab step), so they always
        // use the exact 8-fetch path: hardware filters weight with reduced fixed-point precision
        // (Apple GPUs measurably so), which can move a fused overlay value across the window
        // bottom and flip a pixel between transparent and coloured. Ray casting keeps the
        // hardware filter where the format supports it (many samples per pixel; differences stay
        // within a few levels).
        pk.exact_filter = true;
        if (v.interp_ == Interpolation::Linear) flags |= P::kFlagLinear;
        const Vec3 s0 = screen_point(0.0, 0.0), du = v.right_ * v.ps_, dv = v.down_ * v.ps_;
        if (Status s = layer(v, pk, P::L0, *v.base_, v.bt_, v.base_style_, *g.base_tex, g.lut[0], true, s0, du, dv); !s) return s;
        if (with_overlay) {
            flags |= P::kFlagOverlay;
            if (Status s = layer(v, pk, P::L1, *v.overlay_->vol, v.overlay_->t, v.overlay_->style, *g.ov_tex, g.lut[1], true, s0, du, dv); !s)
                return s;
        }
        if (v.slab_ != SlabMode::None && v.slab_thickness_ > 0.0) {
            // As RenderAccess::make: n samples evenly over [-thickness/2, +thickness/2].
            const double step = v.sample_distance(Quality::Final);
            const auto n = static_cast<std::uint32_t>(std::max(2.0, std::min(4096.0, std::ceil(v.slab_thickness_ / step) + 1.0)));
            const std::uint32_t at = pk.alloc(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto off = static_cast<float>(-0.5 * v.slab_thickness_ + v.slab_thickness_ * static_cast<double>(i) / static_cast<double>(n - 1));
                std::memcpy(&pk.d[at + i], &off, 4);
            }
            pk.u(P::SlabCount, n);
            pk.u(P::SlabMode, static_cast<std::uint32_t>(v.slab_));
            pk.u(P::SlabOffsets, at);
        }
    } else {
        const Vec3 s0 = screen_point(0.5 * sx - 0.5, 0.5 * sy - 0.5), du = v.right_ * (v.ps_ * sx), dv = v.down_ * (v.ps_ * sy);
        const bool mip = v.mode_ != ViewMode::VolumeRender;
        kernel = mip ? P::KernelMip : P::KernelComposite;
        if (Status s = layer(v, pk, P::L0, *v.base_, v.bt_, v.base_style_, *g.base_tex, g.lut[0], mip, s0, du, dv); !s) return s;
        const double step = v.sample_distance(v.quality_);
        // Sample indices k = t / step must stay exact in float: |t| is bounded by the distance of
        // the volume's corners from the plane.
        const Grid& gr = v.base_->grid();
        double tmax = 0.0;
        for (int c = 0; c < 8; ++c) {
            const Vec3 idx{(c & 1) ? gr.dims[0] - 0.5 : -0.5, (c & 2) ? gr.dims[1] - 0.5 : -0.5, (c & 4) ? gr.dims[2] - 0.5 : -0.5};
            tmax = std::max(tmax, std::fabs(dot(gr.index_to_patient(idx) - v.plane_.point, v.n_)));
        }
        if (!(tmax / step < 4.0e6)) return Status(Errc::Unsupported, "gpu: too many samples per ray for float sample indices");
        pk.f(P::Step, step);
        pk.f(P::Ert, std::min(v.rs_.early_termination, 1.0e30));
        if (mip && v.mode_ == ViewMode::VolumeMinIp) flags |= P::kFlagMinIp;
        const View::Bricks& br = *v.bricks_;
        if (v.rs_.empty_space_skipping) {
            flags |= P::kFlagSkip;
            pk.f(P::BrickSize, br.size);
            pk.f(P::BrickInv, 1.0 / br.size);
            for (std::uint32_t a = 0; a < 3; ++a) pk.u(P::BrickNb + a, br.nb[a]);
            if (mip) {
                pk.u(P::BrickMin, pk.floats(br.mn.data(), br.mn.size()));
                pk.u(P::BrickMax, pk.floats(br.mx.data(), br.mx.size()));
            } else {
                const std::size_t n = v.brick_empty_.size();
                const std::uint32_t at = pk.alloc((n + 31) / 32);
                for (std::size_t i = 0; i < n; ++i)
                    if (v.brick_empty_[i]) pk.d[at + i / 32] |= 1u << (i % 32);
                pk.u(P::BrickEmpty, at);
            }
        }
        if (!mip) {
            const auto& tt = v.tft_;
            const std::vector<float>& alpha = v.quality_ == Quality::Final ? tt.a_final : tt.a_inter;
            pk.f(P::TfLo, tt.lo);
            pk.f(P::TfScale, tt.scale);
            pk.u(P::TfEntries, static_cast<std::uint32_t>(tt.r.size()));
            pk.u(P::TfR, pk.floats(tt.r.data(), tt.r.size()));
            pk.u(P::TfG, pk.floats(tt.g.data(), tt.g.size()));
            pk.u(P::TfB, pk.floats(tt.b.data(), tt.b.size()));
            pk.u(P::TfA, pk.floats(alpha.data(), alpha.size()));
            const TransferFunction& tf = v.tf_;
            if (tf.shading || !tf.gradient_opacity.empty()) flags |= P::kFlagGradient;
            if (tf.shading) flags |= P::kFlagShading;
            if (!tf.gradient_opacity.empty()) {
                flags |= P::kFlagGradOpacity;
                const std::uint32_t at = pk.alloc(tf.gradient_opacity.size() * 2);
                for (std::size_t i = 0; i < tf.gradient_opacity.size(); ++i) {
                    const float pv[2] = {static_cast<float>(tf.gradient_opacity[i].value), static_cast<float>(tf.gradient_opacity[i].y)};
                    if (!std::isfinite(pv[0])) pk.finite = false;
                    std::memcpy(&pk.d[at + 2 * i], pv, 8);
                }
                pk.u(P::GopOffset, at);
                pk.u(P::GopCount, static_cast<std::uint32_t>(tf.gradient_opacity.size()));
            }
            pk.f(P::Ambient, tf.ambient);
            pk.f(P::Diffuse, tf.diffuse);
            pk.f(P::Specular, tf.specular);
            pk.f(P::Shininess, tf.shininess);
            const Mat3 gi = v.bt_.to_index.transposed();
            for (std::uint32_t r = 0; r < 3; ++r)
                for (std::uint32_t c = 0; c < 3; ++c) pk.f(P::Gi + 3 * r + c, gi.m[r][c]);
            pk.v3(P::Normal, v.n_);
        }
    }
    pk.u(P::Flags, flags);
    if (!pk.finite) return Status(Errc::Unsupported, "gpu: view parameters exceed the float range");
    if (std::uint64_t{g.data.size()} * 4u > backend->max_buffer_bytes() || g.data.size() > 0xFFFFFFF0u)
        return Status(Errc::Unsupported, "gpu: tables exceed the storage-buffer limit");

    // ---- run
    std::span<std::uint8_t> target = out;
    if (f > 1) {
        g.low.resize(static_cast<std::size_t>(rw) * rh * 4u);
        target = std::span<std::uint8_t>(g.low);
    }
    if (Status s = g.session->run(kernel, rw, rh, g.params, g.data, *g.base_tex, with_overlay ? g.ov_tex.get() : nullptr, target); !s)
        return s;
    if (f > 1) {
        // Bilinear upsampling to the output size (as the CPU renderer does).
        const std::vector<std::uint8_t>& low = g.low;
        for (std::uint32_t y = 0; y < v.h_; ++y) {
            const double fy = std::clamp((static_cast<double>(y) + 0.5) / sy - 0.5, 0.0, rh - 1.0);
            const auto y0 = static_cast<std::uint32_t>(fy);
            const std::uint32_t y1 = std::min(y0 + 1, rh - 1);
            const double ay = fy - y0;
            for (std::uint32_t x = 0; x < v.w_; ++x) {
                const double fx = std::clamp((x + 0.5) / sx - 0.5, 0.0, rw - 1.0);
                const auto x0 = static_cast<std::uint32_t>(fx);
                const std::uint32_t x1 = std::min(x0 + 1, rw - 1);
                const double ax = fx - x0;
                std::uint8_t* px = out.data() + (static_cast<std::size_t>(y) * v.w_ + x) * 4u;
                for (std::size_t c = 0; c < 4; ++c) {
                    auto at = [&](std::uint32_t xx, std::uint32_t yy) {
                        return static_cast<double>(low[(static_cast<std::size_t>(yy) * rw + xx) * 4u + c]);
                    };
                    const double top = at(x0, y0) * (1 - ax) + at(x1, y0) * ax;
                    const double bot = at(x0, y1) * (1 - ax) + at(x1, y1) * ax;
                    px[c] = static_cast<std::uint8_t>(top * (1 - ay) + bot * ay + 0.5);
                }
            }
        }
    }
    return Status::ok();
}

}  // namespace siaara::volume

namespace siaara::gpu {

Status render_view(const volume::View& v, ViewGpu& g, std::span<std::uint8_t> out, bool allow_cpu_device) {
    std::lock_guard<std::mutex> lock(g.m);
    Status s;
    try {
        s = volume::GpuAccess::render(v, g, out, allow_cpu_device);
        g.reason = s.is_ok() ? std::string() : s.message;
    } catch (const std::bad_alloc&) {
        s = Status(Errc::OutOfMemory, "gpu: out of memory");
        g.reason.clear();
    }
    g.last_gpu.store(s.is_ok());
    return s;
}

}  // namespace siaara::gpu
