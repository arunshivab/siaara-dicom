// SIAARA DICOM Toolkit - pixel region blanking for the Clean Pixel Data Option (PS3.15 E.3.1):
// "The Stored Pixel Values are to be changed (blacked out)". Native pixel data only.
//
// Black per Photometric Interpretation (PS3.3 C.7.6.3.1.2): MONOCHROME2 the lowest stored value,
// MONOCHROME1 the highest, RGB 0, YBR_FULL / YBR_FULL_422 Y = 0 with Cb = Cr at the mid value,
// YBR_PARTIAL_* Y at 16/256 of the range; other interpretations (PALETTE COLOR, retired spaces)
// get sample value 0. For 4:2:2 data the chroma of every pixel pair touched is neutralised.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "deid/deid.hpp"
#include "image/pixel_info.hpp"

namespace siaara::deid {

namespace {

using image::Photometric;

// Stored value (already masked to bits_stored, not yet shifted) for sample `s` of a black pixel.
std::uint64_t black_value(const image::PixelInfo& pi, unsigned s) {
    const unsigned bs = pi.bits_stored;
    const std::uint64_t all = bs >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bs) - 1u);
    const std::uint64_t half = bs == 0 ? 0 : (std::uint64_t{1} << (bs - 1));
    switch (pi.photometric) {
        case Photometric::Monochrome1:
            return pi.is_signed ? half - 1u : all;
        case Photometric::Monochrome2:
            return pi.is_signed ? half : 0;  // two's complement minimum is the sign bit alone
        case Photometric::YbrFull:
        case Photometric::YbrFull422:
            return s == 0 ? 0 : half;
        case Photometric::YbrPartial422:
        case Photometric::YbrPartial420:
            return s == 0 ? (bs >= 4 ? (std::uint64_t{1} << (bs - 4)) : 0) : half;
        default:
            return 0;
    }
}

void write_sample(std::vector<std::uint8_t>& v, std::uint64_t bit_offset, const image::PixelInfo& pi,
                  std::uint64_t stored) {
    if (pi.bits_allocated == 1) {
        const auto byte = static_cast<std::size_t>(bit_offset / 8);
        const auto bit = static_cast<unsigned>(bit_offset % 8);
        if (stored & 1u) v[byte] = static_cast<std::uint8_t>(v[byte] | (1u << bit));
        else v[byte] = static_cast<std::uint8_t>(v[byte] & ~(1u << bit));
        return;
    }
    const std::uint64_t container = pi.kind == image::SampleKind::Integer ? (stored << pi.low_bit()) : 0;
    const auto at = static_cast<std::size_t>(bit_offset / 8);
    const unsigned nbytes = pi.bits_allocated / 8u;
    for (unsigned i = 0; i < nbytes; ++i) v[at + i] = static_cast<std::uint8_t>(container >> (8 * i));
}

}  // namespace

Status blank_pixel_regions(Dataset& ds, std::span<const PixelRegion> regions) {
    if (regions.empty()) return Status::ok();
    auto info = image::read_pixel_info(ds);
    if (!info) return info.take_error();
    const image::PixelInfo& pi = *info;
    Element* px = ds.find(pi.pixel_tag);
    if (!px) return Status(Errc::NotFound, "no pixel data");
    if (px->is_encapsulated())
        return Status(Errc::Unsupported, "pixel regions can only be blanked in native (uncompressed) pixel data");
    if (pi.bits_allocated != 1 && pi.bits_allocated % 8 != 0)
        return Status(Errc::Unsupported, "unsupported Bits Allocated for blanking");
    if (pi.bits_allocated == 1 && pi.samples_per_pixel != 1)
        return Status(Errc::Malformed, "1-bit pixel data with several samples");
    const std::uint64_t frame_bits = pi.native_frame_bits();
    std::uint64_t total_bits = 0;
    if (!checked_mul(frame_bits, pi.number_of_frames, total_bits) || total_bits / 8 > px->value.size())
        return Status(Errc::Truncated, "pixel data shorter than the image pixel module describes");
    for (const PixelRegion& r : regions)
        if (r.frame != kAllFrames && r.frame >= pi.number_of_frames)
            return Status(Errc::InvalidArgument, "pixel region frame out of range");

    std::vector<std::uint8_t> v(px->value.data(), px->value.data() + px->value.size());
    const std::uint64_t spp = pi.samples_per_pixel;
    const std::uint64_t pixels = pi.pixels();
    const bool sub = pi.native_subsampled();
    for (const PixelRegion& r : regions) {
        if (r.x >= pi.columns || r.y >= pi.rows || r.width == 0 || r.height == 0) continue;
        const std::uint64_t x1 = std::min<std::uint64_t>(std::uint64_t{r.x} + r.width, pi.columns);
        const std::uint64_t y1 = std::min<std::uint64_t>(std::uint64_t{r.y} + r.height, pi.rows);
        const std::uint32_t f0 = r.frame == kAllFrames ? 0 : r.frame;
        const std::uint32_t f1 = r.frame == kAllFrames ? pi.number_of_frames : r.frame + 1;
        for (std::uint32_t f = f0; f < f1; ++f) {
            const std::uint64_t base = std::uint64_t{f} * frame_bits;
            for (std::uint64_t y = r.y; y < y1; ++y) {
                for (std::uint64_t x = r.x; x < x1; ++x) {
                    const std::uint64_t p = y * pi.columns + x;
                    if (sub) {
                        // Y0 Y1 Cb Cr for each horizontal pixel pair.
                        const std::uint64_t pair = p / 2;
                        const std::uint64_t s0 = pair * 4;
                        write_sample(v, base + (s0 + (p & 1u)) * pi.bits_allocated, pi, black_value(pi, 0));
                        write_sample(v, base + (s0 + 2) * pi.bits_allocated, pi, black_value(pi, 1));
                        write_sample(v, base + (s0 + 3) * pi.bits_allocated, pi, black_value(pi, 2));
                        continue;
                    }
                    for (unsigned s = 0; s < spp; ++s) {
                        const std::uint64_t idx = pi.planar ? s * pixels + p : p * spp + s;
                        write_sample(v, base + idx * pi.bits_allocated, pi, black_value(pi, s));
                    }
                }
            }
        }
    }
    px->value = Bytes::copy_of(v.data(), v.size());
    return Status::ok();
}

}  // namespace siaara::deid
