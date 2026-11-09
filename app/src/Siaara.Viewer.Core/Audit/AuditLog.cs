// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Siaara.Viewer.Core.Audit;

/// <summary>Result of checking the audit chain.</summary>
/// <param name="IsIntact">Every line links to the hash of the line before it.</param>
/// <param name="Lines">Lines read.</param>
/// <param name="FirstBadLine">1-based number of the first broken line, or 0.</param>
public sealed record AuditVerification(bool IsIntact, int Lines, int FirstBadLine);

/// <summary>
/// Append-only, hash-chained audit trail (JSON lines). Each line carries "prev": the SHA-256 (hex)
/// of the previous line's UTF-8 bytes (64 zeros for the first line), so removing or editing a line
/// breaks the chain at that point. Entries name the patient by the store's patient key; no other
/// dataset values are written.
/// </summary>
public sealed class AuditLog : IDisposable
{
    private static readonly string s_genesis = new('0', 64);
    private readonly Lock _gate = new();
    private readonly FileStream _stream;
    private string _prevHash;
    private long _seq;

    /// <summary>Opens (or creates) the log and continues its chain.</summary>
    /// <param name="path">audit.jsonl.</param>
    public AuditLog(string path)
    {
        FilePath = path;
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".");
        (_prevHash, _seq) = ReadTail(path);
        _stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read);
    }

    /// <summary>The log file.</summary>
    public string FilePath { get; }

    /// <summary>User name recorded with each entry.</summary>
    public string User { get; init; } = Environment.UserName;

    /// <summary>Appends an entry.</summary>
    /// <param name="eventName">Event, e.g. "study.open".</param>
    /// <param name="fields">Event data (no dataset values other than the patient key and UIDs).</param>
    public void Append(string eventName, IReadOnlyDictionary<string, string>? fields = null)
    {
        lock (_gate)
        {
            var buffer = new MemoryStream();
            using (var w = new Utf8JsonWriter(buffer))
            {
                w.WriteStartObject();
                w.WriteNumber("seq", _seq + 1);
                w.WriteString("ts", DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture));
                w.WriteString("event", eventName);
                w.WriteString("user", User);
                w.WriteStartObject("data");
                if (fields is not null)
                {
                    foreach (KeyValuePair<string, string> kv in fields.OrderBy(k => k.Key, StringComparer.Ordinal))
                    {
                        w.WriteString(kv.Key, kv.Value);
                    }
                }
                w.WriteEndObject();
                w.WriteString("prev", _prevHash);
                w.WriteEndObject();
            }
            byte[] line = buffer.ToArray();
            _stream.Write(line);
            _stream.WriteByte((byte)'\n');
            _stream.Flush(flushToDisk: true);
            _prevHash = Hash(line);
            _seq++;
        }
    }

    /// <summary>Checks the whole chain of a log file.</summary>
    /// <param name="path">File.</param>
    /// <returns>The result.</returns>
    public static AuditVerification Verify(string path)
    {
        if (!File.Exists(path))
        {
            return new AuditVerification(true, 0, 0);
        }
        string prev = s_genesis;
        int n = 0;
        foreach (string line in File.ReadLines(path, Encoding.UTF8))
        {
            if (line.Length == 0)
            {
                continue;
            }
            n++;
            string? linked;
            try
            {
                using var doc = JsonDocument.Parse(line);
                linked = doc.RootElement.TryGetProperty("prev", out JsonElement p) ? p.GetString() : null;
            }
            catch (JsonException)
            {
                return new AuditVerification(false, n, n);
            }
            if (!string.Equals(linked, prev, StringComparison.Ordinal))
            {
                return new AuditVerification(false, n, n);
            }
            prev = Hash(Encoding.UTF8.GetBytes(line));
        }
        return new AuditVerification(true, n, 0);
    }

    /// <summary>Event names in the file, in order (for tests and the about page).</summary>
    /// <param name="path">File.</param>
    /// <returns>Event names.</returns>
    public static IReadOnlyList<string> ReadEvents(string path)
    {
        var list = new List<string>();
        if (!File.Exists(path))
        {
            return list;
        }
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var r = new StreamReader(fs, Encoding.UTF8);
        while (r.ReadLine() is string line)
        {
            if (line.Length == 0)
            {
                continue;
            }
            using var doc = JsonDocument.Parse(line);
            list.Add(doc.RootElement.GetProperty("event").GetString() ?? "");
        }
        return list;
    }

    private static string Hash(ReadOnlySpan<byte> line) => Convert.ToHexStringLower(SHA256.HashData(line));

    private static (string Hash, long Seq) ReadTail(string path)
    {
        if (!File.Exists(path))
        {
            return (s_genesis, 0);
        }
        string? last = null;
        long count = 0;
        foreach (string line in File.ReadLines(path, Encoding.UTF8))
        {
            if (line.Length > 0)
            {
                last = line;
                count++;
            }
        }
        if (last is null)
        {
            return (s_genesis, 0);
        }
        long seq = count;
        try
        {
            using var doc = JsonDocument.Parse(last);
            if (doc.RootElement.TryGetProperty("seq", out JsonElement s) && s.TryGetInt64(out long v))
            {
                seq = v;
            }
        }
        catch (JsonException)
        {
            // A torn last line still chains: the next entry links to its bytes as written.
        }
        return (Hash(Encoding.UTF8.GetBytes(last)), seq);
    }

    /// <inheritdoc/>
    public void Dispose() => _stream.Dispose();
}
