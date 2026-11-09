// SIAARA DICOM Toolkit - repair and validation findings, change log and report rendering.
//
// Privacy rule: findings and the default change log never contain values from the data set
// unless the value is known to carry no patient information (see value_is_loggable). A detailed
// report (RepairOptions::detailed_report) includes old and new values of every change and is
// marked as containing PHI ("contains_phi": true in JSON, a banner in text).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/types.hpp"

namespace siaara::repair {

enum class Severity : std::uint8_t { Info = 0, Warning = 1, Error = 2 };
std::string_view severity_name(Severity s) noexcept;

// Repair categories (bit mask). 0 = report only.
enum Category : std::uint32_t {
    kStructural = 1u << 0,   // file meta, order, duplicates, padding, group lengths, offset tables
    kValues = 1u << 1,       // VR, length, DA/TM/DT/DS/IS/AS/UI formats
    kUids = 1u << 2,         // replace invalid UIDs with 2.25 UIDs (consistently)
    kCharset = 1u << 3,      // set Specific Character Set (detected, or RepairOptions::charset)
    kAttributes = 1u << 4,   // add missing Type 2 attributes empty, generate required UIDs
    kPixels = 1u << 5,       // header-to-pixel consistency
    kRescue = 1u << 6,       // salvage complete frames of damaged / truncated pixel data
    kDecompress = 1u << 7,   // write pixel data native (Explicit VR Little Endian)
    kStudy = 1u << 8,        // cross-file Patient/Study/Series consistency, UIDs, Instance Numbers
};
constexpr std::uint32_t kAllCategories = (1u << 9) - 1u;
std::string_view category_name(std::uint32_t single_bit) noexcept;
// Parses "structural,values,..." (also "all"). Returns false on an unknown name.
bool parse_categories(std::string_view list, std::uint32_t& out);

// A location: the sequence tags and item indices from the top level down, then the tag.
struct PathStep {
    Tag tag = 0;
    std::int32_t item = -1;  // item index inside `tag` when the location is below it
};
using Path = std::vector<PathStep>;
std::string path_string(const Path& p);          // "(0008,1140)[0](0008,1155)"
std::string tag_string(Tag t);                    // "(0008,0016)"

struct Finding {
    Severity severity = Severity::Error;
    std::string code;        // stable identifier, e.g. "missing-type1"
    std::uint32_t category = 0;  // the repair category that addresses it (0 = none)
    Path path;               // empty for file-level findings
    std::string message;     // never contains data set values
    std::uint64_t offset = 0;  // byte offset in the input, when known
    bool has_offset = false;
    std::string module;      // IOD module, for requirement findings
};

struct Change {
    std::uint32_t category = 0;
    std::string action;      // "set", "add", "remove", "re-encode", ...
    Path path;
    std::string detail;      // never contains data set values
    std::string old_value;   // filled only when loggable or for detailed reports
    std::string new_value;
    bool values_withheld = false;  // old/new not shown (may contain patient data)
};

struct FileReport {
    std::size_t input_index = 0;
    std::string input_path;   // only rendered in detailed reports
    std::string output_path;  // only rendered in detailed reports
    bool written = false;
    bool dry_run = true;
    bool input_unchanged = true;  // SHA-256 of the input before == after
    std::string input_sha256;
    std::string output_sha256;
    std::vector<Finding> findings;        // validation of the input
    std::vector<Change> changes;          // applied (or, in a dry run, planned)
    bool after_computed = false;          // findings_after holds the validation of the (planned) output
    std::vector<Finding> findings_after;  // validation of the written (or, in a dry run, planned) output
    std::vector<std::uint32_t> frames_recovered;  // pixel rescue: 0-based frame indices kept
    std::vector<std::uint32_t> frames_lost;
    std::uint32_t frames_declared = 0;
    bool rescue_attempted = false;
    std::string error;        // failure message (no values)
};

struct Report {
    bool detailed = false;    // contains PHI
    std::vector<FileReport> files;
    std::vector<Finding> study_findings;  // cross-file findings
};

std::size_t count(const std::vector<Finding>& f, Severity s) noexcept;

// True when the value of `tag` (VR `vr`) inside a data set reached through `path` carries no
// patient information: public, not listed in PS3.15 Table E.1-1 (nor any enclosing sequence),
// and not a PN, DA, DT, TM, free text (LT, ST, UT, UC, UR) or binary/unknown VR.
bool value_is_loggable(const Path& path, Tag tag, VR vr) noexcept;
// Printable form of a value for a change log (escaped, at most 64 characters shown).
std::string printable_value(const Element& e);
std::string printable_text(std::string_view s);

std::string to_json(const Report& r);
std::string to_text(const Report& r, Severity min_severity = Severity::Info);
std::string findings_json(const std::vector<Finding>& f);
std::string json_escape(std::string_view s);

}  // namespace siaara::repair
