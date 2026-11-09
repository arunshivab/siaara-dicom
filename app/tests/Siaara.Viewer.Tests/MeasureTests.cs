// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Tests;

internal static class MeasureTests
{
    private static void Drag(ViewerSession s, int x0, int y0, int x1, int y1, int buttons = 1)
    {
        s.OnInput(new InputEvent(0, 0, InputKind.Down, x0, y0, buttons, 0, 0, InputModifiers.None, 0, 0));
        s.OnInput(new InputEvent(0, 0, InputKind.Move, (x0 + x1) / 2, (y0 + y1) / 2, buttons, 0, 0, InputModifiers.None, 0, 0));
        s.OnInput(new InputEvent(0, 0, InputKind.Move, x1, y1, buttons, 0, 0, InputModifiers.None, 0, 0));
        s.OnInput(new InputEvent(0, 0, InputKind.Up, x1, y1, 0, 0, 0, InputModifiers.None, 0, 0));
    }

    private static void Click(ViewerSession s, int x, int y)
    {
        s.OnInput(new InputEvent(0, 0, InputKind.Move, x, y, 0, 0, 0, InputModifiers.None, 0, 0));
        s.OnInput(new InputEvent(0, 0, InputKind.Down, x, y, 1, 0, 0, InputModifiers.None, 0, 0));
        s.OnInput(new InputEvent(0, 0, InputKind.Up, x, y, 0, 0, 0, InputModifiers.None, 0, 0));
    }

    private static async Task<IReadOnlyList<Annotation>> WaitAnnotationsAsync(ViewerSession s, int count)
    {
        FrameKey key = s.GetViewport(0).CurrentImage!.Key;
        for (int i = 0; i < 300; i++)
        {
            IReadOnlyList<Annotation> a = s.GetAnnotations(key);
            if (a.Count == count && a.All(x => x.Label.Length > 0 || x.Warnings.Count > 0))
            {
                return a;
            }
            await Task.Delay(10).ConfigureAwait(false);
        }
        return s.GetAnnotations(key);
    }

    public static void Register()
    {
        T.Case("measure: length in mm on known spacing; angle; undo / redo; saved per SOP + frame", async () =>
        {
            using var env = new TestEnv("length");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("LEN^CT", "LEN1"), 1, 64, spacingMm: 0.7);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "LEN1"), 64, 64).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                s.SetTool(ViewerTool.Length);
                Drag(s, 10, 10, 20, 10);
                IReadOnlyList<Annotation> a = await WaitAnnotationsAsync(s, 1).ConfigureAwait(false);
                T.Require(a.Count == 1);
                T.Eq(a[0].Label, "7 mm");
                T.Eq(a[0].Spacing, "pixel");
                s.SetTool(ViewerTool.Angle);
                Drag(s, 10, 30, 30, 30);
                Click(s, 30, 10);
                a = await WaitAnnotationsAsync(s, 2).ConfigureAwait(false);
                T.Require(a.Count == 2);
                T.Eq(a[1].Label, "90°");
                SeriesImage img = s.GetViewport(0).CurrentImage!;
                for (int i = 0; i < 100 && env.Annotations.Load(img.Instance.SopInstanceUid, img.Frame).Annotations.Count != 2; i++)
                {
                    await Task.Delay(20).ConfigureAwait(false); // the atomic save runs on the store pool
                }
                T.Eq(env.Annotations.Load(img.Instance.SopInstanceUid, img.Frame).Annotations.Count, 2);
                s.Undo();
                a = await WaitAnnotationsAsync(s, 1).ConfigureAwait(false);
                T.Eq(a.Count, 1);
                s.Redo();
                a = await WaitAnnotationsAsync(s, 2).ConfigureAwait(false);
                T.Eq(a.Count, 2);
                ViewportStateMessage st = sink.LastState(0)!;
                T.Check(st.Annotations.Count >= 1);
            }
        });

        T.Case("measure: lengths in px without pixel spacing; manual calibration", async () =>
        {
            using var env = new TestEnv("px");
            string dir = env.Dir("in");
            SampleData.WriteCrNoSpacing(Path.Combine(dir, "cr.dcm"), SampleData.Patient("PX^CR", "PX1"), 64);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "PX1"), 64, 64).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                s.SetTool(ViewerTool.Length);
                Drag(s, 10, 10, 20, 10);
                IReadOnlyList<Annotation> a = await WaitAnnotationsAsync(s, 1).ConfigureAwait(false);
                T.Require(a.Count == 1);
                T.Eq(a[0].Label, "10 px");
                T.Eq(a[0].Spacing, "none");
                s.SetTool(ViewerTool.Calibrate);
                Drag(s, 10, 40, 30, 40);
                a = await WaitAnnotationsAsync(s, 2).ConfigureAwait(false);
                Annotation cal = a.Single(x => x.Kind == AnnotationKind.Calibration);
                await Task.Delay(50).ConfigureAwait(false);
                T.Eq(sink.LastState(0)?.CalibrationPrompt, cal.Id);
                await s.CalibrateAsync(cal.Id, 50).ConfigureAwait(false);
                a = s.GetAnnotations(s.GetViewport(0).CurrentImage!.Key);
                T.Eq(a.Single(x => x.Kind == AnnotationKind.Length).Label, "25 mm");
                T.Eq(a.Single(x => x.Kind == AnnotationKind.Calibration).Label, "50 mm (manually calibrated)");
                T.Eq(a.Single(x => x.Kind == AnnotationKind.Length).Spacing, "manual");
            }
        });

        T.Case("measure: ROI statistics equal the direct toolkit call", async () =>
        {
            using var env = new TestEnv("roi");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("ROI^CT", "ROI1"), 1, 64, spacingMm: 0.5);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "ROI1"), 64, 64).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                s.SetTool(ViewerTool.Ellipse);
                Drag(s, 16, 16, 48, 40);
                IReadOnlyList<Annotation> a = await WaitAnnotationsAsync(s, 1).ConfigureAwait(false);
                T.Require(a.Count == 1);
                using TkFile f = TkFile.Open(s.GetViewport(0).CurrentImage!.Instance.FilePath, headersOnly: false);
                TkRoiStats direct = f.ComputeRoi(0, TkRoiShape.Ellipse, [32, 28, 16, 12, 0], partialWeighting: true);
                T.Check(a[0].Label.StartsWith("Mean " + DisplayFormat.Number(direct.Mean, 1) + " HU", StringComparison.Ordinal));
                T.Check(a[0].Label.Contains("SD " + DisplayFormat.Number(direct.StandardDeviation, 1), StringComparison.Ordinal));
                T.Check(a[0].Label.Contains("Area " + DisplayFormat.Number(direct.AreaMm2!.Value, 1) + " mm²", StringComparison.Ordinal));
                T.Note(a[0].Label);
                s.SetTool(ViewerTool.Probe);
                Drag(s, 33, 20, 33, 20);
                a = await WaitAnnotationsAsync(s, 2).ConfigureAwait(false);
                TkRoiStats px = f.ComputeRoi(0, TkRoiShape.Rectangle, [33, 20, 34, 21], partialWeighting: false);
                T.Check(a[1].Label.EndsWith(DisplayFormat.Number(px.Mean, 1) + " HU", StringComparison.Ordinal));
                T.Note(a[1].Label);
            }
        });

        T.Case("measure: PET SUVbw with and without patient weight", async () =>
        {
            using var env = new TestEnv("suv");
            string dir = env.Dir("in");
            SampleData.WritePet(Path.Combine(dir, "w.dcm"), SampleData.Patient("SUV^WEIGHT", "SUV1"), 70);
            SampleData.WritePet(Path.Combine(dir, "nw.dcm"), SampleData.Patient("SUV^NOWEIGHT", "SUV2"), null);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "SUV1"), 32, 32).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                s.SetTool(ViewerTool.Rectangle);
                Drag(s, 10, 10, 20, 20);
                IReadOnlyList<Annotation> a = await WaitAnnotationsAsync(s, 1).ConfigureAwait(false);
                T.Require(a.Count == 1);
                using TkFile f = TkFile.Open(s.GetViewport(0).CurrentImage!.Instance.FilePath, headersOnly: false);
                TkSuv suv = f.ComputeSuv(0);
                T.Require(suv.BodyWeightFactor is not null);
                TkRoiStats st = f.ComputeRoi(0, TkRoiShape.Rectangle, [10, 10, 20, 20], true);
                T.Near(st.Mean, 10000, 1e-6);
                T.Check(a[0].Label.Contains("Bq/ml", StringComparison.Ordinal));
                T.Check(a[0].Label.Contains("SUVbw mean " + DisplayFormat.Number(st.Mean * suv.BodyWeightFactor!.Value, 2), StringComparison.Ordinal));
                T.Note(a[0].Label + (a[0].Warnings.Count > 0 ? "  warnings: " + string.Join("; ", a[0].Warnings) : ""));
            }
            (ViewerSession s2, FakeSink sink2) = await env.OpenAsync(Gen.StudyOf(env, "SUV2"), 32, 32).ConfigureAwait(false);
            using (s2)
            {
                await ImagingTests.FirstFrameAsync(sink2).ConfigureAwait(false);
                s2.SetTool(ViewerTool.Probe);
                Drag(s2, 16, 16, 16, 16);
                IReadOnlyList<Annotation> a = await WaitAnnotationsAsync(s2, 1).ConfigureAwait(false);
                T.Require(a.Count == 1);
                T.Check(!a[0].Label.Contains("SUV", StringComparison.Ordinal));
                T.Check(a[0].Warnings.Contains("patient weight missing"));
                T.Check(a[0].Warnings.Contains("SUV cannot be computed"));
            }
        });

        T.Case("transform: screen <-> image round-trip for every rotation, flip and zoom, equal to the toolkit's mapping", () =>
        {
            string dir = T.NewTempDir("transform");
            var files = SampleData.WriteCtSeries(dir, SampleData.Patient("TR^CT", "TR1"), 1, 48);
            using TkFile f = TkFile.Open(files[0], headersOnly: false);
            using TkFrame fr = TkFrame.Open(f, 0);
            int checks = 0;
            double maxErr = 0;
            foreach (double rot in new[] { 0.0, 90, 180, 270, 33, -45 })
            {
                foreach ((bool fh, bool fv) in new[] { (false, false), (true, false), (false, true), (true, true) })
                {
                    foreach (double zoom in new[] { 0.0, 1, 2.5 })
                    {
                        foreach ((double px, double py) in new[] { (0.0, 0.0), (13.0, -7.5) })
                        {
                            var t = new ScreenImageTransform(48, 48, 200, 150, zoom, px, py, rot, fh, fv);
                            var spec = new RenderSpec { Width = 200, Height = 150, Zoom = zoom, PanX = px, PanY = py, Rotation = rot, FlipHorizontal = fh, FlipVertical = fv };
                            foreach (PointD p in new PointD[] { new(0, 0), new(47.5, 3.25), new(24, 24), new(10, 40) })
                            {
                                PointD s = t.ToScreen(p);
                                (double tx, double ty) = fr.ImageToView(spec, p.X, p.Y);
                                maxErr = Math.Max(maxErr, Math.Max(Math.Abs(s.X - tx), Math.Abs(s.Y - ty)));
                                PointD back = t.ToImage(s);
                                (double bx, double by) = fr.ViewToImage(spec, s.X, s.Y);
                                maxErr = Math.Max(maxErr, Math.Max(Math.Abs(back.X - p.X), Math.Abs(back.Y - p.Y)));
                                maxErr = Math.Max(maxErr, Math.Max(Math.Abs(bx - p.X), Math.Abs(by - p.Y)));
                                checks++;
                            }
                        }
                    }
                }
            }
            T.Note($"{checks} points, max difference {maxErr:E2}");
            T.Check(maxErr < 1e-6);
        });
    }
}
