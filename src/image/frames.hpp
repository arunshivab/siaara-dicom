// SIAARA DICOM Toolkit - frame access for every supported transfer syntax.
// Written from DICOM PS3.5 8.1 / 8.2 (native and encapsulated encodings), A.4 (fragments, Basic
// Offset Table) and PS3.3 C.7.6.3.1.8 (Extended Offset Table).
//
// A decoded frame is always in the DICOM *native* layout of the data set's own attributes:
//   - little-endian samples of Bits Allocated bits, colour-by-pixel or colour-by-plane as Planar
//     Configuration says; 1-bit frames bit-packed (first pixel in bit 0) and starting at bit 0;
//   - native YBR_FULL_422 / YBR_PARTIAL_422 keep their stored Y Y Cb Cr layout (2 samples per
//     pixel); encapsulated frames come out of the codec with full samples (see codec/codec.hpp);
//   - YBR data is returned as stored; colour conversion is image/color.hpp's job.
// The output of decode() for an encapsulated frame is exactly what a native data set with the same
// attributes would hold for that frame.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "core/transfer_syntax.hpp"
#include "image/pixel_info.hpp"

namespace siaara::image {

struct FrameOptions {
    // Largest buffer the frame API allocates for decoded pixels in one call (decode() returning a
    // vector, decode_range() returning a vector, multi-fragment reassembly). Default 4 GiB.
    std::uint64_t memory_budget = 4ull << 30;
    // Worker threads for decode_range(): 0 = hardware concurrency (at most 8), 1 = calling thread only.
    unsigned threads = 0;
    bool lenient = true;  // accept common non-conformances (recorded in warnings())
    // JPEG chroma upsampling: false = triangular interpolation like libjpeg (default), true =
    // sample replication (the toolkit's behaviour before F5).
    bool jpeg_replicate_chroma = false;
};

class FrameReader {
public:
    // Indexes the frames of `ds` (the top-level data set or an Icon Image item) encoded with the
    // given transfer syntax. The data set's value bytes are shared, not copied: the reader stays
    // valid even if the Dataset object is destroyed.
    static Result<FrameReader> open(const Dataset& ds, std::string_view transfer_syntax_uid,
                                    const FrameOptions& opt = {});

    const PixelInfo& info() const noexcept { return info_; }
    std::uint32_t frame_count() const noexcept { return info_.number_of_frames; }
    bool encapsulated() const noexcept { return encapsulated_; }
    Codec codec() const noexcept { return codec_; }
    // True when the codec itself undoes the JPEG 2000 RCT/ICT (YBR_RCT / YBR_ICT output is RGB).
    bool codec_applies_color_transform() const noexcept {
        return encapsulated_ && (codec_ == Codec::Jpeg2000 || codec_ == Codec::Htj2k);
    }
    const FrameOptions& options() const noexcept { return opt_; }
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    // Size in bytes of one decoded frame (see the layout rules above).
    std::uint64_t frame_bytes() const noexcept { return frame_bytes_; }

    // Decodes frame `index` into out[0 .. frame_bytes()). Thread-safe (const, no shared state).
    Status decode(std::uint32_t index, std::span<std::uint8_t> out) const;
    Result<std::vector<std::uint8_t>> decode(std::uint32_t index) const;

    // Decodes frames [first, first + count) into consecutive frame_bytes() slots of `out`, using
    // up to options().threads workers. Output is identical to decoding one frame at a time; on
    // failure the error of the lowest failing frame index is returned.
    Status decode_range(std::uint32_t first, std::uint32_t count, std::span<std::uint8_t> out) const;
    Result<std::vector<std::uint8_t>> decode_range(std::uint32_t first, std::uint32_t count) const;

    // Native frames whose stored bytes are already in the output layout can be read in place.
    std::optional<std::span<const std::uint8_t>> native_view(std::uint32_t index) const noexcept;

    // Encapsulated only: the encoded bytes of one frame (several fragments are concatenated).
    Status encoded_frame(std::uint32_t index, std::vector<std::uint8_t>& out) const;
    // Encapsulated only: number of fragments that make up frame `index`.
    std::uint32_t fragment_count(std::uint32_t index) const noexcept;

private:
    struct FrameSpan {
        std::uint32_t first = 0;   // index into fragments_
        std::uint32_t count = 0;
        std::uint64_t length = UINT64_MAX;  // Extended Offset Table Lengths limit
    };

    Status index_fragments(const Dataset& ds);
    Status decode_native(std::uint32_t index, std::span<std::uint8_t> out) const;
    Status decode_encapsulated(std::uint32_t index, std::span<std::uint8_t> out) const;

    PixelInfo info_;
    FrameOptions opt_;
    const TransferSyntaxInfo* ts_ = nullptr;
    Codec codec_ = Codec::None;
    bool encapsulated_ = false;
    std::uint64_t frame_bytes_ = 0;
    std::vector<std::string> warnings_;

    // native
    Bytes native_;
    std::uint64_t frame_bits_ = 0;
    std::uint8_t perm_[8] = {};   // byte permutation per sample, when big-endian input needs fixing
    bool permute_ = false;

    // encapsulated
    std::vector<Bytes> fragments_;  // data fragments (Basic Offset Table item excluded)
    std::vector<FrameSpan> frames_;
};

// Bits of a 1-bit packed buffer starting at bit `bit_offset`, copied to out (out bit 0 = first).
// Bits past `bit_count` in the last output byte are cleared.
void copy_bits(const std::uint8_t* in, std::uint64_t bit_offset, std::uint64_t bit_count, std::uint8_t* out) noexcept;

}  // namespace siaara::image
