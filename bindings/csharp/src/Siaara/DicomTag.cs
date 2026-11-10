using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>A DICOM attribute tag (group, element).</summary>
public readonly struct DicomTag : IEquatable<DicomTag>, IComparable<DicomTag>
{
    /// <summary>Creates a tag from (group &lt;&lt; 16) | element.</summary>
    /// <param name="value">The 32-bit tag value.</param>
    public DicomTag(uint value) => Value = value;

    /// <summary>Creates a tag from its group and element numbers.</summary>
    /// <param name="group">Group number.</param>
    /// <param name="element">Element number.</param>
    public DicomTag(ushort group, ushort element) => Value = ((uint)group << 16) | element;

    /// <summary>(group &lt;&lt; 16) | element.</summary>
    public uint Value { get; }

    /// <summary>Group number.</summary>
    public ushort Group => (ushort)(Value >> 16);

    /// <summary>Element number.</summary>
    public ushort Element => (ushort)Value;

    /// <summary>True for private tags (odd group).</summary>
    public bool IsPrivate => (Group & 1) == 1;

    /// <summary>The dictionary keyword, or null for tags the dictionary does not know.</summary>
    public string? Keyword => DicomDictionary.TryLookup(this, out DicomDictionaryEntry? e) ? e.Keyword : null;

    /// <summary>"(GGGG,EEEE)" in upper-case hexadecimal.</summary>
    /// <returns>The formatted tag.</returns>
    public override string ToString() =>
        string.Create(CultureInfo.InvariantCulture, $"({Group:X4},{Element:X4})");

    /// <summary>Parses "(GGGG,EEEE)", "GGGG,EEEE", "GGGGEEEE" or a dictionary keyword.</summary>
    /// <param name="text">The text.</param>
    /// <returns>The tag.</returns>
    /// <exception cref="FormatException">The text is not a tag or a known keyword.</exception>
    public static DicomTag Parse(string text) =>
        TryParse(text, out DicomTag t) ? t : throw new FormatException("Not a DICOM tag or keyword: " + text);

    /// <summary>Parses "(GGGG,EEEE)", "GGGG,EEEE", "GGGGEEEE" or a dictionary keyword.</summary>
    /// <param name="text">The text.</param>
    /// <param name="tag">The tag, when the method returns true.</param>
    /// <returns>True when the text was parsed.</returns>
    public static bool TryParse(string? text, out DicomTag tag)
    {
        tag = default;
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }
        ReadOnlySpan<char> s = text.AsSpan().Trim();
        if (s.Length >= 2 && s[0] == '(' && s[^1] == ')')
        {
            s = s[1..^1];
        }
        Span<char> hex = stackalloc char[8];
        int n = 0;
        foreach (char ch in s)
        {
            if (ch == ',')
            {
                continue;
            }
            if (n == 8 || !char.IsAsciiHexDigit(ch))
            {
                n = -1;
                break;
            }
            hex[n++] = ch;
        }
        if (n == 8 && uint.TryParse(hex, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out uint v))
        {
            tag = new DicomTag(v);
            return true;
        }
        if (DicomDictionary.TryLookup(text.Trim(), out DicomDictionaryEntry? entry))
        {
            tag = entry.Tag;
            return true;
        }
        return false;
    }

    /// <inheritdoc/>
    public bool Equals(DicomTag other) => Value == other.Value;

    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is DicomTag t && t.Value == Value;

    /// <inheritdoc/>
    public override int GetHashCode() => (int)Value;

    /// <inheritdoc/>
    public int CompareTo(DicomTag other) => Value.CompareTo(other.Value);

    /// <summary>Equality.</summary>
    /// <param name="left">First tag.</param>
    /// <param name="right">Second tag.</param>
    /// <returns>True when equal.</returns>
    public static bool operator ==(DicomTag left, DicomTag right) => left.Value == right.Value;

    /// <summary>Inequality.</summary>
    /// <param name="left">First tag.</param>
    /// <param name="right">Second tag.</param>
    /// <returns>True when different.</returns>
    public static bool operator !=(DicomTag left, DicomTag right) => left.Value != right.Value;

    /// <summary>Ordering by tag value.</summary>
    /// <param name="left">First tag.</param>
    /// <param name="right">Second tag.</param>
    /// <returns>True when left sorts first.</returns>
    public static bool operator <(DicomTag left, DicomTag right) => left.Value < right.Value;

    /// <summary>Ordering by tag value.</summary>
    /// <param name="left">First tag.</param>
    /// <param name="right">Second tag.</param>
    /// <returns>True when left sorts last.</returns>
    public static bool operator >(DicomTag left, DicomTag right) => left.Value > right.Value;

    /// <summary>Ordering by tag value.</summary>
    /// <param name="left">First tag.</param>
    /// <param name="right">Second tag.</param>
    /// <returns>True when left does not sort last.</returns>
    public static bool operator <=(DicomTag left, DicomTag right) => left.Value <= right.Value;

    /// <summary>Ordering by tag value.</summary>
    /// <param name="left">First tag.</param>
    /// <param name="right">Second tag.</param>
    /// <returns>True when left does not sort first.</returns>
    public static bool operator >=(DicomTag left, DicomTag right) => left.Value >= right.Value;
}

/// <summary>An entry of the DICOM data dictionary (siaara_dict_entry).</summary>
/// <param name="Tag">The tag.</param>
/// <param name="VR">The value representation, e.g. "PN" (empty when it depends on context).</param>
/// <param name="AlternateVR">The second VR of an attribute such as Pixel Data (OB or OW), or null.</param>
/// <param name="Keyword">The keyword, e.g. PatientName.</param>
/// <param name="Name">The attribute name, e.g. Patient's Name.</param>
/// <param name="ValueMultiplicity">The VM, e.g. "1" or "1-n".</param>
/// <param name="IsRetired">True for retired attributes.</param>
public sealed record DicomDictionaryEntry(
    DicomTag Tag, string VR, string? AlternateVR, string Keyword, string Name, string ValueMultiplicity, bool IsRetired);

/// <summary>Look-ups in the data dictionary compiled into the native library.</summary>
[System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1711", Justification = "\"Data Dictionary\" is the DICOM term (PS3.6).")]
public static partial class DicomDictionary
{
    private static readonly ConcurrentDictionary<uint, DicomDictionaryEntry?> s_byTag = new();

    /// <summary>The entry for a tag.</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The entry.</returns>
    /// <exception cref="SiaaraException">The tag is not in the dictionary (NotFound).</exception>
    public static DicomDictionaryEntry Lookup(DicomTag tag)
    {
        if (TryLookup(tag, out DicomDictionaryEntry? e))
        {
            return e;
        }
        throw new SiaaraException(SiaaraErrorCode.NotFound, "tag " + tag + " is not in the dictionary");
    }

    /// <summary>The entry for a tag, if the dictionary knows it.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="entry">The entry, when the method returns true.</param>
    /// <returns>True when found.</returns>
    public static bool TryLookup(DicomTag tag, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out DicomDictionaryEntry? entry)
    {
        entry = s_byTag.GetOrAdd(tag.Value, static t =>
        {
            var e = new siaara_dict_entry { struct_size = StructSize.Of<siaara_dict_entry>() };
            return Native.siaara_dict_lookup(t, ref e) == Status.Ok ? Convert(e) : null;
        });
        return entry is not null;
    }

    /// <summary>The entry for a keyword (case-sensitive), e.g. "PatientName".</summary>
    /// <param name="keyword">The keyword.</param>
    /// <returns>The entry.</returns>
    /// <exception cref="SiaaraException">Unknown keyword (NotFound).</exception>
    public static DicomDictionaryEntry Lookup(string keyword)
    {
        if (TryLookup(keyword, out DicomDictionaryEntry? e))
        {
            return e;
        }
        throw new SiaaraException(SiaaraErrorCode.NotFound, "keyword " + keyword + " is not in the dictionary");
    }

    /// <summary>The entry for a keyword, if the dictionary knows it.</summary>
    /// <param name="keyword">The keyword.</param>
    /// <param name="entry">The entry, when the method returns true.</param>
    /// <returns>True when found.</returns>
    public static bool TryLookup(string keyword, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out DicomDictionaryEntry? entry)
    {
        ArgumentNullException.ThrowIfNull(keyword);
        var e = new siaara_dict_entry { struct_size = StructSize.Of<siaara_dict_entry>() };
        if (Native.siaara_dict_lookup_keyword(Encoding.UTF8.GetBytes(keyword), ref e) != Status.Ok)
        {
            entry = null;
            return false;
        }
        entry = Convert(e);
        return true;
    }

    private static DicomDictionaryEntry Convert(siaara_dict_entry e)
    {
        string vr = Chars(e.vr);
        string alt = Chars(e.vr_alt);
        return new DicomDictionaryEntry(new DicomTag(e.tag), vr, alt.Length == 0 ? null : alt,
            Marshal.PtrToStringUTF8(e.keyword) ?? string.Empty, Marshal.PtrToStringUTF8(e.name) ?? string.Empty,
            Marshal.PtrToStringUTF8(e.vm) ?? string.Empty, e.retired != 0);
    }

    internal static string Chars(ReadOnlySpan<byte> s)
    {
        int n = s.IndexOf((byte)0);
        return Encoding.ASCII.GetString(n < 0 ? s : s[..n]);
    }
}
