// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Imaging;

/// <summary>A cached object with a reference count: the cache holds one reference while the object
/// is cached, every lease one more. The object is disposed when the count reaches zero, so eviction
/// never disposes a frame or file another thread is using.</summary>
/// <typeparam name="TValue">Frame or file.</typeparam>
internal sealed class Counted<TValue> where TValue : class, IDisposable
{
    private int _refs = 1;

    public Counted(TValue value) => Value = value;

    public TValue Value { get; }

    public int References => Volatile.Read(ref _refs);

    // References taken for waiters at once (under the cache lock, when a decode completes).
    public void AddRefs(int n) => Interlocked.Add(ref _refs, n);

    // Adds a reference unless the object is already released (count 0).
    public bool TryAddRef()
    {
        while (true)
        {
            int r = Volatile.Read(ref _refs);
            if (r <= 0)
            {
                return false;
            }
            if (Interlocked.CompareExchange(ref _refs, r + 1, r) == r)
            {
                return true;
            }
        }
    }

    public void Release()
    {
        if (Interlocked.Decrement(ref _refs) == 0)
        {
            Value.Dispose();
        }
    }
}

/// <summary>Use of a cached decoded frame; dispose it when done. The frame stays valid until then, even if the cache evicts it.</summary>
public sealed class FrameLease : IDisposable
{
    private Counted<TkFrame>? _entry;

    internal FrameLease(Counted<TkFrame> entry)
    {
        _entry = entry;
        Frame = entry.Value;
    }

    /// <summary>The frame.</summary>
    public TkFrame Frame { get; }

    /// <inheritdoc/>
    public void Dispose() => Interlocked.Exchange(ref _entry, null)?.Release();
}

/// <summary>Use of a cached full (pixel data) file handle; dispose it when done.</summary>
public sealed class FileLease : IDisposable
{
    private Counted<TkFile>? _entry;

    internal FileLease(Counted<TkFile> entry)
    {
        _entry = entry;
        File = entry.Value;
    }

    /// <summary>The file.</summary>
    public TkFile File { get; }

    /// <inheritdoc/>
    public void Dispose() => Interlocked.Exchange(ref _entry, null)?.Release();
}

/// <summary>
/// Shared caches of the viewer: full (pixel data) file handles, LRU by count, and decoded frames,
/// LRU by bytes. Decodes run on the Decode pool; concurrent requests for one frame share one decode.
/// Callers get leases (<see cref="FrameLease"/>, <see cref="FileLease"/>): an evicted frame or file
/// is disposed only after the last lease on it is disposed.
/// </summary>
public sealed class RenderCache : IDisposable
{
    private readonly ToolkitScheduler _scheduler;
    private readonly Lock _gate = new();
    private readonly LinkedList<FrameKey> _frameLru = new();
    private readonly Dictionary<FrameKey, (Counted<TkFrame> Entry, LinkedListNode<FrameKey> Node)> _frames = [];
    private readonly LinkedList<string> _fileLru = new();
    private readonly Dictionary<string, (Counted<TkFile> Entry, LinkedListNode<string> Node)> _files = new(StringComparer.Ordinal);
    private readonly Dictionary<FrameKey, PendingDecode> _pending = [];
    private long _bytes;
    private int _prefetchQueued;
    private long _prefetchFailures;

    /// <summary>Creates the cache.</summary>
    /// <param name="scheduler">Scheduler.</param>
    /// <param name="maxDecodedBytes">Decoded frame budget.</param>
    /// <param name="maxFullHandles">Full file handles kept open.</param>
    public RenderCache(ToolkitScheduler scheduler, long maxDecodedBytes, int maxFullHandles)
    {
        _scheduler = scheduler;
        MaxDecodedBytes = maxDecodedBytes;
        MaxFullHandles = Math.Max(1, maxFullHandles);
    }

    /// <summary>Decoded frame budget in bytes.</summary>
    public long MaxDecodedBytes { get; }

    /// <summary>Full handles kept open (raised by sessions with large layouts).</summary>
    public int MaxFullHandles { get; set; }

    /// <summary>Bytes of decoded frames held.</summary>
    public long DecodedBytes => Interlocked.Read(ref _bytes);

    /// <summary>Decoded frames held.</summary>
    public int DecodedFrames
    {
        get
        {
            lock (_gate)
            {
                return _frames.Count;
            }
        }
    }

    /// <summary>Open full handles.</summary>
    public int FullHandles
    {
        get
        {
            lock (_gate)
            {
                return _files.Count;
            }
        }
    }

    /// <summary>Decodes running or queued.</summary>
    public int DecodesInProgress
    {
        get
        {
            lock (_gate)
            {
                return _pending.Count;
            }
        }
    }

    // A decode in progress and the callers waiting for it. When it completes, the frame is cached
    // and one reference per waiter is taken in the same locked step, so no eviction can release
    // it before the waiters hold their leases.
    private sealed class PendingDecode
    {
        public TaskCompletionSource<Counted<TkFrame>> Done { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int Waiters { get; set; }
    }

    /// <summary>Background (prefetch) decodes queued.</summary>
    public int PrefetchQueued => Volatile.Read(ref _prefetchQueued);

    /// <summary>Prefetch decodes that failed (the image is decoded again, and the error shown, when it is displayed).</summary>
    public long PrefetchFailures => Interlocked.Read(ref _prefetchFailures);

    /// <summary>A lease on a decoded frame if it is cached (marks it recently used).</summary>
    /// <param name="key">Frame.</param>
    /// <returns>The lease (dispose it), or null when not cached.</returns>
    public FrameLease? TryLease(FrameKey key)
    {
        lock (_gate)
        {
            if (_frames.TryGetValue(key, out var e) && e.Entry.TryAddRef())
            {
                _frameLru.Remove(e.Node);
                _frameLru.AddFirst(e.Node);
                return new FrameLease(e.Entry);
            }
        }
        return null;
    }

    /// <summary>A lease on a decoded frame, decoding it on the Decode pool if needed.</summary>
    /// <param name="image">Image.</param>
    /// <param name="priority">Normal for the frame on screen, Background for prefetch.</param>
    /// <param name="cancellationToken">Stops waiting (the shared decode continues for other waiters).</param>
    /// <returns>The lease; dispose it.</returns>
    public Task<FrameLease> GetFrameAsync(SeriesImage image, WorkPriority priority, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(image);
        PendingDecode pending;
        bool start;
        lock (_gate)
        {
            if (_frames.TryGetValue(image.Key, out var e) && e.Entry.TryAddRef())
            {
                _frameLru.Remove(e.Node);
                _frameLru.AddFirst(e.Node);
                return Task.FromResult(new FrameLease(e.Entry));
            }
            start = !_pending.TryGetValue(image.Key, out pending!);
            if (start)
            {
                pending = new PendingDecode();
                _pending[image.Key] = pending;
            }
            pending.Waiters++;
        }
        if (start)
        {
            StartDecode(image, pending, priority);
        }
        return WaitLeaseAsync(pending, cancellationToken);
    }

    private static async Task<FrameLease> WaitLeaseAsync(PendingDecode pending, CancellationToken cancellationToken)
    {
        try
        {
            // The reference for this waiter was taken when the decode completed.
            return new FrameLease(await pending.Done.Task.WaitAsync(cancellationToken).ConfigureAwait(false));
        }
        catch (OperationCanceledException) when (!pending.Done.Task.IsCanceled)
        {
            // Gave up waiting: drop the reference reserved for this waiter once the decode ends.
            _ = pending.Done.Task.ContinueWith(static t =>
            {
                if (t.IsCompletedSuccessfully)
                {
                    t.Result.Release();
                }
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            throw;
        }
    }

    private void StartDecode(SeriesImage image, PendingDecode pending, WorkPriority priority)
    {
        if (priority == WorkPriority.Background)
        {
            Interlocked.Increment(ref _prefetchQueued);
        }
        Task<int> run = _scheduler.RunAsync(WorkPool.Decode, _ =>
        {
            try
            {
                Counted<TkFrame> entry;
                using (FileLease full = GetFullFile(image.Instance))
                {
                    entry = new Counted<TkFrame>(TkFrame.Open(full.File, image.Frame));
                }
                Insert(image.Key, entry, pending);
                pending.Done.TrySetResult(entry);
            }
#pragma warning disable CA1031 // The failure goes to every waiter through the task.
            catch (Exception e)
#pragma warning restore CA1031
            {
                lock (_gate)
                {
                    _pending.Remove(image.Key);
                }
                pending.Done.TrySetException(e);
            }
            finally
            {
                if (priority == WorkPriority.Background)
                {
                    Interlocked.Decrement(ref _prefetchQueued);
                }
            }
            return 0;
        }, priority, CancellationToken.None);
        // The scheduler itself may refuse the work (shutting down): the waiters must hear about it.
        _ = run.ContinueWith(t =>
        {
            if (!t.IsCompletedSuccessfully)
            {
                lock (_gate)
                {
                    _pending.Remove(image.Key);
                }
                pending.Done.TrySetException(t.Exception?.GetBaseException() ?? new OperationCanceledException());
            }
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    /// <summary>Queues background decodes of frames not cached yet. Failures are counted (and logged
    /// by the caller's next foreground decode, which reports them on screen).</summary>
    /// <param name="images">Images in priority order.</param>
    public void Prefetch(IEnumerable<SeriesImage> images)
    {
        ArgumentNullException.ThrowIfNull(images);
        foreach (SeriesImage i in images)
        {
            PendingDecode pending;
            lock (_gate)
            {
                if (_pending.ContainsKey(i.Key) || _frames.ContainsKey(i.Key))
                {
                    continue;
                }
                pending = new PendingDecode();
                _pending[i.Key] = pending;
            }
            StartDecode(i, pending, WorkPriority.Background);
            _ = pending.Done.Task.ContinueWith(t =>
            {
                if (t.IsFaulted)
                {
                    _ = t.Exception;
                    Interlocked.Increment(ref _prefetchFailures);
                }
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }

    /// <summary>A lease on the full (pixel data) handle of an instance, opening it if needed.</summary>
    /// <param name="instance">Instance.</param>
    /// <returns>The lease; dispose it.</returns>
    public FileLease GetFullFile(ImageInstance instance)
    {
        ArgumentNullException.ThrowIfNull(instance);
        lock (_gate)
        {
            if (_files.TryGetValue(instance.SopInstanceUid, out var e) && e.Entry.TryAddRef())
            {
                _fileLru.Remove(e.Node);
                _fileLru.AddFirst(e.Node);
                return new FileLease(e.Entry);
            }
        }
        TkFile file = TkFile.Open(instance.FilePath, headersOnly: false);
        if (file.PixelInfo is TkPixelInfo pi && pi.IsLossy)
        {
            instance.IsLossy = true;
        }
        var opened = new Counted<TkFile>(file);
        var evicted = new List<Counted<TkFile>>();
        FileLease lease;
        lock (_gate)
        {
            if (_files.TryGetValue(instance.SopInstanceUid, out var existing) && existing.Entry.TryAddRef())
            {
                // Another thread opened it meanwhile: keep theirs.
                evicted.Add(opened);
                lease = new FileLease(existing.Entry);
            }
            else
            {
                if (_files.Remove(instance.SopInstanceUid, out var dead))
                {
                    _fileLru.Remove(dead.Node);
                }
                opened.TryAddRef();
                lease = new FileLease(opened);
                _files[instance.SopInstanceUid] = (opened, _fileLru.AddFirst(instance.SopInstanceUid));
                while (_files.Count > MaxFullHandles && _fileLru.Last is LinkedListNode<string> last)
                {
                    evicted.Add(_files[last.Value].Entry);
                    _files.Remove(last.Value);
                    _fileLru.RemoveLast();
                }
            }
        }
        foreach (Counted<TkFile> e in evicted)
        {
            e.Release();
        }
        return lease;
    }

    private void Insert(FrameKey key, Counted<TkFrame> entry, PendingDecode pending)
    {
        var evicted = new List<Counted<TkFrame>>();
        lock (_gate)
        {
            // One reference per waiter, taken together with the insert and the end of the pending decode.
            entry.AddRefs(pending.Waiters);
            _pending.Remove(key);
            if (_frames.TryGetValue(key, out var old))
            {
                evicted.Add(old.Entry);
                _frameLru.Remove(old.Node);
                _bytes -= old.Entry.Value.Info.DecodedBytes;
            }
            _frames[key] = (entry, _frameLru.AddFirst(key));
            _bytes += entry.Value.Info.DecodedBytes;
            while (_bytes > MaxDecodedBytes && _frames.Count > 1 && _frameLru.Last is LinkedListNode<FrameKey> last)
            {
                var e = _frames[last.Value];
                evicted.Add(e.Entry);
                _bytes -= e.Entry.Value.Info.DecodedBytes;
                _frames.Remove(last.Value);
                _frameLru.RemoveLast();
            }
        }
        foreach (Counted<TkFrame> f in evicted)
        {
            f.Release();
        }
    }

    /// <summary>Drops (and releases) frames and handles of these instances, e.g. after a delete. Leases still held keep them alive until disposed.</summary>
    /// <param name="sopInstanceUids">Instances.</param>
    public void Forget(IEnumerable<string> sopInstanceUids)
    {
        ArgumentNullException.ThrowIfNull(sopInstanceUids);
        var set = sopInstanceUids.ToHashSet(StringComparer.Ordinal);
        var frames = new List<Counted<TkFrame>>();
        var files = new List<Counted<TkFile>>();
        lock (_gate)
        {
            foreach (FrameKey k in _frames.Keys.Where(k => set.Contains(k.SopInstanceUid)).ToArray())
            {
                var e = _frames[k];
                frames.Add(e.Entry);
                _bytes -= e.Entry.Value.Info.DecodedBytes;
                _frameLru.Remove(e.Node);
                _frames.Remove(k);
            }
            foreach (string s in _files.Keys.Where(set.Contains).ToArray())
            {
                files.Add(_files[s].Entry);
                _fileLru.Remove(_files[s].Node);
                _files.Remove(s);
            }
        }
        frames.ForEach(f => f.Release());
        files.ForEach(f => f.Release());
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        var frames = new List<Counted<TkFrame>>();
        var files = new List<Counted<TkFile>>();
        lock (_gate)
        {
            frames.AddRange(_frames.Values.Select(e => e.Entry));
            files.AddRange(_files.Values.Select(e => e.Entry));
            _frames.Clear();
            _files.Clear();
            _frameLru.Clear();
            _fileLru.Clear();
            _bytes = 0;
        }
        frames.ForEach(f => f.Release());
        files.ForEach(f => f.Release());
    }
}
