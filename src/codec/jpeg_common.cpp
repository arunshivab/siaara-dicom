// SIAARA DICOM Toolkit - JPEG tables, Huffman code construction and DCTs.
// Written from ITU-T T.81 (Annex A.3.3, Annex C, Annex K).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "codec/jpeg_internal.hpp"

namespace siaara::codec::jpeg_detail {

// ------------------------------------------------------------------ Annex K example tables
const std::uint8_t kLumaQuant[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
const std::uint8_t kChromaQuant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

namespace {

HuffSpec make_spec(std::initializer_list<int> counts16, std::initializer_list<int> values) {
    HuffSpec s;
    int l = 1;
    for (int c : counts16) s.counts[l++] = static_cast<std::uint8_t>(c);
    for (int v : values) s.values[s.nvalues++] = static_cast<std::uint8_t>(v);
    return s;
}

}  // namespace

// Table K.3
const HuffSpec kStdDcLuma = make_spec({0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
                                      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
// Table K.4
const HuffSpec kStdDcChroma = make_spec({0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
// Table K.5
const HuffSpec kStdAcLuma = make_spec(
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
     0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
     0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
     0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
     0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
     0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
     0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
     0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
     0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
     0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
     0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa});
// Table K.6
const HuffSpec kStdAcChroma = make_spec(
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
     0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
     0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
     0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
     0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
     0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
     0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
     0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
     0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
     0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
     0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa});

// ------------------------------------------------------------------ Huffman code construction
void build_huffman_spec(const std::uint32_t freq[256], HuffSpec& spec) {
    spec = HuffSpec{};
    // Symbols 0..255 plus a reserved symbol 256 of frequency 1. The reserved symbol ends up with
    // one of the longest codes; dropping it afterwards leaves the all-ones code unused.
    constexpr int kSyms = 257;
    std::uint64_t weight[2 * kSyms];
    int parent[2 * kSyms];
    bool alive[2 * kSyms];
    int nodes = 0;
    int leaf_of_sym[kSyms];
    for (int s = 0; s < kSyms; ++s) {
        leaf_of_sym[s] = -1;
        const std::uint64_t f = s == 256 ? 1u : freq[s];
        if (f == 0) continue;
        leaf_of_sym[s] = nodes;
        weight[nodes] = f;
        parent[nodes] = -1;
        alive[nodes] = true;
        ++nodes;
    }
    const int leaves = nodes;
    // Classic Huffman merge: repeatedly join the two lightest live nodes. n <= 257, so the
    // quadratic search is cheap and keeps the code obvious.
    for (int live = leaves; live > 1; --live) {
        int a = -1, b = -1;
        for (int i = 0; i < nodes; ++i) {
            if (!alive[i]) continue;
            if (a < 0 || weight[i] < weight[a]) { b = a; a = i; }
            else if (b < 0 || weight[i] < weight[b]) b = i;
        }
        weight[nodes] = weight[a] + weight[b];
        parent[nodes] = -1;
        alive[nodes] = true;
        alive[a] = alive[b] = false;
        parent[a] = parent[b] = nodes;
        ++nodes;
    }
    int bits[300] = {};
    int depth_of_sym[kSyms];
    for (int s = 0; s < kSyms; ++s) {
        depth_of_sym[s] = 0;
        if (leaf_of_sym[s] < 0) continue;
        int d = 0;
        for (int n = leaf_of_sym[s]; parent[n] >= 0; n = parent[n]) ++d;
        if (d == 0) d = 1;  // cannot happen (>= 2 leaves), kept for safety
        d = std::min(d, 299);
        depth_of_sym[s] = d;
        ++bits[d];
    }
    // Limit code lengths to 16 (T.81 figure K.3): move pairs of codes up the tree.
    for (int i = 299; i > 16;) {
        if (bits[i] > 0) {
            int j = i - 2;
            while (j > 0 && bits[j] == 0) --j;
            bits[i] -= 2;
            bits[i - 1] += 1;
            bits[j + 1] += 2;
            bits[j] -= 1;
        } else {
            --i;
        }
    }
    // Remove the reserved code point from the longest length present.
    int longest = 16;
    while (longest > 0 && bits[longest] == 0) --longest;
    if (longest > 0) bits[longest] -= 1;

    // Order the real symbols by their original code length (stable by symbol value); the
    // adjusted counts are then handed out in that order (T.81 K.2 / figure K.4).
    int order[256];
    int n = 0;
    for (int s = 0; s < 256; ++s)
        if (leaf_of_sym[s] >= 0) order[n++] = s;
    std::stable_sort(order, order + n,
                     [&](int x, int y) { return depth_of_sym[x] < depth_of_sym[y]; });
    for (int l = 1; l <= 16; ++l) spec.counts[l] = static_cast<std::uint8_t>(bits[l]);
    for (int i = 0; i < n; ++i) spec.values[i] = static_cast<std::uint8_t>(order[i]);
    spec.nvalues = n;
}

bool make_codes(const HuffSpec& spec, std::uint16_t code[256], std::uint8_t len[256]) {
    std::memset(len, 0, 256);
    std::memset(code, 0, 256 * sizeof(std::uint16_t));
    std::uint32_t c = 0;
    int k = 0;
    for (int l = 1; l <= 16; ++l) {
        for (int i = 0; i < spec.counts[l]; ++i) {
            if (k >= spec.nvalues) return false;
            code[spec.values[k]] = static_cast<std::uint16_t>(c);
            len[spec.values[k]] = static_cast<std::uint8_t>(l);
            ++k;
            ++c;
        }
        if (c > (1u << l)) return false;
        c <<= 1;
    }
    return true;
}

// ------------------------------------------------------------------ inverse DCT
// The 8-point 1-D inverse transform x[n] = 1/2 * sum_k c(k) X[k] cos((2n+1)k pi/16) is split into
// an even half (X0, X2, X4, X6: two butterflies) and an odd half (X1, X3, X5, X7: a 4x4 product
// with the cosine matrix), x[n] = E[n] + O[n], x[7-n] = E[n] - O[n]. Constants are
// cos(k pi/16) in Q15; all arithmetic is 64-bit so no input can overflow it.
namespace {

constexpr int kQ = 15;
constexpr std::int64_t kC1 = 32138, kC2 = 30274, kC3 = 27246, kC4 = 23170, kC5 = 18205,
                       kC6 = 12540, kC7 = 6393;
constexpr int kPass1Frac = 4;                       // fraction bits kept between passes
constexpr int kShift1 = kQ + 1 - kPass1Frac;        // +1 is the 1/2 of the 1-D transform
constexpr int kShift2 = kQ + 1 + kPass1Frac;
constexpr std::int64_t kRound1 = std::int64_t{1} << (kShift1 - 1);
constexpr std::int64_t kRound2 = std::int64_t{1} << (kShift2 - 1);

struct Row8 { std::int64_t v[8]; };

inline void inverse_1d(std::int64_t i0, std::int64_t i1, std::int64_t i2, std::int64_t i3,
                       std::int64_t i4, std::int64_t i5, std::int64_t i6, std::int64_t i7,
                       Row8& o) {
    const std::int64_t p = (i0 + i4) * kC4;
    const std::int64_t q = (i0 - i4) * kC4;
    const std::int64_t r = i2 * kC2 + i6 * kC6;
    const std::int64_t s = i2 * kC6 - i6 * kC2;
    const std::int64_t e0 = p + r, e3 = p - r, e1 = q + s, e2 = q - s;
    const std::int64_t o0 = i1 * kC1 + i3 * kC3 + i5 * kC5 + i7 * kC7;
    const std::int64_t o1 = i1 * kC3 - i3 * kC7 - i5 * kC1 - i7 * kC5;
    const std::int64_t o2 = i1 * kC5 - i3 * kC1 + i5 * kC7 + i7 * kC3;
    const std::int64_t o3 = i1 * kC7 - i3 * kC5 + i5 * kC3 - i7 * kC1;
    o.v[0] = e0 + o0; o.v[7] = e0 - o0;
    o.v[1] = e1 + o1; o.v[6] = e1 - o1;
    o.v[2] = e2 + o2; o.v[5] = e2 - o2;
    o.v[3] = e3 + o3; o.v[4] = e3 - o3;
}

inline std::uint16_t clamp_sample(std::int64_t v, int maxval) {
    if (v < 0) return 0;
    if (v > maxval) return static_cast<std::uint16_t>(maxval);
    return static_cast<std::uint16_t>(v);
}

}  // namespace

void idct8x8(const std::int32_t* coef, std::uint16_t* out, std::size_t stride, int level_shift,
             int maxval) {
    std::int64_t tmp[64];
    // Pass 1: columns (vertical frequencies) -> tmp[y][u].
    for (int u = 0; u < 8; ++u) {
        const std::int32_t* c = coef + u;
        if ((c[8] | c[16] | c[24] | c[32] | c[40] | c[48] | c[56]) == 0) {
            const std::int64_t dc = (c[0] * kC4 + kRound1) >> kShift1;
            for (int y = 0; y < 8; ++y) tmp[y * 8 + u] = dc;
            continue;
        }
        Row8 r;
        inverse_1d(c[0], c[8], c[16], c[24], c[32], c[40], c[48], c[56], r);
        for (int y = 0; y < 8; ++y) tmp[y * 8 + u] = (r.v[y] + kRound1) >> kShift1;
    }
    // Pass 2: rows.
    const std::int64_t shift = level_shift;
    for (int y = 0; y < 8; ++y) {
        const std::int64_t* t = tmp + y * 8;
        std::uint16_t* o = out + static_cast<std::size_t>(y) * stride;
        Row8 r;
        inverse_1d(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], r);
        for (int x = 0; x < 8; ++x) o[x] = clamp_sample(((r.v[x] + kRound2) >> kShift2) + shift, maxval);
    }
}

void idct8x8_dc(std::int32_t dc, std::uint16_t* out, std::size_t stride, int level_shift, int maxval) {
    // Identical arithmetic to idct8x8 with only coef[0] set.
    const std::int64_t t = (dc * kC4 + kRound1) >> kShift1;
    const std::uint16_t v = clamp_sample(((t * kC4 + kRound2) >> kShift2) + level_shift, maxval);
    for (int y = 0; y < 8; ++y) {
        std::uint16_t* o = out + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < 8; ++x) o[x] = v;
    }
}

// ------------------------------------------------------------------ forward DCT (encoder)
namespace {

struct FdctMatrix {
    double m[8][8];  // m[u][x] = 1/2 c(u) cos((2x+1) u pi / 16)
    FdctMatrix() {
        const double pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                m[u][x] = 0.5 * (u == 0 ? 1.0 / std::sqrt(2.0) : 1.0) *
                          std::cos((2.0 * x + 1.0) * u * pi / 16.0);
    }
};

const FdctMatrix& fdct_matrix() {
    static const FdctMatrix m;
    return m;
}

}  // namespace

void fdct8x8(const double* in, double* out) {
    const auto& m = fdct_matrix().m;
    double tmp[64];
    for (int y = 0; y < 8; ++y)          // rows: tmp[y][u] = sum_x m[u][x] in[y][x]
        for (int u = 0; u < 8; ++u) {
            double s = 0;
            for (int x = 0; x < 8; ++x) s += m[u][x] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; ++u)          // columns: out[v][u] = sum_y m[v][y] tmp[y][u]
        for (int v = 0; v < 8; ++v) {
            double s = 0;
            for (int y = 0; y < 8; ++y) s += m[v][y] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

}  // namespace siaara::codec::jpeg_detail
