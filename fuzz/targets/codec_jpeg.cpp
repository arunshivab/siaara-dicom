// Fuzz target: JPEG decoder (baseline, extended, lossless; ITU-T T.81). The frame description is
// taken from the codestream's own header, as the frame reader does, and also deliberately varied.
#include "codec/jpeg.hpp"
#include "fuzz.hpp"

using namespace siaara;

namespace {
void try_decode(std::span<const std::uint8_t> s, const codec::FrameInfo& fi) {
    const std::uint64_t n = fi.frame_bytes();
    if (n == 0 || n > (64u << 20)) return;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    (void)codec::decode_jpeg(s, fi, out);
}
}  // namespace

SIAARA_FUZZ_TARGET(codec_jpeg) {
    const std::span<const std::uint8_t> s(data, size);
    codec::JpegHeaderInfo h;
    codec::FrameInfo fi;
    if (codec::jpeg_read_header(s, h)) {
        fi.rows = h.rows;
        fi.columns = h.columns;
        fi.samples_per_pixel = static_cast<std::uint16_t>(h.components == 3 ? 3 : 1);
        fi.bits_allocated = h.precision > 8 ? 16 : 8;
        fi.bits_stored = static_cast<std::uint16_t>(h.precision > 0 && h.precision <= 16 ? h.precision : 8);
        if (fi.rows == 0) fi.rows = 8;  // DNL-defined height: the caller's Rows
        try_decode(s, fi);
        fi.planar = fi.samples_per_pixel == 3;
        try_decode(s, fi);
        fi.planar = false;
        fi.is_signed = true;
        try_decode(s, fi);
    }
    // A fixed small description, whatever the header says (mismatch handling).
    fi = {};
    fi.rows = 16;
    fi.columns = 16;
    fi.bits_allocated = (size & 1u) ? 16 : 8;
    fi.bits_stored = fi.bits_allocated == 16 ? 12 : 8;
    fi.samples_per_pixel = (size & 2u) ? 3 : 1;
    try_decode(s, fi);
    return 0;
}
