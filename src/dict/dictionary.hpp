// SIAARA DICOM Toolkit - data dictionary (generated from DICOM PS3.6 / PS3.7 by tools/gen_dictionary.py)
#pragma once

#include <cstdint>
#include <string_view>

#include "core/types.hpp"

namespace siaara::dict {

struct Entry {
    Tag tag;            // tag value with repeating-group "xx" positions zeroed
    Tag mask;           // bits that must match (0xFFFFFFFF for plain tags; e.g. 0xFF00FFFF for 60xx groups)
    VR vr;              // first/only VR
    VR vr_alt;          // second VR for "US or SS" style entries, VR::None otherwise
    std::string_view keyword;
    std::string_view name;
    std::string_view vm;
    bool retired;
};

// Finds the dictionary entry for a public tag (including repeating groups and group-length
// elements). Returns nullptr for private tags and unknown tags.
const Entry* lookup(Tag tag) noexcept;

// Finds by keyword (e.g. "PatientName"). Returns nullptr if unknown.
const Entry* lookup_keyword(std::string_view keyword) noexcept;

// VR to use when reading implicit VR data. Returns VR::UN for unknown/private tags.
VR implicit_vr(Tag tag) noexcept;

// Number of entries compiled in (for diagnostics/tests).
std::size_t entry_count() noexcept;

struct UidEntry {
    std::string_view uid;
    std::string_view name;
    std::string_view keyword;
    std::string_view type;  // "SOP Class", "Transfer Syntax", ...
    bool retired;
};
const UidEntry* lookup_uid(std::string_view uid) noexcept;
std::size_t uid_count() noexcept;

}  // namespace siaara::dict
