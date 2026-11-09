// SIAARA DICOM Toolkit - JPEG encoder: lossless process 14 (SOF3) and baseline / extended
// sequential DCT (SOF0 8-bit, SOF1 12-bit) with Huffman coding. Written from ITU-T T.81.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "codec/codec.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpeg_internal.hpp"
#include "core/types.hpp"

namespace siaara::codec {
namespace {

using jpeg_detail::HuffSpec;
using jpeg_detail::kZigzag;

// ------------------------------------------------------------------ output helpers
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t v, int len) {  // len <= 24
        if (len == 0) return;
        acc_ = (acc_ << len) | (v & ((1u << len) - 1u));
        n_ += len;
        while (n_ >= 8) {
            n_ -= 8;
            const auto b = static_cast<std::uint8_t>(acc_ >> n_);
            out_.push_back(b);
            if (b == 0xFF) out_.push_back(0x00);  // byte stuffing (F.1.2.3)
        }
        acc_ &= (std::uint64_t{1} << n_) - 1u;
    }
    void flush() {  // pad the last byte with 1-bits (F.1.2.3)
        if (n_ > 0) put((1u << (8 - n_)) - 1u, 8 - n_);
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t acc_ = 0;
    int n_ = 0;
};

void put16(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}
void put_marker(std::vector<std::uint8_t>& o, int m) {
    o.push_back(0xFF);
    o.push_back(static_cast<std::uint8_t>(m));
}

void write_dht(std::vector<std::uint8_t>& o, int tc, int th, const HuffSpec& s) {
    put_marker(o, 0xC4);
    put16(o, static_cast<std::uint32_t>(2 + 17 + s.nvalues));
    o.push_back(static_cast<std::uint8_t>((tc << 4) | th));
    for (int l = 1; l <= 16; ++l) o.push_back(s.counts[l]);
    for (int i = 0; i < s.nvalues; ++i) o.push_back(s.values[i]);
}

inline int category(std::int32_t v) {
    std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
    int s = 0;
    while (a) { ++s; a >>= 1; }
    return s;
}
inline std::uint32_t magnitude_bits(std::int32_t v, int s) {
    return static_cast<std::uint32_t>(v < 0 ? v + (1 << s) - 1 : v);
}

// Entropy coder used twice: first to count symbol frequencies, then to write.
struct Coder {
    bool counting = true;
    std::uint32_t freq[8][256] = {};
    std::uint16_t code[8][256] = {};
    std::uint8_t len[8][256] = {};
    BitWriter* bw = nullptr;
    bool missing = false;  // a symbol had no code (only possible with fixed tables)

    void sym(int t, int s) {
        if (counting) { ++freq[t][s]; return; }
        if (len[t][s] == 0) { missing = true; return; }
        bw->put(code[t][s], len[t][s]);
    }
    void bits(std::uint32_t v, int n) {
        if (!counting) bw->put(v, n);
    }
};

// Reads sample i of pixel px from native input.
inline std::uint32_t load_sample(std::span<const std::uint8_t> in, const FrameInfo& info, std::size_t px,
                                 int i) {
    const std::size_t npx = static_cast<std::size_t>(info.rows) * info.columns;
    const std::size_t spp = info.samples_per_pixel;
    const std::size_t idx = info.planar ? static_cast<std::size_t>(i) * npx + px : px * spp + static_cast<std::size_t>(i);
    if (info.bits_allocated == 8) return in[idx];
    return load_le16(in.data() + 2 * idx);
}

Status check_input(std::span<const std::uint8_t> in, const FrameInfo& info) {
    const std::uint64_t need = info.frame_bytes();
    if (need == 0 || (info.bits_allocated != 8 && info.bits_allocated != 16))
        return Status(Errc::InvalidArgument, "JPEG encode: unsupported frame description");
    if (info.rows > 65535 || info.columns > 65535)
        return Status(Errc::LimitExceeded, "JPEG encode: image larger than 65535 x 65535");
    if (in.size() != need) return Status(Errc::InvalidArgument, "JPEG encode: input size differs from frame size");
    if (info.samples_per_pixel < 1 || info.samples_per_pixel > 4)
        return Status(Errc::Unsupported, "JPEG encode: 1 to 4 samples per pixel");
    return Status::ok();
}

// ------------------------------------------------------------------ lossless (Annex H)
Status encode_lossless(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                       const JpegEncodeOptions& opt, std::vector<std::uint8_t>& out) {
    const int P = info.bits_stored;
    if (P < 2 || P > 16 || P > info.bits_allocated || (P > 8 && info.bits_allocated != 16))
        return Status(Errc::Unsupported, "JPEG lossless: precision must be 2..16 and fit bits allocated");
    if (p.predictor < 1 || p.predictor > 7) return Status(Errc::InvalidArgument, "JPEG lossless: predictor must be 1..7");
    const int pt = opt.point_transform;
    if (pt < 0 || pt >= P) return Status(Errc::InvalidArgument, "JPEG lossless: point transform out of range");
    const int nc = info.samples_per_pixel;
    const std::uint32_t X = info.columns, Y = info.rows;
    const std::size_t npx = static_cast<std::size_t>(X) * Y;
    const std::uint32_t mask = P == 16 ? 0xFFFFu : ((1u << P) - 1u);

    std::vector<std::uint16_t> planes(npx * static_cast<std::size_t>(nc));
    for (int i = 0; i < nc; ++i)
        for (std::size_t px = 0; px < npx; ++px)
            planes[static_cast<std::size_t>(i) * npx + px] =
                static_cast<std::uint16_t>((load_sample(in, info, px, i) & mask) >> pt);

    const int psv = p.predictor;
    const int init = 1 << (P - pt - 1);
    const std::uint32_t ri = opt.restart_interval;
    if (ri % X != 0)  // T.81 H.1.1: whole MCU rows only (one MCU = one pixel here)
        return Status(Errc::InvalidArgument, "JPEG lossless: restart interval must be a multiple of the columns");
    const bool separate = opt.separate_scans || nc == 1;

    // One scan: components comps[0..n). MCU = one pixel (all sampling factors 1).
    auto code_scan = [&](const int* comps, int n, Coder& cd, std::vector<std::uint8_t>* o) {
        BitWriter* bw = cd.bw;
        std::uint32_t todo = ri, rs_x = 0, rs_y = 0, rst = 0;
        for (std::uint32_t y = 0; y < Y; ++y)
            for (std::uint32_t x = 0; x < X; ++x) {
                if (ri) {
                    if (todo == 0) {
                        if (!cd.counting) {
                            bw->flush();
                            put_marker(*o, 0xD0 + static_cast<int>(rst & 7));
                        }
                        ++rst;
                        todo = ri;
                        rs_x = x;
                        rs_y = y;
                    }
                    --todo;
                }
                for (int j = 0; j < n; ++j) {
                    const int i = comps[j];
                    const std::uint16_t* pl = planes.data() + static_cast<std::size_t>(i) * npx;
                    const std::size_t at = static_cast<std::size_t>(y) * X + x;
                    int pred;
                    if (y == rs_y) pred = x == rs_x ? init : pl[at - 1];
                    else if (x == 0) pred = pl[at - X];
                    else {
                        const int ra = pl[at - 1], rb = pl[at - X], rc = pl[at - X - 1];
                        switch (psv) {
                            case 1: pred = ra; break;
                            case 2: pred = rb; break;
                            case 3: pred = rc; break;
                            case 4: pred = ra + rb - rc; break;
                            case 5: pred = ra + ((rb - rc) >> 1); break;
                            case 6: pred = rb + ((ra - rc) >> 1); break;
                            default: pred = (ra + rb) >> 1; break;
                        }
                    }
                    std::int32_t d = static_cast<std::int32_t>((static_cast<std::uint32_t>(pl[at]) -
                                                                 static_cast<std::uint32_t>(pred)) & 0xFFFFu);
                    if (d == 32768) {
                        cd.sym(i, 16);
                        continue;
                    }
                    if (d > 32768) d -= 65536;
                    const int s = category(d);
                    cd.sym(i, s);
                    cd.bits(magnitude_bits(d, s), s);
                }
            }
    };

    int all[4] = {0, 1, 2, 3};
    Coder cd;
    // Pass 1: statistics.
    if (separate) {
        for (int i = 0; i < nc; ++i) code_scan(&all[i], 1, cd, nullptr);
    } else {
        code_scan(all, nc, cd, nullptr);
    }
    HuffSpec spec[4];
    for (int i = 0; i < nc; ++i) {
        jpeg_detail::build_huffman_spec(cd.freq[i], spec[i]);
        if (!jpeg_detail::make_codes(spec[i], cd.code[i], cd.len[i]))
            return Status(Errc::Internal, "JPEG lossless: Huffman construction failed");
    }

    // Pass 2: the codestream.
    out.clear();
    out.reserve(in.size() / 2 + 1024);
    put_marker(out, 0xD8);
    put_marker(out, 0xC3);
    put16(out, static_cast<std::uint32_t>(8 + 3 * nc));
    out.push_back(static_cast<std::uint8_t>(P));
    put16(out, Y);
    put16(out, X);
    out.push_back(static_cast<std::uint8_t>(nc));
    for (int i = 0; i < nc; ++i) {
        out.push_back(static_cast<std::uint8_t>(i + 1));
        out.push_back(0x11);
        out.push_back(0);
    }
    for (int i = 0; i < nc; ++i) write_dht(out, 0, i, spec[i]);
    if (ri) {
        put_marker(out, 0xDD);
        put16(out, 4);
        put16(out, ri);
    }
    auto write_sos = [&](const int* comps, int n) {
        put_marker(out, 0xDA);
        put16(out, static_cast<std::uint32_t>(6 + 2 * n));
        out.push_back(static_cast<std::uint8_t>(n));
        for (int j = 0; j < n; ++j) {
            out.push_back(static_cast<std::uint8_t>(comps[j] + 1));
            out.push_back(static_cast<std::uint8_t>(comps[j] << 4));
        }
        out.push_back(static_cast<std::uint8_t>(psv));
        out.push_back(0);
        out.push_back(static_cast<std::uint8_t>(pt));
    };
    BitWriter bw(out);
    cd.counting = false;
    cd.bw = &bw;
    if (separate) {
        for (int i = 0; i < nc; ++i) {
            write_sos(&all[i], 1);
            code_scan(&all[i], 1, cd, &out);
            bw.flush();
        }
    } else {
        write_sos(all, nc);
        code_scan(all, nc, cd, &out);
        bw.flush();
    }
    put_marker(out, 0xD9);
    if (cd.missing) return Status(Errc::Internal, "JPEG lossless: symbol without code");
    return Status::ok();
}

// ------------------------------------------------------------------ sequential DCT
struct LossyComp {
    int h = 1, v = 1;
    std::uint32_t cw = 0, ch = 0;     // samples
    std::uint32_t bw = 0, bh = 0;     // allocated block grid (MCU padded)
    std::uint32_t nbw = 0, nbh = 0;   // non-interleaved block grid
    std::vector<std::int32_t> samples;  // bw*8 x bh*8, edge replicated
    std::vector<std::int16_t> coef;     // bw*bh blocks, zig-zag order
    int tq = 0;
};

Status encode_lossy(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                    const JpegEncodeOptions& opt, std::vector<std::uint8_t>& out) {
    int P;
    if (info.bits_allocated == 8 && info.bits_stored <= 8) P = 8;
    else if (info.bits_allocated == 16 && info.bits_stored <= 12) P = 12;
    else return Status(Errc::Unsupported, "JPEG lossy: only 8-bit (baseline) or up to 12-bit (extended) data");
    if (info.bits_stored < 1) return Status(Errc::InvalidArgument, "JPEG lossy: bits stored is 0");
    if (info.is_signed) return Status(Errc::Unsupported, "JPEG lossy: signed pixel data is not supported");
    const int nc = info.samples_per_pixel;
    if (nc != 1 && nc != 3) return Status(Errc::Unsupported, "JPEG lossy: 1 or 3 samples per pixel");
    if (!opt.optimize_huffman && P != 8)
        return Status(Errc::InvalidArgument, "JPEG lossy: Annex K tables only cover 8-bit data");
    int lh = nc == 3 ? opt.luma_h : 1, lv = nc == 3 ? opt.luma_v : 1;
    if (lh < 1 || lh > 4 || lv < 1 || lv > 4 || lh * lv + 2 > 10)
        return Status(Errc::InvalidArgument, "JPEG lossy: unsupported sampling factors");

    const std::uint32_t X = info.columns, Y = info.rows;
    const std::size_t npx = static_cast<std::size_t>(X) * Y;
    const std::int32_t maxval = (1 << P) - 1;
    const std::uint32_t in_mask = (1u << info.bits_stored) - 1u;
    const bool ybr = nc == 3 && p.ybr;

    // Full-resolution component samples.
    std::vector<std::int32_t> full(npx * static_cast<std::size_t>(nc));
    for (std::size_t px = 0; px < npx; ++px) {
        if (ybr) {
            const double r = load_sample(in, info, px, 0) & in_mask;
            const double g = load_sample(in, info, px, 1) & in_mask;
            const double b = load_sample(in, info, px, 2) & in_mask;
            const double half = static_cast<double>(1 << (P - 1));
            const double yc[3] = {0.299 * r + 0.587 * g + 0.114 * b,
                                  -0.168736 * r - 0.331264 * g + 0.5 * b + half,
                                  0.5 * r - 0.418688 * g - 0.081312 * b + half};
            for (int i = 0; i < 3; ++i)
                full[static_cast<std::size_t>(i) * npx + px] =
                    std::clamp(static_cast<std::int32_t>(std::lround(yc[i])), 0, maxval);
        } else {
            for (int i = 0; i < nc; ++i)
                full[static_cast<std::size_t>(i) * npx + px] = static_cast<std::int32_t>(load_sample(in, info, px, i) & in_mask);
        }
    }

    const int hmax = lh, vmax = lv;
    const std::uint32_t mcux = (X + 8u * static_cast<std::uint32_t>(hmax) - 1) / (8u * static_cast<std::uint32_t>(hmax));
    const std::uint32_t mcuy = (Y + 8u * static_cast<std::uint32_t>(vmax) - 1) / (8u * static_cast<std::uint32_t>(vmax));
    LossyComp comps[3];
    for (int i = 0; i < nc; ++i) {
        LossyComp& c = comps[i];
        c.h = i == 0 ? lh : 1;
        c.v = i == 0 ? lv : 1;
        c.tq = i == 0 ? 0 : 1;
        const std::uint32_t h = static_cast<std::uint32_t>(c.h), v = static_cast<std::uint32_t>(c.v);
        c.cw = (X * h + static_cast<std::uint32_t>(hmax) - 1) / static_cast<std::uint32_t>(hmax);
        c.ch = (Y * v + static_cast<std::uint32_t>(vmax) - 1) / static_cast<std::uint32_t>(vmax);
        c.nbw = (c.cw + 7) / 8;
        c.nbh = (c.ch + 7) / 8;
        c.bw = mcux * h;
        c.bh = mcuy * v;
        const std::size_t sw = static_cast<std::size_t>(c.bw) * 8, sh = static_cast<std::size_t>(c.bh) * 8;
        c.samples.assign(sw * sh, 0);
        // Downsample by box averaging, then replicate edges into the padding.
        const int fx = hmax / c.h, fy = vmax / c.v;
        const std::int32_t* src = full.data() + static_cast<std::size_t>(i) * npx;
        for (std::uint32_t y = 0; y < c.ch; ++y)
            for (std::uint32_t x = 0; x < c.cw; ++x) {
                std::int64_t sum = 0;
                int cnt = 0;
                for (int dy = 0; dy < fy; ++dy)
                    for (int dx = 0; dx < fx; ++dx) {
                        const std::uint32_t sx = std::min<std::uint32_t>(x * static_cast<std::uint32_t>(fx) + static_cast<std::uint32_t>(dx), X - 1);
                        const std::uint32_t sy = std::min<std::uint32_t>(y * static_cast<std::uint32_t>(fy) + static_cast<std::uint32_t>(dy), Y - 1);
                        sum += src[static_cast<std::size_t>(sy) * X + sx];
                        ++cnt;
                    }
                c.samples[static_cast<std::size_t>(y) * sw + x] = static_cast<std::int32_t>((sum + cnt / 2) / cnt);
            }
        for (std::size_t y = 0; y < sh; ++y)
            for (std::size_t x = 0; x < sw; ++x)
                if (y >= c.ch || x >= c.cw) {
                    const std::size_t yy = std::min<std::size_t>(y, c.ch - 1), xx = std::min<std::size_t>(x, c.cw - 1);
                    c.samples[y * sw + x] = c.samples[yy * sw + xx];
                }
    }

    // Quantisation tables (zig-zag order), Annex K tables scaled by quality.
    const int q = std::clamp(p.quality, 1, 100);
    const double f = q < 50 ? 50.0 / q : (100.0 - q) / 50.0;
    const double depth = P == 12 ? 16.0 : 1.0;
    const int qmax = P == 8 ? 255 : 32767;
    std::uint16_t qt[2][64];
    bool wide_q[2] = {false, false};
    for (int t = 0; t < 2; ++t)
        for (int k = 0; k < 64; ++k) {
            const int base = (t == 0 ? jpeg_detail::kLumaQuant : jpeg_detail::kChromaQuant)[kZigzag[static_cast<std::size_t>(k)]];
            const int v = std::clamp(static_cast<int>(std::lround(base * f * depth)), 1, qmax);
            qt[t][k] = static_cast<std::uint16_t>(v);
            if (v > 255) wide_q[t] = true;
        }

    // Forward DCT + quantisation of every allocated block.
    const std::int32_t ac_lim = (1 << (P + 2)) - 1;  // size category <= P+2 (10 / 14)
    const std::int32_t dc_lim = (1 << (P + 2)) - 1;  // DC differences stay within P+3 bits
    const double shift = static_cast<double>(1 << (P - 1));
    for (int i = 0; i < nc; ++i) {
        LossyComp& c = comps[i];
        const std::size_t sw = static_cast<std::size_t>(c.bw) * 8;
        c.coef.assign(static_cast<std::size_t>(c.bw) * c.bh * 64, 0);
        double blk[64], dct[64];
        for (std::uint32_t by = 0; by < c.bh; ++by)
            for (std::uint32_t bx = 0; bx < c.bw; ++bx) {
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        blk[y * 8 + x] = c.samples[(static_cast<std::size_t>(by) * 8 + static_cast<std::size_t>(y)) * sw +
                                                   static_cast<std::size_t>(bx) * 8 + static_cast<std::size_t>(x)] - shift;
                jpeg_detail::fdct8x8(blk, dct);
                std::int16_t* dst = c.coef.data() + (static_cast<std::size_t>(by) * c.bw + bx) * 64;
                for (int k = 0; k < 64; ++k) {
                    const double v = dct[kZigzag[static_cast<std::size_t>(k)]] / qt[c.tq][k];
                    const std::int32_t lim = k == 0 ? dc_lim : ac_lim;
                    dst[k] = static_cast<std::int16_t>(std::clamp(static_cast<std::int32_t>(std::lround(v)), -lim, lim));
                }
            }
    }

    // Scans: one interleaved scan, or one per component. Tables: DC 0/1 at index 0/1, AC 0/1 at 2/3.
    const std::uint32_t ri = opt.restart_interval;
    struct ScanDef { int n; int ci[3]; };
    std::vector<ScanDef> scans;
    if (nc == 1 || opt.separate_scans) {
        for (int i = 0; i < nc; ++i) scans.push_back({1, {i, 0, 0}});
    } else {
        scans.push_back({nc, {0, 1, 2}});
    }

    auto code_block = [](const std::int16_t* b, int& pred, int dct, int act, Coder& cd) {
        const int diff = b[0] - pred;
        pred = b[0];
        const int s = category(diff);
        cd.sym(dct, s);
        cd.bits(magnitude_bits(diff, s), s);
        int run = 0;
        for (int k = 1; k < 64; ++k) {
            const int v = b[k];
            if (v == 0) { ++run; continue; }
            while (run > 15) { cd.sym(act, 0xF0); run -= 16; }
            const int sz = category(v);
            cd.sym(act, (run << 4) | sz);
            cd.bits(magnitude_bits(v, sz), sz);
            run = 0;
        }
        if (run > 0) cd.sym(act, 0x00);
    };
    auto code_scan = [&](const ScanDef& sd, Coder& cd, std::vector<std::uint8_t>* o) {
        int pred[3] = {0, 0, 0};
        std::uint32_t todo = ri, rst = 0;
        auto before_mcu = [&]() {
            if (!ri) return;
            if (todo == 0) {
                if (!cd.counting) {
                    cd.bw->flush();
                    put_marker(*o, 0xD0 + static_cast<int>(rst & 7));
                }
                ++rst;
                todo = ri;
                pred[0] = pred[1] = pred[2] = 0;
            }
            --todo;
        };
        if (sd.n == 1) {
            const int i = sd.ci[0];
            const LossyComp& c = comps[i];
            const int t = i == 0 ? 0 : 1;
            for (std::uint32_t by = 0; by < c.nbh; ++by)
                for (std::uint32_t bx = 0; bx < c.nbw; ++bx) {
                    before_mcu();
                    code_block(c.coef.data() + (static_cast<std::size_t>(by) * c.bw + bx) * 64, pred[i], t, 2 + t, cd);
                }
            return;
        }
        for (std::uint32_t my = 0; my < mcuy; ++my)
            for (std::uint32_t mx = 0; mx < mcux; ++mx) {
                before_mcu();
                for (int j = 0; j < sd.n; ++j) {
                    const int i = sd.ci[j];
                    const LossyComp& c = comps[i];
                    const int t = i == 0 ? 0 : 1;
                    for (int yy = 0; yy < c.v; ++yy)
                        for (int xx = 0; xx < c.h; ++xx) {
                            const std::size_t bx = static_cast<std::size_t>(mx) * static_cast<std::size_t>(c.h) + static_cast<std::size_t>(xx);
                            const std::size_t by = static_cast<std::size_t>(my) * static_cast<std::size_t>(c.v) + static_cast<std::size_t>(yy);
                            code_block(c.coef.data() + (by * c.bw + bx) * 64, pred[i], t, 2 + t, cd);
                        }
                }
            }
    };

    Coder cd;
    const int ntab = nc == 1 ? 1 : 2;
    HuffSpec spec[4];
    if (opt.optimize_huffman) {
        for (const auto& sd : scans) code_scan(sd, cd, nullptr);
        for (int t = 0; t < ntab; ++t) {
            jpeg_detail::build_huffman_spec(cd.freq[t], spec[t]);
            jpeg_detail::build_huffman_spec(cd.freq[2 + t], spec[2 + t]);
        }
    } else {
        spec[0] = jpeg_detail::kStdDcLuma;
        spec[1] = jpeg_detail::kStdDcChroma;
        spec[2] = jpeg_detail::kStdAcLuma;
        spec[3] = jpeg_detail::kStdAcChroma;
    }
    for (int t = 0; t < 4; ++t)
        if (!jpeg_detail::make_codes(spec[t], cd.code[t], cd.len[t]))
            return Status(Errc::Internal, "JPEG lossy: Huffman construction failed");

    out.clear();
    out.reserve(in.size() / 4 + 2048);
    put_marker(out, 0xD8);
    for (int t = 0; t < ntab; ++t) {
        put_marker(out, 0xDB);
        put16(out, wide_q[t] ? 2 + 129 : 2 + 65);
        out.push_back(static_cast<std::uint8_t>(((wide_q[t] ? 1 : 0) << 4) | t));
        for (int k = 0; k < 64; ++k) {
            if (wide_q[t]) put16(out, qt[t][k]);
            else out.push_back(static_cast<std::uint8_t>(qt[t][k]));
        }
    }
    put_marker(out, P == 8 ? 0xC0 : 0xC1);
    put16(out, static_cast<std::uint32_t>(8 + 3 * nc));
    out.push_back(static_cast<std::uint8_t>(P));
    put16(out, Y);
    put16(out, X);
    out.push_back(static_cast<std::uint8_t>(nc));
    // Component ids: 1,2,3 for YCbCr; 'R','G','B' for untransformed colour so generic decoders
    // do not apply a YCbCr conversion.
    static const std::uint8_t rgb_ids[3] = {'R', 'G', 'B'};
    auto comp_id = [&](int i) { return static_cast<std::uint8_t>(nc == 3 && !ybr ? rgb_ids[i] : i + 1); };
    for (int i = 0; i < nc; ++i) {
        out.push_back(comp_id(i));
        out.push_back(static_cast<std::uint8_t>((comps[i].h << 4) | comps[i].v));
        out.push_back(static_cast<std::uint8_t>(comps[i].tq));
    }
    for (int t = 0; t < ntab; ++t) {
        write_dht(out, 0, t, spec[t]);
        write_dht(out, 1, t, spec[2 + t]);
    }
    if (ri) {
        put_marker(out, 0xDD);
        put16(out, 4);
        put16(out, ri);
    }
    BitWriter bw(out);
    cd.counting = false;
    cd.bw = &bw;
    for (const auto& sd : scans) {
        put_marker(out, 0xDA);
        put16(out, static_cast<std::uint32_t>(6 + 2 * sd.n));
        out.push_back(static_cast<std::uint8_t>(sd.n));
        for (int j = 0; j < sd.n; ++j) {
            const int i = sd.ci[j];
            const int t = i == 0 ? 0 : 1;
            out.push_back(comp_id(i));
            out.push_back(static_cast<std::uint8_t>((t << 4) | t));
        }
        out.push_back(0);
        out.push_back(63);
        out.push_back(0);
        code_scan(sd, cd, &out);
        bw.flush();
    }
    put_marker(out, 0xD9);
    if (cd.missing) return Status(Errc::Internal, "JPEG lossy: symbol without Huffman code");
    return Status::ok();
}

}  // namespace

Status encode_jpeg_ex(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                      const JpegEncodeOptions& opt, std::vector<std::uint8_t>& out) {
    if (Status st = check_input(in, info); !st) return st;
    if (p.lossless) return encode_lossless(in, info, p, opt, out);
    return encode_lossy(in, info, p, opt, out);
}

Status encode_jpeg(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                   std::vector<std::uint8_t>& out) {
    JpegEncodeOptions opt;
    switch (p.chroma_subsampling) {
        case 0: break;
        case 1: opt.luma_h = 2; break;
        case 2: opt.luma_h = 2; opt.luma_v = 2; break;
        case 3: opt.luma_h = 4; break;
        default: return Status(Errc::InvalidArgument, "unknown chroma subsampling");
    }
    opt.restart_interval = p.restart_interval;
    return encode_jpeg_ex(in, info, p, opt, out);
}

}  // namespace siaara::codec
