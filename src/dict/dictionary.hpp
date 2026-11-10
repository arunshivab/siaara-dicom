// SIAARA DICOM Toolkit - data dictionary (generated from DICOM PS3.6 / PS3.7 by tools/gen_dictionary.py)
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/types.hpp"

namespace siaara {
class Dataset;
}

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

// ---------------------------------------------------------------- private dictionary
// Built by tools/gen_private_dict.py only from vendor DICOM Conformance Statements
// (data/private-tags/, docs/PRIVATE_DICTIONARY.md). A key is creator + group + the low byte of
// the element within the creator's block ((gggg,xxEE): EE).

enum class PrivateConfidence : std::uint8_t {
    Stated,   // a document states the creator for this element
    Assumed,  // creator assumed from a nearby declaration; used only when no stated entry exists
};
enum class PrivateMatch : std::uint8_t {
    Exact,  // the creator string matched as written (trailing spaces / NULs ignored)
    Fuzzy,  // matched a spelling variant ignoring ASCII case and whitespace
};

struct PrivateSource {
    std::string_view document;  // path as in data/private-tags (vendor folder / file name)
    std::uint32_t page = 0;     // physical PDF page
};

struct PrivateCandidate {
    VR vr = VR::None;
    PrivateSource source;
};

struct PrivateEntry {
    std::string_view creator;  // the dictionary's spelling (differs from the input for Fuzzy matches)
    std::uint16_t group = 0;
    std::uint8_t element = 0;  // low byte of the element
    std::string_view name;     // most common wording in the documents; may be empty
    std::string_view keyword;  // may be empty
    VR vr = VR::None;          // VR::None: undecided (documents disagree, or none states a VR)
    std::string_view vm;       // empty: undecided or not stated
    bool retired = false;      // a document marks it retired
    bool conflict = false;     // documents disagree on the VR (vr is VR::None; see candidates)
    bool vm_conflict = false;  // documents disagree on the VM (vm is empty)
    PrivateConfidence confidence = PrivateConfidence::Stated;
    PrivateMatch match = PrivateMatch::Exact;
    std::string_view vendor;
    PrivateSource source;               // primary source
    std::uint32_t source_count = 0;     // documents that define this key
    std::uint32_t candidate_count = 0;  // see private_candidate()
    std::uint32_t index = 0;            // internal entry index
};

// Looks up a private element. `creator` is the Private Creator value as stored (trailing spaces
// and NULs are padding and ignored). The exact creator string is tried first, then the spelling
// variants that are equal ignoring ASCII case and whitespace (match == Fuzzy).
std::optional<PrivateEntry> lookup_private(std::string_view creator, std::uint16_t group,
                                           std::uint8_t element_low_byte) noexcept;

// Candidate VRs with their sources. A VR conflict lists every (VR, document) pair; an entry with
// a decided VR has one candidate (the primary source); an entry whose documents state no VR has
// none. Returns false when `i` >= entry.candidate_count.
bool private_candidate(const PrivateEntry& entry, std::size_t i, PrivateCandidate& out) noexcept;

// The VR an implicit-VR reader may use for the element, or VR::UN when the creator/element is
// unknown or the VR is undecided (conflict / not stated).
VR implicit_private_vr(std::string_view creator, std::uint16_t group, std::uint8_t element_low_byte) noexcept;

// Looks up a private data element (gggg,xxEE) of `ds` through its Private Creator (gggg,00xx) in
// the same data set. nullopt for public tags, creator elements, a missing creator or an unknown key.
std::optional<PrivateEntry> lookup_private_element(const Dataset& ds, Tag tag) noexcept;

// Number of private entries and creators compiled in (for diagnostics/tests).
std::size_t private_entry_count() noexcept;
std::size_t private_creator_count() noexcept;

}  // namespace siaara::dict
