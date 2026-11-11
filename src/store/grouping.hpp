// SIAARA DICOM Toolkit - grouping files that arrived together (a ZIP from a mail, with or without a
// DICOMDIR) into patient / study / series, each series ordered for scrolling, and what a DICOMDIR
// lists set against what arrived. All from bytes in memory; nothing is written to disk and no
// file's bytes are kept.
//
// The input is UNTRUSTED (internet mail). Every file is read header-only with the attachment
// preview's limits (core/untrusted.hpp); the grouping as a whole is held to budgets (files,
// bytes added, bytes inflated, text kept). A file past a limit is recorded as skipped with the
// reason; nothing throws for bad bytes.
//
// What is returned is what the files CLAIM: the grouping is not proof that the files belong
// together or to the patient they name. Every text value is returned exactly as stored (decoded
// from its character set, padding removed) and, next to it, a display-safe copy
// (core/display_safe.hpp). A value longer than its cap is not kept and is reported "too long";
// it is never cut and passed on as if whole. A UID takes part in grouping only when it is a valid
// UID (UidValidation: PS3.5 9.1 by default). A Part 10 file that cannot be read (or holds nothing
// after its header) is skipped; ungrouped is only for readable DICOM without a valid Study or
// Series UID. Names with "..", absolute names and names with a drive letter are reported
// (UnsafeName) and never matched to DICOMDIR File IDs.
//
// Patients are told apart by Patient ID plus Issuer of Patient ID (both as stored, compared byte
// for byte); files without a Patient ID form one "no Patient ID" patient per study, never merged
// across studies. Patients, studies and series come in the order their first file was added.
// A Study Instance UID seen under two Patient IDs gives two studies (one under each patient) and
// an inconsistency, never one merged study; a series whose instances disagree on Modality or on
// Rows x Columns is reported as well.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/status.hpp"
#include "core/untrusted.hpp"

namespace siaara::store {

enum class GroupOrder : std::int32_t {
    Position = 0,        // along the slice normal, then Slice Location, then Instance Number, then as added
    InstanceNumber = 1,  // by Instance Number; files without one after, as added
    AsAdded = 2,
};
enum class GroupDuplicates : std::int32_t {
    KeepBoth = 0,   // a second file with the same SOP Instance UID is kept and marked duplicate
    KeepFirst = 1,  // a second file with the same SOP Instance UID is not kept (reported)
};

inline constexpr std::uint64_t kGroupingDefaultMaxFiles = 10'000;
inline constexpr std::uint64_t kGroupingDefaultMaxFileBytes = std::uint64_t{64} << 20;
inline constexpr std::uint64_t kGroupingDefaultMaxTotalBytes = std::uint64_t{2} << 30;
inline constexpr std::uint64_t kGroupingDefaultMaxTotalInflated = std::uint64_t{2} << 30;
inline constexpr std::uint64_t kGroupingDefaultMaxResultBytes = std::uint64_t{64} << 20;
inline constexpr std::uint32_t kGroupingDefaultMaxUidChars = 64;
inline constexpr std::uint32_t kGroupingDefaultMaxTextBytes = 1'024;
inline constexpr GroupOrder kGroupingDefaultOrder = GroupOrder::Position;
inline constexpr GroupDuplicates kGroupingDefaultDuplicates = GroupDuplicates::KeepBoth;
inline constexpr bool kGroupingDefaultIncludePatientFields = false;
inline constexpr bool kGroupingDefaultMatchCaseInsensitive = true;
inline constexpr bool kGroupingDefaultMatchByTrailingComponents = true;
// Largest difference of any of the six direction cosines of Image Orientation (Patient) from the
// series' reference orientation for which a file's position along the normal is still used.
inline constexpr double kGroupingDefaultOrientationTolerance = 1e-4;
// A cross product of the row and column cosines shorter than this gives no normal (the
// orientation is degenerate: parallel or zero vectors). Not an option: a numerical guard.
inline constexpr double kGroupingMinNormalLength = 1e-6;

// When some instances of a series (order Position) have no usable position:
enum class PositionFallback : std::int32_t {
    PerSeries = 0,    // the whole series is ordered by one fallback rule: Slice Location when every
                      // instance has one, else Instance Number when every instance has one, else as added
    PerInstance = 1,  // the positioned ones by position, the rest after them by Slice Location, then
                      // Instance Number, then as added
};
// Equal sort keys (a time series, diffusion, a test bolus at one position). Always followed by the
// order the files were added in (the sorts are stable).
enum class TieBreak : std::int32_t {
    InstanceNumber = 0,   // files with an Instance Number first, by it
    AsAdded = 1,
    AcquisitionTime = 2,  // Acquisition Time (0008,0032), else Content Time (0008,0033); files with one first
};
// Which UIDs group (Study, Series) and count for duplicates (SOP Instance).
enum class UidValidation : std::int32_t {
    Strict = 0,   // PS3.5 9.1: 1 to 64 characters, components of digits separated by single dots, no
                  // empty component, no leading zero unless the component is "0"
    Lenient = 1,  // 1 to 64 characters, digits and dots only
};
// The order of the series inside each study.
enum class GroupSeriesOrder : std::int32_t {
    AsAdded = 0,       // the order their first file was added in
    SeriesNumber = 1,  // by Series Number ascending (of each series' first file); series without one, or
                       // with one that is not a valid IS, after them as added; ties as added
};
inline constexpr GroupSeriesOrder kGroupingDefaultSeriesOrder = GroupSeriesOrder::AsAdded;
inline constexpr PositionFallback kGroupingDefaultPositionFallback = PositionFallback::PerSeries;
inline constexpr TieBreak kGroupingDefaultTieBreak = TieBreak::InstanceNumber;
inline constexpr UidValidation kGroupingDefaultUidValidation = UidValidation::Strict;
inline constexpr std::uint32_t kGroupingDefaultMaxDicomdirs = 4;
inline constexpr std::uint32_t kGroupingMaxDicomdirsLimit = 32;  // the most max_dicomdirs may be (dicomdir_mask bits)

struct GroupingOptions {
    std::uint64_t max_files = kGroupingDefaultMaxFiles;              // files read (add_file calls past it are skipped)
    std::uint64_t max_file_bytes = kGroupingDefaultMaxFileBytes;     // one file's size
    std::uint64_t max_total_bytes = kGroupingDefaultMaxTotalBytes;   // sizes of all files read (and DICOMDIRs)
    std::uint64_t max_total_inflated = kGroupingDefaultMaxTotalInflated;  // inflated sizes of all deflated files
    std::uint64_t max_result_bytes = kGroupingDefaultMaxResultBytes;      // all text kept: stored + display-safe
    std::uint32_t max_uid_chars = kGroupingDefaultMaxUidChars;       // longest UID kept
    std::uint32_t max_text_bytes = kGroupingDefaultMaxTextBytes;     // longest other text kept (names too)
    std::uint32_t max_depth = kUntrustedMaxDepth;                    // the preview's read limits, per file
    std::uint64_t max_elements = kUntrustedMaxElements;
    std::uint64_t max_inflated_size = kUntrustedMaxInflatedSize;
    std::uint64_t max_value_bytes = kUntrustedMaxValueBytes;         // a longer stored value is not decoded
    GroupOrder order = kGroupingDefaultOrder;
    bool include_patient_fields = kGroupingDefaultIncludePatientFields;
    GroupDuplicates duplicates = kGroupingDefaultDuplicates;
    bool match_case_insensitive = kGroupingDefaultMatchCaseInsensitive;
    bool match_by_trailing_components = kGroupingDefaultMatchByTrailingComponents;
    double orientation_tolerance = kGroupingDefaultOrientationTolerance;
    PositionFallback position_fallback = kGroupingDefaultPositionFallback;
    TieBreak tie_break = kGroupingDefaultTieBreak;
    UidValidation uid_validation = kGroupingDefaultUidValidation;
    std::uint32_t max_dicomdirs = kGroupingDefaultMaxDicomdirs;  // more add_dicomdir calls are refused
    GroupSeriesOrder series_order = kGroupingDefaultSeriesOrder;
};

enum class TextStatus : std::int32_t {
    Absent = 0,        // the file does not have the attribute (or it is empty)
    Ok = 1,            // stored and display are set
    TooLong = 2,       // longer than its cap (or than max_value_bytes stored): not kept
    NotAUid = 3,       // a UID field whose value is not a valid UID: kept, not used for grouping
    NotRequested = 4,  // Patient's Name / Patient ID without include_patient_fields
    NotKept = 5,       // a file name not kept because the result budget was reached
};

struct GroupText {
    TextStatus status = TextStatus::Absent;
    std::string stored;   // exactly as stored (as given, for names)
    std::string display;  // display-safe copy
};

enum class TextField : std::int32_t {
    Name = 0, SopClassUid, SopInstanceUid, StudyUid, SeriesUid, Modality, StudyDate, SeriesDescription,
    PatientName, PatientId, Subject,  // Subject: issues only (the UID or File ID the issue is about)
    IssuerOfPatientId,
    Count
};
inline constexpr std::size_t kTextFieldCount = static_cast<std::size_t>(TextField::Count);

// Instance flags.
inline constexpr std::uint32_t kGiSeriesNumber = 1u << 0;
inline constexpr std::uint32_t kGiInstanceNumber = 1u << 1;
inline constexpr std::uint32_t kGiPosition = 1u << 2;        // Image Position (Patient): 3 numbers
inline constexpr std::uint32_t kGiOrientation = 1u << 3;     // Image Orientation (Patient): 6 numbers
inline constexpr std::uint32_t kGiSliceLocation = 1u << 4;
inline constexpr std::uint32_t kGiFrames = 1u << 5;          // Number of Frames
inline constexpr std::uint32_t kGiRows = 1u << 6;
inline constexpr std::uint32_t kGiColumns = 1u << 7;
inline constexpr std::uint32_t kGiPositionUsable = 1u << 8;  // `position` is set and placed the instance
inline constexpr std::uint32_t kGiDuplicate = 1u << 9;       // a SOP Instance UID an earlier file has
inline constexpr std::uint32_t kGiUngrouped = 1u << 10;      // no valid Study or Series UID
inline constexpr std::uint32_t kGiInDicomdir = 1u << 11;     // its name matched a DICOMDIR File ID
inline constexpr std::uint32_t kGiAcquisitionTime = 1u << 12; // acquisition_time is set
inline constexpr std::uint32_t kGiUnsafeName = 1u << 13;      // its name has "..", is absolute or has a drive letter

enum class SortKey : std::int32_t { Position = 0, SliceLocation = 1, InstanceNumber = 2, Added = 3 };
// The rule a series was ordered by: one key for every instance, or Mixed (tiers, PerInstance).
enum class SeriesOrder : std::int32_t { Position = 0, SliceLocation = 1, InstanceNumber = 2, Added = 3, Mixed = 4 };

struct GroupedInstance {
    std::uint32_t file = 0;           // the add_file call (0-based)
    std::uint32_t flags = 0;
    std::int32_t series = -1;         // result: series index, -1 when ungrouped
    SortKey sort_key = SortKey::Added;
    std::uint32_t read_loss = 0;      // DicomFile::ReadLoss bits (truncated, bytes skipped ...)
    std::int64_t series_number = 0, instance_number = 0, number_of_frames = 0;
    std::uint32_t rows = 0, columns = 0;
    std::array<double, 3> image_position{};
    std::array<double, 6> image_orientation{};
    double slice_location = 0;
    double position = 0;              // along the series normal (kGiPositionUsable)
    double acquisition_time = 0;      // seconds since midnight (kGiAcquisitionTime)
    bool series_number_is = false;    // Series Number is a valid IS (PS3.5 6.2: [+-]digits, 1..12 characters, 32-bit)
    std::int32_t duplicate_of = -1;   // the first file with this SOP Instance UID (Grouping::duplicate_of)
    std::array<GroupText, kTextFieldCount> text;  // Name and Subject are not used here (see GroupFile)
};

enum class IssueKind : std::int32_t {
    Skipped = 0,               // a file not grouped: reason, limit, status
    Ungrouped = 1,             // instance without a valid Study or Series UID (reason: which)
    Duplicate = 2,             // instance (or file, KeepFirst) with an earlier file's SOP Instance UID
    StudyPatientConflict = 3,  // a Study UID under more than one Patient ID: one issue per study
    SeriesModalityConflict = 4,
    SeriesSizeConflict = 5,    // Rows x Columns
    DicomdirSkipped = 6,       // a DICOMDIR not read: reason, limit, status
    ListedMissing = 7,         // a DICOMDIR File ID no file name matches (or that is not well formed)
    PresentUnlisted = 8,       // an instance whose name matches no DICOMDIR File ID
    AmbiguousMatch = 9,        // a File ID two or more names match: one issue per name
    UnsafeName = 10,           // a name with a ".." component, absolute, or with a drive letter: never matched
};
enum class SkipReason : std::int32_t { None = 0, NotDicom = 1, Unreadable = 2, FileLimit = 3, TotalBudget = 4 };
enum class Limit : std::int32_t {
    None = 0, MaxFiles = 1, FileBytes = 2, TotalBytes = 3, TotalInflated = 4, ResultBytes = 5,
    ReadLimits = 6,     // nesting depth or elements of one file
    InflatedSize = 7,   // max_inflated_size of one file
    DicomdirRecords = 8,
    MaxDicomdirs = 9,
};
enum class UngroupedReason : std::int32_t { None = 0, StudyUid = 1, SeriesUid = 2, Both = 3 };

struct GroupingIssue {
    IssueKind kind = IssueKind::Skipped;
    std::int32_t reason = 0;           // SkipReason (Skipped, DicomdirSkipped), UngroupedReason (Ungrouped)
    Limit limit = Limit::None;
    Errc status = Errc::Ok;            // the read's status (Skipped, DicomdirSkipped), file_status (ListedMissing)
    std::int32_t file = -1, instance = -1, other_instance = -1, study = -1, series = -1, dicomdir = -1, record = -1;
    std::uint32_t dicomdir_mask = 0;     // the DICOMDIRs that list the File ID (bit i: add_dicomdir call i)
    const GroupText* name = nullptr;     // the file's name, or null
    const GroupText* subject = nullptr;  // the UID or File ID it is about, or null
};

struct GroupedPatient {
    std::uint32_t first_study = 0, study_count = 0, instance_count = 0;
    bool no_patient_id = false;            // the files of one study without a Patient ID
    std::uint32_t first_instance_ref = 0;  // an instance of the patient (its first file): text comes from it
};
struct GroupedStudy {
    std::uint32_t patient = 0, first_series = 0, series_count = 0, instance_count = 0;
    std::uint32_t first_instance_ref = 0;
};
struct GroupedSeries {
    std::uint32_t study = 0, first_instance = 0, instance_count = 0;
    std::uint32_t first_instance_ref = 0;  // the series' first file (in add order)
    bool has_normal = false;
    std::array<double, 3> normal{};
    SeriesOrder order = SeriesOrder::Added;
};

struct GroupingSummary {
    std::uint64_t files = 0, files_read = 0, instances = 0, patients = 0, studies = 0, series = 0, issues = 0;
    std::uint64_t skipped = 0, ungrouped = 0, duplicates = 0, inconsistencies = 0, dicomdirs = 0;
    std::uint64_t listed_missing = 0, present_unlisted = 0, ambiguous = 0, unsafe_names = 0;
    std::uint64_t total_bytes = 0, total_inflated = 0, result_bytes = 0;
    Limit budget_reached = Limit::None;  // the first total budget reached, if any
};

class Grouping {
public:
    explicit Grouping(const GroupingOptions& opt = {});

    // One file (a ZIP member): `name` as the caller has it (never changed), `data` its bytes, only
    // read during the call. Always recorded: as an instance, or as skipped with the reason.
    void add_file(std::string_view name, const std::uint8_t* data, std::size_t size);
    // A DICOMDIR (read with read_dicomdir_memory). Always recorded; one that cannot be read is an
    // issue (DicomdirSkipped).
    void add_dicomdir(const std::uint8_t* data, std::size_t size);

    // Builds the result: groups, orders, matches, lists the issues. The vectors below and every
    // pointer into them stay valid until the next add_file / add_dicomdir / build.
    void build();
    bool built() const noexcept { return built_; }

    const std::vector<GroupedPatient>& patients() const noexcept { return patients_; }
    const std::vector<GroupedStudy>& studies() const noexcept { return studies_; }
    const std::vector<GroupedSeries>& series() const noexcept { return series_; }
    // Result order: grouped instances patient by patient, study by study, series by series, each
    // series in its order; then the ungrouped ones in the order they were added.
    std::size_t instance_count() const noexcept { return order_.size(); }
    const GroupedInstance& instance(std::size_t i) const noexcept { return instances_[order_[i]]; }
    // By internal index (GroupedPatient / GroupedStudy / GroupedSeries::first_instance_ref).
    const GroupedInstance& internal_instance(std::size_t i) const noexcept { return instances_[i]; }
    const std::vector<GroupingIssue>& issues() const noexcept { return issues_; }
    const GroupingSummary& summary() const noexcept { return summary_; }
    const GroupingOptions& options() const noexcept { return opt_; }
    // Result index of the instance a duplicate (kGiDuplicate) repeats, or -1.
    std::int32_t duplicate_of(std::size_t i) const noexcept {
        const std::int32_t d = instance(i).duplicate_of;
        return d < 0 ? -1 : result_of_[static_cast<std::size_t>(d)];
    }
    // The file name of an instance (stored + display), by result index.
    const GroupText& instance_name(std::size_t i) const noexcept { return files_[instance(i).file].name; }

private:
    struct File {
        GroupText name;
        std::int32_t instance = -1;   // internal instance index, -1 when not kept
        SkipReason skip = SkipReason::None;
        Limit limit = Limit::None;
        Errc status = Errc::Ok;
        std::int32_t duplicate_of = -1;  // KeepFirst: internal instance index of the first
        std::vector<std::uint32_t> dicomdir_matches;  // build(): listed File IDs it matched
        bool unsafe_name = false;                      // build(): grouping_name_unsafe
    };
    struct DicomdirEntry {
        std::uint32_t dicomdir = 0;
        std::uint32_t record = 0;
        Errc file_status = Errc::Ok;
        std::size_t components = 0;
        GroupText file_id;               // components joined with '/'
    };
    struct Dicomdir {
        SkipReason skip = SkipReason::None;
        Limit limit = Limit::None;
        Errc status = Errc::Ok;
    };

    bool keep_text(GroupText& t, std::string_view stored, std::uint64_t& cost) const;
    void skip(File& f, SkipReason r, Limit l, Errc status = Errc::Ok);
    void budget(Limit l);

    GroupingOptions opt_;
    std::vector<File> files_;
    std::vector<GroupedInstance> instances_;  // internal order: as added
    std::vector<std::string> patient_keys_;   // internal: per instance, the Patient ID key (never returned)
    std::unordered_map<std::string, std::uint32_t> sop_index_;
    std::vector<Dicomdir> dicomdirs_;
    std::vector<DicomdirEntry> entries_;
    GroupingSummary summary_;
    bool built_ = false;

    std::vector<GroupedPatient> patients_;
    std::vector<GroupedStudy> studies_;
    std::vector<GroupedSeries> series_;
    std::vector<std::uint32_t> order_;        // result index -> internal instance index
    std::vector<std::int32_t> result_of_;     // internal instance index -> result index
    std::vector<GroupingIssue> issues_;
};

// Is `uid` a valid UID for grouping (see UidValidation)?
bool grouping_uid_valid(std::string_view uid, UidValidation rule = kGroupingDefaultUidValidation) noexcept;
// Is a caller's name unsafe as a path: a ".." component, absolute (a leading '/' or '\'), or a drive
// letter ("C:")? Such names are never matched to File IDs and are reported.
bool grouping_name_unsafe(std::string_view name) noexcept;

}  // namespace siaara::store
