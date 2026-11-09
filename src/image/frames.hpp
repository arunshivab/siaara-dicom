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
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "core/transfer_syntax.hpp"
#include "image/pixel_info.hpp"

namespace siaara::image {

// Adjustments made in lenient mode when an encapsulated codestream's own header (JPEG SOF, JPEG-LS
// SOF55, JPEG 2000 SIZ) disagrees with the Image Pixel module. The numeric values are stable (they
// are part of the C API as SIAARA_FRAME_WARN_*). Strict mode refuses such frames instead.
enum class FrameWarningCode : std::uint16_t {
    None = 0,
    // Codestream precision (JPEG 2000 SIZ, JPEG-LS / JPEG SOF P) below Bits Stored: values are used
    // as decoded (no scaling).
    PrecisionBelowBitsStored = 1,
    // Codestream precision above Bits Stored (but within Bits Allocated): values are kept and
    // info().bits_stored reports the codestream precision (taken from frame 0); later frames whose
    // values do not fit that precision fail.
    PrecisionAboveBitsStored = 2,
    // JPEG 2000 component signedness differs from Pixel Representation: the low `precision` bits of
    // each decoded sample are reinterpreted as Pixel Representation says (two's complement or not).
    SignednessMismatch = 3,
    // The codestream has more components than Samples per Pixel: the first ones are used.
    ExtraComponents = 4,
    // The codestream image is larger than Rows x Columns: the top-left Rows x Columns are used.
    CodestreamCropped = 5,
    // The codestream precision exceeds Bits Allocated: frames decode only if every value fits Bits
    // Allocated (otherwise the frame fails); info().bits_stored reports Bits Allocated.
    PrecisionAboveBitsAllocated = 6,
    // Bits Allocated is a container the codec does not produce: decoded at the codestream's own
    // sample size and widened into Bits Allocated.
    ContainerWidened = 7,
    // Bits Allocated is not a valid container (12, 65535 ...): Bits Allocated / Bits Stored are
    // taken from the codestream precision of frame 0 (info() reports them).
    BitsFromCodestream = 8,
    // Single-frame image whose codestream is smaller than Rows x Columns: info() reports the
    // codestream size (Rows / Columns in the data set are taken to be wrong).
    SizeFromCodestream = 9,
    // RLE: a run reaching past the end of its segment was cut, or a segment ended early; the
    // missing bytes are 0.
    RleRunClamped = 10,
    // JPEG: entropy-coded data held Huffman codes that match no table entry; each was replaced by
    // symbol 0 (as the IJG-derived decoders do).
    CorruptEntropyData = 11,
};

struct FrameWarning {
    FrameWarningCode code = FrameWarningCode::None;
    std::uint32_t frame = 0;  // 0-based frame index
    std::string message;      // codestream and attribute values only (never patient data)
};

struct FrameOptions {
    // Largest buffer the frame API allocates for decoded pixels in one call (decode() returning a
    // vector, decode_range() returning a vector, multi-fragment reassembly). Default 4 GiB.
    std::uint64_t memory_budget = 4ull << 30;
    // Worker threads for decode_range(): 0 = hardware concurrency (at most 8), 1 = calling thread only.
    unsigned threads = 0;
    bool lenient = true;  // accept common non-conformances (recorded in warnings())
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
    // Text of every warning, including the messages of frame_warnings().
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    // Codestream / attribute disagreements found in frame 0 when the reader was opened (lenient
    // mode, JPEG family codecs). Other frames are checked when decoded: see check_frame().
    const std::vector<FrameWarning>& frame_warnings() const noexcept { return frame_warnings_; }
    // Bits Stored as the data set declares it (info().bits_stored may differ: see
    // FrameWarningCode::PrecisionAboveBitsStored).
    std::uint16_t declared_bits_stored() const noexcept { return declared_bits_stored_; }
    // The adjustments decode(index) makes for frame `index` (header probe only, no decoding).
    // Fails with the error decode() would report when the frame cannot be adjusted.
    Status check_frame(std::uint32_t index, std::vector<FrameWarning>& out) const;

    // Size in bytes of one decoded frame (see the layout rules above).
    std::uint64_t frame_bytes() const noexcept { return frame_bytes_; }

    // Decodes frame `index` into out[0 .. frame_bytes()). Thread-safe (const, no shared state).
    Status decode(std::uint32_t index, std::span<std::uint8_t> out) const;
    // As above; `warnings` (may be null) receives the adjustments made for this frame.
    Status decode(std::uint32_t index, std::span<std::uint8_t> out, std::vector<FrameWarning>* warnings) const;
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
    Status decode_encapsulated(std::uint32_t index, std::span<std::uint8_t> out,
                               std::vector<FrameWarning>* warnings) const;
    Status frame_data(std::uint32_t index, std::span<const std::uint8_t>& data, std::vector<std::uint8_t>& joined) const;
    void probe_first_frame();

    PixelInfo info_;
    FrameOptions opt_;
    const TransferSyntaxInfo* ts_ = nullptr;
    Codec codec_ = Codec::None;
    bool encapsulated_ = false;
    std::uint64_t frame_bytes_ = 0;
    std::vector<std::string> warnings_;
    std::vector<FrameWarning> frame_warnings_;
    std::vector<FrameWarning> attr_warnings_;  // attribute replacements made at open(), every frame
    std::uint16_t declared_bits_stored_ = 0;

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
