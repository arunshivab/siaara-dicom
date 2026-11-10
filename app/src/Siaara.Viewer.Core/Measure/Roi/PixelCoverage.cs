// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Which pixels an ROI covers, and by how much. Centre mode: a pixel belongs to the ROI when its
// centre is inside (weight 1; even-odd rule with half-open edges for polygons, an exact test for
// ellipses and circles). Partial mode: pixels crossed by the outline get the exact area of the
// pixel square inside the polygon (Sutherland-Hodgman clip, shoelace), the others 0 or 1; ellipses
// and circles use their equal-area 1024-gon. Pixels are visited row by row, left to right.

namespace Siaara.Viewer.Core.Measure.Roi;

/// <summary>Pixel weights of an ROI.</summary>
public static class PixelCoverage
{
    private readonly record struct Box(long C0, long R0, long C1, long R1)
    {
        public bool IsEmpty => C1 < C0 || R1 < R0;
    }

    // Truncation towards zero after clamping; NaN maps to lo.
    private static long ClampToLong(double v, double lo, double hi)
    {
        if (!(v >= lo))
        {
            return (long)lo;
        }
        return v > hi ? (long)hi : (long)v;
    }

    private static Box Bounds(IReadOnlyList<PointD> poly, int rows, int cols)
    {
        double xa = double.PositiveInfinity, ya = xa, xb = double.NegativeInfinity, yb = xb;
        foreach (PointD p in poly)
        {
            xa = Math.Min(xa, p.X);
            xb = Math.Max(xb, p.X);
            ya = Math.Min(ya, p.Y);
            yb = Math.Max(yb, p.Y);
        }
        static double Clamp(double v, double hi) => v < 0 ? 0 : v > hi ? hi : v;
        long c0 = (long)Math.Floor(Clamp(xa, cols)), c1 = (long)Math.Floor(Clamp(xb, cols));
        long r0 = (long)Math.Floor(Clamp(ya, rows)), r1 = (long)Math.Floor(Clamp(yb, rows));
        return new Box(c0, r0, Math.Min(c1, cols - 1L), Math.Min(r1, rows - 1L));
    }

    /// <summary>Calls <paramref name="visit"/>(pixel index, weight) for every pixel with a non-zero weight, row by row.</summary>
    /// <param name="rows">Image rows.</param>
    /// <param name="columns">Image columns.</param>
    /// <param name="shape">Shape.</param>
    /// <param name="partial">Weight edge pixels by covered area.</param>
    /// <param name="visit">Callback (index = row * columns + column).</param>
    /// <exception cref="RoiShapeException">Invalid shape.</exception>
    public static void ForEachWeight(int rows, int columns, RoiShape shape, bool partial, Action<long, double> visit)
    {
        ArgumentNullException.ThrowIfNull(shape);
        ArgumentNullException.ThrowIfNull(visit);
        shape.Validate();
        if (rows <= 0 || columns <= 0)
        {
            return;
        }
        if (!partial && shape.Kind is RoiShapeKind.Ellipse or RoiShapeKind.Circle)
        {
            CentreTestEllipse(rows, columns, shape, visit);
            return;
        }
        IReadOnlyList<PointD> poly = shape.AsPolygon();
        Box b = Bounds(poly, rows, columns);
        if (b.IsEmpty)
        {
            return;
        }
        int bw = (int)(b.C1 - b.C0 + 1), bh = (int)(b.R1 - b.R0 + 1);
        byte[] inside = ScanCentres(poly, b, bw, bh, columns);
        if (!partial)
        {
            for (int r = 0; r < bh; r++)
            {
                for (int c = 0; c < bw; c++)
                {
                    if (inside[(r * bw) + c] != 0)
                    {
                        visit(((b.R0 + r) * columns) + b.C0 + c, 1.0);
                    }
                }
            }
            return;
        }
        byte[] edge = MarkEdges(poly, b, bw, bh, rows, columns);
        var work = new List<PointD>(16);
        var spare = new List<PointD>(16);
        for (int r = 0; r < bh; r++)
        {
            for (int c = 0; c < bw; c++)
            {
                int k = (r * bw) + c;
                double x0 = b.C0 + (double)c, y0 = b.R0 + (double)r;
                double w = edge[k] != 0 ? Math.Min(1.0, ClippedArea(poly, x0, y0, x0 + 1.0, y0 + 1.0, work, spare)) : inside[k] != 0 ? 1.0 : 0.0;
                if (w > 0.0)
                {
                    visit(((b.R0 + r) * columns) + b.C0 + c, w);
                }
            }
        }
    }

    // Exact centre test for ellipses and circles (no polygon).
    private static void CentreTestEllipse(int rows, int columns, RoiShape shape, Action<long, double> visit)
    {
        double rx = shape.RadiusX, ry = shape.Kind == RoiShapeKind.Circle ? shape.RadiusX : shape.RadiusY;
        double ang = shape.Kind == RoiShapeKind.Circle ? 0.0 : shape.Angle;
        double ca = Math.Cos(ang), sa = Math.Sin(ang);
        double ext = Math.Max(rx, ry);
        Box b = Bounds([new PointD(shape.CenterX - ext, shape.CenterY - ext), new PointD(shape.CenterX + ext, shape.CenterY + ext)], rows, columns);
        for (long r = b.R0; r <= b.R1 && !b.IsEmpty; r++)
        {
            for (long c = b.C0; c <= b.C1; c++)
            {
                double dx = c + 0.5 - shape.CenterX, dy = r + 0.5 - shape.CenterY;
                double u = ((dx * ca) + (dy * sa)) / rx, v = ((-dx * sa) + (dy * ca)) / ry;
                if ((u * u) + (v * v) <= 1.0)
                {
                    visit((r * columns) + c, 1.0);
                }
            }
        }
    }

    // Centre classification by scan lines through the pixel centres (even-odd rule, half-open edges).
    private static byte[] ScanCentres(IReadOnlyList<PointD> poly, Box b, int bw, int bh, int columns)
    {
        var inside = new byte[bw * bh];
        var xs = new List<double>();
        int n = poly.Count;
        double lim = columns + 1.0;
        for (long r = b.R0; r <= b.R1; r++)
        {
            double y = r + 0.5;
            xs.Clear();
            for (int i = 0, j = n - 1; i < n; j = i++)
            {
                PointD p = poly[j], q = poly[i];
                if ((p.Y > y) != (q.Y > y))
                {
                    xs.Add(p.X + ((y - p.Y) * (q.X - p.X) / (q.Y - p.Y)));
                }
            }
            xs.Sort();
            for (int k = 0; k + 1 < xs.Count; k += 2)
            {
                // Clamped in double first: a far-away vertex must not overflow the conversion.
                long ca = Math.Max(ClampToLong(Math.Ceiling(xs[k] - 0.5), -1.0, lim), b.C0);
                long cb = Math.Min(ClampToLong(Math.Ceiling(xs[k + 1] - 0.5), -1.0, lim) - 1, b.C1);
                for (long c = ca; c <= cb; c++)
                {
                    inside[((r - b.R0) * bw) + (c - b.C0)] = 1;
                }
            }
        }
        return inside;
    }

    // Pixels crossed by an edge (they get their exact covered area).
    private static byte[] MarkEdges(IReadOnlyList<PointD> poly, Box b, int bw, int bh, int rows, int columns)
    {
        var edge = new byte[bw * bh];
        int n = poly.Count;
        for (int i = 0, j = n - 1; i < n; j = i++)
        {
            PointD p = poly[j], q = poly[i];
            double ya = Math.Min(p.Y, q.Y), yb = Math.Max(p.Y, q.Y);
            long ra = Math.Max(b.R0, ClampToLong(Math.Floor(ya), -1.0, rows + 1.0));
            long rb = Math.Min(b.R1, ClampToLong(Math.Floor(yb), -1.0, rows + 1.0));
            for (long r = ra; r <= rb; r++)
            {
                double s0 = Math.Max(ya, r), s1 = Math.Min(yb, r + 1);
                double xa, xb;
                if (q.Y == p.Y)
                {
                    xa = Math.Min(p.X, q.X);
                    xb = Math.Max(p.X, q.X);
                }
                else
                {
                    double t0 = (s0 - p.Y) / (q.Y - p.Y), t1 = (s1 - p.Y) / (q.Y - p.Y);
                    xa = p.X + (t0 * (q.X - p.X));
                    xb = p.X + (t1 * (q.X - p.X));
                    if (xa > xb)
                    {
                        (xa, xb) = (xb, xa);
                    }
                }
                long ca = Math.Max(b.C0, ClampToLong(Math.Floor(xa), -1.0, columns + 1.0));
                long cb = Math.Min(b.C1, ClampToLong(Math.Floor(xb), -1.0, columns + 1.0));
                for (long c = ca; c <= cb; c++)
                {
                    edge[((r - b.R0) * bw) + (c - b.C0)] = 1;
                }
            }
        }
        _ = bh;
        return edge;
    }

    // Area of the polygon inside the box [x0, x1] x [y0, y1] (four Sutherland-Hodgman passes).
    private static double ClippedArea(IReadOnlyList<PointD> poly, double x0, double y0, double x1, double y1, List<PointD> a, List<PointD> b)
    {
        Clip(poly, a, p => p.X >= x0, (p, q) => new PointD(x0, p.Y + ((x0 - p.X) / (q.X - p.X) * (q.Y - p.Y))));
        Clip(a, b, p => p.X <= x1, (p, q) => new PointD(x1, p.Y + ((x1 - p.X) / (q.X - p.X) * (q.Y - p.Y))));
        Clip(b, a, p => p.Y >= y0, (p, q) => new PointD(p.X + ((y0 - p.Y) / (q.Y - p.Y) * (q.X - p.X)), y0));
        Clip(a, b, p => p.Y <= y1, (p, q) => new PointD(p.X + ((y1 - p.Y) / (q.Y - p.Y) * (q.X - p.X)), y1));
        return b.Count < 3 ? 0.0 : Math.Abs(RoiShape.Shoelace(b));
    }

    private static void Clip(IReadOnlyList<PointD> input, List<PointD> output, Func<PointD, bool> inside, Func<PointD, PointD, PointD> cut)
    {
        output.Clear();
        int n = input.Count;
        for (int i = 0; i < n; i++)
        {
            PointD cur = input[i], prev = input[(i + n - 1) % n];
            bool ci = inside(cur), pi = inside(prev);
            if (ci)
            {
                if (!pi)
                {
                    output.Add(cut(prev, cur));
                }
                output.Add(cur);
            }
            else if (pi)
            {
                output.Add(cut(prev, cur));
            }
        }
    }
}
