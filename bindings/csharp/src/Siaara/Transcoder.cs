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
    };
}

/// <summary>Pixel data transcoding (siaara_file_transcode).</summary>
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
