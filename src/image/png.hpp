// SIAARA DICOM Toolkit - PNG writer for rendered images (8-bit gray or RGBA). Written from the
// PNG specification (ISO/IEC 15948 / W3C PNG, second edition): signature, IHDR / IDAT / IEND
// chunks with CRC-32, a zlib (RFC 1950) stream around our own DEFLATE (codec/deflate.hpp), and
// per-row adaptive filtering (None, Sub, Up, Average, Paeth).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/status.hpp"

namespace siaara::image {

// pixels: width * height * channels bytes, channels 1 (gray) or 4 (RGBA, not premultiplied).
// level: DEFLATE effort 0..9.
Status encode_png(std::span<const std::uint8_t> pixels, std::uint32_t width, std::uint32_t height, unsigned channels,
                  int level, std::vector<std::uint8_t>& out);

// CRC-32 as PNG uses it (ISO 3309 polynomial, reflected, init and final xor 0xFFFFFFFF).
std::uint32_t png_crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) noexcept;
// Adler-32 (RFC 1950).
std::uint32_t adler32(const std::uint8_t* data, std::size_t size, std::uint32_t adler = 1) noexcept;

}  // namespace siaara::image
