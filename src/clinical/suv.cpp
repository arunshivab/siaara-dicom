// SIAARA DICOM Toolkit - PET SUV (see suv.hpp for the references and rules).
#include "clinical/suv.hpp"

#include <cmath>
#include <string_view>

#include "clinical/common.hpp"

namespace siaara::clinical {

namespace {

constexpr Tag kTimezoneOffsetFromUtc = 0x00080201;

void warn(SuvResult& r, SuvWarning w, const char* msg) {
    if (r.warnings & w) return;
    r.warnings |= w;
    r.messages.emplace_back(msg);
}

bool has_value(const Dataset& ds, Tag tag, std::string_view value) {
    const std::size_t n = ds.value_multiplicity(tag);
    for (std::size_t i = 0; i < n; ++i)
        if (auto v = get_text(ds, tag, i); v && *v == value) return true;
    return false;
}

std::optional<int> tz_attribute(const Dataset& ds) {
    auto s = get_text(ds, kTimezoneOffsetFromUtc);
    if (!s || s->size() != 5 || (s->at(0) != '+' && s->at(0) != '-')) return std::nullopt;
    for (std::size_t i = 1; i < 5; ++i)
        if ((*s)[i] < '0' || (*s)[i] > '9') return std::nullopt;
    const int hh = ((*s)[1] - '0') * 10 + ((*s)[2] - '0');
    const int mm = ((*s)[3] - '0') * 10 + ((*s)[4] - '0');
    return ((*s)[0] == '-' ? -1 : 1) * (hh * 3600 + mm * 60);
}

struct Moment {
    double seconds = 0.0;           // as written
    std::optional<int> offset;      // UTC offset, when the DT carried one
    bool date_assumed = false;
};

}  // namespace

double lbm_james(double w, double h, bool male) noexcept {
    const double r = w / h;
    return male ? 1.10 * w - 128.0 * r * r : 1.07 * w - 148.0 * r * r;
}

double lbm_janmahasatian(double w, double h, bool male) noexcept {
    const double hm = h / 100.0;
    const double bmi = w / (hm * hm);
    return male ? 9270.0 * w / (6680.0 + 216.0 * bmi) : 9270.0 * w / (8780.0 + 244.0 * bmi);
}

double bsa_du_bois(double w, double h) noexcept { return 0.007184 * std::pow(w, 0.425) * std::pow(h, 0.725); }

double ibw_devine(double h, bool male) noexcept {
    const double inches = h / 2.54;
    return (male ? 50.0 : 45.5) + 2.3 * (inches - 60.0);
}

double decay_fraction(double seconds, double half_life_s) noexcept { return std::exp2(-seconds / half_life_s); }

SuvResult compute_suv(const Dataset& ds, std::uint32_t frame, const SuvOptions& opt) {
    SuvResult r;
    const std::optional<int> tz = tz_attribute(ds);

    // ---- units and corrections
    if (auto u = get_text(ds, tags::Units)) r.units = *u;
    if (auto dc = get_text(ds, tags::DecayCorrection)) r.decay_correction = *dc;
    bool units_ok = true;
    if (r.units.empty()) {
        warn(r, kSuvMissingUnits, "Units (0054,1001) is missing");
        units_ok = false;
    } else if (r.units == "BQML") {
        r.counts_scale = 1.0;
    } else if (r.units == "CNTS") {
        bool have = false;
        if (opt.counts_scale && *opt.counts_scale > 0.0 && std::isfinite(*opt.counts_scale)) {
            r.counts_scale = *opt.counts_scale;
            have = true;
        } else if (opt.use_vendor_private) {
            // Philips PET private group: Activity Concentration Scale Factor (7053,xx09), Bq/ml per
            // count, as documented in the vendor's DICOM Conformance Statement.
            if (auto t = ds.resolve_private(0x7053, "Philips PET Private Group", 0x09)) {
                if (auto v = ds.get_double(*t); v && *v > 0.0 && std::isfinite(*v)) {
                    r.counts_scale = *v;
                    have = true;
                    warn(r, kSuvVendorFactorUsed, "vendor private activity concentration scale factor used for CNTS");
                }
            }
        }
        if (!have) {
            warn(r, kSuvCountsWithoutScale, "Units CNTS without a usable counts to Bq/ml scale factor");
            units_ok = false;
        }
    } else if (r.units == "GML") {
        warn(r, kSuvAlreadySuv, "Units GML: pixel values are body weight SUV already");
    } else {
        warn(r, kSuvUnsupportedUnits, "Units value is not BQML, CNTS or GML");
        units_ok = false;
    }
    const bool decy = has_value(ds, tags::CorrectedImage, "DECY");
    if (!has_value(ds, tags::CorrectedImage, "ATTN"))
        warn(r, kSuvNotAttenuationCorrected, "Corrected Image does not include ATTN");
    const bool start = r.decay_correction == "START", admin = r.decay_correction == "ADMIN",
               none = r.decay_correction == "NONE";
    if (!start && !admin && !none)
        warn(r, kSuvUnknownDecayCorrection, "Decay Correction (0054,1102) is missing or not START, ADMIN or NONE");
    if ((start || admin) && !decy)
        warn(r, kSuvNotDecayCorrected, "Decay Correction says corrected but Corrected Image lacks DECY");

    // ---- patient
    std::optional<double> w = opt.weight_kg ? opt.weight_kg : ds.get_double(tags::PatientWeight);
    if (w && !(*w > 0.0 && *w <= 1000.0)) {
        warn(r, kSuvImplausibleValue, "patient weight outside 0..1000 kg: not used");
        w.reset();
    }
    if (!w) warn(r, kSuvMissingWeight, "Patient's Weight (0010,1030) missing or zero");
    r.weight_kg = w;
    std::optional<double> h = opt.height_m ? opt.height_m : ds.get_double(tags::PatientSize);
    if (h && !(*h > 0.3 && *h < 3.0)) {
        warn(r, kSuvImplausibleValue, "Patient's Size outside 0.3..3 m (not converted from other units): not used");
        h.reset();
    }
    r.height_m = h;
    std::optional<char> sex = opt.sex;
    if (!sex)
        if (auto s = get_text(ds, tags::PatientSex); s && (*s == "M" || *s == "F")) sex = (*s)[0];

    // ---- radiopharmaceutical
    const auto& rp = items_of(ds, tags::RadiopharmaceuticalInformationSequence);
    if (rp.size() > 1)
        warn(r, kSuvMultipleRadiopharmaceuticals, "more than one Radiopharmaceutical Information item: first used");
    const Dataset* rpi = rp.empty() ? nullptr : &rp.front();
    std::optional<double> dose = opt.injected_dose_bq;
    if (!dose && rpi) dose = rpi->get_double(tags::RadionuclideTotalDose);
    if (dose && !(*dose > 0.0 && std::isfinite(*dose))) {
        warn(r, kSuvImplausibleValue, "Radionuclide Total Dose is not positive: not used");
        dose.reset();
    }
    if (!dose) warn(r, kSuvMissingDose, "Radionuclide Total Dose (0018,1074) missing");
    r.injected_dose_bq = dose;
    std::optional<double> half = opt.half_life_s;
    if (!half && rpi) half = rpi->get_double(tags::RadionuclideHalfLife);
    if (half && !(*half > 0.0 && std::isfinite(*half))) half.reset();
    if (!half && !admin) warn(r, kSuvMissingHalfLife, "Radionuclide Half Life (0018,1075) missing");
    r.half_life_s = half;

    // ---- scan reference time
    std::optional<Moment> scan;
    const auto series_da = get_text(ds, tags::SeriesDate);
    auto moment_of_da_tm = [&](Tag da_tag, Tag tm_tag) -> std::optional<Moment> {
        auto da = get_text(ds, da_tag);
        auto tm = get_text(ds, tm_tag);
        if (!da || !tm) return std::nullopt;
        auto v = combine_da_tm(*da, *tm);
        if (!v) return std::nullopt;
        return Moment{*v, std::nullopt, false};
    };
    std::optional<Moment> acq;
    if (const Dataset* fc = functional_group(ds, frame, tags::FrameContentSequence)) {
        if (auto dt = get_text(*fc, tags::FrameAcquisitionDateTime)) {
            std::optional<int> off;
            if (auto v = parse_dt(*dt, &off)) acq = Moment{*v, off, false};
        }
    }
    if (!acq)
        if (auto dt = get_text(ds, tags::AcquisitionDateTime)) {
            std::optional<int> off;
            if (auto v = parse_dt(*dt, &off)) acq = Moment{*v, off, false};
        }
    if (!acq) acq = moment_of_da_tm(tags::AcquisitionDate, tags::AcquisitionTime);
    if (opt.scan_time) {
        scan = Moment{*opt.scan_time, std::nullopt, false};
        r.scan_time_source = "caller";
    } else if (none) {
        // Not decay corrected: activity is as measured during the acquisition of this image.
        if (acq) {
            scan = acq;
            r.scan_time_source = "Acquisition Date/Time";
        }
    } else if (start) {
        auto series = moment_of_da_tm(tags::SeriesDate, tags::SeriesTime);
        if (series && acq && series->seconds > acq->seconds + 1e-6) {
            warn(r, kSuvSeriesAfterAcquisition, "Series Date/Time is after the acquisition: acquisition time used");
            if (opt.earliest_acquisition_time) {
                scan = Moment{*opt.earliest_acquisition_time, std::nullopt, false};
                r.scan_time_source = "earliest acquisition";
            } else {
                scan = acq;
                r.scan_time_source = "Acquisition Date/Time";
                warn(r, kSuvPerImageScanTime, "scan reference is this image's acquisition time");
            }
        } else if (series) {
            scan = series;
            r.scan_time_source = "Series Date/Time";
        } else if (opt.earliest_acquisition_time) {
            scan = Moment{*opt.earliest_acquisition_time, std::nullopt, false};
            r.scan_time_source = "earliest acquisition";
        } else if (acq) {
            scan = acq;
            r.scan_time_source = "Acquisition Date/Time";
            warn(r, kSuvPerImageScanTime, "Series Date/Time missing: this image's acquisition time used");
        }
    }

    // ---- injection time
    std::optional<Moment> inj;
    if (opt.injection_time) {
        inj = Moment{*opt.injection_time, std::nullopt, false};
    } else if (rpi) {
        if (auto dt = get_text(*rpi, tags::RadiopharmaceuticalStartDateTime)) {
            std::optional<int> off;
            if (auto v = parse_dt(*dt, &off)) inj = Moment{*v, off, false};
        }
        if (!inj)
            if (auto tm = get_text(*rpi, tags::RadiopharmaceuticalStartTime)) {
                // Time only: the date is taken from the scan reference (or the series date).
                auto t = parse_tm(*tm);
                std::optional<double> day;
                if (scan) day = std::floor(scan->seconds / 86400.0) * 86400.0;
                else if (series_da)
                    if (auto d = parse_da(*series_da)) day = static_cast<double>(*d) * 86400.0;
                if (t && day) {
                    inj = Moment{*day + *t, std::nullopt, true};
                    warn(r, kSuvInjectionDateAssumed, "Radiopharmaceutical Start Time without date: scan date used");
                }
            }
    }

    // ---- decay
    if (admin) {
        r.decay_seconds = 0.0;
        if (dose) r.decayed_dose_bq = *dose;
        r.scan_time_source = "administration (ADMIN)";
    } else if (start || none) {
        if (!scan) warn(r, kSuvMissingScanTime, "no usable scan reference time");
        if (!inj) warn(r, kSuvMissingInjectionTime, "no usable radiopharmaceutical start time");
        if (scan && inj) {
            double s = scan->seconds, i = inj->seconds;
            // UTC offsets: shift when both are known (or the other one is given by (0008,0201)).
            std::optional<int> so = scan->offset ? scan->offset : tz;
            std::optional<int> io = inj->offset ? inj->offset : tz;
            if (scan->offset || inj->offset) {
                if (so && io) {
                    s -= *so;
                    i -= *io;
                } else {
                    warn(r, kSuvTimezoneMismatch, "only one time has a UTC offset: compared as written");
                }
            }
            double dt = s - i;
            if (dt < 0.0 && inj->date_assumed && opt.allow_midnight_rollover && dt > -86400.0) {
                dt += 86400.0;
                warn(r, kSuvMidnightRollover, "injection time after scan time: previous day assumed (option)");
            }
            if (dt < 0.0) {
                warn(r, kSuvInconsistentTimes, "radiopharmaceutical start is after the scan reference time");
            } else {
                r.decay_seconds = dt;
                if (half && dt > 10.0 * *half)
                    warn(r, kSuvInconsistentTimes, "scan more than 10 half-lives after injection");
                if (dose && half) r.decayed_dose_bq = *dose * decay_fraction(dt, *half);
            }
        }
    }

    // ---- body size measures
    const bool male = sex && *sex == 'M';
    if (w && h) {
        const double hcm = *h * 100.0;
        r.bsa_m2 = bsa_du_bois(*w, hcm);
        if (sex) {
            r.lbm_james_kg = lbm_james(*w, hcm, male);
            r.lbm_janmahasatian_kg = lbm_janmahasatian(*w, hcm, male);
            r.ibw_kg = ibw_devine(hcm, male);
            if (*r.lbm_james_kg <= 0.0) r.lbm_james_kg.reset();  // James is invalid at high BMI
            if (*r.ibw_kg <= 0.0) r.ibw_kg.reset();
        } else {
            warn(r, kSuvMissingSex, "Patient's Sex not M or F: LBM and IBW not computed");
        }
    } else if (w && !h) {
        warn(r, kSuvMissingHeight, "Patient's Size missing: LBM, BSA and IBW not computed");
    }

    // ---- factors
    if (r.units == "GML") {
        r.bw = 1.0;
        if (w) {
            const double g = *w * 1000.0;
            if (r.lbm_james_kg) r.lbm_james = *r.lbm_james_kg * 1000.0 / g;
            if (r.lbm_janmahasatian_kg) r.lbm_janmahasatian = *r.lbm_janmahasatian_kg * 1000.0 / g;
            if (r.bsa_m2) r.bsa = *r.bsa_m2 * 1e4 / g;
            if (r.ibw_kg) r.ibw = *r.ibw_kg * 1000.0 / g;
        }
        return r;
    }
    if (!units_ok || !r.decayed_dose_bq || !w) return r;
    if (r.units == "CNTS" && (r.warnings & kSuvVendorFactorUsed) == 0 && !opt.counts_scale) return r;
    const double per_bq = r.counts_scale / *r.decayed_dose_bq;
    r.bw = *w * 1000.0 * per_bq;
    if (r.lbm_james_kg) r.lbm_james = *r.lbm_james_kg * 1000.0 * per_bq;
    if (r.lbm_janmahasatian_kg) r.lbm_janmahasatian = *r.lbm_janmahasatian_kg * 1000.0 * per_bq;
    if (r.bsa_m2) r.bsa = *r.bsa_m2 * 1e4 * per_bq;
    if (r.ibw_kg) r.ibw = *r.ibw_kg * 1000.0 * per_bq;
    return r;
}

}  // namespace siaara::clinical
