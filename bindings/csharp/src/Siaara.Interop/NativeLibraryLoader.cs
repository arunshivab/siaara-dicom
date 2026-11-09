using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;

namespace Siaara.Interop;

/// <summary>
/// Loads the native SIAARA library from a fixed location relative to the application base
/// directory: first <c>runtimes/&lt;rid&gt;/native/</c> (the package layout, also used by
/// <c>dotnet build</c> output), then the base directory itself (a RID-specific publish flattens the
/// runtimes folder). The current directory and PATH are never searched.
/// </summary>
internal static class NativeLibraryLoader
{
    private static int s_registered;
    private static readonly Lazy<nint> s_handle = new(Load, LazyThreadSafetyMode.ExecutionAndPublication);
    private static string? s_loadedPath;

    /// <summary>Full path of the loaded library (null before the first native call).</summary>
    internal static string? LoadedPath => s_loadedPath;

    /// <summary>The portable runtime identifier of this process, e.g. linux-x64.</summary>
    internal static string RuntimeIdentifier
    {
        get
        {
            string os = OperatingSystem.IsWindows() ? "win"
                : OperatingSystem.IsMacOS() ? "osx"
                : OperatingSystem.IsLinux() ? "linux"
                : throw new PlatformNotSupportedException("SIAARA supports Windows, macOS and Linux.");
            string arch = RuntimeInformation.ProcessArchitecture switch
            {
                Architecture.X64 => "x64",
                Architecture.X86 => "x86",
                Architecture.Arm64 => "arm64",
                Architecture.Arm => "arm",
                var a => throw new PlatformNotSupportedException("Unsupported process architecture " + a + "."),
            };
            return os + "-" + arch;
        }
    }

    /// <summary>File name of the native library on this operating system.</summary>
    internal static string FileName =>
        OperatingSystem.IsWindows() ? "siaara.dll" : OperatingSystem.IsMacOS() ? "libsiaara.dylib" : "libsiaara.so";

    /// <summary>Candidate paths, in the order they are tried.</summary>
    internal static IReadOnlyList<string> CandidatePaths()
    {
        string baseDir = AppContext.BaseDirectory;
        return new[]
        {
            Path.Combine(baseDir, "runtimes", RuntimeIdentifier, "native", FileName),
            Path.Combine(baseDir, FileName),
        };
    }

    internal static void EnsureRegistered()
    {
        if (Interlocked.Exchange(ref s_registered, 1) == 0)
        {
            NativeLibrary.SetDllImportResolver(typeof(NativeLibraryLoader).Assembly, Resolve);
        }
    }

    private static nint Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        return string.Equals(libraryName, Raw.Library, StringComparison.Ordinal) ? s_handle.Value : 0;
    }

    private static nint Load()
    {
        IReadOnlyList<string> candidates = CandidatePaths();
        foreach (string path in candidates)
        {
            if (File.Exists(path))
            {
                nint h = NativeLibrary.Load(path);
                s_loadedPath = path;
                return h;
            }
        }
        throw new DllNotFoundException(
            "The SIAARA native library was not found. Looked for: " + string.Join(", ", candidates) + ".");
    }
}
