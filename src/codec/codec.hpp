// SIAARA DICOM Toolkit - common codec interface.
// Every pixel codec decodes one frame into the DICOM *native* layout:
//   - little-endian samples of BitsAllocated bits (8 -> 1 byte, 16 -> 2 bytes, 32 -> 4 bytes)
//   - colour-by-pixel (PlanarConfiguration 0) unless FrameInfo::planar is set
//   - YBR data is returned as stored in the codestream; colour conversion is the image layer's job,
//     except where the codec itself defines it (JPEG 2000 RCT/ICT is undone inside the codec).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/status.hpp"

namespace siaara::codec {

struct FrameInfo {
    std::uint32_t rows = 0;
    std::uint32_t columns = 0;
    std::uint16_t samples_per_pixel = 1;
    std::uint16_t bits_allocated = 16;
    std::uint16_t bits_stored = 16;
    bool is_signed = false;           // PixelRepresentation == 1
    bool planar = false;              // PlanarConfiguration == 1 (output layout requested/given)

    // Decoder resource limits (0 selects the default). They bound what a hostile codestream can
    // make a decoder allocate or do; valid images need far less.
    //   memory_budget: working memory a decoder may allocate besides `out` (JPEG component
    //                  planes and coefficients, JPEG 2000 tile buffers and code-block metadata).
    //                  Exceeding it fails with Errc::LimitExceeded.
    //   max_scans:     JPEG scans per frame (progressive JPEG); more fail with LimitExceeded.
    std::uint64_t memory_budget = 0;
    std::uint32_t max_scans = 0;
    static constexpr std::uint64_t kDefaultMemoryBudget = std::uint64_t{4} << 30;  // 4 GiB
    static constexpr std::uint32_t kDefaultMaxScans = 1000;
    std::uint64_t effective_memory_budget() const noexcept { return memory_budget ? memory_budget : kDefaultMemoryBudget; }
    std::uint32_t effective_max_scans() const noexcept { return max_scans ? max_scans : kDefaultMaxScans; }

    // Exact native frame size in bytes, or 0 on overflow / invalid parameters.
    std::uint64_t frame_bytes() const noexcept {
        if (rows == 0 || columns == 0 || samples_per_pixel == 0) return 0;
        std::uint64_t px = 0;
        if (!checked_mul(rows, columns, px)) return 0;
        if (!checked_mul(px, samples_per_pixel, px)) return 0;
        if (bits_allocated == 1) return (px + 7) / 8;
        if (bits_allocated % 8 != 0) return 0;
        std::uint64_t bytes = 0;
        if (!checked_mul(px, bits_allocated / 8u, bytes)) return 0;
        return bytes;
    }
};

struct EncodeParams {
    int quality = 90;          // lossy codecs, 1..100
    int near_lossless = 0;     // JPEG-LS NEAR parameter (0 = lossless)
    int predictor = 1;         // JPEG lossless selection value 1..7
    bool lossless = true;      // codecs that offer both
    bool ybr = false;          // colour: convert RGB to YBR before encoding where the codec allows it
    // Chroma subsampling for lossy colour (JPEG): 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0, 3 = 4:1:1.
    int chroma_subsampling = 0;
    // Restart interval in MCUs (JPEG) / lines (JPEG-LS); 0 = none.
    std::uint16_t restart_interval = 0;
};

// Each codec implements a pair of functions with these exact shapes:
//   Status decode_<codec>(std::span<const std::uint8_t> in, const FrameInfo& info,
//                         std::span<std::uint8_t> out);              // out.size() == info.frame_bytes()
//   Status encode_<codec>(std::span<const std::uint8_t> in, const FrameInfo& info,
//                         const EncodeParams& p, std::vector<std::uint8_t>& out);
// The decoder must never write outside `out`, must fail if the codestream's own header disagrees
// with `info` (standards PARSE-35), and must fail if it produces fewer bytes than expected.

Status decode_rle(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out);
Status encode_rle(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                  std::vector<std::uint8_t>& out);

Status decode_jpeg(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out);
Status encode_jpeg(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                   std::vector<std::uint8_t>& out);  // p.lossless selects process 14, else baseline/extended

Status decode_jpegls(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out);
Status encode_jpegls(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                     std::vector<std::uint8_t>& out);

Status decode_j2k(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out);
Status encode_j2k(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                  std::vector<std::uint8_t>& out);  // also handles HTJ2K codestreams on decode

}  // namespace siaara::codec
