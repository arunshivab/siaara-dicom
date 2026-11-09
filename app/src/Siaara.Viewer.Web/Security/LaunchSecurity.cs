// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace Siaara.Viewer.Web.Security;

/// <summary>
/// Loopback access control. A random 256-bit launch secret is part of the URL the host opens; the
/// browser trades it (GET /launch?k=...) for an HttpOnly, SameSite=Strict session cookie holding a
/// second random 256-bit value. Every request needs that cookie, the Host header must be the
/// loopback address and port, and WebSocket upgrades (Blazor and /ws/view) also need the exact
/// Origin of the app. Comparisons are constant-time.
/// </summary>
public sealed class LaunchSecurity
{
    /// <summary>Session cookie name.</summary>
    public const string CookieName = "siaara_session";

    private readonly byte[] _launchSecret = RandomNumberGenerator.GetBytes(32);
    private readonly byte[] _cookieValue = RandomNumberGenerator.GetBytes(32);

    /// <summary>Port Kestrel listens on (set after start).</summary>
    public int Port { get; private set; }

    /// <summary>The app's origin, e.g. http://127.0.0.1:41234.</summary>
    public string Origin => "http://127.0.0.1:" + Port.ToString(CultureInfo.InvariantCulture);

    /// <summary>The launch secret (hex).</summary>
    public string LaunchSecret => Convert.ToHexStringLower(_launchSecret);

    /// <summary>The cookie value (hex; tests use it to act as the browser).</summary>
    public string CookieValue => Convert.ToHexStringLower(_cookieValue);

    /// <summary>The URL that logs the browser in.</summary>
    public Uri LaunchUrl => new(Origin + "/launch?k=" + LaunchSecret);

    /// <summary>Sets the port once Kestrel is listening.</summary>
    /// <param name="port">Port.</param>
    public void SetPort(int port) => Port = port;

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
    /// <returns>True when authorised.</returns>
    public bool HasCookie(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        return FixedEquals(ctx.Request.Cookies[CookieName], _cookieValue);
    }

    /// <summary>The Origin header is exactly the app's origin.</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when allowed.</returns>
    public bool OriginAllowed(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        string? o = ctx.Request.Headers.Origin;
        return o is not null && string.Equals(o, Origin, StringComparison.Ordinal);
    }

    /// <summary>The Host header names the loopback listener (DNS-rebinding defence).</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when allowed.</returns>
    public bool HostAllowed(HttpContext ctx)
    {
        ArgumentNullException.ThrowIfNull(ctx);
        string host = ctx.Request.Host.Value ?? "";
        string p = Port.ToString(CultureInfo.InvariantCulture);
        return string.Equals(host, "127.0.0.1:" + p, StringComparison.Ordinal) || string.Equals(host, "localhost:" + p, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>A WebSocket upgrade with the cookie and the exact Origin.</summary>
    /// <param name="ctx">Request.</param>
    /// <returns>True when allowed.</returns>
    public bool WebSocketAllowed(HttpContext ctx) => HasCookie(ctx) && OriginAllowed(ctx);

    /// <summary>The middleware: Host check, /launch exchange, cookie on every request, Origin on upgrades and posts.</summary>
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
            ctx.Response.Headers.XFrameOptions = "DENY";
            ctx.Response.Headers["Referrer-Policy"] = "no-referrer";
            ctx.Response.Headers.ContentSecurityPolicy =
                "default-src 'self'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' ws://127.0.0.1:" +
                Port.ToString(CultureInfo.InvariantCulture) + "; frame-ancestors 'none'";
            if (ctx.Request.Path.Equals("/launch", StringComparison.Ordinal))
            {
                if (!FixedEquals(ctx.Request.Query["k"], _launchSecret))
                {
                    ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                    return;
                }
                ctx.Response.Cookies.Append(CookieName, CookieValue, new CookieOptions
                {
                    HttpOnly = true,
                    SameSite = SameSiteMode.Strict,
                    Secure = false,
                    Path = "/",
                    IsEssential = true,
                });
                ctx.Response.Headers.CacheControl = "no-store";
                ctx.Response.Redirect("/");
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
