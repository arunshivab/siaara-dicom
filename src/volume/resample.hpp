// SIAARA DICOM Toolkit - resampling a volume onto another grid (fusion, de-obliquing, isotropic).
// Every output voxel centre is mapped to patient space with the target grid, then into the source
// grid's continuous index space, and sampled with the chosen kernel (sampler.hpp documents the
// kernels and the edge rule). Work is split over output rows with std::thread.
//
// The trilinear row kernel has an SSE2 (x86-64) / NEON (AArch64) version for runs of samples that
// are fully inside the grid; the scalar kernel is the reference, and simd_enabled(false) forces it
// (used by the differential test). SSE2 and NEON are baseline on those targets, so the runtime
// switch is the only check; on other targets (32-bit x86, ...) the scalar kernel is always used.
#pragma once

#include <cstdint>
#include <optional>

#include "core/status.hpp"
#include "volume/geometry.hpp"
#include "volume/sampler.hpp"
#include "volume/volume.hpp"

namespace siaara::volume {

struct ResampleOptions {
    Interpolation interpolation = Interpolation::Linear;
    unsigned threads = 0;                 // 0 = hardware concurrency
    std::optional<double> outside_value;  // value outside the source; default: source minimum; NaN kept for float32 output
    VoxelTypeRequest type = VoxelTypeRequest::Auto;  // Auto: same type as the source
    std::uint64_t memory_budget = 4ull << 30;
    bool statistics = true;               // compute value range and histogram of the result
};

Result<Volume> resample(const Volume& src, const Grid& target, const ResampleOptions& opt = {});

// A grid covering the same patient-space box as `g` with isotropic spacing `spacing` (mm).
Result<Grid> isotropic_grid(const Grid& g, double spacing);

// SIMD row kernels: whether compiled in for this target, and the runtime switch (default on).
bool simd_available() noexcept;
void set_simd_enabled(bool on) noexcept;
bool simd_enabled() noexcept;

}  // namespace siaara::volume
