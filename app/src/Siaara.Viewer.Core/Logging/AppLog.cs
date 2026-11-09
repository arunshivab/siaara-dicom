// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Logging;

/// <summary>Severity of an app log line.</summary>
public enum LogLevel
{
    /// <summary>Debug.</summary>
    Debug = 0,
    /// <summary>Information.</summary>
    Information = 1,
    /// <summary>Warning.</summary>
    Warning = 2,
    /// <summary>Error.</summary>
    Error = 3,
}

/// <summary>
/// Removes anything that could identify a patient from a log line: UIDs, DICOM dates and times,
/// person names in DICOM PN form (with '^'), e-mail addresses, absolute paths (folder names often
/// carry patient names) and long digit runs (IDs, accession numbers). The app never logs dataset
/// values on purpose; this is the safety net every line passes through.
/// </summary>
public static partial class PhiRedactor
{
    [GeneratedRegex(@"\b\d+(?:\.\d+){2,}\b", RegexOptions.CultureInvariant)]
    private static partial Regex UidPattern();

    [GeneratedRegex(@"[^\s""'=:,;()\[\]]*\^[^\s""',;()\[\]]*", RegexOptions.CultureInvariant)]
    private static partial Regex PersonNamePattern();

    [GeneratedRegex(@"[\w.+-]+@[\w-]+(?:\.[\w-]+)+", RegexOptions.CultureInvariant)]
    private static partial Regex EmailPattern();

    [GeneratedRegex(@"(?:[A-Za-z]:\\|\\\\|/)(?:[^\s""'<>|]*)", RegexOptions.CultureInvariant)]
    private static partial Regex PathPattern();

    [GeneratedRegex(@"\b\d{5,}\b", RegexOptions.CultureInvariant)]
    private static partial Regex DigitRunPattern();

    /// <summary>Redacts a line.</summary>
    /// <param name="text">Text.</param>
    /// <returns>The redacted text.</returns>
    public static string Redact(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return string.Empty;
        }
        string s = UidPattern().Replace(text, "[uid]");
        s = EmailPattern().Replace(s, "[email]");
        s = PersonNamePattern().Replace(s, "[name]");
        s = PathPattern().Replace(s, m => m.Value.StartsWith("/ws", StringComparison.Ordinal) || m.Value.StartsWith("/api", StringComparison.Ordinal)
            || m.Value.StartsWith("/viewer", StringComparison.Ordinal) || m.Value.StartsWith("/_", StringComparison.Ordinal) || m.Value == "/"
            ? StripQuery(m.Value) : "[path]");
        s = DigitRunPattern().Replace(s, "[n]");
        return s.Replace('\r', ' ').Replace('\n', ' ');
    }

    private static string StripQuery(string v)
    {
        int q = v.IndexOf('?', StringComparison.Ordinal);
        string p = q >= 0 ? v[..q] : v;
        // Route values after /viewer/ are Study Instance UIDs, already replaced by [uid].
        return p;
    }
}

/// <summary>
/// The app's log: one text file per run day, every line through <see cref="PhiRedactor"/>. Toolkit
/// diagnostics flagged as possibly holding patient data are counted, never written.
/// </summary>
public static class AppLog
{
    private static readonly Lock s_gate = new();
    private static StreamWriter? s_writer;
    private static long s_droppedPhi;

    /// <summary>Minimum level written.</summary>
    public static LogLevel MinimumLevel { get; set; } = LogLevel.Information;

    /// <summary>Also echo to standard error.</summary>
    public static bool EchoToConsole { get; set; }

    /// <summary>Toolkit messages dropped because they may hold patient data.</summary>
    public static long DroppedPhiMessages => Interlocked.Read(ref s_droppedPhi);

    /// <summary>Lines written since start (tests read them back through <see cref="RecentLines"/>).</summary>
    public static IReadOnlyList<string> RecentLines
    {
        get
        {
            lock (s_gate)
            {
                return [.. s_recent];
            }
        }
    }

    private static readonly Queue<string> s_recent = new();

    /// <summary>Starts writing to a folder (logs/siaara-viewer-YYYYMMDD.log).</summary>
    /// <param name="folder">Log folder.</param>
    public static void Open(string folder)
    {
        Directory.CreateDirectory(folder);
        string file = Path.Combine(folder, "siaara-viewer-" + DateTime.UtcNow.ToString("yyyyMMdd", CultureInfo.InvariantCulture) + ".log");
        var fs = new FileStream(file, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
        lock (s_gate)
        {
            s_writer?.Dispose();
            s_writer = new StreamWriter(fs, new UTF8Encoding(false)) { AutoFlush = true };
        }
    }

    /// <summary>Routes the toolkit's diagnostics here (PHI-flagged messages are dropped).</summary>
    public static void AttachToolkit() => Tk.SetLogHandler(static (level, phi, msg) =>
    {
        if (phi)
        {
            Interlocked.Increment(ref s_droppedPhi);
            return;
        }
        Write((LogLevel)(int)level, "toolkit", msg);
    });

    /// <summary>Writes a line.</summary>
    /// <param name="level">Level.</param>
    /// <param name="category">Short category.</param>
    /// <param name="message">Message (redacted before writing).</param>
    public static void Write(LogLevel level, string category, string message)
    {
        if (level < MinimumLevel)
        {
            return;
        }
        string line = string.Create(CultureInfo.InvariantCulture,
            $"{DateTime.UtcNow:yyyy-MM-ddTHH:mm:ss.fffZ} {LevelName(level)} {category}: {PhiRedactor.Redact(message)}");
        lock (s_gate)
        {
            s_recent.Enqueue(line);
            while (s_recent.Count > 2000)
            {
                s_recent.Dequeue();
            }
            try
            {
                s_writer?.WriteLine(line);
            }
            catch (IOException)
            {
                // A full disk must not take the viewer down; the line stays in RecentLines.
            }
            if (EchoToConsole)
            {
                Console.Error.WriteLine(line);
            }
        }
    }

    /// <summary>Information.</summary>
    /// <param name="category">Category.</param>
    /// <param name="message">Message.</param>
    public static void Info(string category, string message) => Write(LogLevel.Information, category, message);

    /// <summary>Warning.</summary>
    /// <param name="category">Category.</param>
    /// <param name="message">Message.</param>
    public static void Warn(string category, string message) => Write(LogLevel.Warning, category, message);

    /// <summary>Error with the exception type and message (redacted).</summary>
    /// <param name="category">Category.</param>
    /// <param name="message">Message.</param>
    /// <param name="exception">Exception or null.</param>
    public static void Error(string category, string message, Exception? exception = null) =>
        Write(LogLevel.Error, category, exception is null ? message : message + " (" + exception.GetType().Name + ": " + exception.Message + ")");

    /// <summary>Stops writing.</summary>
    public static void Close()
    {
        lock (s_gate)
        {
            s_writer?.Dispose();
            s_writer = null;
        }
    }

    private static string LevelName(LogLevel l) => l switch
    {
        LogLevel.Debug => "DBG",
        LogLevel.Information => "INF",
        LogLevel.Warning => "WRN",
        _ => "ERR",
    };
}
