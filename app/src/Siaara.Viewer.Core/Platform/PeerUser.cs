// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Buffers.Binary;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Siaara.Viewer.Core.Platform;

/// <summary>Who owns the process at the other end of a loopback connection.</summary>
public enum PeerOwner
{
    /// <summary>The same OS user as this process.</summary>
    SameUser,

    /// <summary>Another OS user.</summary>
    OtherUser,

    /// <summary>It could not be determined (the connection was not found, the OS refused the question, unsupported OS).</summary>
    Unknown,
}

/// <summary>Finds the OS user at the other end of a loopback TCP connection.</summary>
public interface IPeerUserCheck
{
    /// <summary>The owner of the peer of an accepted connection.</summary>
    /// <param name="local">Our end (the listener's address and port).</param>
    /// <param name="remote">The peer's end.</param>
    /// <returns>The owner.</returns>
    PeerOwner Check(IPEndPoint? local, IPEndPoint? remote);

    /// <summary>How this OS answers, for the log.</summary>
    string Method { get; }
}

/// <summary>
/// The one rule for loopback peers (R-113): a connection is served only when the process at the
/// other end is known to belong to the same OS user. Another user and "could not be determined"
/// are both refused (fail closed), for every path: /launch, pages, the Blazor circuit and /ws/view.
/// </summary>
public static class PeerPolicy
{
    /// <summary>May a connection whose peer has this owner be served?</summary>
    /// <param name="owner">Owner of the peer process.</param>
    /// <returns>True only for the same user.</returns>
    public static bool Allows(PeerOwner owner) => owner == PeerOwner.SameUser;

    /// <summary>The audit "kind" of a refused peer.</summary>
    /// <param name="owner">Owner.</param>
    /// <returns>peer-other-user or peer-unknown.</returns>
    public static string RefusalKind(PeerOwner owner) => owner == PeerOwner.OtherUser ? "peer-other-user" : "peer-unknown";
}

/// <summary>The peer-user check of the running OS.</summary>
public static class PeerUser
{
    /// <summary>
    /// Linux: /proc/net/tcp and tcp6 (uid of the peer's socket). Windows: GetExtendedTcpTable, then the
    /// user SID of the owning process's token. macOS: libproc over the current user's own processes.
    /// Any other OS: always Unknown (refused).
    /// </summary>
    /// <returns>The check.</returns>
    public static IPeerUserCheck ForThisOs() =>
        OperatingSystem.IsLinux() ? new LinuxPeerCheck()
        : OperatingSystem.IsWindows() ? new WindowsPeerCheck()
        : OperatingSystem.IsMacOS() ? new MacPeerCheck()
        : new UnsupportedPeerCheck();

    internal static bool IsLoopback(IPAddress a) => IPAddress.IsLoopback(a.IsIPv4MappedToIPv6 ? a.MapToIPv4() : a);

    private sealed class UnsupportedPeerCheck : IPeerUserCheck
    {
        public string Method => "none (unsupported OS: every connection is refused)";

        public PeerOwner Check(IPEndPoint? local, IPEndPoint? remote) => PeerOwner.Unknown;
    }
}

/// <summary>
/// Linux: the kernel lists every TCP socket of the network namespace in /proc/net/tcp (and tcp6)
/// with the uid of the socket's owner. The peer's socket is the line whose local port is the
/// connection's remote port and whose remote port is our listening port, both addresses loopback.
/// </summary>
[SupportedOSPlatform("linux")]
internal sealed partial class LinuxPeerCheck : IPeerUserCheck
{
    [LibraryImport("libc")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial uint geteuid();

    public string Method => "/proc/net/tcp socket owner uid";

    public PeerOwner Check(IPEndPoint? local, IPEndPoint? remote)
    {
        if (local is null || remote is null || !PeerUser.IsLoopback(remote.Address))
        {
            return PeerOwner.Unknown;
        }
        try
        {
            var lines = new List<string>(File.ReadLines("/proc/net/tcp"));
            if (File.Exists("/proc/net/tcp6"))
            {
                lines.AddRange(File.ReadLines("/proc/net/tcp6"));
            }
            return LinuxProcNet.Decide(lines, local.Port, remote.Port, geteuid());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return PeerOwner.Unknown;
        }
    }
}

/// <summary>Parsing of /proc/net/tcp and /proc/net/tcp6 (pure; tested on every OS).</summary>
internal static class LinuxProcNet
{
    /// <summary>One socket line.</summary>
    internal readonly record struct SocketLine(IPAddress Local, int LocalPort, IPAddress Remote, int RemotePort, uint Uid);

    /// <summary>The owner of the socket connected from <paramref name="clientPort"/> to our <paramref name="serverPort"/> on loopback.</summary>
    public static PeerOwner Decide(IEnumerable<string> lines, int serverPort, int clientPort, uint myUid)
    {
        bool found = false, allMine = true;
        foreach (string line in lines)
        {
            if (!TryParse(line, out SocketLine s) || s.LocalPort != clientPort || s.RemotePort != serverPort
                || !PeerUser.IsLoopback(s.Local) || !PeerUser.IsLoopback(s.Remote))
            {
                continue;
            }
            found = true;
            allMine &= s.Uid == myUid;
        }
        return !found ? PeerOwner.Unknown : allMine ? PeerOwner.SameUser : PeerOwner.OtherUser;
    }

    /// <summary>Parses "  12: 0100007F:1F90 0100007F:C350 01 ... uid timeout inode ..." (addresses are 8 or 32 hex digits).</summary>
    public static bool TryParse(string line, out SocketLine socket)
    {
        socket = default;
        string[] f = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        // sl, local, remote, st, tx:rx, tr:when, retrnsmt, uid, ...
        if (f.Length < 8 || !f[0].EndsWith(':') || !TryEndPoint(f[1], out IPAddress? la, out int lp) || !TryEndPoint(f[2], out IPAddress? ra, out int rp)
            || !uint.TryParse(f[7], NumberStyles.None, CultureInfo.InvariantCulture, out uint uid))
        {
            return false;
        }
        socket = new SocketLine(la!, lp, ra!, rp, uid);
        return true;
    }

    private static bool TryEndPoint(string text, out IPAddress? address, out int port)
    {
        address = null;
        port = 0;
        int colon = text.IndexOf(':', StringComparison.Ordinal);
        if (colon is not (8 or 32) || !int.TryParse(text.AsSpan(colon + 1), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out port)
            || port is < 0 or > 65535)
        {
            return false;
        }
        // The kernel prints each 32-bit word of the address as it sits in memory read as a native integer.
        Span<byte> bytes = stackalloc byte[colon / 2];
        for (int w = 0; w < colon / 8; w++)
        {
            if (!uint.TryParse(text.AsSpan(w * 8, 8), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out uint word))
            {
                return false;
            }
            if (BitConverter.IsLittleEndian)
            {
                BinaryPrimitives.WriteUInt32LittleEndian(bytes[(w * 4)..], word);
            }
            else
            {
                BinaryPrimitives.WriteUInt32BigEndian(bytes[(w * 4)..], word);
            }
        }
        address = new IPAddress(bytes);
        return true;
    }
}

/// <summary>Parsing of the Windows TCP table (pure; tested on every OS).</summary>
internal static class WindowsTcpTable
{
    /// <summary>
    /// The process id owning the loopback socket connected from <paramref name="clientPort"/> to our
    /// <paramref name="serverPort"/> in a MIB_TCPTABLE_OWNER_PID buffer (u32 count, then rows of six u32:
    /// state, local address, local port, remote address, remote port, pid; addresses and ports in
    /// network byte order), or null when there is no such row or more than one owner.
    /// </summary>
    public static uint? FindOwningPid(ReadOnlySpan<byte> table, int serverPort, int clientPort)
    {
        if (table.Length < 4)
        {
            return null;
        }
        uint count = BinaryPrimitives.ReadUInt32LittleEndian(table);
        uint? pid = null;
        for (uint i = 0; i < count; i++)
        {
            int at = 4 + (int)i * 24;
            if (at + 24 > table.Length)
            {
                break;
            }
            ReadOnlySpan<byte> row = table.Slice(at, 24);
            // The first octet of a network-order IPv4 address is the first byte in memory.
            bool loopback = row[4] == 127 && row[12] == 127;
            int localPort = BinaryPrimitives.ReadUInt16BigEndian(row[8..]);
            int remotePort = BinaryPrimitives.ReadUInt16BigEndian(row[16..]);
            if (!loopback || localPort != clientPort || remotePort != serverPort)
            {
                continue;
            }
            uint owner = BinaryPrimitives.ReadUInt32LittleEndian(row[20..]);
            if (pid is uint other && other != owner)
            {
                return null;
            }
            pid = owner;
        }
        return pid;
    }
}

/// <summary>
/// Windows: GetExtendedTcpTable (TCP_TABLE_OWNER_PID_ALL) gives the process id that owns the peer's
/// socket; the user SID of that process's token is compared with ours. A process whose token
/// cannot be opened (another user's, a system process) counts as Unknown and is refused.
/// Compiled everywhere; runs on Windows only.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed unsafe partial class WindowsPeerCheck : IPeerUserCheck
{
    private const int AfInet = 2;
    private const int TcpTableOwnerPidAll = 5;
    private const uint ErrorInsufficientBuffer = 122;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint TokenQuery = 0x0008;
    private const int TokenUser = 1;

    [LibraryImport("iphlpapi.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static partial uint GetExtendedTcpTable(byte* table, uint* size, [MarshalAs(UnmanagedType.Bool)] bool order, int family, int tableClass, uint reserved);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static partial nint OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);

    [LibraryImport("kernel32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static partial nint GetCurrentProcess();

    [LibraryImport("kernel32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CloseHandle(nint handle);

    [LibraryImport("advapi32.dll", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool OpenProcessToken(nint process, uint access, nint* token);

    [LibraryImport("advapi32.dll", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetTokenInformation(nint token, int infoClass, byte* info, uint length, uint* needed);

    [LibraryImport("advapi32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool EqualSid(void* a, void* b);

    public string Method => "GetExtendedTcpTable owner pid, token user SID";

    public PeerOwner Check(IPEndPoint? local, IPEndPoint? remote)
    {
        if (local is null || remote is null || remote.AddressFamily != AddressFamily.InterNetwork && !remote.Address.IsIPv4MappedToIPv6)
        {
            return PeerOwner.Unknown;
        }
        byte[]? table = ReadTable();
        if (table is null || WindowsTcpTable.FindOwningPid(table, local.Port, remote.Port) is not uint pid || pid == 0)
        {
            return PeerOwner.Unknown;
        }
        byte[]? mine = UserSidOf(GetCurrentProcess(), close: false);
        nint process = OpenProcess(ProcessQueryLimitedInformation, false, pid);
        if (mine is null || process == 0)
        {
            return PeerOwner.Unknown;
        }
        byte[]? theirs = UserSidOf(process, close: true);
        if (theirs is null)
        {
            return PeerOwner.Unknown;
        }
        fixed (byte* a = mine)
        fixed (byte* b = theirs)
        {
            // TOKEN_USER starts with a pointer to the SID (which lies later in the same buffer).
            return EqualSid(*(void**)a, *(void**)b) ? PeerOwner.SameUser : PeerOwner.OtherUser;
        }
    }

    private static byte[]? ReadTable()
    {
        uint size = 0;
        _ = GetExtendedTcpTable(null, &size, false, AfInet, TcpTableOwnerPidAll, 0);
        for (int attempt = 0; attempt < 4 && size > 0; attempt++)
        {
            var buf = new byte[size + 4096];
            uint len = (uint)buf.Length;
            fixed (byte* p = buf)
            {
                uint rc = GetExtendedTcpTable(p, &len, false, AfInet, TcpTableOwnerPidAll, 0);
                if (rc == 0)
                {
                    return buf;
                }
                if (rc != ErrorInsufficientBuffer)
                {
                    return null;
                }
                size = len;
            }
        }
        return null;
    }

    // The TOKEN_USER buffer of a process. The buffer must stay pinned at the same address while its SID pointer is
    // used, so the caller compares inside "fixed"; the pointer is rebased here to the returned array.
    private static byte[]? UserSidOf(nint process, bool close)
    {
        nint token;
        if (!OpenProcessToken(process, TokenQuery, &token))
        {
            if (close)
            {
                CloseHandle(process);
            }
            return null;
        }
        try
        {
            uint needed = 0;
            _ = GetTokenInformation(token, TokenUser, null, 0, &needed);
            if (needed is 0 or > 4096)
            {
                return null;
            }
            byte[] buf = GC.AllocateArray<byte>((int)needed, pinned: true);
            fixed (byte* p = buf)
            {
                return GetTokenInformation(token, TokenUser, p, needed, &needed) ? buf : null;
            }
        }
        finally
        {
            CloseHandle(token);
            if (close)
            {
                CloseHandle(process);
            }
        }
    }
}

/// <summary>Reading of a macOS socket_fdinfo buffer (pure; tested on every OS against the documented layout).</summary>
internal static class MacSocketInfo
{
    /// <summary>Size asked from proc_pidfdinfo (at least sizeof(struct socket_fdinfo)).</summary>
    public const int BufferBytes = 1024;

    // struct socket_fdinfo { struct proc_fileinfo pfi (24 bytes); struct socket_info psi; }
    // socket_info: soi_stat (vinfo_stat, 136), soi_so 8, soi_pcb 8, soi_type 4, soi_protocol 4, soi_family 4,
    // seven shorts and a u_short 16, soi_oobmark 4, soi_rcv 24, soi_snd 24, soi_kind 4, rfu_1 4, soi_proto (union).
    // For TCP the union starts with in_sockinfo: insi_fport (int), insi_lport (int), ...
    private const int FamilyOffset = 24 + 160;
    private const int KindOffset = 24 + 232;
    private const int ForeignPortOffset = 24 + 240;
    private const int LocalPortOffset = ForeignPortOffset + 4;
    private const int MinimumBytes = LocalPortOffset + 4;
    private const int SockInfoTcp = 2;
    private const int AfInet = 2;
    private const int AfInet6 = 30;

    /// <summary>The ports of a TCP socket (host order), or false for any other kind of socket.</summary>
    public static bool TryReadTcpPorts(ReadOnlySpan<byte> info, int written, out int localPort, out int foreignPort)
    {
        localPort = foreignPort = 0;
        if (written < MinimumBytes || info.Length < MinimumBytes)
        {
            return false;
        }
        int family = BinaryPrimitives.ReadInt32LittleEndian(info[FamilyOffset..]);
        int kind = BinaryPrimitives.ReadInt32LittleEndian(info[KindOffset..]);
        if (kind != SockInfoTcp || family is not (AfInet or AfInet6))
        {
            return false;
        }
        // The ports are kept in network byte order in the low 16 bits of an int: the first two bytes in memory.
        foreignPort = BinaryPrimitives.ReadUInt16BigEndian(info[ForeignPortOffset..]);
        localPort = BinaryPrimitives.ReadUInt16BigEndian(info[LocalPortOffset..]);
        return true;
    }
}

/// <summary>
/// macOS: there is no per-connection owner for TCP (LOCAL_PEERCRED is for Unix sockets only), and a
/// process without elevated rights can inspect only its own user's processes. So the check is a
/// positive one: libproc lists the processes of the current effective user (proc_listpids,
/// PROC_UID_ONLY), each one's descriptors (PROC_PIDLISTFDS) and, for sockets, the TCP port pair
/// (proc_pidfdinfo, PROC_PIDFDSOCKETINFO). When one of OUR user's processes owns the socket
/// connected from the peer's port to our port, the peer is the same user. When none does, the
/// peer belongs to another user (or to root) or could not be seen: Unknown, refused.
/// Compiled everywhere; runs on macOS only.
/// </summary>
[SupportedOSPlatform("macos")]
internal sealed unsafe partial class MacPeerCheck : IPeerUserCheck
{
    private const string LibProc = "/usr/lib/libproc.dylib";
    private const uint ProcUidOnly = 4;
    private const int ProcPidListFds = 1;
    private const int ProcPidTbsdInfo = 3;
    private const int ProcPidFdSocketInfo = 3;
    private const uint ProxFdTypeSocket = 2;
    private const int BsdInfoBytes = 136;
    private const int BsdInfoUidOffset = 20;

    [LibraryImport("libc")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial uint geteuid();

    [LibraryImport(LibProc)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int proc_listpids(uint type, uint typeInfo, void* buffer, int bufferSize);

    [LibraryImport(LibProc)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int proc_pidinfo(int pid, int flavor, ulong arg, void* buffer, int bufferSize);

    [LibraryImport(LibProc)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int proc_pidfdinfo(int pid, int fd, int flavor, void* buffer, int bufferSize);

    public string Method => "libproc: a socket of one of the user's own processes must own the peer port";

    public PeerOwner Check(IPEndPoint? local, IPEndPoint? remote)
    {
        if (local is null || remote is null || !PeerUser.IsLoopback(remote.Address))
        {
            return PeerOwner.Unknown;
        }
        try
        {
            uint me = geteuid();
            int bytes = proc_listpids(ProcUidOnly, me, null, 0);
            if (bytes <= 0)
            {
                return PeerOwner.Unknown;
            }
            var pids = new int[bytes / sizeof(int) + 64];
            int got;
            fixed (int* p = pids)
            {
                got = proc_listpids(ProcUidOnly, me, p, pids.Length * sizeof(int));
            }
            byte* bsd = stackalloc byte[BsdInfoBytes];
            byte* sock = stackalloc byte[MacSocketInfo.BufferBytes];
            for (int i = 0; i < got / sizeof(int) && i < pids.Length; i++)
            {
                int pid = pids[i];
                if (pid <= 0)
                {
                    continue;
                }
                // Only a process whose effective uid is ours counts, whatever the list said.
                if (proc_pidinfo(pid, ProcPidTbsdInfo, 0, bsd, BsdInfoBytes) < BsdInfoBytes || *(uint*)(bsd + BsdInfoUidOffset) != me)
                {
                    continue;
                }
                int fdBytes = proc_pidinfo(pid, ProcPidListFds, 0, null, 0);
                if (fdBytes <= 0)
                {
                    continue;
                }
                var fds = new byte[fdBytes + 8 * 32];
                int fdGot;
                fixed (byte* f = fds)
                {
                    fdGot = proc_pidinfo(pid, ProcPidListFds, 0, f, fds.Length);
                }
                // struct proc_fdinfo { int32 proc_fd; uint32 proc_fdtype; }
                for (int at = 0; at + 8 <= fdGot; at += 8)
                {
                    if (BitConverter.ToUInt32(fds, at + 4) != ProxFdTypeSocket)
                    {
                        continue;
                    }
                    int written = proc_pidfdinfo(pid, BitConverter.ToInt32(fds, at), ProcPidFdSocketInfo, sock, MacSocketInfo.BufferBytes);
                    if (MacSocketInfo.TryReadTcpPorts(new ReadOnlySpan<byte>(sock, MacSocketInfo.BufferBytes), written, out int lport, out int fport)
                        && lport == remote.Port && fport == local.Port)
                    {
                        return PeerOwner.SameUser;
                    }
                }
            }
            return PeerOwner.Unknown;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException)
        {
            return PeerOwner.Unknown;
        }
    }
}
