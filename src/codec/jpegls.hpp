// SIAARA DICOM Toolkit - JPEG-LS (ITU-T T.87 | ISO/IEC 14495-1), written from the Recommendation.
//
// The public entry points are decode_jpegls / encode_jpegls in codec/codec.hpp. This header adds
// the finer-grained encoder options and a header probe used by tests and test tools.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/codec.hpp"

namespace siaara::codec::jpegls {

// Preset coding parameters as carried by an LSE marker segment of type 1 (T.87 C.2.4.1.1).
// A value of 0 selects the default for that parameter.
struct PresetParams {
    int maxval = 0;
    int t1 = 0;
    int t2 = 0;
    int t3 = 0;
    int reset = 0;
};

struct EncodeOptions {
    int near = 0;                         // NEAR, 0 = lossless
    int interleave = -1;                  // 0, 1, 2; -1 = 0 for planar input, 2 otherwise
    std::uint32_t restart_interval = 0;   // lines per restart interval, 0 = none
    bool write_preset = false;            // emit an LSE type 1 segment carrying `preset`
    PresetParams preset;
};

Status encode(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeOptions& opt,
              std::vector<std::uint8_t>& out);

// What the frame and first scan headers of a codestream say (for tools and tests).
struct StreamInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int precision = 0;
    int components = 0;
    int near = 0;             // NEAR of the first scan
    int interleave = 0;       // ILV of the first scan
    int color_transform = 0;  // APP8 "mrfx" value, 0 = none
};

Status probe(std::span<const std::uint8_t> in, StreamInfo& out);

}  // namespace siaara::codec::jpegls
