// SIAARA DICOM Toolkit - JPEG 2000 / HTJ2K decoder, written from T.800 annexes A, B, E, F, G, H, I
// and T.814 annexes A and B. Codestream syntax, Tier-2 packet decoding with every progression
// order and POC volumes, precincts, PPM/PPT packed headers, SOP/EPH, then block decoding,
// dequantisation, inverse wavelet and colour transforms, DC level shift and output.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "codec/j2k.hpp"
#include "codec/j2k_internal.hpp"
#include "core/types.hpp"

namespace siaara::codec {

namespace {

using namespace j2k::detail;

Status malformed(const std::string& what, std::uint64_t off = 0) {
    return Status(Errc::Malformed, "JPEG 2000: " + what, off);
}
Status unsupported(const std::string& what, std::uint64_t off = 0) {
    return Status(Errc::Unsupported, "JPEG 2000: " + what, off);
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
std::int64_t ceil_div_s(std::int64_t a, std::int64_t b) { return -floor_div(-a, b); }

// ---------------------------------------------------------------- marker segment contents

struct CompSiz {
    int depth = 8;
    bool sgn = false;
    std::uint32_t dx = 1, dy = 1;
};

struct Siz {
    std::uint16_t rsiz = 0;
    std::uint32_t xsiz = 0, ysiz = 0, xo = 0, yo = 0, xt = 0, yt = 0, xto = 0, yto = 0;
    std::uint32_t ntx = 0, nty = 0;
    std::vector<CompSiz> comp;
};

struct CodStyle {
    int levels = 5;
    int xcb = 6, ycb = 6;   // exponents
    std::uint8_t style = 0;
    int transform = 0;      // 0 = 9-7, 1 = 5-3
    std::uint8_t ppx[kMaxLevels + 1] = {};
    std::uint8_t ppy[kMaxLevels + 1] = {};
};

struct QuantStyle {
    int style = 0;
    int guard = 2;
    int n = 0;
    std::uint8_t exp[3 * kMaxLevels + 1] = {};
    std::uint16_t mant[3 * kMaxLevels + 1] = {};
};

struct Poc {
    int rs = 0, re = 0, cs = 0, ce = 0, lye = 0, prog = 0;
};

struct Params {
    bool has_cod = false;
    std::uint8_t scod = 0;
    int prog = 0;
    int layers = 1;
    int mct = 0;
    CodStyle cod;
    std::vector<std::uint8_t> has_coc;
    std::vector<CodStyle> coc;
    bool has_qcd = false;
    QuantStyle qcd;
    std::vector<std::uint8_t> has_qcc;
    std::vector<QuantStyle> qcc;
    std::vector<int> rgn;  // -1: none
    std::vector<Poc> poc;
};

class SegReader {
public:
    SegReader(std::span<const std::uint8_t> s, std::size_t off) : s_(s), off_(off) {}
    bool has(std::size_t n) const { return s_.size() - pos_ >= n; }
    std::size_t left() const { return s_.size() - pos_; }
    std::uint32_t u8() { return s_[pos_++]; }
    std::uint32_t u16() {
        const std::uint32_t v = load_be16(&s_[pos_]);
        pos_ += 2;
        return v;
    }
    std::uint32_t u32() {
        const std::uint32_t v = load_be32(&s_[pos_]);
        pos_ += 4;
        return v;
    }
    std::size_t offset() const { return off_; }

private:
    std::span<const std::uint8_t> s_;
    std::size_t off_;
    std::size_t pos_ = 0;
};

Status parse_siz(SegReader r, Siz& siz) {
    if (!r.has(38)) return malformed("short SIZ", r.offset());
    siz.rsiz = static_cast<std::uint16_t>(r.u16());
    siz.xsiz = r.u32();
    siz.ysiz = r.u32();
    siz.xo = r.u32();
    siz.yo = r.u32();
    siz.xt = r.u32();
    siz.yt = r.u32();
    siz.xto = r.u32();
    siz.yto = r.u32();
    const std::uint32_t csiz = r.u16();
    if (csiz < 1 || csiz > 16384) return malformed("bad Csiz", r.offset());
    if (r.left() != 3u * csiz) return malformed("bad SIZ length", r.offset());
    if (siz.xsiz <= siz.xo || siz.ysiz <= siz.yo) return malformed("empty image area", r.offset());
    if (siz.xt == 0 || siz.yt == 0) return malformed("zero tile size", r.offset());
    if (siz.xto > siz.xo || siz.yto > siz.yo) return malformed("tile offset beyond image offset", r.offset());
    if (std::uint64_t{siz.xto} + siz.xt <= siz.xo || std::uint64_t{siz.yto} + siz.yt <= siz.yo)
        return malformed("first tile outside the image", r.offset());
    siz.ntx = static_cast<std::uint32_t>(ceil_div(std::int64_t{siz.xsiz} - siz.xto, siz.xt));
    siz.nty = static_cast<std::uint32_t>(ceil_div(std::int64_t{siz.ysiz} - siz.yto, siz.yt));
    if (std::uint64_t{siz.ntx} * siz.nty > 65535) return malformed("too many tiles", r.offset());
    siz.comp.resize(csiz);
    for (auto& c : siz.comp) {
        const std::uint32_t s = r.u8();
        c.sgn = (s & 0x80u) != 0;
        c.depth = static_cast<int>(s & 0x7Fu) + 1;
        c.dx = r.u8();
        c.dy = r.u8();
        if (c.depth > 38) return malformed("component depth above 38", r.offset());
        if (c.dx == 0 || c.dy == 0) return malformed("zero component subsampling", r.offset());
    }
    return Status::ok();
}

Status parse_spcod(SegReader& r, bool custom_pp, CodStyle& cs) {
    if (!r.has(5)) return malformed("short COD/COC", r.offset());
    cs.levels = static_cast<int>(r.u8());
    cs.xcb = static_cast<int>(r.u8()) + 2;
    cs.ycb = static_cast<int>(r.u8()) + 2;
    cs.style = static_cast<std::uint8_t>(r.u8());
    cs.transform = static_cast<int>(r.u8());
    if (cs.levels > kMaxLevels) return malformed("more than 32 decomposition levels", r.offset());
    if (cs.xcb > 10 || cs.ycb > 10 || cs.xcb + cs.ycb > 12) return malformed("bad code-block size", r.offset());
    if (cs.transform > 1) return unsupported("Part 2 wavelet kernels", r.offset());
    for (int i = 0; i <= cs.levels; ++i) {
        std::uint32_t v = 0xFF;
        if (custom_pp) {
            if (!r.has(1)) return malformed("short precinct sizes", r.offset());
            v = r.u8();
        }
        cs.ppx[i] = static_cast<std::uint8_t>(v & 15u);
        cs.ppy[i] = static_cast<std::uint8_t>(v >> 4);
        if (i > 0 && (cs.ppx[i] == 0 || cs.ppy[i] == 0)) return malformed("zero precinct size", r.offset());
    }
    return Status::ok();
}

Status parse_quant(SegReader& r, QuantStyle& q) {
    if (!r.has(1)) return malformed("short QCD/QCC", r.offset());
    const std::uint32_t s = r.u8();
    q.style = static_cast<int>(s & 0x1Fu);
    q.guard = static_cast<int>(s >> 5);
    q.n = 0;
    if (q.style == 0) {
        while (r.has(1)) {
            if (q.n >= 3 * kMaxLevels + 1) return malformed("too many quantisation values", r.offset());
            q.exp[q.n] = static_cast<std::uint8_t>(r.u8() >> 3);
            q.mant[q.n] = 0;
            ++q.n;
        }
    } else if (q.style == 1 || q.style == 2) {
        while (r.has(2)) {
            if (q.n >= 3 * kMaxLevels + 1) return malformed("too many quantisation values", r.offset());
            const std::uint32_t v = r.u16();
            q.exp[q.n] = static_cast<std::uint8_t>(v >> 11);
            q.mant[q.n] = static_cast<std::uint16_t>(v & 0x7FFu);
            ++q.n;
        }
        if (q.style == 1 && q.n != 1) return malformed("derived quantisation needs one value", r.offset());
    } else {
        return unsupported("quantisation style", r.offset());
    }
    if (q.n == 0) return malformed("no quantisation values", r.offset());
    return Status::ok();
}

int read_comp_index(SegReader& r, std::size_t csiz) {
    return static_cast<int>(csiz < 257 ? r.u8() : r.u16());
}

// Parses a functional marker segment of the main header or a tile-part header into `p`.
Status parse_functional(std::uint16_t m, SegReader r, const Siz& siz, Params& p) {
    const std::size_t nc = siz.comp.size();
    const std::size_t cbytes = nc < 257 ? 1 : 2;
    switch (m) {
        case kCOD: {
            if (!r.has(5)) return malformed("short COD", r.offset());
            p.scod = static_cast<std::uint8_t>(r.u8());
            p.prog = static_cast<int>(r.u8());
            p.layers = static_cast<int>(r.u16());
            p.mct = static_cast<int>(r.u8());
            if (p.prog > 4) return malformed("bad progression order", r.offset());
            if (p.layers < 1) return malformed("zero layers", r.offset());
            if (p.mct > 1) return unsupported("Part 2 multiple component transform", r.offset());
            if (Status s = parse_spcod(r, (p.scod & 1u) != 0, p.cod); !s) return s;
            p.has_cod = true;
            return Status::ok();
        }
        case kCOC: {
            if (!r.has(cbytes + 1)) return malformed("short COC", r.offset());
            const int c = read_comp_index(r, nc);
            if (static_cast<std::size_t>(c) >= nc) return malformed("COC component out of range", r.offset());
            const std::uint32_t scoc = r.u8();
            CodStyle cs;
            if (Status s = parse_spcod(r, (scoc & 1u) != 0, cs); !s) return s;
            p.has_coc.resize(nc, 0);
            p.coc.resize(nc);
            p.has_coc[static_cast<std::size_t>(c)] = 1;
            p.coc[static_cast<std::size_t>(c)] = cs;
            return Status::ok();
        }
        case kQCD:
            if (Status s = parse_quant(r, p.qcd); !s) return s;
            p.has_qcd = true;
            return Status::ok();
        case kQCC: {
            if (!r.has(cbytes)) return malformed("short QCC", r.offset());
            const int c = read_comp_index(r, nc);
            if (static_cast<std::size_t>(c) >= nc) return malformed("QCC component out of range", r.offset());
            QuantStyle q;
            if (Status s = parse_quant(r, q); !s) return s;
            p.has_qcc.resize(nc, 0);
            p.qcc.resize(nc);
            p.has_qcc[static_cast<std::size_t>(c)] = 1;
            p.qcc[static_cast<std::size_t>(c)] = q;
            return Status::ok();
        }
        case kRGN: {
            if (!r.has(cbytes + 2)) return malformed("short RGN", r.offset());
            const int c = read_comp_index(r, nc);
            if (static_cast<std::size_t>(c) >= nc) return malformed("RGN component out of range", r.offset());
            const std::uint32_t srgn = r.u8();
            const std::uint32_t sp = r.u8();
            if (srgn != 0) return unsupported("ROI style other than Maxshift", r.offset());
            p.rgn.resize(nc, -1);
            p.rgn[static_cast<std::size_t>(c)] = static_cast<int>(sp);
            return Status::ok();
        }
        case kPOC: {
            const std::size_t esz = 5 + 2 * cbytes;
            if (r.left() == 0 || r.left() % esz != 0) return malformed("bad POC length", r.offset());
            while (r.has(esz)) {
                Poc e;
                e.rs = static_cast<int>(r.u8());
                e.cs = read_comp_index(r, nc);
                e.lye = static_cast<int>(r.u16());
                e.re = static_cast<int>(r.u8());
                e.ce = read_comp_index(r, nc);
                if (cbytes == 1 && e.ce == 0) e.ce = 256;
                e.prog = static_cast<int>(r.u8());
                if (e.prog > 4) return malformed("bad POC progression order", r.offset());
                p.poc.push_back(e);
            }
            return Status::ok();
        }
        default:
            return Status::ok();
    }
}

// ---------------------------------------------------------------- codestream structure

struct TileIn {
    std::vector<std::span<const std::uint8_t>> parts;  // data after each SOD
    std::vector<std::uint8_t> ppm;                      // packed packet headers from PPM
    std::vector<std::pair<int, std::span<const std::uint8_t>>> ppt;
    Params params;
    bool any_ppt = false;
    bool has_ppm = false;
    int next_tpsot = 0;
};

struct Codestream {
    Siz siz;
    Params main;
    bool ht = false;
    std::vector<std::unique_ptr<TileIn>> tiles;
    bool truncated = false;
};

// Finds the codestream inside a JP2 file (T.800 I.4), or returns the input when it is raw.
Status locate_codestream(std::span<const std::uint8_t> in, std::span<const std::uint8_t>& cs, bool& wrapped) {
    wrapped = false;
    if (in.size() >= 2 && in[0] == 0xFF && in[1] == 0x4F) {
        cs = in;
        return Status::ok();
    }
    std::size_t pos = 0;
    while (in.size() - pos >= 8) {
        std::uint64_t len = load_be32(&in[pos]);
        const std::uint32_t type = load_be32(&in[pos + 4]);
        std::size_t hdr = 8;
        if (len == 1) {
            if (in.size() - pos < 16) break;
            len = (std::uint64_t{load_be32(&in[pos + 8])} << 32) | load_be32(&in[pos + 12]);
            hdr = 16;
        } else if (len == 0) {
            len = in.size() - pos;
        }
        if (len < hdr || len > in.size() - pos) {
            if (type != 0x6A703263) break;  // a truncated jp2c box is still usable
            len = in.size() - pos;
        }
        if (type == 0x6A703263) {  // 'jp2c'
            cs = in.subspan(pos + hdr, static_cast<std::size_t>(len) - hdr);
            wrapped = true;
            return Status::ok();
        }
        pos += static_cast<std::size_t>(len);
    }
    return malformed("no SOC marker or JP2 codestream box");
}

Status parse_codestream(std::span<const std::uint8_t> in, Codestream& cs) {
    if (in.size() < 4 || load_be16(&in[0]) != kSOC) return malformed("missing SOC");
    if (load_be16(&in[2]) != kSIZ) return malformed("SIZ must follow SOC", 2);
    std::size_t pos = 2;
    bool in_main = true;
    TileIn* cur = nullptr;
    std::vector<std::pair<int, std::vector<std::uint8_t>>> ppm_main;
    std::vector<std::uint8_t> ppm_stream;
    std::size_t ppm_pos = 0;
    bool first_tp_of_tile = false;
    bool seen_siz = false;
    for (;;) {
        if (in.size() - pos < 2) {
            cs.truncated = true;
            break;
        }
        const std::uint16_t m = load_be16(&in[pos]);
        if (m == kEOC) break;
        if ((m >> 8) != 0xFF) return malformed("expected a marker", pos);
        if (m >= 0xFF30 && m <= 0xFF3F) {  // reserved markers without a segment
            pos += 2;
            continue;
        }
        if (m == kSOD) {
            if (in_main || !cur) return malformed("SOD outside a tile-part", pos);
            return malformed("unexpected SOD", pos);
        }
        if (in.size() - pos < 4) {
            cs.truncated = true;
            break;
        }
        const std::size_t len = load_be16(&in[pos + 2]);
        if (len < 2) return malformed("bad marker segment length", pos);
        if (in.size() - pos - 2 < len) {
            if (in_main) return Status(Errc::Truncated, "JPEG 2000: truncated main header", pos);
            cs.truncated = true;
            break;
        }
        const std::span<const std::uint8_t> body = in.subspan(pos + 4, len - 2);
        const std::size_t seg_off = pos;
        pos += 2 + len;
        SegReader r(body, seg_off);
        if (m == kSIZ) {
            if (seen_siz || !in_main) return malformed("second SIZ", seg_off);
            if (Status s = parse_siz(r, cs.siz); !s) return s;
            seen_siz = true;
            cs.tiles.resize(std::size_t{cs.siz.ntx} * cs.siz.nty);
            continue;
        }
        if (!seen_siz) return malformed("SIZ must follow SOC", seg_off);
        if (m == kSOT) {
            if (!r.has(8)) return malformed("short SOT", seg_off);
            const std::uint32_t isot = r.u16();
            const std::uint32_t psot = r.u32();
            const std::uint32_t tpsot = r.u8();
            (void)r.u8();
            if (isot >= cs.tiles.size()) return malformed("tile index out of range", seg_off);
            if (in_main) {
                if (!cs.main.has_cod) return malformed("no COD in the main header", seg_off);
                if (!cs.main.has_qcd) return malformed("no QCD in the main header", seg_off);
                std::sort(ppm_main.begin(), ppm_main.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                for (auto& [z, v] : ppm_main) ppm_stream.insert(ppm_stream.end(), v.begin(), v.end());
                in_main = false;
            }
            auto& slot = cs.tiles[isot];
            if (!slot) slot = std::make_unique<TileIn>();
            cur = slot.get();
            first_tp_of_tile = cur->parts.empty() && cur->next_tpsot == 0;
            if (static_cast<int>(tpsot) != cur->next_tpsot) return malformed("tile-parts out of order", seg_off);
            cur->next_tpsot = static_cast<int>(tpsot) + 1;
            // Tile-part header.
            const std::size_t sot_start = seg_off;
            std::size_t tp_end = psot == 0 ? in.size() : sot_start + psot;
            if (psot != 0 && psot < 14) return malformed("bad Psot", seg_off);
            bool tp_truncated = false;
            if (tp_end > in.size()) {
                tp_end = in.size();
                tp_truncated = true;
            }
            for (;;) {
                if (tp_end - std::min(pos, tp_end) < 2) {
                    cs.truncated = true;
                    return Status::ok();
                }
                const std::uint16_t tm = load_be16(&in[pos]);
                if (tm == kSOD) {
                    pos += 2;
                    break;
                }
                if ((tm >> 8) != 0xFF || tp_end - pos < 4) return malformed("bad tile-part header", pos);
                const std::size_t tl = load_be16(&in[pos + 2]);
                if (tl < 2 || tp_end - pos - 2 < tl) {
                    cs.truncated = true;
                    return Status::ok();
                }
                const std::span<const std::uint8_t> tb = in.subspan(pos + 4, tl - 2);
                SegReader tr(tb, pos);
                const std::size_t toff = pos;
                pos += 2 + tl;
                if (tm == kCOD || tm == kCOC || tm == kQCD || tm == kQCC || tm == kRGN) {
                    if (!first_tp_of_tile) return malformed("functional marker after the first tile-part", toff);
                    if (Status s = parse_functional(tm, tr, cs.siz, cur->params); !s) return s;
                } else if (tm == kPOC) {
                    if (Status s = parse_functional(tm, tr, cs.siz, cur->params); !s) return s;
                } else if (tm == kPPT) {
                    if (!tr.has(1)) return malformed("short PPT", toff);
                    cur->ppt.emplace_back(static_cast<int>(tb[0]), tb.subspan(1));
                    cur->any_ppt = true;
                }
                // PLT, COM and anything else: skipped.
            }
            if (pos > tp_end) return malformed("tile-part header overruns Psot", pos);
            std::size_t data_end = tp_end;
            if (psot == 0) {
                // Last tile-part: data runs to the EOC marker if there is one.
                if (data_end - pos >= 2 && load_be16(&in[data_end - 2]) == kEOC) data_end -= 2;
            }
            cur->parts.push_back(in.subspan(pos, data_end - pos));
            if (!ppm_stream.empty() || ppm_pos > 0) {
                // Nppm bytes of packed headers belong to this tile-part (A.7.4).
                if (ppm_stream.size() - ppm_pos >= 4) {
                    const std::uint32_t n = load_be32(&ppm_stream[ppm_pos]);
                    ppm_pos += 4;
                    const std::size_t take = std::min<std::size_t>(n, ppm_stream.size() - ppm_pos);
                    cur->ppm.insert(cur->ppm.end(), ppm_stream.begin() + static_cast<std::ptrdiff_t>(ppm_pos),
                                    ppm_stream.begin() + static_cast<std::ptrdiff_t>(ppm_pos + take));
                    ppm_pos += take;
                }
                cur->has_ppm = true;
            }
            pos = tp_end;
            if (tp_truncated || psot == 0) {
                if (tp_truncated) cs.truncated = true;
                break;
            }
            continue;
        }
        if (!in_main) return malformed("marker outside a tile-part header", seg_off);
        switch (m) {
            case kCOD: case kCOC: case kQCD: case kQCC: case kRGN: case kPOC:
                if (Status s = parse_functional(m, r, cs.siz, cs.main); !s) return s;
                break;
            case kCAP:
                if (r.has(4)) {
                    const std::uint32_t pcap = r.u32();
                    if (pcap & (1u << 17)) cs.ht = true;
                }
                break;
            case kPPM:
                if (!r.has(1)) return malformed("short PPM", seg_off);
                ppm_main.emplace_back(static_cast<int>(body[0]), std::vector<std::uint8_t>(body.begin() + 1, body.end()));
                break;
            default:
                break;  // TLM, PLM, CRG, COM, PRF, CPF and unknown segments
        }
    }
    if (in_main) return Status(Errc::Truncated, "JPEG 2000: no tile-part", pos);
    if (cs.siz.rsiz & (1u << 14)) cs.ht = true;
    return Status::ok();
}

// ---------------------------------------------------------------- Tier-2 structures

class TagTree {
public:
    void init(std::int64_t w, std::int64_t h) {
        lw_.clear();
        lh_.clear();
        off_.clear();
        std::size_t total = 0;
        std::int64_t cw = std::max<std::int64_t>(w, 0), ch = std::max<std::int64_t>(h, 0);
        if (cw == 0 || ch == 0) {
            val_.clear();
            known_.clear();
            return;
        }
        for (;;) {
            lw_.push_back(cw);
            lh_.push_back(ch);
            off_.push_back(total);
            total += static_cast<std::size_t>(cw * ch);
            if (cw == 1 && ch == 1) break;
            cw = (cw + 1) / 2;
            ch = (ch + 1) / 2;
        }
        val_.assign(total, 0);
        known_.assign(total, 0);
    }
    // Decodes until the value of leaf (x, y) is known to be >= threshold or is known exactly.
    // Returns true if the leaf value is known and below the threshold. Sets `value` when known.
    template <class Reader>
    bool decode(Reader& rd, std::int64_t x, std::int64_t y, int threshold, int& value) {
        std::size_t path[40];
        int depth = 0;
        for (std::size_t l = 0; l < lw_.size(); ++l) {
            path[depth++] = off_[l] + static_cast<std::size_t>(y * lw_[l] + x);
            x >>= 1;
            y >>= 1;
        }
        int low = 0;
        for (int i = depth - 1; i >= 0; --i) {
            const std::size_t n = path[i];
            if (val_[n] < low) val_[n] = low;
            while (!known_[n] && val_[n] < threshold) {
                if (rd.overrun()) return false;
                if (rd.bit()) known_[n] = 1;
                else ++val_[n];
            }
            low = val_[n];
        }
        value = val_[path[0]];
        return known_[path[0]] && val_[path[0]] < threshold;
    }

private:
    std::vector<std::int64_t> lw_, lh_;
    std::vector<std::size_t> off_;
    std::vector<int> val_;
    std::vector<std::uint8_t> known_;
};

class HeaderBits {
public:
    HeaderBits(const std::uint8_t* p, std::size_t n, std::size_t pos) : p_(p), n_(n), pos_(pos) {}
    int bit() {
        if (bits_ == 0) {
            if (pos_ >= n_) {
                over_ = true;
                return 0;
            }
            cur_ = p_[pos_++];
            bits_ = ff_ ? 7 : 8;
            ff_ = cur_ == 0xFF;
        }
        --bits_;
        return static_cast<int>((cur_ >> bits_) & 1u);
    }
    std::uint32_t bits(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | static_cast<std::uint32_t>(bit());
        return v;
    }
    void align() {
        bits_ = 0;
        if (ff_) {
            if (pos_ < n_) ++pos_;
            ff_ = false;
        }
    }
    bool overrun() const { return over_; }
    std::size_t pos() const { return pos_; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t pos_;
    std::uint32_t cur_ = 0;
    int bits_ = 0;
    bool ff_ = false;
    bool over_ = false;
};

struct SegInfo {
    std::uint32_t passes = 0;
    std::uint32_t len = 0;
    bool closed = false;
};

struct Contrib {
    const std::uint8_t* p;
    std::uint32_t len;
};

struct CodeBlock {
    std::int64_t x0, y0, x1, y1;
    int passes = 0;
    int lblock = 3;
    int zbp = 0;
    int p0 = 0;
    bool included = false;
    bool ht = false;
    std::vector<Contrib> contrib;
    std::vector<SegInfo> segs;
};

struct Band {
    Orient o = kLL;
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::int64_t offx = 0, offy = 0;  // Mallat position in the tile-component buffer
    int cbw = 0, cbh = 0;             // code-block exponents
    std::int64_t cbx0 = 0, cby0 = 0, ncbx = 0, ncby = 0;
    int planes = 0;                   // Mb + ROI shift
    float delta = 1.0f;
    std::vector<CodeBlock> cbs;
};

struct PrecBand {
    std::int64_t cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;  // code-block index range (relative to band)
    TagTree incl, zbp;
};

struct Resolution {
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int ppx = 15, ppy = 15;
    std::int64_t npx = 0, npy = 0;
    int nbands = 0;
    Band bands[3];
    std::vector<PrecBand> prec;  // npx * npy * nbands
    std::vector<int> next_layer;
};

struct TileComp {
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    CodStyle cs;
    QuantStyle q;
    int roi = 0;
    std::vector<Resolution> res;
    std::vector<std::int32_t> ibuf;
    std::vector<float> fbuf;
};

int log2_gain(Orient o) { return o == kLL ? 0 : (o == kHH ? 2 : 1); }

bool is_term(const CodeBlock& cb, std::uint8_t style, int i) {
    if (cb.ht) {
        const int j = i - 3 * cb.p0;
        return j >= 0 && (j % 3 == 0 || j % 3 == 2);
    }
    if (style & kCbTermAll) return true;
    if (style & kCbBypass) return i == 9 || (i >= 10 && (i - 10) % 3 != 0);
    return false;
}

// ---------------------------------------------------------------- output sinks

class Sink {
public:
    virtual ~Sink() = default;
    virtual Status begin(const Siz& siz) = 0;
    // Final sample values of component c for tile region (tile reference-grid rectangle tx..,
    // component rectangle cx0..cx1 x cy0..cy1, raster with `stride`).
    virtual void put(std::size_t c, std::int64_t tx0, std::int64_t ty0, std::int64_t tx1, std::int64_t ty1,
                     std::int64_t cx0, std::int64_t cy0, std::int64_t cx1, std::int64_t cy1,
                     const std::int32_t* data, std::size_t stride) = 0;
    std::uint64_t max_samples = std::uint64_t{1} << 28;
};

// ---------------------------------------------------------------- tile decoding

struct Decoder {
    const Codestream& cs;
    Sink& sink;
    std::vector<std::uint32_t> blk_mag, scratch;
    std::vector<std::uint8_t> blk_neg, seg_bytes;

    Status decode_tile(std::size_t t);
};

Status setup_tile(const Codestream& cs, const TileIn* tin, std::size_t t, std::vector<TileComp>& comps,
                  std::int64_t& tx0, std::int64_t& ty0, std::int64_t& tx1, std::int64_t& ty1) {
    const Siz& siz = cs.siz;
    const std::int64_t p = static_cast<std::int64_t>(t % siz.ntx);
    const std::int64_t q = static_cast<std::int64_t>(t / siz.ntx);
    tx0 = std::max<std::int64_t>(std::int64_t{siz.xto} + p * siz.xt, siz.xo);
    ty0 = std::max<std::int64_t>(std::int64_t{siz.yto} + q * siz.yt, siz.yo);
    tx1 = std::min<std::int64_t>(std::int64_t{siz.xto} + (p + 1) * siz.xt, siz.xsiz);
    ty1 = std::min<std::int64_t>(std::int64_t{siz.yto} + (q + 1) * siz.yt, siz.ysiz);
    const Params& mp = cs.main;
    const Params* tp = tin ? &tin->params : nullptr;
    comps.resize(siz.comp.size());
    for (std::size_t c = 0; c < comps.size(); ++c) {
        TileComp& tc = comps[c];
        const CompSiz& sc = siz.comp[c];
        tc.x0 = ceil_div(tx0, sc.dx);
        tc.y0 = ceil_div(ty0, sc.dy);
        tc.x1 = ceil_div(tx1, sc.dx);
        tc.y1 = ceil_div(ty1, sc.dy);
        if (tp && c < tp->has_coc.size() && tp->has_coc[c]) tc.cs = tp->coc[c];
        else if (tp && tp->has_cod) tc.cs = tp->cod;
        else if (c < mp.has_coc.size() && mp.has_coc[c]) tc.cs = mp.coc[c];
        else tc.cs = mp.cod;
        if (tp && c < tp->has_qcc.size() && tp->has_qcc[c]) tc.q = tp->qcc[c];
        else if (tp && tp->has_qcd) tc.q = tp->qcd;
        else if (c < mp.has_qcc.size() && mp.has_qcc[c]) tc.q = mp.qcc[c];
        else tc.q = mp.qcd;
        tc.roi = 0;
        if (tp && c < tp->rgn.size() && tp->rgn[c] >= 0) tc.roi = tp->rgn[c];
        else if (c < mp.rgn.size() && mp.rgn[c] >= 0) tc.roi = mp.rgn[c];

        const CodStyle& st = tc.cs;
        const int nl = st.levels;
        if ((st.style & kCbHT) && !cs.ht) {
            // Tolerated: HT code-blocks signalled only in COD.
        }
        const QuantStyle& qs = tc.q;
        const bool needs_all = qs.style != 1;
        if (needs_all && qs.n < 3 * nl + 1) return malformed("too few quantisation values for the levels");
        const std::int64_t w = tc.x1 - tc.x0, h = tc.y1 - tc.y0;
        if (w < 0 || h < 0) return malformed("bad tile-component size");
        tc.res.assign(static_cast<std::size_t>(nl) + 1, Resolution{});
        for (int r = 0; r <= nl; ++r) {
            Resolution& rs = tc.res[static_cast<std::size_t>(r)];
            const int sh = nl - r;
            rs.x0 = ceil_shift(tc.x0, sh);
            rs.y0 = ceil_shift(tc.y0, sh);
            rs.x1 = ceil_shift(tc.x1, sh);
            rs.y1 = ceil_shift(tc.y1, sh);
            rs.ppx = st.ppx[r];
            rs.ppy = st.ppy[r];
            if (rs.x1 > rs.x0 && rs.y1 > rs.y0) {
                rs.npx = ceil_shift(rs.x1, rs.ppx) - (rs.x0 >> rs.ppx);
                rs.npy = ceil_shift(rs.y1, rs.ppy) - (rs.y0 >> rs.ppy);
            }
            rs.nbands = r == 0 ? 1 : 3;
            const int nb = r == 0 ? nl : nl - r + 1;
            const int pbx = r == 0 ? rs.ppx : rs.ppx - 1;
            const int pby = r == 0 ? rs.ppy : rs.ppy - 1;
            const int cbw = std::min(st.xcb, pbx);
            const int cbh = std::min(st.ycb, pby);
            for (int b = 0; b < rs.nbands; ++b) {
                Band& bd = rs.bands[b];
                bd.o = r == 0 ? kLL : static_cast<Orient>(b + 1);
                const std::int64_t xob = (bd.o == kHL || bd.o == kHH) ? 1 : 0;
                const std::int64_t yob = (bd.o == kLH || bd.o == kHH) ? 1 : 0;
                if (r == 0) {
                    bd.x0 = rs.x0; bd.y0 = rs.y0; bd.x1 = rs.x1; bd.y1 = rs.y1;
                } else {
                    const std::int64_t d = std::int64_t{1} << nb;
                    const std::int64_t hf = std::int64_t{1} << (nb - 1);
                    bd.x0 = ceil_div_s(tc.x0 - hf * xob, d);
                    bd.y0 = ceil_div_s(tc.y0 - hf * yob, d);
                    bd.x1 = ceil_div_s(tc.x1 - hf * xob, d);
                    bd.y1 = ceil_div_s(tc.y1 - hf * yob, d);
                }
                if (r > 0) {
                    const Resolution& lo = tc.res[static_cast<std::size_t>(r) - 1];
                    bd.offx = xob ? lo.x1 - lo.x0 : 0;
                    bd.offy = yob ? lo.y1 - lo.y0 : 0;
                    const std::int64_t bw = bd.x1 - bd.x0, bh = bd.y1 - bd.y0;
                    if (bd.offx + bw > rs.x1 - rs.x0 || bd.offy + bh > rs.y1 - rs.y0)
                        return Status(Errc::Internal, "JPEG 2000: sub-band layout mismatch");
                }
                // Quantisation (E.1).
                const int idx = r == 0 ? 0 : 1 + 3 * (r - 1) + (b);
                int expn = 0;
                std::uint32_t mant = 0;
                if (qs.style == 1) {
                    expn = qs.exp[0] - nl + nb;
                    mant = qs.mant[0];
                    if (r == 0) expn = qs.exp[0];
                } else {
                    expn = qs.exp[idx];
                    mant = qs.mant[idx];
                }
                if (expn < 0) return malformed("negative quantisation exponent");
                bd.planes = qs.guard + expn - 1 + tc.roi;
                if (bd.planes > kMaxCodedPlanes)
                    return unsupported("more than 31 coded bit-planes (sample depth or ROI shift too large)");
                const int rb = siz.comp[c].depth + log2_gain(bd.o);
                bd.delta = std::ldexp(1.0f + static_cast<float>(mant) / 2048.0f, rb - expn);
                bd.cbw = cbw;
                bd.cbh = cbh;
                if (bd.x1 > bd.x0 && bd.y1 > bd.y0) {
                    bd.cbx0 = bd.x0 >> cbw;
                    bd.cby0 = bd.y0 >> cbh;
                    bd.ncbx = ceil_shift(bd.x1, cbw) - bd.cbx0;
                    bd.ncby = ceil_shift(bd.y1, cbh) - bd.cby0;
                    bd.cbs.resize(static_cast<std::size_t>(bd.ncbx * bd.ncby));
                    for (std::int64_t j = 0; j < bd.ncby; ++j) {
                        for (std::int64_t i = 0; i < bd.ncbx; ++i) {
                            CodeBlock& cb = bd.cbs[static_cast<std::size_t>(j * bd.ncbx + i)];
                            cb.x0 = std::max(bd.x0, (bd.cbx0 + i) << cbw);
                            cb.y0 = std::max(bd.y0, (bd.cby0 + j) << cbh);
                            cb.x1 = std::min(bd.x1, (bd.cbx0 + i + 1) << cbw);
                            cb.y1 = std::min(bd.y1, (bd.cby0 + j + 1) << cbh);
                        }
                    }
                }
            }
            // Precincts.
            const std::size_t np = static_cast<std::size_t>(rs.npx * rs.npy);
            rs.prec.resize(np * static_cast<std::size_t>(rs.nbands));
            rs.next_layer.assign(np, 0);
            for (std::int64_t ky = 0; ky < rs.npy; ++ky) {
                for (std::int64_t kx = 0; kx < rs.npx; ++kx) {
                    const std::int64_t px = (rs.x0 >> rs.ppx) + kx;
                    const std::int64_t py = (rs.y0 >> rs.ppy) + ky;
                    for (int b = 0; b < rs.nbands; ++b) {
                        const Band& bd = rs.bands[b];
                        PrecBand& pb = rs.prec[static_cast<std::size_t>((ky * rs.npx + kx) * rs.nbands + b)];
                        if (bd.cbs.empty()) continue;
                        const int sx = pbx - bd.cbw, sy = pby - bd.cbh;
                        pb.cx0 = std::max<std::int64_t>(px << sx, bd.cbx0) - bd.cbx0;
                        pb.cx1 = std::min<std::int64_t>((px + 1) << sx, bd.cbx0 + bd.ncbx) - bd.cbx0;
                        pb.cy0 = std::max<std::int64_t>(py << sy, bd.cby0) - bd.cby0;
                        pb.cy1 = std::min<std::int64_t>((py + 1) << sy, bd.cby0 + bd.ncby) - bd.cby0;
                        if (pb.cx1 < pb.cx0) pb.cx1 = pb.cx0;
                        if (pb.cy1 < pb.cy0) pb.cy1 = pb.cy0;
                        pb.incl.init(pb.cx1 - pb.cx0, pb.cy1 - pb.cy0);
                        pb.zbp.init(pb.cx1 - pb.cx0, pb.cy1 - pb.cy0);
                    }
                }
            }
        }
    }
    return Status::ok();
}

struct PacketStreams {
    const std::uint8_t* body = nullptr;
    std::size_t blen = 0, bpos = 0;
    const std::uint8_t* hdr = nullptr;
    std::size_t hlen = 0, hpos = 0;
    bool separate = false;
    bool sop = false, eph = false;
    bool done = false;  // data exhausted or corrupt: stop reading packets
};

struct Pending {
    CodeBlock* cb;
    int npasses;
    std::size_t first;  // index into lens
    std::size_t count;
    std::uint8_t style;
};

// Reads one packet (B.9, B.10). Returns false when no further packets can be read.
bool read_packet(PacketStreams& ps, TileComp& tc, Resolution& rs, std::size_t k, int layer,
                 std::vector<Pending>& pend, std::vector<std::uint32_t>& lens, std::vector<int>& piece_passes) {
    if (ps.sop && ps.blen - ps.bpos >= 6 && ps.body[ps.bpos] == 0xFF && ps.body[ps.bpos + 1] == 0x91) {
        ps.bpos += 6;
        if (!ps.separate) ps.hpos = ps.bpos;
    }
    if (ps.hpos >= ps.hlen) return false;
    HeaderBits hb(ps.hdr, ps.hlen, ps.hpos);
    pend.clear();
    lens.clear();
    piece_passes.clear();
    const std::uint8_t style = tc.cs.style;
    if (hb.bit()) {
        for (int b = 0; b < rs.nbands; ++b) {
            Band& bd = rs.bands[b];
            PrecBand& pb = rs.prec[k * static_cast<std::size_t>(rs.nbands) + static_cast<std::size_t>(b)];
            for (std::int64_t cy = pb.cy0; cy < pb.cy1; ++cy) {
                for (std::int64_t cx = pb.cx0; cx < pb.cx1; ++cx) {
                    CodeBlock& cb = bd.cbs[static_cast<std::size_t>(cy * bd.ncbx + cx)];
                    bool inc;
                    bool first = false;
                    if (cb.included) {
                        inc = hb.bit() != 0;
                    } else {
                        int v = 0;
                        inc = pb.incl.decode(hb, cx - pb.cx0, cy - pb.cy0, layer + 1, v);
                        first = inc;
                    }
                    if (hb.overrun()) return false;
                    if (!inc) continue;
                    if (first) {
                        int z = 0;
                        if (!pb.zbp.decode(hb, cx - pb.cx0, cy - pb.cy0, 64, z)) return false;
                        cb.zbp = z;
                        cb.included = true;
                        if (z > bd.planes) return false;
                    }
                    // Number of coding passes (table B.4).
                    int n = 1;
                    if (hb.bit()) {
                        n = 2;
                        if (hb.bit()) {
                            const int v2 = static_cast<int>(hb.bits(2));
                            if (v2 < 3) {
                                n = 3 + v2;
                            } else {
                                const int v5 = static_cast<int>(hb.bits(5));
                                n = v5 < 31 ? 6 + v5 : 37 + static_cast<int>(hb.bits(7));
                            }
                        }
                    }
                    while (hb.bit()) {
                        if (++cb.lblock > 32 || hb.overrun()) return false;
                    }
                    if (cb.passes == 0) {
                        cb.ht = (style & kCbHT) != 0;
                        if (cb.ht && (style & kCbHTMixed)) cb.ht = cb.lblock > 3;  // T.814 A.4
                        if (cb.ht) cb.p0 = (n - 1) / 3;
                    }
                    const int maxp = cb.ht ? 3 * cb.p0 + 3 * (bd.planes - cb.zbp) + 3 : 3 * (bd.planes - cb.zbp) - 2;
                    if (cb.passes + n > maxp) return false;
                    Pending pd{&cb, n, lens.size(), 0, style};
                    int start = cb.passes;
                    for (int i = cb.passes; i < cb.passes + n; ++i) {
                        if (is_term(cb, style, i) || i == cb.passes + n - 1) {
                            const int pc = i - start + 1;
                            const int nbits = cb.lblock + static_cast<int>(std::bit_width(static_cast<unsigned>(pc))) - 1;
                            if (nbits > 32) return false;
                            lens.push_back(hb.bits(nbits));
                            piece_passes.push_back(pc);
                            ++pd.count;
                            start = i + 1;
                        }
                    }
                    if (hb.overrun()) return false;
                    pend.push_back(pd);
                    cb.passes += n;
                }
            }
        }
    }
    hb.align();
    if (hb.overrun()) return false;
    ps.hpos = hb.pos();
    if (ps.eph && ps.hlen - ps.hpos >= 2 && ps.hdr[ps.hpos] == 0xFF && ps.hdr[ps.hpos + 1] == 0x92) ps.hpos += 2;
    if (!ps.separate) ps.bpos = ps.hpos;
    // Packet body.
    bool trunc = false;
    for (const Pending& pd : pend) {
        CodeBlock& cb = *pd.cb;
        const std::uint8_t* start = ps.body + ps.bpos;
        std::uint32_t total = 0;
        int pass = cb.passes - pd.npasses;
        for (std::size_t i = 0; i < pd.count; ++i) {
            const std::uint32_t want = lens[pd.first + i];
            const int pc = piece_passes[pd.first + i];
            const std::size_t avail = ps.blen - ps.bpos;
            const std::uint32_t take = trunc ? 0 : static_cast<std::uint32_t>(std::min<std::size_t>(want, avail));
            if (take < want) trunc = true;
            ps.bpos += take;
            total += take;
            pass += pc;
            if (cb.segs.empty() || cb.segs.back().closed) cb.segs.push_back(SegInfo{});
            SegInfo& sg = cb.segs.back();
            sg.passes += static_cast<std::uint32_t>(pc);
            sg.len += take;
            sg.closed = is_term(cb, pd.style, pass - 1) || take < want;
        }
        if (total > 0) cb.contrib.push_back(Contrib{start, total});
    }
    if (!ps.separate) ps.hpos = ps.bpos;
    return !trunc;
}

// Runs the packet sequence of a tile (B.12), calling read_packet for each packet.
void read_packets(const Codestream& cs, const Params* tp, std::vector<TileComp>& comps, PacketStreams& ps,
                  std::int64_t tx0, std::int64_t ty0, std::int64_t tx1, std::int64_t ty1) {
    const Params& mp = cs.main;
    const int layers = (tp && tp->has_cod) ? tp->layers : mp.layers;
    const int prog = (tp && tp->has_cod) ? tp->prog : mp.prog;
    std::vector<Poc> vols = (tp && !tp->poc.empty()) ? tp->poc : mp.poc;
    const int nc = static_cast<int>(comps.size());
    int maxres = 0;
    for (const auto& tc : comps) maxres = std::max(maxres, static_cast<int>(tc.res.size()));
    if (vols.empty()) vols.push_back(Poc{0, maxres, 0, nc, layers, prog});
    std::vector<Pending> pend;
    std::vector<std::uint32_t> lens;
    std::vector<int> pieces;

    auto emit = [&](int l, int r, int c, std::int64_t k) {
        if (ps.done) return;
        TileComp& tc = comps[static_cast<std::size_t>(c)];
        if (r >= static_cast<int>(tc.res.size())) return;
        Resolution& rs = tc.res[static_cast<std::size_t>(r)];
        if (k < 0 || k >= rs.npx * rs.npy) return;
        int& nx = rs.next_layer[static_cast<std::size_t>(k)];
        if (nx != l) return;
        ++nx;
        if (!read_packet(ps, tc, rs, static_cast<std::size_t>(k), l, pend, lens, pieces)) ps.done = true;
    };
    // Precinct of (c, r) whose packets start at reference-grid position (x, y), or -1 (B.12.1.3).
    auto prec_at = [&](int c, int r, std::int64_t x, std::int64_t y) -> std::int64_t {
        TileComp& tc = comps[static_cast<std::size_t>(c)];
        if (r >= static_cast<int>(tc.res.size())) return -1;
        const Resolution& rs = tc.res[static_cast<std::size_t>(r)];
        if (rs.npx == 0 || rs.npy == 0) return -1;
        const int rr = static_cast<int>(tc.res.size()) - 1 - r;
        const CompSiz& sc = cs.siz.comp[static_cast<std::size_t>(c)];
        const std::int64_t ys = std::int64_t{sc.dy} << (rs.ppy + rr);
        const std::int64_t xs = std::int64_t{sc.dx} << (rs.ppx + rr);
        const bool cy = (y % ys == 0) || (y == ty0 && (rs.y0 & ((std::int64_t{1} << rs.ppy) - 1)) != 0);
        const bool cx = (x % xs == 0) || (x == tx0 && (rs.x0 & ((std::int64_t{1} << rs.ppx) - 1)) != 0);
        if (!cx || !cy) return -1;
        const std::int64_t kx = (ceil_div(x, std::int64_t{sc.dx} << rr) >> rs.ppx) - (rs.x0 >> rs.ppx);
        const std::int64_t ky = (ceil_div(y, std::int64_t{sc.dy} << rr) >> rs.ppy) - (rs.y0 >> rs.ppy);
        if (kx < 0 || ky < 0 || kx >= rs.npx || ky >= rs.npy) return -1;
        return ky * rs.npx + kx;
    };

    for (const Poc& v : vols) {
        if (ps.done) break;
        const int rs0 = v.rs, re = std::min(v.re, maxres);
        const int cs0 = v.cs, ce = std::min(v.ce, nc);
        const int lye = std::min(v.lye, layers);
        if (rs0 >= re || cs0 >= ce || lye <= 0) continue;
        auto nprec = [&](int c, int r) -> std::int64_t {
            const TileComp& tc = comps[static_cast<std::size_t>(c)];
            if (r >= static_cast<int>(tc.res.size())) return 0;
            const Resolution& rr = tc.res[static_cast<std::size_t>(r)];
            return rr.npx * rr.npy;
        };
        if (v.prog == 0) {
            for (int l = 0; l < lye && !ps.done; ++l)
                for (int r = rs0; r < re && !ps.done; ++r)
                    for (int c = cs0; c < ce && !ps.done; ++c)
                        for (std::int64_t k = 0; k < nprec(c, r) && !ps.done; ++k) emit(l, r, c, k);
            continue;
        }
        if (v.prog == 1) {
            for (int r = rs0; r < re && !ps.done; ++r)
                for (int l = 0; l < lye && !ps.done; ++l)
                    for (int c = cs0; c < ce && !ps.done; ++c)
                        for (std::int64_t k = 0; k < nprec(c, r) && !ps.done; ++k) emit(l, r, c, k);
            continue;
        }
        // Position-driven orders: step through the reference grid by the gcd of all precinct sizes.
        std::int64_t dx = 0, dy = 0;
        for (int c = cs0; c < ce; ++c) {
            const TileComp& tc = comps[static_cast<std::size_t>(c)];
            for (int r = rs0; r < std::min(re, static_cast<int>(tc.res.size())); ++r) {
                const Resolution& rr = tc.res[static_cast<std::size_t>(r)];
                if (rr.npx == 0 || rr.npy == 0) continue;
                const int lev = static_cast<int>(tc.res.size()) - 1 - r;
                const CompSiz& sc = cs.siz.comp[static_cast<std::size_t>(c)];
                dx = std::gcd(dx, std::int64_t{sc.dx} << (rr.ppx + lev));
                dy = std::gcd(dy, std::int64_t{sc.dy} << (rr.ppy + lev));
            }
        }
        if (dx == 0 || dy == 0) continue;
        auto next = [](std::int64_t p, std::int64_t d) { return (p / d + 1) * d; };
        auto layers_at = [&](int c, int r, std::int64_t x, std::int64_t y) {
            const std::int64_t k = prec_at(c, r, x, y);
            if (k < 0) return;
            for (int l = 0; l < lye && !ps.done; ++l) emit(l, r, c, k);
        };
        if (v.prog == 2) {
            for (int r = rs0; r < re && !ps.done; ++r)
                for (std::int64_t y = ty0; y < ty1 && !ps.done; y = next(y, dy))
                    for (std::int64_t x = tx0; x < tx1 && !ps.done; x = next(x, dx))
                        for (int c = cs0; c < ce && !ps.done; ++c) layers_at(c, r, x, y);
        } else if (v.prog == 3) {
            for (std::int64_t y = ty0; y < ty1 && !ps.done; y = next(y, dy))
                for (std::int64_t x = tx0; x < tx1 && !ps.done; x = next(x, dx))
                    for (int c = cs0; c < ce && !ps.done; ++c)
                        for (int r = rs0; r < re && !ps.done; ++r) layers_at(c, r, x, y);
        } else {
            for (int c = cs0; c < ce && !ps.done; ++c)
                for (std::int64_t y = ty0; y < ty1 && !ps.done; y = next(y, dy))
                    for (std::int64_t x = tx0; x < tx1 && !ps.done; x = next(x, dx))
                        for (int r = rs0; r < re && !ps.done; ++r) layers_at(c, r, x, y);
        }
    }
}

// ROI Maxshift realignment (T.800 H.1) on a half-bit-form magnitude.
inline std::uint32_t roi_adjust(std::uint32_t v, int s) {
    if (v == 0) return 0;
    const int p = std::countr_zero(v);
    const std::uint32_t q = (v - (1u << p)) >> 1;
    if (q < (1u << s)) return v;  // background coefficient
    const int pn = p > s ? p - s : 0;
    return ((q >> s) << 1) | (1u << pn);
}

Status Decoder::decode_tile(std::size_t t) {
    const TileIn* tin = cs.tiles[t].get();
    std::vector<TileComp> comps;
    std::int64_t tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    if (Status s = setup_tile(cs, tin, t, comps, tx0, ty0, tx1, ty1); !s) return s;
    const Params* tp = tin ? &tin->params : nullptr;

    // Packet data.
    std::vector<std::uint8_t> body_buf, hdr_buf;
    if (tin) {
        if (tin->parts.size() == 1) {
            // Use the codestream bytes directly.
        } else {
            for (const auto& p : tin->parts) body_buf.insert(body_buf.end(), p.begin(), p.end());
        }
        PacketStreams ps;
        if (tin->parts.size() == 1) {
            ps.body = tin->parts[0].data();
            ps.blen = tin->parts[0].size();
        } else {
            ps.body = body_buf.data();
            ps.blen = body_buf.size();
        }
        if (tin->has_ppm) {
            ps.hdr = tin->ppm.data();
            ps.hlen = tin->ppm.size();
            ps.separate = true;
        } else if (tin->any_ppt) {
            auto ppt = tin->ppt;
            std::stable_sort(ppt.begin(), ppt.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [z, d] : ppt) hdr_buf.insert(hdr_buf.end(), d.begin(), d.end());
            ps.hdr = hdr_buf.data();
            ps.hlen = hdr_buf.size();
            ps.separate = true;
        } else {
            ps.hdr = ps.body;
            ps.hlen = ps.blen;
        }
        const std::uint8_t scod = (tp && tp->has_cod) ? tp->scod : cs.main.scod;
        ps.sop = (scod & 2u) != 0;
        ps.eph = (scod & 4u) != 0;
        if (ps.body != nullptr || ps.blen == 0) read_packets(cs, tp, comps, ps, tx0, ty0, tx1, ty1);
    }

    // Block decoding and dequantisation into each tile-component buffer.
    const int mct = (tp && tp->has_cod) ? tp->mct : cs.main.mct;
    std::vector<std::int32_t> outbuf;
    for (std::size_t c = 0; c < comps.size(); ++c) {
        TileComp& tc = comps[c];
        const std::size_t w = static_cast<std::size_t>(tc.x1 - tc.x0);
        const std::size_t h = static_cast<std::size_t>(tc.y1 - tc.y0);
        const bool rev = tc.cs.transform == 1;
        if (rev) tc.ibuf.assign(w * h, 0);
        else tc.fbuf.assign(w * h, 0.0f);
        for (auto& rs : tc.res) {
            for (int b = 0; b < rs.nbands; ++b) {
                Band& bd = rs.bands[b];
                for (CodeBlock& cb : bd.cbs) {
                    if (cb.segs.empty()) continue;
                    const int bw = static_cast<int>(cb.x1 - cb.x0);
                    const int bh = static_cast<int>(cb.y1 - cb.y0);
                    // Gather segment bytes.
                    std::vector<Segment> segs;
                    segs.reserve(cb.segs.size());
                    const std::uint8_t* base = nullptr;
                    if (cb.contrib.size() == 1) {
                        base = cb.contrib[0].p;
                    } else {
                        seg_bytes.clear();
                        for (const Contrib& ct : cb.contrib) seg_bytes.insert(seg_bytes.end(), ct.p, ct.p + ct.len);
                        base = seg_bytes.data();
                    }
                    std::size_t off = 0;
                    for (const SegInfo& sg : cb.segs) {
                        segs.push_back(Segment{base ? base + off : nullptr, sg.len, sg.passes});
                        off += sg.len;
                    }
                    BlockParams bp;
                    bp.width = bw;
                    bp.height = bh;
                    bp.orient = bd.o;
                    bp.style = tc.cs.style;
                    bp.planes = bd.planes;
                    bp.zero_planes = cb.zbp;
                    bp.ht_p0 = cb.p0;
                    const std::size_t n = static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh);
                    if (blk_mag.size() < n) {
                        blk_mag.resize(n);
                        blk_neg.resize(n);
                    }
                    const bool ok = cb.ht ? decode_ht_block(bp, segs, blk_mag.data(), blk_neg.data(), static_cast<std::size_t>(bw), scratch)
                                          : decode_t1_block(bp, segs, blk_mag.data(), blk_neg.data(), static_cast<std::size_t>(bw), scratch);
                    if (!ok) continue;
                    const std::size_t ox = static_cast<std::size_t>(bd.offx + (cb.x0 - bd.x0));
                    const std::size_t oy = static_cast<std::size_t>(bd.offy + (cb.y0 - bd.y0));
                    const float scale = bd.delta * 0.5f;
                    for (int y = 0; y < bh; ++y) {
                        const std::uint32_t* mp = blk_mag.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bw);
                        const std::uint8_t* np = blk_neg.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bw);
                        const std::size_t row = (oy + static_cast<std::size_t>(y)) * w + ox;
                        for (int x = 0; x < bw; ++x) {
                            std::uint32_t v = mp[x];
                            if (tc.roi) v = roi_adjust(v, tc.roi);
                            if (rev) {
                                const auto m = static_cast<std::int32_t>(v >> 1);
                                tc.ibuf[row + static_cast<std::size_t>(x)] = np[x] ? -m : m;
                            } else {
                                const float m = static_cast<float>(v) * scale;
                                tc.fbuf[row + static_cast<std::size_t>(x)] = np[x] ? -m : m;
                            }
                        }
                    }
                }
            }
        }
        // Inverse wavelet transform.
        std::vector<ResGeom> geo;
        for (const auto& rs : tc.res) geo.push_back(ResGeom{rs.x0, rs.y0, rs.x1, rs.y1});
        if (w > 0 && h > 0) {
            if (rev) idwt53(tc.ibuf.data(), w, geo);
            else idwt97(tc.fbuf.data(), w, geo);
        }
    }

    // Inverse multiple component transform (G.2, G.3).
    if (mct && comps.size() >= 3) {
        const TileComp &c0 = comps[0], &c1 = comps[1], &c2 = comps[2];
        if (c0.x0 != c1.x0 || c0.x0 != c2.x0 || c0.x1 != c1.x1 || c0.x1 != c2.x1 || c0.y0 != c1.y0 ||
            c0.y0 != c2.y0 || c0.y1 != c1.y1 || c0.y1 != c2.y1)
            return malformed("colour transform on components of different sizes");
        const int t0 = c0.cs.transform;
        if (c1.cs.transform != t0 || c2.cs.transform != t0) return malformed("colour transform with mixed wavelets");
        if (t0 == 1) {
            std::int32_t *y0 = comps[0].ibuf.data(), *y1 = comps[1].ibuf.data(), *y2 = comps[2].ibuf.data();
            for (std::size_t i = 0, n = comps[0].ibuf.size(); i < n; ++i) {
                const std::int64_t g = std::int64_t{y0[i]} - ((std::int64_t{y1[i]} + y2[i]) >> 2);
                const std::int64_t rr = y2[i] + g;
                const std::int64_t bb = y1[i] + g;
                y0[i] = static_cast<std::int32_t>(rr);
                y1[i] = static_cast<std::int32_t>(g);
                y2[i] = static_cast<std::int32_t>(bb);
            }
        } else {
            float *y0 = comps[0].fbuf.data(), *y1 = comps[1].fbuf.data(), *y2 = comps[2].fbuf.data();
            for (std::size_t i = 0, n = comps[0].fbuf.size(); i < n; ++i) {
                const float yy = y0[i], cb = y1[i], cr = y2[i];
                y0[i] = yy + 1.402f * cr;
                y1[i] = yy - 0.34413f * cb - 0.71414f * cr;
                y2[i] = yy + 1.772f * cb;
            }
        }
    }

    // DC level shift, clamping, output.
    for (std::size_t c = 0; c < comps.size(); ++c) {
        TileComp& tc = comps[c];
        const CompSiz& sc = cs.siz.comp[c];
        const std::size_t w = static_cast<std::size_t>(tc.x1 - tc.x0);
        const std::size_t h = static_cast<std::size_t>(tc.y1 - tc.y0);
        if (w == 0 || h == 0) continue;
        const int depth = std::min(sc.depth, 31);
        const std::int64_t lo = sc.sgn ? -(std::int64_t{1} << (depth - 1)) : 0;
        const std::int64_t hi = sc.sgn ? (std::int64_t{1} << (depth - 1)) - 1 : (std::int64_t{1} << depth) - 1;
        const std::int64_t shift = sc.sgn ? 0 : (std::int64_t{1} << (depth - 1));
        const std::int32_t* src;
        if (tc.cs.transform == 1) {
            std::int32_t* d = tc.ibuf.data();
            for (std::size_t i = 0, n = w * h; i < n; ++i)
                d[i] = static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{d[i]} + shift, lo, hi));
            src = d;
        } else {
            outbuf.resize(w * h);
            const float flo = static_cast<float>(lo), fhi = static_cast<float>(hi);
            const float fs = static_cast<float>(shift);
            const float* f = tc.fbuf.data();
            for (std::size_t i = 0, n = w * h; i < n; ++i) {
                float v = f[i] + fs;
                if (!(v >= flo)) v = flo;
                if (!(v <= fhi)) v = fhi;
                outbuf[i] = static_cast<std::int32_t>(std::lrint(v));
            }
            src = outbuf.data();
        }
        sink.put(c, tx0, ty0, tx1, ty1, tc.x0, tc.y0, tc.x1, tc.y1, src, w);
        tc.ibuf = {};
        tc.fbuf = {};
    }
    return Status::ok();
}

Status run_decoder(std::span<const std::uint8_t> in, Sink& sink) {
    std::span<const std::uint8_t> stream;
    bool wrapped = false;
    if (Status s = locate_codestream(in, stream, wrapped); !s) return s;
    Codestream cs;
    if (Status s = parse_codestream(stream, cs); !s) return s;
    if (Status s = sink.begin(cs.siz); !s) return s;
    // Memory bound: every tile-component buffer is at most the component size.
    std::uint64_t total = 0;
    for (const auto& c : cs.siz.comp) {
        const std::uint64_t w = static_cast<std::uint64_t>(ceil_div(cs.siz.xsiz, c.dx) - ceil_div(cs.siz.xo, c.dx));
        const std::uint64_t h = static_cast<std::uint64_t>(ceil_div(cs.siz.ysiz, c.dy) - ceil_div(cs.siz.yo, c.dy));
        std::uint64_t n = 0;
        if (!checked_mul(w, h, n) || !checked_add(total, n, total)) return Status(Errc::LimitExceeded, "JPEG 2000: image too large");
    }
    if (total > sink.max_samples) return Status(Errc::LimitExceeded, "JPEG 2000: image exceeds the sample limit");
    Decoder dec{cs, sink, {}, {}, {}, {}};
    // Tiles without tile-parts (absent from a sparse or truncated codestream) decode as empty
    // tiles: all coefficients zero, like a tile whose packets were all discarded (B.11).
    for (std::size_t t = 0; t < cs.tiles.size(); ++t) {
        if (Status s = dec.decode_tile(t); !s) return s;
    }
    return Status::ok();
}

class FrameSink : public Sink {
public:
    FrameSink(const FrameInfo& info, std::span<std::uint8_t> out) : info_(info), out_(out) {}
    Status begin(const Siz& siz) override {
        siz_ = &siz;
        if (siz.xsiz - siz.xo != info_.columns || siz.ysiz - siz.yo != info_.rows)
            return malformed("image size differs from Rows/Columns");
        if (siz.comp.size() != info_.samples_per_pixel) return malformed("component count differs from SamplesPerPixel");
        if (info_.bits_allocated != 8 && info_.bits_allocated != 16 && info_.bits_allocated != 32)
            return Status(Errc::InvalidArgument, "JPEG 2000: BitsAllocated must be 8, 16 or 32");
        for (const auto& c : siz.comp) {
            if (c.depth > 16) return unsupported("sample precision above 16 bits");
            if (c.depth != info_.bits_stored) return malformed("component precision differs from BitsStored");
            if (c.depth > info_.bits_allocated) return malformed("component precision exceeds BitsAllocated");
            if (c.sgn != info_.is_signed) return malformed("component signedness differs from PixelRepresentation");
        }
        if (info_.frame_bytes() != out_.size()) return Status(Errc::BufferTooSmall, "JPEG 2000: output buffer size");
        max_samples = static_cast<std::uint64_t>(info_.rows) * info_.columns * info_.samples_per_pixel;
        return Status::ok();
    }
    void put(std::size_t c, std::int64_t tx0, std::int64_t ty0, std::int64_t tx1, std::int64_t ty1, std::int64_t cx0,
             std::int64_t cy0, std::int64_t cx1, std::int64_t cy1, const std::int32_t* data, std::size_t stride) override {
        const CompSiz& sc = siz_->comp[c];
        const std::size_t bps = info_.bits_allocated / 8u;
        const std::size_t nc = info_.samples_per_pixel;
        const std::size_t cols = info_.columns;
        for (std::int64_t y = ty0; y < ty1; ++y) {
            const std::int64_t v = std::clamp<std::int64_t>(y / sc.dy, cy0, cy1 - 1) - cy0;
            const std::int32_t* srow = data + static_cast<std::size_t>(v) * stride;
            const std::size_t oy = static_cast<std::size_t>(y - siz_->yo);
            for (std::int64_t x = tx0; x < tx1; ++x) {
                const std::int64_t u = (sc.dx == 1 ? x : std::clamp<std::int64_t>(x / sc.dx, cx0, cx1 - 1)) - cx0;
                const std::int32_t s = srow[u];
                const std::size_t ox = static_cast<std::size_t>(x - siz_->xo);
                std::size_t idx;
                if (info_.planar) idx = (c * info_.rows + oy) * cols + ox;
                else idx = (oy * cols + ox) * nc + c;
                std::uint8_t* d = out_.data() + idx * bps;
                const auto us = static_cast<std::uint32_t>(s);
                if (bps == 1) d[0] = static_cast<std::uint8_t>(us);
                else if (bps == 2) store_le16(d, static_cast<std::uint16_t>(us));
                else store_le32(d, us);
            }
        }
    }

private:
    const FrameInfo& info_;
    std::span<std::uint8_t> out_;
    const Siz* siz_ = nullptr;
};

class ComponentSink : public Sink {
public:
    explicit ComponentSink(std::vector<j2k::Component>& out) : out_(out) {}
    Status begin(const Siz& siz) override {
        out_.clear();
        out_.resize(siz.comp.size());
        std::uint64_t total = 0;
        for (std::size_t c = 0; c < siz.comp.size(); ++c) {
            const CompSiz& sc = siz.comp[c];
            j2k::Component& o = out_[c];
            x0_.push_back(ceil_div(siz.xo, sc.dx));
            y0_.push_back(ceil_div(siz.yo, sc.dy));
            o.width = static_cast<std::uint32_t>(ceil_div(siz.xsiz, sc.dx) - x0_.back());
            o.height = static_cast<std::uint32_t>(ceil_div(siz.ysiz, sc.dy) - y0_.back());
            o.dx = sc.dx;
            o.dy = sc.dy;
            o.precision = sc.depth;
            o.is_signed = sc.sgn;
            if (sc.depth > 31) return unsupported("sample precision above 31 bits");
            total += std::uint64_t{o.width} * o.height;
        }
        if (total > max_samples) return Status(Errc::LimitExceeded, "JPEG 2000: image exceeds the sample limit");
        for (auto& o : out_) o.samples.assign(static_cast<std::size_t>(std::uint64_t{o.width} * o.height), 0);
        return Status::ok();
    }
    void put(std::size_t c, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t cx0, std::int64_t cy0,
             std::int64_t cx1, std::int64_t cy1, const std::int32_t* data, std::size_t stride) override {
        j2k::Component& o = out_[c];
        for (std::int64_t y = cy0; y < cy1; ++y) {
            const std::size_t oy = static_cast<std::size_t>(y - y0_[c]);
            std::copy(data + static_cast<std::size_t>(y - cy0) * stride, data + static_cast<std::size_t>(y - cy0) * stride + static_cast<std::size_t>(cx1 - cx0),
                      o.samples.begin() + static_cast<std::ptrdiff_t>(oy * o.width + static_cast<std::size_t>(cx0 - x0_[c])));
        }
    }

private:
    std::vector<j2k::Component>& out_;
    std::vector<std::int64_t> x0_, y0_;
};

}  // namespace

Status decode_j2k(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out) {
    if (info.frame_bytes() == 0) return Status(Errc::InvalidArgument, "JPEG 2000: invalid frame parameters");
    if (out.size() != info.frame_bytes()) return Status(Errc::BufferTooSmall, "JPEG 2000: output buffer size");
    std::memset(out.data(), 0, out.size());
    FrameSink sink(info, out);
    return run_decoder(in, sink);
}

Status j2k::decode_components(std::span<const std::uint8_t> in, std::vector<Component>& out, std::uint64_t max_samples) {
    ComponentSink sink(out);
    sink.max_samples = max_samples;
    return run_decoder(in, sink);
}

Status j2k::probe(std::span<const std::uint8_t> in, StreamInfo& out) {
    std::span<const std::uint8_t> stream;
    bool wrapped = false;
    if (Status s = locate_codestream(in, stream, wrapped); !s) return s;
    Codestream cs;
    if (Status s = parse_codestream(stream, cs); !s) return s;
    const Siz& z = cs.siz;
    out = StreamInfo{};
    out.width = z.xsiz - z.xo;
    out.height = z.ysiz - z.yo;
    out.x_offset = z.xo;
    out.y_offset = z.yo;
    out.tile_width = z.xt;
    out.tile_height = z.yt;
    out.tiles = z.ntx * z.nty;
    out.components = static_cast<int>(z.comp.size());
    out.precision = z.comp[0].depth;
    out.is_signed = z.comp[0].sgn;
    for (const auto& c : z.comp)
        if (c.dx != 1 || c.dy != 1) out.subsampled = true;
    out.levels = cs.main.cod.levels;
    out.layers = cs.main.layers;
    out.progression = cs.main.prog;
    out.reversible = cs.main.cod.transform == 1;
    out.mct = cs.main.mct != 0;
    out.ht = cs.ht || (cs.main.cod.style & kCbHT) != 0;
    out.cblk_width = 1 << cs.main.cod.xcb;
    out.cblk_height = 1 << cs.main.cod.ycb;
    out.cblk_style = cs.main.cod.style;
    out.jp2_wrapper = wrapped;
    return Status::ok();
}

}  // namespace siaara::codec
