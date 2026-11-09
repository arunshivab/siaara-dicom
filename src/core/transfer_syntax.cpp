#include "core/transfer_syntax.hpp"

#include <array>

namespace siaara {

namespace {
// UIDs and names from PS3.6 Table A-1 (transfer syntax entries), checked against PS3.5 Annex A.
constexpr std::array kTransferSyntaxes = {
    TransferSyntaxInfo{"1.2.840.10008.1.2", "Implicit VR Little Endian", false, false, false, false, Codec::None, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.1", "Explicit VR Little Endian", true, false, false, false, Codec::None, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.1.98", "Encapsulated Uncompressed Explicit VR Little Endian", true, false, true, false, Codec::None, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.1.99", "Deflated Explicit VR Little Endian", true, false, false, false, Codec::Deflate, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.2", "Explicit VR Big Endian", true, true, false, false, Codec::None, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.50", "JPEG Baseline (Process 1)", true, false, true, true, Codec::JpegBaseline, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.51", "JPEG Extended (Process 2 & 4)", true, false, true, true, Codec::JpegExtended, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.52", "JPEG Extended (Process 3 & 5)", true, false, true, true, Codec::JpegExtended, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.53", "JPEG Spectral Selection, Non-Hierarchical (Process 6 & 8)", true, false, true, true, Codec::JpegExtended, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.54", "JPEG Spectral Selection, Non-Hierarchical (Process 7 & 9)", true, false, true, true, Codec::JpegExtended, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.55", "JPEG Full Progression, Non-Hierarchical (Process 10 & 12)", true, false, true, true, Codec::JpegExtended, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.56", "JPEG Full Progression, Non-Hierarchical (Process 11 & 13)", true, false, true, true, Codec::JpegExtended, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.57", "JPEG Lossless, Non-Hierarchical (Process 14)", true, false, true, false, Codec::JpegLossless, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.58", "JPEG Lossless, Non-Hierarchical (Process 15)", true, false, true, false, Codec::JpegLossless, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.70", "JPEG Lossless, Non-Hierarchical, First-Order Prediction (Process 14 [Selection Value 1])", true, false, true, false, Codec::JpegLossless, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.80", "JPEG-LS Lossless Image Compression", true, false, true, false, Codec::JpegLs, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.81", "JPEG-LS Lossy (Near-Lossless) Image Compression", true, false, true, true, Codec::JpegLs, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.90", "JPEG 2000 Image Compression (Lossless Only)", true, false, true, false, Codec::Jpeg2000, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.91", "JPEG 2000 Image Compression", true, false, true, true, Codec::Jpeg2000, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.92", "JPEG 2000 Part 2 Multi-component Image Compression (Lossless Only)", true, false, true, false, Codec::Jpeg2000, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.93", "JPEG 2000 Part 2 Multi-component Image Compression", true, false, true, true, Codec::Jpeg2000, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.94", "JPIP Referenced", true, false, false, false, Codec::Jpip, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.95", "JPIP Referenced Deflate", true, false, false, false, Codec::Jpip, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.100", "MPEG2 Main Profile / Main Level", true, false, true, true, Codec::Mpeg2, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.100.1", "Fragmentable MPEG2 Main Profile / Main Level", true, false, true, true, Codec::Mpeg2, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.101", "MPEG2 Main Profile / High Level", true, false, true, true, Codec::Mpeg2, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.101.1", "Fragmentable MPEG2 Main Profile / High Level", true, false, true, true, Codec::Mpeg2, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.102", "MPEG-4 AVC/H.264 High Profile / Level 4.1", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.102.1", "Fragmentable MPEG-4 AVC/H.264 High Profile / Level 4.1", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.103", "MPEG-4 AVC/H.264 BD-compatible High Profile / Level 4.1", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.103.1", "Fragmentable MPEG-4 AVC/H.264 BD-compatible High Profile / Level 4.1", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.104", "MPEG-4 AVC/H.264 High Profile / Level 4.2 For 2D Video", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.104.1", "Fragmentable MPEG-4 AVC/H.264 High Profile / Level 4.2 For 2D Video", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.105", "MPEG-4 AVC/H.264 High Profile / Level 4.2 For 3D Video", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.105.1", "Fragmentable MPEG-4 AVC/H.264 High Profile / Level 4.2 For 3D Video", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.106", "MPEG-4 AVC/H.264 Stereo High Profile / Level 4.2", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.106.1", "Fragmentable MPEG-4 AVC/H.264 Stereo High Profile / Level 4.2", true, false, true, true, Codec::Mpeg4, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.107", "HEVC/H.265 Main Profile / Level 5.1", true, false, true, true, Codec::Hevc, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.108", "HEVC/H.265 Main 10 Profile / Level 5.1", true, false, true, true, Codec::Hevc, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.110", "JPEG XL Lossless", true, false, true, false, Codec::JpegXl, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.111", "JPEG XL JPEG Recompression", true, false, true, true, Codec::JpegXl, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.112", "JPEG XL", true, false, true, true, Codec::JpegXl, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.201", "High-Throughput JPEG 2000 Image Compression (Lossless Only)", true, false, true, false, Codec::Htj2k, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.202", "High-Throughput JPEG 2000 with RPCL Options Image Compression (Lossless Only)", true, false, true, false, Codec::Htj2k, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.203", "High-Throughput JPEG 2000 Image Compression", true, false, true, true, Codec::Htj2k, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.204", "JPIP HTJ2K Referenced", true, false, false, false, Codec::Jpip, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.4.205", "JPIP HTJ2K Referenced Deflate", true, false, false, false, Codec::Jpip, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.5", "RLE Lossless", true, false, true, false, Codec::Rle, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.8.1", "Deflated Image Frame Compression", true, false, true, false, Codec::DeflateFrame, false},
    // GE private syntax (not in PS3.6): implicit VR little endian, Pixel Data samples big endian.
    TransferSyntaxInfo{"1.2.840.113619.5.2", "GE Private Implicit VR Little Endian with Big Endian Pixel Data", false, false, false, false, Codec::None, true, true},
    TransferSyntaxInfo{"1.2.840.10008.1.2.7.1", "SMPTE ST 2110-20 Uncompressed Progressive Active Video", true, false, false, false, Codec::Smpte2110, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.7.2", "SMPTE ST 2110-20 Uncompressed Interlaced Active Video", true, false, false, false, Codec::Smpte2110, false},
    TransferSyntaxInfo{"1.2.840.10008.1.2.7.3", "SMPTE ST 2110-30 PCM Digital Audio", true, false, false, false, Codec::Smpte2110, false},
};
}  // namespace

const TransferSyntaxInfo* find_transfer_syntax(std::string_view uid) noexcept {
    for (const auto& t : kTransferSyntaxes)
        if (t.uid == uid) return &t;
    return nullptr;
}

}  // namespace siaara
