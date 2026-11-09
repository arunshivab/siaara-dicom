using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>Options of the DICOM JSON encoder (PS3.18 Annex F).</summary>
[Flags]
public enum DicomJsonOptions
{
    /// <summary>Numbers as JSON numbers, U+FFFD for unconvertible text.</summary>
    None = 0,
    /// <summary>DS and IS values as JSON strings.</summary>
    NumbersAsStrings = C.SIAARA_JSON_NUMBERS_AS_STRINGS,
    /// <summary>Fail instead of writing U+FFFD for unconvertible text.</summary>
    StrictCharset = C.SIAARA_JSON_STRICT_CHARSET,
}

/// <summary>
/// A data set borrowed from a <see cref="DicomFile"/>: the main data set, the file meta group or a
/// sequence item. Valid while the file is not disposed; every call checks that and keeps the file
/// alive during the native call. Items are located again (sequence tag and index from the parent)
/// on every call, so changes to the enclosing data sets never leave a dangling pointer.
/// Text getters decode with the Specific Character Set (0008,0005) of the data set, or of the
/// nearest enclosing data set that has one (PS3.5 6.1).
/// </summary>
public sealed class DicomDataset
{
    internal enum RootKind
    {
        Main,
        Meta,
        Item,
    }

    private static readonly DicomTag s_specificCharacterSet = new(0x00080005u);

    private readonly DicomFile _file;
    private readonly RootKind _kind;
    private readonly DicomDataset? _parent;
    private readonly DicomTag _sequenceTag;
    private readonly int _itemIndex;

    internal DicomDataset(DicomFile file, RootKind kind)
    {
        _file = file;
        _kind = kind;
    }

    private DicomDataset(DicomDataset parent, DicomTag sequenceTag, int index)
    {
        _file = parent._file;
        _kind = RootKind.Item;
        _parent = parent;
        _sequenceTag = sequenceTag;
        _itemIndex = index;
    }

    /// <summary>The file that owns this data set.</summary>
    public DicomFile File => _file;

    /// <summary>The enclosing data set of a sequence item; null for the main data set and the meta group.</summary>
    public DicomDataset? Parent => _parent;

    /// <summary>For an item: the sequence it belongs to.</summary>
    public DicomTag? SequenceTag => _kind == RootKind.Item ? _sequenceTag : null;

    /// <summary>For an item: its 0-based index in the sequence.</summary>
    public int ItemIndex => _kind == RootKind.Item ? _itemIndex : -1;

    /// <summary>Resolves the native pointer. The caller holds a lease on the file handle.</summary>
    internal nint Resolve(nint file)
    {
        nint ds;
        switch (_kind)
        {
            case RootKind.Main:
                Status.Check(Native.siaara_file_dataset(file, out ds));
                return ds;
            case RootKind.Meta:
                Status.Check(Native.siaara_file_meta(file, out ds));
                return ds;
            default:
                nint parent = _parent!.Resolve(file);
                Status.Check(Native.siaara_dataset_item(parent, _sequenceTag.Value, (uint)_itemIndex, out ds));
                return ds;
        }
    }

    // ------------------------------------------------------------------ structure

    /// <summary>Number of top-level elements.</summary>
    public int Count
    {
        get
        {
            using var l = new HandleLease(_file.Handle);
            Status.Check(Native.siaara_dataset_element_count(Resolve(l.Pointer), out uint n));
            return (int)n;
        }
    }

    /// <summary>The tags of the top-level elements, in ascending order (a snapshot).</summary>
    public IReadOnlyList<DicomTag> Tags
    {
        get
        {
            using var l = new HandleLease(_file.Handle);
            nint ds = Resolve(l.Pointer);
            Status.Check(Native.siaara_dataset_element_count(ds, out uint n));
            var tags = new DicomTag[n];
            for (uint i = 0; i < n; i++)
            {
                Status.Check(Native.siaara_dataset_element_tag(ds, i, out uint t));
                tags[i] = new DicomTag(t);
            }
            return tags;
        }
    }

    /// <summary>True when the element is present.</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>True when present.</returns>
    public bool Contains(DicomTag tag)
    {
        using var l = new HandleLease(_file.Handle);
        return Native.siaara_dataset_contains(Resolve(l.Pointer), tag.Value) != 0;
    }

    /// <summary>The VR of a present element, e.g. "PN".</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The two-character VR.</returns>
    public string GetVR(DicomTag tag)
    {
        using var l = new HandleLease(_file.Handle);
        Span<byte> vr = stackalloc byte[3];
        Status.Check(Native.siaara_dataset_vr(Resolve(l.Pointer), tag.Value, vr));
        return DicomDictionary.Chars(vr);
    }

    /// <summary>The value multiplicity of a present element.</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The number of values.</returns>
    public int GetValueMultiplicity(DicomTag tag)
    {
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_vm(Resolve(l.Pointer), tag.Value, out uint n));
        return (int)n;
    }

    // ------------------------------------------------------------------ text

    /// <summary>
    /// All values of a text element as one string (values separated by backslashes), decoded to
    /// UTF-8 with the character set; null when the element is absent. Setting stores the value in
    /// the data set's character set with the dictionary VR; setting null removes the element.
    /// </summary>
    /// <param name="tag">The tag.</param>
    public string? this[DicomTag tag]
    {
        get
        {
            if (!Contains(tag))
            {
                return null;
            }
            int vm = GetValueMultiplicity(tag);
            if (vm <= 1)
            {
                return TryGetString(tag, 0, out string? v) ? v : null;
            }
            string[] all = GetStrings(tag);
            return string.Join('\\', all);
        }
        set
        {
            if (value is null)
            {
                _ = Remove(tag);
            }
            else
            {
                SetString(tag, value);
            }
        }
    }

    /// <summary>Value index of a text element, decoded to UTF-8 with the character set, padding removed.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>The value.</returns>
    /// <exception cref="SiaaraException">Absent element (NotFound) or not text (TypeMismatch).</exception>
    public string GetString(DicomTag tag, int index = 0)
    {
        int st = TryGetStringCore(tag, index, out string v);
        Status.Check(st);
        return v;
    }

    /// <summary>Value 0 of a text element, without throwing.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="value">The value, when the method returns true.</param>
    /// <returns>True when the value was read.</returns>
    public bool TryGetString(DicomTag tag, [NotNullWhen(true)] out string? value) => TryGetString(tag, 0, out value);

    /// <summary>A value of a text element, without throwing.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="index">0-based value index.</param>
    /// <param name="value">The value, when the method returns true.</param>
    /// <returns>True when the value was read.</returns>
    public bool TryGetString(DicomTag tag, int index, [NotNullWhen(true)] out string? value)
    {
        if (TryGetStringCore(tag, index, out string v) == Status.Ok)
        {
            value = v;
            return true;
        }
        value = null;
        return false;
    }

    private int TryGetStringCore(DicomTag tag, int index, out string value)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        byte[]? inherited = _kind == RootKind.Item ? _parent!.EffectiveCharacterSet() : null;
        using var l = new HandleLease(_file.Handle);
        var state = (Ds: Resolve(l.Pointer), Tag: tag.Value, Index: (uint)index, Inherited: inherited);
        if (inherited is null)
        {
            return Status.TryReadString(state, static (s, b, out n) =>
                Native.siaara_dataset_get_utf8(s.Ds, s.Tag, s.Index, b, out n), out value);
        }
        return Status.TryReadString(state, static (s, b, out n) =>
            Native.siaara_dataset_get_utf8_cs(s.Ds, s.Tag, s.Index, s.Inherited, b, out n), out value);
    }

    /// <summary>Every value of a text element (decoded with the character set).</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The values (empty for an empty element).</returns>
    public string[] GetStrings(DicomTag tag)
    {
        if (_kind != RootKind.Item || _parent!.EffectiveCharacterSet() is null)
        {
            // One native call for every value (siaara_dataset_get_strings).
            return GetStringsCore(tag, utf8: true);
        }
        int vm = GetValueMultiplicity(tag);
        var values = new string[vm];
        for (int i = 0; i < vm; i++)
        {
            values[i] = GetString(tag, i);
        }
        return values;
    }

    /// <summary>Every value as stored (no character set conversion), in one native call.</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The values (empty for an empty element).</returns>
    public string[] GetRawStrings(DicomTag tag) => GetStringsCore(tag, utf8: false);

    private string[] GetStringsCore(DicomTag tag, bool utf8)
    {
        using var l = new HandleLease(_file.Handle);
        var state = (Ds: Resolve(l.Pointer), Tag: tag.Value, Utf8: utf8 ? 1 : 0);
        byte[] all = Status.ReadArray<byte, (nint Ds, uint Tag, int Utf8)>(state, static (s, b, out n) =>
            Native.siaara_dataset_get_strings(s.Ds, s.Tag, s.Utf8, b, out n, out _));
        var values = new List<string>();
        int start = 0;
        for (int i = 0; i < all.Length; i++)
        {
            if (all[i] == 0)
            {
                values.Add(Encoding.UTF8.GetString(all, start, i - start));
                start = i + 1;
            }
        }
        return values.ToArray();
    }

    /// <summary>A value as stored (no character set conversion); use for UIDs and code strings.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>The value bytes interpreted as UTF-8.</returns>
    public string GetRawString(DicomTag tag, int index = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_file.Handle);
        var state = (Ds: Resolve(l.Pointer), Tag: tag.Value, Index: (uint)index);
        return Status.ReadString(state, static (s, b, out n) =>
            Native.siaara_dataset_get_string(s.Ds, s.Tag, s.Index, b, out n));
    }

    /// <summary>
    /// The (0008,0005) value that applies to this data set: its own, or the nearest enclosing data
    /// set's. Null when none has one.
    /// </summary>
    internal byte[]? EffectiveCharacterSet()
    {
        if (TryGetBytes(s_specificCharacterSet, out byte[]? own))
        {
            return own;
        }
        return _kind == RootKind.Item ? _parent!.EffectiveCharacterSet() : null;
    }

    // ------------------------------------------------------------------ numbers

    /// <summary>A value as an integer (IS, US, SS, UL, SL, ...).</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>The value.</returns>
    public long GetInt64(DicomTag tag, int index = 0)
    {
        int st = TryGetInt64Core(tag, index, out long v);
        Status.Check(st);
        return v;
    }

    /// <summary>A value as an integer, without throwing.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="value">The value.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>True when the value was read.</returns>
    public bool TryGetInt64(DicomTag tag, out long value, int index = 0) => TryGetInt64Core(tag, index, out value) == Status.Ok;

    private int TryGetInt64Core(DicomTag tag, int index, out long value)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_file.Handle);
        return Native.siaara_dataset_get_int64(Resolve(l.Pointer), tag.Value, (uint)index, out value);
    }

    /// <summary>A value as a 32-bit integer.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>The value.</returns>
    /// <exception cref="OverflowException">The value does not fit.</exception>
    public int GetInt32(DicomTag tag, int index = 0) => checked((int)GetInt64(tag, index));

    /// <summary>A value as a double (DS, FL, FD, IS, ...).</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>The value.</returns>
    public double GetDouble(DicomTag tag, int index = 0)
    {
        int st = TryGetDoubleCore(tag, index, out double v);
        Status.Check(st);
        return v;
    }

    /// <summary>A value as a double, without throwing.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="value">The value.</param>
    /// <param name="index">0-based value index.</param>
    /// <returns>True when the value was read.</returns>
    public bool TryGetDouble(DicomTag tag, out double value, int index = 0) => TryGetDoubleCore(tag, index, out value) == Status.Ok;

    private int TryGetDoubleCore(DicomTag tag, int index, out double value)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_file.Handle);
        return Native.siaara_dataset_get_double(Resolve(l.Pointer), tag.Value, (uint)index, out value);
    }

    // ------------------------------------------------------------------ bytes

    /// <summary>A copy of the raw value bytes (as stored; little endian).</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>The bytes.</returns>
    /// <exception cref="SiaaraException">Absent (NotFound), or a sequence / encapsulated value (TypeMismatch).</exception>
    public byte[] GetBytes(DicomTag tag)
    {
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_get_raw(Resolve(l.Pointer), tag.Value, out nint p, out nuint n));
        return NativeSpan.Bytes(p, n).ToArray();
    }

    /// <summary>A copy of the raw value bytes, without throwing.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="value">The bytes, when the method returns true.</param>
    /// <returns>True when the value was read.</returns>
    public bool TryGetBytes(DicomTag tag, [NotNullWhen(true)] out byte[]? value)
    {
        using var l = new HandleLease(_file.Handle);
        if (Native.siaara_dataset_get_raw(Resolve(l.Pointer), tag.Value, out nint p, out nuint n) != Status.Ok)
        {
            value = null;
            return false;
        }
        value = NativeSpan.Bytes(p, n).ToArray();
        return true;
    }

    /// <summary>Copies the raw value bytes into destination without allocating.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="destination">Output buffer.</param>
    /// <returns>The value length; when it exceeds destination.Length nothing is copied.</returns>
    public int CopyBytes(DicomTag tag, Span<byte> destination)
    {
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_get_raw(Resolve(l.Pointer), tag.Value, out nint p, out nuint n));
        ReadOnlySpan<byte> src = NativeSpan.Bytes(p, n);
        if (src.Length <= destination.Length)
        {
            src.CopyTo(destination);
        }
        return src.Length;
    }

    // ------------------------------------------------------------------ sequences

    /// <summary>Number of items of a sequence.</summary>
    /// <param name="tag">The sequence tag.</param>
    /// <returns>The item count.</returns>
    public int GetItemCount(DicomTag tag)
    {
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_item_count(Resolve(l.Pointer), tag.Value, out uint n));
        return (int)n;
    }

    /// <summary>A sequence item.</summary>
    /// <param name="tag">The sequence tag.</param>
    /// <param name="index">0-based item index.</param>
    /// <returns>The item data set.</returns>
    public DicomDataset GetItem(DicomTag tag, int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using (var l = new HandleLease(_file.Handle))
        {
            Status.Check(Native.siaara_dataset_item(Resolve(l.Pointer), tag.Value, (uint)index, out _));
        }
        return new DicomDataset(this, tag, index);
    }

    /// <summary>
    /// Appends an empty item to a sequence (created with VR SQ when absent) and returns it for
    /// writing. Like every item, it is located again by index on each call.
    /// </summary>
    /// <param name="tag">The sequence tag.</param>
    /// <returns>The new item.</returns>
    public DicomDataset AppendItem(DicomTag tag)
    {
        uint index;
        using (var l = new HandleLease(_file.Handle))
        {
            Status.Check(Native.siaara_dataset_append_item(Resolve(l.Pointer), tag.Value, out _, out index));
        }
        return new DicomDataset(this, tag, (int)index);
    }

    /// <summary>Removes item index of a sequence (later items move down by one).</summary>
    /// <param name="tag">The sequence tag.</param>
    /// <param name="index">0-based item index.</param>
    public void RemoveItem(DicomTag tag, int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_remove_item(Resolve(l.Pointer), tag.Value, (uint)index));
    }

    /// <summary>All items of a sequence; an empty list when the element is absent.</summary>
    /// <param name="tag">The sequence tag.</param>
    /// <returns>The items.</returns>
    public IReadOnlyList<DicomDataset> GetSequence(DicomTag tag)
    {
        if (!Contains(tag))
        {
            return Array.Empty<DicomDataset>();
        }
        int n = GetItemCount(tag);
        var items = new DicomDataset[n];
        for (int i = 0; i < n; i++)
        {
            items[i] = new DicomDataset(this, tag, i);
        }
        return items;
    }

    // ------------------------------------------------------------------ setters

    /// <summary>Stores text with the dictionary VR, encoded in the data set's character set.</summary>
    /// <param name="tag">The tag (must be in the dictionary).</param>
    /// <param name="value">All values, backslash separated.</param>
    public void SetString(DicomTag tag, string value)
    {
        DicomDictionaryEntry e = DicomDictionary.Lookup(tag);
        if (e.VR.Length != 2)
        {
            throw new ArgumentException("The VR of " + tag + " depends on context; pass it explicitly.", nameof(tag));
        }
        SetString(tag, e.VR, value);
    }

    /// <summary>
    /// Stores text encoded in the data set's character set (the inherited one for items).
    /// </summary>
    /// <param name="tag">The tag.</param>
    /// <param name="vr">Two-character VR, e.g. "PN".</param>
    /// <param name="value">All values, backslash separated.</param>
    /// <exception cref="SiaaraException">A character cannot be represented (Unsupported); the data set is unchanged.</exception>
    public void SetString(DicomTag tag, string vr, string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        byte[] v = Vr(vr);
        byte[] utf8 = Encoding.UTF8.GetBytes(value);
        byte[]? inherited = _kind == RootKind.Item ? _parent!.EffectiveCharacterSet() : null;
        using var l = new HandleLease(_file.Handle);
        nint ds = Resolve(l.Pointer);
        Status.Check(inherited is null
            ? Native.siaara_dataset_set_utf8(ds, tag.Value, v, utf8)
            : Native.siaara_dataset_set_utf8_cs(ds, tag.Value, v, utf8, inherited));
    }

    /// <summary>Stores a value's bytes exactly as given (UTF-8 of value), without character set conversion.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="vr">Two-character VR.</param>
    /// <param name="value">The value.</param>
    public void SetRawString(DicomTag tag, string vr, string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        byte[] v = Vr(vr);
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_set_string(Resolve(l.Pointer), tag.Value, v, Encoding.UTF8.GetBytes(value)));
    }

    /// <summary>Stores a US value.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="value">The value.</param>
    public void SetUInt16(DicomTag tag, ushort value)
    {
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_set_uint16(Resolve(l.Pointer), tag.Value, value));
    }

    /// <summary>Stores a UL value.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="value">The value.</param>
    public void SetUInt32(DicomTag tag, uint value)
    {
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_set_uint32(Resolve(l.Pointer), tag.Value, value));
    }

    /// <summary>Stores raw value bytes (copied) with a VR, e.g. "OW" for Pixel Data.</summary>
    /// <param name="tag">The tag.</param>
    /// <param name="vr">Two-character VR.</param>
    /// <param name="data">The value bytes (little endian).</param>
    public void SetBytes(DicomTag tag, string vr, ReadOnlySpan<byte> data)
    {
        byte[] v = Vr(vr);
        using var l = new HandleLease(_file.Handle);
        Status.Check(Native.siaara_dataset_set_bytes(Resolve(l.Pointer), tag.Value, v, data));
    }

    /// <summary>Removes an element.</summary>
    /// <param name="tag">The tag.</param>
    /// <returns>True when it was present.</returns>
    public bool Remove(DicomTag tag)
    {
        using var l = new HandleLease(_file.Handle);
        int st = Native.siaara_dataset_remove(Resolve(l.Pointer), tag.Value);
        if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return false;
        }
        Status.Check(st);
        return true;
    }

    // ------------------------------------------------------------------ JSON

    /// <summary>Encodes the data set as one DICOM JSON object (PS3.18 Annex F, compact UTF-8).</summary>
    /// <param name="options">Encoder options.</param>
    /// <returns>The JSON text.</returns>
    public string ToJson(DicomJsonOptions options = DicomJsonOptions.None)
    {
        using var l = new HandleLease(_file.Handle);
        var state = (Ds: Resolve(l.Pointer), Flags: (uint)options);
        return Status.ReadString(state, static (s, b, out n) => Native.siaara_dataset_to_json(s.Ds, s.Flags, b, out n));
    }

    private static byte[] Vr(string vr)
    {
        ArgumentNullException.ThrowIfNull(vr);
        if (vr.Length != 2 || !char.IsAsciiLetterUpper(vr[0]) || !char.IsAsciiLetterUpper(vr[1]))
        {
            throw new ArgumentException("A VR is two upper-case letters, e.g. \"PN\".", nameof(vr));
        }
        return new[] { (byte)vr[0], (byte)vr[1], (byte)0 };
    }
}
