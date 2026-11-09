// SIAARA DICOM Toolkit - HTJ2K block encoder (cleanup pass only), written from T.814 annex F.
// Codes every magnitude bit in a single HT cleanup pass at bit-plane 0, which is what lossless
// coding needs. Bit-stream order mirrors the decoding procedures of clause 7.3 exactly.
#include <algorithm>
#include <array>
#include <cstring>

#include "codec/j2k_internal.hpp"

namespace siaara::codec::j2k::detail {

namespace {

// CxtVLC encoder table: index (c << 8) | (rho << 4) | emb, value = len | w << 3 | e_k << 10,
// len 0 for combinations that cannot occur. Built from the annex C entries as F.3 describes,
// preferring the entry with the most known EMB bits, then the shortest codeword.
struct VlcEncTable {
    std::array<std::uint16_t, 2048> t{};
};

VlcEncTable make_enc_table(std::span<const std::uint32_t> entries) {
    VlcEncTable tb;
    std::array<int, 2048> best_k{}, best_len{};
    best_k.fill(-1);
    for (std::uint32_t e : entries) {
        const std::uint32_t c = e & 7u, rho = (e >> 3) & 15u, uoff = (e >> 7) & 1u, ek = (e >> 8) & 15u,
                            e1 = (e >> 12) & 15u, w = (e >> 16) & 127u, len = (e >> 23) & 7u;
        for (std::uint32_t emb = 0; emb < 16; ++emb) {
            if ((emb & ~rho) != 0) continue;
            if ((emb != 0) != (uoff != 0)) continue;
            if ((emb & ek) != e1) continue;
            const std::size_t idx = (c << 8) | (rho << 4) | emb;
            const int kc = std::popcount(ek);
            if (kc > best_k[idx] || (kc == best_k[idx] && static_cast<int>(len) < best_len[idx])) {
                best_k[idx] = kc;
                best_len[idx] = static_cast<int>(len);
                tb.t[idx] = static_cast<std::uint16_t>(len | (w << 3) | (ek << 10));
            }
        }
    }
    return tb;
}

const VlcEncTable& enc_table(bool initial) {
    static const VlcEncTable t0 = make_enc_table(kHtVlcInitialEntries);
    static const VlcEncTable t1 = make_enc_table(kHtVlcNonInitialEntries);
    return initial ? t0 : t1;
}

class MagSgnPacker {
public:
    void emit(std::uint64_t val, int len) {
        while (len > 0) {
            tmp_ |= static_cast<std::uint32_t>(val & 1u) << bits_;
            val >>= 1;
            --len;
            if (++bits_ == max_) {
                buf.push_back(static_cast<std::uint8_t>(tmp_));
                max_ = tmp_ == 0xFF ? 7 : 8;
                tmp_ = 0;
                bits_ = 0;
            }
        }
    }
    void term() {
        if (bits_ > 0) {
            while (bits_ < max_) tmp_ |= 1u << bits_++;
            if (tmp_ != 0xFF) buf.push_back(static_cast<std::uint8_t>(tmp_));
        } else if (max_ == 7) {
            buf.pop_back();  // discard a trailing 0xFF
        }
    }
    std::vector<std::uint8_t> buf;

private:
    std::uint32_t tmp_ = 0;
    int bits_ = 0, max_ = 8;
};

class VlcPacker {
public:
    VlcPacker() { buf.push_back(0xFF); }
    void emit(std::uint32_t cwd, int len) {
        while (len > 0) {
            tmp |= (cwd & 1u) << bits;
            cwd >>= 1;
            --len;
            ++bits;
            if (last_ > 0x8F && tmp == 0x7F) ++bits;
            if (bits == 8) {
                buf.push_back(static_cast<std::uint8_t>(tmp));
                last_ = tmp;
                tmp = 0;
                bits = 0;
            }
        }
    }
    std::vector<std::uint8_t> buf;
    std::uint32_t tmp = 15;
    int bits = 4;

private:
    std::uint32_t last_ = 0xFF;
};

class MelEncoder {
public:
    void encode(int sym) {
        if (sym == 0) {
            if (++run_ >= t_) {
                bit(1);
                run_ = 0;
                k_ = std::min(12, k_ + 1);
                t_ = 1u << kHtMelExponent[k_];
            }
        } else {
            bit(0);
            int e = kHtMelExponent[k_];
            while (e > 0) {
                --e;
                bit(static_cast<int>((run_ >> e) & 1u));
            }
            run_ = 0;
            k_ = std::max(0, k_ - 1);
            t_ = 1u << kHtMelExponent[k_];
        }
    }
    void term() {
        if (run_ > 0) bit(1);
    }
    std::vector<std::uint8_t> buf;
    std::uint32_t tmp = 0;
    int rem = 8;

private:
    void bit(int b) {
        tmp = 2 * tmp + static_cast<std::uint32_t>(b);
        if (--rem == 0) {
            buf.push_back(static_cast<std::uint8_t>(tmp));
            rem = tmp == 0xFF ? 7 : 8;
            tmp = 0;
        }
    }
    int k_ = 0;
    std::uint32_t run_ = 0;
    std::uint32_t t_ = 1;
};

inline int exponent_of(std::uint32_t mu) { return mu == 0 ? 0 : static_cast<int>(std::bit_width(mu - 1)) + 1; }

// U-VLC components (table 3) for u_in >= 1: prefix codeword/length, suffix value/length,
// extension value/length.
struct Uvlc {
    std::uint32_t pfx = 0, sfx = 0, ext = 0;
    int lp = 0, ls = 0, le = 0;
};
Uvlc uvlc(std::uint32_t u) {
    Uvlc r;
    if (u == 0) return r;
    if (u == 1) { r.pfx = 1; r.lp = 1; }
    else if (u == 2) { r.pfx = 2; r.lp = 2; }
    else if (u <= 4) { r.pfx = 4; r.lp = 3; r.sfx = u - 3; r.ls = 1; }
    else if (u <= 32) { r.pfx = 0; r.lp = 3; r.sfx = u - 5; r.ls = 5; }
    else { r.pfx = 0; r.lp = 3; r.sfx = 28 + (u - 33) % 4; r.ls = 5; r.ext = (u - 33) / 4; r.le = 4; }
    return r;
}

}  // namespace

bool encode_ht_block(int width, int height, int planes, const std::uint32_t* mag, const std::uint8_t* neg,
                     std::size_t stride, EncodedBlock& out) {
    out.data.clear();
    out.pass_end.clear();
    out.pass_dist.clear();
    out.passes = 0;
    out.zero_planes = planes - 1;  // the cleanup pass codes bit-plane 0 (S_blk = Mb - 1)
    const int qw = (width + 1) / 2, qh = (height + 1) / 2;
    const std::size_t sw = static_cast<std::size_t>(2 * qw) + 4;
    // Padded copy: mu and sign per sample, guard columns on both sides, zero padding.
    std::vector<std::uint32_t> mu(sw * static_cast<std::size_t>(2 * qh), 0);
    std::vector<std::uint8_t> sg(mu.size(), 0);
    bool any = false;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const std::uint32_t m = mag[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)];
            if (m >= (1u << 30) || static_cast<int>(std::bit_width(m)) > planes) return false;
            mu[static_cast<std::size_t>(y) * sw + static_cast<std::size_t>(x) + 2] = m;
            sg[static_cast<std::size_t>(y) * sw + static_cast<std::size_t>(x) + 2] = neg[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)];
            any = any || m != 0;
        }
    if (!any) return true;
    auto row = [&](int y) { return mu.data() + static_cast<std::size_t>(y) * sw + 2; };
    auto srow = [&](int y) { return sg.data() + static_cast<std::size_t>(y) * sw + 2; };

    MagSgnPacker ms;
    VlcPacker vlc;
    MelEncoder mel;
    for (int qy = 0; qy < qh; ++qy) {
        const bool initial = qy == 0;
        const std::uint32_t* top = row(2 * qy);
        const std::uint32_t* bot = row(2 * qy + 1);
        const std::uint8_t* stop = srow(2 * qy);
        const std::uint8_t* sbot = srow(2 * qy + 1);
        const std::uint32_t* above = initial ? nullptr : row(2 * qy - 1);
        const VlcEncTable& tb = enc_table(initial);
        for (int qx = 0; qx < qw; qx += 2) {
            const bool pair = qx + 1 < qw;
            const int nq = pair ? 2 : 1;
            std::uint32_t rho[2] = {0, 0}, u[2] = {0, 0}, bigu[2] = {0, 0}, ek[2] = {0, 0}, emb[2] = {0, 0};
            std::uint32_t cw[2] = {0, 0};
            int cl[2] = {0, 0};
            int melsym[2] = {-1, -1};
            for (int k = 0; k < nq; ++k) {
                const int x = 2 * (qx + k);
                const std::uint32_t s4[4] = {top[x], bot[x], top[x + 1], bot[x + 1]};
                int e[4];
                int emax = 0;
                for (int j = 0; j < 4; ++j) {
                    if (s4[j]) rho[k] |= 1u << j;
                    e[j] = exponent_of(s4[j]);
                    emax = std::max(emax, e[j]);
                }
                std::uint32_t c;
                if (initial) {
                    c = ((top[x - 2] | bot[x - 2]) ? 1u : 0u) + (top[x - 1] ? 2u : 0u) + (bot[x - 1] ? 4u : 0u);
                } else {
                    c = ((above[x - 1] | above[x]) ? 1u : 0u) + ((top[x - 1] | bot[x - 1]) ? 2u : 0u) +
                        ((above[x + 1] | above[x + 2]) ? 4u : 0u);
                }
                if (c == 0) melsym[k] = rho[k] ? 1 : 0;
                if (rho[k] == 0) {
                    // Non-AZC quads code an empty significance pattern with a CxtVLC codeword too.
                    if (c != 0) {
                        const std::uint16_t te = tb.t[c << 8];
                        cl[k] = te & 7;
                        if (cl[k] == 0) return false;
                        cw[k] = (te >> 3) & 127u;
                    }
                    continue;
                }
                int kappa = 1;
                if (!initial && (rho[k] & (rho[k] - 1)) != 0) {
                    const int en = std::max(std::max(exponent_of(above[x - 1]), exponent_of(above[x])),
                                            std::max(exponent_of(above[x + 1]), exponent_of(above[x + 2])));
                    kappa = std::max(1, en - 1);
                }
                const int ub = std::max(emax, kappa);
                bigu[k] = static_cast<std::uint32_t>(ub);
                u[k] = static_cast<std::uint32_t>(ub - kappa);
                if (u[k] > 0)
                    for (int j = 0; j < 4; ++j)
                        if ((rho[k] >> j) & 1u && e[j] == emax) emb[k] |= 1u << j;
                const std::uint16_t te = tb.t[(c << 8) | (rho[k] << 4) | emb[k]];
                cl[k] = te & 7;
                if (cl[k] == 0) return false;
                cw[k] = (te >> 3) & 127u;
                ek[k] = (te >> 10) & 15u;
            }
            // MEL symbols (AZC quads) and CxtVLC codewords, in the decoder's order.
            for (int k = 0; k < nq; ++k) {
                if (melsym[k] >= 0) mel.encode(melsym[k]);
                if (cl[k]) vlc.emit(cw[k], cl[k]);
            }
            const bool both = pair && u[0] > 0 && u[1] > 0;
            int mel_pair = -1;
            if (initial && both) {
                mel_pair = std::min(u[0], u[1]) > 2 ? 1 : 0;
                mel.encode(mel_pair);
            }
            Uvlc uv[2];
            bool special2 = false;
            for (int k = 0; k < nq; ++k) {
                if (u[k] == 0) continue;
                if (mel_pair == 1) uv[k] = uvlc(u[k] - 2);
                else if (k == 1 && mel_pair == 0 && u[0] > 2) special2 = true;
                else uv[k] = uvlc(u[k]);
            }
            for (int k = 0; k < nq; ++k) {
                if (u[k] == 0) continue;
                if (k == 1 && special2) vlc.emit(u[1] - 1, 1);
                else vlc.emit(uv[k].pfx, uv[k].lp);
            }
            for (int k = 0; k < nq; ++k)
                if (u[k] && !(k == 1 && special2)) vlc.emit(uv[k].sfx, uv[k].ls);
            for (int k = 0; k < nq; ++k)
                if (u[k] && !(k == 1 && special2)) vlc.emit(uv[k].ext, uv[k].le);
            // MagSgn bits.
            for (int k = 0; k < nq; ++k) {
                if (!rho[k]) continue;
                const int x = 2 * (qx + k);
                const std::uint32_t s4[4] = {top[x], bot[x], top[x + 1], bot[x + 1]};
                const std::uint8_t n4[4] = {stop[x], sbot[x], stop[x + 1], sbot[x + 1]};
                for (int j = 0; j < 4; ++j) {
                    if (!((rho[k] >> j) & 1u)) continue;
                    const int m = static_cast<int>(bigu[k]) - static_cast<int>((ek[k] >> j) & 1u);
                    const std::uint64_t v = 2 * (std::uint64_t{s4[j]} - 1) + (n4[j] ? 1u : 0u);
                    ms.emit(v, m);
                }
            }
        }
    }
    ms.term();
    mel.term();
    // termMELandVLCPackers.
    {
        const std::uint32_t mel_tmp = (mel.tmp << mel.rem) & 0xFFu;
        const std::uint32_t mel_mask = (0xFFu << mel.rem) & 0xFFu;
        const std::uint32_t vlc_mask = vlc.bits == 0 ? 0u : (0xFFu >> (8 - vlc.bits));
        if ((mel_mask | vlc_mask) != 0) {
            const std::uint32_t fuse = mel_tmp | vlc.tmp;
            if (((((fuse ^ mel_tmp) & mel_mask) | ((fuse ^ vlc.tmp) & vlc_mask)) == 0) && fuse != 0xFF) {
                mel.buf.push_back(static_cast<std::uint8_t>(fuse));
            } else {
                mel.buf.push_back(static_cast<std::uint8_t>(mel_tmp));
                vlc.buf.push_back(static_cast<std::uint8_t>(vlc.tmp));
            }
        }
    }
    const std::size_t scup = mel.buf.size() + vlc.buf.size();
    if (scup > 4079 || scup < 2) return false;
    out.data = ms.buf;
    out.data.insert(out.data.end(), mel.buf.begin(), mel.buf.end());
    out.data.insert(out.data.end(), vlc.buf.rbegin(), vlc.buf.rend());
    const std::size_t lcup = out.data.size();
    if (lcup >= 65535) return false;
    out.data[lcup - 1] = static_cast<std::uint8_t>(scup >> 4);
    out.data[lcup - 2] = static_cast<std::uint8_t>((out.data[lcup - 2] & 0xF0u) | (scup & 0x0Fu));
    out.passes = 1;
    out.pass_end.push_back(static_cast<std::uint32_t>(lcup));
    out.pass_dist.push_back(1.0);
    return true;
}

}  // namespace siaara::codec::j2k::detail
