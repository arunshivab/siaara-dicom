// SIAARA DICOM Toolkit - viewport placement of a 2D image (see viewport.hpp).
#include "image/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace siaara::image {

namespace {

// cos / sin of a clockwise angle in degrees, exact for multiples of 90.
void cos_sin(double deg, double& c, double& s) noexcept {
    double r = std::fmod(deg, 360.0);
    if (r < 0) r += 360.0;
    if (r == 0.0) { c = 1; s = 0; return; }
    if (r == 90.0) { c = 0; s = 1; return; }
    if (r == 180.0) { c = -1; s = 0; return; }
    if (r == 270.0) { c = 0; s = -1; return; }
    const double rad = r * 3.14159265358979323846 / 180.0;
    c = std::cos(rad);
    s = std::sin(rad);
}

}  // namespace

Result<ViewportTransform> ViewportTransform::make(const ViewportParams& p, std::uint32_t iw, std::uint32_t ih) {
    if (p.width == 0 || p.height == 0) return Status(Errc::InvalidArgument, "viewport: width and height must be > 0");
    if (iw == 0 || ih == 0) return Status(Errc::InvalidArgument, "viewport: empty image");
    if (!std::isfinite(p.zoom) || p.zoom < 0 || !std::isfinite(p.pan_x) || !std::isfinite(p.pan_y) ||
        !std::isfinite(p.rotation))
        return Status(Errc::InvalidArgument, "viewport: zoom, pan and rotation must be finite (zoom >= 0)");
    double c = 1, s = 0;
    cos_sin(p.rotation, c, s);
    double z = p.zoom;
    if (z == 0.0) {
        const double bw = iw * std::fabs(c) + ih * std::fabs(s);
        const double bh = iw * std::fabs(s) + ih * std::fabs(c);
        z = std::min(p.width / bw, p.height / bh);
    }
    if (!(z > 1e-9) || !(z < 1e9)) return Status(Errc::InvalidArgument, "viewport: zoom out of range");
    ViewportTransform t;
    t.cx_ = iw / 2.0;
    t.cy_ = ih / 2.0;
    t.vx_ = p.width / 2.0 + p.pan_x;
    t.vy_ = p.height / 2.0 + p.pan_y;
    t.zoom_ = z;
    const double fx = p.flip_horizontal ? -1.0 : 1.0, fy = p.flip_vertical ? -1.0 : 1.0;
    t.a_ = z * c * fx;
    t.b_ = -z * s * fy;
    t.c_ = z * s * fx;
    t.d_ = z * c * fy;
    const double det = t.a_ * t.d_ - t.b_ * t.c_;
    t.ia_ = t.d_ / det;
    t.ib_ = -t.b_ / det;
    t.ic_ = -t.c_ / det;
    t.id_ = t.a_ / det;
    return t;
}

void ViewportTransform::to_view(double u, double v, double& x, double& y) const noexcept {
    const double ox = u - cx_, oy = v - cy_;
    x = vx_ + a_ * ox + b_ * oy;
    y = vy_ + c_ * ox + d_ * oy;
}

void ViewportTransform::to_image(double x, double y, double& u, double& v) const noexcept {
    const double ox = x - vx_, oy = y - vy_;
    u = cx_ + ia_ * ox + ib_ * oy;
    v = cy_ + ic_ * ox + id_ * oy;
}

Status place_in_viewport(std::span<const std::uint8_t> src, std::uint32_t iw, std::uint32_t ih, unsigned channels,
                         const ViewportParams& p, std::span<std::uint8_t> dst) {
    if (channels != 1 && channels != 4) return Status(Errc::InvalidArgument, "viewport: 1 or 4 channels");
    if (src.size() < std::uint64_t{iw} * ih * channels) return Status(Errc::Truncated, "viewport: source too short");
    if (dst.size() < std::uint64_t{p.width} * p.height * channels)
        return Status(Errc::BufferTooSmall, "viewport: output buffer too small");
    auto tr = ViewportTransform::make(p, iw, ih);
    if (!tr) return tr.take_error();
    const ViewportTransform& t = *tr;
    double u0 = 0, v0 = 0, u1 = 0, v1 = 0, u2 = 0, v2 = 0;
    t.to_image(0.5, 0.5, u0, v0);
    t.to_image(1.5, 0.5, u1, v1);
    t.to_image(0.5, 1.5, u2, v2);
    const double dux = u1 - u0, dvx = v1 - v0, duy = u2 - u0, dvy = v2 - v0;
    const double W = iw, H = ih;
    const std::size_t stride = std::size_t{iw} * channels;
    const std::uint8_t* s = src.data();
    for (std::uint32_t y = 0; y < p.height; ++y) {
        double u = u0 + duy * y, v = v0 + dvy * y;
        std::uint8_t* out = dst.data() + std::size_t{y} * p.width * channels;
        for (std::uint32_t x = 0; x < p.width; ++x, u += dux, v += dvx, out += channels) {
            if (!(u >= 0.0 && u < W && v >= 0.0 && v < H)) {
                std::memcpy(out, p.background.data(), channels);
                continue;
            }
            if (p.interpolation == ViewportInterpolation::Nearest) {
                const auto i = std::min<std::uint32_t>(static_cast<std::uint32_t>(u), iw - 1);
                const auto j = std::min<std::uint32_t>(static_cast<std::uint32_t>(v), ih - 1);
                std::memcpy(out, s + j * stride + std::size_t{i} * channels, channels);
                continue;
            }
            const double fu = u - 0.5, fv = v - 0.5;
            const double fl_u = std::floor(fu), fl_v = std::floor(fv);
            const double wu = fu - fl_u, wv = fv - fl_v;
            const auto clampi = [](double q, std::uint32_t n) {
                return q < 0 ? 0u : (q >= n - 1.0 ? n - 1 : static_cast<std::uint32_t>(q));
            };
            const std::uint32_t i0 = clampi(fl_u, iw), i1 = clampi(fl_u + 1, iw);
            const std::uint32_t j0 = clampi(fl_v, ih), j1 = clampi(fl_v + 1, ih);
            const std::uint8_t* p00 = s + j0 * stride + std::size_t{i0} * channels;
            const std::uint8_t* p10 = s + j0 * stride + std::size_t{i1} * channels;
            const std::uint8_t* p01 = s + j1 * stride + std::size_t{i0} * channels;
            const std::uint8_t* p11 = s + j1 * stride + std::size_t{i1} * channels;
            for (unsigned k = 0; k < channels; ++k) {
                const double top = p00[k] + (p10[k] - p00[k]) * wu;
                const double bot = p01[k] + (p11[k] - p01[k]) * wu;
                const double val = top + (bot - top) * wv;
                out[k] = static_cast<std::uint8_t>(std::clamp(val + 0.5, 0.0, 255.0));
            }
        }
    }
    return Status::ok();
}

}  // namespace siaara::image
