// SIAARA DICOM Toolkit - multi-threaded folder scan for import.
//
// Walks a folder (or, with use_dicomdir, the files a DICOMDIR at its top lists), skips
// non-DICOM files after looking at their first bytes (preamble + "DICM", or a plausible raw data
// set start), and extracts an IndexRecord from every DICOM file on a pool of worker threads.
// The callback is never called concurrently (calls are serialised), but it may be called from
// any of the worker threads. Symbolic links are not followed unless asked.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/status.hpp"
#include "store/index.hpp"

namespace siaara::store {

enum class ScanKind { Instance = 0, Dicomdir = 1, NotDicom = 2, Error = 3 };

struct ScanItem {
    std::string path;   // UTF-8
    ScanKind kind = ScanKind::Instance;
    Status status;      // for Error: why the DICOM file could not be read
    IndexRecord record; // Instance / Dicomdir
};

struct ScanOptions {
    unsigned threads = 0;          // 0 = hardware concurrency (at most 16)
    bool recursive = true;
    bool follow_symlinks = false;
    bool allow_raw = true;         // accept files without preamble that look like a data set
    bool report_skipped = false;   // also call back for non-DICOM files
    bool use_dicomdir = false;     // when <root>/DICOMDIR exists, scan only the files it lists
    std::uint64_t max_files = 0;   // stop after this many files seen (0 = no limit)
};

struct ScanStats {
    std::uint64_t files = 0, instances = 0, dicomdirs = 0, skipped = 0, errors = 0;
    bool used_dicomdir = false;
};

// Return false from the callback to stop; the scan then fails with Errc::Cancelled.
using ScanCallback = std::function<bool(const ScanItem&)>;

Result<ScanStats> scan_folder(const std::string& utf8_root, const ScanCallback& cb, const ScanOptions& opt = {});

}  // namespace siaara::store
