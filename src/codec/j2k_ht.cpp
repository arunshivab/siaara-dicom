// SIAARA DICOM Toolkit - HTJ2K block decoder, written from ITU-T T.814 clause 7.
// The cleanup pass decodes the MEL, VLC and MagSgn bit-streams of the HT cleanup segment (7.1 to
// 7.3); the SigProp and MagRef passes (7.4, 7.5) refine the result using the HT refinement
// segment. The output is converted to the half-bit form used by dequantisation (7.6).
#include <algorithm>
#include <cstring>

#include "codec/j2k_internal.hpp"

namespace siaara::codec::j2k::detail {

namespace {

// Byte n of the HT cleanup segment as seen by the bit-stream readers (modDcup, 7.1.1).
inline std::uint32_t mod_dcup(const std::uint8_t* d, int lcup, int pos) {
    if (pos == lcup - 1) return 0xFF;
    if (pos == lcup - 2) return d[pos] | 0x0Fu;
    return d[pos];
}

// VLC bit-stream (7.1.4): bytes consumed backwards, bits little-endian.
class VlcReader {
public:
    void init(const std::uint8_t* d, int lcup, int pcup) {
        d_ = d;
        lcup_ = lcup;
        pcup_ = pcup;
        pos_ = lcup - 3;
        last_ = mod_dcup(d, lcup, lcup - 2);
        const std::uint32_t tmp = last_ >> 4;
        const int bits = ((tmp & 7u) < 7u) ? 4 : 3;
        buf_ = tmp & ((1u << bits) - 1u);
        nb_ = bits;
        refill();
    }
    inline void refill() {
        while (nb_ <= 56) {
            std::uint32_t tmp = 0;
            int cnt = 8;
            if (pos_ >= pcup_) {
                tmp = mod_dcup(d_, lcup_, pos_);
                if (last_ > 0x8F && (tmp & 0x7Fu) == 0x7Fu) cnt = 7;
                last_ = tmp;
                --pos_;
            }  // else: past the start of the VLC byte-stream (prefetch or corrupt data): zeros
            buf_ |= static_cast<std::uint64_t>(tmp & ((1u << cnt) - 1u)) << nb_;
            nb_ += cnt;
        }
    }
    inline std::uint32_t peek7() const { return static_cast<std::uint32_t>(buf_ & 0x7Fu); }
    inline void skip(int n) {
        buf_ >>= n;
        nb_ -= n;
        if (nb_ <= 32) refill();
    }
    inline std::uint32_t bits(int n) {
        const auto v = static_cast<std::uint32_t>(buf_ & ((std::uint64_t{1} << n) - 1u));
        skip(n);
        return v;
    }

private:
    const std::uint8_t* d_ = nullptr;
    int lcup_ = 0, pcup_ = 0, pos_ = 0;
    std::uint32_t last_ = 0;
    std::uint64_t buf_ = 0;
    int nb_ = 0;
};

// MagSgn bit-stream (7.1.2): forward, little-endian, with a synthesised 0xFF after Pcup bytes.
class MagSgnReader {
public:
    void init(const std::uint8_t* d, int pcup) {
        d_ = d;
        pcup_ = pcup;
        pos_ = 0;
        last_ = 0;
        buf_ = 0;
        nb_ = 0;
        refill();
    }
    inline void refill() {
        while (nb_ <= 56) {
            const int cnt = last_ == 0xFF ? 7 : 8;
            std::uint32_t tmp = 0xFF;
            if (pos_ < pcup_) tmp = d_[pos_];
            last_ = tmp;
            ++pos_;
            buf_ |= static_cast<std::uint64_t>(tmp & ((1u << cnt) - 1u)) << nb_;
            nb_ += cnt;
        }
    }
    // n <= 32.
    inline std::uint64_t bits(int n) {
        const std::uint64_t v = buf_ & ((std::uint64_t{1} << n) - 1u);
        buf_ >>= n;
        nb_ -= n;
        if (nb_ <= 32) refill();
        return v;
    }

private:
    const std::uint8_t* d_ = nullptr;
    int pcup_ = 0, pos_ = 0;
    std::uint32_t last_ = 0;
    std::uint64_t buf_ = 0;
    int nb_ = 0;
};

// MEL decoder (7.1.3 and 7.3.3).
class MelDecoder {
public:
    void init(const std::uint8_t* d, int lcup, int pcup) {
        d_ = d;
        lcup_ = lcup;
        pos_ = pcup;
        bits_ = 0;
        tmp_ = 0;
        k_ = 0;
        run_ = 0;
        one_ = false;
    }
    inline int bit() {
        if (bits_ == 0) {
            bits_ = tmp_ == 0xFF ? 7 : 8;
            if (pos_ < lcup_) {
                tmp_ = mod_dcup(d_, lcup_, pos_);
                ++pos_;
            } else {
                tmp_ = 0xFF;
            }
        }
        --bits_;
        return static_cast<int>((tmp_ >> bits_) & 1u);
    }
    inline int symbol() {
        if (run_ == 0 && !one_) {
            int e = kHtMelExponent[k_];
            if (bit()) {
                run_ = 1u << e;
                k_ = std::min(12, k_ + 1);
            } else {
                run_ = 0;
                while (e > 0) {
                    run_ = 2 * run_ + static_cast<std::uint32_t>(bit());
                    --e;
                }
                k_ = std::max(0, k_ - 1);
                one_ = true;
            }
        }
        if (run_ > 0) {
            --run_;
            return 0;
        }
        one_ = false;
        return 1;
    }

private:
    const std::uint8_t* d_ = nullptr;
    int lcup_ = 0, pos_ = 0;
    int bits_ = 0;
    std::uint32_t tmp_ = 0;
    int k_ = 0;
    std::uint32_t run_ = 0;
    bool one_ = false;
};

// SigProp bit-stream (7.1.5): forward, little-endian, zeros past the end.
class SigPropReader {
public:
    void init(const std::uint8_t* d, int lref) {
        d_ = d;
        lref_ = lref;
    }
    inline int bit() {
        if (bits_ == 0) {
            bits_ = last_ == 0xFF ? 7 : 8;
            if (pos_ < lref_) tmp_ = d_[pos_++];
            else tmp_ = 0;
            last_ = tmp_;
        }
        const int b = static_cast<int>(tmp_ & 1u);
        tmp_ >>= 1;
        --bits_;
        return b;
    }

private:
    const std::uint8_t* d_ = nullptr;
    int lref_ = 0, pos_ = 0, bits_ = 0;
    std::uint32_t tmp_ = 0, last_ = 0;
};

// MagRef bit-stream (7.1.6): backward from the end of the refinement segment.
class MagRefReader {
public:
    void init(const std::uint8_t* d, int lref) {
        d_ = d;
        pos_ = lref - 1;
    }
    inline int bit() {
        if (bits_ == 0) {
            if (pos_ >= 0) tmp_ = d_[pos_--];
            else tmp_ = 0;
            bits_ = 8;
            if (last_ > 0x8F && (tmp_ & 0x7Fu) == 0x7Fu) bits_ = 7;
            last_ = tmp_;
        }
        const int b = static_cast<int>(tmp_ & 1u);
        tmp_ >>= 1;
        --bits_;
        return b;
    }

private:
    const std::uint8_t* d_ = nullptr;
    int pos_ = 0, bits_ = 0;
    std::uint32_t tmp_ = 0, last_ = 0xFF;
};

inline int exponent_of(std::uint32_t mu) { return mu == 0 ? 0 : static_cast<int>(std::bit_width(mu - 1)) + 1; }

struct QuadInfo {
    std::uint32_t rho = 0, uoff = 0, ek = 0, e1 = 0;
};

// Sample state used by the refinement passes.
constexpr std::uint8_t kStSig = 1;   // significant after the cleanup pass
constexpr std::uint8_t kStZ = 2;     // refinement indicator z_n
constexpr std::uint8_t kStR = 4;     // refinement value r_n
constexpr std::uint8_t kStNeg = 8;   // sign s_n

class HtCleanup {
public:
    // Layout: `smp` holds (mu << 1 | sign) with stride sw_ and two guard columns on each side.
    HtCleanup(int w, int h, std::uint32_t* smp, std::size_t sw) : w_(w), h_(h), smp_(smp), sw_(sw) {}

    bool run(const std::uint8_t* d, int lcup, int sblk_bits) {
        const int scup = static_cast<int>((static_cast<std::uint32_t>(d[lcup - 1]) << 4) + (d[lcup - 2] & 0x0Fu));
        if (scup < 2 || scup > lcup || scup > 4079) return false;
        const int pcup = lcup - scup;
        vlc_.init(d, lcup, pcup);
        mel_.init(d, lcup, pcup);
        ms_.init(d, pcup);
        const int qw = (w_ + 1) / 2;
        const int qh = (h_ + 1) / 2;
        // m_n <= U_q and v < 2^(m_n + 1) must keep mu below 2^sblk_bits.
        const std::uint32_t max_u = static_cast<std::uint32_t>(sblk_bits + 1);
        for (int qy = 0; qy < qh; ++qy) {
            const bool initial = qy == 0;
            std::uint32_t* top = row(2 * qy);
            std::uint32_t* bot = row(2 * qy + 1);
            const std::uint32_t* above = initial ? nullptr : row(2 * qy - 1);
            const std::uint32_t* lut = initial ? kHtVlcInitialLut.data() : kHtVlcNonInitialLut.data();
            for (int qx = 0; qx < qw; qx += 2) {
                const bool pair = qx + 1 < qw;
                QuadInfo q[2];
                for (int k = 0; k < (pair ? 2 : 1); ++k) {
                    const int x = 2 * (qx + k);
                    std::uint32_t c;
                    if (initial) {
                        c = ((top[x - 2] | bot[x - 2]) ? 1u : 0u) + (top[x - 1] ? 2u : 0u) + (bot[x - 1] ? 4u : 0u);
                    } else {
                        c = ((above[x - 1] | above[x]) ? 1u : 0u) + ((top[x - 1] | bot[x - 1]) ? 2u : 0u) +
                            ((above[x + 1] | above[x + 2]) ? 4u : 0u);
                    }
                    if (c == 0 && mel_.symbol() == 0) {
                        q[k] = QuadInfo{};
                    } else {
                        const std::uint32_t e = lut[(c << 7) | vlc_.peek7()];
                        const int len = static_cast<int>(e & 7u);
                        if (len == 0) return false;
                        vlc_.skip(len);
                        q[k].rho = (e >> 4) & 15u;
                        q[k].uoff = (e >> 8) & 1u;
                        q[k].ek = (e >> 12) & 15u;
                        q[k].e1 = (e >> 16) & 15u;
                    }
                    // Provisional significance so the next quad's context sees it.
                    top[x] = (q[k].rho & 1u) ? 2u : 0u;
                    bot[x] = (q[k].rho & 2u) ? 2u : 0u;
                    top[x + 1] = (q[k].rho & 4u) ? 2u : 0u;
                    bot[x + 1] = (q[k].rho & 8u) ? 2u : 0u;
                }
                // Unsigned residuals (7.3.6) in the interleaved order of 7.3.4.
                std::uint32_t u[2] = {0, 0};
                std::uint32_t pfx[2] = {0, 0}, sfx[2] = {0, 0}, ext[2] = {0, 0};
                const bool both = pair && q[0].uoff && q[1].uoff;
                int mel_pair = -1;
                if (initial && both) mel_pair = mel_.symbol();
                bool special2 = false;
                for (int k = 0; k < (pair ? 2 : 1); ++k) {
                    if (!q[k].uoff) continue;
                    if (k == 1 && mel_pair == 0 && pfx[0] > 2) {
                        pfx[1] = vlc_.bits(1) + 1;
                        special2 = true;
                        continue;
                    }
                    if (vlc_.bits(1)) pfx[k] = 1;
                    else if (vlc_.bits(1)) pfx[k] = 2;
                    else pfx[k] = vlc_.bits(1) ? 3 : 5;
                }
                for (int k = 0; k < (pair ? 2 : 1); ++k) {
                    if (!q[k].uoff || (k == 1 && special2)) continue;
                    if (pfx[k] == 3) sfx[k] = vlc_.bits(1);
                    else if (pfx[k] == 5) sfx[k] = vlc_.bits(5);
                }
                for (int k = 0; k < (pair ? 2 : 1); ++k) {
                    if (!q[k].uoff || (k == 1 && special2)) continue;
                    if (sfx[k] >= 28) ext[k] = vlc_.bits(4);
                }
                for (int k = 0; k < (pair ? 2 : 1); ++k) {
                    if (!q[k].uoff) continue;
                    if (k == 1 && special2) u[1] = pfx[1];
                    else u[k] = pfx[k] + sfx[k] + 4 * ext[k] + (mel_pair == 1 ? 2u : 0u);
                }
                // Exponent bounds (7.3.7) and MagSgn values (7.3.8).
                for (int k = 0; k < (pair ? 2 : 1); ++k) {
                    const int x = 2 * (qx + k);
                    std::uint32_t kappa = 1;
                    const std::uint32_t rho = q[k].rho;
                    if (!initial && (rho & (rho - 1)) != 0) {
                        const int emax = std::max(std::max(exponent_of(above[x - 1] >> 1), exponent_of(above[x] >> 1)),
                                                  std::max(exponent_of(above[x + 1] >> 1), exponent_of(above[x + 2] >> 1)));
                        kappa = static_cast<std::uint32_t>(std::max(1, emax - 1));
                    }
                    const std::uint32_t big_u = kappa + u[k];
                    std::uint32_t* cells[4] = {top + x, bot + x, top + x + 1, bot + x + 1};
                    for (int j = 0; j < 4; ++j) {
                        if (!((rho >> j) & 1u)) {
                            *cells[j] = 0;
                            continue;
                        }
                        const std::uint32_t kn = (q[k].ek >> j) & 1u;
                        const std::uint32_t in = (q[k].e1 >> j) & 1u;
                        if (big_u < kn || big_u - kn > max_u || big_u - kn > 32) return false;
                        const int m = static_cast<int>(big_u - kn);
                        std::uint64_t v = m > 0 ? ms_.bits(m) : 0;
                        v += static_cast<std::uint64_t>(in) << m;
                        const std::uint64_t mu = (v >> 1) + 1;
                        if (mu >= (std::uint64_t{1} << sblk_bits)) return false;
                        *cells[j] = static_cast<std::uint32_t>((mu << 1) | (v & 1u));
                    }
                }
            }
        }
        return true;
    }

    std::uint32_t* row(int y) { return smp_ + static_cast<std::size_t>(y) * sw_ + 2; }

private:
    int w_, h_;
    std::uint32_t* smp_;
    std::size_t sw_;
    VlcReader vlc_;
    MelDecoder mel_;
    MagSgnReader ms_;
};

// HT SigProp (7.4): stripe-oriented scan with column groups of four.
void ht_sigprop(int w, int h, std::uint8_t* st, std::size_t ss, const std::uint8_t* d, int lref, bool vcausal) {
    SigPropReader rd;
    rd.init(d, lref);
    auto at = [&](int x, int y) -> std::uint32_t {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return st[static_cast<std::size_t>(y) * ss + static_cast<std::size_t>(x)];
    };
    for (int y0 = 0; y0 < h; y0 += 4) {
        const int y1 = std::min(h, y0 + 4);
        for (int g0 = 0; g0 < w; g0 += 4) {
            const int g1 = std::min(w, g0 + 4);
            for (int x = g0; x < g1; ++x) {
                for (int y = y0; y < y1; ++y) {
                    std::uint8_t& s = st[static_cast<std::size_t>(y) * ss + static_cast<std::size_t>(x)];
                    if (s & kStSig) continue;
                    const bool below_ok = !(vcausal && y + 1 >= y1);
                    std::uint32_t mbr = 0;
                    // Propagation neighbourhood: cleanup significance.
                    mbr |= (at(x - 1, y - 1) | at(x, y - 1) | at(x + 1, y - 1) | at(x - 1, y) | at(x + 1, y)) & kStSig;
                    if (below_ok) mbr |= (at(x - 1, y + 1) | at(x, y + 1) | at(x + 1, y + 1)) & kStSig;
                    // Scan-causal neighbourhood: refinement values decoded earlier in this pass.
                    std::uint32_t rr = at(x - 1, y - 1) | at(x - 1, y) | at(x, y - 1);
                    if (y + 1 < y1) rr |= at(x - 1, y + 1);
                    if (y - 1 < y0) rr |= at(x + 1, y - 1);
                    mbr |= rr & kStR;
                    if (mbr) {
                        s |= kStZ;
                        if (rd.bit()) s |= kStR;
                    }
                }
            }
            for (int x = g0; x < g1; ++x) {
                for (int y = y0; y < y1; ++y) {
                    std::uint8_t& s = st[static_cast<std::size_t>(y) * ss + static_cast<std::size_t>(x)];
                    if ((s & (kStSig | kStR)) == kStR) {
                        if (rd.bit()) s |= kStNeg;
                    }
                }
            }
        }
    }
}

void ht_magref(int w, int h, std::uint8_t* st, std::size_t ss, const std::uint8_t* d, int lref) {
    MagRefReader rd;
    rd.init(d, lref);
    for (int y0 = 0; y0 < h; y0 += 4) {
        const int y1 = std::min(h, y0 + 4);
        for (int x = 0; x < w; ++x) {
            for (int y = y0; y < y1; ++y) {
                std::uint8_t& s = st[static_cast<std::size_t>(y) * ss + static_cast<std::size_t>(x)];
                if (s & kStSig) {
                    s |= kStZ;
                    if (rd.bit()) s |= kStR;
                }
            }
        }
    }
}

}  // namespace

bool decode_ht_block(const BlockParams& bp, std::span<const Segment> segs, std::uint32_t* mag,
                     std::uint8_t* neg, std::size_t stride, std::vector<std::uint32_t>& scratch) {
    const int w = bp.width;
    const int h = bp.height;
    for (int y = 0; y < h; ++y) {
        std::memset(mag + static_cast<std::size_t>(y) * stride, 0, static_cast<std::size_t>(w) * sizeof(std::uint32_t));
        std::memset(neg + static_cast<std::size_t>(y) * stride, 0, static_cast<std::size_t>(w));
    }
    if (segs.empty() || w <= 0 || h <= 0) return true;

    // HT sets: segment 0 is the first cleanup segment (with any placeholder passes), then
    // refinement and cleanup segments alternate. Use the last set with a usable cleanup segment.
    int chosen = -1;
    int set_index = 0, chosen_set = 0;
    for (std::size_t i = 0; i < segs.size(); ++set_index) {
        if (segs[i].len >= 2) {
            chosen = static_cast<int>(i);
            chosen_set = set_index;
        }
        i += 2;
    }
    if (chosen < 0) return true;
    const Segment& cup = segs[static_cast<std::size_t>(chosen)];
    const Segment* ref = static_cast<std::size_t>(chosen) + 1 < segs.size() ? &segs[static_cast<std::size_t>(chosen) + 1] : nullptr;
    int zblk = 1;
    if (ref && ref->len > 0 && ref->passes > 0) zblk = 1 + static_cast<int>(std::min<std::uint32_t>(ref->passes, 2));

    const int sblk = bp.zero_planes + bp.ht_p0 + chosen_set;
    const int p0 = bp.planes - 1 - sblk;  // bit-plane of the cleanup pass
    if (p0 < 0) return false;
    if (p0 == 0) zblk = 1;  // no bit-plane left for refinement

    const int qw = (w + 1) / 2;
    const int qh = (h + 1) / 2;
    const std::size_t sw = static_cast<std::size_t>(2 * qw) + 4;
    const std::size_t nsmp = sw * static_cast<std::size_t>(2 * qh);
    const std::size_t nst = (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) + 3) / 4;
    if (scratch.size() < nsmp + nst) scratch.resize(nsmp + nst);
    std::fill(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(nsmp + nst), 0u);
    HtCleanup cl(w, h, scratch.data(), sw);
    if (!cl.run(cup.data, static_cast<int>(cup.len), sblk + 1)) return false;

    auto* st = reinterpret_cast<std::uint8_t*>(scratch.data() + nsmp);
    const std::size_t ss = static_cast<std::size_t>(w);
    for (int y = 0; y < h; ++y) {
        const std::uint32_t* r = cl.row(y);
        for (int x = 0; x < w; ++x) {
            const std::uint32_t v = r[x];
            std::uint8_t s = 0;
            if (v >> 1) s = static_cast<std::uint8_t>(kStSig | ((v & 1u) ? kStNeg : 0u));
            st[static_cast<std::size_t>(y) * ss + static_cast<std::size_t>(x)] = s;
        }
    }
    if (zblk >= 2) ht_sigprop(w, h, st, ss, ref->data, static_cast<int>(ref->len), (bp.style & kCbVCausal) != 0);
    if (zblk >= 3) ht_magref(w, h, st, ss, ref->data, static_cast<int>(ref->len));

    for (int y = 0; y < h; ++y) {
        const std::uint32_t* r = cl.row(y);
        std::uint32_t* mo = mag + static_cast<std::size_t>(y) * stride;
        std::uint8_t* no = neg + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t s = st[static_cast<std::size_t>(y) * ss + static_cast<std::size_t>(x)];
            const std::uint32_t mu = r[x] >> 1;
            std::uint32_t out = 0;
            if (s & kStZ) {
                const std::uint32_t m2 = 2 * mu + ((s & kStR) ? 1u : 0u);
                if (m2) out = ((2 * m2 + 1) << (p0 - 1));
            } else if (mu) {
                out = (2 * mu + 1) << p0;
            }
            mo[x] = out;
            no[x] = (out && (s & kStNeg)) ? 1 : 0;
        }
    }
    return true;
}

}  // namespace siaara::codec::j2k::detail
