// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text.Json.Serialization;

namespace Siaara.Viewer.Core.Measure;

/// <summary>Kinds of annotation.</summary>
public enum AnnotationKind
{
    /// <summary>Two-point length.</summary>
    Length = 0,
    /// <summary>Open polyline length.</summary>
    Polyline = 1,
    /// <summary>Three-point angle (vertex in the middle).</summary>
    Angle = 2,
    /// <summary>Cobb angle between two lines (four points).</summary>
    Cobb = 3,
    /// <summary>Ellipse ROI from the two corners of its bounding box.</summary>
    Ellipse = 4,
    /// <summary>Circle ROI: centre and a point on the circle.</summary>
    Circle = 5,
    /// <summary>Rectangle ROI from two corners.</summary>
    Rectangle = 6,
    /// <summary>Polygon ROI.</summary>
    Polygon = 7,
    /// <summary>Freehand ROI.</summary>
    Freehand = 8,
    /// <summary>One-pixel probe.</summary>
    Probe = 9,
    /// <summary>Manual calibration line (true length typed by the user).</summary>
    Calibration = 10,
    /// <summary>Arrow: tip (first point) and tail.</summary>
    Arrow = 11,
    /// <summary>Text label at one point.</summary>
    Text = 12,
}

/// <summary>An annotation in image pixel coordinates (stored per SOP + frame + user).</summary>
public sealed class Annotation
{
    /// <summary>Identifier.</summary>
    public string Id { get; set; } = Guid.NewGuid().ToString("N");

    /// <summary>Kind.</summary>
    public AnnotationKind Kind { get; set; }

    /// <summary>Points (image pixel coordinates), x y pairs.</summary>
    public IList<double> Points { get; init; } = [];

    /// <summary>For calibration lines: the true length in mm.</summary>
    public double? TrueLengthMm { get; set; }

    /// <summary>User text of a Text annotation (stored as typed).</summary>
    public string? Text { get; set; }

    /// <summary>Computed text (not stored: recomputed on load).</summary>
    [JsonIgnore]
    public string Label { get; set; } = "";

    /// <summary>Warnings shown with a warning sign (not stored).</summary>
    [JsonIgnore]
    public IList<string> Warnings { get; init; } = [];

    /// <summary>Spacing source tag for the icon (not stored).</summary>
    [JsonIgnore]
    public string Spacing { get; set; } = "";

    /// <summary>Number of points.</summary>
    [JsonIgnore]
    public int Count => Points.Count / 2;

    /// <summary>Point i.</summary>
    /// <param name="i">Index.</param>
    /// <returns>The point.</returns>
    public PointD At(int i) => new(Points[2 * i], Points[(2 * i) + 1]);

    /// <summary>Appends a point.</summary>
    /// <param name="p">Point.</param>
    public void Add(PointD p)
    {
        Points.Add(p.X);
        Points.Add(p.Y);
    }

    /// <summary>Replaces the last point.</summary>
    /// <param name="p">Point.</param>
    public void SetLast(PointD p)
    {
        Points[^2] = p.X;
        Points[^1] = p.Y;
    }

    /// <summary>A deep copy.</summary>
    /// <returns>The copy.</returns>
    public Annotation Clone() => new() { Id = Id, Kind = Kind, Points = [.. Points], TrueLengthMm = TrueLengthMm, Text = Text, Label = Label, Spacing = Spacing, Warnings = [.. Warnings] };
}

/// <summary>The stored annotations of one frame of one instance for one user.</summary>
public sealed class AnnotationDocument
{
    /// <summary>Format version.</summary>
    public int Version { get; set; } = 1;
    /// <summary>SOP Instance UID.</summary>
    public string SopInstanceUid { get; set; } = "";
    /// <summary>Frame.</summary>
    public int Frame { get; set; }
    /// <summary>Annotations.</summary>
    public IList<Annotation> Annotations { get; init; } = [];
    /// <summary>Manual calibration of the instance (mm per pixel), or null.</summary>
    public double? CalibrationMmPerPixel { get; set; }
}

[JsonSourceGenerationOptions(WriteIndented = false, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, UseStringEnumConverter = true)]
[JsonSerializable(typeof(AnnotationDocument))]
internal sealed partial class AnnotationJsonContext : JsonSerializerContext;
