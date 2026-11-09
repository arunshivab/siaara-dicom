// Fuzz target: JPEG-LS decoder (ITU-T T.87 | ISO/IEC 14495-1). Frame description from the
// codestream (probe), then varied; a lossless decode is re-encoded and must round-trip.
#include "codec/jpegls.hpp"
#include "fuzz.hpp"

using namespace siaara;

namespace {
bool try_decode(std::span<const std::uint8_t> s, const codec::FrameInfo& fi, std::vector<std::uint8_t>& out) {
    const std::uint64_t n = fi.frame_bytes();
    if (n == 0 || n > (64u << 20)) return false;
    out.assign(static_cast<std::size_t>(n), 0);
    return codec::decode_jpegls(s, fi, out).is_ok();
}
}  // namespace

SIAARA_FUZZ_TARGET(codec_jpegls) {
    const std::span<const std::uint8_t> s(data, size);
    codec::jpegls::StreamInfo si;
    codec::FrameInfo fi;
    std::vector<std::uint8_t> out;
    if (codec::jpegls::probe(s, si)) {
        fi.rows = si.height;
        fi.columns = si.width;
        fi.samples_per_pixel = static_cast<std::uint16_t>(si.components == 3 ? 3 : 1);
        fi.bits_allocated = si.precision > 8 ? 16 : 8;
        fi.bits_stored = static_cast<std::uint16_t>(si.precision > 0 && si.precision <= 16 ? si.precision : 8);
        if (try_decode(s, fi, out) && si.near == 0 && si.color_transform == 0) {
            std::vector<std::uint8_t> enc, back;
            if (codec::encode_jpegls(out, fi, {}, enc)) {
                FUZZ_REQUIRE(try_decode(enc, fi, back));
                FUZZ_REQUIRE(back == out);
            }
        }
        fi.planar = fi.samples_per_pixel == 3;
        (void)try_decode(s, fi, out);
    }
    fi = {};
    fi.rows = 16;
    fi.columns = 16;
    fi.bits_allocated = (size & 1u) ? 16 : 8;
    fi.bits_stored = fi.bits_allocated == 16 ? 12 : 8;
    fi.samples_per_pixel = (size & 2u) ? 3 : 1;
    (void)try_decode(s, fi, out);
    return 0;
}
