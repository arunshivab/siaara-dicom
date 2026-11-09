// SIAARA DICOM Toolkit - de-identification per DICOM PS3.15 Annex E (Attribute Confidentiality
// Profiles): the Basic Application Level Confidentiality Profile and its Options.
//
// The attribute actions come from Table E.1-1, the safe private attributes from Table E.3.10-1
// and the method codes from PS3.16 CID 7050; all three are generated from the standard's XML by
// tools/gen_deid.py (src/deid/deid_table.inc). Rules this implementation applies where the
// standard leaves a choice are documented next to Options below and in deid.cpp.
//
// No value of the data set is ever logged or put in a Status message or the Report.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::deid {

// Table E.1-1 action codes (PS3.15 E.1.1). None = empty cell (no requirement in that column).
enum class Action : std::uint8_t { None, D, Z, X, K, C, U, ZD, XZ, XD, XZD, XZU };

// Option columns of Table E.1-1, as bits of Options::options (same order as the table).
enum OptionBits : std::uint32_t {
    RetainSafePrivate = 1u << 0,             // E.3.10, CID 7050 113111
    RetainUids = 1u << 1,                    // E.3.9, 113110
    RetainDeviceIdentity = 1u << 2,          // E.3.8, 113109
    RetainInstitutionIdentity = 1u << 3,     // E.3.11, 113112
    RetainPatientCharacteristics = 1u << 4,  // E.3.7, 113108
    RetainLongitudinalFullDates = 1u << 5,   // E.3.6, 113106
    RetainLongitudinalModifiedDates = 1u << 6,  // E.3.6, 113107
    CleanDescriptors = 1u << 7,              // E.3.5, 113105
    CleanStructuredContent = 1u << 8,        // E.3.4, 113104
    CleanGraphics = 1u << 9,                 // E.3.3, 113103
};
constexpr std::size_t kOptionCount = 10;
constexpr std::uint32_t kAllOptionBits = (1u << kOptionCount) - 1u;

// Resolution of the combined actions (X/Z, X/D, X/Z/D, Z/D, X/Z/U*) when the attribute's Type in
// the IOD is not known (no attribute_type callback, or it returned 0).
enum class CombinedPolicy : std::uint8_t {
    // Default. Take the action that keeps the instance conformant whatever the Type is: D for
    // X/D, X/Z/D and Z/D; Z for X/Z; U (keep the sequence, replace the UIDs inside) for X/Z/U*.
    // Dummy and empty values carry no information from the original, so this is never less
    // private than removal, and it never breaks a Type 1 or Type 2 requirement.
    Conformant,
    // Always take X (remove). Smallest output; may drop Type 1/2 attributes.
    Remove,
};

constexpr std::uint32_t kAllFrames = 0xFFFFFFFFu;
// A rectangle of a frame to blank (Clean Pixel Data, E.3.1). x/y are column/row of the top-left.
struct PixelRegion {
    std::uint32_t frame = kAllFrames;
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Attribute Type lookup supplied by the caller: 1, 2 or 3 (1C/2C as 1/2) for `tag` at `path`
// (the sequence tags from the top level down to the item holding it; empty at the top level) in
// an instance of `sop_class_uid`; 0 when unknown.
using AttributeTypeFn = std::function<int(std::string_view sop_class_uid, std::span<const Tag> path, Tag tag)>;

struct Options {
    std::uint32_t options = 0;                // OptionBits
    // Secret key for UID replacement, the Patient ID pseudonym and the date offset. At least 16
    // bytes. The engine keeps only derived HMAC state, wiped before deidentify() returns.
    std::span<const std::uint8_t> key;
    CombinedPolicy combined = CombinedPolicy::Conformant;
    AttributeTypeFn attribute_type;           // optional
    // Clean Pixel Data (E.3.1): blank these rectangles of the top-level native Pixel Data.
    std::vector<PixelRegion> pixel_regions;
    // Set when the caller (or its operator) has established that pixel_regions cover all burned-in
    // identifying text. Only then is Burned In Annotation set to NO and code 113101 recorded.
    bool pixel_data_clean = false;
    // Words kept by the "C" (clean) action on text; everything else is dropped (ASCII,
    // case-insensitive, whole words). Empty: cleaned text becomes the placeholder "CLEANED".
    std::vector<std::string> safe_words;
    // Extensions recommended by E.1.1 notes (both on by default):
    bool remap_unlisted_uids = true;  // UI values not in Table E.1-1 and not well known (DICOM
                                      // root 1.2.840.10008 or the UID registry) are replaced too
    bool dates_by_vr = true;          // DA/DT/TM and PN attributes not in Table E.1-1 are handled
                                      // like listed dates (X/Z, K, C) and names (X/Z)
    std::uint32_t max_shift_days = 3652;  // Modified Dates: offset in 1..max_shift_days days back
};

// Report bits (also the C API's out_flags).
enum ReportBits : std::uint32_t {
    BurnedInAnnotationRemains = 1u << 0,      // pixel data present, (0028,0301) YES or absent, not cleaned
    RecognizableFeaturesRemain = 1u << 1,     // (0028,0302) YES
    NotFullyDeidentified = 1u << 2,           // either of the above
    PixelDataCleaned = 1u << 3,               // pixel_regions were applied
    UnparseableValuesRemoved = 1u << 4,       // dates/UIDs that could not be processed were emptied
};

struct Report {
    std::uint32_t flags = 0;
    std::size_t removed = 0;          // X (including private and group 0004 elements)
    std::size_t emptied = 0;          // Z
    std::size_t dummied = 0;          // D
    std::size_t kept = 0;             // K and unlisted attributes kept
    std::size_t cleaned = 0;          // C on text and binary values
    std::size_t uids_replaced = 0;    // UID values replaced (U, C on UI, D on UI)
    std::size_t dates_shifted = 0;    // date values shifted (Modified Dates)
    std::size_t private_kept = 0;     // safe private attributes kept
    std::size_t private_removed = 0;
    std::size_t items_visited = 0;    // sequence items walked
};

// De-identifies `file` in place: the data set, the file meta (rebuilt: only the version, the
// Media Storage SOP Class/Instance UIDs and the transfer syntax are kept), and the preamble (the
// writer always writes zeros). On failure `file` is left unchanged.
Status deidentify(DicomFile& file, const Options& opt, Report* report = nullptr);

// Blanks rectangles of native (uncompressed) pixel data in `ds` (black for the Photometric
// Interpretation). Fails with Unsupported for encapsulated pixel data.
Status blank_pixel_regions(Dataset& ds, std::span<const PixelRegion> regions);

// The replacement for a UID: 2.25.<decimal of the first 128 bits of HMAC-SHA-256(key, uid)>,
// with the UUID version 8 / RFC 9562 variant bits set. `uid` has its padding removed first.
std::string replacement_uid(std::span<const std::uint8_t> key, std::string_view uid);

// The Modified Dates offset (whole days, negative) for a patient.
std::int32_t date_offset_days(std::span<const std::uint8_t> key, std::string_view patient_id,
                              std::uint32_t max_shift_days = 3652);

// Shifts one DA / DT value (no backslashes) by `days`. Returns false for values that are not
// valid dates. TM values are not changed by a whole-day shift.
bool shift_date(std::string_view value, VR vr, std::int32_t days, std::string& out);

// ---- generated tables (tools/gen_deid.py)
struct ProfileRow {
    Tag tag;
    Tag mask;
    bool retired;
    bool in_standard_iod;
    Action basic;
    Action option[kOptionCount];
    std::string_view name;
};
struct SafePrivateRow {
    std::uint16_t group;
    std::string_view creator;
    std::uint8_t element;  // low byte of the element number
    bool is_sequence;
};
struct MethodCode {
    std::string_view scheme;
    std::string_view value;
    std::string_view meaning;
};
std::span<const ProfileRow> profile_rows() noexcept;
const ProfileRow& private_attributes_row() noexcept;
std::span<const SafePrivateRow> safe_private_rows() noexcept;
std::span<const MethodCode> method_codes() noexcept;
// Row for a public tag (exact match first, then repeating-group rows), or nullptr.
const ProfileRow* find_row(Tag tag) noexcept;
const SafePrivateRow* find_safe_private(std::uint16_t group, std::string_view creator, std::uint8_t element) noexcept;
// The action for a row under the given option bits (options override the Basic Profile; when
// several enabled options apply, K wins over C because E.1.1 says retained attributes shall be
// retained).
Action effective_action(const ProfileRow& row, std::uint32_t options) noexcept;

}  // namespace siaara::deid
