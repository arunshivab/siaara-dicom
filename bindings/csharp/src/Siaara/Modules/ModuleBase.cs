using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using Siaara.Interop;

namespace Siaara.Modules;

/// <summary>
/// Base of every generated module, macro and sequence item class (Generated/Modules.g.cs).
/// Read-only and lazy: a property reads the data set when it is accessed, never caches, and never
/// changes the data set. A missing attribute gives null (an empty list for sequences); a value the
/// typed form cannot hold gives null too, while the text form (the indexer, or the *Raw
/// property of dates and times) always has the value as stored.
/// </summary>
public abstract class ModuleBase
{
    private readonly DicomDataset _ds;
    private readonly DicomDataset[]? _chain;

    /// <summary>Wraps a data set (the file's main data set, or a sequence item).</summary>
    /// <param name="dataset">The data set.</param>
    protected ModuleBase(DicomDataset dataset)
    {
        ArgumentNullException.ThrowIfNull(dataset);
        _ds = dataset;
    }

    /// <summary>
    /// Wraps a chain of data sets, nearest first: each attribute is read from the first data set
    /// that holds it (the PS3.3 rule that control points after the first hold only the values that
    /// change).
    /// </summary>
    /// <param name="chain">The data sets, nearest first (at least one).</param>
    private protected ModuleBase(DicomDataset[] chain)
    {
        ArgumentNullException.ThrowIfNull(chain);
        if (chain.Length == 0)
        {
            throw new ArgumentException("the chain needs at least one data set", nameof(chain));
        }
        _ds = chain[0];
        _chain = chain;
    }

    /// <summary>The data sets read, nearest first (one, except for an inherited view).</summary>
    private protected IReadOnlyList<DicomDataset> Chain => _chain ?? new[] { _ds };

    /// <summary>The data set this object reads (for an inherited view: the nearest one).</summary>
    public DicomDataset Dataset => _ds;

    /// <summary>The data set that holds a tag: this one, or for an inherited view the nearest that has it.</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The data set, or null when none holds the tag.</returns>
    public DicomDataset? SourceOf(DicomTag tag)
    {
        if (_chain is null)
        {
            return _ds.Contains(tag) ? _ds : null;
        }
        foreach (DicomDataset d in _chain)
        {
            if (d.Contains(tag))
            {
                return d;
            }
        }
        return null;
    }

    /// <summary>The value of any attribute of this data set as text (all values, backslash separated).</summary>
    /// <param name="tag">The tag.</param>
    public string? this[DicomTag tag] => SourceOf(tag)?[tag];

    private protected bool IsTablePresent(string tableId)
    {
        byte[] name = Encoding.UTF8.GetBytes(tableId);
        using var l = new HandleLease(_ds.File.Handle);
        Status.Check(Native.siaara_dataset_module_present(_ds.Resolve(l.Pointer), name, out int present));
        return present != 0;
    }

    // ------------------------------------------------------------------ readers used by the generated code

    private protected string? GetStr(uint tag)
    {
        var t = new DicomTag(tag);
        return SourceOf(t)?[t];
    }

    private protected string[]? GetStrs(uint tag)
    {
        var t = new DicomTag(tag);
        return SourceOf(t)?.GetStrings(t);
    }

    private protected long? GetInt(uint tag)
    {
        var t = new DicomTag(tag);
        DicomDataset? d = SourceOf(t);
        return d is not null && d.TryGetInt64(t, out long v) ? v : null;
    }

    private protected long[]? GetInts(uint tag)
    {
        var t = new DicomTag(tag);
        DicomDataset? d = SourceOf(t);
        if (d is null)
        {
            return null;
        }
        int n = d.GetValueMultiplicity(t);
        var r = new long[n];
        for (int i = 0; i < n; i++)
        {
            if (!d.TryGetInt64(t, out r[i], i))
            {
                return null;
            }
        }
        return r;
    }

    private protected double? GetDbl(uint tag)
    {
        var t = new DicomTag(tag);
        DicomDataset? d = SourceOf(t);
        return d is not null && d.TryGetDouble(t, out double v) ? v : null;
    }

    private protected double[]? GetDbls(uint tag)
    {
        var t = new DicomTag(tag);
        DicomDataset? d = SourceOf(t);
        if (d is null)
        {
            return null;
        }
        int n = d.GetValueMultiplicity(t);
        var r = new double[n];
        for (int i = 0; i < n; i++)
        {
            if (!d.TryGetDouble(t, out r[i], i))
            {
                return null;
            }
        }
        return r;
    }

    private protected PersonName? GetPn(uint tag)
    {
        string? s = GetStr(tag);
        return s is null ? null : new PersonName(s);
    }

    private protected PersonName[]? GetPns(uint tag)
    {
        string[]? v = GetStrs(tag);
        if (v is null)
        {
            return null;
        }
        var r = new PersonName[v.Length];
        for (int i = 0; i < v.Length; i++)
        {
            r[i] = new PersonName(v[i]);
        }
        return r;
    }

    private protected DateOnly? GetDate(uint tag) => DicomDateTimeText.ParseDate(GetStr(tag));

    private protected DateOnly?[]? GetDates(uint tag) => Map(GetStrs(tag), DicomDateTimeText.ParseDate);

    private protected TimeOnly? GetTime(uint tag) => DicomDateTimeText.ParseTime(GetStr(tag));

    private protected TimeOnly?[]? GetTimes(uint tag) => Map(GetStrs(tag), DicomDateTimeText.ParseTime);

    private protected DateTime? GetDT(uint tag) => DicomDateTimeText.ParseDateTime(GetStr(tag));

    private protected DateTime?[]? GetDTs(uint tag) => Map(GetStrs(tag), DicomDateTimeText.ParseDateTime);

    private protected DicomTag? GetTag(uint tag)
    {
        DicomTag[]? v = GetTags(tag);
        return v is { Length: > 0 } ? v[0] : null;
    }

    private protected DicomTag[]? GetTags(uint tag)
    {
        byte[]? b = GetBytes(tag);
        if (b is null)
        {
            return null;
        }
        var r = new DicomTag[b.Length / 4];
        for (int i = 0; i < r.Length; i++)
        {
            uint g = (uint)(b[i * 4] | (b[i * 4 + 1] << 8));
            uint e = (uint)(b[i * 4 + 2] | (b[i * 4 + 3] << 8));
            r[i] = new DicomTag((g << 16) | e);
        }
        return r;
    }

    private protected byte[]? GetBytes(uint tag)
    {
        var t = new DicomTag(tag);
        DicomDataset? d = SourceOf(t);
        return d is not null && d.TryGetBytes(t, out byte[]? v) ? v : null;
    }

    private protected IReadOnlyList<DicomDataset> GetItems(uint tag)
    {
        var t = new DicomTag(tag);
        DicomDataset? d = SourceOf(t);
        return d is null ? Array.Empty<DicomDataset>() : d.GetSequence(t);
    }

    private protected IReadOnlyList<T> GetSeq<T>(uint tag, Func<DicomDataset, T> make)
    {
        IReadOnlyList<DicomDataset> items = GetItems(tag);
        if (items.Count == 0)
        {
            return Array.Empty<T>();
        }
        var r = new T[items.Count];
        for (int i = 0; i < r.Length; i++)
        {
            r[i] = make(items[i]);
        }
        return r;
    }

    private static TOut?[]? Map<TOut>(string[]? values, Func<string?, TOut?> parse)
        where TOut : struct
    {
        if (values is null)
        {
            return null;
        }
        var r = new TOut?[values.Length];
        for (int i = 0; i < values.Length; i++)
        {
            r[i] = parse(values[i]);
        }
        return r;
    }
}

/// <summary>
/// DA / TM / DT text to .NET values (PS3.5 Table 6.2-1). Only well-formed values convert; anything
/// else gives null (the text form keeps the value as stored).
/// </summary>
public static class DicomDateTimeText
{
    /// <summary>DA "YYYYMMDD".</summary>
    /// <param name="s">The value.</param>
    /// <returns>The date, or null.</returns>
    public static DateOnly? ParseDate(string? s)
    {
        if (s is null || s.Length != 8 || !AllDigits(s))
        {
            return null;
        }
        return DateOnly.TryParseExact(s, "yyyyMMdd", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateOnly d) ? d : null;
    }

    /// <summary>TM "HH[MM[SS[.F{1,6}]]]" (omitted components are zero in the TimeOnly; the text keeps the precision).</summary>
    /// <param name="s">The value.</param>
    /// <returns>The time, or null.</returns>
    public static TimeOnly? ParseTime(string? s)
    {
        if (s is null)
        {
            return null;
        }
        return TryTime(s.AsSpan(), out TimeOnly t) ? t : null;
    }

    /// <summary>
    /// DT "YYYYMMDD[HH[MM[SS[.F{1,6}]]]][&amp;ZZXX]": at least the full date is needed; the UTC
    /// offset, when present, is not applied (it stays in the text form).
    /// </summary>
    /// <param name="s">The value.</param>
    /// <returns>The date and time (DateTimeKind.Unspecified), or null.</returns>
    public static DateTime? ParseDateTime(string? s)
    {
        if (s is null || s.Length < 8)
        {
            return null;
        }
        ReadOnlySpan<char> v = s.AsSpan();
        int sign = v.IndexOfAny('+', '-');
        if (sign >= 0)
        {
            ReadOnlySpan<char> off = v[(sign + 1)..];
            if (off.Length != 4 || !AllDigits(off))
            {
                return null;
            }
            v = v[..sign];
        }
        DateOnly? d = ParseDate(v[..8].ToString());
        if (d is null)
        {
            return null;
        }
        if (v.Length == 8)
        {
            return d.Value.ToDateTime(TimeOnly.MinValue);
        }
        return TryTime(v[8..], out TimeOnly t) ? d.Value.ToDateTime(t) : null;
    }

    private static bool TryTime(ReadOnlySpan<char> s, out TimeOnly t)
    {
        t = default;
        int dot = s.IndexOf('.');
        ReadOnlySpan<char> main = dot < 0 ? s : s[..dot];
        if (main.Length is not (2 or 4 or 6) || !AllDigits(main))
        {
            return false;
        }
        int hh = int.Parse(main[..2], CultureInfo.InvariantCulture);
        int mm = main.Length >= 4 ? int.Parse(main[2..4], CultureInfo.InvariantCulture) : 0;
        int ss = main.Length == 6 ? int.Parse(main[4..6], CultureInfo.InvariantCulture) : 0;
        long ticks = 0;
        if (dot >= 0)
        {
            ReadOnlySpan<char> f = s[(dot + 1)..];
            if (main.Length != 6 || f.Length is < 1 or > 6 || !AllDigits(f))
            {
                return false;
            }
            long frac = long.Parse(f, CultureInfo.InvariantCulture);
            for (int i = f.Length; i < 7; i++)
            {
                frac *= 10;
            }
            ticks = frac;
        }
        if (hh > 23 || mm > 59 || ss > 60)
        {
            return false;
        }
        // A leap second (60) has no TimeOnly representation: no typed value.
        if (ss == 60)
        {
            return false;
        }
        t = new TimeOnly(hh, mm, ss).Add(TimeSpan.FromTicks(ticks));
        return true;
    }

    private static bool AllDigits(ReadOnlySpan<char> s)
    {
        foreach (char c in s)
        {
            if (c is < '0' or > '9')
            {
                return false;
            }
        }
        return true;
    }
}
