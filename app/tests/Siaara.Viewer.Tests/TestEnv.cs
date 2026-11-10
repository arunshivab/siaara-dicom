// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Buffers;
using System.Collections.Concurrent;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Import;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Tests;

/// <summary>A temporary store with every service around it.</summary>
internal sealed class TestEnv : IDisposable
{
    public TestEnv(string name)
    {
        Root = T.NewTempDir(name);
        Paths = new AppPaths(Path.Combine(Root, "data"));
        Paths.EnsureCreated();
        Scheduler = new ToolkitScheduler();
        Store = StoreService.Open(Path.Combine(Root, "store"), Scheduler);
        Settings = new SettingsStore(Paths.SettingsFile);
        Audit = new AuditLog(Paths.AuditFile);
        Cache = new RenderCache(Scheduler, 512L * 1024 * 1024, 32);
        Annotations = new AnnotationStore(Paths.AnnotationFolder, "tester");
        Import = new ImportService(Store, Scheduler, Audit);
        Services = new ViewerServices(Store, Cache, Scheduler, Settings, Annotations, Audit);
    }

    public string Root { get; }
    public AppPaths Paths { get; }
    public ToolkitScheduler Scheduler { get; }
    public StoreService Store { get; }
    public SettingsStore Settings { get; }
    public AuditLog Audit { get; }
    public RenderCache Cache { get; }
    public AnnotationStore Annotations { get; }
    public ImportService Import { get; }
    public ViewerServices Services { get; }

    public string Dir(string name)
    {
        string d = Path.Combine(Root, name);
        Directory.CreateDirectory(d);
        return d;
    }

    public async Task<ImportProgress> ImportAsync(string folder, bool inPlace = false)
    {
        ImportJob job = Import.Start(new ImportRequest(folder, inPlace));
        await job.Completion.ConfigureAwait(false);
        return job.Progress;
    }

    /// <summary>A loaded session on the first study, with a fake sink attached and viewport 0 sized.</summary>
    public async Task<(ViewerSession Session, FakeSink Sink)> OpenAsync(string studyUid, int w = 256, int h = 256)
    {
        var s = new ViewerSession("test-" + Guid.NewGuid().ToString("N")[..8], studyUid, Services);
        await s.LoadAsync().ConfigureAwait(false);
        var sink = new FakeSink(s);
        s.Attach(sink);
        s.OnInput(new InputEvent(0, 1, InputKind.Resize, 0, 0, 0, 0, 0, InputModifiers.None, w, h));
        return (s, sink);
    }

    public void Dispose()
    {
        Cache.Dispose();
        Store.Dispose();
        Audit.Dispose();
        Scheduler.Dispose();
    }
}

/// <summary>A captured frame.</summary>
internal sealed record CapturedFrame(int Viewport, uint Seq, FramePixelFormat Format, int Width, int Height, byte[] Pixels, DateTime At);

/// <summary>Captures what a session sends; acks automatically unless told not to.</summary>
internal sealed class FakeSink(ViewerSession session) : IViewSink
{
    private readonly int[] _unacked = new int[32];
    private int _maxInFlight;

    public int FrameHeaderBytes => 12;

    public ConcurrentQueue<CapturedFrame> Frames { get; } = new();

    public ConcurrentQueue<(int Viewport, uint Seq, ViewportStateMessage State)> States { get; } = new();

    public SemaphoreSlim FrameSignal { get; } = new(0);

    public SemaphoreSlim StateSignal { get; } = new(0);

    public bool AutoAck { get; set; } = true;

    public int AckDelayMs { get; set; }

    public int MaxInFlight => Volatile.Read(ref _maxInFlight);

    public void SendFrame(int viewport, uint seq, FramePixelFormat format, int width, int height, byte[] rented, int length)
    {
        int n = Interlocked.Increment(ref _unacked[Slot(viewport)]);
        int m;
        while (n > (m = Volatile.Read(ref _maxInFlight)) && Interlocked.CompareExchange(ref _maxInFlight, n, m) != m)
        {
        }
        byte[] px = rented.AsSpan(FrameHeaderBytes, length - FrameHeaderBytes).ToArray();
        ArrayPool<byte>.Shared.Return(rented);
        Frames.Enqueue(new CapturedFrame(viewport, seq, format, width, height, px, DateTime.UtcNow));
        FrameSignal.Release();
        if (AutoAck)
        {
            if (AckDelayMs > 0)
            {
                _ = Task.Delay(AckDelayMs).ContinueWith(_ => Ack(viewport, seq), TaskScheduler.Default);
            }
            else
            {
                _ = Task.Run(() => Ack(viewport, seq));
            }
        }
    }

    // Viewports 0..15, magnifiers (index + ViewerSession.MagnifierViewportBase) 16..31.
    private static int Slot(int viewport) => viewport >= ViewerSession.MagnifierViewportBase ? 16 + ((viewport - ViewerSession.MagnifierViewportBase) & 15) : viewport & 15;

    public void Ack(int viewport, uint seq)
    {
        Interlocked.Decrement(ref _unacked[Slot(viewport)]);
        session.Ack(viewport, seq);
    }

    public void SendState(int viewport, uint seq, ViewportStateMessage state)
    {
        States.Enqueue((viewport, seq, state));
        StateSignal.Release();
    }

    public async Task<CapturedFrame> NextFrameAsync(int timeoutMs = 10000)
    {
        if (!await FrameSignal.WaitAsync(timeoutMs).ConfigureAwait(false))
        {
            throw new TimeoutException("no frame");
        }
        CapturedFrame? last = null;
        while (Frames.TryDequeue(out CapturedFrame? f))
        {
            last = f;
            if (Frames.IsEmpty)
            {
                break;
            }
        }
        while (FrameSignal.CurrentCount > 0 && Frames.IsEmpty)
        {
            FrameSignal.Wait(0);
        }
        return last!;
    }

    public void Drain()
    {
        while (Frames.TryDequeue(out _))
        {
        }
        while (FrameSignal.Wait(0))
        {
        }
        while (States.TryDequeue(out _))
        {
        }
        while (StateSignal.Wait(0))
        {
        }
    }

    public ViewportStateMessage? LastState(int viewport) => States.LastOrDefault(s => s.Viewport == viewport).State;

    /// <summary>Waits until the session has gone quiet (no frame for quietMs).</summary>
    public async Task SettleAsync(int quietMs = 150, int timeoutMs = 10000)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs)
        {
            if (!await FrameSignal.WaitAsync(quietMs).ConfigureAwait(false))
            {
                return;
            }
        }
    }
}

/// <summary>Synthetic test data written through the toolkit.</summary>
internal static class Gen
{
    public static string StudyOf(TestEnv env, string patientId)
    {
        StudyPage p = env.Store.QueryStudiesAsync(new StudyQuery { Text = patientId }).GetAwaiter().GetResult();
        return p.Rows.First(r => r.PatientId == patientId).StudyInstanceUid;
    }
}
