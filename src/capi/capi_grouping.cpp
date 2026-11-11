// SIAARA DICOM Toolkit - C API: the grouping of files that arrived together (store/grouping.hpp).
#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#include "capi/capi_internal.hpp"
#include "store/grouping.hpp"

struct siaara_grouping {
    siaara::store::Grouping g;
    siaara::capi::LiveHandle live_{};
    explicit siaara_grouping(const siaara::store::GroupingOptions& o) : g(o) {}
};

namespace {

using siaara::Errc;
namespace capi = siaara::capi;
namespace st = siaara::store;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
siaara_status ok() noexcept { return capi::clear_error(); }

template <class F>
siaara_status guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return fail(Errc::OutOfMemory, "out of memory");
    } catch (...) {
        return fail(Errc::Internal, "internal error");
    }
}

static_assert(sizeof(siaara_grouping_options) == 136, "siaara_grouping_options layout");
static_assert(sizeof(siaara_grouping_summary) == 160, "siaara_grouping_summary layout");
static_assert(sizeof(siaara_grouped_series) == 48, "siaara_grouped_series layout");
static_assert(sizeof(siaara_grouped_instance) == 160, "siaara_grouped_instance layout");
static_assert(sizeof(siaara_grouping_issue) == 56, "siaara_grouping_issue layout");
static_assert(sizeof(siaara_grouped_patient) == 24, "siaara_grouped_patient layout");
static_assert(static_cast<int>(st::TextField::Subject) == SIAARA_GFIELD_SUBJECT &&
                  static_cast<int>(st::TextField::IssuerOfPatientId) == SIAARA_GFIELD_ISSUER_OF_PATIENT_ID,
              "text fields");
static_assert(static_cast<int>(st::SeriesOrder::Mixed) == SIAARA_GSERIES_ORDER_MIXED, "series order rules");
static_assert(static_cast<int>(st::IssueKind::UnsafeName) == SIAARA_GISSUE_UNSAFE_NAME, "issue kinds");
static_assert(static_cast<int>(st::Limit::MaxDicomdirs) == SIAARA_GLIMIT_MAX_DICOMDIRS, "limits");
static_assert(st::kGiUnsafeName == SIAARA_GINST_UNSAFE_NAME && st::kGiAcquisitionTime == SIAARA_GINST_ACQUISITION_TIME,
              "instance flags");
static_assert(st::kGroupingDefaultMaxDicomdirs == SIAARA_GROUPING_DEFAULT_MAX_DICOMDIRS &&
                  st::kGroupingMaxDicomdirsLimit == SIAARA_GROUPING_MAX_DICOMDIRS_LIMIT,
              "dicomdir defaults");
static_assert(static_cast<int>(st::TextStatus::NotKept) == SIAARA_GTEXT_NOT_KEPT, "text status");
static_assert(static_cast<int>(st::IssueKind::AmbiguousMatch) == SIAARA_GISSUE_AMBIGUOUS_MATCH, "issue kinds");
static_assert(static_cast<int>(st::Limit::DicomdirRecords) == SIAARA_GLIMIT_DICOMDIR_RECORDS, "limits");
static_assert(static_cast<int>(st::SortKey::Added) == SIAARA_GSORT_ADDED, "sort keys");
static_assert(st::kGiInDicomdir == SIAARA_GINST_IN_DICOMDIR && st::kGiSeriesNumber == SIAARA_GINST_SERIES_NUMBER,
              "instance flags");
static_assert(st::kGroupingDefaultMaxFiles == SIAARA_GROUPING_DEFAULT_MAX_FILES &&
                  st::kGroupingDefaultMaxFileBytes == SIAARA_GROUPING_DEFAULT_MAX_FILE_BYTES &&
                  st::kGroupingDefaultMaxTotalBytes == SIAARA_GROUPING_DEFAULT_MAX_TOTAL_BYTES &&
                  st::kGroupingDefaultMaxTotalInflated == SIAARA_GROUPING_DEFAULT_MAX_TOTAL_INFLATED &&
                  st::kGroupingDefaultMaxResultBytes == SIAARA_GROUPING_DEFAULT_MAX_RESULT_BYTES &&
                  st::kGroupingDefaultMaxUidChars == SIAARA_GROUPING_DEFAULT_MAX_UID_CHARS &&
                  st::kGroupingDefaultMaxTextBytes == SIAARA_GROUPING_DEFAULT_MAX_TEXT_BYTES &&
                  siaara::kUntrustedMaxDepth == SIAARA_GROUPING_DEFAULT_MAX_DEPTH &&
                  siaara::kUntrustedMaxElements == SIAARA_GROUPING_DEFAULT_MAX_ELEMENTS &&
                  siaara::kUntrustedMaxInflatedSize == SIAARA_GROUPING_DEFAULT_MAX_INFLATED_SIZE &&
                  siaara::kUntrustedMaxValueBytes == SIAARA_GROUPING_DEFAULT_MAX_VALUE_BYTES,
              "grouping defaults");

// A switch whose default is on.
bool switch_value(int32_t v, bool def, bool& ok_out) {
    if (v == SIAARA_GROUP_SWITCH_DEFAULT) return def;
    if (v == SIAARA_GROUP_SWITCH_ON) return true;
    if (v == SIAARA_GROUP_SWITCH_OFF) return false;
    ok_out = false;
    return def;
}

siaara::Status to_grouping_options(const siaara_grouping_options* in, st::GroupingOptions& o) {
    if (!in) return siaara::Status::ok();
    siaara_grouping_options c;
    // R-081: the struct before series_order was appended (128 bytes) is accepted too.
    constexpr std::size_t kFirstSize = offsetof(siaara_grouping_options, series_order);
    static_assert(kFirstSize == 128, "the first published siaara_grouping_options");
    if (!capi::sized_options(in, kFirstSize, c))
        return siaara::Status(Errc::InvalidArgument, "options must be set up with struct_size");
    auto set = [](auto& field, auto v) {
        if (v) field = v;
    };
    set(o.max_files, static_cast<std::uint64_t>(c.max_files));
    set(o.max_file_bytes, c.max_file_bytes);
    set(o.max_total_bytes, c.max_total_bytes);
    set(o.max_total_inflated, c.max_total_inflated);
    set(o.max_result_bytes, c.max_result_bytes);
    set(o.max_uid_chars, c.max_uid_chars);
    set(o.max_text_bytes, c.max_text_bytes);
    set(o.max_depth, c.max_depth);
    set(o.max_elements, c.max_elements);
    set(o.max_inflated_size, c.max_inflated_size);
    set(o.max_value_bytes, c.max_value_bytes);
    if (c.order < SIAARA_GROUP_ORDER_POSITION || c.order > SIAARA_GROUP_ORDER_AS_ADDED)
        return siaara::Status(Errc::InvalidArgument, "order: SIAARA_GROUP_ORDER_*");
    o.order = static_cast<st::GroupOrder>(c.order);
    if (c.include_patient_fields != 0 && c.include_patient_fields != 1)
        return siaara::Status(Errc::InvalidArgument, "include_patient_fields: 0 or 1");
    o.include_patient_fields = c.include_patient_fields == 1;
    if (c.duplicates != SIAARA_GROUP_DUPLICATES_KEEP_BOTH && c.duplicates != SIAARA_GROUP_DUPLICATES_KEEP_FIRST)
        return siaara::Status(Errc::InvalidArgument, "duplicates: SIAARA_GROUP_DUPLICATES_*");
    o.duplicates = static_cast<st::GroupDuplicates>(c.duplicates);
    bool good = true;
    o.match_case_insensitive = switch_value(c.match_case_insensitive, st::kGroupingDefaultMatchCaseInsensitive, good);
    o.match_by_trailing_components =
        switch_value(c.match_by_trailing_components, st::kGroupingDefaultMatchByTrailingComponents, good);
    if (!good) return siaara::Status(Errc::InvalidArgument, "match switches: SIAARA_GROUP_SWITCH_*");
    if (!std::isfinite(c.orientation_tolerance) || c.orientation_tolerance < 0)
        return siaara::Status(Errc::InvalidArgument, "orientation_tolerance must be finite and not negative");
    if (c.orientation_tolerance > 0) o.orientation_tolerance = c.orientation_tolerance;
    if (c.position_fallback != SIAARA_GROUP_FALLBACK_PER_SERIES && c.position_fallback != SIAARA_GROUP_FALLBACK_PER_INSTANCE)
        return siaara::Status(Errc::InvalidArgument, "position_fallback: SIAARA_GROUP_FALLBACK_*");
    o.position_fallback = static_cast<st::PositionFallback>(c.position_fallback);
    if (c.tie_break < SIAARA_GROUP_TIE_INSTANCE_NUMBER || c.tie_break > SIAARA_GROUP_TIE_ACQUISITION_TIME)
        return siaara::Status(Errc::InvalidArgument, "tie_break: SIAARA_GROUP_TIE_*");
    o.tie_break = static_cast<st::TieBreak>(c.tie_break);
    if (c.uid_validation != SIAARA_GROUP_UID_STRICT && c.uid_validation != SIAARA_GROUP_UID_LENIENT)
        return siaara::Status(Errc::InvalidArgument, "uid_validation: SIAARA_GROUP_UID_*");
    o.uid_validation = static_cast<st::UidValidation>(c.uid_validation);
    if (c.max_dicomdirs > SIAARA_GROUPING_MAX_DICOMDIRS_LIMIT)
        return siaara::Status(Errc::InvalidArgument, "max_dicomdirs: at most SIAARA_GROUPING_MAX_DICOMDIRS_LIMIT");
    set(o.max_dicomdirs, c.max_dicomdirs);
    if (c.series_order != SIAARA_GROUP_SERIES_ORDER_AS_ADDED && c.series_order != SIAARA_GROUP_SERIES_ORDER_SERIES_NUMBER)
        return siaara::Status(Errc::InvalidArgument, "series_order: SIAARA_GROUP_SERIES_ORDER_*");
    o.series_order = static_cast<st::GroupSeriesOrder>(c.series_order);
    return siaara::Status::ok();
}

// Writes `full` to the caller's struct: at least the first version's size is required, a larger
// struct_size is accepted and keeps its size (only the fields known here are written).
template <class T>
bool sized_out(const T& full, T* out) noexcept {
    if (!out || out->struct_size < sizeof(T)) return false;
    const uint32_t size = out->struct_size;
    std::memcpy(out, &full, sizeof(T));
    out->struct_size = size;
    return true;
}

siaara_status need_result(const siaara_grouping* g) {
    if (!g) return fail(Errc::InvalidArgument, "grouping must not be NULL");
    if (!g->g.built()) return fail(Errc::InvalidArgument, "call siaara_grouping_result first (files were added since)");
    return SIAARA_OK;
}

void fill_text(const st::GroupText* t, siaara_grouping_text& o) {
    if (!t) return;
    o.status = static_cast<int32_t>(t->status);
    if (t->status == st::TextStatus::Ok || t->status == st::TextStatus::NotAUid) {
        o.stored = t->stored.c_str();
        o.stored_len = t->stored.size();
        o.display = t->display.c_str();
        o.display_len = t->display.size();
    }
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_create(const siaara_grouping_options* options,
                                                            siaara_grouping** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        st::GroupingOptions o;
        if (siaara::Status s = to_grouping_options(options, o); !s) return fail(s.code, s.message);
        *out = new siaara_grouping(o);
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_grouping_free(siaara_grouping* grouping) SIAARA_NOEXCEPT { delete grouping; }

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_add_file(siaara_grouping* grouping, const char* name,
                                                              size_t name_len, const uint8_t* data, size_t size) {
    return guarded([&]() -> siaara_status {
        if (!grouping || (!name && name_len) || (!data && size))
            return fail(Errc::InvalidArgument, "grouping must be set; name and data may be NULL only with a length of 0");
        grouping->g.add_file(std::string_view(name ? name : "", name_len), data, size);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_add_dicomdir(siaara_grouping* grouping, const uint8_t* data,
                                                                  size_t size) {
    return guarded([&]() -> siaara_status {
        if (!grouping || (!data && size))
            return fail(Errc::InvalidArgument, "grouping must be set; data may be NULL only with a size of 0");
        grouping->g.add_dicomdir(data, size);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_result(siaara_grouping* grouping, siaara_grouping_summary* summary) {
    return guarded([&]() -> siaara_status {
        if (!grouping) return fail(Errc::InvalidArgument, "grouping must not be NULL");
        if (summary && summary->struct_size < sizeof(siaara_grouping_summary))
            return fail(Errc::InvalidArgument, "summary must be set up with struct_size");
        grouping->g.build();
        if (summary) {
            const st::GroupingSummary& s = grouping->g.summary();
            siaara_grouping_summary o{};
            o.budget_reached = static_cast<uint32_t>(s.budget_reached);
            o.files = s.files;
            o.files_read = s.files_read;
            o.instances = s.instances;
            o.patients = s.patients;
            o.studies = s.studies;
            o.series = s.series;
            o.issues = s.issues;
            o.skipped = s.skipped;
            o.ungrouped = s.ungrouped;
            o.duplicates = s.duplicates;
            o.inconsistencies = s.inconsistencies;
            o.dicomdirs = s.dicomdirs;
            o.listed_missing = s.listed_missing;
            o.present_unlisted = s.present_unlisted;
            o.ambiguous = s.ambiguous;
            o.unsafe_names = s.unsafe_names;
            o.total_bytes = s.total_bytes;
            o.total_inflated = s.total_inflated;
            o.result_bytes = s.result_bytes;
            (void)sized_out(o, summary);
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_patient(const siaara_grouping* grouping, uint32_t index,
                                                             siaara_grouped_patient* out) {
    return guarded([&]() -> siaara_status {
        if (siaara_status s = need_result(grouping); s != SIAARA_OK) return s;
        const auto& v = grouping->g.patients();
        if (index >= v.size()) return fail(Errc::InvalidArgument, "patient index out of range");
        siaara_grouped_patient o{};
        o.first_study = v[index].first_study;
        o.study_count = v[index].study_count;
        o.instance_count = v[index].instance_count;
        o.flags = v[index].no_patient_id ? static_cast<uint32_t>(SIAARA_GPATIENT_NO_PATIENT_ID) : 0u;
        if (!sized_out(o, out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_study(const siaara_grouping* grouping, uint32_t index,
                                                           siaara_grouped_study* out) {
    return guarded([&]() -> siaara_status {
        if (siaara_status s = need_result(grouping); s != SIAARA_OK) return s;
        const auto& v = grouping->g.studies();
        if (index >= v.size()) return fail(Errc::InvalidArgument, "study index out of range");
        siaara_grouped_study o{};
        o.patient = v[index].patient;
        o.first_series = v[index].first_series;
        o.series_count = v[index].series_count;
        o.instance_count = v[index].instance_count;
        if (!sized_out(o, out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_series(const siaara_grouping* grouping, uint32_t index,
                                                            siaara_grouped_series* out) {
    return guarded([&]() -> siaara_status {
        if (siaara_status s = need_result(grouping); s != SIAARA_OK) return s;
        const auto& v = grouping->g.series();
        if (index >= v.size()) return fail(Errc::InvalidArgument, "series index out of range");
        siaara_grouped_series o{};
        o.study = v[index].study;
        o.first_instance = v[index].first_instance;
        o.instance_count = v[index].instance_count;
        o.has_normal = v[index].has_normal ? 1 : 0;
        o.order_rule = static_cast<int32_t>(v[index].order);
        for (std::size_t i = 0; i < 3; ++i) o.normal[i] = v[index].normal[i];
        if (!sized_out(o, out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_instance(const siaara_grouping* grouping, uint32_t index,
                                                              siaara_grouped_instance* out) {
    return guarded([&]() -> siaara_status {
        if (siaara_status s = need_result(grouping); s != SIAARA_OK) return s;
        const st::Grouping& g = grouping->g;
        if (index >= g.instance_count()) return fail(Errc::InvalidArgument, "instance index out of range");
        const st::GroupedInstance& in = g.instance(index);
        siaara_grouped_instance o{};
        o.flags = in.flags;
        o.series = in.series;
        o.file = in.file;
        o.sort_key = static_cast<int32_t>(in.sort_key);
        o.read_loss = in.read_loss;
        o.duplicate_of = g.duplicate_of(index);
        o.series_number = in.series_number;
        o.instance_number = in.instance_number;
        o.number_of_frames = in.number_of_frames;
        o.rows = in.rows;
        o.columns = in.columns;
        for (std::size_t i = 0; i < 3; ++i) o.image_position[i] = in.image_position[i];
        for (std::size_t i = 0; i < 6; ++i) o.image_orientation[i] = in.image_orientation[i];
        o.slice_location = in.slice_location;
        o.position = in.position;
        o.acquisition_time = in.acquisition_time;
        if (!sized_out(o, out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_issue_get(const siaara_grouping* grouping, uint32_t index,
                                                               siaara_grouping_issue* out) {
    return guarded([&]() -> siaara_status {
        if (siaara_status s = need_result(grouping); s != SIAARA_OK) return s;
        const auto& v = grouping->g.issues();
        if (index >= v.size()) return fail(Errc::InvalidArgument, "issue index out of range");
        const st::GroupingIssue& i = v[index];
        siaara_grouping_issue o{};
        o.kind = static_cast<int32_t>(i.kind);
        o.reason = i.reason;
        o.limit = static_cast<int32_t>(i.limit);
        o.status = static_cast<siaara_status>(i.status);
        o.file = i.file;
        o.instance = i.instance;
        o.other_instance = i.other_instance;
        o.study = i.study;
        o.series = i.series;
        o.dicomdir = i.dicomdir;
        o.record = i.record;
        o.dicomdir_mask = i.dicomdir_mask;
        if (!sized_out(o, out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_grouping_text_get(const siaara_grouping* grouping, int32_t level,
                                                              uint32_t index, int32_t field,
                                                              siaara_grouping_text* out) {
    return guarded([&]() -> siaara_status {
        if (siaara_status s = need_result(grouping); s != SIAARA_OK) return s;
        if (!out || out->struct_size < sizeof(siaara_grouping_text))
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        if (field < SIAARA_GFIELD_NAME || field > SIAARA_GFIELD_ISSUER_OF_PATIENT_ID)
            return fail(Errc::InvalidArgument, "field: SIAARA_GFIELD_*");
        const st::Grouping& g = grouping->g;
        const auto f = static_cast<st::TextField>(field);
        const st::GroupText* t = nullptr;
        auto in_level = [&](std::initializer_list<int32_t> allowed) {
            for (int32_t a : allowed)
                if (a == field) return true;
            return false;
        };
        // The text of a patient, study or series is that of its first file (as added).
        auto internal_text = [&](std::uint32_t internal) -> const st::GroupText* {
            return &g.internal_instance(internal).text[static_cast<std::size_t>(field)];
        };
        switch (level) {
            case SIAARA_GLEVEL_PATIENT:
                if (index >= g.patients().size()) return fail(Errc::InvalidArgument, "patient index out of range");
                if (!in_level({SIAARA_GFIELD_PATIENT_ID, SIAARA_GFIELD_PATIENT_NAME, SIAARA_GFIELD_ISSUER_OF_PATIENT_ID}))
                    return fail(Errc::InvalidArgument, "field not available for a patient");
                t = internal_text(g.patients()[index].first_instance_ref);
                break;
            case SIAARA_GLEVEL_STUDY:
                if (index >= g.studies().size()) return fail(Errc::InvalidArgument, "study index out of range");
                if (!in_level({SIAARA_GFIELD_STUDY_UID, SIAARA_GFIELD_STUDY_DATE}))
                    return fail(Errc::InvalidArgument, "field not available for a study");
                t = internal_text(g.studies()[index].first_instance_ref);
                break;
            case SIAARA_GLEVEL_SERIES:
                if (index >= g.series().size()) return fail(Errc::InvalidArgument, "series index out of range");
                if (!in_level({SIAARA_GFIELD_SERIES_UID, SIAARA_GFIELD_MODALITY, SIAARA_GFIELD_SERIES_DESCRIPTION}))
                    return fail(Errc::InvalidArgument, "field not available for a series");
                t = internal_text(g.series()[index].first_instance_ref);
                break;
            case SIAARA_GLEVEL_INSTANCE:
                if (index >= g.instance_count()) return fail(Errc::InvalidArgument, "instance index out of range");
                if (f == st::TextField::Subject) return fail(Errc::InvalidArgument, "field not available for an instance");
                t = f == st::TextField::Name ? &g.instance_name(index) : &g.instance(index).text[static_cast<std::size_t>(field)];
                break;
            case SIAARA_GLEVEL_ISSUE: {
                if (index >= g.issues().size()) return fail(Errc::InvalidArgument, "issue index out of range");
                if (!in_level({SIAARA_GFIELD_NAME, SIAARA_GFIELD_SUBJECT}))
                    return fail(Errc::InvalidArgument, "field not available for an issue");
                const st::GroupingIssue& i = g.issues()[index];
                t = f == st::TextField::Name ? i.name : i.subject;
                break;
            }
            default:
                return fail(Errc::InvalidArgument, "level: SIAARA_GLEVEL_*");
        }
        siaara_grouping_text o{};
        o.struct_size = out->struct_size;
        fill_text(t, o);
        (void)sized_out(o, out);
        return ok();
    });
}

}  // extern "C"
