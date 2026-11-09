// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using Siaara.Viewer.Core.Logging;

namespace Siaara.Viewer.Host;

/// <summary>
/// Decision A1, option (a): open the window in the installed browser. Edge or Chrome / Chromium
/// with --app=URL when found (a window without browser chrome), else the default browser. This is
/// the only class that knows how the window is opened, so option (b), a native web-view shell per
/// OS, can replace it without touching anything else.
/// </summary>
internal static class BrowserLauncher
{
    /// <summary>Opens the URL. Returns false when no browser could be started (the URL is then printed).</summary>
    /// <param name="url">Launch URL (contains the launch secret: never logged).</param>
    /// <returns>True when a browser was started.</returns>
    public static bool Open(Uri url)
    {
        string? app = FindAppBrowser();
        if (app is not null && TryStart(app, "--app=" + url.AbsoluteUri, "--new-window"))
        {
            AppLog.Info("host", "window opened with an app-mode browser");
            return true;
        }
        bool ok = OperatingSystem.IsWindows() ? TryShell(url.AbsoluteUri)
            : OperatingSystem.IsMacOS() ? TryStart("open", url.AbsoluteUri)
            : TryStart("xdg-open", url.AbsoluteUri);
        AppLog.Info("host", ok ? "window opened with the default browser" : "no browser could be started");
        return ok;
    }

    private static string? FindAppBrowser()
    {
        var candidates = new List<string>();
        if (OperatingSystem.IsWindows())
        {
            foreach (string? root in new[]
            {
                Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            })
            {
                if (string.IsNullOrEmpty(root))
                {
                    continue;
                }
                candidates.Add(Path.Combine(root, "Microsoft", "Edge", "Application", "msedge.exe"));
                candidates.Add(Path.Combine(root, "Google", "Chrome", "Application", "chrome.exe"));
            }
        }
        else if (OperatingSystem.IsMacOS())
        {
            candidates.Add("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge");
            candidates.Add("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
            candidates.Add("/Applications/Chromium.app/Contents/MacOS/Chromium");
        }
        else
        {
            foreach (string dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':', StringSplitOptions.RemoveEmptyEntries))
            {
                foreach (string name in new[] { "microsoft-edge", "microsoft-edge-stable", "google-chrome", "google-chrome-stable", "chromium", "chromium-browser" })
                {
                    candidates.Add(Path.Combine(dir, name));
                }
            }
        }
        return candidates.FirstOrDefault(File.Exists);
    }

    private static bool TryStart(string file, params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo(file) { UseShellExecute = false, RedirectStandardOutput = false, RedirectStandardError = false };
            foreach (string a in args)
            {
                psi.ArgumentList.Add(a);
            }
            using Process? p = Process.Start(psi);
            return p is not null;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private static bool TryShell(string url)
    {
        try
        {
            using Process? p = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            return p is not null;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }
}
