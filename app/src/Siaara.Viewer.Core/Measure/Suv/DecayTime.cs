// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Times for the decay correction of the injected dose. DICOM DA / TM / DT parsing (PS3.5 6.2,
// including the retired "HH:MM:SS" and "YYYY.MM.DD" forms) to seconds since 1970-01-01 of the
// local time as written, and the rules for the scan reference time (QIBA FDG-PET/CT profile):
//   Decay Correction START: Series Date/Time; if it is later than this image's acquisition (a
//     post-processed series), the acquisition time (or the series' earliest acquisition when the
//     caller gives it); without a series time, the acquisition time. Both flagged.
//   Decay Correction ADMIN: the values are decay corrected to the administration: no decay.
//   Decay Correction NONE: activity as measured during this image's acquisition.
// The injection time is the Radiopharmaceutical Start DateTime, else the Start Time on the scan
// date (flagged). UTC offsets (a DT suffix, or Timezone Offset From UTC) are applied when both
// times have one; when only one has one the times are compared as written (flagged).

using System.Globalization;

namespace Siaara.Viewer.Core.Measure.Suv;

/// <summary>Where a time came from (numbers as the toolkit's former SIAARA_SUV_SRC_*).</summary>
public enum SuvTimeSource
{
    /// <summary>Missing.</summary>
    None = 0,
    /// <summary>Series Date / Series Time.</summary>
    Series = 1,
    /// <summary>This image's acquisition time.</summary>
    Acquisition = 2,
    /// <summary>The series' earliest acquisition (caller option).</summary>
    EarliestAcquisition = 3,
    /// <summary>Given by the caller.</summary>
    Caller = 4,
    /// <summary>Decay Correction ADMIN: no decay.</summary>
    Administration = 5,
    /// <summary>Radiopharmaceutical Start DateTime.</summary>
    StartDateTime = 6,
    /// <summary>Radiopharmaceutical Start Time on the scan date.</summary>
    StartTimeWithScanDate = 7,
}

/// <summary>Decay Correction (0054,1102) as found (numbers as SIAARA_SUV_DECAY_*).</summary>
public enum SuvDecayCorrection
{
    /// <summary>Missing or another value.</summary>
    Unknown = 0,
    /// <summary>START.</summary>
    Start = 1,
    /// <summary>ADMIN.</summary>
    Admin = 2,
    /// <summary>NONE.</summary>
    None = 3,
}

/// <summary>A point in time as written: seconds since 1970-01-01 (no time zone applied), the UTC offset when the value carried one.</summary>
/// <param name="Seconds">Seconds.</param>
/// <param name="UtcOffsetSeconds">Offset of a DT suffix, or null.</param>
/// <param name="DateAssumed">The date was taken from another attribute.</param>
public readonly record struct TimePoint(double Seconds, int? UtcOffsetSeconds = null, bool DateAssumed = false);

/// <summary>DICOM date / time parsing and the decay-time rules.</summary>
public static class DecayTime
{
    private const double Day = 86400.0;

    /// <summary>Removes leading and trailing spaces and NULs (DICOM padding).</summary>
    /// <param name="s">Text.</param>
    /// <returns>Trimmed text.</returns>
    public static string Trim(string s)
    {
        ArgumentNullException.ThrowIfNull(s);
        return s.Trim(' ', '\0');
    }

    private static bool Digits(string s, int pos, int n, out int value)
    {
        value = 0;
        if (pos + n > s.Length)
        {
            return false;
        }
        int v = 0;
        for (int i = 0; i < n; i++)
        {
            char ch = s[pos + i];
            if (ch is < '0' or > '9')
            {
                return false;
            }
            v = (v * 10) + (ch - '0');
        }
        value = v;
        return true;
    }

    private static bool ValidDate(int y, int m, int d)
    {
        if (m is < 1 or > 12 || d < 1)
        {
            return false;
        }
        int[] days = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
        int dim = days[m - 1];
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        {
            dim = 29;
        }
        return d <= dim;
    }

    /// <summary>Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar.</summary>
    /// <param name="y">Year.</param>
    /// <param name="m">Month.</param>
    /// <param name="d">Day.</param>
    /// <returns>Days.</returns>
    public static long DaysFromCivil(long y, int m, int d)
    {
        y -= m <= 2 ? 1 : 0;
        long era = (y >= 0 ? y : y - 399) / 400;
        long yoe = y - (era * 400);
        long mp = (m + 9) % 12;
        long doy = (((153 * mp) + 2) / 5) + d - 1;
        long doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
        return (era * 146097) + doe - 719468;
    }

    /// <summary>Seconds since midnight from TM "HH[MM[SS[.F{1,6}]]]" (also "HH:MM:SS.frac"), or null.</summary>
    /// <param name="text">TM value.</param>
    /// <returns>Seconds.</returns>
    public static double? ParseTime(string? text)
    {
        if (text is null)
        {
            return null;
        }
        string s = Trim(text);
        if (s.Length == 0)
        {
            return null;
        }
        var buf = new System.Text.StringBuilder(32);
        foreach (char ch in s)
        {
            if (ch == ':')
            {
                continue;
            }
            if (buf.Length >= 32)
            {
                return null;
            }
            buf.Append(ch);
        }
        string t = buf.ToString();
        if (!Digits(t, 0, 2, out int hh) || hh > 23)
        {
            return null;
        }
        int mm = 0, ss = 0, pos = 2;
        if (pos < t.Length)
        {
            if (!Digits(t, pos, 2, out mm) || mm > 59)
            {
                return null;
            }
            pos += 2;
        }
        if (pos < t.Length && t[pos] != '.')
        {
            if (!Digits(t, pos, 2, out ss) || ss > 60)
            {
                return null;
            }
            pos += 2;
        }
        double frac = 0.0;
        if (pos < t.Length)
        {
            if (t[pos] != '.' || pos + 1 >= t.Length || t.Length - pos - 1 > 6)
            {
                return null;
            }
            double scale = 0.1;
            for (int i = pos + 1; i < t.Length; i++)
            {
                if (t[i] is < '0' or > '9')
                {
                    return null;
                }
                frac += (t[i] - '0') * scale;
                scale /= 10.0;
            }
        }
        return (hh * 3600.0) + (mm * 60.0) + ss + frac;
    }

    /// <summary>Days since 1970-01-01 from DA "YYYYMMDD" (also "YYYY.MM.DD"), or null.</summary>
    /// <param name="text">DA value.</param>
    /// <returns>Days.</returns>
    public static long? ParseDate(string? text)
    {
        if (text is null)
        {
            return null;
        }
        string s = Trim(text);
        int y, m, d;
        if (s.Length == 8)
        {
            if (!Digits(s, 0, 4, out y) || !Digits(s, 4, 2, out m) || !Digits(s, 6, 2, out d))
            {
                return null;
            }
        }
        else if (s.Length == 10 && s[4] == '.' && s[7] == '.')
        {
            if (!Digits(s, 0, 4, out y) || !Digits(s, 5, 2, out m) || !Digits(s, 8, 2, out d))
            {
                return null;
            }
        }
        else
        {
            return null;
        }
        return ValidDate(y, m, d) ? DaysFromCivil(y, m, d) : null;
    }

    /// <summary>DT "YYYY[MM[DD[HH[MM[SS[.F]]]]]][&amp;ZZXX]" as written (the offset is reported, not applied), or null.</summary>
    /// <param name="text">DT value.</param>
    /// <returns>The time point.</returns>
    public static TimePoint? ParseDateTime(string? text)
    {
        if (text is null)
        {
            return null;
        }
        string s = Trim(text);
        int? offset = null;
        int sign = s.Length > 4 ? s.IndexOfAny(['+', '-'], 4) : -1;
        if (sign >= 0)
        {
            string off = s[sign..];
            if (off.Length != 5 || !Digits(off, 1, 2, out int oh) || !Digits(off, 3, 2, out int om) || oh > 14 || om > 59)
            {
                return null;
            }
            offset = (off[0] == '-' ? -1 : 1) * ((oh * 3600) + (om * 60));
            s = s[..sign];
        }
        if (!Digits(s, 0, 4, out int y))
        {
            return null;
        }
        int m = 1, d = 1, pos = 4;
        if (pos < s.Length)
        {
            if (!Digits(s, pos, 2, out m))
            {
                return null;
            }
            pos += 2;
        }
        if (pos < s.Length)
        {
            if (!Digits(s, pos, 2, out d))
            {
                return null;
            }
            pos += 2;
        }
        if (!ValidDate(y, m, d))
        {
            return null;
        }
        double secs = 0.0;
        if (pos < s.Length)
        {
            if (ParseTime(s[pos..]) is not double t)
            {
                return null;
            }
            secs = t;
        }
        return new TimePoint((DaysFromCivil(y, m, d) * Day) + secs, offset);
    }

    /// <summary>A DA and a TM combined into seconds since 1970 (as written), or null.</summary>
    /// <param name="date">DA value.</param>
    /// <param name="time">TM value.</param>
    /// <returns>Seconds.</returns>
    public static double? Combine(string? date, string? time) =>
        ParseDate(date) is long d && ParseTime(time) is double t ? (d * Day) + t : null;

    /// <summary>Timezone Offset From UTC "&amp;HHMM" in seconds, or null when absent or not in that form.</summary>
    /// <param name="text">The value.</param>
    /// <returns>Seconds.</returns>
    public static int? ParseUtcOffset(string? text)
    {
        if (text is null || text.Length != 5 || (text[0] != '+' && text[0] != '-'))
        {
            return null;
        }
        for (int i = 1; i < 5; i++)
        {
            if (text[i] is < '0' or > '9')
            {
                return null;
            }
        }
        int hh = ((text[1] - '0') * 10) + (text[2] - '0'), mm = ((text[3] - '0') * 10) + (text[4] - '0');
        return (text[0] == '-' ? -1 : 1) * ((hh * 3600) + (mm * 60));
    }

    /// <summary>The image's acquisition time: Frame Acquisition DateTime of its functional group, else Acquisition DateTime, else Acquisition Date + Time.</summary>
    /// <param name="inputs">Inputs.</param>
    /// <returns>The time, or null.</returns>
    public static TimePoint? Acquisition(SuvInputs inputs)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        TimePoint? acq = inputs.FrameAcquisitionDateTime is string f ? ParseDateTime(f) : null;
        acq ??= inputs.AcquisitionDateTime is string a ? ParseDateTime(a) : null;
        if (acq is null && inputs.AcquisitionDate is not null && inputs.AcquisitionTime is not null && Combine(inputs.AcquisitionDate, inputs.AcquisitionTime) is double v)
        {
            acq = new TimePoint(v);
        }
        return acq;
    }

    /// <summary>The scan reference time for decay correction (rules in the file header).</summary>
    /// <param name="inputs">Inputs.</param>
    /// <param name="mode">Decay correction.</param>
    /// <param name="options">Caller options.</param>
    /// <param name="warnings">Warnings so far (added to).</param>
    /// <returns>The time and its source.</returns>
    public static (TimePoint? Time, SuvTimeSource Source) ScanReference(SuvInputs inputs, SuvDecayCorrection mode, SuvOptions options, ref SuvWarnings warnings)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        ArgumentNullException.ThrowIfNull(options);
        TimePoint? acq = Acquisition(inputs);
        if (options.ScanTime is double caller)
        {
            return (new TimePoint(caller), SuvTimeSource.Caller);
        }
        if (mode == SuvDecayCorrection.None)
        {
            return acq is null ? (null, SuvTimeSource.None) : (acq, SuvTimeSource.Acquisition);
        }
        if (mode != SuvDecayCorrection.Start)
        {
            return (null, SuvTimeSource.None);
        }
        TimePoint? series = inputs.SeriesDate is not null && inputs.SeriesTime is not null && Combine(inputs.SeriesDate, inputs.SeriesTime) is double sv
            ? new TimePoint(sv) : null;
        if (series is TimePoint s && acq is TimePoint a && s.Seconds > a.Seconds + 1e-6)
        {
            warnings |= SuvWarnings.SeriesAfterAcquisition;
            if (options.EarliestAcquisitionTime is double earliest)
            {
                return (new TimePoint(earliest), SuvTimeSource.EarliestAcquisition);
            }
            warnings |= SuvWarnings.PerImageScanTime;
            return (a, SuvTimeSource.Acquisition);
        }
        if (series is not null)
        {
            return (series, SuvTimeSource.Series);
        }
        if (options.EarliestAcquisitionTime is double e2)
        {
            return (new TimePoint(e2), SuvTimeSource.EarliestAcquisition);
        }
        if (acq is not null)
        {
            warnings |= SuvWarnings.PerImageScanTime;
            return (acq, SuvTimeSource.Acquisition);
        }
        return (null, SuvTimeSource.None);
    }

    /// <summary>The injection time: Radiopharmaceutical Start DateTime, else Start Time on the scan (or series) date.</summary>
    /// <param name="inputs">Inputs.</param>
    /// <param name="scan">Scan reference time, or null.</param>
    /// <param name="options">Caller options.</param>
    /// <param name="warnings">Warnings so far (added to).</param>
    /// <returns>The time and its source.</returns>
    public static (TimePoint? Time, SuvTimeSource Source) Injection(SuvInputs inputs, TimePoint? scan, SuvOptions options, ref SuvWarnings warnings)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        ArgumentNullException.ThrowIfNull(options);
        if (options.InjectionTime is double caller)
        {
            return (new TimePoint(caller), SuvTimeSource.Caller);
        }
        if (inputs.RadiopharmaceuticalItems == 0)
        {
            return (null, SuvTimeSource.None);
        }
        if (inputs.StartDateTime is string dt && ParseDateTime(dt) is TimePoint p)
        {
            return (p, SuvTimeSource.StartDateTime);
        }
        if (inputs.StartTime is string tm)
        {
            double? t = ParseTime(tm);
            double? day = null;
            if (scan is TimePoint sc)
            {
                day = Math.Floor(sc.Seconds / Day) * Day;
            }
            else if (inputs.SeriesDate is not null && ParseDate(inputs.SeriesDate) is long d)
            {
                day = d * Day;
            }
            if (t is double tt && day is double dd)
            {
                warnings |= SuvWarnings.InjectionDateAssumed;
                return (new TimePoint(dd + tt, null, DateAssumed: true), SuvTimeSource.StartTimeWithScanDate);
            }
        }
        return (null, SuvTimeSource.None);
    }

    /// <summary>Seconds from injection to the scan reference with UTC offsets applied when both are known; null when negative (flagged).</summary>
    /// <param name="scan">Scan reference.</param>
    /// <param name="injection">Injection.</param>
    /// <param name="timezone">Timezone Offset From UTC of the data set, or null.</param>
    /// <param name="options">Caller options.</param>
    /// <param name="warnings">Warnings so far (added to).</param>
    /// <returns>Seconds, or null.</returns>
    public static double? Elapsed(TimePoint scan, TimePoint injection, int? timezone, SuvOptions options, ref SuvWarnings warnings)
    {
        ArgumentNullException.ThrowIfNull(options);
        double s = scan.Seconds, i = injection.Seconds;
        int? so = scan.UtcOffsetSeconds ?? timezone, io = injection.UtcOffsetSeconds ?? timezone;
        if (scan.UtcOffsetSeconds is not null || injection.UtcOffsetSeconds is not null)
        {
            if (so is int a && io is int b)
            {
                s -= a;
                i -= b;
            }
            else
            {
                warnings |= SuvWarnings.TimezoneMismatch;
            }
        }
        double dt = s - i;
        if (dt < 0.0 && injection.DateAssumed && options.AllowMidnightRollover && dt > -Day)
        {
            dt += Day;
            warnings |= SuvWarnings.MidnightRollover;
        }
        if (dt < 0.0)
        {
            warnings |= SuvWarnings.InconsistentTimes;
            return null;
        }
        return dt;
    }

    /// <summary>Fraction of activity left after <paramref name="seconds"/>: 2^(-t / T).</summary>
    /// <param name="seconds">Elapsed seconds.</param>
    /// <param name="halfLifeSeconds">Half-life.</param>
    /// <returns>The fraction.</returns>
    public static double DecayFraction(double seconds, double halfLifeSeconds) => Math.Pow(2.0, -seconds / halfLifeSeconds);

    /// <summary>A time point as ISO text (for display; the zone is the one written).</summary>
    /// <param name="t">Time.</param>
    /// <returns>Text.</returns>
    public static string Format(TimePoint t) =>
        DateTime.UnixEpoch.AddSeconds(t.Seconds).ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)
        + (t.UtcOffsetSeconds is int o ? (o < 0 ? " -" : " +") + TimeSpan.FromSeconds(Math.Abs(o)).ToString(@"hh\:mm", CultureInfo.InvariantCulture) : "");
}
