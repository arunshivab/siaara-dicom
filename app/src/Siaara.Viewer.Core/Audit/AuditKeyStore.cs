// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Core.Settings;

namespace Siaara.Viewer.Core.Audit;

/// <summary>Where the audit key came from.</summary>
public enum AuditKeyProtection
{
    /// <summary>A 0600 key file next to the log (Linux; the fallback elsewhere). Anything running as the user can read it.</summary>
    OwnerOnlyFile,

    /// <summary>A key file wrapped with the Windows data-protection API for the current user.</summary>
    WindowsDataProtection,

    /// <summary>A generic-password item in the user's macOS login keychain.</summary>
    MacKeychain,
}

/// <summary>The audit key and how it is protected.</summary>
/// <param name="Key">32 random bytes (HMAC-SHA256 key).</param>
/// <param name="Protection">Where it is kept.</param>
/// <param name="Created">True when this call created it.</param>
public sealed record AuditKey(ReadOnlyMemory<byte> Key, AuditKeyProtection Protection, bool Created);

/// <summary>Gives the audit log its key.</summary>
public interface IAuditKeySource
{
    /// <summary>Loads the key of the log in <paramref name="auditFolder"/>, creating it when <paramref name="create"/> is set.</summary>
    /// <param name="auditFolder">Folder of audit.jsonl.</param>
    /// <param name="create">Create a new key when none exists.</param>
    /// <returns>The key, or null when there is none and <paramref name="create"/> is false.</returns>
    AuditKey? Load(string auditFolder, bool create);
}

/// <summary>
/// The audit key per OS. Windows: 32 random bytes wrapped by CryptProtectData (current user) in
/// audit.key.dpapi. macOS: a generic password in the login keychain (service
/// "in.sthraas.siaara.viewer.audit", account = SHA-256 of the audit folder path); when the keychain
/// cannot be used (no login keychain in a headless session) the key file is used and that is logged.
/// Linux and others: audit.key, mode 0600, next to the log. The key file is the weaker form: it
/// stops other users and makes edits by tools that do not know the app detectable, but a program
/// running as the same user can read it and re-sign the log.
/// </summary>
public static class AuditKeys
{
    private const int KeyBytes = 32;

    /// <summary>The OS key store described above.</summary>
    public static IAuditKeySource OsStore { get; } = new OsSource();

    /// <summary>Always the owner-only key file (tests; also what Linux uses).</summary>
    public static IAuditKeySource KeyFile { get; } = new FileSource();

    /// <summary>Removes the key of a log folder from the OS store (tests and uninstall).</summary>
    /// <param name="auditFolder">Folder.</param>
    public static void Delete(string auditFolder)
    {
        File.Delete(Path.Combine(auditFolder, "audit.key"));
        File.Delete(Path.Combine(auditFolder, "audit.key.dpapi"));
        if (OperatingSystem.IsMacOS())
        {
            MacKeychain.Delete(Account(auditFolder));
        }
    }

    internal static string Account(string auditFolder) =>
        Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(auditFolder))));

    private sealed class FileSource : IAuditKeySource
    {
        public AuditKey? Load(string auditFolder, bool create) => LoadFile(auditFolder, create);
    }

    private sealed class OsSource : IAuditKeySource
    {
        public AuditKey? Load(string auditFolder, bool create)
        {
            if (OperatingSystem.IsWindows())
            {
                return LoadWindows(auditFolder, create);
            }
            if (OperatingSystem.IsMacOS())
            {
                // A key file from an earlier fallback keeps being used, so the chain stays verifiable.
                if (File.Exists(Path.Combine(auditFolder, "audit.key")))
                {
                    return LoadFile(auditFolder, create);
                }
                try
                {
                    string account = Account(auditFolder);
                    byte[]? k = MacKeychain.Find(account);
                    if (k is { Length: KeyBytes })
                    {
                        return new AuditKey(k, AuditKeyProtection.MacKeychain, false);
                    }
                    if (!create)
                    {
                        return null;
                    }
                    k = RandomNumberGenerator.GetBytes(KeyBytes);
                    MacKeychain.Add(account, k);
                    return new AuditKey(k, AuditKeyProtection.MacKeychain, true);
                }
                catch (Exception e) when (e is CryptographicException or DllNotFoundException or EntryPointNotFoundException)
                {
                    AppLog.Warn("audit", "the keychain could not be used for the audit key; an owner-only key file is used instead (" + e.GetType().Name + ")");
                    return LoadFile(auditFolder, create);
                }
            }
            return LoadFile(auditFolder, create);
        }
    }

    private static AuditKey? LoadFile(string auditFolder, bool create)
    {
        string path = Path.Combine(auditFolder, "audit.key");
        if (File.Exists(path))
        {
            if (!OperatingSystem.IsWindows() && !OwnerOnly.IsOwnerOnly(File.GetUnixFileMode(path)))
            {
                AppLog.Warn("audit", "the audit key file was readable by others; its mode was set to owner-only, treat the audit trail before this as unprotected");
                File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            }
            byte[] k = ReadHex(path);
            return k.Length == KeyBytes ? new AuditKey(k, AuditKeyProtection.OwnerOnlyFile, false)
                : throw new CryptographicException("The audit key file is damaged.");
        }
        if (!create)
        {
            return null;
        }
        byte[] key = RandomNumberGenerator.GetBytes(KeyBytes);
        AtomicFile.WriteAllBytes(path, Encoding.ASCII.GetBytes(Convert.ToHexStringLower(key)));
        return new AuditKey(key, AuditKeyProtection.OwnerOnlyFile, true);
    }

    private static byte[] ReadHex(string path)
    {
        try
        {
            return Convert.FromHexString(File.ReadAllText(path).Trim());
        }
        catch (FormatException)
        {
            return [];
        }
    }

    [SupportedOSPlatform("windows")]
    private static AuditKey? LoadWindows(string auditFolder, bool create)
    {
        string path = Path.Combine(auditFolder, "audit.key.dpapi");
        if (File.Exists(path))
        {
            byte[] k = WindowsDataProtection.Unprotect(File.ReadAllBytes(path));
            return k.Length == KeyBytes ? new AuditKey(k, AuditKeyProtection.WindowsDataProtection, false)
                : throw new CryptographicException("The audit key file is damaged.");
        }
        if (!create)
        {
            return null;
        }
        byte[] key = RandomNumberGenerator.GetBytes(KeyBytes);
        AtomicFile.WriteAllBytes(path, WindowsDataProtection.Protect(key));
        return new AuditKey(key, AuditKeyProtection.WindowsDataProtection, true);
    }
}

/// <summary>CryptProtectData / CryptUnprotectData (crypt32) for the current user. Compiled everywhere, runs on Windows only.</summary>
[SupportedOSPlatform("windows")]
internal static unsafe partial class WindowsDataProtection
{
    private const uint UiForbidden = 0x1;
    private static readonly byte[] s_entropy = Encoding.ASCII.GetBytes("SIAARA Viewer audit key v1");

    [StructLayout(LayoutKind.Sequential)]
    private struct DataBlob
    {
        public uint Size;
        public byte* Data;
    }

    [LibraryImport("crypt32.dll", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CryptProtectData(DataBlob* dataIn, char* description, DataBlob* entropy, void* reserved, void* prompt, uint flags, DataBlob* dataOut);

    [LibraryImport("crypt32.dll", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CryptUnprotectData(DataBlob* dataIn, char** description, DataBlob* entropy, void* reserved, void* prompt, uint flags, DataBlob* dataOut);

    [LibraryImport("kernel32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static partial void* LocalFree(void* mem);

    public static byte[] Protect(byte[] clear) => Run(clear, protect: true, s_entropy);

    public static byte[] Unprotect(byte[] wrapped) => Run(wrapped, protect: false, s_entropy);

    /// <summary>Wraps data for the current user with a purpose-specific entropy (the secret store uses its own).</summary>
    public static byte[] Protect(byte[] clear, byte[] entropy) => Run(clear, protect: true, entropy);

    /// <summary>Unwraps data wrapped with the same entropy.</summary>
    public static byte[] Unprotect(byte[] wrapped, byte[] entropy) => Run(wrapped, protect: false, entropy);

    private static byte[] Run(byte[] input, bool protect, byte[] entropy)
    {
        fixed (byte* pin = input)
        fixed (byte* pe = entropy)
        {
            var din = new DataBlob { Size = (uint)input.Length, Data = pin };
            var ent = new DataBlob { Size = (uint)entropy.Length, Data = pe };
            var dout = default(DataBlob);
            bool ok = protect
                ? CryptProtectData(&din, null, &ent, null, null, UiForbidden, &dout)
                : CryptUnprotectData(&din, null, &ent, null, null, UiForbidden, &dout);
            if (!ok)
            {
                throw new CryptographicException("Windows data protection failed (error " +
                    Marshal.GetLastPInvokeError().ToString(System.Globalization.CultureInfo.InvariantCulture) + ").");
            }
            try
            {
                return new ReadOnlySpan<byte>(dout.Data, (int)dout.Size).ToArray();
            }
            finally
            {
                LocalFree(dout.Data);
            }
        }
    }
}

/// <summary>Generic-password items in the login keychain through SecItemAdd / SecItemCopyMatching / SecItemDelete
/// (Security.framework, CoreFoundation). Compiled everywhere, runs on macOS only.</summary>
[SupportedOSPlatform("macos")]
internal static unsafe partial class MacKeychain
{
    private const string Cf = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    private const string Sec = "/System/Library/Frameworks/Security.framework/Security";
    private const string Service = "in.sthraas.siaara.viewer.audit";
    private const uint Utf8 = 0x08000100;
    private const int ErrSecItemNotFound = -25300;

    [LibraryImport(Cf, StringMarshalling = StringMarshalling.Utf8)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial nint CFStringCreateWithCString(nint allocator, string text, uint encoding);

    [LibraryImport(Cf)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial nint CFDataCreate(nint allocator, byte* bytes, nint length);

    [LibraryImport(Cf)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial nint CFDataGetLength(nint data);

    [LibraryImport(Cf)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial byte* CFDataGetBytePtr(nint data);

    [LibraryImport(Cf)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial nint CFDictionaryCreateMutable(nint allocator, nint capacity, nint keyCallBacks, nint valueCallBacks);

    [LibraryImport(Cf)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial void CFDictionarySetValue(nint dictionary, nint key, nint value);

    [LibraryImport(Cf)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial void CFRelease(nint obj);

    [LibraryImport(Sec)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int SecItemAdd(nint attributes, nint* result);

    [LibraryImport(Sec)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int SecItemCopyMatching(nint query, nint* result);

    [LibraryImport(Sec)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static partial int SecItemDelete(nint query);

    private static nint Constant(nint library, string name) => Marshal.ReadIntPtr(NativeLibrary.GetExport(library, name));

    // A query for our service and the account; the caller releases it.
    private static nint Query(string account, out nint cf, out nint sec, List<nint> owned, string serviceName)
    {
        cf = NativeLibrary.Load(Cf);
        sec = NativeLibrary.Load(Sec);
        nint dict = CFDictionaryCreateMutable(0, 0, NativeLibrary.GetExport(cf, "kCFTypeDictionaryKeyCallBacks"),
            NativeLibrary.GetExport(cf, "kCFTypeDictionaryValueCallBacks"));
        if (dict == 0)
        {
            throw new CryptographicException("The keychain query could not be built.");
        }
        nint service = CFStringCreateWithCString(0, serviceName, Utf8);
        nint acct = CFStringCreateWithCString(0, account, Utf8);
        owned.Add(service);
        owned.Add(acct);
        CFDictionarySetValue(dict, Constant(sec, "kSecClass"), Constant(sec, "kSecClassGenericPassword"));
        CFDictionarySetValue(dict, Constant(sec, "kSecAttrService"), service);
        CFDictionarySetValue(dict, Constant(sec, "kSecAttrAccount"), acct);
        return dict;
    }

    private static void Release(nint dict, List<nint> owned)
    {
        foreach (nint o in owned)
        {
            if (o != 0)
            {
                CFRelease(o);
            }
        }
        CFRelease(dict);
    }

    public static byte[]? Find(string account, string service = Service)
    {
        var owned = new List<nint>();
        nint q = Query(account, out nint cf, out nint sec, owned, service);
        try
        {
            CFDictionarySetValue(q, Constant(sec, "kSecReturnData"), Constant(cf, "kCFBooleanTrue"));
            CFDictionarySetValue(q, Constant(sec, "kSecMatchLimit"), Constant(sec, "kSecMatchLimitOne"));
            nint data;
            int status = SecItemCopyMatching(q, &data);
            if (status == ErrSecItemNotFound)
            {
                return null;
            }
            if (status != 0 || data == 0)
            {
                throw new CryptographicException("The keychain refused the item (status " + status.ToString(System.Globalization.CultureInfo.InvariantCulture) + ").");
            }
            try
            {
                return new ReadOnlySpan<byte>(CFDataGetBytePtr(data), (int)CFDataGetLength(data)).ToArray();
            }
            finally
            {
                CFRelease(data);
            }
        }
        finally
        {
            Release(q, owned);
        }
    }

    public static void Add(string account, byte[] key, string service = Service)
    {
        var owned = new List<nint>();
        nint q = Query(account, out _, out nint sec, owned, service);
        try
        {
            nint data;
            fixed (byte* p = key)
            {
                data = CFDataCreate(0, p, key.Length);
            }
            owned.Add(data);
            CFDictionarySetValue(q, Constant(sec, "kSecValueData"), data);
            int status = SecItemAdd(q, null);
            if (status != 0)
            {
                throw new CryptographicException("The keychain did not store the item (status " + status.ToString(System.Globalization.CultureInfo.InvariantCulture) + ").");
            }
        }
        finally
        {
            Release(q, owned);
        }
    }

    public static void Delete(string account, string service = Service)
    {
        var owned = new List<nint>();
        nint q = Query(account, out _, out _, owned, service);
        try
        {
            _ = SecItemDelete(q);
        }
        finally
        {
            Release(q, owned);
        }
    }
}
