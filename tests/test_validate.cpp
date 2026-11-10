// Validation tests: value rules, generated IOD tables, the validator on deliberately broken files,
// and the privacy rule for reports.
#include <algorithm>
#include <string>

#include "core/types.hpp"
#include "crosscheck/repair_samples.hpp"
#include "repair/iod.hpp"
#include "repair/report.hpp"
#include "repair/scan.hpp"
#include "repair/validate.hpp"
#include "repair/values.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::repair;

namespace {

std::vector<Finding> findings_of(const std::vector<std::uint8_t>& bytes) {
    auto r = validate_bytes(Bytes::copy_of(bytes.data(), bytes.size()));
    if (!r) return {};
    return *r;
}

bool has_code(const std::vector<Finding>& f, std::string_view code) {
    return std::any_of(f.begin(), f.end(), [&](const Finding& x) { return x.code == code; });
}

std::size_t errors(const std::vector<Finding>& f) { return count(f, Severity::Error); }

}  // namespace

TEST_CASE("repair: value rules (PS3.5 6.2)") {
    CHECK(check_value(VR::DA, "20240229") == ValueCheck::Ok);
    CHECK(check_value(VR::DA, "20230229") == ValueCheck::BadDate);
    CHECK(check_value(VR::DA, "2024.01.02") != ValueCheck::Ok);
    CHECK(check_value(VR::DA, "2024010") == ValueCheck::BadFormat);
    CHECK(check_value(VR::TM, "235960.123456") == ValueCheck::Ok);
    CHECK(check_value(VR::TM, "2460") == ValueCheck::BadTime);
    CHECK(check_value(VR::TM, "10:15") == ValueCheck::BadCharacter);
    CHECK(check_value(VR::DT, "20240102101500.5+0530") == ValueCheck::Ok);
    CHECK(check_value(VR::DT, "202413") == ValueCheck::BadDate);
    CHECK(check_value(VR::DS, "-1.5e-3") == ValueCheck::Ok);
    CHECK(check_value(VR::DS, "1,5") == ValueCheck::BadCharacter);
    CHECK(check_value(VR::DS, "0.333333333333333314") == ValueCheck::TooLong);
    CHECK(check_value(VR::IS, "+7") == ValueCheck::Ok);
    CHECK(check_value(VR::IS, "2147483648") == ValueCheck::BadFormat);
    CHECK(check_value(VR::AS, "045Y") == ValueCheck::Ok);
    CHECK(check_value(VR::AS, "045y") == ValueCheck::BadFormat);
    CHECK(check_value(VR::CS, "ORIGINAL") == ValueCheck::Ok);
    CHECK(check_value(VR::CS, "original") == ValueCheck::BadCharacter);
    CHECK(check_value(VR::AE, "STORE\\SCP") == ValueCheck::BadCharacter);
    CHECK(check_value(VR::SH, "12345678901234567") == ValueCheck::TooLong);
    CHECK(check_value(VR::LO, "\xC3\xBC") == ValueCheck::Ok);  // characters, not bytes
    CHECK(check_value(VR::PN, "a^b^c^d^e^f") == ValueCheck::BadFormat);

    CHECK(uid_is_valid("1.2.840.10008.1.2"));
    CHECK(uid_is_valid("2.25.329800735698586629295641978511506172918"));
    CHECK(!uid_is_valid("1.2.03.4"));
    CHECK(!uid_is_valid("1..2"));
    CHECK(!uid_is_valid("9.8.7.6"));      // first arc 0..2
    CHECK(!uid_is_valid("1.40.3"));       // second arc under 1 is at most 39
    CHECK(!uid_is_valid(std::string(65, '1')));

    CHECK(vm_allows("1", 1) && !vm_allows("1", 2));
    CHECK(vm_allows("1-n", 5) && !vm_allows("1-n", 0));
    CHECK(vm_allows("2-2n", 4) && !vm_allows("2-2n", 3));
    CHECK(vm_allows("1-3", 3) && !vm_allows("1-3", 4));
    CHECK(vm_allows("6", 6) && !vm_allows("6", 5));
}

TEST_CASE("repair: unambiguous normalisation only") {
    CHECK(normalise_value(VR::DA, "2024.01.02") == std::optional<std::string>("20240102"));
    CHECK(normalise_value(VR::DA, "2024-01-02") == std::optional<std::string>("20240102"));
    CHECK(!normalise_value(VR::DA, "01/02/2024"));   // day / month order unknown
    CHECK(!normalise_value(VR::DA, "2024.02.30"));   // not a date
    CHECK(normalise_value(VR::TM, "10:15:00") == std::optional<std::string>("101500"));
    CHECK(normalise_value(VR::TM, "10:15") == std::optional<std::string>("1015"));
    CHECK(normalise_value(VR::IS, "+007") == std::optional<std::string>("7"));
    CHECK(normalise_value(VR::IS, "12.0") == std::optional<std::string>("12"));
    CHECK(!normalise_value(VR::IS, "12.5"));
    CHECK(normalise_value(VR::DS, "1.25000000000000000") == std::optional<std::string>("1.25"));
    CHECK(!normalise_value(VR::DS, "0.333333333333333314829616256247"));  // shortest form needs 18 characters
    CHECK(!normalise_value(VR::DS, "1,5"));
    CHECK(normalise_value(VR::AS, "045y") == std::optional<std::string>("045Y"));
    CHECK(normalise_value(VR::CS, "ot") == std::optional<std::string>("OT"));
    CHECK(normalise_value(VR::UI, "1.2.3 ") == std::optional<std::string>("1.2.3"));
    CHECK(!normalise_value(VR::UI, "1.2.03"));

    const std::string a = uid_from_seed("x");
    CHECK(a == uid_from_seed("x"));
    CHECK(a != uid_from_seed("y"));
    CHECK(a.rfind("2.25.", 0) == 0);
    CHECK(uid_is_valid(a));
}

TEST_CASE("repair: generated IOD tables (PS3.3 / PS3.4)") {
    const IodTableStats st = iod_table_stats();
    CHECK(st.attribute_tables > 500);
    CHECK(st.attribute_rows > 8000);
    CHECK(st.iods > 150);
    CHECK(st.sop_classes > 150);
    CHECK(st.conditions > 200);
    CHECK(st.enum_lists > 250);
    const IodReq* ct = iod_for_sop_class("1.2.840.10008.5.1.4.1.1.2");
    REQUIRE(ct != nullptr);
    CHECK(ct->name == "CT Image");
    bool patient = false, ct_image = false;
    for (const ModuleReq& m : ct->modules) {
        if (m.name == "Patient" && m.usage == ModuleUsage::M) {
            patient = true;
            REQUIRE(m.attributes != nullptr);
            for (const ReqNode& n : *m.attributes)
                if (n.tag == tags::PatientName) CHECK(n.type == AttrType::T2);
        }
        if (m.name == "CT Image") ct_image = true;
    }
    CHECK(patient && ct_image);
    // Enumerated Values: Pixel Representation in Image Pixel has 0000H / 0001H.
    const auto* ip = module_attributes("C.7-11c");
    if (ip) {
        for (const ReqNode& n : *ip)
            if (n.tag == tags::PixelRepresentation) CHECK(n.enumerated.size() == 2);
    }
    CHECK(iod_for_sop_class("1.2.3.4") == nullptr);
}

TEST_CASE("repair: SR content items honour conditional includes") {
    // A NUM content item without Measured Value Sequence is an error; a TEXT item is not asked
    // for it (the Numeric Measurement Macro is included only if Value Type is NUM).
    auto make = [](const char* value_type) {
        Dataset ds = repair_samples::base_sc();
        (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.88.11");  // Basic Text SR
        (void)ds.set_string(0x0040A040, VR::CS, "CONTAINER");
        Dataset item;
        (void)item.set_string(0x0040A010, VR::CS, "CONTAINS");
        (void)item.set_string(0x0040A040, VR::CS, value_type);
        Element sq;
        sq.tag = 0x0040A730;
        sq.vr = VR::SQ;
        sq.items.push_back(item);
        ds.put(std::move(sq));
        return repair_samples::part10(ds);
    };
    auto num = findings_of(make("NUM"));
    auto text = findings_of(make("TEXT"));
    auto missing_mvs = [](const std::vector<Finding>& f) {
        return std::any_of(f.begin(), f.end(), [](const Finding& x) {
            return x.code == "missing-type2" && x.path.size() == 2 && x.path.back().tag == 0x0040A300;
        });
    };
    CHECK(missing_mvs(num));
    CHECK(!missing_mvs(text));
}

TEST_CASE("repair: the valid sample has no errors") {
    const auto f = findings_of(repair_samples::part10(repair_samples::base_sc()));
    for (const auto& x : f)
        if (x.severity == Severity::Error) std::fprintf(stderr, "    unexpected: %s %s\n", x.code.c_str(), x.message.c_str());
    CHECK(errors(f) == 0);
}

TEST_CASE("repair: validator detects every broken sample") {
    for (const auto& s : repair_samples::all_samples()) {
        auto r = validate_bytes(Bytes::copy_of(s.bytes.data(), s.bytes.size()));
        REQUIRE(r.is_ok());
        for (const std::string& code : s.expect) {
            const bool found = has_code(*r, code);
            if (!found) std::fprintf(stderr, "    %s: expected finding %s\n", s.name.c_str(), code.c_str());
            CHECK(found);
        }
        if (s.expect.empty()) CHECK(errors(*r) == 0);
    }
}

TEST_CASE("repair: structural findings carry byte offsets") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name != "struct_odd_and_group_length") continue;
        auto r = validate_bytes(Bytes::copy_of(s.bytes.data(), s.bytes.size()));
        REQUIRE(r.is_ok());
        for (const Finding& f : *r)
            if (f.code == "odd-length") {
                CHECK(f.has_offset);
                CHECK(f.offset > 132);
                CHECK(f.offset < s.bytes.size());
            }
    }
}

TEST_CASE("repair: reports never contain data set values unless detailed") {
    std::vector<std::uint8_t> bytes = repair_samples::part10(repair_samples::base_sc());
    Report rep;
    FileReport fr;
    auto f = validate_bytes(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(f.is_ok());
    fr.findings = *f;
    Change c;
    c.category = kStudy;
    c.action = "set";
    c.path = {{tags::PatientName, -1}};
    c.values_withheld = true;
    fr.changes.push_back(c);
    rep.files.push_back(fr);
    const std::string json = to_json(rep);
    const std::string text = to_text(rep);
    CHECK(json.find(repair_samples::kPatientName) == std::string::npos);
    CHECK(text.find(repair_samples::kPatientName) == std::string::npos);
    CHECK(json.find("\"contains_phi\":false") != std::string::npos);
    rep.detailed = true;
    CHECK(to_json(rep).find("\"contains_phi\":true") != std::string::npos);
    CHECK(to_text(rep).find("PATIENT DATA") != std::string::npos);
    // Loggable: image attributes yes; names, IDs, dates, UIDs listed in PS3.15 E.1-1 no.
    CHECK(value_is_loggable({}, tags::Rows, VR::US));
    CHECK(value_is_loggable({}, tags::PhotometricInterpretation, VR::CS));
    CHECK(!value_is_loggable({}, tags::PatientName, VR::PN));
    CHECK(!value_is_loggable({}, tags::PatientID, VR::LO));
    CHECK(!value_is_loggable({}, tags::SOPInstanceUID, VR::UI));
    CHECK(!value_is_loggable({}, 0x00080020, VR::DA));
    CHECK(!value_is_loggable({}, 0x00091010, VR::LO));  // private
}

TEST_CASE("repair: JSON report shape") {
    const auto s = repair_samples::all_samples();
    Report rep;
    for (const auto& x : s) {
        FileReport fr;
        auto f = validate_bytes(Bytes::copy_of(x.bytes.data(), x.bytes.size()));
        if (f) fr.findings = *f;
        rep.files.push_back(std::move(fr));
    }
    const std::string j = to_json(rep);
    CHECK(j.front() == '{' && j.back() == '}');
    CHECK(j.find("\"severity\":\"Error\"") != std::string::npos);
    CHECK(j.find("\"study_findings\":[]") != std::string::npos);
    // Balanced braces / brackets outside strings.
    int depth = 0;
    bool in_str = false, esc = false, ok = true;
    for (char ch : j) {
        if (in_str) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '{' || ch == '[') ++depth;
        else if (ch == '}' || ch == ']') ok = ok && --depth >= 0;
    }
    CHECK(ok && depth == 0 && !in_str);
}

// ---------------------------------------------------------------- Type 1C / 2C conditions (testing 10b item 4)

namespace {

bool finding_at(const std::vector<Finding>& f, std::string_view code, Tag tag) {
    return std::any_of(f.begin(), f.end(), [&](const Finding& x) {
        return x.code == code && x.path.size() == 1 && x.path.back().tag == tag;
    });
}

Dataset rgb_sc(bool planar) {
    Dataset ds = repair_samples::base_sc();
    (void)ds.set_uint16(0x00280002, 3);
    (void)ds.set_string(0x00280004, VR::CS, "RGB");
    if (planar) (void)ds.set_uint16(0x00280006, 0);
    std::vector<std::uint8_t> px(8 * 8 * 3, 0x40);
    (void)ds.set_bytes(0x7FE00010, VR::OB, Bytes::copy_of(px.data(), px.size()));
    return ds;
}

}  // namespace

TEST_CASE("repair: 1C condition with Greater (Planar Configuration, Samples per Pixel > 1)") {
    CHECK(finding_at(findings_of(repair_samples::part10(rgb_sc(false))), "missing-type1", 0x00280006));
    const auto ok = findings_of(repair_samples::part10(rgb_sc(true)));
    CHECK(!finding_at(ok, "missing-type1", 0x00280006));
    CHECK(!finding_at(ok, "present-condition-unmet", 0x00280006));
    // Present although Samples per Pixel is 1: PS3.5 7.4.4, the description has no "may be
    // present otherwise".
    Dataset mono = repair_samples::base_sc();
    (void)mono.set_uint16(0x00280006, 0);
    CHECK(finding_at(findings_of(repair_samples::part10(mono)), "present-condition-unmet", 0x00280006));
    CHECK(!finding_at(findings_of(repair_samples::part10(repair_samples::base_sc())), "present-condition-unmet",
                      0x00280006));
}

TEST_CASE("repair: 1C condition with OR of Equals terms (palette descriptors for PALETTE COLOR)") {
    Dataset ds = repair_samples::base_sc();
    (void)ds.set_string(0x00280004, VR::CS, "PALETTE COLOR");
    const auto f = findings_of(repair_samples::part10(ds));
    CHECK(finding_at(f, "missing-type1", 0x00281101));
    CHECK(finding_at(f, "missing-type1", 0x00281203));
    CHECK(!finding_at(findings_of(repair_samples::part10(repair_samples::base_sc())), "missing-type1", 0x00281101));
}

TEST_CASE("repair: 1C condition IncludesTag (Frame Increment Pointer lists Time Slot Vector)") {
    auto nm = [](std::uint16_t element) {
        Dataset ds = repair_samples::base_sc("1.2.826.0.1.3680043.10.1408.1.90", 8, 8, 8, 2);
        (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.20");  // NM Image
        (void)ds.set_string(0x00080060, VR::CS, "NM");
        const std::uint8_t at[4] = {0x54, 0x00, static_cast<std::uint8_t>(element & 0xFF),
                                    static_cast<std::uint8_t>(element >> 8)};
        (void)ds.set_bytes(0x00280009, VR::AT, Bytes::copy_of(at, sizeof at));
        return repair_samples::part10(ds);
    };
    CHECK(finding_at(findings_of(nm(0x0070)), "missing-type1", 0x00540070));   // Time Slot Vector
    CHECK(!finding_at(findings_of(nm(0x0010)), "missing-type1", 0x00540070));  // Energy Window Vector
}

TEST_CASE("repair: 2C structured rules (Laterality of a paired body part, Patient Orientation)") {
    Dataset knee = repair_samples::base_sc();
    (void)knee.set_string(0x00180015, VR::CS, "KNEE");
    knee.remove(0x00200020);
    knee.remove(0x00200060);
    const auto f = findings_of(repair_samples::part10(knee));
    CHECK(finding_at(f, "missing-type2", 0x00200060));  // paired structure, no laterality anywhere
    CHECK(finding_at(f, "missing-type2", 0x00200020));  // no Image Orientation / Position: required
    Dataset with = knee;
    (void)with.set_string(0x00200062, VR::CS, "L");
    (void)with.set_string(0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
    (void)with.set_string(0x00200032, VR::DS, "0\\0\\0");
    const auto g = findings_of(repair_samples::part10(with));
    CHECK(!finding_at(g, "missing-type2", 0x00200060));
    CHECK(!finding_at(g, "missing-type2", 0x00200020));
    Dataset chest = knee;
    (void)chest.set_string(0x00180015, VR::CS, "CHEST");  // not a paired structure
    CHECK(!finding_at(findings_of(repair_samples::part10(chest)), "missing-type2", 0x00200060));
}

TEST_CASE("repair: conditions that are prose stay 'not evaluated' (Info, never an error)") {
    const auto f = findings_of(repair_samples::part10(repair_samples::base_sc()));
    for (const Finding& x : f)
        if (x.code == "condition-not-evaluated") CHECK(x.severity == Severity::Info);
    CHECK(errors(f) == 0);
    CHECK(iod_table_stats().conditions > 500);
}
