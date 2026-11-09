// SIAARA DICOM Toolkit - pixel data transcoding between transfer syntaxes.
// Written from DICOM PS3.5 section 8 (native and encapsulated pixel data), 10 and Annex A
// (transfer syntaxes, A.4 fragments and the Basic Offset Table), PS3.3 C.7.6.3 (Image Pixel
// module: Photometric Interpretation, Planar Configuration), C.7.6.3.1.8 (Extended Offset Table)
// and C.7.6.1.1.5 (Lossy Image Compression, Ratio and Method).
//
// What a transcode does:
//   - native <-> native (implicit LE, explicit LE, explicit BE, deflated): the data set is unchanged;
//     the writer re-encodes it for the new transfer syntax.
//   - anything else: every frame is decoded with image::FrameReader and re-encoded with the target
//     codec, one fragment per frame, each padded to even length, with a Basic Offset Table (or an
//     Extended Offset Table when the offsets do not fit 32 bits, see OffsetTableMode).
//   - Photometric Interpretation follows the encoding (PS3.5 8.2):
//       JPEG lossy colour       -> YBR_FULL_422 (4:2:2) or YBR_FULL (4:4:4)
//       JPEG 2000 / HTJ2K colour -> YBR_RCT (reversible) or YBR_ICT (irreversible) when the
//                                  multi-component transform is applied to RGB input
//       decoded lossy YBR        -> RGB (YBR_FULL_422 / YBR_FULL from a lossy source, YBR_RCT/ICT)
//     Planar Configuration is 0 for every colour result (encapsulated codecs carry their own order).
//   - Rows, Columns, Bits Allocated, Bits Stored, High Bit, Pixel Representation and Number of
//     Frames are never changed.
//   - When lossy compression is applied, Lossy Image Compression (0028,2110) becomes "01" and one
//     value is appended to Lossy Image Compression Ratio (0028,2112) and Method (0028,2114).
//     Once "01" it stays "01": decompressing lossy data, or re-encoding it losslessly, never claims
//     the pixel data is lossless again.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/frames.hpp"

namespace siaara::codec {

enum class OffsetTableMode {
    Auto,      // Basic Offset Table when all offsets fit 32 bits, else Extended Offset Table
    Basic,     // always a Basic Offset Table (fails with LimitExceeded when offsets overflow)
    Extended,  // Extended Offset Table + Lengths with an empty Basic Offset Table
    Empty,     // empty Basic Offset Table, no Extended Offset Table
};

struct TranscodeParams {
    // Lossy codecs (JPEG baseline/extended, JPEG 2000 irreversible): 1..100.
    int quality = 90;
    // Transfer syntaxes that allow both lossless and lossy coding (JPEG 2000 ".91", JPEG-LS ".81",
    // HTJ2K ".203"): false = code losslessly in them.
    bool lossy = false;
    // JPEG-LS NEAR used for ".81" when `lossy` (1..255).
    int near_lossless = 2;
    // Refuse (Errc::Rejected) any transcode that would apply lossy compression.
    bool allow_lossy = true;
    // JPEG Lossless Process 14 ".57": selection value 1..7 (".70" always uses 1).
    int predictor = 1;
    // JPEG lossy colour: 0 = 4:4:4 (YBR_FULL), 1 = 4:2:2 (YBR_FULL_422).
    int chroma_subsampling = 1;
    OffsetTableMode offset_table = OffsetTableMode::Auto;
    // A transcode that applies lossy compression makes a new image (PS3.3 C.7.6.1.1.5): by default
    // it gets a new SOP Instance UID, Image Type value 1 DERIVED, a Source Image Sequence item
    // referencing the predecessor (CID 7202: 121320 Uncompressed predecessor, or 121330 Lossy
    // compressed predecessor when it was lossy already), a Derivation Code Sequence item
    // (CID 7203: 113040 Lossy Compression) and a Derivation Description with method and ratio.
    // true keeps the SOP Instance UID and leaves those attributes alone (Lossy Image Compression,
    // Ratio and Method are always recorded).
    bool keep_uid_on_lossy = false;
    // Frame decoding limits (memory budget also bounds the native output buffer).
    image::FrameOptions frames;
};

struct TranscodeReport {
    bool pixel_data_recoded = false;  // frames were decoded and encoded again
    bool lossy_applied = false;       // this transcode applied lossy compression
    bool source_lossy = false;        // the source was already lossy (attribute or codestream)
    std::uint64_t encoded_bytes = 0;  // total encoded frame bytes (encapsulated targets)
    std::uint64_t native_bytes = 0;   // total decoded frame bytes
    std::string new_sop_instance_uid; // set when a lossy transcode gave the image a new UID
};

// Transfer syntaxes the transcoder can write / read pixel data in.
bool can_encode(std::string_view ts_uid) noexcept;
bool can_decode(std::string_view ts_uid) noexcept;
// True when encoding in `ts_uid` with these parameters is lossy.
bool encoding_is_lossy(std::string_view ts_uid, const TranscodeParams& p) noexcept;

// Transcodes the top-level data set `ds` (encoded in `source_ts`) to `target_ts`. Element values
// that do not change are shared with `ds`, not copied. Errors: Unsupported (no codec for the
// pair, or the image cannot be represented in the target, e.g. 16-bit data in JPEG baseline),
// Rejected (lossy coding with allow_lossy == false), plus decoder/encoder errors.
Result<Dataset> transcode_dataset(const Dataset& ds, std::string_view source_ts, std::string_view target_ts,
                                  const TranscodeParams& p = {}, TranscodeReport* report = nullptr);

// Transcodes a Part 10 file in place (data set, transfer syntax and file meta Transfer Syntax UID).
// On failure the file is unchanged.
Status transcode_file(DicomFile& file, std::string_view target_ts, const TranscodeParams& p = {},
                      TranscodeReport* report = nullptr);

}  // namespace siaara::codec
