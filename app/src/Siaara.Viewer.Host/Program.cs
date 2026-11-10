// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Host;

/// <summary>
/// siaara-viewer [--data-dir DIR] [--store DIR] [--import DIR] [--no-browser] [--print-url] [--print-launch-file] [--host auto|name|ip] [--no-idle-exit] [--verbose]
/// siaara-viewer --generate-samples DIR [--full]
/// </summary>
internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        string? dataDir = null, store = null, samples = null, import = null;
        string hostMode = "auto";
        bool noBrowser = false, printUrl = false, printFile = false, noIdle = false, verbose = false, full = false;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--data-dir" when i + 1 < args.Length: dataDir = args[++i]; break;
                case "--store" when i + 1 < args.Length: store = args[++i]; break;
                case "--generate-samples" when i + 1 < args.Length: samples = args[++i]; break;
                case "--import" when i + 1 < args.Length: import = args[++i]; break;
                case "--host" when i + 1 < args.Length && args[i + 1] is "auto" or "name" or "ip": hostMode = args[++i]; break;
                case "--full": full = true; break;
                case "--no-browser": noBrowser = true; break;
                case "--print-url": printUrl = true; break;
                case "--print-launch-file": printFile = true; break;
                case "--no-idle-exit": noIdle = true; break;
                case "--verbose": verbose = true; break;
                default:
                    await Console.Error.WriteLineAsync("unknown argument: " + args[i]).ConfigureAwait(false);
                    return 64;
            }
        }

        // 1. The native toolkit must have the ABI this build was compiled against.
        try
        {
            if (!Tk.AbiMatches)
            {
                await Console.Error.WriteLineAsync(string.Create(CultureInfo.InvariantCulture,
                    $"SIAARA Viewer: toolkit version mismatch (library ABI {Tk.AbiVersion}, expected {Tk.ExpectedAbiVersion}).")).ConfigureAwait(false);
                return 2;
            }
        }
        catch (DllNotFoundException e)
        {
            await Console.Error.WriteLineAsync("SIAARA Viewer: the toolkit library was not found. " + e.Message).ConfigureAwait(false);
            return 2;
        }

        if (samples is not null)
        {
            IReadOnlyList<string> files = SampleData.WriteDemoSet(samples, full);
            await Console.Out.WriteLineAsync(string.Create(CultureInfo.InvariantCulture,
                $"Wrote {files.Count} synthetic files to {Path.Combine(samples, "folder")} and a DICOMDIR set to {Path.Combine(samples, "dicomdir-set")}.")).ConfigureAwait(false);
            return 0;
        }

        if (Environment.GetEnvironmentVariable("DOTNET_DbgEnableMiniDump") == "1")
        {
            // Crash dumps would hold patient data from memory: the app never enables them.
            await Console.Error.WriteLineAsync("SIAARA Viewer: crash dumps are enabled in the environment (DOTNET_DbgEnableMiniDump); unset it.").ConfigureAwait(false);
            return 2;
        }

        AppPaths paths = dataDir is null ? AppPaths.ForCurrentUser() : new AppPaths(dataDir);
        paths.EnsureCreated();

        // 2. One instance per user: a second start brings the running window forward.
        using InstanceLock? instance = InstanceLock.TryAcquire(paths.InstanceLockFile, paths.InstanceLaunchFile);
        if (instance is null)
        {
            return await BringForwardAsync(paths, noBrowser).ConfigureAwait(false);
        }

        // The app is served under a random *.localhost name (host-only cookie). 127.0.0.1 only on request, or when
        // an unknown default browser would have to ask a system resolver that does not know such names.
        bool ipHost = Core.Platform.LocalhostNames.UseIpFallback(hostMode, opensBrowser: !noBrowser, appBrowserFound: BrowserLauncher.FindAppBrowser() is not null,
            () => Core.Platform.LocalhostNames.SystemResolves("siaara-probe.localhost"));
        RunningViewer? viewer = await TryStartAsync(new HostOptions
        {
            DataDir = dataDir,
            StoreRoot = store,
            IdleExit = noIdle ? null : TimeSpan.FromSeconds(60),
            Verbose = verbose,
            IpHost = ipHost,
        }).ConfigureAwait(false);
        if (viewer is null)
        {
            return await BringForwardAsync(paths, noBrowser).ConfigureAwait(false);
        }
        await using (viewer.ConfigureAwait(false))
        {
            // Single-use launch secrets: one for this start (valid 5 minutes), and a separate one in the
            // owner-only instance file for a second start, renewed every time it is used. Neither ever
            // appears on a command line: the browser is opened on an owner-only one-shot page file.
            using var pages = new LaunchPages(paths.LaunchFolder);
            pages.StartSweeping();
            viewer.Security.LaunchUsed += (_, used) => pages.Delete(used.Secret);
            pages.Expired += instance.RenewIfFingerprint;
            instance.KeepFresh(viewer.Security);
            if (import is not null)
            {
                // Same path as the import sheet (scan or DICOMDIR, copy into the store, audited).
                Core.Import.ImportJob job = viewer.Runtime.Import.Start(new Core.Import.ImportRequest(import));
                await job.Completion.ConfigureAwait(false);
                Core.Import.ImportProgress p = job.Progress;
                await Console.Out.WriteLineAsync(string.Create(CultureInfo.InvariantCulture,
                    $"import: {p.Added} added, {p.Duplicates} duplicates, {p.Skipped} skipped, {p.Errors} errors")).ConfigureAwait(false);
            }
            Web.Security.LaunchTicket launch = viewer.Security.IssueLaunch(LaunchPages.Lifetime);
            if (printUrl)
            {
                // Asked for: the address (with its single-use secret) goes to standard output, and whoever can
                // read that output within 5 minutes can open the viewer once. See README, "Launch and session".
                await Console.Out.WriteLineAsync(launch.Url.AbsoluteUri).ConfigureAwait(false);
            }
            if (printFile)
            {
                // The path of an owner-only one-shot page (no secret on standard output): open it in a browser on this
                // computer within 5 minutes. For scripts and for starting the window by other means.
                Web.Security.LaunchTicket forFile = viewer.Security.IssueLaunch(LaunchPages.Lifetime);
                await Console.Out.WriteLineAsync(pages.Write(forFile.Endpoint, forFile.Secret)).ConfigureAwait(false);
            }
            if (!noBrowser && !BrowserLauncher.Open(pages.Write(launch.Endpoint, launch.Secret)))
            {
                pages.Delete(launch.Secret);
                if (!printUrl)
                {
                    // Last resort, same exposure as --print-url (documented).
                    await Console.Out.WriteLineAsync("No browser could be started. Open this address in a browser on this computer within 5 minutes:").ConfigureAwait(false);
                    await Console.Out.WriteLineAsync(launch.Url.AbsoluteUri).ConfigureAwait(false);
                }
            }
            await viewer.App.WaitForShutdownAsync().ConfigureAwait(false);
        }
        AppLog.Close();
        return 0;
    }

    private static async Task<RunningViewer?> TryStartAsync(HostOptions o)
    {
        try
        {
            return await ViewerHost.StartAsync(o).ConfigureAwait(false);
        }
        catch (StoreLockedException)
        {
            return null;
        }
    }

    private static async Task<int> BringForwardAsync(AppPaths paths, bool noBrowser)
    {
        InstanceEntry? running = InstanceLock.Read(paths.InstanceLaunchFile);
        if (running is null)
        {
            await Console.Error.WriteLineAsync("SIAARA Viewer is already running (or its store is in use by another program).").ConfigureAwait(false);
            return 3;
        }
        if (!noBrowser)
        {
            // The same one-shot page as a first start; the running instance deletes it when the secret is used or
            // after 5 minutes (and then withdraws the secret).
            using var pages = new LaunchPages(paths.LaunchFolder);
            BrowserLauncher.Open(pages.Write(running.Endpoint, running.Secret));
        }
        await Console.Out.WriteLineAsync("SIAARA Viewer is already running; its window was brought forward.").ConfigureAwait(false);
        return 0;
    }
}
