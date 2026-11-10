// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Security review round 4: R-113 (launch secret off the command line, peer-user check, alerts),
// R-124 (host-only cookie on a random *.localhost name), R-125 (CSP), R-142 (no paths in the audit
// log), R-143 (owner-only files), R-144 (annotation folder names), R-145 (no UIDs in addresses,
// session and connection limits), R-146 (socket limits), R-158 (sign out). The audit log itself
// (R-126, R-127) is in AuditTests.cs.

using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Options;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Viewing;
using Siaara.Viewer.Host;
using Siaara.Viewer.Web.Security;
using Siaara.Viewer.Web.Streaming;

namespace Siaara.Viewer.Tests;

internal static partial class SecurityTests
{
    private static readonly string[] ExpectedFiles = ["audit.jsonl", "audit.anchor", "audit.key", "settings.json", "0.json", "calibration.json", "instance.url", "instance.lock"];
    private static readonly string[] EvilUids = ["..", ".", "", "../..", "..\\..", "a/../../b"];

    // Commands whose members all have the wrong JSON kind, and things that are not commands at all.
    private static readonly string[] WrongKinds =
    [
        "{\"tool\":5,\"layout\":{},\"action\":1,\"activate\":\"x\",\"preset\":[],\"fps\":\"fast\",\"dpr\":null,\"series\":true}",
                    "{\"annotationText\":{\"id\":3,\"text\":4},\"exported\":{\"mode\":7,\"ok\":\"yes\",\"reason\":{}},\"calibrate\":{\"id\":[],\"mm\":{}}}",
                    "{\"colormap\":12,\"syncMember\":\"true\",\"calibrate\":5,\"exported\":\"x\",\"annotationText\":[]}",
                    "[1,2,3]", "\"text\"", "{", "",
    ];

    private const UnixFileMode Mode600 = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private const UnixFileMode Mode700 = Mode600 | UnixFileMode.UserExecute;

    [GeneratedRegex(@"<script(?![^>]*\bsrc=)[^>]*>", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex InlineScript();

    [GeneratedRegex(@"\son[a-z]+\s*=", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex InlineHandler();

    [GeneratedRegex(@"^p:[0-9a-f]{32}/d\d+$", RegexOptions.CultureInvariant)]
    private static partial Regex PathRef();

    // A WebSocket whose peer never reads: every send waits for ever.
    private sealed class StuckWebSocket : WebSocket
    {
        public override WebSocketCloseStatus? CloseStatus => null;
        public override string? CloseStatusDescription => null;
        public override WebSocketState State => WebSocketState.Open;
        public override string? SubProtocol => null;

        public override void Abort()
        {
        }

        public override Task CloseAsync(WebSocketCloseStatus closeStatus, string? statusDescription, CancellationToken cancellationToken) => Task.CompletedTask;

        public override Task CloseOutputAsync(WebSocketCloseStatus closeStatus, string? statusDescription, CancellationToken cancellationToken) => Task.CompletedTask;

        public override void Dispose()
        {
        }

        public override Task<WebSocketReceiveResult> ReceiveAsync(ArraySegment<byte> buffer, CancellationToken cancellationToken) =>
            Task.FromException<WebSocketReceiveResult>(new NotSupportedException());

        public override Task SendAsync(ArraySegment<byte> buffer, WebSocketMessageType messageType, bool endOfMessage, CancellationToken cancellationToken) =>
            Task.Delay(Timeout.Infinite, cancellationToken);
    }

    private static string ProcLine(string local, string remote, uint uid) =>
        $"   7: {local} {remote} 01 00000000:00000000 00:00000000 00000000 {uid,5}        0 123456 1 0000000000000000 20 4 30 10 -1";

    public static void Register()
    {
        T.Case("launch: the secret is never on a command line; the launch page is owner-only and is deleted after use, at expiry and at exit", async () =>
        {
            string data = T.NewTempDir("launchpage");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                string folder = Path.Combine(data, "launch");
                var pages = new LaunchPages(folder);
                v.Security.LaunchUsed += (_, used) => pages.Delete(used.Secret);
                LaunchTicket ticket = v.Security.IssueLaunch(LaunchPages.Lifetime);
                string file = pages.Write(ticket.Endpoint, ticket.Secret);
                // What the browser is started with: the file, nothing that holds the secret.
                string[] args = BrowserLauncher.AppArguments(file);
                T.Check(args.All(a => !a.Contains(ticket.Secret, StringComparison.Ordinal)));
                T.Check(args[0].StartsWith("--app=file://", StringComparison.Ordinal));
                T.Check(!file.Contains(ticket.Secret, StringComparison.Ordinal));
                T.Check(!ticket.Endpoint.AbsoluteUri.Contains(ticket.Secret, StringComparison.Ordinal));
                if (!OperatingSystem.IsWindows())
                {
                    T.Eq(File.GetUnixFileMode(file), Mode600);
                    T.Eq(File.GetUnixFileMode(folder), Mode700);
                }
                T.Check(OwnerOnly.WiderAccess(file) is null);
                string html = await File.ReadAllTextAsync(file).ConfigureAwait(false);
                T.Check(html.Contains("method=\"post\"", StringComparison.Ordinal) && html.Contains("action=\"" + v.Security.Origin + "/launch\"", StringComparison.Ordinal));
                T.Check(html.Contains("name=\"k\" value=\"" + ticket.Secret + "\"", StringComparison.Ordinal));
                // Used: the page is deleted at once.
                using HttpClient anon = TestHost.Client(v, cookie: false);
                using (HttpResponseMessage ok = await TestHost.PostLaunchAsync(anon, ticket.Secret).ConfigureAwait(false))
                {
                    T.Eq(ok.StatusCode, HttpStatusCode.OK);
                }
                T.Check(!File.Exists(file));
                // Unused past its lifetime: deleted by the sweep, and its fingerprint reported so the secret is withdrawn.
                LaunchTicket stale = v.Security.IssueLaunch();
                string staleFile = pages.Write(stale.Endpoint, stale.Secret);
                var expired = new List<string>();
                pages.Expired += expired.Add;
                T.Eq(pages.Sweep(), 0);
                T.Eq(pages.Sweep(nowUtc: DateTime.UtcNow + LaunchPages.Lifetime + TimeSpan.FromSeconds(5)), 1);
                T.Check(!File.Exists(staleFile));
                T.Check(expired.SequenceEqual([LaunchPages.Fingerprint(stale.Secret)]));
                // At exit (and at the next start) every page goes.
                LaunchTicket left = v.Security.IssueLaunch();
                string leftFile = pages.Write(left.Endpoint, left.Secret);
                pages.StartSweeping();
                T.Check(!File.Exists(leftFile));
                leftFile = pages.Write(left.Endpoint, left.Secret);
                pages.Dispose();
                T.Check(!File.Exists(leftFile));
                T.Eq(Directory.GetFiles(folder).Length, 0);

                // The instance file for a second start: only an address on this computer with a well-formed secret is accepted.
                T.Check(InstanceLock.Parse(ticket.Url.AbsoluteUri) is InstanceEntry e && e.Secret == ticket.Secret && e.Endpoint == ticket.Endpoint);
                T.Check(InstanceLock.Parse("http://evil.example:80/launch?k=" + ticket.Secret) is null);
                T.Check(InstanceLock.Parse("http://127.0.0.1:1/launch?k=short") is null);
                T.Check(InstanceLock.Parse("https://127.0.0.1:1/launch?k=" + ticket.Secret) is null);
                T.Check(InstanceLock.Parse("http://127.0.0.1:1/other?k=" + ticket.Secret) is null);
            }
        });

        T.Case("peer: /proc/net/tcp, the Windows TCP table and the macOS socket info are read correctly; only the same user is allowed", async () =>
        {
            // The one rule.
            T.Check(PeerPolicy.Allows(PeerOwner.SameUser));
            T.Check(!PeerPolicy.Allows(PeerOwner.OtherUser));
            T.Check(!PeerPolicy.Allows(PeerOwner.Unknown));
            T.Eq(PeerPolicy.RefusalKind(PeerOwner.OtherUser), "peer-other-user");
            T.Eq(PeerPolicy.RefusalKind(PeerOwner.Unknown), "peer-unknown");

            // Linux: 127.0.0.1:50000 (0xC350) -> 127.0.0.1:8080 (0x1F90). The peer's socket is the line with the ports
            // the other way round from ours; the uid column is its owner.
            string lo = BitConverter.IsLittleEndian ? "0100007F" : "7F000001";
            string lan = BitConverter.IsLittleEndian ? "0A01A8C0" : "C0A8010A";
            string ours = ProcLine(lo + ":1F90", lo + ":C350", 1000), theirs1000 = ProcLine(lo + ":C350", lo + ":1F90", 1000);
            string theirs1001 = ProcLine(lo + ":C350", lo + ":1F90", 1001);
            const string Header = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode";
            T.Check(LinuxProcNet.TryParse(theirs1001, out LinuxProcNet.SocketLine parsed));
            T.Eq((parsed.LocalPort, parsed.RemotePort, parsed.Uid), (50000, 8080, 1001u));
            T.Eq(parsed.Local, IPAddress.Loopback);
            T.Check(!LinuxProcNet.TryParse(Header, out _));
            T.Check(!LinuxProcNet.TryParse("garbage", out _));
            T.Eq(LinuxProcNet.Decide([Header, ours, theirs1000], 8080, 50000, 1000), PeerOwner.SameUser);
            T.Eq(LinuxProcNet.Decide([Header, ours, theirs1001], 8080, 50000, 1000), PeerOwner.OtherUser);
            // Our own accepted socket (uid 1000) must not be taken for the peer's.
            T.Eq(LinuxProcNet.Decide([Header, ours], 8080, 50000, 1000), PeerOwner.Unknown);
            T.Eq(LinuxProcNet.Decide([Header], 8080, 50000, 1000), PeerOwner.Unknown);
            // The same ports on another address are another connection.
            T.Eq(LinuxProcNet.Decide([ProcLine(lan + ":C350", lan + ":1F90", 1000)], 8080, 50000, 1000), PeerOwner.Unknown);
            // Two owners for one port pair: not the same user.
            T.Eq(LinuxProcNet.Decide([theirs1000, theirs1001], 8080, 50000, 1000), PeerOwner.OtherUser);
            // tcp6: ::1 and an IPv4-mapped loopback address.
            string v6lo = BitConverter.IsLittleEndian ? "00000000000000000000000001000000" : "00000000000000000000000000000001";
            string mapped = BitConverter.IsLittleEndian ? "0000000000000000FFFF00000100007F" : "00000000000000000000FFFF7F000001";
            T.Check(LinuxProcNet.TryParse(ProcLine(v6lo + ":C350", v6lo + ":1F90", 5), out LinuxProcNet.SocketLine six) && six.Local.Equals(IPAddress.IPv6Loopback));
            T.Eq(LinuxProcNet.Decide([ProcLine(v6lo + ":C350", v6lo + ":1F90", 1000)], 8080, 50000, 1000), PeerOwner.SameUser);
            T.Eq(LinuxProcNet.Decide([ProcLine(mapped + ":C350", mapped + ":1F90", 1002)], 8080, 50000, 1000), PeerOwner.OtherUser);

            // Windows: MIB_TCPTABLE_OWNER_PID with three rows; ports in network byte order.
            static void Row(Span<byte> row, byte[] local, int localPort, byte[] remote, int remotePort, uint pid)
            {
                BinaryPrimitives.WriteUInt32LittleEndian(row, 5);
                local.CopyTo(row[4..]);
                BinaryPrimitives.WriteUInt16BigEndian(row[8..], (ushort)localPort);
                remote.CopyTo(row[12..]);
                BinaryPrimitives.WriteUInt16BigEndian(row[16..], (ushort)remotePort);
                BinaryPrimitives.WriteUInt32LittleEndian(row[20..], pid);
            }
            var table = new byte[4 + 3 * 24];
            BinaryPrimitives.WriteUInt32LittleEndian(table, 3);
            byte[] loop = [127, 0, 0, 1], other = [192, 168, 1, 10];
            Row(table.AsSpan(4), loop, 8080, loop, 50000, 111);        // our accepted socket
            Row(table.AsSpan(28), loop, 50000, loop, 8080, 4242);      // the peer's socket
            Row(table.AsSpan(52), other, 50000, other, 8080, 999);     // same ports, another address
            T.Eq(WindowsTcpTable.FindOwningPid(table, 8080, 50000), 4242u);
            T.Check(WindowsTcpTable.FindOwningPid(table, 8080, 50001) is null);
            T.Check(WindowsTcpTable.FindOwningPid(table.AsSpan(0, 3), 8080, 50000) is null);
            // A count larger than the buffer does not read past it.
            BinaryPrimitives.WriteUInt32LittleEndian(table, 1000);
            T.Eq(WindowsTcpTable.FindOwningPid(table, 8080, 50000), 4242u);

            // macOS: struct socket_fdinfo with soi_family at 184, soi_kind at 256, insi_fport at 264, insi_lport at 268.
            var info = new byte[MacSocketInfo.BufferBytes];
            BinaryPrimitives.WriteInt32LittleEndian(info.AsSpan(184), 2);
            BinaryPrimitives.WriteInt32LittleEndian(info.AsSpan(256), 2);
            BinaryPrimitives.WriteUInt16BigEndian(info.AsSpan(264), 8080);
            BinaryPrimitives.WriteUInt16BigEndian(info.AsSpan(268), 50000);
            T.Check(MacSocketInfo.TryReadTcpPorts(info, 792, out int lport, out int fport));
            T.Eq((lport, fport), (50000, 8080));
            T.Check(!MacSocketInfo.TryReadTcpPorts(info, 100, out _, out _));
            BinaryPrimitives.WriteInt32LittleEndian(info.AsSpan(256), 3);
            T.Check(!MacSocketInfo.TryReadTcpPorts(info, 792, out _, out _));

            // The real check of this OS on a real loopback connection made by this process: the same user. A
            // connection that does not exist: unknown.
            using var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            using var client = new TcpClient();
            await client.ConnectAsync(IPAddress.Loopback, ((IPEndPoint)listener.LocalEndpoint).Port).ConfigureAwait(false);
            using TcpClient accepted = await listener.AcceptTcpClientAsync().ConfigureAwait(false);
            IPeerUserCheck check = PeerUser.ForThisOs();
            T.Note("peer check on this OS: " + check.Method);
            var local = (IPEndPoint)accepted.Client.LocalEndPoint!;
            var remote = (IPEndPoint)accepted.Client.RemoteEndPoint!;
            T.Eq(check.Check(local, remote), PeerOwner.SameUser);
            T.Eq(check.Check(local, new IPEndPoint(IPAddress.Loopback, remote.Port == 1 ? 2 : 1)), PeerOwner.Unknown);
            T.Eq(check.Check(null, remote), PeerOwner.Unknown);
        });

        T.Case("peer: a connection from another OS user, or from an owner that cannot be determined, gets nothing and is audited", async () =>
        {
            string data = T.NewTempDir("peer");
            var peers = new FixedPeerCheck(PeerOwner.OtherUser);
            RunningViewer v = await TestHost.StartAsync(data, peers).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                LaunchTicket ticket = v.Security.IssueLaunch();
                ViewerSession s = v.Runtime.Sessions.Create("1.2.3.4");
                foreach (PeerOwner owner in new[] { PeerOwner.OtherUser, PeerOwner.Unknown })
                {
                    peers.Owner = owner;
                    // Even with the right secret, the right cookie and the right Origin.
                    using HttpClient stranger = TestHost.Client(v, cookie: true);
                    using (HttpResponseMessage launch = await TestHost.PostLaunchAsync(stranger, ticket.Secret).ConfigureAwait(false))
                    {
                        T.Eq(launch.StatusCode, HttpStatusCode.Forbidden);
                        T.Check(!launch.Headers.Contains("Set-Cookie"));
                        T.Eq((await launch.Content.ReadAsByteArrayAsync().ConfigureAwait(false)).Length, 0);
                    }
                    T.Eq((await stranger.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                    T.Eq((await stranger.GetAsync(new Uri("/_blazor/negotiate", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                    T.Eq((await stranger.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                    (ClientWebSocket? ws, HttpStatusCode code) = await TestHost.ConnectAsync(v, s.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                    T.Check(ws is null);
                    T.Eq(code, HttpStatusCode.Forbidden);
                }
                // The secret was not consumed by the refused attempts: the real user can still use it.
                T.Eq(v.Security.OutstandingSecrets, 1);
                peers.Owner = PeerOwner.SameUser;
                using HttpClient me = TestHost.Client(v, cookie: false);
                int before = peers.Calls;
                using HttpResponseMessage mine = await TestHost.PostLaunchAsync(me, ticket.Secret).ConfigureAwait(false);
                T.Eq(mine.StatusCode, HttpStatusCode.OK);
                // The owner is asked once per connection, not per request (three requests on one kept-alive connection).
                (await me.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false)).Dispose();
                (await me.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false)).Dispose();
                T.Eq(peers.Calls - before, 1);
            }
            T.Check(TestHost.Alerts(data, "peer-other-user").Count >= 1);
            T.Check(TestHost.Alerts(data, "peer-unknown").Count >= 1);
            T.Check(AuditLog.Verify(TestHost.AuditFile(data), AuditKeys.KeyFile).IsIntact);
        });

        T.Case("audit: launches (first, repeated, wrong, expired) and refused Host / cookie / Origin requests are recorded; alerts are rate-limited", async () =>
        {
            string data = T.NewTempDir("alerts");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                using HttpClient anon = TestHost.Client(v, cookie: false);
                using HttpClient authed = TestHost.Client(v, cookie: true);
                LaunchTicket ticket = v.Security.IssueLaunch();
                (await TestHost.PostLaunchAsync(anon, ticket.Secret).ConfigureAwait(false)).Dispose();
                (await TestHost.PostLaunchAsync(anon, ticket.Secret).ConfigureAwait(false)).Dispose();
                (await TestHost.PostLaunchAsync(anon, new string('a', 64)).ConfigureAwait(false)).Dispose();
                LaunchTicket shortLived = v.Security.IssueLaunch(TimeSpan.FromMilliseconds(20));
                await Task.Delay(100).ConfigureAwait(false);
                (await TestHost.PostLaunchAsync(anon, shortLived.Secret).ConfigureAwait(false)).Dispose();
                using (var wrongHost = new HttpRequestMessage(HttpMethod.Get, "/api/health"))
                {
                    wrongHost.Headers.Host = "127.0.0.1:" + TestHost.PortText(v);
                    (await authed.SendAsync(wrongHost).ConfigureAwait(false)).Dispose();
                }
                using (var wrongOrigin = new HttpRequestMessage(HttpMethod.Post, "/api/health"))
                {
                    wrongOrigin.Headers.Add("Origin", "http://evil.example");
                    (await authed.SendAsync(wrongOrigin).ConfigureAwait(false)).Dispose();
                }
                // WebSocket upgrades from a foreign Origin, with the cookie: /ws/view and the Blazor circuit alike are
                // refused by the first middleware (it sees the Upgrade header; it runs before UseWebSockets).
                (ClientWebSocket? foreign, HttpStatusCode foreignCode) = await TestHost.ConnectAsync(v, "x", cookie: true, "http://evil.example").ConfigureAwait(false);
                T.Check(foreign is null && foreignCode == HttpStatusCode.Forbidden);
                using (var circuit = new HttpRequestMessage(HttpMethod.Get, "/_blazor?id=x"))
                {
                    circuit.Headers.Add("Origin", "http://evil.example");
                    circuit.Headers.Add("Connection", "Upgrade");
                    circuit.Headers.Add("Upgrade", "websocket");
                    circuit.Headers.Add("Sec-WebSocket-Version", "13");
                    circuit.Headers.Add("Sec-WebSocket-Key", Convert.ToBase64String(new byte[16]));
                    using HttpResponseMessage refusedCircuit = await authed.SendAsync(circuit).ConfigureAwait(false);
                    T.Eq(refusedCircuit.StatusCode, HttpStatusCode.Forbidden);
                }
                // A flood of requests without the cookie writes at most ten entries a minute.
                for (int i = 0; i < 40; i++)
                {
                    (await anon.GetAsync(new Uri("/viewer/" + new string('f', 32) + "?x=" + ticket.Secret, UriKind.Relative)).ConfigureAwait(false)).Dispose();
                }
            }
            IReadOnlyList<AuditEntry> entries = AuditLog.ReadEntries(TestHost.AuditFile(data));
            T.Eq(entries.Count(e => e.Event == "session.launch" && e.Outcome == AuditOutcome.Success), 1);
            T.Eq(TestHost.Alerts(data, "launch-reused").Count, 1);
            T.Eq(TestHost.Alerts(data, "launch-invalid").Count, 1);
            T.Eq(TestHost.Alerts(data, "launch-expired").Count, 1);
            T.Eq(TestHost.Alerts(data, "host-refused").Count, 1);
            IReadOnlyList<AuditEntry> origins = TestHost.Alerts(data, "origin-refused");
            T.Eq(origins.Count, 3);
            T.Check(origins.Any(e => e.Data["path"] == "/ws/view") && origins.Any(e => e.Data["path"] == "/_blazor"));
            T.Eq(TestHost.Alerts(data, "cookie-refused").Count, 10);
            // Alerts hold the path only: no query (it could be a secret), no session reference.
            string text = await File.ReadAllTextAsync(TestHost.AuditFile(data)).ConfigureAwait(false);
            T.Check(!text.Contains(new string('f', 32), StringComparison.Ordinal) && !text.Contains("?x=", StringComparison.Ordinal));
            T.Check(entries.Where(e => e.Event == AuditLog.SecurityAlert).All(e => e.Outcome == AuditOutcome.Failure));

            // The limiter: ten of a kind per window, then counted; the count is reported once in the next window.
            var limiter = new AlertLimiter(10, TimeSpan.FromMinutes(1));
            DateTime t0 = new(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
            int written = 0;
            for (int i = 0; i < 25; i++)
            {
                (bool write, int suppressed) = limiter.Admit("k", t0.AddSeconds(i));
                written += write ? 1 : 0;
                T.Eq(suppressed, 0);
            }
            T.Eq(written, 10);
            T.Check(limiter.Admit("other", t0).Write);
            T.Eq(limiter.Admit("k", t0.AddMinutes(2)), (true, 15));
            T.Eq(limiter.Admit("k", t0.AddMinutes(2)), (true, 0));
        });

        T.Case("cookie: host-only on a random *.localhost name, never sent to plain 127.0.0.1; the 127.0.0.1 fallback is explicit", async () =>
        {
            RunningViewer v = await TestHost.StartAsync(T.NewTempDir("hostname")).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                T.Check(!v.Security.IpHost);
                T.Eq(v.Security.CookieName, "siaara_session");
                // Bound to 127.0.0.1 only; the name is a 128-bit random label under .localhost.
                T.Check(Regex.IsMatch(v.Security.HostName, "^[0-9a-f]{32}\\.localhost$", RegexOptions.CultureInvariant));
                using HttpClient authed = TestHost.Client(v, cookie: true);
                T.Eq((await authed.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.OK);
                // A request addressed to 127.0.0.1:port (where a browser would share cookies between ports) is refused
                // even with the cookie, so the app never works under a host whose cookies other programs can see.
                using var plain = new HttpRequestMessage(HttpMethod.Get, "/api/health");
                plain.Headers.Host = "127.0.0.1:" + TestHost.PortText(v);
                T.Eq((await authed.SendAsync(plain).ConfigureAwait(false)).StatusCode, HttpStatusCode.BadRequest);
                using HttpClient anon = TestHost.Client(v, cookie: false);
                using HttpResponseMessage launch = await TestHost.PostLaunchAsync(anon, v.Security.IssueLaunch().Secret).ConfigureAwait(false);
                string setCookie = string.Join(";", launch.Headers.GetValues("Set-Cookie")).ToLowerInvariant();
                T.Check(!setCookie.Contains("domain=", StringComparison.Ordinal) && setCookie.Contains("httponly", StringComparison.Ordinal)
                        && setCookie.Contains("samesite=strict", StringComparison.Ordinal) && setCookie.Contains("path=/", StringComparison.Ordinal));
            }
            // Two runs never share a host name.
            using (var a = new LaunchSecurity())
            using (var b = new LaunchSecurity())
            {
                T.Check(a.HostName != b.HostName);
            }
            // The documented fallback.
            RunningViewer ip = await TestHost.StartAsync(T.NewTempDir("iphost"), ipHost: true).ConfigureAwait(false);
            await using (ip.ConfigureAwait(false))
            {
                T.Check(ip.Security.IpHost);
                T.Eq(ip.Security.HostName, "127.0.0.1");
                T.Eq(ip.Security.CookieName, "siaara_session_" + TestHost.PortText(ip));
                using HttpClient authed = TestHost.Client(ip, cookie: true);
                T.Eq((await authed.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.OK);
            }
            // When the fallback is taken: only on request, or when an unknown default browser would have to ask a
            // system resolver that does not know *.localhost names.
            int asked = 0;
            bool Resolves()
            {
                asked++;
                return false;
            }
            T.Check(LocalhostNames.UseIpFallback("ip", true, true, Resolves));
            T.Check(!LocalhostNames.UseIpFallback("name", true, false, Resolves));
            T.Check(!LocalhostNames.UseIpFallback("auto", opensBrowser: false, appBrowserFound: false, Resolves));
            T.Check(!LocalhostNames.UseIpFallback("auto", opensBrowser: true, appBrowserFound: true, Resolves));
            T.Eq(asked, 0);
            T.Check(LocalhostNames.UseIpFallback("auto", opensBrowser: true, appBrowserFound: false, Resolves));
            T.Check(!LocalhostNames.UseIpFallback("auto", opensBrowser: true, appBrowserFound: false, () => true));
            T.Check(LocalhostNames.SystemResolves("localhost"));
            T.Note("system resolver knows *.localhost names: " + LocalhostNames.SystemResolves("siaara-probe.localhost"));
        });

        T.Case("csp: script-src 'self' only (no inline script, no wasm eval), object-src 'none', form-action 'self'; the pages have no inline script", async () =>
        {
            RunningViewer v = await TestHost.StartAsync(T.NewTempDir("csp")).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                using HttpClient authed = TestHost.Client(v, cookie: true);
                using HttpResponseMessage r = await authed.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false);
                // Exactly one policy (Blazor's own frame-ancestors header is switched off).
                T.Eq(r.Headers.GetValues("Content-Security-Policy").Count(), 1);
                string csp = string.Join(";", r.Headers.GetValues("Content-Security-Policy"));
                string[] directives = [.. csp.Split(';', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries)];
                T.Check(directives.Contains("script-src 'self'"));
                T.Check(directives.Contains("object-src 'none'"));
                T.Check(directives.Contains("form-action 'self'"));
                T.Check(directives.Contains("base-uri 'self'"));
                T.Check(directives.Contains("frame-ancestors 'none'"));
                T.Check(directives.Contains("default-src 'self'"));
                T.Check(!csp.Contains("wasm-unsafe-eval", StringComparison.Ordinal) && !csp.Contains("unsafe-eval", StringComparison.Ordinal));
                T.Check(!directives.Single(d => d.StartsWith("script-src", StringComparison.Ordinal)).Contains("unsafe-inline", StringComparison.Ordinal));
                T.Check(directives.Contains("connect-src 'self' ws://" + v.Security.Authority));
                foreach (string path in new[] { "/", "/viewer/" + v.Runtime.Ids.For("1.2.3") })
                {
                    string html = await authed.GetStringAsync(new Uri(path, UriKind.Relative)).ConfigureAwait(false);
                    T.Check(!InlineScript().IsMatch(html));
                    T.Check(!InlineHandler().IsMatch(html));
                }
                // The refusal and launch pages carry the policy too.
                using HttpClient anon = TestHost.Client(v, cookie: false);
                using HttpResponseMessage refused = await anon.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false);
                T.Check(refused.Headers.Contains("Content-Security-Policy"));
                using HttpResponseMessage launch = await TestHost.PostLaunchAsync(anon, v.Security.IssueLaunch().Secret).ConfigureAwait(false);
                T.Check(!InlineScript().IsMatch(await launch.Content.ReadAsStringAsync().ConfigureAwait(false)));
            }
        });

        T.Case("files: audit, anchor, key, log, settings, annotation, instance and launch files are owner-only; a data folder others can reach gives a warning", async () =>
        {
            T.Eq(OwnerOnly.DescribeMode(Mode700), null);
            T.Check(OwnerOnly.DescribeMode(Mode700 | UnixFileMode.GroupRead)!.Contains("group", StringComparison.Ordinal));
            T.Check(OwnerOnly.DescribeMode(Mode700 | UnixFileMode.OtherExecute)!.Contains("every user", StringComparison.Ordinal));
            string root = T.NewTempDir("modes");
            string data = Path.Combine(root, "private");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                T.Check(v.Runtime.DataFolderWarning is null);
                v.Runtime.UpdateSettings(p => p.Theme = "light");
                v.Runtime.Annotations.Save(new AnnotationDocument { SopInstanceUid = "1.2.3", Frame = 0 });
                v.Runtime.Annotations.SaveCalibration("1.2.3", 0.5);
                using InstanceLock? instance = InstanceLock.TryAcquire(Path.Combine(data, "instance.lock"), Path.Combine(data, "instance.url"));
                T.Require(instance is not null);
                instance!.KeepFresh(v.Security);
                using var pages = new LaunchPages(Path.Combine(data, "launch"));
                LaunchTicket t = v.Security.IssueLaunch();
                pages.Write(t.Endpoint, t.Secret);
                string[] files = [.. Directory.GetFiles(data, "*", SearchOption.AllDirectories).Where(f => !f.StartsWith(Path.Combine(data, "store"), StringComparison.Ordinal))];
                string[] names = [.. files.Select(Path.GetFileName).Cast<string>()];
                foreach (string expected in ExpectedFiles)
                {
                    T.Check(names.Contains(expected), expected);
                }
                T.Check(names.Any(n => n.EndsWith(".log", StringComparison.Ordinal)) && names.Any(n => n.StartsWith("launch-", StringComparison.Ordinal)));
                foreach (string f in files)
                {
                    T.Check(OwnerOnly.WiderAccess(f) is null, f);
                    if (!OperatingSystem.IsWindows())
                    {
                        T.Eq(File.GetUnixFileMode(f), Mode600);
                    }
                }
                foreach (string d in new[] { data, Path.Combine(data, "audit"), Path.Combine(data, "logs"), Path.Combine(data, "annotations"), Path.Combine(data, "launch") }
                             .Concat(Directory.GetDirectories(Path.Combine(data, "annotations"), "*", SearchOption.AllDirectories)))
                {
                    T.Check(OwnerOnly.WiderAccess(d) is null, d);
                    if (!OperatingSystem.IsWindows())
                    {
                        T.Eq(File.GetUnixFileMode(d), Mode700);
                    }
                }
            }
            if (OperatingSystem.IsWindows())
            {
                T.Note("group / world mode bits do not exist on Windows; the ACL form of the warning is checked on a real profile only");
                return;
            }
            // A data folder that the group or everybody can enter: banner text, app log line and audit alert; a file
            // written with a wide mode by an earlier version is narrowed when it is opened again.
            string open = Path.Combine(root, "open");
            Directory.CreateDirectory(open);
            File.SetUnixFileMode(open, Mode700 | UnixFileMode.GroupRead | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
            Directory.CreateDirectory(Path.Combine(open, "logs"));
            string oldLog = Path.Combine(open, "logs", "siaara-viewer-" + DateTime.UtcNow.ToString("yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture) + ".log");
            await File.WriteAllTextAsync(oldLog, "").ConfigureAwait(false);
            File.SetUnixFileMode(oldLog, Mode600 | UnixFileMode.GroupRead | UnixFileMode.OtherRead);
            RunningViewer w = await TestHost.StartAsync(open).ConfigureAwait(false);
            await using (w.ConfigureAwait(false))
            {
                T.Check(w.Runtime.DataFolderWarning is string warning && warning.Contains("not private", StringComparison.Ordinal));
                T.Eq(File.GetUnixFileMode(oldLog), Mode600);
                using HttpClient authed = TestHost.Client(w, cookie: true);
                string page = await authed.GetStringAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false);
                T.Check(page.Contains("class=\"banner warning\"", StringComparison.Ordinal) && page.Contains("not private", StringComparison.Ordinal));
            }
            T.Eq(TestHost.Alerts(open, "data-folder-not-private").Count, 1);
            T.Check(Core.Logging.AppLog.RecentLines.Any(l => l.Contains("data folder can be reached by other users", StringComparison.Ordinal)));
        });

        T.Case("annotations: the folder is the SHA-256 of the SOP Instance UID, so '..' cannot leave the store; files of the earlier layout are still read", () =>
        {
            string root = Path.Combine(T.NewTempDir("annpath"), "annotations");
            var store = new AnnotationStore(root, "tester");
            string userFolder = Path.Combine(root, "tester");
            foreach (string evil in EvilUids)
            {
                var doc = new AnnotationDocument { SopInstanceUid = evil, Frame = 3 };
                doc.Annotations.Add(new Annotation { Id = "x" + evil.Length, Kind = AnnotationKind.Length });
                store.Save(doc);
                store.SaveCalibration(evil, 1.5);
                string key = AnnotationStore.FolderKey(evil);
                T.Eq(key.Length, 64);
                T.Check(File.Exists(Path.Combine(userFolder, key, "3.json")), evil);
                T.Check(File.Exists(Path.Combine(userFolder, key, "calibration.json")), evil);
                T.Eq(store.Load(evil, 3).Annotations.Count, 1);
                T.Eq(store.Load(evil, 3).CalibrationMmPerPixel, 1.5);
            }
            // Nothing was written outside the user's folder, and every folder in it is a 64-digit hash.
            T.Eq(Directory.GetFiles(root, "*", SearchOption.TopDirectoryOnly).Length, 0);
            T.Check(Directory.GetDirectories(root).SequenceEqual([userFolder]));
            T.Eq(Directory.GetFiles(userFolder, "*", SearchOption.TopDirectoryOnly).Length, 0);
            T.Check(Directory.GetDirectories(userFolder).All(d => Path.GetFileName(d).Length == 64 && Path.GetFileName(d).All(char.IsAsciiHexDigit)));
            T.Check(AnnotationStore.FolderKey("1.2.3") != AnnotationStore.FolderKey("1.2.4"));
            // A dot-only user name cannot climb either.
            T.Eq(new AnnotationStore(root, "..").User, "user");
            // The earlier layout (folder = the UID) is read; the next save writes the new place.
            var elsewhere = new AnnotationStore(Path.Combine(T.NewTempDir("annold"), "annotations"), "tester");
            var written = new AnnotationDocument { SopInstanceUid = "1.2.840.1", Frame = 0 };
            written.Annotations.Add(new Annotation { Id = "old", Kind = AnnotationKind.Length });
            elsewhere.Save(written);
            elsewhere.SaveCalibration("1.2.840.1", 0.25);
            string legacyFolder = Path.Combine(userFolder, "1.2.840.1");
            Directory.CreateDirectory(legacyFolder);
            File.Copy(elsewhere.PathOf("1.2.840.1", 0), Path.Combine(legacyFolder, "0.json"));
            File.Copy(elsewhere.CalibrationPath("1.2.840.1"), Path.Combine(legacyFolder, "calibration.json"));
            T.Eq(store.LegacyFolder("1.2.840.1"), legacyFolder);
            AnnotationDocument old = store.Load("1.2.840.1", 0);
            T.Eq(old.Annotations.Count, 1);
            T.Eq(old.CalibrationMmPerPixel, 0.25);
            store.Save(old);
            T.Check(File.Exists(Path.Combine(userFolder, AnnotationStore.FolderKey("1.2.840.1"), "0.json")));
            // A file sitting where the earlier code would have looked for the UID ".." (one folder up) is never read.
            File.Copy(elsewhere.PathOf("1.2.840.1", 0), Path.Combine(root, "7.json"));
            T.Eq(store.Load("..", 7).Annotations.Count, 0);
            T.Check(store.LegacyFolder("..") is null && store.LegacyFolder("") is null && store.LegacyFolder("1.2") is not null);
        });

        T.Case("urls: studies and series are named by per-run ids, never by UID; at most 8 sessions; connection limits are set", async () =>
        {
            var ids = new OpaqueIds();
            string id = ids.For("1.2.840.99.1");
            T.Eq(id.Length, 32);
            T.Check(id.All(char.IsAsciiHexDigit) && !id.Contains("840", StringComparison.Ordinal));
            T.Eq(ids.For("1.2.840.99.1"), id);
            T.Check(ids.For("1.2.840.99.2") != id);
            T.Eq(ids.Resolve(id), "1.2.840.99.1");
            T.Check(ids.Resolve("1.2.840.99.1") is null && ids.Resolve(null) is null && ids.Resolve(new string('0', 32)) is null);
            T.Check(new OpaqueIds().Resolve(id) is null);

            string data = T.NewTempDir("urls");
            string dir = Path.Combine(data, "in");
            SamplePatient p = SampleData.Patient("URL^CT", "URL1");
            SampleData.WriteCtSeries(dir, p, 2, 32);
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                await v.Runtime.Import.Start(new Core.Import.ImportRequest(dir)).Completion.ConfigureAwait(false);
                KestrelServerLimits limits = v.App.Services.GetRequiredService<IOptions<KestrelServerOptions>>().Value.Limits;
                T.Eq(limits.MaxConcurrentConnections, (long?)ViewerHost.MaxConnections);
                T.Eq(limits.MaxConcurrentUpgradedConnections, (long?)ViewerHost.MaxUpgradedConnections);
                using HttpClient authed = TestHost.Client(v, cookie: true);
                authed.DefaultRequestHeaders.Add("Origin", v.Security.Origin);
                // The study page and its preview carry no UID.
                string page = await authed.GetStringAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false);
                T.Check(!page.Contains(p.StudyUid, StringComparison.Ordinal));
                // Scripts open a study with the UID in the request body; a UID in the address is not accepted.
                using (HttpResponseMessage inUrl = await authed.PostAsync(new Uri("/api/sessions?study=" + p.StudyUid, UriKind.Relative), null).ConfigureAwait(false))
                {
                    T.Eq(inUrl.StatusCode, HttpStatusCode.BadRequest);
                }
                using (var body = new StringContent("not a uid; drop table", Encoding.ASCII, "text/plain"))
                using (HttpResponseMessage bad = await authed.PostAsync(new Uri("/api/sessions", UriKind.Relative), body).ConfigureAwait(false))
                {
                    T.Eq(bad.StatusCode, HttpStatusCode.BadRequest);
                }
                string studyRef;
                using (var body = new StringContent(p.StudyUid, Encoding.ASCII, "text/plain"))
                using (HttpResponseMessage ok = await authed.PostAsync(new Uri("/api/sessions", UriKind.Relative), body).ConfigureAwait(false))
                {
                    T.Eq(ok.StatusCode, HttpStatusCode.OK);
                    using var doc = System.Text.Json.JsonDocument.Parse(await ok.Content.ReadAsStringAsync().ConfigureAwait(false));
                    studyRef = doc.RootElement.GetProperty("ref").GetString()!;
                    T.Check(v.Runtime.Sessions.Get(doc.RootElement.GetProperty("id").GetString()) is not null);
                }
                T.Eq(studyRef, v.Runtime.Ids.For(p.StudyUid));
                T.Eq((await authed.GetAsync(new Uri("/viewer/" + studyRef, UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.OK);

                // Session limit: a ninth session replaces the one that has been without a window the longest...
                SessionRegistry reg = v.Runtime.Sessions;
                reg.CloseAll();
                var made = new List<ViewerSession>();
                for (int i = 0; i < SessionRegistry.MaxSessions; i++)
                {
                    made.Add(reg.Create(p.StudyUid));
                    await Task.Delay(5).ConfigureAwait(false);
                }
                T.Eq(reg.Count, SessionRegistry.MaxSessions);
                ViewerSession ninth = reg.Create(p.StudyUid);
                T.Eq(reg.Count, SessionRegistry.MaxSessions);
                T.Check(reg.Get(made[0].Id) is null && reg.Get(made[1].Id) is not null && reg.Get(ninth.Id) is not null);
                // ... and when every session has a window, opening another one fails (and is audited).
                foreach (ViewerSession s in made.Skip(1).Append(ninth))
                {
                    s.Attach(new FakeSink(s));
                }
                bool refused = false;
                try
                {
                    await v.Runtime.OpenStudyAsync(p.StudyUid).ConfigureAwait(false);
                }
                catch (SessionLimitException)
                {
                    refused = true;
                }
                T.Check(refused);
                T.Eq(reg.Count, SessionRegistry.MaxSessions);
                using (var body = new StringContent(p.StudyUid, Encoding.ASCII, "text/plain"))
                using (HttpResponseMessage tooMany = await authed.PostAsync(new Uri("/api/sessions", UriKind.Relative), body).ConfigureAwait(false))
                {
                    T.Eq(tooMany.StatusCode, HttpStatusCode.TooManyRequests);
                }
            }
            T.Check(AuditLog.ReadEntries(TestHost.AuditFile(data)).Any(e => e.Event == "study.open" && e.Outcome == AuditOutcome.Failure
                                                                             && e.Reason.Contains("too many", StringComparison.Ordinal)));
        });

        T.Case("socket: inbound messages over 64 KB close it; JSON values of the wrong kind are ignored; the send queue is bounded", async () =>
        {
            T.Eq(FrameProtocol.MaxInboundMessageBytes, 64 * 1024);
            using var env = new TestEnv("sockjson");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("SOCK^CT", "SOCK1"), 2, 32);
            await env.ImportAsync(dir).ConfigureAwait(false);
            (ViewerSession session, FakeSink _) = await env.OpenAsync(Gen.StudyOf(env, "SOCK1"), 64, 64).ConfigureAwait(false);
            using (session)
            {
                // Every member has the wrong JSON kind: the earlier code threw InvalidOperationException out of the receive loop.
                foreach (string json in WrongKinds)
                {
                    InputRouter.Dispatch(session, FrameProtocol.Decode(FrameProtocol.Command(0, json))!.Value);
                }
                // A good command still works afterwards.
                InputRouter.Dispatch(session, FrameProtocol.Decode(FrameProtocol.Command(0, "{\"layout\":\"1x2\"}"))!.Value);
                T.Eq(session.Layout, new LayoutModel(1, 2));
            }

            // The sending side: a browser that never reads fills the queue; then nothing more is queued and the socket is given up.
            using (var life = new CancellationTokenSource())
            using (var stuck = new StuckWebSocket())
            using (var sink = new SocketSink(stuck, life))
            {
                Task sending = sink.RunAsync(life.Token);
                for (int i = 0; i < FrameProtocol.MaxQueuedOutbound * 4; i++)
                {
                    sink.SendState(0, (uint)i, new ViewportStateMessage());
                    byte[] rented = System.Buffers.ArrayPool<byte>.Shared.Rent(64);
                    sink.SendFrame(0, (uint)i, FramePixelFormat.Gray8, 4, 4, rented, FrameProtocol.FrameHeaderBytes + 16);
                    T.Check(sink.Queued <= FrameProtocol.MaxQueuedOutbound);
                }
                T.Check(sink.Overflowed);
                T.Check(life.IsCancellationRequested);
                try
                {
                    await sending.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                }
                T.Check(sending.IsCompleted);
            }

            // Over the real socket: an oversized message closes it with "message too big"; the host goes on serving.
            string data = T.NewTempDir("sockbig");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                ViewerSession s = v.Runtime.Sessions.Create("1.2.3.4");
                (ClientWebSocket? ws, _) = await TestHost.ConnectAsync(v, s.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Require(ws is not null);
                using (ws)
                {
                    await ws!.SendAsync(FrameProtocol.Command(0, "{\"action\":7,\"tool\":{}}"), WebSocketMessageType.Binary, true, CancellationToken.None).ConfigureAwait(false);
                    await ws.SendAsync(FrameProtocol.Command(0, "{\"layout\":\"2x2\"}"), WebSocketMessageType.Binary, true, CancellationToken.None).ConfigureAwait(false);
                    for (int i = 0; i < 100 && s.Layout != new LayoutModel(2, 2); i++)
                    {
                        await Task.Delay(20).ConfigureAwait(false);
                    }
                    T.Eq(s.Layout, new LayoutModel(2, 2));
                    var big = new byte[FrameProtocol.MaxInboundMessageBytes + 1];
                    big[0] = 5;
                    await ws.SendAsync(big, WebSocketMessageType.Binary, true, CancellationToken.None).ConfigureAwait(false);
                    using var cts = new CancellationTokenSource(10000);
                    var buf = new byte[1 << 16];
                    WebSocketReceiveResult r;
                    do
                    {
                        r = await ws.ReceiveAsync(buf, cts.Token).ConfigureAwait(false);
                    }
                    while (r.MessageType != WebSocketMessageType.Close);
                    T.Eq(r.CloseStatus, WebSocketCloseStatus.MessageTooBig);
                }
                // A message of exactly the limit is accepted.
                (ClientWebSocket? ws2, _) = await TestHost.ConnectAsync(v, s.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Require(ws2 is not null);
                using (ws2)
                {
                    byte[] atLimit = FrameProtocol.Command(0, "{\"layout\":\"1x1\"," + "\"pad\":\"" + new string('x', FrameProtocol.MaxInboundMessageBytes - 30) + "\"}");
                    T.Check(atLimit.Length <= FrameProtocol.MaxInboundMessageBytes);
                    await ws2!.SendAsync(atLimit, WebSocketMessageType.Binary, true, CancellationToken.None).ConfigureAwait(false);
                    for (int i = 0; i < 100 && s.Layout != new LayoutModel(1, 1); i++)
                    {
                        await Task.Delay(20).ConfigureAwait(false);
                    }
                    T.Eq(s.Layout, new LayoutModel(1, 1));
                }
            }
        });

        T.Case("sign out: the cookie stops working at once, viewer sessions and their sockets close, the next launch gets a new cookie", async () =>
        {
            string data = T.NewTempDir("signout");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                string oldCookie = v.Security.CookieValue;
                using HttpClient authed = TestHost.Client(v, cookie: true);
                T.Eq((await authed.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.OK);
                ViewerSession s = v.Runtime.Sessions.Create("1.2.3.4");
                (ClientWebSocket? ws, _) = await TestHost.ConnectAsync(v, s.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Require(ws is not null);
                int signedOut = 0;
                v.Security.SignedOut += (_, _) => signedOut++;
                using (ws)
                {
                    // Sign-out is a POST: it needs the cookie and the app's Origin like every other change.
                    using (var noOrigin = new HttpRequestMessage(HttpMethod.Post, "/signout"))
                    {
                        T.Eq((await authed.SendAsync(noOrigin).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                    }
                    T.Eq(v.Security.CookieValue, oldCookie);
                    using (var post = new HttpRequestMessage(HttpMethod.Post, "/signout"))
                    {
                        post.Headers.Add("Origin", v.Security.Origin);
                        using HttpResponseMessage done = await authed.SendAsync(post).ConfigureAwait(false);
                        T.Eq(done.StatusCode, HttpStatusCode.Redirect);
                        T.Eq(done.Headers.Location?.OriginalString, LaunchSecurity.SignedOutPath);
                        // The signed-out page needs no cookie and says what to do.
                        using HttpClient nobody = TestHost.Client(v, cookie: false);
                        string page = await nobody.GetStringAsync(new Uri(LaunchSecurity.SignedOutPath, UriKind.Relative)).ConfigureAwait(false);
                        T.Check(page.Contains("Signed out", StringComparison.Ordinal) && page.Contains("Start SIAARA Viewer again", StringComparison.Ordinal));
                    }
                    T.Eq(signedOut, 1);
                    T.Check(v.Security.CookieValue != oldCookie);
                    T.Eq(v.Runtime.Sessions.Count, 0);
                    // The old cookie is refused at once, on HTTP and on a new socket; the open socket is closed.
                    T.Eq((await authed.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                    bool closed = false;
                    try
                    {
                        using var cts = new CancellationTokenSource(10000);
                        var buf = new byte[1 << 16];
                        while (!closed)
                        {
                            WebSocketReceiveResult r = await ws!.ReceiveAsync(buf, cts.Token).ConfigureAwait(false);
                            closed = r.MessageType == WebSocketMessageType.Close;
                        }
                    }
                    catch (WebSocketException)
                    {
                        closed = true;
                    }
                    T.Check(closed);
                }
                ViewerSession s2 = v.Runtime.Sessions.Create("1.2.3.4");
                (ClientWebSocket? stale, HttpStatusCode code) = await TestHost.ConnectAsync(v, s2.Id, cookie: true, v.Security.Origin, cookieValue: oldCookie).ConfigureAwait(false);
                T.Check(stale is null);
                T.Eq(code, HttpStatusCode.Forbidden);
                // Starting the viewer again (a new launch secret) gives a working session with the new cookie.
                using HttpClient anon = TestHost.Client(v, cookie: false);
                using HttpResponseMessage launch = await TestHost.PostLaunchAsync(anon, v.Security.IssueLaunch().Secret).ConfigureAwait(false);
                T.Eq(launch.StatusCode, HttpStatusCode.OK);
                string setCookie = string.Join(";", launch.Headers.GetValues("Set-Cookie"));
                T.Check(setCookie.Contains(v.Security.CookieValue, StringComparison.Ordinal) && !setCookie.Contains(oldCookie, StringComparison.Ordinal));
                using HttpClient again = TestHost.Client(v, cookie: true);
                T.Eq((await again.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.OK);
            }
            T.Eq(AuditLog.ReadEntries(TestHost.AuditFile(data)).Count(e => e.Event == "session.signout"), 1);
        });

        T.Case("audit: folders are written as a keyed reference and a depth, never as a path", () =>
        {
            string dir = T.NewTempDir("pathref");
            using var a = new AuditLog(Path.Combine(dir, "audit.jsonl"), AuditKeys.KeyFile);
            string folder = Path.Combine(dir, "DOE^JOHN 1961-04-12", "CT chest");
            string r = a.PathReference(folder);
            T.Check(PathRef().IsMatch(r), r);
            T.Check(!r.Contains("DOE", StringComparison.Ordinal) && !r.Contains("chest", StringComparison.Ordinal));
            T.Eq(a.PathReference(folder), r);
            T.Eq(a.PathReference(folder + Path.DirectorySeparatorChar + "."), r);
            T.Check(a.PathReference(Path.Combine(dir, "DOE^JANE 1961-04-12", "CT chest")) != r);
            T.Check(r.EndsWith("/d" + (dir.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries).Length + 2).ToString(System.Globalization.CultureInfo.InvariantCulture),
                StringComparison.Ordinal));
            T.Eq(a.PathReference(null), "");
            // Another installation (another key) gives another reference for the same folder.
            using var b = new AuditLog(Path.Combine(T.NewTempDir("pathref2"), "audit.jsonl"), AuditKeys.KeyFile);
            T.Check(b.PathReference(folder) != r);
        });
    }
}
