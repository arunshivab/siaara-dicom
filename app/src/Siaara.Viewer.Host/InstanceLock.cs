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

    /// <summary>Publishes the launch URL for a second start.</summary>
    /// <param name="url">URL.</param>
    public void PublishUrl(Uri url)
    {
        File.WriteAllText(_urlFile, url.AbsoluteUri, new UTF8Encoding(false));
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(_urlFile, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
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
