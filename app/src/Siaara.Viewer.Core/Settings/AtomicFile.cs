// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Settings;

/// <summary>Atomic file replacement: write a temporary file in the same folder, flush it to disk, rename over the target.
/// The temporary file is created for the current user only (0600 / owner-only ACL), so the result is too; missing
/// folders are created owner-only.</summary>
public static class AtomicFile
{
    /// <summary>Writes bytes atomically.</summary>
    /// <param name="path">Target.</param>
    /// <param name="data">Content.</param>
    public static void WriteAllBytes(string path, ReadOnlySpan<byte> data)
    {
        string full = Path.GetFullPath(path);
        string dir = Path.GetDirectoryName(full) ?? ".";
        Platform.OwnerOnly.CreateDirectory(dir);
        string tmp = Path.Combine(dir, "." + Path.GetFileName(full) + "." + Guid.NewGuid().ToString("N")[..8] + ".tmp");
        try
        {
            using (FileStream fs = Platform.OwnerOnly.CreateNew(tmp))
            {
                fs.Write(data);
                fs.Flush(flushToDisk: true);
            }
            File.Move(tmp, full, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmp))
            {
                File.Delete(tmp);
            }
        }
    }
}
