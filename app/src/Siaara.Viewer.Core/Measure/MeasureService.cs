// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Measure;

/// <summary>What the measurement maths needs to know about one frame.</summary>
public sealed class MeasureContext
{
    /// <summary>Distance between rows (mm), 0 when unknown.</summary>
    public double RowSpacing { get; init; }
    /// <summary>Distance between columns (mm), 0 when unknown.</summary>
    public double ColumnSpacing { get; init; }
    /// <summary>Where the spacing came from.</summary>
    public TkSpacingSource Source { get; init; }
    /// <summary>Manual calibration (mm per pixel) or null.</summary>
    public double? CalibrationMmPerPixel { get; init; }
    /// <summary>Modality.</summary>
    public string Modality { get; init; } = "";
    /// <summary>Value units (from Rescale Type or the modality).</summary>
    public string Units { get; init; } = "";
    /// <summary>Grayscale image (ROI statistics need it).</summary>
    public bool Monochrome { get; init; } = true;
    /// <summary>SUVbw factor for PET, or null.</summary>
    public TkSuv? Suv { get; init; }
    /// <summary>Frame index for ROI statistics.</summary>
    public int Frame { get; init; }
    /// <summary>Gives the full (pixel data) file for ROI statistics; null = no ROI statistics.</summary>
    public Func<Imaging.FileLease>? PixelFile { get; init; }

    /// <summary>The spacing tag sent to the browser for the icon.</summary>
    public string SpacingTag => CalibrationMmPerPixel is not null ? "manual" : Source switch
    {
        TkSpacingSource.PixelSpacing => "pixel",
        TkSpacingSource.Imager => "imager",
        TkSpacingSource.ImagerMagnified => "magnified",
        TkSpacingSource.NominalScanned => "nominal",
        TkSpacingSource.UltrasoundRegion => "us",
        _ => "none",
    };

    /// <summary>True when lengths can be given in mm.</summary>
    public bool HasSpacing => CalibrationMmPerPixel is > 0 || (RowSpacing > 0 && ColumnSpacing > 0);

    /// <summary>Units of a modality value from Rescale Type, else the modality's usual unit.</summary>
    /// <param name="rescaleType">Rescale Type (0028,1054) or null.</param>
    /// <param name="modality">Modality.</param>
    /// <param name="petUnits">Units (0054,1001) or null.</param>
    /// <returns>Unit text.</returns>
    public static string UnitsFor(string? rescaleType, string modality, string? petUnits)
    {
        if (!string.IsNullOrWhiteSpace(rescaleType) && !string.Equals(rescaleType.Trim(), "US", StringComparison.Ordinal))
        {
            return rescaleType.Trim();
        }
        return modality switch
        {
            "CT" => "HU",
            "PT" => (petUnits ?? "").Trim() switch
            {
                "BQML" => "Bq/ml",
                "CNTS" => "counts",
                "" => "",
                string u => u,
            },
            _ => "",
        };
    }
}

/// <summary>
/// Measurement numbers, computed on the server: lengths and angles from pixel spacing (pixels
/// when the spacing source is NONE), ROI statistics through the toolkit's roi_stats_compute with
/// partial-pixel weighting, SUVbw for PET.
/// </summary>
public static class MeasureService
{
    /// <summary>Recomputes the label and warnings of an annotation.</summary>
    /// <param name="a">Annotation.</param>
    /// <param name="ctx">Frame context.</param>
    public static void Update(Annotation a, MeasureContext ctx)
    {
        ArgumentNullException.ThrowIfNull(a);
        ArgumentNullException.ThrowIfNull(ctx);
        a.Warnings.Clear();
        a.Spacing = ctx.SpacingTag;
        a.Label = a.Kind switch
        {
            AnnotationKind.Length => a.Count >= 2 ? FormatLength(Length(a.At(0), a.At(1), ctx), ctx) : "",
            AnnotationKind.Calibration => a.Count >= 2 && a.TrueLengthMm is double t
                ? DisplayFormat.Number(t, 2) + " mm (manually calibrated)"
                : a.Count >= 2 ? FormatLength(Length(a.At(0), a.At(1), ctx), ctx) : "",
            AnnotationKind.Polyline => FormatLength(PolylineLength(a, ctx), ctx),
            AnnotationKind.Angle => a.Count >= 3 ? DisplayFormat.Number(Angle(a.At(0), a.At(1), a.At(2), ctx), 1) + "°" : "",
            AnnotationKind.Cobb => a.Count >= 4 ? "Cobb " + DisplayFormat.Number(Cobb(a.At(0), a.At(1), a.At(2), a.At(3), ctx), 1) + "°" : "",
            AnnotationKind.Probe => a.Count >= 1 ? ProbeText(a.At(0), ctx, a) : "",
            AnnotationKind.Arrow => "",
            AnnotationKind.Text => a.Text ?? "",
            _ => RoiText(a, ctx),
        };
    }

    /// <summary>Physical length between two image points (mm with spacing, else pixels).</summary>
    /// <param name="p">From.</param>
    /// <param name="q">To.</param>
    /// <param name="ctx">Context.</param>
    /// <returns>Length.</returns>
    public static double Length(PointD p, PointD q, MeasureContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        (double sx, double sy) = Scale(ctx);
        double dx = (q.X - p.X) * sx, dy = (q.Y - p.Y) * sy;
        return Math.Sqrt((dx * dx) + (dy * dy));
    }

    private static (double Sx, double Sy) Scale(MeasureContext ctx) =>
        ctx.CalibrationMmPerPixel is double c && c > 0 ? (c, c)
        : ctx.RowSpacing > 0 && ctx.ColumnSpacing > 0 ? (ctx.ColumnSpacing, ctx.RowSpacing)
        : (1, 1);

    private static double PolylineLength(Annotation a, MeasureContext ctx)
    {
        double s = 0;
        for (int i = 1; i < a.Count; i++)
        {
            s += Length(a.At(i - 1), a.At(i), ctx);
        }
        return s;
    }

    /// <summary>Text for a length: "12.3 mm" or "45.6 px".</summary>
    /// <param name="value">Value.</param>
    /// <param name="ctx">Context.</param>
    /// <returns>Text.</returns>
    public static string FormatLength(double value, MeasureContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        return ctx.HasSpacing ? DisplayFormat.Number(value, 1) + " mm" : DisplayFormat.Number(value, 1) + " px";
    }

    /// <summary>Angle at vertex b in degrees (physical space).</summary>
    /// <param name="a">First arm end.</param>
    /// <param name="b">Vertex.</param>
    /// <param name="c">Second arm end.</param>
    /// <param name="ctx">Context.</param>
    /// <returns>Degrees 0..180.</returns>
    public static double Angle(PointD a, PointD b, PointD c, MeasureContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        (double sx, double sy) = Scale(ctx);
        double ux = (a.X - b.X) * sx, uy = (a.Y - b.Y) * sy, vx = (c.X - b.X) * sx, vy = (c.Y - b.Y) * sy;
        double d = Math.Sqrt((ux * ux) + (uy * uy)) * Math.Sqrt((vx * vx) + (vy * vy));
        return d == 0 ? 0 : Math.Acos(Math.Clamp(((ux * vx) + (uy * vy)) / d, -1, 1)) * 180 / Math.PI;
    }

    /// <summary>Cobb angle: the acute angle between lines p0-p1 and p2-p3.</summary>
    /// <param name="p0">Line 1 start.</param>
    /// <param name="p1">Line 1 end.</param>
    /// <param name="p2">Line 2 start.</param>
    /// <param name="p3">Line 2 end.</param>
    /// <param name="ctx">Context.</param>
    /// <returns>Degrees 0..90.</returns>
    public static double Cobb(PointD p0, PointD p1, PointD p2, PointD p3, MeasureContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        (double sx, double sy) = Scale(ctx);
        double a1 = Math.Atan2((p1.Y - p0.Y) * sy, (p1.X - p0.X) * sx), a2 = Math.Atan2((p3.Y - p2.Y) * sy, (p3.X - p2.X) * sx);
        double d = Math.Abs(a1 - a2) * 180 / Math.PI % 180;
        return d > 90 ? 180 - d : d;
    }

    /// <summary>The toolkit ROI shape and parameters of an ROI annotation, or null when it has too few points.</summary>
    /// <param name="a">Annotation.</param>
    /// <returns>Shape and parameters.</returns>
    public static (TkRoiShape Shape, double[] Parameters)? RoiParameters(Annotation a)
    {
        ArgumentNullException.ThrowIfNull(a);
        switch (a.Kind)
        {
            case AnnotationKind.Rectangle when a.Count >= 2:
                {
                    PointD p = a.At(0), q = a.At(1);
                    return (TkRoiShape.Rectangle, [Math.Min(p.X, q.X), Math.Min(p.Y, q.Y), Math.Max(p.X, q.X), Math.Max(p.Y, q.Y)]);
                }
            case AnnotationKind.Ellipse when a.Count >= 2:
                {
                    PointD p = a.At(0), q = a.At(1);
                    return (TkRoiShape.Ellipse, [(p.X + q.X) / 2, (p.Y + q.Y) / 2, Math.Abs(q.X - p.X) / 2, Math.Abs(q.Y - p.Y) / 2, 0]);
                }
            case AnnotationKind.Circle when a.Count >= 2:
                {
                    PointD c = a.At(0), e = a.At(1);
                    return (TkRoiShape.Circle, [c.X, c.Y, Math.Sqrt(((e.X - c.X) * (e.X - c.X)) + ((e.Y - c.Y) * (e.Y - c.Y)))]);
                }
            case AnnotationKind.Polygon when a.Count >= 3:
                return (TkRoiShape.Polygon, [.. a.Points]);
            case AnnotationKind.Freehand when a.Count >= 3:
                return (TkRoiShape.Freehand, [.. a.Points]);
            case AnnotationKind.Probe when a.Count >= 1:
                {
                    double x = Math.Floor(a.At(0).X), y = Math.Floor(a.At(0).Y);
                    return (TkRoiShape.Rectangle, [x, y, x + 1, y + 1]);
                }
            default:
                return null;
        }
    }

    private static TkRoiStats? Stats(Annotation a, MeasureContext ctx)
    {
        if (ctx.PixelFile is null || !ctx.Monochrome || RoiParameters(a) is not var (shape, prm))
        {
            return null;
        }
        if (shape == TkRoiShape.Rectangle && (prm[2] - prm[0] <= 0 || prm[3] - prm[1] <= 0))
        {
            return null;
        }
        try
        {
            using Imaging.FileLease f = ctx.PixelFile();
            return f.File.ComputeRoi(ctx.Frame, shape, prm, partialWeighting: a.Kind != AnnotationKind.Probe);
        }
        catch (TkException)
        {
            return null;
        }
    }

    private static string ProbeText(PointD p, MeasureContext ctx, Annotation a)
    {
        string where = "(" + Math.Floor(p.X).ToString(CultureInfo.InvariantCulture) + ", " + Math.Floor(p.Y).ToString(CultureInfo.InvariantCulture) + ")";
        if (!ctx.Monochrome)
        {
            return where;
        }
        TkRoiStats? s = Stats(a, ctx);
        if (s is null || s.Pixels == 0)
        {
            return where + " outside the image";
        }
        string text = where + " " + DisplayFormat.Number(s.Mean, 1) + (ctx.Units.Length > 0 ? " " + ctx.Units : "");
        AddSuv(a, ctx, s, ref text, meanOnly: true);
        return text;
    }

    private static string RoiText(Annotation a, MeasureContext ctx)
    {
        if (!ctx.Monochrome)
        {
            a.Warnings.Add("ROI statistics need a grayscale image");
            return "";
        }
        TkRoiStats? s = Stats(a, ctx);
        if (s is null)
        {
            return "";
        }
        string u = ctx.Units.Length > 0 ? " " + ctx.Units : "";
        string area = s.AreaMm2 is double mm2 && ctx.CalibrationMmPerPixel is null
            ? DisplayFormat.Number(mm2, 1) + " mm²"
            : ctx.CalibrationMmPerPixel is double c ? DisplayFormat.Number(s.AreaPixels * c * c, 1) + " mm²" : DisplayFormat.Number(s.AreaPixels, 1) + " px²";
        string text = "Mean " + DisplayFormat.Number(s.Mean, 1) + u + "  SD " + DisplayFormat.Number(s.StandardDeviation, 1)
                      + "  Min " + DisplayFormat.Number(s.Min, 1) + "  Max " + DisplayFormat.Number(s.Max, 1) + "  Area " + area;
        AddSuv(a, ctx, s, ref text, meanOnly: false);
        return text;
    }

    private static void AddSuv(Annotation a, MeasureContext ctx, TkRoiStats s, ref string text, bool meanOnly)
    {
        if (!string.Equals(ctx.Modality, "PT", StringComparison.Ordinal) || ctx.Suv is null)
        {
            return;
        }
        foreach (string w in ctx.Suv.Warnings)
        {
            a.Warnings.Add(w);
        }
        if (ctx.Suv.BodyWeightFactor is double f)
        {
            text += meanOnly
                ? "  SUVbw " + DisplayFormat.Number(s.Mean * f, 2)
                : "  SUVbw mean " + DisplayFormat.Number(s.Mean * f, 2) + " max " + DisplayFormat.Number(s.Max * f, 2);
        }
        else
        {
            a.Warnings.Add("SUV cannot be computed");
        }
    }

    /// <summary>Screen outline of an annotation for the browser (polyline points in viewport pixels).</summary>
    /// <param name="a">Annotation.</param>
    /// <param name="t">Transform.</param>
    /// <returns>x y pairs and whether the outline is closed.</returns>
    public static (double[] Points, bool Closed) Outline(Annotation a, ScreenImageTransform t)
    {
        ArgumentNullException.ThrowIfNull(a);
        ArgumentNullException.ThrowIfNull(t);
        IEnumerable<PointD> pts;
        bool closed = false;
        switch (a.Kind)
        {
            case AnnotationKind.Ellipse when a.Count >= 2:
                {
                    PointD p = a.At(0), q = a.At(1);
                    double cx = (p.X + q.X) / 2, cy = (p.Y + q.Y) / 2, rx = Math.Abs(q.X - p.X) / 2, ry = Math.Abs(q.Y - p.Y) / 2;
                    pts = Enumerable.Range(0, 64).Select(k => new PointD(cx + (rx * Math.Cos(k * Math.PI / 32)), cy + (ry * Math.Sin(k * Math.PI / 32))));
                    closed = true;
                    break;
                }
            case AnnotationKind.Circle when a.Count >= 2:
                {
                    PointD c = a.At(0), e = a.At(1);
                    double r = Math.Sqrt(((e.X - c.X) * (e.X - c.X)) + ((e.Y - c.Y) * (e.Y - c.Y)));
                    pts = Enumerable.Range(0, 64).Select(k => new PointD(c.X + (r * Math.Cos(k * Math.PI / 32)), c.Y + (r * Math.Sin(k * Math.PI / 32))));
                    closed = true;
                    break;
                }
            case AnnotationKind.Rectangle when a.Count >= 2:
                {
                    PointD p = a.At(0), q = a.At(1);
                    pts = [p, new PointD(q.X, p.Y), q, new PointD(p.X, q.Y)];
                    closed = true;
                    break;
                }
            case AnnotationKind.Probe when a.Count >= 1:
                {
                    double x = Math.Floor(a.At(0).X), y = Math.Floor(a.At(0).Y);
                    pts = [new PointD(x, y), new PointD(x + 1, y), new PointD(x + 1, y + 1), new PointD(x, y + 1)];
                    closed = true;
                    break;
                }
            case AnnotationKind.Polygon:
            case AnnotationKind.Freehand:
                pts = Enumerable.Range(0, a.Count).Select(a.At);
                closed = true;
                break;
            default:
                pts = Enumerable.Range(0, a.Count).Select(a.At);
                break;
        }
        return (pts.Select(t.ToScreen).SelectMany(p => new[] { Math.Round(p.X, 1), Math.Round(p.Y, 1) }).ToArray(), closed);
    }
}
