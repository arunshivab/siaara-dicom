using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>Options of the Basic Application Level Confidentiality Profile (PS3.15 Table E.1-1).</summary>
[Flags]
public enum DeidentificationOptions
{
    /// <summary>Basic profile only.</summary>
    None = 0,
    /// <summary>Retain Safe Private Option.</summary>
    RetainSafePrivate = (int)C.SIAARA_DEID_RETAIN_SAFE_PRIVATE,
    /// <summary>Retain UIDs Option.</summary>
    RetainUids = (int)C.SIAARA_DEID_RETAIN_UIDS,
    /// <summary>Retain Device Identity Option.</summary>
    RetainDeviceIdentity = (int)C.SIAARA_DEID_RETAIN_DEVICE_IDENTITY,
    /// <summary>Retain Institution Identity Option.</summary>
    RetainInstitutionIdentity = (int)C.SIAARA_DEID_RETAIN_INSTITUTION_IDENTITY,
    /// <summary>Retain Patient Characteristics Option.</summary>
    RetainPatientCharacteristics = (int)C.SIAARA_DEID_RETAIN_PATIENT_CHARACTERISTICS,
    /// <summary>Retain Longitudinal Temporal Information with Full Dates Option.</summary>
    RetainLongitudinalFullDates = (int)C.SIAARA_DEID_RETAIN_LONG_FULL_DATES,
    /// <summary>Retain Longitudinal Temporal Information with Modified Dates Option.</summary>
    RetainLongitudinalModifiedDates = (int)C.SIAARA_DEID_RETAIN_LONG_MODIFIED_DATES,
    /// <summary>Clean Descriptors Option.</summary>
    CleanDescriptors = (int)C.SIAARA_DEID_CLEAN_DESCRIPTORS,
    /// <summary>Clean Structured Content Option.</summary>
    CleanStructuredContent = (int)C.SIAARA_DEID_CLEAN_STRUCTURED_CONTENT,
    /// <summary>Clean Graphics Option.</summary>
    CleanGraphics = (int)C.SIAARA_DEID_CLEAN_GRAPHICS,
}

/// <summary>How X/Z, X/D, X/Z/D, Z/D and X/Z/U* actions are resolved.</summary>
public enum CombinedActionPolicy
{
    /// <summary>D, Z or U: never breaks a Type 1/2 requirement (default).</summary>
    Conformant = C.SIAARA_DEID_COMBINED_CONFORMANT,
    /// <summary>X wherever allowed.</summary>
    Remove = C.SIAARA_DEID_COMBINED_REMOVE,
}

/// <summary>Result flags of a de-identification.</summary>
[Flags]
public enum DeidentificationResults
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>Pixel data may still carry burned-in annotation.</summary>
    BurnedInAnnotationRemains = (int)C.SIAARA_DEID_BURNED_IN_ANNOTATION_REMAINS,
    /// <summary>Recognizable Visual Features is YES.</summary>
    RecognizableFeaturesRemain = (int)C.SIAARA_DEID_RECOGNIZABLE_FEATURES_REMAIN,
    /// <summary>Either of the two above: do not release without review.</summary>
    NotFullyDeidentified = (int)C.SIAARA_DEID_NOT_FULLY_DEIDENTIFIED,
    /// <summary>Regions were blanked.</summary>
    PixelDataCleaned = (int)C.SIAARA_DEID_PIXEL_DATA_CLEANED,
    /// <summary>Invalid dates / numbers were emptied.</summary>
    UnparseableValuesRemoved = (int)C.SIAARA_DEID_UNPARSEABLE_VALUES_REMOVED,
}

/// <summary>A rectangle to blank in native Pixel Data (Clean Pixel Data Option).</summary>
/// <param name="X">Column of the top-left pixel.</param>
/// <param name="Y">Row of the top-left pixel.</param>
/// <param name="Width">Width.</param>
/// <param name="Height">Height.</param>
/// <param name="Frame">0-based frame, or null for every frame.</param>
public readonly record struct PixelRegion(int X, int Y, int Width, int Height, int? Frame = null);

/// <summary>
/// De-identification (PS3.15 Annex E) with a secret key that makes UID replacement and date
/// shifting repeatable. The key is copied and wiped on Dispose.
/// </summary>
public sealed class Deidentifier : IDisposable
{
    private byte[] _key;

    /// <summary>Creates a de-identifier.</summary>
    /// <param name="key">Secret key, at least 16 bytes.</param>
    public Deidentifier(ReadOnlySpan<byte> key)
    {
        if (key.Length < 16)
        {
            throw new ArgumentException("The key needs at least 16 bytes.", nameof(key));
        }
        _key = key.ToArray();
    }

    /// <summary>Profile options.</summary>
    public DeidentificationOptions Options { get; set; }

    /// <summary>Resolution of combined actions.</summary>
    public CombinedActionPolicy CombinedPolicy { get; set; }

    /// <summary>The caller asserts that CleanRegions cover all burned-in text.</summary>
    public bool PixelDataClean { get; set; }

    /// <summary>Rectangles to blank.</summary>
    public IList<PixelRegion> CleanRegions { get; } = new List<PixelRegion>();

    /// <summary>Words kept by the clean action (ASCII, separated by spaces or line breaks).</summary>
    public string? SafeWords { get; set; }

    /// <summary>Modified Dates offset range in days; 0 = default (3652).</summary>
    public int MaxShiftDays { get; set; }

    /// <summary>De-identifies the file in place (data set, file meta, preamble); unchanged on failure.</summary>
    /// <param name="file">The file.</param>
    /// <returns>Result flags: check NotFullyDeidentified before releasing.</returns>
    public DeidentificationResults Deidentify(DicomFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        ObjectDisposedException.ThrowIf(_key.Length == 0, this);
        var regions = new siaara_deid_region[CleanRegions.Count];
        for (int i = 0; i < regions.Length; i++)
        {
            PixelRegion r = CleanRegions[i];
            regions[i] = new siaara_deid_region
            {
                frame = r.Frame is int f ? (uint)f : C.SIAARA_DEID_ALL_FRAMES,
                x = (uint)r.X,
                y = (uint)r.Y,
                width = (uint)r.Width,
                height = (uint)r.Height,
            };
        }
        using var mem = new NativeAllocations();
        byte[] words = SafeWords is null ? Array.Empty<byte>() : Encoding.ASCII.GetBytes(SafeWords);
        var o = new siaara_deid_options
        {
            struct_size = StructSize.Of<siaara_deid_options>(),
            flags = (uint)Options,
            key = mem.Array<byte>(_key),
            key_len = (nuint)_key.Length,
            combined_policy = (int)CombinedPolicy,
            pixel_data_clean = PixelDataClean ? 1 : 0,
            regions = mem.Array<siaara_deid_region>(regions),
            region_count = (nuint)regions.Length,
            safe_words = mem.Array<byte>(words),
            safe_words_len = (nuint)words.Length,
            max_shift_days = (uint)Math.Max(0, MaxShiftDays),
        };
        using var l = new HandleLease(file.Handle);
        Status.Check(Native.siaara_deidentify(l.Pointer, in o, out uint flags));
        return (DeidentificationResults)flags;
    }

    /// <summary>The replacement Deidentify uses for a UID with this key.</summary>
    /// <param name="uid">The original UID.</param>
    /// <returns>The replacement UID.</returns>
    public string GetReplacementUid(string uid)
    {
        ArgumentException.ThrowIfNullOrEmpty(uid);
        ObjectDisposedException.ThrowIf(_key.Length == 0, this);
        var state = (Key: _key, Uid: Encoding.ASCII.GetBytes(uid));
        return Status.ReadString(state, static (s, b, out n) => Native.siaara_deid_replacement_uid(s.Key, s.Uid, b, out n));
    }

    /// <summary>Wipes the key.</summary>
    public void Dispose()
    {
        CryptographicOperations.ZeroMemory(_key);
        _key = Array.Empty<byte>();
        GC.SuppressFinalize(this);
    }
}
