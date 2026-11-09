// SIAARA DICOM Toolkit - value representation rules (DICOM PS3.5 6.2, Table 6.2-1; 6.4 Value
// Multiplicity; 9.1 UID encoding rules) used by the validator and the value repair.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/types.hpp"

namespace siaara::repair {

// Maximum length of one value (bytes, or characters for LO/SH/PN/ST/LT/UC), 0 = no limit
// beyond the 32-bit length. `fixed` is set for VRs whose values have exactly that size.
struct VrLimit {
    std::uint32_t max = 0;
    bool fixed = false;
    bool in_characters = false;
};
VrLimit vr_limit(VR vr) noexcept;

// Result of checking one value (one backslash-separated part) of a string VR.
enum class ValueCheck : std::uint8_t {
    Ok,
    BadCharacter,   // a character the VR's repertoire does not allow
    BadFormat,      // characters fine but the value does not follow the VR's format
    TooLong,
    BadDate,        // a DA / DT whose fields are out of range (month 13, day 32 ...)
    BadTime,
};
// `value` is one value with padding already removed as PS3.5 allows for the VR.
ValueCheck check_value(VR vr, std::string_view value) noexcept;
std::string_view value_check_name(ValueCheck c) noexcept;

// An unambiguous normalised form of a malformed value, or nullopt when the intended value cannot
// be known. Only shapes whose meaning is certain are rewritten:
//   DA  "YYYY.MM.DD" (ACR-NEMA) and "YYYY-MM-DD"            -> "YYYYMMDD"
//   TM  "HH:MM[:SS[.F]]" (ACR-NEMA)                          -> "HHMM[SS[.F]]"
//   DS  value longer than 16 characters whose shortest round-trip form fits (same double)
//   DS / IS: surrounding spaces, a leading '+', leading zeros  ("+007" -> "7"), IS "12.0" -> "12"
//   UI  trailing spaces (space padding instead of NUL), leading/trailing spaces
//   AS  "nnnY" written lower case ("045y" -> "045Y")
//   CS  lower case letters -> upper case when the result is a valid CS value
// Never: dates in other orders (01/02/2020), decimal commas, truncation of over-long text.
std::optional<std::string> normalise_value(VR vr, std::string_view value);

// UID syntax (PS3.5 9.1): 1..64 characters, components of digits separated by '.', no empty
// component, no leading zero in a multi-digit component.
bool uid_is_valid(std::string_view uid) noexcept;

// Value Multiplicity string from the dictionary ("1", "1-n", "2-2n", "1-3", "6") checked against a count.
bool vm_allows(std::string_view vm, std::size_t count) noexcept;

// A new UID 2.25.<decimal of 128 bits> derived from `seed` with SHA-256 (UUID version 8 bits set,
// RFC 9562). The same seed always gives the same UID.
std::string uid_from_seed(std::string_view seed);

// Splits on '\\' (for VRs that allow multiple values); returns the count.
std::size_t split_count(std::string_view s) noexcept;

}  // namespace siaara::repair
