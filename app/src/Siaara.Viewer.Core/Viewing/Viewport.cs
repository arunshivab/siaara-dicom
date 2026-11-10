// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>Mouse tools of the left button.</summary>
public enum ViewerTool
{
    /// <summary>Window / level.</summary>
    WindowLevel = 0,
    /// <summary>Zoom.</summary>
    Zoom = 1,
    /// <summary>Pan.</summary>
    Pan = 2,
    /// <summary>Scroll through images.</summary>
    Scroll = 3,
    /// <summary>Length.</summary>
    Length = 4,
    /// <summary>Polyline.</summary>
    Polyline = 5,
    /// <summary>Angle.</summary>
    Angle = 6,
    /// <summary>Cobb angle.</summary>
    Cobb = 7,
    /// <summary>Ellipse ROI.</summary>
    Ellipse = 8,
    /// <summary>Circle ROI.</summary>
    Circle = 9,
    /// <summary>Rectangle ROI.</summary>
    Rectangle = 10,
    /// <summary>Polygon ROI.</summary>
    Polygon = 11,
    /// <summary>Freehand ROI.</summary>
    Freehand = 12,
    /// <summary>Pixel probe.</summary>
    Probe = 13,
    /// <summary>Manual calibration line.</summary>
    Calibrate = 14,
    /// <summary>Magnifier glass while the left button is held.</summary>
    Magnify = 15,
    /// <summary>Arrow annotation.</summary>
    Arrow = 16,
    /// <summary>Text annotation.</summary>
    Text = 17,
    /// <summary>Cross-reference: a point here moves the other viewports of the same frame of reference to it.</summary>
    CrossReference = 18,
}

/// <summary>A drag in progress.</summary>
internal sealed class DragState
{
    public int Button { get; init; }
    public int StartX { get; init; }
    public int StartY { get; init; }
    public int LastX { get; set; }
    public int LastY { get; set; }
    public ViewerTool Tool { get; init; }
    public DisplayState StartState { get; init; } = new();
    public double StartCenter { get; init; }
    public double StartWidth { get; init; }
    public double StartZoom { get; init; }
    public int StartIndex { get; init; }
    public double ScrollAccumulator { get; set; }
}

/// <summary>One viewport of a session. All members are guarded by the session lock.</summary>
public sealed class Viewport
{
    internal Viewport(int index)
    {
        Index = index;
    }

    /// <summary>Index in the grid.</summary>
    public int Index { get; }

    /// <summary>Series shown, or null.</summary>
    public SeriesSource? Series { get; internal set; }

    /// <summary>Index of the series in the session list, or -1.</summary>
    public int SeriesIndex { get; internal set; } = -1;

    /// <summary>Image shown.</summary>
    public int ImageIndex { get; internal set; }

    /// <summary>Display state.</summary>
    public DisplayState State { get; internal set; } = new();

    /// <summary>Window name shown with the W/L (preset or file explanation).</summary>
    public string WindowName { get; internal set; } = "";

    /// <summary>Width in device pixels.</summary>
    public int Width { get; internal set; }

    /// <summary>Height in device pixels.</summary>
    public int Height { get; internal set; }

    /// <summary>Back-pressure of this viewport.</summary>
    public FrameFlow Flow { get; } = new();

    /// <summary>Cine timing.</summary>
    public CinePlayer Cine { get; } = new();

    /// <summary>The current image, or null.</summary>
    public SeriesImage? CurrentImage => Series is { Images.Count: > 0 } s ? s.Images[Math.Clamp(ImageIndex, 0, s.Images.Count - 1)] : null;

    /// <summary>Frames rendered and sent.</summary>
    public long FramesSent { get; internal set; }

    internal DragState? Drag { get; set; }
    internal Annotation? Drawing { get; set; }
    internal string? CalibrationPrompt { get; set; }
    internal string? TextPrompt { get; set; }

    /// <summary>Flow control of the magnifier's frames (sent as viewport index + magnifier base).</summary>
    public FrameFlow MagnifierFlow { get; } = new();

    // Magnifier: centre in viewport pixels while the magnifier tool is held, else null.
    internal (int X, int Y)? Magnifier { get; set; }
    internal DateTime LastRoiUpdate { get; set; }
    internal CancellationTokenSource? CineStop { get; set; }

    // Geometry of the last render (input mapping and annotation projection).
    internal ScreenImageTransform? Transform { get; set; }
    internal TkFrameInfo? FrameInfo { get; set; }
    internal (double Center, double Width)? ShownWindow { get; set; }

    // Cine ring cache: rendered frames for one display state.
    internal CineRing Ring { get; } = new();
}

/// <summary>Rendered frames of one display state, at most a byte budget (oldest dropped first).</summary>
internal sealed class CineRing
{
    private readonly Dictionary<int, byte[]> _frames = [];
    private readonly Queue<int> _order = new();
    private long _bytes;
    private string _stateKey = "";

    public long Budget { get; set; } = 256L * 1024 * 1024;

    public long Bytes => _bytes;

    public void Reset(string stateKey)
    {
        if (string.Equals(stateKey, _stateKey, StringComparison.Ordinal))
        {
            return;
        }
        _stateKey = stateKey;
        _frames.Clear();
        _order.Clear();
        _bytes = 0;
    }

    public bool TryGet(string stateKey, int index, out byte[] pixels)
    {
        if (string.Equals(stateKey, _stateKey, StringComparison.Ordinal) && _frames.TryGetValue(index, out byte[]? p))
        {
            pixels = p;
            return true;
        }
        pixels = [];
        return false;
    }

    public void Put(string stateKey, int index, ReadOnlySpan<byte> pixels)
    {
        Reset(stateKey);
        if (_frames.ContainsKey(index) || pixels.Length > Budget)
        {
            return;
        }
        while (_bytes + pixels.Length > Budget && _order.TryDequeue(out int old))
        {
            if (_frames.Remove(old, out byte[]? b))
            {
                _bytes -= b.Length;
            }
        }
        _frames[index] = pixels.ToArray();
        _order.Enqueue(index);
        _bytes += pixels.Length;
    }
}
