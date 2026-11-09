// JPEG-LS codec tests: round trips, near-lossless error bounds, interleave modes, presets,
// restart intervals, colour transforms, header checks, known answers from an independent encoder,
// truncation and corruption robustness.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/jpegls.hpp"
#include "core/types.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::codec;

namespace {

// Known-answer vectors generated with CharLS (pyjpegls 1.5.1); used only as test data.
// kCharlsGray8: 16x12 8-bit lossless; kCharlsRgbNear2: 8x6 RGB, NEAR=2, ILV 2 (pixels are
// CharLS's own decode); kCharlsGray12: 8x12 12-bit lossless.
const std::uint8_t kCharlsGray8[] = {
    0xFF, 0xD8, 0xFF, 0xF7, 0x00, 0x0B, 0x08, 0x00, 0x0C, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00, 0xFF,
    0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0xA0, 0x50, 0x2A, 0xE7, 0x6D, 0x6A,
    0x52, 0x80, 0x00, 0x25, 0x54, 0xD1, 0xF8, 0xC3, 0x31, 0x43, 0x4F, 0x46, 0x47, 0x60, 0xD9, 0x9B,
    0x69, 0xA0, 0x09, 0x00, 0x35, 0x06, 0xDB, 0x09, 0xB4, 0x92, 0xF8, 0xE9, 0x23, 0x55, 0x0C, 0xBA,
    0xEA, 0x91, 0x54, 0x28, 0xD9, 0x75, 0x5D, 0x50, 0x00, 0x00, 0x0D, 0x60, 0x00, 0x00, 0x0E, 0x30,
    0xD6, 0xED, 0xB3, 0x6C, 0xC0, 0x00, 0x00, 0x1B, 0xA0, 0x2B, 0x2D, 0xDB, 0x66, 0xDB, 0x61, 0x46,
    0xB7, 0x6D, 0x93, 0x6D, 0x96, 0xCB, 0x76, 0xD9, 0xB6, 0xD9, 0x2A, 0xB7, 0x6D, 0xFB, 0xB6, 0xDA,
    0x8B, 0x76, 0x9E, 0x05, 0xFF, 0xD9,
};

const std::uint8_t kCharlsGray8Pixels[] = {
    0x00, 0x0D, 0x1A, 0x27, 0x34, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x8F, 0x9C, 0xA9, 0xB6, 0xC3,
    0x07, 0x14, 0x21, 0x2E, 0x3B, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x96, 0xA3, 0xB0, 0xBD, 0xCA,
    0x0E, 0x1B, 0x28, 0x35, 0x42, 0x4D, 0x4D, 0x4D, 0x03, 0x4D, 0x4D, 0x9D, 0xAA, 0xB7, 0xC4, 0xD1,
    0x15, 0x22, 0x2F, 0x3C, 0x49, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xA4, 0xB1, 0xBE, 0xCB, 0xD8,
    0x1C, 0x29, 0x36, 0x43, 0x50, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xAB, 0xB8, 0xC5, 0xD2, 0xDF,
    0x23, 0x30, 0x3D, 0x4A, 0x57, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xB2, 0xBF, 0xCC, 0xD9, 0xE6,
    0x2A, 0x37, 0x44, 0xFA, 0x5E, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xB9, 0xC6, 0xD3, 0xE0, 0xED,
    0x31, 0x3E, 0x4B, 0x58, 0x65, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xC0, 0xCD, 0xDA, 0xE7, 0xF4,
    0x38, 0x45, 0x52, 0x5F, 0x6C, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xC7, 0xD4, 0xE1, 0xEE, 0xFB,
    0x3F, 0x4C, 0x59, 0x66, 0x73, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xCE, 0xDB, 0xE8, 0xF5, 0x02,
    0x46, 0x53, 0x60, 0x6D, 0x7A, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xD5, 0xE2, 0xEF, 0xFC, 0x09,
    0x4D, 0x5A, 0x67, 0x74, 0x81, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D, 0xDC, 0xE9, 0xF6, 0x03, 0x10,
};

const std::uint8_t kCharlsRgbNear2[] = {
    0xFF, 0xD8, 0xFF, 0xF7, 0x00, 0x11, 0x08, 0x00, 0x06, 0x00, 0x08, 0x03, 0x01, 0x11, 0x00, 0x02,
    0x11, 0x00, 0x03, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
    0x02, 0x02, 0x00, 0x51, 0x02, 0x90, 0x39, 0x07, 0xCF, 0x00, 0x00, 0x00, 0x2D, 0xA6, 0xA3, 0xD0,
    0x5B, 0x40, 0x00, 0x1A, 0xD6, 0x5D, 0x65, 0xD6, 0x56, 0xA0, 0x15, 0xAD, 0xD5, 0x96, 0x77, 0xD2,
    0x2C, 0xD7, 0x6B, 0xBB, 0x9B, 0x9E, 0xF7, 0xDD, 0xDA, 0xC8, 0xFF, 0x5D, 0xAF, 0xB0, 0xFF, 0xD9,
};

const std::uint8_t kCharlsRgbNear2Pixels[] = {
    0x00, 0x00, 0x0A, 0x1E, 0x01, 0x0A, 0x3C, 0x02, 0x0A, 0x5A, 0x00, 0x0A, 0x79, 0x00, 0xC8, 0x94,
    0x00, 0xC6, 0xB4, 0x00, 0xC9, 0xD0, 0x00, 0xC8, 0x00, 0x28, 0x0A, 0x1E, 0x27, 0x0B, 0x3C, 0x2A,
    0x0C, 0x5A, 0x26, 0x0C, 0x79, 0x29, 0xC8, 0x94, 0x27, 0xC6, 0xB4, 0x29, 0xC9, 0xD0, 0x26, 0xC9,
    0x00, 0x50, 0x0B, 0x1E, 0x4F, 0x0C, 0x3C, 0x4F, 0x08, 0x5A, 0x50, 0x08, 0x79, 0x50, 0xC9, 0x94,
    0x50, 0xC7, 0xB4, 0x50, 0xC9, 0xD0, 0x52, 0xCA, 0x00, 0x79, 0x0C, 0x1E, 0x7A, 0x0C, 0x3C, 0x77,
    0x08, 0x5A, 0x79, 0x08, 0x79, 0x76, 0xC9, 0x94, 0x77, 0xC8, 0xB4, 0x78, 0xCA, 0xD0, 0x79, 0xC6,
    0x00, 0x9E, 0x08, 0x1E, 0x9E, 0x08, 0x3C, 0xA1, 0x08, 0x5A, 0xA1, 0x08, 0x79, 0xA0, 0xC9, 0x94,
    0xA1, 0xC9, 0xB4, 0xA2, 0xCA, 0xD0, 0x9E, 0xC6, 0x00, 0xC9, 0x08, 0x1E, 0xC9, 0x09, 0x3C, 0xC9,
    0x0A, 0x5A, 0xCA, 0x0A, 0x79, 0xCA, 0xC9, 0x94, 0xC6, 0xCA, 0xB4, 0xC6, 0xCA, 0xD0, 0xC9, 0xC6,
};

const std::uint8_t kCharlsGray12[] = {
    0xFF, 0xD8, 0xFF, 0xF7, 0x00, 0x0B, 0x0C, 0x00, 0x0C, 0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xFF,
    0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x4F, 0x00, 0x00,
    0x00, 0x01, 0x3D, 0xF3, 0x5F, 0x2D, 0xF2, 0x5F, 0x1D, 0xF1, 0x40, 0x00, 0x00, 0x00, 0x09, 0xE0,
    0x00, 0x00, 0x00, 0x28, 0x3E, 0x74, 0x08, 0x00, 0x00, 0x00, 0x00, 0x04, 0xF7, 0xCC, 0x80, 0x49,
    0x58, 0x09, 0x1A, 0xF9, 0x90, 0x40, 0x48, 0xB0, 0x00, 0x00, 0x00, 0x14, 0x09, 0x13, 0x04, 0x09,
    0x0E, 0xF9, 0x50, 0x81, 0x23, 0x89, 0x0A, 0xF9, 0x10, 0x81, 0x20, 0xDF, 0x1A, 0x10, 0x12, 0x78,
    0x90, 0x6F, 0x89, 0x08, 0x12, 0x05, 0xF0, 0xA0, 0x82, 0x10, 0x20, 0x81, 0x04, 0x10, 0x80, 0x49,
    0x27, 0xBF, 0x25, 0x13, 0xE0, 0x3D, 0xF3, 0xDF, 0x92, 0x89, 0xF3, 0x08, 0xFE, 0x4A, 0x27, 0xBC,
    0x7B, 0xA7, 0xBF, 0x25, 0x13, 0xDC, 0x3E, 0x51, 0x28, 0x1E, 0xD9, 0xED, 0x1F, 0x04, 0x94, 0x0F,
    0x64, 0xF6, 0x0F, 0x90, 0x46, 0xE7, 0xAC, 0x7C, 0x12, 0x50, 0x3D, 0x53, 0xD4, 0x3E, 0x09, 0x26,
    0x48, 0xCD, 0x04, 0xA0, 0x7A, 0x47, 0xA2, 0x7C, 0x12, 0x50, 0x3E, 0x40, 0xFF, 0xD9,
};

const std::uint8_t kCharlsGray12Pixels[] = {
    0x00, 0x00, 0xE8, 0x03, 0xD0, 0x07, 0xB8, 0x0B, 0xA0, 0x0F, 0x88, 0x03, 0x70, 0x07, 0x58, 0x0B,
    0xB8, 0x0B, 0xA0, 0x0F, 0x88, 0x03, 0x70, 0x07, 0x58, 0x0B, 0x40, 0x0F, 0x28, 0x03, 0x10, 0x07,
    0x70, 0x07, 0x58, 0x0B, 0x40, 0x0F, 0x28, 0x03, 0x10, 0x07, 0xF8, 0x0A, 0xE0, 0x0E, 0xC8, 0x02,
    0x28, 0x03, 0x10, 0x07, 0xF8, 0x0A, 0xE0, 0x0E, 0xC8, 0x02, 0xB0, 0x06, 0x98, 0x0A, 0x80, 0x0E,
    0xE0, 0x0E, 0xC8, 0x02, 0xB0, 0x06, 0x98, 0x0A, 0x80, 0x0E, 0x68, 0x02, 0x50, 0x06, 0x38, 0x0A,
    0x98, 0x0A, 0x80, 0x0E, 0x68, 0x02, 0x50, 0x06, 0x38, 0x0A, 0x20, 0x0E, 0x08, 0x02, 0xF0, 0x05,
    0x98, 0x0A, 0x80, 0x0E, 0x68, 0x02, 0x50, 0x06, 0x38, 0x0A, 0x20, 0x0E, 0x08, 0x02, 0xF0, 0x05,
    0xE0, 0x0E, 0xC8, 0x02, 0xB0, 0x06, 0x98, 0x0A, 0x80, 0x0E, 0x68, 0x02, 0x50, 0x06, 0x38, 0x0A,
    0x28, 0x03, 0x10, 0x07, 0xF8, 0x0A, 0xE0, 0x0E, 0xC8, 0x02, 0xB0, 0x06, 0x98, 0x0A, 0x80, 0x0E,
    0x70, 0x07, 0x58, 0x0B, 0x40, 0x0F, 0x28, 0x03, 0x10, 0x07, 0xF8, 0x0A, 0xE0, 0x0E, 0xC8, 0x02,
    0xB8, 0x0B, 0xA0, 0x0F, 0x88, 0x03, 0x70, 0x07, 0x58, 0x0B, 0x40, 0x0F, 0x28, 0x03, 0x10, 0x07,
    0x00, 0x00, 0xE8, 0x03, 0xD0, 0x07, 0xB8, 0x0B, 0xA0, 0x0F, 0x88, 0x03, 0x70, 0x07, 0x58, 0x0B,
};


FrameInfo make_info(std::uint32_t rows, std::uint32_t cols, int spp, int bits, bool planar = false) {
    FrameInfo fi;
    fi.rows = rows;
    fi.columns = cols;
    fi.samples_per_pixel = static_cast<std::uint16_t>(spp);
    fi.bits_stored = static_cast<std::uint16_t>(bits);
    fi.bits_allocated = bits > 8 ? 16 : 8;
    fi.planar = planar;
    return fi;
}

std::size_t bytes_per_sample(const FrameInfo& fi) { return fi.bits_allocated / 8u; }

int get_sample(const std::vector<std::uint8_t>& buf, const FrameInfo& fi, std::size_t i) {
    return bytes_per_sample(fi) == 1 ? buf[i] : load_le16(&buf[2 * i]);
}

void set_sample(std::vector<std::uint8_t>& buf, const FrameInfo& fi, std::size_t i, int v) {
    if (bytes_per_sample(fi) == 1) buf[i] = static_cast<std::uint8_t>(v);
    else store_le16(&buf[2 * i], static_cast<std::uint16_t>(v));
}

// Native-layout index of sample (y, x, c).
std::size_t index_of(const FrameInfo& fi, std::size_t y, std::size_t x, std::size_t c) {
    const std::size_t cols = fi.columns, nc = fi.samples_per_pixel;
    return fi.planar ? (c * fi.rows + y) * cols + x : (y * cols + x) * nc + c;
}

enum class Kind { Gradient, Noise, Flat, Edges, Smooth };

std::vector<std::uint8_t> synthetic(const FrameInfo& fi, Kind kind, std::uint32_t seed) {
    std::mt19937 rng(seed);
    const int maxval = (1 << fi.bits_stored) - 1;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(fi.frame_bytes()));
    for (std::size_t y = 0; y < fi.rows; ++y) {
        for (std::size_t x = 0; x < fi.columns; ++x) {
            for (std::size_t c = 0; c < fi.samples_per_pixel; ++c) {
                long v = 0;
                switch (kind) {
                    case Kind::Gradient:
                        v = static_cast<long>((x * (c + 1) * 37 + y * 11) % static_cast<std::size_t>(maxval + 1));
                        break;
                    case Kind::Noise:
                        v = static_cast<long>(static_cast<std::size_t>(rng()) % static_cast<std::uint32_t>(maxval + 1));
                        break;
                    case Kind::Flat:
                        v = (static_cast<std::size_t>(rng()) % 50 == 0) ? static_cast<long>(static_cast<std::size_t>(rng()) % static_cast<std::uint32_t>(maxval + 1))
                                              : maxval / 3 + static_cast<long>(c);
                        break;
                    case Kind::Edges:
                        v = (((x / 5) + (y / 3) + c) % 2) ? maxval / 6 : maxval - maxval / 9;
                        break;
                    case Kind::Smooth:
                        v = maxval / 2 + static_cast<long>((static_cast<double>(maxval) / 3.0) *
                                                           std::sin(static_cast<double>(x) / 7.0 + static_cast<double>(y) / 11.0)) +
                            static_cast<long>(static_cast<std::size_t>(rng()) % 3) - 1;
                        break;
                }
                v = std::clamp(v, 0L, static_cast<long>(maxval));
                set_sample(buf, fi, index_of(fi, y, x, c), static_cast<int>(v));
            }
        }
    }
    return buf;
}

int max_abs_error(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, const FrameInfo& fi) {
    const std::size_t n = static_cast<std::size_t>(fi.frame_bytes()) / bytes_per_sample(fi);
    int worst = 0;
    for (std::size_t i = 0; i < n; ++i) worst = std::max(worst, std::abs(get_sample(a, fi, i) - get_sample(b, fi, i)));
    return worst;
}

Status encode_with(const std::vector<std::uint8_t>& px, const FrameInfo& fi, int near, int ilv,
                   std::vector<std::uint8_t>& out, std::uint32_t restart = 0) {
    jpegls::EncodeOptions opt;
    opt.near = near;
    opt.interleave = ilv;
    opt.restart_interval = restart;
    return jpegls::encode(px, fi, opt, out);
}

Status decode_to(const std::vector<std::uint8_t>& stream, const FrameInfo& fi, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(fi.frame_bytes()), 0xCD);
    return decode_jpegls(stream, fi, out);
}

// Decodes into a buffer with guard bytes on both sides and checks they are untouched.
bool decode_guarded(const std::vector<std::uint8_t>& stream, const FrameInfo& fi, Status* status = nullptr) {
    const std::size_t n = static_cast<std::size_t>(fi.frame_bytes());
    std::vector<std::uint8_t> buf(n + 64, 0x5A);
    const std::vector<std::uint8_t> in(stream.begin(), stream.end());  // exact size: ASan sees over-reads
    Status s = decode_jpegls(in, fi, std::span<std::uint8_t>(buf.data() + 32, n));
    if (status) *status = s;
    for (std::size_t i = 0; i < 32; ++i)
        if (buf[i] != 0x5A || buf[n + 32 + i] != 0x5A) return false;
    return true;
}

const Kind kAllKinds[] = {Kind::Gradient, Kind::Noise, Kind::Flat, Kind::Edges, Kind::Smooth};

}  // namespace

TEST_CASE("jpegls: lossless round trip, 1 component, 2..16 bits, all image kinds") {
    std::uint32_t seed = 1;
    for (int bits : {2, 3, 5, 8, 10, 12, 15, 16}) {
        for (Kind kind : kAllKinds) {
            const std::uint32_t dims[][2] = {{23, 37}, {1, 1}, {1, 19}, {17, 1}, {64, 64}};
            for (const auto& d : dims) {
                const FrameInfo fi = make_info(d[0], d[1], 1, bits);
                const auto px = synthetic(fi, kind, seed++);
                std::vector<std::uint8_t> stream, back;
                REQUIRE(encode_with(px, fi, 0, 0, stream).is_ok());
                REQUIRE(decode_to(stream, fi, back).is_ok());
                CHECK(back == px);
            }
        }
    }
}

TEST_CASE("jpegls: near-lossless error bound per sample, NEAR 0..10") {
    std::uint32_t seed = 100;
    for (int bits : {2, 4, 8, 12, 16}) {
        const int maxval = (1 << bits) - 1;
        for (int near = 0; near <= 10 && near <= maxval / 2; ++near) {
            for (Kind kind : kAllKinds) {
                const FrameInfo fi = make_info(19, 31, 1, bits);
                const auto px = synthetic(fi, kind, seed++);
                std::vector<std::uint8_t> stream, back;
                REQUIRE(encode_with(px, fi, near, 0, stream).is_ok());
                REQUIRE(decode_to(stream, fi, back).is_ok());
                CHECK(max_abs_error(px, back, fi) <= near);
                if (near == 0) CHECK(back == px);
            }
        }
    }
}

TEST_CASE("jpegls: colour round trips in ILV 0, 1 and 2, lossless and near-lossless") {
    std::uint32_t seed = 300;
    for (int spp : {2, 3, 4}) {
        for (int bits : {2, 8, 12, 16}) {
            for (int ilv = 0; ilv <= 2; ++ilv) {
                for (int near : {0, 1, 3, 10}) {
                    if (near > ((1 << bits) - 1) / 2) continue;
                    for (Kind kind : kAllKinds) {
                        const FrameInfo fi = make_info(13, 29, spp, bits);
                        const auto px = synthetic(fi, kind, seed++);
                        std::vector<std::uint8_t> stream, back;
                        REQUIRE(encode_with(px, fi, near, ilv, stream).is_ok());
                        jpegls::StreamInfo si;
                        REQUIRE(jpegls::probe(stream, si).is_ok());
                        CHECK(si.interleave == ilv);
                        CHECK(si.near == near);
                        REQUIRE(decode_to(stream, fi, back).is_ok());
                        CHECK(max_abs_error(px, back, fi) <= near);
                    }
                }
            }
        }
    }
}

TEST_CASE("jpegls: planar layouts on input and output") {
    const FrameInfo pixel = make_info(21, 18, 3, 8, false);
    FrameInfo planar = pixel;
    planar.planar = true;
    const auto px = synthetic(pixel, Kind::Smooth, 7);
    for (int ilv = 0; ilv <= 2; ++ilv) {
        std::vector<std::uint8_t> stream, back;
        REQUIRE(encode_with(px, pixel, 0, ilv, stream).is_ok());
        REQUIRE(decode_to(stream, planar, back).is_ok());
        bool same = true;
        for (std::size_t y = 0; y < 21; ++y)
            for (std::size_t x = 0; x < 18; ++x)
                for (std::size_t c = 0; c < 3; ++c)
                    same = same && back[index_of(planar, y, x, c)] == px[index_of(pixel, y, x, c)];
        CHECK(same);
        // And planar input back to pixel order.
        std::vector<std::uint8_t> stream2, back2;
        REQUIRE(encode_with(back, planar, 0, ilv, stream2).is_ok());
        REQUIRE(decode_to(stream2, pixel, back2).is_ok());
        CHECK(back2 == px);
    }
}

TEST_CASE("jpegls: flat images are coded in run mode") {
    const FrameInfo fi = make_info(256, 256, 1, 16);
    std::vector<std::uint8_t> px(static_cast<std::size_t>(fi.frame_bytes()));
    for (std::size_t i = 0; i < px.size(); i += 2) store_le16(&px[i], 1234);
    std::vector<std::uint8_t> stream, back;
    REQUIRE(encode_with(px, fi, 0, 0, stream).is_ok());
    CHECK(stream.size() < 600);  // 131072 bytes of samples: only possible with run mode
    REQUIRE(decode_to(stream, fi, back).is_ok());
    CHECK(back == px);
    // Runs broken by single samples, at line ends and mid-line.
    for (std::size_t y = 0; y < 256; y += 3) store_le16(&px[2 * (y * 256 + (y * 7) % 256)], 40000);
    REQUIRE(encode_with(px, fi, 0, 0, stream).is_ok());
    REQUIRE(decode_to(stream, fi, back).is_ok());
    CHECK(back == px);
}

TEST_CASE("jpegls: restart intervals") {
    std::uint32_t seed = 900;
    for (int spp : {1, 3}) {
        for (int ilv = 0; ilv <= 2; ++ilv) {
            if (spp == 1 && ilv > 0) continue;
            for (std::uint32_t interval : {1u, 2u, 7u, 40u, 70000u}) {
                for (int near : {0, 2}) {
                    const FrameInfo fi = make_info(40, 33, spp, 12);
                    const auto px = synthetic(fi, Kind::Flat, seed++);
                    std::vector<std::uint8_t> stream, back;
                    REQUIRE(encode_with(px, fi, near, ilv, stream, interval).is_ok());
                    REQUIRE(decode_to(stream, fi, back).is_ok());
                    CHECK(max_abs_error(px, back, fi) <= near);
                }
            }
        }
    }
    // A restart marker with the wrong index is rejected.
    const FrameInfo fi = make_info(8, 8, 1, 8);
    const auto px = synthetic(fi, Kind::Gradient, 5);
    std::vector<std::uint8_t> stream, back;
    REQUIRE(encode_with(px, fi, 0, 0, stream, 2).is_ok());
    bool patched = false;
    for (std::size_t i = 0; i + 1 < stream.size(); ++i)
        if (stream[i] == 0xFF && stream[i + 1] == 0xD1) {
            stream[i + 1] = 0xD3;
            patched = true;
            break;
        }
    REQUIRE(patched);
    CHECK(!decode_to(stream, fi, back).is_ok());
}

TEST_CASE("jpegls: custom preset parameters (LSE type 1)") {
    struct Case { int bits; int near; jpegls::PresetParams p; };
    const Case cases[] = {
        {8, 0, {255, 5, 9, 30, 100}},  {8, 2, {0, 6, 12, 40, 0}},     {8, 0, {200, 0, 0, 0, 0}},
        {12, 0, {4095, 10, 40, 200, 32}}, {12, 3, {3000, 0, 0, 0, 16}}, {16, 0, {0, 0, 0, 0, 255}},
        {16, 5, {65535, 30, 90, 400, 64}}, {4, 1, {12, 2, 3, 4, 3}},
    };
    std::uint32_t seed = 1200;
    for (const Case& c : cases) {
        for (int spp : {1, 3}) {
            const FrameInfo fi = make_info(25, 26, spp, c.bits);
            auto px = synthetic(fi, Kind::Smooth, seed++);
            if (c.p.maxval != 0)
                for (std::size_t i = 0; i < px.size() / bytes_per_sample(fi); ++i)
                    set_sample(px, fi, i, std::min(get_sample(px, fi, i), c.p.maxval));
            jpegls::EncodeOptions opt;
            opt.near = c.near;
            opt.write_preset = true;
            opt.preset = c.p;
            std::vector<std::uint8_t> stream, back;
            REQUIRE(jpegls::encode(px, fi, opt, stream).is_ok());
            REQUIRE(decode_to(stream, fi, back).is_ok());
            CHECK(max_abs_error(px, back, fi) <= c.near);
        }
    }
    // Invalid presets are refused by the encoder and by the decoder.
    const FrameInfo fi = make_info(4, 4, 1, 8);
    const auto px = synthetic(fi, Kind::Gradient, 3);
    std::vector<std::uint8_t> stream;
    jpegls::EncodeOptions opt;
    opt.write_preset = true;
    opt.preset = {255, 9, 5, 30, 64};  // T1 > T2
    CHECK(jpegls::encode(px, fi, opt, stream).code == Errc::InvalidArgument);
    opt.preset = {255, 3, 7, 21, 2};  // RESET < 3
    CHECK(jpegls::encode(px, fi, opt, stream).code == Errc::InvalidArgument);
    opt.preset = {300, 0, 0, 0, 0};  // MAXVAL above 2^P - 1
    CHECK(jpegls::encode(px, fi, opt, stream).code == Errc::InvalidArgument);
    opt.preset = {100, 0, 0, 0, 0};  // samples above MAXVAL
    CHECK(jpegls::encode(px, fi, opt, stream).code == Errc::InvalidArgument);
}

TEST_CASE("jpegls: decodes independent encoder output (CharLS known answers)") {
    std::vector<std::uint8_t> back;
    {
        const FrameInfo fi = make_info(12, 16, 1, 8);
        const std::vector<std::uint8_t> s(std::begin(kCharlsGray8), std::end(kCharlsGray8));
        REQUIRE(decode_to(s, fi, back).is_ok());
        CHECK(back == std::vector<std::uint8_t>(std::begin(kCharlsGray8Pixels), std::end(kCharlsGray8Pixels)));
        // Our encoder produces a stream with the same pixels.
        std::vector<std::uint8_t> ours, back2;
        REQUIRE(encode_with(back, fi, 0, 0, ours).is_ok());
        REQUIRE(decode_to(ours, fi, back2).is_ok());
        CHECK(back2 == back);
    }
    {
        const FrameInfo fi = make_info(6, 8, 3, 8);
        const std::vector<std::uint8_t> s(std::begin(kCharlsRgbNear2), std::end(kCharlsRgbNear2));
        REQUIRE(decode_to(s, fi, back).is_ok());
        CHECK(back == std::vector<std::uint8_t>(std::begin(kCharlsRgbNear2Pixels), std::end(kCharlsRgbNear2Pixels)));
    }
    {
        const FrameInfo fi = make_info(12, 8, 1, 12);
        const std::vector<std::uint8_t> s(std::begin(kCharlsGray12), std::end(kCharlsGray12));
        REQUIRE(decode_to(s, fi, back).is_ok());
        CHECK(back == std::vector<std::uint8_t>(std::begin(kCharlsGray12Pixels), std::end(kCharlsGray12Pixels)));
    }
}

TEST_CASE("jpegls: frame header must match the DICOM attributes") {
    const std::vector<std::uint8_t> s(std::begin(kCharlsGray8), std::end(kCharlsGray8));
    std::vector<std::uint8_t> back;
    CHECK(decode_to(s, make_info(12, 16, 1, 8), back).is_ok());
    CHECK(decode_to(s, make_info(13, 16, 1, 8), back).code == Errc::Malformed);
    CHECK(decode_to(s, make_info(12, 15, 1, 8), back).code == Errc::Malformed);
    CHECK(decode_to(s, make_info(12, 16, 3, 8), back).code == Errc::Malformed);
    CHECK(decode_to(s, make_info(12, 16, 1, 7), back).code == Errc::Malformed);
    FrameInfo wide = make_info(12, 16, 1, 8);
    wide.bits_allocated = 16;  // 8-bit samples in a 16-bit container are fine
    REQUIRE(decode_to(s, wide, back).is_ok());
    CHECK(load_le16(&back[2 * 6 * 16 + 6]) == 250);
    std::vector<std::uint8_t> small(10);
    CHECK(decode_jpegls(s, make_info(12, 16, 1, 8), small).code == Errc::BufferTooSmall);
    FrameInfo bad = make_info(12, 16, 1, 8);
    bad.bits_allocated = 12;
    std::vector<std::uint8_t> any(1000);
    CHECK(!decode_jpegls(s, bad, any).is_ok());
}

TEST_CASE("jpegls: 8-bit samples in 16-bit containers and signed data") {
    FrameInfo fi = make_info(9, 14, 1, 8);
    fi.bits_allocated = 16;
    auto px = synthetic(fi, Kind::Smooth, 77);
    std::vector<std::uint8_t> stream, back;
    REQUIRE(encode_with(px, fi, 0, 0, stream).is_ok());
    REQUIRE(decode_to(stream, fi, back).is_ok());
    CHECK(back == px);

    // 12-bit signed values stored sign-extended in 16 bits come back sign-extended.
    FrameInfo sfi = make_info(5, 6, 1, 12);
    sfi.is_signed = true;
    std::vector<std::uint8_t> spx(static_cast<std::size_t>(sfi.frame_bytes()));
    for (std::size_t i = 0; i < 30; ++i)
        store_le16(&spx[2 * i], static_cast<std::uint16_t>(static_cast<std::int16_t>(static_cast<int>(i) * 130 - 2048)));
    REQUIRE(encode_with(spx, sfi, 0, 0, stream).is_ok());
    REQUIRE(decode_to(stream, sfi, back).is_ok());
    CHECK(back == spx);

    // Bits above BitsStored are ignored on input.
    FrameInfo ufi = make_info(3, 3, 1, 10);
    std::vector<std::uint8_t> upx(static_cast<std::size_t>(ufi.frame_bytes()));
    for (std::size_t i = 0; i < 9; ++i) store_le16(&upx[2 * i], static_cast<std::uint16_t>(0xFC00 | (i * 100)));
    REQUIRE(encode_with(upx, ufi, 0, 0, stream).is_ok());
    REQUIRE(decode_to(stream, ufi, back).is_ok());
    for (std::size_t i = 0; i < 9; ++i) CHECK(load_le16(&back[2 * i]) == i * 100);
}

namespace {

// Test-only forward HP colour transforms, the inverses of what the decoder applies.
void forward_hp(std::vector<std::uint8_t>& px, const FrameInfo& fi, int t) {
    const int bits = fi.bits_stored, mask = (1 << bits) - 1, half = 1 << (bits - 1);
    const std::size_t n = static_cast<std::size_t>(fi.rows) * fi.columns;
    for (std::size_t i = 0; i < n; ++i) {
        const int r = get_sample(px, fi, 3 * i), g = get_sample(px, fi, 3 * i + 1), b = get_sample(px, fi, 3 * i + 2);
        int v1 = 0, v2 = 0, v3 = 0;
        if (t == 1) {
            v1 = (r - g + half) & mask;
            v2 = g;
            v3 = (b - g + half) & mask;
        } else if (t == 2) {
            v1 = (r - g + half) & mask;
            v2 = g;
            v3 = (b - ((r + g) >> 1) + half) & mask;
        } else {
            v2 = (b - g + half) & mask;
            v3 = (r - g + half) & mask;
            v1 = (g + ((v2 + v3) >> 2) - half / 2) & mask;
        }
        set_sample(px, fi, 3 * i, v1);
        set_sample(px, fi, 3 * i + 1, v2);
        set_sample(px, fi, 3 * i + 2, v3);
    }
}

std::vector<std::uint8_t> with_app8(const std::vector<std::uint8_t>& stream, int t) {
    std::vector<std::uint8_t> out = {0xFF, 0xD8, 0xFF, 0xE8, 0x00, 0x07, 'm', 'r', 'f', 'x', static_cast<std::uint8_t>(t)};
    out.insert(out.end(), stream.begin() + 2, stream.end());
    return out;
}

}  // namespace

TEST_CASE("jpegls: HP colour transforms signalled in APP8 mrfx") {
    for (int bits : {8, 16}) {
        for (int t = 1; t <= 3; ++t) {
            for (int ilv = 0; ilv <= 2; ++ilv) {
                const FrameInfo fi = make_info(11, 13, 3, bits);
                const auto rgb = synthetic(fi, Kind::Noise, static_cast<std::uint32_t>(t * 10 + ilv + bits));
                auto coded = rgb;
                forward_hp(coded, fi, t);
                std::vector<std::uint8_t> stream, back;
                REQUIRE(encode_with(coded, fi, 0, ilv, stream).is_ok());
                const auto s = with_app8(stream, t);
                jpegls::StreamInfo si;
                REQUIRE(jpegls::probe(s, si).is_ok());
                CHECK(si.color_transform == t);
                REQUIRE(decode_to(s, fi, back).is_ok());
                CHECK(back == rgb);
                FrameInfo planar = fi;
                planar.planar = true;
                REQUIRE(decode_to(s, planar, back).is_ok());
                bool same = true;
                for (std::size_t i = 0; i < 11 * 13; ++i)
                    for (std::size_t c = 0; c < 3; ++c)
                        same = same && get_sample(back, fi, c * 143 + i) == get_sample(rgb, fi, 3 * i + c);
                CHECK(same);
            }
        }
    }
    const FrameInfo fi = make_info(4, 4, 3, 8);
    std::vector<std::uint8_t> stream, back;
    REQUIRE(encode_with(synthetic(fi, Kind::Gradient, 1), fi, 0, 2, stream).is_ok());
    CHECK(decode_to(with_app8(stream, 4), fi, back).code == Errc::Unsupported);
    const FrameInfo gray = make_info(4, 4, 1, 8);
    REQUIRE(encode_with(synthetic(gray, Kind::Gradient, 1), gray, 0, 0, stream).is_ok());
    CHECK(!decode_to(with_app8(stream, 1), gray, back).is_ok());
}

TEST_CASE("jpegls: oversize dimensions (LSE type 4) and hand-made malformed headers") {
    const std::vector<std::uint8_t> base(std::begin(kCharlsGray8), std::end(kCharlsGray8));
    const FrameInfo fi = make_info(12, 16, 1, 8);
    const std::vector<std::uint8_t> expect(std::begin(kCharlsGray8Pixels), std::end(kCharlsGray8Pixels));
    std::vector<std::uint8_t> back;
    // SOF55 at offset 2 (P at 6, Y 7..8, X 9..10, Nf 11, C 12, HV 13, Tq 14); SOS at 15
    // (Ns 19, Cs 20, Tm 21, NEAR 22, ILV 23, Al 24).
    {
        auto s = base;
        s[9] = 0;
        s[10] = 0;  // X = 0, given by LSE type 4 instead
        const std::uint8_t lse[] = {0xFF, 0xF8, 0x00, 0x08, 0x04, 0x02, 0x00, 0x0C, 0x00, 0x10};
        s.insert(s.begin() + 15, std::begin(lse), std::end(lse));
        REQUIRE(decode_to(s, fi, back).is_ok());
        CHECK(back == expect);
    }
    auto patched = [&](std::size_t at, std::uint8_t v) {
        auto s = base;
        s[at] = v;
        return s;
    };
    CHECK(decode_to(patched(21, 1), fi, back).code == Errc::Unsupported);   // mapping table
    CHECK(decode_to(patched(23, 3), fi, back).code == Errc::Malformed);     // ILV 3
    CHECK(decode_to(patched(24, 1), fi, back).code == Errc::Unsupported);   // point transform
    CHECK(decode_to(patched(22, 200), fi, back).code == Errc::Malformed);   // NEAR > MAXVAL / 2
    CHECK(decode_to(patched(13, 0x22), fi, back).code == Errc::Unsupported); // subsampling
    CHECK(decode_to(patched(20, 9), fi, back).code == Errc::Malformed);     // unknown component
    CHECK(decode_to(patched(6, 1), fi, back).code == Errc::Malformed);      // P = 1
    CHECK(decode_to(patched(11, 5), fi, back).code == Errc::Unsupported);   // 5 components
    CHECK(decode_to(patched(3, 0xC0), fi, back).code == Errc::Unsupported); // SOF0
    CHECK(decode_to(patched(0, 0x00), fi, back).code == Errc::Malformed);   // no SOI
    CHECK(decode_to(patched(17, 0xFF), fi, back).code == Errc::Truncated);  // SOS length too long
    {
        auto s = base;
        const std::uint8_t lse[] = {0xFF, 0xF8, 0x00, 0x0D, 0x01, 0x00, 0xFF, 0x00, 0x09,
                                    0x00, 0x05, 0x00, 0x15, 0x00, 0x40};  // T1 > T2
        s.insert(s.begin() + 15, std::begin(lse), std::end(lse));
        CHECK(decode_to(s, fi, back).code == Errc::Malformed);
    }
    {
        auto s = base;  // a second scan for the same component
        s.insert(s.end() - 2, base.begin() + 15, base.end() - 2);
        CHECK(!decode_to(s, fi, back).is_ok());
    }
    CHECK(!decode_to({}, fi, back).is_ok());
}

TEST_CASE("jpegls: encode_jpegls entry point and argument checks") {
    const FrameInfo fi = make_info(30, 20, 3, 8);
    const auto px = synthetic(fi, Kind::Smooth, 4);
    EncodeParams p;
    std::vector<std::uint8_t> stream, back;
    REQUIRE(encode_jpegls(px, fi, p, stream).is_ok());
    REQUIRE(decode_to(stream, fi, back).is_ok());
    CHECK(back == px);
    p.near_lossless = 4;
    REQUIRE(encode_jpegls(px, fi, p, stream).is_ok());
    REQUIRE(decode_to(stream, fi, back).is_ok());
    CHECK(max_abs_error(px, back, fi) <= 4);
    p.near_lossless = 128;  // above MAXVAL / 2
    CHECK(encode_jpegls(px, fi, p, stream).code == Errc::InvalidArgument);
    p.near_lossless = -1;
    CHECK(encode_jpegls(px, fi, p, stream).code == Errc::InvalidArgument);
    p.near_lossless = 0;
    const std::vector<std::uint8_t> short_in(px.begin(), px.end() - 1);
    CHECK(encode_jpegls(short_in, fi, p, stream).code == Errc::InvalidArgument);
    FrameInfo five = fi;
    five.samples_per_pixel = 5;
    CHECK(!encode_jpegls(px, five, p, stream).is_ok());
}

namespace {

std::vector<std::vector<std::uint8_t>> robustness_streams(std::vector<FrameInfo>& infos) {
    std::vector<std::vector<std::uint8_t>> out;
    struct Spec { FrameInfo fi; Kind kind; int near; int ilv; std::uint32_t restart; };
    const Spec specs[] = {
        {make_info(17, 23, 1, 8), Kind::Smooth, 0, 0, 0},
        {make_info(12, 19, 1, 16), Kind::Flat, 0, 0, 4},
        {make_info(10, 14, 3, 8), Kind::Noise, 2, 1, 0},
        {make_info(9, 15, 3, 12), Kind::Flat, 0, 2, 3},
        {make_info(8, 11, 3, 8), Kind::Edges, 1, 0, 0},
    };
    std::uint32_t seed = 4000;
    for (const Spec& sp : specs) {
        std::vector<std::uint8_t> s;
        if (!encode_with(synthetic(sp.fi, sp.kind, seed++), sp.fi, sp.near, sp.ilv, s, sp.restart).is_ok()) continue;
        out.push_back(s);
        infos.push_back(sp.fi);
    }
    out.emplace_back(std::begin(kCharlsRgbNear2), std::end(kCharlsRgbNear2));
    infos.push_back(make_info(6, 8, 3, 8));
    return out;
}

}  // namespace

TEST_CASE("jpegls: every truncation is an error and stays in bounds") {
    std::vector<FrameInfo> infos;
    const auto streams = robustness_streams(infos);
    REQUIRE(streams.size() == 6);
    for (std::size_t k = 0; k < streams.size(); ++k) {
        Status s;
        CHECK(decode_guarded(streams[k], infos[k], &s));
        CHECK(s.is_ok());
        int accepted = 0;
        for (std::size_t len = 0; len < streams[k].size(); ++len) {
            const std::vector<std::uint8_t> cut(streams[k].begin(), streams[k].begin() + static_cast<std::ptrdiff_t>(len));
            CHECK(decode_guarded(cut, infos[k], &s));
            if (s.is_ok()) ++accepted;
        }
        CHECK(accepted == 0);
    }
}

TEST_CASE("jpegls: random corruption never crashes or writes out of bounds") {
    std::vector<FrameInfo> infos;
    const auto streams = robustness_streams(infos);
    std::mt19937 rng(20261109);
    int iterations = 0, rejected = 0;
    for (std::size_t k = 0; k < streams.size(); ++k) {
        for (int it = 0; it < 1500; ++it) {
            auto s = streams[k];
            const int flips = 1 + static_cast<int>(static_cast<std::size_t>(rng()) % 4);
            for (int f = 0; f < flips; ++f) {
                const std::size_t at = static_cast<std::size_t>(rng()) % s.size();
                switch (static_cast<std::size_t>(rng()) % 3) {
                    case 0: s[at] = static_cast<std::uint8_t>(rng()); break;
                    case 1: s[at] ^= static_cast<std::uint8_t>(1u << (static_cast<std::size_t>(rng()) % 8)); break;
                    default: s[at] = 0xFF; break;
                }
            }
            Status st;
            CHECK(decode_guarded(s, infos[k], &st));
            ++iterations;
            if (!st.is_ok()) ++rejected;
        }
    }
    CHECK(iterations == 9000);
    CHECK(rejected > 0);
    // Garbage that looks like a header.
    for (int it = 0; it < 2000; ++it) {
        std::vector<std::uint8_t> s = {0xFF, 0xD8};
        const std::size_t n = static_cast<std::size_t>(rng()) % 200;
        for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<std::uint8_t>(static_cast<std::size_t>(rng()) % 4 == 0 ? 0xFF : rng()));
        CHECK(decode_guarded(s, make_info(5, 7, 1, 8)));
    }
}
