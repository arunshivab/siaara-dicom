// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Connections.Features;
using Microsoft.AspNetCore.Http;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Platform;

namespace Siaara.Viewer.Web.Security;

/// <summary>A launch secret was used.</summary>
/// <param name="secret">The secret (no longer valid).</param>
public sealed class LaunchUsedEventArgs(string secret) : EventArgs
{
    /// <summary>The secret, as hex.</summary>
    public string Secret { get; } = secret;
}

/// <summary>One single-use way into the app.</summary>
/// <param name="Secret">The launch secret (64 hex digits). Never put it on a command line or in a log.</param>
/// <param name="Endpoint">Where to POST it as form field "k": http://&lt;host&gt;:&lt;port&gt;/launch.</param>
/// <param name="Url">The same as a GET address (for --print-url: the secret is in the address, so in the browser's history).</param>
public sealed record LaunchTicket(string Secret, Uri Endpoint, Uri Url);

/// <summary>
/// Loopback access control.
/// <para>Host name (R-124): the app is served as http://&lt;32 random hex digits&gt;.localhost:&lt;port&gt;. Kestrel
/// still binds 127.0.0.1 only; browsers resolve every *.localhost name to the loopback interface. The
/// session cookie is host-only for that name, so the browser never sends it to another program
/// listening on a 127.0.0.1 port. Fallback (<c>ipHost</c>): the host is 127.0.0.1 and the cookie is
/// named after the port; browsers then send it to every 127.0.0.1 port.</para>
/// <para>Launch (R-113): a launch secret (256 random bits) is single-use. POST /launch with form field
/// k (the one-shot page the host opens the browser on does that), or GET /launch?k= (--print-url),
/// trades it once for an HttpOnly, SameSite=Strict, host-only session cookie (another 256 random
/// bits) valid until the app stops or the user signs out. A wrong, expired or repeated secret is
/// answered with a page that says so and written to the audit log as a security alert.</para>
/// <para>Every connection must come from a process of the same OS user (<see cref="PeerPolicy"/>);
/// every request needs the exact Host header and the cookie; WebSocket upgrades and non-GET requests
/// also need the app's exact Origin. Refusals are audited (rate-limited). Comparisons are constant-time.</para>
/// </summary>
public sealed class LaunchSecurity : IDisposable
{
    private const string PeerItemKey = "siaara.peer";
    private const int SpentRemembered = 256;
    private readonly Lock _gate = new();
    private readonly List<(byte[] Secret, DateTime? ExpiresUtc)> _secrets = [];
    private readonly Dictionary<string, string> _spent = new(StringComparer.Ordinal);
    private readonly Queue<string> _spentOrder = new();
    private readonly AuditLog? _audit;
    private readonly IPeerUserCheck _peers;
    private readonly AlertLimiter _limiter = new(10, TimeSpan.FromMinutes(1));
    private byte[] _cookieValue = RandomNumberGenerator.GetBytes(32);
    private CancellationTokenSource _session = new();

    /// <summary>Creates the access control.</summary>
    /// <param name="audit">Audit log for launches, sign-outs and refusals (null in unit tests).</param>
    /// <param name="peers">Peer-user check (null = the one of this OS).</param>
    /// <param name="ipHost">Serve as 127.0.0.1 instead of a random *.localhost name (the documented fallback).</param>
    public LaunchSecurity(AuditLog? audit = null, IPeerUserCheck? peers = null, bool ipHost = false)
    {
        _audit = audit;
        _peers = peers ?? PeerUser.ForThisOs();
        IpHost = ipHost;
        HostName = ipHost ? "127.0.0.1" : Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16)) + ".localhost";
    }

    /// <summary>Port Kestrel listens on (set after start).</summary>
    public int Port { get; private set; }

    /// <summary>True in the 127.0.0.1 fallback.</summary>
    public bool IpHost { get; }

    /// <summary>The host name the app is served under: &lt;random&gt;.localhost, or 127.0.0.1 in the fallback.</summary>
    public string HostName { get; }

    /// <summary>Host and port as the Host header must carry them.</summary>
    public string Authority => HostName + ":" + Port.ToString(CultureInfo.InvariantCulture);

    /// <summary>The app's origin, e.g. http://0f3c...9a.localhost:41234.</summary>
    public string Origin => "http://" + Authority;

    /// <summary>Session cookie name. With the random host name the cookie is unique to this run by its host; in the
    /// 127.0.0.1 fallback the name carries the port so two instances do not overwrite each other's cookie.</summary>
    public string CookieName => IpHost ? "siaara_session_" + Port.ToString(CultureInfo.InvariantCulture) : "siaara_session";

    /// <summary>The cookie value (hex; tests use it to act as the browser).</summary>
    public string CookieValue => Convert.ToHexStringLower(Volatile.Read(ref _cookieValue));

    /// <summary>How the peer-user check works on this OS.</summary>
    public string PeerCheckMethod => _peers.Method;

    /// <summary>Cancelled when the user signs out: sockets of the signed-out session close.</summary>
    public CancellationToken SessionToken
    {
        get
        {
            lock (_gate)
            {
                return _session.Token;
            }
        }
    }

    /// <summary>Launch secrets issued and not used yet (tests).</summary>
    public int OutstandingSecrets
    {
        get
        {
            lock (_gate)
            {
                Prune();
                return _secrets.Count;
            }
        }
    }

    /// <summary>Raised (on a request thread) after a launch secret was used.</summary>
    public event EventHandler<LaunchUsedEventArgs>? LaunchUsed;

    /// <summary>Raised after a sign-out: open pages leave.</summary>
    public event EventHandler? SignedOut;

    /// <summary>Sets the port after Kestrel started.</summary>
    /// <param name="port">Port.</param>
    public void SetPort(int port) => Port = port;

    /// <summary>A new single-use launch secret.</summary>
    /// <param name="lifetime">How long it stays valid when unused; null = until used.</param>
    /// <returns>The ticket.</returns>
    public LaunchTicket IssueLaunch(TimeSpan? lifetime = null)
    {
        byte[] secret = RandomNumberGenerator.GetBytes(32);
        lock (_gate)
        {
            Prune();
            _secrets.Add((secret, lifetime is TimeSpan l ? DateTime.UtcNow + l : null));
        }
        string hex = Convert.ToHexStringLower(secret);
        return new LaunchTicket(hex, new Uri(Origin + "/launch"), new Uri(Origin + "/launch?k=" + hex));
    }

    /// <summary>Withdraws an unused launch secret (for example the one in a replaced instance file).</summary>
    /// <param name="ticket">The ticket.</param>
    public void Revoke(LaunchTicket ticket)
    {
        ArgumentNullException.ThrowIfNull(ticket);
        _ = TryConsume(ticket.Secret, "revoked", out _);
    }

    /// <summary>
    /// Signs the browser out: the session cookie stops working at once (a new value is made for the
    /// next launch), sockets of the session are closed and open pages are told to leave. Start SIAARA
    /// Viewer again to get a new window.
    /// </summary>
    public void SignOut()
    {
        CancellationTokenSource old;
        lock (_gate)
        {
            Volatile.Write(ref _cookieValue, RandomNumberGenerator.GetBytes(32));
            old = _session;
            _session = new CancellationTokenSource();
        }
        _audit?.Append("session.signout", AuditOutcome.Success);
        AppLog.Info("security", "signed out: the session cookie was invalidated");
        try
        {
            old.Cancel();
        }
        finally
        {
            old.Dispose();
        }
        SignedOut?.Invoke(this, EventArgs.Empty);
    }

    private static string Fingerprint(byte[] secret) => Convert.ToHexStringLower(SHA256.HashData(secret));

    // Remembers (as a hash) what became of a secret, so a later attempt can be told apart: used / expired / revoked.
    private void RememberLocked(byte[] secret, string fate)
    {
        string f = Fingerprint(secret);
        if (_spent.TryAdd(f, fate))
        {
            _spentOrder.Enqueue(f);
            while (_spentOrder.Count > SpentRemembered)
            {
                _spent.Remove(_spentOrder.Dequeue());
            }
        }
    }

    private void Prune()
    {
        DateTime now = DateTime.UtcNow;
        for (int i = _secrets.Count - 1; i >= 0; i--)
        {
            if (_secrets[i].ExpiresUtc is DateTime e && e < now)
            {
                RememberLocked(_secrets[i].Secret, "expired");
                _secrets.RemoveAt(i);
            }
        }
    }

    // Removes the matching secret. Every entry is compared (constant time each) so the time taken does
    // not depend on which one matched. On failure, "why" is launch-reused, launch-expired, launch-revoked
    // or launch-invalid.
    private bool TryConsume(string? hex, string fate, out string why)
    {
        why = "launch-invalid";
        if (hex is null || hex.Length != 64)
        {
            return false;
        }
        byte[] got;
        try
        {
            got = Convert.FromHexString(hex);
        }
        catch (FormatException)
        {
            return false;
        }
        lock (_gate)
        {
            Prune();
            int found = -1;
            for (int i = 0; i < _secrets.Count; i++)
            {
                if (CryptographicOperations.FixedTimeEquals(got, _secrets[i].Secret))
                {
                    found = i;
                }
            }
            if (found < 0)
            {
                if (_spent.TryGetValue(Fingerprint(got), out string? was))
                {
                    why = was == "used" ? "launch-reused" : "launch-" + was;
                }
                return false;
            }
            RememberLocked(_secrets[found].Secret, fate);
            _secrets.RemoveAt(found);
            return true;
        }
    }

    private static bool FixedEquals(string? hex, byte[] expected)
    {
        if (hex is null || hex.Length != expected.Length * 2)
        {
            return false;
        }
        byte[] got;
        try
        {
            got = Convert.FromHexString(hex);
        }
        catch (FormatException)
        {
            return false;
        }
        return CryptographicOperations.FixedTimeEquals(got, expected);
    }

    /// <summary>The request carries the session cookie.</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when it does.</returns>
    public bool HasCookie(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        return FixedEquals(ctx.Request.Cookies[CookieName], Volatile.Read(ref _cookieValue));
    }

    /// <summary>The Origin header is exactly the app's origin.</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when it is.</returns>
    public bool OriginAllowed(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        string? o = ctx.Request.Headers.Origin;
        return o is not null && string.Equals(o, Origin, StringComparison.Ordinal);
    }

    /// <summary>The Host header is exactly the app's host name and port (DNS rebinding, other ports, plain 127.0.0.1).</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when it is.</returns>
    public bool HostAllowed(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        return string.Equals(ctx.Request.Host.Value, Authority, StringComparison.Ordinal) && ctx.Connection.LocalPort == Port;
    }

    /// <summary>
    /// The process at the other end of the connection belongs to the same OS user (<see cref="PeerPolicy"/>).
    /// Asked once per connection; the answer is kept with the connection.
    /// </summary>
    /// <param name="ctx">Request.</param>
    /// <returns>The owner of the peer.</returns>
    public PeerOwner PeerOf(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        IDictionary<object, object?>? items = ctx.Features.Get<IConnectionItemsFeature>()?.Items;
        if (items is not null && items.TryGetValue(PeerItemKey, out object? known) && known is PeerOwner cached)
        {
            return cached;
        }
        ConnectionInfo c = ctx.Connection;
        PeerOwner owner = c.LocalIpAddress is null || c.RemoteIpAddress is null ? PeerOwner.Unknown
            : _peers.Check(new IPEndPoint(c.LocalIpAddress, c.LocalPort), new IPEndPoint(c.RemoteIpAddress, c.RemotePort));
        items?[PeerItemKey] = owner;
        return owner;
    }

    /// <summary>A WebSocket upgrade is allowed (cookie and Origin; the peer and the Host header were checked before).</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when allowed.</returns>
    public bool WebSocketAllowed(HttpContext ctx) => HasCookie(ctx) && OriginAllowed(ctx);

    /// <summary>The Content-Security-Policy of every response (R-125): no inline script, no WebAssembly, no plug-ins,
    /// forms only to the app itself, and a base address only on the app's own origin (the root document has
    /// &lt;base href="/"&gt;, which Blazor needs, so 'none' is not possible).</summary>
    public string ContentSecurityPolicy =>
        "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' ws://" + Authority +
        "; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'none'";

    private void Refuse(string kind, HttpContext ctx)
    {
        if (_audit is null)
        {
            return;
        }
        (bool write, int suppressed) = _limiter.Admit(kind);
        if (suppressed > 0)
        {
            _audit.Alert("alerts-suppressed", new Dictionary<string, string>
            {
                ["of"] = kind,
                ["count"] = suppressed.ToString(CultureInfo.InvariantCulture),
            });
        }
        if (write)
        {
            // The path only (never the query: it can hold a secret), cut short.
            string path = ctx.Request.Path.Value ?? "";
            _audit.Alert(kind, new Dictionary<string, string>
            {
                ["method"] = ctx.Request.Method.Length <= 8 ? ctx.Request.Method : "other",
                ["path"] = path.StartsWith("/viewer/", StringComparison.Ordinal) ? "/viewer/" : path.Length <= 48 ? path : path[..48],
            });
        }
    }

    /// <summary>Address of the page shown after a sign-out (served without the cookie; it holds no data).</summary>
    public const string SignedOutPath = "/signed-out";

    private static Task WritePageAsync(HttpContext ctx, int status, string title, string body, bool refreshToRoot = false)
    {
        ctx.Response.StatusCode = status;
        ctx.Response.ContentType = "text/html; charset=utf-8";
        string html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">" + (refreshToRoot ? "<meta http-equiv=\"refresh\" content=\"0;url=/\">" : "") +
            "<link rel=\"icon\" href=\"data:,\"><title>SIAARA Viewer</title></head><body style=\"font-family:sans-serif;margin:3em\"><h1>" + WebUtility.HtmlEncode(title) + "</h1><p>" + body + "</p></body></html>";
        return ctx.Response.Body.WriteAsync(Encoding.UTF8.GetBytes(html)).AsTask();
    }

    // The secret of a launch request: the query for GET, the form field k for POST (read by hand: small, no form machinery).
    private static async Task<string?> ReadSecretAsync(HttpContext ctx)
    {
        if (HttpMethods.IsGet(ctx.Request.Method))
        {
            return ctx.Request.Query["k"];
        }
        if (!HttpMethods.IsPost(ctx.Request.Method) || ctx.Request.ContentLength is not (> 0 and <= 256))
        {
            return null;
        }
        var buf = new byte[(int)ctx.Request.ContentLength.Value];
        int n = 0;
        while (n < buf.Length)
        {
            int r = await ctx.Request.Body.ReadAsync(buf.AsMemory(n), ctx.RequestAborted).ConfigureAwait(false);
            if (r == 0)
            {
                break;
            }
            n += r;
        }
        string form = Encoding.ASCII.GetString(buf, 0, n);
        foreach (string pair in form.Split('&'))
        {
            if (pair.StartsWith("k=", StringComparison.Ordinal))
            {
                return pair[2..];
            }
        }
        return null;
    }

    private async Task LaunchAsync(HttpContext ctx)
    {
        string? secret = await ReadSecretAsync(ctx).ConfigureAwait(false);
        if (!TryConsume(secret, "used", out string why))
        {
            Refuse(why, ctx);
            (string title, string text) = why switch
            {
                "launch-reused" => ("This link was already used",
                    "A SIAARA Viewer launch link works once, and this one has been used. If it was not you who opened the viewer with it, another program on this " +
                    "computer may have; the attempt is recorded in the audit log. Close this page and start SIAARA Viewer again to open a new window."),
                "launch-expired" => ("This link has expired", "The launch link was not used in time. Start SIAARA Viewer again to open a new window."),
                _ => ("This link does not work", "Start SIAARA Viewer again to open a window. The attempt is recorded in the audit log."),
            };
            await WritePageAsync(ctx, StatusCodes.Status403Forbidden, title, text).ConfigureAwait(false);
            return;
        }
        // Host-only (no Domain attribute), path /, session lifetime (no Expires): gone when the browser closes,
        // worthless when the app stops or the user signs out.
        ctx.Response.Cookies.Append(CookieName, CookieValue, new CookieOptions
        {
            HttpOnly = true,
            SameSite = SameSiteMode.Strict,
            Secure = false,
            Path = "/",
            IsEssential = true,
        });
        _audit?.Append("session.launch", AuditOutcome.Success, new Dictionary<string, string> { ["via"] = HttpMethods.IsPost(ctx.Request.Method) ? "page" : "address" });
        // Not a redirect: the request may come from the local launch page (another site), and a SameSite=Strict
        // cookie is not sent along a redirect of such a navigation. This page is ours, so its own move to / carries it.
        await WritePageAsync(ctx, StatusCodes.Status200OK, "Opening SIAARA Viewer", "<a href=\"/\">Continue</a>", refreshToRoot: true).ConfigureAwait(false);
        LaunchUsed?.Invoke(this, new LaunchUsedEventArgs(secret!));
    }

    /// <summary>Adds the checks to the pipeline (first middleware).</summary>
    /// <param name="app">App.</param>
    public void Use(IApplicationBuilder app)
    {
        ArgumentNullException.ThrowIfNull(app);
        app.Use(async (ctx, next) =>
        {
            PeerOwner peer = PeerOf(ctx);
            if (!PeerPolicy.Allows(peer))
            {
                // Another OS user (or an owner we cannot determine) gets nothing, not even a page.
                Refuse(PeerPolicy.RefusalKind(peer), ctx);
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                ctx.Response.Headers.Connection = "close";
                return;
            }
            if (!HostAllowed(ctx))
            {
                Refuse("host-refused", ctx);
                ctx.Response.StatusCode = StatusCodes.Status400BadRequest;
                return;
            }
            ctx.Response.Headers.XContentTypeOptions = "nosniff";
            // Pages, thumbnails and API answers can hold patient data: none of them may land in the
            // browser's disk cache. Static framework files (scripts, styles) set their own caching
            // later in the pipeline and hold no patient data.
            ctx.Response.Headers.CacheControl = "no-store";
            ctx.Response.Headers.Pragma = "no-cache";
            ctx.Response.Headers.XFrameOptions = "DENY";
            ctx.Response.Headers["Referrer-Policy"] = "no-referrer";
            ctx.Response.Headers.ContentSecurityPolicy = ContentSecurityPolicy;
            if (ctx.Request.Path.Equals("/launch", StringComparison.Ordinal))
            {
                await LaunchAsync(ctx).ConfigureAwait(false);
                return;
            }
            if (ctx.Request.Path.Equals(SignedOutPath, StringComparison.Ordinal) && HttpMethods.IsGet(ctx.Request.Method) && !HasCookie(ctx))
            {
                await WritePageAsync(ctx, StatusCodes.Status200OK, "Signed out",
                    "This window no longer has access to SIAARA Viewer. Start SIAARA Viewer again to open a new window.").ConfigureAwait(false);
                return;
            }
            if (!HasCookie(ctx))
            {
                Refuse("cookie-refused", ctx);
                await WritePageAsync(ctx, StatusCodes.Status403Forbidden, "Open SIAARA Viewer from its launcher",
                    "This page needs a window opened by SIAARA Viewer. Start SIAARA Viewer (again) to open one.").ConfigureAwait(false);
                return;
            }
            // By the request's own headers: this middleware runs before UseWebSockets, so WebSockets.IsWebSocketRequest
            // is not yet set here. Every upgrade (the Blazor circuit as well as /ws/view) needs the exact Origin.
            bool upgrade = ctx.Request.Headers.Upgrade.Count > 0 || ctx.Features.Get<Microsoft.AspNetCore.Http.Features.IHttpUpgradeFeature>()?.IsUpgradableRequest == true;
            bool unsafeMethod = !HttpMethods.IsGet(ctx.Request.Method) && !HttpMethods.IsHead(ctx.Request.Method);
            if ((upgrade || unsafeMethod) && !OriginAllowed(ctx))
            {
                Refuse("origin-refused", ctx);
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }
            await next(ctx).ConfigureAwait(false);
        });
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        lock (_gate)
        {
            _session.Dispose();
        }
    }
}

/// <summary>
/// Keeps refusals from flooding the audit log: at most <c>limit</c> entries of one kind per window;
/// the rest are counted and reported as one "alerts-suppressed" entry when the next one is admitted.
/// </summary>
public sealed class AlertLimiter(int limit, TimeSpan window)
{
    private readonly Lock _gate = new();
    private readonly Dictionary<string, (DateTime Start, int Written, int Suppressed)> _kinds = new(StringComparer.Ordinal);

    /// <summary>Asks whether an alert of this kind may be written now.</summary>
    /// <param name="kind">Alert kind.</param>
    /// <param name="nowUtc">The time (tests); null = now.</param>
    /// <returns>Write it or not, and how many were dropped in the window that just ended (to report once).</returns>
    public (bool Write, int SuppressedBefore) Admit(string kind, DateTime? nowUtc = null)
    {
        DateTime now = nowUtc ?? DateTime.UtcNow;
        lock (_gate)
        {
            _kinds.TryGetValue(kind, out (DateTime Start, int Written, int Suppressed) k);
            int report = 0;
            if (k.Written == 0 && k.Suppressed == 0 || now - k.Start >= window)
            {
                report = k.Suppressed;
                k = (now, 0, 0);
            }
            bool write = k.Written < limit;
            k = write ? (k.Start, k.Written + 1, k.Suppressed) : (k.Start, k.Written, k.Suppressed + 1);
            _kinds[kind] = k;
            return (write, report);
        }
    }
}
