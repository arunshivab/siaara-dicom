// SIAARA DICOM Toolkit - transfer syntaxes (PS3.5 section 10 and Annex A, PS3.6 table A-1)
#pragma once

#include <string_view>

namespace siaara {

enum class Codec {
    None,           // native (uncompressed) pixel data
    Deflate,        // whole data set deflated
    DeflateFrame,   // Deflated Image Frame Compression: each frame deflated separately (PS3.5 A.4.13)
    Rle,
    JpegBaseline,   // process 1
    JpegExtended,   // processes 2 and 4
    JpegLossless,   // process 14 (all predictors) and process 14 SV1
    JpegLs,         // lossless and near-lossless
    Jpeg2000,       // lossless and lossy, including Part 2 multi-component
    Htj2k,          // High-Throughput JPEG 2000 (lossless, RPCL lossless, lossy)
    JpegXl,         // JPEG XL (lossless, JPEG recompression, lossy)
    Mpeg2,
    Mpeg4,
    Hevc,
    Smpte2110,      // SMPTE ST 2110 uncompressed/compressed video (retain only)
    Jpip,           // referenced pixel data
    Unknown,
};

struct TransferSyntaxInfo {
    std::string_view uid;
    std::string_view name;
    bool explicit_vr;
    bool big_endian;
    bool encapsulated;
    bool lossy;
    Codec codec;
    bool retired;
};

// Looks up a transfer syntax by UID (padding must already be removed). Returns nullptr if unknown.
const TransferSyntaxInfo* find_transfer_syntax(std::string_view uid) noexcept;

namespace ts {
constexpr std::string_view ImplicitVRLittleEndian = "1.2.840.10008.1.2";
constexpr std::string_view ExplicitVRLittleEndian = "1.2.840.10008.1.2.1";
constexpr std::string_view EncapsulatedUncompressedExplicitVRLittleEndian = "1.2.840.10008.1.2.1.98";
constexpr std::string_view DeflatedExplicitVRLittleEndian = "1.2.840.10008.1.2.1.99";
constexpr std::string_view ExplicitVRBigEndian = "1.2.840.10008.1.2.2";
}  // namespace ts

}  // namespace siaara
