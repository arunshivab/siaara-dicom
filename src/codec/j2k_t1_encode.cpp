// SIAARA DICOM Toolkit - JPEG 2000 Tier-1 block encoder (EBCOT) and MQ encoder, written from
// T.800 annexes C.2 and D. Default code-block style (one arithmetic codeword segment per block,
// terminated after the last pass), with truncation lengths and distortion decreases recorded for
// every coding pass so the caller can form quality layers.
#include <algorithm>
#include <cstring>

#include "codec/j2k_internal.hpp"

namespace siaara::codec::j2k::detail {

// ---------------------------------------------------------------- MQ encoder (C.2)

void MqEncoder::byte_out() {
    // BYTEOUT (figure C.8). b_ is the byte being completed; have_b_ says whether it belongs to
    // the output yet (the byte before the first one does not).
    auto push = [&](std::uint8_t nb) {
        if (have_b_) out_.push_back(b_);
        b_ = nb;
        have_b_ = true;
    };
    if (have_b_ && b_ == 0xFF) {
        push(static_cast<std::uint8_t>(c_ >> 20));
        c_ &= 0xFFFFFu;
        ct_ = 7;
        return;
    }
    if (c_ < 0x8000000u) {
        push(static_cast<std::uint8_t>(c_ >> 19));
        c_ &= 0x7FFFFu;
        ct_ = 8;
        return;
    }
    ++b_;  // carry (never reaches the byte before the output: see C.2.8)
    if (b_ == 0xFF) {
        c_ &= 0x7FFFFFFu;
        push(static_cast<std::uint8_t>(c_ >> 20));
        c_ &= 0xFFFFFu;
        ct_ = 7;
    } else {
        push(static_cast<std::uint8_t>(c_ >> 19));
        c_ &= 0x7FFFFu;
        ct_ = 8;
    }
}

void MqEncoder::encode(std::uint8_t& cx, int d) {
    const MqState& s = kMqStates[cx];
    const std::uint32_t qe = s.qe;
    a_ -= qe;
    if (d == s.mps) {
        if ((a_ & 0x8000u) != 0) {
            c_ += qe;
            return;
        }
        if (a_ < qe) a_ = qe;
        else c_ += qe;
        cx = s.next_mps;
    } else {
        if (a_ < qe) c_ += qe;
        else a_ = qe;
        cx = s.next_lps;
    }
    do {
        a_ <<= 1;
        c_ <<= 1;
        if (--ct_ == 0) byte_out();
    } while ((a_ & 0x8000u) == 0);
}

std::size_t MqEncoder::flush() {
    // FLUSH with SETBITS (C.2.9).
    const std::uint32_t top = c_ + a_;
    c_ |= 0xFFFFu;
    if (c_ >= top) c_ -= 0x8000u;
    c_ <<= ct_;
    byte_out();
    c_ <<= ct_;
    byte_out();
    if (have_b_ && b_ != 0xFF) out_.push_back(b_);
    have_b_ = false;
    return out_.size() - start_;
}

namespace {

constexpr std::uint32_t kNegN = 1u << 8;
constexpr std::uint32_t kNegS = 1u << 9;
constexpr std::uint32_t kNegW = 1u << 10;
constexpr std::uint32_t kNegE = 1u << 11;
constexpr std::uint32_t kSig = 1u << 12;
constexpr std::uint32_t kVisit = 1u << 13;
constexpr std::uint32_t kRefined = 1u << 14;

inline std::uint32_t sign_index(std::uint32_t f) {
    return ((f >> 1) & 1u) | ((f >> 5) & 2u) | ((f >> 1) & 4u) | ((f >> 1) & 8u) | ((f >> 4) & 0xF0u);
}

class BlockEncoder {
public:
    BlockEncoder(int w, int h, Orient o, const std::uint32_t* mag, const std::uint8_t* neg, std::size_t ms,
                 std::vector<std::uint32_t>& flags, MqEncoder& mq)
        : w_(w), h_(h), fs_(static_cast<std::size_t>(w) + 2), mag_(mag), neg_(neg), ms_(ms),
          sig_(kSigContext[o].data()), mq_(mq) {
        flags.assign(fs_ * (static_cast<std::size_t>(h) + 2), 0u);
        f_ = flags.data() + fs_ + 1;
        reset_contexts(cx_);
    }

    // Each pass returns the decrease of squared error (in units of the quantiser step), using
    // midpoint reconstruction as the decoder does.
    double sigprop(int p) {
        double dd = 0;
        for (int y0 = 0; y0 < h_; y0 += 4) {
            const int rows = std::min(4, h_ - y0);
            for (int x = 0; x < w_; ++x) {
                std::uint32_t* fp = f_ + static_cast<std::size_t>(y0) * fs_ + static_cast<std::size_t>(x);
                for (int r = 0; r < rows; ++r, fp += fs_) {
                    const std::uint32_t fl = *fp;
                    if ((fl & kSig) || (fl & 0xFFu) == 0) continue;
                    const std::size_t o = static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x);
                    const int bit = static_cast<int>((mag_[o] >> p) & 1u);
                    mq_.encode(cx_[sig_[fl & 0xFFu]], bit);
                    if (bit) {
                        code_sign(fp, o);
                        dd += sig_gain(mag_[o], p);
                    }
                    *fp |= kVisit;
                }
            }
        }
        return dd;
    }

    double magref(int p) {
        double dd = 0;
        for (int y0 = 0; y0 < h_; y0 += 4) {
            const int rows = std::min(4, h_ - y0);
            for (int x = 0; x < w_; ++x) {
                std::uint32_t* fp = f_ + static_cast<std::size_t>(y0) * fs_ + static_cast<std::size_t>(x);
                for (int r = 0; r < rows; ++r, fp += fs_) {
                    const std::uint32_t fl = *fp;
                    if ((fl & (kSig | kVisit)) != kSig) continue;
                    const std::size_t o = static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x);
                    const int ctx = (fl & kRefined) ? kCtxMag0 + 2 : ((fl & 0xFFu) ? kCtxMag0 + 1 : kCtxMag0);
                    mq_.encode(cx_[ctx], static_cast<int>((mag_[o] >> p) & 1u));
                    dd += ref_gain(mag_[o], p);
                    *fp |= kRefined;
                }
            }
        }
        return dd;
    }

    double cleanup(int p) {
        double dd = 0;
        for (int y0 = 0; y0 < h_; y0 += 4) {
            const int rows = std::min(4, h_ - y0);
            for (int x = 0; x < w_; ++x) {
                std::uint32_t* col = f_ + static_cast<std::size_t>(y0) * fs_ + static_cast<std::size_t>(x);
                int r = 0;
                if (rows == 4 && ((col[0] | col[fs_] | col[2 * fs_] | col[3 * fs_]) & (0xFFu | kSig | kVisit)) == 0) {
                    int first = -1;
                    for (int k = 0; k < 4; ++k) {
                        const std::size_t o = static_cast<std::size_t>(y0 + k) * ms_ + static_cast<std::size_t>(x);
                        if ((mag_[o] >> p) & 1u) {
                            first = k;
                            break;
                        }
                    }
                    mq_.encode(cx_[kCtxRun], first >= 0 ? 1 : 0);
                    if (first < 0) continue;
                    mq_.encode(cx_[kCtxUniform], first >> 1);
                    mq_.encode(cx_[kCtxUniform], first & 1);
                    const std::size_t o = static_cast<std::size_t>(y0 + first) * ms_ + static_cast<std::size_t>(x);
                    code_sign(col + static_cast<std::size_t>(first) * fs_, o);
                    dd += sig_gain(mag_[o], p);
                    r = first + 1;
                }
                std::uint32_t* fp = col + static_cast<std::size_t>(r) * fs_;
                for (; r < rows; ++r, fp += fs_) {
                    const std::uint32_t fl = *fp;
                    if ((fl & (kSig | kVisit)) == 0) {
                        const std::size_t o = static_cast<std::size_t>(y0 + r) * ms_ + static_cast<std::size_t>(x);
                        const int bit = static_cast<int>((mag_[o] >> p) & 1u);
                        mq_.encode(cx_[sig_[fl & 0xFFu]], bit);
                        if (bit) {
                            code_sign(fp, o);
                            dd += sig_gain(mag_[o], p);
                        }
                    }
                    *fp &= ~kVisit;
                }
            }
        }
        return dd;
    }

private:
    // Error decrease when a sample becomes significant at plane p (reconstruction 1.5 * 2^p).
    static double sig_gain(std::uint32_t m, int p) {
        const double t = static_cast<double>(m) + 0.5;  // midpoint of the quantisation cell
        const double rec = 1.5 * static_cast<double>(1u << p);
        return t * t - (t - rec) * (t - rec);
    }
    static double ref_gain(std::uint32_t m, int p) {
        const double t = static_cast<double>(m) + 0.5;
        const double s = static_cast<double>(1u << p);
        const double base = static_cast<double>((m >> (p + 1)) << (p + 1));
        const double old_rec = base + s;
        const double new_rec = base + (((m >> p) & 1u) ? 1.5 * s : 0.5 * s);
        return (t - old_rec) * (t - old_rec) - (t - new_rec) * (t - new_rec);
    }

    void code_sign(std::uint32_t* fp, std::size_t o) {
        const bool n = neg_[o] != 0;
        const std::uint8_t v = kSignContext[sign_index(*fp)];
        mq_.encode(cx_[v & 0x7Fu], (n ? 1 : 0) ^ (v >> 7));
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
    std::uint32_t* f_ = nullptr;
    const std::uint32_t* mag_;
    const std::uint8_t* neg_;
    std::size_t ms_;
    const std::uint8_t* sig_;
    MqEncoder& mq_;
    std::uint8_t cx_[kNumContexts];
};

}  // namespace

void encode_t1_block(int width, int height, Orient orient, int planes, const std::uint32_t* mag,
                     const std::uint8_t* neg, std::size_t stride, EncodedBlock& out) {
    out.data.clear();
    out.pass_end.clear();
    out.pass_dist.clear();
    out.passes = 0;
    std::uint32_t maxv = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) maxv = std::max(maxv, mag[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)]);
    const int nbits = static_cast<int>(std::bit_width(maxv));
    out.zero_planes = planes - nbits;
    if (nbits == 0) return;
    std::vector<std::uint32_t> flags;
    MqEncoder mq(out.data);
    mq.init();
    BlockEncoder enc(width, height, orient, mag, neg, stride, flags, mq);
    std::vector<std::size_t> emitted;
    for (int p = nbits - 1; p >= 0; --p) {
        if (p != nbits - 1) {
            out.pass_dist.push_back(enc.sigprop(p));
            emitted.push_back(out.data.size());
            out.pass_dist.push_back(enc.magref(p));
            emitted.push_back(out.data.size());
        }
        out.pass_dist.push_back(enc.cleanup(p));
        emitted.push_back(out.data.size());
    }
    const std::size_t total = mq.flush();
    out.passes = static_cast<int>(emitted.size());
    // Truncation lengths: the bytes emitted so far plus enough of the following ones to cover
    // the coder registers (C holds at most 27 pending bits, plus the byte being completed); a
    // contribution may not end with 0xFF (B.10.7).
    std::size_t prev = 0;
    for (std::size_t i = 0; i < emitted.size(); ++i) {
        std::size_t len = i + 1 == emitted.size() ? total : std::min(total, emitted[i] + 5);
        len = std::max(len, prev);
        while (len < total && len > 0 && out.data[len - 1] == 0xFF) ++len;
        out.pass_end.push_back(static_cast<std::uint32_t>(len));
        prev = len;
    }
}

}  // namespace siaara::codec::j2k::detail
