// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Settings;

/// <summary>Per-user folders of the app (settings, store, logs, audit, annotations, instance lock).</summary>
public sealed class AppPaths
{
    /// <summary>Uses a base folder.</summary>
    /// <param name="baseFolder">Base folder.</param>
    public AppPaths(string baseFolder)
    {
        BaseFolder = Path.GetFullPath(baseFolder);
    }

    /// <summary>The per-user default: %LOCALAPPDATA%\Siaara, ~/Library/Application Support/Siaara or $XDG_DATA_HOME/siaara.</summary>
    /// <returns>The paths.</returns>
    public static AppPaths ForCurrentUser()
    {
        if (OperatingSystem.IsWindows())
        {
            return new AppPaths(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Siaara"));
        }
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (OperatingSystem.IsMacOS())
        {
            return new AppPaths(Path.Combine(home, "Library", "Application Support", "Siaara"));
        }
        string? xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
        string root = string.IsNullOrEmpty(xdg) || !Path.IsPathRooted(xdg) ? Path.Combine(home, ".local", "share") : xdg;
        return new AppPaths(Path.Combine(root, "siaara"));
    }

    /// <summary>Base folder.</summary>
    public string BaseFolder { get; }

    /// <summary>Default store root.</summary>
    public string DefaultStoreRoot => Path.Combine(BaseFolder, "store");

    /// <summary>settings.json.</summary>
    public string SettingsFile => Path.Combine(BaseFolder, "settings.json");

    /// <summary>Log folder.</summary>
    public string LogFolder => Path.Combine(BaseFolder, "logs");

    /// <summary>Audit log file.</summary>
    public string AuditFile => Path.Combine(BaseFolder, "audit", "audit.jsonl");

    /// <summary>Annotation folder.</summary>
    public string AnnotationFolder => Path.Combine(BaseFolder, "annotations");

    /// <summary>Single-instance lock file (held open while running).</summary>
    public string InstanceLockFile => Path.Combine(BaseFolder, "instance.lock");

    /// <summary>Launch URL of the running instance (owner-only permissions).</summary>
    public string InstanceLaunchFile => Path.Combine(BaseFolder, "instance.url");

    /// <summary>Creates the base folder (owner-only on Unix).</summary>
    public void EnsureCreated()
    {
        if (OperatingSystem.IsWindows())
        {
            Directory.CreateDirectory(BaseFolder);
        }
        else
        {
            Directory.CreateDirectory(BaseFolder, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        }
    }
}
