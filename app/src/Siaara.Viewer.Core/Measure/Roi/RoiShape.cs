// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// ROI shapes in image pixel coordinates ((0,0) = top-left corner of the first pixel; the centre
// of pixel (c, r) is (c + 0.5, r + 0.5)). Written for the app; the behaviour (parameter rules,
// validation, the equal-area polygon used for ellipses) matches what the toolkit did before ROI
// statistics moved into the app, as checked against the golden file in the tests.

namespace Siaara.Viewer.Core.Measure.Roi;

/// <summary>Kind of ROI; the numbers are those of the former toolkit call (SIAARA_ROI_*), kept for the golden file.</summary>
public enum RoiShapeKind
{
    /// <summary>Parameters x0 y0 x1 y1 (any two opposite corners).</summary>
    Rectangle = 0,
    /// <summary>Parameters cx cy rx ry angle (radians, rotation of the rx axis from +x towards +y).</summary>
    Ellipse = 1,
    /// <summary>Parameters cx cy radius.</summary>
    Circle = 2,
    /// <summary>x y pairs (at least 3 vertices; closed implicitly).</summary>
    Polygon = 3,
    /// <summary>x y pairs, as a polygon.</summary>
    Freehand = 4,
}

/// <summary>A shape whose parameters are invalid (degenerate, not finite, wrong count).</summary>
public sealed class RoiShapeException : ArgumentException
{
    /// <summary>Creates the exception.</summary>
    public RoiShapeException()
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Plain-word reason.</param>
    public RoiShapeException(string message) : base(message)
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Plain-word reason.</param>
    /// <param name="innerException">Cause.</param>
    public RoiShapeException(string message, Exception innerException) : base(message, innerException)
    {
    }
}

/// <summary>An ROI shape.</summary>
public sealed class RoiShape
{
    /// <summary>Vertices of the polygon that stands in for an ellipse or circle in area weighting.</summary>
    public const int EllipseVertices = 1024;

    private RoiShape(RoiShapeKind kind) => Kind = kind;

    /// <summary>Kind.</summary>
    public RoiShapeKind Kind { get; }

    /// <summary>Rectangle corner x.</summary>
    public double X0 { get; private init; }

    /// <summary>Rectangle corner y.</summary>
    public double Y0 { get; private init; }

    /// <summary>Opposite rectangle corner x.</summary>
    public double X1 { get; private init; }

    /// <summary>Opposite rectangle corner y.</summary>
    public double Y1 { get; private init; }

    /// <summary>Ellipse / circle centre x.</summary>
    public double CenterX { get; private init; }

    /// <summary>Ellipse / circle centre y.</summary>
    public double CenterY { get; private init; }

    /// <summary>Semi-axis along the rotated x direction (circle: the radius).</summary>
    public double RadiusX { get; private init; }

    /// <summary>Semi-axis along the rotated y direction (circle: the radius).</summary>
    public double RadiusY { get; private init; }

    /// <summary>Ellipse rotation in radians.</summary>
    public double Angle { get; private init; }

    /// <summary>Polygon / freehand vertices.</summary>
    public IReadOnlyList<PointD> Vertices { get; private init; } = [];

    /// <summary>A rectangle.</summary>
    /// <param name="x0">Corner x.</param>
    /// <param name="y0">Corner y.</param>
    /// <param name="x1">Opposite corner x.</param>
    /// <param name="y1">Opposite corner y.</param>
    /// <returns>The shape.</returns>
    public static RoiShape Rectangle(double x0, double y0, double x1, double y1) => new(RoiShapeKind.Rectangle) { X0 = x0, Y0 = y0, X1 = x1, Y1 = y1 };

    /// <summary>A circle.</summary>
    /// <param name="cx">Centre x.</param>
    /// <param name="cy">Centre y.</param>
    /// <param name="radius">Radius.</param>
    /// <returns>The shape.</returns>
    public static RoiShape Circle(double cx, double cy, double radius) =>
        new(RoiShapeKind.Circle) { CenterX = cx, CenterY = cy, RadiusX = radius, RadiusY = radius };

    /// <summary>An ellipse.</summary>
    /// <param name="cx">Centre x.</param>
    /// <param name="cy">Centre y.</param>
    /// <param name="rx">Semi-axis along the rotated x direction.</param>
    /// <param name="ry">Semi-axis along the rotated y direction.</param>
    /// <param name="angle">Rotation in radians.</param>
    /// <returns>The shape.</returns>
    public static RoiShape Ellipse(double cx, double cy, double rx, double ry, double angle = 0) =>
        new(RoiShapeKind.Ellipse) { CenterX = cx, CenterY = cy, RadiusX = rx, RadiusY = ry, Angle = angle };

    /// <summary>A polygon.</summary>
    /// <param name="vertices">Vertices.</param>
    /// <returns>The shape.</returns>
    public static RoiShape Polygon(IEnumerable<PointD> vertices) => new(RoiShapeKind.Polygon) { Vertices = [.. vertices] };

    /// <summary>A freehand outline.</summary>
    /// <param name="vertices">Vertices.</param>
    /// <returns>The shape.</returns>
    public static RoiShape Freehand(IEnumerable<PointD> vertices) => new(RoiShapeKind.Freehand) { Vertices = [.. vertices] };

    /// <summary>A shape from a kind and a flat parameter list (the layout of <see cref="RoiShapeKind"/>).</summary>
    /// <param name="kind">Kind.</param>
    /// <param name="p">Parameters.</param>
    /// <returns>The shape.</returns>
    /// <exception cref="RoiShapeException">Wrong number of parameters.</exception>
    public static RoiShape FromParameters(RoiShapeKind kind, IReadOnlyList<double> p)
    {
        ArgumentNullException.ThrowIfNull(p);
        switch (kind)
        {
            case RoiShapeKind.Rectangle when p.Count == 4:
                return Rectangle(p[0], p[1], p[2], p[3]);
            case RoiShapeKind.Ellipse when p.Count == 5:
                return Ellipse(p[0], p[1], p[2], p[3], p[4]);
            case RoiShapeKind.Circle when p.Count == 3:
                return Circle(p[0], p[1], p[2]);
            case RoiShapeKind.Polygon or RoiShapeKind.Freehand when p.Count >= 6 && p.Count % 2 == 0:
                {
                    var v = new PointD[p.Count / 2];
                    for (int i = 0; i < v.Length; i++)
                    {
                        v[i] = new PointD(p[2 * i], p[(2 * i) + 1]);
                    }
                    return kind == RoiShapeKind.Polygon ? Polygon(v) : Freehand(v);
                }
            default:
                throw new RoiShapeException(kind switch
                {
                    RoiShapeKind.Rectangle => "a rectangle needs 4 parameters",
                    RoiShapeKind.Ellipse => "an ellipse needs 5 parameters",
                    RoiShapeKind.Circle => "a circle needs 3 parameters",
                    RoiShapeKind.Polygon or RoiShapeKind.Freehand => "a polygon needs x y pairs (at least 3 vertices)",
                    _ => "unknown ROI shape",
                });
        }
    }

    /// <summary>Shape area in pixel units (analytic; polygons by the shoelace formula).</summary>
    public double Area => Kind switch
    {
        RoiShapeKind.Rectangle => Math.Abs((X1 - X0) * (Y1 - Y0)),
        RoiShapeKind.Circle => Math.PI * RadiusX * RadiusX,
        RoiShapeKind.Ellipse => Math.PI * RadiusX * RadiusY,
        _ => Vertices.Count < 3 ? 0 : Math.Abs(Shoelace(Vertices)),
    };

    /// <summary>Signed area of a closed polygon.</summary>
    /// <param name="v">Vertices.</param>
    /// <returns>Area (positive for counter-clockwise in a y-up frame).</returns>
    public static double Shoelace(IReadOnlyList<PointD> v)
    {
        ArgumentNullException.ThrowIfNull(v);
        double a = 0;
        int n = v.Count;
        for (int i = 0, j = n - 1; i < n; j = i++)
        {
            a += (v[j].X * v[i].Y) - (v[i].X * v[j].Y);
        }
        return 0.5 * a;
    }

    /// <summary>Throws when the shape has no area or a parameter is not finite.</summary>
    /// <exception cref="RoiShapeException">Invalid shape.</exception>
    public void Validate()
    {
        static bool F(double v) => double.IsFinite(v);
        switch (Kind)
        {
            case RoiShapeKind.Rectangle:
                if (!F(X0) || !F(Y0) || !F(X1) || !F(Y1) || X0 == X1 || Y0 == Y1)
                {
                    throw new RoiShapeException("degenerate rectangle");
                }
                return;
            case RoiShapeKind.Circle:
                if (!F(CenterX) || !F(CenterY) || !F(RadiusX) || !(RadiusX > 0))
                {
                    throw new RoiShapeException("degenerate circle");
                }
                return;
            case RoiShapeKind.Ellipse:
                if (!F(CenterX) || !F(CenterY) || !F(Angle) || !F(RadiusX) || !F(RadiusY) || !(RadiusX > 0) || !(RadiusY > 0))
                {
                    throw new RoiShapeException("degenerate ellipse");
                }
                return;
            default:
                if (Vertices.Count < 3)
                {
                    throw new RoiShapeException("a polygon needs at least 3 vertices");
                }
                if (Vertices.Count > (1 << 24))
                {
                    throw new RoiShapeException("too many polygon vertices");
                }
                foreach (PointD p in Vertices)
                {
                    if (!F(p.X) || !F(p.Y))
                    {
                        throw new RoiShapeException("a polygon vertex is not a finite number");
                    }
                }
                if (Math.Abs(Shoelace(Vertices)) <= 0)
                {
                    throw new RoiShapeException("the polygon has no area");
                }
                return;
        }
    }

    /// <summary>
    /// The outline as a polygon: the rectangle's corners, the vertices, or for an ellipse / circle a
    /// <see cref="EllipseVertices"/>-gon with exactly the ellipse's area (vertices on the ellipse
    /// scaled by sqrt(2 pi / (N sin(2 pi / N)))).
    /// </summary>
    /// <returns>Vertices.</returns>
    public IReadOnlyList<PointD> AsPolygon()
    {
        switch (Kind)
        {
            case RoiShapeKind.Rectangle:
                {
                    double xa = Math.Min(X0, X1), xb = Math.Max(X0, X1), ya = Math.Min(Y0, Y1), yb = Math.Max(Y0, Y1);
                    return [new PointD(xa, ya), new PointD(xb, ya), new PointD(xb, yb), new PointD(xa, yb)];
                }
            case RoiShapeKind.Circle:
                return EqualAreaPolygon(CenterX, CenterY, RadiusX, RadiusX, 0);
            case RoiShapeKind.Ellipse:
                return EqualAreaPolygon(CenterX, CenterY, RadiusX, RadiusY, Angle);
            default:
                return Vertices;
        }
    }

    private static PointD[] EqualAreaPolygon(double cx, double cy, double rx, double ry, double angle)
    {
        const double n = EllipseVertices;
        double k = Math.Sqrt(2.0 * Math.PI / (n * Math.Sin(2.0 * Math.PI / n)));
        double ca = Math.Cos(angle), sa = Math.Sin(angle);
        var v = new PointD[EllipseVertices];
        for (int i = 0; i < EllipseVertices; i++)
        {
            double t = 2.0 * Math.PI * i / n;
            double ex = rx * k * Math.Cos(t), ey = ry * k * Math.Sin(t);
            v[i] = new PointD(cx + (ex * ca) - (ey * sa), cy + (ex * sa) + (ey * ca));
        }
        return v;
    }
}
