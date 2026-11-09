// SIAARA DICOM Toolkit - resampling (see resample.hpp) and the shared row sampler.
#include "volume/resample.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "volume/parallel.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#define SIAARA_VOLUME_SSE2 1
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SIAARA_VOLUME_NEON 1
#include <arm_neon.h>
#endif

namespace siaara::volume {

namespace {
std::atomic<bool> g_simd{true};
}  // namespace

bool simd_available() noexcept {
#if defined(SIAARA_VOLUME_SSE2) || defined(SIAARA_VOLUME_NEON)
    return true;
#else
    return false;
#endif
}
void set_simd_enabled(bool on) noexcept { g_simd.store(on, std::memory_order_relaxed); }
bool simd_enabled() noexcept { return simd_available() && g_simd.load(std::memory_order_relaxed); }

namespace detail {

namespace {

#if defined(SIAARA_VOLUME_SSE2) || defined(SIAARA_VOLUME_NEON)
// Four trilinear samples, all with 0 <= c < n - 1 on every axis (no clamping needed).
template <class T>
void linear4(const VoxelAccess<T>& a, const double* xs, const double* ys, const double* zs, float* out) noexcept {
    alignas(16) float c[8][4];
    alignas(16) float tx[4], ty[4], tz[4];
    const T* p = a.p;
    for (int q = 0; q < 4; ++q) {
        const auto ix = static_cast<std::int64_t>(xs[q]), iy = static_cast<std::int64_t>(ys[q]), iz = static_cast<std::int64_t>(zs[q]);
        tx[q] = static_cast<float>(xs[q] - static_cast<double>(ix));
        ty[q] = static_cast<float>(ys[q] - static_cast<double>(iy));
        tz[q] = static_cast<float>(zs[q] - static_cast<double>(iz));
        const std::int64_t b = iz * a.sz + iy * a.sy + ix;
        c[0][q] = static_cast<float>(p[b]);
        c[1][q] = static_cast<float>(p[b + 1]);
        c[2][q] = static_cast<float>(p[b + a.sy]);
        c[3][q] = static_cast<float>(p[b + a.sy + 1]);
        c[4][q] = static_cast<float>(p[b + a.sz]);
        c[5][q] = static_cast<float>(p[b + a.sz + 1]);
        c[6][q] = static_cast<float>(p[b + a.sz + a.sy]);
        c[7][q] = static_cast<float>(p[b + a.sz + a.sy + 1]);
    }
#if defined(SIAARA_VOLUME_SSE2)
    auto lerp = [](__m128 u, __m128 v, __m128 t) { return _mm_add_ps(u, _mm_mul_ps(_mm_sub_ps(v, u), t)); };
    const __m128 vx = _mm_load_ps(tx), vy = _mm_load_ps(ty), vz = _mm_load_ps(tz);
    const __m128 c00 = lerp(_mm_load_ps(c[0]), _mm_load_ps(c[1]), vx);
    const __m128 c10 = lerp(_mm_load_ps(c[2]), _mm_load_ps(c[3]), vx);
    const __m128 c01 = lerp(_mm_load_ps(c[4]), _mm_load_ps(c[5]), vx);
    const __m128 c11 = lerp(_mm_load_ps(c[6]), _mm_load_ps(c[7]), vx);
    _mm_storeu_ps(out, lerp(lerp(c00, c10, vy), lerp(c01, c11, vy), vz));
#else
    auto lerp = [](float32x4_t u, float32x4_t v, float32x4_t t) { return vaddq_f32(u, vmulq_f32(vsubq_f32(v, u), t)); };
    const float32x4_t vx = vld1q_f32(tx), vy = vld1q_f32(ty), vz = vld1q_f32(tz);
    const float32x4_t c00 = lerp(vld1q_f32(c[0]), vld1q_f32(c[1]), vx);
    const float32x4_t c10 = lerp(vld1q_f32(c[2]), vld1q_f32(c[3]), vx);
    const float32x4_t c01 = lerp(vld1q_f32(c[4]), vld1q_f32(c[5]), vx);
    const float32x4_t c11 = lerp(vld1q_f32(c[6]), vld1q_f32(c[7]), vx);
    vst1q_f32(out, lerp(lerp(c00, c10, vy), lerp(c01, c11, vy), vz));
#endif
}
#endif

}  // namespace

template <class T>
void sample_row(const VoxelAccess<T>& a, Interpolation m, double x0, double y0, double z0, double dx, double dy, double dz,
                std::size_t n, float outside, float* out) noexcept {
    std::size_t i = 0;
#if defined(SIAARA_VOLUME_SSE2) || defined(SIAARA_VOLUME_NEON)
    if (m == Interpolation::Linear && simd_enabled() && a.nx > 1 && a.ny > 1 && a.nz > 1) {
        const double hx = a.ex - 0.5, hy = a.ey - 0.5, hz = a.ez - 0.5;
        for (; i + 4 <= n; i += 4) {
            double xs[4], ys[4], zs[4];
            bool interior = true;
            for (int q = 0; q < 4; ++q) {
                const double t = static_cast<double>(i + static_cast<std::size_t>(q));
                xs[q] = x0 + t * dx;
                ys[q] = y0 + t * dy;
                zs[q] = z0 + t * dz;
                interior = interior && xs[q] >= 0.0 && xs[q] < hx && ys[q] >= 0.0 && ys[q] < hy && zs[q] >= 0.0 && zs[q] < hz;
            }
            if (interior) {
                linear4(a, xs, ys, zs, out + i);
                continue;
            }
            for (int q = 0; q < 4; ++q)
                out[i + static_cast<std::size_t>(q)] =
                    a.inside(xs[q], ys[q], zs[q]) ? sample_linear(a, xs[q], ys[q], zs[q]) : outside;
        }
    }
#endif
    for (; i < n; ++i) {
        const double t = static_cast<double>(i);
        const double x = x0 + t * dx, y = y0 + t * dy, z = z0 + t * dz;
        out[i] = a.inside(x, y, z) ? sample(a, m, x, y, z) : outside;
    }
}

template void sample_row<std::int16_t>(const VoxelAccess<std::int16_t>&, Interpolation, double, double, double, double, double,
                                       double, std::size_t, float, float*) noexcept;
template void sample_row<float>(const VoxelAccess<float>&, Interpolation, double, double, double, double, double, double,
                                std::size_t, float, float*) noexcept;

}  // namespace detail

Result<Volume> resample(const Volume& src, const Grid& target, const ResampleOptions& opt) {
    if (Status s = target.validate(); !s) return s;
    if (src.voxel_count() == 0) return Status(Errc::InvalidArgument, "resample: empty source volume");
    auto st = GridTransform::make(src.grid());
    if (!st) return Status(Errc::InvalidArgument, "resample: source grid is degenerate");
    VoxelType type = src.type();
    if (opt.type == VoxelTypeRequest::Int16) type = VoxelType::Int16;
    if (opt.type == VoxelTypeRequest::Float32) type = VoxelType::Float32;
    auto r = Volume::allocate(target, type, opt.memory_budget);
    if (!r) return r;
    Volume& out = *r;
    const double outside_d = opt.outside_value.value_or(src.value_min);
    const float outside = std::isfinite(outside_d) ? static_cast<float>(outside_d) : 0.0f;
    const std::uint32_t nx = target.dims[0], ny = target.dims[1], nz = target.dims[2];
    const unsigned workers = detail::resolve_threads(opt.threads);
    std::vector<std::vector<float>> rowbuf;
    try {
        rowbuf.assign(workers, std::vector<float>(nx));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "resample: out of memory");
    }
    const Mat3 tm = target.index_to_patient_matrix();
    const Vec3 step = st->index_direction(tm.col(0));
    const detail::VoxelAccess<std::int16_t> ai(src.i16(), src.grid());
    const detail::VoxelAccess<float> af(src.f32(), src.grid());
    const std::uint64_t rows = std::uint64_t{ny} * nz;
    detail::parallel_for(rows, workers, [&](std::uint64_t row, unsigned w) noexcept {
        const auto j = static_cast<std::uint32_t>(row % ny), k = static_cast<std::uint32_t>(row / ny);
        const Vec3 p0 = target.origin + tm * Vec3{0.0, static_cast<double>(j), static_cast<double>(k)};
        const Vec3 i0 = st->index(p0);
        const std::size_t base = (static_cast<std::size_t>(k) * ny + j) * nx;
        float* dst = type == VoxelType::Float32 ? out.f32() + base : rowbuf[w].data();
        if (src.type() == VoxelType::Int16)
            detail::sample_row(ai, opt.interpolation, i0.x, i0.y, i0.z, step.x, step.y, step.z, nx, outside, dst);
        else
            detail::sample_row(af, opt.interpolation, i0.x, i0.y, i0.z, step.x, step.y, step.z, nx, outside, dst);
        if (type == VoxelType::Int16) {
            std::int16_t* o = out.i16() + base;
            for (std::uint32_t i = 0; i < nx; ++i) {
                const float v = dst[i];
                o[i] = static_cast<std::int16_t>(std::isfinite(v) ? std::clamp(std::nearbyint(v), -32768.0f, 32767.0f) : 0.0f);
            }
        }
    });
    if (opt.statistics) out.update_statistics(opt.threads);
    out.phase_count = src.phase_count;
    out.phase_index = src.phase_index;
    out.phase_key = src.phase_key;
    return r;
}

Result<Grid> isotropic_grid(const Grid& g, double spacing) {
    if (Status s = g.validate(); !s) return s;
    if (!std::isfinite(spacing) || !(spacing > 0.0)) return Status(Errc::InvalidArgument, "resample: spacing must be positive");
    Grid o = g;
    for (std::size_t a = 0; a < 3; ++a) {
        const double extent = (g.dims[a] - 1.0) * g.spacing[a];
        const double n = std::floor(extent / spacing + 1e-9) + 1.0;
        if (n > 4294967295.0) return Status(Errc::LimitExceeded, "resample: too many voxels");
        o.dims[a] = static_cast<std::uint32_t>(n);
        o.spacing[a] = spacing;
    }
    std::uint64_t c = 0;
    if (!o.voxel_count(c)) return Status(Errc::LimitExceeded, "resample: too many voxels");
    return o;
}

}  // namespace siaara::volume
