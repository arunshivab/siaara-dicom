// SIAARA DICOM Toolkit - DICOM Part 10 file and raw data set reader (PS3.5 7, PS3.10 7)
#pragma once

#include <cstdint>
#include <string>

#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara {

struct ReadOptions {
    bool stop_before_pixel_data = false;   // header-only parse
    bool allow_raw_dataset = true;         // accept files without preamble/"DICM"
    bool lenient = true;                   // accept and flag common non-conformances
    std::uint32_t max_depth = 64;          // sequence nesting limit (PARSE-20)
    std::uint64_t max_elements = 10'000'000;
    std::uint64_t max_items_per_sequence = 1'000'000;
    std::uint64_t max_fragments = 1'000'000;
    std::uint64_t max_inflated_size = 4ull << 30;  // deflated TS output cap
    // Implicit VR (little endian) private data elements whose Private Creator and element the
    // private dictionary knows with a decided VR get that VR for interpretation (getters, JSON,
    // C API). The bytes are unchanged and Element::ReadInfo::private_vr records it, so saving
    // keeps the element as read. Undecided VRs (documents disagree) stay UN. Explicit VRs in the
    // stream always win. docs/PRIVATE_DICTIONARY.md.
    bool private_dictionary = true;
};

// Parses a complete Part 10 file held in `data` (the Bytes keeps the storage alive; values in
// the returned data set point into it without copying, except for big-endian and deflated input).
Result<DicomFile> read_part10(const Bytes& data, const ReadOptions& opt = {});

// Parses a data set with no preamble or file meta, using the given transfer syntax UID.
Result<Dataset> read_dataset(const Bytes& data, std::string_view transfer_syntax_uid,
                             const ReadOptions& opt = {});

// As read_dataset, keeping what the parse reports (R-116): the returned DicomFile has the data
// set, `transfer_syntax`, the lenient-mode warnings and the read-loss bits (read_loss,
// read_loss_offset, bytes_not_read); its meta group is empty.
Result<DicomFile> read_dataset_report(const Bytes& data, std::string_view transfer_syntax_uid,
                                      const ReadOptions& opt = {});

// Convenience: memory-maps and parses a file.
Result<DicomFile> read_file(const std::string& utf8_path, const ReadOptions& opt = {});

// Quick check: does the buffer look like a Part 10 file (preamble + "DICM")?
bool has_part10_header(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace siaara
