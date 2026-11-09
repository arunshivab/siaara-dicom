// SIAARA DICOM Toolkit - 3D volumes built from DICOM image series.
//
// A Volume is a dense voxel grid (int16 or float32, i fastest, then j, then k) in patient space
// with the Modality LUT / Rescale applied per frame. It is built either from the instances of one
// series (one frame each, or several single-plane multi-frame objects) or from one enhanced
// multi-frame object (per-frame functional groups):
//   1. every frame's plane (IPP / IOP / Pixel Spacing, functional groups first) and phase keys are
//      read (see geometry.hpp);
//   2. frames are checked for one orientation, one Rows x Columns, one pixel spacing;
//   3. 4D series (positions repeated) are split into phases by Temporal Position Index, Stack ID,
//      Temporal Position Identifier or Trigger Time - the first key that makes positions unique;
//   4. the frames of the chosen phase are sorted along the slice normal; duplicate positions,
//      non-uniform spacing, missing slices and gantry tilt are detected and reported;
//   5. frames are decoded in parallel (FrameReader) straight into the voxel buffer.
// Gantry tilt is kept exactly as a sheared grid (axes[2] along the slice-to-slice shift), or, with
// VolumeOptions::deshear_tilt, resampled in-plane (bilinear) onto an orthogonal grid.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "volume/geometry.hpp"

namespace siaara::volume {

enum class VoxelType : std::uint8_t { Int16, Float32 };
enum class VoxelTypeRequest : std::uint8_t {
    Auto,     // int16 when every modality output value is an integer that fits; float32 otherwise
    Int16,    // forced: values are rounded and clamped (flag ValuesClamped when that changed any)
    Float32,  // forced
};

// Findings of volume building (bit flags, also in the C API).
enum VolumeFlag : std::uint32_t {
    kNonUniformSpacing  = 1u << 0,  // slice gaps differ by more than the tolerance
    kMissingSlices      = 1u << 1,  // a gap is more than 1.5x the nominal spacing
    kDuplicatePositions = 1u << 2,  // frames at the same position were dropped (after phase split)
    kGantryTilt         = 1u << 3,  // stacking direction is not along the slice normal
    kDesheared          = 1u << 4,  // tilt was resampled onto an orthogonal grid
    kMultiPhase         = 1u << 5,  // the series has several phases (phase_count > 1)
    kSyntheticGeometry  = 1u << 6,  // positions / orientation missing: geometry was made up
    kSingleSlice        = 1u << 7,  // one slice: k spacing from Spacing Between Slices / Slice Thickness / 1 mm
    kValuesClamped      = 1u << 8,  // int16 output had to round or clamp values
    kSpacingFromHeader  = 1u << 9,  // positions all equal: k spacing taken from the header
};

struct Histogram {
    double min = 0.0;    // value at the lower edge of bin 0
    double max = 0.0;    // value at the upper edge of the last bin
    std::vector<std::uint64_t> bins;  // equal-width bins over [min, max]
};

class Volume {
public:
    Volume() = default;
    Volume(Volume&&) noexcept = default;
    Volume& operator=(Volume&&) noexcept = default;
    Volume(const Volume&) = delete;
    Volume& operator=(const Volume&) = delete;

    // Allocates an uninitialised voxel buffer for the grid. LimitExceeded when the byte size
    // overflows, exceeds `memory_budget`, or does not fit in size_t on this platform.
    static Result<Volume> allocate(const Grid& grid, VoxelType type, std::uint64_t memory_budget = 4ull << 30);
    // Deep copy (same budget rules).
    Result<Volume> clone(std::uint64_t memory_budget = 4ull << 30) const;

    const Grid& grid() const noexcept { return grid_; }
    Grid& grid() noexcept { return grid_; }
    VoxelType type() const noexcept { return type_; }
    std::uint64_t voxel_count() const noexcept { return count_; }
    std::uint64_t byte_size() const noexcept { return count_ * (type_ == VoxelType::Int16 ? 2u : 4u); }
    const void* data() const noexcept;
    std::int16_t* i16() noexcept { return i16_.get(); }
    const std::int16_t* i16() const noexcept { return i16_.get(); }
    float* f32() noexcept { return f32_.get(); }
    const float* f32() const noexcept { return f32_.get(); }
    std::size_t index(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept {
        return (static_cast<std::size_t>(k) * grid_.dims[1] + j) * grid_.dims[0] + i;
    }
    float at(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept {
        const std::size_t n = index(i, j, k);
        return type_ == VoxelType::Int16 ? static_cast<float>(i16_[n]) : f32_[n];
    }
    void set(std::uint32_t i, std::uint32_t j, std::uint32_t k, float v) noexcept;

    // Recomputes value_min / value_max (finite voxels) and the histogram (256 bins by default), in
    // parallel. Fails only when memory runs out.
    Status update_statistics(unsigned threads = 0, std::uint32_t bins = 256);
    // Statistics from exact int16 value counts (4 x 65536 tables each, see volume.cpp). histogram.bins
    // must already be sized and zeroed. May throw std::bad_alloc (callers catch it).
    void set_int16_statistics(const std::vector<const std::vector<std::uint64_t>*>& parts, std::uint32_t bins);

    double value_min = 0.0;
    double value_max = 0.0;
    Histogram histogram;
    std::vector<std::string> warnings;  // never contain PHI
    std::uint32_t flags = 0;            // VolumeFlag bits
    std::uint32_t phase_count = 1;
    std::uint32_t phase_index = 0;
    std::string phase_key;              // attribute used to split phases, empty when one phase
    double tilt_degrees = 0.0;          // angle between stacking direction and slice normal
    std::uint32_t source_frames = 0;    // frames used (after duplicates were dropped)

private:
    Grid grid_;
    VoxelType type_ = VoxelType::Float32;
    std::uint64_t count_ = 0;
    std::unique_ptr<std::int16_t[]> i16_;
    std::unique_ptr<float[]> f32_;
};

struct VolumeOptions {
    VoxelTypeRequest type = VoxelTypeRequest::Auto;
    std::uint32_t phase = 0;              // which phase of a 4D series (0-based)
    unsigned threads = 0;                 // 0 = hardware concurrency (at most 16)
    bool deshear_tilt = false;            // resample a tilted stack onto an orthogonal grid
    std::uint64_t memory_budget = 4ull << 30;  // voxel buffer limit (bytes)
    std::uint64_t max_frames = 100000;    // frames considered at most
    double spacing_tolerance = 0.01;      // relative: gaps within (1 +- tol) x nominal are uniform
    double orientation_tolerance = 1e-3;  // max difference of direction cosines between frames
    double tilt_tolerance_degrees = 0.05; // below this the stack is treated as untilted
    bool strict = false;                  // fail (instead of warn) on non-uniform spacing, gaps, duplicates
    bool lenient = true;                  // passed to the parser / pixel layer
    std::uint32_t histogram_bins = 256;
};

// One frame source: a parsed file plus its transfer syntax (DicomFile carries both).
Result<Volume> build_volume(std::span<const DicomFile* const> files, const VolumeOptions& opt = {});
// Reads the files (in parallel, memory-mapped) and builds the volume.
Result<Volume> build_volume_from_paths(std::span<const std::string> utf8_paths, const VolumeOptions& opt = {});

}  // namespace siaara::volume
