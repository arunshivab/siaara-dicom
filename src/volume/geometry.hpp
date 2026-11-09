// SIAARA DICOM Toolkit - minimal patient-space geometry for the 3D layer (src/volume).
//
// This is deliberately small and self-contained: the clinical module (src/clinical: geometry,
// series sorting) is being written in parallel and will replace these helpers later. Everything
// the volume code needs from DICOM geometry is in this header, so the swap touches one place.
//
// Written from DICOM PS3.3 C.7.6.2 (Image Plane module: Image Position (Patient) (0020,0032),
// Image Orientation (Patient) (0020,0037), Pixel Spacing (0028,0030)) and C.7.6.16 (Multi-frame
// Functional Groups: Plane Position, Plane Orientation, Pixel Measures, Frame Content,
// Cardiac Synchronization). Coordinates are the DICOM patient coordinate system (LPS, mm).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara::volume {

namespace tags {
constexpr Tag ImagePositionPatient           = 0x00200032;
constexpr Tag ImageOrientationPatient        = 0x00200037;
constexpr Tag PixelSpacing                   = 0x00280030;
constexpr Tag SliceThickness                 = 0x00180050;
constexpr Tag SpacingBetweenSlices           = 0x00180088;
constexpr Tag InstanceNumber                 = 0x00200013;
constexpr Tag AcquisitionNumber              = 0x00200012;
constexpr Tag TemporalPositionIdentifier     = 0x00200100;
constexpr Tag NumberOfTemporalPositions      = 0x00200105;
constexpr Tag TriggerTime                    = 0x00181060;
constexpr Tag SharedFunctionalGroupsSequence = 0x52009229;
constexpr Tag PerFrameFunctionalGroupsSequence = 0x52009230;
constexpr Tag PlanePositionSequence          = 0x00209113;
constexpr Tag PlaneOrientationSequence       = 0x00209116;
constexpr Tag PixelMeasuresSequence          = 0x00289110;
constexpr Tag FrameContentSequence           = 0x00209111;
constexpr Tag TemporalPositionIndex          = 0x00209128;
constexpr Tag StackID                        = 0x00209056;
constexpr Tag InStackPositionNumber          = 0x00209057;
constexpr Tag CardiacSynchronizationSequence = 0x00189118;
constexpr Tag NominalCardiacTriggerDelayTime = 0x00209153;
}  // namespace tags

// ---------------------------------------------------------------- vectors
struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    constexpr Vec3() = default;
    constexpr Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    double operator[](int i) const noexcept { return i == 0 ? x : (i == 1 ? y : z); }
    double& operator[](int i) noexcept { return i == 0 ? x : (i == 1 ? y : z); }
};
constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }
constexpr Vec3 operator*(Vec3 a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
constexpr Vec3 operator*(double s, Vec3 a) noexcept { return {a.x * s, a.y * s, a.z * s}; }
constexpr double dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }
inline bool finite(Vec3 a) noexcept { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
// Unit vector, or nullopt for a zero / non-finite vector.
std::optional<Vec3> normalized(Vec3 a) noexcept;

// 3x3 matrix, m[row][col]. Used as "columns are axes": M * (i, j, k) = i*col0 + j*col1 + k*col2.
struct Mat3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static Mat3 from_columns(Vec3 a, Vec3 b, Vec3 c) noexcept;
    Vec3 col(int c) const noexcept { return {m[0][c], m[1][c], m[2][c]}; }
    Vec3 operator*(Vec3 v) const noexcept {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 transposed() const noexcept;
    double determinant() const noexcept;
    // Inverse, or nullopt when |det| is tiny relative to the matrix scale (singular).
    std::optional<Mat3> inverse() const noexcept;
};

// ---------------------------------------------------------------- voxel grid
// The geometry of a voxel grid in patient space. Voxel (i, j, k) - i fastest in memory - has its
// centre at origin + i*spacing[0]*axes[0] + j*spacing[1]*axes[1] + k*spacing[2]*axes[2].
// axes are unit vectors; axes[2] may be non-orthogonal to axes[0], axes[1] (gantry tilt kept as a
// sheared grid). For a DICOM slice stack: axes[0] = row direction (increasing column index),
// axes[1] = column direction (increasing row index), axes[2] = stacking direction.
struct Grid {
    std::array<std::uint32_t, 3> dims{0, 0, 0};
    std::array<double, 3> spacing{1.0, 1.0, 1.0};
    Vec3 origin;
    std::array<Vec3, 3> axes{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};

    // Number of voxels; false on 64-bit overflow.
    bool voxel_count(std::uint64_t& out) const noexcept;
    // Index -> patient matrix (axes scaled by spacing).
    Mat3 index_to_patient_matrix() const noexcept;
    Vec3 index_to_patient(Vec3 idx) const noexcept { return origin + index_to_patient_matrix() * idx; }
    // Checks dims > 0, finite positive spacing, unit finite axes and an invertible matrix.
    Status validate() const;
};

// Patient <-> continuous index mapping, precomputed (both directions).
struct GridTransform {
    Mat3 to_patient;   // index -> patient (without origin)
    Mat3 to_index;     // patient -> index (without origin)
    Vec3 origin;
    static std::optional<GridTransform> make(const Grid& g) noexcept;
    Vec3 patient(Vec3 idx) const noexcept { return origin + to_patient * idx; }
    Vec3 index(Vec3 p) const noexcept { return to_index * (p - origin); }
    // A patient-space direction expressed in index units.
    Vec3 index_direction(Vec3 d) const noexcept { return to_index * d; }
};

// ---------------------------------------------------------------- DICOM plane attributes
// Plane description of one frame, from the per-frame / shared functional groups or the top-level
// Image Plane module. Missing attributes are reported through the has_* flags.
struct FramePlane {
    bool has_position = false;
    bool has_orientation = false;
    bool has_spacing = false;
    Vec3 position;              // centre of the first transmitted pixel (row 0, column 0)
    Vec3 row_dir{1, 0, 0};      // direction of increasing column index (IOP values 1-3)
    Vec3 col_dir{0, 1, 0};      // direction of increasing row index (IOP values 4-6)
    double row_spacing = 1.0;   // Pixel Spacing value 1: distance between rows (mm)
    double col_spacing = 1.0;   // Pixel Spacing value 2: distance between columns (mm)
    std::optional<double> slice_thickness;
    std::optional<double> spacing_between_slices;
    Vec3 normal() const noexcept { return cross(row_dir, col_dir); }
};

// Keys used to split a series into phases (4D) and to break ties.
struct FrameKeys {
    std::optional<std::int64_t> temporal_position_index;    // Frame Content (0020,9128)
    std::optional<std::string> stack_id;                    // Frame Content (0020,9056)
    std::optional<std::int64_t> in_stack_position;          // Frame Content (0020,9057)
    std::optional<std::int64_t> temporal_position_identifier;  // (0020,0100)
    std::optional<double> trigger_time;                     // (0018,1060) or (0020,9153)
    std::optional<std::int64_t> instance_number;            // (0020,0013)
};

// The functional group item `group_seq` for `frame`: Per-frame Functional Groups item `frame`
// first, then the Shared Functional Groups. nullptr when absent (or for non-enhanced data sets).
const Dataset* functional_group(const Dataset& ds, std::uint32_t frame, Tag group_seq) noexcept;

// Reads the plane of a frame. Values that are present but malformed (wrong multiplicity,
// non-finite, zero-length or non-orthogonal direction cosines, non-positive spacing) fail with
// Malformed; absent values just leave the has_* flag false.
Result<FramePlane> read_frame_plane(const Dataset& ds, std::uint32_t frame);
FrameKeys read_frame_keys(const Dataset& ds, std::uint32_t frame);

}  // namespace siaara::volume
