using System;
using System.Runtime.InteropServices;
using Siaara.Interop;

namespace Siaara.Web;

/// <summary>Options of <see cref="DicomWebServer"/> (siaara_web_server_options).</summary>
public sealed record DicomWebServerOptions
{
    /// <summary>Port; 0 = ephemeral (see <see cref="DicomWebServer.Port"/>).</summary>
    public int Port { get; init; }
    /// <summary>Bind address; null = 127.0.0.1. Off loopback the server needs TLS plus <see cref="Authorize"/> or client certificates.</summary>
    public string? BindAddress { get; init; }
    /// <summary>Prefix of every route; null = "/dicomweb".</summary>
    public string? BasePath { get; init; }
    /// <summary>Absolute URL of the base path for BulkDataURI / RetrieveURL; null = from the Host header.</summary>
    public string? PublicBaseUrl { get; init; }
    /// <summary>HTTPS with this identity (Pkcs12File, or certificate and key); null = HTTP.</summary>
    public TlsOptions? Tls { get; init; }
    /// <summary>Mutual TLS.</summary>
    public bool RequireClientCertificate { get; init; }
    /// <summary>DANGEROUS: plain HTTP or no access control off loopback.</summary>
    public bool AllowInsecureRemote { get; init; }
    /// <summary>Do not offer STOW-RS.</summary>
    public bool ReadOnly { get; init; }
    /// <summary>STOW-RS replaces instances already in the store.</summary>
    public bool ReplaceDuplicates { get; init; }
    /// <summary>
    /// Access control for every request: (method, target, Authorization header or "") -> allow.
    /// Called on server threads, several at once; null = everything allowed.
    /// </summary>
    public Func<string, string, string, bool>? Authorize { get; init; }
    /// <summary>CORS origin; null = no CORS ("*" is refused with Authorize).</summary>
    public string? CorsAllowOrigin { get; init; }
    /// <summary>Concurrent connections; 0 = 32.</summary>
    public int MaxConnections { get; init; }
    /// <summary>QIDO-RS result cap; 0 = 10000.</summary>
    public int MaxQidoResults { get; init; }
    /// <summary>STOW-RS request body limit; 0 = 256 MiB.</summary>
    public long MaxBodyBytes { get; init; }
}

/// <summary>
/// A DICOMweb origin server over a <see cref="DicomStore"/> (siaara_web_server): QIDO-RS,
/// WADO-RS (instances, metadata, frames, bulk data), WADO-URI and STOW-RS. Runs on its own
/// threads; Dispose stops it.
/// </summary>
public sealed class DicomWebServer : IDisposable
{
    private readonly WebServerHandle _handle;
    private readonly DicomStore _store;

    private DicomWebServer(WebServerHandle handle, DicomStore store)
    {
        _handle = handle;
        _store = store;
    }

    /// <summary>Starts the server.</summary>
    /// <param name="options">Options.</param>
    /// <param name="store">The store it serves (must stay open while the server runs).</param>
    /// <returns>The running server.</returns>
    public static DicomWebServer Start(DicomWebServerOptions options, DicomStore store)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(store);
        using var mem = new NativeAllocations();
        GCHandle gc = options.Authorize is null ? default : GCHandle.Alloc(new AuthorizeSink(options.Authorize));
        var o = new siaara_web_server_options
        {
            struct_size = StructSize.Of<siaara_web_server_options>(),
            port = (uint)Math.Clamp(options.Port, 0, 65536),
            bind_address = mem.Utf8(options.BindAddress),
            base_path = mem.Utf8(options.BasePath),
            public_base_url = mem.Utf8(options.PublicBaseUrl),
            require_client_certificate = options.RequireClientCertificate ? 1 : 0,
            allow_insecure_remote = options.AllowInsecureRemote ? 1 : 0,
            read_only = options.ReadOnly ? 1 : 0,
            duplicate_policy = options.ReplaceDuplicates ? C.SIAARA_DUP_REPLACE : C.SIAARA_DUP_KEEP,
            authorize = options.Authorize is null ? 0 : Callbacks.WebAuthorizePointer,
            authorize_user = options.Authorize is null ? 0 : GCHandle.ToIntPtr(gc),
            cors_allow_origin = mem.Utf8(options.CorsAllowOrigin),
            max_connections = (uint)Math.Max(0, options.MaxConnections),
            max_qido_results = (uint)Math.Max(0, options.MaxQidoResults),
            max_body_bytes = (ulong)Math.Max(0, options.MaxBodyBytes),
        };
        if (options.Tls is not null)
        {
            siaara_tls_options t = options.Tls.ToNative(mem);
            o.tls = mem.Array(new ReadOnlySpan<siaara_tls_options>(in t));
        }
        var h = new WebServerHandle();
        bool added = false;
        store.Handle.DangerousAddRef(ref added);
        try
        {
            int st = Native.siaara_web_server_start(in o, store.Handle.DangerousGetHandle(), out nint p);
            if (st != C.SIAARA_OK)
            {
                if (gc.IsAllocated)
                {
                    gc.Free();
                }
                h.Dispose();
                throw Status.Error(st);
            }
            h.Attach(p);
            h.CallbackTarget = gc;
            return new DicomWebServer(h, store);
        }
        finally
        {
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
            Status.Check(Native.siaara_web_server_port(l.Pointer, out uint p));
            return (int)p;
        }
    }

    /// <summary>The store it serves.</summary>
    public DicomStore Store => _store;

    /// <summary>Stops and frees the server.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }

    private sealed class AuthorizeSink : IWebAuthorizeSink
    {
        private readonly Func<string, string, string, bool> _authorize;

        internal AuthorizeSink(Func<string, string, string, bool> authorize) => _authorize = authorize;

        public void OnCallbackException(Exception exception)
        {
        }

        public bool Authorize(string method, string target, string authorization) => _authorize(method, target, authorization);
    }
}
