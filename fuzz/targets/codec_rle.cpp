// Fuzz target: RLE Lossless decoder (PS3.5 G). Input: 4 bytes of frame description, then the
// RLE frame (header of 16 offsets plus segments). Decoded frames are re-encoded and must round-trip.
#include "codec/codec.hpp"
#include "fuzz.hpp"

using namespace siaara;

SIAARA_FUZZ_TARGET(codec_rle) {
    siaara_fuzz::Input in(data, size);
    codec::FrameInfo fi;
    fi.rows = 1u + in.u8() % 64u;
    fi.columns = 1u + in.u8() % 64u;
    const std::uint8_t f = in.u8();
    fi.samples_per_pixel = (f & 1u) ? 3 : 1;
    static constexpr std::uint16_t kBits[] = {8, 16, 32, 8};
    fi.bits_allocated = kBits[(f >> 1) & 3u];
    fi.bits_stored = fi.bits_allocated;
    fi.is_signed = (f & 8u) != 0;
    fi.planar = (f & 16u) != 0 && fi.samples_per_pixel == 3;
    (void)in.u8();
    const std::span<const std::uint8_t> stream(in.ptr(), in.remaining());
    std::vector<std::uint8_t> out(static_cast<std::size_t>(fi.frame_bytes()));
    if (!codec::decode_rle(stream, fi, out)) return 0;
    std::vector<std::uint8_t> enc;
    FUZZ_REQUIRE(codec::encode_rle(out, fi, {}, enc).is_ok());
    std::vector<std::uint8_t> back(out.size());
    FUZZ_REQUIRE(codec::decode_rle(enc, fi, back).is_ok());
    FUZZ_REQUIRE(back == out);
    return 0;
}
