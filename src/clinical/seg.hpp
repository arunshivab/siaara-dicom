// SIAARA DICOM Toolkit - segmentations (DICOM SEG).
// Written from DICOM PS3.3 A.51 / C.8.20 (Segmentation IOD and Segmentation Image module:
// Segmentation Type BINARY / FRACTIONAL / LABELMAP, Segment Sequence, Recommended Display CIELab
// Value C.10.7.1.1), C.8.20.3 (Segmentation functional groups: Segment Identification) and
// C.7.6.16.2.6 (Derivation Image functional group).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/geometry.hpp"
#include "clinical/roi.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara::clinical {

struct SourceReference {
    std::string sop_class_uid, sop_instance_uid;
    std::vector<std::int32_t> frames;  // 1-based Referenced Frame Number; empty = frame 1 / all
};

enum class SegmentationType : std::uint8_t { Binary, Fractional, Labelmap, Unknown };

struct Segment {
    std::uint32_t number = 0;
    std::string label, description, algorithm_type, algorithm_name, tracking_id, tracking_uid;
    std::optional<Code> category, type;
    std::optional<std::array<std::uint16_t, 3>> cielab;  // Recommended Display CIELab Value
    std::optional<std::array<std::uint8_t, 3>> rgb;      // converted with cielab_to_rgb8
    std::vector<std::uint32_t> frames;                   // 0-based SEG frames holding this segment;
                                                         // empty for LABELMAP (every frame holds every
                                                         // segment): use Segmentation::frame_count
};

struct SegFrame {
    std::uint32_t segment = 0;              // Referenced Segment Number (0 for LABELMAP)
    std::vector<SourceReference> sources;   // Derivation Image Sequence > Source Image Sequence
    PlaneGeometry geometry;
};

struct Segmentation {
    SegmentationType type = SegmentationType::Unknown;
    std::string fractional_type;            // PROBABILITY / OCCUPANCY
    std::uint32_t max_fractional = 1;       // Maximum Fractional Value (FRACTIONAL)
    std::uint32_t rows = 0, columns = 0;
    // Frames read: Number of Frames, limited in lenient mode (with a warning) to the Per-Frame
    // Functional Groups items and to the frames the Pixel Data holds (R-054).
    std::uint32_t number_of_frames = 0;
    std::uint32_t declared_number_of_frames = 0;  // Number of Frames (0028,0008) as stored
    std::string frame_of_reference_uid;
    std::vector<Segment> segments;
    std::vector<SegFrame> frames;
    std::vector<SourceReference> referenced_images;  // Referenced Series Sequence (common instance reference)
    std::vector<std::string> warnings;

    const Segment* segment(std::uint32_t number) const noexcept;
    // SEG frames holding segment `s` (number_of_frames for LABELMAP).
    std::uint32_t frame_count(const Segment& s) const noexcept;
    // SEG frames (0-based) of `segment_number` derived from the source instance (and 1-based
    // frame; 0 = any frame). LABELMAP frames carry every segment.
    std::vector<std::uint32_t> frames_for_source(std::uint32_t segment_number, std::string_view sop_instance,
                                                 std::uint32_t frame_1based = 0) const;
    // SEG frames of the segment lying in `plane` (same Frame of Reference and orientation,
    // distance along the normal <= tolerance mm): for SEGs without derivation references.
    std::vector<std::uint32_t> frames_on_plane(std::uint32_t segment_number, const PlaneGeometry& plane,
                                               double tolerance_mm = 0.01) const;
};

// Reads the segmentation description (no pixel data is decoded).
Result<Segmentation> read_segmentation(const Dataset& ds, bool lenient = true);

// Decodes SEG frame `frame` (0-based) to rows x columns bytes: BINARY 0/1; FRACTIONAL the stored
// value 0..Maximum Fractional Value; LABELMAP 1 where the label equals segment_number (or the
// label itself, clamped to 255, when segment_number is 0).
Result<std::vector<std::uint8_t>> segment_mask(const Dataset& ds, std::string_view transfer_syntax_uid,
                                               const Segmentation& seg, std::uint32_t frame,
                                               std::uint32_t segment_number = 0);

}  // namespace siaara::clinical
