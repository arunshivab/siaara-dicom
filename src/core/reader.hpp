// SIAARA DICOM Toolkit - DICOM Part 10 file and raw data set reader (PS3.5 7, PS3.10 7)
#pragma once

#include <cstdint>
#include <string>

#include "core/dataset.hpp"
#include "core/io.hpp"
#include "core/status.hpp"

namespace siaara {

// Default of ReadOptions::recover_overlong_lengths, and so of the C API (siaara_read_options with
// recover_overlong_lengths = SIAARA_RECOVER_DEFAULT) and of C# (DicomReadOptions with
// RecoverOverlongLengths = null). THE ONE PLACE TO FLIP IT (security review 2026-11-11,
// Decision 2: on in lenient mode, or opt-in). true: lenient reads apply the recovery; false: only
// callers that ask for it do. Strict reads never apply it.
inline constexpr bool kRecoverOverlongLengthsDefault = true;

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
    // Lenient mode only: a text value in implicit VR little endian whose stated length runs 1..7
    // bytes into the next element's header (GE Genesis files) is read that much shorter when a
    // run of three known element headers starts there and none starts at the stated end (no run
    // of three, and not even one fitting header of a standard attribute in any tag order). Each
    // recovery is a warning (ELEMENT_LENGTH_RECOVERED) and sets DicomFile::kReadRecovered, so a
    // save is refused unless incomplete saves are allowed. Never applied to the file meta group,
    // to explicit VR or big endian data, to binary VRs or to group 7FE0. Readers of data that
    // did not come from a local file (network receivers) should pass false.
    bool recover_overlong_lengths = kRecoverOverlongLengthsDefault;
    // read_file() only (R-160): how the file is brought into memory - io::OpenMode::Auto (default:
    // read into owned memory up to open_read_threshold bytes, memory-mapped above it), Map
    // (always the zero-copy mapping) or Read (always owned memory, at most open_read_max_bytes;
    // a larger file is Errc::LimitExceeded). A mapped file that another process shortens can end
    // the process when the missing pages are touched; the size is re-checked after mapping and
    // before pixel data is decoded (Errc::Truncated), which narrows that window but cannot close
    // it. Values, zero-copy accessors, the read report and keep-as-read saves are the same in
    // all three modes.
    io::OpenMode open_mode = io::kDefaultOpenMode;
    std::uint64_t open_read_threshold = io::kDefaultOpenReadThreshold;
    std::uint64_t open_read_max_bytes = io::kDefaultOpenReadMaxBytes;
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
