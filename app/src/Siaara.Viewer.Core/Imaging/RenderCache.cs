// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Collections.Concurrent;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Imaging;

/// <summary>
/// Shared caches of the viewer: full (pixel data) file handles, LRU by count, and decoded frames,
/// LRU by bytes. Decodes run on the Decode pool; concurrent requests for one frame share one decode.
/// Evicted handles are disposed; the bindings keep native memory alive for calls already running
/// on them, and a later call on an evicted handle raises ObjectDisposedException, which callers
/// answer by asking the cache again.
/// </summary>
public sealed class RenderCache : IDisposable
{
    private readonly ToolkitScheduler _scheduler;
    private readonly Lock _gate = new();
    private readonly LinkedList<FrameKey> _frameLru = new();
    private readonly Dictionary<FrameKey, (TkFrame Frame, LinkedListNode<FrameKey> Node)> _frames = [];
    private readonly LinkedList<string> _fileLru = new();
    private readonly Dictionary<string, (TkFile File, LinkedListNode<string> Node)> _files = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<FrameKey, Task<TkFrame>> _pending = new();
    private long _bytes;
    private int _prefetchQueued;

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

    /// <summary>Full handles kept open (viewports x per-viewport limit; adjusted by the layout).</summary>
    public int MaxFullHandles { get; set; }

    /// <summary>Bytes of decoded frames held.</summary>
    public long DecodedBytes => Interlocked.Read(ref _bytes);

    /// <summary>Frames decoded and held.</summary>
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

    /// <summary>Full handles open.</summary>
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
    public int DecodesInProgress => _pending.Count;

    /// <summary>Prefetch requests queued.</summary>
    public int PrefetchQueued => Volatile.Read(ref _prefetchQueued);

    /// <summary>The decoded frame when it is cached (no decode).</summary>
    /// <param name="key">Key.</param>
    /// <param name="frame">The frame.</param>
    /// <returns>True when cached.</returns>
    public bool TryGetDecoded(FrameKey key, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out TkFrame? frame)
    {
        lock (_gate)
        {
            if (_frames.TryGetValue(key, out var e))
            {
                _frameLru.Remove(e.Node);
                _frameLru.AddFirst(e.Node);
                frame = e.Frame;
                return true;
            }
        }
        frame = null;
        return false;
    }

    /// <summary>The decoded frame, decoding it on the Decode pool when needed.</summary>
    /// <param name="image">Image.</param>
    /// <param name="priority">Normal for the visible image, Background for prefetch.</param>
    /// <param name="cancellationToken">Token (a shared decode is not cancelled by one waiter).</param>
    /// <returns>The frame.</returns>
    public Task<TkFrame> GetFrameAsync(SeriesImage image, WorkPriority priority, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(image);
        if (TryGetDecoded(image.Key, out TkFrame? f))
        {
            return Task.FromResult(f);
        }
        Task<TkFrame> t = _pending.GetOrAdd(image.Key, _ =>
        {
            if (priority == WorkPriority.Background)
            {
                Interlocked.Increment(ref _prefetchQueued);
            }
            return _scheduler.RunAsync(WorkPool.Decode, _ =>
            {
                try
                {
                    return Decode(image);
                }
                finally
                {
                    if (priority == WorkPriority.Background)
                    {
                        Interlocked.Decrement(ref _prefetchQueued);
                    }
                    _pending.TryRemove(image.Key, out Task<TkFrame>? _);
                }
            }, priority, CancellationToken.None);
        });
        return t.WaitAsync(cancellationToken);
    }

    /// <summary>Queues background decodes (prefetch); already cached frames are skipped.</summary>
    /// <param name="images">Images in priority order.</param>
    public void Prefetch(IEnumerable<SeriesImage> images)
    {
        ArgumentNullException.ThrowIfNull(images);
        foreach (SeriesImage i in images)
        {
            if (!TryGetDecoded(i.Key, out _) && !_pending.ContainsKey(i.Key))
            {
                _ = GetFrameAsync(i, WorkPriority.Background, CancellationToken.None).ContinueWith(static t => _ = t.Exception,
                    CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.Default);
            }
        }
    }

    /// <summary>The full (pixel data) file of an instance, opening it when needed (call on a toolkit pool).</summary>
    /// <param name="instance">Instance.</param>
    /// <returns>The file.</returns>
    public TkFile GetFullFile(ImageInstance instance)
    {
        ArgumentNullException.ThrowIfNull(instance);
        lock (_gate)
        {
            if (_files.TryGetValue(instance.SopInstanceUid, out var e) && !e.File.IsDisposed)
            {
                _fileLru.Remove(e.Node);
                _fileLru.AddFirst(e.Node);
                return e.File;
            }
        }
        TkFile file = TkFile.Open(instance.FilePath, headersOnly: false);
        if (file.PixelInfo is TkPixelInfo pi && pi.IsLossy)
        {
            instance.IsLossy = true;
        }
        var evicted = new List<TkFile>();
        lock (_gate)
        {
            if (_files.TryGetValue(instance.SopInstanceUid, out var existing))
            {
                // Another thread opened it meanwhile: keep theirs.
                evicted.Add(file);
                file = existing.File;
            }
            else
            {
                LinkedListNode<string> node = _fileLru.AddFirst(instance.SopInstanceUid);
                _files[instance.SopInstanceUid] = (file, node);
                while (_files.Count > MaxFullHandles && _fileLru.Last is LinkedListNode<string> last)
                {
                    evicted.Add(_files[last.Value].File);
                    _files.Remove(last.Value);
                    _fileLru.RemoveLast();
                }
            }
        }
        foreach (TkFile e in evicted)
        {
            e.Dispose();
        }
        return file;
    }

    private TkFrame Decode(SeriesImage image)
    {
        for (int attempt = 0; ; attempt++)
        {
            try
            {
                TkFile full = GetFullFile(image.Instance);
                TkFrame frame = TkFrame.Open(full, image.Frame);
                Insert(image.Key, frame);
                return frame;
            }
            catch (ObjectDisposedException) when (attempt == 0)
            {
                // The full handle was evicted between lookup and decode: open it again once.
            }
        }
    }

    private void Insert(FrameKey key, TkFrame frame)
    {
        var evicted = new List<TkFrame>();
        lock (_gate)
        {
            if (_frames.TryGetValue(key, out var old))
            {
                evicted.Add(old.Frame);
                _frameLru.Remove(old.Node);
                _bytes -= old.Frame.Info.DecodedBytes;
            }
            _frames[key] = (frame, _frameLru.AddFirst(key));
            _bytes += frame.Info.DecodedBytes;
            while (_bytes > MaxDecodedBytes && _frames.Count > 1 && _frameLru.Last is LinkedListNode<FrameKey> last)
            {
                var e = _frames[last.Value];
                evicted.Add(e.Frame);
                _bytes -= e.Frame.Info.DecodedBytes;
                _frames.Remove(last.Value);
                _frameLru.RemoveLast();
            }
        }
        foreach (TkFrame f in evicted)
        {
            f.Dispose();
        }
    }

    /// <summary>Drops every cached frame and file of the given instances (after a delete).</summary>
    /// <param name="sopInstanceUids">Instances.</param>
    public void Forget(IEnumerable<string> sopInstanceUids)
    {
        ArgumentNullException.ThrowIfNull(sopInstanceUids);
        var set = sopInstanceUids.ToHashSet(StringComparer.Ordinal);
        var disposeFrames = new List<TkFrame>();
        var disposeFiles = new List<TkFile>();
        lock (_gate)
        {
            foreach (FrameKey k in _frames.Keys.Where(k => set.Contains(k.SopInstanceUid)).ToArray())
            {
                var e = _frames[k];
                disposeFrames.Add(e.Frame);
                _bytes -= e.Frame.Info.DecodedBytes;
                _frameLru.Remove(e.Node);
                _frames.Remove(k);
            }
            foreach (string s in _files.Keys.Where(set.Contains).ToArray())
            {
                disposeFiles.Add(_files[s].File);
                _fileLru.Remove(_files[s].Node);
                _files.Remove(s);
            }
        }
        disposeFrames.ForEach(f => f.Dispose());
        disposeFiles.ForEach(f => f.Dispose());
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        lock (_gate)
        {
            foreach (var e in _frames.Values)
            {
                e.Frame.Dispose();
            }
            foreach (var e in _files.Values)
            {
                e.File.Dispose();
            }
            _frames.Clear();
            _files.Clear();
            _frameLru.Clear();
            _fileLru.Clear();
            _bytes = 0;
        }
    }
}
