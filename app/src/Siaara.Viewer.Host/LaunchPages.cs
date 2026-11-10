// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Net;
using System.Security.Cryptography;
using System.Text;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Web.Security;

namespace Siaara.Viewer.Host;

/// <summary>
/// One-shot launch pages (R-113). The launch secret never appears on a command line: the host writes
/// a small HTML file that only the current user can read (0600 / owner-only ACL, in the owner-only
/// "launch" folder of the per-user data folder) and opens the browser on that FILE. The page posts
/// the secret to /launch (a form submitted by one line of script, with a button for browsers that
/// run no script), so the secret is not in an address or in the browser's history either. The file
/// is named after a hash of the secret and is deleted when the secret is used, when it is older
/// than <see cref="Lifetime"/>, when the app stops and when the next start finds it.
/// </summary>
internal sealed class LaunchPages : IDisposable
{
    /// <summary>How long a launch page (and the secret in it) may stay unused.</summary>
    public static readonly TimeSpan Lifetime = TimeSpan.FromMinutes(5);

    private readonly string _folder;
    private Timer? _sweep;

    public LaunchPages(string folder)
    {
        _folder = folder;
        OwnerOnly.CreateDirectory(folder);
    }

    /// <summary>Raised for each expired page that was deleted, with the fingerprint in its name (the secret is revoked by it).</summary>
    public event Action<string>? Expired;

    /// <summary>The name part that identifies a secret without revealing it: the first 32 hex digits of its SHA-256.</summary>
    /// <param name="secretHex">Launch secret.</param>
    /// <returns>Fingerprint.</returns>
    public static string Fingerprint(string secretHex)
    {
        ArgumentNullException.ThrowIfNull(secretHex);
        byte[] bytes;
        try
        {
            bytes = Convert.FromHexString(secretHex);
        }
        catch (FormatException)
        {
            bytes = Encoding.UTF8.GetBytes(secretHex);
        }
        return Convert.ToHexStringLower(SHA256.HashData(bytes))[..32];
    }

    private string PathOf(string fingerprint) => Path.Combine(_folder, "launch-" + fingerprint + ".html");

    /// <summary>The page's HTML.</summary>
    /// <param name="endpoint">http://host:port/launch.</param>
    /// <param name="secretHex">Launch secret.</param>
    /// <returns>HTML.</returns>
    public static string Html(Uri endpoint, string secretHex)
    {
        ArgumentNullException.ThrowIfNull(endpoint);
        return "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"referrer\" content=\"no-referrer\"><title>SIAARA Viewer</title></head>\n" +
            "<body style=\"font-family:sans-serif;margin:3em\">\n<form id=\"f\" method=\"post\" action=\"" + WebUtility.HtmlEncode(endpoint.AbsoluteUri) + "\">" +
            "<input type=\"hidden\" name=\"k\" value=\"" + WebUtility.HtmlEncode(secretHex) + "\">" +
            "<p>Opening SIAARA Viewer...</p><button type=\"submit\">Open SIAARA Viewer</button></form>\n" +
            "<script>document.getElementById('f').submit();</script>\n</body></html>\n";
    }

    /// <summary>Writes the page for a ticket, owner-only from the create call on.</summary>
    /// <param name="endpoint">http://host:port/launch.</param>
    /// <param name="secretHex">Launch secret.</param>
    /// <returns>The file.</returns>
    public string Write(Uri endpoint, string secretHex)
    {
        string path = PathOf(Fingerprint(secretHex));
        File.Delete(path);
        using (FileStream fs = OwnerOnly.CreateNew(path))
        {
            fs.Write(new UTF8Encoding(false).GetBytes(Html(endpoint, secretHex)));
            fs.Flush(flushToDisk: true);
        }
        return path;
    }

    /// <summary>Deletes the page of a secret (it was used).</summary>
    /// <param name="secretHex">Launch secret.</param>
    public void Delete(string secretHex) => TryDelete(PathOf(Fingerprint(secretHex)));

    /// <summary>Deletes pages older than <see cref="Lifetime"/> (all of them with <paramref name="all"/>).</summary>
    /// <param name="all">Delete every page.</param>
    /// <param name="nowUtc">The time (tests); null = now.</param>
    /// <returns>Pages deleted.</returns>
    public int Sweep(bool all = false, DateTime? nowUtc = null)
    {
        int n = 0;
        DateTime now = nowUtc ?? DateTime.UtcNow;
        try
        {
            foreach (string f in Directory.GetFiles(_folder, "launch-*.html"))
            {
                if (all || now - File.GetLastWriteTimeUtc(f) > Lifetime)
                {
                    string name = Path.GetFileNameWithoutExtension(f);
                    if (TryDelete(f))
                    {
                        n++;
                        if (!all)
                        {
                            Expired?.Invoke(name["launch-".Length..]);
                        }
                    }
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            AppLog.Error("host", "launch pages could not be swept", e);
        }
        return n;
    }

    /// <summary>Deletes what an earlier run left behind and starts the periodic sweep (the running instance only).</summary>
    public void StartSweeping()
    {
        Sweep(all: true);
        _sweep = new Timer(_ => Sweep(), null, TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(30));
    }

    private static bool TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
                return true;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            AppLog.Error("host", "a launch page could not be deleted", e);
        }
        return false;
    }

    /// <summary>Stops sweeping and, for the running instance, deletes every page.</summary>
    public void Dispose()
    {
        if (_sweep is not null)
        {
            _sweep.Dispose();
            Sweep(all: true);
        }
    }
}
