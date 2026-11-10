// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// M1.1 features: intended-use label, magnifier, colour maps, reference lines, cross-reference,
// linked scrolling, sync groups, arrow and text annotations, export audit.

using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Geometry;
using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Tests;

internal static class ViewerFeatureTests
{
    private const int Slices = 20, Size = 64;
    private const double Spacing = 1.0, Gap = 2.0;

    private static void Input(ViewerSession s, int vp, InputKind kind, int x, int y, int buttons, int key = 0, int wheel = 0) =>
        s.OnInput(new InputEvent(vp, 0, kind, x, y, buttons, wheel, key, InputModifiers.None, 0, 0));

    private static void Resize(ViewerSession s, int vp, int w, int h) =>
        s.OnInput(new InputEvent(vp, 0, InputKind.Resize, 0, 0, 0, 0, 0, InputModifiers.None, w, h));

    // Axial (series 1), sagittal (2) and coronal (3) of one phantom in one frame of reference.
    private static async Task<string> WriteMprAsync(TestEnv env, string id)
    {
        string dir = env.Dir("mpr-" + id);
        SamplePatient p = SampleData.Patient("MPR^" + id, id);
        string forUid = SampleData.NewUid();
        SampleData.WriteCtSeries(Path.Combine(dir, "ax"), p, Slices, Size, Spacing, Gap, 1, frameOfReferenceUid: forUid);
        SampleData.WriteReformatSeries(Path.Combine(dir, "sag"), p, sagittal: true, 8, Slices, Size, Spacing, Gap, 2, forUid);
        SampleData.WriteReformatSeries(Path.Combine(dir, "cor"), p, sagittal: false, 8, Slices, Size, Spacing, Gap, 3, forUid);
        await env.ImportAsync(dir).ConfigureAwait(false);
        return Gen.StudyOf(env, id);
    }

    private static async Task<T2?> WaitForAsync<T2>(Func<T2?> probe, int timeoutMs = 5000) where T2 : class
    {
        for (int i = 0; i < timeoutMs / 10; i++)
        {
            if (probe() is T2 v)
            {
                return v;
            }
            await Task.Delay(10).ConfigureAwait(false);
        }
        return probe();
    }

    private static async Task<bool> WaitUntilAsync(Func<bool> probe, int timeoutMs = 5000)
    {
        for (int i = 0; i < timeoutMs / 10; i++)
        {
            if (probe())
            {
                return true;
            }
            await Task.Delay(10).ConfigureAwait(false);
        }
        return probe();
    }

    public static void Register()
    {
        T.Case("geometry: reference line of an axial slice on a sagittal image; nearest plane; clipping", () =>
        {
            const string forUid = "1.2.3";
            // Axial at z = 10: rows along y, columns along x, 64 x 64 at 1 mm.
            var axial = new ImagePlane(forUid, new Vec3(0, 0, 10), new Vec3(1, 0, 0), new Vec3(0, 1, 0), 1, 1, 64, 64);
            // Sagittal at x = 20: columns along y, rows downwards in z from z = 38 (20 rows at 2 mm).
            var sag = new ImagePlane(forUid, new Vec3(20, 0, 38), new Vec3(0, 1, 0), new Vec3(0, 0, -1), 1, 2, 64, 20);
            (PointD A, PointD B)? line = PlaneMath.ReferenceLine(axial, sag);
            T.Require(line is not null);
            (PointD a, PointD b) = line!.Value;
            // z = 10 on the sagittal image: row centre (38 - 10) / 2 = 14, so y = 14.5 in corner coordinates.
            T.Near(a.Y, 14.5, 1e-9);
            T.Near(b.Y, 14.5, 1e-9);
            T.Near(Math.Min(a.X, b.X), 0, 1e-9);
            T.Near(Math.Max(a.X, b.X), 64, 1e-9);
            // The sagittal plane on the axial image: the column x = 20 (centre), corner coordinate 20.5.
            (PointD c, PointD d) = PlaneMath.ReferenceLine(sag, axial)!.Value;
            T.Near(c.X, 20.5, 1e-9);
            T.Near(d.X, 20.5, 1e-9);
            // Parallel planes and other frames of reference have none.
            var axial2 = new ImagePlane(forUid, new Vec3(0, 0, 12), new Vec3(1, 0, 0), new Vec3(0, 1, 0), 1, 1, 64, 64);
            T.Check(PlaneMath.ReferenceLine(axial, axial2) is null);
            var other = new ImagePlane("9.9", new Vec3(20, 0, 38), new Vec3(0, 1, 0), new Vec3(0, 0, -1), 1, 2, 64, 20);
            T.Check(PlaneMath.ReferenceLine(axial, other) is null);
            // Nearest plane and the patient <-> image round trip.
            ImagePlane?[] stack = [.. Enumerable.Range(0, 10).Select(k => (ImagePlane?)new ImagePlane(forUid, new Vec3(0, 0, k * 2.0), new Vec3(1, 0, 0), new Vec3(0, 1, 0), 1, 1, 64, 64))];
            T.Eq(PlaneMath.NearestPlane(stack, forUid, new Vec3(5, 5, 7.2)), 4);
            T.Eq(PlaneMath.NearestPlane(stack, forUid, new Vec3(5, 5, 70), 10), -1);
            T.Eq(PlaneMath.NearestPlane(stack, "other", new Vec3(5, 5, 7.2)), -1);
            Vec3 q = sag.ToPatient(new PointD(10.25, 3.75));
            (PointD back, double dist) = sag.ToImage(q);
            T.Near(back.X, 10.25, 1e-9);
            T.Near(back.Y, 3.75, 1e-9);
            T.Near(dist, 0, 1e-9);
            T.Check(PlaneMath.ClipToRect(new PointD(-10, -10), new PointD(-1, -5), 64, 64) is null);
        });

        T.Case("viewer: reference lines, cross-reference and linked scrolling by frame of reference", async () =>
        {
            using var env = new TestEnv("mpr");
            string study = await WriteMprAsync(env, "MPR1").ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(study, 128, 128).ConfigureAwait(false);
            using (s)
            {
                T.Require(await WaitUntilAsync(() => s.Loaded && s.Series.Count == 3).ConfigureAwait(false));
                Resize(s, 1, 128, 128);
                Resize(s, 2, 128, 128);
                await sink.SettleAsync().ConfigureAwait(false);
                T.Eq(s.GetViewport(0).Series!.SeriesNumber, 1);
                T.Eq(s.GetViewport(1).Series!.SeriesNumber, 2);
                // Viewport 0 (axial) is active: the sagittal viewport shows a horizontal scout line.
                ViewportStateMessage? st = await WaitForAsync(() => sink.LastState(1) is { ReferenceLines.Count: > 0 } m ? m : null).ConfigureAwait(false);
                T.Require(st is not null);
                ReferenceLineView line = st!.ReferenceLines[0];
                T.Eq(line.Kind, "scout");
                T.Near(line.Points[1], line.Points[3], 0.11);
                double y0 = line.Points[1];
                // Scrolling the axial stack moves the line on the sagittal image.
                sink.Drain();
                for (int i = 0; i < 5; i++)
                {
                    Input(s, 0, InputKind.Wheel, 64, 64, 0, wheel: 1);
                }
                ViewportStateMessage? st2 = await WaitForAsync(() => sink.LastState(1) is { ReferenceLines.Count: > 0 } m
                                                                     && Math.Abs(m.ReferenceLines[0].Points[1] - y0) > 1 ? m : null).ConfigureAwait(false);
                T.Require(st2 is not null);
                T.Near(st2!.ReferenceLines[0].Points[1], st2.ReferenceLines[0].Points[3], 0.11);

                // Cross-reference: a click in the axial image at column 50 moves the sagittal viewport
                // to the plane nearest x = 50 and the coronal one to the plane nearest the clicked y.
                s.SetTool(ViewerTool.CrossReference);
                ScreenImageTransform t = await WaitForAsync(() => s.GetViewport(0).Transform).ConfigureAwait(false) ?? throw new InvalidOperationException("no transform");
                PointD screen = t.ToScreen(new PointD(50.5, 10.5));
                Input(s, 0, InputKind.Down, (int)Math.Round(screen.X), (int)Math.Round(screen.Y), 1);
                Input(s, 0, InputKind.Up, (int)Math.Round(screen.X), (int)Math.Round(screen.Y), 0);
                // Sagittal planes are at x = 4, 12, ..., 52, 60 mm: 52 is nearest to 50. Coronal planes at the
                // same y values: 12 is nearest to 10. (The display order is the toolkit's sort along the normal.)
                T.Check(await WaitUntilAsync(() => s.GetViewport(1).CurrentImage?.Plane?.FirstPixelCentre.X == 52).ConfigureAwait(false));
                T.Check(await WaitUntilAsync(() => s.GetViewport(2).CurrentImage?.Plane?.FirstPixelCentre.Y == 12).ConfigureAwait(false));
                int sagIndex = s.GetViewport(1).ImageIndex;
                T.Check(await WaitUntilAsync(() => sink.LastState(1) is { } m && m.ReferenceLines.Any(r => r.Kind == "cross")).ConfigureAwait(false));

                // Linked scrolling: two axial-parallel viewports follow each other. Put the axial series
                // in viewport 3 as well, then scroll viewport 0 with the link on.
                s.SetLayout(new LayoutModel(2, 2));
                s.AssignSeries(3, 0);
                Resize(s, 3, 128, 128);
                s.Activate(0);
                s.RunAction("linkScroll");
                T.Check(s.LinkScroll);
                int before = s.GetViewport(0).ImageIndex;
                Input(s, 0, InputKind.Wheel, 64, 64, 0, wheel: 1);
                int expected = Math.Min(before + 1, Slices - 1);
                T.Check(await WaitUntilAsync(() => s.GetViewport(3).ImageIndex == expected).ConfigureAwait(false));
                // The sagittal viewport is not parallel: it keeps its slice.
                T.Eq(s.GetViewport(1).ImageIndex, sagIndex);
            }
        });

        T.Case("viewer: sync group shares W/L, zoom and pan between chosen viewports only", async () =>
        {
            using var env = new TestEnv("sync");
            string study = await WriteMprAsync(env, "SYN1").ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(study, 128, 128).ConfigureAwait(false);
            using (s)
            {
                T.Require(await WaitUntilAsync(() => s.Loaded && s.Series.Count == 3).ConfigureAwait(false));
                Resize(s, 1, 128, 128);
                Resize(s, 2, 128, 128);
                await sink.SettleAsync().ConfigureAwait(false);
                s.SetSyncMember(0, true);
                s.SetSyncMember(1, true);
                // Right-drag W/L in viewport 0.
                Input(s, 0, InputKind.Down, 64, 64, 2);
                Input(s, 0, InputKind.Move, 90, 40, 2);
                Input(s, 0, InputKind.Up, 90, 40, 0);
                DisplayState a = s.GetViewport(0).State, b = s.GetViewport(1).State, c = s.GetViewport(2).State;
                T.Eq(a.Voi, TkVoiSource.Explicit);
                T.Eq(b.Voi, TkVoiSource.Explicit);
                T.Near(b.WindowCenter, a.WindowCenter, 1e-9);
                T.Near(b.WindowWidth, a.WindowWidth, 1e-9);
                T.Check(c.Voi != TkVoiSource.Explicit);
                // Zoom off in the group: Ctrl+wheel zoom in viewport 0 does not reach viewport 1.
                s.RunAction("syncZoom");
                T.Check(!s.Sync.Zoom);
                s.OnInput(new InputEvent(0, 0, InputKind.Wheel, 64, 64, 0, -1, 0, InputModifiers.Control, 0, 0));
                T.Check(s.GetViewport(0).State.Zoom > 0);
                T.Eq(s.GetViewport(1).State.Zoom, 0.0);
                // Pan is shared.
                s.SetTool(ViewerTool.Pan);
                Input(s, 0, InputKind.Down, 64, 64, 1);
                Input(s, 0, InputKind.Move, 74, 69, 1);
                Input(s, 0, InputKind.Up, 74, 69, 0);
                T.Near(s.GetViewport(1).State.PanX, s.GetViewport(0).State.PanX, 1e-9);
                T.Near(s.GetViewport(1).State.PanY, 5, 1e-9);
                T.Eq(s.GetViewport(2).State.PanX, 0.0);
                // The menu reports membership; the state message carries it.
                T.Check(await WaitUntilAsync(() => sink.LastState(1)?.Menu?.Synced == true).ConfigureAwait(false));
            }
        });

        T.Case("viewer: colour maps render the toolkit's standard palettes; magnifier centres the pointed pixel", async () =>
        {
            using var env = new TestEnv("cmap");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("CMAP^CT", "CMAP1"), 1, Size, Spacing);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "CMAP1"), 128, 128).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                // 1:1 (nearest neighbour): the colour map applies to each pixel's gray level.
                sink.Drain();
                s.RunAction("oneToOne");
                CapturedFrame gray = await sink.NextFrameAsync().ConfigureAwait(false);
                await sink.SettleAsync().ConfigureAwait(false);
                T.Eq(gray.Format, FramePixelFormat.Gray8);
                sink.Drain();
                s.SetColormap("hotIron");
                T.Eq(s.GetViewport(0).State.Colormap, TkColormap.HotIron);
                CapturedFrame hot = await sink.NextFrameAsync().ConfigureAwait(false);
                T.Eq(hot.Format, FramePixelFormat.Rgba);
                T.Eq(hot.Pixels.Length, gray.Pixels.Length * 4);
                byte[] lut = Tk.ColormapRgba8(TkColormap.HotIron);
                int mismatches = 0;
                for (int i = 0; i < gray.Pixels.Length; i++)
                {
                    int g = gray.Pixels[i];
                    for (int k = 0; k < 3; k++)
                    {
                        if (Math.Abs(hot.Pixels[(4 * i) + k] - lut[(4 * g) + k]) > 1)
                        {
                            mismatches++;
                        }
                    }
                }
                T.Eq(mismatches, 0);
                T.Eq(sink.LastState(0)?.Menu?.Colormap, "hotIron");
                T.Check(sink.LastState(0)?.Menu?.Colormaps.Count == 6);
                T.Check(!s.GetViewport(0).State.Equals(new DisplayState()));
                s.SetColormap("no-such-map");
                T.Eq(s.GetViewport(0).State.Colormap, TkColormap.HotIron);
                s.SetColormap("gray");

                // Magnifier: hold the tool, frames arrive on the magnifier index; its centre is the
                // image point under the pointer (the toolkit's own mapping says so).
                s.SetTool(ViewerTool.Magnify);
                sink.Drain();
                Input(s, 0, InputKind.Down, 40, 50, 1);
                Input(s, 0, InputKind.Move, 44, 52, 1);
                CapturedFrame? mag = null;
                for (int i = 0; i < 100 && mag is null; i++)
                {
                    mag = sink.Frames.LastOrDefault(f => f.Viewport == ViewerSession.MagnifierViewportBase);
                    await Task.Delay(20).ConfigureAwait(false);
                }
                // The newest one is for the last pointer position.
                await Task.Delay(300).ConfigureAwait(false);
                mag = sink.Frames.LastOrDefault(f => f.Viewport == ViewerSession.MagnifierViewportBase);
                Input(s, 0, InputKind.Up, 44, 52, 0);
                T.Require(mag is not null);
                T.Eq(mag!.Width, ViewerSession.MagnifierCssSize);
                Viewport vp = s.GetViewport(0);
                ScreenImageTransform t = vp.Transform!;
                SeriesImage img = vp.CurrentImage!;
                RenderSpec spec = ViewerSession.MagnifierSpec(vp.FrameInfo!, vp.State, t, 44, 52, 1, 1);
                using TkFile f = TkFile.Open(img.Instance.FilePath, headersOnly: false);
                using TkFrame fr = TkFrame.Open(f, img.Frame);
                (double X, double Y) centre = fr.ViewToImage(spec, spec.Width / 2.0, spec.Height / 2.0);
                PointD under = t.ToImage(new PointD(44, 52));
                T.Near(centre.X, under.X, 1e-6);
                T.Near(centre.Y, under.Y, 1e-6);
                T.Near(spec.Zoom, t.Zoom * ViewerSession.MagnifierFactor, 1e-9);
                // The magnifier frame equals the toolkit's render of that spec.
                byte[] reference = fr.Render(spec);
                T.Check(reference.AsSpan().SequenceEqual(mag.Pixels));
                // No magnifier frames after the button is up.
                await sink.SettleAsync().ConfigureAwait(false);
                sink.Drain();
                Input(s, 0, InputKind.Move, 60, 60, 0);
                await Task.Delay(200).ConfigureAwait(false);
                T.Check(!sink.Frames.Any(x => x.Viewport >= ViewerSession.MagnifierViewportBase));
            }
        });

        T.Case("viewer: arrow and text annotations (text stored as typed, empty text removes); label and export audit", async () =>
        {
            using var env = new TestEnv("arrow");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("ARROW^CT", "ARR1"), 1, Size, Spacing);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "ARR1"), 128, 128).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                FrameKey key = s.GetViewport(0).CurrentImage!.Key;
                s.SetTool(ViewerTool.Arrow);
                Input(s, 0, InputKind.Down, 30, 30, 1);
                Input(s, 0, InputKind.Move, 60, 70, 1);
                Input(s, 0, InputKind.Up, 60, 70, 0);
                T.Require(await WaitUntilAsync(() => s.GetAnnotations(key).Count == 1).ConfigureAwait(false));
                Annotation arrow = s.GetAnnotations(key)[0];
                T.Eq(arrow.Kind, AnnotationKind.Arrow);
                T.Eq(arrow.Count, 2);
                T.Eq(arrow.Label, "");

                s.SetTool(ViewerTool.Text);
                Input(s, 0, InputKind.Down, 80, 40, 1);
                Input(s, 0, InputKind.Up, 80, 40, 0);
                ViewportStateMessage? prompt = await WaitForAsync(() => sink.LastState(0) is { TextPrompt: not null } m ? m : null).ConfigureAwait(false);
                T.Require(prompt is not null);
                const string typed = "  lesion ^1  ";
                await s.SetAnnotationTextAsync(prompt!.TextPrompt!, typed).ConfigureAwait(false);
                Annotation text = s.GetAnnotations(key).Single(a => a.Kind == AnnotationKind.Text);
                T.Eq(text.Text, typed);
                T.Eq(text.Label, typed);
                T.Check(sink.LastState(0)!.Annotations.Any(a => a.Kind == "text" && a.Label == typed));
                // Saved and loaded unchanged.
                AnnotationDocument saved = env.Annotations.Load(key.SopInstanceUid, key.Frame);
                T.Eq(saved.Annotations.Single(a => a.Kind == AnnotationKind.Text).Text, typed);
                // A second label cancelled at the prompt is removed.
                Input(s, 0, InputKind.Down, 90, 90, 1);
                Input(s, 0, InputKind.Up, 90, 90, 0);
                ViewportStateMessage? p2 = await WaitForAsync(() => sink.LastState(0) is { TextPrompt: not null } m && m.TextPrompt != prompt.TextPrompt ? m : null)
                    .ConfigureAwait(false);
                T.Require(p2 is not null);
                await s.SetAnnotationTextAsync(p2!.TextPrompt!, "").ConfigureAwait(false);
                T.Eq(s.GetAnnotations(key).Count(a => a.Kind == AnnotationKind.Text), 1);

                // Every state message carries the intended-use label for exports.
                T.Eq(sink.LastState(0)!.Disclaimer, Disclaimer.Text);
                s.ReportExport("file", true, null);
                s.ReportExport("clipboard", false, "NotAllowedError <script>");
            }
            env.Audit.Dispose();
            string[] lines = File.ReadAllLines(env.Paths.AuditFile);
            string[] exports = [.. lines.Where(l => l.Contains("\"image.export\"", StringComparison.Ordinal))];
            T.Eq(exports.Length, 2);
            T.Check(exports[0].Contains("\"outcome\":\"" + AuditOutcome.Success + "\"", StringComparison.Ordinal));
            T.Check(exports[1].Contains("\"outcome\":\"" + AuditOutcome.Failure + "\"", StringComparison.Ordinal));
            T.Check(exports[1].Contains("\"reason\":\"NotAllowedErrorscript\"", StringComparison.Ordinal));
            T.Check(AuditLog.Verify(env.Paths.AuditFile).IsIntact);
        });
    }
}
