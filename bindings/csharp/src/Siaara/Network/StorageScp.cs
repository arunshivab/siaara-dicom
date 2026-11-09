using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Siaara.Interop;
using Siaara.Web;

namespace Siaara.Network;

/// <summary>Options of <see cref="StorageScp"/> (siaara_scp_options).</summary>
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
    /// <summary>Maximum concurrent associations; 0 = 8.</summary>
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
}

/// <summary>An event of the Storage SCP.</summary>
/// <param name="Kind">Kind.</param>
/// <param name="DimseStatus">Status returned to the peer.</param>
/// <param name="CallingAeTitle">The peer's AE title.</param>
/// <param name="SopClassUid">SOP Class UID (empty for C-ECHO).</param>
/// <param name="SopInstanceUid">SOP Instance UID (empty for C-ECHO).</param>
public sealed record ScpEvent(ScpEventKind Kind, int DimseStatus, string CallingAeTitle, string SopClassUid, string SopInstanceUid);

/// <summary>Counters of a running SCP.</summary>
/// <param name="ActiveAssociations">Associations now.</param>
/// <param name="AssociationsHandled">Handled so far.</param>
/// <param name="AssociationsRejected">Rejected so far.</param>
/// <param name="InstancesStored">Stored.</param>
/// <param name="StoreFailures">Failed.</param>
/// <param name="Echoes">C-ECHO requests.</param>
/// <param name="BytesReceived">Data set bytes of stored instances.</param>
public sealed record StorageScpStatistics(long ActiveAssociations, long AssociationsHandled, long AssociationsRejected, long InstancesStored,
                                          long StoreFailures, long Echoes, long BytesReceived);

/// <summary>
/// A Storage SCP (and Verification SCP) adding received instances to a <see cref="DicomStore"/>
/// (siaara_scp). Runs on its own threads; <see cref="Received"/> is raised on them, one at a time
/// (in Blazor use InvokeAsync). Dispose stops it.
/// </summary>
public sealed class StorageScp : IDisposable
{
    private readonly ScpHandle _handle;
    private readonly DicomStore _store;  // kept alive while the SCP runs
    private readonly Relay _relay;

    private StorageScp(ScpHandle handle, DicomStore store, Relay relay)
    {
        _handle = handle;
        _store = store;
        _relay = relay;
    }

    /// <summary>Raised after each stored instance, failure and C-ECHO.</summary>
    public event EventHandler<ScpEvent>? Received
    {
        add => _relay.Handlers += value;
        remove => _relay.Handlers -= value;
    }

    /// <summary>Starts listening.</summary>
    /// <param name="options">Options.</param>
    /// <param name="store">Destination store (must stay open while the SCP runs).</param>
    /// <returns>The running SCP.</returns>
    public static StorageScp Start(StorageScpOptions options, DicomStore store)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(store);
        using var mem = new NativeAllocations();
        var allowed = new nint[options.AllowedCallingAeTitles.Count];
        for (int i = 0; i < allowed.Length; i++)
        {
            allowed[i] = mem.Utf8(options.AllowedCallingAeTitles[i]);
        }
        static int Ms(TimeSpan? t) => t is null ? 0 : (int)Math.Clamp(t.Value.TotalMilliseconds, 1, int.MaxValue);
        var o = new siaara_scp_options
        {
            struct_size = StructSize.Of<siaara_scp_options>(),
            port = (uint)Math.Clamp(options.Port, 0, 65536),
            ae_title = mem.Utf8(options.AeTitle),
            bind_address = mem.Utf8(options.BindAddress),
            allowed_calling_aes = mem.Array<nint>(allowed),
            allowed_count = (uint)allowed.Length,
            max_associations = (uint)Math.Max(0, options.MaxAssociations),
            accept_any_called_ae = options.AcceptAnyCalledAeTitle ? 1 : 0,
            accept_any_sop_class = options.AcceptAnySopClass ? 1 : 0,
            accept_compressed = options.AcceptCompressed ? 1 : 0,
            duplicate_policy = options.ReplaceDuplicates ? C.SIAARA_DUP_REPLACE : C.SIAARA_DUP_KEEP,
            idle_timeout_ms = Ms(options.IdleTimeout),
            dimse_timeout_ms = Ms(options.DimseTimeout),
            max_pdu = (uint)Math.Max(0, options.MaxPdu),
            allow_any_calling_ae = options.AllowAnyCallingAeTitle ? 1 : 0,
        };
        if (options.Tls is not null)
        {
            siaara_tls_options t = options.Tls.ToNative(mem);
            o.tls = mem.Array(new ReadOnlySpan<siaara_tls_options>(in t));
        }
        var relay = new Relay();
        ScpHandle? h = new ScpHandle();
        GCHandle gc = GCHandle.Alloc(relay);
        bool added = false;
        store.Handle.DangerousAddRef(ref added);
        try
        {
            int st = Native.siaara_scp_start(in o, store.Handle.DangerousGetHandle(), Callbacks.ScpEventPointer, GCHandle.ToIntPtr(gc), out nint p);
            if (st != C.SIAARA_OK)
            {
                gc.Free();
                throw Status.Error(st);
            }
            h.Attach(p);
            h.CallbackTarget = gc;
            var scp = new StorageScp(h, store, relay);
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
            (long)s.instances_stored, (long)s.store_failures, (long)s.echoes, (long)s.bytes_received);
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
        GC.SuppressFinalize(this);
    }

    private sealed class Relay : IScpSink
    {
        internal EventHandler<ScpEvent>? Handlers { get; set; }

        public void OnCallbackException(Exception exception)
        {
        }

        public void OnEvent(int kind, int dimseStatus, int outcome, string callingAe, string sopClassUid, string sopInstanceUid) =>
            Handlers?.Invoke(null, new ScpEvent((ScpEventKind)kind, dimseStatus, callingAe, sopClassUid, sopInstanceUid));
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
