// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Measure;

/// <summary>A point in double precision.</summary>
/// <param name="X">X.</param>
/// <param name="Y">Y.</param>
public readonly record struct PointD(double X, double Y);

/// <summary>
/// Pure geometry between viewport pixels and image pixel coordinates ((0,0) = top-left corner of
/// the first pixel), following the toolkit's 2D viewport model: flips, then clockwise rotation,
/// then zoom about the image centre, which sits at the viewport centre plus the pan. Zoom 0 fits
/// the rotated image into the viewport. The tests check it against siaara_frame_map_point.
/// </summary>
public sealed class ScreenImageTransform
{
    private readonly double _cx, _cy, _vx, _vy;
    private readonly double _a, _b, _c, _d;
    private readonly double _ia, _ib, _ic, _id;

    /// <summary>Builds the transform.</summary>
    /// <param name="imageWidth">Image columns.</param>
    /// <param name="imageHeight">Image rows.</param>
    /// <param name="viewWidth">Viewport width.</param>
    /// <param name="viewHeight">Viewport height.</param>
    /// <param name="zoom">Output pixels per image pixel; 0 = fit.</param>
    /// <param name="panX">Pan x (viewport pixels).</param>
    /// <param name="panY">Pan y.</param>
    /// <param name="rotation">Clockwise degrees.</param>
    /// <param name="flipHorizontal">Mirror left-right.</param>
    /// <param name="flipVertical">Mirror top-bottom.</param>
    public ScreenImageTransform(int imageWidth, int imageHeight, int viewWidth, int viewHeight, double zoom, double panX, double panY,
                                double rotation, bool flipHorizontal, bool flipVertical)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(imageWidth);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(imageHeight);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(viewWidth);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(viewHeight);
        (double cos, double sin) = CosSin(rotation);
        double z = zoom;
        if (z == 0)
        {
            double bw = (imageWidth * Math.Abs(cos)) + (imageHeight * Math.Abs(sin));
            double bh = (imageWidth * Math.Abs(sin)) + (imageHeight * Math.Abs(cos));
            z = Math.Min(viewWidth / bw, viewHeight / bh);
        }
        Zoom = z;
        _cx = imageWidth / 2.0;
        _cy = imageHeight / 2.0;
        _vx = (viewWidth / 2.0) + panX;
        _vy = (viewHeight / 2.0) + panY;
        double fx = flipHorizontal ? -1 : 1, fy = flipVertical ? -1 : 1;
        _a = z * cos * fx;
        _b = -z * sin * fy;
        _c = z * sin * fx;
        _d = z * cos * fy;
        double det = (_a * _d) - (_b * _c);
        _ia = _d / det;
        _ib = -_b / det;
        _ic = -_c / det;
        _id = _a / det;
    }

    /// <summary>The effective zoom (after fitting).</summary>
    public double Zoom { get; }

    /// <summary>Image point to viewport.</summary>
    /// <param name="p">Image point.</param>
    /// <returns>Viewport point.</returns>
    public PointD ToScreen(PointD p)
    {
        double ox = p.X - _cx, oy = p.Y - _cy;
        return new PointD(_vx + (_a * ox) + (_b * oy), _vy + (_c * ox) + (_d * oy));
    }

    /// <summary>Viewport point to image.</summary>
    /// <param name="p">Viewport point.</param>
    /// <returns>Image point.</returns>
    public PointD ToImage(PointD p)
    {
        double ox = p.X - _vx, oy = p.Y - _vy;
        return new PointD(_cx + (_ia * ox) + (_ib * oy), _cy + (_ic * ox) + (_id * oy));
    }

    /// <summary>Exact cosine / sine for multiples of 90 degrees (as the toolkit does).</summary>
    /// <param name="degrees">Angle.</param>
    /// <returns>(cos, sin).</returns>
    public static (double Cos, double Sin) CosSin(double degrees)
    {
        double r = degrees % 360.0;
        if (r < 0)
        {
            r += 360.0;
        }
        return r switch
        {
            0.0 => (1, 0),
            90.0 => (0, 1),
            180.0 => (-1, 0),
            270.0 => (0, -1),
            _ => (Math.Cos(r * Math.PI / 180.0), Math.Sin(r * Math.PI / 180.0)),
        };
    }
}
