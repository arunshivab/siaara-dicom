// SIAARA DICOM Toolkit - DEFLATE (RFC 1951) raw stream support, written from the RFC.
#pragma once

#include <cstdint>
#include <vector>

#include "core/status.hpp"

namespace siaara::codec {

// Inflates a raw DEFLATE stream (no zlib/gzip header). `max_output` caps the output size
// (decompression-bomb protection, standards PARSE-16/PARSE-31). `consumed` receives the number
// of input bytes used.
Status inflate_raw(const std::uint8_t* in, std::size_t in_size, std::size_t max_output,
                   std::vector<std::uint8_t>& out, std::size_t* consumed = nullptr);

// Deflates into a raw DEFLATE stream. level 0..9 (0 = stored blocks only).
Status deflate_raw(const std::uint8_t* in, std::size_t in_size, int level,
                   std::vector<std::uint8_t>& out);

}  // namespace siaara::codec
