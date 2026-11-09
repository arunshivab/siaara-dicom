// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Buffers;
using System.Globalization;
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
public sealed record ViewerServices(StoreService Store, RenderCache Cache, ToolkitScheduler Scheduler, SettingsStore Settings, AnnotationStore Annotations);

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

    /// <summary>Device pixel ratio of the browser (true-size zoom).</summary>
    public double DevicePixelRatio { get; private set; } = 1;

    /// <summary>All series headers are read.</summary>
    public bool Loaded { get; private set; }

    /// <summary>Loading problem in plain words, or null.</summary>
    public string? LoadError { get; private set; }

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
                IReadOnlyList<SeriesSource> loaded = await tasks[i].ConfigureAwait(false);
                lock (_gate)
                {
                    _series.AddRange(loaded.Where(s => s.Images.Count > 0));
                    AssignDefaults();
                }
                RaiseUi();
                InvalidateAll();
            }
            lock (_gate)
            {
                Loaded = true;
                if (_series.Count == 0)
                {
                    LoadError = "This study has no images that can be shown.";
                }
            }
            RaiseUi();
            InvalidateAll();
        }
        catch (OperationCanceledException)
        {
            // Session closed while loading.
        }
        catch (TkException e)
        {
            LoadError = "The study could not be read: " + e.Message;
            AppLog.Error("viewer", "study load failed", e);
            RaiseUi();
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

    private void StartRender(Viewport vp) => _ = RenderAsync(vp);

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
        TkFrame frame = await _svc.Cache.GetFrameAsync(job.Image, WorkPriority.Normal, _closedToken).ConfigureAwait(false);
        AnnotationDocument doc = await GetDocumentAsync(job.Image.Key).ConfigureAwait(false);
        IViewSink? sink;
        lock (_gate)
        {
            sink = _sink;
        }
        if (sink is null)
        {
            return false;
        }
        bool gray = frame.Info.Monochrome;
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
        $"{j.Image?.Instance.SopInstanceUid}|{j.Width}x{j.Height}|{s.Zoom}|{s.PanX}|{s.PanY}|{s.Rotation}|{s.FlipHorizontal}|{s.FlipVertical}|{s.Voi}|{s.VoiIndex}|{s.WindowCenter}|{s.WindowWidth}|{s.Invert}|{s.NoOverlays}|{s.Linear}");

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
        RenderSpec spec = st.ToSpec(job.Width, job.Height, frame.Info.Monochrome);
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
        if (img is null || !_svc.Cache.TryGetDecoded(img.Key, out TkFrame? frame))
        {
            return;
        }
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
            RenderSpec spec = vp.State.ToSpec(vp.Width, vp.Height, frame.Info.Monochrome);
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
        _ = frame;
        return new ViewportStateMessage
        {
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
        bool render = false, annotations = false, ui = false, roi = false;
        Annotation? committed = null;
        lock (_gate)
        {
            Touch();
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
        }
        Finish(vp, render, annotations, ui, roi, committed);
    }

    private void InvalidateLater(IEnumerable<Viewport> vps)
    {
        foreach (Viewport v in vps.ToArray())
        {
            _ = Task.Run(() => Invalidate(v));
        }
    }

    private void Finish(Viewport vp, bool render, bool annotations, bool ui, bool roi, Annotation? committed)
    {
        if (committed is not null)
        {
            _ = CommitAsync(vp, committed);
        }
        else if (roi)
        {
            _ = UpdateDrawingAsync(vp);
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
        if (KindOf(tool) is not AnnotationKind kind || ImagePoint(vp, e.X, e.Y) is not PointD p)
        {
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
            case 90 when ctrl: _ = Task.Run(Undo); break;                    // Ctrl+Z
            case 89 when ctrl: _ = Task.Run(Redo); break;                    // Ctrl+Y
            case 90: SetToolLocked(ViewerTool.Zoom, ref ui); break;          // Z
            case 80: SetToolLocked(ViewerTool.Pan, ref ui); break;           // P
            case 76: SetToolLocked(ViewerTool.Length, ref ui); break;        // L
            case 65: SetToolLocked(ViewerTool.Angle, ref ui); break;         // A
            case 69: SetToolLocked(ViewerTool.Ellipse, ref ui); break;       // E
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
        bool render = false, ui = false, all = false;
        Viewport vp;
        lock (_gate)
        {
            Touch();
            vp = _viewports[ActiveViewport];
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
            }
        }
        switch (action)
        {
            case "undo": Undo(); return;
            case "redo": Redo(); return;
            case "clearAnnotations": _ = ClearAnnotationsAsync(vp); return;
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
        lock (_gate)
        {
            vp = _viewports[ActiveViewport];
            ApplyPresetLocked(vp, n, ref render);
        }
        if (render)
        {
            Invalidate(vp);
        }
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
        _ = CineLoopAsync(vp, cts.Token);
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

    private TkSuv? SafeSuv(SeriesImage img)
    {
        try
        {
            return _svc.Cache.GetFullFile(img.Instance).ComputeSuv(img.Frame);
        }
        catch (TkException)
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
        TkFrameInfo? info = _svc.Cache.TryGetDecoded(key, out TkFrame? f) ? f.Info : null;
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
                await _svc.Scheduler.RunAsync(WorkPool.StoreRead, _ => _svc.Annotations.SaveCalibration(img.Instance.SopInstanceUid, scale), _closedToken)
                    .ConfigureAwait(false);
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
        return _svc.Scheduler.RunAsync(WorkPool.StoreRead, _ => _svc.Annotations.Save(snapshot), _closedToken);
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
    public void Undo() => _ = SwapAsync(_undo, _redo, useBefore: true);

    /// <summary>Redoes the last undone change.</summary>
    public void Redo() => _ = SwapAsync(_redo, _undo, useBefore: false);

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
        lock (_gate)
        {
            foreach (Viewport vp in _viewports)
            {
                StopCineLocked(vp);
            }
            _sink = null;
        }
        _closed.Cancel();
        _closed.Dispose();
    }
}
