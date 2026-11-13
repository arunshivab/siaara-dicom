// SIAARA DICOM Toolkit - DEFLATE (RFC 1951) raw stream support, written from the RFC.
#pragma once

#include <cstdint>
#include <vector>

#include "core/status.hpp"

namespace siaara::codec {

// Inflates a raw DEFLATE stream (no zlib/gzip header). `max_output` caps the output size
// (decompression-bomb protection, standards PARSE-16/PARSE-31). `consumed` receives the number
// of input bytes used. `produced` receives the number of bytes the stream inflated to before
// inflating ended, on every outcome: on success out.size(); on failure (truncated or corrupt
// stream, the max_output cap) what was produced before the failure, although `out` is then
// emptied (R-238: a caller charging inflated bytes against a budget charges this).
Status inflate_raw(const std::uint8_t* in, std::size_t in_size, std::size_t max_output,
                   std::vector<std::uint8_t>& out, std::size_t* consumed = nullptr,
                   std::size_t* produced = nullptr);

// Deflates into a raw DEFLATE stream. level 0..9 (0 = stored blocks only).
Status deflate_raw(const std::uint8_t* in, std::size_t in_size, int level,
                   std::vector<std::uint8_t>& out);

}  // namespace siaara::codec
