// SIAARA DICOM Toolkit - repair of DICOM files (owner rules):
//  - the input file is never changed: it is mapped read-only, and its SHA-256 is compared before
//    and after every run (FileReport::input_unchanged);
//  - a repair always writes a NEW file; an output path that is (or resolves to) an input is refused,
//    and an existing output is replaced only with RepairOptions::overwrite;
//  - the default is report only (dry run): nothing is written unless `write` is set, and only the
//    categories in `fix` are applied;
//  - every change is logged (category, action, location, old -> new). Values that may hold patient
//    data are withheld from the log unless `detailed_report` is set; such a report is marked as
//    containing PHI;
//  - everything the repair does not need to change is written as read (keep-as-read writer).
//
// Categories (repair/report.hpp):
//  structural  file meta rebuilt from the data set (version, Media Storage SOP Class / Instance UID,
//              Transfer Syntax, Implementation UID / name), group lengths dropped (or recomputed),
//              odd lengths padded, Basic / Extended Offset Table rebuilt, misplaced group 0000/0002
//              elements removed. Element order, duplicates (the kept copy is logged), unterminated
//              sequences / items, stray delimiters and VR-less elements are always written correctly
//              because the output is a valid encoding; those changes are logged under structural.
//  values      lossless VR re-encoding (UN -> dictionary VR, string <-> string, US <-> SS ... when every
//              value fits), unambiguous normalisation of DA/TM/DS/IS/UI/AS/CS values (values.hpp).
//  uids        invalid UIDs (not class / transfer syntax UIDs) replaced by 2.25 UIDs; the same old
//              value gets the same new value in every file and every reference of the run.
//  charset     Specific Character Set set to the detected one (valid UTF-8 -> ISO_IR 192) or to
//              RepairOptions::charset after checking that all text decodes in it.
//  attributes  missing Type 2 (and 2C whose condition holds) attributes added empty; missing SOP
//              Instance / Study / Series Instance UIDs generated. No clinical value is ever invented.
//  pixels      header-to-pixel consistency fixes (repair/pixels.hpp).
//  rescue      complete frames of truncated / damaged pixel data kept (encapsulated frames are kept
//              only when they decode), Number of Frames and per-frame functional groups trimmed.
//  decompress  pixel data written native (Explicit VR Little Endian) so the file opens everywhere.
//  study       cross-file: Patient / Study / Series attributes harmonised to the majority, duplicate
//              SOP Instance UIDs renumbered, Instance Numbers renumbered by position, missing Study /
//              Series UIDs filled from the other files.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "repair/report.hpp"
#include "repair/scan.hpp"
#include "repair/study.hpp"
#include "repair/validate.hpp"

namespace siaara::repair {

struct RepairOptions {
    std::uint32_t fix = 0;                  // Category bits; 0 = report only
    bool write = false;                     // false: dry run (changes planned and reported, nothing written)
    bool detailed_report = false;           // include old/new values and paths (PHI)
    std::string charset;                    // charset category: value to set when detection is ambiguous
    bool recompute_group_lengths = false;   // structural: recompute instead of dropping them
    DuplicatePolicy duplicates = DuplicatePolicy::KeepFirst;
    StudyOptions study;
    std::string uid_seed;                   // seed for generated UIDs; empty = random for this run
    bool overwrite = false;                 // replace an existing output file (never an input)
    ValidateOptions validate;
    ScanOptions scan;
};

// Validates and (optionally) repairs `inputs`. Outputs go to `out_dir` named after the input file
// name (a numeric suffix keeps names unique). Fails only for bad arguments; per-file failures are
// reported in FileReport::error.
Result<Report> repair_files(std::span<const std::string> inputs, const std::string& out_dir, const RepairOptions& opt);

// One file with an explicit output path (may be empty for a dry run).
Result<Report> repair_file(const std::string& input, const std::string& output, const RepairOptions& opt);

// In memory: `out` receives the repaired Part 10 stream when opt.write is set.
Report repair_buffer(const Bytes& input, std::vector<std::uint8_t>* out, const RepairOptions& opt);

std::string sha256_hex(const std::uint8_t* data, std::size_t size);

}  // namespace siaara::repair
