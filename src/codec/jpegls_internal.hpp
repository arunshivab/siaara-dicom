// SIAARA DICOM Toolkit - JPEG-LS internals shared by the encoder and the decoder.
// Written from ITU-T T.87 (06/98), Annex A (regular and run mode), Annex B (multi-component),
// Annex C (marker segments). One template implements the context model for both directions so
// the encoder and decoder cannot drift apart.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "codec/jpegls.hpp"
#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::codec::jpegls::detail {

// Run-length order table J (T.87 A.7.1.2).
inline constexpr int kJ[32] = {0, 0, 0, 0, 1, 1, 1,  1,  2,  2,  2,  2,  3,  3,  3,  3,
                               4, 4, 5, 5, 6, 6, 7,  7,  8,  9,  10, 11, 12, 13, 14, 15};

inline constexpr int kRegularContexts = 365;
inline constexpr int kMinC = -128;
inline constexpr int kMaxC = 127;

// Marker codes (T.87 table C.1 and T.81 table B.1).
inline constexpr std::uint8_t kSOI = 0xD8;
inline constexpr std::uint8_t kEOI = 0xD9;
inline constexpr std::uint8_t kSOS = 0xDA;
inline constexpr std::uint8_t kDNL = 0xDC;
inline constexpr std::uint8_t kDRI = 0xDD;
inline constexpr std::uint8_t kSOF55 = 0xF7;
inline constexpr std::uint8_t kLSE = 0xF8;
inline constexpr std::uint8_t kAPP8 = 0xE8;
inline constexpr std::uint8_t kRST0 = 0xD0;

// Parameters of one scan after defaults are resolved (T.87 A.2.1, C.2.4.1.1).
struct CodingParams {
    int maxval = 0;
    int near = 0;
    int t1 = 0, t2 = 0, t3 = 0;
    int reset = 64;
    int range = 0;
    int qbpp = 0;
    int bpp = 0;
    int limit = 0;
};

// Resolves defaults and validates the result. Errors are Malformed (they come from the input or
// from caller options; the encoder remaps them to InvalidArgument).
Status derive_params(int precision, int near, const PresetParams& preset, CodingParams& out);

// Checks the FrameInfo limits shared by the encoder and the decoder.
Status validate_frame_info(const FrameInfo& info);

struct RegularContext {
    std::int64_t a;
    std::int32_t b;
    std::int32_t c;
    std::int32_t n;
    std::int32_t unused;
};

struct RunContext {
    std::int64_t a;
    std::int32_t n;
    std::int32_t nn;
};

// Smallest k with (n << k) >= a, for n >= 1 and 0 <= a < 2^40.
inline int golomb_k(std::int64_t n, std::int64_t a) noexcept {
    const int d = std::bit_width(static_cast<std::uint64_t>(a)) -
                  std::bit_width(static_cast<std::uint64_t>(n));
    if (d <= 0) return n < a ? 1 : 0;
    return d + ((n << d) < a ? 1 : 0);
}

// Median edge detector (T.87 A.4.1), written as selects so it compiles without branches.
inline int median_predictor(int ra, int rb, int rc) noexcept {
    const int hi = ra > rb ? ra : rb;
    const int lo = ra > rb ? rb : ra;
    int p = ra + rb - rc;
    p = rc <= lo ? hi : p;
    return rc >= hi ? lo : p;
}

// ------------------------------------------------------------------------------------ BitReader
// Reads entropy-coded bits, removing the stuffed bit after each 0xFF byte (T.87 A.1). It stops at
// the first marker (0xFF followed by a byte with the high bit set) and from there on supplies
// zero bits, counting them so the caller can tell whether the scan used bits that do not exist.
// The cache holds 0..63 valid bits, left aligned; bits below the valid ones are always zero.
class BitReader {
public:
    void start(const std::uint8_t* p, const std::uint8_t* end) noexcept {
        p_ = p;
        end_ = end;
        cache_ = 0;
        bits_ = 0;
        virtual_bits_ = 0;
        after_ff_ = false;
        stopped_ = false;
    }

    // Guarantees at least 32 bits in the cache. Where the next 8 input bytes hold no 0xFF the
    // cache is topped up without a data-dependent branch.
    void fill() noexcept {
        if (!after_ff_ && !stopped_ && end_ - p_ >= 8) {
            std::uint64_t w = 0;
            for (int i = 0; i < 8; ++i) w = (w << 8) | p_[i];
            if (!has_ff_byte(w)) {
                const int nb = (63 - bits_) >> 3;  // 0..7 whole bytes
                cache_ |= ((w >> 1) >> (63 - 8 * nb)) << (64 - bits_ - 8 * nb);
                bits_ += 8 * nb;
                p_ += nb;
                return;
            }
        }
        if (bits_ < 32) refill();
    }

    std::uint32_t read(int n) noexcept {  // 0 <= n <= 32, cache must hold n bits
        const std::uint64_t v = (cache_ >> 1) >> (63 - n);
        skip(n);
        return static_cast<std::uint32_t>(v);
    }

    std::uint32_t read_bit() noexcept {
        fill();
        const auto v = static_cast<std::uint32_t>(cache_ >> 63);
        skip(1);
        return v;
    }

    // Limited-length Golomb code (T.87 A.5.3). Returns the mapped error value; sets bad on an
    // impossible code.
    std::uint64_t golomb(int k, int glimit, int qbpp) noexcept {
        fill();
        if (cache_ != 0) {
            const int lz = std::countl_zero(cache_);  // < bits_
            const int total = lz + 1 + k;
            if (lz < glimit - qbpp - 1 && total <= bits_) {
                const std::uint64_t low = ((cache_ << (lz + 1)) >> 1) >> (63 - k);
                skip(total);
                return (static_cast<std::uint64_t>(lz) << k) | low;
            }
        }
        return golomb_slow(k, glimit, qbpp);
    }

    bool bad() const noexcept { return bad_; }
    void set_bad() noexcept { bad_ = true; }

    // True when the decoder has consumed bits beyond the end of the entropy-coded data.
    bool overran() const noexcept { return virtual_bits_ > static_cast<std::uint64_t>(bits_); }

    // Position from which to look for the marker that ends this entropy-coded segment.
    const std::uint8_t* resume_point() const noexcept { return p_; }

private:
    static bool has_ff_byte(std::uint64_t w) noexcept {
        const std::uint64_t inv = ~w;
        return ((inv - 0x0101010101010101ull) & ~inv & 0x8080808080808080ull) != 0;
    }

    void skip(int n) noexcept {  // 0 <= n <= bits_ <= 63
        cache_ <<= n;
        bits_ -= n;
    }

    std::uint64_t golomb_slow(int k, int glimit, int qbpp) noexcept {
        const int max_zeros = glimit - qbpp - 1;
        refill();
        int zeros = 0;
        while (cache_ == 0) {  // a prefix longer than the cache, or past the end of the data
            zeros += bits_;
            bits_ = 0;
            if (zeros > max_zeros) {
                bad_ = true;
                return 0;
            }
            refill();
        }
        const int lz = std::countl_zero(cache_);
        zeros += lz;
        skip(lz + 1);
        if (zeros > max_zeros) {
            bad_ = true;
            return 0;
        }
        fill();
        if (zeros < max_zeros) {
            if (k > 32) {
                bad_ = true;
                return 0;
            }
            return (static_cast<std::uint64_t>(zeros) << k) | read(k);
        }
        return static_cast<std::uint64_t>(read(qbpp)) + 1;
    }

    // Tops the cache up to 56..63 bits.
    void refill() noexcept {
        while (bits_ <= 55) {
            if (!after_ff_ && !stopped_ && end_ - p_ >= 8) {
                std::uint64_t w = 0;
                for (int i = 0; i < 8; ++i) w = (w << 8) | p_[i];
                if (!has_ff_byte(w)) {
                    const int nb = (63 - bits_) >> 3;  // 1..7 whole bytes
                    cache_ |= (w >> (64 - 8 * nb)) << (64 - bits_ - 8 * nb);
                    bits_ += 8 * nb;
                    p_ += nb;
                    continue;
                }
            }
            if (stopped_ || p_ >= end_) {
                stopped_ = true;
                bits_ += 8;  // zero bits
                virtual_bits_ += 8;
                continue;
            }
            const std::uint8_t b = *p_;
            if (after_ff_) {
                // The byte after a data 0xFF carries 7 bits; its high bit was checked to be zero
                // when the 0xFF was taken.
                cache_ |= static_cast<std::uint64_t>(b) << (57 - bits_);
                bits_ += 7;
                after_ff_ = false;
                ++p_;
                continue;
            }
            if (b == 0xFF) {
                if (end_ - p_ < 2 || (p_[1] & 0x80)) {
                    stopped_ = true;
                    continue;
                }
                after_ff_ = true;
            }
            cache_ |= static_cast<std::uint64_t>(b) << (56 - bits_);
            bits_ += 8;
            ++p_;
        }
    }

    const std::uint8_t* p_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    std::uint64_t cache_ = 0;
    int bits_ = 0;
    std::uint64_t virtual_bits_ = 0;
    bool after_ff_ = false;
    bool stopped_ = false;
    bool bad_ = false;
};

// ------------------------------------------------------------------------------------ BitWriter
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

    void put(std::uint64_t v, int n) noexcept {  // 0 <= n <= 56, v < 2^n
        acc_ = (acc_ << n) | v;
        count_ += n;
        for (;;) {
            if (last_ff_) {
                if (count_ < 7) break;
                out_.push_back(static_cast<std::uint8_t>((acc_ >> (count_ - 7)) & 0x7F));
                count_ -= 7;
                last_ff_ = false;
            } else {
                if (count_ < 8) break;
                const auto b = static_cast<std::uint8_t>(acc_ >> (count_ - 8));
                out_.push_back(b);
                count_ -= 8;
                last_ff_ = b == 0xFF;
            }
        }
    }

    void zeros(int n) noexcept {
        while (n > 48) {
            put(0, 48);
            n -= 48;
        }
        put(0, n);
    }

    void golomb(std::uint32_t m, int k, int glimit, int qbpp) noexcept {
        const std::uint32_t high = m >> k;
        const auto max_zeros = static_cast<std::uint32_t>(glimit - qbpp - 1);
        if (high < max_zeros) {
            zeros(static_cast<int>(high));
            const std::uint64_t low = m & ((std::uint64_t{1} << k) - 1);
            put((std::uint64_t{1} << k) | low, k + 1);
        } else {
            zeros(static_cast<int>(max_zeros));
            put(1, 1);
            put(m - 1, qbpp);
        }
    }

    // Pads the last byte with zero bits. A trailing 0xFF gets a zero byte after it so the
    // following marker cannot be mistaken for data.
    void flush() noexcept {
        const int need = last_ff_ ? 7 : 8;
        if (count_ > 0) put(0, need - count_);
        if (last_ff_) put(0, 7);
        acc_ = 0;
        count_ = 0;
        last_ff_ = false;
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t acc_ = 0;
    int count_ = 0;
    bool last_ff_ = false;
};

// ------------------------------------------------------------------------------------ ScanCoder
// Line buffers hold reconstructed samples at indices 1..w; index 0 and w+1 are the edge samples
// of T.87 A.2.1 / A.3. `Lossless` (NEAR == 0) removes the near-lossless arithmetic from the inner
// loops; the coding is identical either way.
template <bool Enc, bool Lossless>
class ScanCoder {
public:
    using Io = std::conditional_t<Enc, BitWriter, BitReader>;

    ScanCoder(const CodingParams& p, Io& io) : p_(p), io_(io) {
        qstep_ = 2 * p.near + 1;
        rspan_ = p.range * qstep_;
        max_code_ = static_cast<std::uint64_t>(2 * p.range + 2);
        // Gradient quantisation table (A.3.3), filled region by region.
        qlut_.resize(static_cast<std::size_t>(2 * p.maxval + 1));
        const auto fill = [&](int lo, int hi, int q) {  // d in [lo, hi]
            lo = std::max(lo, -p.maxval);
            hi = std::min(hi, p.maxval);
            if (lo > hi) return;
            std::fill(qlut_.begin() + (lo + p.maxval), qlut_.begin() + (hi + p.maxval + 1),
                      static_cast<std::int8_t>(q));
        };
        fill(-p.maxval, -p.t3, -4);
        fill(-p.t3 + 1, -p.t2, -3);
        fill(-p.t2 + 1, -p.t1, -2);
        fill(-p.t1 + 1, -p.near - 1, -1);
        fill(-p.near, p.near, 0);
        fill(p.near + 1, p.t1 - 1, 1);
        fill(p.t1, p.t2 - 1, 2);
        fill(p.t2, p.t3 - 1, 3);
        fill(p.t3, p.maxval, 4);
        q_ = qlut_.data() + p.maxval;
        reset_contexts();
    }

    void reset_contexts() noexcept {
        const std::int64_t a0 = std::max(2, (p_.range + 32) / 64);
        for (auto& c : reg_) c = RegularContext{a0, 0, 0, 1, 0};
        for (auto& c : run_) c = RunContext{a0, 1, 0};
    }

    bool failed() const noexcept {
        if constexpr (Enc) return false;
        else return io_.bad();
    }

    // One line of one component (ILV 0 and 1).
    void line(std::int32_t* prev, std::int32_t* cur, const std::int32_t* src, int w, int& ri) {
        cur[0] = prev[1];
        prev[w + 1] = prev[w];
        int x = 1;
        int ra = cur[0];
        int rc = prev[0];
        int rb = prev[1];
        while (x <= w) {
            const int rd = prev[x + 1];
            const int qi = 81 * q_[rd - rb] + 9 * q_[rb - rc] + q_[rc - ra];
            if (qi == 0) {
                x = run_single(prev, cur, src, x, w, ri);
                if (x <= w) {
                    ra = cur[x - 1];
                    rc = prev[x - 1];
                    rb = prev[x];
                }
                continue;
            }
            ra = regular(qi, median_predictor(ra, rb, rc), Enc ? src[x] : 0);
            cur[x] = ra;
            rc = rb;
            rb = rd;
            ++x;
        }
    }

    // One line of nc components coded sample by sample (ILV 2, T.87 B.3).
    void line_interleaved(int nc, std::int32_t* const* prev, std::int32_t* const* cur,
                          const std::int32_t* const* src, int w, int& ri) {
        for (int c = 0; c < nc; ++c) {
            cur[c][0] = prev[c][1];
            prev[c][w + 1] = prev[c][w];
        }
        int x = 1;
        int qi[4] = {0, 0, 0, 0};
        while (x <= w) {
            bool all_flat = true;
            for (int c = 0; c < nc; ++c) {
                const std::int32_t* pv = prev[c];
                const int ra = cur[c][x - 1];
                qi[c] = 81 * q_[pv[x + 1] - pv[x]] + 9 * q_[pv[x] - pv[x - 1]] + q_[pv[x - 1] - ra];
                all_flat = all_flat && qi[c] == 0;
            }
            if (all_flat) {
                x = run_interleaved(nc, prev, cur, src, x, w, ri);
                continue;
            }
            for (int c = 0; c < nc; ++c) {
                const int ra = cur[c][x - 1];
                const int rb = prev[c][x];
                const int rc = prev[c][x - 1];
                cur[c][x] = regular(qi[c], median_predictor(ra, rb, rc), Enc ? src[c][x] : 0);
            }
            ++x;
        }
    }

private:
    int clamp_sample(int v) const noexcept {
        v = v < 0 ? 0 : v;
        return v > p_.maxval ? p_.maxval : v;
    }

    // Prediction error after near-lossless quantisation and modulo reduction (A.4.4, A.4.5).
    // `neg` is 0 or -1 (SIGN = +1 or -1).
    int encode_error(int ix, int px, int neg) const noexcept {
        int e = ((ix - px) ^ neg) - neg;
        if constexpr (!Lossless) e = e > 0 ? (e + p_.near) / qstep_ : -((p_.near - e) / qstep_);
        if (e < 0) e += p_.range;
        if (e >= (p_.range + 1) / 2) e -= p_.range;
        return e;
    }

    // Reconstructed value from the (reduced) signed error; identical in encoder and decoder.
    int reconstruct(int px, int signed_err) const noexcept {
        if constexpr (Lossless) {
            int rx = px + signed_err;
            rx = rx < 0 ? rx + p_.range : rx;
            rx = rx > p_.maxval ? rx - p_.range : rx;
            // A corrupt code can carry an error up to max_code_ / 2 (> RANGE): one wrap is then not
            // enough, and an out-of-range sample would index outside the gradient table.
            if (static_cast<unsigned>(rx) > static_cast<unsigned>(p_.maxval)) rx = ((rx % p_.range) + p_.range) % p_.range;
            return rx;
        } else {
            int rx = px + signed_err * qstep_;
            rx += rx < -p_.near ? rspan_ : 0;
            rx -= rx > p_.maxval + p_.near ? rspan_ : 0;
            return clamp_sample(rx);
        }
    }

    int checked_code(std::uint64_t m) noexcept {
        if (m > max_code_) {
            if constexpr (!Enc) io_.set_bad();
            return 0;
        }
        return static_cast<int>(m);
    }

    int regular(int qi, int pmed, int ix) {
        const int neg = qi >> 31;  // 0 or -1
        RegularContext& c = reg_[(qi ^ neg) - neg];
        const int px = clamp_sample(pmed + ((c.c ^ neg) - neg));
        const int k = golomb_k(c.n, c.a);
        // A.5.2: with NEAR == 0, k == 0 and a negative bias the mapping is mirrored.
        const int flip = (Lossless && k == 0 && 2 * c.b <= -c.n) ? -1 : 0;
        int e;
        if constexpr (Enc) {
            e = encode_error(ix, px, neg);
            const int me = e ^ flip;
            const int m = me >= 0 ? 2 * me : -2 * me - 1;
            io_.golomb(static_cast<std::uint32_t>(m), k, p_.limit, p_.qbpp);
        } else {
            const int m = checked_code(io_.golomb(k, p_.limit, p_.qbpp));
            e = ((m >> 1) ^ -(m & 1)) ^ flip;
        }
        const int rx = reconstruct(px, (e ^ neg) - neg);
        // Context update and bias correction (A.6).
        if constexpr (Lossless) c.b += e;
        else c.b += e * qstep_;
        c.a += e < 0 ? -e : e;
        if (c.n == p_.reset) {
            c.a >>= 1;
            c.b >>= 1;
            c.n >>= 1;
        }
        ++c.n;
        // Bias correction (A.6.2) as selects: at most one of `low`/`high` holds, and outside
        // both cases B already lies in (-N, 0], so the final clamp only acts after a correction.
        const int n = c.n;
        const bool low = c.b <= -n;
        const bool high = c.b > 0;
        c.b += low ? n : 0;
        c.b -= high ? n : 0;
        c.c += (high && c.c < kMaxC) ? 1 : 0;
        c.c -= (low && c.c > kMinC) ? 1 : 0;
        c.b = c.b < -n + 1 ? -n + 1 : c.b;
        c.b = c.b > 0 ? 0 : c.b;
        return rx;
    }

    // Run interruption sample (A.7.2).
    int interruption(int ra, int rb, int ix, int ritype, int ri) {
        RunContext& c = run_[ritype];
        const int px = ritype ? ra : rb;
        const int neg = (ritype == 0 && ra > rb) ? -1 : 0;
        const std::int64_t temp = ritype ? c.a + (c.n >> 1) : c.a;
        const int k = golomb_k(c.n, temp);
        const int glimit = p_.limit - kJ[ri] - 1;
        const bool keep_low = k == 0 && 2 * c.nn < c.n;
        int e;
        int em;
        if constexpr (Enc) {
            e = encode_error(ix, px, neg);
            const bool map = (keep_low && e > 0) || (e < 0 && !keep_low);
            em = 2 * (e < 0 ? -e : e) - ritype - (map ? 1 : 0);
            io_.golomb(static_cast<std::uint32_t>(em), k, glimit, p_.qbpp);
        } else {
            em = checked_code(io_.golomb(k, glimit, p_.qbpp));
            const int t = em + ritype;
            const int map = t & 1;
            const int mag = (t + map) >> 1;
            e = ((map != 0) != keep_low) ? -mag : mag;
        }
        const int rx = reconstruct(px, (e ^ neg) - neg);
        if (e < 0) ++c.nn;
        c.a += (em + 1 - ritype) >> 1;
        if (c.n == p_.reset) {
            c.a >>= 1;
            c.n >>= 1;
            c.nn >>= 1;
        }
        ++c.n;
        return rx;
    }

    // Codes the length of a run (A.7.1). The encoder passes the length it measured; the decoder
    // returns the decoded length. `eol` tells whether the run reaches the end of the line.
    int run_length(int count_if_enc, int remaining, int& ri, bool& eol) {
        if constexpr (Enc) {
            int r = count_if_enc;
            eol = count_if_enc == remaining;
            while (r >= (1 << kJ[ri])) {
                io_.put(1, 1);
                r -= 1 << kJ[ri];
                if (ri < 31) ++ri;
            }
            if (eol) {
                if (r > 0) io_.put(1, 1);
            } else {
                io_.put(0, 1);
                io_.put(static_cast<std::uint64_t>(r), kJ[ri]);
            }
            return count_if_enc;
        } else {
            int n = 0;
            for (;;) {
                if (n == remaining) {
                    eol = true;
                    return n;
                }
                if (io_.read_bit()) {
                    const int len = 1 << kJ[ri];
                    if (remaining - n >= len) {
                        n += len;
                        if (ri < 31) ++ri;
                    } else {
                        n = remaining;
                    }
                } else {
                    io_.fill();
                    const int r = static_cast<int>(io_.read(kJ[ri]));
                    if (n + r >= remaining) {
                        io_.set_bad();
                        eol = true;
                        return remaining;
                    }
                    eol = false;
                    return n + r;
                }
            }
        }
    }

    bool within_near(int a, int b) const noexcept {
        if constexpr (Lossless) return a == b;
        else return std::abs(a - b) <= p_.near;
    }

    int run_single(std::int32_t* prev, std::int32_t* cur, const std::int32_t* src, int x, int w,
                   int& ri) {
        const int ra = cur[x - 1];
        const int remaining = w - x + 1;
        int count = 0;
        if constexpr (Enc) {
            while (count < remaining && within_near(src[x + count], ra)) ++count;
        }
        bool eol = false;
        count = run_length(count, remaining, ri, eol);
        std::fill(cur + x, cur + x + count, ra);
        x += count;
        if (eol) return x;
        const int rb = prev[x];
        const int ritype = within_near(ra, rb) ? 1 : 0;
        cur[x] = interruption(ra, rb, Enc ? src[x] : 0, ritype, ri);
        if (ri > 0) --ri;
        return x + 1;
    }

    // In sample-interleaved mode the interruption samples of all components are coded with the
    // RItype 0 context: a component may match its run value while another does not, so a zero
    // error must be codable.
    int run_interleaved(int nc, std::int32_t* const* prev, std::int32_t* const* cur,
                        const std::int32_t* const* src, int x, int w, int& ri) {
        int ra[4] = {0, 0, 0, 0};
        for (int c = 0; c < nc; ++c) ra[c] = cur[c][x - 1];
        const int remaining = w - x + 1;
        int count = 0;
        if constexpr (Enc) {
            while (count < remaining) {
                bool same = true;
                for (int c = 0; c < nc; ++c) same = same && within_near(src[c][x + count], ra[c]);
                if (!same) break;
                ++count;
            }
        }
        bool eol = false;
        count = run_length(count, remaining, ri, eol);
        for (int c = 0; c < nc; ++c) std::fill(cur[c] + x, cur[c] + x + count, ra[c]);
        x += count;
        if (eol) return x;
        for (int c = 0; c < nc; ++c)
            cur[c][x] = interruption(ra[c], prev[c][x], Enc ? src[c][x] : 0, 0, ri);
        if (ri > 0) --ri;
        return x + 1;
    }

    CodingParams p_;
    Io& io_;
    int qstep_ = 1;
    int rspan_ = 0;
    std::uint64_t max_code_ = 0;
    std::vector<std::int8_t> qlut_;
    const std::int8_t* q_ = nullptr;
    RegularContext reg_[kRegularContexts];
    RunContext run_[2];
};

}  // namespace siaara::codec::jpegls::detail
