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
    // Where spool files (patient data) are written. Empty: a private per-user directory under the
    // system temporary directory ("siaara-spool-<uid>", mode 0700 and owned by the user on POSIX;
    // an owner-only DACL on Windows), which is refused if it exists with other owners or access.
    // Files are always created exclusively (no symlinks followed), readable by the owner only.
    std::string directory;
    std::uint64_t max_size = 64ull << 30;     // largest data set accepted on disk (bytes)
    // Leftover "siaara-spool-*.part" files (e.g. from a crash) not modified for this long are
    // removed from the directory the first time it is used in a process. 0: never.
    int stale_after_minutes = 60;
};

// The directory spool files go to for `cfg` (creating and checking the private default).
Result<std::string> spool_directory(const SpoolConfig& cfg);
// Removes stale "siaara-spool-*.part" files from the spool directory (see stale_after_minutes).
// Returns the number removed. Called automatically on first use of each directory.
Result<std::size_t> remove_stale_spool_files(const SpoolConfig& cfg);

// A received data set on disk (a complete Part 10 file), memory-mapped read-only.
// The file owns its mapping: every Bytes view of the data set keeps the SpoolFile alive, so the
// mapping is released, and an uncommitted file deleted, exactly when the last reference goes.
// On Windows the file is mapped through a handle opened with FILE_SHARE_DELETE, so commit() can
// rename it while it is mapped; deletion waits until the view and the handles are closed (a mapped
// file cannot be deleted on Windows). Other code that keeps the file open without
// FILE_SHARE_DELETE (e.g. core io::map_file) blocks the rename and the deletion while it does so.
class SpoolFile {
public:
    // Opens and maps `path`; the data set starts at `data_offset`. On failure the file is removed.
    static Result<std::shared_ptr<SpoolFile>> open(const std::string& path, std::uint64_t data_offset);
    ~SpoolFile();
    SpoolFile(const SpoolFile&) = delete;
    SpoolFile& operator=(const SpoolFile&) = delete;

    // Current location of the file (the temporary name until committed).
    std::string path() const;
    // Byte offset of the data set in the file (after preamble and file meta).
    std::uint64_t data_offset() const noexcept { return data_offset_; }
    // The whole mapped file.
    const std::uint8_t* data() const noexcept { return base_; }
    std::uint64_t size() const noexcept { return size_; }
    // Moves the file to `target` now, replacing an existing file (an atomic rename when both are on
    // the same volume; otherwise a copy to a temporary name next to `target` renamed into place).
    // Synchronous on every platform; the mapping and all views stay valid. Returns IoError and
    // leaves the spool file uncommitted on failure.
    Status commit(const std::string& target);
    bool committed() const;

private:
    SpoolFile() = default;
    struct Native;
    mutable std::mutex mu_;
    std::string path_;
    std::uint64_t data_offset_ = 0;
    bool committed_ = false;
    bool delete_original_ = false;  // a cross-volume commit copied the file; remove the original
    std::string original_;          // the spool name, when delete_original_
    const std::uint8_t* base_ = nullptr;
    std::uint64_t size_ = 0;
    std::unique_ptr<Native> native_;
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
