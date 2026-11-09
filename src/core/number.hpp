// SIAARA DICOM Toolkit - locale-independent, correctly rounded decimal-to-double conversion.
#pragma once

#include <optional>
#include <string_view>

namespace siaara {

// Parses a decimal floating-point token ([+-]digits[.digits][(e|E)[+-]digits]) in the "C" locale,
// with correct rounding (delegates to the C library's strtod with an explicit C locale, so it
// never depends on the process locale and works on libc++ where floating std::from_chars is
// missing). Returns nullopt for malformed text or overflow. Underflow returns the (signed) zero or
// subnormal the C library produces.
std::optional<double> parse_decimal_c(std::string_view token);

}  // namespace siaara
