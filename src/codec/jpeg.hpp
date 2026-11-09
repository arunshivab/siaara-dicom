// SIAARA DICOM Toolkit - JPEG codec family (ITU-T T.81 | ISO/IEC 10918-1).
// Written from T.81 and DICOM PS3.5 section 8.2.1 / Annex A.4.1.
//
// Decoding (decode_jpeg in codec.hpp):
//   - processes 1 (baseline), 2/4 (extended sequential, 8/12-bit, Huffman), 10/12 (progressive,
//     spectral selection + successive approximation, 8/12-bit) and 14 (lossless, predictors 1-7,
//     point transform, 2..16-bit precision); arithmetic coding and hierarchical mode are refused
//     with Errc::Unsupported.
//   - 1..4 components, any sampling factors 1..4, restart intervals, multiple scans, DNL.
//     A lossless scan whose restart interval is not a whole number of MCU rows (forbidden by
//     T.81 H.1.1, and decoded differently by different decoders) is rejected as Malformed.
//   - Colour values are returned AS STORED in the codestream: no YCbCr -> RGB conversion is done
//     here, whatever the APP0/APP14 markers say. The image layer converts according to the
//     Photometric Interpretation (YBR_FULL, YBR_FULL_422, RGB ...).
//   - Subsampled components are brought to full size by sample replication (each stored sample
//     covers Hmax/Hi x Vmax/Vi output samples); no interpolation filter is applied.
//   - Output is DICOM native: 8-bit samples when bits_allocated == 8, little-endian 16-bit when
//     bits_allocated == 16; colour-by-pixel unless FrameInfo::planar.
//   - Lossless with FrameInfo::is_signed: samples are sign-extended from min(P, bits_stored) bits.
//     Lossy (DCT) data is returned as the unsigned values the codestream holds.
//   - The SOF header must agree with FrameInfo: rows (or 0 + a matching DNL), columns, number of
//     components == samples_per_pixel, and precision P <= bits_allocated with
//     bits_allocated == 8 for P <= 8 or 16 otherwise (P <= 8 into 16-bit storage is accepted).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/codec.hpp"

namespace siaara::codec {

// Extra encoder controls that EncodeParams does not carry. encode_jpeg() uses the defaults.
struct JpegEncodeOptions {
    // Luma sampling factors for lossy 3-component images; chroma is always 1x1.
    //   1x1 = 4:4:4 (default), 2x1 = 4:2:2, 2x2 = 4:2:0, 4x1 = 4:1:1.
    int luma_h = 1;
    int luma_v = 1;
    std::uint16_t restart_interval = 0;  // MCUs per restart interval, 0 = none. Lossless: must be a
                                         // multiple of the columns (T.81 H.1.1: whole MCU rows)
    int point_transform = 0;             // lossless only, 0 .. P-1
    bool optimize_huffman = true;        // false: T.81 Annex K example tables (8-bit lossy only)
    bool separate_scans = false;         // one scan per component instead of one interleaved scan
};

Status encode_jpeg_ex(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                      const JpegEncodeOptions& opt, std::vector<std::uint8_t>& out);

// Frame header summary, read without decoding (for callers that need to inspect a codestream).
struct JpegHeaderInfo {
    int sof_marker = 0;       // 0xC0 .. 0xCF
    int precision = 0;
    std::uint32_t rows = 0;   // 0 when defined by DNL
    std::uint32_t columns = 0;
    int components = 0;
    bool lossless = false;
    bool progressive = false;
};
Status jpeg_read_header(std::span<const std::uint8_t> in, JpegHeaderInfo& hdr);

}  // namespace siaara::codec
