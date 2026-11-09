using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Siaara.Interop;
using Siaara.Volumes;

namespace Siaara;

/// <summary>Output pixel format of <see cref="DicomFrame.Render(Span{byte}, ViewportOptions?)"/>.</summary>
public enum PixelOutputFormat
{
    /// <summary>4 bytes per pixel: R, G, B, A.</summary>
    Rgba8 = C.SIAARA_PIXELS_RGBA8,
    /// <summary>1 byte per pixel (luma of the rendered colour).</summary>
    Gray8 = C.SIAARA_PIXELS_GRAY8,
}

/// <summary>VOI LUT Function (PS3.3 C.11.2.1.3).</summary>
public enum VoiFunction
{
    /// <summary>LINEAR.</summary>
    Linear = C.SIAARA_VOI_LINEAR,
    /// <summary>LINEAR_EXACT.</summary>
    LinearExact = C.SIAARA_VOI_LINEAR_EXACT,
    /// <summary>SIGMOID.</summary>
    Sigmoid = C.SIAARA_VOI_SIGMOID,
}

/// <summary>Properties of a decoded frame (SIAARA_FRAMEF_*).</summary>
[Flags]
public enum FrameCharacteristics
{
    /// <summary>None.</summary>
    None = 0,
    /// <summary>MONOCHROME1 or MONOCHROME2.</summary>
    Monochrome = C.SIAARA_FRAMEF_MONOCHROME,
    /// <summary>MONOCHROME1 / Presentation LUT Shape INVERSE.</summary>
    Inverted = C.SIAARA_FRAMEF_INVERTED,
    /// <summary>PALETTE COLOR.</summary>
    Palette = C.SIAARA_FRAMEF_PALETTE,
    /// <summary>Supplemental Palette Color LUT.</summary>
    SupplementalPalette = C.SIAARA_FRAMEF_SUPPLEMENTAL_PALETTE,
    /// <summary>Modality LUT Sequence instead of rescale.</summary>
    ModalityLut = C.SIAARA_FRAMEF_MODALITY_LUT,
    /// <summary>Presentation LUT Sequence.</summary>
    PresentationLut = C.SIAARA_FRAMEF_PRESENTATION_LUT,
    /// <summary>Rectangular shutter.</summary>
    RectangularShutter = C.SIAARA_FRAMEF_SHUTTER_RECTANGULAR,
    /// <summary>Circular shutter.</summary>
    CircularShutter = C.SIAARA_FRAMEF_SHUTTER_CIRCULAR,
    /// <summary>Polygonal shutter.</summary>
    PolygonalShutter = C.SIAARA_FRAMEF_SHUTTER_POLYGONAL,
    /// <summary>Bitmap shutter.</summary>
    BitmapShutter = C.SIAARA_FRAMEF_SHUTTER_BITMAP,
    /// <summary>Float / Double Float Pixel Data.</summary>
    FloatingPoint = C.SIAARA_FRAMEF_FLOAT,
    /// <summary>VOI from the Frame VOI LUT functional group.</summary>
    VoiFromFunctionalGroups = C.SIAARA_FRAMEF_VOI_FROM_FUNCTIONAL_GROUPS,
}

/// <summary>An RGBA colour.</summary>
/// <param name="R">Red.</param>
/// <param name="G">Green.</param>
/// <param name="B">Blue.</param>
/// <param name="A">Alpha.</param>
public readonly record struct Rgba8Color(byte R, byte G, byte B, byte A = 255);

/// <summary>Viewport and presentation options of <see cref="DicomFrame"/> rendering (siaara_view2d_options).</summary>
public sealed record ViewportOptions
{
    /// <summary>Output width; 0 together with Height 0 = the image size.</summary>
    public int Width { get; init; }
    /// <summary>Output height.</summary>
    public int Height { get; init; }
    /// <summary>Output format.</summary>
    public PixelOutputFormat Format { get; init; }
    /// <summary>Output pixels per image pixel; 0 = fit the viewport.</summary>
    public double Zoom { get; init; }
    /// <summary>Image centre offset from the viewport centre (output pixels).</summary>
    public double PanX { get; init; }
    /// <summary>Image centre offset from the viewport centre (output pixels).</summary>
    public double PanY { get; init; }
    /// <summary>Clockwise rotation in degrees (after the flips).</summary>
    public double Rotation { get; init; }
    /// <summary>Mirror left-right.</summary>
    public bool FlipHorizontal { get; init; }
    /// <summary>Mirror top-bottom.</summary>
    public bool FlipVertical { get; init; }
    /// <summary>Nearest or Linear.</summary>
    public Interpolation Interpolation { get; init; } = Interpolation.Nearest;
    /// <summary>Colour map for grayscale images.</summary>
    public Colormap Colormap { get; init; } = Colormap.Gray;
    /// <summary>VOI selection.</summary>
    public VoiMode VoiMode { get; init; } = VoiMode.FromFile;
    /// <summary>For <see cref="VoiMode.FromFile"/>: index into <see cref="DicomFrame.VoiEntries"/>.</summary>
    public int VoiIndex { get; init; }
    /// <summary>For <see cref="VoiMode.Explicit"/>.</summary>
    public double WindowCenter { get; init; }
    /// <summary>For <see cref="VoiMode.Explicit"/>.</summary>
    public double WindowWidth { get; init; }
    /// <summary>For <see cref="VoiMode.Explicit"/>.</summary>
    public VoiFunction VoiFunction { get; init; } = VoiFunction.Linear;
    /// <summary>Invert on top of MONOCHROME1.</summary>
    public bool Invert { get; init; }
    /// <summary>Hide overlays.</summary>
    public bool NoOverlays { get; init; }
    /// <summary>Do not apply shutters.</summary>
    public bool NoShutters { get; init; }
    /// <summary>Bit k shows overlay group 0x6000 + 2k; 0 = all.</summary>
    public uint OverlayGroups { get; init; }
    /// <summary>Overlay colour; null = opaque white.</summary>
    public Rgba8Color? OverlayColor { get; init; }
    /// <summary>Colour outside the image (default: transparent black).</summary>
    public Rgba8Color Background { get; init; } = new(0, 0, 0, 0);
    /// <summary>A presentation state to apply (rendered at its displayed area, then placed).</summary>
    public PresentationState? PresentationState { get; init; }

    /// <summary>An explicit window.</summary>
    /// <param name="center">Window center.</param>
    /// <param name="width">Window width.</param>
    /// <returns>A copy with the window set.</returns>
    public ViewportOptions WithWindow(double center, double width) =>
        this with { VoiMode = VoiMode.Explicit, WindowCenter = center, WindowWidth = width };

    internal siaara_view2d_options ToNative(nint presentationState)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(Width);
        ArgumentOutOfRangeException.ThrowIfNegative(Height);
        ArgumentOutOfRangeException.ThrowIfNegative(VoiIndex);
        var o = new siaara_view2d_options
        {
            struct_size = StructSize.Of<siaara_view2d_options>(),
            viewport_width = (uint)Width,
            viewport_height = (uint)Height,
            output_format = (int)Format,
            zoom = Zoom,
            pan_x = PanX,
            pan_y = PanY,
            rotation = Rotation,
            flip_horizontal = FlipHorizontal ? 1 : 0,
            flip_vertical = FlipVertical ? 1 : 0,
            interpolation = (int)Interpolation,
            colormap = (int)Colormap,
            voi_mode = (int)VoiMode,
            voi_index = (uint)VoiIndex,
            window_center = WindowCenter,
            window_width = WindowWidth,
            voi_function = (int)VoiFunction,
            invert = Invert ? 1 : 0,
            no_overlays = NoOverlays ? 1 : 0,
            no_shutters = NoShutters ? 1 : 0,
            overlay_groups = OverlayGroups,
            presentation_state = presentationState,
        };
        if (OverlayColor is Rgba8Color c)
        {
            o.overlay_rgba[0] = c.R;
            o.overlay_rgba[1] = c.G;
            o.overlay_rgba[2] = c.B;
            o.overlay_rgba[3] = c.A;
        }
        o.background_rgba[0] = Background.R;
        o.background_rgba[1] = Background.G;
        o.background_rgba[2] = Background.B;
        o.background_rgba[3] = Background.A;
        return o;
    }
}

/// <summary>Properties of a decoded frame (siaara_frame_properties).</summary>
public sealed record FrameProperties
{
    /// <summary>0-based frame index.</summary>
    public int Frame { get; init; }
    /// <summary>Number of Frames of the image.</summary>
    public int NumberOfFrames { get; init; }
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
    /// <summary>Flags.</summary>
    public FrameCharacteristics Flags { get; init; }
    /// <summary>Number of VOI entries (windows, then VOI LUTs).</summary>
    public int VoiCount { get; init; }
    /// <summary>Number of overlays.</summary>
    public int OverlayCount { get; init; }
    /// <summary>Colour shown where shuttered.</summary>
    public Rgba8Color ShutterColor { get; init; }
    /// <summary>Rescale Slope (1 with a Modality LUT).</summary>
    public double RescaleSlope { get; init; }
    /// <summary>Rescale Intercept (0 with a Modality LUT).</summary>
    public double RescaleIntercept { get; init; }
    /// <summary>Minimum modality value in the frame.</summary>
    public double MinValue { get; init; }
    /// <summary>Maximum modality value in the frame.</summary>
    public double MaxValue { get; init; }
    /// <summary>Bytes of the decoded frame (native layout).</summary>
    public long DecodedBytes { get; init; }
    /// <summary>Photometric Interpretation.</summary>
    public string PhotometricInterpretation { get; init; } = string.Empty;
}

/// <summary>A window or a VOI LUT of a frame.</summary>
/// <param name="Index">The voi index for <see cref="VoiMode.FromFile"/>.</param>
/// <param name="IsLut">True for a VOI LUT Sequence item.</param>
/// <param name="Function">VOI LUT Function (windows).</param>
/// <param name="Center">Window center.</param>
/// <param name="Width">Window width.</param>
/// <param name="LutEntries">LUT entries.</param>
/// <param name="LutFirstMapped">First stored value mapped.</param>
/// <param name="LutBits">Bits per LUT entry.</param>
/// <param name="FromFunctionalGroups">From the Frame VOI LUT functional group.</param>
/// <param name="Explanation">Window Center &amp; Width Explanation or LUT Explanation.</param>
public sealed record VoiEntry(int Index, bool IsLut, VoiFunction Function, double Center, double Width, int LutEntries,
                              int LutFirstMapped, int LutBits, bool FromFunctionalGroups, string Explanation);

/// <summary>An overlay plane (siaara_overlay_info).</summary>
/// <param name="Index">Index for <see cref="DicomFrame.GetOverlayMask(int)"/>.</param>
/// <param name="Group">0x6000 .. 0x601E.</param>
/// <param name="Rows">Overlay Rows.</param>
/// <param name="Columns">Overlay Columns.</param>
/// <param name="OriginRow">1-based origin row.</param>
/// <param name="OriginColumn">1-based origin column.</param>
/// <param name="Type">'G' or 'R'.</param>
/// <param name="Frames">Number of Frames in Overlay.</param>
/// <param name="IsEmbedded">Retired embedded overlay.</param>
/// <param name="CoversFrame">The overlay has a plane for this frame.</param>
/// <param name="Label">Overlay Label.</param>
/// <param name="Description">Overlay Description.</param>
public sealed record OverlayPlane(int Index, int Group, int Rows, int Columns, int OriginRow, int OriginColumn, char Type,
                                  int Frames, bool IsEmbedded, bool CoversFrame, string Label, string Description);

/// <summary>A palette: entries x (R, G, B, A) in the palette's own precision.</summary>
/// <param name="Entries">Number of entries.</param>
/// <param name="FirstMapped">First stored value mapped.</param>
/// <param name="Bits">8 or 16.</param>
/// <param name="HasAlpha">Alpha data present.</param>
/// <param name="IsSegmented">Read from segmented data.</param>
/// <param name="IsSupplemental">Supplemental palette of a grayscale image.</param>
/// <param name="Rgba">entries * 4 values.</param>
public sealed record ColorPalette(int Entries, int FirstMapped, int Bits, bool HasAlpha, bool IsSegmented, bool IsSupplemental,
                                  ushort[] Rgba);

/// <summary>
/// A frame decoded once for repeated rendering (siaara_frame): window / level, colour maps, zoom,
/// pan and rotation without decoding again. Independent of the file it came from. Immutable: may
/// be rendered from many threads at once.
/// </summary>
public sealed class DicomFrame : IDisposable
{
    private readonly FrameHandle _handle;
    private FrameProperties? _properties;

    private DicomFrame(FrameHandle handle) => _handle = handle;

    /// <summary>Decodes frame index of the file.</summary>
    /// <param name="file">The file (may be disposed afterwards).</param>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The frame.</returns>
    public static DicomFrame Open(DicomFile file, int frame = 0)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        var h = new FrameHandle();
        using (var l = new HandleLease(file.Handle))
        {
            int st = Native.siaara_frame_open(l.Pointer, (uint)frame, out nint p);
            if (st != C.SIAARA_OK)
            {
                h.Dispose();
                throw Status.Error(st);
            }
            h.Attach(p);
        }
        return new DicomFrame(h);
    }

    /// <summary>True after Dispose.</summary>
    public bool IsDisposed => _handle.IsClosed;

    /// <summary>The frame properties.</summary>
    public FrameProperties Properties => _properties ??= ReadProperties();

    private FrameProperties ReadProperties()
    {
        var p = new siaara_frame_properties { struct_size = StructSize.Of<siaara_frame_properties>() };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_frame_info(l.Pointer, ref p));
        return new FrameProperties
        {
            Frame = (int)p.frame,
            NumberOfFrames = (int)p.number_of_frames,
            Rows = (int)p.rows,
            Columns = (int)p.columns,
            SamplesPerPixel = (int)p.samples_per_pixel,
            BitsAllocated = (int)p.bits_allocated,
            BitsStored = (int)p.bits_stored,
            Flags = (FrameCharacteristics)p.flags,
            VoiCount = (int)p.voi_count,
            OverlayCount = (int)p.overlay_count,
            ShutterColor = new Rgba8Color(p.shutter_rgb[0], p.shutter_rgb[1], p.shutter_rgb[2]),
            RescaleSlope = p.rescale_slope,
            RescaleIntercept = p.rescale_intercept,
            MinValue = p.min_value,
            MaxValue = p.max_value,
            DecodedBytes = (long)p.decoded_bytes,
            PhotometricInterpretation = DicomDictionary.Chars(p.photometric),
        };
    }

    /// <summary>
    /// Zero-copy, read-only view of the decoded pixels (DICOM native layout). The frame stays
    /// alive until the returned owner is disposed.
    /// </summary>
    /// <returns>The pixel memory.</returns>
    public IMemoryOwner<byte> LeasePixels()
    {
        nint data;
        ulong bytes;
        using (var l = new HandleLease(_handle))
        {
            Status.Check(Native.siaara_frame_pixels(l.Pointer, out data, out bytes));
        }
        if (bytes > int.MaxValue)
        {
            throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "frame larger than a span can address");
        }
        return new BorrowedMemoryManager<byte>(_handle, data, (int)bytes);
    }

    // ------------------------------------------------------------------ rendering

    /// <summary>Bytes needed for a rendering with these options.</summary>
    /// <param name="options">Options, or null (image size, RGBA8).</param>
    /// <returns>The size in bytes.</returns>
    public int GetRenderSize(ViewportOptions? options = null)
    {
        int st = RenderCore(options, default, out nuint needed);
        Status.Check(st);
        return DicomFile.ToInt(needed);
    }

    /// <summary>Renders into destination.</summary>
    /// <param name="destination">At least GetRenderSize bytes.</param>
    /// <param name="options">Options, or null.</param>
    /// <returns>Bytes written.</returns>
    public int Render(Span<byte> destination, ViewportOptions? options = null)
    {
        if (destination.IsEmpty)
        {
            throw new ArgumentException("destination must not be empty", nameof(destination));
        }
        Status.Check(RenderCore(options, destination, out nuint needed));
        return DicomFile.ToInt(needed);
    }

    /// <summary>Renders without throwing for native failures (hot path, e.g. during window drags).</summary>
    /// <param name="destination">Output buffer.</param>
    /// <param name="options">Options, or null.</param>
    /// <param name="bytesWritten">Bytes written, or needed when the buffer is too small.</param>
    /// <returns>The status.</returns>
    public SiaaraErrorCode TryRender(Span<byte> destination, ViewportOptions? options, out int bytesWritten)
    {
        bytesWritten = 0;
        if (destination.IsEmpty)
        {
            return SiaaraErrorCode.BufferTooSmall;
        }
        int st = RenderCore(options, destination, out nuint needed);
        bytesWritten = needed > int.MaxValue ? int.MaxValue : (int)needed;
        return (SiaaraErrorCode)st;
    }

    /// <summary>Renders to a new array.</summary>
    /// <param name="options">Options, or null.</param>
    /// <returns>The pixels.</returns>
    public byte[] Render(ViewportOptions? options = null)
    {
        byte[] buf = new byte[GetRenderSize(options)];
        Render(buf, options);
        return buf;
    }

    private int RenderCore(ViewportOptions? options, Span<byte> destination, out nuint needed)
    {
        using var l = new HandleLease(_handle);
        if (options is null)
        {
            return Native.siaara_frame_render(l.Pointer, in Unsafe.NullRef<siaara_view2d_options>(), destination, out needed);
        }
        if (options.PresentationState is null)
        {
            siaara_view2d_options o = options.ToNative(0);
            return Native.siaara_frame_render(l.Pointer, in o, destination, out needed);
        }
        using var ps = new HandleLease(options.PresentationState.Handle);
        siaara_view2d_options op = options.ToNative(ps.Pointer);
        return Native.siaara_frame_render(l.Pointer, in op, destination, out needed);
    }

    /// <summary>Viewport point to image PIXEL coordinates ((0,0) = top left corner of the first pixel).</summary>
    /// <param name="options">The options the view was rendered with.</param>
    /// <param name="x">Viewport x.</param>
    /// <param name="y">Viewport y.</param>
    /// <returns>The image point.</returns>
    public (double X, double Y) ViewToImage(ViewportOptions? options, double x, double y) => Map(options, true, x, y);

    /// <summary>Image PIXEL coordinates to the viewport.</summary>
    /// <param name="options">The options the view is rendered with.</param>
    /// <param name="x">Image x (column).</param>
    /// <param name="y">Image y (row).</param>
    /// <returns>The viewport point.</returns>
    public (double X, double Y) ImageToView(ViewportOptions? options, double x, double y) => Map(options, false, x, y);

    private (double, double) Map(ViewportOptions? options, bool toImage, double x, double y)
    {
        using var l = new HandleLease(_handle);
        double ox, oy;
        if (options is null)
        {
            Status.Check(Native.siaara_frame_map_point(l.Pointer, in Unsafe.NullRef<siaara_view2d_options>(), toImage ? 1 : 0, x, y,
                out ox, out oy));
        }
        else if (options.PresentationState is null)
        {
            siaara_view2d_options o = options.ToNative(0);
            Status.Check(Native.siaara_frame_map_point(l.Pointer, in o, toImage ? 1 : 0, x, y, out ox, out oy));
        }
        else
        {
            using var ps = new HandleLease(options.PresentationState.Handle);
            siaara_view2d_options o = options.ToNative(ps.Pointer);
            Status.Check(Native.siaara_frame_map_point(l.Pointer, in o, toImage ? 1 : 0, x, y, out ox, out oy));
        }
        return (ox, oy);
    }

    // ------------------------------------------------------------------ VOI, overlays, shutters, palette

    /// <summary>The frame's windows (file order), then its VOI LUTs.</summary>
    public IReadOnlyList<VoiEntry> VoiEntries
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_voi_count(l.Pointer, out uint n));
            var list = new VoiEntry[n];
            for (uint i = 0; i < n; i++)
            {
                var info = new siaara_voi_info { struct_size = StructSize.Of<siaara_voi_info>() };
                var state = (Frame: l.Pointer, Index: i);
                Status.Check(Native.siaara_voi_get(l.Pointer, i, ref info, default, out _));
                string text = Status.ReadString(state, static (s, b, out n) =>
                {
                    var tmp = new siaara_voi_info { struct_size = StructSize.Of<siaara_voi_info>() };
                    return Native.siaara_voi_get(s.Frame, s.Index, ref tmp, b, out n);
                });
                list[i] = new VoiEntry((int)i, info.kind == C.SIAARA_VOI_KIND_LUT, (VoiFunction)info.function, info.center, info.width,
                    (int)info.lut_entries, info.lut_first_mapped, (int)info.lut_bits, info.from_functional_groups != 0, text);
            }
            return list;
        }
    }

    /// <summary>LUT Data of a VOI LUT entry.</summary>
    /// <param name="index">Index of a VOI LUT in <see cref="VoiEntries"/>.</param>
    /// <returns>The entries.</returns>
    public ushort[] GetVoiLutData(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_handle);
        return Status.ReadArray<ushort, (nint Frame, uint Index)>((l.Pointer, (uint)index), static (s, b, out n) =>
            Native.siaara_voi_lut_data(s.Frame, s.Index, b, out n));
    }

    /// <summary>The overlay planes of the image.</summary>
    public IReadOnlyList<OverlayPlane> Overlays
    {
        get
        {
            int n = Properties.OverlayCount;
            var list = new OverlayPlane[n];
            using var l = new HandleLease(_handle);
            for (int i = 0; i < n; i++)
            {
                var o = new siaara_overlay_info { struct_size = StructSize.Of<siaara_overlay_info>() };
                Status.Check(Native.siaara_overlay_get(l.Pointer, (uint)i, ref o));
                var s0 = (Frame: l.Pointer, Index: (uint)i, Field: C.SIAARA_OVERLAY_LABEL);
                string label = Status.ReadString(s0, static (s, b, out n) => Native.siaara_overlay_text(s.Frame, s.Index, s.Field, b, out n));
                var s1 = s0 with { Field = C.SIAARA_OVERLAY_DESCRIPTION };
                string desc = Status.ReadString(s1, static (s, b, out n) => Native.siaara_overlay_text(s.Frame, s.Index, s.Field, b, out n));
                list[i] = new OverlayPlane(i, (int)o.group, (int)o.rows, (int)o.columns, o.origin_row, o.origin_column, (char)o.type,
                    (int)o.frames, o.embedded != 0, o.covers_frame != 0, label, desc);
            }
            return list;
        }
    }

    /// <summary>rows x columns bytes: 1 where the overlay is set for this frame.</summary>
    /// <param name="index">Overlay index.</param>
    /// <returns>The mask.</returns>
    public byte[] GetOverlayMask(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_handle);
        return Status.ReadArray<byte, (nint Frame, uint Index)>((l.Pointer, (uint)index), static (s, b, out n) =>
            Native.siaara_overlay_mask(s.Frame, s.Index, b, out n));
    }

    /// <summary>rows x columns bytes: 1 = visible, 0 = covered by the display shutters.</summary>
    /// <returns>The mask.</returns>
    public byte[] GetShutterMask()
    {
        using var l = new HandleLease(_handle);
        return Status.ReadArray<byte, nint>(l.Pointer, static (f, b, out n) => Native.siaara_shutter_mask(f, b, out n));
    }

    /// <summary>The image's palette (PALETTE COLOR or supplemental), or null.</summary>
    /// <returns>The palette.</returns>
    public ColorPalette? GetPalette()
    {
        using var l = new HandleLease(_handle);
        var info = new siaara_palette_info { struct_size = StructSize.Of<siaara_palette_info>() };
        int st = Native.siaara_frame_palette(l.Pointer, ref info, default, out nuint needed);
        if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return null;
        }
        Status.Check(st);
        var data = new ushort[DicomFile.ToInt(needed)];
        Status.Check(Native.siaara_frame_palette(l.Pointer, ref info, data, out _));
        return new ColorPalette((int)info.entries, info.first_mapped, (int)info.bits, info.has_alpha != 0, info.segmented != 0,
            info.supplemental != 0, data);
    }

    /// <summary>A standard colour map as 256 RGBA8 entries.</summary>
    /// <param name="colormap">The colour map.</param>
    /// <returns>1024 bytes.</returns>
    public static byte[] GetColormapRgba8(Colormap colormap)
    {
        var buf = new byte[1024];
        Status.Check(Native.siaara_colormap_rgba8((int)colormap, buf, out _));
        return buf;
    }

    /// <summary>Frees the frame.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}
