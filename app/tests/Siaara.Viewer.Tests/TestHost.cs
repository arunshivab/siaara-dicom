// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Host;

namespace Siaara.Viewer.Tests;

/// <summary>A peer check with a fixed answer (acts as "another user" or "unknown").</summary>
internal sealed class FixedPeerCheck(PeerOwner owner) : IPeerUserCheck
{
    public PeerOwner Owner { get; set; } = owner;

    public string Method => "test";

    /// <summary>How often the host asked (once per connection is the rule).</summary>
    public int Calls => Volatile.Read(ref _calls);

    private int _calls;

    public PeerOwner Check(IPEndPoint? local, IPEndPoint? remote)
    {
        Interlocked.Increment(ref _calls);
        return Owner;
    }
}

/// <summary>
/// Starts the real host for tests and talks to it the way a browser does: the request names the
/// app's random *.localhost host (Host header, Origin), the TCP connection goes to 127.0.0.1 and the
/// port, as a browser's own resolver would have it. The audit key is the key file, so no test
/// touches the keychain or DPAPI except the one that is about them.
/// </summary>
internal static class TestHost
{
    public static Task<RunningViewer> StartAsync(string dataDir, IPeerUserCheck? peers = null, bool ipHost = false) =>
        ViewerHost.StartAsync(new HostOptions
        {
            DataDir = dataDir,
            MapStaticAssets = false,
            AuditKeys = AuditKeys.KeyFile,
            Peers = peers,
            IpHost = ipHost,
            Secrets = SecretStores.EncryptedFiles(Path.Combine(dataDir, "secrets")),
        });

    public static string AuditFile(string dataDir) => Path.Combine(dataDir, "audit", "audit.jsonl");

    public static IReadOnlyList<AuditEntry> Alerts(string dataDir, string kind) =>
        [.. AuditLog.ReadEntries(AuditFile(dataDir)).Where(e => e.Event == AuditLog.SecurityAlert && e.Data.TryGetValue("kind", out string? k) && k == kind)];

    // Connects to the loopback interface whatever the host name in the address is.
    private static SocketsHttpHandler Handler(int port) => new()
    {
        UseCookies = false,
        AllowAutoRedirect = false,
        UseProxy = false,
        ConnectCallback = async (_, ct) =>
        {
            var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
            try
            {
                await s.ConnectAsync(new IPEndPoint(IPAddress.Loopback, port), ct).ConfigureAwait(false);
                return new NetworkStream(s, ownsSocket: true);
            }
            catch
            {
                s.Dispose();
                throw;
            }
        },
    };

    public static HttpClient Client(RunningViewer v, bool cookie)
    {
        SocketsHttpHandler? handler = Handler(v.Port);
        try
        {
            // Plain HTTP on the loopback interface: there is no certificate to check (CA5399 does not apply).
#pragma warning disable CA5399
            var c = new HttpClient(handler, disposeHandler: true) { BaseAddress = v.BaseUrl };
#pragma warning restore CA5399
            handler = null;
            if (cookie)
            {
                c.DefaultRequestHeaders.Add("Cookie", v.Security.CookieName + "=" + v.Security.CookieValue);
            }
            return c;
        }
        finally
        {
            handler?.Dispose();
        }
    }

    public static async Task<(ClientWebSocket? Socket, HttpStatusCode Status)> ConnectAsync(RunningViewer v, string session, bool cookie, string? origin,
        string? cookieValue = null)
    {
        var ws = new ClientWebSocket();
        ws.Options.CollectHttpResponseDetails = true;
        if (cookie)
        {
            ws.Options.SetRequestHeader("Cookie", v.Security.CookieName + "=" + (cookieValue ?? v.Security.CookieValue));
        }
        if (origin is not null)
        {
            ws.Options.SetRequestHeader("Origin", origin);
        }
#pragma warning disable CA2000 // The invoker owns and disposes the handler.
        using var invoker = new HttpMessageInvoker(Handler(v.Port), disposeHandler: true);
#pragma warning restore CA2000
        try
        {
            await ws.ConnectAsync(new Uri("ws://" + v.Security.Authority + "/ws/view?s=" + session), invoker, CancellationToken.None).ConfigureAwait(false);
            return (ws, ws.HttpStatusCode);
        }
        catch (WebSocketException)
        {
            HttpStatusCode code = ws.HttpStatusCode;
            ws.Dispose();
            return (null, code);
        }
    }

    public static async Task<HttpResponseMessage> PostLaunchAsync(HttpClient c, string secret)
    {
        using var content = new StringContent("k=" + secret, System.Text.Encoding.ASCII, "application/x-www-form-urlencoded");
        return await c.PostAsync(new Uri("/launch", UriKind.Relative), content).ConfigureAwait(false);
    }

    public static string PortText(RunningViewer v) => v.Port.ToString(CultureInfo.InvariantCulture);
}
