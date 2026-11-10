using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;
using Siaara.Web;

namespace Siaara.Network;

/// <summary>Query/Retrieve information model.</summary>
public enum QueryRetrieveModel
{
    /// <summary>Patient Root.</summary>
    PatientRoot = C.SIAARA_QR_PATIENT_ROOT,
    /// <summary>Study Root.</summary>
    StudyRoot = C.SIAARA_QR_STUDY_ROOT,
    /// <summary>Modality Worklist (C-FIND only).</summary>
    Worklist = C.SIAARA_QR_WORKLIST,
}

/// <summary>The remote application entity and association settings (siaara_dimse_peer).</summary>
public sealed record DimsePeer
{
    /// <summary>Host name or address.</summary>
    public required string Host { get; init; }
    /// <summary>TCP port.</summary>
    public required int Port { get; init; }
    /// <summary>Our AE title.</summary>
    public string CallingAeTitle { get; init; } = "SIAARA_SCU";
    /// <summary>The peer's AE title.</summary>
    public string CalledAeTitle { get; init; } = "ANY-SCP";
    /// <summary>TLS (DICOM over TLS); null = plain TCP.</summary>
    public TlsOptions? Tls { get; init; }
    /// <summary>TCP connect timeout; null = 15 s.</summary>
    public TimeSpan? ConnectTimeout { get; init; }
    /// <summary>Association negotiation / release timeout; null = 30 s.</summary>
    public TimeSpan? AssociationTimeout { get; init; }
    /// <summary>Wait for each DIMSE response; null = 60 s.</summary>
    public TimeSpan? DimseTimeout { get; init; }
    /// <summary>Largest PDU we accept; 0 = 65536.</summary>
    public int MaxPdu { get; init; }
    /// <summary>C-STORE may code lossily when the peer accepts only lossy syntaxes.</summary>
    public bool AllowLossyTranscode { get; init; }

    private static int Ms(TimeSpan? t) => t is null ? 0 : (int)Math.Clamp(t.Value.TotalMilliseconds, 1, int.MaxValue);

    internal siaara_dimse_peer ToNative(NativeAllocations mem)
    {
        ArgumentException.ThrowIfNullOrEmpty(Host);
        ArgumentOutOfRangeException.ThrowIfNegative(MaxPdu);
        var p = new siaara_dimse_peer
        {
            struct_size = StructSize.Of<siaara_dimse_peer>(),
            port = (uint)Math.Clamp(Port, 0, 65536),
            host = mem.Utf8(Host),
            calling_ae = mem.Utf8(CallingAeTitle),
            called_ae = mem.Utf8(CalledAeTitle),
            connect_timeout_ms = Ms(ConnectTimeout),
            acse_timeout_ms = Ms(AssociationTimeout),
            dimse_timeout_ms = Ms(DimseTimeout),
            max_pdu = (uint)MaxPdu,
            allow_lossy_transcode = AllowLossyTranscode ? 1 : 0,
        };
        if (Tls is not null)
        {
            siaara_tls_options t = Tls.ToNative(mem);
            p.tls = mem.Array(new ReadOnlySpan<siaara_tls_options>(in t));
        }
        return p;
    }
}

/// <summary>Outcome of a DIMSE operation (siaara_dimse_result).</summary>
/// <param name="Outcome">Ok, or Rejected when the peer refused (DimseStatus says why) or some items failed.</param>
/// <param name="DimseStatus">Final DIMSE status (0 = Success).</param>
/// <param name="Completed">Sub-operations or files done.</param>
/// <param name="Failed">Failed ones.</param>
/// <param name="Warning">Ones with a warning.</param>
/// <param name="Remaining">Remaining as reported.</param>
/// <param name="Matches">C-FIND matches.</param>
/// <param name="Current">Progress: index of the item just handled.</param>
/// <param name="Message">The library's message for a non-Ok outcome.</param>
public sealed record DimseResult(SiaaraErrorCode Outcome, int DimseStatus, int Completed, int Failed, int Warning, int Remaining,
                                 int Matches, int Current, string Message)
{
    internal static DimseResult From(int status, in siaara_dimse_result r, string message) =>
        new((SiaaraErrorCode)status, (int)r.status, (int)r.completed, (int)r.failed, (int)r.warning, (int)r.remaining,
            (int)r.matches, (int)r.current, message);

    /// <summary>True when the operation succeeded (with or without warnings).</summary>
    public bool IsSuccess => Outcome == SiaaraErrorCode.Ok;

    /// <summary>C-FIND / C-MOVE / C-GET: the extended negotiation the peer accepted.</summary>
    public QueryNegotiation AcceptedNegotiation { get; init; }
}

/// <summary>Result of a Storage Commitment request.</summary>
/// <param name="Result">The operation outcome.</param>
/// <param name="FailureReasons">Per instance: 0 committed, else the Failure Reason, 0xFFFF not reported.</param>
public sealed record CommitmentResult(DimseResult Result, IReadOnlyList<int> FailureReasons)
{
    /// <summary>The Transaction UID of the request.</summary>
    public string TransactionUid { get; init; } = string.Empty;
}

/// <summary>SOP Class Extended Negotiation for Query/Retrieve (PS3.4 C.5).</summary>
[Flags]
public enum QueryNegotiation
{
    /// <summary>None.</summary>
    None = 0,
    /// <summary>Relational queries (C-FIND) / relational retrieval (C-MOVE, C-GET).</summary>
    Relational = C.SIAARA_QR_RELATIONAL,
    /// <summary>Combined date-time matching (C-FIND).</summary>
    CombinedDateTime = C.SIAARA_QR_COMBINED_DATETIME,
    /// <summary>Fuzzy semantic matching of person names (C-FIND).</summary>
    FuzzyPersonNames = C.SIAARA_QR_FUZZY_NAMES,
    /// <summary>Timezone query adjustment (C-FIND).</summary>
    TimezoneAdjustment = C.SIAARA_QR_TIMEZONE,
}

/// <summary>Options of a query or retrieve (siaara_qr_options).</summary>
public sealed record QueryOptions
{
    /// <summary>Extended negotiation to propose.</summary>
    public QueryNegotiation Negotiation { get; init; }
    /// <summary>C-FIND: send C-CANCEL after this many matches (0 = all); not an error.</summary>
    public int MaxMatches { get; init; }
}

/// <summary>Query/Retrieve level.</summary>
public enum QueryLevel
{
    /// <summary>PATIENT.</summary>
    Patient = C.SIAARA_LEVEL_PATIENT,
    /// <summary>STUDY.</summary>
    Study = C.SIAARA_LEVEL_STUDY,
    /// <summary>SERIES.</summary>
    Series = C.SIAARA_LEVEL_SERIES,
    /// <summary>IMAGE.</summary>
    Image = C.SIAARA_LEVEL_INSTANCE,
}

/// <summary>Matching values of a Q/R identifier (siaara_qr_keys); null = universal / return key.</summary>
public sealed record QueryKeys
{
    /// <summary>Add the level's usual return keys.</summary>
    public bool ReturnKeys { get; init; } = true;
    /// <summary>Patient ID.</summary>
    public string? PatientId { get; init; }
    /// <summary>Patient's Name (wildcards allowed).</summary>
    public string? PatientName { get; init; }
    /// <summary>Patient's Birth Date (or range).</summary>
    public string? PatientBirthDate { get; init; }
    /// <summary>Patient's Sex.</summary>
    public string? PatientSex { get; init; }
    /// <summary>Study Instance UID (a backslash-separated list allowed).</summary>
    public string? StudyInstanceUid { get; init; }
    /// <summary>Study Date (or range).</summary>
    public string? StudyDate { get; init; }
    /// <summary>Study Time (or range).</summary>
    public string? StudyTime { get; init; }
    /// <summary>Accession Number.</summary>
    public string? AccessionNumber { get; init; }
    /// <summary>Study ID.</summary>
    public string? StudyId { get; init; }
    /// <summary>Study Description.</summary>
    public string? StudyDescription { get; init; }
    /// <summary>Referring Physician's Name.</summary>
    public string? ReferringPhysician { get; init; }
    /// <summary>Modalities in Study.</summary>
    public string? ModalitiesInStudy { get; init; }
    /// <summary>Series Instance UID.</summary>
    public string? SeriesInstanceUid { get; init; }
    /// <summary>Modality.</summary>
    public string? Modality { get; init; }
    /// <summary>Series Number.</summary>
    public string? SeriesNumber { get; init; }
    /// <summary>Series Description.</summary>
    public string? SeriesDescription { get; init; }
    /// <summary>SOP Instance UID.</summary>
    public string? SopInstanceUid { get; init; }
    /// <summary>SOP Class UID.</summary>
    public string? SopClassUid { get; init; }
    /// <summary>Instance Number.</summary>
    public string? InstanceNumber { get; init; }
}

/// <summary>Options of a C-GET (siaara_get_options).</summary>
public sealed record DimseGetOptions
{
    /// <summary>Destination store, or null.</summary>
    public DicomStore? Store { get; init; }
    /// <summary>Destination folder (files &lt;SOP Instance UID&gt;.dcm, owner-only), or null.</summary>
    public string? Folder { get; init; }
    /// <summary>Replace instances / files already present (else the existing one is kept).</summary>
    public bool ReplaceDuplicates { get; init; }
    /// <summary>Propose only uncompressed transfer syntaxes.</summary>
    public bool NativeOnly { get; init; }
    /// <summary>Refuse (0xA900) sub-operations whose command UIDs differ from the data set's.</summary>
    public bool StrictStoreUids { get; init; }
    /// <summary>Storage SOP classes to accept; null = the toolkit's list.</summary>
    public IReadOnlyList<string>? SopClasses { get; init; }
}

/// <summary>Options of a Storage Commitment request (siaara_commit_options).</summary>
public sealed record CommitmentOptions
{
    /// <summary>Total wait for the report; null = 60 s.</summary>
    public TimeSpan? Wait { get; init; }
    /// <summary>A running SCP with <see cref="ScpServiceOptions.CommitmentReports"/> that receives reports sent on a new association; null = same association only.</summary>
    public StorageScp? Receiver { get; init; }
    /// <summary>With a receiver: how long the association stays open for a report on it (null = 2 s; zero = release at once).</summary>
    public TimeSpan? SameAssociationWait { get; init; }
    /// <summary>Transaction UID to use; null = a new one.</summary>
    public string? TransactionUid { get; init; }
}

/// <summary>A lossy transcode gave an instance a new SOP Instance UID (R-093).</summary>
/// <param name="Index">Position of the file in the call.</param>
/// <param name="OriginalSopInstanceUid">The file's own UID.</param>
/// <param name="NewSopInstanceUid">The UID sent.</param>
public sealed record UidChange(int Index, string OriginalSopInstanceUid, string NewSopInstanceUid);

/// <summary>DIMSE-N command (siaara_n_request.command_field).</summary>
public enum NCommand
{
    /// <summary>N-EVENT-REPORT.</summary>
    EventReport = C.SIAARA_N_EVENT_REPORT,
    /// <summary>N-GET.</summary>
    Get = C.SIAARA_N_GET,
    /// <summary>N-SET.</summary>
    Set = C.SIAARA_N_SET,
    /// <summary>N-ACTION.</summary>
    Action = C.SIAARA_N_ACTION,
    /// <summary>N-CREATE.</summary>
    Create = C.SIAARA_N_CREATE,
    /// <summary>N-DELETE.</summary>
    Delete = C.SIAARA_N_DELETE,
}

/// <summary>A generic DIMSE-N request (UPS, Instance Availability, ...).</summary>
/// <param name="Command">The command.</param>
/// <param name="SopClassUid">SOP class of the command.</param>
public sealed record NRequest(NCommand Command, string SopClassUid)
{
    /// <summary>Presentation context abstract syntax (e.g. a Meta SOP Class); null = SopClassUid.</summary>
    public string? AbstractSyntax { get; init; }
    /// <summary>SOP Instance UID; null for N-CREATE lets the SCP choose.</summary>
    public string? SopInstanceUid { get; init; }
    /// <summary>Event Type ID / Action Type ID.</summary>
    public int TypeId { get; init; }
    /// <summary>N-GET Attribute Identifier List (null: all).</summary>
    public IReadOnlyList<DicomTag>? Attributes { get; init; }
    /// <summary>Data set to send, or null.</summary>
    public DicomDataset? Dataset { get; init; }
    /// <summary>Propose role selection with us as SCP (e.g. N-EVENT-REPORT to a subscriber).</summary>
    public bool ProposeScpRole { get; init; }
}

/// <summary>Response of a DIMSE-N request.</summary>
/// <param name="Result">The outcome.</param>
/// <param name="SopInstanceUid">SOP Instance UID of the response.</param>
/// <param name="Dataset">Response data set (dispose it), or null.</param>
public sealed record NResponse(DimseResult Result, string SopInstanceUid, DicomFile? Dataset);

/// <summary>
/// DIMSE service class user calls: C-ECHO, C-FIND, C-MOVE, C-GET, C-STORE, Storage Commitment,
/// MPPS, generic DIMSE-N (UPS, Instance Availability). Each call opens one association and is
/// cancellable at any time (the association is aborted), except during the TCP connect. Every
/// call has a synchronous form, which runs on the calling thread, and an Async form, which runs
/// it on a thread-pool thread. Calls return a <see cref="DimseResult"/> for outcomes the peer
/// decided (Ok, Rejected) and throw <see cref="OperationCanceledException"/> when cancelled and
/// <see cref="SiaaraException"/> for other failures (network, timeout, bad arguments). Progress and
/// match callbacks run on the calling native thread; use a Progress&lt;T&gt; to marshal to a UI.
/// </summary>
public static class Dimse
{
    private static DimseResult Run(DimsePeer peer, Func<siaara_dimse_peer, nint, (int Status, siaara_dimse_result R)> call, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(peer);
        token.ThrowIfCancellationRequested();
        using var mem = new NativeAllocations();
        siaara_dimse_peer p = peer.ToNative(mem);
        using var cancel = new NativeCancellation(token);
        (int st, siaara_dimse_result r) = call(p, cancel.Pointer);
        string msg = st == C.SIAARA_OK ? string.Empty : Status.LastMessage();
        if (st == C.SIAARA_ERR_CANCELLED)
        {
            throw new OperationCanceledException(msg, token);
        }
        if (st != C.SIAARA_OK && st != C.SIAARA_ERR_REJECTED)
        {
            throw new SiaaraException((SiaaraErrorCode)st, msg);
        }
        return DimseResult.From(st, in r, msg);
    }

    private static siaara_dimse_result NewResult() => new() { struct_size = StructSize.Of<siaara_dimse_result>() };

    private static siaara_qr_options NewQr(QueryOptions? o) => new()
    {
        struct_size = StructSize.Of<siaara_qr_options>(),
        flags = (uint)(o?.Negotiation ?? QueryNegotiation.None),
        max_matches = (uint)Math.Max(0, o?.MaxMatches ?? 0),
    };

    /// <summary>A file holding a copy of a native data set borrowed during a callback.</summary>
    internal static DicomFile CloneDataset(nint dataset) =>
        DicomFile.CreateWith(dataset, static d => (Native.siaara_dataset_clone(d, out nint p), p));

    // ------------------------------------------------------------------ C-ECHO
    /// <summary>C-ECHO.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="cancellationToken">Cancels (aborts the association).</param>
    /// <returns>The outcome.</returns>
    public static DimseResult Echo(DimsePeer peer, CancellationToken cancellationToken = default) =>
        Run(peer, (p, c) =>
        {
            siaara_dimse_result r = NewResult();
            int st = Native.siaara_dimse_echo(in p, c, ref r);
            return (st, r);
        }, cancellationToken);

    /// <summary>C-ECHO on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="cancellationToken">Cancels (aborts the association).</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> EchoAsync(DimsePeer peer, CancellationToken cancellationToken = default) =>
        Task.Run(() => Echo(peer, cancellationToken), cancellationToken);

    // ------------------------------------------------------------------ C-FIND
    /// <summary>C-FIND: every match as a data set (in a DicomFile of its own).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">Information model.</param>
    /// <param name="identifier">Query keys (with Query/Retrieve Level for the Q/R models).</param>
    /// <param name="maxMatches">Sends C-CANCEL after this many matches (0 = all).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome and the matches (dispose them).</returns>
    public static (DimseResult Result, IReadOnlyList<DicomFile> Matches) Find(DimsePeer peer, QueryRetrieveModel model,
        DicomDataset identifier, int maxMatches = 0, CancellationToken cancellationToken = default)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maxMatches);
        var list = new List<DicomFile>();
        try
        {
            // Each match is copied natively (no JSON round trip) and kept.
            DimseResult res = FindCore(peer, model, identifier, new QueryOptions { MaxMatches = maxMatches }, d =>
            {
                list.Add(CloneDataset(d));
                return true;
            }, cancellationToken);
            return (res, list);
        }
        catch
        {
            foreach (DicomFile f in list)
            {
                f.Dispose();
            }
            throw;
        }
    }

    /// <summary>C-FIND on a thread-pool thread: every match as a data set (in a DicomFile of its own).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">Information model.</param>
    /// <param name="identifier">Query keys (with Query/Retrieve Level for the Q/R models).</param>
    /// <param name="maxMatches">Sends C-CANCEL after this many matches (0 = all).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome and the matches (dispose them).</returns>
    public static Task<(DimseResult Result, IReadOnlyList<DicomFile> Matches)> FindAsync(DimsePeer peer, QueryRetrieveModel model,
        DicomDataset identifier, int maxMatches = 0, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identifier);
        ArgumentOutOfRangeException.ThrowIfNegative(maxMatches);
        return Task.Run(() => Find(peer, model, identifier, maxMatches, cancellationToken), cancellationToken);
    }

    /// <summary>
    /// C-FIND with a callback per match: the data set is valid during the callback only (copy what
    /// you need, or clone it). Return false to stop (C-CANCEL; the result is then Ok).
    /// <see cref="DimseResult.AcceptedNegotiation"/> tells what the peer accepted.
    /// </summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">Information model.</param>
    /// <param name="identifier">Query keys.</param>
    /// <param name="onMatch">Called for each match on the calling thread.</param>
    /// <param name="options">Extended negotiation and match limit, or null.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static DimseResult Find(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, Func<DicomDataset, bool> onMatch,
        QueryOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(onMatch);
        return FindCore(peer, model, identifier, options, d =>
        {
            using DicomFile f = CloneDataset(d);
            return onMatch(f.Dataset);
        }, cancellationToken);
    }

    /// <summary>As <see cref="Find(DimsePeer, QueryRetrieveModel, DicomDataset, Func{DicomDataset, bool}, QueryOptions, CancellationToken)"/> on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">Information model.</param>
    /// <param name="identifier">Query keys.</param>
    /// <param name="onMatch">Called for each match on the worker thread.</param>
    /// <param name="options">Extended negotiation and match limit, or null.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> FindAsync(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier,
        Func<DicomDataset, bool> onMatch, QueryOptions? options = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => Find(peer, model, identifier, onMatch, options, cancellationToken), cancellationToken);

    private static DimseResult FindCore(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, QueryOptions? options,
        Func<nint, bool> onMatch, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(identifier);
        var sink = new FindSink(onMatch);
        GCHandle gc = GCHandle.Alloc(sink);
        try
        {
            using var l = new HandleLease(identifier.File.Handle);
            nint id = identifier.Resolve(l.Pointer);
            siaara_qr_options qo = NewQr(options);
            DimseResult res;
            try
            {
                res = Run(peer, (p, c) =>
                {
                    siaara_dimse_result r = NewResult();
                    int st = Native.siaara_dimse_find_ex(in p, (int)model, id, ref qo, c, Callbacks.FindPointer, GCHandle.ToIntPtr(gc), ref r);
                    if (st == C.SIAARA_ERR_CANCELLED && sink.StoppedByCaller && !token.IsCancellationRequested)
                    {
                        st = C.SIAARA_OK;  // our own C-CANCEL
                    }
                    return (st, r);
                }, token);
            }
            catch (OperationCanceledException) when (!token.IsCancellationRequested)
            {
                sink.ThrowIfFailed();  // a callback that threw stopped the query: report its exception
                throw;
            }
            sink.ThrowIfFailed();
            return res with { AcceptedNegotiation = (QueryNegotiation)qo.accepted_flags };
        }
        finally
        {
            gc.Free();
        }
    }

    // ------------------------------------------------------------------ C-MOVE
    /// <summary>C-MOVE to a destination AE the peer knows.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">PatientRoot or StudyRoot.</param>
    /// <param name="identifier">Keys selecting the instances.</param>
    /// <param name="destinationAeTitle">Destination AE title.</param>
    /// <param name="progress">Pending responses (optional).</param>
    /// <param name="options">Relational retrieval, or null.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static DimseResult Move(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, string destinationAeTitle,
        IProgress<DimseResult>? progress = null, QueryOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identifier);
        ArgumentException.ThrowIfNullOrEmpty(destinationAeTitle);
        var sink = new ProgressSink(progress);
        GCHandle gc = GCHandle.Alloc(sink);
        try
        {
            using var l = new HandleLease(identifier.File.Handle);
            nint id = identifier.Resolve(l.Pointer);
            byte[] dest = Status.Utf8Z(destinationAeTitle)!;
            siaara_qr_options qo = NewQr(options);
            DimseResult res = Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_move_ex(in p, (int)model, id, dest, ref qo, c, Callbacks.DimseProgressPointer,
                    GCHandle.ToIntPtr(gc), ref r);
                return (st, r);
            }, cancellationToken);
            sink.ThrowIfFailed();
            return res with { AcceptedNegotiation = (QueryNegotiation)qo.accepted_flags };
        }
        finally
        {
            gc.Free();
        }
    }

    /// <summary>C-MOVE on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">PatientRoot or StudyRoot.</param>
    /// <param name="identifier">Keys selecting the instances.</param>
    /// <param name="destinationAeTitle">Destination AE title.</param>
    /// <param name="progress">Pending responses (optional).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> MoveAsync(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, string destinationAeTitle,
        IProgress<DimseResult>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identifier);
        ArgumentException.ThrowIfNullOrEmpty(destinationAeTitle);
        return Task.Run(() => Move(peer, model, identifier, destinationAeTitle, progress, null, cancellationToken), cancellationToken);
    }

    // ------------------------------------------------------------------ C-GET
    /// <summary>C-GET with every option of siaara_get_options.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">PatientRoot or StudyRoot.</param>
    /// <param name="identifier">Keys selecting the instances.</param>
    /// <param name="getOptions">Destination and policies.</param>
    /// <param name="progress">Pending responses (optional).</param>
    /// <param name="options">Relational retrieval, or null.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static DimseResult Get(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, DimseGetOptions getOptions,
        IProgress<DimseResult>? progress = null, QueryOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identifier);
        ArgumentNullException.ThrowIfNull(getOptions);
        if (getOptions.Store is null && getOptions.Folder is null)
        {
            throw new ArgumentException("A store or a folder is required.");
        }
        var sink = new ProgressSink(progress);
        GCHandle gc = GCHandle.Alloc(sink);
        using var mem = new NativeAllocations();
        try
        {
            using var l = new HandleLease(identifier.File.Handle);
            nint id = identifier.Resolve(l.Pointer);
            DicomStore? store = getOptions.Store;
            bool addedStore = false;
            store?.Handle.DangerousAddRef(ref addedStore);
            try
            {
                nint classes = 0;
                if (getOptions.SopClasses is not null && getOptions.SopClasses.Count > 0)
                {
                    var a = new nint[getOptions.SopClasses.Count];
                    for (int i = 0; i < a.Length; i++)
                    {
                        a[i] = mem.Utf8(getOptions.SopClasses[i] ?? throw new ArgumentException("null SOP class"));
                    }
                    classes = mem.Array<nint>(a);
                }
                var o = new siaara_get_options
                {
                    struct_size = StructSize.Of<siaara_get_options>(),
                    duplicate_policy = getOptions.ReplaceDuplicates ? C.SIAARA_DUP_REPLACE : C.SIAARA_DUP_KEEP,
                    store = store is null ? 0 : store.Handle.DangerousGetHandle(),
                    folder = mem.Utf8(getOptions.Folder),
                    sop_classes = classes,
                    sop_class_count = (uint)(getOptions.SopClasses?.Count ?? 0),
                    native_only = getOptions.NativeOnly ? 1 : 0,
                    strict_store_uids = getOptions.StrictStoreUids ? 1 : 0,
                };
                siaara_qr_options qo = NewQr(options);
                DimseResult res = Run(peer, (p, c) =>
                {
                    siaara_dimse_result r = NewResult();
                    int st = Native.siaara_dimse_get_ex(in p, (int)model, id, in o, ref qo, c, Callbacks.DimseProgressPointer,
                        GCHandle.ToIntPtr(gc), ref r);
                    return (st, r);
                }, cancellationToken);
                sink.ThrowIfFailed();
                return res with { AcceptedNegotiation = (QueryNegotiation)qo.accepted_flags };
            }
            finally
            {
                if (addedStore)
                {
                    store!.Handle.DangerousRelease();
                }
            }
        }
        finally
        {
            gc.Free();
        }
    }

    /// <summary>C-GET into a store and / or a folder (files named &lt;SOP Instance UID&gt;.dcm).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">PatientRoot or StudyRoot.</param>
    /// <param name="identifier">Keys selecting the instances.</param>
    /// <param name="store">Destination store, or null.</param>
    /// <param name="folder">Destination folder, or null (one of the two is required).</param>
    /// <param name="progress">Pending responses (optional).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> GetAsync(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, DicomStore? store,
        string? folder = null, IProgress<DimseResult>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identifier);
        if (store is null && folder is null)
        {
            throw new ArgumentException("A store or a folder is required.");
        }
        return GetAsync(peer, model, identifier, new DimseGetOptions { Store = store, Folder = folder }, progress, null, cancellationToken);
    }

    /// <summary>C-GET with every option, on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="model">PatientRoot or StudyRoot.</param>
    /// <param name="identifier">Keys selecting the instances.</param>
    /// <param name="getOptions">Destination and policies.</param>
    /// <param name="progress">Pending responses (optional).</param>
    /// <param name="options">Relational retrieval, or null.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> GetAsync(DimsePeer peer, QueryRetrieveModel model, DicomDataset identifier, DimseGetOptions getOptions,
        IProgress<DimseResult>? progress = null, QueryOptions? options = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => Get(peer, model, identifier, getOptions, progress, options, cancellationToken), cancellationToken);

    // ------------------------------------------------------------------ C-STORE
    /// <summary>C-STORE of files over one association, transcoding when the peer needs another syntax.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="paths">Files.</param>
    /// <param name="progress">After every file (optional).</param>
    /// <param name="onUidChange">A lossy transcode gave a file's instance a new SOP Instance UID (optional).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome: Rejected when some files were not stored (see Failed).</returns>
    public static DimseResult StoreFiles(DimsePeer peer, IReadOnlyList<string> paths, IProgress<DimseResult>? progress = null,
        Action<UidChange>? onUidChange = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(paths);
        if (paths.Count == 0)
        {
            throw new ArgumentException("At least one path is needed.", nameof(paths));
        }
        var sink = new ProgressSink(progress, onUidChange);
        GCHandle gc = GCHandle.Alloc(sink);
        using var mem = new NativeAllocations();
        try
        {
            var ptrs = new nint[paths.Count];
            for (int i = 0; i < paths.Count; i++)
            {
                ArgumentException.ThrowIfNullOrEmpty(paths[i], nameof(paths));
                ptrs[i] = mem.Utf8(paths[i]);
            }
            DimseResult res = Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_store_files_ex(in p, ptrs, default, (uint)ptrs.Length, c,
                    Callbacks.DimseProgressPointer, Callbacks.UidChangePointer, GCHandle.ToIntPtr(gc), ref r);
                return (st, r);
            }, cancellationToken);
            sink.ThrowIfFailed();
            return res;
        }
        finally
        {
            gc.Free();
        }
    }

    /// <summary>C-STORE of files on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="paths">Files.</param>
    /// <param name="progress">After every file (optional).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome: Rejected when some files were not stored (see Failed).</returns>
    public static Task<DimseResult> StoreFilesAsync(DimsePeer peer, IReadOnlyList<string> paths, IProgress<DimseResult>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(paths);
        if (paths.Count == 0)
        {
            throw new ArgumentException("At least one path is needed.", nameof(paths));
        }
        return Task.Run(() => StoreFiles(peer, paths, progress, null, cancellationToken), cancellationToken);
    }

    /// <summary>C-STORE of one file in memory.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="file">The instance.</param>
    /// <param name="onUidChange">A lossy transcode gave the instance a new SOP Instance UID (optional).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static DimseResult Store(DimsePeer peer, DicomFile file, Action<UidChange>? onUidChange = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(file);
        var sink = new ProgressSink(null, onUidChange);
        GCHandle gc = GCHandle.Alloc(sink);
        try
        {
            using var l = new HandleLease(file.Handle);
            nint f = l.Pointer;
            DimseResult res = Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_store_ex(in p, f, c, Callbacks.UidChangePointer, GCHandle.ToIntPtr(gc), ref r);
                return (st, r);
            }, cancellationToken);
            sink.ThrowIfFailed();
            return res;
        }
        finally
        {
            gc.Free();
        }
    }

    /// <summary>C-STORE of one file in memory, on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="file">The instance.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> StoreAsync(DimsePeer peer, DicomFile file, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(file);
        return Task.Run(() => Store(peer, file, null, cancellationToken), cancellationToken);
    }

    // ------------------------------------------------------------------ Storage Commitment
    /// <summary>Storage Commitment Push Model, with the report on the same or (with a receiver) a new association.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="instances">SOP Class and SOP Instance UIDs.</param>
    /// <param name="options">Wait, receiver, Transaction UID; null = same association, 60 s.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The result per instance and the Transaction UID.</returns>
    public static CommitmentResult Commit(DimsePeer peer, IReadOnlyList<(string SopClassUid, string SopInstanceUid)> instances,
        CommitmentOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(instances);
        if (instances.Count == 0)
        {
            throw new ArgumentException("At least one instance is needed.", nameof(instances));
        }
        static int Ms(TimeSpan? t, int none) => t is null ? none : (int)Math.Clamp(t.Value.TotalMilliseconds, 0, int.MaxValue);
        using var mem = new NativeAllocations();
        var cls = new nint[instances.Count];
        var ins = new nint[instances.Count];
        for (int i = 0; i < instances.Count; i++)
        {
            cls[i] = mem.Utf8(instances[i].SopClassUid ?? throw new ArgumentException("null UID", nameof(instances)));
            ins[i] = mem.Utf8(instances[i].SopInstanceUid ?? throw new ArgumentException("null UID", nameof(instances)));
        }
        var reasons = new ushort[instances.Count];
        StorageScp? receiver = options?.Receiver;
        bool added = false;
        receiver?.Handle.DangerousAddRef(ref added);
        try
        {
            var txn = new byte[65];
            nint txnOut = mem.Array<byte>(txn);
            // null: the native default (2 s); zero: release at once (-1).
            int same = options?.SameAssociationWait is not TimeSpan w ? 0 : w <= TimeSpan.Zero ? -1 : Ms(w, 0);
            var o = new siaara_commit_options
            {
                struct_size = StructSize.Of<siaara_commit_options>(),
                wait_ms = Ms(options?.Wait, 0),
                receiver = receiver is null ? 0 : receiver.Handle.DangerousGetHandle(),
                same_association_ms = same,
                transaction_uid = mem.Utf8(options?.TransactionUid),
                transaction_uid_out = txnOut,
            };
            DimseResult res = Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_commit_ex(in p, in o, cls, ins, (uint)cls.Length, c, reasons, ref r);
                return (st, r);
            }, cancellationToken);
            var list = new int[reasons.Length];
            for (int i = 0; i < list.Length; i++)
            {
                list[i] = reasons[i];
            }
            string uid = Marshal.PtrToStringUTF8(txnOut) ?? string.Empty;
            return new CommitmentResult(res, list) { TransactionUid = uid };
        }
        finally
        {
            if (added)
            {
                receiver!.Handle.DangerousRelease();
            }
        }
    }

    /// <summary>Storage Commitment Push Model, report on the same association.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="instances">SOP Class and SOP Instance UIDs.</param>
    /// <param name="wait">How long to wait for the report; null = 60 s.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The result per instance.</returns>
    public static Task<CommitmentResult> CommitAsync(DimsePeer peer, IReadOnlyList<(string SopClassUid, string SopInstanceUid)> instances,
        TimeSpan? wait = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(instances);
        if (instances.Count == 0)
        {
            throw new ArgumentException("At least one instance is needed.", nameof(instances));
        }
        return Task.Run(() => Commit(peer, instances, new CommitmentOptions { Wait = wait }, cancellationToken), cancellationToken);
    }

    /// <summary>Storage Commitment with options, on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="instances">SOP Class and SOP Instance UIDs.</param>
    /// <param name="options">Wait, receiver, Transaction UID.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The result per instance.</returns>
    public static Task<CommitmentResult> CommitAsync(DimsePeer peer, IReadOnlyList<(string SopClassUid, string SopInstanceUid)> instances,
        CommitmentOptions options, CancellationToken cancellationToken = default) =>
        Task.Run(() => Commit(peer, instances, options, cancellationToken), cancellationToken);

    // ------------------------------------------------------------------ MPPS and DIMSE-N
    /// <summary>MPPS N-CREATE (Performed Procedure Step Status IN PROGRESS ...).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="attributes">The attributes.</param>
    /// <param name="sopInstanceUid">The MPPS SOP Instance UID, or null for a new one.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome and the UID used.</returns>
    public static (DimseResult Result, string SopInstanceUid) MppsCreate(DimsePeer peer, DicomDataset attributes, string? sopInstanceUid = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(attributes);
        using var l = new HandleLease(attributes.File.Handle);
        nint ds = attributes.Resolve(l.Pointer);
        byte[]? uid = Status.Utf8Z(sopInstanceUid);
        var buf = new byte[80];
        nuint n = 0;
        DimseResult res = Run(peer, (p, c) =>
        {
            siaara_dimse_result r = NewResult();
            int st = Native.siaara_dimse_mpps_create(in p, uid, ds, c, buf, out n, ref r);
            return (st, r);
        }, cancellationToken);
        return (res, res.IsSuccess ? System.Text.Encoding.UTF8.GetString(buf, 0, (int)Math.Min(n, (nuint)buf.Length - 1)) : sopInstanceUid ?? string.Empty);
    }

    /// <summary>MPPS N-CREATE on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="attributes">The attributes.</param>
    /// <param name="sopInstanceUid">The MPPS SOP Instance UID, or null for a new one.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome and the UID used.</returns>
    public static Task<(DimseResult Result, string SopInstanceUid)> MppsCreateAsync(DimsePeer peer, DicomDataset attributes,
        string? sopInstanceUid = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => MppsCreate(peer, attributes, sopInstanceUid, cancellationToken), cancellationToken);

    /// <summary>MPPS N-SET (e.g. COMPLETED with the Performed Series Sequence).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="sopInstanceUid">The MPPS SOP Instance UID.</param>
    /// <param name="modifications">The changes.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static DimseResult MppsSet(DimsePeer peer, string sopInstanceUid, DicomDataset modifications, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(sopInstanceUid);
        ArgumentNullException.ThrowIfNull(modifications);
        using var l = new HandleLease(modifications.File.Handle);
        nint ds = modifications.Resolve(l.Pointer);
        byte[] uid = Status.Utf8Z(sopInstanceUid)!;
        return Run(peer, (p, c) =>
        {
            siaara_dimse_result r = NewResult();
            int st = Native.siaara_dimse_mpps_set(in p, uid, ds, c, ref r);
            return (st, r);
        }, cancellationToken);
    }

    /// <summary>MPPS N-SET on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="sopInstanceUid">The MPPS SOP Instance UID.</param>
    /// <param name="modifications">The changes.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> MppsSetAsync(DimsePeer peer, string sopInstanceUid, DicomDataset modifications,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => MppsSet(peer, sopInstanceUid, modifications, cancellationToken), cancellationToken);

    /// <summary>A DIMSE-N request on its own association (UPS, Instance Availability, ...).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="request">The request.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome, the response UID and data set.</returns>
    public static NResponse SendNRequest(DimsePeer peer, NRequest request, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        using var mem = new NativeAllocations();
        bool added = false;
        try
        {
            nint ds = 0;
            if (request.Dataset is not null)
            {
                request.Dataset.File.Handle.DangerousAddRef(ref added);
                ds = request.Dataset.Resolve(request.Dataset.File.Handle.DangerousGetHandle());
            }
            uint[] tags = request.Attributes is null ? Array.Empty<uint>() : request.Attributes.Select(t => t.Value).ToArray();
            var rq = new siaara_n_request
            {
                struct_size = StructSize.Of<siaara_n_request>(),
                command_field = (uint)request.Command,
                abstract_syntax = mem.Utf8(request.AbstractSyntax),
                sop_class_uid = mem.Utf8(request.SopClassUid),
                sop_instance_uid = mem.Utf8(request.SopInstanceUid),
                type_id = (uint)Math.Clamp(request.TypeId, 0, 0xFFFF),
                attribute_count = (uint)tags.Length,
                attribute_tags = mem.Array<uint>(tags),
                dataset = ds,
                propose_scp_role = request.ProposeScpRole ? 1 : 0,
            };
            var buf = new byte[80];
            nuint n = 0;
            nint outFile = 0;
            DimseResult res = Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_n_request(in p, in rq, c, out outFile, buf, out n, ref r);
                return (st, r);
            }, cancellationToken);
            string uid = System.Text.Encoding.UTF8.GetString(buf, 0, (int)Math.Min(n, (nuint)buf.Length - 1));
#pragma warning disable CA2000 // ownership passes to the caller through NResponse.Dataset
            DicomFile? file = outFile == 0 ? null : DicomFile.FromOwnedPointer(outFile);
#pragma warning restore CA2000
            return new NResponse(res, uid, file);
        }
        finally
        {
            if (added)
            {
                request.Dataset!.File.Handle.DangerousRelease();
            }
        }
    }

    /// <summary>A DIMSE-N request on a thread-pool thread.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="request">The request.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome, the response UID and data set.</returns>
    public static Task<NResponse> SendNRequestAsync(DimsePeer peer, NRequest request, CancellationToken cancellationToken = default) =>
        Task.Run(() => SendNRequest(peer, request, cancellationToken), cancellationToken);

    // ------------------------------------------------------------------ identifiers
    /// <summary>A Query/Retrieve identifier (siaara_qr_identifier): level, keys, UTF-8, and the usual return keys.</summary>
    /// <param name="level">Query/Retrieve Level.</param>
    /// <param name="keys">Matching values, or null.</param>
    /// <returns>A file whose data set is the identifier; dispose it.</returns>
    public static DicomFile CreateQueryIdentifier(QueryLevel level, QueryKeys? keys = null)
    {
        keys ??= new QueryKeys();
        using var mem = new NativeAllocations();
        var k = new siaara_qr_keys
        {
            struct_size = StructSize.Of<siaara_qr_keys>(),
            return_keys = keys.ReturnKeys ? 1 : 0,
            patient_id = mem.Utf8(keys.PatientId),
            patient_name = mem.Utf8(keys.PatientName),
            patient_birth_date = mem.Utf8(keys.PatientBirthDate),
            patient_sex = mem.Utf8(keys.PatientSex),
            study_instance_uid = mem.Utf8(keys.StudyInstanceUid),
            study_date = mem.Utf8(keys.StudyDate),
            study_time = mem.Utf8(keys.StudyTime),
            accession_number = mem.Utf8(keys.AccessionNumber),
            study_id = mem.Utf8(keys.StudyId),
            study_description = mem.Utf8(keys.StudyDescription),
            referring_physician = mem.Utf8(keys.ReferringPhysician),
            modalities_in_study = mem.Utf8(keys.ModalitiesInStudy),
            series_instance_uid = mem.Utf8(keys.SeriesInstanceUid),
            modality = mem.Utf8(keys.Modality),
            series_number = mem.Utf8(keys.SeriesNumber),
            series_description = mem.Utf8(keys.SeriesDescription),
            sop_instance_uid = mem.Utf8(keys.SopInstanceUid),
            sop_class_uid = mem.Utf8(keys.SopClassUid),
            instance_number = mem.Utf8(keys.InstanceNumber),
        };
        return DicomFile.CreateWith((Level: (int)level, Keys: k), static s =>
        {
            int st = Native.siaara_qr_identifier(s.Level, in s.Keys, out nint p);
            return (st, p);
        });
    }

    /// <summary>A Modality Worklist query identifier with the usual return keys.</summary>
    /// <param name="stationAeTitle">Scheduled Station AE Title, or null.</param>
    /// <param name="modality">Modality, or null.</param>
    /// <param name="date">Date or range "YYYYMMDD-YYYYMMDD", or null.</param>
    /// <param name="patientName">Patient's Name (wildcards allowed), or null.</param>
    /// <param name="patientId">Patient ID, or null.</param>
    /// <returns>A file whose data set is the identifier; dispose it.</returns>
    public static DicomFile CreateWorklistQuery(string? stationAeTitle = null, string? modality = null, string? date = null,
        string? patientName = null, string? patientId = null) =>
        DicomFile.CreateWith((A: Status.Utf8Z(stationAeTitle), M: Status.Utf8Z(modality), D: Status.Utf8Z(date),
                              N: Status.Utf8Z(patientName), I: Status.Utf8Z(patientId)), static s =>
        {
            int st = Native.siaara_worklist_query(s.A, s.M, s.D, s.N, s.I, out nint p);
            return (st, p);
        });

    private abstract class SinkBase : ICallbackSink
    {
        private Exception? _error;

        public void OnCallbackException(Exception exception) => _error ??= exception;

        internal void ThrowIfFailed()
        {
            if (_error is not null)
            {
                throw new InvalidOperationException("A callback failed: " + _error.Message, _error);
            }
        }
    }

    private sealed class FindSink : SinkBase, IFindSink
    {
        private readonly Func<nint, bool> _onMatch;

        internal FindSink(Func<nint, bool> onMatch) => _onMatch = onMatch;

        internal bool StoppedByCaller { get; private set; }

        public int OnMatch(nint dataset)
        {
            if (_onMatch(dataset))
            {
                return 0;
            }
            StoppedByCaller = true;
            return 1;
        }
    }

    private sealed class ProgressSink : SinkBase, IDimseProgressSink, IUidChangeSink
    {
        private readonly IProgress<DimseResult>? _progress;
        private readonly Action<UidChange>? _onUidChange;

        internal ProgressSink(IProgress<DimseResult>? progress, Action<UidChange>? onUidChange = null)
        {
            _progress = progress;
            _onUidChange = onUidChange;
        }

        public int OnProgress(in siaara_dimse_result progress)
        {
            _progress?.Report(DimseResult.From(C.SIAARA_OK, in progress, string.Empty));
            return 0;
        }

        public void OnUidChange(int index, string originalUid, string newUid) =>
            _onUidChange?.Invoke(new UidChange(index, originalUid, newUid));
    }
}
