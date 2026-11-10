// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text;

namespace Siaara.Viewer.Host;

/// <summary>
/// Per-user single-instance lock: an exclusively opened lock file while the app runs, plus a file
/// with the running instance's launch URL (owner-only permissions) so a second start can bring the
/// existing window forward instead of opening the store twice.
/// </summary>
internal sealed class InstanceLock : IDisposable
{
    private readonly FileStream _lock;
    private readonly string _urlFile;
    private readonly Lock _gate = new();
    private Uri? _published;

    private InstanceLock(FileStream lockStream, string urlFile)
    {
        _lock = lockStream;
        _urlFile = urlFile;
    }

    /// <summary>Takes the lock, or returns null when another instance holds it.</summary>
    /// <param name="lockFile">Lock file.</param>
    /// <param name="urlFile">URL file.</param>
    /// <returns>The lock or null.</returns>
    public static InstanceLock? TryAcquire(string lockFile, string urlFile)
    {
        try
        {
            var fs = new FileStream(lockFile, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            // FileShare.None is an exclusive lock on Windows and an exclusive flock() on Unix.
            return new InstanceLock(fs, urlFile);
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

    /// <summary>
    /// Writes the launch URL for a second start. The file is created owner-only BEFORE anything is
    /// written to it (mode 0600 on Unix, a protected ACL granting only the current user on Windows),
    /// under a temporary name, then renamed over the old file, so no other user can ever read a
    /// secret from it. The URL is single-use; the host writes a fresh one after each use.
    /// </summary>
    /// <param name="url">URL.</param>
    public void PublishUrl(Uri url)
    {
        ArgumentNullException.ThrowIfNull(url);
        string dir = Path.GetDirectoryName(Path.GetFullPath(_urlFile)) ?? ".";
        string tmp = Path.Combine(dir, "." + Path.GetFileName(_urlFile) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (FileStream fs = CreateOwnerOnly(tmp))
            {
                fs.Write(new UTF8Encoding(false).GetBytes(url.AbsoluteUri));
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
    /// Keeps a fresh, unused launch URL in the instance file: publishes one now and a new one each
    /// time the published one is used (the running instance is brought forward by a second start).
    /// </summary>
    /// <param name="security">The running instance's launch security.</param>
    public void KeepFresh(Web.Security.LaunchSecurity security)
    {
        ArgumentNullException.ThrowIfNull(security);
        lock (_gate)
        {
            _published = security.IssueLaunchUrl();
            PublishUrl(_published);
        }
        security.LaunchUrlUsed += (_, used) =>
        {
            lock (_gate)
            {
                if (_published is not null && used.Url == _published)
                {
                    try
                    {
                        _published = security.IssueLaunchUrl();
                        PublishUrl(_published);
                    }
                    catch (IOException e)
                    {
                        Core.Logging.AppLog.Error("host", "the instance file could not be renewed", e);
                    }
                    catch (UnauthorizedAccessException e)
                    {
                        Core.Logging.AppLog.Error("host", "the instance file could not be renewed", e);
                    }
                }
            }
        };
    }

    /// <summary>Creates a new file readable and writable by the current user only (permissions set at creation).</summary>
    /// <param name="path">File (must not exist).</param>
    /// <returns>The open stream.</returns>
    internal static FileStream CreateOwnerOnly(string path)
    {
        if (OperatingSystem.IsWindows())
        {
            return CreateOwnerOnlyWindows(path);
        }
        return new FileStream(path, new FileStreamOptions
        {
            Mode = FileMode.CreateNew,
            Access = FileAccess.Write,
            Share = FileShare.None,
            UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite,
        });
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    private static FileStream CreateOwnerOnlyWindows(string path)
    {
        using var me = System.Security.Principal.WindowsIdentity.GetCurrent();
        var security = new System.Security.AccessControl.FileSecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.SetOwner(me.User!);
        security.AddAccessRule(new System.Security.AccessControl.FileSystemAccessRule(me.User!,
            System.Security.AccessControl.FileSystemRights.FullControl, System.Security.AccessControl.AccessControlType.Allow));
        return System.IO.FileSystemAclExtensions.Create(new FileInfo(path), FileMode.CreateNew, System.Security.AccessControl.FileSystemRights.FullControl,
            FileShare.None, 4096, FileOptions.None, security);
    }

    /// <summary>The URL of the running instance, or null.</summary>
    /// <param name="urlFile">URL file.</param>
    /// <returns>URL.</returns>
    public static Uri? ReadUrl(string urlFile)
    {
        try
        {
            return Uri.TryCreate(File.ReadAllText(urlFile).Trim(), UriKind.Absolute, out Uri? u) && u.IsLoopback ? u : null;
        }
        catch (IOException)
        {
            return null;
        }
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
