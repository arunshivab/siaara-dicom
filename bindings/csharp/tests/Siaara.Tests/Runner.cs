using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;

namespace Siaara.Tests;

/// <summary>Thrown by Require to abort the current test case.</summary>
internal sealed class RequireFailedException : Exception
{
    public RequireFailedException() { }
    public RequireFailedException(string message) : base(message) { }
    public RequireFailedException(string message, Exception inner) : base(message, inner) { }
}

/// <summary>
/// Minimal test framework in the style of tests/test.hpp: named cases, CHECK (record and go on),
/// REQUIRE (record and leave the case), failures printed as "  FAILED: expr (file:line)".
/// </summary>
internal static class T
{
    private static readonly List<(string Name, Action Body)> s_cases = new();
    internal static int Failures { get; private set; }
    internal static int Skipped { get; private set; }

    /// <summary>Command-line options shared with the cases.</summary>
    internal static string? LayoutExe { get; set; }
    internal static string RepoRoot { get; set; } = "";
    internal static string TempRoot { get; set; } = "";

    internal static void Case(string name, Action body) => s_cases.Add((name, body));

    internal static void Check(bool condition, [CallerArgumentExpression(nameof(condition))] string expr = "",
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!condition)
        {
            Console.Error.WriteLine($"  FAILED: {expr} ({Path.GetFileName(file)}:{line})");
            Failures++;
        }
    }

    internal static void Require(bool condition, [CallerArgumentExpression(nameof(condition))] string expr = "",
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!condition)
        {
            Console.Error.WriteLine($"  FAILED (fatal): {expr} ({Path.GetFileName(file)}:{line})");
            Failures++;
            throw new RequireFailedException(expr);
        }
    }

    internal static void CheckEq<TV>(TV a, TV b, [CallerArgumentExpression(nameof(a))] string ea = "",
        [CallerArgumentExpression(nameof(b))] string eb = "", [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!EqualityComparer<TV>.Default.Equals(a, b))
        {
            Console.Error.WriteLine($"  FAILED: {ea} == {eb} (got {a} vs {b}) ({Path.GetFileName(file)}:{line})");
            Failures++;
        }
    }

    internal static void CheckNear(double a, double b, double tol, [CallerArgumentExpression(nameof(a))] string ea = "",
        [CallerArgumentExpression(nameof(b))] string eb = "", [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!(Math.Abs(a - b) <= tol))
        {
            Console.Error.WriteLine($"  FAILED: {ea} ~= {eb} (got {a} vs {b}, tol {tol}) ({Path.GetFileName(file)}:{line})");
            Failures++;
        }
    }

    /// <summary>Checks that action throws TException; returns it (or null).</summary>
    internal static TException? Throws<TException>(Action action, [CallerArgumentExpression(nameof(action))] string expr = "",
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException e)
        {
            return e;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"  FAILED: {expr} threw {e.GetType().Name}: {e.Message} ({Path.GetFileName(file)}:{line})");
            Failures++;
            return null;
        }
        Console.Error.WriteLine($"  FAILED: {expr} did not throw {typeof(TException).Name} ({Path.GetFileName(file)}:{line})");
        Failures++;
        return null;
    }

    internal static void Skip(string reason)
    {
        Console.WriteLine("  SKIPPED: " + reason);
        Skipped++;
    }

    /// <summary>A new empty directory under the run's temporary root.</summary>
    internal static string NewTempDir(string name)
    {
        string d = Path.Combine(TempRoot, name + "-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(d);
        return d;
    }

    internal static int Run(List<string> filters)
    {
        int run = 0, failedCases = 0;
        var sw = Stopwatch.StartNew();
        foreach ((string name, Action body) in s_cases)
        {
            if (filters.Count > 0 && !filters.Exists(f => name.StartsWith(f, StringComparison.Ordinal)))
            {
                continue;
            }
            run++;
            int before = Failures;
            Console.WriteLine(name);
            try
            {
                body();
            }
            catch (RequireFailedException)
            {
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"  FAILED: unexpected {e.GetType().Name}: {e.Message}\n{e.StackTrace}");
                Failures++;
            }
            if (Failures != before)
            {
                failedCases++;
            }
        }
        Console.WriteLine($"{run} test cases, {failedCases} failed, {Failures} failed checks, {Skipped} skipped ({sw.Elapsed.TotalSeconds:F1} s)");
        return failedCases == 0 && run > 0 ? 0 : 1;
    }
}

internal static class Program
{
    private static int Main(string[] args)
    {
        var filters = new List<string>();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--layout-exe": T.LayoutExe = args[++i]; break;
                case "--repo": T.RepoRoot = args[++i]; break;
                default: filters.Add(args[i]); break;
            }
        }
        if (T.RepoRoot.Length == 0)
        {
            foreach (var a in typeof(Program).Assembly.GetCustomAttributes(typeof(System.Reflection.AssemblyMetadataAttribute), false))
            {
                var m = (System.Reflection.AssemblyMetadataAttribute)a;
                if (m.Key == "SiaaraRepoRoot" && m.Value is not null)
                {
                    T.RepoRoot = m.Value;
                }
            }
        }
        T.TempRoot = Path.Combine(Path.GetTempPath(), "siaara-cs-tests-" + Environment.ProcessId);
        Directory.CreateDirectory(T.TempRoot);
        try
        {
            CoreTests.Register();
            PixelTests.Register();
            CharsetTests.Register();
            JsonWebTests.Register();
            StoreTests.Register();
            DeidTranscodeTests.Register();
            ClinicalTests.Register();
            VolumeTests.Register();
            InteropTests.Register();
            ViewerTests.Register();
            NetworkTests.Register();
            Network2Tests.Register();
            PreviewTests.Register();
            DisplayTextTests.Register();
            DicomdirMemoryTests.Register();
            EncoderTests.Register();
            PrivateDictionaryTests.Register();
            NamedTests.Register();
            return T.Run(filters);
        }
        finally
        {
            try
            {
                Directory.Delete(T.TempRoot, recursive: true);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }
}
