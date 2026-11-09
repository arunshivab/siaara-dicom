// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Tests;

internal static class PerfTests
{
    private static double P95(List<double> v)
    {
        v.Sort();
        return v[(int)Math.Min(v.Count - 1, Math.Ceiling(v.Count * 0.95) - 1)];
    }

    public static void Register()
    {
        T.Case("perf (reported): 500-slice CT import from a folder and a DICOMDIR, first image, W/L drag, scroll", async () =>
        {
            int slices = T.Quick ? 100 : 500, size = T.Quick ? 256 : 512;
            using var env = new TestEnv("perf");
            string dir = env.Dir("ct");
            var gen = Stopwatch.StartNew();
            IReadOnlyList<string> files = SampleData.WriteCtSeries(dir, SampleData.Patient("PERF^CT", "PERF1"), slices, size, 0.7, 1.0);
            T.Note($"generated {slices} x {size}x{size} CT in {gen.Elapsed.TotalSeconds:F1} s");
            var imp = Stopwatch.StartNew();
            var p = await env.ImportAsync(dir).ConfigureAwait(false);
            imp.Stop();
            T.Eq(p.Added, (long)slices);
            T.Perf($"import (copy) {slices} files of {size}x{size}: {imp.Elapsed.TotalSeconds:F2} s = {slices / imp.Elapsed.TotalSeconds:F0} files/s");

            // The same set through a DICOMDIR, indexed in place, into a second store.
            using (var env2 = new TestEnv("perf-dicomdir"))
            {
                string set = env2.Dir("set");
                Tk.WriteDicomdir(set, files);
                var d = Stopwatch.StartNew();
                var p2 = await env2.ImportAsync(set, inPlace: true).ConfigureAwait(false);
                T.Check(p2.UsedDicomdir);
                T.Eq(p2.Added, (long)slices);
                T.Perf($"import from DICOMDIR (index in place) {slices} files: {d.Elapsed.TotalSeconds:F2} s = {slices / d.Elapsed.TotalSeconds:F0} files/s");
            }

            var open = Stopwatch.StartNew();
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "PERF1"), 512, 512).ConfigureAwait(false);
            using (s)
            {
                await sink.FrameSignal.WaitAsync(20000).ConfigureAwait(false);
                double first = open.Elapsed.TotalMilliseconds;
                T.Perf($"first image {first:F0} ms after opening the study ({slices} headers read and sorted, one {size}x{size} decode, target < 300 ms)");
                await sink.SettleAsync(300).ConfigureAwait(false);
                await Task.Delay(500).ConfigureAwait(false); // let the ±10 prefetch run

                // W/L drag on a cached frame: time from input to frame, one at a time.
                var wl = new List<double>();
                s.OnInput(new InputEvent(0, 0, InputKind.Down, 256, 256, 2, 0, 0, InputModifiers.None, 0, 0));
                for (int i = 1; i <= 100; i++)
                {
                    sink.Drain();
                    var t = Stopwatch.StartNew();
                    s.OnInput(new InputEvent(0, 0, InputKind.Move, 256 + i, 256 + (i / 2), 2, 0, 0, InputModifiers.None, 0, 0));
                    await sink.FrameSignal.WaitAsync(5000).ConfigureAwait(false);
                    wl.Add(t.Elapsed.TotalMilliseconds);
                    await Task.Delay(2).ConfigureAwait(false);
                }
                s.OnInput(new InputEvent(0, 0, InputKind.Up, 356, 306, 0, 0, 0, InputModifiers.None, 0, 0));
                T.Perf($"W/L drag 512x512 viewport on a {size}x{size} frame: median {wl.Order().ElementAt(wl.Count / 2):F1} ms, p95 {P95(wl):F1} ms (target p95 < 16 ms; render + in-process delivery, no browser)");

                // Scroll through every slice, one wheel step per acknowledged frame.
                await sink.SettleAsync(200).ConfigureAwait(false);
                var scroll = Stopwatch.StartNew();
                var per = new List<double>();
                for (int i = 1; i < slices; i++)
                {
                    sink.Drain();
                    var t = Stopwatch.StartNew();
                    s.OnInput(new InputEvent(0, 0, InputKind.Wheel, 10, 10, 0, 1, 0, InputModifiers.None, 0, 0));
                    await sink.FrameSignal.WaitAsync(5000).ConfigureAwait(false);
                    per.Add(t.Elapsed.TotalMilliseconds);
                }
                scroll.Stop();
                T.Eq(s.GetViewport(0).ImageIndex, slices - 1);
                T.Perf($"scroll {slices} slices: {scroll.Elapsed.TotalSeconds:F2} s, {slices / scroll.Elapsed.TotalSeconds:F0} slices/s, p95 {P95(per):F1} ms per slice");
            }
        });
    }
}
