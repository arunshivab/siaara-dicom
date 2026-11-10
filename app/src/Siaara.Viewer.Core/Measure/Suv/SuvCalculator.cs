// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// PET standardized uptake values, written for the app from DICOM PS3.3 C.8.9.1 (PET Series: Units,
// Decay Correction, Corrected Image, Series Date/Time), C.8.9.2 (Radiopharmaceutical Information),
// PS3.17 SUV notes and the QIBA FDG-PET/CT profile's rules (see DecayTime). SUV = modality value
// (Bq/ml after rescale) x factor: body weight, lean body mass and ideal body weight SUV in g/ml,
// BSA SUV in cm²/ml. Units CNTS need a counts to Bq/ml scale (caller, or the Philips private
// Activity Concentration Scale Factor, flagged); Units GML are SUVbw already (factor 1).
// The results equal the toolkit's former siaara_suv_factor on the golden file.

namespace Siaara.Viewer.Core.Measure.Suv;

/// <summary>Values the user confirmed or entered, used instead of the data set's.</summary>
public sealed record SuvOptions
{
    /// <summary>Weight in kg.</summary>
    public double? WeightKg { get; init; }
    /// <summary>Height in m.</summary>
    public double? HeightM { get; init; }
    /// <summary>Injected dose in Bq.</summary>
    public double? InjectedDoseBq { get; init; }
    /// <summary>Half-life in s.</summary>
    public double? HalfLifeSeconds { get; init; }
    /// <summary>'M' or 'F'.</summary>
    public char? Sex { get; init; }
    /// <summary>Injection time, seconds since 1970 as <see cref="DecayTime.ParseDateTime"/> gives.</summary>
    public double? InjectionTime { get; init; }
    /// <summary>Explicit scan reference time (seconds since 1970).</summary>
    public double? ScanTime { get; init; }
    /// <summary>Earliest acquisition time of the whole series (QIBA rule for post-processed series times).</summary>
    public double? EarliestAcquisitionTime { get; init; }
    /// <summary>Bq/ml per count for Units CNTS.</summary>
    public double? CountsScale { get; init; }
    /// <summary>Use the Philips private scale factor for CNTS.</summary>
    public bool UseVendorPrivate { get; init; } = true;
    /// <summary>A time-only injection after the scan is taken as the day before.</summary>
    public bool AllowMidnightRollover { get; init; }
}

/// <summary>SUV factors of a PET frame, the inputs used and the warnings.</summary>
public sealed record SuvResult
{
    /// <summary>Warnings.</summary>
    public SuvWarnings Warnings { get; init; }
    /// <summary>Body-weight SUV factor (g/ml per Bq/ml), or null.</summary>
    public double? BodyWeight { get; init; }
    /// <summary>LBM (James) SUV factor, or null.</summary>
    public double? LeanBodyMassJames { get; init; }
    /// <summary>LBM (Janmahasatian) SUV factor, or null.</summary>
    public double? LeanBodyMassJanmahasatian { get; init; }
    /// <summary>BSA SUV factor (cm²/ml per Bq/ml), or null.</summary>
    public double? BodySurfaceArea { get; init; }
    /// <summary>IBW SUV factor, or null.</summary>
    public double? IdealBodyWeight { get; init; }
    /// <summary>Counts to Bq/ml scale included in the factors (1 for BQML).</summary>
    public double CountsScale { get; init; } = 1.0;
    /// <summary>Seconds from injection to the scan reference, or null.</summary>
    public double? DecaySeconds { get; init; }
    /// <summary>Injected dose decayed to the scan reference (Bq), or null.</summary>
    public double? DecayedDoseBq { get; init; }
    /// <summary>Weight used (kg), or null.</summary>
    public double? WeightKg { get; init; }
    /// <summary>Height used (m), or null.</summary>
    public double? HeightM { get; init; }
    /// <summary>Dose used (Bq), or null.</summary>
    public double? InjectedDoseBq { get; init; }
    /// <summary>Half-life used (s), or null.</summary>
    public double? HalfLifeSeconds { get; init; }
    /// <summary>Lean body mass, James (kg), or null.</summary>
    public double? LeanBodyMassJamesKg { get; init; }
    /// <summary>Lean body mass, Janmahasatian (kg), or null.</summary>
    public double? LeanBodyMassJanmahasatianKg { get; init; }
    /// <summary>Body surface area (m²), or null.</summary>
    public double? BodySurfaceAreaM2 { get; init; }
    /// <summary>Ideal body weight (kg), or null.</summary>
    public double? IdealBodyWeightKg { get; init; }
    /// <summary>Units as found ("" when missing).</summary>
    public string Units { get; init; } = "";
    /// <summary>Decay correction as found.</summary>
    public SuvDecayCorrection DecayCorrection { get; init; }
    /// <summary>Where the scan reference time came from.</summary>
    public SuvTimeSource ScanTimeSource { get; init; }
    /// <summary>Where the injection time came from.</summary>
    public SuvTimeSource InjectionTimeSource { get; init; }
    /// <summary>The inputs and the attributes they came from.</summary>
    public SuvInputs? Inputs { get; init; }

    /// <summary>The warnings in plain words.</summary>
    public IReadOnlyList<string> WarningTexts => SuvWarningText.All(Warnings);
}

/// <summary>Computes SUV factors.</summary>
public static class SuvCalculator
{
    /// <summary>SUV factors of a frame, reading the inputs from the data set.</summary>
    /// <param name="ds">The data set (headers are enough).</param>
    /// <param name="frame">Frame.</param>
    /// <param name="options">Overrides, or null.</param>
    /// <returns>The result (never throws for missing inputs: see the warnings).</returns>
    public static SuvResult Compute(Toolkit.TkDataset ds, int frame, SuvOptions? options = null) => Compute(SuvInputs.Read(ds, frame), options);

    /// <summary>SUV factors from inputs already read.</summary>
    /// <param name="inputs">Inputs.</param>
    /// <param name="options">Overrides, or null.</param>
    /// <returns>The result.</returns>
    public static SuvResult Compute(SuvInputs inputs, SuvOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        SuvOptions opt = options ?? new SuvOptions();
        var w = SuvWarnings.None;
        int? tz = DecayTime.ParseUtcOffset(inputs.TimezoneOffset);

        // ---- units and corrections
        string units = inputs.Units ?? "";
        double countsScale = 1.0;
        bool unitsOk = true;
        switch (units)
        {
            case "":
                w |= SuvWarnings.MissingUnits;
                unitsOk = false;
                break;
            case "BQML":
                break;
            case "CNTS":
                {
                    bool have = false;
                    if (opt.CountsScale is double c && c > 0.0 && double.IsFinite(c))
                    {
                        countsScale = c;
                        have = true;
                    }
                    else if (opt.UseVendorPrivate && inputs.PhilipsScaleFactor is double v && v > 0.0 && double.IsFinite(v))
                    {
                        countsScale = v;
                        have = true;
                        w |= SuvWarnings.VendorFactorUsed;
                    }
                    if (!have)
                    {
                        w |= SuvWarnings.CountsWithoutScale;
                        unitsOk = false;
                    }
                    break;
                }
            case "GML":
                w |= SuvWarnings.AlreadySuv;
                break;
            default:
                w |= SuvWarnings.UnsupportedUnits;
                unitsOk = false;
                break;
        }
        bool decy = inputs.CorrectedImage.Contains("DECY", StringComparer.Ordinal);
        if (!inputs.CorrectedImage.Contains("ATTN", StringComparer.Ordinal))
        {
            w |= SuvWarnings.NotAttenuationCorrected;
        }
        SuvDecayCorrection mode = inputs.DecayCorrection switch
        {
            "START" => SuvDecayCorrection.Start,
            "ADMIN" => SuvDecayCorrection.Admin,
            "NONE" => SuvDecayCorrection.None,
            _ => SuvDecayCorrection.Unknown,
        };
        if (mode == SuvDecayCorrection.Unknown)
        {
            w |= SuvWarnings.UnknownDecayCorrection;
        }
        if (mode is SuvDecayCorrection.Start or SuvDecayCorrection.Admin && !decy)
        {
            w |= SuvWarnings.NotDecayCorrected;
        }

        // ---- patient
        double? weight = opt.WeightKg ?? inputs.WeightKg;
        if (weight is double wk && !(wk > 0.0 && wk <= 1000.0))
        {
            w |= SuvWarnings.ImplausibleValue;
            weight = null;
        }
        if (weight is null)
        {
            w |= SuvWarnings.MissingWeight;
        }
        double? height = opt.HeightM ?? inputs.HeightM;
        if (height is double hm && !(hm > 0.3 && hm < 3.0))
        {
            // Not converted from other units (a value in cm is refused, not guessed).
            w |= SuvWarnings.ImplausibleValue;
            height = null;
        }
        char? sex = opt.Sex ?? (inputs.Sex is "M" or "F" ? inputs.Sex[0] : null);

        // ---- radiopharmaceutical
        if (inputs.RadiopharmaceuticalItems > 1)
        {
            w |= SuvWarnings.MultipleRadiopharmaceuticals;
        }
        double? dose = opt.InjectedDoseBq ?? (inputs.RadiopharmaceuticalItems > 0 ? inputs.TotalDoseBq : null);
        if (dose is double dq && !(dq > 0.0 && double.IsFinite(dq)))
        {
            w |= SuvWarnings.ImplausibleValue;
            dose = null;
        }
        if (dose is null)
        {
            w |= SuvWarnings.MissingDose;
        }
        double? half = opt.HalfLifeSeconds ?? (inputs.RadiopharmaceuticalItems > 0 ? inputs.HalfLifeSeconds : null);
        if (half is double hl && !(hl > 0.0 && double.IsFinite(hl)))
        {
            half = null;
        }
        if (half is null && mode != SuvDecayCorrection.Admin)
        {
            w |= SuvWarnings.MissingHalfLife;
        }

        // ---- times and decay
        (TimePoint? scan, SuvTimeSource scanSource) = DecayTime.ScanReference(inputs, mode, opt, ref w);
        (TimePoint? injection, SuvTimeSource injectionSource) = DecayTime.Injection(inputs, scan, opt, ref w);
        double? decaySeconds = null, decayedDose = null;
        if (mode == SuvDecayCorrection.Admin)
        {
            decaySeconds = 0.0;
            decayedDose = dose;
            scanSource = SuvTimeSource.Administration;
        }
        else if (mode is SuvDecayCorrection.Start or SuvDecayCorrection.None)
        {
            if (scan is null)
            {
                w |= SuvWarnings.MissingScanTime;
            }
            if (injection is null)
            {
                w |= SuvWarnings.MissingInjectionTime;
            }
            if (scan is TimePoint s && injection is TimePoint i && DecayTime.Elapsed(s, i, tz, opt, ref w) is double dt)
            {
                decaySeconds = dt;
                if (half is double h1 && dt > 10.0 * h1)
                {
                    w |= SuvWarnings.InconsistentTimes;
                }
                if (dose is double d && half is double h2)
                {
                    decayedDose = d * DecayTime.DecayFraction(dt, h2);
                }
            }
        }

        // ---- body size
        double? lbmJames = null, lbmJanma = null, bsa = null, ibw = null;
        bool male = sex == 'M';
        if (weight is double wkg && height is double hgt)
        {
            double hcm = hgt * 100.0;
            bsa = BodySize.SurfaceAreaDuBois(wkg, hcm);
            if (sex is not null)
            {
                lbmJames = BodySize.LeanBodyMassJames(wkg, hcm, male);
                lbmJanma = BodySize.LeanBodyMassJanmahasatian(wkg, hcm, male);
                ibw = BodySize.IdealBodyWeightDevine(hcm, male);
                if (lbmJames <= 0.0)
                {
                    lbmJames = null; // James is not valid at high BMI
                }
                if (ibw <= 0.0)
                {
                    ibw = null;
                }
            }
            else
            {
                w |= SuvWarnings.MissingSex;
            }
        }
        else if (weight is not null && height is null)
        {
            w |= SuvWarnings.MissingHeight;
        }

        var result = new SuvResult
        {
            Warnings = w,
            CountsScale = countsScale,
            DecaySeconds = decaySeconds,
            DecayedDoseBq = decayedDose,
            WeightKg = weight,
            HeightM = height,
            InjectedDoseBq = dose,
            HalfLifeSeconds = half,
            LeanBodyMassJamesKg = lbmJames,
            LeanBodyMassJanmahasatianKg = lbmJanma,
            BodySurfaceAreaM2 = bsa,
            IdealBodyWeightKg = ibw,
            Units = units,
            DecayCorrection = mode,
            ScanTimeSource = scanSource,
            InjectionTimeSource = injectionSource,
            Inputs = inputs,
        };

        // ---- factors
        if (units == "GML")
        {
            if (weight is not double g0)
            {
                return result with { BodyWeight = 1.0 };
            }
            double g = g0 * 1000.0;
            return result with
            {
                BodyWeight = 1.0,
                LeanBodyMassJames = lbmJames * 1000.0 / g,
                LeanBodyMassJanmahasatian = lbmJanma * 1000.0 / g,
                BodySurfaceArea = bsa * 1e4 / g,
                IdealBodyWeight = ibw * 1000.0 / g,
            };
        }
        if (!unitsOk || decayedDose is not double decayed || weight is not double kg)
        {
            return result;
        }
        if (units == "CNTS" && (w & SuvWarnings.VendorFactorUsed) == 0 && opt.CountsScale is null)
        {
            return result;
        }
        double perBq = countsScale / decayed;
        return result with
        {
            BodyWeight = kg * 1000.0 * perBq,
            LeanBodyMassJames = lbmJames * 1000.0 * perBq,
            LeanBodyMassJanmahasatian = lbmJanma * 1000.0 * perBq,
            BodySurfaceArea = bsa * 1e4 * perBq,
            IdealBodyWeight = ibw * 1000.0 * perBq,
        };
    }
}
