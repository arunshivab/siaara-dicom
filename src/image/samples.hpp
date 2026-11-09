// SIAARA DICOM Toolkit - sample extraction helpers shared by the image layer (internal).
// Stored values per PS3.5 8.1 / PS3.3 C.7.6.3.1: the value occupies bits low_bit .. high_bit of a
// little-endian container of Bits Allocated bits; Pixel Representation 1 means two's complement.
#pragma once

#include <cstdint>
#include <cstring>

#include "image/pixel_info.hpp"

namespace siaara::image::detail {

struct SampleReader {
    unsigned bytes = 2;        // container bytes (0 for 1-bit data)
    unsigned shift = 0;        // low bit
    std::uint64_t mask = 0;    // (1 << bits_stored) - 1
    std::uint64_t sign = 0;    // 1 << (bits_stored - 1) when signed, else 0
    bool one_bit = false;

    explicit SampleReader(const PixelInfo& pi) noexcept
        : bytes(pi.bits_allocated == 1 ? 0u : pi.bits_allocated / 8u),
          shift(pi.low_bit()),
          mask(pi.bits_stored >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << pi.bits_stored) - 1u),
          sign(pi.is_signed ? std::uint64_t{1} << (pi.bits_stored - 1u) : 0u),
          one_bit(pi.bits_allocated == 1) {}

    std::uint64_t raw(const std::uint8_t* frame, std::uint64_t i) const noexcept {
        if (one_bit) return (static_cast<std::uint64_t>(frame[i >> 3]) >> (i & 7u)) & 1u;
        const std::uint8_t* p = frame + i * bytes;
        switch (bytes) {
            case 1: return p[0];
            case 2: return static_cast<std::uint64_t>(p[0]) | (static_cast<std::uint64_t>(p[1]) << 8);
            case 4: {
                std::uint64_t v = 0;
                for (int k = 3; k >= 0; --k) v = (v << 8) | p[k];
                return v;
            }
            default: {
                std::uint64_t v = 0;
                for (int k = 7; k >= 0; --k) v = (v << 8) | p[k];
                return v;
            }
        }
    }
    // The stored value: bits low_bit..high_bit, sign-extended when Pixel Representation is 1.
    std::int64_t value(const std::uint8_t* frame, std::uint64_t i) const noexcept {
        const std::uint64_t v = (raw(frame, i) >> shift) & mask;
        if (sign && (v & sign)) return static_cast<std::int64_t>(v | ~mask);
        return static_cast<std::int64_t>(v);
    }
};

inline float load_f32(const std::uint8_t* p) noexcept {
    std::uint32_t b = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                      (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline double load_f64(const std::uint8_t* p) noexcept {
    std::uint64_t b = 0;
    for (int k = 7; k >= 0; --k) b = (b << 8) | p[k];
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

// Smallest / largest stored value for the pixel description.
// (Bits Stored above 62 saturates at the int64 limits.)
inline std::int64_t min_stored(const PixelInfo& pi) noexcept {
    if (!pi.is_signed) return 0;
    return pi.bits_stored >= 63 ? INT64_MIN : -(std::int64_t{1} << (pi.bits_stored - 1));
}
inline std::int64_t max_stored(const PixelInfo& pi) noexcept {
    if (pi.bits_stored >= 63) return INT64_MAX;
    return pi.is_signed ? (std::int64_t{1} << (pi.bits_stored - 1)) - 1
                        : static_cast<std::int64_t>((std::uint64_t{1} << pi.bits_stored) - 1u);
}

}  // namespace siaara::image::detail
