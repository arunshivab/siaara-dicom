using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Siaara.Interop;
using Siaara.Web;

namespace Siaara.Network;

/// <summary>Options of <see cref="StorageScp"/> (siaara_scp_options): the listening side, peers, limits.</summary>
public sealed record StorageScpOptions
{
    /// <summary>Our AE title.</summary>
    public string AeTitle { get; init; } = "SIAARA_SCP";
    /// <summary>Port; 0 = ephemeral (see <see cref="StorageScp.Port"/>).</summary>
    public int Port { get; init; }
    /// <summary>Bind address; null = all interfaces.</summary>
    public string? BindAddress { get; init; }
    /// <summary>Calling AE titles accepted. Empty requires <see cref="AllowAnyCallingAeTitle"/>.</summary>
    public IReadOnlyList<string> AllowedCallingAeTitles { get; init; } = Array.Empty<string>();
    /// <summary>Accept any peer when the list is empty (explicit, NET-05).</summary>
    public bool AllowAnyCallingAeTitle { get; init; }
    /// <summary>Maximum concurrent associations; 0 = 8, at most 64.</summary>
    public int MaxAssociations { get; init; }
    /// <summary>Also accept associations addressed to another AE title.</summary>
    public bool AcceptAnyCalledAeTitle { get; init; }
    /// <summary>Store any SOP class (default: the toolkit's storage list).</summary>
    public bool AcceptAnySopClass { get; init; }
    /// <summary>Also accept the compressed transfer syntaxes the toolkit knows.</summary>
    public bool AcceptCompressed { get; init; }
    /// <summary>Replace instances already in the store.</summary>
    public bool ReplaceDuplicates { get; init; }
    /// <summary>Idle timeout; null = 5 min.</summary>
    public TimeSpan? IdleTimeout { get; init; }
    /// <summary>DIMSE timeout; null = 60 s.</summary>
    public TimeSpan? DimseTimeout { get; init; }
    /// <summary>Largest PDU accepted; 0 = 65536.</summary>
    public int MaxPdu { get; init; }
    /// <summary>TLS (the server needs an identity); null = plain TCP.</summary>
    public TlsOptions? Tls { get; init; }
    /// <summary>Refuse (0xA900) a C-STORE whose command UIDs differ from the data set's; false: store it under the data set's UIDs and report it.</summary>
    public bool StrictStoreUids { get; init; }
    /// <summary>Folder for data sets received to disk; null = a private per-user temp folder.</summary>
    public string? SpoolDirectory { get; init; }
    /// <summary>Data sets larger than this are received to disk; 0 = 16 MiB.</summary>
    public long SpoolThreshold { get; init; }
    /// <summary>All spool files together; 0 = no cap (over it: 0xA700).</summary>
    public long SpoolMaxTotalBytes { get; init; }
    /// <summary>Free disk space the spool leaves; 0 = 64 MiB.</summary>
    public long SpoolMinFreeDiskBytes { get; init; }
    /// <summary>Events queued for <see cref="StorageScp.Received"/> handlers; 0 = 1024. See the drop policy there.</summary>
    public int EventQueueLimit { get; init; }
}

/// <summary>When the Storage Commitment SCP sends its report (siaara_scp_services.commitment_report_mode).</summary>
public enum CommitmentReportMode
{
    /// <summary>On the requestor's association; on a new one when it is gone.</summary>
    SameThenNew = C.SIAARA_COMMIT_REPORT_SAME_THEN_NEW,
    /// <summary>Only on the requestor's association.</summary>
    Same = C.SIAARA_COMMIT_REPORT_SAME,
    /// <summary>Always on a new association to the requestor (from <see cref="ScpServiceOptions.AeTable"/>).</summary>
    New = C.SIAARA_COMMIT_REPORT_NEW,
}

/// <summary>A service of the SCP, for <see cref="ScpServiceOptions.AllowLists"/>.</summary>
public enum ScpService
{
    /// <summary>C-ECHO.</summary>
    Verification = C.SIAARA_SVC_VERIFICATION,
    /// <summary>C-STORE.</summary>
    Storage = C.SIAARA_SVC_STORAGE,
    /// <summary>Q/R C-FIND.</summary>
    Find = C.SIAARA_SVC_FIND,
    /// <summary>Q/R C-MOVE.</summary>
    Move = C.SIAARA_SVC_MOVE,
    /// <summary>Q/R C-GET.</summary>
    Get = C.SIAARA_SVC_GET,
    /// <summary>Modality Worklist.</summary>
    Worklist = C.SIAARA_SVC_WORKLIST,
    /// <summary>MPPS.</summary>
    Mpps = C.SIAARA_SVC_MPPS,
    /// <summary>Storage Commitment.</summary>
    Commitment = C.SIAARA_SVC_COMMITMENT,
    /// <summary>UPS.</summary>
    Ups = C.SIAARA_SVC_UPS,
    /// <summary>Instance Availability Notification.</summary>
    InstanceAvailability = C.SIAARA_SVC_IAN,
}

/// <summary>A remote AE the SCP opens associations to (C-MOVE destination, commitment / UPS report receiver).</summary>
/// <param name="AeTitle">Its AE title.</param>
/// <param name="Host">Host name or address.</param>
/// <param name="Port">TCP port.</param>
/// <param name="Tls">Use TLS (<see cref="ScpServiceOptions.OutboundTls"/>).</param>
public sealed record ScpAeEntry(string AeTitle, string Host, int Port, bool Tls = false);

/// <summary>A Modality Worklist query: the requestor and its identifier (valid during the provider call).</summary>
/// <param name="CallingAeTitle">The requestor.</param>
/// <param name="Identifier">The query keys.</param>
public sealed record WorklistQuery(string CallingAeTitle, DicomDataset Identifier);

/// <summary>The services of <see cref="StorageScp.Start(StorageScpOptions, ScpServiceOptions, DicomStore)"/> (siaara_scp_services).</summary>
public sealed record ScpServiceOptions
{
    /// <summary>Storage SCP into the store.</summary>
    public bool Storage { get; init; } = true;
    /// <summary>C-FIND (Patient and Study Root, every level) answered from the store.</summary>
    public bool Find { get; init; }
    /// <summary>C-MOVE from the store to <see cref="AeTable"/> destinations.</summary>
    public bool Move { get; init; }
    /// <summary>C-GET from the store.</summary>
    public bool Get { get; init; }
    /// <summary>Modality Worklist (<see cref="WorklistProvider"/>, else <see cref="StorageScp.SetWorklist"/>).</summary>
    public bool Worklist { get; init; }
    /// <summary>MPPS SCP (N-CREATE / N-SET).</summary>
    public bool Mpps { get; init; }
    /// <summary>Storage Commitment SCP decided from the store.</summary>
    public bool Commitment { get; init; }
    /// <summary>When commitment reports are sent.</summary>
    public CommitmentReportMode CommitmentReportMode { get; init; }
    /// <summary>Receive Storage Commitment reports sent on new associations (for <see cref="Dimse.Commit(DimsePeer, IReadOnlyList{ValueTuple{string, string}}, CommitmentOptions, System.Threading.CancellationToken)"/>).</summary>
    public bool CommitmentReports { get; init; }
    /// <summary>UPS Push / Pull / Watch / Query SCP.</summary>
    public bool Ups { get; init; }
    /// <summary>Instance Availability Notification SCP (events).</summary>
    public bool InstanceAvailability { get; init; }
    /// <summary>Conversions (StoreTransferSyntax, C-GET / C-MOVE sub-operations) may be lossy.</summary>
    public bool AllowLossy { get; init; }
    /// <summary>Storage SOP classes accepted; null = the toolkit's list.</summary>
    public IReadOnlyList<string>? StorageSopClasses { get; init; }
    /// <summary>Transfer syntaxes accepted for storage, preferred first; null = native (+ compressed with AcceptCompressed).</summary>
    public IReadOnlyList<string>? TransferSyntaxes { get; init; }
    /// <summary>Convert received instances to this syntax before storing; null = keep.</summary>
    public string? StoreTransferSyntax { get; init; }
    /// <summary>C-MOVE destinations and report receivers.</summary>
    public IReadOnlyList<ScpAeEntry> AeTable { get; init; } = Array.Empty<ScpAeEntry>();
    /// <summary>TLS client settings for <see cref="AeTable"/> entries with Tls.</summary>
    public TlsOptions? OutboundTls { get; init; }
    /// <summary>C-FIND matches; 0 = 10000.</summary>
    public int MaxFindResults { get; init; }
    /// <summary>Instances per C-MOVE / C-GET; 0 = 65535.</summary>
    public int MaxRetrieveInstances { get; init; }
    /// <summary>Accept relational queries and retrieval.</summary>
    public bool Relational { get; init; }
    /// <summary>Accept fuzzy semantic matching of person names.</summary>
    public bool FuzzyPersonNames { get; init; }
    /// <summary>Accept combined date-time matching.</summary>
    public bool CombinedDateTime { get; init; }
    /// <summary>
    /// Supplies candidate worklist items for each query (on the association's thread, possibly several
    /// at once). The toolkit applies the matching rules and returns only the requested keys, so
    /// returning every item is correct. Null: the list of <see cref="StorageScp.SetWorklist"/>.
    /// </summary>
    public Func<WorklistQuery, IEnumerable<DicomDataset>>? WorklistProvider { get; init; }
    /// <summary>Worklist items per query; 0 = 10000.</summary>
    public int MaxWorklistItems { get; init; }
    /// <summary>MPPS instances kept; 0 = 10000.</summary>
    public int MaxMppsInstances { get; init; }
    /// <summary>Instances per commitment request; 0 = 10000.</summary>
    public int MaxCommitmentItems { get; init; }
    /// <summary>Per-service allow-lists of calling AE titles (a service without an entry is open to every accepted peer).</summary>
    public IReadOnlyDictionary<ScpService, IReadOnlyList<string>> AllowLists { get; init; } =
        new Dictionary<ScpService, IReadOnlyList<string>>();
}

/// <summary>Kind of <see cref="ScpEvent"/>.</summary>
public enum ScpEventKind
{
    /// <summary>An instance was added to the store.</summary>
    Stored = C.SIAARA_SCP_STORED,
    /// <summary>An instance could not be stored.</summary>
    StoreFailed = C.SIAARA_SCP_STORE_FAILED,
    /// <summary>C-ECHO.</summary>
    Echo = C.SIAARA_SCP_ECHO,
    /// <summary>C-FIND answered from the store (Completed = matches).</summary>
    Find = C.SIAARA_SCP_FIND,
    /// <summary>C-MOVE finished.</summary>
    Move = C.SIAARA_SCP_MOVE,
    /// <summary>C-GET finished.</summary>
    Get = C.SIAARA_SCP_GET,
    /// <summary>Worklist query answered (Completed = items).</summary>
    Worklist = C.SIAARA_SCP_WORKLIST,
    /// <summary>MPPS N-CREATE.</summary>
    MppsCreate = C.SIAARA_SCP_MPPS_CREATE,
    /// <summary>MPPS N-SET.</summary>
    MppsSet = C.SIAARA_SCP_MPPS_SET,
    /// <summary>Storage Commitment request decided (Outcome 1: report delivered).</summary>
    CommitmentRequest = C.SIAARA_SCP_COMMIT_REQUEST,
    /// <summary>Storage Commitment report received.</summary>
    CommitmentReport = C.SIAARA_SCP_COMMIT_REPORT,
    /// <summary>UPS request served.</summary>
    Ups = C.SIAARA_SCP_UPS,
    /// <summary>A request refused by an allow-list.</summary>
    Refused = C.SIAARA_SCP_REFUSED,
    /// <summary>A lossy sub-operation gave an instance a new SOP Instance UID.</summary>
    UidChanged = C.SIAARA_SCP_UID_CHANGED,
    /// <summary>Instance Availability Notification received.</summary>
    InstanceAvailability = C.SIAARA_SCP_IAN,
}

/// <summary>An event of the SCP.</summary>
/// <param name="Kind">Kind.</param>
/// <param name="DimseStatus">Status returned to the peer.</param>
/// <param name="CallingAeTitle">The peer's AE title.</param>
/// <param name="SopClassUid">SOP Class UID (empty for C-ECHO).</param>
/// <param name="SopInstanceUid">SOP Instance UID (empty for C-ECHO).</param>
public sealed record ScpEvent(ScpEventKind Kind, int DimseStatus, string CallingAeTitle, string SopClassUid, string SopInstanceUid)
{
    /// <summary>Stored: how the store took it (added, replaced, kept the existing one); commitment request: 1 = report delivered.</summary>
    public int Outcome { get; init; }
    /// <summary>The peer's network address (when known).</summary>
    public string PeerAddress { get; init; } = string.Empty;
    /// <summary>The C-STORE command's UIDs differed from the data set's (stored under the data set's).</summary>
    public bool UidMismatch { get; init; }
    /// <summary>The command's Affected SOP Class UID when <see cref="UidMismatch"/>.</summary>
    public string CommandSopClassUid { get; init; } = string.Empty;
    /// <summary>The command's Affected SOP Instance UID when <see cref="UidMismatch"/>.</summary>
    public string CommandSopInstanceUid { get; init; } = string.Empty;
    /// <summary>The UID before a lossy conversion gave the instance a new one.</summary>
    public string OriginalSopInstanceUid { get; init; } = string.Empty;
    /// <summary>Storage Commitment Transaction UID.</summary>
    public string TransactionUid { get; init; } = string.Empty;
    /// <summary>C-MOVE destination.</summary>
    public string DestinationAeTitle { get; init; } = string.Empty;
    /// <summary>Completed sub-operations / matches / committed instances.</summary>
    public int Completed { get; init; }
    /// <summary>Failed ones.</summary>
    public int Failed { get; init; }
    /// <summary>Ones with a warning.</summary>
    public int Warning { get; init; }
    /// <summary>DIMSE command field of the request.</summary>
    public int CommandField { get; init; }
    /// <summary>The event's data set (MPPS attributes, identifier, commitment data, ...) as DICOM JSON; null when none.</summary>
    public string? DatasetJson { get; init; }
}

/// <summary>Counters of a running SCP.</summary>
/// <param name="ActiveAssociations">Associations now.</param>
/// <param name="AssociationsHandled">Handled so far.</param>
/// <param name="AssociationsRejected">Rejected so far.</param>
/// <param name="InstancesStored">Stored.</param>
/// <param name="StoreFailures">Failed.</param>
/// <param name="Echoes">C-ECHO requests.</param>
/// <param name="BytesReceived">Data set bytes of stored instances.</param>
public sealed record StorageScpStatistics(long ActiveAssociations, long AssociationsHandled, long AssociationsRejected, long InstancesStored,
                                          long StoreFailures, long Echoes, long BytesReceived)
{
    /// <summary>Events not delivered to the handlers (drop policy).</summary>
    public long EventsDropped { get; init; }
    /// <summary>C-FIND requests answered.</summary>
    public long Finds { get; init; }
    /// <summary>C-MOVE and C-GET requests.</summary>
    public long Retrieves { get; init; }
    /// <summary>Their C-STORE sub-operations.</summary>
    public long SubOperations { get; init; }
    /// <summary>MPPS N-CREATE and N-SET.</summary>
    public long Mpps { get; init; }
    /// <summary>Storage Commitment requests answered.</summary>
    public long Commitments { get; init; }
    /// <summary>Requests refused by an allow-list or a limit.</summary>
    public long Refused { get; init; }
}

/// <summary>
/// The SCP (siaara_scp): Verification and Storage into a <see cref="DicomStore"/>, and with
/// <see cref="ScpServiceOptions"/> every other service (Q/R from the store, Worklist, MPPS,
/// Storage Commitment, UPS, Instance Availability) on the same port. Runs on its own threads;
/// <see cref="Received"/> is raised on one dispatcher thread, one event at a time (in Blazor use
/// InvokeAsync). Drop policy: once <see cref="StorageScpOptions.EventQueueLimit"/> events wait
/// for the handlers, an association waits at most 1 s for room, then the event is dropped and
/// counted (<see cref="StorageScpStatistics.EventsDropped"/>); after Stop nothing waits.
/// Dispose stops it.
/// </summary>
public sealed class StorageScp : IDisposable
{
    private readonly ScpHandle _handle;
    private readonly DicomStore _store;  // kept alive while the SCP runs
    private readonly Relay _relay;
    private readonly WorklistRelay? _worklist;

    private StorageScp(ScpHandle handle, DicomStore store, Relay relay, WorklistRelay? worklist)
    {
        _handle = handle;
        _store = store;
        _relay = relay;
        _worklist = worklist;
    }

    /// <summary>Raised after each event.</summary>
    public event EventHandler<ScpEvent>? Received
    {
        add => _relay.Handlers += value;
        remove => _relay.Handlers -= value;
    }

    internal ScpHandle Handle => _handle;

    /// <summary>Starts a Storage (and Verification) SCP.</summary>
    /// <param name="options">Options.</param>
    /// <param name="store">Destination store (must stay open while the SCP runs).</param>
    /// <returns>The running SCP.</returns>
    public static StorageScp Start(StorageScpOptions options, DicomStore store) => StartCore(options, null, store);

    /// <summary>Starts an SCP with the given services on one port.</summary>
    /// <param name="options">Options.</param>
    /// <param name="services">Services.</param>
    /// <param name="store">The store the services receive into and answer from.</param>
    /// <returns>The running SCP.</returns>
    public static StorageScp Start(StorageScpOptions options, ScpServiceOptions services, DicomStore store)
    {
        ArgumentNullException.ThrowIfNull(services);
        return StartCore(options, services, store);
    }

    private static StorageScp StartCore(StorageScpOptions options, ScpServiceOptions? services, DicomStore store)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(store);
        using var mem = new NativeAllocations();
        static nint Strings(NativeAllocations m, IReadOnlyList<string>? list)
        {
            if (list is null || list.Count == 0)
            {
                return 0;
            }
            var a = new nint[list.Count];
            for (int i = 0; i < a.Length; i++)
            {
                a[i] = m.Utf8(list[i] ?? throw new ArgumentException("null entry in a list"));
            }
            return m.Array<nint>(a);
        }
        static int Ms(TimeSpan? t) => t is null ? 0 : (int)Math.Clamp(t.Value.TotalMilliseconds, 1, int.MaxValue);
        static uint U(int v) => (uint)Math.Max(0, v);
        static ulong UL(long v) => (ulong)Math.Max(0, v);
        var o = new siaara_scp_options
        {
            struct_size = StructSize.Of<siaara_scp_options>(),
            port = (uint)Math.Clamp(options.Port, 0, 65536),
            ae_title = mem.Utf8(options.AeTitle),
            bind_address = mem.Utf8(options.BindAddress),
            allowed_calling_aes = Strings(mem, options.AllowedCallingAeTitles),
            allowed_count = (uint)options.AllowedCallingAeTitles.Count,
            max_associations = U(options.MaxAssociations),
            accept_any_called_ae = options.AcceptAnyCalledAeTitle ? 1 : 0,
            accept_any_sop_class = options.AcceptAnySopClass ? 1 : 0,
            accept_compressed = options.AcceptCompressed ? 1 : 0,
            duplicate_policy = options.ReplaceDuplicates ? C.SIAARA_DUP_REPLACE : C.SIAARA_DUP_KEEP,
            idle_timeout_ms = Ms(options.IdleTimeout),
            dimse_timeout_ms = Ms(options.DimseTimeout),
            max_pdu = U(options.MaxPdu),
            allow_any_calling_ae = options.AllowAnyCallingAeTitle ? 1 : 0,
            strict_store_uids = options.StrictStoreUids ? 1 : 0,
            spool_directory = mem.Utf8(options.SpoolDirectory),
            spool_threshold = UL(options.SpoolThreshold),
            spool_max_total_bytes = UL(options.SpoolMaxTotalBytes),
            spool_min_free_disk_bytes = UL(options.SpoolMinFreeDiskBytes),
            event_queue_limit = U(options.EventQueueLimit),
        };
        if (options.Tls is not null)
        {
            siaara_tls_options t = options.Tls.ToNative(mem);
            o.tls = mem.Array(new ReadOnlySpan<siaara_tls_options>(in t));
        }
        var relay = new Relay();
        WorklistRelay? worklist = services?.WorklistProvider is null ? null : new WorklistRelay(services.WorklistProvider);
        GCHandle wgc = worklist is null ? default : GCHandle.Alloc(worklist);
        siaara_scp_services sv = default;
        if (services is not null)
        {
            var ae = new siaara_ae_entry[services.AeTable.Count];
            for (int i = 0; i < ae.Length; i++)
            {
                ScpAeEntry e = services.AeTable[i] ?? throw new ArgumentException("null AE table entry");
                ae[i] = new siaara_ae_entry
                {
                    struct_size = StructSize.Of<siaara_ae_entry>(),
                    port = (uint)Math.Clamp(e.Port, 0, 65536),
                    ae_title = mem.Utf8(e.AeTitle),
                    host = mem.Utf8(e.Host),
                    tls = e.Tls ? 1 : 0,
                };
            }
            var acls = new List<siaara_scp_acl>();
            foreach (KeyValuePair<ScpService, IReadOnlyList<string>> kv in services.AllowLists)
            {
                acls.Add(new siaara_scp_acl
                {
                    struct_size = StructSize.Of<siaara_scp_acl>(),
                    service = (int)kv.Key,
                    calling_aes = Strings(mem, kv.Value),
                    count = (uint)(kv.Value?.Count ?? 0),
                });
            }
            sv = new siaara_scp_services
            {
                struct_size = StructSize.Of<siaara_scp_services>(),
                storage = services.Storage ? 1 : 0,
                find = services.Find ? 1 : 0,
                move = services.Move ? 1 : 0,
                get = services.Get ? 1 : 0,
                worklist = services.Worklist ? 1 : 0,
                mpps = services.Mpps ? 1 : 0,
                commitment = services.Commitment ? 1 : 0,
                commitment_report_mode = (int)services.CommitmentReportMode,
                commitment_reports = services.CommitmentReports ? 1 : 0,
                ups = services.Ups ? 1 : 0,
                instance_availability = services.InstanceAvailability ? 1 : 0,
                allow_lossy = services.AllowLossy ? 1 : 0,
                storage_sop_classes = Strings(mem, services.StorageSopClasses),
                storage_sop_class_count = (uint)(services.StorageSopClasses?.Count ?? 0),
                transfer_syntaxes = Strings(mem, services.TransferSyntaxes),
                transfer_syntax_count = (uint)(services.TransferSyntaxes?.Count ?? 0),
                store_transfer_syntax = mem.Utf8(services.StoreTransferSyntax),
                ae_table = mem.Array<siaara_ae_entry>(ae),
                ae_count = (uint)ae.Length,
                max_find_results = U(services.MaxFindResults),
                max_retrieve_instances = U(services.MaxRetrieveInstances),
                relational = services.Relational ? 1 : 0,
                fuzzy_person_names = services.FuzzyPersonNames ? 1 : 0,
                combined_datetime = services.CombinedDateTime ? 1 : 0,
                worklist_cb = worklist is null ? 0 : Callbacks.WorklistPointer,
                worklist_user = worklist is null ? 0 : GCHandle.ToIntPtr(wgc),
                max_worklist_items = U(services.MaxWorklistItems),
                max_mpps_instances = U(services.MaxMppsInstances),
                max_commitment_items = U(services.MaxCommitmentItems),
                acl_count = (uint)acls.Count,
                acls = mem.Array<siaara_scp_acl>(acls.ToArray()),
            };
            if (services.OutboundTls is not null)
            {
                siaara_tls_options t = services.OutboundTls.ToNative(mem);
                sv.outbound_tls = mem.Array(new ReadOnlySpan<siaara_tls_options>(in t));
            }
        }
        ScpHandle? h = new ScpHandle();
        GCHandle gc = GCHandle.Alloc(relay);
        bool added = false;
        store.Handle.DangerousAddRef(ref added);
        try
        {
            nint p;
            int st = services is null
                ? Native.siaara_scp_start(in o, store.Handle.DangerousGetHandle(), Callbacks.ScpEventPointer, GCHandle.ToIntPtr(gc), out p)
                : Native.siaara_scp_start_ex(in o, in sv, store.Handle.DangerousGetHandle(), Callbacks.ScpEventPointer,
                    GCHandle.ToIntPtr(gc), out p);
            if (st != C.SIAARA_OK)
            {
                gc.Free();
                if (wgc.IsAllocated)
                {
                    wgc.Free();
                }
                throw Status.Error(st);
            }
            h.Attach(p);
            h.CallbackTarget = gc;
            h.WorklistTarget = wgc;
            var scp = new StorageScp(h, store, relay, worklist);
            h = null;
            return scp;
        }
        finally
        {
            h?.Dispose();
            if (added)
            {
                store.Handle.DangerousRelease();
            }
        }
    }

    /// <summary>The listening port.</summary>
    public int Port
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_scp_port(l.Pointer, out uint p));
            return (int)p;
        }
    }

    /// <summary>The store instances are added to.</summary>
    public DicomStore Store => _store;

    /// <summary>Counters.</summary>
    /// <returns>A snapshot.</returns>
    public StorageScpStatistics GetStatistics()
    {
        var s = new siaara_scp_stats { struct_size = StructSize.Of<siaara_scp_stats>() };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_scp_stats_get(l.Pointer, ref s));
        return new StorageScpStatistics((long)s.active_associations, (long)s.associations_handled, (long)s.associations_rejected,
            (long)s.instances_stored, (long)s.store_failures, (long)s.echoes, (long)s.bytes_received)
        {
            EventsDropped = (long)s.events_dropped,
            Finds = (long)s.finds,
            Retrieves = (long)s.retrieves,
            SubOperations = (long)s.sub_operations,
            Mpps = (long)s.mpps,
            Commitments = (long)s.commitments,
            Refused = (long)s.refused,
        };
    }

    /// <summary>Replaces the served worklist (used when no <see cref="ScpServiceOptions.WorklistProvider"/> is set).</summary>
    /// <param name="items">Scheduled procedure steps (copied).</param>
    public void SetWorklist(IReadOnlyList<DicomDataset> items)
    {
        ArgumentNullException.ThrowIfNull(items);
        var held = new List<System.Runtime.InteropServices.SafeHandle>(items.Count);
        try
        {
            var ptrs = new nint[items.Count];
            for (int i = 0; i < ptrs.Length; i++)
            {
                DicomDataset d = items[i] ?? throw new ArgumentException("null item", nameof(items));
                bool added = false;
                d.File.Handle.DangerousAddRef(ref added);
                if (added)
                {
                    held.Add(d.File.Handle);
                }
                ptrs[i] = d.Resolve(d.File.Handle.DangerousGetHandle());
            }
            using var sl = new HandleLease(_handle);
            Status.Check(Native.siaara_scp_set_worklist(sl.Pointer, ptrs, (uint)ptrs.Length));
        }
        finally
        {
            foreach (System.Runtime.InteropServices.SafeHandle h in held)
            {
                h.DangerousRelease();
            }
        }
    }

    /// <summary>Replaces the served worklist from a DICOM JSON array.</summary>
    /// <param name="json">The items.</param>
    public void SetWorklistJson(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_scp_set_worklist_json(l.Pointer, System.Text.Encoding.UTF8.GetBytes(json)));
    }

    /// <summary>The current state of an MPPS instance received by the SCP, or null when unknown.</summary>
    /// <param name="sopInstanceUid">MPPS SOP Instance UID.</param>
    /// <returns>A file holding its attributes (dispose it), or null.</returns>
    public DicomFile? GetMpps(string sopInstanceUid)
    {
        ArgumentException.ThrowIfNullOrEmpty(sopInstanceUid);
        using var l = new HandleLease(_handle);
        int st = Native.siaara_scp_mpps_get(l.Pointer, Status.Utf8Z(sopInstanceUid), out nint p);
        if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return null;
        }
        Status.Check(st);
        return DicomFile.FromOwnedPointer(p);
    }

    /// <summary>Schedules a UPS workitem locally (Procedure Step State must be SCHEDULED).</summary>
    /// <param name="attributes">The workitem.</param>
    /// <param name="sopInstanceUid">Its UID, or null for a new one.</param>
    /// <returns>The UID used.</returns>
    public string CreateUps(DicomDataset attributes, string? sopInstanceUid = null)
    {
        ArgumentNullException.ThrowIfNull(attributes);
        using var fl = new HandleLease(attributes.File.Handle);
        nint ds = attributes.Resolve(fl.Pointer);
        using var l = new HandleLease(_handle);
        // One call (it creates the workitem): a buffer that holds any UID.
        Span<byte> buf = stackalloc byte[80];
        Status.Check(Native.siaara_scp_ups_create(l.Pointer, Status.Utf8Z(sopInstanceUid), ds, buf, out nuint n));
        return System.Text.Encoding.UTF8.GetString(buf[..(int)n]);
    }

    /// <summary>The current state of a UPS workitem, or null when unknown.</summary>
    /// <param name="sopInstanceUid">Workitem UID.</param>
    /// <returns>A file holding it (dispose it), or null.</returns>
    public DicomFile? GetUps(string sopInstanceUid)
    {
        ArgumentException.ThrowIfNullOrEmpty(sopInstanceUid);
        using var l = new HandleLease(_handle);
        int st = Native.siaara_scp_ups_get(l.Pointer, Status.Utf8Z(sopInstanceUid), out nint p);
        if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return null;
        }
        Status.Check(st);
        return DicomFile.FromOwnedPointer(p);
    }

    /// <summary>Stops accepting and aborts running associations (idempotent).</summary>
    public void Stop()
    {
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_scp_stop(l.Pointer));
    }

    /// <summary>Stops and frees the SCP.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.KeepAlive(_worklist);
        GC.SuppressFinalize(this);
    }

    internal static string? DatasetJson(nint dataset) =>
        dataset == 0 ? null : Status.ReadString(dataset, static (d, b, out n) => Native.siaara_dataset_to_json(d, 0, b, out n));

    private sealed class Relay : IScpSink
    {
        internal EventHandler<ScpEvent>? Handlers { get; set; }

        public void OnCallbackException(Exception exception)
        {
        }

        public void OnEvent(in ScpEventData e)
        {
            EventHandler<ScpEvent>? h = Handlers;
            if (h is null)
            {
                return;
            }
            h(null, new ScpEvent((ScpEventKind)e.Kind, e.DimseStatus, e.CallingAe, e.SopClassUid, e.SopInstanceUid)
            {
                Outcome = e.Outcome,
                PeerAddress = e.PeerAddress,
                UidMismatch = e.UidMismatch,
                CommandSopClassUid = e.CommandSopClassUid,
                CommandSopInstanceUid = e.CommandSopInstanceUid,
                OriginalSopInstanceUid = e.OriginalSopInstanceUid,
                TransactionUid = e.TransactionUid,
                DestinationAeTitle = e.DestinationAe,
                Completed = e.Completed,
                Failed = e.Failed,
                Warning = e.Warning,
                CommandField = e.CommandField,
                DatasetJson = DatasetJson(e.Dataset),
            });
        }
    }

    private sealed class WorklistRelay : IWorklistSink
    {
        private readonly Func<WorklistQuery, IEnumerable<DicomDataset>> _provider;

        internal WorklistRelay(Func<WorklistQuery, IEnumerable<DicomDataset>> provider) => _provider = provider;

        public void OnCallbackException(Exception exception)
        {
        }

        public uint OnQuery(string callingAe, nint identifier, WorklistEmitter emitter)
        {
            using DicomFile id = Dimse.CloneDataset(identifier);
            foreach (DicomDataset item in _provider(new WorklistQuery(callingAe, id.Dataset)))
            {
                if (item is null)
                {
                    continue;
                }
                using var l = new HandleLease(item.File.Handle);
                if (!emitter.Emit(item.Resolve(l.Pointer)))
                {
                    break;
                }
            }
            return 0;
        }
    }
}

/// <summary>Severity of a library log message.</summary>
public enum SiaaraLogLevel
{
    /// <summary>Debug.</summary>
    Debug = C.SIAARA_LOG_DEBUG,
    /// <summary>Information.</summary>
    Information = C.SIAARA_LOG_INFO,
    /// <summary>Warning.</summary>
    Warning = C.SIAARA_LOG_WARNING,
    /// <summary>Error.</summary>
    Error = C.SIAARA_LOG_ERROR,
}

/// <summary>Process-wide diagnostics of the native library (siaara_set_log_callback).</summary>
public static class SiaaraLog
{
    private static readonly object s_gate = new();
    private static GCHandle s_target;

    /// <summary>
    /// Installs a handler (null removes it). containsPhi is true when the message may hold patient
    /// data or SOP Instance UIDs: keep those out of general logs. The handler may run on several
    /// native threads at once and must not throw (exceptions are dropped).
    /// </summary>
    /// <param name="handler">(level, containsPhi, message), or null.</param>
    /// <param name="minimumLevel">Messages below are not delivered.</param>
    public static void SetHandler(Action<SiaaraLogLevel, bool, string>? handler, SiaaraLogLevel minimumLevel = SiaaraLogLevel.Information)
    {
        lock (s_gate)
        {
            GCHandle old = s_target;
            if (handler is null)
            {
                Status.Check(Native.siaara_set_log_callback(0, 0, (int)minimumLevel));
            }
            else
            {
                GCHandle gc = GCHandle.Alloc(new Sink(handler));
                int st = Native.siaara_set_log_callback(Callbacks.LogPointer, GCHandle.ToIntPtr(gc), (int)minimumLevel);
                if (st != C.SIAARA_OK)
                {
                    gc.Free();
                    throw Status.Error(st);
                }
                s_target = gc;
            }
            if (handler is null)
            {
                s_target = default;
            }
            // A call already running may still use the old target: it stays alive (one small
            // object per replacement) rather than risking a dangling handle.
            _ = old;
        }
    }

    private sealed class Sink : ILogSink
    {
        private readonly Action<SiaaraLogLevel, bool, string> _handler;

        internal Sink(Action<SiaaraLogLevel, bool, string> handler) => _handler = handler;

        public void OnCallbackException(Exception exception)
        {
        }

        public void OnLog(int level, bool containsPhi, string message) => _handler((SiaaraLogLevel)level, containsPhi, message);
    }
}
