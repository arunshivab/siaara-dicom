// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// M1.2: the app's own SUV and ROI code (Measure/Suv, Measure/Roi) against the golden file written
// with the toolkit's former siaara_suv_factor / siaara_roi_stats_compute on the same synthetic
// cases: factors within 1e-12 (relative), ROI values within 1e-9, identical warnings, sources,
// pixel counts and errors.

using System.Text.Json;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Measure.Roi;
using Siaara.Viewer.Core.Measure.Suv;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Tests;

internal static class SuvRoiGoldenTests
{
    private static JsonElement Golden()
    {
        string path = Path.Combine(AppContext.BaseDirectory, "Golden", "suv-roi-golden.json");
        using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
        return doc.RootElement.Clone();
    }

    private static double? Num(JsonElement e, string name) => e.GetProperty(name).ValueKind == JsonValueKind.Null ? null : e.GetProperty(name).GetDouble();

    private static double s_maxRel;

    private static bool Close(double? expected, double? got, double rel, out string text)
    {
        if (expected is double ex && got is double gx && ex != gx)
        {
            s_maxRel = Math.Max(s_maxRel, Math.Abs(ex - gx) / Math.Max(Math.Abs(ex), Math.Abs(gx)));
        }
        text = $"{expected?.ToString("R", System.Globalization.CultureInfo.InvariantCulture) ?? "null"} vs {got?.ToString("R", System.Globalization.CultureInfo.InvariantCulture) ?? "null"}";
        if (expected is null || got is null)
        {
            return expected is null && got is null;
        }
        double e = expected.Value, g = got.Value;
        return e == g || Math.Abs(e - g) <= rel * Math.Max(Math.Abs(e), Math.Abs(g));
    }

    // The app's ROI path on a file: frame check, shape, float32 modality values, frame spacing.
    public static RoiStatistics ComputeRoi(TkFile f, int frame, int shape, double[] parameters, bool partial)
    {
        TkPixelInfo pi = f.PixelInfo ?? throw new TkException(TkError.Unsupported);
        if (pi.SamplesPerPixel != 1)
        {
            throw new TkException(TkError.Unsupported);
        }
        if (frame < 0 || frame >= Math.Max(1, pi.Frames))
        {
            throw new ArgumentOutOfRangeException(nameof(frame));
        }
        RoiShape s = RoiShape.FromParameters((RoiShapeKind)shape, parameters);
        s.Validate();
        float[] values = f.DecodeModalityFloat32(frame);
        TkPlane g = f.GetGeometry(frame);
        return RoiStatistics.Compute(values, pi.Rows, pi.Columns, s, partial, g.RowSpacing, g.ColumnSpacing, g.Source);
    }

    public static void Register()
    {
        T.Case("golden: SUV factors, sources and warnings equal the toolkit's on 64 synthetic PET data sets", () =>
        {
            JsonElement golden = Golden().GetProperty("suv");
            IReadOnlyList<SuvCase> cases = SuvRoiCases.Suv();
            T.Eq(golden.GetArrayLength(), cases.Count);
            int compared = 0;
            s_maxRel = 0;
            foreach ((SuvCase c, JsonElement g) in cases.Zip(golden.EnumerateArray()))
            {
                T.Require(g.GetProperty("name").GetString() == c.Name);
                using TkFile f = TkFile.Create();
                c.Build(f);
                SuvResult r = SuvCalculator.Compute(f.Dataset, c.Frame);
                string where = c.Name + ": ";
                if ((uint)r.Warnings != g.GetProperty("warnings").GetUInt32())
                {
                    T.Check(false, where + $"warnings 0x{(uint)r.Warnings:x} vs 0x{g.GetProperty("warnings").GetUInt32():x}");
                }
                foreach ((string name, double? mine) in new (string, double?)[]
                {
                    ("bw", r.BodyWeight), ("lbmJames", r.LeanBodyMassJames), ("lbmJanmahasatian", r.LeanBodyMassJanmahasatian), ("bsa", r.BodySurfaceArea),
                    ("ibw", r.IdealBodyWeight), ("decaySeconds", r.DecaySeconds ?? 0), ("decayedDoseBq", r.DecayedDoseBq ?? 0),
                    ("weightKg", r.WeightKg > 0 ? r.WeightKg : null), ("injectedDoseBq", r.InjectedDoseBq > 0 ? r.InjectedDoseBq : null),
                    ("halfLifeSeconds", r.HalfLifeSeconds > 0 ? r.HalfLifeSeconds : null),
                })
                {
                    if (!Close(Num(g, name), mine, 1e-12, out string text))
                    {
                        T.Check(false, where + name + " " + text);
                    }
                    compared++;
                }
                T.Check((int)r.ScanTimeSource == g.GetProperty("scanTimeSource").GetInt32(), where + "scan time source " + r.ScanTimeSource);
                T.Check((int)r.InjectionTimeSource == g.GetProperty("injectionTimeSource").GetInt32(), where + "injection time source " + r.InjectionTimeSource);
                T.Check((int)r.DecayCorrection == g.GetProperty("decayCorrection").GetInt32(), where + "decay correction " + r.DecayCorrection);
            }
            T.Note(compared + " SUV values compared; largest relative difference " + s_maxRel.ToString("G3", System.Globalization.CultureInfo.InvariantCulture));
        });

        T.Case("golden: ROI statistics equal the toolkit's on 235 cases (shapes, edge pixels, rotated ellipses, spacing sources, errors)", () =>
        {
            JsonElement golden = Golden().GetProperty("roi");
            IReadOnlyList<RoiCase> cases = SuvRoiCases.Roi();
            var images = SuvRoiCases.RoiImages().ToDictionary(i => i.Name);
            T.Eq(golden.GetArrayLength(), cases.Count);
            int errors = 0, compared = 0;
            s_maxRel = 0;
            foreach ((RoiCase c, JsonElement g) in cases.Zip(golden.EnumerateArray()))
            {
                T.Require(g.GetProperty("name").GetString() == c.Name && g.GetProperty("image").GetString() == c.Image);
                using TkFile f = TkFile.Create();
                images[c.Image].Build(f);
                string where = c.Image[..2] + c.Name + ": ";
                RoiStatistics? r = null;
                try
                {
                    r = ComputeRoi(f, c.Frame, c.Shape, c.Parameters, c.Partial);
                }
                catch (Exception e) when (e is ArgumentException or TkException)
                {
                    errors++;
                }
                bool expectError = g.GetProperty("error").GetBoolean();
                T.Check((r is null) == expectError, where + (expectError ? "expected an error" : "unexpected error"));
                if (r is null || expectError)
                {
                    continue;
                }
                T.Check(r.Pixels == g.GetProperty("pixels").GetInt64(), where + $"pixels {r.Pixels} vs {g.GetProperty("pixels").GetInt64()}");
                foreach ((string name, double? mine) in new (string, double?)[]
                {
                    ("count", r.Count), ("sum", r.Sum), ("mean", r.Mean), ("sd", r.StandardDeviation), ("sdSample", r.SampleStandardDeviation),
                    ("min", r.Min), ("max", r.Max), ("areaPx", r.AreaPixels), ("areaMm2", r.AreaMm2),
                })
                {
                    // Sums of many weights: an absolute floor for values that are zero up to rounding.
                    double? want = Num(g, name);
                    bool ok = Close(want, mine, 1e-9, out string text)
                              || (want is double w0 && mine is double m0 && Math.Abs(w0 - m0) <= 1e-9 * Math.Max(1.0, Math.Abs(r.Mean)));
                    if (!ok)
                    {
                        T.Check(false, where + name + " " + text);
                    }
                    compared++;
                }
                T.Check((int)r.SpacingSource == g.GetProperty("spacingSource").GetInt32(), where + "spacing source " + r.SpacingSource);
            }
            T.Note($"{compared} ROI values compared, {errors} expected errors reproduced; largest relative difference "
                   + s_maxRel.ToString("G3", System.Globalization.CultureInfo.InvariantCulture));
        });

        T.Case("suv: caller overrides, sources of the inputs, vendor factor off, midnight rollover option", () =>
        {
            SuvCase baseCase = SuvRoiCases.Suv()[0];
            using TkFile f = TkFile.Create();
            baseCase.Build(f);
            SuvResult plain = SuvCalculator.Compute(f.Dataset, 0);
            T.Require(plain.BodyWeight is not null);
            T.Eq(plain.Inputs!.Sources["weight"], "Patient's Weight (0010,1030)");
            T.Check(plain.Inputs.Sources["dose"].EndsWith("Radionuclide Total Dose (0018,1074)", StringComparison.Ordinal));
            T.Check(plain.WarningTexts.SequenceEqual(["injection date assumed"]));
            // A confirmed weight replaces the stored one: the factor scales with it.
            SuvResult heavier = SuvCalculator.Compute(f.Dataset, 0, new SuvOptions { WeightKg = 140 });
            T.Near(heavier.BodyWeight!.Value, 2 * plain.BodyWeight!.Value, 1e-15);
            // No decay: injection at the scan reference time.
            SuvResult noDecay = SuvCalculator.Compute(f.Dataset, 0, new SuvOptions { InjectionTime = 1772360100 });
            T.Eq(noDecay.InjectionTimeSource, SuvTimeSource.Caller);
            T.Eq(noDecay.DecaySeconds, 0.0);
            T.Near(noDecay.BodyWeight!.Value, 70 * 1000 / 370000000.0, 1e-18);
            // Time-only injection after the scan: inconsistent, unless the rollover option is on.
            SuvCase late = SuvRoiCases.Suv().First(c => c.Name == "injection after the scan (date assumed)");
            using TkFile g = TkFile.Create();
            late.Build(g);
            T.Check(SuvCalculator.Compute(g.Dataset, 0).BodyWeight is null);
            SuvResult rolled = SuvCalculator.Compute(g.Dataset, 0, new SuvOptions { AllowMidnightRollover = true });
            T.Check((rolled.Warnings & SuvWarnings.MidnightRollover) != 0);
            T.Near(rolled.DecaySeconds!.Value, 22.25 * 3600, 1e-6);
            // CNTS: the Philips factor only with the vendor option; a caller scale always.
            SuvCase cnts = SuvRoiCases.Suv().First(c => c.Name == "CNTS with Philips factor, block 10");
            using TkFile h = TkFile.Create();
            cnts.Build(h);
            T.Check(SuvCalculator.Compute(h.Dataset, 0).BodyWeight is not null);
            T.Check(SuvCalculator.Compute(h.Dataset, 0, new SuvOptions { UseVendorPrivate = false }).BodyWeight is null);
            SuvResult scaled = SuvCalculator.Compute(h.Dataset, 0, new SuvOptions { UseVendorPrivate = false, CountsScale = 2.5 });
            T.Near(scaled.BodyWeight!.Value, SuvCalculator.Compute(h.Dataset, 0).BodyWeight!.Value, 1e-18);
            T.Check(SuvCalculator.Compute(h.Dataset, 0).Inputs!.Sources["philipsScaleFactor"].Contains("(7053,1009)", StringComparison.Ordinal));
            // Date and time parsing edge cases.
            T.Eq(DecayTime.ParseTime("235960"), 86400.0);
            T.Eq(DecayTime.ParseTime("24"), null);
            T.Eq(DecayTime.ParseTime("1230.1234567"), null);
            T.Eq(DecayTime.ParseDate("20240229"), 19782L);
            T.Eq(DecayTime.ParseDate("20230229"), null);
            T.Eq(DecayTime.ParseDateTime("2026")?.Seconds, DecayTime.DaysFromCivil(2026, 1, 1) * 86400.0);
            T.Eq(DecayTime.ParseDateTime("20260301+1500"), null);
            T.Eq(DecayTime.ParseDateTime("20260301-0130")?.UtcOffsetSeconds, -5400);
        });

        T.Case("roi: shapes, area and coverage invariants of the app's implementation", () =>
        {
            // Partial weights of a shape fully inside the image sum to its area (exact for polygons).
            var values = new float[20 * 20];
            Array.Fill(values, 1f);
            foreach (RoiShape s in new[]
            {
                RoiShape.Rectangle(2.25, 3.5, 11.75, 9.125), RoiShape.Polygon([new PointD(3.3, 2.2), new PointD(15.1, 4.7), new PointD(9.9, 16.4)]),
                RoiShape.Ellipse(10, 10, 6.5, 3.25, 0.7), RoiShape.Circle(9.5, 10.25, 4.4),
            })
            {
                RoiStatistics st = RoiStatistics.Compute(values, 20, 20, s, partial: true);
                T.Near(st.Count, s.Area, s.Kind is RoiShapeKind.Ellipse or RoiShapeKind.Circle ? 1e-9 * s.Area : 1e-12 * s.Area);
                T.Near(st.Mean, 1, 1e-12);
                T.Near(st.StandardDeviation, 0, 1e-6);
            }
            // NaN values are skipped; spacing gives mm2 only with a source.
            values[(5 * 20) + 5] = float.NaN;
            RoiStatistics nan = RoiStatistics.Compute(values, 20, 20, RoiShape.Rectangle(4, 4, 7, 7), partial: false, 0.5, 0.25, TkSpacingSource.PixelSpacing);
            T.Eq(nan.Pixels, 8L);
            T.Near(nan.AreaMm2!.Value, 9 * 0.125, 1e-15);
            T.Check(RoiStatistics.Compute(values, 20, 20, RoiShape.Rectangle(4, 4, 7, 7), false, 0.5, 0.25, TkSpacingSource.None).AreaMm2 is null);
            bool threw = false;
            try
            {
                RoiStatistics.Compute(values, 19, 20, RoiShape.Rectangle(4, 4, 7, 7), false);
            }
            catch (ArgumentException)
            {
                threw = true;
            }
            T.Check(threw);
        });
    }
}
