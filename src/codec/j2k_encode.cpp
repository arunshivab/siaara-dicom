// SIAARA DICOM Toolkit - JPEG 2000 / HTJ2K encoder, written from T.800 (informative encoder
// clauses of annexes B, D, E, F, G) and T.814 annex F.
//
// Lossless: reversible 5-3 wavelet and RCT. Lossy: irreversible 9-7 wavelet and ICT with scalar
// quantisation whose step sizes follow the synthesis norms of each sub-band, so that every
// sub-band contributes equally to the squared error. Quality layers are formed by
// rate-distortion optimised truncation of the code-block bit-streams (convex hull of each block's
// truncation points, one slope threshold per layer). HTJ2K: lossless, cleanup pass only.
// One precinct per resolution, any of the five progression orders, optional tiling.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "codec/j2k.hpp"
#include "codec/j2k_internal.hpp"
#include "core/types.hpp"

namespace siaara::codec {

namespace {

using namespace j2k::detail;

Status invalid(const std::string& what) { return Status(Errc::InvalidArgument, "JPEG 2000 encoder: " + what); }

void put16(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    put16(o, v >> 16);
    put16(o, v & 0xFFFFu);
}

// Packet header bit writer with the bit stuffing of B.10.1.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void bit(int b) {
        cur_ = static_cast<std::uint32_t>((cur_ << 1) | static_cast<std::uint32_t>(b & 1));
        if (--left_ == 0) emit();
    }
    void bits(std::uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bit(static_cast<int>((v >> i) & 1u));
    }
    void finish() {
        if (left_ != cap_) {
            cur_ <<= left_;
            left_ = 0;
            emit();
        }
        if (last_ff_) out_.push_back(0x00);  // the stuffed zero bit after a final 0xFF
    }

private:
    void emit() {
        const auto b = static_cast<std::uint8_t>(cur_);
        out_.push_back(b);
        last_ff_ = b == 0xFF;
        cap_ = last_ff_ ? 7 : 8;
        left_ = cap_;
        cur_ = 0;
    }
    std::vector<std::uint8_t>& out_;
    std::uint32_t cur_ = 0;
    int left_ = 8;
    int cap_ = 8;
    bool last_ff_ = false;
};

// Tag tree encoder (B.10.2), the counterpart of the decoder's.
class TagTreeEnc {
public:
    void init(std::size_t w, std::size_t h, const std::vector<int>& leaves) {
        lw_.clear();
        lh_.clear();
        off_.clear();
        std::size_t total = 0, cw = w, ch = h;
        for (;;) {
            lw_.push_back(cw);
            lh_.push_back(ch);
            off_.push_back(total);
            total += cw * ch;
            if (cw == 1 && ch == 1) break;
            cw = (cw + 1) / 2;
            ch = (ch + 1) / 2;
        }
        val_.assign(total, 1 << 30);
        low_.assign(total, 0);
        known_.assign(total, 0);
        for (std::size_t i = 0; i < w * h; ++i) val_[i] = leaves[i];
        for (std::size_t l = 0; l + 1 < lw_.size(); ++l) {
            for (std::size_t y = 0; y < lh_[l]; ++y)
                for (std::size_t x = 0; x < lw_[l]; ++x) {
                    int& parent = val_[off_[l + 1] + (y / 2) * lw_[l + 1] + x / 2];
                    parent = std::min(parent, val_[off_[l] + y * lw_[l] + x]);
                }
        }
    }
    void encode(BitWriter& bw, std::size_t x, std::size_t y, int threshold) {
        std::size_t path[40];
        std::size_t depth = 0;
        for (std::size_t l = 0; l < lw_.size(); ++l) {
            path[depth++] = off_[l] + y * lw_[l] + x;
            x >>= 1;
            y >>= 1;
        }
        int low = 0;
        for (std::size_t i = depth; i-- > 0;) {
            const std::size_t n = path[i];
            if (low > low_[n]) low_[n] = low;
            else low = low_[n];
            while (low < threshold) {
                if (low >= val_[n]) {
                    if (!known_[n]) {
                        bw.bit(1);
                        known_[n] = 1;
                    }
                    break;
                }
                bw.bit(0);
                ++low;
            }
            low_[n] = low;
        }
    }

private:
    std::vector<std::size_t> lw_, lh_, off_;
    std::vector<int> val_, low_;
    std::vector<std::uint8_t> known_;
};

struct EncBlock {
    std::int64_t x0, y0, x1, y1;
    EncodedBlock eb;
    double weight = 1.0;          // squared-error weight of one quantiser step
    std::vector<int> hull;        // truncation points (pass counts) on the convex hull
    std::vector<double> slope;    // distortion-rate slope of each hull point
    std::vector<int> layer_passes;
    int lblock = 3;
};

struct EncBand {
    Orient o = kLL;
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::int64_t offx = 0, offy = 0;
    std::int64_t cbx0 = 0, cby0 = 0, ncbx = 0, ncby = 0;
    int cbw = 6, cbh = 6;
    int expn = 0;
    std::uint32_t mant = 0;
    float delta = 1.0f;
    double norm = 1.0;
    std::vector<EncBlock> cbs;
    TagTreeEnc incl, zbp;
};

struct EncRes {
    std::int64_t x0, y0, x1, y1;
    int nbands = 1;
    EncBand bands[3];
};

// Synthesis norm of the 1-D low-pass (`high` false) or high-pass branch after `levels` levels.
double synthesis_norm(bool reversible, int levels, bool high) {
    if (levels == 0) return 1.0;
    const std::int64_t n = std::int64_t{1} << (levels + 6);
    std::vector<ResGeom> geo;
    for (int r = 0; r <= levels; ++r) geo.push_back(ResGeom{0, 0, ceil_shift(n, levels - r), 1});
    // Impulse in the middle of the requested band at the deepest level.
    const std::int64_t nl = ceil_shift(geo[1].x1, 1);  // low-pass count of level `levels`
    const std::int64_t width_low = geo[0].x1;
    std::size_t pos = high ? static_cast<std::size_t>(width_low + (geo[1].x1 - nl) / 2) : static_cast<std::size_t>(width_low / 2);
    if (reversible) {
        std::vector<std::int32_t> v(static_cast<std::size_t>(n), 0);
        const double amp = 1 << 20;
        v[pos] = static_cast<std::int32_t>(amp);
        idwt53(v.data(), static_cast<std::size_t>(n), geo);
        double s = 0;
        for (auto x : v) s += static_cast<double>(x) * x;
        return std::sqrt(s) / amp;
    }
    std::vector<float> v(static_cast<std::size_t>(n), 0.0f);
    v[pos] = 1.0f;
    idwt97(v.data(), static_cast<std::size_t>(n), geo);
    double s = 0;
    for (auto x : v) s += static_cast<double>(x) * x;
    return std::sqrt(s);
}

// Convex hull of a block's (rate, distortion) truncation points (T.800 J.14.4, informative).
void build_hull(EncBlock& b) {
    b.hull.clear();
    b.slope.clear();
    const int n = b.eb.passes;
    std::vector<double> rate(static_cast<std::size_t>(n) + 1, 0.0), dist(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        rate[static_cast<std::size_t>(i) + 1] = b.eb.pass_end[static_cast<std::size_t>(i)];
        dist[static_cast<std::size_t>(i) + 1] = dist[static_cast<std::size_t>(i)] + b.eb.pass_dist[static_cast<std::size_t>(i)] * b.weight;
    }
    std::vector<int> h{0};
    for (int i = 1; i <= n; ++i) {
        for (;;) {
            const int last = h.back();
            const double dr = rate[static_cast<std::size_t>(i)] - rate[static_cast<std::size_t>(last)];
            const double dd = dist[static_cast<std::size_t>(i)] - dist[static_cast<std::size_t>(last)];
            if (dd <= 0) break;
            const double s = dr > 0 ? dd / dr : 1e300;
            if (h.size() >= 2) {
                const int prev = h[h.size() - 2];
                const double pr = rate[static_cast<std::size_t>(last)] - rate[static_cast<std::size_t>(prev)];
                const double pd = dist[static_cast<std::size_t>(last)] - dist[static_cast<std::size_t>(prev)];
                const double ps = pr > 0 ? pd / pr : 1e300;
                if (s >= ps) {
                    h.pop_back();
                    continue;
                }
            }
            h.push_back(i);
            break;
        }
    }
    for (std::size_t k = 1; k < h.size(); ++k) {
        const double dr = rate[static_cast<std::size_t>(h[k])] - rate[static_cast<std::size_t>(h[k - 1])];
        const double dd = dist[static_cast<std::size_t>(h[k])] - dist[static_cast<std::size_t>(h[k - 1])];
        b.hull.push_back(h[k]);
        b.slope.push_back(dr > 0 ? dd / dr : 1e300);
    }
}

int passes_at(const EncBlock& b, double lambda) {
    int p = 0;
    for (std::size_t k = 0; k < b.hull.size(); ++k) {
        if (b.slope[k] >= lambda) p = b.hull[k];
        else break;
    }
    return p;
}

std::uint64_t bytes_at(const std::vector<EncBlock*>& blocks, double lambda) {
    std::uint64_t s = 0;
    for (const EncBlock* b : blocks) {
        const int p = passes_at(*b, lambda);
        if (p > 0) s += b->eb.pass_end[static_cast<std::size_t>(p) - 1];
    }
    return s;
}

struct Plan {
    std::uint32_t w = 0, h = 0;
    int nc = 1;
    int bits = 8;
    bool sgn = false;
    bool reversible = true;
    bool mct = false;
    bool ht = false;
    int levels = 0;
    int layers = 1;
    int cbw = 6, cbh = 6;
    int prog = 0;
    std::uint32_t tw = 0, th = 0;
    double delta0 = 1.0;  // lossy: base step (in sample units) of a unit-norm sub-band
    double rate = 0;      // lossy: bits per sample target, 0 = keep all passes
    int guard = 2;
    bool sop = false, eph = false;
};

// Exponent / mantissa (E-3) for a requested step `d` of a sub-band with dynamic range rb.
void step_to_qcd(double d, int rb, int& expn, std::uint32_t& mant) {
    int e = rb - static_cast<int>(std::floor(std::log2(d)));
    e = std::clamp(e, 0, 31);
    double m = (d / std::ldexp(1.0, rb - e) - 1.0) * 2048.0;
    long mi = std::lround(m);
    if (mi > 2047) mi = 2047;
    if (mi < 0) mi = 0;
    expn = e;
    mant = static_cast<std::uint32_t>(mi);
}

class TileEncoder {
public:
    TileEncoder(const Plan& pl, std::span<const std::uint8_t> in, const FrameInfo& info) : pl_(pl), in_(in), info_(info) {
        for (int l = 0; l <= pl.levels; ++l) {
            nlow_.push_back(synthesis_norm(pl.reversible, l, false));
            nhigh_.push_back(l == 0 ? 1.0 : synthesis_norm(pl.reversible, l, true));
        }
    }

    // Appends one tile-part for the tile with reference-grid rectangle [x0,x1) x [y0,y1).
    Status encode(std::uint32_t index, std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1,
                  std::vector<std::uint8_t>& out, int& guard_main, bool first_tile);
    // Sub-band parameters of component 0 of the last tile (for the main QCD).
    const std::vector<EncRes>& bands0() const { return last_bands_; }

private:
    void load(std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1);
    void geometry(std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1);

    const Plan& pl_;
    std::span<const std::uint8_t> in_;
    const FrameInfo& info_;
    std::vector<double> nlow_, nhigh_;
    std::vector<std::vector<std::int32_t>> ibuf_;
    std::vector<std::vector<float>> fbuf_;
    std::vector<std::vector<EncRes>> res_;  // per component
    std::vector<EncRes> last_bands_;
    std::uint32_t sop_count_ = 0;
};

void TileEncoder::load(std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1) {
    const std::size_t w = static_cast<std::size_t>(x1 - x0), h = static_cast<std::size_t>(y1 - y0);
    const std::size_t nc = static_cast<std::size_t>(pl_.nc);
    const std::size_t bps = info_.bits_allocated / 8u;
    ibuf_.assign(nc, std::vector<std::int32_t>(w * h));
    const std::int32_t shift = pl_.sgn ? 0 : (1 << (pl_.bits - 1));
    const std::uint32_t mask = (pl_.bits >= 32) ? 0xFFFFFFFFu : ((1u << pl_.bits) - 1u);
    for (std::size_t c = 0; c < nc; ++c) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t px = static_cast<std::size_t>(x0) + x, py = static_cast<std::size_t>(y0) + y;
                std::size_t idx;
                if (info_.planar) idx = (c * info_.rows + py) * info_.columns + px;
                else idx = (py * info_.columns + px) * nc + c;
                const std::uint8_t* s = in_.data() + idx * bps;
                std::uint32_t v = bps == 1 ? s[0] : load_le16(s);
                v &= mask;
                std::int32_t sv;
                if (pl_.sgn && (v >> (pl_.bits - 1)) & 1u) sv = static_cast<std::int32_t>(v) - (1 << pl_.bits);
                else sv = static_cast<std::int32_t>(v);
                ibuf_[c][y * w + x] = sv - shift;
            }
        }
    }
}

void TileEncoder::geometry(std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1) {
    const int nl = pl_.levels;
    res_.assign(static_cast<std::size_t>(pl_.nc), std::vector<EncRes>(static_cast<std::size_t>(nl) + 1));
    for (auto& comp : res_) {
        for (int r = 0; r <= nl; ++r) {
            EncRes& rs = comp[static_cast<std::size_t>(r)];
            const int sh = nl - r;
            rs.x0 = ceil_shift(x0, sh);
            rs.y0 = ceil_shift(y0, sh);
            rs.x1 = ceil_shift(x1, sh);
            rs.y1 = ceil_shift(y1, sh);
            rs.nbands = r == 0 ? 1 : 3;
            const int nb = r == 0 ? nl : nl - r + 1;
            for (int b = 0; b < rs.nbands; ++b) {
                EncBand& bd = rs.bands[b];
                bd.o = r == 0 ? kLL : static_cast<Orient>(b + 1);
                const std::int64_t xob = (bd.o == kHL || bd.o == kHH) ? 1 : 0;
                const std::int64_t yob = (bd.o == kLH || bd.o == kHH) ? 1 : 0;
                if (r == 0) {
                    bd.x0 = rs.x0; bd.y0 = rs.y0; bd.x1 = rs.x1; bd.y1 = rs.y1;
                } else {
                    const std::int64_t hf = std::int64_t{1} << (nb - 1);
                    // x0 - hf can be negative: ceiling division that is exact for both signs.
                    auto cdiv = [nb](std::int64_t a) {
                        const std::int64_t d = std::int64_t{1} << nb;
                        return a >= 0 ? (a + d - 1) / d : -((-a) / d);
                    };
                    bd.x0 = cdiv(x0 - hf * xob);
                    bd.y0 = cdiv(y0 - hf * yob);
                    bd.x1 = cdiv(x1 - hf * xob);
                    bd.y1 = cdiv(y1 - hf * yob);
                    const EncRes& lo = comp[static_cast<std::size_t>(r) - 1];
                    bd.offx = xob ? lo.x1 - lo.x0 : 0;
                    bd.offy = yob ? lo.y1 - lo.y0 : 0;
                }
                bd.norm = (xob ? nhigh_[static_cast<std::size_t>(nb)] : nlow_[static_cast<std::size_t>(nb)]) *
                          (yob ? nhigh_[static_cast<std::size_t>(nb)] : nlow_[static_cast<std::size_t>(nb)]);
                bd.cbw = pl_.cbw;
                bd.cbh = pl_.cbh;
                bd.cbs.clear();
                if (bd.x1 > bd.x0 && bd.y1 > bd.y0) {
                    bd.cbx0 = bd.x0 >> bd.cbw;
                    bd.cby0 = bd.y0 >> bd.cbh;
                    bd.ncbx = ceil_shift(bd.x1, bd.cbw) - bd.cbx0;
                    bd.ncby = ceil_shift(bd.y1, bd.cbh) - bd.cby0;
                    bd.cbs.resize(static_cast<std::size_t>(bd.ncbx * bd.ncby));
                    for (std::int64_t j = 0; j < bd.ncby; ++j)
                        for (std::int64_t i = 0; i < bd.ncbx; ++i) {
                            EncBlock& cb = bd.cbs[static_cast<std::size_t>(j * bd.ncbx + i)];
                            cb.x0 = std::max(bd.x0, (bd.cbx0 + i) << bd.cbw);
                            cb.y0 = std::max(bd.y0, (bd.cby0 + j) << bd.cbh);
                            cb.x1 = std::min(bd.x1, (bd.cbx0 + i + 1) << bd.cbw);
                            cb.y1 = std::min(bd.y1, (bd.cby0 + j + 1) << bd.cbh);
                        }
                } else {
                    bd.ncbx = bd.ncby = 0;
                }
            }
        }
    }
}

Status TileEncoder::encode(std::uint32_t index, std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1,
                           std::vector<std::uint8_t>& out, int& guard_main, bool first_tile) {
    load(x0, y0, x1, y1);
    geometry(x0, y0, x1, y1);
    const std::size_t w = static_cast<std::size_t>(x1 - x0), h = static_cast<std::size_t>(y1 - y0);
    const std::size_t nc = static_cast<std::size_t>(pl_.nc);
    // Colour transform (G.2 / G.3) and wavelet transform (F.4).
    if (pl_.reversible) {
        if (pl_.mct) {
            std::int32_t *r = ibuf_[0].data(), *g = ibuf_[1].data(), *b = ibuf_[2].data();
            for (std::size_t i = 0; i < w * h; ++i) {
                const std::int32_t y = static_cast<std::int32_t>((std::int64_t{r[i]} + 2 * std::int64_t{g[i]} + b[i]) >> 2);
                const std::int32_t cb = b[i] - g[i];
                const std::int32_t cr = r[i] - g[i];
                r[i] = y;
                g[i] = cb;
                b[i] = cr;
            }
        }
    } else {
        fbuf_.assign(nc, std::vector<float>(w * h));
        for (std::size_t c = 0; c < nc; ++c)
            for (std::size_t i = 0; i < w * h; ++i) fbuf_[c][i] = static_cast<float>(ibuf_[c][i]);
        ibuf_.clear();
        if (pl_.mct) {
            float *r = fbuf_[0].data(), *g = fbuf_[1].data(), *b = fbuf_[2].data();
            for (std::size_t i = 0; i < w * h; ++i) {
                const float y = 0.299f * r[i] + 0.587f * g[i] + 0.114f * b[i];
                const float cb = -0.16875f * r[i] - 0.33126f * g[i] + 0.5f * b[i];
                const float cr = 0.5f * r[i] - 0.41869f * g[i] - 0.08131f * b[i];
                r[i] = y;
                g[i] = cb;
                b[i] = cr;
            }
        }
    }
    int guard = guard_main;
    // Quantiser parameters per sub-band (shared by all components through QCD).
    for (std::size_t c = 0; c < nc; ++c) {
        std::vector<ResGeom> geo;
        for (const auto& rs : res_[c]) geo.push_back(ResGeom{rs.x0, rs.y0, rs.x1, rs.y1});
        if (w > 0 && h > 0) {
            if (pl_.reversible) fdwt53(ibuf_[c].data(), w, geo);
            else fdwt97(fbuf_[c].data(), w, geo);
        }
    }
    std::vector<std::uint32_t> mag;
    std::vector<std::uint8_t> neg;
    std::vector<EncBlock*> all;
    // First pass: quantise every block, find the largest magnitude per band for the guard bits.
    struct Pending { EncBlock* cb; EncBand* bd; std::size_t c; };
    std::vector<Pending> work;
    int need_guard = 0;
    std::vector<std::vector<std::uint32_t>> qmag;
    std::vector<std::vector<std::uint8_t>> qneg;
    for (std::size_t c = 0; c < nc; ++c) {
        for (auto& rs : res_[c]) {
            for (int b = 0; b < rs.nbands; ++b) {
                EncBand& bd = rs.bands[b];
                const int rb = pl_.bits + (bd.o == kLL ? 0 : (bd.o == kHH ? 2 : 1));
                if (pl_.reversible) {
                    bd.expn = rb + (pl_.mct ? 1 : 0);
                    bd.mant = 0;
                    bd.delta = 1.0f;
                } else {
                    step_to_qcd(pl_.delta0 / bd.norm, rb, bd.expn, bd.mant);
                    bd.delta = static_cast<float>(std::ldexp(1.0 + bd.mant / 2048.0, rb - bd.expn));
                }
                const double wgt = pl_.reversible ? bd.norm * bd.norm : (static_cast<double>(bd.delta) * bd.norm) * (static_cast<double>(bd.delta) * bd.norm);
                for (EncBlock& cb : bd.cbs) {
                    const std::size_t bw = static_cast<std::size_t>(cb.x1 - cb.x0), bh = static_cast<std::size_t>(cb.y1 - cb.y0);
                    std::vector<std::uint32_t> m(bw * bh);
                    std::vector<std::uint8_t> sg(bw * bh);
                    std::uint32_t maxv = 0;
                    const std::size_t ox = static_cast<std::size_t>(bd.offx + (cb.x0 - bd.x0));
                    const std::size_t oy = static_cast<std::size_t>(bd.offy + (cb.y0 - bd.y0));
                    const float inv = 1.0f / bd.delta;
                    for (std::size_t y = 0; y < bh; ++y) {
                        for (std::size_t x = 0; x < bw; ++x) {
                            const std::size_t si = (oy + y) * w + ox + x;
                            std::uint32_t q;
                            bool n;
                            if (pl_.reversible) {
                                const std::int32_t v = ibuf_[c][si];
                                n = v < 0;
                                q = static_cast<std::uint32_t>(n ? -std::int64_t{v} : std::int64_t{v});
                            } else {
                                const float v = fbuf_[c][si];
                                n = v < 0;
                                const float a = std::min(std::fabs(v) * inv, 1.0e9f);
                                q = static_cast<std::uint32_t>(a);
                            }
                            m[y * bw + x] = q;
                            sg[y * bw + x] = (n && q) ? 1 : 0;
                            maxv = std::max(maxv, q);
                        }
                    }
                    const int nbits = static_cast<int>(std::bit_width(maxv));
                    need_guard = std::max(need_guard, nbits - bd.expn + 1);
                    cb.weight = wgt;
                    qmag.push_back(std::move(m));
                    qneg.push_back(std::move(sg));
                    work.push_back({&cb, &bd, c});
                }
            }
        }
    }
    // Guard bits (E.1): the main header uses the first tile's; a later tile that needs more
    // gets its own QCD in its tile-part header.
    if (need_guard > 7) return Status(Errc::Internal, "JPEG 2000 encoder: coefficient range exceeds 7 guard bits");
    guard = std::max(guard, need_guard);
    if (first_tile) guard_main = guard;
    const bool tile_qcd = guard != guard_main;
    sop_count_ = 0;
    // Block coding.
    for (std::size_t i = 0; i < work.size(); ++i) {
        EncBlock& cb = *work[i].cb;
        EncBand& bd = *work[i].bd;
        const int bw = static_cast<int>(cb.x1 - cb.x0), bh = static_cast<int>(cb.y1 - cb.y0);
        const int planes = guard + bd.expn - 1;
        if (pl_.ht) {
            if (!encode_ht_block(bw, bh, planes, qmag[i].data(), qneg[i].data(), static_cast<std::size_t>(bw), cb.eb))
                return Status(Errc::Internal, "JPEG 2000 encoder: HT block coding failed");
        } else {
            encode_t1_block(bw, bh, bd.o, planes, qmag[i].data(), qneg[i].data(), static_cast<std::size_t>(bw), cb.eb);
        }
        build_hull(cb);
        all.push_back(&cb);
        qmag[i] = {};
        qneg[i] = {};
    }
    ibuf_.clear();
    fbuf_.clear();

    // Layer formation: one distortion-rate slope threshold per layer.
    const int L = pl_.layers;
    std::uint64_t total = bytes_at(all, -1.0);
    std::uint64_t final_budget = total;
    if (!pl_.reversible && pl_.rate > 0) {
        final_budget = static_cast<std::uint64_t>(pl_.rate * static_cast<double>(w * h * nc) / 8.0);
    }
    std::vector<double> lambdas(static_cast<std::size_t>(L));
    for (int l = 0; l < L; ++l) {
        const bool last = l == L - 1;
        const std::uint64_t budget = last ? final_budget : (final_budget >> (L - 1 - l));
        if (last && budget >= total) {
            lambdas[static_cast<std::size_t>(l)] = -1.0;
            continue;
        }
        // Bisection on the slope threshold, in the log domain.
        double llo = -60, lhi = 120;
        for (int it = 0; it < 80; ++it) {
            const double mid = 0.5 * (llo + lhi);
            const double lam = std::exp2(mid);
            if (bytes_at(all, lam) > budget) llo = mid;
            else lhi = mid;
        }
        lambdas[static_cast<std::size_t>(l)] = std::exp2(lhi);
    }
    for (EncBlock* b : all) {
        b->layer_passes.assign(static_cast<std::size_t>(L), 0);
        int prev = 0;
        for (int l = 0; l < L; ++l) {
            int p = lambdas[static_cast<std::size_t>(l)] < 0 ? b->eb.passes : passes_at(*b, lambdas[static_cast<std::size_t>(l)]);
            p = std::max(p, prev);
            b->layer_passes[static_cast<std::size_t>(l)] = p;
            prev = p;
        }
    }
    // Tag trees per band (one precinct per resolution).
    for (std::size_t c = 0; c < nc; ++c) {
        for (auto& rs : res_[c]) {
            for (int b = 0; b < rs.nbands; ++b) {
                EncBand& bd = rs.bands[b];
                if (bd.cbs.empty()) continue;
                std::vector<int> first(bd.cbs.size()), zb(bd.cbs.size());
                const int planes = guard + bd.expn - 1;
                for (std::size_t i = 0; i < bd.cbs.size(); ++i) {
                    int f = L;
                    for (int l = 0; l < L; ++l)
                        if (bd.cbs[i].layer_passes[static_cast<std::size_t>(l)] > 0) {
                            f = l;
                            break;
                        }
                    first[i] = f;
                    zb[i] = bd.cbs[i].eb.passes > 0 ? bd.cbs[i].eb.zero_planes : planes;
                }
                bd.incl.init(static_cast<std::size_t>(bd.ncbx), static_cast<std::size_t>(bd.ncby), first);
                bd.zbp.init(static_cast<std::size_t>(bd.ncbx), static_cast<std::size_t>(bd.ncby), zb);
            }
        }
    }

    // Packets.
    std::vector<std::uint8_t> data;
    std::vector<int> done_passes(all.size(), 0);
    auto packet = [&](int l, std::size_t c, int r) {
        EncRes& rs = res_[c][static_cast<std::size_t>(r)];
        if (rs.x1 <= rs.x0 || rs.y1 <= rs.y0) return;  // no precinct
        if (pl_.sop) {
            put16(data, kSOP);
            put16(data, 4);
            put16(data, static_cast<std::uint32_t>(sop_count_++ & 0xFFFF));
        }
        std::vector<std::uint8_t> hdr;
        BitWriter bw(hdr);
        std::vector<std::pair<const EncBlock*, std::pair<std::uint32_t, std::uint32_t>>> bodies;
        bool any = false;
        for (int b = 0; b < rs.nbands && !any; ++b)
            for (const EncBlock& cb : rs.bands[b].cbs) {
                const int prev = l == 0 ? 0 : cb.layer_passes[static_cast<std::size_t>(l) - 1];
                if (cb.layer_passes[static_cast<std::size_t>(l)] > prev) {
                    any = true;
                    break;
                }
            }
        bw.bit(any ? 1 : 0);
        if (any) {
            for (int b = 0; b < rs.nbands; ++b) {
                EncBand& bd = rs.bands[b];
                for (std::int64_t j = 0; j < bd.ncby; ++j) {
                    for (std::int64_t i = 0; i < bd.ncbx; ++i) {
                        EncBlock& cb = bd.cbs[static_cast<std::size_t>(j * bd.ncbx + i)];
                        const int prev = l == 0 ? 0 : cb.layer_passes[static_cast<std::size_t>(l) - 1];
                        const int cur = cb.layer_passes[static_cast<std::size_t>(l)];
                        const bool inc = cur > prev;
                        if (prev == 0) {
                            bd.incl.encode(bw, static_cast<std::size_t>(i), static_cast<std::size_t>(j), l + 1);
                        } else {
                            bw.bit(inc ? 1 : 0);
                        }
                        if (!inc) continue;
                        if (prev == 0) bd.zbp.encode(bw, static_cast<std::size_t>(i), static_cast<std::size_t>(j), 1 << 20);
                        const int n = cur - prev;
                        // Table B.4.
                        if (n == 1) bw.bit(0);
                        else if (n == 2) bw.bits(2, 2);
                        else if (n <= 5) bw.bits(0xC | static_cast<std::uint32_t>(n - 3), 4);
                        else if (n <= 36) bw.bits((0xFu << 5) | static_cast<std::uint32_t>(n - 6), 9);
                        else bw.bits((0x1FFu << 7) | static_cast<std::uint32_t>(n - 37), 16);
                        const std::uint32_t start = prev ? cb.eb.pass_end[static_cast<std::size_t>(prev) - 1] : 0;
                        const std::uint32_t end = cb.eb.pass_end[static_cast<std::size_t>(cur) - 1];
                        const std::uint32_t len = end - start;
                        const int extra = static_cast<int>(std::bit_width(static_cast<unsigned>(n))) - 1;
                        const int need = std::max(1, static_cast<int>(std::bit_width(len))) - extra;
                        while (cb.lblock < need) {
                            bw.bit(1);
                            ++cb.lblock;
                        }
                        bw.bit(0);
                        bw.bits(len, cb.lblock + extra);
                        bodies.push_back({&cb, {start, len}});
                    }
                }
            }
        }
        bw.finish();
        data.insert(data.end(), hdr.begin(), hdr.end());
        if (pl_.eph) put16(data, kEPH);
        for (const auto& [cb, sl] : bodies)
            data.insert(data.end(), cb->eb.data.begin() + static_cast<std::ptrdiff_t>(sl.first),
                        cb->eb.data.begin() + static_cast<std::ptrdiff_t>(sl.first) + static_cast<std::ptrdiff_t>(sl.second));
    };
    const int R = pl_.levels + 1;
    switch (pl_.prog) {
        case 0:
            for (int l = 0; l < L; ++l) for (int r = 0; r < R; ++r) for (std::size_t c = 0; c < nc; ++c) packet(l, c, r);
            break;
        case 1:
            for (int r = 0; r < R; ++r) for (int l = 0; l < L; ++l) for (std::size_t c = 0; c < nc; ++c) packet(l, c, r);
            break;
        case 2:
            for (int r = 0; r < R; ++r) for (std::size_t c = 0; c < nc; ++c) for (int l = 0; l < L; ++l) packet(l, c, r);
            break;
        default:  // PCRL and CPRL coincide with one precinct per resolution
            for (std::size_t c = 0; c < nc; ++c) for (int r = 0; r < R; ++r) for (int l = 0; l < L; ++l) packet(l, c, r);
            break;
    }

    // Tile-part: SOT, optional QCD, SOD, data.
    const std::size_t sot = out.size();
    put16(out, kSOT);
    put16(out, 10);
    put16(out, index);
    put32(out, 0);  // Psot, patched below
    out.push_back(0);
    out.push_back(1);
    if (tile_qcd) {
        put16(out, kQCD);
        const std::size_t nb = static_cast<std::size_t>(3 * pl_.levels + 1);
        put16(out, static_cast<std::uint32_t>(pl_.reversible ? 3 + nb : 3 + 2 * nb));
        out.push_back(static_cast<std::uint8_t>((guard << 5) | (pl_.reversible ? 0 : 2)));
        const auto& comp = res_[0];
        for (const auto& rs : comp)
            for (int b = 0; b < rs.nbands; ++b) {
                if (pl_.reversible) out.push_back(static_cast<std::uint8_t>(rs.bands[b].expn << 3));
                else put16(out, static_cast<std::uint32_t>(rs.bands[b].expn << 11) | rs.bands[b].mant);
            }
    }
    put16(out, kSOD);
    out.insert(out.end(), data.begin(), data.end());
    const std::size_t psot = out.size() - sot;
    if (psot > 0xFFFFFFFFu) return Status(Errc::LimitExceeded, "JPEG 2000 encoder: tile-part too large");
    store_be32(&out[sot + 6], static_cast<std::uint32_t>(psot));
    last_bands_ = res_[0];
    return Status::ok();
}

}  // namespace

Status j2k::encode(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeOptions& opt,
                   std::vector<std::uint8_t>& out) try {
    if (info.frame_bytes() == 0) return invalid("invalid frame parameters");
    if (!info.encode_fits_budget(16)) return Status(Errc::LimitExceeded, "JPEG 2000 encoder: frame exceeds the memory budget");
    if (in.size() != info.frame_bytes()) return invalid("input size differs from the frame size");
    if (info.bits_allocated != 8 && info.bits_allocated != 16) return invalid("BitsAllocated must be 8 or 16");
    if (info.bits_stored < 1 || info.bits_stored > info.bits_allocated) return invalid("bad BitsStored");
    if (info.samples_per_pixel > 16384) return invalid("too many samples per pixel");
    if (opt.ht && !opt.lossless) return invalid("the HTJ2K encoder is lossless only");
    if (opt.layers < 1 || opt.layers > 64) return invalid("layers must be 1..64");
    // NaN or infinite values reached the step size and a double-to-integer budget cast (R-065).
    if (!std::isfinite(opt.quality) || !std::isfinite(opt.rate) || opt.rate < 0.0 || opt.rate > 1e6)
        return invalid("quality and rate must be finite (rate 0..1e6 bits per sample)");
    if (opt.levels < 0 || opt.levels > kMaxLevels) return invalid("levels must be 0..32");
    if (opt.cblk_width_exp < 2 || opt.cblk_height_exp < 2 || opt.cblk_width_exp > 10 || opt.cblk_height_exp > 10 ||
        opt.cblk_width_exp + opt.cblk_height_exp > 12)
        return invalid("bad code-block size");
    Plan pl;
    pl.w = info.columns;
    pl.h = info.rows;
    pl.nc = info.samples_per_pixel;
    pl.bits = info.bits_stored;
    pl.sgn = info.is_signed;
    pl.reversible = opt.lossless;
    pl.ht = opt.ht;
    pl.mct = opt.mct && pl.nc >= 3;
    pl.layers = opt.ht ? 1 : opt.layers;
    pl.cbw = opt.cblk_width_exp;
    pl.cbh = opt.cblk_height_exp;
    pl.prog = static_cast<int>(opt.progression);
    if (pl.prog < 0 || pl.prog > 4) return invalid("bad progression order");
    pl.tw = opt.tile_width ? opt.tile_width : pl.w;
    pl.th = opt.tile_height ? opt.tile_height : pl.h;
    pl.sop = opt.sop;
    pl.eph = opt.eph;
    std::uint32_t mind = std::min(pl.tw, pl.th);
    mind = std::min(mind, std::min(pl.w, pl.h));
    int maxl = 0;
    while ((std::uint64_t{2} << maxl) <= mind && maxl < kMaxLevels) ++maxl;
    pl.levels = std::min(opt.levels, maxl);
    if (!pl.reversible) {
        const double q = std::clamp(opt.quality, 1.0, 100.0);
        pl.delta0 = std::ldexp(1.0, pl.bits - 8) * 0.4 * std::exp2((100.0 - q) / 10.0);
        pl.rate = opt.rate;
    }
    // Main header is written after the first tile has fixed the guard bits.
    std::vector<std::uint8_t> tiles;
    TileEncoder te(pl, in, info);
    const std::uint32_t ntx = (pl.w + pl.tw - 1) / pl.tw, nty = (pl.h + pl.th - 1) / pl.th;
    if (std::uint64_t{ntx} * nty > 65535) return invalid("too many tiles");
    int guard = 2;
    std::vector<std::uint8_t> qcd_body;
    for (std::uint32_t ty = 0; ty < nty; ++ty) {
        for (std::uint32_t tx = 0; tx < ntx; ++tx) {
            const std::int64_t x0 = std::int64_t{tx} * pl.tw, y0 = std::int64_t{ty} * pl.th;
            const std::int64_t x1 = std::min<std::int64_t>(x0 + pl.tw, pl.w), y1 = std::min<std::int64_t>(y0 + pl.th, pl.h);
            if (Status s = te.encode(ty * ntx + tx, x0, y0, x1, y1, tiles, guard, tx == 0 && ty == 0); !s) return s;
            if (tx == 0 && ty == 0) {
                // Record the main QCD from the first tile's step sizes.
                const auto& comp = te.bands0();
                qcd_body.push_back(static_cast<std::uint8_t>((guard << 5) | (pl.reversible ? 0 : 2)));
                for (const auto& rs : comp)
                    for (int b = 0; b < rs.nbands; ++b) {
                        if (pl.reversible) qcd_body.push_back(static_cast<std::uint8_t>(rs.bands[b].expn << 3));
                        else put16(qcd_body, static_cast<std::uint32_t>(rs.bands[b].expn << 11) | rs.bands[b].mant);
                    }
            }
        }
    }
    out.clear();
    put16(out, kSOC);
    // SIZ (A.5.1).
    put16(out, kSIZ);
    put16(out, static_cast<std::uint32_t>(38 + 3 * pl.nc));
    put16(out, pl.ht ? 0x4000u : 0u);
    put32(out, pl.w);
    put32(out, pl.h);
    put32(out, 0);
    put32(out, 0);
    put32(out, pl.tw);
    put32(out, pl.th);
    put32(out, 0);
    put32(out, 0);
    put16(out, static_cast<std::uint32_t>(pl.nc));
    for (int c = 0; c < pl.nc; ++c) {
        out.push_back(static_cast<std::uint8_t>((pl.bits - 1) | (pl.sgn ? 0x80 : 0)));
        out.push_back(1);
        out.push_back(1);
    }
    if (pl.ht) {
        // CAP (T.814 A.3): Part 15 capabilities; HTONLY, SINGLEHT, RGNFREE, HOMOGENEOUS,
        // reversible only, magnitude bound B from the largest number of coded bit-planes.
        put16(out, kCAP);
        put16(out, 8);
        put32(out, 1u << 17);
        const int b = guard + pl.bits + 2 + (pl.mct ? 1 : 0) - 1;
        const int p = b <= 8 ? 0 : (b < 28 ? b - 8 : std::min(31, 20 + (b - 27 + 3) / 4));
        put16(out, static_cast<std::uint32_t>(p));
    }
    // COD (A.6.1).
    put16(out, kCOD);
    put16(out, 12);
    out.push_back(static_cast<std::uint8_t>((pl.sop ? 2 : 0) | (pl.eph ? 4 : 0)));
    out.push_back(static_cast<std::uint8_t>(pl.prog));
    put16(out, static_cast<std::uint32_t>(pl.layers));
    out.push_back(pl.mct ? 1 : 0);
    out.push_back(static_cast<std::uint8_t>(pl.levels));
    out.push_back(static_cast<std::uint8_t>(pl.cbw - 2));
    out.push_back(static_cast<std::uint8_t>(pl.cbh - 2));
    out.push_back(pl.ht ? kCbHT : 0);
    out.push_back(pl.reversible ? 1 : 0);
    // QCD (A.6.4).
    put16(out, kQCD);
    put16(out, static_cast<std::uint32_t>(2 + qcd_body.size()));
    out.insert(out.end(), qcd_body.begin(), qcd_body.end());
    out.insert(out.end(), tiles.begin(), tiles.end());
    put16(out, kEOC);
    return Status::ok();
} catch (...) {
    return Status::out_of_memory("JPEG 2000 encoder: out of memory");  // R-062: never thrown
}

Status encode_j2k(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                  std::vector<std::uint8_t>& out) {
    j2k::EncodeOptions o;
    o.lossless = p.lossless;
    o.quality = p.quality;
    o.mct = info.samples_per_pixel >= 3;
    return j2k::encode(in, info, o, out);
}

}  // namespace siaara::codec
