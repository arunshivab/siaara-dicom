// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Imaging;

/// <summary>
/// Series thumbnails for the browser's preview strip: the middle instance (middle frame), fitted
/// into 128x128 by the toolkit renderer with the file's window, PNG-encoded by the toolkit. LRU of
/// 2,000 entries by SOP Instance UID.
/// </summary>
public sealed class ThumbnailService
{
    /// <summary>Thumbnail edge.</summary>
    public const int Size = 128;

    /// <summary>Entries kept.</summary>
    public const int Capacity = 2000;

    private readonly StoreService _store;
    private readonly ToolkitScheduler _scheduler;
    private readonly Lock _gate = new();
    private readonly LinkedList<string> _lru = new();
    private readonly Dictionary<string, (byte[] Png, LinkedListNode<string> Node)> _cache = new(StringComparer.Ordinal);

    /// <summary>Creates the service.</summary>
    /// <param name="store">Store.</param>
    /// <param name="scheduler">Scheduler.</param>
    public ThumbnailService(StoreService store, ToolkitScheduler scheduler)
    {
        _store = store;
        _scheduler = scheduler;
    }

    /// <summary>Thumbnails cached.</summary>
    public int Count
    {
        get
        {
            lock (_gate)
            {
                return _cache.Count;
            }
        }
    }

    /// <summary>PNG of a series, or null when it has no displayable image.</summary>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>PNG bytes.</returns>
    public async Task<byte[]?> GetSeriesThumbnailAsync(string seriesInstanceUid, CancellationToken cancellationToken)
    {
        IReadOnlyList<InstanceRow> inst = await _store.GetInstancesAsync(seriesInstanceUid, cancellationToken).ConfigureAwait(false);
        var images = inst.Where(i => i.HasPixelData).ToArray();
        if (images.Length == 0)
        {
            return null;
        }
        InstanceRow mid = images[images.Length / 2];
        lock (_gate)
        {
            if (_cache.TryGetValue(mid.SopInstanceUid, out var e))
            {
                _lru.Remove(e.Node);
                _lru.AddFirst(e.Node);
                return e.Png;
            }
        }
        byte[]? png = await _scheduler.RunAsync(WorkPool.StoreRead, _ => Render(mid), cancellationToken).ConfigureAwait(false);
        if (png is null)
        {
            return null;
        }
        lock (_gate)
        {
            if (!_cache.ContainsKey(mid.SopInstanceUid))
            {
                _cache[mid.SopInstanceUid] = (png, _lru.AddFirst(mid.SopInstanceUid));
                while (_cache.Count > Capacity && _lru.Last is LinkedListNode<string> last)
                {
                    _cache.Remove(last.Value);
                    _lru.RemoveLast();
                }
            }
        }
        return png;
    }

    /// <summary>Drops cached thumbnails of these instances (after a delete).</summary>
    /// <param name="sopInstanceUids">Instances.</param>
    public void Forget(IEnumerable<string> sopInstanceUids)
    {
        ArgumentNullException.ThrowIfNull(sopInstanceUids);
        lock (_gate)
        {
            foreach (string sop in sopInstanceUids)
            {
                if (_cache.Remove(sop, out var e))
                {
                    _lru.Remove(e.Node);
                }
            }
        }
    }

    private static byte[]? Render(InstanceRow row)
    {
        try
        {
            using TkFile f = TkFile.Open(row.FilePath, headersOnly: false);
            using TkFrame fr = TkFrame.Open(f, Math.Max(0, row.Frames / 2));
            var spec = new RenderSpec { Width = Size, Height = Size, Zoom = 0, Linear = true, Gray = false };
            byte[] px = fr.Render(spec);
            return Tk.EncodePng(px, Size, Size, gray: false);
        }
        catch (TkException)
        {
            return null;
        }
    }
}
