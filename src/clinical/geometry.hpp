// SIAARA DICOM Toolkit - image plane geometry.
// Written from DICOM PS3.3 C.7.6.2 (Image Plane: Image Position/Orientation (Patient), Pixel
// Spacing, the equation of C.7.6.2.1.1), C.7.6.16.2.1/2.3/2.4 (Pixel Measures, Plane Position,
// Plane Orientation functional groups), 10.7.1.3 and C.8.11.4 (Imager Pixel Spacing, Estimated
// Radiographic Magnification Factor, Pixel Spacing Calibration Type), C.8.6.2 (Nominal Scanned
// Pixel Spacing), C.8.5.5 (Sequence of Ultrasound Regions), C.7.6.1.1.1 (patient orientation
// letters) and C.7.6.16.2.2 / C.7.6.17 (Frame Content, Stack ID, Temporal Position Index).
//
// Coordinates: (column, row) pixel indices are 0-based with pixel (0,0) centred on Image Position
// (Patient), as in C.7.6.2.1.1. Patient coordinates are millimetres in the LPS patient system.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara::clinical {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const noexcept { return {x * s, y * s, z * s}; }
    Vec3 operator-() const noexcept { return {-x, -y, -z}; }
    double dot(const Vec3& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const noexcept { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const noexcept { return std::sqrt(dot(*this)); }
    Vec3 normalized() const noexcept {
        const double n = norm();
        return n > 0.0 ? Vec3{x / n, y / n, z / n} : Vec3{};
    }
};

enum class GeometrySource : std::uint8_t {
    None,
    Classic,    // top-level Image Plane attributes
    PerFrame,   // Per-frame Functional Groups Sequence item
    Shared,     // Shared Functional Groups Sequence
};

// Where a pixel spacing came from ("calibration source").
enum class SpacingSource : std::uint8_t {
    None = 0,
    PixelSpacing = 1,                // (0028,0030), or the Pixel Measures functional group
    ImagerPixelSpacing = 2,          // (0018,1164) at the detector plane, not corrected
    ImagerPixelSpacingMagnified = 3, // (0018,1164) divided by (0018,1114): estimated at the patient
    NominalScannedPixelSpacing = 4,  // (0018,2010): digitised film / document plane
    UltrasoundRegion = 5,            // Physical Delta X/Y of a Sequence of Ultrasound Regions item
};

struct PixelSpacingInfo {
    double row = 0.0;     // mm between the centres of adjacent rows (vertical), value 1 of Pixel Spacing
    double column = 0.0;  // mm between the centres of adjacent columns (horizontal), value 2
    SpacingSource source = SpacingSource::None;
    GeometrySource from = GeometrySource::None;
    bool at_detector = false;        // spacing applies at the detector plane, not in the patient
    bool calibrated = false;         // Pixel Spacing Calibration Type present (GEOMETRY / FIDUCIAL)
    std::string calibration_type;
    double magnification = 1.0;      // Estimated Radiographic Magnification Factor applied (1 = none)
    int ultrasound_region = -1;      // item index used for SpacingSource::UltrasoundRegion
    bool valid() const noexcept { return source != SpacingSource::None && row > 0.0 && column > 0.0; }
};

struct PlaneGeometry {
    std::uint32_t rows = 0, columns = 0;
    bool has_position = false;
    bool has_orientation = false;
    bool orthogonal = true;          // row and column cosines perpendicular (within 1e-4)
    Vec3 position;                   // centre of the first transmitted pixel (TLHC)
    Vec3 row_cosine;                 // direction of increasing column index (IOP values 1..3), unit length
    Vec3 column_cosine;              // direction of increasing row index (IOP values 4..6), unit length
    Vec3 normal;                     // row_cosine x column_cosine
    GeometrySource position_from = GeometrySource::None;
    GeometrySource orientation_from = GeometrySource::None;
    PixelSpacingInfo spacing;
    std::optional<double> slice_thickness;
    std::optional<double> spacing_between_slices;
    std::string frame_of_reference_uid;
    std::vector<std::string> warnings;

    // True when pixels can be placed in patient space.
    bool complete() const noexcept { return has_position && has_orientation && spacing.valid(); }
    // PS3.3 equation C.7.6.2.1-1 with column index `col` and row index `row` (fractions allowed).
    Vec3 pixel_to_patient(double col, double row) const noexcept;
    // Inverse: projects p onto the plane. `distance` (optional) receives the signed distance from
    // the plane along the normal. False when the geometry is incomplete or degenerate.
    bool patient_to_pixel(const Vec3& p, double& col, double& row, double* distance = nullptr) const noexcept;
};

// Pixel spacing for a frame, in the order: Pixel Measures functional group (per-frame, then
// shared), Pixel Spacing (with Imager Pixel Spacing / magnification rules for projection images),
// Imager Pixel Spacing, Nominal Scanned Pixel Spacing, Sequence of Ultrasound Regions, none.
// For ultrasound, `us_point` (column, row; 0-based) selects the region containing it; without a
// point a region is used only when all 2D regions with cm units agree.
PixelSpacingInfo resolve_pixel_spacing(const Dataset& ds, std::uint32_t frame = 0,
                                       const double* us_point = nullptr);

// Plane geometry of a frame (0-based). Functional groups are used first (per-frame, then
// shared), then the classic attributes. Fails only for invalid arguments (frame past the end);
// missing attributes are reported through has_position / has_orientation / spacing.
Result<PlaneGeometry> frame_geometry(const Dataset& ds, std::uint32_t frame = 0);

// ---------------------------------------------------------------- orientation labels
// Letters for a direction in LPS: +x L, -x R, +y P, -y A, +z H, -z F (C.7.6.1.1.1), most
// significant component first; components with |c| <= threshold are omitted ("" for a zero
// vector). Oblique directions give combinations such as "LPH".
std::string orientation_label(const Vec3& direction, double threshold = 1e-4, std::size_t max_letters = 3);

struct EdgeLabels {
    std::string left, right, top, bottom;
};
// Labels at the four image edges; nullopt when the orientation is unknown.
std::optional<EdgeLabels> edge_labels(const PlaneGeometry& g, double threshold = 1e-4);

// ---------------------------------------------------------------- reference lines
struct ReferenceLine {
    bool valid = false;
    double col0 = 0, row0 = 0, col1 = 0, row1 = 0;  // in the target's pixel indices
    bool same_frame_of_reference = false;
    std::string reason;   // why there is no line (parallel planes, incomplete geometry, outside)
};
// Intersection of `other`'s plane with `target`'s plane, clipped to the target image extent
// (-0.5 .. columns-0.5, -0.5 .. rows-0.5) and, when clip_to_other is set, to other's extent too.
// Different (or missing) Frame of Reference UIDs give no line unless allow_other_frame_of_reference.
ReferenceLine reference_line(const PlaneGeometry& target, const PlaneGeometry& other, bool clip_to_other = true,
                             bool allow_other_frame_of_reference = false);

// ---------------------------------------------------------------- series sorting
enum SortWarning : std::uint32_t {
    kSortMissingGeometry = 1u << 0,          // some items lack position or orientation
    kSortMultipleStacks = 1u << 1,           // more than one stack (Stack ID, orientation or offset)
    kSortMixedOrientation = 1u << 2,         // more than one plane orientation
    kSortDuplicatePositions = 1u << 3,       // two items at the same position in one group
    kSortUnevenSpacing = 1u << 4,            // spacing between positions differs within a group
    kSortGantryTilt = 1u << 5,               // positions advance obliquely to the normal
    kSortMultipleFramesOfReference = 1u << 6,
    kSortTemporalSplit = 1u << 7,            // split by temporal position / trigger time (4D)
    kSortSizeMismatch = 1u << 8,             // rows / columns differ between items
};

struct SeriesSortOptions {
    bool expand_frames = true;            // one item per frame for enhanced (functional group) objects
    double duplicate_tolerance = 0.01;    // mm along the normal
    double spacing_tolerance_abs = 0.01;  // mm
    double spacing_tolerance_rel = 0.01;  // of the median spacing
    double orientation_tolerance = 1e-4;  // 1 - |cos| between normals
    double stack_offset_tolerance = 1.0;  // mm: in-plane offset that separates parallel stacks
    double tilt_tolerance_deg = 0.05;
};

struct SortItem {
    std::uint32_t instance = 0;      // index into the input
    std::uint32_t frame = 0;
    PlaneGeometry geometry;
    double position = 0.0;           // along the group normal (mm)
    std::optional<std::int64_t> instance_number;
    std::optional<double> acquisition_time;  // seconds (DT or DA+TM, as written)
    std::optional<double> temporal_index;    // Temporal Position Index / Identifier
    std::optional<double> trigger_time;      // ms
    std::string stack_id;
    std::optional<std::int64_t> in_stack_position;
};

struct SortGroup {
    std::vector<std::uint32_t> items;   // indices into SeriesSort::items, sorted
    std::string stack_id;
    std::optional<double> temporal_key;
    bool temporal_by_trigger = false;
    Vec3 normal;
    double spacing = 0.0;               // median distance between consecutive positions
    bool uneven = false, duplicates = false, tilted = false, geometry = true;
    double tilt_deg = 0.0;
};

struct SeriesSort {
    std::vector<SortItem> items;
    std::vector<std::uint32_t> order;   // all items: groups concatenated
    std::vector<SortGroup> groups;
    std::uint32_t warnings = 0;         // SortWarning bits
};

// Sorts instances (and frames) along the slice normal, then by Instance Number and acquisition
// time, after splitting by Stack ID, temporal position (Temporal Position Index / Identifier, or
// trigger time when positions repeat), orientation and in-plane offset. Never fails: datasets
// without geometry are placed last (by Instance Number) and flagged. nullptr entries are skipped.
SeriesSort sort_series(std::span<const Dataset* const> instances, const SeriesSortOptions& opt = {});

}  // namespace siaara::clinical
