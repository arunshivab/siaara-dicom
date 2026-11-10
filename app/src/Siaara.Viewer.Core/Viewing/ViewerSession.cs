// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Buffers;
using System.Globalization;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Geometry;
using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>Services a session uses.</summary>
/// <param name="Store">Store.</param>
/// <param name="Cache">Render cache.</param>
/// <param name="Scheduler">Scheduler.</param>
/// <param name="Settings">Settings.</param>
/// <param name="Annotations">Annotation store.</param>
/// <param name="Audit">Audit log for image exports and annotation / calibration changes, or null (tests).</param>
public sealed record ViewerServices(StoreService Store, RenderCache Cache, ToolkitScheduler Scheduler, SettingsStore Settings, AnnotationStore Annotations,
                                    AuditLog? Audit = null);

/// <summary>Which display properties the viewports of the sync group share.</summary>
/// <param name="Members">Viewport indexes in the group.</param>
/// <param name="WindowLevel">Window centre / width (and file VOI choice).</param>
/// <param name="Zoom">Zoom factor.</param>
/// <param name="Pan">Pan.</param>
public sealed record SyncGroup(IReadOnlySet<int> Members, bool WindowLevel, bool Zoom, bool Pan)
{
    /// <summary>True when the viewport shares something with others.</summary>
    /// <param name="viewport">Viewport index.</param>
    /// <returns>Membership.</returns>
    public bool Has(int viewport) => Members.Contains(viewport) && (WindowLevel || Zoom || Pan);
}

/// <summary>
/// One open study in one browser tab: series, layout, viewports, tools, annotations, cine. The
/// browser talks to it through the /ws/view socket (input, commands, acks); the Blazor toolbar
/// calls the same methods. Renders run on the Interactive pool, decodes on the Decode pool, at
/// most one render and one unacknowledged frame per viewport. All state is guarded by one lock;
/// no toolkit call is made while holding it.
/// </summary>
public sealed class ViewerSession : IDisposable
{
    private const int MaxViewports = LayoutModel.MaxSide * LayoutModel.MaxSide;

    /// <summary>Magnifier frames travel as viewport index + this value (frame and ack messages).</summary>
    public const int MagnifierViewportBase = 0x4000;

    /// <summary>Magnification of the magnifier glass relative to the viewport's zoom.</summary>
    public const double MagnifierFactor = 3.0;

    /// <summary>Magnifier glass side in CSS pixels (times the device pixel ratio on the wire).</summary>
    public const int MagnifierCssSize = 200;

    private static readonly string[] s_colormaps = ["gray", "inverseGray", "hotIron", "pet", "hotMetalBlue", "pet20Step"];
    private readonly ViewerServices _svc;
    private readonly Lock _gate = new();
    private readonly Viewport[] _viewports = new Viewport[MaxViewports];
    private readonly List<SeriesSource> _series = [];
    private readonly Dictionary<FrameKey, AnnotationDocument> _docs = [];
    private readonly Stack<(FrameKey Key, List<Annotation> Before, List<Annotation> After)> _undo = new();
    private readonly Stack<(FrameKey Key, List<Annotation> Before, List<Annotation> After)> _redo = new();
    private readonly CancellationTokenSource _closed = new();
    private readonly CancellationToken _closedToken;
    private IViewSink? _sink;

    /// <summary>Creates a session (call <see cref="LoadAsync"/> next).</summary>
    /// <param name="id">Random session id.</param>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="services">Services.</param>
    public ViewerSession(string id, string studyInstanceUid, ViewerServices services)
    {
        ArgumentNullException.ThrowIfNull(services);
        Id = id;
        StudyInstanceUid = studyInstanceUid;
        _svc = services;
        _closedToken = _closed.Token;
        for (int i = 0; i < MaxViewports; i++)
        {
            _viewports[i] = new Viewport(i);
        }
        // Open while arriving: images added to this study later (retrieve, receiver, import) join the open series.
        _svc.Store.EventReceived += OnStoreEvent;
    }

    /// <summary>Session id (in the socket URL).</summary>
    public string Id { get; }

    /// <summary>Study.</summary>
    public string StudyInstanceUid { get; }

    /// <summary>Study row (patient bar), once loaded.</summary>
    public StudyRow? Study { get; private set; }

    /// <summary>Layout.</summary>
    public LayoutModel Layout { get; private set; } = new(1, 1);

    /// <summary>Active viewport.</summary>
    public int ActiveViewport { get; private set; }

    /// <summary>Left-button tool.</summary>
    public ViewerTool Tool { get; private set; } = ViewerTool.WindowLevel;

    /// <summary>Privacy mode (F8): all text hidden.</summary>
    public bool Privacy { get; private set; }

    /// <summary>The sync group (W/L, zoom, pan shared by chosen viewports).</summary>
    public SyncGroup Sync { get; private set; } = new(new HashSet<int>(), true, true, true);

    /// <summary>Linked scrolling by frame of reference.</summary>
    public bool LinkScroll { get; private set; }

    /// <summary>Reference lines of the active viewport's slice are drawn on the other viewports.</summary>
    public bool ShowReferenceLines { get; private set; } = true;

    // Cross-reference point (patient space) and its frame of reference, or null.
    private (Vec3 Point, string FrameOfReference)? _cross;

    /// <summary>Device pixel ratio of the browser (true-size zoom).</summary>
    public double DevicePixelRatio { get; private set; } = 1;

    /// <summary>All series headers are read.</summary>
    public bool Loaded { get; private set; }

    /// <summary>Loading problem in plain words, or null.</summary>
    public string? LoadError { get; private set; }

    /// <summary>Last failure of a background task in plain words (shown in the status bar), or null.</summary>
    public string? BackgroundError { get; private set; }

    /// <summary>Background task failures so far (tests and diagnostics).</summary>
    public int BackgroundFailures => Volatile.Read(ref _backgroundFailures);

    private int _backgroundFailures;
    private bool _disposed;

    /// <summary>Last touched (idle clean-up).</summary>
    public DateTime LastActivityUtc { get; private set; } = DateTime.UtcNow;

    /// <summary>A sink is attached.</summary>
    public bool Connected
    {
        get
        {
            lock (_gate)
            {
                return _sink is not null;
            }
        }
    }

    /// <summary>Raised when something the Blazor UI shows changed (layout, tool, active viewport, series, cine, privacy).</summary>
    public event EventHandler? UiChanged;

    /// <summary>Series loaded so far (display order).</summary>
    public IReadOnlyList<SeriesSource> Series
    {
        get
        {
            lock (_gate)
            {
                return [.. _series];
            }
        }
    }

    /// <summary>A viewport.</summary>
    /// <param name="index">Index.</param>
    /// <returns>The viewport.</returns>
    public Viewport GetViewport(int index) => _viewports[Math.Clamp(index, 0, MaxViewports - 1)];

    private UserPrefs Prefs => _svc.Settings.Current;

    /// <summary>
    /// Watches a background task: a failure is logged (exception type and toolkit status only) and
    /// shown in the viewer (status bar, or the viewport when one is given) instead of disappearing.
    /// Cancellation because the session closed is not a failure.
    /// </summary>
    /// <param name="task">The task.</param>
    /// <param name="what">What it does, in plain words ("saving the measurement").</param>
    /// <param name="viewport">Viewport whose picture depends on it, or null.</param>
    public void Observe(Task task, string what, Viewport? viewport = null)
    {
        ArgumentNullException.ThrowIfNull(task);
        if (task.IsCompletedSuccessfully)
        {
            return;
        }
        _ = task.ContinueWith(t => Report(t, what, viewport), CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    private void Report(Task t, string what, Viewport? viewport)
    {
        if (!t.IsFaulted || t.Exception is not AggregateException agg)
        {
            return;
        }
        Exception e = agg.GetBaseException();
        if (e is OperationCanceledException || (e is ObjectDisposedException && _closedToken.IsCancellationRequested))
        {
            return;
        }
        Interlocked.Increment(ref _backgroundFailures);
        AppLog.Error("viewer", what + " failed", e);
        string text = char.ToUpperInvariant(what[0]) + what[1..] + " failed: " + Plain(e) + ".";
        lock (_gate)
        {
            BackgroundError = text;
        }
        if (viewport is not null)
        {
            SendStateOnly(viewport, text);
        }
        RaiseUi();
    }

    /// <summary>Plain words for an exception: the toolkit's status text, else the exception type (never a message that could hold a path or a value).</summary>
    /// <param name="e">Exception.</param>
    /// <returns>Text.</returns>
    public static string Plain(Exception e) => e switch
    {
        TkException tk => tk.Message,
        OutOfMemoryException => "out of memory",
        UnauthorizedAccessException => "access denied",
        IOException => "file system error",
        ObjectDisposedException => "the image was closed",
        null => "unknown error",
        _ => e.GetType().Name,
    };

    /// <summary>Clears the background error shown in the status bar.</summary>
    public void ClearBackgroundError()
    {
        lock (_gate)
        {
            BackgroundError = null;
        }
        RaiseUi();
    }

    private void RaiseUi()
    {
        try
        {
            UiChanged?.Invoke(this, EventArgs.Empty);
        }
#pragma warning disable CA1031 // A failing UI subscriber (closed circuit) must not break the viewer.
        catch (Exception e)
#pragma warning restore CA1031
        {
            AppLog.Warn("viewer", "UI subscriber failed: " + e.GetType().Name);
        }
    }

    // ================================================================== loading

    /// <summary>Reads the study, its series and their headers (visible series first) and fills the viewports.</summary>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Completion.</returns>
    public async Task LoadAsync(CancellationToken cancellationToken = default)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _closedToken);
        CancellationToken ct = linked.Token;
        var failedSeries = new List<string>();
        try
        {
            Study = await _svc.Store.GetStudyAsync(StudyInstanceUid, ct).ConfigureAwait(false);
            IReadOnlyList<SeriesRow> rows = await _svc.Store.GetSeriesAsync(StudyInstanceUid, ct).ConfigureAwait(false);
            var imageRows = rows.Where(r => r.Instances > 0).ToList();
            lock (_gate)
            {
                Layout = LayoutModel.ForSeriesCount(imageRows.Count);
                _svc.Cache.MaxFullHandles = Math.Max(_svc.Cache.MaxFullHandles, Prefs.MaxFullHandlesPerViewport * Layout.Count);
            }
            RaiseUi();
            int visible = Layout.Count;
            // Visible series in parallel on the Decode pool, the rest after them at background priority.
            var tasks = imageRows.Select((row, i) => LoadSeriesAsync(row, i < visible ? WorkPriority.Normal : WorkPriority.Background, ct)).ToArray();
            for (int i = 0; i < tasks.Length; i++)
            {
                IReadOnlyList<SeriesSource> loaded;
                try
                {
                    loaded = await tasks[i].ConfigureAwait(false);
                }
                catch (Exception e) when (e is not OperationCanceledException)
                {
                    // One unreadable series must not stop the others; it is named in the warning.
                    AppLog.Error("viewer", "series load failed", e);
                    failedSeries.Add((imageRows[i].SeriesNumber?.ToString(CultureInfo.InvariantCulture) ?? "-") + " (" + Plain(e) + ")");
                    continue;
                }
                bool keep;
                lock (_gate)
                {
                    keep = !_disposed;
                    if (keep)
                    {
                        _series.AddRange(loaded.Where(s => s.Images.Count > 0));
                        AssignDefaults();
                    }
                }
                if (!keep)
                {
                    DisposeHeaders(loaded);
                    return;
                }
                RaiseUi();
                InvalidateAll();
            }
        }
        catch (OperationCanceledException)
        {
            // Session closed while loading.
        }
#pragma warning disable CA1031 // Any failure must end in a message on screen, never in "Loading..." forever.
        catch (Exception e)
#pragma warning restore CA1031
        {
            lock (_gate)
            {
                LoadError = "The study could not be read: " + Plain(e) + ".";
            }
            AppLog.Error("viewer", "study load failed", e);
        }
        finally
        {
            lock (_gate)
            {
                Loaded = true;
                if (failedSeries.Count > 0)
                {
                    BackgroundError = "Series that could not be read: " + string.Join(", ", failedSeries) + ".";
                }
                if (_series.Count == 0 && LoadError is null && !_disposed)
                {
                    LoadError = failedSeries.Count > 0 ? "The series of this study could not be read." : "This study has no images that can be shown.";
                }
            }
        }
        if (!_closedToken.IsCancellationRequested)
        {
            RaiseUi();
            InvalidateAll();
            // Images that arrived while the headers were being read.
            StartGrowIfPending();
        }
    }

    // ================================================================== open while arriving

    private readonly HashSet<string> _arrived = new(StringComparer.Ordinal);
    private bool _growing;
    private long _grown;

    /// <summary>How many images joined open series after the study was loaded (tests, status).</summary>
    public long ImagesArrived => Interlocked.Read(ref _grown);

    // On the store's event pump: remember the series and leave.
    private void OnStoreEvent(object? sender, StoreEventArgs e)
    {
        StoreEvent ev = e.Event;
        if (ev.Kind != TkStoreEventKind.Added || ev.SeriesInstanceUid.Length == 0 || !string.Equals(ev.StudyInstanceUid, StudyInstanceUid, StringComparison.Ordinal))
        {
            return;
        }
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }
            _arrived.Add(ev.SeriesInstanceUid);
        }
        StartGrowIfPending();
    }

    private void StartGrowIfPending()
    {
        lock (_gate)
        {
            if (_disposed || _growing || !Loaded || _arrived.Count == 0)
            {
                return;
            }
            _growing = true;
        }
        Observe(GrowLoopAsync(), "adding arriving images");
    }

    private async Task GrowLoopAsync()
    {
        try
        {
            while (true)
            {
                // Images arrive in bursts: gather a moment, then read the new headers in one go.
                await Task.Delay(250, _closedToken).ConfigureAwait(false);
                string[] series;
                lock (_gate)
                {
                    series = [.. _arrived];
                    _arrived.Clear();
                    if (series.Length == 0 || _disposed)
                    {
                        _growing = false;
                        return;
                    }
                }
                await GrowAsync(series, _closedToken).ConfigureAwait(false);
            }
        }
        catch
        {
            lock (_gate)
            {
                _growing = false;
            }
            throw;
        }
    }

    private async Task GrowAsync(string[] seriesUids, CancellationToken ct)
    {
        IReadOnlyList<SeriesRow> rows = await _svc.Store.GetSeriesAsync(StudyInstanceUid, ct).ConfigureAwait(false);
        StudyRow? study = Study ?? await _svc.Store.GetStudyAsync(StudyInstanceUid, ct).ConfigureAwait(false);
        bool changed = false;
        foreach (string uid in seriesUids)
        {
            SeriesRow? row = rows.FirstOrDefault(r => r.SeriesInstanceUid == uid);
            if (row is null)
            {
                continue;
            }
            IReadOnlyList<InstanceRow> inst = await _svc.Store.GetInstancesAsync(uid, ct).ConfigureAwait(false);
            ImageInstance[] open;
            lock (_gate)
            {
                open = [.. _series.Where(s => s.SeriesInstanceUid == uid).SelectMany(s => s.Images).Select(i => i.Instance).Distinct()];
            }
            var known = new HashSet<string>(open.Select(i => i.FilePath), StringComparer.Ordinal);
            string[] fresh = [.. inst.Where(i => i.HasPixelData && !known.Contains(i.FilePath)).Select(i => i.FilePath)];
            if (fresh.Length == 0)
            {
                continue;
            }
            IReadOnlyList<SeriesSource> grown = await _svc.Scheduler.RunAsync(WorkPool.Decode, t => SeriesSource.Grow(row.Record, open, fresh, t),
                WorkPriority.Normal, ct).ConfigureAwait(false);
            bool keep;
            lock (_gate)
            {
                keep = !_disposed;
                if (keep)
                {
                    ReplaceSeriesLocked(uid, [.. grown.Where(s => s.Images.Count > 0)]);
                    Study ??= study;
                    if (_series.Count > 0)
                    {
                        LoadError = null;
                    }
                }
            }
            if (!keep)
            {
                // The session closed meanwhile: its own headers are released by Dispose, the new ones here.
                foreach (ImageInstance i in grown.SelectMany(s => s.Images).Select(x => x.Instance).Distinct().Except(open))
                {
                    i.Dispose();
                }
                return;
            }
            Interlocked.Add(ref _grown, fresh.Length);
            changed = true;
        }
        if (changed)
        {
            RaiseUi();
            InvalidateAll();
        }
    }

    // Under the lock: the sources of one series are replaced by its grown form. A viewport showing the series keeps
    // the image it shows (found again by SOP Instance UID and frame), its window, zoom and pan; only its position in
    // the re-sorted stack changes.
    private void ReplaceSeriesLocked(string seriesUid, List<SeriesSource> grown)
    {
        int at = _series.FindIndex(s => s.SeriesInstanceUid == seriesUid);
        var shown = new List<(Viewport Vp, FrameKey? Key, int Stack)>();
        foreach (Viewport vp in _viewports)
        {
            if (vp.Series is { } s && s.SeriesInstanceUid == seriesUid)
            {
                shown.Add((vp, vp.CurrentImage?.Key, s.Stack));
            }
        }
        _series.RemoveAll(s => s.SeriesInstanceUid == seriesUid);
        if (at < 0 || at > _series.Count)
        {
            at = _series.Count;
        }
        _series.InsertRange(at, grown);
        foreach ((Viewport vp, FrameKey? key, int stack) in shown)
        {
            SeriesSource? target = null;
            int index = -1;
            if (key is FrameKey k)
            {
                foreach (SeriesSource g in grown)
                {
                    for (int i = 0; i < g.Images.Count; i++)
                    {
                        if (g.Images[i].Key == k)
                        {
                            (target, index) = (g, i);
                            break;
                        }
                    }
                    if (target is not null)
                    {
                        break;
                    }
                }
            }
            target ??= grown.FirstOrDefault(g => g.Stack == stack) ?? grown.FirstOrDefault();
            bool wasPlaying = vp.Cine.IsPlaying;
            if (target is null)
            {
                StopCineLocked(vp);
                vp.Series = null;
                vp.SeriesIndex = -1;
                continue;
            }
            vp.Series = target;
            vp.ImageIndex = index >= 0 ? index : Math.Clamp(vp.ImageIndex, 0, target.Images.Count - 1);
            // Rendered frames are cached by position in the stack, which has just changed.
            vp.Ring.Reset("");
            if (wasPlaying)
            {
                RestartCineLocked(vp);
            }
        }
        foreach (Viewport vp in _viewports)
        {
            vp.SeriesIndex = vp.Series is null ? -1 : _series.IndexOf(vp.Series);
        }
        AssignDefaults();
    }

    private static void DisposeHeaders(IEnumerable<SeriesSource> series)
    {
        foreach (ImageInstance i in series.SelectMany(s => s.Images).Select(x => x.Instance).Distinct())
        {
            i.Dispose();
        }
    }

    private async Task<IReadOnlyList<SeriesSource>> LoadSeriesAsync(SeriesRow row, WorkPriority priority, CancellationToken ct)
    {
        IReadOnlyList<InstanceRow> inst = await _svc.Store.GetInstancesAsync(row.SeriesInstanceUid, ct).ConfigureAwait(false);
        var paths = inst.Where(i => i.HasPixelData).Select(i => i.FilePath).ToArray();
        return await _svc.Scheduler.RunAsync(WorkPool.Decode, t => SeriesSource.Load(row.Record, paths, t), priority, ct).ConfigureAwait(false);
    }

    // Under the lock: viewports without a series get the next series by order.
    private void AssignDefaults()
    {
        for (int v = 0; v < Layout.Count; v++)
        {
            Viewport vp = _viewports[v];
            if (vp.Series is null && v < _series.Count)
            {
                SetSeriesLocked(vp, v);
            }
        }
    }

    private void SetSeriesLocked(Viewport vp, int seriesIndex)
    {
        StopCineLocked(vp);
        SeriesSource s = _series[seriesIndex];
        vp.Series = s;
        vp.SeriesIndex = seriesIndex;
        vp.ImageIndex = 0;
        vp.State = new DisplayState { Linear = Prefs.LinearInterpolation };
        vp.WindowName = "";
        vp.Drawing = null;
        vp.Drag = null;
        vp.Cine.Fps = s.DefaultFps(Prefs.StackCineFps, Prefs.MultiFrameCineFps);
    }

    // ================================================================== sink

    /// <summary>Attaches the socket; every viewport is sent again.</summary>
    /// <param name="sink">Sink.</param>
    public void Attach(IViewSink sink)
    {
        lock (_gate)
        {
            _sink = sink;
            Touch();
        }
        foreach (Viewport vp in _viewports)
        {
            if (vp.Flow.Reset())
            {
                StartRender(vp);
            }
            if (vp.MagnifierFlow.Reset())
            {
                Observe(MagnifierAsync(vp), "the magnifier");
            }
        }
    }

    /// <summary>Detaches the socket (cine stops).</summary>
    /// <param name="sink">The sink that is going away.</param>
    public void Detach(IViewSink sink)
    {
        lock (_gate)
        {
            if (ReferenceEquals(_sink, sink))
            {
                _sink = null;
                foreach (Viewport vp in _viewports)
                {
                    StopCineLocked(vp);
                }
            }
        }
    }

    private void Touch() => LastActivityUtc = DateTime.UtcNow;

    // ================================================================== rendering

    /// <summary>Marks a viewport changed: renders now, or after the frame in flight is acknowledged.</summary>
    /// <param name="vp">Viewport.</param>
    public void Invalidate(Viewport vp)
    {
        ArgumentNullException.ThrowIfNull(vp);
        if (vp.Flow.Request())
        {
            StartRender(vp);
        }
    }

    private void InvalidateAll()
    {
        int n;
        lock (_gate)
        {
            n = Layout.Count;
        }
        for (int i = 0; i < n; i++)
        {
            Invalidate(_viewports[i]);
        }
    }

    /// <summary>The browser acknowledged a frame.</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seq">Sequence number.</param>
    public void Ack(int viewport, uint seq)
    {
        if (viewport >= MagnifierViewportBase && viewport < MagnifierViewportBase + MaxViewports)
        {
            Viewport mv = _viewports[viewport - MagnifierViewportBase];
            if (mv.MagnifierFlow.Ack(seq))
            {
                Observe(MagnifierAsync(mv), "the magnifier");
            }
            return;
        }
        if (viewport < 0 || viewport >= MaxViewports)
        {
            return;
        }
        Viewport vp = _viewports[viewport];
        if (vp.Flow.Ack(seq))
        {
            StartRender(vp);
        }
    }

    private void StartRender(Viewport vp) => Observe(RenderAsync(vp), "showing the image", vp);

    private sealed record RenderJob(SeriesImage? Image, DisplayState State, int Width, int Height, int ImageIndex, int ImageCount, bool Visible,
                                    bool Playing, string? Message);

    private async Task RenderAsync(Viewport vp)
    {
        while (true)
        {
            bool sent = false;
            try
            {
                sent = await RenderOnceAsync(vp).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
#pragma warning disable CA1031 // A failed render must leave the viewport usable; the reason is shown in the viewport.
            catch (Exception e)
#pragma warning restore CA1031
            {
                AppLog.Error("render", "render failed", e);
                SendStateOnly(vp, "This image could not be shown: " + (e is TkException tk ? tk.Message : e.GetType().Name));
            }
            if (sent || !vp.Flow.Abandon())
            {
                return;
            }
        }
    }

    private RenderJob Snapshot(Viewport vp)
    {
        lock (_gate)
        {
            bool visible = vp.Index < Layout.Count && _sink is not null;
            string? msg = null;
            if (vp.Series is null)
            {
                msg = !Loaded ? "Loading..." : LoadError ?? (_series.Count > 0 ? "Drop a series here" : "No images");
            }
            return new RenderJob(vp.CurrentImage, vp.State, vp.Width, vp.Height, vp.ImageIndex, vp.Series?.Images.Count ?? 0, visible, vp.Cine.IsPlaying, msg);
        }
    }

    // Returns true when a frame was sent (the flow then waits for its ack).
    private async Task<bool> RenderOnceAsync(Viewport vp)
    {
        RenderJob job = Snapshot(vp);
        if (!job.Visible || job.Width <= 0 || job.Height <= 0)
        {
            return false;
        }
        if (job.Image is null)
        {
            SendStateOnly(vp, job.Message);
            return false;
        }
        using FrameLease lease = await _svc.Cache.GetFrameAsync(job.Image, WorkPriority.Normal, _closedToken).ConfigureAwait(false);
        TkFrame frame = lease.Frame;
        AnnotationDocument doc = await GetDocumentAsync(job.Image.Key).ConfigureAwait(false);
        WarmPlanes(vp);
        IViewSink? sink;
        lock (_gate)
        {
            sink = _sink;
        }
        if (sink is null)
        {
            return false;
        }
        bool gray = frame.Info.Monochrome && job.State.Colormap == TkColormap.Gray;
        RenderSpec spec = ResolveSpec(vp, frame, job);
        int pixelBytes = job.Width * job.Height * (gray ? 1 : 4);
        int header = sink.FrameHeaderBytes;
        string ringKey = RingKey(spec, job);
        byte[] buffer = ArrayPool<byte>.Shared.Rent(header + pixelBytes);
        bool handedOver = false;
        try
        {
            bool fromRing;
            lock (_gate)
            {
                vp.Ring.Budget = (long)Prefs.CineCacheMegabytes * 1024 * 1024;
                fromRing = job.Playing && vp.Ring.TryGet(ringKey, job.ImageIndex, out byte[] cached) && cached.Length == pixelBytes
                           && CopyInto(cached, buffer, header);
            }
            if (!fromRing)
            {
                TkError st = await _svc.Scheduler.RunAsync(WorkPool.Interactive,
                    _ => frame.TryRender(buffer.AsSpan(header, pixelBytes), spec, out int _), _closedToken).ConfigureAwait(false);
                if (st != TkError.None)
                {
                    throw new TkException(st);
                }
                if (job.Playing)
                {
                    lock (_gate)
                    {
                        vp.Ring.Put(ringKey, job.ImageIndex, buffer.AsSpan(header, pixelBytes));
                    }
                }
            }
            var transform = new ScreenImageTransform(frame.Info.Columns, frame.Info.Rows, job.Width, job.Height, spec.Zoom, spec.PanX, spec.PanY,
                spec.Rotation, spec.FlipHorizontal, spec.FlipVertical);
            ViewportStateMessage state;
            uint seq;
            lock (_gate)
            {
                vp.Transform = transform;
                vp.FrameInfo = frame.Info;
                state = BuildStateLocked(vp, job, frame, spec, transform, doc);
                seq = vp.Flow.BeginSend();
                vp.FramesSent++;
                if (job.Playing)
                {
                    vp.Cine.FramesShown++;
                }
            }
            sink.SendFrame(vp.Index, seq, gray ? FramePixelFormat.Gray8 : FramePixelFormat.Rgba, job.Width, job.Height, buffer, header + pixelBytes);
            handedOver = true;
            sink.SendState(vp.Index, seq, state);
        }
        finally
        {
            if (!handedOver)
            {
                ArrayPool<byte>.Shared.Return(buffer);
            }
        }
        if (!job.Playing)
        {
            Prefetch(vp, job);
        }
        return true;
    }

    private static bool CopyInto(byte[] src, byte[] dst, int offset)
    {
        src.CopyTo(dst.AsSpan(offset));
        return true;
    }

    private static string RingKey(RenderSpec s, RenderJob j) => string.Create(CultureInfo.InvariantCulture,
        $"{j.Image?.Instance.SopInstanceUid}|{j.Width}x{j.Height}|{s.Zoom}|{s.PanX}|{s.PanY}|{s.Rotation}|{s.FlipHorizontal}|{s.FlipVertical}|{s.Voi}|{s.VoiIndex}|{s.WindowCenter}|{s.WindowWidth}|{s.Invert}|{s.NoOverlays}|{s.Linear}|{s.Colormap}");

    private RenderSpec ResolveSpec(Viewport vp, TkFrame frame, RenderJob job)
    {
        DisplayState st = job.State;
        double c, w;
        string name;
        if (st.Voi == TkVoiSource.File && frame.Voi.Count > 0)
        {
            TkVoi v = frame.Voi[Math.Clamp(st.VoiIndex, 0, frame.Voi.Count - 1)];
            (c, w, name) = (v.Center, v.Width, v.IsLut ? "VOI LUT" : v.Explanation);
        }
        else if (st.Voi == TkVoiSource.Explicit)
        {
            (c, w, name) = (st.WindowCenter, st.WindowWidth, vp.WindowName);
        }
        else
        {
            (c, w, name) = ((frame.Info.MinValue + frame.Info.MaxValue) / 2, Math.Max(1, frame.Info.MaxValue - frame.Info.MinValue), "full range");
        }
        lock (_gate)
        {
            vp.ShownWindow = frame.Info.Monochrome ? (c, w) : null;
            if (st.Voi != TkVoiSource.Explicit)
            {
                vp.WindowName = name;
            }
        }
        RenderSpec spec = st.ToSpec(job.Width, job.Height, frame.Info.Monochrome && st.Colormap == TkColormap.Gray);
        return st.Voi == TkVoiSource.File && frame.Voi.Count == 0 ? spec with { Voi = TkVoiSource.FullRange } : spec;
    }

    private void Prefetch(Viewport vp, RenderJob job)
    {
        SeriesSource? s;
        lock (_gate)
        {
            s = vp.Series;
        }
        if (s is null)
        {
            return;
        }
        var order = new List<SeriesImage>();
        int i = job.ImageIndex;
        for (int d = 1; d <= 10; d++)
        {
            if (i + d < s.Images.Count)
            {
                order.Add(s.Images[i + d]);
            }
            if (i - d >= 0)
            {
                order.Add(s.Images[i - d]);
            }
        }
        // Then the rest of the visible series, as long as it fits half the decode budget.
        long perFrame = Math.Max(1, (long)s.Images[0].Instance.Header.GetDouble(Tags.Rows).GetValueOrDefault(512)
                                    * (long)s.Images[0].Instance.Header.GetDouble(Tags.Columns).GetValueOrDefault(512) * 2);
        if (perFrame * s.Images.Count < _svc.Cache.MaxDecodedBytes / 2)
        {
            for (int d = 11; d < s.Images.Count; d++)
            {
                if (i + d < s.Images.Count)
                {
                    order.Add(s.Images[i + d]);
                }
                if (i - d >= 0)
                {
                    order.Add(s.Images[i - d]);
                }
            }
        }
        _svc.Cache.Prefetch(order);
    }

    private void SendStateOnly(Viewport vp, string? message)
    {
        IViewSink? sink;
        ViewportStateMessage st;
        uint seq;
        lock (_gate)
        {
            sink = _sink;
            seq = vp.Flow.LastSent;
            st = new ViewportStateMessage
            {
                Message = message,
                Active = vp.Index == ActiveViewport,
                Privacy = Privacy,
                Warnings = vp.Series?.Warnings ?? [],
            };
        }
        sink?.SendState(vp.Index, seq, st);
    }

    // Annotation-only change: no new frame, state for the frame on screen.
    private void SendAnnotationState(Viewport vp)
    {
        IViewSink? sink;
        lock (_gate)
        {
            sink = _sink;
        }
        if (sink is null)
        {
            return;
        }
        SeriesImage? img;
        lock (_gate)
        {
            img = vp.CurrentImage;
        }
        if (img is null)
        {
            return;
        }
        using FrameLease? held = _svc.Cache.TryLease(img.Key);
        if (held is null)
        {
            return;
        }
        TkFrame frame = held.Frame;
        WarmPlanes(vp);
        AnnotationDocument? doc;
        lock (_gate)
        {
            _docs.TryGetValue(img.Key, out doc);
        }
        if (doc is null)
        {
            return;
        }
        ViewportStateMessage st;
        uint seq;
        lock (_gate)
        {
            if (vp.Transform is null)
            {
                return;
            }
            RenderJob job = new(img, vp.State, vp.Width, vp.Height, vp.ImageIndex, vp.Series?.Images.Count ?? 0, true, vp.Cine.IsPlaying, null);
            RenderSpec spec = vp.State.ToSpec(vp.Width, vp.Height, frame.Info.Monochrome && vp.State.Colormap == TkColormap.Gray);
            st = BuildStateLocked(vp, job, frame, spec, vp.Transform, doc);
            seq = vp.Flow.LastSent;
        }
        sink.SendState(vp.Index, seq, st);
    }

    private ViewportStateMessage BuildStateLocked(Viewport vp, RenderJob job, TkFrame frame, RenderSpec spec, ScreenImageTransform t,
                                                  AnnotationDocument doc)
    {
        SeriesImage img = job.Image!;
        var warnings = new List<string>(vp.Series?.Warnings ?? []);
        if (img.Instance.IsLossy)
        {
            warnings.Add("LOSSY: this image was compressed with loss of information.");
        }
        OverlayText? text = null;
        OrientationView? orient = null;
        if (!Privacy)
        {
            var dyn = new OverlayDynamic(job.ImageIndex, job.ImageCount, img.Frame, vp.ShownWindow?.Center, vp.ShownWindow?.Width, vp.WindowName,
                t.Zoom, spec.Invert, job.Playing ? vp.Cine.Fps : null);
            text = OverlayTextBuilder.Build(img.Instance, dyn, OverlayTextBuilder.TemplateFor(Prefs, img.Instance.Modality));
            orient = Orientation(img, spec, t);
        }
        var views = new List<AnnotationView>();
        foreach (Annotation a in doc.Annotations.Append(vp.Drawing).OfType<Annotation>())
        {
            (double[] pts, bool closed) = MeasureService.Outline(a, t);
            bool drawing = ReferenceEquals(a, vp.Drawing);
            views.Add(new AnnotationView(a.Id, a.Kind.ToString().ToLowerInvariant(), pts, closed && !(drawing && a.Kind == AnnotationKind.Polygon),
                Privacy ? "" : a.Label, a.Spacing, [.. a.Warnings], drawing));
        }
        return new ViewportStateMessage
        {
            ReferenceLines = ReferenceLinesLocked(vp, img, t),
            Menu = MenuLocked(vp, frame.Info.Monochrome),
            TextPrompt = vp.TextPrompt,
            OverlayText = text,
            Orientation = orient,
            Annotations = views,
            Warnings = warnings,
            Cine = vp.Series is { Images.Count: > 1 } ? new CineView(vp.Cine.IsPlaying, Math.Round(vp.Cine.Fps, 2), vp.Cine.Mode == CineMode.Bounce ? "bounce" : "loop") : null,
            Privacy = Privacy,
            Active = vp.Index == ActiveViewport,
            ImageIndex = job.ImageIndex,
            ImageCount = job.ImageCount,
            CalibrationPrompt = vp.CalibrationPrompt,
        };
    }

    // Under the lock. Planes come from the per-instance cache; a plane not cached yet is read from the
    // header (a short toolkit call on already-parsed attributes, no I/O).
    private List<ReferenceLineView> ReferenceLinesLocked(Viewport vp, SeriesImage img, ScreenImageTransform t)
    {
        var lines = new List<ReferenceLineView>();
        ImagePlane? target = img.Plane;
        if (target is null)
        {
            return lines;
        }
        if (ShowReferenceLines && vp.Index != ActiveViewport && _viewports[ActiveViewport].CurrentImage is { } src
            && src.Plane is { } source && PlaneMath.ReferenceLine(source, target) is var (a, b))
        {
            PointD sa = t.ToScreen(a), sb = t.ToScreen(b);
            string label = _viewports[ActiveViewport].Series?.SeriesNumber?.ToString(CultureInfo.InvariantCulture) ?? "";
            lines.Add(new ReferenceLineView("scout", [Math.Round(sa.X, 1), Math.Round(sa.Y, 1), Math.Round(sb.X, 1), Math.Round(sb.Y, 1)], label));
        }
        if (_cross is var (point, forUid) && string.Equals(forUid, target.FrameOfReferenceUid, StringComparison.Ordinal))
        {
            (PointD p, double distance) = target.ToImage(point);
            double tolerance = Math.Max(1.0, 0.6 * Math.Max(target.RowSpacing, target.ColumnSpacing));
            if (Math.Abs(distance) <= Math.Max(tolerance, SliceGap(vp)) && p.X >= 0 && p.Y >= 0 && p.X <= target.Columns && p.Y <= target.Rows)
            {
                PointD sp = t.ToScreen(p);
                lines.Add(new ReferenceLineView("cross", [Math.Round(sp.X, 1), Math.Round(sp.Y, 1)], ""));
            }
        }
        return lines;
    }

    // Half the distance to the neighbouring slice (cross-reference tolerance), from cached planes.
    private static double SliceGap(Viewport vp)
    {
        if (vp.Series is not { Images.Count: > 1 } s)
        {
            return 0;
        }
        int i = Math.Clamp(vp.ImageIndex, 0, s.Images.Count - 1), j = i + 1 < s.Images.Count ? i + 1 : i - 1;
        ImagePlane? a = s.Images[i].Plane, b = s.Images[j].Plane;
        return a is null || b is null ? 0 : Math.Abs(a.ToImage(b.FirstPixelCentre).Distance) / 2;
    }

    private ViewportMenu MenuLocked(Viewport vp, bool monochrome)
    {
        IReadOnlyList<Settings.WindowPresetSetting> presets = vp.Series is null ? [] : WindowPresets.For(Prefs, vp.Series.Modality);
        return new ViewportMenu([.. presets.Select(p => p.Name)], ColormapName(vp.State.Colormap), monochrome ? s_colormaps : [],
            vp.State.ShowOverlays, Sync.Members.Contains(vp.Index), LinkScroll, ShowReferenceLines);
    }

    /// <summary>Wire name of a colour map.</summary>
    /// <param name="c">Colour map.</param>
    /// <returns>The name.</returns>
    public static string ColormapName(TkColormap c) => s_colormaps[Math.Clamp((int)c, 0, s_colormaps.Length - 1)];

    /// <summary>Colour map from its wire name.</summary>
    /// <param name="name">Name.</param>
    /// <param name="colormap">Result.</param>
    /// <returns>True when known.</returns>
    public static bool TryParseColormap(string? name, out TkColormap colormap)
    {
        int i = Array.FindIndex(s_colormaps, n => string.Equals(n, name, StringComparison.OrdinalIgnoreCase));
        colormap = i < 0 ? TkColormap.Gray : (TkColormap)i;
        return i >= 0;
    }

    // Reads (and caches) the planes the state message needs, outside the lock.
    private void WarmPlanes(Viewport vp)
    {
        SeriesImage? own, active, neighbour = null;
        lock (_gate)
        {
            own = vp.CurrentImage;
            active = _viewports[ActiveViewport].CurrentImage;
            if (vp.Series is { Images.Count: > 1 } s)
            {
                int i = Math.Clamp(vp.ImageIndex, 0, s.Images.Count - 1);
                neighbour = s.Images[i + 1 < s.Images.Count ? i + 1 : i - 1];
            }
        }
        _ = own?.Plane;
        _ = active?.Plane;
        _ = neighbour?.Plane;
    }

    private static OrientationView? Orientation(SeriesImage img, RenderSpec spec, ScreenImageTransform t)
    {
        if (spec.Rotation % 90 != 0)
        {
            return null;
        }
        TkOrientation? o = img.Instance.Header.GetOrientation(img.Frame);
        if (o is null)
        {
            return null;
        }
        PointD c = t.ToScreen(new PointD(0, 0));
        PointD rx = t.ToScreen(new PointD(1, 0));
        PointD dy = t.ToScreen(new PointD(0, 1));
        // Screen direction of the image's +x (towards o.Right) and +y (towards o.Bottom).
        (double X, double Y) ux = (rx.X - c.X, rx.Y - c.Y), uy = (dy.X - c.X, dy.Y - c.Y);
        string At(double sx, double sy)
        {
            double dx = (ux.X * sx) + (ux.Y * sy), dyv = (uy.X * sx) + (uy.Y * sy);
            return Math.Abs(dx) >= Math.Abs(dyv) ? (dx > 0 ? o.Right : o.Left) : (dyv > 0 ? o.Bottom : o.Top);
        }
        return new OrientationView(At(-1, 0), At(1, 0), At(0, -1), At(0, 1));
    }

    private async Task<AnnotationDocument> GetDocumentAsync(FrameKey key)
    {
        lock (_gate)
        {
            if (_docs.TryGetValue(key, out AnnotationDocument? d))
            {
                return d;
            }
        }
        AnnotationDocument loaded = await _svc.Scheduler.RunAsync(WorkPool.StoreRead, _ => _svc.Annotations.Load(key.SopInstanceUid, key.Frame),
            _closedToken).ConfigureAwait(false);
        lock (_gate)
        {
            if (_docs.TryGetValue(key, out AnnotationDocument? d))
            {
                return d;
            }
            _docs[key] = loaded;
        }
        if (loaded.Annotations.Count > 0)
        {
            await RecomputeAsync(key, loaded.Annotations).ConfigureAwait(false);
        }
        return loaded;
    }

    // ================================================================== input

    /// <summary>Handles a browser input event.</summary>
    /// <param name="e">Event.</param>
    public void OnInput(InputEvent e)
    {
        if (e.Viewport < 0 || e.Viewport >= MaxViewports)
        {
            return;
        }
        Viewport vp = _viewports[e.Viewport];
        bool render = false, annotations = false, ui = false, roi = false, indexChanged, magnify;
        Annotation? committed = null;
        List<Viewport>? synced;
        PointD? cross = null;
        lock (_gate)
        {
            Touch();
            DisplayState before = vp.State;
            int beforeIndex = vp.ImageIndex;
            switch (e.Kind)
            {
                case InputKind.Resize:
                    if (e.Width != vp.Width || e.Height != vp.Height)
                    {
                        vp.Width = Math.Clamp(e.Width, 0, 8192);
                        vp.Height = Math.Clamp(e.Height, 0, 8192);
                        render = true;
                    }
                    break;
                case InputKind.Down:
                    if (ActiveViewport != vp.Index)
                    {
                        ActiveViewport = vp.Index;
                        ui = true;
                        render = true;
                        InvalidateLater(_viewports.Where(v => v.Index != vp.Index && v.Index < Layout.Count));
                    }
                    OnDownLocked(vp, e, ref render, ref annotations, ref committed);
                    break;
                case InputKind.Move:
                    OnMoveLocked(vp, e, ref render, ref annotations, ref roi);
                    break;
                case InputKind.Up:
                    OnUpLocked(vp, e, ref render, ref annotations, ref committed);
                    break;
                case InputKind.Wheel:
                    OnWheelLocked(vp, e, ref render);
                    break;
                case InputKind.Key:
                    OnKeyLocked(vp, e, ref render, ref annotations, ref ui, ref committed);
                    break;
            }
            if (vp.Drag is { Tool: ViewerTool.CrossReference } && e.Kind is InputKind.Down or InputKind.Move && ImagePoint(vp, e.X, e.Y) is PointD cp)
            {
                cross = cp;
            }
            synced = PropagateSyncLocked(vp, before);
            indexChanged = vp.ImageIndex != beforeIndex;
            magnify = vp.Magnifier is not null && e.Kind is InputKind.Down or InputKind.Move;
        }
        Finish(vp, render, annotations, ui, roi, committed);
        synced?.ForEach(Invalidate);
        if (indexChanged)
        {
            AfterSliceChange(vp);
        }
        if (magnify)
        {
            InvalidateMagnifier(vp);
        }
        if (cross is PointD point)
        {
            Observe(CrossReferenceAsync(vp, point), "cross-reference");
        }
    }

    // ================================================================== sync group, linked scrolling, cross-reference

    // Under the lock: copies the properties the sync group shares from src to the other members.
    private List<Viewport>? PropagateSyncLocked(Viewport src, DisplayState before)
    {
        DisplayState now = src.State;
        if (ReferenceEquals(before, now) || !Sync.Has(src.Index))
        {
            return null;
        }
        bool wl = Sync.WindowLevel && (before.Voi != now.Voi || before.VoiIndex != now.VoiIndex || before.WindowCenter != now.WindowCenter
                                       || before.WindowWidth != now.WindowWidth);
        bool zoom = Sync.Zoom && before.Zoom != now.Zoom;
        bool pan = Sync.Pan && (before.PanX != now.PanX || before.PanY != now.PanY);
        if (!wl && !zoom && !pan)
        {
            return null;
        }
        var changed = new List<Viewport>();
        foreach (int i in Sync.Members)
        {
            if (i == src.Index || i >= Layout.Count || _viewports[i].Series is null)
            {
                continue;
            }
            Viewport v = _viewports[i];
            DisplayState st = v.State;
            if (wl)
            {
                st = st with { Voi = now.Voi, VoiIndex = now.VoiIndex, WindowCenter = now.WindowCenter, WindowWidth = now.WindowWidth };
                v.WindowName = src.WindowName;
            }
            if (zoom)
            {
                st = st with { Zoom = now.Zoom };
            }
            if (pan)
            {
                st = st with { PanX = now.PanX, PanY = now.PanY };
            }
            if (st != v.State)
            {
                v.State = st;
                changed.Add(v);
            }
        }
        return changed;
    }

    // The slice of vp changed: other viewports show its new reference line; linked viewports follow.
    private void AfterSliceChange(Viewport vp)
    {
        bool active, link;
        Viewport[] others;
        lock (_gate)
        {
            active = vp.Index == ActiveViewport;
            link = LinkScroll;
            others = [.. _viewports.Take(Layout.Count).Where(v => v.Index != vp.Index && v.Series is not null)];
        }
        if (link)
        {
            Observe(LinkedScrollAsync(vp), "linked scrolling");
        }
        else if (active)
        {
            foreach (Viewport o in others)
            {
                SendAnnotationState(o);
            }
        }
    }

    private async Task LinkedScrollAsync(Viewport src)
    {
        SeriesImage? img;
        (Viewport V, SeriesSource S)[] targets;
        lock (_gate)
        {
            img = src.CurrentImage;
            targets = [.. _viewports.Take(Layout.Count).Where(v => v.Index != src.Index && v.Series is not null).Select(v => (v, v.Series!))];
        }
        if (img is null)
        {
            return;
        }
        var moves = await _svc.Scheduler.RunAsync(WorkPool.Interactive, _ =>
        {
            var list = new List<(Viewport V, SeriesSource S, int Index)>();
            if (img.Plane is not { } plane || plane.FrameOfReferenceUid.Length == 0)
            {
                return list;
            }
            foreach ((Viewport v, SeriesSource s) in targets)
            {
                ImagePlane?[] planes = [.. s.Images.Select(i => i.Plane)];
                ImagePlane? any = planes.FirstOrDefault(p => p is not null);
                // Only roughly parallel stacks scroll together; reference lines cover the others.
                if (any is null || !plane.SameFrameOfReference(any) || plane.Parallelism(any) < 0.9)
                {
                    continue;
                }
                int idx = PlaneMath.NearestPlane(planes, plane.FrameOfReferenceUid, plane.Centre);
                if (idx >= 0)
                {
                    list.Add((v, s, idx));
                }
            }
            return list;
        }, _closedToken).ConfigureAwait(false);
        var redraw = new List<Viewport>();
        lock (_gate)
        {
            foreach ((Viewport v, SeriesSource s, int idx) in moves)
            {
                if (ReferenceEquals(v.Series, s) && v.ImageIndex != idx)
                {
                    v.ImageIndex = idx;
                    v.Drawing = null;
                    redraw.Add(v);
                }
            }
            redraw.AddRange(targets.Select(t => t.V).Where(v => !redraw.Contains(v)));
        }
        foreach (Viewport v in redraw)
        {
            Invalidate(v);
        }
    }

    private async Task CrossReferenceAsync(Viewport src, PointD imagePoint)
    {
        SeriesImage? img;
        lock (_gate)
        {
            img = src.CurrentImage;
        }
        if (img is null)
        {
            return;
        }
        ImagePlane? plane = await _svc.Scheduler.RunAsync(WorkPool.Interactive, _ => img.Plane, _closedToken).ConfigureAwait(false);
        if (plane is null || plane.FrameOfReferenceUid.Length == 0)
        {
            return;
        }
        Vec3 point = plane.ToPatient(imagePoint);
        (Viewport V, SeriesSource S)[] targets;
        lock (_gate)
        {
            _cross = (point, plane.FrameOfReferenceUid);
            targets = [.. _viewports.Take(Layout.Count).Where(v => v.Index != src.Index && v.Series is not null).Select(v => (v, v.Series!))];
        }
        var moves = await _svc.Scheduler.RunAsync(WorkPool.Interactive, _ =>
        {
            var list = new List<(Viewport V, SeriesSource S, int Index)>();
            foreach ((Viewport v, SeriesSource s) in targets)
            {
                // Any orientation: the slice through the point (within 10 mm), if the stack reaches it.
                int idx = PlaneMath.NearestPlane([.. s.Images.Select(i => i.Plane)], plane.FrameOfReferenceUid, point, 10.0);
                if (idx >= 0)
                {
                    list.Add((v, s, idx));
                }
            }
            return list;
        }, _closedToken).ConfigureAwait(false);
        lock (_gate)
        {
            foreach ((Viewport v, SeriesSource s, int idx) in moves)
            {
                if (ReferenceEquals(v.Series, s))
                {
                    v.ImageIndex = idx;
                    v.Drawing = null;
                }
            }
        }
        InvalidateAll();
    }

    // ================================================================== magnifier

    private void InvalidateMagnifier(Viewport vp)
    {
        if (vp.MagnifierFlow.Request())
        {
            Observe(MagnifierAsync(vp), "the magnifier");
        }
    }

    private async Task MagnifierAsync(Viewport vp)
    {
        while (true)
        {
            bool sent = false;
            try
            {
                sent = await MagnifierOnceAsync(vp).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
#pragma warning disable CA1031 // The magnifier is a convenience: a failed render must not break the viewport.
            catch (Exception e)
#pragma warning restore CA1031
            {
                AppLog.Error("render", "magnifier render failed", e);
            }
            if (sent || !vp.MagnifierFlow.Abandon())
            {
                return;
            }
        }
    }

    private async Task<bool> MagnifierOnceAsync(Viewport vp)
    {
        SeriesImage? img;
        DisplayState st;
        (int X, int Y)? at;
        ScreenImageTransform? t;
        IViewSink? sink;
        double dpr;
        lock (_gate)
        {
            (img, st, at, t, sink, dpr) = (vp.CurrentImage, vp.State, vp.Magnifier, vp.Transform, _sink, DevicePixelRatio);
        }
        if (img is null || at is not var (mx, my) || t is null || sink is null)
        {
            return false;
        }
        using FrameLease lease = await _svc.Cache.GetFrameAsync(img, WorkPriority.Normal, _closedToken).ConfigureAwait(false);
        TkFrame frame = lease.Frame;
        RenderSpec spec = MagnifierSpec(frame.Info, st, t, mx, my, dpr, frame.Voi.Count);
        int pixelBytes = spec.Width * spec.Height * (spec.Gray ? 1 : 4);
        int header = sink.FrameHeaderBytes;
        byte[] buffer = ArrayPool<byte>.Shared.Rent(header + pixelBytes);
        bool handedOver = false;
        try
        {
            TkError err = await _svc.Scheduler.RunAsync(WorkPool.Interactive,
                _ => frame.TryRender(buffer.AsSpan(header, pixelBytes), spec, out int _), _closedToken).ConfigureAwait(false);
            if (err != TkError.None)
            {
                throw new TkException(err);
            }
            uint seq = vp.MagnifierFlow.BeginSend();
            sink.SendFrame(MagnifierViewportBase + vp.Index, seq, spec.Gray ? FramePixelFormat.Gray8 : FramePixelFormat.Rgba, spec.Width, spec.Height,
                buffer, header + pixelBytes);
            handedOver = true;
        }
        finally
        {
            if (!handedOver)
            {
                ArrayPool<byte>.Shared.Return(buffer);
            }
        }
        return true;
    }

    /// <summary>
    /// The magnifier's render: a small square viewport at <see cref="MagnifierFactor"/> times the
    /// viewport's zoom, panned so the image point under (x, y) is at its centre; window, colour map,
    /// rotation and flips as in the viewport.
    /// </summary>
    /// <param name="info">Frame.</param>
    /// <param name="st">Viewport state.</param>
    /// <param name="t">Viewport transform of the frame on screen.</param>
    /// <param name="x">Pointer x (viewport pixels).</param>
    /// <param name="y">Pointer y.</param>
    /// <param name="dpr">Device pixel ratio.</param>
    /// <param name="fileVoiCount">Windows stored in the file.</param>
    /// <returns>The spec.</returns>
    public static RenderSpec MagnifierSpec(TkFrameInfo info, DisplayState st, ScreenImageTransform t, int x, int y, double dpr, int fileVoiCount)
    {
        ArgumentNullException.ThrowIfNull(info);
        ArgumentNullException.ThrowIfNull(st);
        ArgumentNullException.ThrowIfNull(t);
        int size = Math.Clamp((int)Math.Round(MagnifierCssSize * dpr), 64, 1024);
        double zoom = Math.Clamp(t.Zoom * MagnifierFactor, 0.05, 256);
        PointD ip = t.ToImage(new PointD(x, y));
        var centred = new ScreenImageTransform(info.Columns, info.Rows, size, size, zoom, 0, 0, st.Rotation, st.FlipHorizontal, st.FlipVertical);
        PointD sp = centred.ToScreen(ip);
        RenderSpec spec = st.ToSpec(size, size, info.Monochrome && st.Colormap == TkColormap.Gray) with
        {
            Zoom = zoom,
            PanX = (size / 2.0) - sp.X,
            PanY = (size / 2.0) - sp.Y,
        };
        return st.Voi == TkVoiSource.File && fileVoiCount == 0 ? spec with { Voi = TkVoiSource.FullRange } : spec;
    }

    private void InvalidateLater(IEnumerable<Viewport> vps)
    {
        foreach (Viewport v in vps.ToArray())
        {
            Observe(Task.Run(() => Invalidate(v)), "redrawing a viewport");
        }
    }

    private void Finish(Viewport vp, bool render, bool annotations, bool ui, bool roi, Annotation? committed)
    {
        if (committed is not null)
        {
            Observe(CommitAsync(vp, committed), "saving the measurement");
        }
        else if (roi)
        {
            Observe(UpdateDrawingAsync(vp), "measuring");
        }
        if (render)
        {
            Invalidate(vp);
        }
        else if (annotations)
        {
            SendAnnotationState(vp);
        }
        if (ui)
        {
            RaiseUi();
        }
    }

    private static PointD? ImagePoint(Viewport vp, int x, int y) => vp.Transform?.ToImage(new PointD(x, y));

    private static AnnotationKind? KindOf(ViewerTool t) => t switch
    {
        ViewerTool.Length => AnnotationKind.Length,
        ViewerTool.Polyline => AnnotationKind.Polyline,
        ViewerTool.Angle => AnnotationKind.Angle,
        ViewerTool.Cobb => AnnotationKind.Cobb,
        ViewerTool.Ellipse => AnnotationKind.Ellipse,
        ViewerTool.Circle => AnnotationKind.Circle,
        ViewerTool.Rectangle => AnnotationKind.Rectangle,
        ViewerTool.Polygon => AnnotationKind.Polygon,
        ViewerTool.Freehand => AnnotationKind.Freehand,
        ViewerTool.Probe => AnnotationKind.Probe,
        ViewerTool.Calibrate => AnnotationKind.Calibration,
        ViewerTool.Arrow => AnnotationKind.Arrow,
        ViewerTool.Text => AnnotationKind.Text,
        _ => null,
    };

    private static int PointsNeeded(AnnotationKind k) => k switch
    {
        AnnotationKind.Angle => 3,
        AnnotationKind.Cobb => 4,
        AnnotationKind.Probe => 1,
        AnnotationKind.Polygon or AnnotationKind.Polyline => int.MaxValue,
        _ => 2,
    };

    private void OnDownLocked(Viewport vp, InputEvent e, ref bool render, ref bool annotations, ref Annotation? committed)
    {
        int button = (e.Buttons & 1) != 0 ? 1 : (e.Buttons & 4) != 0 ? 4 : (e.Buttons & 2) != 0 ? 2 : 1;
        ViewerTool tool = button switch
        {
            2 => ViewerTool.WindowLevel,
            4 => ViewerTool.Pan,
            _ => (e.Modifiers & InputModifiers.Control) != 0 && Tool == ViewerTool.WindowLevel ? ViewerTool.Zoom : Tool,
        };
        (double c, double w) = vp.ShownWindow ?? (vp.State.WindowCenter, vp.State.WindowWidth);
        vp.Drag = new DragState
        {
            Button = button,
            StartX = e.X,
            StartY = e.Y,
            LastX = e.X,
            LastY = e.Y,
            Tool = tool,
            StartState = vp.State,
            StartCenter = c,
            StartWidth = w,
            StartZoom = vp.Transform?.Zoom ?? 1,
            StartIndex = vp.ImageIndex,
        };
        if (tool == ViewerTool.Magnify)
        {
            vp.Magnifier = (e.X, e.Y);
            return;
        }
        if (KindOf(tool) is not AnnotationKind kind || ImagePoint(vp, e.X, e.Y) is not PointD p)
        {
            return;
        }
        if (kind == AnnotationKind.Text)
        {
            // One click places the label; the browser then asks for the text (TextPrompt).
            var label = new Annotation { Kind = AnnotationKind.Text };
            label.Add(p);
            committed = label;
            return;
        }
        if (vp.Drawing is { } d && d.Kind == kind)
        {
            if (kind == AnnotationKind.Polygon && d.Count >= 4 && NearFirst(vp, d, e))
            {
                d.Points.RemoveAt(d.Points.Count - 1);
                d.Points.RemoveAt(d.Points.Count - 1);
                committed = d;
                vp.Drawing = null;
                return;
            }
            d.SetLast(p);
            annotations = true;
            return;
        }
        var a = new Annotation { Kind = kind };
        a.Add(p);
        if (kind != AnnotationKind.Probe)
        {
            a.Add(p);
        }
        vp.Drawing = a;
        annotations = true;
        _ = render;
    }

    private static bool NearFirst(Viewport vp, Annotation d, InputEvent e)
    {
        if (vp.Transform is null)
        {
            return false;
        }
        PointD f = vp.Transform.ToScreen(d.At(0));
        return Math.Abs(f.X - e.X) < 10 && Math.Abs(f.Y - e.Y) < 10;
    }

    private static void OnMoveLocked(Viewport vp, InputEvent e, ref bool render, ref bool annotations, ref bool roi)
    {
        if (vp.Drawing is { } d && ImagePoint(vp, e.X, e.Y) is PointD p)
        {
            if (d.Kind == AnnotationKind.Freehand && vp.Drag is not null)
            {
                PointD last = d.At(d.Count - 1);
                if (Math.Abs(last.X - p.X) + Math.Abs(last.Y - p.Y) >= 1)
                {
                    d.Add(p);
                }
            }
            else
            {
                d.SetLast(p);
            }
            annotations = true;
            if (IsRoi(d.Kind) && (DateTime.UtcNow - vp.LastRoiUpdate).TotalMilliseconds >= 100)
            {
                vp.LastRoiUpdate = DateTime.UtcNow;
                roi = true;
            }
            return;
        }
        if (vp.Drag is not { } drag || e.Buttons == 0)
        {
            return;
        }
        if (drag.Tool == ViewerTool.Magnify && vp.Magnifier is not null)
        {
            vp.Magnifier = (e.X, e.Y);
            return;
        }
        int dx = e.X - drag.StartX, dy = e.Y - drag.StartY;
        int sx = e.X - drag.LastX, sy = e.Y - drag.LastY;
        drag.LastX = e.X;
        drag.LastY = e.Y;
        switch (drag.Tool)
        {
            case ViewerTool.WindowLevel when vp.FrameInfo is { Monochrome: true } fi:
                {
                    double k = Math.Max(fi.MaxValue - fi.MinValue, 1) / 512.0;
                    vp.State = vp.State with
                    {
                        Voi = TkVoiSource.Explicit,
                        WindowCenter = drag.StartCenter + (dy * k),
                        WindowWidth = Math.Max(1, drag.StartWidth + (dx * k)),
                    };
                    vp.WindowName = "";
                    render = true;
                    break;
                }
            case ViewerTool.Zoom:
                vp.State = vp.State with { Zoom = Math.Clamp(drag.StartZoom * Math.Exp(-dy / 200.0), 0.05, 64) };
                render = true;
                break;
            case ViewerTool.Pan:
                vp.State = vp.State with { PanX = vp.State.PanX + sx, PanY = vp.State.PanY + sy };
                render = true;
                break;
            case ViewerTool.Scroll when vp.Series is { } s:
                {
                    drag.ScrollAccumulator += sy;
                    int step = (int)(drag.ScrollAccumulator / 6);
                    if (step != 0)
                    {
                        drag.ScrollAccumulator -= step * 6;
                        render |= SetIndexLocked(vp, vp.ImageIndex + step, s);
                    }
                    break;
                }
        }
    }

    private static bool IsRoi(AnnotationKind k) => k is AnnotationKind.Ellipse or AnnotationKind.Circle or AnnotationKind.Rectangle
        or AnnotationKind.Polygon or AnnotationKind.Freehand or AnnotationKind.Probe;

    private static void OnUpLocked(Viewport vp, InputEvent e, ref bool render, ref bool annotations, ref Annotation? committed)
    {
        vp.Drag = null;
        vp.Magnifier = null;
        if (vp.Drawing is not { } d || ImagePoint(vp, e.X, e.Y) is not PointD p)
        {
            return;
        }
        if (d.Kind != AnnotationKind.Freehand)
        {
            d.SetLast(p);
        }
        int needed = PointsNeeded(d.Kind);
        if (d.Count >= needed || d.Kind == AnnotationKind.Freehand)
        {
            if (TooSmall(vp, d))
            {
                vp.Drawing = null;
                annotations = true;
                return;
            }
            committed = d;
            vp.Drawing = null;
            return;
        }
        // Multi-click shapes: the next point follows the pointer until the next click.
        d.Add(p);
        annotations = true;
        _ = render;
    }

    private static bool TooSmall(Viewport vp, Annotation d)
    {
        if (d.Kind == AnnotationKind.Probe || vp.Transform is null)
        {
            return false;
        }
        PointD a = vp.Transform.ToScreen(d.At(0));
        double max = 0;
        for (int i = 1; i < d.Count; i++)
        {
            PointD b = vp.Transform.ToScreen(d.At(i));
            max = Math.Max(max, Math.Abs(b.X - a.X) + Math.Abs(b.Y - a.Y));
        }
        return max < 3;
    }

    private static void OnWheelLocked(Viewport vp, InputEvent e, ref bool render)
    {
        if (vp.Series is not { } s || e.Wheel == 0)
        {
            return;
        }
        if ((e.Modifiers & InputModifiers.Control) != 0 && vp.Transform is { } t)
        {
            // Zoom about the pointer: keep the image point under the cursor fixed.
            double z0 = t.Zoom, z1 = Math.Clamp(z0 * (e.Wheel > 0 ? 1 / 1.15 : 1.15), 0.05, 64);
            double cx = (vp.Width / 2.0) + vp.State.PanX, cy = (vp.Height / 2.0) + vp.State.PanY;
            double f = z1 / z0;
            vp.State = vp.State with { Zoom = z1, PanX = vp.State.PanX + ((e.X - cx) * (1 - f)), PanY = vp.State.PanY + ((e.Y - cy) * (1 - f)) };
            render = true;
            return;
        }
        render |= SetIndexLocked(vp, vp.ImageIndex + Math.Sign(e.Wheel), s);
    }

    private static bool SetIndexLocked(Viewport vp, int index, SeriesSource s)
    {
        int i = Math.Clamp(index, 0, s.Images.Count - 1);
        if (i == vp.ImageIndex)
        {
            return false;
        }
        vp.ImageIndex = i;
        vp.Drawing = null;
        return true;
    }

    private void OnKeyLocked(Viewport vp, InputEvent e, ref bool render, ref bool annotations, ref bool ui, ref Annotation? committed)
    {
        bool ctrl = (e.Modifiers & (InputModifiers.Control | InputModifiers.Meta)) != 0;
        switch (e.Key)
        {
            case 87: SetToolLocked(ViewerTool.WindowLevel, ref ui); break;   // W
            case 90 when ctrl: Observe(Task.Run(Undo), "undo"); break;       // Ctrl+Z
            case 89 when ctrl: Observe(Task.Run(Redo), "redo"); break;       // Ctrl+Y
            case 90: SetToolLocked(ViewerTool.Zoom, ref ui); break;          // Z
            case 80: SetToolLocked(ViewerTool.Pan, ref ui); break;           // P
            case 76: SetToolLocked(ViewerTool.Length, ref ui); break;        // L
            case 65: SetToolLocked(ViewerTool.Angle, ref ui); break;         // A
            case 69: SetToolLocked(ViewerTool.Ellipse, ref ui); break;       // E
            case 77: SetToolLocked(ViewerTool.Magnify, ref ui); break;       // M
            case 84: SetToolLocked(ViewerTool.Text, ref ui); break;          // T
            case 88: SetToolLocked(ViewerTool.CrossReference, ref ui); break; // X
            case 82: Apply(vp, s => s with { Rotation = (s.Rotation + 90) % 360 }, ref render); break; // R
            case 72: Apply(vp, s => s with { FlipHorizontal = !s.FlipHorizontal }, ref render); break; // H
            case 86: Apply(vp, s => s with { FlipVertical = !s.FlipVertical }, ref render); break;     // V
            case 73: Apply(vp, s => s with { Invert = !s.Invert }, ref render); break;                 // I
            case 70: Apply(vp, s => s with { Zoom = 0, PanX = 0, PanY = 0 }, ref render); break;       // F
            case 27:                                                                                   // Esc
                if (vp.Drawing is not null)
                {
                    vp.Drawing = null;
                    annotations = true;
                }
                else
                {
                    ResetLocked(vp, ref render);
                }
                break;
            case 13 when vp.Drawing is { Kind: AnnotationKind.Polygon or AnnotationKind.Polyline } d && d.Count >= 3: // Enter ends a polygon
                d.Points.RemoveAt(d.Points.Count - 1);
                d.Points.RemoveAt(d.Points.Count - 1);
                committed = d;
                vp.Drawing = null;
                break;
            case >= 49 and <= 57: ApplyPresetLocked(vp, e.Key - 49, ref render); break;               // 1-9
            case 32: ToggleCineLocked(vp); ui = true; break;                                          // Space
            case 119: Privacy = !Privacy; ui = true; InvalidateLater(_viewports.Take(Layout.Count)); break; // F8
            case 9:                                                                                    // Tab
                ActiveViewport = (ActiveViewport + ((e.Modifiers & InputModifiers.Shift) != 0 ? Layout.Count - 1 : 1)) % Layout.Count;
                ui = true;
                InvalidateLater(_viewports.Take(Layout.Count));
                break;
            case 38 or 37 or 33 when vp.Series is { } s1: render |= SetIndexLocked(vp, vp.ImageIndex - (e.Key == 33 ? 10 : 1), s1); break;
            case 40 or 39 or 34 when vp.Series is { } s2: render |= SetIndexLocked(vp, vp.ImageIndex + (e.Key == 34 ? 10 : 1), s2); break;
            case 36 when vp.Series is { } s3: render |= SetIndexLocked(vp, 0, s3); break;
            case 35 when vp.Series is { } s4: render |= SetIndexLocked(vp, s4.Images.Count - 1, s4); break;
        }
    }

    private void SetToolLocked(ViewerTool t, ref bool ui)
    {
        Tool = t;
        foreach (Viewport v in _viewports)
        {
            v.Drawing = null;
        }
        ui = true;
    }

    private static void Apply(Viewport vp, Func<DisplayState, DisplayState> f, ref bool render)
    {
        vp.State = f(vp.State);
        render = true;
    }

    private void ResetLocked(Viewport vp, ref bool render)
    {
        vp.State = new DisplayState { Linear = Prefs.LinearInterpolation };
        vp.WindowName = "";
        render = true;
    }

    private void ApplyPresetLocked(Viewport vp, int n, ref bool render)
    {
        if (vp.Series is null)
        {
            return;
        }
        IReadOnlyList<Settings.WindowPresetSetting> presets = WindowPresets.For(Prefs, vp.Series.Modality);
        if (n < presets.Count)
        {
            vp.State = vp.State with { Voi = TkVoiSource.Explicit, WindowCenter = presets[n].Center, WindowWidth = presets[n].Width };
            vp.WindowName = presets[n].Name;
            render = true;
        }
        else if (n == presets.Count && vp.FrameInfo is not null)
        {
            // The key after the presets cycles through the file's own windows.
            vp.State = vp.State with { Voi = TkVoiSource.File, VoiIndex = vp.State.Voi == TkVoiSource.File ? vp.State.VoiIndex + 1 : 0 };
            render = true;
        }
    }

    // ================================================================== commands (toolbar and socket)

    /// <summary>Selects the left-button tool.</summary>
    /// <param name="tool">Tool.</param>
    public void SetTool(ViewerTool tool)
    {
        bool ui = false;
        lock (_gate)
        {
            SetToolLocked(tool, ref ui);
        }
        RaiseUi();
        InvalidateAll();
    }

    /// <summary>Changes the layout (1x1 to 4x4); new viewports get the next series.</summary>
    /// <param name="layout">Layout.</param>
    public void SetLayout(LayoutModel layout)
    {
        lock (_gate)
        {
            Layout = LayoutModel.Of(layout.Rows, layout.Columns);
            if (ActiveViewport >= Layout.Count)
            {
                ActiveViewport = 0;
            }
            for (int v = Layout.Count; v < MaxViewports; v++)
            {
                StopCineLocked(_viewports[v]);
            }
            _svc.Cache.MaxFullHandles = Math.Max(_svc.Cache.MaxFullHandles, Prefs.MaxFullHandlesPerViewport * Layout.Count);
            AssignDefaults();
        }
        RaiseUi();
        InvalidateAll();
    }

    /// <summary>Puts a series into a viewport (drag from the series strip).</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seriesIndex">Index into <see cref="Series"/>.</param>
    public void AssignSeries(int viewport, int seriesIndex)
    {
        Viewport vp = GetViewport(viewport);
        lock (_gate)
        {
            if (seriesIndex < 0 || seriesIndex >= _series.Count)
            {
                return;
            }
            SetSeriesLocked(vp, seriesIndex);
            ActiveViewport = vp.Index;
        }
        RaiseUi();
        InvalidateAll();
    }

    /// <summary>Makes a viewport active.</summary>
    /// <param name="viewport">Viewport.</param>
    public void Activate(int viewport)
    {
        lock (_gate)
        {
            ActiveViewport = Math.Clamp(viewport, 0, Layout.Count - 1);
        }
        RaiseUi();
        InvalidateAll();
    }

    /// <summary>Runs a named action on the active viewport (the toolbar buttons and keyboard commands).</summary>
    /// <param name="action">invert, rotate, flipH, flipV, fit, oneToOne, trueSize, reset, overlays, privacy, undo, redo,
    /// cineToggle, cinePlay, cinePause, cineFirst, cineLast, cineLoop, cineBounce, nextViewport, clearAnnotations.</param>
    public void RunAction(string action)
    {
        bool render = false, ui = false, all = false, indexChanged;
        Viewport vp;
        List<Viewport>? synced;
        lock (_gate)
        {
            Touch();
            vp = _viewports[ActiveViewport];
            DisplayState before = vp.State;
            int beforeIndex = vp.ImageIndex;
            switch (action)
            {
                case "invert": Apply(vp, s => s with { Invert = !s.Invert }, ref render); break;
                case "rotate": Apply(vp, s => s with { Rotation = (s.Rotation + 90) % 360 }, ref render); break;
                case "rotateLeft": Apply(vp, s => s with { Rotation = (s.Rotation + 270) % 360 }, ref render); break;
                case "flipH": Apply(vp, s => s with { FlipHorizontal = !s.FlipHorizontal }, ref render); break;
                case "flipV": Apply(vp, s => s with { FlipVertical = !s.FlipVertical }, ref render); break;
                case "fit": Apply(vp, s => s with { Zoom = 0, PanX = 0, PanY = 0 }, ref render); break;
                case "oneToOne": Apply(vp, s => s with { Zoom = 1, PanX = 0, PanY = 0 }, ref render); break;
                case "trueSize": TrueSizeLocked(vp, ref render); break;
                case "reset": ResetLocked(vp, ref render); break;
                case "overlays": Apply(vp, s => s with { ShowOverlays = !s.ShowOverlays }, ref render); break;
                case "privacy": Privacy = !Privacy; ui = true; all = true; break;
                case "cineToggle": ToggleCineLocked(vp); ui = true; break;
                case "cinePlay" when !vp.Cine.IsPlaying: ToggleCineLocked(vp); ui = true; break;
                case "cinePause" when vp.Cine.IsPlaying: ToggleCineLocked(vp); ui = true; break;
                case "cineFirst" when vp.Series is { } s1: StopCineLocked(vp); render |= SetIndexLocked(vp, 0, s1); ui = true; break;
                case "cineLast" when vp.Series is { } s2: StopCineLocked(vp); render |= SetIndexLocked(vp, s2.Images.Count - 1, s2); ui = true; break;
                case "cineLoop": vp.Cine.Mode = CineMode.Loop; RestartCineLocked(vp); ui = true; break;
                case "cineBounce": vp.Cine.Mode = CineMode.Bounce; RestartCineLocked(vp); ui = true; break;
                case "nextViewport": ActiveViewport = (ActiveViewport + 1) % Layout.Count; ui = true; all = true; break;
                case "syncToggle": ToggleSyncMemberLocked(vp.Index); ui = true; all = true; break;
                case "syncAll": Sync = Sync with { Members = Enumerable.Range(0, Layout.Count).ToHashSet() }; ui = true; all = true; break;
                case "syncNone": Sync = Sync with { Members = new HashSet<int>() }; ui = true; all = true; break;
                case "syncWl": Sync = Sync with { WindowLevel = !Sync.WindowLevel }; ui = true; break;
                case "syncZoom": Sync = Sync with { Zoom = !Sync.Zoom }; ui = true; break;
                case "syncPan": Sync = Sync with { Pan = !Sync.Pan }; ui = true; break;
                case "linkScroll": LinkScroll = !LinkScroll; ui = true; all = true; break;
                case "refLines": ShowReferenceLines = !ShowReferenceLines; ui = true; all = true; break;
                case "clearCross": _cross = null; all = true; break;
            }
            synced = PropagateSyncLocked(vp, before);
            indexChanged = vp.ImageIndex != beforeIndex;
        }
        synced?.ForEach(Invalidate);
        if (indexChanged)
        {
            AfterSliceChange(vp);
        }
        switch (action)
        {
            case "undo": Undo(); return;
            case "redo": Redo(); return;
            case "clearAnnotations": Observe(ClearAnnotationsAsync(vp), "clearing the measurements"); return;
        }
        if (all)
        {
            InvalidateAll();
        }
        else if (render)
        {
            Invalidate(vp);
        }
        if (ui)
        {
            RaiseUi();
        }
    }

    private void TrueSizeLocked(Viewport vp, ref bool render)
    {
        if (vp.CurrentImage is not { } img)
        {
            return;
        }
        TkPlane g;
        try
        {
            g = img.Instance.Header.GetGeometry(img.Frame);
        }
        catch (TkException)
        {
            return;
        }
        if (g.ColumnSpacing <= 0)
        {
            return;
        }
        double pxPerMm = DevicePixelRatio * 96.0 / 25.4 * Math.Max(0.1, Prefs.DisplayCalibration);
        vp.State = vp.State with { Zoom = g.ColumnSpacing * pxPerMm, PanX = 0, PanY = 0 };
        render = true;
    }

    /// <summary>Applies window preset n (0-based) to the active viewport.</summary>
    /// <param name="n">Preset index.</param>
    public void ApplyPreset(int n)
    {
        bool render = false;
        Viewport vp;
        List<Viewport>? synced;
        lock (_gate)
        {
            vp = _viewports[ActiveViewport];
            DisplayState before = vp.State;
            ApplyPresetLocked(vp, n, ref render);
            synced = PropagateSyncLocked(vp, before);
        }
        if (render)
        {
            Invalidate(vp);
        }
        synced?.ForEach(Invalidate);
    }

    /// <summary>Sets the colour map of the active viewport (grayscale images; "gray" = none).</summary>
    /// <param name="name">Colour map name (gray, inverseGray, hotIron, pet, hotMetalBlue, pet20Step).</param>
    public void SetColormap(string name)
    {
        if (!TryParseColormap(name, out TkColormap c))
        {
            return;
        }
        Viewport vp;
        lock (_gate)
        {
            vp = _viewports[ActiveViewport];
            vp.State = vp.State with { Colormap = c };
        }
        Invalidate(vp);
        RaiseUi();
    }

    private void ToggleSyncMemberLocked(int index)
    {
        var members = new HashSet<int>(Sync.Members);
        if (!members.Remove(index))
        {
            members.Add(index);
        }
        Sync = Sync with { Members = members };
    }

    /// <summary>Puts a viewport in or out of the sync group.</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="member">Member or not.</param>
    public void SetSyncMember(int viewport, bool member)
    {
        lock (_gate)
        {
            var members = new HashSet<int>(Sync.Members);
            if (member)
            {
                members.Add(Math.Clamp(viewport, 0, MaxViewports - 1));
            }
            else
            {
                members.Remove(viewport);
            }
            Sync = Sync with { Members = members };
        }
        RaiseUi();
        InvalidateAll();
    }

    /// <summary>The user typed the text of a Text annotation; empty text (or cancel) removes it.</summary>
    /// <param name="annotationId">Annotation.</param>
    /// <param name="text">Text as typed (stored unchanged).</param>
    /// <returns>Completion.</returns>
    public async Task SetAnnotationTextAsync(string annotationId, string? text)
    {
        Viewport vp;
        SeriesImage? img;
        lock (_gate)
        {
            vp = _viewports.FirstOrDefault(v => v.TextPrompt == annotationId) ?? _viewports[ActiveViewport];
            vp.TextPrompt = null;
            img = vp.CurrentImage;
        }
        if (img is null)
        {
            return;
        }
        AnnotationDocument doc = await GetDocumentAsync(img.Key).ConfigureAwait(false);
        lock (_gate)
        {
            Annotation? a = doc.Annotations.FirstOrDefault(x => x.Id == annotationId && x.Kind == AnnotationKind.Text);
            if (a is null)
            {
                return;
            }
            if (string.IsNullOrEmpty(text))
            {
                doc.Annotations.Remove(a);
            }
            else
            {
                a.Text = text;
                a.Label = text;
            }
        }
        await SaveAsync(doc).ConfigureAwait(false);
        SendAnnotationState(vp);
        RaiseUi();
    }

    /// <summary>The browser exported or copied the active viewport's image (with the intended-use label): audited.</summary>
    /// <param name="mode">"file" or "clipboard".</param>
    /// <param name="succeeded">The export worked.</param>
    /// <param name="reason">Plain-word failure reason from the browser, or null.</param>
    public void ReportExport(string mode, bool succeeded, string? reason)
    {
        SeriesImage? img;
        lock (_gate)
        {
            img = _viewports[ActiveViewport].CurrentImage;
        }
        var fields = new Dictionary<string, string>
        {
            ["mode"] = mode is "file" or "clipboard" ? mode : "other",
            ["patientKey"] = Study?.PatientKey ?? "",
            ["study"] = StudyInstanceUid,
            ["sop"] = img?.Instance.SopInstanceUid ?? "",
        };
        // Browser error names only ("NotAllowedError"); never image or patient data.
        string? why = succeeded || string.IsNullOrEmpty(reason) ? null : new string([.. reason.Where(char.IsAsciiLetterOrDigit).Take(64)]);
        _svc.Audit?.Append("image.export", succeeded ? AuditOutcome.Success : AuditOutcome.Failure, fields, why);
    }

    /// <summary>Sets the cine rate of the active viewport.</summary>
    /// <param name="fps">Frames per second (0.5..120).</param>
    public void SetCineFps(double fps)
    {
        lock (_gate)
        {
            Viewport vp = _viewports[ActiveViewport];
            vp.Cine.Fps = Math.Clamp(fps, 0.5, 120);
            RestartCineLocked(vp);
        }
        RaiseUi();
    }

    /// <summary>Sets the browser's device pixel ratio.</summary>
    /// <param name="dpr">Ratio.</param>
    public void SetDevicePixelRatio(double dpr)
    {
        lock (_gate)
        {
            DevicePixelRatio = Math.Clamp(dpr, 0.5, 8);
        }
    }

    // ================================================================== cine

    private void ToggleCineLocked(Viewport vp)
    {
        if (vp.Cine.IsPlaying)
        {
            StopCineLocked(vp);
            Invalidate(vp);
            return;
        }
        if (vp.Series is not { Images.Count: > 1 } s)
        {
            return;
        }
        vp.Cine.Play(vp.ImageIndex, s.Images.Count);
        var cts = new CancellationTokenSource();
        vp.CineStop = cts;
        Observe(CineLoopAsync(vp, cts.Token), "cine");
    }

    private static void RestartCineLocked(Viewport vp)
    {
        if (vp.Cine.IsPlaying && vp.Series is { } s)
        {
            vp.Cine.Play(vp.ImageIndex, s.Images.Count);
        }
    }

    private static void StopCineLocked(Viewport vp)
    {
        vp.Cine.Pause();
        vp.CineStop?.Cancel();
        vp.CineStop?.Dispose();
        vp.CineStop = null;
    }

    private async Task CineLoopAsync(Viewport vp, CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                double wait;
                lock (_gate)
                {
                    if (!vp.Cine.IsPlaying)
                    {
                        return;
                    }
                    wait = vp.Cine.MillisecondsToNext;
                }
                await Task.Delay(TimeSpan.FromMilliseconds(Math.Max(1, wait)), ct).ConfigureAwait(false);
                bool changed;
                lock (_gate)
                {
                    if (!vp.Cine.IsPlaying || vp.Series is null)
                    {
                        return;
                    }
                    int idx = vp.Cine.CurrentIndex;
                    changed = idx != vp.ImageIndex;
                    vp.ImageIndex = idx;
                }
                if (changed)
                {
                    // Coalesced: while the last frame is not acknowledged this only marks the viewport
                    // dirty, so slow browsers skip frames and the speed stays true.
                    Invalidate(vp);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Stopped.
        }
    }

    // ================================================================== annotations

    private MeasureContext Context(SeriesImage img, TkFrameInfo? info, AnnotationDocument doc)
    {
        TkFile h = img.Instance.Header;
        TkPlane g;
        try
        {
            g = h.GetGeometry(img.Frame);
        }
        catch (TkException)
        {
            g = new TkPlane(0, 0, TkSpacingSource.None, false, 0, 0);
        }
        string modality = img.Instance.Modality;
        return new MeasureContext
        {
            RowSpacing = g.RowSpacing,
            ColumnSpacing = g.ColumnSpacing,
            Source = g.Source,
            CalibrationMmPerPixel = doc.CalibrationMmPerPixel,
            Modality = modality,
            Units = MeasureContext.UnitsFor(h.GetString(Tags.RescaleType), modality, h.GetString(Tags.Units)),
            Monochrome = info?.Monochrome ?? true,
            Suv = string.Equals(modality, "PT", StringComparison.Ordinal) ? SafeSuv(img) : null,
            Frame = img.Frame,
            PixelFile = () => _svc.Cache.GetFullFile(img.Instance),
        };
    }

    // SUV factors from the header (the app's own computation, Measure/Suv); null when unreadable.
    private static Measure.Suv.SuvResult? SafeSuv(SeriesImage img)
    {
        try
        {
            return Measure.Suv.SuvCalculator.Compute(img.Instance.Header.Dataset, img.Frame);
        }
        catch (Exception e) when (e is TkException or ObjectDisposedException)
        {
            return null;
        }
    }

    private async Task RecomputeAsync(FrameKey key, IList<Annotation> list)
    {
        SeriesImage? img = FindImage(key);
        if (img is null)
        {
            return;
        }
        AnnotationDocument doc;
        TkFrameInfo? info;
        using (FrameLease? f = _svc.Cache.TryLease(key))
        {
            info = f?.Frame.Info;
        }
        lock (_gate)
        {
            doc = _docs[key];
        }
        Annotation[] copies;
        lock (_gate)
        {
            copies = [.. list];
        }
        await _svc.Scheduler.RunAsync(WorkPool.Interactive, _ =>
        {
            MeasureContext ctx = Context(img, info, doc);
            foreach (Annotation a in copies)
            {
                MeasureService.Update(a, ctx);
            }
        }, _closedToken).ConfigureAwait(false);
    }

    private SeriesImage? FindImage(FrameKey key)
    {
        lock (_gate)
        {
            foreach (SeriesSource s in _series)
            {
                foreach (SeriesImage i in s.Images)
                {
                    if (i.Key == key)
                    {
                        return i;
                    }
                }
            }
        }
        return null;
    }

    private async Task UpdateDrawingAsync(Viewport vp)
    {
        SeriesImage? img;
        Annotation? d;
        AnnotationDocument? doc;
        lock (_gate)
        {
            img = vp.CurrentImage;
            d = vp.Drawing?.Clone();
            doc = img is null ? null : _docs.GetValueOrDefault(img.Key);
        }
        if (img is null || d is null || doc is null)
        {
            return;
        }
        TkFrameInfo? info = vp.FrameInfo;
        try
        {
            await _svc.Scheduler.RunAsync(WorkPool.Interactive, _ => MeasureService.Update(d, Context(img, info, doc)), _closedToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        lock (_gate)
        {
            if (vp.Drawing is { } live && live.Id == d.Id)
            {
                live.Label = d.Label;
                live.Warnings.Clear();
                foreach (string w in d.Warnings)
                {
                    live.Warnings.Add(w);
                }
            }
        }
        SendAnnotationState(vp);
    }

    private async Task CommitAsync(Viewport vp, Annotation a)
    {
        SeriesImage? img;
        lock (_gate)
        {
            img = vp.CurrentImage;
        }
        if (img is null)
        {
            return;
        }
        AnnotationDocument doc = await GetDocumentAsync(img.Key).ConfigureAwait(false);
        TkFrameInfo? info = vp.FrameInfo;
        await _svc.Scheduler.RunAsync(WorkPool.Interactive, _ => MeasureService.Update(a, Context(img, info, doc)), _closedToken).ConfigureAwait(false);
        List<Annotation> before, after;
        lock (_gate)
        {
            before = [.. doc.Annotations.Select(x => x.Clone())];
            doc.Annotations.Add(a);
            after = [.. doc.Annotations.Select(x => x.Clone())];
            _undo.Push((img.Key, before, after));
            _redo.Clear();
            if (a.Kind == AnnotationKind.Calibration)
            {
                vp.CalibrationPrompt = a.Id;
            }
            else if (a.Kind == AnnotationKind.Text)
            {
                vp.TextPrompt = a.Id;
            }
        }
        await SaveAsync(doc).ConfigureAwait(false);
        SendAnnotationState(vp);
        RaiseUi();
    }

    /// <summary>The user typed the true length of a calibration line: sets the instance's mm per pixel.</summary>
    /// <param name="annotationId">Calibration annotation.</param>
    /// <param name="trueLengthMm">True length; 0 or less cancels.</param>
    /// <returns>Completion.</returns>
    public async Task CalibrateAsync(string annotationId, double trueLengthMm)
    {
        Viewport vp;
        SeriesImage? img;
        lock (_gate)
        {
            vp = _viewports.FirstOrDefault(v => v.CalibrationPrompt == annotationId) ?? _viewports[ActiveViewport];
            vp.CalibrationPrompt = null;
            img = vp.CurrentImage;
        }
        if (img is null)
        {
            return;
        }
        AnnotationDocument doc = await GetDocumentAsync(img.Key).ConfigureAwait(false);
        Annotation? line;
        lock (_gate)
        {
            line = doc.Annotations.FirstOrDefault(x => x.Id == annotationId);
            if (line is null || line.Count < 2)
            {
                return;
            }
            if (!(trueLengthMm > 0) || double.IsInfinity(trueLengthMm))
            {
                doc.Annotations.Remove(line);
                line = null;
            }
        }
        if (line is not null)
        {
            double px = MeasureService.Length(line.At(0), line.At(1), new MeasureContext());
            if (px > 0)
            {
                line.TrueLengthMm = trueLengthMm;
                double scale = trueLengthMm / px;
                var calibration = new Dictionary<string, string>
                {
                    ["patientKey"] = Study?.PatientKey ?? "",
                    ["study"] = StudyInstanceUid,
                    ["sop"] = img.Instance.SopInstanceUid,
                    ["mmPerPixel"] = scale.ToString("R", CultureInfo.InvariantCulture),
                };
                try
                {
                    await _svc.Scheduler.RunAsync(WorkPool.StoreRead, _ => _svc.Annotations.SaveCalibration(img.Instance.SopInstanceUid, scale), _closedToken)
                        .ConfigureAwait(false);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    _svc.Audit?.Append("calibration.change", AuditOutcome.Failure, calibration, Plain(e));
                    throw;
                }
                _svc.Audit?.Append("calibration.change", AuditOutcome.Success, calibration);
                lock (_gate)
                {
                    foreach (KeyValuePair<FrameKey, AnnotationDocument> kv in _docs.Where(k => k.Key.SopInstanceUid == img.Instance.SopInstanceUid))
                    {
                        kv.Value.CalibrationMmPerPixel = scale;
                    }
                }
            }
        }
        await RecomputeAsync(img.Key, doc.Annotations).ConfigureAwait(false);
        await SaveAsync(doc).ConfigureAwait(false);
        Invalidate(vp);
    }

    private Task SaveAsync(AnnotationDocument doc)
    {
        AnnotationDocument snapshot;
        lock (_gate)
        {
            snapshot = new AnnotationDocument { SopInstanceUid = doc.SopInstanceUid, Frame = doc.Frame, Annotations = [.. doc.Annotations.Select(a => a.Clone())] };
        }
        return SaveAuditedAsync(snapshot);
    }

    // Every stored change of a frame's annotations is audited (R-127): which image, how many shapes; never their text.
    private async Task SaveAuditedAsync(AnnotationDocument snapshot)
    {
        var fields = new Dictionary<string, string>
        {
            ["patientKey"] = Study?.PatientKey ?? "",
            ["study"] = StudyInstanceUid,
            ["sop"] = snapshot.SopInstanceUid,
            ["frame"] = snapshot.Frame.ToString(CultureInfo.InvariantCulture),
            ["annotations"] = snapshot.Annotations.Count.ToString(CultureInfo.InvariantCulture),
        };
        try
        {
            await _svc.Scheduler.RunAsync(WorkPool.StoreRead, _ => _svc.Annotations.Save(snapshot), _closedToken).ConfigureAwait(false);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _svc.Audit?.Append("annotation.change", AuditOutcome.Failure, fields, Plain(e));
            throw;
        }
        _svc.Audit?.Append("annotation.change", AuditOutcome.Success, fields);
    }

    private async Task ClearAnnotationsAsync(Viewport vp)
    {
        SeriesImage? img;
        lock (_gate)
        {
            img = vp.CurrentImage;
        }
        if (img is null)
        {
            return;
        }
        AnnotationDocument doc = await GetDocumentAsync(img.Key).ConfigureAwait(false);
        lock (_gate)
        {
            _undo.Push((img.Key, [.. doc.Annotations.Select(a => a.Clone())], []));
            _redo.Clear();
            doc.Annotations.Clear();
        }
        await SaveAsync(doc).ConfigureAwait(false);
        SendAnnotationState(vp);
        RaiseUi();
    }

    /// <summary>Undoes the last annotation change.</summary>
    public void Undo() => Observe(SwapAsync(_undo, _redo, useBefore: true), "undo");

    /// <summary>Redoes the last undone change.</summary>
    public void Redo() => Observe(SwapAsync(_redo, _undo, useBefore: false), "redo");

    private async Task SwapAsync(Stack<(FrameKey Key, List<Annotation> Before, List<Annotation> After)> from,
                                 Stack<(FrameKey Key, List<Annotation> Before, List<Annotation> After)> to, bool useBefore)
    {
        (FrameKey Key, List<Annotation> Before, List<Annotation> After) e;
        AnnotationDocument? doc;
        lock (_gate)
        {
            if (!from.TryPop(out e))
            {
                return;
            }
            to.Push(e);
            doc = _docs.GetValueOrDefault(e.Key);
            if (doc is null)
            {
                return;
            }
            doc.Annotations.Clear();
            foreach (Annotation a in (useBefore ? e.Before : e.After).Select(x => x.Clone()))
            {
                doc.Annotations.Add(a);
            }
        }
        await SaveAsync(doc).ConfigureAwait(false);
        foreach (Viewport vp in _viewports.Where(v => v.CurrentImage?.Key == e.Key))
        {
            SendAnnotationState(vp);
        }
        RaiseUi();
    }

    /// <summary>Annotations of a frame (tests and the measurement panel).</summary>
    /// <param name="key">Frame.</param>
    /// <returns>Copies.</returns>
    public IReadOnlyList<Annotation> GetAnnotations(FrameKey key)
    {
        lock (_gate)
        {
            return _docs.TryGetValue(key, out AnnotationDocument? d) ? [.. d.Annotations.Select(a => a.Clone())] : [];
        }
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        List<SeriesSource> series;
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            foreach (Viewport vp in _viewports)
            {
                StopCineLocked(vp);
                vp.Series = null;
            }
            _sink = null;
            series = [.. _series];
            _series.Clear();
            _arrived.Clear();
        }
        _svc.Store.EventReceived -= OnStoreEvent;
        _closed.Cancel();
        // Header handles (one per instance) are released now; work still running on them sees
        // ObjectDisposedException after the cancellation and stops quietly.
        DisposeHeaders(series);
        _closed.Dispose();
    }
}
