// SIAARA DICOM Toolkit - search normalisation for the local store.
//
// fold(): case folding and accent stripping for Latin scripts (Latin-1 Supplement, Latin
// Extended-A, the commonly used parts of Latin Extended-B, Latin Extended Additional including
// Vietnamese and Indic transliteration letters), lower-casing for Greek (tonos removed, final
// sigma folded) and Cyrillic (YO folded to IE), full-width ASCII to ASCII, removal of combining
// diacritical marks (U+0300..U+036F), and whitespace collapsing. CJK, kana, Hangul and every other
// script are kept as-is. Malformed UTF-8 bytes become U+FFFD.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace siaara::store {

std::string fold(std::string_view utf8);

// Normalised forms of a decoded PN value: one per non-empty component group (alphabetic,
// ideographic, phonetic). '^' separators become single spaces before folding.
std::array<std::string, 3> name_forms(std::string_view utf8_pn);
// Normalises a name search pattern the same way ('*' and '?' are kept).
std::string fold_name_pattern(std::string_view utf8);

// DICOM wildcard match (PS3.4 C.2.2.2.4): '*' any sequence (possibly empty), '?' exactly one
// character (a whole UTF-8 sequence). Byte-exact otherwise; callers fold both sides first when
// case/accent insensitivity is wanted.
bool wildcard_match(std::string_view pattern, std::string_view text) noexcept;
bool has_wildcards(std::string_view pattern) noexcept;

// Date key for range comparisons: YYYYMMDD as a number, accepting the ACR-NEMA "YYYY.MM.DD"
// form; 0 when the value is not a date.
std::uint32_t date_key(std::string_view da) noexcept;
// Parses a DICOM date range "A-B", "A-", "-B" or "A" into inclusive keys. False when malformed.
bool parse_date_range(std::string_view range, std::uint32_t& lo, std::uint32_t& hi) noexcept;

}  // namespace siaara::store
