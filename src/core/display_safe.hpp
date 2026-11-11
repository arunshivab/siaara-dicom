// SIAARA DICOM Toolkit - display-safe copies of text that came from outside (file contents, file
// names in a ZIP, DICOMDIR File IDs).
//
// A display-safe copy is made NEXT TO the text, never instead of it: the text itself is always
// returned as it was. The copy is plain text that cannot reorder, hide or break the line it is
// shown in:
//   - C0 controls (U+0000..U+001F), DEL and C1 controls (U+007F..U+009F) become a space;
//   - the line separator U+2028 and the paragraph separator U+2029 become a space;
//   - Unicode format characters (general category Cf: bidirectional marks, embeddings, overrides
//     and isolates, zero-width characters, the byte order mark, tag characters, ...; the ranges
//     of Unicode 16) are removed;
//   - every byte that is not part of a well-formed UTF-8 sequence becomes U+FFFD (an encoded
//     surrogate, an overlong form or a value above U+10FFFF is not well formed), one per byte.
// Everything else is kept as it is. The copy is still text from outside: escape it before it
// goes into HTML.
//
// One implementation: the C API (siaara_text_display_safe), the grouping, the C# bindings
// (DicomAttachmentPreview.CaptionText) all call this.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace siaara::text {

// Is `c` a Unicode format character (general category Cf, Unicode 16)?
bool is_format_character(std::uint32_t c) noexcept;

// The display-safe copy of `utf8` (see above). Never longer than 3 x utf8.size() bytes.
std::string display_safe(std::string_view utf8);

// Length in bytes of display_safe(utf8), without building it.
std::size_t display_safe_size(std::string_view utf8) noexcept;

}  // namespace siaara::text
