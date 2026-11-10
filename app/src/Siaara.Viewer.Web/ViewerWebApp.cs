// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.DataProtection;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Siaara.Viewer.Core.Viewing;
using Siaara.Viewer.Web.Security;
using Siaara.Viewer.Web.Streaming;

namespace Siaara.Viewer.Web;

/// <summary>Registers and maps the web layer.</summary>
public static class ViewerWebApp
{
    /// <summary>Longest Study Instance UID accepted in a request body (the standard allows 64 characters).</summary>
    private const int MaxUidBytes = 128;

    /// <summary>Adds the viewer's services (runtime and security are created by the host).</summary>
    /// <param name="services">Services.</param>
    /// <param name="runtime">Runtime.</param>
    /// <param name="security">Security.</param>
    /// <returns>The services.</returns>
    public static IServiceCollection AddSiaaraViewer(this IServiceCollection services, ViewerRuntime runtime, LaunchSecurity security)
    {
        ArgumentNullException.ThrowIfNull(runtime);
        // Keys only protect Blazor circuit state of this run (the session cookie changes every start):
        // keep them in memory instead of writing unencrypted key files to the user profile.
        services.AddDataProtection().UseEphemeralDataProtectionProvider();
        services.AddSingleton(runtime);
        services.AddSingleton(security);
        services.AddSingleton(runtime.Sessions);
        services.AddRazorComponents().AddInteractiveServerComponents(o =>
        {
            o.DetailedErrors = false;
            o.DisconnectedCircuitRetentionPeriod = TimeSpan.FromMinutes(2);
        });
        return services;
    }

    /// <summary>Middleware and endpoints: security first, then WebSockets, static files, Blazor, /ws/view, /api/thumb.</summary>
    /// <param name="app">App.</param>
    /// <param name="mapStaticAssets">Map the static web assets (false in tests that have no manifest).</param>
    public static void MapSiaaraViewer(this WebApplication app, bool mapStaticAssets = true)
    {
        ArgumentNullException.ThrowIfNull(app);
        LaunchSecurity sec = app.Services.GetRequiredService<LaunchSecurity>();
        sec.Use(app);
        app.UseWebSockets(new WebSocketOptions { KeepAliveInterval = TimeSpan.FromSeconds(20) });
        app.UseAntiforgery();
        if (mapStaticAssets)
        {
            app.MapStaticAssets();
        }
        // Blazor would add a second Content-Security-Policy header ("frame-ancestors 'self'"); the app's own policy
        // (LaunchSecurity.ContentSecurityPolicy, frame-ancestors 'none') is the only one sent.
        app.MapRazorComponents<App>().AddInteractiveServerRenderMode(o => o.ContentSecurityFrameAncestorsPolicy = null);
        app.MapViewSocket();
        // Study and Series UIDs never travel in a URL (browser history): the series is named by its per-run id.
        app.MapGet("/api/thumb", async (HttpContext ctx, ViewerRuntime rt) =>
        {
            string? series = rt.Ids.Resolve(ctx.Request.Query["series"]);
            if (series is null)
            {
                return Results.NotFound();
            }
            byte[]? png = await rt.Thumbnails.GetSeriesThumbnailAsync(series, ctx.RequestAborted).ConfigureAwait(false);
            // Thumbnails show patient images: never into the browser's disk cache.
            ctx.Response.Headers.CacheControl = "no-store";
            ctx.Response.Headers.Pragma = "no-cache";
            return png is null ? Results.NotFound() : Results.Bytes(png, "image/png");
        });
        app.MapGet("/api/health", () => Results.Text("ok"));
        // Opens a study for a viewer that is not the Blazor page (scripts, a future native shell). Same
        // rules as the page: cookie, and as a POST the exact Origin; audited as study.open. The Study
        // Instance UID is the request body (text), not part of the address. The answer holds the
        // session id for /ws/view and the per-run id for /viewer/<ref>.
        app.MapPost("/api/sessions", async (HttpContext ctx, ViewerRuntime rt) =>
        {
            string? study = await ReadSmallBodyAsync(ctx).ConfigureAwait(false);
            if (string.IsNullOrEmpty(study))
            {
                return Results.BadRequest();
            }
            try
            {
                ViewerSession s = await rt.OpenStudyAsync(study).ConfigureAwait(false);
                return Results.Text("{\"id\":\"" + s.Id + "\",\"ref\":\"" + rt.Ids.For(study) + "\"}", "application/json");
            }
            catch (SessionLimitException)
            {
                return Results.StatusCode(StatusCodes.Status429TooManyRequests);
            }
        });
        // Sign out / lock for scripts (R-158; the pages' Sign out button calls ViewerRuntime.SignOut over the circuit):
        // the cookie stops working at once, viewer sessions and their sockets close.
        app.MapPost("/signout", (HttpContext ctx, ViewerRuntime rt) =>
        {
            rt.SignOut(sec);
            ctx.Response.Cookies.Delete(sec.CookieName, new CookieOptions { Path = "/", HttpOnly = true, SameSite = SameSiteMode.Strict });
            return Results.Redirect(LaunchSecurity.SignedOutPath);
        });
    }

    // A short text body (a UID); null when it is missing, too long or not plain UID characters.
    private static async Task<string?> ReadSmallBodyAsync(HttpContext ctx)
    {
        if (ctx.Request.ContentLength is not (> 0 and <= MaxUidBytes))
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
        string text = Encoding.ASCII.GetString(buf, 0, n).Trim();
        return text.Length > 0 && text.All(c => char.IsAsciiDigit(c) || c == '.') ? text : null;
    }
}
