using System;
using System.Collections.Generic;
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
}

/// <summary>Result of a Storage Commitment request.</summary>
/// <param name="Result">The operation outcome.</param>
/// <param name="FailureReasons">Per instance: 0 committed, else the Failure Reason, 0xFFFF not reported.</param>
public sealed record CommitmentResult(DimseResult Result, IReadOnlyList<int> FailureReasons);

/// <summary>
/// DIMSE service class user calls (C-ECHO, C-FIND, C-MOVE, C-GET, C-STORE, Storage Commitment).
/// Each call opens one association, runs on a thread-pool thread and is cancellable at any time
/// (the association is aborted), except during the TCP connect. Calls return a
/// <see cref="DimseResult"/> for outcomes the peer decided (Ok, Rejected) and throw
/// <see cref="OperationCanceledException"/> when cancelled and <see cref="SiaaraException"/> for
/// other failures (network, timeout, bad arguments). Progress is reported on the calling native
/// thread; use a Progress&lt;T&gt; to marshal it to the UI.
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

    /// <summary>C-ECHO.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="cancellationToken">Cancels (aborts the association).</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> EchoAsync(DimsePeer peer, CancellationToken cancellationToken = default) =>
        Task.Run(() => Run(peer, (p, c) =>
        {
            siaara_dimse_result r = NewResult();
            int st = Native.siaara_dimse_echo(in p, c, ref r);
            return (st, r);
        }, cancellationToken), cancellationToken);

    /// <summary>C-FIND: every match as a data set (in a DicomFile of its own).</summary>
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
        return Task.Run(() =>
        {
            var sink = new FindSink(maxMatches);
            GCHandle gc = GCHandle.Alloc(sink);
            try
            {
                using var l = new HandleLease(identifier.File.Handle);
                nint id = identifier.Resolve(l.Pointer);
                DimseResult res = Run(peer, (p, c) =>
                {
                    siaara_dimse_result r = NewResult();
                    int st = Native.siaara_dimse_find(in p, (int)model, id, c, Callbacks.FindPointer, GCHandle.ToIntPtr(gc), ref r);
                    if (st == C.SIAARA_ERR_CANCELLED && sink.Limited)
                    {
                        st = C.SIAARA_OK;  // our own C-CANCEL after maxMatches
                    }
                    return (st, r);
                }, cancellationToken);
                sink.ThrowIfFailed();
                return (res, (IReadOnlyList<DicomFile>)sink.Matches);
            }
            catch
            {
                foreach (DicomFile f in sink.Matches)
                {
                    f.Dispose();
                }
                throw;
            }
            finally
            {
                gc.Free();
            }
        }, cancellationToken);
    }

    /// <summary>C-MOVE to a destination AE the peer knows.</summary>
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
        return Task.Run(() =>
        {
            var sink = new ProgressSink(progress);
            GCHandle gc = GCHandle.Alloc(sink);
            try
            {
                using var l = new HandleLease(identifier.File.Handle);
                nint id = identifier.Resolve(l.Pointer);
                byte[] dest = Status.Utf8Z(destinationAeTitle)!;
                DimseResult res = Run(peer, (p, c) =>
                {
                    siaara_dimse_result r = NewResult();
                    int st = Native.siaara_dimse_move(in p, (int)model, id, dest, c, Callbacks.DimseProgressPointer,
                        GCHandle.ToIntPtr(gc), ref r);
                    return (st, r);
                }, cancellationToken);
                sink.ThrowIfFailed();
                return res;
            }
            finally
            {
                gc.Free();
            }
        }, cancellationToken);
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
        return Task.Run(() =>
        {
            var sink = new ProgressSink(progress);
            GCHandle gc = GCHandle.Alloc(sink);
            using var mem = new NativeAllocations();
            try
            {
                using var l = new HandleLease(identifier.File.Handle);
                nint id = identifier.Resolve(l.Pointer);
                bool addedStore = false;
                store?.Handle.DangerousAddRef(ref addedStore);
                try
                {
                    var o = new siaara_get_options
                    {
                        struct_size = StructSize.Of<siaara_get_options>(),
                        store = store is null ? 0 : store.Handle.DangerousGetHandle(),
                        folder = mem.Utf8(folder),
                    };
                    DimseResult res = Run(peer, (p, c) =>
                    {
                        siaara_dimse_result r = NewResult();
                        int st = Native.siaara_dimse_get(in p, (int)model, id, in o, c, Callbacks.DimseProgressPointer,
                            GCHandle.ToIntPtr(gc), ref r);
                        return (st, r);
                    }, cancellationToken);
                    sink.ThrowIfFailed();
                    return res;
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
        }, cancellationToken);
    }

    /// <summary>C-STORE of files over one association, transcoding when the peer needs another syntax.</summary>
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
        return Task.Run(() =>
        {
            var sink = new ProgressSink(progress);
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
                    int st = Native.siaara_dimse_store_files(in p, ptrs, default, (uint)ptrs.Length, c,
                        Callbacks.DimseProgressPointer, GCHandle.ToIntPtr(gc), ref r);
                    return (st, r);
                }, cancellationToken);
                sink.ThrowIfFailed();
                return res;
            }
            finally
            {
                gc.Free();
            }
        }, cancellationToken);
    }

    /// <summary>C-STORE of one file in memory.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="file">The instance.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static Task<DimseResult> StoreAsync(DimsePeer peer, DicomFile file, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(file);
        return Task.Run(() =>
        {
            using var l = new HandleLease(file.Handle);
            nint f = l.Pointer;
            return Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_store(in p, f, c, ref r);
                return (st, r);
            }, cancellationToken);
        }, cancellationToken);
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
        int ms = wait is null ? 0 : (int)Math.Clamp(wait.Value.TotalMilliseconds, 1, int.MaxValue);
        return Task.Run(() =>
        {
            using var mem = new NativeAllocations();
            var cls = new nint[instances.Count];
            var ins = new nint[instances.Count];
            for (int i = 0; i < instances.Count; i++)
            {
                cls[i] = mem.Utf8(instances[i].SopClassUid ?? throw new ArgumentException("null UID", nameof(instances)));
                ins[i] = mem.Utf8(instances[i].SopInstanceUid ?? throw new ArgumentException("null UID", nameof(instances)));
            }
            var reasons = new ushort[instances.Count];
            DimseResult res = Run(peer, (p, c) =>
            {
                siaara_dimse_result r = NewResult();
                int st = Native.siaara_dimse_commit(in p, cls, ins, (uint)cls.Length, ms, c, reasons, ref r);
                return (st, r);
            }, cancellationToken);
            var list = new int[reasons.Length];
            for (int i = 0; i < list.Length; i++)
            {
                list[i] = reasons[i];
            }
            return new CommitmentResult(res, list);
        }, cancellationToken);
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
        private readonly int _max;

        internal FindSink(int max) => _max = max;

        internal List<DicomFile> Matches { get; } = new();

        internal bool Limited { get; private set; }

        public int OnMatch(nint dataset)
        {
            string json = Status.ReadString(dataset, static (d, b, out n) => Native.siaara_dataset_to_json(d, 0, b, out n));
            Matches.Add(DicomFile.FromJson(json));
            if (_max > 0 && Matches.Count >= _max)
            {
                Limited = true;
                return 1;
            }
            return 0;
        }
    }

    private sealed class ProgressSink : SinkBase, IDimseProgressSink
    {
        private readonly IProgress<DimseResult>? _progress;

        internal ProgressSink(IProgress<DimseResult>? progress) => _progress = progress;

        public int OnProgress(in siaara_dimse_result progress)
        {
            _progress?.Report(DimseResult.From(C.SIAARA_OK, in progress, string.Empty));
            return 0;
        }
    }
}
