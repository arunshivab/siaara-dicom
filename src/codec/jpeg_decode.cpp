// SIAARA DICOM Toolkit - JPEG decoder (ITU-T T.81 processes 1, 2, 4, 10, 12 and 14).
// Written from T.81: marker syntax (Annex B), Huffman decoding (Annex F.2.2, G.2, H.2),
// progressive refinement (Annex G.1.2), lossless prediction (Annex H.1.2).
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "codec/codec.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpeg_internal.hpp"
#include "core/types.hpp"

namespace siaara::codec {
namespace {

using jpeg_detail::kZigzag;

constexpr int kFastBits = 10;
constexpr int kFastSize = 1 << kFastBits;

// ------------------------------------------------------------------ Huffman decoding table
// fast_len/fast_sym give the symbol for any code of up to kFastBits bits from one lookup.
// fast_val additionally pre-decodes the magnitude bits when code + magnitude fit in kFastBits:
//   bits 0..7 = total bits to consume, bits 8..15 = zero run (AC), bits 16..31 = signed value.
// A zero fast_val entry means "not pre-decoded".
struct HuffTable {
    bool defined = false;
    int nvalues = 0;
    std::uint8_t values[256] = {};
    std::int32_t maxcode[17] = {};
    std::int32_t valoff[17] = {};
    std::uint8_t fast_len[kFastSize] = {};
    std::uint8_t fast_sym[kFastSize] = {};
    std::int32_t fast_val[kFastSize] = {};
};

inline std::int32_t extend(std::int32_t v, int s) {
    return v < (std::int32_t{1} << (s - 1)) ? v - ((std::int32_t{1} << s) - 1) : v;
}

bool build_table(HuffTable& t, const std::uint8_t counts[17], const std::uint8_t* vals, int n, bool ac) {
    t = HuffTable{};
    t.nvalues = n;
    std::memcpy(t.values, vals, static_cast<std::size_t>(n));
    std::uint32_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; ++l) {
        t.valoff[l] = k - static_cast<std::int32_t>(code);
        for (int i = 0; i < counts[l]; ++i, ++k, ++code) {
            if (l > kFastBits) continue;
            const int shift = kFastBits - l;
            const std::uint32_t base = code << shift;
            if (base + (1u << shift) > kFastSize) return false;
            const int sym = vals[k];
            const int s = sym & 15;
            const int run = ac ? sym >> 4 : 0;
            const int mag = ac ? s : sym;
            for (std::uint32_t j = 0; j < (1u << shift); ++j) {
                const std::uint32_t idx = base + j;
                t.fast_len[idx] = static_cast<std::uint8_t>(l);
                t.fast_sym[idx] = static_cast<std::uint8_t>(sym);
                if (ac && s == 0) continue;            // EOB / ZRL / EOBn: handled by the caller
                if (!ac && mag > 15) continue;         // DC sizes > 15 take the slow path
                if (l + mag > kFastBits) continue;
                std::int32_t v = 0;
                if (mag > 0) {
                    const std::int32_t extra =
                        static_cast<std::int32_t>((idx >> (kFastBits - l - mag)) & ((1u << mag) - 1));
                    v = extend(extra, mag);
                }
                t.fast_val[idx] = static_cast<std::int32_t>((static_cast<std::uint32_t>(v) << 16) |
                                                            (static_cast<std::uint32_t>(run) << 8) |
                                                            static_cast<std::uint32_t>(l + mag));
            }
        }
        t.maxcode[l] = counts[l] ? static_cast<std::int32_t>(code) - 1 : -1;
        if (code > (1u << l)) return false;  // over-subscribed code lengths
        code <<= 1;
    }
    t.defined = true;
    return true;
}

// ------------------------------------------------------------------ entropy-coded segment reader
// Bits are kept MSB-first in a 64-bit word. 0xFF 0x00 is unstuffed; any other 0xFF xx stops the
// reader in front of the marker, after which zero bytes are supplied and counted in `pad` so the
// caller can tell when decoding ran past the real data.
struct BitReader {
    const std::uint8_t* d = nullptr;
    std::size_t n = 0;
    std::size_t pos = 0;
    std::uint64_t buf = 0;
    int bits = 0;
    std::uint32_t pad = 0;
    bool marker = false;

    void start(const std::uint8_t* data, std::size_t size, std::size_t p) {
        d = data; n = size; pos = p; buf = 0; bits = 0; pad = 0; marker = false;
    }
    void refill() {
        while (bits <= 56) {
            std::uint32_t b = 0;
            if (!marker && pos < n) {
                b = d[pos];
                if (b == 0xFF) {
                    if (pos + 1 < n && d[pos + 1] == 0) {
                        pos += 2;
                    } else {
                        marker = true;
                        continue;
                    }
                } else {
                    ++pos;
                }
            } else {
                ++pad;
            }
            buf |= static_cast<std::uint64_t>(b) << (56 - bits);
            bits += 8;
        }
    }
    void ensure32() { if (bits < 32) refill(); }
    std::uint32_t peek(int k) const { return static_cast<std::uint32_t>(buf >> (64 - k)); }
    void skip(int k) { buf <<= k; bits -= k; }
    std::int32_t get(int k) {  // 1 <= k <= 16
        if (bits < k) refill();
        const std::uint32_t v = peek(k);
        skip(k);
        return static_cast<std::int32_t>(v);
    }
    bool overrun() const { return static_cast<std::uint64_t>(pad) * 8 > static_cast<std::uint64_t>(bits); }
};

// ------------------------------------------------------------------ frame / scan state
enum class Mode { Sequential, Progressive, Lossless };

struct Comp {
    int id = 0, h = 1, v = 1, tq = 0;
    std::uint32_t bw = 0, bh = 0;    // allocated grid: blocks (DCT) or samples (lossless), MCU padded
    std::uint32_t cw = 0, ch = 0;    // component size in samples
    std::uint32_t nbw = 0, nbh = 0;  // grid covered by a non-interleaved scan
    std::size_t stride = 0;          // plane stride in samples
    std::vector<std::uint16_t> plane;
    std::vector<std::int16_t> coef;  // progressive: 64 per block, zig-zag order
    std::uint16_t qsnap[64] = {};
    bool qsnap_set = false;
    int pred = 0;
    int td = 0, ta = 0;
    bool coded = false;
    std::uint32_t rs_x = 0, rs_y = 0;  // lossless: first sample of the current restart interval
    int pt = 0;                        // lossless: point transform of the scan that coded it
};

struct Scan {
    int n = 0;
    int ci[4] = {};
    int ss = 0, se = 63, ah = 0, al = 0;
};

Status malformed(const char* what, std::size_t off) { return Status(Errc::Malformed, what, off); }

class Decoder {
public:
    Decoder(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out)
        : d_(in.data()), n_(in.size()), info_(info), out_(out) {
        std::memset(coef_al_, -1, sizeof coef_al_);
    }
    Status run();

private:
    Status next_marker(int& m);
    Status segment(std::size_t& begin, std::size_t& end);
    Status parse_dqt(std::size_t b, std::size_t e);
    Status parse_dht(std::size_t b, std::size_t e);
    Status parse_sof(int marker, std::size_t b, std::size_t e);
    Status parse_sos(std::size_t b, std::size_t e, Scan& s);
    Status decode_scan(const Scan& s);
    Status scan_sequential(const Scan& s);
    Status scan_progressive(const Scan& s);
    Status scan_lossless(const Scan& s);
    Status scan_lossless_single(const Scan& s);
    Status restart();
    Status mcu_status() const;
    Status finish_progressive();
    Status write_output();

    template <class F> Status for_each_block(const Scan& s, F&& f);

    // entropy decoding helpers (bits >= 32 on entry for decode_sym / decode_diff)
    int decode_sym(const HuffTable& t) {
        const std::uint32_t look = br_.peek(kFastBits);
        const int len = t.fast_len[look];
        if (len) {
            br_.skip(len);
            return t.fast_sym[look];
        }
        const std::uint32_t c16 = br_.peek(16);
        for (int l = kFastBits + 1; l <= 16; ++l) {
            const std::int32_t c = static_cast<std::int32_t>(c16 >> (16 - l));
            if (c <= t.maxcode[l]) {
                const std::int32_t idx = c + t.valoff[l];
                if (idx < 0 || idx >= t.nvalues) break;
                br_.skip(l);
                return t.values[idx];
            }
        }
        if (tolerant_) {
            // No code of 1..16 bits matches. Like the IJG-derived decoders, take the 16 bits plus
            // one more as consumed and use symbol 0 (difference / DC difference 0, AC end of block).
            ++bad_codes_;
            br_.skip(17);
            return 0;
        }
        err_ = true;
        return 0;
    }
public:
    bool tolerant_ = false;        // JpegDecodeOptions::tolerate_bad_codes
    std::uint64_t bad_codes_ = 0;  // codes replaced by symbol 0
private:
    std::int32_t receive_extend(int s) { return extend(br_.get(s), s); }
    std::int32_t decode_diff(const HuffTable& t, int max_size) {
        const std::int32_t pk = t.fast_val[br_.peek(kFastBits)];
        if (pk) {
            br_.skip(pk & 0xFF);
            return pk >> 16;
        }
        const int s = decode_sym(t);
        if (s > max_size) { err_ = true; return 0; }
        if (s == 0) return 0;
        if (s == 16) return 32768;
        return receive_extend(s);
    }
    void decode_block_seq(Comp& c, const HuffTable& dc, const HuffTable& ac, const std::uint16_t* q,
                          std::uint16_t* dst);
    void decode_dc_first(Comp& c, std::int16_t* blk, const HuffTable& dc, int al);
    void decode_ac_first(std::int16_t* blk, const HuffTable& ac, int ss, int se, int al);
    void decode_ac_refine(std::int16_t* blk, const HuffTable& ac, int ss, int se, int al);

    const std::uint8_t* d_;
    std::size_t n_;
    FrameInfo info_;
    std::span<std::uint8_t> out_;
    std::size_t pos_ = 0;
    BitReader br_;
    bool err_ = false;

    HuffTable dc_[4], ac_[4];
    std::uint16_t q_[4][64] = {};  // zig-zag order
    bool qdef_[4] = {};
    std::uint32_t ri_ = 0;
    std::uint32_t next_rst_ = 0;
    std::uint32_t eobrun_ = 0;

    bool have_frame_ = false;
    Mode mode_ = Mode::Sequential;
    int precision_ = 8;
    std::uint32_t sof_rows_ = 0;
    bool dnl_seen_ = false;
    int scans_done_ = 0;
    // Progressive: the Al of the last scan that coded coefficient k (zig-zag) of each component,
    // -1 while it is still uncoded (T.81 G.1.1.1).
    std::int8_t coef_al_[4][64];
    int hmax_ = 1, vmax_ = 1;
    std::uint32_t mcux_ = 0, mcuy_ = 0;
    int ncomp_ = 0;
    Comp comps_[4];
};

// ------------------------------------------------------------------ marker level
Status Decoder::next_marker(int& m) {
    // Between segments only markers are expected; stray bytes are skipped, fill bytes (0xFF)
    // before a marker are allowed (B.1.1.2).
    while (pos_ < n_) {
        if (d_[pos_] != 0xFF) { ++pos_; continue; }
        while (pos_ < n_ && d_[pos_] == 0xFF) ++pos_;
        if (pos_ >= n_) break;
        const int b = d_[pos_++];
        if (b == 0) continue;
        m = b;
        return Status::ok();
    }
    return Status(Errc::Truncated, "JPEG: end of data before EOI", n_);
}

Status Decoder::segment(std::size_t& begin, std::size_t& end) {
    if (n_ - pos_ < 2) return Status(Errc::Truncated, "JPEG: truncated segment length", pos_);
    const std::size_t len = load_be16(d_ + pos_);
    if (len < 2) return malformed("JPEG: bad segment length", pos_);
    if (len > n_ - pos_) return Status(Errc::Truncated, "JPEG: truncated segment", pos_);
    begin = pos_ + 2;
    end = pos_ + len;
    pos_ = end;
    return Status::ok();
}

Status Decoder::parse_dqt(std::size_t b, std::size_t e) {
    while (b < e) {
        const int pq = d_[b] >> 4, tq = d_[b] & 15;
        ++b;
        if (pq > 1 || tq > 3) return malformed("JPEG: bad DQT table spec", b - 1);
        const std::size_t need = pq ? 128 : 64;
        if (e - b < need) return malformed("JPEG: short DQT", b);
        for (int k = 0; k < 64; ++k)
            q_[tq][k] = pq ? load_be16(d_ + b + 2 * static_cast<std::size_t>(k)) : d_[b + static_cast<std::size_t>(k)];
        qdef_[tq] = true;
        b += need;
    }
    return Status::ok();
}

Status Decoder::parse_dht(std::size_t b, std::size_t e) {
    while (b < e) {
        if (e - b < 17) return malformed("JPEG: short DHT", b);
        const int tc = d_[b] >> 4, th = d_[b] & 15;
        if (tc > 1 || th > 3) return malformed("JPEG: bad DHT table spec", b);
        std::uint8_t counts[17] = {};
        int total = 0;
        for (int l = 1; l <= 16; ++l) {
            counts[l] = d_[b + static_cast<std::size_t>(l)];
            total += counts[l];
        }
        b += 17;
        if (total > 256 || e - b < static_cast<std::size_t>(total))
            return malformed("JPEG: bad DHT counts", b);
        HuffTable& t = tc ? ac_[th] : dc_[th];
        if (!build_table(t, counts, d_ + b, total, tc == 1)) return malformed("JPEG: invalid Huffman table", b);
        b += static_cast<std::size_t>(total);
    }
    return Status::ok();
}

Status Decoder::parse_sof(int marker, std::size_t b, std::size_t e) {
    if (have_frame_) return malformed("JPEG: more than one frame header", b);
    if (e - b < 6) return malformed("JPEG: short SOF", b);
    switch (marker) {
        case 0xC0: case 0xC1: mode_ = Mode::Sequential; break;
        case 0xC2: mode_ = Mode::Progressive; break;
        case 0xC3: mode_ = Mode::Lossless; break;
        default: return Status(Errc::Unsupported, "JPEG: hierarchical or arithmetic-coded process", b);
    }
    precision_ = d_[b];
    sof_rows_ = load_be16(d_ + b + 1);
    const std::uint32_t cols = load_be16(d_ + b + 3);
    ncomp_ = d_[b + 5];
    if (mode_ == Mode::Lossless) {
        if (precision_ < 2 || precision_ > 16) return malformed("JPEG: bad lossless precision", b);
    } else if (marker == 0xC0) {
        if (precision_ != 8) return malformed("JPEG: baseline precision must be 8", b);
    } else if (precision_ != 8 && precision_ != 12) {
        return malformed("JPEG: DCT precision must be 8 or 12", b);
    }
    if (ncomp_ < 1 || ncomp_ > 4) return malformed("JPEG: bad component count", b);
    if (e - b != 6 + 3 * static_cast<std::size_t>(ncomp_)) return malformed("JPEG: bad SOF length", b);

    // Agreement with the DICOM attributes (PS3.5 8.2.1).
    if (cols != info_.columns) return Status(Errc::TypeMismatch, "JPEG: columns differ from DICOM header", b);
    if (sof_rows_ != 0 && sof_rows_ != info_.rows)
        return Status(Errc::TypeMismatch, "JPEG: rows differ from DICOM header", b);
    if (ncomp_ != info_.samples_per_pixel)
        return Status(Errc::TypeMismatch, "JPEG: component count differs from samples per pixel", b);
    if (precision_ > info_.bits_allocated || (precision_ > 8 && info_.bits_allocated != 16))
        return Status(Errc::TypeMismatch, "JPEG: precision does not fit bits allocated", b);

    for (int i = 0; i < ncomp_; ++i) {
        const std::size_t p = b + 6 + 3 * static_cast<std::size_t>(i);
        Comp& c = comps_[i];
        c.id = d_[p];
        c.h = d_[p + 1] >> 4;
        c.v = d_[p + 1] & 15;
        c.tq = d_[p + 2];
        if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) return malformed("JPEG: bad sampling factor", p);
        if (c.tq > 3) return malformed("JPEG: bad quantisation table selector", p);
        for (int j = 0; j < i; ++j)
            if (comps_[j].id == c.id) return malformed("JPEG: duplicate component id", p);
        hmax_ = std::max(hmax_, c.h);
        vmax_ = std::max(vmax_, c.v);
    }
    const std::uint32_t unit = mode_ == Mode::Lossless ? 1u : 8u;
    const std::uint32_t X = info_.columns, Y = info_.rows;
    std::uint64_t work = 0;  // component planes and coefficients, charged to the memory budget
    mcux_ = (X + unit * static_cast<std::uint32_t>(hmax_) - 1) / (unit * static_cast<std::uint32_t>(hmax_));
    mcuy_ = (Y + unit * static_cast<std::uint32_t>(vmax_) - 1) / (unit * static_cast<std::uint32_t>(vmax_));
    for (int i = 0; i < ncomp_; ++i) {
        Comp& c = comps_[i];
        const std::uint32_t h = static_cast<std::uint32_t>(c.h), v = static_cast<std::uint32_t>(c.v);
        c.cw = (X * h + static_cast<std::uint32_t>(hmax_) - 1) / static_cast<std::uint32_t>(hmax_);
        c.ch = (Y * v + static_cast<std::uint32_t>(vmax_) - 1) / static_cast<std::uint32_t>(vmax_);
        c.nbw = (c.cw + unit - 1) / unit;
        c.nbh = (c.ch + unit - 1) / unit;
        c.bw = mcux_ * h;
        c.bh = mcuy_ * v;
        std::uint64_t samples = 0;
        if (!checked_mul(static_cast<std::uint64_t>(c.bw) * unit, static_cast<std::uint64_t>(c.bh) * unit, samples) ||
            samples > (std::uint64_t{1} << 36))
            return Status(Errc::LimitExceeded, "JPEG: component too large", b);
        c.stride = static_cast<std::size_t>(c.bw) * unit;
        // The plane (16-bit samples) and, for progressive frames, 64 16-bit coefficients a block.
        work += samples * 2u * (mode_ == Mode::Progressive ? 2u : 1u);
        if (work > info_.effective_memory_budget())
            return Status(Errc::LimitExceeded, "JPEG: component planes exceed the memory budget", b);
        c.plane.assign(static_cast<std::size_t>(samples), 0);
        if (mode_ == Mode::Progressive)
            c.coef.assign(static_cast<std::size_t>(c.bw) * c.bh * 64, 0);
    }
    have_frame_ = true;
    return Status::ok();
}

Status Decoder::parse_sos(std::size_t b, std::size_t e, Scan& s) {
    if (!have_frame_) return malformed("JPEG: SOS before SOF", b);
    if (e - b < 1) return malformed("JPEG: short SOS", b);
    s.n = d_[b];
    if (s.n < 1 || s.n > 4 || e - b != 4 + 2 * static_cast<std::size_t>(s.n))
        return malformed("JPEG: bad SOS length", b);
    int last = -1, blocks = 0;
    for (int i = 0; i < s.n; ++i) {
        const std::size_t p = b + 1 + 2 * static_cast<std::size_t>(i);
        int found = -1;
        for (int j = 0; j < ncomp_; ++j)
            if (comps_[j].id == d_[p]) found = j;
        if (found < 0 || found <= last) return malformed("JPEG: bad scan component selector", p);
        last = found;
        s.ci[i] = found;
        Comp& c = comps_[found];
        c.td = d_[p + 1] >> 4;
        c.ta = d_[p + 1] & 15;
        if (c.td > 3 || c.ta > 3) return malformed("JPEG: bad table selector", p);
        blocks += c.h * c.v;
    }
    if (s.n > 1 && blocks > 10) return malformed("JPEG: too many blocks in MCU", b);
    const std::size_t p = b + 1 + 2 * static_cast<std::size_t>(s.n);
    s.ss = d_[p];
    s.se = d_[p + 1];
    s.ah = d_[p + 2] >> 4;
    s.al = d_[p + 2] & 15;
    if (mode_ == Mode::Progressive) {
        if (s.ss > s.se || s.se > 63 || (s.ss == 0 && s.se != 0) || (s.ss > 0 && s.n != 1) ||
            s.ah > 13 || s.al > 13 || (s.ah != 0 && s.al != s.ah - 1))
            return malformed("JPEG: bad progressive scan parameters", p);
        // T.81 G.1.1.1: a first scan (Ah = 0) codes coefficients no earlier scan coded; a
        // refinement continues exactly where the previous scan of those coefficients stopped
        // (its Ah is that scan's Al). Without this, every repeated all-EOB scan costs a pass
        // over every block for a few bytes of input.
        for (int i = 0; i < s.n; ++i) {
            std::int8_t* al = coef_al_[s.ci[i]];
            for (int k = s.ss; k <= s.se; ++k) {
                if (s.ah == 0 && al[k] != -1)
                    return malformed("JPEG: progressive scan codes coefficients an earlier scan coded", p);
                if (s.ah != 0 && al[k] != s.ah)
                    return malformed("JPEG: successive approximation scan does not refine the previous one", p);
            }
            for (int k = s.ss; k <= s.se; ++k) al[k] = static_cast<std::int8_t>(s.al);
        }
    } else if (mode_ == Mode::Lossless) {
        if (s.ss < 1 || s.ss > 7) return malformed("JPEG: bad lossless predictor", p);
        if (s.al >= precision_) return malformed("JPEG: bad point transform", p);
        // T.81 H.1.1: the restart interval of a lossless scan is a whole number of MCU rows.
        // Other intervals have no defined prediction rule, and decoders disagree on them.
        const std::uint32_t per_row = s.n == 1 ? comps_[s.ci[0]].cw : mcux_;
        if (ri_ % per_row != 0) return malformed("JPEG: lossless restart interval is not a multiple of the MCU row", p);
    }
    // Tables used by this scan must exist.
    for (int i = 0; i < s.n; ++i) {
        Comp& c = comps_[s.ci[i]];
        const bool need_dc = mode_ != Mode::Progressive || (s.ss == 0 && s.ah == 0);
        const bool need_ac = mode_ == Mode::Sequential || (mode_ == Mode::Progressive && s.ss > 0);
        if (need_dc && !dc_[c.td].defined) return malformed("JPEG: DC/lossless table not defined", p);
        if (need_ac && !ac_[c.ta].defined) return malformed("JPEG: AC table not defined", p);
        if (mode_ != Mode::Lossless && !qdef_[c.tq]) return malformed("JPEG: quantisation table not defined", p);
        if (mode_ == Mode::Progressive && !c.qsnap_set) {
            std::memcpy(c.qsnap, q_[c.tq], sizeof c.qsnap);
            c.qsnap_set = true;
        }
    }
    return Status::ok();
}

// ------------------------------------------------------------------ scan plumbing
Status Decoder::mcu_status() const {
    if (err_) return malformed("JPEG: corrupt entropy-coded data", br_.pos);
    if (br_.overrun()) {
        if (br_.pos >= n_) return Status(Errc::Truncated, "JPEG: entropy-coded data truncated", br_.pos);
        return malformed("JPEG: entropy-coded segment ends early", br_.pos);
    }
    return Status::ok();
}

Status Decoder::restart() {
    std::size_t p = br_.pos;
    while (p < n_ && d_[p] != 0xFF) ++p;
    while (p < n_ && d_[p] == 0xFF) ++p;
    if (p >= n_) return Status(Errc::Truncated, "JPEG: missing restart marker", p);
    if (d_[p] != 0xD0 + (next_rst_ & 7)) return malformed("JPEG: expected restart marker missing", p);
    ++next_rst_;
    br_.start(d_, n_, p + 1);
    for (int i = 0; i < ncomp_; ++i) comps_[i].pred = 0;
    eobrun_ = 0;
    return Status::ok();
}

template <class F>
Status Decoder::for_each_block(const Scan& s, F&& f) {
    std::uint32_t todo = ri_;
    if (s.n == 1) {
        Comp& c = comps_[s.ci[0]];
        for (std::uint32_t by = 0; by < c.nbh; ++by)
            for (std::uint32_t bx = 0; bx < c.nbw; ++bx) {
                if (ri_) {
                    if (todo == 0) {
                        if (Status st = restart(); !st) return st;
                        todo = ri_;
                    }
                    --todo;
                }
                f(c, bx, by);
                if (err_ || br_.overrun()) return mcu_status();
            }
        return Status::ok();
    }
    for (std::uint32_t my = 0; my < mcuy_; ++my)
        for (std::uint32_t mx = 0; mx < mcux_; ++mx) {
            if (ri_) {
                if (todo == 0) {
                    if (Status st = restart(); !st) return st;
                    todo = ri_;
                }
                --todo;
            }
            for (int i = 0; i < s.n; ++i) {
                Comp& c = comps_[s.ci[i]];
                const std::uint32_t h = static_cast<std::uint32_t>(c.h), v = static_cast<std::uint32_t>(c.v);
                for (std::uint32_t yy = 0; yy < v; ++yy)
                    for (std::uint32_t xx = 0; xx < h; ++xx) f(c, mx * h + xx, my * v + yy);
            }
            if (err_ || br_.overrun()) return mcu_status();
        }
    return Status::ok();
}

Status Decoder::decode_scan(const Scan& s) {
    br_.start(d_, n_, pos_);
    next_rst_ = 0;
    eobrun_ = 0;
    for (int i = 0; i < ncomp_; ++i) comps_[i].pred = 0;
    Status st;
    switch (mode_) {
        case Mode::Sequential: st = scan_sequential(s); break;
        case Mode::Progressive: st = scan_progressive(s); break;
        case Mode::Lossless: st = scan_lossless(s); break;
    }
    if (!st) return st;
    for (int i = 0; i < s.n; ++i) comps_[s.ci[i]].coded = true;
    pos_ = br_.pos;  // the marker search continues from where the entropy data stopped
    ++scans_done_;
    return Status::ok();
}

// ------------------------------------------------------------------ sequential DCT
void Decoder::decode_block_seq(Comp& c, const HuffTable& dc, const HuffTable& ac, const std::uint16_t* q,
                               std::uint16_t* dst) {
    std::int32_t blk[64];
    std::memset(blk, 0, sizeof blk);
    br_.ensure32();
    const std::int32_t diff = decode_diff(dc, precision_ == 8 ? 11 : 15);
    c.pred = static_cast<std::int16_t>(c.pred + diff);
    blk[0] = c.pred * static_cast<std::int32_t>(q[0]);
    int last = 0;
    for (int k = 1; k < 64;) {
        br_.ensure32();
        const std::int32_t pk = ac.fast_val[br_.peek(kFastBits)];
        if (pk) {
            k += (pk >> 8) & 0xFF;
            br_.skip(pk & 0xFF);
            if (k > 63) { err_ = true; return; }
            blk[kZigzag[static_cast<std::size_t>(k)]] = (pk >> 16) * static_cast<std::int32_t>(q[k]);
            last = k++;
            continue;
        }
        const int rs = decode_sym(ac);
        const int r = rs >> 4, sz = rs & 15;
        if (sz == 0) {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) { err_ = true; return; }
        blk[kZigzag[static_cast<std::size_t>(k)]] = receive_extend(sz) * static_cast<std::int32_t>(q[k]);
        last = k++;
    }
    const int shift = 1 << (precision_ - 1), maxval = (1 << precision_) - 1;
    if (last == 0) jpeg_detail::idct8x8_dc(blk[0], dst, c.stride, shift, maxval);
    else jpeg_detail::idct8x8(blk, dst, c.stride, shift, maxval);
}

Status Decoder::scan_sequential(const Scan& s) {
    return for_each_block(s, [&](Comp& c, std::uint32_t bx, std::uint32_t by) {
        std::uint16_t* dst = c.plane.data() + static_cast<std::size_t>(by) * 8 * c.stride + static_cast<std::size_t>(bx) * 8;
        decode_block_seq(c, dc_[c.td], ac_[c.ta], q_[c.tq], dst);
    });
}

// ------------------------------------------------------------------ progressive DCT (Annex G)
void Decoder::decode_dc_first(Comp& c, std::int16_t* blk, const HuffTable& dc, int al) {
    br_.ensure32();
    const std::int32_t diff = decode_diff(dc, precision_ == 8 ? 11 : 15);
    c.pred = static_cast<std::int16_t>(c.pred + diff);
    blk[0] = static_cast<std::int16_t>(static_cast<std::uint32_t>(c.pred) << al);
}

void Decoder::decode_ac_first(std::int16_t* blk, const HuffTable& ac, int ss, int se, int al) {
    if (eobrun_) { --eobrun_; return; }
    for (int k = ss; k <= se;) {
        br_.ensure32();
        const int rs = decode_sym(ac);
        const int r = rs >> 4, sz = rs & 15;
        if (sz == 0) {
            if (r == 15) { k += 16; continue; }
            eobrun_ = (1u << r) - 1;
            if (r) eobrun_ += static_cast<std::uint32_t>(br_.get(r));
            return;
        }
        k += r;
        if (k > se) { err_ = true; return; }
        blk[k] = static_cast<std::int16_t>(static_cast<std::uint32_t>(receive_extend(sz)) << al);
        ++k;
    }
}

void Decoder::decode_ac_refine(std::int16_t* blk, const HuffTable& ac, int ss, int se, int al) {
    const int p1 = 1 << al, m1 = -p1;
    auto refine = [&](std::int16_t& c) {
        if (br_.get(1) && (c & p1) == 0) c = static_cast<std::int16_t>(c + (c >= 0 ? p1 : m1));
    };
    int k = ss;
    if (eobrun_ == 0) {
        for (; k <= se; ++k) {
            br_.ensure32();
            const int rs = decode_sym(ac);
            if (err_) return;
            int r = rs >> 4;
            const int sz = rs & 15;
            int v = 0;
            if (sz != 0) {
                if (sz != 1) { err_ = true; return; }
                v = br_.get(1) ? p1 : m1;
            } else if (r != 15) {
                eobrun_ = 1u << r;
                if (r) eobrun_ += static_cast<std::uint32_t>(br_.get(r));
                break;
            }
            // Walk over r still-zero coefficients (refining the non-zero ones met on the way),
            // then place v in the next zero position.
            for (; k <= se; ++k) {
                std::int16_t& c = blk[k];
                if (c != 0) {
                    refine(c);
                } else {
                    if (r == 0) {
                        if (v) c = static_cast<std::int16_t>(v);
                        break;
                    }
                    --r;
                }
            }
        }
    }
    if (eobrun_ > 0) {
        for (; k <= se; ++k)
            if (blk[k] != 0) refine(blk[k]);
        --eobrun_;
    }
}

Status Decoder::scan_progressive(const Scan& s) {
    const bool dc = s.ss == 0;
    return for_each_block(s, [&](Comp& c, std::uint32_t bx, std::uint32_t by) {
        std::int16_t* blk = c.coef.data() + (static_cast<std::size_t>(by) * c.bw + bx) * 64;
        if (dc) {
            if (s.ah == 0) decode_dc_first(c, blk, dc_[c.td], s.al);
            else if (br_.get(1)) blk[0] = static_cast<std::int16_t>(blk[0] | (1 << s.al));
        } else if (s.ah == 0) {
            decode_ac_first(blk, ac_[c.ta], s.ss, s.se, s.al);
        } else {
            decode_ac_refine(blk, ac_[c.ta], s.ss, s.se, s.al);
        }
    });
}

Status Decoder::finish_progressive() {
    const int shift = 1 << (precision_ - 1), maxval = (1 << precision_) - 1;
    std::int32_t blk[64];
    for (int i = 0; i < ncomp_; ++i) {
        Comp& c = comps_[i];
        if (!c.qsnap_set) continue;  // never scanned: plane stays 0, reported in run()
        for (std::uint32_t by = 0; by < c.nbh; ++by)
            for (std::uint32_t bx = 0; bx < c.nbw; ++bx) {
                const std::int16_t* src = c.coef.data() + (static_cast<std::size_t>(by) * c.bw + bx) * 64;
                std::memset(blk, 0, sizeof blk);
                int last = 0;
                for (int k = 0; k < 64; ++k) {
                    if (src[k] == 0) continue;
                    blk[kZigzag[static_cast<std::size_t>(k)]] = src[k] * static_cast<std::int32_t>(c.qsnap[k]);
                    last = k;
                }
                std::uint16_t* dst = c.plane.data() + static_cast<std::size_t>(by) * 8 * c.stride + static_cast<std::size_t>(bx) * 8;
                if (last == 0) jpeg_detail::idct8x8_dc(blk[0], dst, c.stride, shift, maxval);
                else jpeg_detail::idct8x8(blk, dst, c.stride, shift, maxval);
            }
    }
    return Status::ok();
}

// ------------------------------------------------------------------ lossless (Annex H)
template <int Psv>
inline int predict(int ra, int rb, int rc) {
    if constexpr (Psv == 1) return ra;
    else if constexpr (Psv == 2) return rb;
    else if constexpr (Psv == 3) return rc;
    else if constexpr (Psv == 4) return ra + rb - rc;
    else if constexpr (Psv == 5) return ra + ((rb - rc) >> 1);
    else if constexpr (Psv == 6) return rb + ((ra - rc) >> 1);
    else return (ra + rb) >> 1;
}

inline int predict_any(int psv, int ra, int rb, int rc) {
    switch (psv) {
        case 1: return predict<1>(ra, rb, rc);
        case 2: return predict<2>(ra, rb, rc);
        case 3: return predict<3>(ra, rb, rc);
        case 4: return predict<4>(ra, rb, rc);
        case 5: return predict<5>(ra, rb, rc);
        case 6: return predict<6>(ra, rb, rc);
        default: return predict<7>(ra, rb, rc);
    }
}

Status Decoder::scan_lossless(const Scan& s) {
    if (s.n == 1) return scan_lossless_single(s);
    // Interleaved: MCU = Hi x Vi samples of each component, in frame order.
    const int psv = s.ss;
    const int init = 1 << (precision_ - s.al - 1);
    for (int i = 0; i < s.n; ++i) comps_[s.ci[i]].rs_x = comps_[s.ci[i]].rs_y = 0;
    std::uint32_t todo = ri_;
    for (std::uint32_t my = 0; my < mcuy_; ++my)
        for (std::uint32_t mx = 0; mx < mcux_; ++mx) {
            if (ri_) {
                if (todo == 0) {
                    if (Status st = restart(); !st) return st;
                    todo = ri_;
                    for (int i = 0; i < s.n; ++i) {
                        Comp& c = comps_[s.ci[i]];
                        c.rs_x = mx * static_cast<std::uint32_t>(c.h);
                        c.rs_y = my * static_cast<std::uint32_t>(c.v);
                    }
                }
                --todo;
            }
            for (int i = 0; i < s.n; ++i) {
                Comp& c = comps_[s.ci[i]];
                const std::uint32_t h = static_cast<std::uint32_t>(c.h), v = static_cast<std::uint32_t>(c.v);
                for (std::uint32_t yy = 0; yy < v; ++yy)
                    for (std::uint32_t xx = 0; xx < h; ++xx) {
                        const std::uint32_t x = mx * h + xx, y = my * v + yy;
                        std::uint16_t* p = c.plane.data() + static_cast<std::size_t>(y) * c.stride + x;
                        int pred;
                        if (y == c.rs_y) pred = x == c.rs_x ? init : p[-1];
                        else if (x == 0) pred = p[-static_cast<std::ptrdiff_t>(c.stride)];
                        else {
                            const std::uint16_t* up = p - c.stride;
                            pred = predict_any(psv, p[-1], up[0], up[-1]);
                        }
                        br_.ensure32();
                        *p = static_cast<std::uint16_t>(pred + decode_diff(dc_[c.td], 16));
                    }
            }
            if (err_ || br_.overrun()) return mcu_status();
        }
    return Status::ok();
}

Status Decoder::scan_lossless_single(const Scan& s) {
    Comp& c = comps_[s.ci[0]];
    const HuffTable& t = dc_[c.td];
    const int psv = s.ss;
    const int init = 1 << (precision_ - s.al - 1);
    std::uint32_t rs_x = 0, rs_y = 0;
    std::uint32_t todo = ri_;
    auto diff = [&]() {
        br_.ensure32();
        return decode_diff(t, 16);
    };
    for (std::uint32_t y = 0; y < c.ch; ++y) {
        std::uint16_t* row = c.plane.data() + static_cast<std::size_t>(y) * c.stride;
        const std::uint16_t* up = y ? row - c.stride : row;
        std::uint32_t x = 0;
        while (x < c.cw) {
            std::uint32_t x1 = c.cw;
            if (ri_) {
                if (todo == 0) {
                    if (Status st = restart(); !st) return st;
                    todo = ri_;
                    rs_x = x;
                    rs_y = y;
                }
                x1 = std::min<std::uint32_t>(c.cw, x + todo);
                todo -= x1 - x;
            }
            if (y == rs_y) {
                if (x == rs_x) { row[x] = static_cast<std::uint16_t>(init + diff()); ++x; }
                for (; x < x1; ++x) row[x] = static_cast<std::uint16_t>(row[x - 1] + diff());
            } else {
                if (x == 0) { row[0] = static_cast<std::uint16_t>(up[0] + diff()); ++x; }
                switch (psv) {
#define SIAARA_JPEG_RUN(P)                                                                        \
    case P:                                                                                       \
        for (; x < x1; ++x)                                                                       \
            row[x] = static_cast<std::uint16_t>(predict<P>(row[x - 1], up[x], up[x - 1]) + diff()); \
        break;
                    SIAARA_JPEG_RUN(1) SIAARA_JPEG_RUN(2) SIAARA_JPEG_RUN(3) SIAARA_JPEG_RUN(4)
                    SIAARA_JPEG_RUN(5) SIAARA_JPEG_RUN(6) SIAARA_JPEG_RUN(7)
#undef SIAARA_JPEG_RUN
                    default: break;
                }
            }
            x = x1;
        }
        if (err_ || br_.overrun()) return mcu_status();
    }
    return Status::ok();
}

// ------------------------------------------------------------------ output
Status Decoder::write_output() {
    const std::uint32_t rows = info_.rows, cols = info_.columns;
    const bool wide = info_.bits_allocated == 16;
    const std::size_t spp = static_cast<std::size_t>(ncomp_);
    const std::size_t bps = wide ? 2 : 1;
    const std::size_t plane_px = static_cast<std::size_t>(rows) * cols;
    // Sample transform: lossless point transform, precision mask, sign extension.
    const bool lossless = mode_ == Mode::Lossless;
    int sbits = 0;
    if (lossless && info_.is_signed) sbits = std::min<int>(precision_, info_.bits_stored ? info_.bits_stored : precision_);
    const std::uint32_t sbit = sbits ? (1u << (sbits - 1)) : 0;
    std::vector<std::uint32_t> xmap(cols);
    for (int i = 0; i < ncomp_; ++i) {
        const Comp& c = comps_[i];
        const int pt = lossless ? c.pt : 0;
        const std::uint32_t vmask = lossless ? ((1u << (precision_ - pt)) - 1u) : 0xFFFFu;
        const bool plain = !lossless || (pt == 0 && precision_ == 16 && sbits == 0);
        const bool full_x = c.h == hmax_;
        for (std::uint32_t x = 0; x < cols; ++x)
            xmap[x] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * static_cast<std::uint32_t>(c.h) /
                                                 static_cast<std::uint32_t>(hmax_));
        for (std::uint32_t y = 0; y < rows; ++y) {
            const std::uint32_t sy = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * static_cast<std::uint32_t>(c.v) /
                                                                static_cast<std::uint32_t>(vmax_));
            const std::uint16_t* src = c.plane.data() + static_cast<std::size_t>(sy) * c.stride;
            std::uint8_t* dst;
            std::size_t step;
            if (info_.planar || spp == 1) {
                dst = out_.data() + (static_cast<std::size_t>(i) * plane_px + static_cast<std::size_t>(y) * cols) * bps;
                step = bps;
            } else {
                dst = out_.data() + (static_cast<std::size_t>(y) * cols * spp + static_cast<std::size_t>(i)) * bps;
                step = bps * spp;
            }
            for (std::uint32_t x = 0; x < cols; ++x, dst += step) {
                std::uint32_t v = src[full_x ? x : xmap[x]];
                if (!plain) {
                    v = (v & vmask) << pt;
                    if (sbit) v = ((v ^ sbit) - sbit) & 0xFFFFu;
                }
                if (wide) store_le16(dst, static_cast<std::uint16_t>(v));
                else *dst = static_cast<std::uint8_t>(v);
            }
        }
    }
    return Status::ok();
}

// ------------------------------------------------------------------ driver
Status Decoder::run() {
    const std::uint64_t need = info_.frame_bytes();
    if (need == 0 || (info_.bits_allocated != 8 && info_.bits_allocated != 16))
        return Status(Errc::InvalidArgument, "JPEG: unsupported frame description");
    if (out_.size() != need)
        return Status(out_.size() < need ? Errc::BufferTooSmall : Errc::InvalidArgument,
                      "JPEG: output size differs from frame size");
    if (n_ < 4 || d_[0] != 0xFF || d_[1] != 0xD8) return malformed("JPEG: missing SOI", 0);
    pos_ = 2;
    for (;;) {
        int m = 0;
        if (Status st = next_marker(m); !st) return st;
        if (m == 0xD9) break;                         // EOI
        if (m >= 0xD0 && m <= 0xD7) continue;         // stray RSTn between segments
        if (m == 0x01) continue;                      // TEM
        if (m == 0xD8) return malformed("JPEG: unexpected SOI", pos_ - 2);
        std::size_t b = 0, e = 0;
        if (Status st = segment(b, e); !st) return st;
        Status st;
        switch (m) {
            case 0xDB: st = parse_dqt(b, e); break;
            case 0xC4: st = parse_dht(b, e); break;
            case 0xDD:
                if (e - b != 2) return malformed("JPEG: bad DRI length", b);
                ri_ = load_be16(d_ + b);
                break;
            case 0xDC: {
                if (e - b != 2) return malformed("JPEG: bad DNL length", b);
                const std::uint32_t nl = load_be16(d_ + b);
                if (!have_frame_ || scans_done_ == 0) return malformed("JPEG: DNL before first scan", b);
                if (nl != info_.rows) return Status(Errc::TypeMismatch, "JPEG: DNL rows differ from DICOM header", b);
                dnl_seen_ = true;
                break;
            }
            case 0xCC: return Status(Errc::Unsupported, "JPEG: arithmetic coding not supported", b);
            case 0xDE: case 0xDF: return Status(Errc::Unsupported, "JPEG: hierarchical mode not supported", b);
            case 0xF7: return Status(Errc::Unsupported, "JPEG: JPEG-LS codestream given to the JPEG decoder", b);
            case 0xDA: {
                if (static_cast<std::uint32_t>(scans_done_) >= info_.effective_max_scans())
                    return Status(Errc::LimitExceeded, "JPEG: more scans than FrameInfo::max_scans", b);
                Scan s;
                if (st = parse_sos(b, e, s); !st) return st;
                if (mode_ == Mode::Lossless)
                    for (int i = 0; i < s.n; ++i) comps_[s.ci[i]].pt = s.al;
                st = decode_scan(s);
                break;
            }
            default:
                if (m >= 0xC0 && m <= 0xCF) st = parse_sof(m, b, e);
                // APPn, COM, JPGn and anything else with a length: skipped.
                break;
        }
        if (!st) return st;
    }
    if (!have_frame_) return malformed("JPEG: no frame header", n_);
    if (scans_done_ == 0) return malformed("JPEG: no scan", n_);
    if (sof_rows_ == 0 && !dnl_seen_) return malformed("JPEG: rows defined by DNL but no DNL marker", n_);
    for (int i = 0; i < ncomp_; ++i)
        if (!comps_[i].coded) return malformed("JPEG: component missing from all scans", n_);
    if (mode_ == Mode::Progressive)
        if (Status st = finish_progressive(); !st) return st;
    return write_output();
}

}  // namespace

Status decode_jpeg(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out) {
    // Allocation failure is reported, never thrown (decision 13.1).
    try {
        // The decoder carries ~55 KB of Huffman lookup tables: keep it off the stack.
        std::unique_ptr<Decoder> dec(new (std::nothrow) Decoder(in, info, out));
        if (!dec) return Status(Errc::OutOfMemory, "JPEG: out of memory");
        return dec->run();
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "JPEG: out of memory");
    }
}

Status decode_jpeg_ex(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out,
                      const JpegDecodeOptions& opt, std::uint64_t& bad_codes) {
    bad_codes = 0;
    std::unique_ptr<Decoder> dec(new (std::nothrow) Decoder(in, info, out));
    if (!dec) return Status(Errc::OutOfMemory, "JPEG: out of memory");
    dec->tolerant_ = opt.tolerate_bad_codes;
    Status st = dec->run();
    bad_codes = dec->bad_codes_;
    return st;
}

Status jpeg_read_header(std::span<const std::uint8_t> in, JpegHeaderInfo& hdr) {
    const std::uint8_t* d = in.data();
    const std::size_t n = in.size();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return Status(Errc::Malformed, "JPEG: missing SOI", 0);
    std::size_t pos = 2;
    while (pos < n) {
        if (d[pos] != 0xFF) { ++pos; continue; }
        while (pos < n && d[pos] == 0xFF) ++pos;
        if (pos >= n) break;
        const int m = d[pos++];
        if (m == 0 || m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
        if (m == 0xD9 || m == 0xDA) break;
        if (n - pos < 2) break;
        const std::size_t len = load_be16(d + pos);
        if (len < 2 || len > n - pos) break;
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof || m == 0xF7) {
            if (len < 8) break;
            hdr.sof_marker = m;
            hdr.precision = d[pos + 2];
            hdr.rows = load_be16(d + pos + 3);
            hdr.columns = load_be16(d + pos + 5);
            hdr.components = d[pos + 7];
            hdr.lossless = m == 0xC3 || m == 0xC7 || m == 0xCB || m == 0xCF;
            hdr.progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
            return Status::ok();
        }
        pos += len;
    }
    return Status(Errc::Malformed, "JPEG: no frame header found", pos);
}

}  // namespace siaara::codec
