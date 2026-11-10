// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Measure.Suv;

/// <summary>
/// What the SUV computation had to assume or could not do. The 23 bits keep the values of the
/// toolkit's former SIAARA_SUV_* flags so results compare one to one with the golden file. A factor
/// is never produced on a guess: every assumption sets a bit, a missing input leaves the factor empty.
/// </summary>
[Flags]
public enum SuvWarnings
{
    /// <summary>No warning.</summary>
    None = 0,
    /// <summary>Patient's Weight missing (or not usable).</summary>
    MissingWeight = 1 << 0,
    /// <summary>Radionuclide Total Dose missing (or not usable).</summary>
    MissingDose = 1 << 1,
    /// <summary>Radionuclide Half Life missing.</summary>
    MissingHalfLife = 1 << 2,
    /// <summary>No usable injection (radiopharmaceutical start) time.</summary>
    MissingInjectionTime = 1 << 3,
    /// <summary>No usable scan reference time.</summary>
    MissingScanTime = 1 << 4,
    /// <summary>Injection after the scan reference, or more than 10 half-lives before it.</summary>
    InconsistentTimes = 1 << 5,
    /// <summary>Units CNTS without a usable counts to Bq/ml scale factor.</summary>
    CountsWithoutScale = 1 << 6,
    /// <summary>Decay Correction START / ADMIN but DECY not in Corrected Image.</summary>
    NotDecayCorrected = 1 << 7,
    /// <summary>ATTN not in Corrected Image.</summary>
    NotAttenuationCorrected = 1 << 8,
    /// <summary>Units not BQML, CNTS or GML.</summary>
    UnsupportedUnits = 1 << 9,
    /// <summary>Series Date/Time later than the acquisition: the acquisition time was used.</summary>
    SeriesAfterAcquisition = 1 << 10,
    /// <summary>Patient's Size missing: LBM, BSA and IBW not computed.</summary>
    MissingHeight = 1 << 11,
    /// <summary>Patient's Sex not M or F: LBM and IBW not computed.</summary>
    MissingSex = 1 << 12,
    /// <summary>Injection taken as the day before (caller option).</summary>
    MidnightRollover = 1 << 13,
    /// <summary>Radiopharmaceutical Start Time only: the scan date was used.</summary>
    InjectionDateAssumed = 1 << 14,
    /// <summary>Units (0054,1001) missing.</summary>
    MissingUnits = 1 << 15,
    /// <summary>Weight, height or dose outside plausible ranges: not used.</summary>
    ImplausibleValue = 1 << 16,
    /// <summary>Units GML: the values are body-weight SUV already.</summary>
    AlreadySuv = 1 << 17,
    /// <summary>Decay Correction missing or not START, ADMIN or NONE.</summary>
    UnknownDecayCorrection = 1 << 18,
    /// <summary>More than one Radiopharmaceutical Information item: the first was used.</summary>
    MultipleRadiopharmaceuticals = 1 << 19,
    /// <summary>Only one of the compared times carries a UTC offset: compared as written.</summary>
    TimezoneMismatch = 1 << 20,
    /// <summary>The Philips private activity concentration scale factor was used for CNTS.</summary>
    VendorFactorUsed = 1 << 21,
    /// <summary>The scan reference is this image's acquisition time (may differ per bed position).</summary>
    PerImageScanTime = 1 << 22,
}

/// <summary>Plain words for the warnings (never patient values).</summary>
public static class SuvWarningText
{
    /// <summary>Text of one warning.</summary>
    /// <param name="w">A single flag.</param>
    /// <returns>The text.</returns>
    public static string Of(SuvWarnings w) => w switch
    {
        SuvWarnings.MissingWeight => "patient weight missing",
        SuvWarnings.MissingDose => "injected dose missing",
        SuvWarnings.MissingHalfLife => "half-life missing",
        SuvWarnings.MissingInjectionTime => "injection time missing",
        SuvWarnings.MissingScanTime => "scan time missing",
        SuvWarnings.InconsistentTimes => "times inconsistent",
        SuvWarnings.CountsWithoutScale => "counts without a scale factor",
        SuvWarnings.NotDecayCorrected => "not decay corrected",
        SuvWarnings.NotAttenuationCorrected => "not attenuation corrected",
        SuvWarnings.UnsupportedUnits => "units not supported",
        SuvWarnings.SeriesAfterAcquisition => "series time after acquisition",
        SuvWarnings.MissingHeight => "patient height missing",
        SuvWarnings.MissingSex => "patient sex missing",
        SuvWarnings.MidnightRollover => "midnight rollover assumed",
        SuvWarnings.InjectionDateAssumed => "injection date assumed",
        SuvWarnings.MissingUnits => "units missing",
        SuvWarnings.ImplausibleValue => "implausible value",
        SuvWarnings.AlreadySuv => "values are already SUV",
        SuvWarnings.UnknownDecayCorrection => "unknown decay correction",
        SuvWarnings.MultipleRadiopharmaceuticals => "several radiopharmaceuticals",
        SuvWarnings.TimezoneMismatch => "time zone mismatch",
        SuvWarnings.VendorFactorUsed => "vendor SUV factor used",
        SuvWarnings.PerImageScanTime => "per-image scan time used",
        _ => "SUV warning",
    };

    /// <summary>Texts of every set flag, in bit order.</summary>
    /// <param name="warnings">Flags.</param>
    /// <returns>The texts.</returns>
    public static IReadOnlyList<string> All(SuvWarnings warnings)
    {
        var list = new List<string>();
        for (int bit = 0; bit < 23; bit++)
        {
            var f = (SuvWarnings)(1 << bit);
            if ((warnings & f) != 0)
            {
                list.Add(Of(f));
            }
        }
        return list;
    }
}
