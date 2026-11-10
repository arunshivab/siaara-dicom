// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Web.Security;

namespace Siaara.Viewer.Host;

/// <summary>What a second start reads from the instance file: where the running instance listens and one unused launch secret.</summary>
/// <param name="Endpoint">http://host:port/launch.</param>
/// <param name="Secret">Launch secret (single use).</param>
internal sealed record InstanceEntry(Uri Endpoint, string Secret);

/// <summary>
/// Per-user single-instance lock: an exclusively opened lock file while the app runs, plus the
/// instance file with the running instance's address and one unused launch secret, so a second
/// start can bring a window forward instead of opening the store twice. The instance file is
/// created for the current user only in the create call itself (mode 0600 on Unix, a protected ACL
/// naming only the user on Windows), under a temporary name, and renamed into place; a second start
/// refuses a file that others can read. Each secret in it is single-use and is replaced the moment
/// it is used or expires.
/// </summary>
internal sealed class InstanceLock : IDisposable
{
    private readonly FileStream _lock;
    private readonly string _urlFile;
    private readonly Lock _gate = new();
    private LaunchTicket? _published;
    private LaunchSecurity? _security;

    private InstanceLock(FileStream lockStream, string urlFile)
    {
        _lock = lockStream;
        _urlFile = urlFile;
    }

    /// <summary>Takes the lock, or returns null when another instance holds it.</summary>
    /// <param name="lockFile">Lock file.</param>
    /// <param name="urlFile">Instance file.</param>
    /// <returns>The lock or null.</returns>
    public static InstanceLock? TryAcquire(string lockFile, string urlFile)
    {
        try
        {
            // FileShare.None is an exclusive lock on Windows and an exclusive flock() on Unix.
            return new InstanceLock(OwnerOnly.OpenAppend(lockFile, FileShare.None), urlFile);
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>Writes the instance file (owner-only from the create call on, then renamed over the old one).</summary>
    /// <param name="ticket">The address and unused secret for a second start.</param>
    public void Publish(LaunchTicket ticket)
    {
        ArgumentNullException.ThrowIfNull(ticket);
        string dir = Path.GetDirectoryName(Path.GetFullPath(_urlFile)) ?? ".";
        string tmp = Path.Combine(dir, "." + Path.GetFileName(_urlFile) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (FileStream fs = OwnerOnly.CreateNew(tmp))
            {
                fs.Write(new UTF8Encoding(false).GetBytes(ticket.Url.AbsoluteUri));
                fs.Flush(flushToDisk: true);
            }
            File.Move(tmp, _urlFile, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmp))
            {
                File.Delete(tmp);
            }
        }
    }

    /// <summary>
    /// Keeps a fresh, unused launch secret in the instance file: publishes one now and a new one each
    /// time the published one is used (a second start brought a window forward) or withdrawn.
    /// </summary>
    /// <param name="security">The running instance's launch security.</param>
    public void KeepFresh(LaunchSecurity security)
    {
        ArgumentNullException.ThrowIfNull(security);
        _security = security;
        Renew(null);
        security.LaunchUsed += (_, used) => Renew(used.Secret);
    }

    /// <summary>The launch page made from the published secret expired unused: withdraw that secret and publish a new one.</summary>
    /// <param name="fingerprint">Fingerprint from the page's file name.</param>
    public void RenewIfFingerprint(string fingerprint)
    {
        LaunchTicket? current;
        lock (_gate)
        {
            current = _published;
        }
        if (current is not null && LaunchPages.Fingerprint(current.Secret) == fingerprint)
        {
            _security?.Revoke(current);
            Renew(current.Secret);
        }
    }

    // Publishes a new ticket when none is published yet (secret null) or the given secret is the published one.
    private void Renew(string? secret)
    {
        lock (_gate)
        {
            if (_security is null || secret is not null && _published?.Secret != secret)
            {
                return;
            }
            try
            {
                _published = _security.IssueLaunch();
                Publish(_published);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                AppLog.Error("host", "the instance file could not be renewed", e);
            }
        }
    }

    /// <summary>
    /// Reads the instance file of the running instance. Null when it is missing, malformed, not an
    /// address of this computer's loopback interface, or readable by anybody but the current user.
    /// </summary>
    /// <param name="urlFile">Instance file.</param>
    /// <returns>The entry or null.</returns>
    public static InstanceEntry? Read(string urlFile)
    {
        try
        {
            if (!File.Exists(urlFile) || OwnerOnly.WiderAccess(urlFile) is not null)
            {
                return null;
            }
            return Parse(File.ReadAllText(urlFile));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>Parses the instance file's text: http://HOST:PORT/launch?k=SECRET with HOST 127.0.0.1 or a *.localhost name.</summary>
    /// <param name="text">File content.</param>
    /// <returns>The entry or null.</returns>
    public static InstanceEntry? Parse(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        if (!Uri.TryCreate(text.Trim(), UriKind.Absolute, out Uri? u) || u.Scheme != Uri.UriSchemeHttp || u.AbsolutePath != "/launch"
            || !(u.Host == "127.0.0.1" || u.Host.EndsWith(".localhost", StringComparison.Ordinal)) || !u.Query.StartsWith("?k=", StringComparison.Ordinal))
        {
            return null;
        }
        string secret = u.Query[3..];
        return secret.Length == 64 && secret.All(char.IsAsciiHexDigit) ? new InstanceEntry(new Uri(u.GetLeftPart(UriPartial.Path)), secret) : null;
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        try
        {
            File.Delete(_urlFile);
        }
        catch (IOException)
        {
            // Best effort.
        }
        _lock.Dispose();
    }
}
