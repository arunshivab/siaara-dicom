// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;

namespace Siaara.Viewer.Core.Text;

/// <summary>
/// Display-only formatting of stored DICOM values. Every method returns a new string for the screen;
/// the stored value is never changed (data integrity rule).
/// </summary>
public static class DisplayFormat
{
    /// <summary>A person name for display: "Family^Given^Middle^Prefix^Suffix" becomes "Family, Given Middle".
    /// The first non-empty component group (alphabetic, ideographic, phonetic) is shown.</summary>
    /// <param name="stored">Stored PN value.</param>
    /// <returns>Display text.</returns>
    public static string PersonName(string? stored)
    {
        if (string.IsNullOrEmpty(stored))
        {
            return "";
        }
        string group = stored.Split('=').FirstOrDefault(g => g.Replace("^", "", StringComparison.Ordinal).Trim().Length > 0) ?? "";
        if (group.Length == 0)
        {
            return "";
        }
        string[] c = group.Split('^');
        string family = c[0].Trim();
        string given = string.Join(' ', c.Skip(1).Take(2).Select(s => s.Trim()).Where(s => s.Length > 0));
        string prefix = c.Length > 3 ? c[3].Trim() : "";
        string suffix = c.Length > 4 ? c[4].Trim() : "";
        string s = family;
        if (given.Length > 0)
        {
            s = s.Length > 0 ? s + ", " + given : given;
        }
        if (prefix.Length > 0)
        {
            s = prefix + " " + s;
        }
        if (suffix.Length > 0)
        {
            s += " " + suffix;
        }
        return s;
    }

    /// <summary>A DA value (YYYYMMDD) as YYYY-MM-DD; other text is shown as stored.</summary>
    /// <param name="stored">Stored DA.</param>
    /// <returns>Display text.</returns>
    public static string Date(string? stored)
    {
        if (string.IsNullOrEmpty(stored))
        {
            return "";
        }
        string s = stored.Trim();
        return s.Length == 8 && s.All(char.IsAsciiDigit) ? $"{s[..4]}-{s[4..6]}-{s[6..8]}" : stored;
    }

    /// <summary>A TM value (HHMMSS.frac) as HH:MM:SS.</summary>
    /// <param name="stored">Stored TM.</param>
    /// <returns>Display text.</returns>
    public static string Time(string? stored)
    {
        if (string.IsNullOrEmpty(stored))
        {
            return "";
        }
        string s = stored.Trim();
        if (s.Length >= 6 && s[..6].All(char.IsAsciiDigit))
        {
            return $"{s[..2]}:{s[2..4]}:{s[4..6]}";
        }
        if (s.Length == 4 && s.All(char.IsAsciiDigit))
        {
            return $"{s[..2]}:{s[2..4]}";
        }
        return stored;
    }

    /// <summary>Age: the AS value ("045Y" as "45Y") or, without it, whole years between birth and study date.</summary>
    /// <param name="patientAge">Patient's Age (AS) or null.</param>
    /// <param name="birthDate">Birth date (DA).</param>
    /// <param name="studyDate">Study date (DA).</param>
    /// <returns>Display text, or "".</returns>
    public static string Age(string? patientAge, string? birthDate, string? studyDate)
    {
        if (!string.IsNullOrWhiteSpace(patientAge))
        {
            string a = patientAge.Trim();
            if (a.Length == 4 && a[..3].All(char.IsAsciiDigit))
            {
                return int.Parse(a[..3], CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture) + a[3];
            }
            return patientAge;
        }
        if (TryDate(birthDate, out DateOnly b) && TryDate(studyDate, out DateOnly s) && s >= b)
        {
            int years = s.Year - b.Year - (s.DayOfYear < b.DayOfYear ? 1 : 0);
            return years.ToString(CultureInfo.InvariantCulture) + "Y";
        }
        return "";
    }

    /// <summary>Sex/age as "F / 45Y".</summary>
    /// <param name="sex">Patient's Sex.</param>
    /// <param name="age">Age display text.</param>
    /// <returns>Display text.</returns>
    public static string SexAge(string? sex, string age)
    {
        ArgumentNullException.ThrowIfNull(age);
        string s = (sex ?? "").Trim();
        if (s.Length == 0)
        {
            return age;
        }
        return age.Length == 0 ? s : s + " / " + age;
    }

    /// <summary>Parses a DA value.</summary>
    /// <param name="stored">Stored DA.</param>
    /// <param name="date">Date.</param>
    /// <returns>True when valid.</returns>
    public static bool TryDate(string? stored, out DateOnly date) =>
        DateOnly.TryParseExact((stored ?? "").Trim(), "yyyyMMdd", CultureInfo.InvariantCulture, DateTimeStyles.None, out date);

    /// <summary>A number for overlays (invariant, up to digits decimals, trailing zeros removed).</summary>
    /// <param name="value">Value.</param>
    /// <param name="digits">Decimals.</param>
    /// <returns>Text.</returns>
    public static string Number(double value, int digits = 1) =>
        Math.Round(value, digits).ToString("0." + new string('#', Math.Max(1, digits)), CultureInfo.InvariantCulture);
}
