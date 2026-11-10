// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Runtime.Versioning;
using System.Security.AccessControl;
using System.Security.Principal;

namespace Siaara.Viewer.Core.Platform;

/// <summary>
/// Files and folders that only the current OS user can read or write. The permission is part of
/// the create call (mode 0600 / 0700 on Unix, a protected ACL naming only the current user on
/// Windows), so there is no moment at which another user could open a new file.
/// </summary>
public static class OwnerOnly
{
    private const UnixFileMode FileMode600 = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private const UnixFileMode DirMode700 = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;

    /// <summary>Mode bits that give a group or everybody access.</summary>
    public const UnixFileMode GroupOrOther = UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute
        | UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute;

    /// <summary>Creates a new file (it must not exist) for writing.</summary>
    /// <param name="path">File.</param>
    /// <param name="share">Sharing with other handles.</param>
    /// <returns>The open stream.</returns>
    public static FileStream CreateNew(string path, FileShare share = FileShare.None)
    {
        if (OperatingSystem.IsWindows())
        {
            return CreateWindows(path, FileMode.CreateNew, share);
        }
        return new FileStream(path, new FileStreamOptions
        {
            Mode = FileMode.CreateNew,
            Access = FileAccess.Write,
            Share = share,
            UnixCreateMode = FileMode600,
        });
    }

    /// <summary>
    /// Opens a file for appending, creating it owner-only when missing. An existing file written by an
    /// earlier version with a wider mode is narrowed to 0600 (Unix).
    /// </summary>
    /// <param name="path">File.</param>
    /// <param name="share">Sharing with other handles.</param>
    /// <returns>The open stream, positioned at the end.</returns>
    public static FileStream OpenAppend(string path, FileShare share = FileShare.Read)
    {
        if (OperatingSystem.IsWindows())
        {
            if (!File.Exists(path))
            {
                try
                {
                    return CreateWindows(path, FileMode.CreateNew, share);
                }
                catch (IOException) when (File.Exists(path))
                {
                    // Created by somebody else in between: open it below.
                }
            }
            return new FileStream(path, FileMode.Append, FileAccess.Write, share);
        }
        var fs = new FileStream(path, new FileStreamOptions
        {
            Mode = FileMode.Append,
            Access = FileAccess.Write,
            Share = share,
            UnixCreateMode = FileMode600,
        });
        try
        {
            if ((File.GetUnixFileMode(fs.SafeFileHandle) & GroupOrOther) != 0)
            {
                File.SetUnixFileMode(fs.SafeFileHandle, FileMode600);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Not ours to change; the folder check reports wide access.
        }
        return fs;
    }

    /// <summary>Creates a folder (and its missing parents) for the current user only. An existing folder is left as it is.</summary>
    /// <param name="path">Folder.</param>
    public static void CreateDirectory(string path)
    {
        if (Directory.Exists(path))
        {
            return;
        }
        if (OperatingSystem.IsWindows())
        {
            CreateDirectoryWindows(path);
        }
        else
        {
            // One level at a time: the mode given to Directory.CreateDirectory applies to the last component only.
            string? parent = Path.GetDirectoryName(Path.GetFullPath(path));
            if (!string.IsNullOrEmpty(parent) && !Directory.Exists(parent))
            {
                CreateDirectory(parent);
            }
            Directory.CreateDirectory(path, DirMode700);
        }
    }

    /// <summary>True when the mode of a Unix file or folder gives no access to a group or to others.</summary>
    /// <param name="mode">Mode.</param>
    /// <returns>Owner only.</returns>
    public static bool IsOwnerOnly(UnixFileMode mode) => (mode & GroupOrOther) == 0;

    /// <summary>
    /// Who besides the current user can reach a file or folder, in plain words, or null when nobody can
    /// (or it does not exist). Unix: the group / other mode bits. Windows: allow rules for anybody other
    /// than the user, SYSTEM and the Administrators group.
    /// </summary>
    /// <param name="path">File or folder.</param>
    /// <returns>A description of the wider access, or null.</returns>
    public static string? WiderAccess(string path)
    {
        try
        {
            if (!File.Exists(path) && !Directory.Exists(path))
            {
                return null;
            }
            if (OperatingSystem.IsWindows())
            {
                return WiderAccessWindows(path);
            }
            return DescribeMode(File.GetUnixFileMode(path));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return "its permissions could not be read";
        }
    }

    /// <summary>The wider-access text for a Unix mode (null when owner-only).</summary>
    /// <param name="mode">Mode.</param>
    /// <returns>Text or null.</returns>
    public static string? DescribeMode(UnixFileMode mode)
    {
        bool group = (mode & (UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute)) != 0;
        bool other = (mode & (UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute)) != 0;
        return other ? "every user of this computer can reach it" : group ? "the members of its group can reach it" : null;
    }

    [SupportedOSPlatform("windows")]
    private static FileSecurity FileAclForMe()
    {
        using var me = WindowsIdentity.GetCurrent();
        var security = new FileSecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.SetOwner(me.User!);
        security.AddAccessRule(new FileSystemAccessRule(me.User!, FileSystemRights.FullControl, AccessControlType.Allow));
        return security;
    }

    [SupportedOSPlatform("windows")]
    private static FileStream CreateWindows(string path, FileMode mode, FileShare share) =>
        new FileInfo(path).Create(mode, FileSystemRights.FullControl, share, 4096, FileOptions.None, FileAclForMe());

    [SupportedOSPlatform("windows")]
    private static void CreateDirectoryWindows(string path)
    {
        string? parent = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(parent) && !Directory.Exists(parent))
        {
            CreateDirectoryWindows(parent);
        }
        using var me = WindowsIdentity.GetCurrent();
        var security = new DirectorySecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.SetOwner(me.User!);
        security.AddAccessRule(new FileSystemAccessRule(me.User!, FileSystemRights.FullControl,
            InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        new DirectoryInfo(path).Create(security);
    }

    [SupportedOSPlatform("windows")]
    private static string? WiderAccessWindows(string path)
    {
        using var me = WindowsIdentity.GetCurrent();
        FileSystemSecurity acl = Directory.Exists(path) ? new DirectoryInfo(path).GetAccessControl() : new FileInfo(path).GetAccessControl();
        var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        foreach (FileSystemAccessRule rule in acl.GetAccessRules(true, true, typeof(SecurityIdentifier)))
        {
            if (rule.AccessControlType != AccessControlType.Allow || rule.IdentityReference is not SecurityIdentifier sid)
            {
                continue;
            }
            if (sid == me.User || sid.Equals(system) || sid.Equals(admins))
            {
                continue;
            }
            return "its access list also lets other accounts in";
        }
        return null;
    }
}
