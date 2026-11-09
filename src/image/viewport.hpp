// SIAARA DICOM Toolkit - placing a rendered 2D image into a viewport: zoom, pan, rotation, flips
// and resampling (nearest neighbour or bilinear). Plain geometry, no DICOM semantics.
//
// Image coordinates are continuous: (0,0) is the top left corner of the top left pixel and pixel
// (i, j) covers [i, i+1) x [j, j+1) (the PIXEL units of PS3.3 C.10.5.1.2). Viewport coordinates
// use the same convention for output pixels.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/status.hpp"

namespace siaara::image {

enum class ViewportInterpolation { Nearest, Linear };

struct ViewportParams {
    std::uint32_t width = 0;   // output size in pixels (both > 0)
    std::uint32_t height = 0;
    double zoom = 0.0;         // output pixels per image pixel; 0 = fit the (rotated) image
    double pan_x = 0.0;        // image centre offset from the viewport centre, output pixels
    double pan_y = 0.0;
    double rotation = 0.0;     // clockwise degrees, applied after the flips
    bool flip_horizontal = false;
    bool flip_vertical = false;
    ViewportInterpolation interpolation = ViewportInterpolation::Nearest;
    std::array<std::uint8_t, 4> background{0, 0, 0, 255};
};

// The affine map between image and viewport coordinates for an image of iw x ih pixels.
class ViewportTransform {
public:
    static Result<ViewportTransform> make(const ViewportParams& p, std::uint32_t iw, std::uint32_t ih);
    void to_view(double u, double v, double& x, double& y) const noexcept;
    void to_image(double x, double y, double& u, double& v) const noexcept;
    double zoom() const noexcept { return zoom_; }

private:
    double cx_ = 0, cy_ = 0, vx_ = 0, vy_ = 0, zoom_ = 1;
    double a_ = 1, b_ = 0, c_ = 0, d_ = 1;  // image offset -> view offset (flip, rotation, zoom)
    double ia_ = 1, ib_ = 0, ic_ = 0, id_ = 1;
};

// Resamples `src` (iw x ih pixels of `channels` bytes: 1 or 4) into `dst` (p.width x p.height
// pixels of `channels` bytes). Pixels outside the image get the background (its first byte for
// one channel).
Status place_in_viewport(std::span<const std::uint8_t> src, std::uint32_t iw, std::uint32_t ih, unsigned channels,
                         const ViewportParams& p, std::span<std::uint8_t> dst);

}  // namespace siaara::image
