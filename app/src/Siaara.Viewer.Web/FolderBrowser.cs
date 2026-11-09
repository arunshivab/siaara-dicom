// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Web;

/// <summary>Server-side folder picker for the import sheet (a browser cannot give a local path).</summary>
public static class FolderBrowser
{
    /// <summary>Starting points: the drives on Windows, "/" and the home folder elsewhere.</summary>
    /// <returns>Paths.</returns>
    public static IReadOnlyList<string> Roots()
    {
        var list = new List<string> { Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) };
        if (OperatingSystem.IsWindows())
        {
            list.AddRange(DriveInfo.GetDrives().Where(d => d.IsReady).Select(d => d.RootDirectory.FullName));
        }
        else
        {
            list.Add("/");
            if (Directory.Exists("/media"))
            {
                list.Add("/media");
            }
            if (Directory.Exists("/Volumes"))
            {
                list.Add("/Volumes");
            }
        }
        return list.Where(p => p.Length > 0).Distinct(StringComparer.Ordinal).ToArray();
    }

    /// <summary>Sub-folders of a folder (unreadable folders give an empty list), sorted by name.</summary>
    /// <param name="path">Folder.</param>
    /// <returns>Full paths.</returns>
    public static IReadOnlyList<string> SubFolders(string path)
    {
        try
        {
            return Directory.EnumerateDirectories(path)
                .Where(d => !Path.GetFileName(d).StartsWith('.'))
                .Order(StringComparer.OrdinalIgnoreCase)
                .Take(2000)
                .ToArray();
        }
        catch (IOException)
        {
            return [];
        }
        catch (UnauthorizedAccessException)
        {
            return [];
        }
    }

    /// <summary>Parent folder, or null at a root.</summary>
    /// <param name="path">Folder.</param>
    /// <returns>Parent.</returns>
    public static string? Parent(string path) => Directory.GetParent(path)?.FullName;
}
