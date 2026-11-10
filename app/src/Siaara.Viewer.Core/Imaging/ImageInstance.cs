// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Collections.Concurrent;
using Siaara.Viewer.Core.Geometry;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Imaging;

/// <summary>Identifies one displayable frame.</summary>
/// <param name="SopInstanceUid">SOP Instance UID.</param>
/// <param name="Frame">0-based frame.</param>
public readonly record struct FrameKey(string SopInstanceUid, int Frame);

/// <summary>
/// One instance of a series: its header (read with stop-before-pixel-data and kept open, read-only)
/// and the facts the viewer needs without pixel data. Pixel data is opened lazily through
/// <see cref="ImageCache"/>.
/// </summary>
public sealed class ImageInstance : IDisposable
{
    private static readonly HashSet<string> s_lossySyntaxes = new(StringComparer.Ordinal)
    {
        "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.91",
        "1.2.840.10008.1.2.4.203", "1.2.840.10008.1.2.4.100", "1.2.840.10008.1.2.4.101", "1.2.840.10008.1.2.4.102",
        "1.2.840.10008.1.2.4.103", "1.2.840.10008.1.2.4.104", "1.2.840.10008.1.2.4.105", "1.2.840.10008.1.2.4.106",
        "1.2.840.10008.1.2.4.107", "1.2.840.10008.1.2.4.108",
    };

    private ImageInstance(string path, TkFile header)
    {
        FilePath = path;
        Header = header;
        SopInstanceUid = header.GetString(Tags.SopInstanceUid) ?? path;
        Modality = (header.GetString(Tags.Modality) ?? "").Trim();
        FrameCount = (int)Math.Max(1, header.GetDouble(Tags.NumberOfFrames) ?? 1);
        string ts = SafeTransferSyntax(header);
        IsLossy = string.Equals((header.GetString(Tags.LossyImageCompression) ?? "").Trim(), "01", StringComparison.Ordinal)
                  || s_lossySyntaxes.Contains(ts);
        FrameOfReferenceUid = header.GetString(Tags.FrameOfReferenceUid) ?? "";
    }

    private readonly ConcurrentDictionary<int, ImagePlane?> _planes = new();

    /// <summary>Frame of Reference UID ("" when absent).</summary>
    public string FrameOfReferenceUid { get; }

    /// <summary>Patient-space plane of a frame (cached), or null without complete geometry.</summary>
    /// <param name="frame">Frame.</param>
    /// <returns>The plane.</returns>
    public ImagePlane? GetPlane(int frame) => _planes.GetOrAdd(frame, f =>
    {
        try
        {
            return Header.IsDisposed ? null : Header.GetGeometry(f).ToImagePlane(FrameOfReferenceUid);
        }
        catch (TkException)
        {
            return null;
        }
        catch (ObjectDisposedException)
        {
            return null;
        }
    });

    /// <summary>Reads the header of a file.</summary>
    /// <param name="path">File.</param>
    /// <returns>The instance.</returns>
    public static ImageInstance OpenHeader(string path) => new(path, TkFile.Open(path, headersOnly: true));

    /// <summary>File path.</summary>
    public string FilePath { get; }

    /// <summary>Header (no pixel data). Read-only, shared.</summary>
    public TkFile Header { get; }

    /// <summary>SOP Instance UID.</summary>
    public string SopInstanceUid { get; }

    /// <summary>Modality.</summary>
    public string Modality { get; }

    /// <summary>Number of frames.</summary>
    public int FrameCount { get; }

    /// <summary>Lossy compressed now or before (Lossy Image Compression "01" or a lossy transfer syntax).</summary>
    public bool IsLossy { get; internal set; }

    /// <summary>Overlay text lines that do not change between frames (built once).</summary>
    internal OverlayStatic? OverlayStatic { get; set; }

    private static string SafeTransferSyntax(TkFile f)
    {
        try
        {
            return f.TransferSyntaxUid;
        }
        catch (TkException)
        {
            return "";
        }
    }

    /// <inheritdoc/>
    public void Dispose() => Header.Dispose();
}

/// <summary>One display position of a series: an instance and a frame.</summary>
/// <param name="Instance">The instance.</param>
/// <param name="Frame">0-based frame.</param>
public sealed record SeriesImage(ImageInstance Instance, int Frame)
{
    /// <summary>Patient-space plane of this frame, or null.</summary>
    public ImagePlane? Plane => Instance.GetPlane(Frame);

    /// <summary>Key of the frame.</summary>
    public FrameKey Key => new(Instance.SopInstanceUid, Frame);
}
