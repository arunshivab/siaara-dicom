// SIAARA DICOM Toolkit - header-to-pixel consistency (PS3.3 C.7.6.3 Image Pixel module,
// C.7.6.6 Multi-frame; PS3.5 8.1.1 native frame sizes, 8.2 / A.4 encapsulation, the Basic Offset
// Table and C.7.6.3.1.8 Extended Offset Table) and pixel rescue of damaged files.
//
// The image attributes are compared with what the pixel data itself says: the native value length,
// or the codestream headers (JPEG SOF, JPEG-LS SOF55, JPEG 2000 SIZ/COD, RLE header) read with the
// codec probes. A fix is proposed only when exactly one consistent set of attributes explains the
// pixel data; otherwise the finding is reported and nothing is changed.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/dataset.hpp"
#include "repair/report.hpp"
#include "repair/scan.hpp"

namespace siaara::repair {

struct AttrFix {
    Tag tag = 0;
    VR vr = VR::US;
    bool text = false;       // CS / IS value in `str`, else the binary `number`
    std::uint32_t number = 0;
    std::string str;
    std::string reason;      // why (no data set values other than image attributes)
};

struct FrameSpan {
    std::uint32_t first = 0;  // index into the data fragments (offset table item excluded)
    std::uint32_t count = 0;
};

struct PixelAnalysis {
    bool present = false;
    bool encapsulated = false;
    std::uint32_t declared_frames = 1;
    std::vector<Finding> findings;
    std::vector<AttrFix> fixes;           // header fixes (category pixels)
    // encapsulated
    std::vector<FrameSpan> frames;        // frames as found in the fragments
    std::string layout_source;            // "basic offset table", "one fragment per frame", ...
    bool bot_invalid = false;             // Basic Offset Table present but wrong
    bool eot_invalid = false;             // Extended Offset Table present but wrong
    // native
    std::uint64_t frame_bits = 0;
    std::uint64_t native_expected = 0;    // bytes the header describes
    std::uint64_t native_actual = 0;
    std::uint32_t native_complete_frames = 0;
};

// `scan` may be null (data set built in memory).
PixelAnalysis analyse_pixels(const Dataset& ds, std::string_view transfer_syntax_uid, const PixelScan* scan);

// Basic Offset Table bytes for `frames` of `fragments` (data fragments only), or empty when an
// offset would not fit 32 bits.
std::vector<std::uint8_t> build_basic_offset_table(const std::vector<Bytes>& data_fragments,
                                                   const std::vector<FrameSpan>& frames, bool* overflow);

// The encoded bytes of one frame (its fragments concatenated).
std::vector<std::uint8_t> frame_bytes_of(const std::vector<Bytes>& data_fragments, const FrameSpan& f);

}  // namespace siaara::repair
