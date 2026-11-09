// SIAARA DICOM Toolkit - tolerant structural scanner for validation and repair.
// Written from DICOM PS3.5 section 7 (data set encoding, element order, group lengths, sequences
// and items), 8.2 / A.4 (encapsulated pixel data fragments, Basic Offset Table) and PS3.10 7.1
// (preamble, "DICM", File Meta Information).
//
// Unlike core/reader, the scanner never stops at the first problem it can step over: it records
// every structural non-conformance as a Finding (with its byte offset) and builds the best data
// set it can. The input bytes are never written; values are views into them (big-endian values
// and deflated bodies are converted into private copies, as in the reader).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "repair/report.hpp"

namespace siaara::repair {

enum class DuplicatePolicy : std::uint8_t {
    KeepFirst,  // keep the first copy unless it is empty and a later copy is not (default)
    KeepLast,
};

struct ScanOptions {
    std::uint32_t max_depth = 64;
    std::uint64_t max_elements = 10'000'000;
    std::uint64_t max_items_per_sequence = 1'000'000;
    std::uint64_t max_fragments = 1'000'000;
    std::uint64_t max_inflated_size = 4ull << 30;
    std::size_t max_findings = 5000;
    DuplicatePolicy duplicates = DuplicatePolicy::KeepFirst;
};

struct PixelScan {
    bool present = false;
    bool encapsulated = false;
    Tag tag = 0;
    std::uint64_t offset = 0;                // element header offset
    // native
    bool value_truncated = false;
    std::uint64_t declared_length = 0;       // length field (native)
    // encapsulated
    bool sequence_unterminated = false;      // no Sequence Delimitation Item before the end
    bool last_fragment_truncated = false;
    std::uint64_t truncated_declared = 0;    // length field of the truncated fragment
    Bytes partial_fragment;                  // bytes of the truncated fragment that exist
    // Offset of each fragment item header relative to the first byte of the first data
    // fragment's item header (the origin Basic Offset Table values use), for data fragments only.
    std::vector<std::uint64_t> item_offsets;
};

struct GroupLengthInfo {
    Path path;
    std::uint16_t group = 0;
    std::uint32_t declared = 0;
    std::uint64_t actual = 0;
    std::uint64_t offset = 0;
};

struct ScanResult {
    DicomFile file;
    Bytes source;                       // whole input
    std::vector<Finding> findings;      // structural findings, in input order
    PixelScan pixel;
    bool had_dicm = false;
    bool meta_present = false;
    bool raw_dataset = false;           // no preamble and no "DICM"
    bool preamble_nonzero = false;
    bool encoding_mismatch = false;     // the body's encoding differs from the transfer syntax
    bool body_explicit_vr = true;       // as actually parsed
    bool body_big_endian = false;
    std::string declared_transfer_syntax;  // from the meta (padding removed), may be empty
    std::vector<GroupLengthInfo> group_lengths;  // every (gggg,0000) seen, with the actual length
    std::uint32_t duplicates = 0, out_of_order = 0, odd_lengths = 0, unterminated = 0;
    bool fatal = false;                 // nothing could be parsed (not DICOM)
};

// Scans a whole file image. Fails only for empty input, inputs that are not DICOM at all, or
// limit violations; everything else becomes a Finding.
Result<ScanResult> scan(const Bytes& data, const ScanOptions& opt = {});
Result<ScanResult> scan_file(const std::string& utf8_path, const ScanOptions& opt = {});

// Parses `bytes` as a sequence's items encoded in Implicit VR Little Endian (used for UN
// elements whose dictionary VR is SQ, PS3.5 6.2.2). Fails when the bytes are not items.
Status parse_implicit_items(const Bytes& bytes, std::vector<Dataset>& items, const ScanOptions& opt = {});

}  // namespace siaara::repair
