using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Siaara.Interop;

namespace Siaara;

/// <summary>Information about the loaded native library.</summary>
public static class SiaaraLibrary
{
    /// <summary>The ABI version these bindings were generated for (SIAARA_ABI_VERSION).</summary>
    public static uint ExpectedAbiVersion => C.SIAARA_ABI_VERSION;

    /// <summary>The ABI version of the loaded native library (siaara_abi_version).</summary>
    public static uint AbiVersion => Native.siaara_abi_version();

    /// <summary>The native library version string.</summary>
    public static string Version => Marshal.PtrToStringUTF8(Native.siaara_version_string()) ?? string.Empty;

    /// <summary>Full path of the loaded native library (loads it if needed).</summary>
    public static string? NativeLibraryPath
    {
        get
        {
            _ = Native.siaara_abi_version();
            return NativeLibraryLoader.LoadedPath;
        }
    }

    /// <summary>
    /// Number of native handles currently allocated by the library (siaara_debug_live_handles):
    /// files, frames, stores, results, clients, ... For leak tests; a snapshot.
    /// </summary>
    public static long LiveHandleCount
    {
        get
        {
            Status.Check(Native.siaara_debug_live_handles(out long n));
            return n;
        }
    }

    /// <summary>The runtime identifier used to find the native library, e.g. linux-x64.</summary>
    public static string RuntimeIdentifier => NativeLibraryLoader.RuntimeIdentifier;

    /// <summary>
    /// Whether TLS can be used in this process (siaara_tls_available): SChannel on Windows,
    /// Network.framework on macOS, the system OpenSSL 3 on Linux.
    /// </summary>
    /// <param name="reason">Why not, when the method returns false.</param>
    /// <returns>True when TLS is available.</returns>
    public static bool IsTlsAvailable(out string reason)
    {
        bool ok = Native.siaara_tls_available() != 0;
        reason = ok ? string.Empty : Status.LastMessage();
        return ok;
    }
}

/// <summary>struct_size values for the C structs.</summary>
internal static class StructSize
{
    internal static uint Of<T>() where T : unmanaged => (uint)Unsafe.SizeOf<T>();
}
