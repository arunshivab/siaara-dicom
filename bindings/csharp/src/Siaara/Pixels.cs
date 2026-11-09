using System;
using Siaara.Interop;

namespace Siaara;

/// <summary>Output layout of <see cref="DicomFile.DecodeFrame(int, Span{byte}, FrameFormat)"/>.</summary>
public enum FrameFormat
{
    /// <summary>
    /// DICOM native layout of the file's attributes: little-endian samples of Bits Allocated bits,
    /// Planar Configuration as stored, 1-bit frames bit-packed, YBR 4:2:2 as stored.
    /// </summary>
    Native = C.SIAARA_FRAME_NATIVE,
    /// <summary>float32 per sample after the Modality LUT / rescale (grayscale), colour-by-pixel.</summary>
    Float32Modality = C.SIAARA_FRAME_FLOAT32_MODALITY,
}

/// <summary>Sample type of the pixel data.</summary>
public enum PixelSampleKind
{
    /// <summary>Integer samples.</summary>
    Integral = C.SIAARA_SAMPLES_INTEGER,
    /// <summary>Float Pixel Data (32-bit IEEE).</summary>
    SinglePrecision = C.SIAARA_SAMPLES_FLOAT32,
    /// <summary>Double Float Pixel Data (64-bit IEEE).</summary>
    DoublePrecision = C.SIAARA_SAMPLES_FLOAT64,
}

/// <summary>The validated Image Pixel module (siaara_pixel_info).</summary>
public sealed record DicomPixelInfo
{
    /// <summary>Rows.</summary>
    public int Rows { get; init; }
    /// <summary>Columns.</summary>
    public int Columns { get; init; }
    /// <summary>Samples per Pixel.</summary>
    public int SamplesPerPixel { get; init; }
    /// <summary>Bits Allocated.</summary>
    public int BitsAllocated { get; init; }
    /// <summary>Bits Stored.</summary>
    public int BitsStored { get; init; }
    /// <summary>High Bit.</summary>
    public int HighBit { get; init; }
    /// <summary>True for two's complement samples.</summary>
    public bool IsSigned { get; init; }
    /// <summary>True for colour-by-plane.</summary>
    public bool IsPlanar { get; init; }
    /// <summary>Number of Frames.</summary>
    public int NumberOfFrames { get; init; }
    /// <summary>Lossy Image Compression "01" or a lossy transfer syntax.</summary>
    public bool IsLossy { get; init; }
    /// <summary>Sample type.</summary>
    public PixelSampleKind SampleKind { get; init; }
    /// <summary>True when the pixel data is encapsulated (compressed).</summary>
    public bool IsEncapsulated { get; init; }
    /// <summary>Bytes of one frame in <see cref="FrameFormat.Native"/>.</summary>
    public long FrameBytes { get; init; }
    /// <summary>Photometric Interpretation.</summary>
    public string PhotometricInterpretation { get; init; } = string.Empty;

    internal static DicomPixelInfo FromNative(siaara_pixel_info p) => new()
    {
        Rows = (int)p.rows,
        Columns = (int)p.columns,
        SamplesPerPixel = (int)p.samples_per_pixel,
        BitsAllocated = (int)p.bits_allocated,
        BitsStored = (int)p.bits_stored,
        HighBit = (int)p.high_bit,
        IsSigned = p.pixel_representation != 0,
        IsPlanar = p.planar_configuration != 0,
        NumberOfFrames = (int)p.number_of_frames,
        IsLossy = p.lossy != 0,
        SampleKind = (PixelSampleKind)p.sample_kind,
        IsEncapsulated = p.encapsulated != 0,
        FrameBytes = (long)p.frame_bytes,
        PhotometricInterpretation = DicomDictionary.Chars(p.photometric),
    };
}

/// <summary>How the VOI (window) is chosen for rendering.</summary>
public enum VoiMode
{
    /// <summary>The file's first window / VOI LUT; the full pixel range if it has none.</summary>
    FromFile = C.SIAARA_VOI_FILE,
    /// <summary><see cref="RenderOptions.WindowCenter"/> / <see cref="RenderOptions.WindowWidth"/> (LINEAR).</summary>
    Explicit = C.SIAARA_VOI_EXPLICIT,
    /// <summary>Minimum to maximum of the frame.</summary>
    FullRange = C.SIAARA_VOI_FULL_RANGE,
}

/// <summary>Options of the 2D presentation pipeline (siaara_render_options).</summary>
public sealed record RenderOptions
{
    /// <summary>0-based frame.</summary>
    public int Frame { get; init; }
    /// <summary>VOI selection.</summary>
    public VoiMode VoiMode { get; init; }
    /// <summary>Invert on top of MONOCHROME1 / Presentation LUT Shape.</summary>
    public bool Invert { get; init; }
    /// <summary>Window center for <see cref="VoiMode.Explicit"/>.</summary>
    public double WindowCenter { get; init; }
    /// <summary>Window width (at least 1) for <see cref="VoiMode.Explicit"/>.</summary>
    public double WindowWidth { get; init; }
    /// <summary>Do not burn in overlays.</summary>
    public bool NoOverlays { get; init; }
    /// <summary>Do not apply display shutters.</summary>
    public bool NoShutters { get; init; }

    /// <summary>Options with an explicit window.</summary>
    /// <param name="center">Window center.</param>
    /// <param name="width">Window width.</param>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The options.</returns>
    public static RenderOptions Window(double center, double width, int frame = 0) =>
        new() { Frame = frame, VoiMode = VoiMode.Explicit, WindowCenter = center, WindowWidth = width };

    internal siaara_render_options ToNative()
    {
        ArgumentOutOfRangeException.ThrowIfNegative(Frame);
        return new siaara_render_options
        {
            struct_size = StructSize.Of<siaara_render_options>(),
            frame = (uint)Frame,
            voi_mode = (int)VoiMode,
            invert = Invert ? 1 : 0,
            window_center = WindowCenter,
            window_width = WindowWidth,
            no_overlays = NoOverlays ? 1 : 0,
            no_shutters = NoShutters ? 1 : 0,
        };
    }
}
