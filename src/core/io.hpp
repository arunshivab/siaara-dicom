// SIAARA DICOM Toolkit - file input/output (memory mapping, whole-file reads, atomic writes)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::io {

// Maps (or, where mapping is unavailable, reads) a whole file read-only.
// The returned Bytes keeps the mapping alive for as long as any view into it exists.
Result<Bytes> map_file(const std::string& utf8_path);

// Reads the whole file into memory (no mapping).
Result<Bytes> read_file(const std::string& utf8_path);

// Writes bytes to a temporary file in the same folder, then renames it over the target,
// so a crash never leaves a half-written DICOM file.
Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size);

}  // namespace siaara::io
