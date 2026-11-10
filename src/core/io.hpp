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
// `read_ahead` hints the OS to prefetch the whole file; turn it off for header-only scans.
Result<Bytes> map_file(const std::string& utf8_path, bool read_ahead = true);

// Reads the whole file into memory (no mapping).
Result<Bytes> read_file(const std::string& utf8_path);

enum class WriteMode : std::uint8_t {
    Replace,    // an existing target is replaced (atomically)
    NoReplace,  // an existing target is kept: Errc::Rejected, nothing written (no check-then-write race)
};

// Writes bytes to a temporary file in the same folder, then renames it over the target, so a crash
// never leaves a half-written DICOM file. The temporary file has an unpredictable name and is
// created exclusively without following symlinks, readable by the owner only (mode 0600 on POSIX,
// an owner-only DACL on Windows), so the written file is owner-only too (R-071): it may hold
// patient data. Concurrent writers of one target never share a temporary file; the last rename wins.
Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size,
                         WriteMode mode = WriteMode::Replace);

}  // namespace siaara::io
