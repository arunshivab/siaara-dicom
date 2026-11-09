// SIAARA DICOM Toolkit - base64 (RFC 4648 section 4, standard alphabet, with padding).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"

namespace siaara::web {

// Appends the base64 encoding of `data` to `out`. Fails only when the output size overflows.
Status base64_encode(std::span<const std::uint8_t> data, std::string& out);
std::string base64_encode(std::string_view data);

// Decodes standard base64. Padding is optional; ASCII whitespace (CR, LF, TAB, space) between
// quanta is skipped (PS3.19 XML may wrap lines). Any other character, a lone trailing symbol
// or non-zero padding bits is an error. `max_out` caps the decoded size.
Status base64_decode(std::string_view text, std::vector<std::uint8_t>& out,
                     std::uint64_t max_out = UINT64_MAX);

}  // namespace siaara::web
