// SIAARA DICOM Toolkit - file input/output (memory mapping, whole-file reads, atomic writes)
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::io {

// How a file opened by path is brought into memory (R-160).
//
// A memory-mapped file is not a private copy: if another process shortens the file while it is
// mapped (a file on a shared or network folder being rewritten in place), touching a page past
// the new end ends the process (SIGBUS on POSIX, an in-page exception on Windows). Reading the
// file into owned memory has no such exposure and costs the file's size in memory.
enum class OpenMode : std::uint8_t {
    Auto = 0,  // Read up to OpenOptions::read_threshold bytes, Map above it
    Map = 1,   // zero-copy mapping; see check_mapping() for what is re-checked
    Read = 2,  // the whole file in owned memory, at most OpenOptions::read_max_bytes
};
// The defaults of every open-by-path entry point. Change them here only.
inline constexpr OpenMode kDefaultOpenMode = OpenMode::Auto;
inline constexpr std::uint64_t kDefaultOpenReadThreshold = 256ull << 20;  // Auto: files up to 256 MiB are read
inline constexpr std::uint64_t kDefaultOpenReadMaxBytes = 4ull << 30;     // Read: the largest file accepted

struct OpenOptions {
    OpenMode mode = kDefaultOpenMode;
    std::uint64_t read_threshold = kDefaultOpenReadThreshold;
    // OpenMode::Read only: a larger file is Errc::LimitExceeded (nothing is read). An allocation
    // that fails is Errc::OutOfMemory.
    std::uint64_t read_max_bytes = kDefaultOpenReadMaxBytes;
    bool read_ahead = true;  // Map: hint the OS to prefetch; off for header-only access
};
// Opens a whole file read-only as `opt` says. The Bytes owns the memory or keeps the mapping alive.
Result<Bytes> open_file(const std::string& utf8_path, const OpenOptions& opt = {});

// Maps (or, where mapping is unavailable, reads) a whole file read-only.
// The returned Bytes keeps the mapping alive for as long as any view into it exists.
// `read_ahead` hints the OS to prefetch the whole file; turn it off for header-only scans.
// For the toolkit's own files (owner-only, only ever replaced by rename) and for callers that
// asked for OpenMode::Map; everything else opens through open_file().
Result<Bytes> map_file(const std::string& utf8_path, bool read_ahead = true);

// Reads the whole file into memory (no mapping). A file larger than `max_bytes` is
// Errc::LimitExceeded; a failed allocation is Errc::OutOfMemory.
Result<Bytes> read_file(const std::string& utf8_path, std::uint64_t max_bytes = UINT64_MAX);

// When `p` points into a file mapping made by this library: compares the file's current size
// with the mapped length and returns Errc::Truncated when the file has become shorter (a file
// replaced by rename is a different file and does not count). Ok for any other pointer. Called
// after mapping and again before pixel data is accessed through a mapping. This narrows the
// exposure described at OpenMode; it cannot close it - the file can still be shortened between
// this check and the access. Only OpenMode::Read (or Auto below the threshold) is safe.
Status check_mapping(const std::uint8_t* p) noexcept;

namespace detail {
// Registration of a live mapping for check_mapping (io.cpp and the store's header mapping).
// POSIX: `id_a` / `id_b` are st_dev / st_ino and `handle` is unused; Windows: `handle` is the
// open file HANDLE, which must stay open until unregister_mapping.
void register_mapping(const void* addr, std::size_t len, const std::string& utf8_path, std::uint64_t id_a,
                      std::uint64_t id_b, void* handle);
void unregister_mapping(const void* addr) noexcept;
}  // namespace detail

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
