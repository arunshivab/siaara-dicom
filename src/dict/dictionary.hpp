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

// "US or SS" elements whose VR follows Pixel Representation (0028,0103): US when it is 0, SS
// when it is 1 (PS3.5 / PS3.6; the same set pydicom corrects, plus the retired Perimeter Value
// and Gray / Large Palette LUT descriptors). Gray Lookup Table Data (0028,1200) is not included:
// its values are LUT output, not pixel values. Testing-chat F8.
bool us_or_ss_by_pixel_representation(Tag tag) noexcept;
// LUT and palette descriptors (0028,1100-1103, 0028,1111-1113, 0028,3002): only the second value
// (first stored value mapped) is US or SS; the number of entries and the bits per entry are
// always unsigned (PS3.3 C.11.1.1.1, C.7.6.3.1.5).
bool is_lut_descriptor(Tag tag) noexcept;

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
