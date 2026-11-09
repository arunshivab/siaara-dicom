// SIAARA DICOM Toolkit - JPEG 2000 internals shared by the decoder and the encoder.
// Written from ITU-T T.800 (06/2019) | ISO/IEC 15444-1 and ITU-T T.814 (06/2019) | ISO/IEC 15444-15.
//
// Coefficient representation used between the block coders and dequantisation ("half-bit form"):
// a code-block sample is an unsigned magnitude word plus a sign. When the last magnitude bit
// decoded for a sample belongs to bit-plane p, the word holds the decoded magnitude bits shifted
// left by one, plus a single 1 bit at position p (the midpoint of the remaining uncertainty
// interval). Thus word / 2 is the T.800 E.1 reconstruction with r = 1/2, and word >> 1 is the exact
// value when every bit-plane was decoded. A zero word is an insignificant sample.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/status.hpp"

namespace siaara::codec::j2k::detail {

// ---------------------------------------------------------------- markers (T.800 A.2, T.814 A)
inline constexpr std::uint16_t kSOC = 0xFF4F;
inline constexpr std::uint16_t kCAP = 0xFF50;
inline constexpr std::uint16_t kSIZ = 0xFF51;
inline constexpr std::uint16_t kCOD = 0xFF52;
inline constexpr std::uint16_t kCOC = 0xFF53;
inline constexpr std::uint16_t kTLM = 0xFF55;
inline constexpr std::uint16_t kPRF = 0xFF56;
inline constexpr std::uint16_t kPLM = 0xFF57;
inline constexpr std::uint16_t kPLT = 0xFF58;
inline constexpr std::uint16_t kCPF = 0xFF59;
inline constexpr std::uint16_t kQCD = 0xFF5C;
inline constexpr std::uint16_t kQCC = 0xFF5D;
inline constexpr std::uint16_t kRGN = 0xFF5E;
inline constexpr std::uint16_t kPOC = 0xFF5F;
inline constexpr std::uint16_t kPPM = 0xFF60;
inline constexpr std::uint16_t kPPT = 0xFF61;
inline constexpr std::uint16_t kCRG = 0xFF63;
inline constexpr std::uint16_t kCOM = 0xFF64;
inline constexpr std::uint16_t kSOT = 0xFF90;
inline constexpr std::uint16_t kSOP = 0xFF91;
inline constexpr std::uint16_t kEPH = 0xFF92;
inline constexpr std::uint16_t kSOD = 0xFF93;
inline constexpr std::uint16_t kEOC = 0xFFD9;

// Code-block style bits (T.800 table A.19, T.814 tables A.3/A.4).
inline constexpr std::uint8_t kCbBypass = 0x01;
inline constexpr std::uint8_t kCbReset = 0x02;
inline constexpr std::uint8_t kCbTermAll = 0x04;
inline constexpr std::uint8_t kCbVCausal = 0x08;
inline constexpr std::uint8_t kCbPTerm = 0x10;
inline constexpr std::uint8_t kCbSegSym = 0x20;
inline constexpr std::uint8_t kCbHT = 0x40;
inline constexpr std::uint8_t kCbHTMixed = 0x80;

// Sub-band orientations, in packet order (T.800 B.9).
enum Orient : std::uint8_t { kLL = 0, kHL = 1, kLH = 2, kHH = 3 };

inline constexpr int kMaxLevels = 32;           // T.800 A.6.1: NL <= 32
inline constexpr int kMaxCodedPlanes = 31;      // Mb (+ ROI shift) that fits the half-bit form

// ---------------------------------------------------------------- MQ coder (T.800 annex C)
// One state per (Qe index, MPS sense): index = 2 * I + MPS. Transitions already apply SWITCH.
struct MqState {
    std::uint32_t qe;
    std::uint8_t mps;
    std::uint8_t next_mps;
    std::uint8_t next_lps;
};
extern const std::array<MqState, 94> kMqStates;

// Context labels used by the Tier-1 coder (T.800 tables D.1, D.3, D.4, D.5).
inline constexpr int kCtxSig0 = 0;      // 0..8 significance
inline constexpr int kCtxSign0 = 9;     // 9..13 sign
inline constexpr int kCtxMag0 = 14;     // 14..16 magnitude refinement
inline constexpr int kCtxRun = 17;
inline constexpr int kCtxUniform = 18;
inline constexpr int kNumContexts = 19;

// Initial states (T.800 table D.7).
inline void reset_contexts(std::uint8_t* ctx) {
    for (int i = 0; i < kNumContexts; ++i) ctx[i] = 0;
    ctx[kCtxSig0] = 4 * 2;
    ctx[kCtxRun] = 3 * 2;
    ctx[kCtxUniform] = 46 * 2;
}

class MqDecoder {
public:
    // `data` must stay valid; bytes past `len` read as 0xFF (T.800 D.4.1).
    void init(const std::uint8_t* data, std::size_t len) {
        cur_ = data;
        end_ = data + len;
        c_ = static_cast<std::uint32_t>(len > 0 ? *cur_ : 0xFFu) << 16;
        if (len == 0) cur_ = end_;
        byte_in();
        c_ <<= 7;
        ct_ -= 7;
        a_ = 0x8000;
    }

    inline int decode(std::uint8_t& cx) {
        const MqState& s = kMqStates[cx];
        const std::uint32_t qe = s.qe;
        a_ -= qe;
        int d;
        if ((c_ >> 16) < qe) {
            if (a_ < qe) {
                d = s.mps;
                cx = s.next_mps;
            } else {
                d = s.mps ^ 1;
                cx = s.next_lps;
            }
            a_ = qe;
            renorm();
        } else {
            c_ -= qe << 16;
            if ((a_ & 0x8000u) == 0) {
                if (a_ < qe) {
                    d = s.mps ^ 1;
                    cx = s.next_lps;
                } else {
                    d = s.mps;
                    cx = s.next_mps;
                }
                renorm();
            } else {
                d = s.mps;
            }
        }
        return d;
    }

private:
    inline void renorm() {
        do {
            if (ct_ == 0) byte_in();
            a_ <<= 1;
            c_ <<= 1;
            --ct_;
        } while ((a_ & 0x8000u) == 0);
    }
    // BYTEIN (T.800 C.3.4). cur_ points at the byte consumed last (B) or equals end_.
    inline void byte_in() {
        if (cur_ >= end_) {
            c_ += 0xFF00u;
            ct_ = 8;
            return;
        }
        if (*cur_ == 0xFF) {
            const std::uint8_t* nx = cur_ + 1;
            if (nx >= end_ || *nx > 0x8F) {
                c_ += 0xFF00u;
                ct_ = 8;
            } else {
                cur_ = nx;
                c_ += static_cast<std::uint32_t>(*cur_) << 9;
                ct_ = 7;
            }
        } else {
            ++cur_;
            if (cur_ >= end_) {
                c_ += 0xFF00u;
                ct_ = 8;
            } else {
                c_ += static_cast<std::uint32_t>(*cur_) << 8;
                ct_ = 8;
            }
        }
    }

    const std::uint8_t* cur_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    std::uint32_t a_ = 0;
    std::uint32_t c_ = 0;
    int ct_ = 0;
};

class MqEncoder {
public:
    explicit MqEncoder(std::vector<std::uint8_t>& out) : out_(out) {}
    void init() {
        start_ = out_.size();
        a_ = 0x8000;
        c_ = 0;
        ct_ = 12;
        have_b_ = false;
        b_ = 0;
    }
    void encode(std::uint8_t& cx, int d);
    // Predictable/standard termination (T.800 D.4.2); returns the segment length in bytes.
    std::size_t flush();
    std::size_t bytes_so_far() const { return out_.size() - start_ + (have_b_ ? 1 : 0); }
    // Bytes needed so far to decode everything coded up to now if terminated here (approximate,
    // used by rate control only): current length plus the register contents.
    std::size_t length_estimate() const { return bytes_so_far() + 3; }

private:
    void byte_out();
    std::vector<std::uint8_t>& out_;
    std::size_t start_ = 0;
    std::uint32_t a_ = 0;
    std::uint32_t c_ = 0;
    int ct_ = 0;
    bool have_b_ = false;
    std::uint8_t b_ = 0;
};

// ---------------------------------------------------------------- Tier-1 lookup tables
// Neighbour significance bits: NW N NE W E SW S SE in bits 0..7.
inline constexpr std::uint32_t kNbNW = 1u << 0, kNbN = 1u << 1, kNbNE = 1u << 2, kNbW = 1u << 3,
                               kNbE = 1u << 4, kNbSW = 1u << 5, kNbS = 1u << 6, kNbSE = 1u << 7;
// Significance context for each orientation (LL/LH share a table) from the neighbour byte.
extern const std::array<std::array<std::uint8_t, 256>, 4> kSigContext;
// Sign context and XOR bit from (sigN, sigS, sigW, sigE, negN, negS, negW, negE) bits 0..7:
// value = context | (xorbit << 7).
extern const std::array<std::uint8_t, 256> kSignContext;

// ---------------------------------------------------------------- HT VLC (T.814 annex C)
// Decode table: index (context << 7) | next 7 VLC bits; entry bits: 0..2 codeword length,
// 4..7 rho, 8 u_off, 12..15 e_k, 16..19 e_1. A zero entry is not reachable for valid data.
extern const std::array<std::uint32_t, 1024> kHtVlcInitialLut;
extern const std::array<std::uint32_t, 1024> kHtVlcNonInitialLut;
// The standard's entries for the encoder: c | rho<<3 | u_off<<7 | e_k<<8 | e_1<<12 | w<<16 | len<<23.
extern const std::span<const std::uint32_t> kHtVlcInitialEntries;
extern const std::span<const std::uint32_t> kHtVlcNonInitialEntries;
inline constexpr int kHtMelExponent[13] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5};

// ---------------------------------------------------------------- block decoding
struct Segment {
    const std::uint8_t* data = nullptr;
    std::uint32_t len = 0;
    std::uint32_t passes = 0;
};

struct BlockParams {
    int width = 0;
    int height = 0;
    Orient orient = kLL;
    std::uint8_t style = 0;
    int planes = 0;       // Mb plus any ROI shift: number of magnitude bit-planes of the sub-band
    int zero_planes = 0;  // P from the packet headers
    int ht_p0 = 0;        // HT placeholder pass groups
};

// Decodes one code-block into `mag` (half-bit form, raster order, stride `stride`) and `neg`
// (1 for negative). Both arrays must hold width x height entries at the given stride and are
// fully written (zeros where nothing was decoded). Returns false if the data is unusable; the
// block is then left all-zero, which is what the caller wants for corrupt input.
bool decode_t1_block(const BlockParams& bp, std::span<const Segment> segs, std::uint32_t* mag,
                     std::uint8_t* neg, std::size_t stride, std::vector<std::uint32_t>& scratch);
bool decode_ht_block(const BlockParams& bp, std::span<const Segment> segs, std::uint32_t* mag,
                     std::uint8_t* neg, std::size_t stride, std::vector<std::uint32_t>& scratch);

// ---------------------------------------------------------------- block encoding
struct EncodedBlock {
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> pass_end;  // cumulative byte length after each pass
    std::vector<double> pass_dist;        // distortion decrease of each pass (rate control)
    int zero_planes = 0;
    int passes = 0;
};
// `mag` holds magnitudes of quantised coefficients, `neg` the signs, raster with `stride`.
void encode_t1_block(int width, int height, Orient orient, int planes, const std::uint32_t* mag,
                     const std::uint8_t* neg, std::size_t stride, EncodedBlock& out);
// HT cleanup-only encoding (lossless use); returns false if a magnitude cannot be represented.
bool encode_ht_block(int width, int height, int planes, const std::uint32_t* mag,
                     const std::uint8_t* neg, std::size_t stride, EncodedBlock& out);

// ---------------------------------------------------------------- wavelet transforms (annex F)
// A tile-component's coefficients in Mallat layout: resolution r's LL band occupies the top-left
// (w_r x h_r) corner, and the detail bands of resolution r+1 sit to its right / below. `x0s`/`y0s`
// give the resolution origins on the reference grid of the component (trx0/try0 for r = 0..NL).
struct ResGeom {
    std::int64_t x0, y0, x1, y1;
};
void idwt53(std::int32_t* data, std::size_t stride, std::span<const ResGeom> res);
void idwt97(float* data, std::size_t stride, std::span<const ResGeom> res);
void fdwt53(std::int32_t* data, std::size_t stride, std::span<const ResGeom> res);
void fdwt97(float* data, std::size_t stride, std::span<const ResGeom> res);

// 9/7 lifting constants (T.800 table F.4).
inline constexpr float kAlpha = -1.586134342059924f;
inline constexpr float kBeta = -0.052980118572961f;
inline constexpr float kGamma = 0.882911075530934f;
inline constexpr float kDelta = 0.443506852043971f;
inline constexpr float kK = 1.230174104914001f;

// ---------------------------------------------------------------- small helpers
inline std::int64_t ceil_div(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }  // a >= 0, b > 0
inline std::int64_t ceil_shift(std::int64_t a, int s) { return (a + (std::int64_t{1} << s) - 1) >> s; }

}  // namespace siaara::codec::j2k::detail
