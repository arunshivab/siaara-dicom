// SIAARA DICOM Toolkit - JPEG 2000 Part 1 (ITU-T T.800 | ISO/IEC 15444-1) and High-Throughput
// JPEG 2000 (ITU-T T.814 | ISO/IEC 15444-15), written from the Recommendations.
//
// The public entry points are decode_j2k / encode_j2k in codec/codec.hpp. This header adds a
// header probe, a component-level decoder (for tools and tests that need each component at its
// own resolution) and the finer-grained encoder options.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/codec.hpp"

namespace siaara::codec::j2k {

// What the main header of a codestream says.
struct StreamInfo {
    std::uint32_t width = 0;        // Xsiz - XOsiz
    std::uint32_t height = 0;       // Ysiz - YOsiz
    std::uint32_t x_offset = 0;     // XOsiz
    std::uint32_t y_offset = 0;     // YOsiz
    std::uint32_t tile_width = 0;
    std::uint32_t tile_height = 0;
    std::uint32_t tiles = 0;
    int components = 0;
    int precision = 0;              // of component 0
    bool is_signed = false;         // of component 0
    bool subsampled = false;        // any XRsiz/YRsiz != 1
    int levels = 0;                 // decomposition levels (main COD)
    int layers = 0;
    int progression = 0;            // 0 LRCP .. 4 CPRL
    bool reversible = false;        // 5-3 transform (main COD)
    bool mct = false;
    bool ht = false;                // HT code-blocks in the main COD, or HT signalled in Rsiz
    int cblk_width = 0;
    int cblk_height = 0;
    std::uint8_t cblk_style = 0;
    bool jp2_wrapper = false;
};

Status probe(std::span<const std::uint8_t> in, StreamInfo& out);

// One decoded component at its own resolution: samples in raster order, after the inverse
// colour transform and DC level shift, clamped to the component's precision.
struct Component {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t dx = 1;  // XRsiz
    std::uint32_t dy = 1;  // YRsiz
    int precision = 0;
    bool is_signed = false;
    std::vector<std::int32_t> samples;
};

// `max_samples` bounds the total number of component samples (memory limit).
Status decode_components(std::span<const std::uint8_t> in, std::vector<Component>& out,
                         std::uint64_t max_samples = std::uint64_t{1} << 28);

enum class Progression : std::uint8_t { LRCP = 0, RLCP = 1, RPCL = 2, PCRL = 3, CPRL = 4 };

struct EncodeOptions {
    bool lossless = true;          // 5-3 + RCT, else 9-7 + ICT with quantisation
    bool ht = false;               // HTJ2K block coder (lossless only)
    int levels = 5;                // decomposition levels (reduced for small images)
    int layers = 1;                // quality layers (Part 1 coder only)
    int cblk_width_exp = 6;        // code-block 2^n x 2^n, n in 2..10, sum <= 12
    int cblk_height_exp = 6;
    bool mct = true;               // colour transform for 3-component input
    double quality = 90;           // lossy: 1..100, maps to a target rate (ignored if rate > 0)
    double rate = 0;               // lossy: bits per sample target of the final layer (0 = use quality)
    Progression progression = Progression::LRCP;
    std::uint32_t tile_width = 0;  // 0 = single tile
    std::uint32_t tile_height = 0;
    bool sop = false;
    bool eph = false;
};

Status encode(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeOptions& opt,
              std::vector<std::uint8_t>& out);

}  // namespace siaara::codec::j2k
