using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;

namespace Siaara;

/// <summary>The areas of a store that hold instances which arrived incomplete (SIAARA_AREA_*).</summary>
public enum QuarantineArea
{
    /// <summary>Kept for a decision: release to the archive or delete.</summary>
    Quarantine = C.SIAARA_AREA_QUARANTINE,
    /// <summary>Copies of refused instances, for diagnosis.</summary>
    Rejected = C.SIAARA_AREA_REJECTED,
}

/// <summary>One item of a quarantine area (siaara_quarantine_record). No patient data beyond the UIDs.</summary>
/// <param name="Id">SHA-256 of the bytes (hex).</param>
/// <param name="Area">Which area.</param>
/// <param name="Time">When it was kept.</param>
/// <param name="Source">"C-STORE", "C-GET", "STOW-RS" or "WADO-RS".</param>
/// <param name="CallingAeTitle">The sender's AE title (empty for DICOMweb).</param>
/// <param name="PeerAddress">Network address when known.</param>
/// <param name="SopClassUid">SOP Class UID when readable.</param>
/// <param name="SopInstanceUid">SOP Instance UID when readable.</param>
/// <param name="Size">Bytes.</param>
/// <param name="ReadLoss">SIAARA_READ_* loss bits.</param>
/// <param name="Warning">The first read warning.</param>
/// <param name="RecordMissing">The item's bytes are there but its record is not (e.g. a crash between the two
/// writes): only <paramref name="Id"/>, <paramref name="Size"/> and <paramref name="Time"/> are known. Such items are
/// kept, never deleted silently; release, export or delete them like any other.</param>
public sealed record QuarantineItem(string Id, QuarantineArea Area, DateTimeOffset Time, string Source, string CallingAeTitle,
                                    string PeerAddress, string SopClassUid, string SopInstanceUid, long Size, int ReadLoss, string Warning,
                                    bool RecordMissing = false);

/// <summary>Options for opening a store (siaara_store_options).</summary>
public sealed record DicomStoreOptions
{
    /// <summary>Create the folder and index when missing (default true).</summary>
    public bool Create { get; init; } = true;
    /// <summary>fsync every change (slower, survives power loss).</summary>
    public bool Durable { get; init; }
    /// <summary>Re-index the data folder when the index is unreadable.</summary>
    public bool RebuildIfCorrupt { get; init; }
    /// <summary>Journal records before compaction; 0 = default (50,000).</summary>
    public long CompactMinRecords { get; init; }
}

/// <summary>What to do when an added instance is already in the store.</summary>
public enum DuplicatePolicy
{
    /// <summary>Keep the stored instance.</summary>
    Keep = C.SIAARA_DUP_KEEP,
    /// <summary>Replace it.</summary>
    Replace = C.SIAARA_DUP_REPLACE,
}

/// <summary>How a file is added.</summary>
public enum AddMode
{
    /// <summary>Copy the file into the store.</summary>
    Copy = C.SIAARA_ADD_COPY,
    /// <summary>Index the file in place.</summary>
    Reference = C.SIAARA_ADD_REFERENCE,
}

/// <summary>Result of an add.</summary>
public enum AddOutcome
{
    /// <summary>A new instance was added.</summary>
    Added = C.SIAARA_ADD_ADDED,
    /// <summary>An existing instance was replaced.</summary>
    Replaced = C.SIAARA_ADD_REPLACED,
    /// <summary>The existing instance was kept.</summary>
    KeptExisting = C.SIAARA_ADD_KEPT_EXISTING,
}

/// <summary>Kind of a store change.</summary>
public enum StoreChangeKind
{
    /// <summary>An instance was added.</summary>
    Added = C.SIAARA_EVENT_ADDED,
    /// <summary>An instance was replaced.</summary>
    Replaced = C.SIAARA_EVENT_REPLACED,
    /// <summary>An instance was deleted.</summary>
    Deleted = C.SIAARA_EVENT_DELETED,
    /// <summary>The index was rebuilt (UIDs are empty).</summary>
    Rebuilt = C.SIAARA_EVENT_REBUILT,
}

/// <summary>Data of <see cref="DicomStore.Changed"/>.</summary>
public sealed class StoreChangedEventArgs : EventArgs
{
    internal StoreChangedEventArgs(StoreChangeKind kind, string sop, string series, string study)
    {
        Kind = kind;
        SopInstanceUid = sop;
        SeriesInstanceUid = series;
        StudyInstanceUid = study;
    }

    /// <summary>The change.</summary>
    public StoreChangeKind Kind { get; }
    /// <summary>SOP Instance UID ("" for Rebuilt).</summary>
    public string SopInstanceUid { get; }
    /// <summary>Series Instance UID ("" for Rebuilt).</summary>
    public string SeriesInstanceUid { get; }
    /// <summary>Study Instance UID ("" for Rebuilt).</summary>
    public string StudyInstanceUid { get; }
}

/// <summary>
/// Store query filter (siaara_store_filter). Text filters accept the C-FIND wildcards * and ?;
/// names and descriptions match case- and accent-insensitively; dates take a date or a range
/// "YYYYMMDD-YYYYMMDD"; Modality may list several values separated by backslashes; UIDs are exact.
/// </summary>
public sealed record DicomStoreQuery
{
    /// <summary>Level of the rows returned.</summary>
    public DicomLevel Level { get; init; } = DicomLevel.Study;
    /// <summary>Patient's Name.</summary>
    public string? PatientName { get; init; }
    /// <summary>Patient ID.</summary>
    public string? PatientId { get; init; }
    /// <summary>Patient's Birth Date.</summary>
    public string? PatientBirthDate { get; init; }
    /// <summary>Patient's Sex.</summary>
    public string? PatientSex { get; init; }
    /// <summary>Study Date (or range).</summary>
    public string? StudyDate { get; init; }
    /// <summary>Accession Number.</summary>
    public string? AccessionNumber { get; init; }
    /// <summary>Study Description.</summary>
    public string? StudyDescription { get; init; }
    /// <summary>Referring Physician's Name.</summary>
    public string? ReferringPhysician { get; init; }
    /// <summary>Modality (backslash-separated list allowed).</summary>
    public string? Modality { get; init; }
    /// <summary>Series Description.</summary>
    public string? SeriesDescription { get; init; }
    /// <summary>Body Part Examined.</summary>
    public string? BodyPart { get; init; }
    /// <summary>Study Instance UID.</summary>
    public string? StudyInstanceUid { get; init; }
    /// <summary>Series Instance UID.</summary>
    public string? SeriesInstanceUid { get; init; }
    /// <summary>SOP Instance UID.</summary>
    public string? SopInstanceUid { get; init; }
    /// <summary>Rows to skip (paging).</summary>
    public int Offset { get; init; }
    /// <summary>Maximum rows; 0 = all.</summary>
    public int Limit { get; init; }
    /// <summary>Stop as soon as Offset + Limit rows are known (Total is then a lower bound).</summary>
    public bool NoTotal { get; init; }
}

/// <summary>Rows of a store query (a snapshot).</summary>
public sealed class DicomStoreRecordCollection : IReadOnlyList<DicomIndexRecord>
{
    private readonly DicomIndexRecord[] _rows;

    internal DicomStoreRecordCollection(DicomIndexRecord[] rows, int total, bool totalExact)
    {
        _rows = rows;
        Total = total;
        IsTotalExact = totalExact;
    }

    /// <summary>Matches before offset / limit.</summary>
    public int Total { get; }

    /// <summary>False when <see cref="DicomStoreQuery.NoTotal"/> stopped early (Total is a lower bound).</summary>
    public bool IsTotalExact { get; }

    /// <inheritdoc/>
    public int Count => _rows.Length;

    /// <inheritdoc/>
    public DicomIndexRecord this[int index] => _rows[index];

    /// <inheritdoc/>
    public IEnumerator<DicomIndexRecord> GetEnumerator() => ((IEnumerable<DicomIndexRecord>)_rows).GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => _rows.GetEnumerator();
}

/// <summary>Result of <see cref="DicomStore.Verify"/>.</summary>
/// <param name="Checked">Instances checked.</param>
/// <param name="Missing">Indexed, file absent.</param>
/// <param name="SizeMismatch">File size differs.</param>
/// <param name="ContentMismatch">Deep check: another instance or unreadable.</param>
/// <param name="Orphans">Files the index does not know.</param>
/// <param name="Repaired">Problems repaired.</param>
public sealed record DicomStoreVerifyReport(int Checked, int Missing, int SizeMismatch, int ContentMismatch, int Orphans, int Repaired);

/// <summary>Store statistics (siaara_store_stats).</summary>
/// <param name="Patients">Patients.</param>
/// <param name="Studies">Studies.</param>
/// <param name="Series">Series.</param>
/// <param name="Instances">Instances.</param>
/// <param name="DataBytes">Bytes of stored files.</param>
/// <param name="JournalBytes">Journal size.</param>
/// <param name="JournalRecords">Journal records.</param>
/// <param name="SnapshotBytes">Snapshot size.</param>
/// <param name="Generation">Index generation.</param>
/// <param name="ReplayedRecords">Journal records applied at open.</param>
/// <param name="RecoveredBytes">Torn journal tail removed at open.</param>
public sealed record DicomStoreStatistics(long Patients, long Studies, long Series, long Instances, long DataBytes,
    long JournalBytes, long JournalRecords, long SnapshotBytes, long Generation, long ReplayedRecords, long RecoveredBytes);

/// <summary>
/// A local instance store with a crash-safe index (siaara_store). May be used from many threads.
/// Only one process may open a store folder at a time.
/// </summary>
/// <remarks>
/// <see cref="Changed"/> is raised after each change commits, on the thread that made the change,
/// with no store lock held (handlers may query or change the store). In a Blazor app, marshal to
/// the UI with InvokeAsync. An exception thrown by a handler is caught at the native boundary and
/// rethrown from the store method that caused the change (after the change has committed).
/// </remarks>
public sealed class DicomStore : IDisposable
{
    private readonly StoreHandle _handle;

    internal StoreHandle Handle => _handle;
    private readonly EventSink _sink = new();
    private readonly Lock _gate = new();

    private DicomStore(StoreHandle handle) => _handle = handle;

    /// <summary>Opens (or creates) a store.</summary>
    /// <param name="root">Store folder.</param>
    /// <param name="options">Options, or null for the defaults.</param>
    /// <returns>The store.</returns>
    /// <exception cref="SiaaraException">Rejected when another process has it open.</exception>
    public static DicomStore Open(string root, DicomStoreOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(root);
        byte[] r = Status.Utf8(root);
        int st;
        nint p;
        if (options is null)
        {
            st = Native.siaara_store_open(r, in Unsafe.NullRef<siaara_store_options>(), out p);
        }
        else
        {
            var o = new siaara_store_options
            {
                struct_size = StructSize.Of<siaara_store_options>(),
                create = options.Create ? 1 : 0,
                durable = options.Durable ? 1 : 0,
                rebuild_if_corrupt = options.RebuildIfCorrupt ? 1 : 0,
                compact_min_records = (ulong)Math.Max(0, options.CompactMinRecords),
            };
            st = Native.siaara_store_open(r, in o, out p);
        }
        Status.Check(st);
        var h = new StoreHandle();
        h.Attach(p);
        return new DicomStore(h);
    }

    /// <summary>Raised after each committed change (see the class remarks).</summary>
    public event EventHandler<StoreChangedEventArgs>? Changed
    {
        add
        {
            lock (_gate)
            {
                if (!_handle.HasCallback)
                {
                    Status.Check(_handle.InstallCallback(_sink));
                }
                _sink.Sender = new WeakReference<DicomStore>(this);
                _sink.Handlers += value;
            }
        }
        remove
        {
            lock (_gate)
            {
                _sink.Handlers -= value;
            }
        }
    }

    /// <summary>Adds a file.</summary>
    /// <param name="path">File path.</param>
    /// <param name="policy">Duplicate policy.</param>
    /// <param name="mode">Copy or reference in place.</param>
    /// <returns>What happened.</returns>
    public AddOutcome AddFile(string path, DuplicatePolicy policy = DuplicatePolicy.Keep, AddMode mode = AddMode.Copy)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        int st, outcome;
        using (var l = new HandleLease(_handle))
        {
            st = Native.siaara_store_add_file(l.Pointer, Status.Utf8(path), (int)policy, (int)mode, out outcome);
        }
        Finish(st);
        return (AddOutcome)outcome;
    }

    /// <summary>Adds an encoded instance (copied; a raw data set is stored as a Part 10 file).</summary>
    /// <param name="data">The bytes.</param>
    /// <param name="policy">Duplicate policy.</param>
    /// <returns>What happened.</returns>
    public AddOutcome AddMemory(ReadOnlySpan<byte> data, DuplicatePolicy policy = DuplicatePolicy.Keep)
    {
        int st, outcome;
        using (var l = new HandleLease(_handle))
        {
            st = Native.siaara_store_add_memory(l.Pointer, data, (int)policy, out outcome);
        }
        Finish(st);
        return (AddOutcome)outcome;
    }

    /// <summary>Queries the index.</summary>
    /// <param name="query">The filter.</param>
    /// <returns>The rows.</returns>
    public DicomStoreRecordCollection Query(DicomStoreQuery query)
    {
        ArgumentNullException.ThrowIfNull(query);
        using var strings = new NativeAllocations();
        var f = new siaara_store_filter
        {
            struct_size = StructSize.Of<siaara_store_filter>(),
            level = (int)query.Level,
            patient_name = strings.Utf8(query.PatientName),
            patient_id = strings.Utf8(query.PatientId),
            patient_birth_date = strings.Utf8(query.PatientBirthDate),
            patient_sex = strings.Utf8(query.PatientSex),
            study_date = strings.Utf8(query.StudyDate),
            accession_number = strings.Utf8(query.AccessionNumber),
            study_description = strings.Utf8(query.StudyDescription),
            referring_physician = strings.Utf8(query.ReferringPhysician),
            modality = strings.Utf8(query.Modality),
            series_description = strings.Utf8(query.SeriesDescription),
            body_part = strings.Utf8(query.BodyPart),
            study_uid = strings.Utf8(query.StudyInstanceUid),
            series_uid = strings.Utf8(query.SeriesInstanceUid),
            sop_instance_uid = strings.Utf8(query.SopInstanceUid),
            offset = (uint)Math.Max(0, query.Offset),
            limit = (uint)Math.Max(0, query.Limit),
            flags = query.NoTotal ? C.SIAARA_QUERY_NO_TOTAL : 0u,
        };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_query(l.Pointer, in f, out nint result));
        return ReadResult(result);
    }

    /// <summary>Queries on a thread-pool thread (large stores; see the README for when to prefer Query).</summary>
    /// <param name="query">The filter.</param>
    /// <param name="cancellationToken">Cancels before the query starts.</param>
    /// <returns>The rows.</returns>
    public Task<DicomStoreRecordCollection> QueryAsync(DicomStoreQuery query, CancellationToken cancellationToken = default) =>
        Task.Run(() => Query(query), cancellationToken);

    /// <summary>Series of a study, ordered by Series Number, with file paths.</summary>
    /// <param name="studyInstanceUid">Study Instance UID.</param>
    /// <returns>The rows.</returns>
    public DicomStoreRecordCollection GetSeries(string studyInstanceUid)
    {
        ArgumentException.ThrowIfNullOrEmpty(studyInstanceUid);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_get_series(l.Pointer, Status.Utf8(studyInstanceUid), out nint result));
        return ReadResult(result);
    }

    /// <summary>Instances of a series, ordered by Instance Number then slice position, with file paths.</summary>
    /// <param name="seriesInstanceUid">Series Instance UID.</param>
    /// <returns>The rows.</returns>
    public DicomStoreRecordCollection GetInstances(string seriesInstanceUid)
    {
        ArgumentException.ThrowIfNullOrEmpty(seriesInstanceUid);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_get_instances(l.Pointer, Status.Utf8(seriesInstanceUid), out nint result));
        return ReadResult(result);
    }

    private static DicomStoreRecordCollection ReadResult(nint pointer)
    {
        using var h = new StoreResultHandle();
        h.Attach(pointer);
        using var l = new HandleLease(h);
        Status.Check(Native.siaara_store_result_count(l.Pointer, out uint count, out uint total, out int exact));
        var rows = new DicomIndexRecord[count];
        for (uint i = 0; i < count; i++)
        {
            var key = (Res: l.Pointer, Index: i);
            string patientKey = Status.ReadString(key, static (s, b, out n) => Native.siaara_store_patient_key(s.Res, s.Index, b, out n));
            rows[i] = DicomIndex.Read(key, static ((nint Res, uint Index) s, ref siaara_index_record r, Span<byte> b, out nuint n) =>
                Native.siaara_store_result_get(s.Res, s.Index, ref r, b, out n), patientKey);
        }
        return new DicomStoreRecordCollection(rows, (int)total, exact != 0);
    }

    /// <summary>Deletes a patient (key from <see cref="DicomIndexRecord.PatientKey"/>), study, series or instance.
    /// Copied files are deleted; referenced files are left alone.</summary>
    /// <param name="level">Level of key.</param>
    /// <param name="key">Patient key or UID.</param>
    /// <returns>Instances removed.</returns>
    public int Delete(DicomLevel level, string key)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        int st;
        uint removed;
        using (var l = new HandleLease(_handle))
        {
            st = Native.siaara_store_delete(l.Pointer, (int)level, Status.Utf8(key), out removed);
        }
        Finish(st);
        return (int)removed;
    }

    /// <summary>Copies instances into destination (replacing duplicates) and removes them from this store.</summary>
    /// <param name="destination">Destination store.</param>
    /// <param name="level">Level of key.</param>
    /// <param name="key">Patient key or UID.</param>
    /// <returns>Instances moved.</returns>
    public int Move(DicomStore destination, DicomLevel level, string key)
    {
        ArgumentNullException.ThrowIfNull(destination);
        ArgumentException.ThrowIfNullOrEmpty(key);
        int st;
        uint moved;
        using (var l = new HandleLease(_handle))
        using (var d = new HandleLease(destination._handle))
        {
            st = Native.siaara_store_move(l.Pointer, d.Pointer, (int)level, Status.Utf8(key), out moved);
        }
        Finish(st);
        return (int)moved;
    }

    /// <summary>Checks the index against the files (blocking; see <see cref="VerifyAsync"/>).</summary>
    /// <param name="deep">Re-read every file.</param>
    /// <param name="repair">Fix what can be fixed.</param>
    /// <returns>The report.</returns>
    public DicomStoreVerifyReport Verify(bool deep = false, bool repair = false)
    {
        var r = new siaara_store_verify_report { struct_size = StructSize.Of<siaara_store_verify_report>() };
        int st;
        using (var l = new HandleLease(_handle))
        {
            st = Native.siaara_store_verify(l.Pointer, deep ? 1 : 0, repair ? 1 : 0, ref r);
        }
        Finish(st);
        return new DicomStoreVerifyReport((int)r.@checked, (int)r.missing, (int)r.size_mismatch, (int)r.content_mismatch,
            (int)r.orphans, (int)r.repaired);
    }

    /// <summary>Verify on a thread-pool thread (it reads every file: long-running and blocking).</summary>
    /// <param name="deep">Re-read every file.</param>
    /// <param name="repair">Fix what can be fixed.</param>
    /// <param name="cancellationToken">Cancels, also while running (a cancelled verify repairs nothing).</param>
    /// <param name="progress">Instances checked / total (optional).</param>
    /// <returns>The report.</returns>
    public Task<DicomStoreVerifyReport> VerifyAsync(bool deep = false, bool repair = false, IProgress<OperationProgress>? progress = null,
        CancellationToken cancellationToken = default) =>
        Task.Run(() =>
        {
            var r = new siaara_store_verify_report { struct_size = StructSize.Of<siaara_store_verify_report>() };
            RunLong(progress, (c, cb, u) =>
            {
                using var l = new HandleLease(_handle);
                return Native.siaara_store_verify_ex(l.Pointer, deep ? 1 : 0, repair ? 1 : 0, c, cb, u, ref r);
            }, cancellationToken);
            return new DicomStoreVerifyReport((int)r.@checked, (int)r.missing, (int)r.size_mismatch, (int)r.content_mismatch,
                (int)r.orphans, (int)r.repaired);
        }, cancellationToken);

    private static void RunLong(IProgress<OperationProgress>? progress, LongCall.Body body, CancellationToken token)
    {
        try
        {
            LongCall.Run(progress, body, token);
        }
        catch
        {
            EventSink.TakePending();
            throw;
        }
        ThrowPendingHandlerException();
    }

    /// <summary>Re-indexes the data folder and every referenced file still present (blocking).</summary>
    public void Rebuild()
    {
        int st;
        using (var l = new HandleLease(_handle))
        {
            st = Native.siaara_store_rebuild(l.Pointer);
        }
        Finish(st);
    }

    /// <summary>Rebuild on a thread-pool thread (long-running and blocking).</summary>
    /// <param name="cancellationToken">Cancels, also while running (the index is then unchanged).</param>
    /// <param name="progress">Files indexed (total unknown) (optional).</param>
    /// <returns>The task.</returns>
    public Task RebuildAsync(IProgress<OperationProgress>? progress = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => RunLong(progress, (c, cb, u) =>
        {
            using var l = new HandleLease(_handle);
            return Native.siaara_store_rebuild_ex(l.Pointer, c, cb, u);
        }, cancellationToken), cancellationToken);

    /// <summary>Compacts the journal into a snapshot.</summary>
    public void Compact()
    {
        int st;
        using (var l = new HandleLease(_handle))
        {
            st = Native.siaara_store_compact(l.Pointer);
        }
        Finish(st);
    }

    /// <summary>Current statistics.</summary>
    /// <returns>The statistics.</returns>
    public DicomStoreStatistics GetStatistics()
    {
        var s = new siaara_store_stats { struct_size = StructSize.Of<siaara_store_stats>() };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_stats_get(l.Pointer, ref s));
        return new DicomStoreStatistics((long)s.patients, (long)s.studies, (long)s.series, (long)s.instances,
            (long)s.data_bytes, (long)s.journal_bytes, (long)s.journal_records, (long)s.snapshot_bytes,
            (long)s.generation, (long)s.replayed_records, (long)s.recovered_bytes);
    }

    private static void Finish(int status)
    {
        if (status != Status.Ok)
        {
            SiaaraException e = Status.Error(status);
            EventSink.TakePending();
            throw e;
        }
        ThrowPendingHandlerException();
    }

    private static void ThrowPendingHandlerException()
    {
        Exception? e = EventSink.TakePending();
        if (e is not null)
        {
            ExceptionDispatchInfo.Capture(e).Throw();
        }
    }

    // ------------------------------------------------------------------ quarantine (R-116)
    private static string Fixed(ReadOnlySpan<byte> bytes)
    {
        int n = bytes.IndexOf((byte)0);
        return System.Text.Encoding.UTF8.GetString(n < 0 ? bytes : bytes[..n]);
    }

    /// <summary>The items of the quarantine (incomplete instances kept outside the archive) or of the "rejected"
    /// area (copies of refused instances), newest first.</summary>
    /// <param name="area">Which area.</param>
    /// <returns>The records.</returns>
    public IReadOnlyList<QuarantineItem> QuarantineList(QuarantineArea area = QuarantineArea.Quarantine)
    {
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_quarantine_count(l.Pointer, (int)area, out uint n, out _));
        var list = new List<QuarantineItem>((int)n);
        for (uint i = 0; i < n; i++)
        {
            var r = new siaara_quarantine_record { struct_size = StructSize.Of<siaara_quarantine_record>() };
            int st = Native.siaara_store_quarantine_get(l.Pointer, (int)area, i, ref r);
            if (st == C.SIAARA_ERR_NOT_FOUND)
            {
                break;  // removed meanwhile
            }
            Status.Check(st);
            list.Add(new QuarantineItem(Fixed(r.id), area, DateTimeOffset.FromUnixTimeSeconds(r.time), Fixed(r.source), Fixed(r.calling_ae),
                Fixed(r.peer), Fixed(r.sop_class_uid), Fixed(r.sop_instance_uid), (long)r.size, (int)r.read_loss, Fixed(r.warning),
                r.record_missing != 0));
        }
        return list;
    }

    /// <summary>The bytes of an item, exactly as received.</summary>
    /// <param name="id">The item's id (SHA-256).</param>
    /// <param name="area">Which area.</param>
    /// <returns>The bytes.</returns>
    public byte[] QuarantineBytes(string id, QuarantineArea area = QuarantineArea.Quarantine)
    {
        ArgumentException.ThrowIfNullOrEmpty(id);
        using var l = new HandleLease(_handle);
        byte[] idz = System.Text.Encoding.UTF8.GetBytes(id + "\0");
        Status.Check(Native.siaara_store_quarantine_bytes(l.Pointer, (int)area, idz, default, out nuint needed));
        var buf = new byte[(int)needed];
        if (buf.Length != 0)
        {
            Status.Check(Native.siaara_store_quarantine_bytes(l.Pointer, (int)area, idz, buf, out _));
        }
        return buf;
    }

    /// <summary>Moves an item to the archive: stored unchanged and flagged incomplete in the index.</summary>
    /// <param name="id">The item's id.</param>
    /// <param name="replaceDuplicates">Replace an instance already in the archive.</param>
    /// <param name="area">Which area.</param>
    /// <returns>The SOP Instance UID the instance is stored under.</returns>
    public string QuarantineRelease(string id, bool replaceDuplicates = false, QuarantineArea area = QuarantineArea.Quarantine)
    {
        ArgumentException.ThrowIfNullOrEmpty(id);
        using var l = new HandleLease(_handle);
        Span<byte> uid = stackalloc byte[65];
        Status.Check(Native.siaara_store_quarantine_release(l.Pointer, (int)area, System.Text.Encoding.UTF8.GetBytes(id + "\0"),
            replaceDuplicates ? C.SIAARA_DUP_REPLACE : C.SIAARA_DUP_KEEP, out _, uid, out _));
        return Fixed(uid);
    }

    /// <summary>Writes an item's bytes, unchanged, to folder/&lt;id&gt;.dcm; the item stays.</summary>
    /// <param name="id">The item's id.</param>
    /// <param name="folder">Destination folder.</param>
    /// <param name="area">Which area.</param>
    public void QuarantineExport(string id, string folder, QuarantineArea area = QuarantineArea.Quarantine)
    {
        ArgumentException.ThrowIfNullOrEmpty(id);
        ArgumentException.ThrowIfNullOrEmpty(folder);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_quarantine_export(l.Pointer, (int)area, System.Text.Encoding.UTF8.GetBytes(id + "\0"),
            System.Text.Encoding.UTF8.GetBytes(folder + "\0")));
    }

    /// <summary>Deletes an item.</summary>
    /// <param name="id">The item's id.</param>
    /// <param name="area">Which area.</param>
    public void QuarantineDelete(string id, QuarantineArea area = QuarantineArea.Quarantine)
    {
        ArgumentException.ThrowIfNullOrEmpty(id);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_store_quarantine_delete(l.Pointer, (int)area, System.Text.Encoding.UTF8.GetBytes(id + "\0")));
    }

    /// <summary>Closes the store (removes the callback first).</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>
    /// Callback target. Holds only a weak reference to the store, so an undisposed store can still
    /// be collected and its handle finalized. Handler exceptions are kept per thread (the callback
    /// runs on the thread that made the change) and rethrown by that thread's store call.
    /// </summary>
    private sealed class EventSink : IStoreEventSink
    {
        [ThreadStatic]
        private static Exception? t_pending;

        internal WeakReference<DicomStore>? Sender { get; set; }
        internal EventHandler<StoreChangedEventArgs>? Handlers { get; set; }

        public void OnStoreEvent(int kind, string sopInstanceUid, string seriesUid, string studyUid)
        {
            EventHandler<StoreChangedEventArgs>? h = Handlers;
            if (h is null)
            {
                return;
            }
            DicomStore? sender = null;
            _ = Sender?.TryGetTarget(out sender);
            h(sender, new StoreChangedEventArgs((StoreChangeKind)kind, sopInstanceUid, seriesUid, studyUid));
        }

        public void OnCallbackException(Exception exception) => t_pending ??= exception;

        internal static Exception? TakePending()
        {
            Exception? e = t_pending;
            t_pending = null;
            return e;
        }
    }
}
