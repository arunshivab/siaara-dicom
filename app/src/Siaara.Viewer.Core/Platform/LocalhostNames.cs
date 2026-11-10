// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Runtime.InteropServices;

namespace Siaara.Viewer.Core.Platform;

/// <summary>
/// Whether the operating system's own resolver knows names under .localhost (RFC 6761). Chromium
/// browsers (Edge, Chrome) and Firefox resolve every *.localhost name to the loopback interface
/// themselves; a browser that asks the system (Safari, GNOME Web) only reaches the app when the
/// system resolver does too. .NET's own Dns class is not asked, because it answers such names itself.
/// </summary>
public static partial class LocalhostNames
{
    [LibraryImport("libc", StringMarshalling = StringMarshalling.Utf8)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int getaddrinfo(string node, string? service, nint hints, out nint result);

    [LibraryImport("libc")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial void freeaddrinfo(nint result);

    /// <summary>Asks getaddrinfo (Unix) for a name. On Windows the answer is always true: the browsers found there resolve the names themselves.</summary>
    /// <param name="hostName">For example "0123abcd.localhost".</param>
    /// <returns>True when the system resolver gives an address.</returns>
    public static bool SystemResolves(string hostName)
    {
        if (OperatingSystem.IsWindows())
        {
            return true;
        }
        try
        {
            if (getaddrinfo(hostName, null, 0, out nint list) != 0)
            {
                return false;
            }
            freeaddrinfo(list);
            return true;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
    }

    /// <summary>
    /// Which host name the app is served under. The random *.localhost name is the rule; 127.0.0.1 is the
    /// fallback (its cookie is visible to every 127.0.0.1 port) and is chosen only when asked for, or when
    /// the window will be opened by an unknown default browser on a system whose resolver does not know
    /// *.localhost names (that browser could not reach the app at all).
    /// </summary>
    /// <param name="requested">"ip", "name" or "auto".</param>
    /// <param name="opensBrowser">The host is going to open a browser window.</param>
    /// <param name="appBrowserFound">An Edge / Chrome / Chromium app-mode browser was found (it resolves the names itself).</param>
    /// <param name="systemResolves">Asks the system resolver (called only when needed).</param>
    /// <returns>True for the 127.0.0.1 fallback.</returns>
    public static bool UseIpFallback(string requested, bool opensBrowser, bool appBrowserFound, Func<bool> systemResolves)
    {
        ArgumentNullException.ThrowIfNull(systemResolves);
        return requested switch
        {
            "ip" => true,
            "name" => false,
            _ => opensBrowser && !appBrowserFound && !systemResolves(),
        };
    }
}
