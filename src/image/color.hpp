// SIAARA DICOM Toolkit - colour models and palettes.
// Written from DICOM PS3.3 C.7.6.3.1.2 (Photometric Interpretation: YBR_FULL, YBR_FULL_422,
// YBR_PARTIAL_420/422, YBR_ICT, YBR_RCT), C.7.6.3.1.5/6 (Palette Color Lookup Tables),
// C.7.9 (Palette Color Lookup Table module, segmented data C.7.9.2), C.7.6.19 (Supplemental
// Palette) and PS3.6 Annex B (standard colour palettes).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/pixel_info.hpp"

namespace siaara::image {

enum class ChromaUpsampling {
    Replicate,  // each chroma sample covers both pixels of its pair
    Linear,     // cosited (PS3.3 C.7.6.3.1.2): odd pixels take the mean of the neighbouring chroma samples
};

struct ColorOptions {
    ChromaUpsampling upsampling = ChromaUpsampling::Replicate;
    // The frame came from a JPEG 2000 codec that already undid the RCT/ICT, so YBR_RCT / YBR_ICT
    // samples are RGB (FrameReader::codec_applies_color_transform()).
    bool codec_applied_mct = false;
    // The frame is in Native Format (4:2:2 data still subsampled). False for codec output.
    bool native_layout = true;
};

// Byte size of interleaved 3-sample output for the frame (rows * columns * 3 * container bytes).
std::uint64_t rgb_frame_bytes(const PixelInfo& pi) noexcept;

// Planar (R..R G..G B..B) <-> interleaved (RGBRGB...) for `pixels` pixels of `samples` samples of
// `bytes_per_sample` bytes. in and out must not overlap.
Status planar_to_interleaved(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint64_t pixels,
                             unsigned samples, unsigned bytes_per_sample);
Status interleaved_to_planar(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint64_t pixels,
                             unsigned samples, unsigned bytes_per_sample);

// Native YBR 4:2:2 (Y1 Y2 Cb Cr per pixel pair) to full interleaved Y Cb Cr.
Status expand_422(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint32_t rows,
                  std::uint32_t columns, unsigned bytes_per_sample, ChromaUpsampling up);

// Converts a decoded frame (FrameReader layout) of a 3-sample image to interleaved RGB samples
// with the frame's own container size (8-bit -> 1 byte, 16 -> 2 bytes LE, 32 -> 4 bytes LE).
// RGB is copied (de-planarised), YBR_* are converted with the inverse of the PS3.3 equations
// (offsets and ranges scaled to Bits Stored), rounded to nearest and clamped. HSV/ARGB/CMYK and
// unknown models fail with Unsupported. out.size() must be rgb_frame_bytes(pi).
Status to_rgb(const PixelInfo& pi, std::span<const std::uint8_t> frame, std::span<std::uint8_t> out,
              const ColorOptions& opt = {});

// Converts a colour frame to interleaved Y Cb Cr full (no conversion: just layout) - used when a
// caller wants the stored YBR values at full resolution.
Status to_interleaved_full(const PixelInfo& pi, std::span<const std::uint8_t> frame, std::span<std::uint8_t> out,
                           const ColorOptions& opt = {});

// ---------------------------------------------------------------- palettes
struct PaletteLut {
    std::uint32_t entries = 0;     // 1..65536
    std::int32_t first_mapped = 0; // first input value mapped
    std::uint16_t bits = 8;        // 8 or 16: entry range 0..255 or 0..65535
    std::vector<std::uint16_t> red, green, blue;  // `entries` values each
    std::vector<std::uint16_t> alpha;             // optional (0028,1204), 8-bit entries
    bool segmented = false;

    // Index for an input value: below first_mapped -> 0, past the end -> entries - 1.
    std::uint32_t index(std::int64_t v) const noexcept {
        const std::int64_t i = v - first_mapped;
        if (i <= 0) return 0;
        if (i >= static_cast<std::int64_t>(entries)) return entries - 1;
        return static_cast<std::uint32_t>(i);
    }
    // Entry scaled to 8 bits (16-bit entries: round(v * 255 / 65535)).
    std::uint8_t to8(std::uint16_t v) const noexcept {
        return bits == 8 ? static_cast<std::uint8_t>(v)
                         : static_cast<std::uint8_t>((std::uint32_t{v} * 255u + 32767u) / 65535u);
    }
};

// Reads (0028,1101-1103/1104) descriptors and (0028,1201-1204) or segmented (0028,1221-1223) data.
// `input_signed` selects how the second descriptor value is read (Pixel Representation).
Result<PaletteLut> read_palette(const Dataset& ds, bool input_signed, bool lenient = true);

// Expands segmented palette data (PS3.3 C.7.9.2) to `entries` values.
Status expand_segmented(std::span<const std::uint16_t> words, std::uint32_t entries, std::vector<std::uint16_t>& out);

// Maps a PALETTE COLOR frame to interleaved RGB at the LUT's own precision (uint16 per sample,
// 0..255 or 0..65535). out.size() == pixels * 3.
Status apply_palette(const PixelInfo& pi, std::span<const std::uint8_t> frame, const PaletteLut& lut,
                     std::span<std::uint16_t> out);

// PS3.6 Annex B standard palettes, generated from the standard by tools/gen_palettes.py.
enum class StandardPalette { HotIron, Pet, HotMetalBlue, Pet20Step };
const PaletteLut& standard_palette(StandardPalette p);
// SOP Instance UID (PS3.6 table B-1) and Content Label of a standard palette.
std::string_view standard_palette_uid(StandardPalette p) noexcept;
std::string_view standard_palette_label(StandardPalette p) noexcept;

}  // namespace siaara::image
