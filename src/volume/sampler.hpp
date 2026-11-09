// SIAARA DICOM Toolkit - voxel sampling kernels shared by resampling and rendering (internal).
//
// Coordinates are continuous voxel indices (voxel centres at integers). A point is inside the
// volume when every coordinate c satisfies -0.5 <= c <= n - 0.5 (the voxel footprint). Neighbours
// needed by the interpolation kernels are clamped to the grid (edge replication), so a sample at
// an integer index equals the voxel value for every kernel except the approximating B-spline.
//
// Kernels:
//   Nearest       - the voxel whose centre is closest (ties round up: floor(c + 0.5)).
//   Linear        - trilinear.
//   CubicCatmullRom - separable Catmull-Rom cubic convolution (Keys, a = -0.5): interpolating
//                   (passes through voxel values), C1, may overshoot slightly near edges.
//   CubicBSpline  - separable uniform cubic B-spline applied to the voxel values directly (no
//                   prefilter): approximating (smooths; does not pass through voxel values), C2,
//                   never overshoots. Equals scipy.ndimage.map_coordinates(order=3, prefilter=False).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "volume/volume.hpp"

namespace siaara::volume {

enum class Interpolation : std::uint8_t { Nearest, Linear, CubicCatmullRom, CubicBSpline };

namespace detail {

template <class T>
struct VoxelAccess {
    const T* p = nullptr;
    std::int64_t nx = 0, ny = 0, nz = 0;
    std::int64_t sy = 0, sz = 0;  // strides of j and k
    double ex = 0, ey = 0, ez = 0;  // upper footprint edges n - 0.5

    VoxelAccess() = default;
    VoxelAccess(const T* data, const Grid& g) noexcept
        : p(data), nx(g.dims[0]), ny(g.dims[1]), nz(g.dims[2]), sy(g.dims[0]),
          sz(static_cast<std::int64_t>(g.dims[0]) * g.dims[1]), ex(g.dims[0] - 0.5), ey(g.dims[1] - 0.5),
          ez(g.dims[2] - 0.5) {}

    float at(std::int64_t i, std::int64_t j, std::int64_t k) const noexcept {
        return static_cast<float>(p[k * sz + j * sy + i]);
    }
    bool inside(double x, double y, double z) const noexcept {
        return x >= -0.5 && x <= ex && y >= -0.5 && y <= ey && z >= -0.5 && z <= ez;
    }
};

inline std::int64_t clampi(std::int64_t v, std::int64_t n) noexcept { return v < 0 ? 0 : (v >= n ? n - 1 : v); }

template <class T>
float sample_nearest(const VoxelAccess<T>& a, double x, double y, double z) noexcept {
    const auto i = clampi(static_cast<std::int64_t>(std::floor(x + 0.5)), a.nx);
    const auto j = clampi(static_cast<std::int64_t>(std::floor(y + 0.5)), a.ny);
    const auto k = clampi(static_cast<std::int64_t>(std::floor(z + 0.5)), a.nz);
    return a.at(i, j, k);
}

template <class T>
float sample_linear(const VoxelAccess<T>& a, double x, double y, double z) noexcept {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy), iz = static_cast<std::int64_t>(fz);
    const float tx = static_cast<float>(x - fx), ty = static_cast<float>(y - fy), tz = static_cast<float>(z - fz);
    const std::int64_t x0 = clampi(ix, a.nx), x1 = clampi(ix + 1, a.nx);
    const std::int64_t y0 = clampi(iy, a.ny) * a.sy, y1 = clampi(iy + 1, a.ny) * a.sy;
    const std::int64_t z0 = clampi(iz, a.nz) * a.sz, z1 = clampi(iz + 1, a.nz) * a.sz;
    const T* p = a.p;
    const float c000 = static_cast<float>(p[z0 + y0 + x0]), c100 = static_cast<float>(p[z0 + y0 + x1]);
    const float c010 = static_cast<float>(p[z0 + y1 + x0]), c110 = static_cast<float>(p[z0 + y1 + x1]);
    const float c001 = static_cast<float>(p[z1 + y0 + x0]), c101 = static_cast<float>(p[z1 + y0 + x1]);
    const float c011 = static_cast<float>(p[z1 + y1 + x0]), c111 = static_cast<float>(p[z1 + y1 + x1]);
    // u * (1 - t) + v * t is exact at both t = 0 and t = 1 (u + (v - u) * t is not at t = 1).
    const float sx = 1.0f - tx, sy = 1.0f - ty, sz = 1.0f - tz;
    const float c00 = c000 * sx + c100 * tx, c10 = c010 * sx + c110 * tx;
    const float c01 = c001 * sx + c101 * tx, c11 = c011 * sx + c111 * tx;
    const float c0 = c00 * sy + c10 * ty, c1 = c01 * sy + c11 * ty;
    return c0 * sz + c1 * tz;
}

inline void catmull_rom_weights(double t, double w[4]) noexcept {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
    w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
    w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}
inline void bspline_weights(double t, double w[4]) noexcept {
    const double s = 1.0 - t, t2 = t * t, t3 = t2 * t;
    w[0] = s * s * s / 6.0;
    w[1] = (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
    w[2] = (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
    w[3] = t3 / 6.0;
}

template <class T>
float sample_cubic(const VoxelAccess<T>& a, double x, double y, double z, bool bspline) noexcept {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    double wx[4], wy[4], wz[4];
    if (bspline) {
        bspline_weights(x - fx, wx);
        bspline_weights(y - fy, wy);
        bspline_weights(z - fz, wz);
    } else {
        catmull_rom_weights(x - fx, wx);
        catmull_rom_weights(y - fy, wy);
        catmull_rom_weights(z - fz, wz);
    }
    const auto ix = static_cast<std::int64_t>(fx) - 1, iy = static_cast<std::int64_t>(fy) - 1, iz = static_cast<std::int64_t>(fz) - 1;
    std::int64_t xi[4], yi[4], zi[4];
    for (int n = 0; n < 4; ++n) {
        xi[n] = clampi(ix + n, a.nx);
        yi[n] = clampi(iy + n, a.ny) * a.sy;
        zi[n] = clampi(iz + n, a.nz) * a.sz;
    }
    double acc = 0.0;
    for (int c = 0; c < 4; ++c) {
        if (wz[c] == 0.0) continue;
        double plane = 0.0;
        for (int b = 0; b < 4; ++b) {
            const T* row = a.p + zi[c] + yi[b];
            const double r = wx[0] * row[xi[0]] + wx[1] * row[xi[1]] + wx[2] * row[xi[2]] + wx[3] * row[xi[3]];
            plane += wy[b] * r;
        }
        acc += wz[c] * plane;
    }
    return static_cast<float>(acc);
}

template <class T>
float sample(const VoxelAccess<T>& a, Interpolation m, double x, double y, double z) noexcept {
    switch (m) {
        case Interpolation::Nearest: return sample_nearest(a, x, y, z);
        case Interpolation::Linear: return sample_linear(a, x, y, z);
        case Interpolation::CubicCatmullRom: return sample_cubic(a, x, y, z, false);
        case Interpolation::CubicBSpline: return sample_cubic(a, x, y, z, true);
    }
    return 0.0f;
}

// Samples n points p0 + i * step (i = 0 .. n-1) into out; points outside get `outside`.
// The trilinear case may use the SIMD row kernel (resample.cpp) when it is enabled.
template <class T>
void sample_row(const VoxelAccess<T>& a, Interpolation m, double x0, double y0, double z0, double dx, double dy, double dz,
                std::size_t n, float outside, float* out) noexcept;

}  // namespace detail
}  // namespace siaara::volume
