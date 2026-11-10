// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace Siaara.Viewer.Tests;

/// <summary>Aborts the current case (a REQUIRE failed).</summary>
internal sealed class RequireFailedException : Exception
{
    public RequireFailedException()
    {
    }

    public RequireFailedException(string message) : base(message)
    {
    }

    public RequireFailedException(string message, Exception inner) : base(message, inner)
    {
    }
}

/// <summary>A minimal test framework (no NuGet): named async cases, Check (record and go on), Require (record and leave the case).</summary>
internal static class T
{
    private static readonly List<(string Name, Func<Task> Body)> s_cases = [];
    private static readonly List<(string Name, bool Ok, double Seconds)> s_results = [];
    private static readonly List<string> s_perf = [];

    public static int Failures { get; private set; }

    public static string TempRoot { get; set; } = "";

    public static bool Quick { get; set; }

    public static void Case(string name, Func<Task> body) => s_cases.Add((name, body));

    public static void Case(string name, Action body) => s_cases.Add((name, () =>
    {
        body();
        return Task.CompletedTask;
    }));

    public static void Check(bool condition, [CallerArgumentExpression(nameof(condition))] string expr = "", [CallerFilePath] string file = "",
        [CallerLineNumber] int line = 0)
    {
        if (!condition)
        {
            Console.Error.WriteLine($"  FAILED: {expr} ({Path.GetFileName(file)}:{line})");
            Failures++;
        }
    }

    public static void Require(bool condition, [CallerArgumentExpression(nameof(condition))] string expr = "", [CallerFilePath] string file = "",
        [CallerLineNumber] int line = 0)
    {
        if (!condition)
        {
            Console.Error.WriteLine($"  FAILED (fatal): {expr} ({Path.GetFileName(file)}:{line})");
            Failures++;
            throw new RequireFailedException(expr);
        }
    }

    public static void Eq<TV>(TV a, TV b, [CallerArgumentExpression(nameof(a))] string ea = "", [CallerArgumentExpression(nameof(b))] string eb = "",
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!EqualityComparer<TV>.Default.Equals(a, b))
        {
            Console.Error.WriteLine($"  FAILED: {ea} == {eb} (got {a} vs {b}) ({Path.GetFileName(file)}:{line})");
            Failures++;
        }
    }

    public static void Near(double a, double b, double tol, [CallerArgumentExpression(nameof(a))] string ea = "", [CallerArgumentExpression(nameof(b))] string eb = "",
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!(Math.Abs(a - b) <= tol))
        {
            Console.Error.WriteLine($"  FAILED: {ea} ~= {eb} (got {a} vs {b}, tol {tol}) ({Path.GetFileName(file)}:{line})");
            Failures++;
        }
    }

    /// <summary>A performance figure (reported, never failing).</summary>
    public static void Perf(string text)
    {
        Console.WriteLine("  PERF: " + text);
        s_perf.Add(text);
    }

    public static void Note(string text) => Console.WriteLine("  " + text);

    public static string NewTempDir(string name)
    {
        string d = Path.Combine(TempRoot, name + "-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(d);
        return d;
    }

    public static async Task<int> RunAsync(IReadOnlyList<string> filters)
    {
        int run = 0, failed = 0;
        var total = Stopwatch.StartNew();
        foreach ((string name, Func<Task> body) in s_cases)
        {
            if (filters.Count > 0 && !filters.Any(f => name.StartsWith(f, StringComparison.Ordinal)))
            {
                continue;
            }
            run++;
            int before = Failures;
            Console.WriteLine(name);
            var sw = Stopwatch.StartNew();
            try
            {
                await body().WaitAsync(TimeSpan.FromMinutes(5)).ConfigureAwait(false);
            }
            catch (RequireFailedException)
            {
            }
#pragma warning disable CA1031 // The runner reports any failure of a case and goes on with the next one.
            catch (Exception e)
#pragma warning restore CA1031
            {
                Console.Error.WriteLine($"  FAILED: unexpected {e.GetType().Name}: {e.Message}\n{e.StackTrace}");
                Failures++;
            }
            bool ok = Failures == before;
            if (!ok)
            {
                failed++;
            }
            s_results.Add((name, ok, sw.Elapsed.TotalSeconds));
        }
        Console.WriteLine();
        Console.WriteLine("Summary:");
        foreach ((string n, bool ok, double s) in s_results)
        {
            Console.WriteLine($"  {(ok ? "PASS" : "FAIL")}  {n}  ({s:F2} s)");
        }
        if (s_perf.Count > 0)
        {
            Console.WriteLine("Performance (reported, not failing):");
            foreach (string p in s_perf)
            {
                Console.WriteLine("  " + p);
            }
        }
        Console.WriteLine($"{run} test cases, {failed} failed, {Failures} failed checks ({total.Elapsed.TotalSeconds:F1} s)");
        return failed == 0 && run > 0 ? 0 : 1;
    }
}

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var filters = new List<string>();
        foreach (string a in args)
        {
            if (a == "--quick")
            {
                T.Quick = true;
            }
            else
            {
                filters.Add(a);
            }
        }
        // Real path (macOS: /var -> /private/var), so paths the toolkit resolves compare equal.
        T.TempRoot = Path.Combine(RealPath(Path.GetTempPath()), "siaara-viewer-tests-" + Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Directory.CreateDirectory(T.TempRoot);
        try
        {
            CoreTests.Register();
            StoreImportTests.Register();
            ImagingTests.Register();
            MeasureTests.Register();
            ViewerFeatureTests.Register();
            SuvRoiGoldenTests.Register();
            RobustnessTests.Register();
            ProtocolTests.Register();
            HostTests.Register();
            PerfTests.Register();
            return await T.RunAsync(filters).ConfigureAwait(false);
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

    // Resolves symbolic links in every component of an existing directory path.
    private static string RealPath(string path)
    {
        string full = Path.GetFullPath(path);
        string cur = Path.GetPathRoot(full) ?? "";
        foreach (string part in full.Substring(cur.Length).Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
        {
            string next = Path.Combine(cur, part);
            var info = new DirectoryInfo(next);
            cur = info.LinkTarget is null ? next : (info.ResolveLinkTarget(true)?.FullName ?? next);
        }
        return cur;
    }
}
