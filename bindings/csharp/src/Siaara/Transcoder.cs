using System;
using System.Runtime.CompilerServices;
using Siaara.Interop;

namespace Siaara;

/// <summary>Offset table written into encapsulated pixel data.</summary>
public enum OffsetTableKind
{
    /// <summary>Basic Offset Table, Extended when offsets exceed 32 bits.</summary>
    Automatic = C.SIAARA_OFFSET_TABLE_AUTO,
    /// <summary>Basic Offset Table.</summary>
    Basic = C.SIAARA_OFFSET_TABLE_BASIC,
    /// <summary>Extended Offset Table.</summary>
    Extended = C.SIAARA_OFFSET_TABLE_EXTENDED,
    /// <summary>Empty Basic Offset Table.</summary>
    Empty = C.SIAARA_OFFSET_TABLE_EMPTY,
}

/// <summary>Codec parameters for <see cref="Transcoder"/> (siaara_transcode_params).</summary>
public sealed record TranscodeParameters
{
    /// <summary>Lossy JPEG / JPEG 2000 quality 1..100; 0 = default (90).</summary>
    public int Quality { get; init; }
    /// <summary>Code lossily in syntaxes that allow both (JPEG-LS .81, JPEG 2000 .91, HTJ2K .203).</summary>
    public bool Lossy { get; init; }
    /// <summary>JPEG-LS NEAR for lossy .81; 0 = default (2).</summary>
    public int NearLossless { get; init; }
    /// <summary>
    /// Allow lossy compression at all. False (the default here) refuses any transcode that would
    /// be lossy (SiaaraErrorCode.Rejected). Passing null parameters instead uses the C default,
    /// which allows lossy coding for lossy-only syntaxes such as JPEG baseline.
    /// </summary>
    public bool AllowLossy { get; init; }
    /// <summary>JPEG lossless (.57) predictor 1..7; 0 = default (1).</summary>
    public int Predictor { get; init; }
    /// <summary>JPEG lossy colour: true for 4:2:2 (YBR_FULL_422), false for 4:4:4.</summary>
    public bool ChromaSubsampling { get; init; }
    /// <summary>Offset table of the encapsulated output.</summary>
    public OffsetTableKind OffsetTable { get; init; }
    /// <summary>Keep the SOP Instance UID when lossy compression is applied (default false: the
    /// result is a new, derived image with a new UID that references its predecessor; PS3.3 C.7.6.1.1.5).</summary>
    public bool KeepUidOnLossy { get; init; }

    internal siaara_transcode_params ToNative() => new()
    {
        struct_size = StructSize.Of<siaara_transcode_params>(),
        quality = Quality,
        lossy = Lossy ? 1 : 0,
        near_lossless = NearLossless,
        allow_lossy = AllowLossy ? 1 : 0,
        predictor = Predictor,
        chroma_subsampling = ChromaSubsampling ? 1 : 0,
        offset_table = (int)OffsetTable,
        keep_uid_on_lossy = KeepUidOnLossy ? 1 : 0,
    };
}

/// <summary>What a lossy transcode did with the history lists Lossy Image Compression Ratio (0028,2112)
/// and Method (0028,2114), which hold one value per lossy step in the same order (PS3.3 C.7.6.1.1.5.1).</summary>
public enum LossyHistory
{
    /// <summary>No lossy compression was applied: both attributes are untouched.</summary>
    NotApplicable = C.SIAARA_LOSSY_HISTORY_NOT_APPLICABLE,
    /// <summary>The source had as many Ratio as Method values (or neither): one value appended to each.</summary>
    Aligned = C.SIAARA_LOSSY_HISTORY_ALIGNED,
    /// <summary>LOSSY_METHOD_FILLED: the source had one more Ratio than Method value and its own lossy
    /// transfer syntax proves the method of that step; that Method value was filled in
    /// (<see cref="TranscodeReport.LossyMethodFilled"/>), then one value appended to each.</summary>
    MethodFilled = C.SIAARA_LOSSY_HISTORY_METHOD_FILLED,
    /// <summary>LOSSY_HISTORY_MISALIGNED: any other mismatch. The Ratio was appended and Method was left
    /// exactly as it was; nothing is guessed.</summary>
    Misaligned = C.SIAARA_LOSSY_HISTORY_MISALIGNED,
}

/// <summary>What a transcode did (siaara_transcode_report).</summary>
/// <param name="PixelDataRecoded">Frames were decoded and encoded again.</param>
/// <param name="LossyApplied">This transcode applied lossy compression.</param>
/// <param name="SourceLossy">The source was lossy already (attribute or codestream).</param>
/// <param name="LossyHistory">What was done with the Ratio / Method history lists.</param>
/// <param name="EncodedBytes">Total encoded frame bytes (encapsulated targets).</param>
/// <param name="NativeBytes">Total decoded frame bytes.</param>
/// <param name="NewSopInstanceUid">The new UID a lossy transcode gave the image, else empty.</param>
/// <param name="LossyMethodFilled">MethodFilled: the defined term written for the earlier step, else empty.</param>
/// <param name="LossyHistoryNote">MethodFilled / Misaligned: "LOSSY_METHOD_FILLED: ..." or
/// "LOSSY_HISTORY_MISALIGNED: ..." with the value counts found, what was written and why; else empty.</param>
public sealed record TranscodeReport(bool PixelDataRecoded, bool LossyApplied, bool SourceLossy, LossyHistory LossyHistory,
                                     long EncodedBytes, long NativeBytes, string NewSopInstanceUid,
                                     string LossyMethodFilled, string LossyHistoryNote)
{
    internal static TranscodeReport From(in siaara_transcode_report r) =>
        new(r.pixel_data_recoded != 0, r.lossy_applied != 0, r.source_lossy != 0, (LossyHistory)r.lossy_history,
            (long)r.encoded_bytes, (long)r.native_bytes, DicomDictionary.Chars(r.new_sop_instance_uid),
            DicomDictionary.Chars(r.lossy_method_filled), DicomDictionary.Chars(r.lossy_history_note));
}

/// <summary>Pixel data transcoding (siaara_file_transcode, siaara_file_transcode_ex).</summary>
public static class Transcoder
{
    /// <summary>
    /// Converts a file's pixel data to transferSyntaxUid in place and updates the Image Pixel,
    /// Lossy Image Compression and file meta attributes. On failure the file is unchanged.
    /// </summary>
    /// <param name="file">The file.</param>
    /// <param name="transferSyntaxUid">Target transfer syntax, e.g. <see cref="DicomTransferSyntaxes.RLELossless"/>.</param>
    /// <param name="parameters">Codec parameters, or null for the C defaults.</param>
    /// <exception cref="SiaaraException">Unsupported (cannot be represented) or Rejected (lossy not allowed).</exception>
    public static void Transcode(DicomFile file, string transferSyntaxUid, TranscodeParameters? parameters = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentException.ThrowIfNullOrEmpty(transferSyntaxUid);
        byte[] ts = Status.Utf8Z(transferSyntaxUid)!;
        using var l = new HandleLease(file.Handle);
        if (parameters is null)
        {
            Status.Check(Native.siaara_file_transcode(l.Pointer, ts, in Unsafe.NullRef<siaara_transcode_params>()));
        }
        else
        {
            siaara_transcode_params p = parameters.ToNative();
            Status.Check(Native.siaara_file_transcode(l.Pointer, ts, in p));
        }
    }

    /// <summary>
    /// As <see cref="Transcode"/>, returning what was done: whether lossy compression was applied, the
    /// new SOP Instance UID and how the lossy history lists were handled (<see cref="LossyHistory"/>).
    /// </summary>
    /// <param name="file">The file.</param>
    /// <param name="transferSyntaxUid">Target transfer syntax.</param>
    /// <param name="parameters">Codec parameters, or null for the C defaults.</param>
    /// <returns>The report.</returns>
    /// <exception cref="SiaaraException">Unsupported (cannot be represented) or Rejected (lossy not allowed).</exception>
    public static TranscodeReport TranscodeWithReport(DicomFile file, string transferSyntaxUid,
                                                      TranscodeParameters? parameters = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentException.ThrowIfNullOrEmpty(transferSyntaxUid);
        byte[] ts = Status.Utf8Z(transferSyntaxUid)!;
        using var l = new HandleLease(file.Handle);
        var r = new siaara_transcode_report { struct_size = StructSize.Of<siaara_transcode_report>() };
        if (parameters is null)
        {
            Status.Check(Native.siaara_file_transcode_ex(l.Pointer, ts, in Unsafe.NullRef<siaara_transcode_params>(), ref r));
        }
        else
        {
            siaara_transcode_params p = parameters.ToNative();
            Status.Check(Native.siaara_file_transcode_ex(l.Pointer, ts, in p, ref r));
        }
        return TranscodeReport.From(in r);
    }

    /// <summary>Transcodes, returning the status instead of throwing.</summary>
    /// <param name="file">The file.</param>
    /// <param name="transferSyntaxUid">Target transfer syntax.</param>
    /// <param name="parameters">Codec parameters, or null.</param>
    /// <returns>The status (Ok on success).</returns>
    public static SiaaraErrorCode TryTranscode(DicomFile file, string transferSyntaxUid, TranscodeParameters? parameters = null)
    {
        try
        {
            Transcode(file, transferSyntaxUid, parameters);
            return SiaaraErrorCode.Ok;
        }
        catch (SiaaraException e)
        {
            return e.ErrorCode;
        }
    }
}
