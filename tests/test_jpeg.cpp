// Tests for the JPEG codec family (src/codec/jpeg*).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "codec/codec.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpeg_internal.hpp"
#include "core/types.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::codec;

namespace {

using Buf = std::vector<std::uint8_t>;

FrameInfo frame(std::uint32_t rows, std::uint32_t cols, int spp, int alloc, int stored, bool sgn = false,
                bool planar = false) {
    FrameInfo f;
    f.rows = rows;
    f.columns = cols;
    f.samples_per_pixel = static_cast<std::uint16_t>(spp);
    f.bits_allocated = static_cast<std::uint16_t>(alloc);
    f.bits_stored = static_cast<std::uint16_t>(stored);
    f.is_signed = sgn;
    f.planar = planar;
    return f;
}

// Smooth gradients plus noise; values fit bits_stored (sign-extended when signed).
Buf make_image(const FrameInfo& f, unsigned seed, double noise = 3.0) {
    Buf b(static_cast<std::size_t>(f.frame_bytes()));
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, noise);
    const double maxv = std::ldexp(1.0, f.bits_stored) - 1;
    const std::size_t npx = static_cast<std::size_t>(f.rows) * f.columns;
    for (std::uint32_t y = 0; y < f.rows; ++y)
        for (std::uint32_t x = 0; x < f.columns; ++x)
            for (int i = 0; i < f.samples_per_pixel; ++i) {
                const double t = 0.5 + 0.35 * std::sin(0.11 * x + 0.7 * i) * std::cos(0.07 * y - 0.3 * i);
                double v = t * maxv + nd(rng) * maxv / 255.0;
                v = std::clamp(v, 0.0, maxv);
                std::int64_t iv = static_cast<std::int64_t>(std::lround(v));
                if (f.is_signed) iv -= static_cast<std::int64_t>(1) << (f.bits_stored - 1);
                const std::size_t px = static_cast<std::size_t>(y) * f.columns + x;
                const std::size_t idx = f.planar ? static_cast<std::size_t>(i) * npx + px : px * f.samples_per_pixel + static_cast<std::size_t>(i);
                if (f.bits_allocated == 8) b[idx] = static_cast<std::uint8_t>(iv);
                else store_le16(b.data() + 2 * idx, static_cast<std::uint16_t>(iv));
            }
    return b;
}

std::int64_t sample_at(const Buf& b, const FrameInfo& f, std::size_t idx) {
    if (f.bits_allocated == 8) return b[idx];
    return load_le16(b.data() + 2 * idx);
}

double psnr(const Buf& a, const Buf& b, const FrameInfo& f) {
    const std::size_t n = a.size() / (f.bits_allocated / 8u);
    double se = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(sample_at(a, f, i) - sample_at(b, f, i));
        se += d * d;
    }
    if (se == 0) return 99.0;
    const double peak = std::ldexp(1.0, f.bits_stored) - 1;
    return 10.0 * std::log10(peak * peak * static_cast<double>(n) / se);
}

Status roundtrip(const Buf& img, const FrameInfo& f, const EncodeParams& p, const JpegEncodeOptions& o, Buf& dec,
                 Buf* stream = nullptr) {
    Buf enc;
    if (Status st = encode_jpeg_ex(img, f, p, o, enc); !st) return st;
    dec.assign(img.size(), 0xCD);
    Status st = decode_jpeg(enc, f, dec);
    if (stream) *stream = enc;
    return st;
}

EncodeParams lossless(int predictor) {
    EncodeParams p;
    p.lossless = true;
    p.predictor = predictor;
    return p;
}
EncodeParams lossy(int q, bool ybr = false) {
    EncodeParams p;
    p.lossless = false;
    p.quality = q;
    p.ybr = ybr;
    return p;
}

// Offset of the first marker m (0xFFm) found by walking the segments after SOI; npos if absent.
constexpr std::size_t npos = static_cast<std::size_t>(-1);
std::size_t find_segment(const Buf& s, int m) {
    std::size_t pos = 2;
    while (pos + 4 <= s.size() && s[pos] == 0xFF) {
        if (s[pos + 1] == m) return pos;
        if (s[pos + 1] == 0xDA) return npos;
        pos += 2u + std::size_t{load_be16(s.data() + pos + 2)};
    }
    return npos;
}

Buf insert(Buf s, std::size_t at, std::initializer_list<std::uint8_t> bytes) {
    s.insert(s.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin(), bytes.end());
    return s;
}

}  // namespace

// ================================================================== tables and transforms
TEST_CASE("jpeg: zig-zag table is the T.81 figure A.6 permutation") {
    const auto& z = jpeg_detail::kZigzag;
    std::set<int> seen(z.begin(), z.begin() + 64);
    CHECK_EQ(seen.size(), 64u);
    const int head[10] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24};
    for (int i = 0; i < 10; ++i) CHECK_EQ(z[static_cast<std::size_t>(i)], head[i]);
    CHECK_EQ(z[63], 63);
    CHECK_EQ(z[62], 62);
    CHECK_EQ(z[61], 55);
    CHECK_EQ(z[79], 63);
}

TEST_CASE("jpeg: Annex K Huffman tables are complete prefix codes") {
    const jpeg_detail::HuffSpec* specs[4] = {&jpeg_detail::kStdDcLuma, &jpeg_detail::kStdDcChroma,
                                             &jpeg_detail::kStdAcLuma, &jpeg_detail::kStdAcChroma};
    for (const auto* s : specs) {
        int total = 0;
        for (int l = 1; l <= 16; ++l) total += s->counts[l];
        CHECK_EQ(total, s->nvalues);
        std::uint16_t code[256];
        std::uint8_t len[256];
        CHECK(jpeg_detail::make_codes(*s, code, len));
    }
    CHECK_EQ(jpeg_detail::kStdAcLuma.nvalues, 162);
    CHECK_EQ(jpeg_detail::kStdAcChroma.nvalues, 162);
}

TEST_CASE("jpeg: optimised Huffman code lengths stay within 16 bits") {
    std::uint32_t freq[256] = {};
    // Fibonacci-like frequencies force a very deep unconstrained tree.
    std::uint32_t a = 1, b = 1;
    for (int s = 0; s < 40; ++s) {
        freq[s] = a;
        const std::uint32_t c = a + b;
        a = b;
        b = c;
    }
    jpeg_detail::HuffSpec spec;
    jpeg_detail::build_huffman_spec(freq, spec);
    CHECK_EQ(spec.nvalues, 40);
    std::uint16_t code[256];
    std::uint8_t len[256];
    REQUIRE(jpeg_detail::make_codes(spec, code, len));
    for (int s = 0; s < 40; ++s) CHECK(len[s] >= 1 && len[s] <= 16);
    // The all-ones code of the longest length must stay unused.
    int longest = 0;
    for (int s = 0; s < 40; ++s) longest = std::max<int>(longest, len[s]);
    for (int s = 0; s < 40; ++s)
        if (len[s] == longest) CHECK(code[s] != (1u << longest) - 1u);
    // Single symbol.
    std::uint32_t one[256] = {};
    one[7] = 100;
    jpeg_detail::build_huffman_spec(one, spec);
    REQUIRE(jpeg_detail::make_codes(spec, code, len));
    CHECK_EQ(spec.nvalues, 1);
    CHECK_EQ(len[7], 1);
    CHECK_EQ(code[7], 0);
}

TEST_CASE("jpeg: integer IDCT agrees with the exact transform") {
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> big(-1024, 1023), small(-60, 60), pick(0, 3);
    const double pi = 3.14159265358979323846;
    int worst = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        std::int32_t c[64];
        for (int k = 0; k < 64; ++k) c[k] = k == 0 ? big(rng) * 4 : (pick(rng) == 0 ? small(rng) * (iter % 3 + 1) : 0);
        std::uint16_t out[64];
        jpeg_detail::idct8x8(c, out, 8, 2048, 4095);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                double s = 0;
                for (int v = 0; v < 8; ++v)
                    for (int u = 0; u < 8; ++u)
                        s += (u ? 1.0 : std::sqrt(0.5)) * (v ? 1.0 : std::sqrt(0.5)) * c[v * 8 + u] *
                             std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
                const long ref = std::clamp(std::lround(s / 4 + 2048), 0L, 4095L);
                worst = std::max(worst, static_cast<int>(std::abs(ref - out[y * 8 + x])));
            }
        // DC-only shortcut matches the full transform.
        std::int32_t d[64] = {};
        d[0] = c[0];
        std::uint16_t o1[64], o2[64];
        jpeg_detail::idct8x8(d, o1, 8, 2048, 4095);
        jpeg_detail::idct8x8_dc(d[0], o2, 8, 2048, 4095);
        CHECK(std::memcmp(o1, o2, sizeof o1) == 0);
    }
    CHECK(worst <= 1);
}

// ================================================================== lossless
TEST_CASE("jpeg: lossless round trip, all predictors and precisions") {
    struct Cfg { int alloc, stored; };
    const Cfg cfgs[] = {{8, 8}, {8, 2}, {8, 5}, {16, 8}, {16, 10}, {16, 12}, {16, 15}, {16, 16}};
    for (const Cfg& c : cfgs)
        for (int psv = 1; psv <= 7; ++psv) {
            const FrameInfo f = frame(29, 37, 1, c.alloc, c.stored);
            const Buf img = make_image(f, static_cast<unsigned>(psv * 31 + c.stored), 12.0);
            Buf dec;
            Status st = roundtrip(img, f, lossless(psv), {}, dec);
            CHECK(st.is_ok());
            if (!st || dec != img) {
                std::fprintf(stderr, "  lossless mismatch: alloc %d stored %d psv %d (%s)\n", c.alloc, c.stored, psv,
                             st.message.c_str());
                CHECK(false);
            }
        }
}

TEST_CASE("jpeg: lossless full-range 16-bit noise and the 32768 difference") {
    const FrameInfo f = frame(16, 33, 1, 16, 16);
    Buf img(static_cast<std::size_t>(f.frame_bytes()));
    std::mt19937 rng(3);
    for (std::size_t i = 0; i < img.size(); i += 2) store_le16(img.data() + i, static_cast<std::uint16_t>(rng()));
    // Alternate 0 / 32768 in the first row: predictor 1 then sees differences of exactly 32768.
    for (std::uint32_t x = 0; x < f.columns; ++x) store_le16(img.data() + 2 * x, x % 2 ? 0x8000 : 0);
    for (int psv = 1; psv <= 7; ++psv) {
        Buf dec;
        CHECK(roundtrip(img, f, lossless(psv), {}, dec).is_ok());
        CHECK(dec == img);
    }
}

TEST_CASE("jpeg: lossless signed data comes back sign-correct") {
    for (int stored : {12, 16, 7}) {
        const int alloc = stored > 8 ? 16 : 8;
        const FrameInfo f = frame(21, 19, 1, alloc, stored, true);
        const Buf img = make_image(f, static_cast<unsigned>(stored), 20.0);
        for (int psv : {1, 4, 7}) {
            Buf dec;
            CHECK(roundtrip(img, f, lossless(psv), {}, dec).is_ok());
            CHECK(dec == img);
        }
        if (alloc == 16) {  // spot-check a negative value really is sign-extended
            bool neg = false;
            for (std::size_t i = 0; i < img.size(); i += 2) neg |= (load_le16(img.data() + i) & 0x8000) != 0;
            CHECK(neg);
        }
    }
}

TEST_CASE("jpeg: lossless colour, interleaved, planar and separate scans") {
    for (bool planar : {false, true})
        for (bool separate : {false, true})
            for (int alloc : {8, 16}) {
                const FrameInfo f = frame(17, 23, 3, alloc, alloc == 8 ? 8 : 12, false, planar);
                const Buf img = make_image(f, 11, 8.0);
                JpegEncodeOptions o;
                o.separate_scans = separate;
                for (int psv : {1, 5, 6}) {
                    Buf dec;
                    CHECK(roundtrip(img, f, lossless(psv), o, dec).is_ok());
                    CHECK(dec == img);
                }
            }
    // Four components.
    const FrameInfo f4 = frame(9, 11, 4, 8, 8);
    const Buf img4 = make_image(f4, 5, 8.0);
    Buf dec;
    CHECK(roundtrip(img4, f4, lossless(7), {}, dec).is_ok());
    CHECK(dec == img4);
}

TEST_CASE("jpeg: lossless restart intervals (row aligned and mid-row)") {
    for (int spp : {1, 3}) {
        const FrameInfo f = frame(23, 19, spp, 16, 16);
        const Buf img = make_image(f, 99, 40.0);
        for (std::uint16_t ri : {std::uint16_t(1), std::uint16_t(7), std::uint16_t(19), std::uint16_t(38),
                                 std::uint16_t(100), std::uint16_t(1000)})
            for (int psv : {1, 2, 4, 7}) {
                JpegEncodeOptions o;
                o.restart_interval = ri;
                Buf dec, stream;
                CHECK(roundtrip(img, f, lossless(psv), o, dec, &stream).is_ok());
                CHECK(dec == img);
            }
    }
}

TEST_CASE("jpeg: lossless point transform") {
    const FrameInfo f = frame(13, 14, 1, 16, 12);
    const Buf img = make_image(f, 4, 30.0);
    for (int pt : {1, 3, 11}) {
        JpegEncodeOptions o;
        o.point_transform = pt;
        Buf dec;
        CHECK(roundtrip(img, f, lossless(6), o, dec).is_ok());
        bool ok = true;
        for (std::size_t i = 0; i < img.size(); i += 2) {
            const unsigned want = static_cast<unsigned>((load_le16(img.data() + i) >> pt) << pt);
            ok &= load_le16(dec.data() + i) == want;
        }
        CHECK(ok);
    }
}

TEST_CASE("jpeg: tiny images") {
    for (std::uint32_t r : {1u, 2u, 9u})
        for (std::uint32_t c : {1u, 3u, 17u}) {
            const FrameInfo f = frame(r, c, 1, 16, 16);
            const Buf img = make_image(f, r * 7 + c, 50.0);
            Buf dec;
            CHECK(roundtrip(img, f, lossless(4), {}, dec).is_ok());
            CHECK(dec == img);
            const FrameInfo g = frame(r, c, 3, 8, 8);
            const Buf img3 = make_image(g, r + c, 2.0);
            JpegEncodeOptions o;
            o.luma_h = o.luma_v = 2;
            CHECK(roundtrip(img3, g, lossy(95, true), o, dec).is_ok());
        }
}

// ================================================================== lossy
TEST_CASE("jpeg: baseline 8-bit and extended 12-bit quality") {
    const FrameInfo f8 = frame(64, 80, 1, 8, 8);
    const Buf img8 = make_image(f8, 21, 2.0);
    Buf dec;
    REQUIRE(roundtrip(img8, f8, lossy(90), {}, dec).is_ok());
    const double p90 = psnr(img8, dec, f8);
    CHECK(p90 > 38.0);
    REQUIRE(roundtrip(img8, f8, lossy(50), {}, dec).is_ok());
    const double p50 = psnr(img8, dec, f8);
    CHECK(p50 > 32.0);
    CHECK(p50 < p90);
    REQUIRE(roundtrip(img8, f8, lossy(100), {}, dec).is_ok());
    CHECK(psnr(img8, dec, f8) > 45.0);

    const FrameInfo f12 = frame(48, 56, 1, 16, 12);
    const Buf img12 = make_image(f12, 22, 2.0);
    REQUIRE(roundtrip(img12, f12, lossy(90), {}, dec).is_ok());
    CHECK(psnr(img12, dec, f12) > 40.0);
    REQUIRE(roundtrip(img12, f12, lossy(100), {}, dec).is_ok());
    CHECK(psnr(img12, dec, f12) > 55.0);
    // Values must stay within 12 bits.
    bool in_range = true;
    for (std::size_t i = 0; i < dec.size(); i += 2) in_range &= load_le16(dec.data() + i) <= 4095;
    CHECK(in_range);
    // 10-bit stored data goes through the 12-bit process.
    const FrameInfo f10 = frame(16, 16, 1, 16, 10);
    CHECK(roundtrip(make_image(f10, 2), f10, lossy(80), {}, dec).is_ok());
}

TEST_CASE("jpeg: colour subsampling modes with RGB to YCbCr") {
    const FrameInfo f = frame(45, 61, 3, 8, 8);
    const Buf img = make_image(f, 8, 1.0);
    struct Mode { int h, v; double min_psnr; };
    const Mode modes[] = {{1, 1, 36.0}, {2, 1, 30.0}, {2, 2, 30.0}, {4, 1, 28.0}};
    for (const Mode& m : modes) {
        JpegEncodeOptions o;
        o.luma_h = m.h;
        o.luma_v = m.v;
        Buf dec;
        REQUIRE(roundtrip(img, f, lossy(92, true), o, dec).is_ok());
        // Decoder returns YCbCr as stored: convert back to RGB here (T.871 equations).
        Buf rgb(dec.size());
        for (std::size_t i = 0; i < dec.size(); i += 3) {
            const double Y = dec[i], cb = dec[i + 1] - 128.0, cr = dec[i + 2] - 128.0;
            const double r = Y + 1.402 * cr, g = Y - 0.344136 * cb - 0.714136 * cr, b = Y + 1.772 * cb;
            rgb[i] = static_cast<std::uint8_t>(std::clamp(std::lround(r), 0L, 255L));
            rgb[i + 1] = static_cast<std::uint8_t>(std::clamp(std::lround(g), 0L, 255L));
            rgb[i + 2] = static_cast<std::uint8_t>(std::clamp(std::lround(b), 0L, 255L));
        }
        const double p = psnr(img, rgb, f);
        if (p < m.min_psnr) std::fprintf(stderr, "  %dx%d psnr %.2f\n", m.h, m.v, p);
        CHECK(p >= m.min_psnr);
        // Same coefficients coded as one scan per component decode identically.
        o.separate_scans = true;
        Buf dec2;
        REQUIRE(roundtrip(img, f, lossy(92, true), o, dec2).is_ok());
        CHECK(dec2 == dec);
        // Planar output is the same data rearranged.
        Buf enc;
        o.separate_scans = false;
        REQUIRE(encode_jpeg_ex(img, f, lossy(92, true), o, enc).is_ok());
        FrameInfo fp = f;
        fp.planar = true;
        Buf pl(dec.size());
        REQUIRE(decode_jpeg(enc, fp, pl).is_ok());
        const std::size_t npx = static_cast<std::size_t>(f.rows) * f.columns;
        bool same = true;
        for (std::size_t px = 0; px < npx; ++px)
            for (std::size_t c = 0; c < 3; ++c) same &= pl[c * npx + px] == dec[px * 3 + c];
        CHECK(same);
    }
    // RGB coded without colour transform, 4:4:4.
    Buf dec;
    REQUIRE(roundtrip(img, f, lossy(92, false), {}, dec).is_ok());
    CHECK(psnr(img, dec, f) > 36.0);
}

TEST_CASE("jpeg: Annex K tables and optimised tables give identical pixels") {
    const FrameInfo f = frame(40, 40, 3, 8, 8);
    const Buf img = make_image(f, 77, 6.0);
    JpegEncodeOptions a, b;
    a.luma_h = a.luma_v = 2;
    b = a;
    b.optimize_huffman = false;
    Buf da, db, sa, sb;
    REQUIRE(roundtrip(img, f, lossy(75, true), a, da, &sa).is_ok());
    REQUIRE(roundtrip(img, f, lossy(75, true), b, db, &sb).is_ok());
    CHECK(da == db);
    CHECK(sa.size() <= sb.size());
    FrameInfo f12 = frame(8, 8, 1, 16, 12);
    Buf out;
    CHECK(!encode_jpeg_ex(make_image(f12, 1), f12, lossy(75), b, out).is_ok());
}

TEST_CASE("jpeg: lossy restart intervals decode like the plain stream") {
    for (int spp : {1, 3}) {
        const FrameInfo f = frame(50, 70, spp, 8, 8);
        const Buf img = make_image(f, 5, 4.0);
        JpegEncodeOptions o;
        if (spp == 3) o.luma_h = o.luma_v = 2;
        Buf ref;
        REQUIRE(roundtrip(img, f, lossy(85, true), o, ref).is_ok());
        for (std::uint16_t ri : {std::uint16_t(1), std::uint16_t(3), std::uint16_t(5), std::uint16_t(64)}) {
            o.restart_interval = ri;
            Buf dec, stream;
            CHECK(roundtrip(img, f, lossy(85, true), o, dec, &stream).is_ok());
            CHECK(dec == ref);
            o.separate_scans = true;
            CHECK(roundtrip(img, f, lossy(85, true), o, dec).is_ok());
            CHECK(dec == ref);
            o.separate_scans = false;
        }
    }
    // 12-bit with restarts
    const FrameInfo f = frame(24, 40, 1, 16, 12);
    const Buf img = make_image(f, 6, 4.0);
    Buf ref, dec;
    REQUIRE(roundtrip(img, f, lossy(85), {}, ref).is_ok());
    JpegEncodeOptions o;
    o.restart_interval = 2;
    CHECK(roundtrip(img, f, lossy(85), o, dec).is_ok());
    CHECK(dec == ref);
}

// ================================================================== header checks and markers
TEST_CASE("jpeg: SOF must agree with FrameInfo") {
    const FrameInfo f = frame(20, 30, 1, 16, 12);
    const Buf img = make_image(f, 1);
    Buf enc;
    REQUIRE(encode_jpeg(img, f, lossless(1), enc).is_ok());
    Buf out(img.size());
    REQUIRE(decode_jpeg(enc, f, out).is_ok());
    FrameInfo g = f;
    g.rows = 21;
    Buf o2(static_cast<std::size_t>(g.frame_bytes()));
    CHECK(decode_jpeg(enc, g, o2).code == Errc::TypeMismatch);
    g = f;
    g.columns = 29;
    o2.resize(static_cast<std::size_t>(g.frame_bytes()));
    CHECK(decode_jpeg(enc, g, o2).code == Errc::TypeMismatch);
    g = f;
    g.samples_per_pixel = 3;
    o2.resize(static_cast<std::size_t>(g.frame_bytes()));
    CHECK(decode_jpeg(enc, g, o2).code == Errc::TypeMismatch);
    g = f;
    g.bits_allocated = 8;
    g.bits_stored = 8;
    o2.resize(static_cast<std::size_t>(g.frame_bytes()));
    CHECK(decode_jpeg(enc, g, o2).code == Errc::TypeMismatch);
    // Wrong output size.
    Buf small(img.size() - 1);
    CHECK(!decode_jpeg(enc, f, small).is_ok());
    // P = 8 lossless into 16-bit storage is accepted.
    const FrameInfo f8 = frame(5, 6, 1, 8, 8);
    const Buf img8 = make_image(f8, 2);
    REQUIRE(encode_jpeg(img8, f8, lossless(2), enc).is_ok());
    FrameInfo w = f8;
    w.bits_allocated = 16;
    Buf o16(static_cast<std::size_t>(w.frame_bytes()));
    REQUIRE(decode_jpeg(enc, w, o16).is_ok());
    bool same = true;
    for (std::size_t i = 0; i < img8.size(); ++i) same &= load_le16(o16.data() + 2 * i) == img8[i];
    CHECK(same);
}

TEST_CASE("jpeg: APPn, COM, fill bytes and DNL are handled") {
    const FrameInfo f = frame(19, 27, 1, 8, 8);
    const Buf img = make_image(f, 9);
    for (bool lossy_mode : {false, true}) {
        Buf enc;
        REQUIRE(encode_jpeg(img, f, lossy_mode ? lossy(80) : lossless(3), enc).is_ok());
        Buf ref(img.size());
        REQUIRE(decode_jpeg(enc, f, ref).is_ok());
        // APP1 + COM after SOI, APP14 before SOS.
        Buf s = insert(enc, 2, {0xFF, 0xE1, 0x00, 0x06, 'E', 'x', 0xFF, 0xD9, 0xFF, 0xFE, 0x00, 0x05, 'h', 'i', '!'});
        const std::size_t sos = find_segment(s, 0xDA);
        REQUIRE(sos != npos);
        s = insert(s, sos, {0xFF, 0xEE, 0x00, 0x04, 0x01, 0x02});
        Buf out(img.size());
        CHECK(decode_jpeg(s, f, out).is_ok());
        CHECK(out == ref);
        // Fill bytes before markers, including before EOI.
        Buf t = insert(enc, find_segment(enc, 0xDA), {0xFF, 0xFF, 0xFF});
        t = insert(t, t.size() - 2, {0xFF, 0xFF});
        t = insert(t, 2, {0xFF});
        CHECK(decode_jpeg(t, f, out).is_ok());
        CHECK(out == ref);
        // DNL: zero rows in SOF, line count after the scan.
        Buf u = enc;
        const int sofm = lossy_mode ? 0xC0 : 0xC3;
        const std::size_t sof = find_segment(u, sofm);
        REQUIRE(sof != npos);
        u[sof + 5] = 0;
        u[sof + 6] = 0;
        u = insert(u, u.size() - 2, {0xFF, 0xDC, 0x00, 0x04, 0x00, static_cast<std::uint8_t>(f.rows)});
        std::fill(out.begin(), out.end(), 0);
        CHECK(decode_jpeg(u, f, out).is_ok());
        CHECK(out == ref);
        // DNL that disagrees, and a missing DNL.
        Buf v = u;
        v[v.size() - 3] = static_cast<std::uint8_t>(f.rows + 1);
        CHECK(!decode_jpeg(v, f, out).is_ok());
        Buf w = enc;
        w[sof + 5] = 0;
        w[sof + 6] = 0;
        CHECK(!decode_jpeg(w, f, out).is_ok());
    }
}

TEST_CASE("jpeg: unsupported processes are refused cleanly") {
    const FrameInfo f = frame(8, 8, 1, 8, 8);
    Buf enc;
    REQUIRE(encode_jpeg(make_image(f, 3), f, lossy(80), enc).is_ok());
    const std::size_t sof = find_segment(enc, 0xC0);
    REQUIRE(sof != npos);
    Buf out(64);
    for (int m : {0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}) {
        Buf s = enc;
        s[sof + 1] = static_cast<std::uint8_t>(m);
        CHECK(decode_jpeg(s, f, out).code == Errc::Unsupported);
    }
    Buf s = enc;
    s[sof + 1] = 0xF7;  // JPEG-LS SOF55
    CHECK(decode_jpeg(s, f, out).code == Errc::Unsupported);
    // Baseline must be 8-bit precision.
    s = enc;
    s[sof + 4] = 12;
    CHECK(!decode_jpeg(s, f, out).is_ok());
    CHECK(!decode_jpeg(Buf{0xFF, 0xD8}, f, out).is_ok());
    CHECK(!decode_jpeg(Buf{}, f, out).is_ok());
}
