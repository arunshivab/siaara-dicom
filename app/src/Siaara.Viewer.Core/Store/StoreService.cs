// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Threading.Channels;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Store;

/// <summary>The store is open in another process (another SIAARA Viewer is running).</summary>
public sealed class StoreLockedException : Exception
{
    /// <summary>Creates the exception.</summary>
    public StoreLockedException() : base("The image store is in use by another process.")
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Message.</param>
    public StoreLockedException(string message) : base(message)
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Message.</param>
    /// <param name="innerException">Cause.</param>
    public StoreLockedException(string message, Exception innerException) : base(message, innerException)
    {
    }
}

/// <summary>
/// The app's view of the toolkit store. Queries run on the StoreRead pool, changes on the Import
/// pool. The store callback only posts into a <see cref="Channel{T}"/>; a pump drains it and raises
/// <see cref="StudiesChanged"/> in batches (Blazor subscribers marshal with InvokeAsync).
/// </summary>
public sealed class StoreService : IDisposable
{
    private readonly ToolkitScheduler _scheduler;
    private readonly Channel<StoreEvent> _events = Channel.CreateUnbounded<StoreEvent>(new UnboundedChannelOptions { SingleReader = true });
    private readonly CancellationTokenSource _stop = new();
    private readonly Task _pump;

    private StoreService(TkStore store, ToolkitScheduler scheduler, string root)
    {
        Store = store;
        _scheduler = scheduler;
        Root = root;
        Store.Changed += OnStoreChanged;
        _pump = Task.Run(PumpAsync);
    }

    /// <summary>Opens the store (create, durable, rebuild if corrupt).</summary>
    /// <param name="root">Store folder.</param>
    /// <param name="scheduler">Scheduler.</param>
    /// <returns>The service.</returns>
    /// <exception cref="StoreLockedException">Another process has the store open.</exception>
    public static StoreService Open(string root, ToolkitScheduler scheduler)
    {
        ArgumentNullException.ThrowIfNull(scheduler);
        try
        {
            return new StoreService(TkStore.Open(root, durable: true, rebuildIfCorrupt: true), scheduler, root);
        }
        catch (TkException e) when (e.Error == TkError.Rejected)
        {
            throw new StoreLockedException("The image store is in use by another process.", e);
        }
    }

    /// <summary>Store folder.</summary>
    public string Root { get; }

    internal TkStore Store { get; }

    /// <summary>Batched study changes (raised on a pool thread).</summary>
    public event EventHandler<StudiesChangedEventArgs>? StudiesChanged;

    /// <summary>Raw store events in arrival order (raised on the pump thread, after the batch event).</summary>
    public event EventHandler<StoreEventArgs>? EventReceived;

    private void OnStoreChanged(object? sender, TkStoreEventArgs e) =>
        _events.Writer.TryWrite(new StoreEvent(e.Kind, e.SopInstanceUid, e.SeriesInstanceUid, e.StudyInstanceUid));

    private async Task PumpAsync()
    {
        ChannelReader<StoreEvent> reader = _events.Reader;
        try
        {
            while (await reader.WaitToReadAsync(_stop.Token).ConfigureAwait(false))
            {
                // Collect what arrives within 100 ms into one notification (imports post one event per file).
                await Task.Delay(100, _stop.Token).ConfigureAwait(false);
                var studies = new HashSet<string>(StringComparer.Ordinal);
                var raw = new List<StoreEvent>();
                bool rebuilt = false;
                while (reader.TryRead(out StoreEvent ev))
                {
                    raw.Add(ev);
                    if (ev.Kind == TkStoreEventKind.Rebuilt)
                    {
                        rebuilt = true;
                    }
                    else if (ev.StudyInstanceUid.Length > 0)
                    {
                        studies.Add(ev.StudyInstanceUid);
                    }
                }
                Raise(new StudiesChangedEventArgs(studies, rebuilt), raw);
            }
        }
        catch (OperationCanceledException)
        {
            // Shutting down.
        }
    }

    private void Raise(StudiesChangedEventArgs e, List<StoreEvent> raw)
    {
        try
        {
            StudiesChanged?.Invoke(this, e);
            EventHandler<StoreEventArgs>? h = EventReceived;
            if (h is not null)
            {
                foreach (StoreEvent ev in raw)
                {
                    h(this, new StoreEventArgs(ev));
                }
            }
        }
#pragma warning disable CA1031 // A failing subscriber must not stop live updates for the others.
        catch (Exception ex)
#pragma warning restore CA1031
        {
            AppLog.Error("store", "store change subscriber failed", ex);
        }
    }

    /// <summary>A page of studies. The first page is queried with NO_TOTAL (Total is a lower bound); call <see cref="CountStudiesAsync"/> for the exact total.</summary>
    /// <param name="query">Filter and page.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>The page.</returns>
    public Task<StudyPage> QueryStudiesAsync(StudyQuery query, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(query);
        return _scheduler.RunAsync(WorkPool.StoreRead, ct => QueryStudies(query, exactTotal: false, ct), cancellationToken);
    }

    /// <summary>Exact number of studies matching the filter.</summary>
    /// <param name="query">Filter (paging ignored).</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Total.</returns>
    public Task<int> CountStudiesAsync(StudyQuery query, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(query);
        return _scheduler.RunAsync(WorkPool.StoreRead, ct => QueryStudies(query with { Offset = 0, Limit = 1 }, exactTotal: true, ct).Total,
            cancellationToken);
    }

    private static TkStoreFilter BaseFilter(StudyQuery q) => new()
    {
        Level = TkLevel.Study,
        StudyDate = q.DateFilter,
        Modality = q.ModalityFilter,
    };

    private StudyPage QueryStudies(StudyQuery q, bool exactTotal, CancellationToken ct)
    {
        TkStoreFilter f = BaseFilter(q);
        string text = q.Text.Trim();
        if (text.Length == 0)
        {
            TkQueryResult r = Store.Query(f with { Offset = q.Offset, Limit = q.Limit, NoTotal = !exactTotal });
            return new StudyPage(r.Rows.Select(x => new StudyRow(x)).ToArray(), r.Total, r.TotalExact);
        }
        // The store ANDs its filters; the search box ORs name / ID / accession. Each part is
        // queried in the store's own order (newest first), merged without duplicates in that order.
        string pattern = Escape(text);
        int want = exactTotal ? 0 : q.Offset + q.Limit;
        TkStoreFilter[] parts =
        [
            f with { PatientName = "*" + pattern + "*", Limit = want, NoTotal = !exactTotal },
            f with { PatientId = pattern + "*", Limit = want, NoTotal = !exactTotal },
            f with { AccessionNumber = pattern + "*", Limit = want, NoTotal = !exactTotal },
        ];
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var merged = new List<TkRecord>();
        bool exact = true;
        foreach (TkStoreFilter part in parts)
        {
            ct.ThrowIfCancellationRequested();
            TkQueryResult r = Store.Query(part);
            exact &= r.TotalExact;
            foreach (TkRecord row in r.Rows)
            {
                if (seen.Add(row.StudyInstanceUid))
                {
                    merged.Add(row);
                }
            }
        }
        merged.Sort(CompareStudies);
        var page = merged.Skip(q.Offset).Take(q.Limit).Select(x => new StudyRow(x)).ToArray();
        return new StudyPage(page, merged.Count, exactTotal && exact);
    }

    // Same order as the store: study date descending, then time descending, then UID.
    private static int CompareStudies(TkRecord a, TkRecord b)
    {
        int c = string.CompareOrdinal(b.StudyDate, a.StudyDate);
        if (c != 0)
        {
            return c;
        }
        c = string.CompareOrdinal(b.StudyTime, a.StudyTime);
        return c != 0 ? c : string.CompareOrdinal(a.StudyInstanceUid, b.StudyInstanceUid);
    }

    private static string Escape(string text) => text.Replace("*", "", StringComparison.Ordinal).Replace("?", "", StringComparison.Ordinal);

    /// <summary>One study, or null.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>The row.</returns>
    public Task<StudyRow?> GetStudyAsync(string studyInstanceUid, CancellationToken cancellationToken = default) =>
        _scheduler.RunAsync(WorkPool.StoreRead, _ =>
        {
            TkQueryResult r = Store.Query(new TkStoreFilter { Level = TkLevel.Study, StudyInstanceUid = studyInstanceUid, Limit = 1 });
            return r.Rows.Count > 0 ? new StudyRow(r.Rows[0]) : null;
        }, cancellationToken);

    /// <summary>Series of a study by Series Number.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Rows.</returns>
    public Task<IReadOnlyList<SeriesRow>> GetSeriesAsync(string studyInstanceUid, CancellationToken cancellationToken = default) =>
        _scheduler.RunAsync<IReadOnlyList<SeriesRow>>(WorkPool.StoreRead,
            _ => Store.GetSeries(studyInstanceUid).Select(r => new SeriesRow(r)).ToArray(), cancellationToken);

    /// <summary>Instances of a series with their files.</summary>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Rows.</returns>
    public Task<IReadOnlyList<InstanceRow>> GetInstancesAsync(string seriesInstanceUid, CancellationToken cancellationToken = default) =>
        _scheduler.RunAsync<IReadOnlyList<InstanceRow>>(WorkPool.StoreRead, _ => Store.GetInstances(seriesInstanceUid)
            .Select(r => new InstanceRow(r.SopInstanceUid, r.FilePath, r.InstanceNumber, Math.Max(1, r.NumberOfFrames), r.HasPixelData))
            .ToArray(), cancellationToken);

    /// <summary>Deletes a study (copied files are removed, referenced files are left).</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Instances removed.</returns>
    public Task<int> DeleteStudyAsync(string studyInstanceUid, CancellationToken cancellationToken = default) =>
        _scheduler.RunAsync(WorkPool.Import, _ => Store.Delete(TkLevel.Study, studyInstanceUid), cancellationToken);

    /// <summary>Deletes a patient by store patient key.</summary>
    /// <param name="patientKey">Key from <see cref="StudyRow.PatientKey"/>.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Instances removed.</returns>
    public Task<int> DeletePatientAsync(string patientKey, CancellationToken cancellationToken = default) =>
        _scheduler.RunAsync(WorkPool.Import, _ => Store.Delete(TkLevel.Patient, patientKey), cancellationToken);

    /// <summary>Store totals.</summary>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Totals.</returns>
    public Task<TkStoreStats> GetStatisticsAsync(CancellationToken cancellationToken = default) =>
        _scheduler.RunAsync(WorkPool.StoreRead, _ => Store.GetStatistics(), cancellationToken);

    /// <inheritdoc/>
    public void Dispose()
    {
        Store.Changed -= OnStoreChanged;
        _events.Writer.TryComplete();
        _stop.Cancel();
        try
        {
            _pump.Wait(TimeSpan.FromSeconds(2));
        }
        catch (AggregateException)
        {
            // The pump ends with cancellation.
        }
        Store.Dispose();
        _stop.Dispose();
    }
}

/// <summary>One raw store event.</summary>
public sealed class StoreEventArgs : EventArgs
{
    /// <summary>Creates the event.</summary>
    /// <param name="event">The event.</param>
    public StoreEventArgs(StoreEvent @event) => Event = @event;

    /// <summary>The event.</summary>
    public StoreEvent Event { get; }
}
