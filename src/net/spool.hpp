// SIAARA DICOM Toolkit - receiving large data sets straight to disk.
// While a C-STORE data set arrives (PS3.8 P-DATA-TF fragments, PS3.7 C-STORE-RQ), once it grows
// past SpoolConfig::threshold the fragments are written to a temporary Part 10 file (preamble,
// file meta built from the C-STORE command, then the data set bytes exactly as received) instead
// of being held in memory. When the message is complete the file is memory-mapped and parsed from
// the mapping, so the data set never has to fit in memory twice.
//
// The receiving callback gets the file as StoreRequest::spool and may move it into its final place
// with SpoolFile::commit (an atomic rename: the spool directory should be on the same file system
// as the destination). A spool file that was not committed is deleted when the last reference to
// it (including the memory mapping) goes away.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::net {

struct SpoolConfig {
    std::uint64_t threshold = 64ull << 20;   // data sets larger than this go to disk (bytes)
    std::string directory;                    // empty: the system temporary directory
    std::uint64_t max_size = 64ull << 30;     // largest data set accepted on disk (bytes)
};

// A received data set on disk (a complete Part 10 file).
class SpoolFile {
public:
    explicit SpoolFile(std::string path, std::uint64_t data_offset)
        : path_(std::move(path)), data_offset_(data_offset) {}
    ~SpoolFile();
    SpoolFile(const SpoolFile&) = delete;
    SpoolFile& operator=(const SpoolFile&) = delete;

    // Current location of the file (the temporary name until committed).
    std::string path() const;
    // Byte offset of the data set in the file (after preamble and file meta).
    std::uint64_t data_offset() const noexcept { return data_offset_; }
    // Moves the file to `target` atomically, replacing an existing file. On POSIX systems the rename
    // happens now. On Windows a mapped file cannot be renamed, so the rename is done when the last
    // view of the data set is released; errors are then only visible through committed()/path().
    Status commit(const std::string& target);
    bool committed() const;

private:
    mutable std::mutex mu_;
    std::string path_;
    std::string pending_target_;
    std::uint64_t data_offset_ = 0;
    bool committed_ = false;
};

// Writes one data set to a spool file. Used by Association::receive.
class SpoolWriter {
public:
    // `prefix` is written first (the Part 10 header); the data set follows.
    static Result<std::unique_ptr<SpoolWriter>> open(const SpoolConfig& cfg, const std::vector<std::uint8_t>& prefix);
    ~SpoolWriter();
    SpoolWriter(const SpoolWriter&) = delete;
    SpoolWriter& operator=(const SpoolWriter&) = delete;

    Status write(const std::uint8_t* data, std::size_t n);
    std::uint64_t data_bytes() const noexcept { return written_; }
    // Closes and memory-maps the file. `data` views the data set bytes; `file` owns the file.
    struct Finished {
        Bytes data;
        std::shared_ptr<SpoolFile> file;
    };
    Result<Finished> finish();

private:
    SpoolWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    std::uint64_t prefix_size_ = 0;
    std::uint64_t written_ = 0;
};

// Part 10 header (preamble, "DICM", file meta) for a received instance, as the spool file starts.
Status part10_header(std::string_view sop_class, std::string_view sop_instance, std::string_view transfer_syntax,
                     std::string_view source_ae, std::vector<std::uint8_t>& out);

}  // namespace siaara::net
