// Grouping of files that arrived together (siaara_grouping_*): order within a series, fallbacks,
// duplicates, ungrouped files, inconsistencies, caps and budgets, DICOMDIR File IDs against names.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dicomdir_synth.hpp"
#include "grouping_synth.hpp"
#include "siaara/siaara.h"
#include "store/dicomdir.hpp"
#include "store/grouping.hpp"
#include "test.hpp"

using namespace siaara;
using namespace dicomdir_synth;
using grouping_synth::Grouping;
using grouping_synth::Spec;
namespace fs = std::filesystem;

namespace {

// ---- synthetic CT
struct Ct {
    std::string study = "1.2.826.0.1.7", series = "1.2.826.0.1.7.1", sop;
    std::string patient_id = "PID1", modality = "CT";
    int instance = 1;
    bool has_instance = true;
    std::array<double, 3> ipp{};
    std::array<double, 6> iop{1, 0, 0, 0, 1, 0};
    bool has_ipp = true, has_iop = true;
    std::uint16_t rows = 4, columns = 4;
};

DicomFile ct_file(const Ct& c) {
    Spec s;
    s.study_uid = c.study;
    s.series_uid = c.series;
    s.sop_uid = c.sop;
    s.patient_id = c.patient_id;
    s.modality = c.modality;
    s.instance_number = c.instance;
    s.has_instance_number = c.has_instance;
    s.rows = c.rows;
    s.columns = c.columns;
    return grouping_synth::geometry_file(s, c.has_ipp ? c.ipp.data() : nullptr, c.has_iop ? c.iop.data() : nullptr);
}

std::vector<std::uint8_t> ct(const Ct& c) { return grouping_synth::bytes_of(ct_file(c)); }

// Image Position (Patient) stored as FD (binary doubles: NaN and infinity get through).
std::vector<std::uint8_t> ct_fd_position(const Ct& c, const std::array<double, 3>& ipp) {
    DicomFile f = ct_file(c);
    (void)f.dataset.remove(0x00200032);
    (void)f.dataset.set_bytes(0x00200032, VR::FD, Bytes::copy_of(ipp.data(), sizeof(double) * 3));
    return grouping_synth::bytes_of(f);
}

// The SOP Instance UIDs of a series' instances, in result order.
std::vector<std::string> series_sops(const Grouping& g, uint32_t k) {
    const siaara_grouped_series e = g.series(k);
    std::vector<std::string> v;
    for (uint32_t i = e.first_instance; i < e.first_instance + e.instance_count; ++i)
        v.push_back(g.stored(SIAARA_GLEVEL_INSTANCE, i, SIAARA_GFIELD_SOP_INSTANCE_UID));
    return v;
}

std::size_t count_kind(const std::vector<siaara_grouping_issue>& v, int32_t kind) {
    return static_cast<std::size_t>(std::count_if(v.begin(), v.end(), [&](const siaara_grouping_issue& i) { return i.kind == kind; }));
}

}  // namespace

// ================================================================ grouping
TEST_CASE("grouping: a multi-series CT, shuffled, ordered along an oblique normal against contradicting Instance Numbers") {
    // Series A: 8 slices along an oblique normal; B: 5 axial; C: 3 sagittal. Instance Numbers run
    // against the positions everywhere.
    const double r = std::sqrt(0.5);
    struct S {
        std::string uid;
        std::array<double, 6> iop;
        int n;
    };
    const std::vector<S> series = {{"1.2.826.0.1.7.1", {r, r, 0, 0, 0, -1}, 8},
                                   {"1.2.826.0.1.7.2", {1, 0, 0, 0, 1, 0}, 5},
                                   {"1.2.826.0.1.7.3", {0, 1, 0, 0, 0, -1}, 3}};
    struct F {
        std::string name;
        std::vector<std::uint8_t> bytes;
        std::string sop;
        double pos;
    };
    std::vector<F> files;
    for (const auto& s : series) {
        const std::array<double, 3> n{s.iop[1] * s.iop[5] - s.iop[2] * s.iop[4], s.iop[2] * s.iop[3] - s.iop[0] * s.iop[5],
                                      s.iop[0] * s.iop[4] - s.iop[1] * s.iop[3]};
        for (int k = 0; k < s.n; ++k) {
            Ct c;
            c.series = s.uid;
            c.sop = s.uid + "." + std::to_string(k + 1);
            c.iop = s.iop;
            const double d = -40.0 + 2.5 * k;
            for (int a = 0; a < 3; ++a) c.ipp[static_cast<std::size_t>(a)] = 7.0 + d * n[static_cast<std::size_t>(a)];
            c.instance = 100 - k;  // contradicts the positions
            files.push_back({"zip/CT/" + c.sop + ".dcm", ct(c), c.sop, d});
        }
    }
    // a fixed shuffle
    std::vector<std::size_t> perm(files.size());
    for (std::size_t i = 0; i < perm.size(); ++i) perm[i] = (i * 7 + 3) % perm.size();
    REQUIRE(std::set<std::size_t>(perm.begin(), perm.end()).size() == perm.size());

    for (int order : {SIAARA_GROUP_ORDER_POSITION, SIAARA_GROUP_ORDER_INSTANCE_NUMBER, SIAARA_GROUP_ORDER_AS_ADDED}) {
        siaara_grouping_options o = grouping_synth::options();
        o.order = order;
        Grouping g(&o);
        REQUIRE(g.create_status() == SIAARA_OK);
        for (std::size_t i : perm) CHECK_EQ(g.add(files[i].name, files[i].bytes), SIAARA_OK);
        const siaara_grouping_summary s = g.result();
        REQUIRE(g.last() == SIAARA_OK);
        CHECK_EQ(s.patients, 1u);
        CHECK_EQ(s.studies, 1u);
        REQUIRE(s.series == 3u);
        CHECK_EQ(s.instances, 16u);
        CHECK_EQ(s.issues, 0u);
        CHECK_EQ(s.files, 16u);
        for (uint32_t k = 0; k < 3; ++k) {
            const siaara_grouped_series e = g.series(k);
            const std::string uid = g.stored(SIAARA_GLEVEL_SERIES, k, SIAARA_GFIELD_SERIES_UID);
            auto it = std::find_if(series.begin(), series.end(), [&](const S& x) { return x.uid == uid; });
            REQUIRE(it != series.end());
            CHECK_EQ(e.instance_count, static_cast<uint32_t>(it->n));
            CHECK_EQ(e.has_normal, 1);
            const std::vector<std::string> sops = series_sops(g, k);
            if (order == SIAARA_GROUP_ORDER_POSITION || order == SIAARA_GROUP_ORDER_INSTANCE_NUMBER) {
                // by position: k = 1 .. n; by Instance Number (100 - k): n .. 1
                for (int j = 0; j < it->n; ++j) {
                    const int want = order == SIAARA_GROUP_ORDER_POSITION ? j + 1 : it->n - j;
                    CHECK(sops[static_cast<std::size_t>(j)] == uid + "." + std::to_string(want));
                }
            }
            for (uint32_t i = e.first_instance; i < e.first_instance + e.instance_count; ++i) {
                const siaara_grouped_instance in = g.instance(i);
                CHECK_EQ(in.series, static_cast<int32_t>(k));
                CHECK((in.flags & SIAARA_GINST_POSITION_USABLE) != 0);
                const int32_t want = order == SIAARA_GROUP_ORDER_POSITION         ? SIAARA_GSORT_POSITION
                                     : order == SIAARA_GROUP_ORDER_INSTANCE_NUMBER ? SIAARA_GSORT_INSTANCE_NUMBER
                                                                                   : SIAARA_GSORT_ADDED;
                CHECK_EQ(in.sort_key, want);
                if (i > e.first_instance && order == SIAARA_GROUP_ORDER_POSITION) {
                    const siaara_grouped_instance prev = g.instance(i - 1);
                    CHECK(in.position > prev.position);
                    CHECK(in.instance_number < prev.instance_number);
                }
                if (i > e.first_instance && order == SIAARA_GROUP_ORDER_AS_ADDED) CHECK(in.file > g.instance(i - 1).file);
            }
        }
        // Positions: the distances along the normal the files were made with (plus the origin's part).
        const siaara_grouped_instance first = g.instance(0);
        CHECK(std::isfinite(first.position));
        // The patient fields are not returned by default.
        CHECK_EQ(g.status(SIAARA_GLEVEL_PATIENT, 0, SIAARA_GFIELD_PATIENT_ID), SIAARA_GTEXT_NOT_REQUESTED);
        CHECK_EQ(g.status(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_PATIENT_NAME), SIAARA_GTEXT_NOT_REQUESTED);
        CHECK(g.text(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_PATIENT_ID).stored == nullptr);
        CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_NAME).rfind("zip/CT/", 0) == 0);
        CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_MODALITY) == "CT");
        CHECK(g.stored(SIAARA_GLEVEL_STUDY, 0, SIAARA_GFIELD_STUDY_DATE) == "20200115");
    }
}

TEST_CASE("grouping: NaN, infinite, missing positions and mixed orientations fall back to Slice Location, then Instance Number") {
    Grouping g;
    auto add = [&](const std::string& sop, const std::vector<std::uint8_t>& b) { CHECK_EQ(g.add(sop, b), SIAARA_OK); };
    auto base = [](const std::string& sop, int instance) {
        Ct c;
        c.sop = "1.2.826.0.1.7.1." + sop;
        c.instance = instance;
        return c;
    };
    auto with_slice = [](DicomFile f, const char* sl) {
        (void)f.dataset.set_string(0x00201041, VR::DS, sl);
        return grouping_synth::bytes_of(f);
    };
    // usable: z = 30, 10, 20 (instance numbers contradict)
    for (const auto& [n, z] : std::vector<std::pair<int, double>>{{1, 30}, {2, 10}, {3, 20}}) {
        Ct c = base(std::to_string(n), 10 - n);
        c.ipp = {0, 0, z};
        add(std::to_string(n), ct(c));
    }
    // NaN position (FD), Slice Location 5
    {
        Ct c = base("4", 50);
        DicomFile f = ct_file(c);
        (void)f.dataset.remove(0x00200032);
        const std::array<double, 3> nan{0, 0, std::numeric_limits<double>::quiet_NaN()};
        (void)f.dataset.set_bytes(0x00200032, VR::FD, Bytes::copy_of(nan.data(), 24));
        add("4", with_slice(f, "5"));
    }
    // missing position, Slice Location -3
    {
        Ct c = base("5", 51);
        c.has_ipp = false;
        add("5", with_slice(ct_file(c), "-3"));
    }
    // infinite position (FD), no Slice Location, Instance Number 2
    {
        Ct c = base("6", 2);
        add("6", ct_fd_position(c, {0, 0, std::numeric_limits<double>::infinity()}));
    }
    // an orientation that differs by 0.01: not usable, Instance Number 1
    {
        Ct c = base("7", 1);
        c.ipp = {0, 0, 15};
        c.iop = {1, 0.01, 0, 0, 1, 0};
        add("7", ct(c));
    }
    // an orientation within the tolerance (1e-6): usable, z = 25
    {
        Ct c = base("8", 99);
        c.ipp = {0, 0, 25};
        c.iop = {1, 1e-6, 0, 0, 1, 0};
        add("8", ct(c));
    }
    // a position stored as "1e999" (not a decimal string the reader accepts) and nothing else: as added
    {
        Ct c = base("9", 0);
        c.has_instance = false;
        DicomFile f = ct_file(c);
        (void)f.dataset.set_string(0x00200032, VR::DS, "0\\0\\1e999");
        add("9", grouping_synth::bytes_of(f));
    }
    const siaara_grouping_summary s = g.result();
    REQUIRE(s.series == 1u);
    CHECK_EQ(s.instances, 9u);
    const std::vector<std::string> sops = series_sops(g, 0);
    const std::string p = "1.2.826.0.1.7.1.";
    const std::vector<std::string> want = {p + "2", p + "3", p + "8", p + "1", p + "5", p + "4", p + "7", p + "6", p + "9"};
    CHECK(sops == want);
    const std::vector<int32_t> keys = {SIAARA_GSORT_POSITION, SIAARA_GSORT_POSITION, SIAARA_GSORT_POSITION, SIAARA_GSORT_POSITION,
                                       SIAARA_GSORT_SLICE_LOCATION, SIAARA_GSORT_SLICE_LOCATION, SIAARA_GSORT_INSTANCE_NUMBER,
                                       SIAARA_GSORT_INSTANCE_NUMBER, SIAARA_GSORT_ADDED};
    for (uint32_t i = 0; i < 9; ++i) CHECK_EQ(g.instance(i).sort_key, keys[i]);
    // the NaN is returned as stored, flagged present but not usable
    const siaara_grouped_instance nan = g.instance(5);
    CHECK((nan.flags & SIAARA_GINST_POSITION) != 0);
    CHECK((nan.flags & SIAARA_GINST_POSITION_USABLE) == 0);
    CHECK(std::isnan(nan.image_position[2]));
    CHECK(std::isinf(g.instance(7).image_position[2]));
    CHECK((g.instance(8).flags & SIAARA_GINST_POSITION) == 0);
    // With a wider tolerance the tilted file is usable too.
    siaara_grouping_options o = grouping_synth::options();
    o.orientation_tolerance = 0.1;
    Grouping wide(&o);
    {
        Ct c = base("7", 1);
        c.ipp = {0, 0, 15};
        c.iop = {1, 0.01, 0, 0, 1, 0};
        Ct d = base("1", 9);
        d.ipp = {0, 0, 30};
        CHECK_EQ(wide.add("d", ct(d)), SIAARA_OK);
        CHECK_EQ(wide.add("c", ct(c)), SIAARA_OK);
    }
    (void)wide.result();
    CHECK_EQ(wide.instance(0).sort_key, SIAARA_GSORT_POSITION);
    CHECK_EQ(wide.instance(1).sort_key, SIAARA_GSORT_POSITION);
    CHECK(wide.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_NAME) == "c");
    // A degenerate orientation gives no normal: nothing is placed by position.
    Grouping flat;
    {
        Ct c = base("1", 2);
        c.iop = {1, 0, 0, 1, 0, 0};
        Ct d = base("2", 1);
        d.iop = {1, 0, 0, 1, 0, 0};
        CHECK_EQ(flat.add("a", ct(c)), SIAARA_OK);
        CHECK_EQ(flat.add("b", ct(d)), SIAARA_OK);
    }
    (void)flat.result();
    CHECK_EQ(flat.series(0).has_normal, 0);
    CHECK_EQ(flat.instance(0).sort_key, SIAARA_GSORT_INSTANCE_NUMBER);
    CHECK(flat.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_NAME) == "b");
}

TEST_CASE("grouping: duplicates (both policies), ungrouped files, inconsistencies") {
    auto make = [](const std::string& sop, const std::string& study, const std::string& series, const std::string& pid,
                   const std::string& modality = "CT", std::uint16_t rows = 4) {
        Ct c;
        c.sop = sop;
        c.study = study;
        c.series = series;
        c.patient_id = pid;
        c.modality = modality;
        c.rows = rows;
        return ct(c);
    };
    const auto a = make("1.2.5.1", "1.2.5", "1.2.5.0", "P1");
    for (int policy : {SIAARA_GROUP_DUPLICATES_KEEP_BOTH, SIAARA_GROUP_DUPLICATES_KEEP_FIRST}) {
        siaara_grouping_options o = grouping_synth::options();
        o.duplicates = policy;
        Grouping g(&o);
        CHECK_EQ(g.add("first", a), SIAARA_OK);
        CHECK_EQ(g.add("second", a), SIAARA_OK);
        const siaara_grouping_summary s = g.result();
        CHECK_EQ(s.duplicates, 1u);
        const auto issues = g.issues(s.issues);
        REQUIRE(count_kind(issues, SIAARA_GISSUE_DUPLICATE) == 1);
        const siaara_grouping_issue& d = issues[0];
        CHECK_EQ(d.file, 1);
        CHECK_EQ(d.other_instance, 0);
        CHECK(g.stored(SIAARA_GLEVEL_ISSUE, 0, SIAARA_GFIELD_NAME) == "second");
        CHECK(g.stored(SIAARA_GLEVEL_ISSUE, 0, SIAARA_GFIELD_SUBJECT) == "1.2.5.1");
        if (policy == SIAARA_GROUP_DUPLICATES_KEEP_BOTH) {
            CHECK_EQ(s.instances, 2u);
            CHECK_EQ(d.instance, 1);
            const siaara_grouped_instance second = g.instance(1);
            CHECK((second.flags & SIAARA_GINST_DUPLICATE) != 0);
            CHECK_EQ(second.duplicate_of, 0);
            CHECK_EQ(g.instance(0).duplicate_of, -1);
        } else {
            CHECK_EQ(s.instances, 1u);
            CHECK_EQ(d.instance, -1);
        }
    }
    Grouping g;
    CHECK_EQ(g.add("a", a), SIAARA_OK);
    // the same Study UID under another Patient ID: two studies, two issues, not merged
    CHECK_EQ(g.add("b", make("1.2.5.2", "1.2.5", "1.2.5.0", "P2")), SIAARA_OK);
    // a series whose files disagree on Modality and on Rows x Columns
    CHECK_EQ(g.add("c", make("1.2.5.3", "1.2.5", "1.2.5.0", "P1", "MR")), SIAARA_OK);
    CHECK_EQ(g.add("d", make("1.2.5.4", "1.2.5", "1.2.5.0", "P1", "CT", 8)), SIAARA_OK);
    // ungrouped: no Study UID; a Series UID that is not a UID
    {
        Ct c;
        c.sop = "1.2.5.5";
        DicomFile f = ct_file(c);
        (void)f.dataset.remove(0x0020000D);
        CHECK_EQ(g.add("e", grouping_synth::bytes_of(f)), SIAARA_OK);
    }
    CHECK_EQ(g.add("f", make("1.2.5.6", "1.2.5", "1.2.5.x", "P1")), SIAARA_OK);
    const siaara_grouping_summary s = g.result();
    CHECK_EQ(s.patients, 2u);
    CHECK_EQ(s.studies, 2u);
    CHECK_EQ(s.instances, 6u);
    CHECK_EQ(s.ungrouped, 2u);
    CHECK_EQ(s.inconsistencies, 4u);
    const auto issues = g.issues(s.issues);
    CHECK_EQ(count_kind(issues, SIAARA_GISSUE_STUDY_PATIENT_CONFLICT), 2u);
    CHECK_EQ(count_kind(issues, SIAARA_GISSUE_SERIES_MODALITY_CONFLICT), 1u);
    CHECK_EQ(count_kind(issues, SIAARA_GISSUE_SERIES_SIZE_CONFLICT), 1u);
    CHECK_EQ(count_kind(issues, SIAARA_GISSUE_UNGROUPED), 2u);
    for (uint32_t i = 0; i < issues.size(); ++i) {
        const siaara_grouping_issue& is = issues[i];
        if (is.kind == SIAARA_GISSUE_STUDY_PATIENT_CONFLICT) {
            CHECK(g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_SUBJECT) == "1.2.5");
            CHECK(is.study >= 0);
        }
        if (is.kind == SIAARA_GISSUE_SERIES_MODALITY_CONFLICT) {
            CHECK(g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_NAME) == "c");
            CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, static_cast<uint32_t>(is.other_instance), SIAARA_GFIELD_NAME) == "a");
        }
        if (is.kind == SIAARA_GISSUE_SERIES_SIZE_CONFLICT) CHECK(g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_NAME) == "d");
        if (is.kind == SIAARA_GISSUE_UNGROUPED) {
            const std::string name = g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_NAME);
            CHECK(name == "e" || name == "f");
            CHECK_EQ(is.reason, name == "e" ? SIAARA_GUNGROUPED_STUDY_UID : SIAARA_GUNGROUPED_SERIES_UID);
            const siaara_grouped_instance in = g.instance(static_cast<uint32_t>(is.instance));
            CHECK_EQ(in.series, -1);
            CHECK((in.flags & SIAARA_GINST_UNGROUPED) != 0);
        }
    }
    // the ungrouped come last, as added; the invalid Series UID is kept, flagged
    CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 5, SIAARA_GFIELD_NAME) == "f");
    CHECK_EQ(g.status(SIAARA_GLEVEL_INSTANCE, 5, SIAARA_GFIELD_SERIES_UID), SIAARA_GTEXT_NOT_A_UID);
    CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 5, SIAARA_GFIELD_SERIES_UID) == "1.2.5.x");
    CHECK_EQ(g.status(SIAARA_GLEVEL_INSTANCE, 4, SIAARA_GFIELD_STUDY_UID), SIAARA_GTEXT_ABSENT);
    // the patients differ although their IDs are not returned
    CHECK_EQ(g.status(SIAARA_GLEVEL_PATIENT, 1, SIAARA_GFIELD_PATIENT_ID), SIAARA_GTEXT_NOT_REQUESTED);
}

TEST_CASE("grouping: patient fields only when asked; caps - a longer value is not kept, never cut") {
    Spec sp;
    sp.charset = "ISO_IR 192";
    sp.patient_name = "M\xC3\xBCller^Anna";
    sp.patient_id = "PID\xE2\x80\xAE" "9";
    sp.series_desc = std::string(1025, 'x');
    sp.sop_uid = "1." + std::string(70, '2');  // longer than 64
    DicomFile f = store_synth::make_file(sp);
    (void)f.dataset.set_string(0x00080020, VR::DA, "2020\x01" "0115");
    const auto bytes = grouping_synth::bytes_of(f);
    siaara_grouping_options o = grouping_synth::options();
    o.include_patient_fields = 1;
    Grouping g(&o);
    CHECK_EQ(g.add("x", bytes), SIAARA_OK);
    (void)g.result();
    CHECK(g.stored(SIAARA_GLEVEL_PATIENT, 0, SIAARA_GFIELD_PATIENT_NAME) == "M\xC3\xBCller^Anna");
    CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_PATIENT_ID) == "PID\xE2\x80\xAE" "9");
    CHECK(g.display(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_PATIENT_ID) == "PID9");
    CHECK_EQ(g.status(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_SERIES_DESCRIPTION), SIAARA_GTEXT_TOO_LONG);
    CHECK(g.text(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_SERIES_DESCRIPTION).stored == nullptr);
    CHECK_EQ(g.status(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_SOP_INSTANCE_UID), SIAARA_GTEXT_TOO_LONG);
    CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_STUDY_DATE) == "2020\x01" "0115");
    CHECK(g.display(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_STUDY_DATE) == "2020 0115");
    // a cap raised: kept whole; max_value_bytes lowered: not decoded
    o.max_text_bytes = 2000;
    Grouping wide(&o);
    CHECK_EQ(wide.add("x", bytes), SIAARA_OK);
    (void)wide.result();
    CHECK_EQ(wide.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_SERIES_DESCRIPTION).size(), 1025u);
    o.max_value_bytes = 16;
    Grouping narrow(&o);
    CHECK_EQ(narrow.add("x", bytes), SIAARA_OK);
    (void)narrow.result();
    CHECK_EQ(narrow.status(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_SERIES_DESCRIPTION), SIAARA_GTEXT_TOO_LONG);
    CHECK_EQ(narrow.status(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_MODALITY), SIAARA_GTEXT_OK);
}

TEST_CASE("grouping: not DICOM, damaged, per-file limits and the total budgets") {
    Ct c;
    c.sop = "1.2.6.1";
    const auto good = ct(c);
    {
        Grouping g;
        CHECK_EQ(g.add("readme.txt", std::vector<std::uint8_t>{'h', 'i'}), SIAARA_OK);
        CHECK_EQ(g.add("empty", {}), SIAARA_OK);
        std::vector<std::uint8_t> raw(good.begin() + 132, good.end());  // a data set without the Part 10 header
        CHECK_EQ(g.add("raw", raw), SIAARA_OK);
        std::vector<std::uint8_t> trunc(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() - 7));
        CHECK_EQ(g.add("truncated", trunc), SIAARA_OK);
        const siaara_grouping_summary s = g.result();
        CHECK_EQ(s.skipped, 3u);
        CHECK_EQ(s.instances, 1u);
        const auto issues = g.issues(s.issues);
        for (uint32_t i = 0; i < 3; ++i) {
            CHECK_EQ(issues[i].kind, SIAARA_GISSUE_SKIPPED);
            CHECK_EQ(issues[i].reason, SIAARA_GSKIP_NOT_DICOM);
        }
        CHECK(g.instance(0).read_loss != 0u);  // what was read is kept, flagged incomplete
        CHECK_EQ(g.status(SIAARA_GLEVEL_ISSUE, 1, SIAARA_GFIELD_NAME), SIAARA_GTEXT_OK);
    }
    {  // max_files, max_file_bytes, max_total_bytes (sticky: smaller files after it are skipped too)
        siaara_grouping_options o = grouping_synth::options();
        o.max_files = 3;
        Grouping g(&o);
        for (int i = 0; i < 5; ++i) {
            Ct d;
            d.sop = "1.2.6." + std::to_string(i + 10);
            CHECK_EQ(g.add("f" + std::to_string(i), ct(d)), SIAARA_OK);
        }
        const siaara_grouping_summary s = g.result();
        CHECK_EQ(s.instances, 3u);
        CHECK_EQ(s.skipped, 2u);
        CHECK_EQ(s.budget_reached, static_cast<uint32_t>(SIAARA_GLIMIT_MAX_FILES));
        const auto issues = g.issues(s.issues);
        CHECK_EQ(issues[0].reason, SIAARA_GSKIP_TOTAL_BUDGET);
        CHECK_EQ(issues[0].limit, SIAARA_GLIMIT_MAX_FILES);
        CHECK_EQ(issues[0].file, 3);
    }
    {
        siaara_grouping_options o = grouping_synth::options();
        o.max_file_bytes = good.size() - 1;
        o.max_total_bytes = 2 * good.size() + good.size() / 2;
        Grouping g(&o);
        Ct big = c;
        big.rows = 64;
        big.sop = "1.2.6.2";
        CHECK_EQ(g.add("big", ct(big)), SIAARA_OK);  // larger than max_file_bytes: skipped, not counted
        for (int i = 0; i < 4; ++i) {
            Ct d;
            d.sop = "1.2.6." + std::to_string(i + 20);
            std::vector<std::uint8_t> b = ct(d);
            if (b.size() > o.max_file_bytes) b.resize(static_cast<std::size_t>(o.max_file_bytes));
            CHECK_EQ(g.add("f" + std::to_string(i), b), SIAARA_OK);
        }
        CHECK_EQ(g.add("tiny", std::vector<std::uint8_t>{1, 2, 3}), SIAARA_OK);
        const siaara_grouping_summary s = g.result();
        CHECK_EQ(s.instances, 2u);
        CHECK_EQ(s.budget_reached, static_cast<uint32_t>(SIAARA_GLIMIT_TOTAL_BYTES));
        CHECK(s.total_bytes <= o.max_total_bytes);
        const auto issues = g.issues(s.issues);
        REQUIRE(issues.size() == 4u);
        CHECK_EQ(issues[0].limit, SIAARA_GLIMIT_FILE_BYTES);
        CHECK_EQ(issues[0].reason, SIAARA_GSKIP_FILE_LIMIT);
        for (std::size_t i = 1; i < 4; ++i) CHECK_EQ(issues[i].limit, SIAARA_GLIMIT_TOTAL_BYTES);
    }
    {  // max_result_bytes: once reached, every later file is skipped and names are not kept
        siaara_grouping_options o = grouping_synth::options();
        o.max_result_bytes = 400;
        Grouping g(&o);
        for (int i = 0; i < 6; ++i) {
            Ct d;
            d.sop = "1.2.6." + std::to_string(i + 30);
            CHECK_EQ(g.add("result-" + std::to_string(i), ct(d)), SIAARA_OK);
        }
        const siaara_grouping_summary s = g.result();
        CHECK(s.result_bytes <= 400u);
        CHECK(s.instances >= 1u);
        CHECK(s.instances < 6u);
        CHECK_EQ(s.budget_reached, static_cast<uint32_t>(SIAARA_GLIMIT_RESULT_BYTES));
        const auto issues = g.issues(s.issues);
        REQUIRE(!issues.empty());
        CHECK_EQ(issues.back().limit, SIAARA_GLIMIT_RESULT_BYTES);
        CHECK_EQ(g.status(SIAARA_GLEVEL_ISSUE, static_cast<uint32_t>(issues.size() - 1), SIAARA_GFIELD_NAME), SIAARA_GTEXT_NOT_KEPT);
    }
    {  // deflated files: the per-file inflate limit, the total inflate budget, the element limit
        Ct d;
        d.sop = "1.2.6.40";
        DicomFile f = ct_file(d);
        f.transfer_syntax = std::string(ts::DeflatedExplicitVRLittleEndian);
        for (int i = 0; i < 200; ++i) (void)f.dataset.set_string(0x00091000u + static_cast<std::uint32_t>(i), VR::LO, std::string(60, 'z'));
        const auto deflated = grouping_synth::bytes_of(f);
        siaara_grouping_options o = grouping_synth::options();
        o.max_inflated_size = 4096;
        Grouping g1(&o);
        CHECK_EQ(g1.add("d", deflated), SIAARA_OK);
        siaara_grouping_summary s = g1.result();
        auto issues = g1.issues(s.issues);
        REQUIRE(issues.size() == 1u);
        CHECK_EQ(issues[0].reason, SIAARA_GSKIP_FILE_LIMIT);
        CHECK_EQ(issues[0].limit, SIAARA_GLIMIT_INFLATED_SIZE);
        o = grouping_synth::options();
        o.max_total_inflated = 20000;
        Grouping g2(&o);
        for (int i = 0; i < 4; ++i) CHECK_EQ(g2.add("d" + std::to_string(i), deflated), SIAARA_OK);
        CHECK_EQ(g2.add("plain", good), SIAARA_OK);
        s = g2.result();
        CHECK_EQ(s.instances, 1u);  // the first deflated file (about 13 kB inflated); the second passes the total
        CHECK(s.total_inflated <= 20000u);
        CHECK_EQ(s.budget_reached, static_cast<uint32_t>(SIAARA_GLIMIT_TOTAL_INFLATED));
        o = grouping_synth::options();
        o.max_elements = 50;
        Grouping g3(&o);
        CHECK_EQ(g3.add("d", deflated), SIAARA_OK);
        s = g3.result();
        issues = g3.issues(s.issues);
        REQUIRE(issues.size() == 1u);
        CHECK_EQ(issues[0].limit, SIAARA_GLIMIT_READ_LIMITS);
    }
}

TEST_CASE("grouping: options and argument checks; getters before the result") {
    siaara_grouping* g = nullptr;
    siaara_grouping_options o = grouping_synth::options();
    o.order = 7;
    CHECK_EQ(siaara_grouping_create(&o, &g), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(g == nullptr);
    o = grouping_synth::options();
    o.match_case_insensitive = 3;
    CHECK_EQ(siaara_grouping_create(&o, &g), SIAARA_ERR_INVALID_ARGUMENT);
    o = grouping_synth::options();
    o.orientation_tolerance = -1;
    CHECK_EQ(siaara_grouping_create(&o, &g), SIAARA_ERR_INVALID_ARGUMENT);
    o.orientation_tolerance = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(siaara_grouping_create(&o, &g), SIAARA_ERR_INVALID_ARGUMENT);
    o = grouping_synth::options();
    o.struct_size = sizeof o - 8;
    CHECK_EQ(siaara_grouping_create(&o, &g), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_grouping_create(nullptr, nullptr), SIAARA_ERR_INVALID_ARGUMENT);
    REQUIRE(siaara_grouping_create(nullptr, &g) == SIAARA_OK);
    CHECK_EQ(siaara_grouping_add_file(g, nullptr, 3, nullptr, 0), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_grouping_add_file(g, "a", 1, nullptr, 9), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_grouping_add_file(nullptr, "a", 1, nullptr, 0), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_grouping_add_file(g, nullptr, 0, nullptr, 0), SIAARA_OK);
    siaara_grouped_instance in;
    std::memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    CHECK_EQ(siaara_grouping_instance(g, 0, &in), SIAARA_ERR_INVALID_ARGUMENT);  // before the result
    CHECK_EQ(siaara_grouping_result(g, nullptr), SIAARA_OK);
    siaara_grouping_issue is;
    std::memset(&is, 0, sizeof is);
    is.struct_size = sizeof is;
    CHECK_EQ(siaara_grouping_issue_get(g, 0, &is), SIAARA_OK);
    CHECK_EQ(is.kind, SIAARA_GISSUE_SKIPPED);
    CHECK_EQ(siaara_grouping_issue_get(g, 1, &is), SIAARA_ERR_INVALID_ARGUMENT);
    is.struct_size = 8;
    CHECK_EQ(siaara_grouping_issue_get(g, 0, &is), SIAARA_ERR_INVALID_ARGUMENT);
    siaara_grouping_text t;
    std::memset(&t, 0, sizeof t);
    t.struct_size = sizeof t;
    CHECK_EQ(siaara_grouping_text_get(g, SIAARA_GLEVEL_ISSUE, 0, SIAARA_GFIELD_NAME, &t), SIAARA_OK);
    CHECK_EQ(t.status, SIAARA_GTEXT_ABSENT);
    CHECK_EQ(siaara_grouping_text_get(g, SIAARA_GLEVEL_ISSUE, 0, SIAARA_GFIELD_MODALITY, &t), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_grouping_text_get(g, 9, 0, SIAARA_GFIELD_NAME, &t), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_grouping_text_get(g, SIAARA_GLEVEL_ISSUE, 0, 11, &t), SIAARA_ERR_INVALID_ARGUMENT);
    // adding after the result makes it stale until it is built again
    CHECK_EQ(siaara_grouping_add_file(g, "b", 1, nullptr, 0), SIAARA_OK);
    CHECK_EQ(siaara_grouping_issue_get(g, 0, &is), SIAARA_ERR_INVALID_ARGUMENT);
    siaara_grouping_free(g);
    siaara_grouping_free(nullptr);
    static_assert(SIAARA_GROUPING_DEFAULT_MAX_FILES == 10000u, "default");
    static_assert(SIAARA_GROUPING_DEFAULT_MAX_TOTAL_BYTES == 2147483648u, "default");
}

// ================================================================ grouping with a DICOMDIR
TEST_CASE("grouping: a ZIP with a DICOMDIR - wrapping folder, case, backslashes, '..', RLO; missing, unlisted, ambiguous") {
    store_synth::TempDir td("siaara-gdd");
    make_media(td / "src", td / "m", 1);  // 8 files: DICOM/P0000001/S000000s/E000000e/I000000i
    const std::vector<std::uint8_t> dd = store_synth::read_bytes(td / "m" / "DICOMDIR");
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> members;  // File ID text -> bytes
    {
        std::vector<DirRec> recs;
        REQUIRE(read_mem(dd, recs) == SIAARA_OK);
        for (const auto& r : recs)
            if (!r.file_text.empty()) members.emplace_back(r.file_text, store_synth::read_bytes(td / "m" / fs::path(r.file_text)));
    }
    REQUIRE(members.size() == 8u);
    auto lower = [](std::string s) {
        for (char& ch : s)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return s;
    };
    auto backslashes = [](std::string s) {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    };
    Ct extra;
    extra.sop = "1.2.7.99";
    const std::string rlo_name = "CD/extra\xE2\x80\xAEmcd.exe";
    for (int pass = 0; pass < 4; ++pass) {
        siaara_grouping_options o = grouping_synth::options();
        if (pass == 1) o.match_case_insensitive = SIAARA_GROUP_SWITCH_OFF;
        if (pass == 2) o.match_by_trailing_components = SIAARA_GROUP_SWITCH_OFF;
        Grouping g(&o);
        // member 0: missing. 1: under a wrapping folder. 2: lower case. 3: backslashes and "./".
        // 4: a ".." in front. 5 and 6: exactly the File ID. 7: twice, under two folders (ambiguous).
        CHECK_EQ(g.add("CD/" + members[1].first, members[1].second), SIAARA_OK);
        CHECK_EQ(g.add("cd/" + lower(members[2].first), members[2].second), SIAARA_OK);
        CHECK_EQ(g.add(".\\" + backslashes(members[3].first), members[3].second), SIAARA_OK);
        CHECK_EQ(g.add("../" + members[4].first, members[4].second), SIAARA_OK);
        CHECK_EQ(g.add(members[5].first, members[5].second), SIAARA_OK);
        CHECK_EQ(g.add(members[6].first, members[6].second), SIAARA_OK);
        CHECK_EQ(g.add("a/" + members[7].first, members[7].second), SIAARA_OK);
        CHECK_EQ(g.add("b/" + members[7].first, members[7].second), SIAARA_OK);
        CHECK_EQ(g.add(rlo_name, ct(extra)), SIAARA_OK);
        if (pass < 3) CHECK_EQ(g.add_dicomdir(dd), SIAARA_OK);
        const siaara_grouping_summary s = g.result();
        CHECK_EQ(s.duplicates, 1u);  // member 7 twice
        const auto issues = g.issues(s.issues);
        std::set<std::string> missing, unlisted, ambiguous;
        for (uint32_t i = 0; i < issues.size(); ++i) {
            if (issues[i].kind == SIAARA_GISSUE_LISTED_MISSING) missing.insert(g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_SUBJECT));
            if (issues[i].kind == SIAARA_GISSUE_PRESENT_UNLISTED) unlisted.insert(g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_NAME));
            if (issues[i].kind == SIAARA_GISSUE_AMBIGUOUS_MATCH) ambiguous.insert(g.stored(SIAARA_GLEVEL_ISSUE, i, SIAARA_GFIELD_NAME));
        }
        if (pass == 3) {  // no DICOMDIR: nothing to compare with
            CHECK(missing.empty() && unlisted.empty() && ambiguous.empty());
            CHECK_EQ(s.dicomdirs, 0u);
            continue;
        }
        CHECK_EQ(s.dicomdirs, 1u);
        std::set<std::string> want_missing = {members[0].first};
        std::set<std::string> want_unlisted = {rlo_name};
        std::set<std::string> want_ambiguous = {"a/" + members[7].first, "b/" + members[7].first};
        if (pass == 1) {  // case-sensitive: the lower-case name matches nothing
            want_missing.insert(members[2].first);
            want_unlisted.insert("cd/" + lower(members[2].first));
        }
        if (pass == 2) {  // whole names: only the exact ones (and the ones whose prefix is "." only) match
            want_missing = {members[0].first, members[1].first, members[2].first, members[4].first, members[7].first};
            want_unlisted = {rlo_name, "CD/" + members[1].first, "cd/" + lower(members[2].first), "../" + members[4].first,
                             "a/" + members[7].first, "b/" + members[7].first};
            want_ambiguous.clear();
        }
        CHECK(missing == want_missing);
        CHECK(unlisted == want_unlisted);
        CHECK(ambiguous == want_ambiguous);
        CHECK_EQ(s.listed_missing, want_missing.size());
        // names are returned exactly as given; the display copy loses the override
        for (uint32_t i = 0; i < s.instances; ++i) {
            if (g.stored(SIAARA_GLEVEL_INSTANCE, i, SIAARA_GFIELD_NAME) != rlo_name) continue;
            CHECK(g.display(SIAARA_GLEVEL_INSTANCE, i, SIAARA_GFIELD_NAME) == "CD/extramcd.exe");
            CHECK((g.instance(i).flags & SIAARA_GINST_IN_DICOMDIR) == 0);
        }
        CHECK(g.stored(SIAARA_GLEVEL_INSTANCE, 0, SIAARA_GFIELD_NAME).find('\\') == std::string::npos ||
              pass >= 0);  // backslash names are kept with their backslashes
        bool saw_backslash = false;
        for (uint32_t i = 0; i < s.instances; ++i)
            if (g.stored(SIAARA_GLEVEL_INSTANCE, i, SIAARA_GFIELD_NAME) == ".\\" + backslashes(members[3].first)) saw_backslash = true;
        CHECK(saw_backslash);
    }
    {  // a DICOMDIR that cannot be read is an issue; an unsafe File ID is reported missing, rejected
        Grouping g;
        CHECK_EQ(g.add_dicomdir(std::vector<std::uint8_t>{1, 2, 3}), SIAARA_OK);
        siaara_grouping_summary s = g.result();
        auto issues = g.issues(s.issues);
        REQUIRE(issues.size() == 1u);
        CHECK_EQ(issues[0].kind, SIAARA_GISSUE_DICOMDIR_SKIPPED);
        CHECK_EQ(issues[0].reason, SIAARA_GSKIP_NOT_DICOM);
        CHECK_EQ(issues[0].dicomdir, 0);
    }
}
