// SIAARA DICOM Toolkit - Image Pixel description (DICOM PS3.3 C.7.6.3, C.7.6.6 Multi-frame,
// C.7.6.3.1.2 Photometric Interpretation, PS3.5 8.1 / 8.2 for native frame sizes).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara::image {

namespace tags {
constexpr Tag LossyImageCompression       = 0x00282110;
constexpr Tag PixelPresentation           = 0x00089205;
constexpr Tag IconImageSequence           = 0x00880200;
constexpr Tag ExtendedOffsetTable         = 0x7FE00001;
constexpr Tag ExtendedOffsetTableLengths  = 0x7FE00002;
}  // namespace tags

enum class Photometric {
    Unknown,        // absent or a value this toolkit does not define (text kept in PixelInfo)
    Monochrome1,
    Monochrome2,
    PaletteColor,
    Rgb,
    YbrFull,
    YbrFull422,
    YbrPartial422,  // retired, still seen
    YbrPartial420,
    YbrIct,
    YbrRct,
    Hsv,            // retired
    Argb,           // retired
    Cmyk,           // retired
    Xyb,            // JPEG XL
};

enum class SampleKind { Integer, Float32, Float64 };

struct PixelInfoOptions {
    bool lenient = true;  // accept common non-conformances (recorded in PixelInfo::warnings)
    // The pixel data is encapsulated: in lenient mode a Bits Allocated that is not a valid container
    // (12, 65535 ...) is accepted provisionally and bits_from_codestream is set, so that the frame
    // layer takes the bit depth from the codestream.
    bool encapsulated = false;
};

// The validated Image Pixel description of one data set (top level, or an Icon Image item).
struct PixelInfo {
    std::uint32_t rows = 0;
    std::uint32_t columns = 0;
    std::uint16_t samples_per_pixel = 1;
    std::uint16_t bits_allocated = 0;
    std::uint16_t bits_stored = 0;
    std::uint16_t high_bit = 0;
    bool is_signed = false;          // Pixel Representation 1
    bool planar = false;             // Planar Configuration 1 (colour-by-plane)
    Photometric photometric = Photometric::Unknown;
    std::string photometric_text;    // value as found, padding removed
    std::uint32_t number_of_frames = 1;
    bool lossy_attribute = false;    // Lossy Image Compression (0028,2110) == "01"
    SampleKind kind = SampleKind::Integer;
    Tag pixel_tag = siaara::tags::PixelData;  // PixelData, FloatPixelData or DoubleFloatPixelData
    std::vector<std::string> warnings;
    std::uint16_t declared_bits_allocated = 0;  // Bits Allocated as found in the data set
    bool bits_from_codestream = false;          // see PixelInfoOptions::encapsulated

    // Lowest bit of a stored sample within its container: high_bit + 1 - bits_stored (0 normally).
    unsigned low_bit() const noexcept { return static_cast<unsigned>(high_bit + 1u - bits_stored); }
    bool is_monochrome() const noexcept {
        return photometric == Photometric::Monochrome1 || photometric == Photometric::Monochrome2;
    }
    // Native (uncompressed) YBR 4:2:2 stores Y Y Cb Cr per two pixels (PS3.3 C.7.6.3.1.2).
    bool native_subsampled() const noexcept {
        return photometric == Photometric::YbrFull422 || photometric == Photometric::YbrPartial422;
    }
    // Pixels per frame (rows * columns); never overflows (both are 16-bit attributes).
    std::uint64_t pixels() const noexcept { return std::uint64_t{rows} * columns; }
    // Bits of one frame in Native Format, and its byte size rounded up (1-bit frames need not
    // start on a byte boundary: PS3.5 8.1.1 packs frames contiguously).
    std::uint64_t native_frame_bits() const noexcept;
    std::uint64_t native_frame_bytes() const noexcept { return (native_frame_bits() + 7) / 8; }
    // Size of a frame as the codecs produce it (full samples per pixel, codec.hpp layout).
    std::uint64_t decoded_frame_bytes() const noexcept { return frame_info().frame_bytes(); }
    codec::FrameInfo frame_info() const noexcept;
};

Photometric parse_photometric(std::string_view text) noexcept;
std::string_view photometric_name(Photometric p) noexcept;

// Reads and validates the Image Pixel module (plus Number of Frames). Fails with Malformed for
// values the standard forbids (BitsAllocated outside {1,8,16,32,64}, BitsStored > BitsAllocated,
// zero rows/columns, wrong Samples per Pixel for the Photometric Interpretation ...), NotFound when
// there is no pixel data element, and LimitExceeded when the sizes overflow 64 bits.
Result<PixelInfo> read_pixel_info(const Dataset& ds, const PixelInfoOptions& opt = {});

// Number of Frames (0028,0008), 1 when absent. Fails like read_pixel_info.
Result<std::uint32_t> frame_count(const Dataset& ds, const PixelInfoOptions& opt = {});

// The Icon Image Sequence (0088,0200) item, or nullptr.
const Dataset* icon_image(const Dataset& ds) noexcept;

}  // namespace siaara::image
