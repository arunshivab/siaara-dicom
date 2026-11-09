// SIAARA DICOM Toolkit - PET standardized uptake values.
// Written from DICOM PS3.3 C.8.9.1 (PET Series: Units, Decay Correction, Corrected Image, Series
// Date/Time as the reference for temporally related attributes), C.8.9.2 (PET Isotope:
// Radiopharmaceutical Information Sequence), C.8.9.4 (PET Image: Frame Reference Time), PS3.17
// (SUV computation notes) and the QIBA FDG-PET/CT Profile's vendor-neutral SUV rules, which this
// implementation follows for the choice of scan reference time.
//
// SUV = modality value (Bq/ml after rescale) x factor. Body weight SUV in g/ml; BSA SUV in cm2/ml;
// lean / ideal body weight SUVs in g/ml. A factor is never produced on a guess: every assumption
// sets a warning bit, and a missing input leaves the factor empty.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/dataset.hpp"

namespace siaara::clinical {

enum SuvWarning : std::uint32_t {
    kSuvMissingWeight = 1u << 0,
    kSuvMissingDose = 1u << 1,
    kSuvMissingHalfLife = 1u << 2,
    kSuvMissingInjectionTime = 1u << 3,
    kSuvMissingScanTime = 1u << 4,
    kSuvInconsistentTimes = 1u << 5,          // injection after the scan reference, or > 10 half-lives
    kSuvCountsWithoutScale = 1u << 6,         // Units CNTS and no usable Bq/ml scale factor
    kSuvNotDecayCorrected = 1u << 7,          // Decay Correction START/ADMIN but DECY not in Corrected Image
    kSuvNotAttenuationCorrected = 1u << 8,    // ATTN not in Corrected Image
    kSuvUnsupportedUnits = 1u << 9,
    kSuvSeriesAfterAcquisition = 1u << 10,    // Series Date/Time later than the acquisition: acquisition used
    kSuvMissingHeight = 1u << 11,             // LBM / BSA / IBW not computed
    kSuvMissingSex = 1u << 12,                // LBM / IBW not computed
    kSuvMidnightRollover = 1u << 13,          // injection date taken as the day before (option)
    kSuvInjectionDateAssumed = 1u << 14,      // Radiopharmaceutical Start Time only: scan date used
    kSuvMissingUnits = 1u << 15,
    kSuvImplausibleValue = 1u << 16,          // weight, height or dose outside plausible ranges: not used
    kSuvAlreadySuv = 1u << 17,                // Units GML: values are body weight SUV already
    kSuvUnknownDecayCorrection = 1u << 18,    // Decay Correction absent or not START / ADMIN / NONE
    kSuvMultipleRadiopharmaceuticals = 1u << 19,
    kSuvTimezoneMismatch = 1u << 20,          // only one of the times carries a UTC offset
    kSuvVendorFactorUsed = 1u << 21,          // Philips private SUV factor used for CNTS
    kSuvPerImageScanTime = 1u << 22,          // reference is this image's acquisition time (may differ per bed)
};

struct SuvOptions {
    // Overrides for the corresponding attributes (e.g. values confirmed by the user).
    std::optional<double> weight_kg, height_m, injected_dose_bq, half_life_s;
    std::optional<char> sex;                 // 'M' or 'F'
    std::optional<double> injection_time;    // seconds since 1970, as parse_dt / combine_da_tm give
    std::optional<double> scan_time;         // explicit scan reference time (seconds since 1970)
    // Earliest acquisition time of the whole series, used instead of this image's acquisition
    // time when Series Date/Time is unusable (QIBA rule for post-processed series times).
    std::optional<double> earliest_acquisition_time;
    std::optional<double> counts_scale;      // Bq/ml per count, for Units CNTS
    bool use_vendor_private = true;          // Philips (7053,xx00) / (7053,xx09) for CNTS
    bool allow_midnight_rollover = false;    // time-only injection after the scan: take the previous day
};

struct SuvResult {
    // SUV factors: SUV = modality value x factor. Empty when not computable.
    std::optional<double> bw, bsa, lbm_james, lbm_janmahasatian, ibw;
    double counts_scale = 1.0;               // applied before the factors for CNTS (included in them)
    // Inputs and intermediates as used.
    std::optional<double> weight_kg, height_m, injected_dose_bq, half_life_s;
    std::optional<double> decay_seconds, decayed_dose_bq;
    std::optional<double> lbm_james_kg, lbm_janmahasatian_kg, bsa_m2, ibw_kg;
    std::string units, decay_correction, scan_time_source;
    // Where the injection time came from: "caller", "Radiopharmaceutical Start DateTime",
    // "Radiopharmaceutical Start Time + scan date"; empty when none was usable (F11).
    std::string injection_time_source;
    std::uint32_t warnings = 0;              // SuvWarning bits
    std::vector<std::string> messages;       // one per warning, never containing patient values
    bool ok() const noexcept { return bw.has_value(); }
};

// SUV factors for frame `frame` of a PET image (the frame only matters for enhanced objects).
SuvResult compute_suv(const Dataset& ds, std::uint32_t frame = 0, const SuvOptions& opt = {});

// Formulas (weight kg, height cm).
double lbm_james(double weight_kg, double height_cm, bool male) noexcept;          // kg
double lbm_janmahasatian(double weight_kg, double height_cm, bool male) noexcept;  // kg
double bsa_du_bois(double weight_kg, double height_cm) noexcept;                   // m2
double ibw_devine(double height_cm, bool male) noexcept;                           // kg
// Fraction of activity remaining after `seconds` with the given half-life: 2^(-t / T).
double decay_fraction(double seconds, double half_life_s) noexcept;

}  // namespace siaara::clinical
