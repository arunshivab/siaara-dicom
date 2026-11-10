// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace Siaara.Viewer.Web.Security;

/// <summary>A launch URL was used.</summary>
/// <param name="url">The URL (its secret is no longer valid).</param>
public sealed class LaunchUrlUsedEventArgs(Uri url) : EventArgs
{
    /// <summary>The URL.</summary>
    public Uri Url { get; } = url;
}

/// <summary>
/// Loopback access control. The host opens the browser with a URL holding a random 256-bit launch
/// secret. A launch secret is single-use: GET /launch?k=... trades it once for an HttpOnly,
/// SameSite=Strict, host-only session cookie (a second random 256-bit value, named after the port)
/// and the secret is gone; a URL seen later (process list, shell history, the per-user instance
/// file) is worthless. Every request needs the cookie, the Host header must be exactly
/// 127.0.0.1:&lt;port&gt;, and WebSocket upgrades and non-GET requests also need the app's exact Origin.
/// Comparisons are constant-time.
/// </summary>
public sealed class LaunchSecurity
{
    private readonly byte[] _cookieValue = RandomNumberGenerator.GetBytes(32);
    private readonly Lock _gate = new();
    private readonly List<(byte[] Secret, DateTime? ExpiresUtc)> _secrets = [];

    /// <summary>Port Kestrel listens on (set after start).</summary>
    public int Port { get; private set; }

    /// <summary>The app's origin, e.g. http://127.0.0.1:41234.</summary>
    public string Origin => "http://127.0.0.1:" + Port.ToString(CultureInfo.InvariantCulture);

    /// <summary>Session cookie name; includes the port so instances (and other apps on 127.0.0.1) never share it.</summary>
    public string CookieName => "siaara_session_" + Port.ToString(CultureInfo.InvariantCulture);

    /// <summary>The cookie value (hex; tests use it to act as the browser).</summary>
    public string CookieValue => Convert.ToHexStringLower(_cookieValue);

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

    /// <summary>Raised (on a request thread) after a launch secret was used; the argument is the URL that held it.</summary>
    public event EventHandler<LaunchUrlUsedEventArgs>? LaunchUrlUsed;

    /// <summary>Sets the port after Kestrel started.</summary>
    /// <param name="port">Port.</param>
    public void SetPort(int port) => Port = port;

    /// <summary>A new single-use launch URL.</summary>
    /// <param name="lifetime">How long it stays valid when unused; null = until used.</param>
    /// <returns>The URL.</returns>
    public Uri IssueLaunchUrl(TimeSpan? lifetime = null)
    {
        byte[] secret = RandomNumberGenerator.GetBytes(32);
        lock (_gate)
        {
            Prune();
            _secrets.Add((secret, lifetime is TimeSpan l ? DateTime.UtcNow + l : null));
        }
        return UrlFor(secret);
    }

    /// <summary>Withdraws an unused launch URL (for example the one in a replaced instance file).</summary>
    /// <param name="url">The URL.</param>
    public void Revoke(Uri url)
    {
        ArgumentNullException.ThrowIfNull(url);
        _ = TryConsume(SecretOf(url), out _);
    }

    private Uri UrlFor(byte[] secret) => new(Origin + "/launch?k=" + Convert.ToHexStringLower(secret));

    private static string? SecretOf(Uri url)
    {
        string q = url.Query;
        int i = q.IndexOf("k=", StringComparison.Ordinal);
        return i < 0 ? null : q[(i + 2)..];
    }

    private void Prune()
    {
        DateTime now = DateTime.UtcNow;
        _secrets.RemoveAll(s => s.ExpiresUtc is DateTime e && e < now);
    }

    // Removes and reports the matching secret. Every entry is compared (constant time each) so the
    // time taken does not depend on which one matched.
    private bool TryConsume(string? hex, out byte[] secret)
    {
        secret = [];
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
                return false;
            }
            secret = _secrets[found].Secret;
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
        return FixedEquals(ctx.Request.Cookies[CookieName], _cookieValue);
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

    /// <summary>The Host header is exactly 127.0.0.1 and the app's port (DNS rebinding, other ports).</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when it is.</returns>
    public bool HostAllowed(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        return string.Equals(ctx.Request.Host.Value, "127.0.0.1:" + Port.ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal)
               && ctx.Connection.LocalPort == Port;
    }

    /// <summary>A WebSocket upgrade is allowed (cookie and Origin).</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when allowed.</returns>
    public bool WebSocketAllowed(HttpContext ctx) => HasCookie(ctx) && OriginAllowed(ctx);

    /// <summary>Adds the checks to the pipeline (first middleware).</summary>
    /// <param name="app">App.</param>
    public void Use(IApplicationBuilder app)
    {
        ArgumentNullException.ThrowIfNull(app);
        app.Use(async (ctx, next) =>
        {
            if (!HostAllowed(ctx))
            {
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
            ctx.Response.Headers.ContentSecurityPolicy =
                "default-src 'self'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' ws://127.0.0.1:" +
                Port.ToString(CultureInfo.InvariantCulture) + "; frame-ancestors 'none'";
            if (ctx.Request.Path.Equals("/launch", StringComparison.Ordinal))
            {
                ctx.Response.Headers.CacheControl = "no-store";
                if (!TryConsume(ctx.Request.Query["k"], out byte[] used))
                {
                    ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                    ctx.Response.ContentType = "text/plain; charset=utf-8";
                    await ctx.Response.Body.WriteAsync(Encoding.UTF8.GetBytes("This launch link was already used or has expired. Start SIAARA Viewer again to open a window."))
                        .ConfigureAwait(false);
                    return;
                }
                // Host-only (no Domain attribute), path /, session lifetime.
                ctx.Response.Cookies.Append(CookieName, CookieValue, new CookieOptions
                {
                    HttpOnly = true,
                    SameSite = SameSiteMode.Strict,
                    Secure = false,
                    Path = "/",
                    IsEssential = true,
                });
                ctx.Response.Redirect("/");
                LaunchUrlUsed?.Invoke(this, new LaunchUrlUsedEventArgs(UrlFor(used)));
                return;
            }
            if (!HasCookie(ctx))
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                ctx.Response.ContentType = "text/plain; charset=utf-8";
                await ctx.Response.Body.WriteAsync(Encoding.UTF8.GetBytes("Open SIAARA Viewer from its launcher.")).ConfigureAwait(false);
                return;
            }
            bool upgrade = ctx.WebSockets.IsWebSocketRequest;
            bool unsafeMethod = !HttpMethods.IsGet(ctx.Request.Method) && !HttpMethods.IsHead(ctx.Request.Method);
            if ((upgrade || unsafeMethod) && !OriginAllowed(ctx))
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }
            await next(ctx).ConfigureAwait(false);
        });
    }
}
