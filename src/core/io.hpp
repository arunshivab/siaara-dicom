// SIAARA DICOM Toolkit - file input/output (memory mapping, whole-file reads, atomic writes)
#pragma once

#include <cstdint>
#include <functional>
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

// One piece of a file written by write_parts_atomic.
struct Slice {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// write_file_atomic for content that exists in several pieces (e.g. a file meta block and a mapped
// data set) and whose final name is known only once every byte was seen (e.g. a content hash):
// the pieces are written in order, in chunks of at most kWriteChunk bytes, to an exclusive
// owner-only temporary file in the folder of `folder_path` (any path inside the target folder;
// only its folder part is used). `observe` (may be empty) sees every chunk, in order, before it
// is written. After the last byte is flushed `final_path` is asked for the target; when it
// returns an error the temporary file is removed and that error is returned (nothing is kept).
// Nothing of the content's size is ever allocated.
inline constexpr std::size_t kWriteChunk = std::size_t{1} << 20;
Status write_parts_atomic(const std::string& folder_path, const Slice* parts, std::size_t part_count,
                          const std::function<void(const std::uint8_t*, std::size_t)>& observe,
                          const std::function<Result<std::string>()>& final_path, WriteMode mode = WriteMode::Replace);

}  // namespace siaara::io
