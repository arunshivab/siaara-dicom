// Fuzz target: the grouping of files that arrived together (siaara_grouping_*): every file read
// header-only with the untrusted limits, the budgets, ordering (NaN / infinity never reach a
// comparator), duplicates, inconsistencies, DICOMDIR File ID matching; then the whole result walked
// through the getters.
// Input: byte 0 picks the options; then entries until the input ends:
//   u8 kind (bit 0: a DICOMDIR), u8 name length, name, u32 data length (little endian, cut to what
//   is left), data.
#include <cmath>
#include <cstring>
#include <string>

#include "fuzz.hpp"
#include "siaara/siaara.h"

namespace {

void check_text(const siaara_grouping* g, int32_t level, uint32_t index, int32_t field) {
    siaara_grouping_text t;
    std::memset(&t, 0, sizeof t);
    t.struct_size = sizeof t;
    FUZZ_REQUIRE(siaara_grouping_text_get(g, level, index, field, &t) == SIAARA_OK);
    FUZZ_REQUIRE((t.stored != nullptr) == (t.status == SIAARA_GTEXT_OK || t.status == SIAARA_GTEXT_NOT_A_UID));
    if (!t.stored) return;
    FUZZ_REQUIRE(t.display != nullptr && t.display[t.display_len] == '\0' && t.stored[t.stored_len] == '\0');
    // The display copy is the library's cleaner applied to the stored value.
    std::string again(t.display_len + 1, '\0');
    std::size_t needed = 0;
    FUZZ_REQUIRE(siaara_text_display_safe(t.stored, t.stored_len, &again[0], again.size(), &needed) == SIAARA_OK);
    FUZZ_REQUIRE(needed == t.display_len && std::memcmp(again.data(), t.display, needed) == 0);
}

}  // namespace

SIAARA_FUZZ_TARGET(grouping) {
    siaara_fuzz::Input in(data, size);
    const std::uint8_t pick = in.u8();
    siaara_grouping_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.order = pick % 3;
    o.duplicates = (pick >> 2) & 1;
    o.match_case_insensitive = (pick & 8) ? SIAARA_GROUP_SWITCH_OFF : SIAARA_GROUP_SWITCH_DEFAULT;
    o.match_by_trailing_components = (pick & 16) ? SIAARA_GROUP_SWITCH_OFF : SIAARA_GROUP_SWITCH_DEFAULT;
    o.include_patient_fields = (pick >> 5) & 1;
    o.position_fallback = pick >> 7;
    o.tie_break = (pick / 7) % 3;
    o.uid_validation = (pick >> 4) & 1;
    o.series_order = (pick >> 1) & 1;
    if (pick & 64) {  // small budgets, so they are reached
        o.max_files = 3;
        o.max_result_bytes = 600;
        o.max_total_inflated = 4096;
        o.max_text_bytes = 16;
    }
    siaara_grouping* g = nullptr;
    FUZZ_REQUIRE(siaara_grouping_create(&o, &g) == SIAARA_OK);
    while (in.remaining() > 0) {
        const std::uint8_t kind = in.u8();
        const std::string_view name = in.take(in.u8());
        std::size_t len = in.u32();
        if (len > in.remaining()) len = in.remaining();
        const std::uint8_t* p = in.ptr();
        (void)in.take(len);
        if (kind & 1) FUZZ_REQUIRE(siaara_grouping_add_dicomdir(g, len ? p : nullptr, len) == SIAARA_OK);
        else FUZZ_REQUIRE(siaara_grouping_add_file(g, name.data(), name.size(), len ? p : nullptr, len) == SIAARA_OK);
    }
    siaara_grouping_summary s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    FUZZ_REQUIRE(siaara_grouping_result(g, &s) == SIAARA_OK);
    if (pick & 64) FUZZ_REQUIRE(s.result_bytes <= 600 && s.files_read <= 3 && s.total_inflated <= 4096);
    std::uint64_t in_series = 0;
    for (uint32_t k = 0; k < s.series; ++k) {
        siaara_grouped_series e;
        std::memset(&e, 0, sizeof e);
        e.struct_size = sizeof e;
        FUZZ_REQUIRE(siaara_grouping_series(g, k, &e) == SIAARA_OK);
        FUZZ_REQUIRE(e.instance_count > 0 && e.first_instance + e.instance_count <= s.instances);
        in_series += e.instance_count;
        double last = -INFINITY;
        int32_t last_key = SIAARA_GSORT_POSITION;
        for (uint32_t i = e.first_instance; i < e.first_instance + e.instance_count; ++i) {
            siaara_grouped_instance n;
            std::memset(&n, 0, sizeof n);
            n.struct_size = sizeof n;
            FUZZ_REQUIRE(siaara_grouping_instance(g, i, &n) == SIAARA_OK);
            FUZZ_REQUIRE(n.series == static_cast<int32_t>(k));
            FUZZ_REQUIRE(n.sort_key >= last_key);  // tiers in order
            if (n.sort_key == SIAARA_GSORT_POSITION) {
                FUZZ_REQUIRE((n.flags & SIAARA_GINST_POSITION_USABLE) && std::isfinite(n.position));
                FUZZ_REQUIRE(n.position >= last);
                last = n.position;
            }
            last_key = n.sort_key;
        }
        for (int32_t f : {SIAARA_GFIELD_SERIES_UID, SIAARA_GFIELD_MODALITY, SIAARA_GFIELD_SERIES_DESCRIPTION})
            check_text(g, SIAARA_GLEVEL_SERIES, k, f);
    }
    FUZZ_REQUIRE(in_series + s.ungrouped == s.instances);
    for (uint32_t i = 0; i < s.instances; ++i)
        for (int32_t f = SIAARA_GFIELD_NAME; f <= SIAARA_GFIELD_ISSUER_OF_PATIENT_ID; ++f)
            if (f != SIAARA_GFIELD_SUBJECT) check_text(g, SIAARA_GLEVEL_INSTANCE, i, f);
    for (uint32_t i = 0; i < s.patients; ++i) check_text(g, SIAARA_GLEVEL_PATIENT, i, SIAARA_GFIELD_PATIENT_ID);
    for (uint32_t i = 0; i < s.studies; ++i) check_text(g, SIAARA_GLEVEL_STUDY, i, SIAARA_GFIELD_STUDY_UID);
    for (uint32_t i = 0; i < s.issues; ++i) {
        siaara_grouping_issue is;
        std::memset(&is, 0, sizeof is);
        is.struct_size = sizeof is;
        FUZZ_REQUIRE(siaara_grouping_issue_get(g, i, &is) == SIAARA_OK);
        FUZZ_REQUIRE(is.instance < static_cast<int64_t>(s.instances) && is.other_instance < static_cast<int64_t>(s.instances));
        check_text(g, SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_NAME);
        check_text(g, SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_SUBJECT);
    }
    if (!o.include_patient_fields && s.instances > 0) {
        siaara_grouping_text t;
        std::memset(&t, 0, sizeof t);
        t.struct_size = sizeof t;
        FUZZ_REQUIRE(siaara_grouping_text_get(g, SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_PATIENT_ID, &t) == SIAARA_OK);
        FUZZ_REQUIRE(t.status == SIAARA_GTEXT_NOT_REQUESTED && t.stored == nullptr);
    }
    siaara_grouping_free(g);
    return 0;
}
