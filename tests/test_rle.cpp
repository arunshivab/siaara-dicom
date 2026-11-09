// RLE Lossless (DICOM PS3.5 Annex G) tests: round trips, known streams, header checks, robustness.
#include <cstring>
#include <random>
#include <vector>

#include "codec/codec.hpp"
#include "core/types.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::codec;

namespace {

using Buf = std::vector<std::uint8_t>;

FrameInfo frame(std::uint32_t rows, std::uint32_t cols, std::uint16_t spp, std::uint16_t bits, bool planar = false) {
    FrameInfo fi;
    fi.rows = rows;
    fi.columns = cols;
    fi.samples_per_pixel = spp;
    fi.bits_allocated = bits;
    fi.bits_stored = bits;
    fi.planar = planar;
    return fi;
}

Buf pattern(const FrameInfo& fi, int kind, std::uint64_t seed) {
    Buf b(static_cast<std::size_t>(static_cast<std::size_t>(fi.frame_bytes())));
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < b.size(); ++i) {
        switch (kind) {
            case 0: b[i] = static_cast<std::uint8_t>(rng()); break;                  // random
            case 1: b[i] = 0; break;                                                  // zeros
            case 2: b[i] = static_cast<std::uint8_t>((i / 37) % 3 == 0 ? rng() : 5); break;  // runs + noise
            default: b[i] = static_cast<std::uint8_t>(i * 13 + (i >> 9)); break;       // ramp
        }
    }
    return b;
}

// Checks the encoder's structural promises: header count and offsets, even-length segments,
// and runs that never cross a row boundary (PS3.5 G.3.1).
bool well_formed(const Buf& z, const FrameInfo& fi) {
    const std::uint32_t segs = static_cast<std::uint32_t>(fi.samples_per_pixel) * (fi.bits_allocated / 8u);
    if (z.size() < 64 || z.size() % 2 || load_le32(z.data()) != segs) return false;
    for (std::uint32_t s = 0; s < 15; ++s) {
        const std::uint32_t off = load_le32(z.data() + 4 + 4 * s);
        if (s >= segs) {
            if (off != 0) return false;
            continue;
        }
        if (off % 2 || (s == 0 && off != 64)) return false;
        const std::size_t end = s + 1 < segs ? load_le32(z.data() + 8 + 4 * s) : z.size();
        std::size_t i = off, produced = 0;
        while (produced < static_cast<std::size_t>(fi.rows) * fi.columns) {
            if (i >= end) return false;
            const std::uint8_t h = z[i++];
            std::size_t n = 0;
            if (h < 128) {
                n = h + 1u;
                i += n;
            } else if (h > 128) {
                n = 257u - h;
                ++i;
            } else {
                return false;  // the encoder never emits no-ops
            }
            if (produced / fi.columns != (produced + n - 1) / fi.columns) return false;
            produced += n;
        }
        if (i > end || end - i > 1) return false;  // at most the pad byte follows
    }
    return true;
}

bool roundtrip(const FrameInfo& fi, const Buf& raw, bool* wf = nullptr) {
    Buf z;
    if (!encode_rle(raw, fi, EncodeParams{}, z)) return false;
    if (wf) *wf = well_formed(z, fi);
    Buf out(raw.size(), 0xCC);
    if (!decode_rle(z, fi, out)) return false;
    return out == raw;
}

Buf header(std::initializer_list<std::uint32_t> words) {
    Buf h(64, 0);
    std::size_t i = 0;
    for (std::uint32_t w : words) store_le32(h.data() + 4 * i++, w);
    return h;
}

}  // namespace

TEST_CASE("rle: round trips for 8/16/32 bits, 1 and 3 samples, planar 0 and 1") {
    int failures = 0, malformed = 0, cases = 0;
    const std::uint32_t sizes[][2] = {{1, 1}, {1, 2}, {2, 1}, {3, 5}, {17, 33}, {64, 129}, {128, 128}};
    for (std::uint16_t bits : {std::uint16_t{8}, std::uint16_t{16}, std::uint16_t{32}})
        for (std::uint16_t spp : {std::uint16_t{1}, std::uint16_t{3}})
            for (bool planar : {false, true})
                for (const auto& sz : sizes)
                    for (int kind = 0; kind < 4; ++kind) {
                        const FrameInfo fi = frame(sz[0], sz[1], spp, bits, planar);
                        bool wf = false;
                        if (!roundtrip(fi, pattern(fi, kind, static_cast<std::uint64_t>(cases)), &wf)) ++failures;
                        if (!wf) ++malformed;
                        ++cases;
                    }
    CHECK_EQ(failures, 0);
    CHECK_EQ(malformed, 0);
    CHECK_EQ(cases, 3 * 2 * 2 * 7 * 4);
}

TEST_CASE("rle: large frame and long rows") {
    const FrameInfo fi = frame(512, 512, 1, 16);
    CHECK(roundtrip(fi, pattern(fi, 2, 1)));
    CHECK(roundtrip(fi, pattern(fi, 0, 2)));
    const FrameInfo wide = frame(3, 1000, 3, 8);  // rows longer than one 128-byte run
    bool wf = false;
    CHECK(roundtrip(wide, pattern(wide, 1, 3), &wf));
    CHECK(wf);
    CHECK(roundtrip(wide, pattern(wide, 0, 4), &wf));
    CHECK(wf);
}

TEST_CASE("rle: decodes a hand-built 8-bit stream") {
    // FE 07: 7 x3; 02 01 02 03: literal 1 2 3; 80: no-op; 00 09: literal 9; one pad byte.
    Buf z = header({1, 64});
    z.insert(z.end(), {0xFE, 0x07, 0x02, 0x01, 0x02, 0x03, 0x80, 0x00, 0x09, 0x00});
    Buf out(7);
    REQUIRE(decode_rle(z, frame(1, 7, 1, 8), out).is_ok());
    CHECK(out == Buf({7, 7, 7, 1, 2, 3, 9}));
}

TEST_CASE("rle: segment order is most significant byte first, per sample") {
    {  // 16-bit, one sample: segment 1 holds the high bytes, output is little endian.
        Buf z = header({2, 64, 68});
        z.insert(z.end(), {0x01, 0x12, 0xAB, 0x00, 0x01, 0x34, 0xCD, 0x00});
        Buf out(4);
        REQUIRE(decode_rle(z, frame(1, 2, 1, 16), out).is_ok());
        CHECK(out == Buf({0x34, 0x12, 0xCD, 0xAB}));
    }
    // RGB 8-bit, two pixels (10,20,30) and (11,21,31): segments R, G, B.
    Buf z = header({3, 64, 68, 72});
    z.insert(z.end(), {0x01, 10, 11, 0x00, 0x01, 20, 21, 0x00, 0x01, 30, 31, 0x00});
    Buf out(6);
    REQUIRE(decode_rle(z, frame(1, 2, 3, 8, false), out).is_ok());
    CHECK(out == Buf({10, 20, 30, 11, 21, 31}));
    REQUIRE(decode_rle(z, frame(1, 2, 3, 8, true), out).is_ok());
    CHECK(out == Buf({10, 11, 20, 21, 30, 31}));
    // 32-bit, one sample: four segments from most to least significant.
    Buf w = header({4, 64, 66, 68, 70});
    w.insert(w.end(), {0x00, 0x11, 0x00, 0x22, 0x00, 0x33, 0x00, 0x44});
    Buf o4(4);
    REQUIRE(decode_rle(w, frame(1, 1, 1, 32), o4).is_ok());
    CHECK(o4 == Buf({0x44, 0x33, 0x22, 0x11}));
}

TEST_CASE("rle: header must agree with the frame") {
    const FrameInfo fi = frame(4, 4, 1, 16);
    Buf good;
    REQUIRE(encode_rle(pattern(fi, 2, 9), fi, EncodeParams{}, good).is_ok());
    Buf out(static_cast<std::size_t>(static_cast<std::size_t>(fi.frame_bytes())));
    REQUIRE(decode_rle(good, fi, out).is_ok());

    Buf z = good;
    store_le32(z.data(), 1);  // segment count disagrees with samples x bytes
    CHECK(decode_rle(z, fi, out).code == Errc::Malformed);
    store_le32(z.data(), 3);
    CHECK(decode_rle(z, fi, out).code == Errc::Malformed);
    {
        Buf o8(16);
        CHECK(decode_rle(good, frame(4, 4, 1, 8), o8).code == Errc::Malformed);
        Buf o3(96);
        CHECK(decode_rle(good, frame(4, 4, 3, 16), o3).code == Errc::Malformed);
    }
    z = good;
    store_le32(z.data() + 4, 60);  // first segment inside the header
    CHECK(decode_rle(z, fi, out).code == Errc::Malformed);
    z = good;
    store_le32(z.data() + 8, 64);  // second segment not after the first
    CHECK(decode_rle(z, fi, out).code == Errc::Malformed);
    z = good;
    store_le32(z.data() + 8, static_cast<std::uint32_t>(z.size() + 1));  // past the end of the data
    CHECK(decode_rle(z, fi, out).code == Errc::Malformed);
    z = good;
    store_le32(z.data() + 8, 0xFFFFFFFFu);
    CHECK(decode_rle(z, fi, out).code == Errc::Malformed);
    CHECK(decode_rle(Buf(good.begin(), good.begin() + 63), fi, out).code == Errc::Truncated);
    CHECK(decode_rle(Buf(), fi, out).code == Errc::Truncated);
}

TEST_CASE("rle: short segments and short output fail") {
    const FrameInfo fi = frame(8, 8, 3, 8);
    Buf z;
    REQUIRE(encode_rle(pattern(fi, 0, 5), fi, EncodeParams{}, z).is_ok());
    Buf out(static_cast<std::size_t>(static_cast<std::size_t>(fi.frame_bytes())));
    int accepted = 0;
    for (std::size_t n = 64; n < z.size() - 1; ++n) {  // the last byte may be padding
        if (decode_rle(Buf(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(n)), fi, out)) ++accepted;
    }
    CHECK_EQ(accepted, 0);
    // A segment that ends early: one replicate run of 3 for a 1x7 frame.
    Buf s = header({1, 64});
    s.insert(s.end(), {0xFE, 0x07});
    Buf o7(7);
    CHECK(decode_rle(s, frame(1, 7, 1, 8), o7).code == Errc::Truncated);
    // A literal run whose bytes are missing.
    Buf l = header({1, 64});
    l.insert(l.end(), {0x06, 1, 2});
    CHECK(decode_rle(l, frame(1, 7, 1, 8), o7).code == Errc::Truncated);
    // Output buffer of the wrong size.
    Buf small(out.size() - 1), big(out.size() + 1);
    CHECK(decode_rle(z, fi, small).code == Errc::BufferTooSmall);
    CHECK(decode_rle(z, fi, big).code == Errc::InvalidArgument);
}

TEST_CASE("rle: runs past the frame are cut, never written outside the output") {
    // 1x10 frame, a replicate run of 128 then a literal run of 128 with all its bytes.
    Buf z = header({1, 64});
    z.insert(z.end(), {0x81, 0x55});
    z.push_back(0x7F);
    for (int i = 0; i < 128; ++i) z.push_back(static_cast<std::uint8_t>(i));
    Buf guard(10 + 64, 0xEE);
    std::span<std::uint8_t> out(guard.data() + 32, 10);
    REQUIRE(decode_rle(z, frame(1, 10, 1, 8), out).is_ok());
    for (std::size_t i = 0; i < guard.size(); ++i) {
        const bool inside = i >= 32 && i < 42;
        CHECK(guard[i] == (inside ? 0x55 : 0xEE));
    }
}

TEST_CASE("rle: corrupted streams never crash or write outside the output") {
    std::mt19937_64 rng(31);
    int escaped = 0;
    for (int iter = 0; iter < 2000; ++iter) {
        const auto bits = static_cast<std::uint16_t>(iter % 3 == 0 ? 8 : iter % 3 == 1 ? 16 : 32);
        const auto spp = static_cast<std::uint16_t>((iter & 4) ? 3 : 1);
        const FrameInfo fi = frame(1 + static_cast<std::uint32_t>(static_cast<std::size_t>(rng()) % 9), 1 + static_cast<std::uint32_t>(static_cast<std::size_t>(rng()) % 40),
                                   spp, bits, (iter & 8) != 0);
        Buf z;
        REQUIRE(encode_rle(pattern(fi, static_cast<int>(static_cast<std::size_t>(rng()) % 4), rng()), fi, EncodeParams{}, z).is_ok());
        const int flips = 1 + static_cast<int>(static_cast<std::size_t>(rng()) % 6);
        for (int f = 0; f < flips; ++f) z[static_cast<std::size_t>(static_cast<std::size_t>(rng()) % z.size())] ^= static_cast<std::uint8_t>(1u << (static_cast<std::size_t>(rng()) % 8));
        if (static_cast<std::size_t>(rng()) % 4 == 0) z.resize(static_cast<std::size_t>(rng()) % (z.size() + 1));
        const std::size_t n = static_cast<std::size_t>(static_cast<std::size_t>(fi.frame_bytes()));
        Buf guard(n + 64, 0xA5);
        (void)decode_rle(z, fi, std::span<std::uint8_t>(guard.data() + 32, n));
        for (std::size_t i = 0; i < 32; ++i)
            if (guard[i] != 0xA5 || guard[n + 32 + i] != 0xA5) ++escaped;
    }
    CHECK_EQ(escaped, 0);
}

TEST_CASE("rle: unsupported and invalid parameters") {
    Buf z, out(8);
    const Buf raw(8, 1);
    CHECK(encode_rle(raw, frame(2, 2, 1, 12), EncodeParams{}, z).code == Errc::Unsupported);
    CHECK(encode_rle(raw, frame(2, 2, 1, 1), EncodeParams{}, z).code == Errc::Unsupported);
    CHECK(encode_rle(Buf(4 * 2 * 8), frame(2, 2, 2, 64), EncodeParams{}, z).code == Errc::Unsupported);  // 16 segments
    CHECK(encode_rle(raw, frame(0, 2, 1, 8), EncodeParams{}, z).code == Errc::InvalidArgument);
    CHECK(encode_rle(raw, frame(2, 2, 1, 8), EncodeParams{}, z).code == Errc::InvalidArgument);  // 8 bytes for 4
    CHECK(decode_rle(Buf(80), frame(2, 2, 1, 12), out).code == Errc::Unsupported);
    // 32-bit RGB needs 12 segments, 64-bit one-sample needs 8: both within the 15 allowed.
    const FrameInfo rgb32 = frame(5, 7, 3, 32, true);
    CHECK(roundtrip(rgb32, pattern(rgb32, 2, 1)));
    const FrameInfo f64 = frame(5, 7, 1, 64);
    CHECK(roundtrip(f64, pattern(f64, 3, 1)));
}
