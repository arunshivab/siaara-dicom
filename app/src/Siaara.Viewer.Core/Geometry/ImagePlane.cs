// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Patient-space geometry of image planes, for reference lines, cross-reference and linked
// scrolling. The toolkit supplies the plane of each frame (siaara_frame_geometry: position of the
// first pixel centre, row / column direction cosines, spacing); the maths below is the app's own.
// Coordinates: patient space in mm (DICOM LPS); image pixel coordinates with (0,0) at the top-left
// CORNER of the first pixel, so the centre of pixel (c, r) is (c + 0.5, r + 0.5), as in the
// measurement code and the toolkit's viewport mapping.

using Siaara.Viewer.Core.Measure;

namespace Siaara.Viewer.Core.Geometry;

/// <summary>A point or direction in patient space (mm).</summary>
/// <param name="X">X (towards the patient's left).</param>
/// <param name="Y">Y (towards posterior).</param>
/// <param name="Z">Z (towards head).</param>
public readonly record struct Vec3(double X, double Y, double Z)
{
    /// <summary>Sum.</summary>
    /// <param name="a">A.</param>
    /// <param name="b">B.</param>
    /// <returns>a + b.</returns>
    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);

    /// <summary>Difference.</summary>
    /// <param name="a">A.</param>
    /// <param name="b">B.</param>
    /// <returns>a - b.</returns>
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);

    /// <summary>Scaling.</summary>
    /// <param name="a">Vector.</param>
    /// <param name="k">Factor.</param>
    /// <returns>a k.</returns>
    public static Vec3 operator *(Vec3 a, double k) => new(a.X * k, a.Y * k, a.Z * k);

    /// <summary>Sum (named alternative of the operator).</summary>
    /// <param name="a">A.</param>
    /// <param name="b">B.</param>
    /// <returns>a + b.</returns>
    public static Vec3 Add(Vec3 a, Vec3 b) => a + b;

    /// <summary>Difference (named alternative of the operator).</summary>
    /// <param name="a">A.</param>
    /// <param name="b">B.</param>
    /// <returns>a - b.</returns>
    public static Vec3 Subtract(Vec3 a, Vec3 b) => a - b;

    /// <summary>Scaling (named alternative of the operator).</summary>
    /// <param name="a">Vector.</param>
    /// <param name="k">Factor.</param>
    /// <returns>a k.</returns>
    public static Vec3 Multiply(Vec3 a, double k) => a * k;

    /// <summary>Dot product.</summary>
    /// <param name="o">Other.</param>
    /// <returns>The product.</returns>
    public double Dot(Vec3 o) => (X * o.X) + (Y * o.Y) + (Z * o.Z);

    /// <summary>Cross product.</summary>
    /// <param name="o">Other.</param>
    /// <returns>this x o.</returns>
    public Vec3 Cross(Vec3 o) => new((Y * o.Z) - (Z * o.Y), (Z * o.X) - (X * o.Z), (X * o.Y) - (Y * o.X));

    /// <summary>Length.</summary>
    public double Length => Math.Sqrt(Dot(this));

    /// <summary>Unit vector in the same direction (zero stays zero).</summary>
    /// <returns>The unit vector.</returns>
    public Vec3 Normalized()
    {
        double n = Length;
        return n > 0 ? this * (1 / n) : this;
    }
}

/// <summary>The plane of one frame in patient space.</summary>
public sealed class ImagePlane
{
    /// <summary>Creates a plane.</summary>
    /// <param name="frameOfReferenceUid">Frame of Reference UID ("" when absent).</param>
    /// <param name="firstPixelCentre">Image Position (Patient): centre of the first pixel.</param>
    /// <param name="rowDirection">Direction of increasing column index (image x).</param>
    /// <param name="columnDirection">Direction of increasing row index (image y).</param>
    /// <param name="columnSpacing">mm between column centres (applies to x).</param>
    /// <param name="rowSpacing">mm between row centres (applies to y).</param>
    /// <param name="columns">Columns.</param>
    /// <param name="rows">Rows.</param>
    public ImagePlane(string frameOfReferenceUid, Vec3 firstPixelCentre, Vec3 rowDirection, Vec3 columnDirection, double columnSpacing,
                      double rowSpacing, int columns, int rows)
    {
        FrameOfReferenceUid = frameOfReferenceUid;
        FirstPixelCentre = firstPixelCentre;
        RowDirection = rowDirection.Normalized();
        ColumnDirection = columnDirection.Normalized();
        Normal = RowDirection.Cross(ColumnDirection).Normalized();
        ColumnSpacing = columnSpacing;
        RowSpacing = rowSpacing;
        Columns = columns;
        Rows = rows;
    }

    /// <summary>Frame of Reference UID.</summary>
    public string FrameOfReferenceUid { get; }

    /// <summary>Centre of the first pixel.</summary>
    public Vec3 FirstPixelCentre { get; }

    /// <summary>Unit direction of image x.</summary>
    public Vec3 RowDirection { get; }

    /// <summary>Unit direction of image y.</summary>
    public Vec3 ColumnDirection { get; }

    /// <summary>Unit normal (row x column).</summary>
    public Vec3 Normal { get; }

    /// <summary>mm per pixel along x.</summary>
    public double ColumnSpacing { get; }

    /// <summary>mm per pixel along y.</summary>
    public double RowSpacing { get; }

    /// <summary>Columns.</summary>
    public int Columns { get; }

    /// <summary>Rows.</summary>
    public int Rows { get; }

    /// <summary>Centre of the image in patient space.</summary>
    public Vec3 Centre => ToPatient(new PointD(Columns / 2.0, Rows / 2.0));

    /// <summary>True when the planes share a frame of reference (non-empty and equal).</summary>
    /// <param name="other">Other plane.</param>
    /// <returns>True when they can be related.</returns>
    public bool SameFrameOfReference(ImagePlane other)
    {
        ArgumentNullException.ThrowIfNull(other);
        return FrameOfReferenceUid.Length > 0 && string.Equals(FrameOfReferenceUid, other.FrameOfReferenceUid, StringComparison.Ordinal);
    }

    /// <summary>Image pixel coordinates (corner convention) to patient space.</summary>
    /// <param name="p">Image point.</param>
    /// <returns>Patient point.</returns>
    public Vec3 ToPatient(PointD p) =>
        FirstPixelCentre + (RowDirection * ((p.X - 0.5) * ColumnSpacing)) + (ColumnDirection * ((p.Y - 0.5) * RowSpacing));

    /// <summary>Orthogonal projection of a patient point onto the plane, in image pixel coordinates.</summary>
    /// <param name="q">Patient point.</param>
    /// <returns>The image point and the signed distance from the plane (mm, along the normal).</returns>
    public (PointD Point, double Distance) ToImage(Vec3 q)
    {
        Vec3 d = q - FirstPixelCentre;
        return (new PointD((d.Dot(RowDirection) / ColumnSpacing) + 0.5, (d.Dot(ColumnDirection) / RowSpacing) + 0.5), d.Dot(Normal));
    }

    /// <summary>|cos| of the angle between the normals (1 = parallel planes).</summary>
    /// <param name="other">Other plane.</param>
    /// <returns>The value.</returns>
    public double Parallelism(ImagePlane other)
    {
        ArgumentNullException.ThrowIfNull(other);
        return Math.Abs(Normal.Dot(other.Normal));
    }

    /// <summary>Corners of the image (corner convention) in patient space, clockwise from the top left.</summary>
    /// <returns>Four points.</returns>
    public Vec3[] Corners() =>
    [
        ToPatient(new PointD(0, 0)), ToPatient(new PointD(Columns, 0)), ToPatient(new PointD(Columns, Rows)), ToPatient(new PointD(0, Rows)),
    ];
}

/// <summary>Reference-line and slice-matching maths.</summary>
public static class PlaneMath
{
    /// <summary>Planes closer to parallel than this (|cos| of the normals) have no reference line.</summary>
    public const double ParallelLimit = 0.9999;

    /// <summary>
    /// Where the source plane cuts the target image: the segment of the intersection line that lies
    /// inside both images, in the target's pixel coordinates. Null when the planes are (nearly)
    /// parallel, do not share a frame of reference, or do not overlap.
    /// </summary>
    /// <param name="source">Plane shown elsewhere (the active viewport's slice).</param>
    /// <param name="target">Plane on which the line is drawn.</param>
    /// <returns>Segment end points in target pixel coordinates, or null.</returns>
    public static (PointD A, PointD B)? ReferenceLine(ImagePlane source, ImagePlane target)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(target);
        if (!source.SameFrameOfReference(target) || source.Parallelism(target) > ParallelLimit)
        {
            return null;
        }
        // Clip the source image rectangle against the target plane: the polygon edges that cross the
        // plane give the part of the intersection line covered by the source image.
        Vec3[] c = source.Corners();
        var hits = new List<Vec3>(4);
        for (int i = 0; i < 4; i++)
        {
            Vec3 p = c[i], q = c[(i + 1) % 4];
            double dp = (p - target.FirstPixelCentre).Dot(target.Normal), dq = (q - target.FirstPixelCentre).Dot(target.Normal);
            if (dp == 0)
            {
                hits.Add(p);
            }
            else if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0))
            {
                hits.Add(p + ((q - p) * (dp / (dp - dq))));
            }
        }
        if (hits.Count < 2)
        {
            return null;
        }
        // The two extreme hits along the line direction.
        Vec3 dir = source.Normal.Cross(target.Normal).Normalized();
        Vec3 lo = hits[0], hi = hits[0];
        foreach (Vec3 h in hits)
        {
            if (h.Dot(dir) < lo.Dot(dir))
            {
                lo = h;
            }
            if (h.Dot(dir) > hi.Dot(dir))
            {
                hi = h;
            }
        }
        PointD a = target.ToImage(lo).Point, b = target.ToImage(hi).Point;
        return ClipToRect(a, b, target.Columns, target.Rows);
    }

    /// <summary>Liang-Barsky clip of a segment to [0, width] x [0, height].</summary>
    /// <param name="a">Start.</param>
    /// <param name="b">End.</param>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    /// <returns>The clipped segment, or null when it is outside.</returns>
    public static (PointD A, PointD B)? ClipToRect(PointD a, PointD b, double width, double height)
    {
        double t0 = 0, t1 = 1, dx = b.X - a.X, dy = b.Y - a.Y;
        bool Edge(double p, double q)
        {
            if (p == 0)
            {
                return q >= 0;
            }
            double r = q / p;
            if (p < 0)
            {
                if (r > t1)
                {
                    return false;
                }
                t0 = Math.Max(t0, r);
            }
            else
            {
                if (r < t0)
                {
                    return false;
                }
                t1 = Math.Min(t1, r);
            }
            return true;
        }
        if (!Edge(-dx, a.X) || !Edge(dx, width - a.X) || !Edge(-dy, a.Y) || !Edge(dy, height - a.Y))
        {
            return null;
        }
        return (new PointD(a.X + (t0 * dx), a.Y + (t0 * dy)), new PointD(a.X + (t1 * dx), a.Y + (t1 * dy)));
    }

    /// <summary>
    /// Index of the plane nearest to a patient point (smallest distance along each plane's normal),
    /// among planes of the same frame of reference; -1 when none qualifies or the nearest is farther
    /// than <paramref name="maxDistanceMm"/>.
    /// </summary>
    /// <param name="planes">Planes (null entries skipped).</param>
    /// <param name="frameOfReferenceUid">Frame of reference of the point.</param>
    /// <param name="point">Patient point.</param>
    /// <param name="maxDistanceMm">Largest accepted distance.</param>
    /// <returns>The index, or -1.</returns>
    public static int NearestPlane(IReadOnlyList<ImagePlane?> planes, string frameOfReferenceUid, Vec3 point, double maxDistanceMm = double.PositiveInfinity)
    {
        ArgumentNullException.ThrowIfNull(planes);
        if (string.IsNullOrEmpty(frameOfReferenceUid))
        {
            return -1;
        }
        int best = -1;
        double bestDistance = double.PositiveInfinity;
        for (int i = 0; i < planes.Count; i++)
        {
            if (planes[i] is not { } p || !string.Equals(p.FrameOfReferenceUid, frameOfReferenceUid, StringComparison.Ordinal))
            {
                continue;
            }
            double d = Math.Abs(p.ToImage(point).Distance);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = i;
            }
        }
        return bestDistance <= maxDistanceMm ? best : -1;
    }
}
