// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>How one viewport presents its image; turned into a <see cref="RenderSpec"/> per render.</summary>
public sealed record DisplayState
{
    /// <summary>Output pixels per image pixel; 0 = fit.</summary>
    public double Zoom { get; init; }
    /// <summary>Pan x (viewport pixels).</summary>
    public double PanX { get; init; }
    /// <summary>Pan y.</summary>
    public double PanY { get; init; }
    /// <summary>Clockwise rotation (degrees).</summary>
    public double Rotation { get; init; }
    /// <summary>Mirror left-right.</summary>
    public bool FlipHorizontal { get; init; }
    /// <summary>Mirror top-bottom.</summary>
    public bool FlipVertical { get; init; }
    /// <summary>Invert grayscale.</summary>
    public bool Invert { get; init; }
    /// <summary>Window source.</summary>
    public TkVoiSource Voi { get; init; } = TkVoiSource.File;
    /// <summary>File VOI index.</summary>
    public int VoiIndex { get; init; }
    /// <summary>Explicit centre.</summary>
    public double WindowCenter { get; init; }
    /// <summary>Explicit width.</summary>
    public double WindowWidth { get; init; }
    /// <summary>Show overlays and shutters.</summary>
    public bool ShowOverlays { get; init; } = true;
    /// <summary>Bilinear interpolation.</summary>
    public bool Linear { get; init; } = true;

    /// <summary>The render spec for a viewport size.</summary>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    /// <param name="gray">Gray8 output.</param>
    /// <returns>The spec.</returns>
    public RenderSpec ToSpec(int width, int height, bool gray) => new()
    {
        Width = width,
        Height = height,
        Gray = gray,
        Zoom = Zoom,
        PanX = PanX,
        PanY = PanY,
        Rotation = Rotation,
        FlipHorizontal = FlipHorizontal,
        FlipVertical = FlipVertical,
        Linear = Linear && Zoom != 1,
        Voi = Voi,
        VoiIndex = VoiIndex,
        WindowCenter = WindowCenter,
        WindowWidth = Math.Max(1, WindowWidth),
        Invert = Invert,
        NoOverlays = !ShowOverlays,
        NoShutters = !ShowOverlays,
    };
}

/// <summary>Viewport grid layout (1x1 to 4x4).</summary>
/// <param name="Rows">Rows.</param>
/// <param name="Columns">Columns.</param>
public readonly record struct LayoutModel(int Rows, int Columns)
{
    /// <summary>Largest grid.</summary>
    public const int MaxSide = 4;

    /// <summary>Viewports in the grid.</summary>
    public int Count => Rows * Columns;

    /// <summary>A layout clamped to 1..4 per side.</summary>
    /// <param name="rows">Rows.</param>
    /// <param name="columns">Columns.</param>
    /// <returns>The layout.</returns>
    public static LayoutModel Of(int rows, int columns) => new(Math.Clamp(rows, 1, MaxSide), Math.Clamp(columns, 1, MaxSide));

    /// <summary>Default for a number of series: 1 → 1x1, 2 → 1x2, 3 or more → 2x2.</summary>
    /// <param name="seriesCount">Series.</param>
    /// <returns>The layout.</returns>
    public static LayoutModel ForSeriesCount(int seriesCount) => seriesCount switch
    {
        <= 1 => new LayoutModel(1, 1),
        2 => new LayoutModel(1, 2),
        _ => new LayoutModel(2, 2),
    };

    /// <summary>Parses "RxC" (for example "2x3").</summary>
    /// <param name="text">Text.</param>
    /// <param name="layout">Layout.</param>
    /// <returns>True when valid.</returns>
    public static bool TryParse(string? text, out LayoutModel layout)
    {
        layout = new LayoutModel(1, 1);
        string[] p = (text ?? "").Split('x', 'X');
        if (p.Length == 2 && int.TryParse(p[0], System.Globalization.CultureInfo.InvariantCulture, out int r)
            && int.TryParse(p[1], System.Globalization.CultureInfo.InvariantCulture, out int c) && r is >= 1 and <= MaxSide && c is >= 1 and <= MaxSide)
        {
            layout = new LayoutModel(r, c);
            return true;
        }
        return false;
    }

    /// <inheritdoc/>
    public override string ToString() => Rows.ToString(System.Globalization.CultureInfo.InvariantCulture) + "x" + Columns.ToString(System.Globalization.CultureInfo.InvariantCulture);
}
