// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Collections.Concurrent;

namespace Siaara.Viewer.Core.Toolkit;

/// <summary>Worker pools for toolkit calls.</summary>
public enum WorkPool
{
    /// <summary>Renders for the visible viewports (cores/2, above-normal priority).</summary>
    Interactive = 0,
    /// <summary>Header reads, frame decodes and prefetch (cores/2, below-normal priority).</summary>
    Decode = 1,
    /// <summary>Import: one scan producer plus one store writer (one import job at a time).</summary>
    Import = 2,
    /// <summary>Store queries and thumbnails (cores/4).</summary>
    StoreRead = 3,
}

/// <summary>Priority inside a pool: Normal work runs before Background work (prefetch).</summary>
public enum WorkPriority
{
    /// <summary>On demand.</summary>
    Normal = 0,
    /// <summary>Prefetch and other speculative work.</summary>
    Background = 1,
}

/// <summary>
/// Bounded, dedicated thread pools for every toolkit call, so Kestrel and Blazor threads never wait
/// on slow native work and a long import cannot starve rendering. Work is a delegate that receives
/// the cancellation token (passed on to the bindings, which tie it to native cancel tokens where
/// the C API has one); cancelled work that has not started never runs.
/// </summary>
public sealed class ToolkitScheduler : IDisposable
{
    private readonly List<Pool> _pools = new(4);

    /// <summary>Creates the pools.</summary>
    /// <param name="cores">Processor count to size for (default: this machine).</param>
    public ToolkitScheduler(int? cores = null)
    {
        int n = Math.Max(1, cores ?? Environment.ProcessorCount);
        // Order matches WorkPool.
        _pools.Add(new Pool("tk-interactive", Math.Max(1, n / 2), ThreadPriority.AboveNormal));
        _pools.Add(new Pool("tk-decode", Math.Max(1, n / 2), ThreadPriority.BelowNormal));
        _pools.Add(new Pool("tk-import", 2, ThreadPriority.BelowNormal));
        _pools.Add(new Pool("tk-store", Math.Max(1, n / 4), ThreadPriority.Normal));
    }

    /// <summary>Threads of a pool.</summary>
    /// <param name="pool">Pool.</param>
    /// <returns>Thread count.</returns>
    public int GetThreadCount(WorkPool pool) => _pools[(int)pool].Threads;

    /// <summary>Queued plus running items of a pool.</summary>
    /// <param name="pool">Pool.</param>
    /// <returns>Count.</returns>
    public int GetLoad(WorkPool pool) => _pools[(int)pool].Load;

    /// <summary>Runs work on a pool at normal priority.</summary>
    /// <typeparam name="T">Result type.</typeparam>
    /// <param name="pool">Pool.</param>
    /// <param name="work">Work; gets the token.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>The result.</returns>
    public Task<T> RunAsync<T>(WorkPool pool, Func<CancellationToken, T> work, CancellationToken cancellationToken = default) =>
        RunAsync(pool, work, WorkPriority.Normal, cancellationToken);

    /// <summary>Runs work on a pool.</summary>
    /// <typeparam name="T">Result type.</typeparam>
    /// <param name="pool">Pool.</param>
    /// <param name="work">Work; gets the token.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <param name="priority">Priority inside the pool.</param>
    /// <returns>The result.</returns>
    public Task<T> RunAsync<T>(WorkPool pool, Func<CancellationToken, T> work, WorkPriority priority, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(work);
        if (cancellationToken.IsCancellationRequested)
        {
            return Task.FromCanceled<T>(cancellationToken);
        }
        var item = new Item<T>(work, cancellationToken);
        _pools[(int)pool].Post(item, priority);
        return item.Task;
    }

    /// <summary>Runs work without a result on a pool.</summary>
    /// <param name="pool">Pool.</param>
    /// <param name="work">Work.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Completion.</returns>
    public Task RunAsync(WorkPool pool, Action<CancellationToken> work, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(work);
        return RunAsync(pool, ct =>
        {
            work(ct);
            return true;
        }, WorkPriority.Normal, cancellationToken);
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        foreach (Pool p in _pools)
        {
            p.Dispose();
        }
    }

    private abstract class WorkItem
    {
        public abstract void Execute();

        public abstract void Abandon();
    }

    private sealed class Item<T> : WorkItem
    {
        private readonly Func<CancellationToken, T> _work;
        private readonly CancellationToken _token;
        private readonly TaskCompletionSource<T> _tcs = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public Item(Func<CancellationToken, T> work, CancellationToken token)
        {
            _work = work;
            _token = token;
        }

        public Task<T> Task => _tcs.Task;

        public override void Execute()
        {
            if (_token.IsCancellationRequested)
            {
                _tcs.TrySetCanceled(_token);
                return;
            }
            try
            {
                _tcs.TrySetResult(_work(_token));
            }
            catch (OperationCanceledException e)
            {
                _tcs.TrySetCanceled(e.CancellationToken.IsCancellationRequested ? e.CancellationToken : _token);
            }
#pragma warning disable CA1031 // A worker thread must survive any failure of one item; the exception goes to the awaiting caller.
            catch (Exception e)
#pragma warning restore CA1031
            {
                _tcs.TrySetException(e);
            }
        }

        public override void Abandon() => _tcs.TrySetCanceled();
    }

    private sealed class Pool : IDisposable
    {
        private readonly ConcurrentQueue<WorkItem> _normal = new();
        private readonly ConcurrentQueue<WorkItem> _background = new();
        private readonly SemaphoreSlim _signal = new(0);
        private readonly CancellationTokenSource _stop = new();
        private readonly Thread[] _threads;
        private int _load;

        public Pool(string name, int threads, ThreadPriority priority)
        {
            _threads = new Thread[threads];
            for (int i = 0; i < threads; i++)
            {
                _threads[i] = new Thread(Loop) { IsBackground = true, Name = name + "-" + i.ToString(System.Globalization.CultureInfo.InvariantCulture), Priority = priority };
                _threads[i].Start();
            }
        }

        public int Threads => _threads.Length;

        public int Load => Volatile.Read(ref _load);

        public void Post(WorkItem item, WorkPriority priority)
        {
            if (_stop.IsCancellationRequested)
            {
                item.Abandon();
                return;
            }
            Interlocked.Increment(ref _load);
            (priority == WorkPriority.Normal ? _normal : _background).Enqueue(item);
            _signal.Release();
        }

        private void Loop()
        {
            while (true)
            {
                try
                {
                    _signal.Wait(_stop.Token);
                }
                catch (OperationCanceledException)
                {
                    return;
                }
                if (_normal.TryDequeue(out WorkItem? item) || _background.TryDequeue(out item))
                {
                    try
                    {
                        item.Execute();
                    }
                    finally
                    {
                        Interlocked.Decrement(ref _load);
                    }
                }
            }
        }

        public void Dispose()
        {
            _stop.Cancel();
            foreach (Thread t in _threads)
            {
                t.Join(TimeSpan.FromSeconds(2));
            }
            while (_normal.TryDequeue(out WorkItem? i) || _background.TryDequeue(out i))
            {
                i.Abandon();
            }
            _signal.Dispose();
            _stop.Dispose();
        }
    }
}
