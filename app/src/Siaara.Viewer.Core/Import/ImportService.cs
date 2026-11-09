// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Threading.Channels;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Import;

/// <summary>What an import job asks for.</summary>
/// <param name="Root">Folder to import.</param>
/// <param name="IndexInPlace">Reference the files where they are instead of copying them into the store.</param>
public sealed record ImportRequest(string Root, bool IndexInPlace = false);

/// <summary>A snapshot of import progress.</summary>
/// <param name="FilesSeen">Files looked at.</param>
/// <param name="Instances">DICOM instances found.</param>
/// <param name="Skipped">Non-DICOM files.</param>
/// <param name="Errors">Files that failed.</param>
/// <param name="Added">New instances in the store.</param>
/// <param name="Duplicates">Already in the store (kept).</param>
/// <param name="UsedDicomdir">The file list came from a DICOMDIR.</param>
/// <param name="Finished">The job is over.</param>
/// <param name="Cancelled">The job was cancelled.</param>
/// <param name="Elapsed">Time since start.</param>
public sealed record ImportProgress(long FilesSeen, long Instances, long Skipped, long Errors, long Added, long Duplicates, bool UsedDicomdir,
                                    bool Finished, bool Cancelled, TimeSpan Elapsed);

/// <summary>A file that could not be imported: path and plain-word status only, never dataset values.</summary>
/// <param name="Path">File.</param>
/// <param name="Status">What went wrong.</param>
public sealed record ImportError(string Path, string Status);

/// <summary>A running or finished import.</summary>
public sealed class ImportJob : IDisposable
{
    private readonly CancellationTokenSource _cts = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly ConcurrentQueue<ImportError> _errors = new();
    private long _seen, _instances, _skipped, _errorCount, _added, _duplicates;
    private volatile bool _finished;
    private volatile bool _usedDicomdir;

    internal ImportJob(ImportRequest request)
    {
        Request = request;
    }

    /// <summary>The request.</summary>
    public ImportRequest Request { get; }

    /// <summary>Completes when the job is over (never faults; see <see cref="Fault"/>).</summary>
    public Task Completion { get; internal set; } = Task.CompletedTask;

    /// <summary>A failure that stopped the whole job (plain words), or null.</summary>
    public string? Fault { get; internal set; }

    /// <summary>Errors so far (at most 1,000 kept).</summary>
    public IReadOnlyList<ImportError> Errors => [.. _errors];

    internal CancellationToken Token => _cts.Token;

    /// <summary>Asks the job to stop (stops within 200 ms: the scan callback refuses the next file).</summary>
    public void Cancel() => _cts.Cancel();

    /// <summary>Current progress.</summary>
    public ImportProgress Progress => new(Interlocked.Read(ref _seen), Interlocked.Read(ref _instances), Interlocked.Read(ref _skipped),
        Interlocked.Read(ref _errorCount), Interlocked.Read(ref _added), Interlocked.Read(ref _duplicates), _usedDicomdir, _finished,
        _cts.IsCancellationRequested, _clock.Elapsed);

    internal void Seen() => Interlocked.Increment(ref _seen);

    internal void Instance() => Interlocked.Increment(ref _instances);

    internal void Skip() => Interlocked.Increment(ref _skipped);

    internal void Added() => Interlocked.Increment(ref _added);

    internal void Duplicate() => Interlocked.Increment(ref _duplicates);

    internal void UsedDicomdir() => _usedDicomdir = true;

    internal void Error(string path, string status)
    {
        Interlocked.Increment(ref _errorCount);
        if (_errors.Count < 1000)
        {
            _errors.Enqueue(new ImportError(path, status));
        }
    }

    internal void Finish()
    {
        _clock.Stop();
        _finished = true;
    }

    /// <inheritdoc/>
    public void Dispose() => _cts.Dispose();
}

/// <summary>
/// Imports a folder into the store. With a DICOMDIR at the root (any letter case) its records are
/// read (files resolved case-insensitively); otherwise the folder is scanned (recursive, raw data
/// sets allowed, skipped files reported). Paths go through a bounded channel of 4,096 entries to a
/// single writer that adds each file with DUP_KEEP and COPY or REFERENCE. One job at a time.
/// </summary>
public sealed class ImportService
{
    private readonly StoreService _store;
    private readonly ToolkitScheduler _scheduler;
    private readonly AuditLog? _audit;
    private readonly Lock _gate = new();

    /// <summary>Creates the service.</summary>
    /// <param name="store">Store.</param>
    /// <param name="scheduler">Scheduler.</param>
    /// <param name="audit">Audit log, or null.</param>
    public ImportService(StoreService store, ToolkitScheduler scheduler, AuditLog? audit)
    {
        _store = store;
        _scheduler = scheduler;
        _audit = audit;
    }

    /// <summary>The current or last job.</summary>
    public ImportJob? Current { get; private set; }

    /// <summary>Finds a DICOMDIR directly in root, ignoring letter case.</summary>
    /// <param name="root">Folder.</param>
    /// <returns>The path or null.</returns>
    public static string? FindDicomdir(string root)
    {
        if (!Directory.Exists(root))
        {
            return null;
        }
        foreach (string f in Directory.EnumerateFiles(root))
        {
            if (string.Equals(Path.GetFileName(f), "DICOMDIR", StringComparison.OrdinalIgnoreCase))
            {
                return f;
            }
        }
        return null;
    }

    /// <summary>Starts a job.</summary>
    /// <param name="request">Request.</param>
    /// <returns>The job.</returns>
    /// <exception cref="InvalidOperationException">A job is already running.</exception>
    public ImportJob Start(ImportRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        lock (_gate)
        {
            if (Current is { Progress.Finished: false })
            {
                throw new InvalidOperationException("An import is already running.");
            }
            var job = new ImportJob(request);
            Current = job;
            job.Completion = RunAsync(job);
            return job;
        }
    }

    private async Task RunAsync(ImportJob job)
    {
        string root = Path.GetFullPath(job.Request.Root);
        var paths = Channel.CreateBounded<string>(new BoundedChannelOptions(4096) { SingleReader = true, SingleWriter = true, FullMode = BoundedChannelFullMode.Wait });
        Task producer = _scheduler.RunAsync(WorkPool.Import, ct => Produce(job, root, paths.Writer, ct), job.Token);
        Task consumer = _scheduler.RunAsync(WorkPool.Import, ct => Consume(job, paths.Reader, ct), job.Token);
        try
        {
            await producer.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Cancelled: the consumer sees the token too.
        }
        catch (TkException e)
        {
            job.Fault = e.Message;
        }
        catch (IOException e)
        {
            job.Fault = "the folder could not be read (" + e.GetType().Name + ")";
        }
        catch (UnauthorizedAccessException)
        {
            job.Fault = "access to the folder was denied";
        }
        finally
        {
            paths.Writer.TryComplete();
        }
        try
        {
            await consumer.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Cancelled.
        }
        job.Finish();
        ImportProgress p = job.Progress;
        AppLog.Info("import", string.Create(CultureInfo.InvariantCulture,
            $"import finished: seen {p.FilesSeen}, instances {p.Instances}, added {p.Added}, duplicates {p.Duplicates}, skipped {p.Skipped}, errors {p.Errors}, cancelled {p.Cancelled}, {p.Elapsed.TotalSeconds:F1} s"));
        _audit?.Append("import", new Dictionary<string, string>
        {
            ["root"] = root,
            ["mode"] = job.Request.IndexInPlace ? "reference" : "copy",
            ["dicomdir"] = p.UsedDicomdir ? "yes" : "no",
            ["seen"] = p.FilesSeen.ToString(CultureInfo.InvariantCulture),
            ["instances"] = p.Instances.ToString(CultureInfo.InvariantCulture),
            ["added"] = p.Added.ToString(CultureInfo.InvariantCulture),
            ["duplicates"] = p.Duplicates.ToString(CultureInfo.InvariantCulture),
            ["skipped"] = p.Skipped.ToString(CultureInfo.InvariantCulture),
            ["errors"] = p.Errors.ToString(CultureInfo.InvariantCulture),
            ["cancelled"] = p.Cancelled ? "yes" : "no",
        });
    }

    private static void Post(ChannelWriter<string> writer, string path, CancellationToken ct)
    {
        // Called on the scan thread: wait for room, but give up quickly on cancel.
        while (!writer.TryWrite(path))
        {
            ct.ThrowIfCancellationRequested();
            if (!writer.WaitToWriteAsync(ct).AsTask().GetAwaiter().GetResult())
            {
                return;
            }
        }
    }

    private static void Produce(ImportJob job, string root, ChannelWriter<string> writer, CancellationToken ct)
    {
        if (!Directory.Exists(root))
        {
            job.Fault = "the folder does not exist";
            return;
        }
        string? dicomdir = FindDicomdir(root);
        if (dicomdir is not null)
        {
            job.UsedDicomdir();
            job.Seen();
            Tk.ReadDicomdir(dicomdir, rec =>
            {
                if (rec.FilePath.Length == 0 && rec.FileStatus == TkError.None)
                {
                    return; // PATIENT / STUDY / SERIES records reference no file.
                }
                job.Seen();
                if (rec.FileStatus != TkError.None || rec.FilePath.Length == 0)
                {
                    job.Error(rec.FilePath.Length > 0 ? rec.FilePath : "(listed in DICOMDIR)",
                        rec.FileStatus == TkError.NotFound ? "file listed in the DICOMDIR is missing" : Tk.Describe(rec.FileStatus));
                    return;
                }
                job.Instance();
                Post(writer, rec.FilePath, ct);
            }, ct);
            return;
        }
        Tk.ScanFolder(root, item =>
        {
            job.Seen();
            switch (item.Kind)
            {
                case TkScanKind.Instance:
                    job.Instance();
                    Post(writer, item.Path, ct);
                    break;
                case TkScanKind.NotDicom:
                case TkScanKind.Dicomdir:
                    job.Skip();
                    break;
                default:
                    job.Error(item.Path, Tk.Describe(item.Status));
                    break;
            }
        }, reportSkipped: true, ct);
    }

    private void Consume(ImportJob job, ChannelReader<string> reader, CancellationToken ct)
    {
        while (true)
        {
            string? path;
            while (!reader.TryRead(out path))
            {
                ct.ThrowIfCancellationRequested();
                if (!reader.WaitToReadAsync(ct).AsTask().GetAwaiter().GetResult())
                {
                    return;
                }
            }
            ct.ThrowIfCancellationRequested();
            try
            {
                TkAddOutcome o = _store.Store.AddFile(path, job.Request.IndexInPlace);
                if (o == TkAddOutcome.KeptExisting)
                {
                    job.Duplicate();
                }
                else
                {
                    job.Added();
                }
            }
            catch (TkException e)
            {
                job.Error(path, e.Message);
            }
        }
    }
}
