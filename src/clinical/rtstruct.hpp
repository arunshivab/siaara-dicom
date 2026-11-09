// SIAARA DICOM Toolkit - RT Structure Sets.
// Written from DICOM PS3.3 A.19 (RT Structure Set IOD), C.8.8.5 (Structure Set module), C.8.8.6
// (ROI Contour module: Contour Geometric Type, Contour Data, Contour Image Sequence, ROI Display
// Color) and C.8.8.8 (RT ROI Observations module).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clinical/geometry.hpp"
#include "clinical/roi.hpp"
#include "clinical/seg.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara::clinical {

// ---------------------------------------------------------------- RT Structure Set
struct RtContour {
    std::int64_t number = 0;
    std::string geometric_type;              // POINT / OPEN_PLANAR / OPEN_NONPLANAR / CLOSED_PLANAR
    std::vector<Vec3> points;                // patient coordinates (mm)
    std::vector<SourceReference> images;     // Contour Image Sequence
};

struct RtRoi {
    std::int64_t number = 0;
    std::string name, description, generation_algorithm, interpreted_type, frame_of_reference_uid;
    std::optional<std::array<std::uint8_t, 3>> color;  // ROI Display Color
    std::vector<RtContour> contours;
};

struct RtStructureSet {
    std::string label;
    std::vector<RtRoi> rois;
    std::vector<SourceReference> referenced_images;  // Referenced Frame of Reference > ... > Contour Image
    std::vector<std::string> warnings;
};

struct RtReadOptions {
    bool lenient = true;
    std::uint64_t max_points = 1ull << 26;  // over all contours
};

Result<RtStructureSet> read_rtstruct(const Dataset& ds, const RtReadOptions& opt = {});

struct ContourOnImage {
    std::uint32_t roi = 0, contour = 0;      // indices into rois / contours
    std::vector<Point2> points;              // PIXEL-unit image coordinates (centre of pixel c = c + 0.5)
    bool closed = false;
    bool by_reference = false;               // matched by SOP Instance UID (else by plane)
};
// Contours of every ROI that belong on the image: those whose Contour Image Sequence references
// `sop_instance_uid`; contours without image references are matched by plane (same Frame of
// Reference, every point within tolerance of the plane; tolerance < 0 means half the slice
// thickness, or 0.5 mm when unknown).
std::vector<ContourOnImage> contours_on_image(const RtStructureSet& ss, const PlaneGeometry& image,
                                              std::string_view sop_instance_uid, double tolerance_mm = -1.0);

}  // namespace siaara::clinical
