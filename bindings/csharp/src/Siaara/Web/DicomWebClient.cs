using System;
using System.Buffers;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;

namespace Siaara.Web;

/// <summary>QIDO-RS search level.</summary>
public enum QidoLevel
{
    /// <summary>Studies.</summary>
    Studies = C.SIAARA_QIDO_STUDIES,
    /// <summary>Series.</summary>
    Series = C.SIAARA_QIDO_SERIES,
    /// <summary>Instances.</summary>
    Instances = C.SIAARA_QIDO_INSTANCES,
}

/// <summary>TLS options (siaara_tls_options). Strings are copied; the password is wiped when freed.</summary>
public sealed record TlsOptions
{
    /// <summary>PEM trust anchors; null for the OS trust store.</summary>
    public string? CaFile { get; init; }
    /// <summary>Client certificate chain, PEM (Linux / OpenSSL only).</summary>
    public string? CertificateFile { get; init; }
    /// <summary>Its private key, PEM (Linux / OpenSSL only).</summary>
    public string? PrivateKeyFile { get; init; }
    /// <summary>Client identity as PKCS#12 (every OS).</summary>
    public string? Pkcs12File { get; init; }
    /// <summary>PKCS#12 / encrypted key password.</summary>
    public string? Password { get; init; }
    /// <summary>Windows: certificate thumbprint in CurrentUser\MY; macOS: keychain identity label.</summary>
    public string? OsIdentity { get; init; }
    /// <summary>Name or IP the server certificate must carry; null for the URL host.</summary>
    public string? ExpectedIdentity { get; init; }
    /// <summary>DANGEROUS: no certificate checks (warned per connection). Test use only.</summary>
    public bool InsecureSkipValidation { get; init; }
    /// <summary>Refuse TLS 1.2.</summary>
    public bool Tls13Only { get; init; }

    /// <summary>Hides the password.</summary>
    /// <returns>A description without secrets.</returns>
    public override string ToString() => "TlsOptions { CaFile = " + CaFile + ", Pkcs12File = " + Pkcs12File + ", ... }";
}

/// <summary>
/// DICOMweb client (QIDO-RS, WADO-RS, STOW-RS) with one persistent HTTP connection
/// (siaara_web_client). The C client is not thread safe; this wrapper serialises calls with a lock,
/// so one instance can be shared, but calls run one at a time: use one client per concurrent flow.
/// </summary>
/// <remarks>
/// The *Async methods run the blocking native call on a thread-pool thread (Task.Run): the C API has
/// no asynchronous I/O. Cancellation only prevents a call that has not started; a running request
/// ends by completing or by the client's timeout.
/// </remarks>
public sealed class DicomWebClient : IDisposable
{
    private readonly WebClientHandle _handle;
    private readonly Lock _gate = new();

    /// <summary>Creates a client for http:// or https:// (OS trust store, no client certificate).</summary>
    /// <param name="baseUrl">e.g. "http://host:8080/dicom-web".</param>
    /// <param name="timeout">Connect and network-wait timeout; null = 30 s.</param>
    public DicomWebClient(string baseUrl, TimeSpan? timeout = null)
        : this(Create(baseUrl, timeout, null))
    {
    }

    /// <summary>Creates a client for an https:// base URL with TLS options.</summary>
    /// <param name="baseUrl">https:// base URL.</param>
    /// <param name="tls">TLS options.</param>
    /// <param name="timeout">Timeout; null = 30 s.</param>
    public DicomWebClient(string baseUrl, TlsOptions tls, TimeSpan? timeout = null)
        : this(Create(baseUrl, timeout, tls ?? throw new ArgumentNullException(nameof(tls))))
    {
    }

    private DicomWebClient(WebClientHandle handle) => _handle = handle;

    private static WebClientHandle Create(string baseUrl, TimeSpan? timeout, TlsOptions? tls)
    {
        ArgumentException.ThrowIfNullOrEmpty(baseUrl);
        int ms = timeout is null ? 0 : (int)Math.Clamp(timeout.Value.TotalMilliseconds, 1, int.MaxValue);
        byte[] url = Status.Utf8(baseUrl);
        int st;
        nint p;
        if (tls is null)
        {
            st = Native.siaara_web_client_create(url, ms, out p);
        }
        else
        {
            using var mem = new NativeAllocations();
            var o = new siaara_tls_options
            {
                struct_size = StructSize.Of<siaara_tls_options>(),
                ca_file = mem.Utf8(tls.CaFile),
                certificate_file = mem.Utf8(tls.CertificateFile),
                private_key_file = mem.Utf8(tls.PrivateKeyFile),
                pkcs12_file = mem.Utf8(tls.Pkcs12File),
                password = mem.Utf8(tls.Password),
                os_identity = mem.Utf8(tls.OsIdentity),
                expected_identity = mem.Utf8(tls.ExpectedIdentity),
                insecure_skip_validation = tls.InsecureSkipValidation ? 1 : 0,
                tls13_only = tls.Tls13Only ? 1 : 0,
            };
            st = Native.siaara_web_client_create_tls(url, ms, in o, out p);
        }
        Status.Check(st);
        var h = new WebClientHandle();
        h.Attach(p);
        return h;
    }

    /// <summary>Sets the Authorization header sent with every request ("Bearer x", "Basic y"); null removes it.</summary>
    /// <param name="value">Header value.</param>
    public void SetAuthorization(string? value)
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_web_client_set_authorization(l.Pointer, value is null ? default : Encoding.UTF8.GetBytes(value)));
        }
    }

    /// <summary>QIDO-RS search; returns the application/dicom+json result array.</summary>
    /// <param name="level">Search level.</param>
    /// <param name="query">URL query without '?', already percent-encoded (e.g. "PatientID=X&amp;limit=10"), or null.</param>
    /// <param name="studyInstanceUid">Narrows the path to a study, or null.</param>
    /// <param name="seriesInstanceUid">Narrows the path to a series, or null.</param>
    /// <returns>The JSON text.</returns>
    /// <exception cref="SiaaraException">Rejected (HTTP failure), NotFound (404), Network, Timeout.</exception>
    public string Search(QidoLevel level, string? query = null, string? studyInstanceUid = null, string? seriesInstanceUid = null)
    {
        byte[]? study = Status.Utf8Z(studyInstanceUid);
        byte[]? series = Status.Utf8Z(seriesInstanceUid);
        byte[]? q = query is null ? null : Encoding.UTF8.GetBytes(query);
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            byte[] buf = ArrayPool<byte>.Shared.Rent(64 * 1024);
            try
            {
                int st = Native.siaara_web_qido(l.Pointer, (int)level, study, series, q, buf, out nuint needed);
                if (st == Status.Ok)
                {
                    return Encoding.UTF8.GetString(buf, 0, (int)needed);
                }
                if (st != Status.BufferTooSmall)
                {
                    throw Status.Error(st);
                }
                // The response stays in the client: read it without a new request.
                return Status.ReadString(l.Pointer, static (nint c, Span<byte> b, out nuint n) => Native.siaara_web_last_response(c, b, out n));
            }
            finally
            {
                ArrayPool<byte>.Shared.Return(buf);
            }
        }
    }

    /// <summary>QIDO-RS on a thread-pool thread (blocking network I/O).</summary>
    /// <param name="level">Search level.</param>
    /// <param name="query">URL query, or null.</param>
    /// <param name="studyInstanceUid">Study, or null.</param>
    /// <param name="seriesInstanceUid">Series, or null.</param>
    /// <param name="cancellationToken">Cancels before the request starts.</param>
    /// <returns>The JSON text.</returns>
    public Task<string> SearchAsync(QidoLevel level, string? query = null, string? studyInstanceUid = null,
        string? seriesInstanceUid = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => Search(level, query, studyInstanceUid, seriesInstanceUid), cancellationToken);

    /// <summary>The body of the last successful QIDO response.</summary>
    /// <returns>The JSON text.</returns>
    public string GetLastResponse()
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            return Status.ReadString(l.Pointer, static (nint c, Span<byte> b, out nuint n) => Native.siaara_web_last_response(c, b, out n));
        }
    }

    /// <summary>WADO-RS retrieve of one instance.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="sopInstanceUid">Instance.</param>
    /// <returns>The instance (caller disposes).</returns>
    public DicomFile RetrieveInstance(string studyInstanceUid, string seriesInstanceUid, string sopInstanceUid)
    {
        ArgumentException.ThrowIfNullOrEmpty(studyInstanceUid);
        ArgumentException.ThrowIfNullOrEmpty(seriesInstanceUid);
        ArgumentException.ThrowIfNullOrEmpty(sopInstanceUid);
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_web_wado_instance(l.Pointer, Status.Utf8Z(studyInstanceUid), Status.Utf8Z(seriesInstanceUid),
                Status.Utf8Z(sopInstanceUid), out nint f));
            return DicomFile.FromOwnedPointer(f);
        }
    }

    /// <summary>WADO-RS on a thread-pool thread (blocking network I/O).</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="sopInstanceUid">Instance.</param>
    /// <param name="cancellationToken">Cancels before the request starts.</param>
    /// <returns>The instance.</returns>
    public Task<DicomFile> RetrieveInstanceAsync(string studyInstanceUid, string seriesInstanceUid, string sopInstanceUid,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => RetrieveInstance(studyInstanceUid, seriesInstanceUid, sopInstanceUid), cancellationToken);

    /// <summary>STOW-RS of one file (in its current transfer syntax).</summary>
    /// <param name="file">The file.</param>
    /// <param name="studyInstanceUid">Target study, or null.</param>
    /// <exception cref="SiaaraException">Rejected when the server refused the instance.</exception>
    public void Store(DicomFile file, string? studyInstanceUid = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            using var f = new HandleLease(file.Handle);
            Status.Check(Native.siaara_web_stow(l.Pointer, f.Pointer, Status.Utf8Z(studyInstanceUid)));
        }
    }

    /// <summary>STOW-RS on a thread-pool thread (blocking network I/O).</summary>
    /// <param name="file">The file.</param>
    /// <param name="studyInstanceUid">Target study, or null.</param>
    /// <param name="cancellationToken">Cancels before the request starts.</param>
    /// <returns>The task.</returns>
    public Task StoreAsync(DicomFile file, string? studyInstanceUid = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => Store(file, studyInstanceUid), cancellationToken);

    /// <summary>Closes the connection.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}
