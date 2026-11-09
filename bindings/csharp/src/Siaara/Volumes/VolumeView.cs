using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;

namespace Siaara.Volumes;

/// <summary>View mode.</summary>
public enum ViewMode
{
    /// <summary>Multi-planar reformat.</summary>
    Mpr = C.SIAARA_VIEW_MPR,
    /// <summary>Curved MPR, straightened.</summary>
    CurvedStraightened = C.SIAARA_VIEW_CURVED_STRAIGHTENED,
    /// <summary>Curved MPR, stretched.</summary>
    CurvedStretched = C.SIAARA_VIEW_CURVED_STRETCHED,
    /// <summary>Volume rendering with a transfer function.</summary>
    VolumeRender = C.SIAARA_VIEW_VOLUME_RENDER,
    /// <summary>Maximum intensity projection.</summary>
    VolumeMip = C.SIAARA_VIEW_VOLUME_MIP,
    /// <summary>Minimum intensity projection.</summary>
    VolumeMinIp = C.SIAARA_VIEW_VOLUME_MINIP,
}

/// <summary>Resampling interpolation.</summary>
public enum Interpolation
{
    /// <summary>Nearest neighbour.</summary>
    Nearest = C.SIAARA_INTERP_NEAREST,
    /// <summary>Trilinear.</summary>
    Linear = C.SIAARA_INTERP_LINEAR,
    /// <summary>Catmull-Rom (interpolating).</summary>
    Cubic = C.SIAARA_INTERP_CUBIC,
    /// <summary>Cubic B-spline without prefilter (smoothing).</summary>
    CubicBSpline = C.SIAARA_INTERP_CUBIC_BSPLINE,
}

/// <summary>MPR slab mode.</summary>
public enum SlabMode
{
    /// <summary>Thin slice.</summary>
    None = C.SIAARA_SLAB_NONE,
    /// <summary>Maximum.</summary>
    Mip = C.SIAARA_SLAB_MIP,
    /// <summary>Minimum.</summary>
    MinIp = C.SIAARA_SLAB_MINIP,
    /// <summary>Average.</summary>
    Average = C.SIAARA_SLAB_AVERAGE,
}

/// <summary>Volume rendering quality.</summary>
public enum RenderQuality
{
    /// <summary>Fast, for interaction.</summary>
    Interactive = C.SIAARA_QUALITY_INTERACTIVE,
    /// <summary>Full quality.</summary>
    Final = C.SIAARA_QUALITY_FINAL,
}

/// <summary>Colour map (PS3.6 Annex B palettes).</summary>
public enum Colormap
{
    /// <summary>Gray.</summary>
    Gray = C.SIAARA_COLORMAP_GRAY,
    /// <summary>Inverse gray.</summary>
    InverseGray = C.SIAARA_COLORMAP_INVERSE_GRAY,
    /// <summary>Hot Iron.</summary>
    HotIron = C.SIAARA_COLORMAP_HOT_IRON,
    /// <summary>PET.</summary>
    Pet = C.SIAARA_COLORMAP_PET,
    /// <summary>Hot Metal Blue.</summary>
    HotMetalBlue = C.SIAARA_COLORMAP_HOT_METAL_BLUE,
    /// <summary>PET 20 Step.</summary>
    Pet20Step = C.SIAARA_COLORMAP_PET_20_STEP,
}

/// <summary>Volume rendering transfer function (siaara_transfer_function).</summary>
public sealed record TransferFunction
{
    /// <summary>(value, opacity 0..1) pairs, sorted by value.</summary>
    public ReadOnlyMemory<double> OpacityPoints { get; init; }
    /// <summary>(value, r, g, b 0..1) quadruples; empty = white.</summary>
    public ReadOnlyMemory<double> ColorPoints { get; init; }
    /// <summary>(|gradient| per mm, factor 0..1) pairs; empty = off.</summary>
    public ReadOnlyMemory<double> GradientPoints { get; init; }
    /// <summary>Phong shading (headlight).</summary>
    public bool Shading { get; init; }
    /// <summary>Ambient (all four 0 = defaults 0.25, 0.7, 0.25, 24).</summary>
    public double Ambient { get; init; }
    /// <summary>Diffuse.</summary>
    public double Diffuse { get; init; }
    /// <summary>Specular.</summary>
    public double Specular { get; init; }
    /// <summary>Shininess.</summary>
    public double Shininess { get; init; }
    /// <summary>mm the opacities refer to; 0 = 1 mm.</summary>
    public double UnitDistance { get; init; }
    /// <summary>Sample distance in mm; 0 = half the smallest voxel spacing.</summary>
    public double SampleDistance { get; init; }
}

/// <summary>Fusion layer options (siaara_fusion_options).</summary>
public sealed record FusionOptions
{
    /// <summary>Colour map.</summary>
    public Colormap Colormap { get; init; } = Colormap.Pet;
    /// <summary>Window center.</summary>
    public double WindowCenter { get; init; }
    /// <summary>Window width; 0 = full range.</summary>
    public double WindowWidth { get; init; }
    /// <summary>Opacity 0..1.</summary>
    public double Opacity { get; init; } = 0.5;
    /// <summary>Values at or below the window are opaque (default transparent).</summary>
    public bool OpaqueBelowWindow { get; init; }
    /// <summary>Resample the overlay into the base grid once.</summary>
    public bool Preresample { get; init; }
}

/// <summary>Result of <see cref="VolumeView.Pick"/>.</summary>
/// <param name="Hit">The point maps into the scene (VR: a surface was hit).</param>
/// <param name="Inside">Inside the base volume.</param>
/// <param name="Patient">Patient point (mm, LPS).</param>
/// <param name="Index">Continuous voxel index in the base volume.</param>
/// <param name="Value">Base value.</param>
/// <param name="OverlayInside">Inside the overlay.</param>
/// <param name="OverlayValue">Overlay value.</param>
public sealed record PickResult(bool Hit, bool Inside, Point3D Patient, Point3D Index, double Value, bool OverlayInside, double OverlayValue);

/// <summary>
/// A rendered view of a volume (siaara_view): MPR, curved MPR, slab, MIP/MinIP, volume rendering and
/// fusion. The view keeps its volumes alive natively. Calls are serialised with a lock (the C view
/// must not be changed while it renders and renders from one thread at a time).
/// </summary>
public sealed class VolumeView : IDisposable
{
    private readonly ViewHandle _handle;
    private readonly Lock _gate = new();

    private VolumeView(ViewHandle h, int width, int height)
    {
        _handle = h;
        Width = width;
        Height = height;
    }

    /// <summary>Creates a view (MPR through the middle slice, full-range window, fitted).</summary>
    /// <param name="volume">Base volume.</param>
    /// <param name="width">Output width in pixels.</param>
    /// <param name="height">Output height in pixels.</param>
    /// <returns>The view.</returns>
    public static VolumeView Create(Volume volume, int width, int height)
    {
        ArgumentNullException.ThrowIfNull(volume);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
        nint v;
        using (var l = new HandleLease(volume.Handle))
        {
            Status.Check(Native.siaara_view_create(l.Pointer, (uint)width, (uint)height, out v));
        }
        var h = new ViewHandle();
        h.Attach(v);
        return new VolumeView(h, width, height);
    }

    /// <summary>Output width.</summary>
    public int Width { get; }

    /// <summary>Output height.</summary>
    public int Height { get; }

    /// <summary>Bytes of one RGBA8 rendering.</summary>
    public int RenderSize => checked(Width * Height * 4);

    /// <summary>Sets the plane: centre, viewing direction (into the screen) and up (screen right = normal x up).</summary>
    /// <param name="point">Centre (mm, LPS).</param>
    /// <param name="normal">Viewing direction.</param>
    /// <param name="up">Up vector.</param>
    /// <param name="pixelSpacing">mm per pixel; 0 = fit the volume.</param>
    public void SetPlane(Point3D point, Point3D normal, Point3D up, double pixelSpacing = 0)
    {
        Span<double> p = stackalloc double[9];
        point.CopyTo(p[..3]);
        normal.CopyTo(p.Slice(3, 3));
        up.CopyTo(p.Slice(6, 3));
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_set_plane(l.Pointer, p[..3], p.Slice(3, 3), p.Slice(6, 3), pixelSpacing));
        }
    }

    /// <summary>Sets mode, interpolation, slab (MPR only) and quality (volume rendering).</summary>
    /// <param name="mode">Mode.</param>
    /// <param name="interpolation">Interpolation.</param>
    /// <param name="slab">Slab mode.</param>
    /// <param name="slabThickness">Slab thickness (mm).</param>
    /// <param name="quality">Quality.</param>
    public void SetMode(ViewMode mode, Interpolation interpolation = Interpolation.Linear, SlabMode slab = SlabMode.None,
        double slabThickness = 0, RenderQuality quality = RenderQuality.Final)
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_set_mode(l.Pointer, (int)mode, (int)interpolation, (int)slab, slabThickness, (int)quality));
        }
    }

    /// <summary>Sets the window of a layer (0 = base, 1 = fusion overlay); width 0 = full range.</summary>
    /// <param name="layer">Layer.</param>
    /// <param name="center">Center.</param>
    /// <param name="width">Width.</param>
    public void SetWindow(int layer, double center, double width)
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_set_window(l.Pointer, layer, center, width));
        }
    }

    /// <summary>Sets the colour map of a layer.</summary>
    /// <param name="layer">Layer.</param>
    /// <param name="colormap">Colour map.</param>
    public void SetColormap(int layer, Colormap colormap)
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_set_colormap(l.Pointer, layer, (int)colormap));
        }
    }

    /// <summary>Path for the curved modes (at least 2 points).</summary>
    /// <param name="points">Points (mm, LPS).</param>
    /// <param name="spline">Catmull-Rom spline through the points.</param>
    public void SetCurve(ReadOnlySpan<Point3D> points, bool spline = true)
    {
        ReadOnlySpan<double> xyz = MemoryMarshal.Cast<Point3D, double>(points);
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_set_curve(l.Pointer, xyz, (uint)points.Length, spline ? 1 : 0));
        }
    }

    /// <summary>Sets the volume rendering transfer function.</summary>
    /// <param name="tf">The transfer function.</param>
    public void SetTransferFunction(TransferFunction tf)
    {
        ArgumentNullException.ThrowIfNull(tf);
        if (tf.OpacityPoints.Length % 2 != 0 || tf.ColorPoints.Length % 4 != 0 || tf.GradientPoints.Length % 2 != 0)
        {
            throw new ArgumentException("Opacity and gradient points are pairs, colour points quadruples.", nameof(tf));
        }
        using var mem = new NativeAllocations();
        var t = new siaara_transfer_function
        {
            struct_size = StructSize.Of<siaara_transfer_function>(),
            opacity_points = mem.Array(tf.OpacityPoints.Span),
            opacity_count = (uint)(tf.OpacityPoints.Length / 2),
            color_points = mem.Array(tf.ColorPoints.Span),
            color_count = (uint)(tf.ColorPoints.Length / 4),
            gradient_points = mem.Array(tf.GradientPoints.Span),
            gradient_count = (uint)(tf.GradientPoints.Length / 2),
            shading = tf.Shading ? 1 : 0,
            ambient = tf.Ambient,
            diffuse = tf.Diffuse,
            specular = tf.Specular,
            shininess = tf.Shininess,
            unit_distance = tf.UnitDistance,
            sample_distance = tf.SampleDistance,
        };
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_set_transfer_function(l.Pointer, in t));
        }
    }

    /// <summary>Sets (or with overlay null removes) the fusion layer; the view keeps the overlay alive.</summary>
    /// <param name="overlay">Overlay volume, or null.</param>
    /// <param name="options">Options, or null (PET colour map, full range, opacity 0.5).</param>
    public void SetFusionLayer(Volume? overlay, FusionOptions? options = null)
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            if (overlay is null)
            {
                Status.Check(Native.siaara_view_set_fusion_layer(l.Pointer, 0, in Unsafe.NullRef<siaara_fusion_options>()));
                return;
            }
            using var o = new HandleLease(overlay.Handle);
            if (options is null)
            {
                Status.Check(Native.siaara_view_set_fusion_layer(l.Pointer, o.Pointer, in Unsafe.NullRef<siaara_fusion_options>()));
            }
            else
            {
                var f = new siaara_fusion_options
                {
                    struct_size = StructSize.Of<siaara_fusion_options>(),
                    colormap = (int)options.Colormap,
                    window_center = options.WindowCenter,
                    window_width = options.WindowWidth,
                    opacity = options.Opacity,
                    opaque_below_window = options.OpaqueBelowWindow ? 1 : 0,
                    preresample = options.Preresample ? 1 : 0,
                };
                Status.Check(Native.siaara_view_set_fusion_layer(l.Pointer, o.Pointer, in f));
            }
        }
    }

    /// <summary>Renders Width x Height RGBA8 into destination.</summary>
    /// <param name="destination">At least RenderSize bytes.</param>
    /// <returns>Bytes written.</returns>
    public int RenderRgba8(Span<byte> destination)
    {
        if (destination.IsEmpty)
        {
            throw new ArgumentException("destination must not be empty", nameof(destination));
        }
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_render_rgba8(l.Pointer, destination, out nuint n));
            return (int)n;
        }
    }

    /// <summary>Renders without throwing (hot path).</summary>
    /// <param name="destination">Output buffer.</param>
    /// <param name="bytesWritten">Bytes written or needed.</param>
    /// <returns>The status.</returns>
    public SiaaraErrorCode TryRenderRgba8(Span<byte> destination, out int bytesWritten)
    {
        bytesWritten = 0;
        if (destination.IsEmpty)
        {
            bytesWritten = RenderSize;
            return SiaaraErrorCode.BufferTooSmall;
        }
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            int st = Native.siaara_view_render_rgba8(l.Pointer, destination, out nuint n);
            bytesWritten = (int)n;
            return (SiaaraErrorCode)st;
        }
    }

    /// <summary>Renders into a new array.</summary>
    /// <returns>The RGBA8 image.</returns>
    public byte[] RenderRgba8()
    {
        byte[] b = new byte[RenderSize];
        RenderRgba8(b);
        return b;
    }

    /// <summary>Renders into a pooled buffer; dispose it to return the buffer.</summary>
    /// <returns>The pooled RGBA8 image.</returns>
    public PooledBuffer RenderRgba8Pooled()
    {
        var buf = new PooledBuffer(RenderSize);
        try
        {
            RenderRgba8(buf.Span);
            return buf;
        }
        catch
        {
            buf.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Renders on a thread-pool thread: a Final-quality volume rendering can take long enough to
    /// block a request thread. Interactive MPR renders are fast; call RenderRgba8 for those.
    /// </summary>
    /// <param name="cancellationToken">Cancels before rendering starts.</param>
    /// <returns>A pooled RGBA8 image (dispose it).</returns>
    public Task<PooledBuffer> RenderRgba8Async(CancellationToken cancellationToken = default) =>
        Task.Run(RenderRgba8Pooled, cancellationToken);

    /// <summary>Screen point (pixels) to patient point and values, for crosshair synchronisation.</summary>
    /// <param name="x">Screen x.</param>
    /// <param name="y">Screen y.</param>
    /// <returns>The pick result.</returns>
    public PickResult Pick(double x, double y)
    {
        var r = new siaara_pick_result { struct_size = StructSize.Of<siaara_pick_result>() };
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_view_pick(l.Pointer, x, y, ref r));
        }
        return new PickResult(r.hit != 0, r.inside != 0, Point3D.From(r.patient), Point3D.From(r.index), r.value,
            r.overlay_inside != 0, r.overlay_value);
    }

    /// <summary>Frees the view.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}
