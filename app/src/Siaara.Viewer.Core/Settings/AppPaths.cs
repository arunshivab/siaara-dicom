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

    /// <summary>Address and an unused launch secret of the running instance, for a second start (owner-only).</summary>
    public string InstanceLaunchFile => Path.Combine(BaseFolder, "instance.url");

    /// <summary>Folder of the one-shot launch pages the browser is opened on (owner-only; each page is deleted after use).</summary>
    public string LaunchFolder => Path.Combine(BaseFolder, "launch");

    /// <summary>Creates the base folder and the app's own sub-folders for the current user only (0700 / owner-only ACL).
    /// Existing folders keep their permissions; <see cref="AccessWarning"/> reports them when they are wider.</summary>
    public void EnsureCreated()
    {
        Platform.OwnerOnly.CreateDirectory(BaseFolder);
        Platform.OwnerOnly.CreateDirectory(LogFolder);
        Platform.OwnerOnly.CreateDirectory(Path.GetDirectoryName(AuditFile)!);
        Platform.OwnerOnly.CreateDirectory(AnnotationFolder);
        Platform.OwnerOnly.CreateDirectory(LaunchFolder);
    }

    /// <summary>
    /// A warning in plain words when the data folder can be reached by other users (group or world
    /// mode bits on Unix, other accounts in the access list on Windows), or null. The folder holds
    /// patient images, the audit trail and the annotations; the app does not change the permissions of
    /// a folder it did not create.
    /// </summary>
    /// <returns>The warning or null.</returns>
    public string? AccessWarning()
    {
        string? wide = Platform.OwnerOnly.WiderAccess(BaseFolder);
        return wide is null ? null
            : "The SIAARA data folder is not private: " + wide + ". Patient data in it can be read by others. Restrict the folder to your own account.";
    }
}
