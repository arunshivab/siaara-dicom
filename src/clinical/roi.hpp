// SIAARA DICOM Toolkit - 2D regions of interest and measurements on a decoded frame.
// Image coordinates follow the PIXEL annotation units of PS3.3 C.10.5.1.2: x = column, y = row,
// (0,0) is the top left corner of the top left pixel, so pixel (c, r) covers [c, c+1) x [r, r+1)
// and its centre is (c + 0.5, r + 0.5). Values are modality values (e.g. from
// image::decode_modality_f32). Distances use the patient geometry of C.7.6.2.1.1 when complete,
// otherwise the resolved pixel spacing, otherwise pixels.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "clinical/geometry.hpp"
#include "core/status.hpp"

namespace siaara::clinical {

struct Point2 {
    double x = 0.0, y = 0.0;
};

enum class RoiShape : std::uint8_t { Rectangle, Ellipse, Circle, Polygon, Freehand };

struct Roi {
    RoiShape shape = RoiShape::Rectangle;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // rectangle corners
    double cx = 0, cy = 0, rx = 0, ry = 0;   // circle (rx = radius) / ellipse centre and semi-axes
    double angle = 0;                        // ellipse: rotation of the rx axis from +x towards +y (radians)
    std::vector<Point2> vertices;            // polygon / freehand (closed implicitly)

    static Roi rectangle(double x0, double y0, double x1, double y1);
    static Roi circle(double cx, double cy, double radius);
    static Roi ellipse(double cx, double cy, double rx, double ry, double angle = 0.0);
    static Roi polygon(std::vector<Point2> v);
    static Roi freehand(std::vector<Point2> v);
    // Shape area in pixel units (analytic; polygons by the shoelace formula).
    double area() const noexcept;
};

struct RoiOptions {
    // false: a pixel belongs to the ROI when its centre is inside (weight 1).
    // true:  each pixel is weighted by the fraction of its area inside the shape (exact for
    //        rectangles and polygons; ellipses and circles use a 1024-gon of equal area).
    bool partial_pixels = false;
};

struct RoiStats {
    double count = 0.0;          // number of pixels (sum of weights with partial_pixels)
    double sum = 0.0;            // sum of weight x value
    double mean = 0.0;
    double sd = 0.0;             // population standard deviation (weighted)
    double sd_sample = 0.0;      // with the n - 1 (unbiased, frequency weights) denominator
    double min = 0.0, max = 0.0; // over pixels with a non-zero weight
    double area_px = 0.0;        // shape area in pixel units (Roi::area)
    std::optional<double> area_mm2;      // area_px x row spacing x column spacing
    SpacingSource spacing_source = SpacingSource::None;
    bool spacing_at_detector = false;
    std::uint64_t pixels = 0;    // pixels with a non-zero weight
};

// Statistics of `values` (rows x columns, one sample per pixel) inside `roi`. NaN values are
// skipped. Fails for a size mismatch or a degenerate shape (no area, < 3 vertices, non-finite).
Result<RoiStats> roi_stats(std::span<const float> values, std::uint32_t rows, std::uint32_t columns, const Roi& roi,
                           const PixelSpacingInfo* spacing = nullptr, const RoiOptions& opt = {});

// Per-pixel weights (rows x columns, 0..1) of a shape: the mask roi_stats uses.
Result<std::vector<float>> roi_mask(std::uint32_t rows, std::uint32_t columns, const Roi& roi, const RoiOptions& opt = {});

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
