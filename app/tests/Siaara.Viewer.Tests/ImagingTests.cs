// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Tests;

internal static class ImagingTests
{
    private static readonly TkTag OverlayRows = new(0x60000010);
    private static readonly TkTag OverlayColumns = new(0x60000011);
    private static readonly TkTag OverlayType = new(0x60000040);
    private static readonly TkTag OverlayOrigin = new(0x60000050);
    private static readonly TkTag OverlayBitsAllocated = new(0x60000100);
    private static readonly TkTag OverlayBitPosition = new(0x60000102);
    private static readonly TkTag OverlayData = new(0x60003000);

    /// <summary>Waits for the frames an action causes and returns the last one.</summary>
    public static async Task<CapturedFrame> AfterAsync(FakeSink sink, Action action)
    {
        sink.Drain();
        action();
        await sink.FrameSignal.WaitAsync(10000).ConfigureAwait(false);
        await sink.SettleAsync().ConfigureAwait(false);
        return sink.Frames.Last();
    }

    public static async Task<CapturedFrame> FirstFrameAsync(FakeSink sink)
    {
        await sink.FrameSignal.WaitAsync(15000).ConfigureAwait(false);
        await sink.SettleAsync().ConfigureAwait(false);
        return sink.Frames.Last();
    }

    private static byte[] Reference(SeriesImage img, RenderSpec spec)
    {
        using TkFile f = TkFile.Open(img.Instance.FilePath, headersOnly: false);
        using TkFrame fr = TkFrame.Open(f, img.Frame);
        return fr.Render(spec);
    }

    public static void Register()
    {
        T.Case("series: uneven spacing is reported in plain words", async () =>
        {
            using var env = new TestEnv("uneven");
            string dir = env.Dir("in");
            double[] z = [0, 2, 4, 7, 9, 11];
            SampleData.WriteCtSeries(dir, SampleData.Patient("UNEVEN^CT", "UNEV1"), z.Length, 16, positionOf: k => z[k]);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink _) = await env.OpenAsync(Gen.StudyOf(env, "UNEV1")).ConfigureAwait(false);
            using (s)
            {
                SeriesSource src = s.Series.Single();
                T.Eq(src.Images.Count, 6);
                T.Check(src.Warnings.Any(w => w.Contains("Uneven slice spacing", StringComparison.Ordinal)));
                // Sorted by position (instance numbers follow the same order here).
                T.Eq(src.Images[0].Instance.Header.GetString(Tags.InstanceNumber), "1");
            }
        });

        T.Case("series: several stacks in one series become sub-series", async () =>
        {
            using var env = new TestEnv("stacks");
            string dir = env.Dir("in");
            SamplePatient p = SampleData.Patient("STACK^CT", "STK1");
            var files = SampleData.WriteCtSeries(dir, p, 8, 16).ToList();
            // Re-orient half of them sagittally, same series.
            for (int i = 4; i < 8; i++)
            {
                using TkFile f = TkFile.Open(files[i], headersOnly: false);
                f.SetString(Tags.ImageOrientationPatient, "0\\1\\0\\0\\0\\-1");
                f.SetString(Tags.ImagePositionPatient, (i * 2).ToString(System.Globalization.CultureInfo.InvariantCulture) + "\\0\\0");
                f.Save(files[i]);
            }
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink _) = await env.OpenAsync(Gen.StudyOf(env, "STK1")).ConfigureAwait(false);
            using (s)
            {
                T.Note("sub-series: " + s.Series.Count + "; warnings: " + string.Join(" | ", s.Series[0].Warnings));
                T.Check(s.Series.Count >= 2);
                T.Eq(s.Series.Sum(x => x.Images.Count), 8);
                T.Check(s.Series[0].Warnings.Count > 0);
            }
        });

        T.Case("series: multi-frame expands to one image per frame with its own cine rate", async () =>
        {
            using var env = new TestEnv("multiframe");
            string dir = env.Dir("in");
            SampleData.WriteUsClip(Path.Combine(dir, "clip.dcm"), SampleData.Patient("US^CLIP", "US1"), frames: 30, frameTimeMs: 40);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "US1"), 160, 120).ConfigureAwait(false);
            using (s)
            {
                SeriesSource src = s.Series.Single();
                T.Eq(src.Images.Count, 30);
                T.Check(src.IsMultiFrame);
                T.Eq(src.Images[29].Frame, 29);
                T.Near(src.DefaultFps(10, 15), 25.0, 1e-9);
                T.Near(s.GetViewport(0).Cine.Fps, 25.0, 1e-9);
                CapturedFrame f = await FirstFrameAsync(sink).ConfigureAwait(false);
                T.Eq(f.Format, FramePixelFormat.Gray8);
            }
        });

        T.Case("series: lossy flag shows LOSSY and a plain-word warning", async () =>
        {
            using var env = new TestEnv("lossy");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("LOSSY^CT", "LOS1"), 3, 16, lossyFlag: true);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "LOS1")).ConfigureAwait(false);
            using (s)
            {
                T.Check(s.Series[0].AnyLossy);
                await FirstFrameAsync(sink).ConfigureAwait(false);
                ViewportStateMessage st = sink.LastState(0)!;
                T.Check(st.Warnings.Any(w => w.StartsWith("LOSSY", StringComparison.Ordinal)));
                T.Check(st.OverlayText!.BottomRight.Contains("LOSSY"));
            }
        });

        T.Case("render: file, explicit and full-range windows equal the toolkit's reference bytes", async () =>
        {
            using var env = new TestEnv("render");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(Path.Combine(dir, "ct"), SampleData.Patient("REN^CT", "REN1"), 5, 64);
            SampleData.WriteCrNoSpacing(Path.Combine(dir, "cr", "cr.dcm"), SampleData.Patient("REN^CR", "REN2"), 64);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "REN1"), 128, 96).ConfigureAwait(false);
            using (s)
            {
                CapturedFrame f = await FirstFrameAsync(sink).ConfigureAwait(false);
                SeriesImage img = s.GetViewport(0).CurrentImage!;
                RenderSpec spec = new DisplayState().ToSpec(128, 96, gray: true);
                T.Eq(f.Width, 128);
                T.Eq(f.Height, 96);
                T.Check(f.Pixels.AsSpan().SequenceEqual(Reference(img, spec)));
                T.Check(sink.LastState(0)!.OverlayText!.BottomRight.Any(l => l.StartsWith("W 400 L 40", StringComparison.Ordinal)));

                CapturedFrame lung = await AfterAsync(sink, () => s.ApplyPreset(3)).ConfigureAwait(false);
                T.Check(lung.Pixels.AsSpan().SequenceEqual(Reference(img, spec with { Voi = TkVoiSource.Explicit, WindowCenter = -600, WindowWidth = 1500 })));
                T.Check(!lung.Pixels.AsSpan().SequenceEqual(f.Pixels));
                T.Check(sink.LastState(0)!.OverlayText!.BottomRight.Any(l => l.Contains("(Lung)", StringComparison.Ordinal)));

                CapturedFrame inv = await AfterAsync(sink, () => s.RunAction("invert")).ConfigureAwait(false);
                T.Check(inv.Pixels.AsSpan().SequenceEqual(Reference(img, spec with { Voi = TkVoiSource.Explicit, WindowCenter = -600, WindowWidth = 1500, Invert = true })));
            }
            (ViewerSession s2, FakeSink sink2) = await env.OpenAsync(Gen.StudyOf(env, "REN2"), 64, 64).ConfigureAwait(false);
            using (s2)
            {
                CapturedFrame f = await FirstFrameAsync(sink2).ConfigureAwait(false);
                SeriesImage img = s2.GetViewport(0).CurrentImage!;
                T.Check(f.Pixels.AsSpan().SequenceEqual(Reference(img, new DisplayState().ToSpec(64, 64, true) with { Voi = TkVoiSource.FullRange })));
                T.Check(sink2.LastState(0)!.OverlayText!.BottomRight.Any(l => l.Contains("full range", StringComparison.Ordinal)));
            }
        });

        T.Case("render: overlays on and off", async () =>
        {
            using var env = new TestEnv("overlay");
            string dir = env.Dir("in");
            var files = SampleData.WriteCtSeries(dir, SampleData.Patient("OVL^CT", "OVL1"), 1, 32);
            using (TkFile f = TkFile.Open(files[0], headersOnly: false))
            {
                f.SetUInt16(OverlayRows, 32);
                f.SetUInt16(OverlayColumns, 32);
                f.SetRawString(OverlayType, "CS", "G");
                f.SetBytes(OverlayOrigin, "SS", [1, 0, 1, 0]);
                f.SetUInt16(OverlayBitsAllocated, 1);
                f.SetUInt16(OverlayBitPosition, 0);
                var bits = new byte[32 * 32 / 8];
                for (int i = 0; i < bits.Length / 2; i++)
                {
                    bits[i] = 0xFF; // top half set
                }
                f.SetBytes(OverlayData, "OW", bits);
                f.Save(files[0]);
            }
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "OVL1"), 32, 32).ConfigureAwait(false);
            using (s)
            {
                CapturedFrame on = await FirstFrameAsync(sink).ConfigureAwait(false);
                CapturedFrame off = await AfterAsync(sink, () => s.RunAction("overlays")).ConfigureAwait(false);
                SeriesImage img = s.GetViewport(0).CurrentImage!;
                RenderSpec spec = new DisplayState().ToSpec(32, 32, true);
                T.Check(on.Pixels.AsSpan().SequenceEqual(Reference(img, spec)));
                T.Check(off.Pixels.AsSpan().SequenceEqual(Reference(img, spec with { NoOverlays = true, NoShutters = true })));
                T.Check(!on.Pixels.AsSpan().SequenceEqual(off.Pixels));
                T.Eq(on.Pixels[5 * 32 + 5], (byte)255); // overlay is white in the top half
            }
        });

        T.Case("viewer: layouts 1x1 to 4x4, default layout and privacy hides all text", async () =>
        {
            using var env = new TestEnv("layouts");
            string dir = env.Dir("in");
            SamplePatient p = SampleData.Patient("LAY^OUT", "LAY1");
            for (int i = 1; i <= 5; i++)
            {
                SampleData.WriteCtSeries(Path.Combine(dir, "s" + i.ToString(System.Globalization.CultureInfo.InvariantCulture)), p, 2, 16, seriesNumber: i);
            }
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "LAY1")).ConfigureAwait(false);
            using (s)
            {
                T.Eq(s.Layout, new LayoutModel(2, 2));
                T.Eq(s.Series.Count, 5);
                for (int v = 0; v < 4; v++)
                {
                    T.Eq(s.GetViewport(v).Series?.SeriesNumber, v + 1);
                }
                for (int r = 1; r <= 4; r++)
                {
                    for (int c = 1; c <= 4; c++)
                    {
                        s.SetLayout(new LayoutModel(r, c));
                        T.Eq(s.Layout.Count, r * c);
                    }
                }
                s.SetLayout(new LayoutModel(1, 1));
                await FirstFrameAsync(sink).ConfigureAwait(false);
                T.Check(sink.LastState(0)!.OverlayText!.TopLeft.Contains("SAMPLE, LAY OUT"));
                await AfterAsync(sink, () => s.RunAction("privacy")).ConfigureAwait(false);
                ViewportStateMessage st = sink.LastState(0)!;
                T.Check(st.Privacy && st.OverlayText is null && st.Orientation is null);
                s.AssignSeries(0, 4);
                T.Eq(s.GetViewport(0).Series?.SeriesNumber, 5);
            }
        });
    }
}
