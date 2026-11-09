// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Siaara.Viewer.Web.Security;
using Siaara.Viewer.Web.Streaming;

namespace Siaara.Viewer.Web;

/// <summary>Registers and maps the web layer.</summary>
public static class ViewerWebApp
{
    /// <summary>Adds the viewer's services (runtime and security are created by the host).</summary>
    /// <param name="services">Services.</param>
    /// <param name="runtime">Runtime.</param>
    /// <param name="security">Security.</param>
    /// <returns>The services.</returns>
    public static IServiceCollection AddSiaaraViewer(this IServiceCollection services, ViewerRuntime runtime, LaunchSecurity security)
    {
        ArgumentNullException.ThrowIfNull(runtime);
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
        app.MapRazorComponents<App>().AddInteractiveServerRenderMode();
        app.MapViewSocket();
        app.MapGet("/api/thumb", async (HttpContext ctx, ViewerRuntime rt) =>
        {
            string? series = ctx.Request.Query["series"];
            if (string.IsNullOrEmpty(series))
            {
                return Results.BadRequest();
            }
            byte[]? png = await rt.Thumbnails.GetSeriesThumbnailAsync(series, ctx.RequestAborted).ConfigureAwait(false);
            ctx.Response.Headers.CacheControl = "private, max-age=300";
            return png is null ? Results.NotFound() : Results.Bytes(png, "image/png");
        });
        app.MapGet("/api/health", () => Results.Text("ok"));
    }
}
