// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Net;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Components.Server.Circuits;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Web;
using Siaara.Viewer.Web.Security;

namespace Siaara.Viewer.Host;

/// <summary>How to start the host.</summary>
internal sealed record HostOptions
{
    /// <summary>Per-user data folder override (tests); null = the per-user default.</summary>
    public string? DataDir { get; init; }
    /// <summary>Store folder override; null = settings, then the default.</summary>
    public string? StoreRoot { get; init; }
    /// <summary>Map static web assets (false when the test process has no manifest).</summary>
    public bool MapStaticAssets { get; init; } = true;
    /// <summary>Stop when no window has been connected for this long (null = never).</summary>
    public TimeSpan? IdleExit { get; init; }
    /// <summary>Echo the app log to stderr.</summary>
    public bool Verbose { get; init; }
    /// <summary>Serve as 127.0.0.1 with a port-named cookie instead of a random *.localhost name (the documented fallback).</summary>
    public bool IpHost { get; init; }
    /// <summary>Audit key source (tests use the key file); null = the OS key store.</summary>
    public IAuditKeySource? AuditKeys { get; init; }
    /// <summary>Peer-user check (tests replace it to act as another user); null = the one of this OS.</summary>
    public Core.Platform.IPeerUserCheck? Peers { get; init; }
}

/// <summary>A started viewer: Kestrel on 127.0.0.1 at a random port.</summary>
internal sealed class RunningViewer : IAsyncDisposable
{
    private readonly ToolkitScheduler _scheduler;

    internal RunningViewer(WebApplication app, ViewerRuntime runtime, LaunchSecurity security, ToolkitScheduler scheduler)
    {
        App = app;
        Runtime = runtime;
        Security = security;
        _scheduler = scheduler;
    }

    public WebApplication App { get; }
    public ViewerRuntime Runtime { get; }
    public LaunchSecurity Security { get; }
    public int Port => Security.Port;
    public Uri BaseUrl => new(Security.Origin + "/");

    private Task _idle = Task.CompletedTask;

    /// <summary>Stops the app when no Blazor circuit has been open for <paramref name="idle"/>.</summary>
    /// <param name="circuits">Circuit counter.</param>
    /// <param name="idle">Idle time.</param>
    public void WatchIdle(CircuitCounter circuits, TimeSpan idle) => _idle = WatchIdleAsync(circuits, idle);

    private async Task WatchIdleAsync(CircuitCounter circuits, TimeSpan idle)
    {
        try
        {
            using var timer = new PeriodicTimer(TimeSpan.FromSeconds(5));
            DateTime started = DateTime.UtcNow;
            while (await timer.WaitForNextTickAsync(App.Lifetime.ApplicationStopping).ConfigureAwait(false))
            {
                DateTime since = circuits.EverConnected ? circuits.LastChangeUtc : started;
                if (circuits.Count == 0 && DateTime.UtcNow - since > idle)
                {
                    AppLog.Info("host", "no window connected: shutting down");
                    App.Lifetime.StopApplication();
                    return;
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Stopping.
        }
    }

    public async ValueTask DisposeAsync()
    {
        Runtime.Audit.Append("app.stop", AuditOutcome.Success);
        await App.StopAsync().ConfigureAwait(false);
        await _idle.ConfigureAwait(false);
        await App.DisposeAsync().ConfigureAwait(false);
        Runtime.Dispose();
        Runtime.Store.Dispose();
        Runtime.Audit.Dispose();
        Security.Dispose();
        _scheduler.Dispose();
    }
}

/// <summary>Counts Blazor circuits for the idle exit.</summary>
internal sealed class CircuitCounter : CircuitHandler
{
    private int _count;

    public int Count => Volatile.Read(ref _count);

    public DateTime LastChangeUtc { get; private set; } = DateTime.UtcNow;

    public bool EverConnected { get; private set; }

    public override Task OnCircuitOpenedAsync(Circuit circuit, CancellationToken cancellationToken)
    {
        Interlocked.Increment(ref _count);
        EverConnected = true;
        LastChangeUtc = DateTime.UtcNow;
        return Task.CompletedTask;
    }

    public override Task OnCircuitClosedAsync(Circuit circuit, CancellationToken cancellationToken)
    {
        Interlocked.Decrement(ref _count);
        LastChangeUtc = DateTime.UtcNow;
        return Task.CompletedTask;
    }
}

/// <summary>Composition of the host (also used by the end-to-end tests).</summary>
internal static class ViewerHost
{
    /// <summary>Kestrel's limit of open (not upgraded) connections.</summary>
    public const int MaxConnections = 64;

    /// <summary>Kestrel's limit of open WebSocket connections (Blazor circuits and /ws/view).</summary>
    public const int MaxUpgradedConnections = 32;

    private static int s_unobservedHooked;

    /// <summary>Opens the store and starts Kestrel. Throws <see cref="StoreLockedException"/> when another process has the store.</summary>
    /// <param name="o">Options.</param>
    /// <param name="args">Command-line arguments for the web host.</param>
    /// <returns>The running viewer.</returns>
    public static async Task<RunningViewer> StartAsync(HostOptions o, string[]? args = null)
    {
        ArgumentNullException.ThrowIfNull(o);
        if (Interlocked.Exchange(ref s_unobservedHooked, 1) == 0)
        {
            // Safety net: a task failure nobody observed is logged (type only) instead of vanishing.
            TaskScheduler.UnobservedTaskException += (_, e) =>
            {
                AppLog.Error("host", "unobserved background task failure", e.Exception.GetBaseException());
                e.SetObserved();
            };
        }
        AppPaths paths = o.DataDir is null ? AppPaths.ForCurrentUser() : new AppPaths(o.DataDir);
        paths.EnsureCreated();
        AppLog.Open(paths.LogFolder);
        AppLog.EchoToConsole = o.Verbose;
        AppLog.AttachToolkit();
        var settings = new SettingsStore(paths.SettingsFile);
        string storeRoot = o.StoreRoot ?? settings.Current.StoreRoot ?? paths.DefaultStoreRoot;
        ToolkitScheduler? scheduler = null;
        StoreService? store = null;
        AuditLog? audit = null;
        ViewerRuntime? runtime = null;
        WebApplication? app = null;
        try
        {
            scheduler = new ToolkitScheduler();
            store = StoreService.Open(storeRoot, scheduler);
            audit = new AuditLog(paths.AuditFile, o.AuditKeys ?? AuditKeys.OsStore);
            runtime = new ViewerRuntime(paths, settings, scheduler, store, audit);
            app = await BuildAndStartAsync(runtime, o, args).ConfigureAwait(false);
            LaunchSecurity security = app.Services.GetRequiredService<LaunchSecurity>();
            audit.Append("app.start", AuditOutcome.Success, new Dictionary<string, string>
            {
                ["version"] = typeof(ViewerHost).Assembly.GetName().Version?.ToString() ?? "",
                ["toolkit"] = Tk.Version,
                ["abi"] = Tk.AbiVersion.ToString(CultureInfo.InvariantCulture),
                // Never the path itself (AuditLog.PathReference).
                ["store"] = audit.PathReference(storeRoot),
                ["host"] = security.IpHost ? "ip" : "localhost-name",
                ["peerCheck"] = security.PeerCheckMethod,
                ["auditKey"] = audit.KeyProtection.ToString(),
            });
            if (runtime.DataFolderWarning is not null)
            {
                AppLog.Warn("security", "the data folder can be reached by other users of this computer");
                audit.Alert("data-folder-not-private");
            }
            if (security.IpHost)
            {
                AppLog.Warn("security", "serving as 127.0.0.1: the session cookie is visible to other programs listening on 127.0.0.1");
            }
            AppLog.Info("host", "listening on the loopback interface; peer check: " + security.PeerCheckMethod);
            var running = new RunningViewer(app, runtime, security, scheduler);
            if (o.IdleExit is TimeSpan idle)
            {
                running.WatchIdle(app.Services.GetRequiredService<CircuitCounter>(), idle);
            }
            (app, runtime, audit, store, scheduler) = (null, null, null, null, null);
            return running;
        }
        finally
        {
            if (app is not null)
            {
                await app.DisposeAsync().ConfigureAwait(false);
            }
            runtime?.Dispose();
            audit?.Dispose();
            store?.Dispose();
            scheduler?.Dispose();
        }
    }

    private static async Task<WebApplication> BuildAndStartAsync(ViewerRuntime runtime, HostOptions o, string[]? args)
    {
        // Owned by the RunningViewer (disposed with it); the service container only holds a reference.
#pragma warning disable CA2000
        var security = new LaunchSecurity(runtime.Audit, o.Peers, o.IpHost);
#pragma warning restore CA2000
        WebApplicationBuilder builder = WebApplication.CreateBuilder(new WebApplicationOptions
        {
            Args = args ?? [],
            ContentRootPath = AppContext.BaseDirectory,
            ApplicationName = typeof(ViewerHost).Assembly.GetName().Name,
            EnvironmentName = Environments.Production,
        });
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton<ILoggerProvider, RedactingLoggerProvider>();
        builder.WebHost.UseStaticWebAssets();
        builder.WebHost.ConfigureKestrel(k =>
        {
            // Bound to 127.0.0.1 only, whatever host name the app is served under. HTTP/1.1 only (no TLS, so no HTTP/2).
            k.Listen(IPAddress.Loopback, 0, l => l.Protocols = Microsoft.AspNetCore.Server.Kestrel.Core.HttpProtocols.Http1);
            k.AddServerHeader = false;
            k.Limits.MaxRequestBodySize = 1024 * 1024;
            // One user, a few windows: a browser keeps at most six connections per host, plus one socket per
            // Blazor circuit and one per viewer session (R-145).
            k.Limits.MaxConcurrentConnections = MaxConnections;
            k.Limits.MaxConcurrentUpgradedConnections = MaxUpgradedConnections;
        });
        var circuits = new CircuitCounter();
        builder.Services.AddSingleton(circuits);
        builder.Services.AddSingleton<CircuitHandler>(circuits);
        builder.Services.AddSiaaraViewer(runtime, security);
        WebApplication app = builder.Build();
        app.MapSiaaraViewer(o.MapStaticAssets);
        await app.StartAsync().ConfigureAwait(false);
        string address = app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.First();
        security.SetPort(new Uri(address).Port);
        return app;
    }
}
