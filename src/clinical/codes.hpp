// SIAARA DICOM Toolkit - coded terminology used by the clinical writers.
// The tables are generated from DICOM PS3.16 (part16.xml) by tools/gen_codes.py; nothing is
// hand-typed. Context Groups: CID 7010 (Key Object Selection Document Title) and CID 7011
// (Rejected for Quality Reasons); DCM concept 113012 (Key Object Description) from table D-1.
#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "clinical/common.hpp"

namespace siaara::clinical {

struct CodeEntry {
    std::string_view scheme;
    std::string_view value;
    std::string_view meaning;
    Code code() const { return Code{std::string(value), std::string(scheme), std::string(meaning), {}}; }
};

namespace codes {
#include "clinical/codes_data.inc"
}  // namespace codes

// Rows of a generated Context Group (empty span for groups that are not generated).
std::span<const CodeEntry> context_group(int cid) noexcept;
// Version and UID of a generated Context Group ("" when not generated).
std::string_view context_group_version(int cid) noexcept;
std::string_view context_group_uid(int cid) noexcept;
// The row of `cid` with the given code value (and scheme, if not empty).
std::optional<Code> cid_code(int cid, std::string_view value, std::string_view scheme = {});

}  // namespace siaara::clinical
