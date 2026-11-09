// SIAARA DICOM Toolkit - PNG writer (see png.hpp).
#include "image/png.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <new>

#include "codec/deflate.hpp"

namespace siaara::image {

namespace {

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> t = [] {
        std::array<std::uint32_t, 256> a{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            a[n] = c;
        }
        return a;
    }();
    return t;
}

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 24));
    o.push_back(static_cast<std::uint8_t>(v >> 16));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}

void chunk(std::vector<std::uint8_t>& o, const char type[4], const std::uint8_t* data, std::size_t size) {
    put32(o, static_cast<std::uint32_t>(size));
    const std::size_t start = o.size();
    o.insert(o.end(), type, type + 4);
    if (size) o.insert(o.end(), data, data + size);
    put32(o, png_crc32(o.data() + start, size + 4));
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    if (pb <= pc) return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

}  // namespace

std::uint32_t png_crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) noexcept {
    const auto& t = crc_table();
    std::uint32_t c = crc ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size, std::uint32_t adler) noexcept {
    std::uint32_t a = adler & 0xFFFFu, b = adler >> 16;
    while (size) {
        const std::size_t n = size < 5552 ? size : 5552;  // keeps b below 2^32 before the modulo
        for (std::size_t i = 0; i < n; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        data += n;
        size -= n;
    }
    return (b << 16) | a;
}

Status encode_png(std::span<const std::uint8_t> pixels, std::uint32_t width, std::uint32_t height, unsigned channels,
                  int level, std::vector<std::uint8_t>& out) {
    if (channels != 1 && channels != 4) return Status(Errc::InvalidArgument, "png: 1 or 4 channels");
    if (width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu)
        return Status(Errc::InvalidArgument, "png: bad image size");
    if (level < 0 || level > 9) return Status(Errc::InvalidArgument, "png: level must be 0..9");
    const std::uint64_t stride = std::uint64_t{width} * channels;
    if (pixels.size() < stride * height) return Status(Errc::Truncated, "png: pixel buffer too short");
    if ((stride + 1) * height > SIZE_MAX / 2) return Status(Errc::LimitExceeded, "png: image too large");
    try {
        const auto st = static_cast<std::size_t>(stride);
        // Filtered scanlines: one filter type byte per row, chosen by the smallest sum of
        // absolute values of the filtered bytes taken as signed (the usual heuristic).
        std::vector<std::uint8_t> raw((st + 1) * height);
        std::vector<std::uint8_t> cand(st);
        std::vector<std::uint8_t> best(st);
        const std::uint8_t* prev = nullptr;
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* row = pixels.data() + st * y;
            std::uint64_t best_sum = UINT64_MAX;
            std::uint8_t best_type = 0;
            for (std::uint8_t type = 0; type < 5; ++type) {
                if (level == 0 && type != 0) break;
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < st; ++i) {
                    const int a = i >= channels ? row[i - channels] : 0;
                    const int b = prev ? prev[i] : 0;
                    const int c = (prev && i >= channels) ? prev[i - channels] : 0;
                    int pred = 0;
                    switch (type) {
                        case 1: pred = a; break;
                        case 2: pred = b; break;
                        case 3: pred = (a + b) / 2; break;
                        case 4: pred = paeth(a, b, c); break;
                        default: break;
                    }
                    const auto v = static_cast<std::uint8_t>(row[i] - pred);
                    cand[i] = v;
                    sum += static_cast<std::uint64_t>(v < 128 ? v : 256 - v);
                }
                if (sum < best_sum) {
                    best_sum = sum;
                    best_type = type;
                    best.swap(cand);
                }
            }
            raw[(st + 1) * y] = best_type;
            std::memcpy(raw.data() + (st + 1) * y + 1, best.data(), st);
            prev = row;
        }
        // zlib wrapper: CMF (deflate, 32 KiB window), FLG with FCHECK, data, Adler-32.
        std::vector<std::uint8_t> z;
        z.push_back(0x78);
        const std::uint8_t flevel = level >= 7 ? 3 : level >= 6 ? 2 : level >= 2 ? 1 : 0;
        std::uint8_t flg = static_cast<std::uint8_t>(flevel << 6);
        flg = static_cast<std::uint8_t>(flg + (31 - ((0x78u * 256u + flg) % 31u)) % 31u);
        z.push_back(flg);
        std::vector<std::uint8_t> def;
        if (Status s = codec::deflate_raw(raw.data(), raw.size(), level, def); !s) return s;
        z.insert(z.end(), def.begin(), def.end());
        put32(z, adler32(raw.data(), raw.size()));

        out.clear();
        static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        out.insert(out.end(), sig, sig + 8);
        std::vector<std::uint8_t> ihdr;
        put32(ihdr, width);
        put32(ihdr, height);
        ihdr.push_back(8);                                // bit depth
        ihdr.push_back(channels == 4 ? 6 : 0);            // colour type: RGBA or grayscale
        ihdr.push_back(0);                                // deflate
        ihdr.push_back(0);                                // adaptive filtering
        ihdr.push_back(0);                                // no interlace
        chunk(out, "IHDR", ihdr.data(), ihdr.size());
        chunk(out, "IDAT", z.data(), z.size());
        chunk(out, "IEND", nullptr, 0);
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "png: out of memory");
    }
    return Status::ok();
}

}  // namespace siaara::image
