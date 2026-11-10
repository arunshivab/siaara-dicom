// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Viewing;
using Siaara.Viewer.Web.Streaming;

namespace Siaara.Viewer.Tests;

internal static class ProtocolTests
{
    public static void Register()
    {
        T.Case("protocol: flow control keeps one frame in flight and ignores stale acks", () =>
        {
            var f = new FrameFlow();
            T.Check(f.Request());          // idle: render now
            T.Check(!f.Request());         // rendering: coalesced
            T.Check(!f.Request());
            uint s1 = f.BeginSend();
            T.Eq(f.InFlight, 1);
            T.Check(!f.Request());         // in flight: coalesced
            T.Check(!f.Ack(s1 + 5));       // unknown seq ignored
            T.Check(!f.Ack(0));
            T.Eq(f.InFlight, 1);
            T.Check(f.Ack(s1));            // ack: one render of the newest state
            uint s2 = f.BeginSend();
            T.Check(s2 > s1);
            T.Check(!f.Ack(s1));           // stale ack of an older frame does nothing
            T.Eq(f.InFlight, 1);
            T.Check(!f.Ack(s2));           // nothing dirty: idle
            T.Eq(f.InFlight, 0);
            T.Check(!f.Busy);
            T.Check(f.Request());
            T.Check(!f.Abandon());
            T.Check(f.Request());
            T.Check(!f.Request());
            T.Check(f.Abandon());          // a newer request was waiting
        });

        T.Case("protocol: messages encode and decode (frame, state, ack, input, command)", () =>
        {
            var hdr = new byte[FrameProtocol.FrameHeaderBytes];
            FrameProtocol.WriteFrameHeader(hdr, FramePixelFormat.Gray8, 7, 123456789, 640, 480);
            var h = FrameProtocol.ReadFrameHeader(hdr);
            T.Eq(h, (FramePixelFormat.Gray8, 7, 123456789u, 640, 480));
            T.Eq(hdr[0], (byte)1);
            byte[] st = FrameProtocol.State(3, 42, "{\"a\":1}"u8);
            T.Eq(FrameProtocol.ReadState(st), (3, 42u, "{\"a\":1}"));
            BrowserMessage? ack = FrameProtocol.Decode(FrameProtocol.Ack(2, 99));
            T.Eq(ack?.Type, MessageType.Ack);
            T.Eq(ack?.Viewport, 2);
            T.Eq(ack?.Seq, 99u);
            var ev = new InputEvent(1, 5, InputKind.Wheel, -12, 300, 4, -1, 38, InputModifiers.Control | InputModifiers.Shift, 800, 600);
            BrowserMessage? inp = FrameProtocol.Decode(FrameProtocol.Input(ev));
            T.Eq(inp?.Input, ev);
            BrowserMessage? cmd = FrameProtocol.Decode(FrameProtocol.Command(4, "{\"tool\":\"length\"}"));
            T.Eq(cmd?.Type, MessageType.Command);
            T.Eq(System.Text.Encoding.UTF8.GetString(cmd!.Value.Json.Span), "{\"tool\":\"length\"}");
            T.Check(FrameProtocol.Decode(new byte[] { 9, 0, 0, 0 }) is null);
            T.Check(FrameProtocol.Decode(new byte[] { 4, 0, 0 }) is null); // short input
            T.Check(InputRouter.TryParseTool("ellipse", out ViewerTool t) && t == ViewerTool.Ellipse);
        });

        T.Case("protocol: a slow browser never has more than one frame in flight; the newest state wins", async () =>
        {
            using var env = new TestEnv("backpressure");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("BP^CT", "BP1"), 120, 64);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "BP1"), 128, 128).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                sink.AckDelayMs = 40;
                sink.Drain();
                for (int i = 0; i < 100; i++)
                {
                    s.OnInput(new InputEvent(0, (uint)i, InputKind.Wheel, 10, 10, 0, 1, 0, InputModifiers.None, 0, 0));
                    await Task.Delay(1).ConfigureAwait(false);
                }
                await sink.SettleAsync(300).ConfigureAwait(false);
                int frames = sink.Frames.Count;
                T.Note($"100 scroll inputs with a 40 ms ack delay gave {frames} frames");
                T.Eq(sink.MaxInFlight, 1);
                T.Check(frames < 100);
                T.Eq(s.GetViewport(0).ImageIndex, 100);
                T.Eq(sink.LastState(0)?.ImageIndex, 100); // the last frame shows the newest state
                uint last = sink.Frames.Last().Seq;
                T.Check(sink.Frames.Select(f => f.Seq).Zip(sink.Frames.Select(f => f.Seq).Skip(1)).All(p => p.Second > p.First));
                T.Check(last > 0);
            }
        });

        T.Case("cine: a multi-frame US plays at its own rate (Frame Time 40 ms = 25 fps)", async () =>
        {
            using var env = new TestEnv("cine");
            string dir = env.Dir("in");
            SampleData.WriteUsClip(Path.Combine(dir, "clip.dcm"), SampleData.Patient("CINE^US", "CINE1"), frames: 60, frameTimeMs: 40);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(Gen.StudyOf(env, "CINE1"), 160, 120).ConfigureAwait(false);
            using (s)
            {
                await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
                sink.Drain();
                var sw = Stopwatch.StartNew();
                s.RunAction("cineToggle");
                await Task.Delay(2000).ConfigureAwait(false);
                s.RunAction("cineToggle");
                double secs = sw.Elapsed.TotalSeconds;
                int shown = sink.Frames.Count;
                int index = s.GetViewport(0).ImageIndex;
                T.Perf($"cine 25 fps clip: {shown} frames in {secs:F2} s = {shown / secs:F1} fps shown, index {index}");
                // How many frames a loaded machine renders is performance (reported above); the clock-driven
                // position below is the correctness check. Never more frames than the clock allows.
                T.Check(shown >= 1 && shown <= 56);
                T.Check(index is >= 45 and <= 55);
                // With a slow browser frames are skipped but the position follows the clock.
                sink.Drain();
                sink.AckDelayMs = 150;
                s.RunAction("cineFirst");
                await sink.SettleAsync(250).ConfigureAwait(false);
                sink.Drain();
                s.RunAction("cineToggle");
                await Task.Delay(1000).ConfigureAwait(false);
                s.RunAction("cineToggle");
                int slowIndex = s.GetViewport(0).ImageIndex;
                T.Perf($"cine with a 150 ms ack delay: {sink.Frames.Count} frames sent in 1 s, position {slowIndex} (true speed: about 25)");
                T.Check(sink.Frames.Count <= 8);
                T.Check(slowIndex is >= 20 and <= 30);
                T.Eq(sink.MaxInFlight, 1);
            }
        });
    }
}
