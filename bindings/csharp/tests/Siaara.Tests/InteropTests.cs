using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class InteropTests
{
    private static string? RunProcess(string file, IEnumerable<string> args, string? workDir, out int exitCode)
    {
        var psi = new ProcessStartInfo(file) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        foreach (string a in args)
        {
            psi.ArgumentList.Add(a);
        }
        if (workDir is not null)
        {
            psi.WorkingDirectory = workDir;
        }
        try
        {
            using Process p = Process.Start(psi)!;
            string output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
            p.WaitForExit();
            exitCode = p.ExitCode;
            return output;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            exitCode = -1;
            return null;
        }
    }

    internal static void Register()
    {
        Case("interop: struct layout matches the C compiler (sizeof / offsetof of every struct)", () =>
        {
            List<string> measured = StructLayouts.Measure();
            if (LayoutExe is null)
            {
                Skip("--layout-exe not given (CMake target siaara_csharp_struct_layout)");
                return;
            }
            string? output = RunProcess(LayoutExe, Array.Empty<string>(), null, out int code);
            Require(output is not null && code == 0);
            string[] expected = output!.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            CheckEq(expected.Length, measured.Count);
            int structs = 0, mismatches = 0;
            for (int i = 0; i < Math.Min(expected.Length, measured.Count); i++)
            {
                if (expected[i].StartsWith("S ", StringComparison.Ordinal))
                {
                    structs++;
                }
                if (!string.Equals(expected[i], measured[i], StringComparison.Ordinal))
                {
                    Console.Error.WriteLine($"    C: {expected[i]}   C#: {measured[i]}");
                    mismatches++;
                }
            }
            CheckEq(mismatches, 0);
            CheckEq(structs, measured.Count(l => l.StartsWith("S ", StringComparison.Ordinal)));
            Check(structs > 0);
            Console.WriteLine($"  {structs} structs, {measured.Count - structs} fields compared");
        });

        Case("interop: Marshal.SizeOf agrees with Unsafe.SizeOf for the blittable structs", () =>
        {
            Dictionary<string, string> sizes = StructLayouts.Measure().Where(l => l.StartsWith("S ", StringComparison.Ordinal))
                .Select(l => l.Split(' ')).ToDictionary(p => p[1], p => p[2]);
            int compared = 0;
            try
            {
                foreach (string line in StructLayouts.MarshalSizes())
                {
                    string[] p = line.Split(' ');
                    CheckEq(p[1], sizes[p[0]]);
                    compared++;
                }
                Console.WriteLine($"  {compared} structs compared");
            }
            catch (ArgumentException e)
            {
                Skip("Marshal.SizeOf does not support these types under DisableRuntimeMarshalling: " + e.Message);
            }
        });

        Case("interop: generated files are up to date (tools/gen_csharp_interop.py --check)", () =>
        {
            string script = Path.Combine(RepoRoot, "tools", "gen_csharp_interop.py");
            if (!File.Exists(script))
            {
                Skip("repository not found at " + RepoRoot);
                return;
            }
            string? output = null;
            int code = -1;
            foreach (string py in OperatingSystem.IsWindows() ? new[] { "python", "py", "python3" } : new[] { "python3", "python" })
            {
                output = RunProcess(py, new[] { script, "--check" }, RepoRoot, out code);
                if (output is not null)
                {
                    break;
                }
            }
            if (output is null)
            {
                Skip("python not found");
                return;
            }
            Console.WriteLine("  " + output.Trim());
            CheckEq(code, 0);
        });

        Case("interop: SafeHandle leak test (create/dispose loop, memory stays flat)", () =>
        {
            // The C API has no live-handle counter, so this measures process memory: 3 x 20,000
            // files (with pixel data) created and disposed, plus 20,000 left to the finalizer.
            static void Round(int n, bool dispose)
            {
                for (int i = 0; i < n; i++)
                {
                    DicomFile f = Builders.Smoke();
                    _ = f.Dataset.GetString(DicomTags.PatientName);
                    _ = f.DecodeFrame(0);
                    if (dispose)
                    {
                        f.Dispose();
                    }
                }
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
            }
            static long Rss()
            {
                using Process p = Process.GetCurrentProcess();
                p.Refresh();
                return p.WorkingSet64;
            }
            Round(20_000, true); // warm-up
            long liveBefore = SiaaraLibrary.LiveHandleCount;
            long start = Rss();
            for (int r = 0; r < 3; r++)
            {
                Round(20_000, true);
            }
            long afterDispose = Rss();
            // Finalizer path: the first round grows the heaps (garbage waits for the finalizer
            // thread); later rounds must reuse that memory instead of growing further.
            Round(20_000, false);
            long afterFinalize1 = Rss();
            for (int r = 0; r < 3; r++)
            {
                Round(20_000, false);
            }
            long afterFinalize4 = Rss();
            Console.WriteLine($"  RSS: start {start / 1024} KiB, after 60,000 disposed {afterDispose / 1024} KiB, " +
                $"after 20,000 finalized {afterFinalize1 / 1024} KiB, after 80,000 finalized {afterFinalize4 / 1024} KiB");
            Check(afterDispose - start < 16L * 1024 * 1024);
            // The finalizer path is checked exactly with the native live-handle counter: RSS after
            // finalization depends on GC heap sizing and varies between runners (21 MB seen on CI).
            Check(SiaaraLibrary.LiveHandleCount == liveBefore);
            Check(afterFinalize4 - afterFinalize1 < 64L * 1024 * 1024);
        });
    }
}
