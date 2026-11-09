// SIAARA DICOM Toolkit - data set and Part 10 file writer (PS3.5 7, PS3.10 7)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara {

constexpr std::string_view kImplementationClassUid = "2.25.208936390756097257212108686309372694101";
constexpr std::string_view kImplementationVersionName = "SIAARA_010";

struct WriteOptions {
    bool sequences_undefined_length = false;  // write SQ/items with undefined length + delimiters
    bool keep_group_lengths = false;          // keep (gggg,0000) elements other than group 0002
    int deflate_level = 6;                    // for the Deflated transfer syntax
};

// Encodes a data set body in the given transfer syntax (native syntaxes only, plus deflate).
// Encapsulated pixel data is written as-is; transcoding pixel data is the codec layer's job.
Status write_dataset(const Dataset& ds, std::string_view transfer_syntax_uid,
                     std::vector<std::uint8_t>& out, const WriteOptions& opt = {});

// Writes a full Part 10 stream: 128-byte preamble, "DICM", file meta (built from `file.meta`
// plus required elements filled in from the data set), then the body.
Status write_part10(const DicomFile& file, std::vector<std::uint8_t>& out,
                    const WriteOptions& opt = {});

Status write_file(const DicomFile& file, const std::string& utf8_path, const WriteOptions& opt = {});

}  // namespace siaara
