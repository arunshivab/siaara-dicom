// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text.Json;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Viewing;
using Siaara.Viewer.Host;
using Siaara.Viewer.Web.Security;
using Siaara.Viewer.Web.Streaming;

namespace Siaara.Viewer.Tests;

internal static class HostTests
{
    private static readonly string[] RequiredEvents = ["app.start", "import", "study.open", "study.delete", "app.stop"];

    private static Task<RunningViewer> StartAsync(string name) =>
        TestHost.StartAsync(T.NewTempDir(name));

    private static HttpClient Client(RunningViewer v, bool cookie) => TestHost.Client(v, cookie);

    private static Task<(ClientWebSocket? Socket, HttpStatusCode Status)> ConnectAsync(RunningViewer v, string session, bool cookie, string? origin) =>
        TestHost.ConnectAsync(v, session, cookie, origin);

    private static async Task<byte[]> ReceiveAsync(ClientWebSocket ws, int timeoutMs = 10000)
    {
        using var cts = new CancellationTokenSource(timeoutMs);
        var buf = new byte[1 << 20];
        using var ms = new MemoryStream();
        while (true)
        {
            WebSocketReceiveResult r = await ws.ReceiveAsync(buf, cts.Token).ConfigureAwait(false);
            await ms.WriteAsync(buf.AsMemory(0, r.Count), cts.Token).ConfigureAwait(false);
            if (r.EndOfMessage)
            {
                return ms.ToArray();
            }
        }
    }

    private static async Task<byte[]> ReceiveFrameAsync(ClientWebSocket ws, List<string>? states = null)
    {
        while (true)
        {
            byte[] m = await ReceiveAsync(ws).ConfigureAwait(false);
            if (m[0] == 1)
            {
                return m;
            }
            if (m[0] == 2)
            {
                states?.Add(FrameProtocol.ReadState(m).Json);
            }
        }
    }

    private static Task SendAsync(ClientWebSocket ws, byte[] msg) => ws.SendAsync(msg, WebSocketMessageType.Binary, true, CancellationToken.None);

    public static void Register()
    {
        T.Case("security: loopback only, launch secret to cookie, cookie and Origin checked on HTTP and WebSocket", async () =>
        {
            RunningViewer v = await StartAsync("security").ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                ICollection<string> addresses = v.App.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses;
                T.Check(addresses.Count == 1 && addresses.All(a => new Uri(a).Host == "127.0.0.1"));
                IPAddress? lan = NetworkInterface.GetAllNetworkInterfaces()
                    .Where(n => n.OperationalStatus == OperationalStatus.Up && n.NetworkInterfaceType != NetworkInterfaceType.Loopback)
                    .SelectMany(n => n.GetIPProperties().UnicastAddresses).Select(a => a.Address)
                    .FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(a));
                if (lan is not null)
                {
                    using var tcp = new TcpClient();
                    bool refused = false;
                    try
                    {
                        await tcp.ConnectAsync(lan, v.Port).WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false);
                    }
                    catch (SocketException)
                    {
                        refused = true;
                    }
                    catch (TimeoutException)
                    {
                        refused = true;
                    }
                    T.Check(refused);
                    T.Note("non-loopback address " + lan + " refused the port");
                }
                else
                {
                    T.Note("no non-loopback IPv4 interface on this machine to try");
                }

                using HttpClient anon = Client(v, cookie: false);
                T.Eq((await anon.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                T.Eq((await anon.GetAsync(new Uri("/launch?k=00", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                // The app is served under a random *.localhost name, bound to 127.0.0.1.
                T.Check(v.Security.HostName.Length == 32 + ".localhost".Length && v.Security.HostName.EndsWith(".localhost", StringComparison.Ordinal));
                T.Eq(v.Security.Origin, "http://" + v.Security.HostName + ":" + TestHost.PortText(v));
                Web.Security.LaunchTicket ticket = v.Security.IssueLaunch();
                string secret = ticket.Secret;
                T.Eq((await anon.GetAsync(new Uri("/launch?k=" + new string('0', 64), UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                // The launch page posts the secret (not in any address); the answer is a page of ours, not a redirect.
                using HttpResponseMessage launch = await TestHost.PostLaunchAsync(anon, secret).ConfigureAwait(false);
                T.Eq(launch.StatusCode, HttpStatusCode.OK);
                string setCookie = string.Join(";", launch.Headers.GetValues("Set-Cookie")).ToLowerInvariant();
                T.Check(setCookie.Contains("httponly", StringComparison.Ordinal) && setCookie.Contains("samesite=strict", StringComparison.Ordinal));
                T.Check(setCookie.Contains(v.Security.CookieValue, StringComparison.Ordinal));
                T.Check(!setCookie.Contains(secret, StringComparison.Ordinal));
                // Host-only (no Domain), path /, no lifetime (a session cookie).
                T.Check(!setCookie.Contains("domain=", StringComparison.Ordinal));
                T.Check(setCookie.Contains("path=/", StringComparison.Ordinal));
                T.Check(!setCookie.Contains("expires=", StringComparison.Ordinal) && !setCookie.Contains("max-age=", StringComparison.Ordinal));
                T.Check(setCookie.StartsWith(v.Security.CookieName + "=", StringComparison.Ordinal));
                string launched = await launch.Content.ReadAsStringAsync().ConfigureAwait(false);
                T.Check(launched.Contains("http-equiv=\"refresh\"", StringComparison.Ordinal) && !launched.Contains(secret, StringComparison.Ordinal));
                // Single use: the same secret is refused the second time, by POST and in an address.
                using (HttpResponseMessage again = await TestHost.PostLaunchAsync(anon, secret).ConfigureAwait(false))
                {
                    T.Eq(again.StatusCode, HttpStatusCode.Forbidden);
                    T.Check(!again.Headers.Contains("Set-Cookie"));
                    T.Check((await again.Content.ReadAsStringAsync().ConfigureAwait(false)).Contains("already used", StringComparison.Ordinal));
                }
                T.Eq((await anon.GetAsync(ticket.Url).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                // The printed address (--print-url) works once as a GET.
                Web.Security.LaunchTicket printed = v.Security.IssueLaunch();
                using (HttpResponseMessage byUrl = await anon.GetAsync(printed.Url).ConfigureAwait(false))
                {
                    T.Eq(byUrl.StatusCode, HttpStatusCode.OK);
                    T.Check(byUrl.Headers.Contains("Set-Cookie"));
                }
                // An unused secret expires; a revoked one is refused.
                Web.Security.LaunchTicket shortLived = v.Security.IssueLaunch(TimeSpan.FromMilliseconds(50));
                await Task.Delay(150).ConfigureAwait(false);
                using (HttpResponseMessage late = await TestHost.PostLaunchAsync(anon, shortLived.Secret).ConfigureAwait(false))
                {
                    T.Eq(late.StatusCode, HttpStatusCode.Forbidden);
                    T.Check((await late.Content.ReadAsStringAsync().ConfigureAwait(false)).Contains("expired", StringComparison.Ordinal));
                }
                Web.Security.LaunchTicket revoked = v.Security.IssueLaunch();
                v.Security.Revoke(revoked);
                T.Eq((await TestHost.PostLaunchAsync(anon, revoked.Secret).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                T.Eq(v.Security.OutstandingSecrets, 0);

                using HttpClient authed = Client(v, cookie: true);
                T.Eq((await authed.GetAsync(new Uri("/api/health", UriKind.Relative)).ConfigureAwait(false)).StatusCode, HttpStatusCode.OK);
                using (var rebinding = new HttpRequestMessage(HttpMethod.Get, "/api/health"))
                {
                    rebinding.Headers.Host = "evil.example:" + v.Port.ToString(System.Globalization.CultureInfo.InvariantCulture);
                    T.Eq((await authed.SendAsync(rebinding).ConfigureAwait(false)).StatusCode, HttpStatusCode.BadRequest);
                }
                // Only the exact host name and port: plain 127.0.0.1 (where cookies are shared by every port), "localhost",
                // another *.localhost name and another port are all refused.
                foreach (string host in new[] { v.Security.HostName + ":" + (v.Port == 65535 ? 1 : v.Port + 1).ToString(System.Globalization.CultureInfo.InvariantCulture),
                                                "127.0.0.1:" + TestHost.PortText(v), "localhost:" + TestHost.PortText(v), "127.0.0.1", v.Security.HostName,
                                                new string('a', 32) + ".localhost:" + TestHost.PortText(v) })
                {
                    using var other = new HttpRequestMessage(HttpMethod.Get, "/api/health");
                    other.Headers.Host = host;
                    T.Eq((await authed.SendAsync(other).ConfigureAwait(false)).StatusCode, HttpStatusCode.BadRequest);
                }
                using (var post = new HttpRequestMessage(HttpMethod.Post, "/api/health"))
                {
                    post.Headers.Add("Origin", "http://evil.example");
                    T.Eq((await authed.SendAsync(post).ConfigureAwait(false)).StatusCode, HttpStatusCode.Forbidden);
                }

                ViewerSession s = v.Runtime.Sessions.Create("1.2.3.4");
                (ClientWebSocket? w1, HttpStatusCode c1) = await ConnectAsync(v, s.Id, cookie: false, v.Security.Origin).ConfigureAwait(false);
                T.Check(w1 is null);
                T.Eq(c1, HttpStatusCode.Forbidden);
                (ClientWebSocket? w2, HttpStatusCode c2) = await ConnectAsync(v, s.Id, cookie: true, "http://evil.example").ConfigureAwait(false);
                T.Check(w2 is null);
                T.Eq(c2, HttpStatusCode.Forbidden);
                (ClientWebSocket? w3, HttpStatusCode c3) = await ConnectAsync(v, s.Id, cookie: true, null).ConfigureAwait(false);
                T.Check(w3 is null);
                T.Eq(c3, HttpStatusCode.Forbidden);
                (ClientWebSocket? w4, HttpStatusCode c4) = await ConnectAsync(v, "nosuchsession", cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Check(w4 is null);
                T.Eq(c4, HttpStatusCode.NotFound);
                (ClientWebSocket? w5, HttpStatusCode c5) = await ConnectAsync(v, s.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Check(w5 is not null);
                T.Eq(c5, HttpStatusCode.SwitchingProtocols);
                if (w5 is not null)
                {
                    await w5.CloseAsync(WebSocketCloseStatus.NormalClosure, "", CancellationToken.None).ConfigureAwait(false);
                    w5.Dispose();
                }
            }
        });

        T.Case("e2e: real Kestrel host streams frames over /ws/view with the right size and format; audit and logs", async () =>
        {
            string data = T.NewTempDir("e2e");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            string studyUid, patientId = "E2E0001", patientName = "E2EPATIENT^SECRETNAME";
            await using (v.ConfigureAwait(false))
            {
                string dir = Path.Combine(data, "incoming");
                SamplePatient p = SampleData.Patient("X", patientId) with { PatientName = patientName };
                SampleData.WriteCtSeries(dir, p, 12, 128);
                var job = v.Runtime.Import.Start(new Core.Import.ImportRequest(dir));
                await job.Completion.ConfigureAwait(false);
                T.Eq(job.Progress.Added, 12L);
                studyUid = p.StudyUid;

                using HttpClient http = Client(v, cookie: true);
                string page = await http.GetStringAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false);
                T.Check(page.Contains("Patient name", StringComparison.Ordinal));
                T.Check(page.Contains("_framework/blazor.web.js", StringComparison.Ordinal));
                T.Check(page.Contains("siaara-view.js", StringComparison.Ordinal));
                T.Check(page.Contains("SAMPLE", StringComparison.Ordinal) || page.Contains("E2EPATIENT", StringComparison.Ordinal));
                // No UID in any address (R-145): the page and the thumbnails use per-run ids.
                T.Check(!page.Contains(studyUid, StringComparison.Ordinal));
                using HttpResponseMessage viewer = await http.GetAsync(new Uri("/viewer/" + v.Runtime.Ids.For(studyUid), UriKind.Relative)).ConfigureAwait(false);
                T.Eq(viewer.StatusCode, HttpStatusCode.OK);
                string seriesUid = (await v.Runtime.Store.GetSeriesAsync(studyUid).ConfigureAwait(false))[0].SeriesInstanceUid;
                using HttpResponseMessage byUid = await http.GetAsync(new Uri("/api/thumb?series=" + Uri.EscapeDataString(seriesUid), UriKind.Relative)).ConfigureAwait(false);
                T.Eq(byUid.StatusCode, HttpStatusCode.NotFound);
                using HttpResponseMessage thumb = await http.GetAsync(new Uri("/api/thumb?series=" + v.Runtime.Ids.For(seriesUid), UriKind.Relative)).ConfigureAwait(false);
                T.Eq(thumb.Content.Headers.ContentType?.MediaType, "image/png");

                var clock = Stopwatch.StartNew();
                ViewerSession session = await v.Runtime.OpenStudyAsync(studyUid).ConfigureAwait(false);
                (ClientWebSocket? ws, HttpStatusCode code) = await ConnectAsync(v, session.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Require(ws is not null);
                using (ws)
                {
                    await SendAsync(ws!, FrameProtocol.Input(new InputEvent(0, 1, InputKind.Resize, 0, 0, 0, 0, 0, InputModifiers.None, 300, 200))).ConfigureAwait(false);
                    var states = new List<string>();
                    byte[] f1 = await ReceiveFrameAsync(ws!, states).ConfigureAwait(false);
                    T.Perf($"e2e first frame over the socket {clock.Elapsed.TotalMilliseconds:F0} ms after opening the study (12 x 128x128 CT)");
                    var h1 = FrameProtocol.ReadFrameHeader(f1);
                    T.Eq(h1.Format, FramePixelFormat.Gray8);
                    T.Eq(h1.Viewport, 0);
                    T.Eq(h1.Width, 300);
                    T.Eq(h1.Height, 200);
                    T.Eq(f1.Length, FrameProtocol.FrameHeaderBytes + 300 * 200);
                    byte[] s1 = await ReceiveAsync(ws!).ConfigureAwait(false);
                    T.Eq(s1[0], (byte)2);
                    (int svp, uint sseq, string json) = FrameProtocol.ReadState(s1);
                    T.Eq(sseq, h1.Seq);
                    using (var doc = JsonDocument.Parse(json))
                    {
                        T.Check(doc.RootElement.TryGetProperty("overlayText", out JsonElement ot) && ot.GetProperty("topLeft").GetArrayLength() > 0);
                        T.Eq(doc.RootElement.GetProperty("imageCount").GetInt32(), 12);
                    }
                    T.Eq(svp, 0);
                    // Without an ack nothing more is sent, whatever the input.
                    await SendAsync(ws!, FrameProtocol.Input(new InputEvent(0, 2, InputKind.Wheel, 10, 10, 0, 1, 0, InputModifiers.None, 0, 0))).ConfigureAwait(false);
                    using (var quiet = new CancellationTokenSource(400))
                    {
                        bool gotFrame = false;
                        try
                        {
                            while (true)
                            {
                                var buf = new byte[1 << 16];
                                WebSocketReceiveResult r = await ws!.ReceiveAsync(buf, quiet.Token).ConfigureAwait(false);
                                if (buf[0] == 1)
                                {
                                    gotFrame = true;
                                    break;
                                }
                            }
                        }
                        catch (OperationCanceledException)
                        {
                        }
                        T.Check(!gotFrame);
                    }
                }
                // The cancelled receive aborted that socket: reconnect (the session re-sends every viewport).
                (ClientWebSocket? ws2, _) = await ConnectAsync(v, session.Id, cookie: true, v.Security.Origin).ConfigureAwait(false);
                T.Require(ws2 is not null);
                using (ws2)
                {
                    await SendAsync(ws2!, FrameProtocol.Input(new InputEvent(0, 3, InputKind.Resize, 0, 0, 0, 0, 0, InputModifiers.None, 300, 200))).ConfigureAwait(false);
                    byte[] f2 = await ReceiveFrameAsync(ws2!).ConfigureAwait(false);
                    var h2 = FrameProtocol.ReadFrameHeader(f2);
                    await SendAsync(ws2!, FrameProtocol.Ack(0, h2.Seq)).ConfigureAwait(false);
                    // Right-drag = window / level: a new frame with different pixels.
                    await SendAsync(ws2!, FrameProtocol.Input(new InputEvent(0, 4, InputKind.Down, 150, 100, 2, 0, 0, InputModifiers.None, 0, 0))).ConfigureAwait(false);
                    await SendAsync(ws2!, FrameProtocol.Input(new InputEvent(0, 5, InputKind.Move, 220, 160, 2, 0, 0, InputModifiers.None, 0, 0))).ConfigureAwait(false);
                    await SendAsync(ws2!, FrameProtocol.Input(new InputEvent(0, 6, InputKind.Up, 220, 160, 0, 0, 0, InputModifiers.None, 0, 0))).ConfigureAwait(false);
                    byte[] f3 = await ReceiveFrameAsync(ws2!).ConfigureAwait(false);
                    var h3 = FrameProtocol.ReadFrameHeader(f3);
                    T.Check(h3.Seq > h2.Seq);
                    T.Eq((h3.Width, h3.Height), (300, 200));
                    T.Check(!f3.AsSpan(12).SequenceEqual(f2.AsSpan(12)));
                    await SendAsync(ws2!, FrameProtocol.Ack(0, h3.Seq)).ConfigureAwait(false);
                    // A command over the socket: 2x2 layout.
                    await SendAsync(ws2!, FrameProtocol.Command(0, "{\"layout\":\"2x2\"}")).ConfigureAwait(false);
                    await Task.Delay(100).ConfigureAwait(false);
                    T.Eq(session.Layout, new LayoutModel(2, 2));
                    await ws2!.CloseAsync(WebSocketCloseStatus.NormalClosure, "", CancellationToken.None).ConfigureAwait(false);
                }
                StudyRow row = (await v.Runtime.Store.QueryStudiesAsync(new StudyQuery { Text = patientId }).ConfigureAwait(false)).Rows.Single();
                T.Eq(await v.Runtime.DeleteStudyAsync(row).ConfigureAwait(false), 12);
            }
            string audit = Path.Combine(data, "audit", "audit.jsonl");
            // Folders are never written as paths (R-142).
            string auditText = File.ReadAllText(audit);
            T.Check(!auditText.Contains(data, StringComparison.Ordinal) && !auditText.Contains("incoming", StringComparison.Ordinal));
            T.Check(auditText.Contains("\"root\":\"p:", StringComparison.Ordinal) && auditText.Contains("\"store\":\"p:", StringComparison.Ordinal));
            IReadOnlyList<string> events = AuditLog.ReadEvents(audit);
            T.Note("audit events: " + string.Join(", ", events));
            foreach (string e in RequiredEvents)
            {
                T.Check(events.Contains(e));
            }
            T.Check(AuditLog.Verify(audit, AuditKeys.KeyFile).IsIntact);
            T.Check(File.ReadAllText(audit).Contains("\"patientKey\"", StringComparison.Ordinal));
            // No PHI in the app logs: neither the name, the ID nor the study UID.
            string logs = string.Join("\n", AppLog.RecentLines) + "\n" + string.Join("\n",
                Directory.GetFiles(Path.Combine(data, "logs")).Select(f =>
                {
                    using var fs = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                    using var r = new StreamReader(fs);
                    return r.ReadToEnd();
                }));
            T.Check(logs.Length > 0);
            T.Check(!logs.Contains("SECRETNAME", StringComparison.Ordinal));
            T.Check(!logs.Contains("E2EPATIENT", StringComparison.Ordinal));
            T.Check(!logs.Contains(patientId, StringComparison.Ordinal));
            T.Check(!logs.Contains(studyUid, StringComparison.Ordinal));
        });
    }
}
