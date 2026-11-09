// SIAARA DICOM Toolkit - overlay planes and display shutters.
// Written from DICOM PS3.3 C.9.2 (Overlay Plane, including Multi-frame Overlay and the retired
// embedded overlays of PS3.3-2004), C.7.6.11 (Display Shutter) and C.7.6.15 (Bitmap Display Shutter).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/pixel_info.hpp"

namespace siaara::image {

struct Overlay {
    std::uint16_t group = 0x6000;       // 0x6000 .. 0x601E (even)
    std::uint32_t rows = 0;
    std::uint32_t columns = 0;
    std::int32_t origin_row = 1;        // 1-based, may be < 1 (above / left of the image)
    std::int32_t origin_column = 1;
    char type = 'G';                    // 'G' graphics or 'R' ROI
    std::uint32_t frames = 1;           // Number of Frames in Overlay
    std::uint32_t image_frame_origin = 1;
    bool multi_frame = false;           // Number of Frames in Overlay present
    bool embedded = false;              // retired: stored in an unused bit of the Pixel Data
    std::uint16_t bit_position = 0;     // embedded overlays: bit within the pixel container
    Bytes data;                         // Overlay Data (60xx,3000), bit-packed, first pixel in bit 0
    std::string description, label, subtype;

    // Overlay frame for an image frame (0-based), or nullopt if the overlay does not cover it.
    std::optional<std::uint32_t> overlay_frame(std::uint32_t image_frame) const noexcept;
};

// Reads all overlay groups. Groups that are malformed are skipped (lenient) or reported (strict).
Result<std::vector<Overlay>> read_overlays(const Dataset& ds, const PixelInfo& pi, bool lenient = true);

// ORs overlay `ov` for image frame `frame` into `mask` (rows * columns bytes, 1 = overlay bit set).
// `frame_pixels` is the decoded frame (needed for embedded overlays only; may be empty otherwise).
Status overlay_mask(const Overlay& ov, const PixelInfo& pi, std::uint32_t frame,
                    std::span<const std::uint8_t> frame_pixels, std::span<std::uint8_t> mask);

// ---------------------------------------------------------------- shutters
struct Shutters {
    bool rectangular = false, circular = false, polygonal = false, bitmap = false;
    std::int32_t left = 0, right = 0, upper = 0, lower = 0;      // columns / rows, 1-based inclusive
    double center_row = 0, center_column = 0, radius = 0;
    std::vector<std::pair<double, double>> vertices;            // (row, column), 1-based
    std::uint16_t overlay_group = 0;                            // bitmap shutter
    std::optional<std::uint16_t> presentation_value;            // P-Value 0..FFFF
    std::optional<std::array<std::uint16_t, 3>> presentation_lab;  // CIELab, PS3.3 C.10.7.1.1 encoding
    bool any() const noexcept { return rectangular || circular || polygonal || bitmap; }
};

Result<Shutters> read_shutters(const Dataset& ds, bool lenient = true);

// Writes the visibility of every pixel into `visible` (rows * columns bytes): 1 = shown, 0 = shuttered.
// All shapes combine so that the least amount of image remains visible (C.7.6.11).
Status shutter_mask(const Shutters& sh, const PixelInfo& pi, std::uint32_t frame, const std::vector<Overlay>& overlays,
                    std::span<std::uint8_t> visible);

// sRGB colour for the shutter: CIELab if given, else the P-Value as gray, else black.
std::array<std::uint8_t, 3> shutter_rgb(const Shutters& sh) noexcept;

// CIELab (DICOM encoding: L 0..FFFF = 0..100, a/b 0..FFFF = -128..127, D50) to 8-bit sRGB.
std::array<std::uint8_t, 3> cielab_to_srgb(std::uint16_t l, std::uint16_t a, std::uint16_t b) noexcept;

}  // namespace siaara::image
