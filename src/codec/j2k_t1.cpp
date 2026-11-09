// SIAARA DICOM Toolkit - JPEG 2000 Tier-1 block decoder (EBCOT), written from T.800 annex D.
// Implements the significance propagation, magnitude refinement and cleanup passes with the MQ
// decoder, and every code-block style option: selective arithmetic coding bypass, context reset,
// termination on every pass, vertically causal contexts, predictable termination (nothing to do
// on decode) and segmentation symbols.
#include <algorithm>
#include <cstring>

#include "codec/j2k_internal.hpp"

namespace siaara::codec::j2k::detail {

namespace {

// Per-sample state word (neighbour bits 0..7 are the kNb* constants).
constexpr std::uint32_t kNegN = 1u << 8;
constexpr std::uint32_t kNegS = 1u << 9;
constexpr std::uint32_t kNegW = 1u << 10;
constexpr std::uint32_t kNegE = 1u << 11;
constexpr std::uint32_t kSig = 1u << 12;
constexpr std::uint32_t kVisit = 1u << 13;
constexpr std::uint32_t kRefined = 1u << 14;
constexpr std::uint32_t kCausalMask = ~(kNbSW | kNbS | kNbSE | kNegS);

inline std::uint32_t sign_index(std::uint32_t f) {
    return ((f >> 1) & 1u) | ((f >> 5) & 2u) | ((f >> 1) & 4u) | ((f >> 1) & 8u) | ((f >> 4) & 0xF0u);
}

// Raw (bypass) bit input, D.6: MSB first, the MSB after a 0xFF byte is a stuffed bit.
class RawDecoder {
public:
    void init(const std::uint8_t* d, std::size_t n) {
        p_ = d;
        end_ = d + n;
        ct_ = 0;
        last_ff_ = false;
    }
    inline int bit() {
        if (ct_ == 0) {
            const std::uint8_t b = p_ < end_ ? *p_++ : 0xFF;
            if (last_ff_) {
                c_ = b & 0x7Fu;
                ct_ = 7;
            } else {
                c_ = b;
                ct_ = 8;
            }
            last_ff_ = b == 0xFF;
        }
        --ct_;
        return static_cast<int>((c_ >> ct_) & 1u);
    }

private:
    const std::uint8_t* p_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    std::uint32_t c_ = 0;
    int ct_ = 0;
    bool last_ff_ = false;
};

class BlockDecoder {
public:
    BlockDecoder(int w, int h, Orient o, bool vcausal, std::uint32_t* flags, std::uint32_t* mag,
                 std::uint8_t* neg, std::size_t ms)
        : w_(w), h_(h), fs_(static_cast<std::size_t>(w) + 2), f_(flags + fs_ + 1), mag_(mag), neg_(neg),
          ms_(ms), sig_(kSigContext[o].data()), vcausal_(vcausal) {
        reset_contexts(cx_);
    }

    void reset() { reset_contexts(cx_); }
    MqDecoder mq;
    RawDecoder raw;

    template <bool kRaw>
    void sigprop(int p) {
        const std::uint32_t one = 3u << p;
        for (int y0 = 0; y0 < h_; y0 += 4) {
            const int rows = std::min(4, h_ - y0);
            for (int x = 0; x < w_; ++x) {
                std::uint32_t* fp = f_ + static_cast<std::size_t>(y0) * fs_ + static_cast<std::size_t>(x);
                for (int r = 0; r < rows; ++r, fp += fs_) {
                    std::uint32_t fl = *fp;
                    if (fl & kSig) continue;
                    if (vcausal_ && r == 3) fl &= kCausalMask;
                    if ((fl & 0xFFu) == 0) continue;
                    int bit;
                    if constexpr (kRaw) bit = raw.bit();
                    else bit = mq.decode(cx_[sig_[fl & 0xFFu]]);
                    if (bit) {
                        int s;
                        if constexpr (kRaw) {
                            s = raw.bit();
                        } else {
                            const std::uint8_t v = kSignContext[sign_index(fl)];
                            s = mq.decode(cx_[v & 0x7Fu]) ^ (v >> 7);
                        }
                        set_significant(fp, s != 0);
                        const std::size_t o = static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x);
                        mag_[o] = one;
                        neg_[o] = static_cast<std::uint8_t>(s);
                    }
                    *fp |= kVisit;
                }
            }
        }
    }

    template <bool kRaw>
    void magref(int p) {
        const std::uint32_t half = 1u << p;
        for (int y0 = 0; y0 < h_; y0 += 4) {
            const int rows = std::min(4, h_ - y0);
            for (int x = 0; x < w_; ++x) {
                std::uint32_t* fp = f_ + static_cast<std::size_t>(y0) * fs_ + static_cast<std::size_t>(x);
                for (int r = 0; r < rows; ++r, fp += fs_) {
                    std::uint32_t fl = *fp;
                    if ((fl & (kSig | kVisit)) != kSig) continue;
                    int bit;
                    if constexpr (kRaw) {
                        bit = raw.bit();
                    } else {
                        if (vcausal_ && r == 3) fl &= kCausalMask;
                        const int ctx = (fl & kRefined) ? kCtxMag0 + 2 : ((fl & 0xFFu) ? kCtxMag0 + 1 : kCtxMag0);
                        bit = mq.decode(cx_[ctx]);
                    }
                    std::uint32_t& m = mag_[static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x)];
                    m = bit ? m + half : m - half;
                    *fp |= kRefined;
                }
            }
        }
    }

    void cleanup(int p, bool segsym) {
        const std::uint32_t one = 3u << p;
        for (int y0 = 0; y0 < h_; y0 += 4) {
            const int rows = std::min(4, h_ - y0);
            for (int x = 0; x < w_; ++x) {
                std::uint32_t* col = f_ + static_cast<std::size_t>(y0) * fs_ + static_cast<std::size_t>(x);
                int r = 0;
                if (rows == 4) {
                    const std::uint32_t a = col[0] | col[fs_] | col[2 * fs_];
                    std::uint32_t last = col[3 * fs_];
                    if (vcausal_) last &= kCausalMask;
                    if (((a | last) & (0xFFu | kSig | kVisit)) == 0) {
                        if (!mq.decode(cx_[kCtxRun])) continue;
                        r = mq.decode(cx_[kCtxUniform]) << 1;
                        r |= mq.decode(cx_[kCtxUniform]);
                        std::uint32_t* fp = col + static_cast<std::size_t>(r) * fs_;
                        std::uint32_t fl = *fp;
                        if (vcausal_ && r == 3) fl &= kCausalMask;
                        const std::uint8_t v = kSignContext[sign_index(fl)];
                        const int s = mq.decode(cx_[v & 0x7Fu]) ^ (v >> 7);
                        set_significant(fp, s != 0);
                        const std::size_t o = static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x);
                        mag_[o] = one;
                        neg_[o] = static_cast<std::uint8_t>(s);
                        ++r;
                    }
                }
                std::uint32_t* fp = col + static_cast<std::size_t>(r) * fs_;
                for (; r < rows; ++r, fp += fs_) {
                    std::uint32_t fl = *fp;
                    if ((fl & (kSig | kVisit)) == 0) {
                        if (vcausal_ && r == 3) fl &= kCausalMask;
                        if (mq.decode(cx_[sig_[fl & 0xFFu]])) {
                            const std::uint8_t v = kSignContext[sign_index(fl)];
                            const int s = mq.decode(cx_[v & 0x7Fu]) ^ (v >> 7);
                            set_significant(fp, s != 0);
                            const std::size_t o = static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x);
                            mag_[o] = one;
                            neg_[o] = static_cast<std::uint8_t>(s);
                        }
                    }
                    *fp &= ~kVisit;
                }
            }
        }
        if (segsym) {
            for (int i = 0; i < 4; ++i) (void)mq.decode(cx_[kCtxUniform]);
        }
    }

private:
    inline void set_significant(std::uint32_t* fp, bool n) {
        const std::size_t s = fs_;
        fp[-static_cast<std::ptrdiff_t>(s) - 1] |= kNbSE;
        fp[-static_cast<std::ptrdiff_t>(s)] |= kNbS | (n ? kNegS : 0u);
        fp[-static_cast<std::ptrdiff_t>(s) + 1] |= kNbSW;
        fp[-1] |= kNbE | (n ? kNegE : 0u);
        fp[1] |= kNbW | (n ? kNegW : 0u);
        fp[s - 1] |= kNbNE;
        fp[s] |= kNbN | (n ? kNegN : 0u);
        fp[s + 1] |= kNbNW;
        *fp |= kSig;
    }

    int w_, h_;
    std::size_t fs_;
    std::uint32_t* f_;
    std::uint32_t* mag_;
    std::uint8_t* neg_;
    std::size_t ms_;
    const std::uint8_t* sig_;
    bool vcausal_;
    std::uint8_t cx_[kNumContexts];
};

}  // namespace

bool decode_t1_block(const BlockParams& bp, std::span<const Segment> segs, std::uint32_t* mag,
                     std::uint8_t* neg, std::size_t stride, std::vector<std::uint32_t>& scratch) {
    const int w = bp.width;
    const int h = bp.height;
    for (int y = 0; y < h; ++y) {
        std::memset(mag + static_cast<std::size_t>(y) * stride, 0, static_cast<std::size_t>(w) * sizeof(std::uint32_t));
        std::memset(neg + static_cast<std::size_t>(y) * stride, 0, static_cast<std::size_t>(w));
    }
    if (segs.empty() || w <= 0 || h <= 0) return true;
    const int top = bp.planes - 1 - bp.zero_planes;
    if (top < 0) return false;
    const std::size_t nflags = (static_cast<std::size_t>(w) + 2) * (static_cast<std::size_t>(h) + 2);
    if (scratch.size() < nflags) scratch.resize(nflags);
    std::fill(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(nflags), 0u);

    BlockDecoder dec(w, h, bp.orient, (bp.style & kCbVCausal) != 0, scratch.data(), mag, neg, stride);
    const bool bypass = (bp.style & kCbBypass) != 0;
    const bool reset = (bp.style & kCbReset) != 0;
    const bool segsym = (bp.style & kCbSegSym) != 0;

    std::size_t seg = 0;
    std::uint32_t left = 0;  // passes remaining in the current segment
    bool raw_seg = false;
    bool first = true;
    int pass = 0;
    for (;;) {
        if (left == 0) {
            if (!first) ++seg;
            while (seg < segs.size() && segs[seg].passes == 0) ++seg;
            if (seg >= segs.size()) break;
            first = false;
            left = segs[seg].passes;
            const int type = pass % 3;
            raw_seg = bypass && pass >= 10 && type != 0;
            if (raw_seg) dec.raw.init(segs[seg].data, segs[seg].len);
            else dec.mq.init(segs[seg].data, segs[seg].len);
        }
        const int type = pass % 3;  // 0 cleanup, 1 significance propagation, 2 refinement
        const int plane = top - (pass + 2) / 3;
        if (plane < 0) break;
        if (reset && pass > 0) dec.reset();
        const bool raw = bypass && pass >= 10 && type != 0;
        if (raw != raw_seg) return true;  // segment structure inconsistent with the style: stop
        if (type == 0) dec.cleanup(plane, segsym);
        else if (type == 1) raw ? dec.sigprop<true>(plane) : dec.sigprop<false>(plane);
        else raw ? dec.magref<true>(plane) : dec.magref<false>(plane);
        ++pass;
        --left;
    }
    return true;
}

}  // namespace siaara::codec::j2k::detail
