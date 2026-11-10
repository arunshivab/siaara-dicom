// SIAARA DICOM Toolkit - image points, line profiles and calibrated measurements on a decoded frame.
// (ROI statistics are not part of the toolkit: applications compute them from the modality values.)
// Image coordinates follow the PIXEL annotation units of PS3.3 C.10.5.1.2: x = column, y = row,
// (0,0) is the top left corner of the top left pixel, so pixel (c, r) covers [c, c+1) x [r, r+1)
// and its centre is (c + 0.5, r + 0.5). Values are modality values (e.g. from
// image::decode_modality_f32). Distances use the patient geometry of C.7.6.2.1.1 when complete,
// otherwise the resolved pixel spacing, otherwise pixels.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "clinical/geometry.hpp"

namespace siaara::clinical {

struct Point2 {
    double x = 0.0, y = 0.0;
};

// ---------------------------------------------------------------- profile and measurements
struct ProfileSample {
    double distance = 0.0;  // from the start, in mm when `mm` is set, else pixels
    double x = 0.0, y = 0.0;
    double value = 0.0;     // bilinear between pixel centres; NaN outside the image
};
struct LineProfile {
    std::vector<ProfileSample> samples;
    bool mm = false;
};
// Samples from a to b (inclusive); samples == 0 means one per pixel of length (at least 2).
LineProfile line_profile(std::span<const float> values, std::uint32_t rows, std::uint32_t columns, Point2 a, Point2 b,
                         const PlaneGeometry* geometry = nullptr, std::uint32_t samples = 0);

enum class MeasureSpace : std::uint8_t { Pixels, Spacing, Patient };
struct Measurement {
    double value = 0.0;       // mm (or pixels), degrees for angles
    MeasureSpace space = MeasureSpace::Pixels;
    SpacingSource spacing_source = SpacingSource::None;
    bool at_detector = false;
};
// Distance between two image points: in patient space (C.7.6.2.1.1) when the geometry is
// complete, with the pixel spacing when only that is known, else in pixels.
Measurement measure_distance(Point2 a, Point2 b, const PlaneGeometry& g);
// Angle at `vertex` between the rays to a and c (0..180 degrees).
Measurement measure_angle(Point2 a, Point2 vertex, Point2 c, const PlaneGeometry& g);
// Cobb angle: the angle between the line a0-a1 and the line b0-b1 (0..90 degrees).
Measurement measure_cobb(Point2 a0, Point2 a1, Point2 b0, Point2 b1, const PlaneGeometry& g);

}  // namespace siaara::clinical
