// Clinical layer: PET SUV (src/clinical/suv.*) and the DA / TM / DT parsing it relies on.
//
// Worked example used below (every number shown; computed by hand / independently in Python):
//   Patient's Weight 75 kg, Patient's Size 1.80 m, Patient's Sex M
//   Radionuclide Total Dose 370 MBq = 370 000 000 Bq, Radionuclide Half Life 6586.2 s (F-18)
//   Radiopharmaceutical Start DateTime 20260301093000, Series Date/Time 20260301 / 103000
//   Units BQML, Decay Correction START, Corrected Image ATTN\DECY
//   decay time t = 10:30:00 - 09:30:00 = 3600 s
//   t / T = 3600 / 6586.2 = 0.546597430992074
//   decay fraction = 2^-0.546597430992074 = 0.684632919572002
//   decayed dose = 370e6 x 0.684632919572002 = 253 314 180.241641 Bq
//   SUVbw factor = 75 000 g / 253 314 180.241641 Bq = 2.96075016126046e-4 g/ml per Bq/ml
//   LBM James (male) = 1.10 x 75 - 128 x (75 / 180)^2 = 82.5 - 22.2222 = 60.2777777777778 kg
//     SUVlbm factor = 60 277.7777777778 / 253 314 180.241641 = 2.37956587034637e-4
//   BMI = 75 / 1.8^2 = 23.1481481481481; Janmahasatian (male) = 9270 x 75 / (6680 + 216 x BMI)
//     = 695 250 / 11 680 = 59.5248287671233 kg; factor 2.34984195161682e-4
//   BSA Du Bois = 0.007184 x 75^0.425 x 180^0.725 = 1.94240562380594 m2;
//     SUVbsa factor = 19 424.0562380594 cm2 / 253 314 180.241641 = 7.66797035188891e-5
//   IBW Devine (male) = 50 + 2.3 x (180 / 2.54 - 60) = 74.992125984252 kg; factor 2.96043932134852e-4
#include <cmath>
#include <string>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/suv.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

bool rel(double a, double b, double r = 1e-12) { return std::fabs(a - b) <= r * std::fabs(b); }
void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }

Dataset pet(bool with_start_datetime = true) {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.128");
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.4.5");
    put(d, ct::Units, VR::CS, "BQML");
    put(d, ct::DecayCorrection, VR::CS, "START");
    put(d, ct::CorrectedImage, VR::CS, "ATTN\\DECY");
    put(d, ct::PatientWeight, VR::DS, "75");
    put(d, ct::PatientSize, VR::DS, "1.80");
    put(d, ct::PatientSex, VR::CS, "M");
    put(d, ct::SeriesDate, VR::DA, "20260301");
    put(d, ct::SeriesTime, VR::TM, "103000");
    put(d, ct::AcquisitionDate, VR::DA, "20260301");
    put(d, ct::AcquisitionTime, VR::TM, "103512");
    Dataset rp;
    put(rp, ct::RadionuclideTotalDose, VR::DS, "370000000");
    put(rp, ct::RadionuclideHalfLife, VR::DS, "6586.2");
    if (with_start_datetime) put(rp, ct::RadiopharmaceuticalStartDateTime, VR::DT, "20260301093000");
    else put(rp, ct::RadiopharmaceuticalStartTime, VR::TM, "093000");
    d.put(make_sequence(ct::RadiopharmaceuticalInformationSequence, {rp}));
    return d;
}

Dataset& rp_of(Dataset& d) { return d.find(ct::RadiopharmaceuticalInformationSequence)->items[0]; }

}  // namespace

TEST_CASE("suv: DA, TM and DT parsing (PS3.5 6.2)") {
    CHECK(parse_tm("103000") == 37800.0);
    CHECK(parse_tm("10") == 36000.0);
    CHECK(parse_tm("1030") == 37800.0);
    CHECK(parse_tm("10:30:00") == 37800.0);
    CHECK(std::fabs(*parse_tm("103000.25") - 37800.25) < 1e-9);
    CHECK(!parse_tm("25").has_value());
    CHECK(!parse_tm("1061").has_value());
    CHECK(!parse_tm("10a0").has_value());
    CHECK(!parse_tm("103000.1234567").has_value());
    CHECK(parse_da("19700101") == 0);
    CHECK(parse_da("19700102") == 1);
    CHECK(parse_da("20000301") == 11017);  // 30 years incl. 7 leap days + Jan/Feb 2000 (31 + 29)
    CHECK(parse_da("1970.01.02") == 1);
    CHECK(!parse_da("20230229").has_value());
    CHECK(parse_da("20240229").has_value());
    CHECK(!parse_da("2024022").has_value());
    std::optional<int> off;
    CHECK(parse_dt("19700101000001", &off) == 1.0);
    CHECK(!off.has_value());
    CHECK(parse_dt("19700102+0530", &off) == 86400.0);
    CHECK(off == 19800);
    CHECK(parse_dt("197001020100-0100", &off) == 90000.0);
    CHECK(off == -3600);
    CHECK(parse_dt("1970") == 0.0);
    CHECK(!parse_dt("1970+05").has_value());
    CHECK(!parse_dt("x").has_value());
    CHECK(combine_da_tm("19700102", "000010") == 86410.0);
}

TEST_CASE("suv: worked example, body weight / LBM / BSA / IBW") {
    const SuvResult r = compute_suv(pet());
    REQUIRE(r.ok());
    CHECK_EQ(r.warnings, 0u);
    CHECK(r.messages.empty());
    CHECK_EQ(r.scan_time_source, std::string("Series Date/Time"));
    CHECK(r.decay_seconds == 3600.0);
    CHECK(rel(*r.decayed_dose_bq, 253314180.241641, 1e-13));
    CHECK(rel(*r.bw, 2.96075016126046e-4));
    CHECK(rel(*r.lbm_james_kg, 60.2777777777778));
    CHECK(rel(*r.lbm_james, 2.37956587034637e-4));
    CHECK(rel(*r.lbm_janmahasatian_kg, 59.5248287671233));
    CHECK(rel(*r.lbm_janmahasatian, 2.34984195161682e-4));
    CHECK(rel(*r.bsa_m2, 1.94240562380594));
    CHECK(rel(*r.bsa, 7.66797035188891e-5));
    CHECK(rel(*r.ibw_kg, 74.992125984252));
    CHECK(rel(*r.ibw, 2.96043932134852e-4));
    // A pixel of 5000 Bq/ml: SUVbw = 5000 x 2.96075016126046e-4 = 1.48037508063023.
    CHECK(rel(5000.0 * *r.bw, 1.48037508063023));
    // Formulas directly.
    CHECK(rel(lbm_james(60, 160, false), 1.07 * 60 - 148 * (60.0 / 160) * (60.0 / 160)));
    CHECK(rel(decay_fraction(6586.2, 6586.2), 0.5));
}

TEST_CASE("suv: decay correction ADMIN and NONE, time rules") {
    {  // ADMIN: images are corrected to the administration time, no decay of the dose.
        Dataset d = pet();
        put(d, ct::DecayCorrection, VR::CS, "ADMIN");
        auto r = compute_suv(d);
        REQUIRE(r.ok());
        CHECK(r.decay_seconds == 0.0);
        CHECK(rel(*r.bw, 75000.0 / 370e6));
    }
    {  // NONE: dose decayed to this image's acquisition (10:35:12 -> 3912 s).
        Dataset d = pet();
        put(d, ct::DecayCorrection, VR::CS, "NONE");
        put(d, ct::CorrectedImage, VR::CS, "ATTN");
        auto r = compute_suv(d);
        REQUIRE(r.ok());
        CHECK(r.decay_seconds == 3912.0);
        CHECK(rel(*r.bw, 75000.0 / (370e6 * std::exp2(-3912.0 / 6586.2))));
        CHECK_EQ(r.scan_time_source, std::string("Acquisition Date/Time"));
    }
    {  // Series time after the acquisition (post-processed series): acquisition used, flagged.
        Dataset d = pet();
        put(d, ct::SeriesTime, VR::TM, "120000");
        auto r = compute_suv(d);
        REQUIRE(r.ok());
        CHECK((r.warnings & kSuvSeriesAfterAcquisition) != 0);
        CHECK((r.warnings & kSuvPerImageScanTime) != 0);
        CHECK(r.decay_seconds == 3912.0);
        SuvOptions o;
        o.earliest_acquisition_time = *combine_da_tm("20260301", "103000");
        auto r2 = compute_suv(d, 0, o);
        CHECK(r2.decay_seconds == 3600.0);
        CHECK((r2.warnings & kSuvPerImageScanTime) == 0);
    }
    {  // Start Time only: date assumed from the scan, flagged; same factor.
        auto r = compute_suv(pet(false));
        REQUIRE(r.ok());
        CHECK((r.warnings & kSuvInjectionDateAssumed) != 0);
        CHECK(r.decay_seconds == 3600.0);
    }
    {  // Injection after the scan: no factor unless the rollover option is set.
        Dataset d = pet(false);
        put(rp_of(d), ct::RadiopharmaceuticalStartTime, VR::TM, "233000");
        put(d, ct::SeriesTime, VR::TM, "003000");
        put(d, ct::AcquisitionTime, VR::TM, "003000");
        auto r = compute_suv(d);
        CHECK(!r.ok());
        CHECK((r.warnings & kSuvInconsistentTimes) != 0);
        SuvOptions o;
        o.allow_midnight_rollover = true;
        auto r2 = compute_suv(d, 0, o);
        REQUIRE(r2.ok());
        CHECK((r2.warnings & kSuvMidnightRollover) != 0);
        CHECK(r2.decay_seconds == 3600.0);
    }
    {  // UTC offsets on both times are honoured: +0100 injection at 10:30 = 09:30 UTC.
        Dataset d = pet();
        put(rp_of(d), ct::RadiopharmaceuticalStartDateTime, VR::DT, "20260301103000+0100");
        put(d, 0x00080201, VR::SH, "+0000");
        auto r = compute_suv(d);
        REQUIRE(r.ok());
        CHECK(r.decay_seconds == 3600.0);
        CHECK((r.warnings & kSuvTimezoneMismatch) == 0);
        d.remove(0x00080201);
        auto r2 = compute_suv(d);  // only one side has an offset: compared as written, flagged
        CHECK((r2.warnings & kSuvTimezoneMismatch) != 0);
        CHECK(r2.decay_seconds == 0.0);  // 10:30 vs 10:30 as written
    }
}

TEST_CASE("suv: units GML and CNTS") {
    {
        Dataset d = pet();
        put(d, ct::Units, VR::CS, "GML");
        auto r = compute_suv(d);
        REQUIRE(r.ok());
        CHECK(*r.bw == 1.0);
        CHECK((r.warnings & kSuvAlreadySuv) != 0);
        CHECK(rel(*r.lbm_james, 60.2777777777778 / 75.0));
    }
    {
        Dataset d = pet();
        put(d, ct::Units, VR::CS, "CNTS");
        auto r = compute_suv(d);
        CHECK(!r.ok());
        CHECK((r.warnings & kSuvCountsWithoutScale) != 0);
        // Philips private Activity Concentration Scale Factor (7053,xx09): 2.5 Bq/ml per count.
        put(d, 0x70530010, VR::LO, "Philips PET Private Group");
        put(d, 0x70531009, VR::DS, "2.5");
        auto r2 = compute_suv(d);
        REQUIRE(r2.ok());
        CHECK((r2.warnings & kSuvVendorFactorUsed) != 0);
        CHECK(rel(*r2.bw, 2.5 * 2.96075016126046e-4));
        SuvOptions o;
        o.use_vendor_private = false;
        CHECK(!compute_suv(d, 0, o).ok());
        o.counts_scale = 4.0;
        auto r3 = compute_suv(d, 0, o);
        REQUIRE(r3.ok());
        CHECK(rel(*r3.bw, 4.0 * 2.96075016126046e-4));
    }
    {
        Dataset d = pet();
        put(d, ct::Units, VR::CS, "PROPCPS");
        auto r = compute_suv(d);
        CHECK(!r.ok());
        CHECK((r.warnings & kSuvUnsupportedUnits) != 0);
    }
}

TEST_CASE("suv: missing and implausible inputs are reported, never guessed") {
    struct Case {
        Tag tag;
        bool in_rp;
        std::uint32_t bit;
    };
    const Case cases[] = {
        {ct::PatientWeight, false, kSuvMissingWeight},
        {ct::RadionuclideTotalDose, true, kSuvMissingDose},
        {ct::RadionuclideHalfLife, true, kSuvMissingHalfLife},
        {ct::RadiopharmaceuticalStartDateTime, true, kSuvMissingInjectionTime},
        {ct::Units, false, kSuvMissingUnits},
    };
    for (const auto& c : cases) {
        Dataset d = pet();
        if (c.in_rp) rp_of(d).remove(c.tag);
        else d.remove(c.tag);
        auto r = compute_suv(d);
        CHECK(!r.ok());
        CHECK((r.warnings & c.bit) != 0);
        CHECK(!r.messages.empty());
    }
    {  // no scan time at all
        Dataset d = pet();
        d.remove(ct::SeriesTime);
        d.remove(ct::AcquisitionTime);
        auto r = compute_suv(d);
        CHECK(!r.ok());
        CHECK((r.warnings & kSuvMissingScanTime) != 0);
    }
    {  // height in cm by mistake: not used for LBM; SUVbw still fine.
        Dataset d = pet();
        put(d, ct::PatientSize, VR::DS, "180");
        auto r = compute_suv(d);
        REQUIRE(r.ok());
        CHECK((r.warnings & kSuvImplausibleValue) != 0);
        CHECK(!r.lbm_james.has_value());
        CHECK((r.warnings & kSuvMissingHeight) != 0);
    }
    {  // sex other: no LBM / IBW but BSA
        Dataset d = pet();
        put(d, ct::PatientSex, VR::CS, "O");
        auto r = compute_suv(d);
        CHECK(r.ok() && r.bsa && !r.lbm_james && !r.ibw);
        CHECK((r.warnings & kSuvMissingSex) != 0);
    }
    {  // not decay corrected per Corrected Image, decay correction absent, ATTN missing
        Dataset d = pet();
        put(d, ct::CorrectedImage, VR::CS, "NORM");
        auto r = compute_suv(d);
        CHECK((r.warnings & kSuvNotDecayCorrected) != 0);
        CHECK((r.warnings & kSuvNotAttenuationCorrected) != 0);
        d.remove(ct::DecayCorrection);
        auto r2 = compute_suv(d);
        CHECK((r2.warnings & kSuvUnknownDecayCorrection) != 0);
        CHECK(!r2.ok());
    }
    {  // zero weight, negative dose, garbage strings, empty data set
        Dataset d = pet();
        put(d, ct::PatientWeight, VR::DS, "0");
        put(rp_of(d), ct::RadionuclideTotalDose, VR::DS, "-5");
        put(rp_of(d), ct::RadionuclideHalfLife, VR::DS, "abc");
        auto r = compute_suv(d);
        CHECK(!r.ok());
        CHECK((r.warnings & (kSuvMissingWeight | kSuvMissingDose | kSuvMissingHalfLife)) ==
              (kSuvMissingWeight | kSuvMissingDose | kSuvMissingHalfLife));
        CHECK(!compute_suv(Dataset{}).ok());
    }
    {  // overrides fill the gaps
        Dataset d = pet();
        d.remove(ct::PatientWeight);
        SuvOptions o;
        o.weight_kg = 75.0;
        auto r = compute_suv(d, 0, o);
        REQUIRE(r.ok());
        CHECK(rel(*r.bw, 2.96075016126046e-4));
    }
    {  // more than ten half-lives: flagged but computed
        Dataset d = pet();
        put(d, ct::SeriesDate, VR::DA, "20260302");
        put(d, ct::AcquisitionDate, VR::DA, "20260302");
        auto r = compute_suv(d);
        CHECK((r.warnings & kSuvInconsistentTimes) != 0);
        CHECK(r.ok());
    }
}

TEST_CASE("suv: C API") {
    DicomFile f;
    f.dataset = pet();
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &file) == SIAARA_OK);
    siaara_suv s{};
    s.struct_size = sizeof s;
    CHECK_EQ(siaara_suv_factor(file, 0, &s), SIAARA_OK);
    CHECK(s.has_bw == 1);
    CHECK(rel(s.bw, 2.96075016126046e-4));
    CHECK(s.has_bsa == 1 && rel(s.bsa, 7.66797035188891e-5));
    CHECK(s.decay_seconds == 3600.0);
    CHECK_EQ(s.warnings, 0u);
    siaara_suv bad{};
    CHECK_EQ(siaara_suv_factor(file, 0, &bad), SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file_free(file);
}
