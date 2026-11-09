// SIAARA DICOM Toolkit - store helpers: CRC-32, SHA-256, UID generation, UTF-8 paths,
// low-level file I/O with sync/truncate, and an inter-process lock file.
//
// Everything here is written from the public specifications (ISO 3309 / ITU-T V.42 CRC-32,
// FIPS 180-4 SHA-256, ISO/IEC 9834-8 "2.25" UUID-derived UIDs). No exceptions escape: all
// std::filesystem calls use the error_code overloads.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::store {

// ---- checksums and hashes
std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed = 0) noexcept;
std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size) noexcept;
std::string to_hex(const std::uint8_t* data, std::size_t size);

// A new UID of the form 2.25.<decimal of 128 random bits> (PS3.5 B.2, ISO/IEC 9834-8).
std::string generate_uid();

// ---- paths (std::filesystem paths are wchar_t on Windows: convert through u8string)
std::filesystem::path u8path(std::string_view utf8);
std::string to_utf8(const std::filesystem::path& p);

// ---- low-level file
// A file handle with explicit append, positional read, flush to disk and truncate.
class OsFile {
public:
    OsFile() = default;
    ~OsFile();
    OsFile(const OsFile&) = delete;
    OsFile& operator=(const OsFile&) = delete;
    OsFile(OsFile&& o) noexcept;
    OsFile& operator=(OsFile&& o) noexcept;

    enum class Mode { ReadOnly, ReadWrite, CreateTruncate };
    static Result<OsFile> open(const std::filesystem::path& p, Mode mode);

    bool is_open() const noexcept;
    Result<std::uint64_t> size() const;
    // Reads exactly `n` bytes at `offset` (fails with Truncated if the file is shorter).
    Status read_at(std::uint64_t offset, void* buf, std::size_t n) const;
    // Writes all bytes at the end of the file.
    Status append(const void* data, std::size_t n);
    Status truncate(std::uint64_t size);
    Status sync();  // flush OS buffers to the device (fsync / FlushFileBuffers)
    void close() noexcept;

private:
#if defined(_WIN32)
    void* h_ = nullptr;  // HANDLE; nullptr = closed
#else
    int fd_ = -1;
#endif
};

// Maps a whole file read-only for header access: unlike io::map_file it asks the OS not to read
// ahead (MADV_RANDOM / no sequential-scan hint), so parsing the header of a large file touches only
// the pages it reads. Falls back to io::read_file where mapping is unavailable.
Result<Bytes> map_for_header(const std::string& utf8_path);

// Reads a whole (small) file. Fails with IoError when it cannot be read.
Result<std::vector<std::uint8_t>> read_whole(const std::filesystem::path& p);
// Writes `data` to `<p>.tmp-<random>`, optionally syncs, then renames over `p`
// (and syncs the folder on POSIX when `durable`).
Status write_replace(const std::filesystem::path& p, const std::uint8_t* data, std::size_t size,
                     bool durable);
// Flushes a folder's entry table to disk (POSIX); a no-op where unsupported.
void sync_dir(const std::filesystem::path& dir) noexcept;

// An exclusive, non-blocking inter-process lock held for the lifetime of the object.
// POSIX: flock(LOCK_EX|LOCK_NB) on the lock file (released by the OS if the process dies).
// Windows: the lock file is opened with no sharing (also released when the process dies).
// A process-wide registry additionally refuses a second lock of the same file in this process.
class LockFile {
public:
    LockFile() = default;
    ~LockFile();
    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;
    LockFile(LockFile&& o) noexcept;
    LockFile& operator=(LockFile&& o) noexcept;

    // Fails with Errc::Rejected when another process (or this one) holds the lock.
    static Result<LockFile> acquire(const std::filesystem::path& p);
    void release() noexcept;

private:
    std::string key_;
#if defined(_WIN32)
    void* h_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// ---- byte serialisation (little endian, varint lengths), bounds-checked reading
class ByteWriter {
public:
    std::vector<std::uint8_t> buf;
    void u8(std::uint8_t v) { buf.push_back(v); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f64(double v);
    void varint(std::uint64_t v);
    void str(std::string_view s);
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* p, std::size_t n) noexcept : p_(p), n_(n) {}
    bool ok() const noexcept { return ok_; }
    bool at_end() const noexcept { return pos_ == n_; }
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    double f64();
    std::uint64_t varint();
    std::string str();

private:
    bool need(std::size_t k) noexcept;
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace siaara::store
