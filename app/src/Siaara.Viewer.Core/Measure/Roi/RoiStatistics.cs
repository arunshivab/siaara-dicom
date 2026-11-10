// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Weighted statistics of modality values inside an ROI. The values come from the toolkit as one
// float32 per pixel after the Modality LUT / rescale (siaara_frame_decode with
// SIAARA_FRAME_FLOAT32_MODALITY); everything else is the app's own code.

using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Measure.Roi;

/// <summary>Statistics of an ROI in modality units.</summary>
public sealed record RoiStatistics
{
    /// <summary>Pixels with a non-zero weight.</summary>
    public long Pixels { get; init; }

    /// <summary>Sum of the weights (the pixel count in centre mode).</summary>
    public double Count { get; init; }

    /// <summary>Sum of weight x value.</summary>
    public double Sum { get; init; }

    /// <summary>Weighted mean.</summary>
    public double Mean { get; init; }

    /// <summary>Weighted population standard deviation.</summary>
    public double StandardDeviation { get; init; }

    /// <summary>With the n - 1 denominator (frequency weights); 0 for a count of 1 or less.</summary>
    public double SampleStandardDeviation { get; init; }

    /// <summary>Minimum over pixels with a non-zero weight (0 when there are none).</summary>
    public double Min { get; init; }

    /// <summary>Maximum over pixels with a non-zero weight (0 when there are none).</summary>
    public double Max { get; init; }

    /// <summary>Shape area in pixel units (analytic).</summary>
    public double AreaPixels { get; init; }

    /// <summary>Area in mm² (area x row spacing x column spacing), or null without a spacing.</summary>
    public double? AreaMm2 { get; init; }

    /// <summary>Where the spacing for <see cref="AreaMm2"/> came from (None without one).</summary>
    public TkSpacingSource SpacingSource { get; init; }

    /// <summary>Computes the statistics. NaN values are skipped.</summary>
    /// <param name="values">rows x columns values, row by row.</param>
    /// <param name="rows">Rows.</param>
    /// <param name="columns">Columns.</param>
    /// <param name="shape">Shape in image pixel coordinates.</param>
    /// <param name="partial">Weight edge pixels by the covered area.</param>
    /// <param name="rowSpacing">mm between rows (0 when unknown).</param>
    /// <param name="columnSpacing">mm between columns (0 when unknown).</param>
    /// <param name="spacingSource">Source of the spacing (None: no area in mm²).</param>
    /// <returns>The statistics.</returns>
    /// <exception cref="RoiShapeException">Invalid shape.</exception>
    /// <exception cref="ArgumentException">The value count is not rows x columns.</exception>
    public static RoiStatistics Compute(ReadOnlySpan<float> values, int rows, int columns, RoiShape shape, bool partial, double rowSpacing = 0,
                                        double columnSpacing = 0, TkSpacingSource spacingSource = TkSpacingSource.None)
    {
        ArgumentNullException.ThrowIfNull(shape);
        if (rows < 0 || columns < 0 || values.Length != (long)rows * columns)
        {
            throw new ArgumentException("values size does not match rows x columns", nameof(values));
        }
        float[] v = values.ToArray();
        var weights = new List<(long Index, double Weight)>();
        PixelCoverage.ForEachWeight(rows, columns, shape, partial, (i, w) =>
        {
            if (!float.IsNaN(v[i]))
            {
                weights.Add((i, w));
            }
        });
        double area = shape.Area;
        bool hasSpacing = spacingSource != TkSpacingSource.None && rowSpacing > 0 && columnSpacing > 0;
        var empty = new RoiStatistics
        {
            AreaPixels = area,
            AreaMm2 = hasSpacing ? area * rowSpacing * columnSpacing : null,
            SpacingSource = hasSpacing ? spacingSource : TkSpacingSource.None,
        };
        if (weights.Count == 0)
        {
            return empty;
        }
        double sw = 0, sum = 0, min = double.PositiveInfinity, max = double.NegativeInfinity;
        foreach ((long i, double w) in weights)
        {
            double x = v[i];
            sw += w;
            sum += w * x;
            min = Math.Min(min, x);
            max = Math.Max(max, x);
        }
        double mean = sum / sw;
        double ss = 0;
        foreach ((long i, double w) in weights)
        {
            double d = v[i] - mean;
            ss += w * d * d;
        }
        return empty with
        {
            Pixels = weights.Count,
            Count = sw,
            Sum = sum,
            Mean = mean,
            StandardDeviation = Math.Sqrt(ss / sw),
            SampleStandardDeviation = sw > 1.0 ? Math.Sqrt(ss / (sw - 1.0)) : 0.0,
            Min = min,
            Max = max,
        };
    }
}
