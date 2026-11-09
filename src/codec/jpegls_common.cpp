// SIAARA DICOM Toolkit - JPEG-LS coding parameters (ITU-T T.87 A.2.1 and C.2.4.1.1).
#include <algorithm>

#include "codec/jpegls_internal.hpp"

namespace siaara::codec::jpegls::detail {

namespace {

// Smallest n with 2^n >= v (v >= 1).
int ceil_log2(int v) {
    int n = 0;
    while ((1 << n) < v) ++n;
    return n;
}

// CLAMP of T.87 C.2.4.1.1.1: values outside [lo, maxval] fall back to lo.
int clamp_threshold(int v, int lo, int maxval) { return (v > maxval || v < lo) ? lo : v; }

}  // namespace

Status derive_params(int precision, int near, const PresetParams& preset, CodingParams& out) {
    if (precision < 2 || precision > 16) return Status(Errc::Malformed, "JPEG-LS: precision out of range");
    const int full = (1 << precision) - 1;
    CodingParams p;
    p.maxval = preset.maxval != 0 ? preset.maxval : full;
    if (p.maxval < 1 || p.maxval > full) return Status(Errc::Malformed, "JPEG-LS: MAXVAL out of range");
    if (near < 0 || near > std::min(255, p.maxval / 2))
        return Status(Errc::Malformed, "JPEG-LS: NEAR out of range");
    p.near = near;

    // Default thresholds (C.2.4.1.1.1) with BASIC_T1..3 = 3, 7, 21.
    int d1 = 0, d2 = 0, d3 = 0;
    if (p.maxval >= 128) {
        const int factor = (std::min(p.maxval, 4095) + 128) / 256;
        d1 = clamp_threshold(factor * (3 - 2) + 2 + 3 * near, near + 1, p.maxval);
        d2 = clamp_threshold(factor * (7 - 3) + 3 + 5 * near, d1, p.maxval);
        d3 = clamp_threshold(factor * (21 - 4) + 4 + 7 * near, d2, p.maxval);
    } else {
        const int factor = 256 / (p.maxval + 1);
        d1 = clamp_threshold(std::max(2, 3 / factor + 3 * near), near + 1, p.maxval);
        d2 = clamp_threshold(std::max(3, 7 / factor + 5 * near), d1, p.maxval);
        d3 = clamp_threshold(std::max(4, 21 / factor + 7 * near), d2, p.maxval);
    }
    p.t1 = preset.t1 != 0 ? preset.t1 : d1;
    p.t2 = preset.t2 != 0 ? preset.t2 : d2;
    p.t3 = preset.t3 != 0 ? preset.t3 : d3;
    if (!(near + 1 <= p.t1 && p.t1 <= p.t2 && p.t2 <= p.t3 && p.t3 <= p.maxval))
        return Status(Errc::Malformed, "JPEG-LS: thresholds T1/T2/T3 out of range");
    p.reset = preset.reset != 0 ? preset.reset : 64;
    if (p.reset < 3 || p.reset > std::max(255, p.maxval))
        return Status(Errc::Malformed, "JPEG-LS: RESET out of range");

    p.range = (p.maxval + 2 * near) / (2 * near + 1) + 1;
    p.qbpp = ceil_log2(p.range);
    p.bpp = std::max(2, ceil_log2(p.maxval + 1));
    p.limit = 2 * (p.bpp + std::max(8, p.bpp));
    out = p;
    return Status::ok();
}

}  // namespace siaara::codec::jpegls::detail
