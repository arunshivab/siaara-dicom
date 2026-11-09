// C API tests: PNG / JPEG encoders for rendered images, GPU device choice, lossless JPEG-LS of
// signed samples.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/deflate.hpp"
#include "image/png.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::vector<std::uint8_t> buffer_bytes(const siaara_buffer* b) {
    const uint8_t* d = nullptr;
    uint64_t n = 0;
    if (siaara_buffer_data(b, &d, &n) != SIAARA_OK) return {};
    return std::vector<std::uint8_t>(d, d + n);
}

// Minimal PNG reader for the test: chunks with CRC check, zlib + inflate, unfiltering.
bool read_png(const std::vector<std::uint8_t>& png, unsigned channels, std::uint32_t& w, std::uint32_t& h,
              std::vector<std::uint8_t>& pixels) {
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 8 || std::memcmp(png.data(), sig, 8) != 0) return false;
    std::vector<std::uint8_t> idat;
    std::size_t pos = 8;
    bool end = false;
    while (pos + 12 <= png.size() && !end) {
        const std::uint32_t len = be32(&png[pos]);
        const std::string type(reinterpret_cast<const char*>(&png[pos + 4]), 4);
        if (pos + 12 + len > png.size()) return false;
        const std::uint32_t crc = be32(&png[pos + 8 + len]);
        if (crc != siaara::image::png_crc32(&png[pos + 4], len + 4)) return false;
        const std::uint8_t* data = &png[pos + 8];
        if (type == "IHDR") {
            w = be32(data);
            h = be32(data + 4);
            if (data[8] != 8 || data[9] != (channels == 4 ? 6 : 0)) return false;
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data, data + len);
        } else if (type == "IEND") {
            end = true;
        }
        pos += 12 + len;
    }
    if (!end || idat.size() < 6 || (idat[0] * 256 + idat[1]) % 31 != 0 || (idat[0] & 0x0F) != 8) return false;
    std::vector<std::uint8_t> raw;
    if (!siaara::codec::inflate_raw(idat.data() + 2, idat.size() - 6, 1u << 24, raw).is_ok()) return false;
    if (be32(&idat[idat.size() - 4]) != siaara::image::adler32(raw.data(), raw.size())) return false;
    const std::size_t st = std::size_t{w} * channels;
    if (raw.size() != (st + 1) * h) return false;
    pixels.assign(st * h, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t type = raw[(st + 1) * y];
        const std::uint8_t* f = &raw[(st + 1) * y + 1];
        std::uint8_t* row = &pixels[st * y];
        const std::uint8_t* prev = y ? &pixels[st * (y - 1)] : nullptr;
        for (std::size_t i = 0; i < st; ++i) {
            const int a = i >= channels ? row[i - channels] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = prev && i >= channels ? prev[i - channels] : 0;
            int pred = 0;
            if (type == 1) pred = a;
            else if (type == 2) pred = b;
            else if (type == 3) pred = (a + b) / 2;
            else if (type == 4) {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            } else if (type != 0) {
                return false;
            }
            row[i] = static_cast<std::uint8_t>(f[i] + pred);
        }
    }
    return true;
}

}  // namespace

TEST_CASE("capi/encode: CRC-32 and Adler-32 reference values") {
    const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(siaara::image::png_crc32(digits, 9) == 0xCBF43926u);
    const char* iend = "IEND";
    CHECK(siaara::image::png_crc32(reinterpret_cast<const std::uint8_t*>(iend), 4) == 0xAE426082u);
    const char* w = "Wikipedia";
    CHECK(siaara::image::adler32(reinterpret_cast<const std::uint8_t*>(w), 9) == 0x11E60398u);
}

TEST_CASE("capi/encode: PNG of RGBA and gray images round trips exactly") {
    for (int level : {0, 1, 6, 9}) {
        const std::uint32_t W = 37, H = 23;
        std::vector<std::uint8_t> rgba(W * H * 4);
        for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<std::uint8_t>((i * 7 + (i / 148) * 13) ^ (i >> 3));
        siaara_buffer* b = nullptr;
        REQUIRE(siaara_encode_png(rgba.data(), rgba.size(), W, H, SIAARA_PIXELS_RGBA8, level, &b) == SIAARA_OK);
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint8_t> back;
        CHECK(read_png(buffer_bytes(b), 4, w, h, back));
        CHECK(w == W && h == H && back == rgba);
        siaara_buffer_free(b);
        std::vector<std::uint8_t> gray(W * H);
        for (std::size_t i = 0; i < gray.size(); ++i) gray[i] = static_cast<std::uint8_t>(i % W * 6);
        REQUIRE(siaara_encode_png(gray.data(), gray.size(), W, H, SIAARA_PIXELS_GRAY8, level, &b) == SIAARA_OK);
        CHECK(read_png(buffer_bytes(b), 1, w, h, back));
        CHECK(back == gray);
        siaara_buffer_free(b);
    }
    siaara_buffer* b = nullptr;
    std::uint8_t px[4] = {};
    CHECK(siaara_encode_png(px, 4, 2, 1, SIAARA_PIXELS_RGBA8, 6, &b) == SIAARA_ERR_INVALID_ARGUMENT);  // too short
    CHECK(siaara_encode_png(px, 4, 1, 1, 7, 6, &b) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_encode_png(px, 4, 1, 1, SIAARA_PIXELS_RGBA8, 10, &b) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_encode_png(px, 4, 1, 1, SIAARA_PIXELS_RGBA8, -1, &b) == SIAARA_OK);
    siaara_buffer_free(b);
    siaara_buffer_free(nullptr);
}

TEST_CASE("capi/encode: JPEG of RGBA and gray images decodes close to the input") {
    const std::uint32_t W = 32, H = 16;
    std::vector<std::uint8_t> rgba(W * H * 4), gray(W * H);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::size_t i = y * W + x;
            rgba[4 * i] = static_cast<std::uint8_t>(x * 8);
            rgba[4 * i + 1] = static_cast<std::uint8_t>(y * 16);
            rgba[4 * i + 2] = 128;
            rgba[4 * i + 3] = 255;
            gray[i] = static_cast<std::uint8_t>(x * 4 + y * 4);
        }
    siaara_buffer* b = nullptr;
    REQUIRE(siaara_encode_jpeg(gray.data(), gray.size(), W, H, SIAARA_PIXELS_GRAY8, 95, &b) == SIAARA_OK);
    std::vector<std::uint8_t> j = buffer_bytes(b);
    siaara_buffer_free(b);
    REQUIRE(j.size() > 24);
    CHECK(j[2] == 0xFF && j[3] == 0xE0 && std::memcmp(&j[6], "JFIF", 4) == 0);
    siaara::codec::FrameInfo fi;
    fi.rows = H;
    fi.columns = W;
    fi.samples_per_pixel = 1;
    fi.bits_allocated = 8;
    fi.bits_stored = 8;
    std::vector<std::uint8_t> dec(W * H);
    REQUIRE(siaara::codec::decode_jpeg(j, fi, dec).is_ok());
    int maxerr = 0;
    for (std::size_t i = 0; i < dec.size(); ++i) maxerr = std::max(maxerr, std::abs(int(dec[i]) - int(gray[i])));
    CHECK(maxerr <= 8);
    REQUIRE(siaara_encode_jpeg(rgba.data(), rgba.size(), W, H, SIAARA_PIXELS_RGBA8, 0, &b) == SIAARA_OK);
    j = buffer_bytes(b);
    siaara_buffer_free(b);
    CHECK(j.size() > 100 && j[0] == 0xFF && j[1] == 0xD8 && j[j.size() - 2] == 0xFF && j[j.size() - 1] == 0xD9);
    CHECK(siaara_encode_jpeg(rgba.data(), rgba.size(), W, H, SIAARA_PIXELS_RGBA8, 101, &b) == SIAARA_ERR_INVALID_ARGUMENT);
}

TEST_CASE("capi/gpu: device selection") {
    uint32_t count = 0;
    REQUIRE(siaara_gpu_devices(nullptr, 0, &count) == SIAARA_OK);
    CHECK(siaara_gpu_select(count) == SIAARA_ERR_NOT_FOUND);
    if (count > 0) {
        const uint32_t pick = count - 1;
        REQUIRE(siaara_gpu_select(pick) == SIAARA_OK);
        std::vector<siaara_gpu_device_info> d(count);
        for (auto& x : d) {
            std::memset(&x, 0, sizeof x);
            x.struct_size = sizeof x;
        }
        REQUIRE(siaara_gpu_devices(d.data(), count, &count) == SIAARA_OK);
        for (uint32_t i = 0; i < count; ++i) CHECK((d[i].selected != 0) == (i == pick));
        REQUIRE(siaara_gpu_select(0) == SIAARA_OK);
    }
}

TEST_CASE("capi/transcode: lossless JPEG-LS of signed samples, near-lossless refused") {
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_create(&f) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    auto s = [&](uint32_t tag, const char* vr, const char* v) { (void)siaara_dataset_set_string(ds, tag, vr, v, std::strlen(v)); };
    s(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2");
    s(0x00080018, "UI", "2.25.31337");
    s(0x00280004, "CS", "MONOCHROME2");
    (void)siaara_dataset_set_uint16(ds, 0x00280002, 1);
    (void)siaara_dataset_set_uint16(ds, 0x00280010, 8);
    (void)siaara_dataset_set_uint16(ds, 0x00280011, 8);
    (void)siaara_dataset_set_uint16(ds, 0x00280100, 16);
    (void)siaara_dataset_set_uint16(ds, 0x00280101, 12);
    (void)siaara_dataset_set_uint16(ds, 0x00280102, 11);
    (void)siaara_dataset_set_uint16(ds, 0x00280103, 1);
    std::vector<std::uint8_t> px(128);
    for (std::size_t i = 0; i < 64; ++i) {
        const int v = static_cast<int>(i * 97 % 4096) - 2048;  // -2048 .. 2047, sign-extended in 16 bits
        px[2 * i] = static_cast<std::uint8_t>(v & 0xFF);
        px[2 * i + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    }
    REQUIRE(siaara_dataset_set_bytes(ds, 0x7FE00010, "OW", px.data(), px.size()) == SIAARA_OK);
    siaara_transcode_params near;
    std::memset(&near, 0, sizeof near);
    near.struct_size = sizeof near;
    near.lossy = 1;
    near.allow_lossy = 1;
    CHECK(siaara_file_transcode(f, "1.2.840.10008.1.2.4.81", &near) == SIAARA_ERR_UNSUPPORTED);
    REQUIRE(siaara_file_transcode(f, "1.2.840.10008.1.2.4.80", nullptr) == SIAARA_OK);
    std::vector<std::uint8_t> out(128);
    size_t need = 0;
    REQUIRE(siaara_frame_decode(f, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) == SIAARA_OK);
    CHECK(out == px);
    siaara_file_free(f);
}
