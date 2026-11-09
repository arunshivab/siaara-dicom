using System;
using System.Buffers;
using System.Collections.Generic;
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

    /// <summary>The C struct; its strings live in mem (wiped when mem is disposed).</summary>
    internal siaara_tls_options ToNative(NativeAllocations mem) => new()
    {
        struct_size = StructSize.Of<siaara_tls_options>(),
        ca_file = mem.Utf8(CaFile),
        certificate_file = mem.Utf8(CertificateFile),
        private_key_file = mem.Utf8(PrivateKeyFile),
        pkcs12_file = mem.Utf8(Pkcs12File),
        password = mem.Utf8(Password),
        os_identity = mem.Utf8(OsIdentity),
        expected_identity = mem.Utf8(ExpectedIdentity),
        insecure_skip_validation = InsecureSkipValidation ? 1 : 0,
        tls13_only = Tls13Only ? 1 : 0,
    };
}

/// <summary>One part of a DICOMweb response.</summary>
/// <param name="ContentType">Content-Type of the part.</param>
/// <param name="Data">The bytes.</param>
public sealed record DicomWebPart(string ContentType, byte[] Data)
{
    /// <summary>Parses the part as a Part 10 stream.</summary>
    /// <returns>The file (caller disposes).</returns>
    public DicomFile ToDicomFile() => DicomFile.Open(Data);
}

/// <summary>
/// DICOMweb client (QIDO-RS, WADO-RS, STOW-RS) with one persistent HTTP connection
/// (siaara_web_client). The C client is not thread safe; this wrapper serialises calls with a lock,
/// so one instance can be shared, but calls run one at a time: use one client per concurrent flow.
/// </summary>
/// <remarks>
/// The *Async methods run the blocking native call on a thread-pool thread (Task.Run): the C API has
/// no asynchronous I/O. Cancelling the token also aborts a request in progress (a native token is
/// attached to the client for the call; its connection is shut down), surfacing as
/// OperationCanceledException.
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
    /// <param name="cancellationToken">Cancels (also a request in progress: its connection is closed).</param>
    /// <returns>The JSON text.</returns>
    public Task<string> SearchAsync(QidoLevel level, string? query = null, string? studyInstanceUid = null,
        string? seriesInstanceUid = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => WithToken(() => Search(level, query, studyInstanceUid, seriesInstanceUid), cancellationToken), cancellationToken);

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
    /// <param name="cancellationToken">Cancels (also a request in progress: its connection is closed).</param>
    /// <returns>The instance.</returns>
    public Task<DicomFile> RetrieveInstanceAsync(string studyInstanceUid, string seriesInstanceUid, string sopInstanceUid,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => WithToken(() => RetrieveInstance(studyInstanceUid, seriesInstanceUid, sopInstanceUid), cancellationToken), cancellationToken);

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
    /// <param name="cancellationToken">Cancels (also a request in progress: its connection is closed).</param>
    /// <returns>The task.</returns>
    public Task StoreAsync(DicomFile file, string? studyInstanceUid = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => WithToken(() =>
        {
            Store(file, studyInstanceUid);
            return true;
        }, cancellationToken), cancellationToken);

    // ------------------------------------------------------------------ cancellation, metadata, frames, bulk retrieve

    /// <summary>
    /// Runs body with a native token attached to the client (siaara_web_client_set_cancel), so
    /// cancelling aborts the request in progress. Cancellation surfaces as OperationCanceledException.
    /// </summary>
    private T WithToken<T>(Func<T> body, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        if (!token.CanBeCanceled)
        {
            return body();
        }
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            using var cancel = new NativeCancellation(token);
            Status.Check(Native.siaara_web_client_set_cancel(l.Pointer, cancel.Pointer));
            try
            {
                return body();
            }
            catch (SiaaraException e) when (e.ErrorCode == SiaaraErrorCode.Cancelled)
            {
                throw new OperationCanceledException(e.Message, e, token);
            }
            finally
            {
                _ = Native.siaara_web_client_set_cancel(l.Pointer, 0);
            }
        }
    }

    /// <summary>WADO-RS metadata (DICOM JSON array) of a study, a series or an instance.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null for the study.</param>
    /// <param name="sopInstanceUid">Instance, or null.</param>
    /// <returns>The JSON text.</returns>
    public string RetrieveMetadata(string studyInstanceUid, string? seriesInstanceUid = null, string? sopInstanceUid = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(studyInstanceUid);
        byte[] st = Status.Utf8Z(studyInstanceUid)!;
        byte[]? se = Status.Utf8Z(seriesInstanceUid), si = Status.Utf8Z(sopInstanceUid);
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            int r = Native.siaara_web_wado_metadata(l.Pointer, st, se, si, default, out _);
            Status.Check(r);
            return Status.ReadString(l.Pointer, static (nint c, Span<byte> b, out nuint n) => Native.siaara_web_last_response(c, b, out n));
        }
    }

    /// <summary>WADO-RS metadata on a thread-pool thread.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null.</param>
    /// <param name="sopInstanceUid">Instance, or null.</param>
    /// <param name="cancellationToken">Cancels (also a request in progress).</param>
    /// <returns>The JSON text.</returns>
    public Task<string> RetrieveMetadataAsync(string studyInstanceUid, string? seriesInstanceUid = null, string? sopInstanceUid = null,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => WithToken(() => RetrieveMetadata(studyInstanceUid, seriesInstanceUid, sopInstanceUid), cancellationToken), cancellationToken);

    /// <summary>WADO-RS frames (1-based) of an instance, one part per frame.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="sopInstanceUid">Instance.</param>
    /// <param name="frames">1-based frame numbers.</param>
    /// <param name="transferSyntaxes">Acceptable syntaxes in preference order ("*" = as stored); null = server default.</param>
    /// <returns>The parts.</returns>
    public IReadOnlyList<DicomWebPart> RetrieveFrames(string studyInstanceUid, string seriesInstanceUid, string sopInstanceUid,
        IReadOnlyList<int> frames, IReadOnlyList<string>? transferSyntaxes = null)
    {
        ArgumentNullException.ThrowIfNull(frames);
        var f = new uint[frames.Count];
        for (int i = 0; i < f.Length; i++)
        {
            ArgumentOutOfRangeException.ThrowIfNegativeOrZero(frames[i], nameof(frames));
            f[i] = (uint)frames[i];
        }
        using var mem = new NativeAllocations();
        nint[] ts = Syntaxes(mem, transferSyntaxes);
        return Parts(l => (Native.siaara_web_wado_frames(l, Status.Utf8Z(studyInstanceUid), Status.Utf8Z(seriesInstanceUid),
            Status.Utf8Z(sopInstanceUid), f, (uint)f.Length, ts, (uint)ts.Length, out nint p), p));
    }

    /// <summary>WADO-RS frames on a thread-pool thread.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="sopInstanceUid">Instance.</param>
    /// <param name="frames">1-based frame numbers.</param>
    /// <param name="transferSyntaxes">Acceptable syntaxes, or null.</param>
    /// <param name="cancellationToken">Cancels (also a request in progress).</param>
    /// <returns>The parts.</returns>
    public Task<IReadOnlyList<DicomWebPart>> RetrieveFramesAsync(string studyInstanceUid, string seriesInstanceUid, string sopInstanceUid,
        IReadOnlyList<int> frames, IReadOnlyList<string>? transferSyntaxes = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => WithToken(() => RetrieveFrames(studyInstanceUid, seriesInstanceUid, sopInstanceUid, frames,
            transferSyntaxes), cancellationToken), cancellationToken);

    /// <summary>WADO-RS retrieve of a study, a series or an instance: Part 10 streams.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null for the whole study.</param>
    /// <param name="sopInstanceUid">Instance, or null.</param>
    /// <param name="transferSyntaxes">Acceptable syntaxes, or null.</param>
    /// <returns>The parts.</returns>
    public IReadOnlyList<DicomWebPart> Retrieve(string studyInstanceUid, string? seriesInstanceUid = null, string? sopInstanceUid = null,
        IReadOnlyList<string>? transferSyntaxes = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(studyInstanceUid);
        using var mem = new NativeAllocations();
        nint[] ts = Syntaxes(mem, transferSyntaxes);
        return Parts(l => (Native.siaara_web_wado_retrieve(l, Status.Utf8Z(studyInstanceUid), Status.Utf8Z(seriesInstanceUid),
            Status.Utf8Z(sopInstanceUid), ts, (uint)ts.Length, out nint p), p));
    }

    /// <summary>WADO-RS retrieve on a thread-pool thread.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null.</param>
    /// <param name="sopInstanceUid">Instance, or null.</param>
    /// <param name="transferSyntaxes">Acceptable syntaxes, or null.</param>
    /// <param name="cancellationToken">Cancels (also a request in progress).</param>
    /// <returns>The parts.</returns>
    public Task<IReadOnlyList<DicomWebPart>> RetrieveAsync(string studyInstanceUid, string? seriesInstanceUid = null,
        string? sopInstanceUid = null, IReadOnlyList<string>? transferSyntaxes = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => WithToken(() => Retrieve(studyInstanceUid, seriesInstanceUid, sopInstanceUid, transferSyntaxes), cancellationToken),
            cancellationToken);

    /// <summary>Retrieves a study / series / instance and adds it to a store (siaara_web_parts_add_to_store).</summary>
    /// <param name="store">Destination.</param>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null.</param>
    /// <param name="replaceDuplicates">Replace instances already present.</param>
    /// <param name="cancellationToken">Cancels (also a request in progress).</param>
    /// <returns>Instances added.</returns>
    public Task<int> RetrieveIntoStoreAsync(DicomStore store, string studyInstanceUid, string? seriesInstanceUid = null,
        bool replaceDuplicates = false, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentException.ThrowIfNullOrEmpty(studyInstanceUid);
        return Task.Run(() => WithToken(() =>
        {
            using var mem = new NativeAllocations();
            lock (_gate)
            {
                using var l = new HandleLease(_handle);
                Status.Check(Native.siaara_web_wado_retrieve(l.Pointer, Status.Utf8Z(studyInstanceUid), Status.Utf8Z(seriesInstanceUid),
                    default, default, 0, out nint p));
                using var parts = new WebPartsHandle();
                parts.Attach(p);
                using var s = new HandleLease(store.Handle);
                Status.Check(Native.siaara_web_parts_add_to_store(p, s.Pointer, replaceDuplicates ? C.SIAARA_DUP_REPLACE : C.SIAARA_DUP_KEEP,
                    out uint added, out _));
                return (int)added;
            }
        }, cancellationToken), cancellationToken);
    }

    /// <summary>WADO-URI (legacy): the response body.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <param name="sopInstanceUid">Object.</param>
    /// <param name="contentType">Content type, or null = application/dicom.</param>
    /// <param name="transferSyntax">Transfer syntax UID, or null.</param>
    /// <returns>The body.</returns>
    public byte[] WadoUri(string studyInstanceUid, string seriesInstanceUid, string sopInstanceUid, string? contentType = null,
        string? transferSyntax = null)
    {
        DicomWebPart[] parts = Parts(l => (Native.siaara_web_wado_uri(l, Status.Utf8Z(studyInstanceUid),
            Status.Utf8Z(seriesInstanceUid), Status.Utf8Z(sopInstanceUid), Status.Utf8Z(contentType), Status.Utf8Z(transferSyntax),
            out nint p), p));
        return parts.Length == 0 ? Array.Empty<byte>() : parts[0].Data;
    }

    private static nint[] Syntaxes(NativeAllocations mem, IReadOnlyList<string>? ts)
    {
        if (ts is null)
        {
            return Array.Empty<nint>();
        }
        var r = new nint[ts.Count];
        for (int i = 0; i < r.Length; i++)
        {
            r[i] = mem.Utf8(ts[i] ?? throw new ArgumentException("null transfer syntax", nameof(ts)));
        }
        return r;
    }

    private DicomWebPart[] Parts(Func<nint, (int Status, nint Parts)> call)
    {
        lock (_gate)
        {
            using var l = new HandleLease(_handle);
            (int st, nint p) = call(l.Pointer);
            Status.Check(st);
            using var h = new WebPartsHandle();
            h.Attach(p);
            Status.Check(Native.siaara_web_parts_count(p, out uint n));
            var list = new DicomWebPart[n];
            for (uint i = 0; i < n; i++)
            {
                var state = (Parts: p, Index: i);
                string ct = Status.ReadString(state, static (s, b, out need) => Native.siaara_web_parts_get(s.Parts, s.Index, out _, out _, b, out need));
                Status.Check(Native.siaara_web_parts_get(p, i, out nint data, out ulong size, default, out _));
                list[i] = new DicomWebPart(ct, NativeSpan.Bytes(data, (nuint)size).ToArray());
            }
            return list;
        }
    }

    /// <summary>Closes the connection.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}
