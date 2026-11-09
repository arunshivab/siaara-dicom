// Fuzz target: JPEG 2000 Part 1 and HTJ2K decoder (T.800, T.814): header probe, component decoder
// and the frame decoder with the description taken from the codestream.
#include "codec/j2k.hpp"
#include "fuzz.hpp"

using namespace siaara;

SIAARA_FUZZ_TARGET(codec_j2k) {
    const std::span<const std::uint8_t> s(data, size);
    codec::j2k::StreamInfo si;
    if (!codec::j2k::probe(s, si)) return 0;
    // Large (valid) images are decoded once only, to keep each input well under the 2 s hang limit.
    const bool small = std::uint64_t{si.width} * si.height * static_cast<std::uint64_t>(si.components > 0 ? si.components : 1) <=
                       (std::uint64_t{1} << 18);
    if (small) {
        std::vector<codec::j2k::Component> comps;
        (void)codec::j2k::decode_components(s, comps, std::uint64_t{1} << 22);
    }
    codec::FrameInfo fi;
    fi.rows = si.height;
    fi.columns = si.width;
    fi.samples_per_pixel = static_cast<std::uint16_t>(si.components == 3 ? 3 : 1);
    fi.bits_allocated = si.precision > 8 ? (si.precision > 16 ? 32 : 16) : 8;
    fi.bits_stored = static_cast<std::uint16_t>(si.precision > 0 && si.precision <= 32 ? si.precision : 8);
    fi.is_signed = si.is_signed;
    const std::uint64_t n = fi.frame_bytes();
    if (n == 0 || n > (16u << 20)) return 0;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    (void)codec::decode_j2k(s, fi, out);
    if (small && fi.samples_per_pixel == 3) {
        fi.planar = true;
        (void)codec::decode_j2k(s, fi, out);
    }
    return 0;
}
