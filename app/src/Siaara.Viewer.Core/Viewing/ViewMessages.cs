// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Text;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>Pixel format of a frame message.</summary>
public enum FramePixelFormat
{
    /// <summary>Not a frame.</summary>
    None = 0,
    /// <summary>Raw RGBA, 4 bytes per pixel.</summary>
    Rgba = 1,
    /// <summary>Raw gray, 1 byte per pixel.</summary>
    Gray8 = 2,
    /// <summary>JPEG (LAN mode, M3).</summary>
    Jpeg = 3,
    /// <summary>PNG (LAN mode, M3).</summary>
    Png = 4,
}

/// <summary>Kinds of browser input.</summary>
public enum InputKind
{
    /// <summary>Pointer down.</summary>
    Down = 0,
    /// <summary>Pointer move.</summary>
    Move = 1,
    /// <summary>Pointer up.</summary>
    Up = 2,
    /// <summary>Wheel.</summary>
    Wheel = 3,
    /// <summary>Key down.</summary>
    Key = 4,
    /// <summary>Viewport resized (device pixels).</summary>
    Resize = 5,
}

/// <summary>Modifier bits of an input event.</summary>
[Flags]
public enum InputModifiers
{
    /// <summary>None.</summary>
    None = 0,
    /// <summary>Shift.</summary>
    Shift = 1,
    /// <summary>Control.</summary>
    Control = 2,
    /// <summary>Alt.</summary>
    Alt = 4,
    /// <summary>Meta / command.</summary>
    Meta = 8,
}

/// <summary>One input event from the browser (coordinates in viewport device pixels).</summary>
/// <param name="Viewport">Viewport index.</param>
/// <param name="Seq">Browser sequence number.</param>
/// <param name="Kind">Kind.</param>
/// <param name="X">X.</param>
/// <param name="Y">Y.</param>
/// <param name="Buttons">Buttons (1 left, 2 right, 4 middle).</param>
/// <param name="Wheel">Wheel delta (positive = down / away).</param>
/// <param name="Key">Key code (KeyboardEvent.keyCode values).</param>
/// <param name="Modifiers">Modifiers.</param>
/// <param name="Width">Resize: width.</param>
/// <param name="Height">Resize: height.</param>
public readonly record struct InputEvent(int Viewport, uint Seq, InputKind Kind, int X, int Y, int Buttons, int Wheel, int Key,
                                         InputModifiers Modifiers, int Width, int Height);

/// <summary>An annotation as the browser draws it (viewport pixels).</summary>
/// <param name="Id">Identifier.</param>
/// <param name="Kind">Kind name.</param>
/// <param name="Points">x y pairs.</param>
/// <param name="Closed">Closed outline.</param>
/// <param name="Label">Text.</param>
/// <param name="Spacing">Spacing source tag for the icon.</param>
/// <param name="Warnings">Warnings (drawn with a warning sign).</param>
/// <param name="Drawing">Still being drawn.</param>
public sealed record AnnotationView(string Id, string Kind, IReadOnlyList<double> Points, bool Closed, string Label, string Spacing,
                                    IReadOnlyList<string> Warnings, bool Drawing);

/// <summary>Cine state for the browser.</summary>
/// <param name="Playing">Playing.</param>
/// <param name="Fps">Rate.</param>
/// <param name="Mode">"loop" or "bounce".</param>
public sealed record CineView(bool Playing, double Fps, string Mode);

/// <summary>Orientation labels at the viewport edges (after rotation and flips).</summary>
/// <param name="Left">Left.</param>
/// <param name="Right">Right.</param>
/// <param name="Top">Top.</param>
/// <param name="Bottom">Bottom.</param>
public sealed record OrientationView(string Left, string Right, string Top, string Bottom);

/// <summary>The state message of one viewport (UTF-8 JSON in the protocol).</summary>
public sealed record ViewportStateMessage
{
    /// <summary>Corner text; null in privacy mode.</summary>
    public OverlayText? OverlayText { get; init; }
    /// <summary>Annotations.</summary>
    public IReadOnlyList<AnnotationView> Annotations { get; init; } = [];
    /// <summary>Plain-word warnings (never hidden, also in privacy mode).</summary>
    public IReadOnlyList<string> Warnings { get; init; } = [];
    /// <summary>Cine state.</summary>
    public CineView? Cine { get; init; }
    /// <summary>Edge labels; null in privacy mode or at oblique angles.</summary>
    public OrientationView? Orientation { get; init; }
    /// <summary>Privacy mode (F8).</summary>
    public bool Privacy { get; init; }
    /// <summary>This viewport is active.</summary>
    public bool Active { get; init; }
    /// <summary>Image index (0-based).</summary>
    public int ImageIndex { get; init; }
    /// <summary>Images.</summary>
    public int ImageCount { get; init; }
    /// <summary>A short message instead of an image ("Loading", "Drop a series here").</summary>
    public string? Message { get; init; }
    /// <summary>Id of a calibration line waiting for its true length.</summary>
    public string? CalibrationPrompt { get; init; }
}

/// <summary>Where a session sends its messages (the /ws/view socket).</summary>
public interface IViewSink
{
    /// <summary>Bytes the sink needs in front of the pixels (protocol header).</summary>
    int FrameHeaderBytes { get; }

    /// <summary>Sends a frame. <paramref name="rented"/> comes from ArrayPool&lt;byte&gt;.Shared with the pixels at
    /// <see cref="FrameHeaderBytes"/>; the sink owns it from now on and returns it to the pool.</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seq">Sequence number.</param>
    /// <param name="format">Format.</param>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    /// <param name="rented">Buffer.</param>
    /// <param name="length">Header plus pixel bytes.</param>
    void SendFrame(int viewport, uint seq, FramePixelFormat format, int width, int height, byte[] rented, int length);

    /// <summary>Sends a state message.</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seq">Sequence number of the frame it describes.</param>
    /// <param name="state">State.</param>
    void SendState(int viewport, uint seq, ViewportStateMessage state);
}
