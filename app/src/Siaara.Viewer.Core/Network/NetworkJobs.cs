// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using System.Globalization;
using Siaara.Viewer.Core.Logging;

namespace Siaara.Viewer.Core.Network;

/// <summary>What a network job does.</summary>
public enum NetworkJobKind
{
    /// <summary>Fetching a study or series from a node.</summary>
    Retrieve,
    /// <summary>Sending images to a node.</summary>
    Send,
    /// <summary>Images arriving at our receiver.</summary>
    Receive,
}

/// <summary>State of a network job.</summary>
public enum NetworkJobState
{
    /// <summary>Waiting for one of the transfer slots.</summary>
    Queued,
    /// <summary>Running.</summary>
    Running,
    /// <summary>Finished, everything transferred.</summary>
    Completed,
    /// <summary>Finished, but some images failed or were not confirmed.</summary>
    CompletedWithErrors,
    /// <summary>Failed.</summary>
    Failed,
    /// <summary>Cancelled by the user.</summary>
    Cancelled,
}

/// <summary>
/// One row of the activity panel: a retrieve, a send, or a burst of images arriving at our receiver.
/// <see cref="Title"/> and <see cref="Notes"/> are for the screen only (the title may name the patient); they are
/// never written to a log.
/// </summary>
public sealed class NetworkJob : IDisposable
{
    private readonly Lock _gate = new();
    private readonly CancellationTokenSource _cts = new();
    private readonly TaskCompletionSource _done = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly List<string> _notes = [];
    private readonly Stopwatch _clock = new();
    private readonly NetworkJobs _owner;
    private NetworkJobState _state = NetworkJobState.Queued;
    private int _completed, _failed, _warning, _total;
    private long _bytes, _totalBytes;
    private string? _error;
    private (long Ms, int Items, long Bytes) _mark, _prev;
    private Func<NetworkJob>? _retry;

    internal NetworkJob(NetworkJobs owner, NetworkJobKind kind, string nodeName, string title, string? studyInstanceUid, int total)
    {
        _owner = owner;
        Id = Guid.NewGuid().ToString("N")[..12];
        Kind = kind;
        NodeName = nodeName;
        Title = title;
        StudyInstanceUid = studyInstanceUid;
        _total = Math.Max(0, total);
        CreatedUtc = DateTime.UtcNow;
        Token = _cts.Token;
    }

    /// <summary>Id of the row.</summary>
    public string Id { get; }
    /// <summary>Kind.</summary>
    public NetworkJobKind Kind { get; }
    /// <summary>Name of the node (or the sender's AE title for arriving images).</summary>
    public string NodeName { get; }
    /// <summary>Text for the screen.</summary>
    public string Title { get; }
    /// <summary>The study, when the job is about one.</summary>
    public string? StudyInstanceUid { get; }
    /// <summary>When the job was created.</summary>
    public DateTime CreatedUtc { get; }
    /// <summary>When it ended, or null.</summary>
    public DateTime? EndedUtc { get; private set; }
    /// <summary>Ends when the job has reached a final state.</summary>
    public Task Completion => _done.Task;

    // Taken once: it stays usable after the row was dropped from the list and its source disposed.
    internal CancellationToken Token { get; }

    /// <summary>State.</summary>
    public NetworkJobState State
    {
        get
        {
            lock (_gate)
            {
                return _state;
            }
        }
    }

    /// <summary>True in a final state.</summary>
    public bool IsFinished => State is not (NetworkJobState.Queued or NetworkJobState.Running);

    /// <summary>Images done.</summary>
    public int Completed => Volatile.Read(ref _completed);
    /// <summary>Images failed.</summary>
    public int Failed => Volatile.Read(ref _failed);
    /// <summary>Images transferred with a warning.</summary>
    public int Warnings => Volatile.Read(ref _warning);
    /// <summary>Images expected; 0 when unknown.</summary>
    public int Total => Volatile.Read(ref _total);
    /// <summary>Bytes transferred, when the transport reports them.</summary>
    public long Bytes => Interlocked.Read(ref _bytes);
    /// <summary>Why the job failed, in plain words; null otherwise.</summary>
    public string? Error
    {
        get
        {
            lock (_gate)
            {
                return _error;
            }
        }
    }

    /// <summary>Lines for the screen: per-image failures, changed identifiers, the commitment result.</summary>
    public IReadOnlyList<string> Notes
    {
        get
        {
            lock (_gate)
            {
                return [.. _notes];
            }
        }
    }

    /// <summary>Progress from 0 to 1, or null when the total is unknown.</summary>
    public double? Fraction
    {
        get
        {
            long tb = Interlocked.Read(ref _totalBytes);
            if (tb > 0)
            {
                return Math.Clamp((double)Bytes / tb, 0, 1);
            }
            int t = Total;
            return t > 0 ? Math.Clamp((double)(Completed + Failed) / t, 0, 1) : null;
        }
    }

    /// <summary>Current speed as text ("12.3 images/s", "4.5 MB/s"), or "" when not running.</summary>
    public string SpeedText
    {
        get
        {
            lock (_gate)
            {
                if (_state != NetworkJobState.Running)
                {
                    return "";
                }
                long now = _clock.ElapsedMilliseconds;
                (long ms, int items, long bytes) = now - _mark.Ms >= 500 ? _mark : _prev;
                double dt = (now - ms) / 1000.0;
                if (dt < 0.2)
                {
                    return "";
                }
                long db = Bytes - bytes;
                int di = Completed + Failed - items;
                return db > 0 ? (db / dt / 1048576.0).ToString("F1", CultureInfo.InvariantCulture) + " MB/s"
                    : di > 0 ? (di / dt).ToString("F1", CultureInfo.InvariantCulture) + " images/s" : "";
            }
        }
    }

    /// <summary>True when the job can be started again (a failed, cancelled or partly failed retrieve or send).</summary>
    public bool CanRetry => _retry is not null && State is NetworkJobState.Failed or NetworkJobState.Cancelled or NetworkJobState.CompletedWithErrors;

    internal void SetRetry(Func<NetworkJob> retry) => _retry = retry;

    /// <summary>Starts the same transfer again as a new job.</summary>
    /// <returns>The new job, or null when this one cannot be retried.</returns>
    public NetworkJob? Retry() => CanRetry ? _retry!() : null;

    /// <summary>Asks the job to stop. The row shows "Cancelled" within one second; a connection that is still being
    /// opened is abandoned in the background.</summary>
    public void Cancel()
    {
        if (IsFinished)
        {
            return;
        }
        try
        {
            _cts.Cancel();
        }
        catch (ObjectDisposedException)
        {
            return;
        }
        _ = Task.Delay(TimeSpan.FromMilliseconds(900)).ContinueWith(_ => Finish(NetworkJobState.Cancelled, null), TaskScheduler.Default);
    }

    internal bool Begin()
    {
        lock (_gate)
        {
            if (_state != NetworkJobState.Queued)
            {
                return false;
            }
            _state = NetworkJobState.Running;
            _clock.Start();
        }
        _owner.Touch();
        return true;
    }

    internal void Report(int completed, int failed, int warning, int total = -1)
    {
        Volatile.Write(ref _completed, completed);
        Volatile.Write(ref _failed, failed);
        Volatile.Write(ref _warning, warning);
        if (total > 0)
        {
            Volatile.Write(ref _total, total);
        }
        Sample();
    }

    internal void ReportBytes(long bytes, long totalBytes)
    {
        Interlocked.Exchange(ref _bytes, bytes);
        Interlocked.Exchange(ref _totalBytes, totalBytes);
        Sample();
    }

    private void Sample()
    {
        lock (_gate)
        {
            long now = _clock.ElapsedMilliseconds;
            if (now - _mark.Ms >= 1000)
            {
                _prev = _mark;
                _mark = (now, Completed + Failed, Bytes);
            }
        }
        _owner.Touch();
    }

    internal void Note(string line)
    {
        lock (_gate)
        {
            if (_notes.Count < 200)
            {
                _notes.Add(line);
            }
            else if (_notes.Count == 200)
            {
                _notes.Add("(more lines are not shown)");
            }
        }
        _owner.Touch();
    }

    // The first final state wins (a cancel shown after one second is not overwritten by the worker ending later).
    internal bool Finish(NetworkJobState state, string? error)
    {
        lock (_gate)
        {
            if (_state is not (NetworkJobState.Queued or NetworkJobState.Running))
            {
                return false;
            }
            _state = state;
            _error = error;
            _clock.Stop();
            EndedUtc = DateTime.UtcNow;
        }
        _done.TrySetResult();
        _owner.Trim();
        _owner.Touch();
        return true;
    }

    /// <inheritdoc/>
    public void Dispose() => _cts.Dispose();
}

/// <summary>
/// The activity list: every retrieve, send and receive row, newest first; the last 200 are kept in memory. At most
/// <see cref="MaxTransfers"/> retrieve and send jobs run at once, the others wait as "Queued". <see cref="Changed"/>
/// is raised at most ten times a second however fast progress arrives.
/// </summary>
public sealed class NetworkJobs : IDisposable
{
    /// <summary>Transfers that run at the same time.</summary>
    public const int MaxTransfers = 4;

    /// <summary>Rows kept.</summary>
    public const int MaxKept = 200;

    private readonly Lock _gate = new();
    private readonly List<NetworkJob> _jobs = [];
    private readonly SemaphoreSlim _slots = new(MaxTransfers, MaxTransfers);
    private readonly Timer _timer;
    private int _dirty;
    private int _running;
    private int _peak;
    private volatile bool _disposed;

    /// <summary>Creates the list.</summary>
    public NetworkJobs()
    {
        _timer = new Timer(_ => Flush(), null, 100, 100);
    }

    /// <summary>Raised (on a timer thread, at most 10 times a second) when any row changed.</summary>
    public event EventHandler? Changed;

    /// <summary>How often <see cref="Changed"/> was raised (tests).</summary>
    public long Notifications => Interlocked.Read(ref _notifications);

    private long _notifications;

    /// <summary>The most transfers that ever ran at the same time (tests).</summary>
    public int PeakRunning => Volatile.Read(ref _peak);

    /// <summary>Jobs that are queued or running.</summary>
    public int ActiveCount
    {
        get
        {
            lock (_gate)
            {
                return _jobs.Count(j => !j.IsFinished);
            }
        }
    }

    /// <summary>The rows, newest first.</summary>
    /// <returns>A snapshot.</returns>
    public IReadOnlyList<NetworkJob> Snapshot()
    {
        lock (_gate)
        {
            var list = new NetworkJob[_jobs.Count];
            for (int i = 0; i < list.Length; i++)
            {
                list[i] = _jobs[_jobs.Count - 1 - i];
            }
            return list;
        }
    }

    internal void Touch() => Interlocked.Exchange(ref _dirty, 1);

    private void Flush()
    {
        if (_disposed || Interlocked.Exchange(ref _dirty, 0) == 0)
        {
            return;
        }
        Interlocked.Increment(ref _notifications);
        try
        {
            Changed?.Invoke(this, EventArgs.Empty);
        }
#pragma warning disable CA1031 // A failing subscriber (a closed window) must not stop updates for the others.
        catch (Exception e)
#pragma warning restore CA1031
        {
            AppLog.Error("network", "activity subscriber failed", e);
        }
    }

    private void Add(NetworkJob job)
    {
        lock (_gate)
        {
            _jobs.Add(job);
        }
        Trim();
        Touch();
    }

    // Finished rows beyond the limit go, oldest first; unfinished ones always stay.
    internal void Trim()
    {
        lock (_gate)
        {
            for (int i = 0; _jobs.Count > MaxKept && i < _jobs.Count;)
            {
                if (_jobs[i].IsFinished)
                {
                    _jobs[i].Dispose();
                    _jobs.RemoveAt(i);
                }
                else
                {
                    i++;
                }
            }
        }
    }

    /// <summary>Adds a row that is not a transfer of ours (images arriving at the receiver); it is running at once and
    /// does not take a transfer slot.</summary>
    /// <param name="nodeName">Sender.</param>
    /// <param name="title">Text for the screen.</param>
    /// <returns>The row.</returns>
    internal NetworkJob AddReceive(string nodeName, string title)
    {
#pragma warning disable CA2000 // Owned by the list (disposed when the row is dropped).
        var job = new NetworkJob(this, NetworkJobKind.Receive, nodeName, title, null, 0);
#pragma warning restore CA2000
        job.Begin();
        Add(job);
        return job;
    }

    /// <summary>Queues a transfer. The work runs on its own thread once a slot is free and returns the final state
    /// (it throws for failures; <see cref="NetworkText.Describe"/> turns them into the row's error).</summary>
    /// <param name="kind">Retrieve or Send.</param>
    /// <param name="nodeName">Node name.</param>
    /// <param name="title">Text for the screen (never logged).</param>
    /// <param name="studyInstanceUid">Study, or null.</param>
    /// <param name="total">Images expected, 0 when unknown.</param>
    /// <param name="work">The transfer.</param>
    /// <param name="retry">Starts the same transfer again, or null.</param>
    /// <returns>The job.</returns>
    internal NetworkJob Start(NetworkJobKind kind, string nodeName, string title, string? studyInstanceUid, int total,
        Func<NetworkJob, CancellationToken, NetworkJobState> work, Func<NetworkJob>? retry)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
#pragma warning disable CA2000 // Owned by the list (disposed when the row is dropped).
        var job = new NetworkJob(this, kind, nodeName, title, studyInstanceUid, total);
#pragma warning restore CA2000
        if (retry is not null)
        {
            job.SetRetry(retry);
        }
        Add(job);
        _ = Task.Factory.StartNew(() => Run(job, work), CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        return job;
    }

    private void Run(NetworkJob job, Func<NetworkJob, CancellationToken, NetworkJobState> work)
    {
        bool slot = false;
        try
        {
            _slots.Wait(job.Token);
            slot = true;
            if (!job.Begin())
            {
                return;
            }
            int now = Interlocked.Increment(ref _running);
            int peak;
            while (now > (peak = Volatile.Read(ref _peak)) && Interlocked.CompareExchange(ref _peak, now, peak) != peak)
            {
            }
            try
            {
                NetworkJobState end = work(job, job.Token);
                job.Finish(job.Token.IsCancellationRequested && end != NetworkJobState.Completed ? NetworkJobState.Cancelled : end, null);
            }
            finally
            {
                Interlocked.Decrement(ref _running);
            }
        }
        catch (OperationCanceledException)
        {
            job.Finish(NetworkJobState.Cancelled, null);
        }
        catch (ObjectDisposedException)
        {
            job.Finish(NetworkJobState.Cancelled, null);
        }
#pragma warning disable CA1031 // Whatever went wrong ends as a row with a plain-word error, never as a lost task.
        catch (Exception e)
#pragma warning restore CA1031
        {
            if (e is not (Toolkit.TkException or NetworkException))
            {
                AppLog.Error("network", "transfer failed", e);
            }
            job.Finish(job.Token.IsCancellationRequested ? NetworkJobState.Cancelled : NetworkJobState.Failed, NetworkText.Describe(e));
        }
        finally
        {
            if (slot && !_disposed)
            {
                try
                {
                    _slots.Release();
                }
                catch (ObjectDisposedException)
                {
                    // Shutting down.
                }
            }
        }
    }

    /// <summary>Cancels every unfinished job (shutdown, sign out) and waits briefly for them to stop.</summary>
    public void CancelAll()
    {
        NetworkJob[] open;
        lock (_gate)
        {
            open = [.. _jobs.Where(j => !j.IsFinished && j.Kind != NetworkJobKind.Receive)];
        }
        foreach (NetworkJob j in open)
        {
            j.Cancel();
        }
        Task.WaitAll([.. open.Select(j => j.Completion)], TimeSpan.FromSeconds(2));
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        CancelAll();
        // Workers still inside a toolkit call (a connect that cannot be interrupted) end on their own; give
        // them a moment so nothing uses the store after it closes.
        SpinWait.SpinUntil(() => Volatile.Read(ref _running) == 0, TimeSpan.FromSeconds(20));
        _disposed = true;
        _timer.Dispose();
        _slots.Dispose();
    }
}
