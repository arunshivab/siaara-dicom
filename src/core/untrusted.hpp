// SIAARA DICOM Toolkit - read options for bytes that came from outside (a mail attachment, a
// member of a ZIP): the attachment preview's limits (examples/c/attachment_preview.h
// preview_default_limits, C# DicomPreviewOptions), for the readers inside the library that take
// such bytes (the DICOMDIR reader from memory, the grouping).
#pragma once

#include <algorithm>
#include <cstdint>

#include "core/reader.hpp"

namespace siaara {

inline constexpr std::uint32_t kUntrustedMaxDepth = 16;                         // sequence nesting
inline constexpr std::uint64_t kUntrustedMaxElements = 200'000;                 // elements, and items of the whole read
inline constexpr std::uint64_t kUntrustedMaxInflatedSize = std::uint64_t{128} << 20;  // a deflated data set inflates to at most this
inline constexpr std::uint64_t kUntrustedMaxValueBytes = 65'536;               // a longer text value is not decoded

// Part 10 only (no raw data sets), lenient, no overlong-length recovery, and the limits given
// (each 0 = the kUntrusted* default). max_elements also bounds sequence items (per sequence and
// in all) and fragments, as siaara_read_options.max_elements does.
inline ReadOptions untrusted_read_options(std::uint32_t max_depth = 0, std::uint64_t max_elements = 0,
                                          std::uint64_t max_inflated_size = 0, bool header_only = true) {
    ReadOptions o;
    o.stop_before_pixel_data = header_only;
    o.allow_raw_dataset = false;
    o.lenient = true;
    o.recover_overlong_lengths = false;
    o.max_depth = max_depth ? max_depth : kUntrustedMaxDepth;
    o.max_elements = max_elements ? max_elements : kUntrustedMaxElements;
    o.max_items_per_sequence = std::min(o.max_items_per_sequence, o.max_elements);
    o.max_items_total = o.max_elements;
    o.max_fragments = std::min(o.max_fragments, o.max_elements);
    o.max_inflated_size = max_inflated_size ? max_inflated_size : kUntrustedMaxInflatedSize;
    return o;
}

}  // namespace siaara
