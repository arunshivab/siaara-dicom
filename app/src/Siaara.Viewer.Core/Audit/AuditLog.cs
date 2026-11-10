// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Core.Settings;

namespace Siaara.Viewer.Core.Audit;

/// <summary>Values of the audit "outcome" field.</summary>
public static class AuditOutcome
{
    /// <summary>The action completed.</summary>
    public const string Success = "success";

    /// <summary>The action failed (the entry's "reason" field says why, in plain words).</summary>
    public const string Failure = "failure";

    /// <summary>The user stopped the action before it completed.</summary>
    public const string Cancelled = "cancelled";

    /// <summary>The action is about to start (written before a destructive action; its outcome follows).</summary>
    public const string Intent = "intent";
}

/// <summary>An entry that had to be written could not be: the action it belongs to must not run.</summary>
public sealed class AuditUnavailableException : Exception
{
    /// <summary>Creates the exception.</summary>
    public AuditUnavailableException()
        : base("The audit log could not be written.")
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Message.</param>
    public AuditUnavailableException(string message)
        : base(message)
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Message.</param>
    /// <param name="inner">Cause.</param>
    public AuditUnavailableException(string message, Exception inner)
        : base(message, inner)
    {
    }
}

/// <summary>Result of checking the audit trail.</summary>
/// <param name="IsIntact">Every line is signed with the audit key, links to the line before it, the numbers have no gap and the anchor agrees with the end.</param>
/// <param name="Lines">Signed lines read (all files).</param>
/// <param name="FirstBadLine">1-based number of the first line that fails (counted over all files in order), or 0.</param>
/// <param name="Problem">What is wrong, in plain words, or null.</param>
/// <param name="Recovered">Torn fragments (a crash in the middle of a write) that a later "audit.recovered" entry accounts for.</param>
/// <param name="Files">Files read (rotated ones and the current one).</param>
public sealed record AuditVerification(bool IsIntact, int Lines, int FirstBadLine, string? Problem = null, int Recovered = 0, int Files = 0);

/// <summary>An entry as read back.</summary>
/// <param name="Seq">Sequence number.</param>
/// <param name="Event">Event name.</param>
/// <param name="Outcome">Outcome.</param>
/// <param name="Reason">Reason or empty.</param>
/// <param name="Data">Data fields.</param>
public sealed record AuditEntry(long Seq, string Event, string Outcome, string Reason, IReadOnlyDictionary<string, string> Data);

/// <summary>
/// Append-only audit trail (JSON lines). Each line carries "seq" (1, 2, 3, ...), "prev" (the "mac"
/// of the line before it; 64 zeros for the first) and "mac": HMAC-SHA256, with the audit key
/// (<see cref="AuditKeys"/>), of the line without its "mac" member. Without the key nobody can
/// change, add or reorder a line unnoticed. The newest sequence number and mac are also kept in a
/// small owner-only anchor file (audit.anchor, itself signed), so cutting lines off the end is
/// detected. A crash in the middle of a write leaves a torn fragment; the next start ends it with
/// a line break and writes "audit.recovered" with the fragment's SHA-256, which the check accepts.
/// The file is rotated by size (audit.NNNNNNNNNNNN.jsonl, named after its last sequence number)
/// and the chain continues across files. Opening reads only the anchor and the end of the file.
/// Entries name the patient by the store's patient key; no other dataset values are written, and
/// folders are written as a keyed hash plus their depth (<see cref="PathReference"/>).
/// A log in the format of earlier versions (no "mac") is kept aside as audit.legacy-*.jsonl and
/// its SHA-256 is the first thing recorded in the new trail.
/// </summary>
public sealed class AuditLog : IDisposable
{
    /// <summary>Event name of a security alert.</summary>
    public const string SecurityAlert = "security.alert";

    private const string RecoveredEvent = "audit.recovered";
    private const int MacSuffixLength = 8 + 64 + 2; // ,"mac":"<64 hex>"}
    private static readonly string s_genesis = new('0', 64);
    private static readonly byte[] s_macPrefix = Encoding.ASCII.GetBytes(",\"mac\":\"");
    private readonly Lock _gate = new();
    private readonly byte[] _key;
    private readonly string _folder;
    private FileStream _stream;
    private string _prevMac = s_genesis;
    private long _seq;
    private bool _disposed;
    private bool _unsettled;

    /// <summary>Opens (or creates) the log with the key from the OS key store and continues its chain.</summary>
    /// <param name="path">audit.jsonl.</param>
    public AuditLog(string path)
        : this(path, AuditKeys.OsStore)
    {
    }

    /// <summary>Opens (or creates) the log and continues its chain.</summary>
    /// <param name="path">audit.jsonl.</param>
    /// <param name="keys">Key source.</param>
    /// <param name="maxFileBytes">Rotate when the file reaches this size (default 16 MB).</param>
    public AuditLog(string path, IAuditKeySource keys, long maxFileBytes = 16L * 1024 * 1024)
    {
        ArgumentNullException.ThrowIfNull(keys);
        FilePath = Path.GetFullPath(path);
        MaxFileBytes = Math.Max(1024, maxFileBytes);
        _folder = Path.GetDirectoryName(FilePath) ?? ".";
        OwnerOnly.CreateDirectory(_folder);
        AuditKey key = keys.Load(_folder, create: true)!;
        _key = key.Key.ToArray();
        KeyProtection = key.Protection;
        var notes = new List<(string Event, string Outcome, Dictionary<string, string> Data, string? Reason)>();
        if (key.Created && (File.Exists(FilePath) && new FileInfo(FilePath).Length > 0 || RotatedFiles(_folder).Count > 0) && !IsLegacy(FilePath))
        {
            // Lines exist but their key is gone: they cannot be checked any more. Keep them aside.
            SetAside("unverifiable", notes, "the audit key was missing; a new key was made");
        }
        if (IsLegacy(FilePath))
        {
            SetAside("legacy", notes, null);
        }
        _stream = OwnerOnly.OpenAppend(FilePath);
        try
        {
            Resume(notes);
            foreach ((string ev, string outcome, Dictionary<string, string> data, string? reason) in notes)
            {
                WriteEntry(ev, outcome, data, reason);
            }
        }
        catch
        {
            _stream.Dispose();
            throw;
        }
    }

    /// <summary>The current log file.</summary>
    public string FilePath { get; }

    /// <summary>Size at which the file is rotated.</summary>
    public long MaxFileBytes { get; }

    /// <summary>How the key is protected on this computer.</summary>
    public AuditKeyProtection KeyProtection { get; }

    /// <summary>User name recorded with each entry.</summary>
    public string User { get; init; } = Environment.UserName;

    /// <summary>Sequence number of the newest entry.</summary>
    public long LastSequence
    {
        get
        {
            lock (_gate)
            {
                return _seq;
            }
        }
    }

    /// <summary>The anchor file next to a log file.</summary>
    /// <param name="logPath">audit.jsonl.</param>
    /// <returns>audit.anchor.</returns>
    public static string AnchorPath(string logPath) => Path.Combine(Path.GetDirectoryName(Path.GetFullPath(logPath)) ?? ".", "audit.anchor");

    /// <summary>
    /// Appends an entry. A failure to write (disk full, file gone) is logged in the app log and
    /// reported by the return value; it never throws, so an audit problem cannot take down an action
    /// that already happened. Use <see cref="AppendRequired"/> before an action that must not run unaudited.
    /// </summary>
    /// <param name="eventName">Event, e.g. "study.open".</param>
    /// <param name="outcome">An <see cref="AuditOutcome"/> value.</param>
    /// <param name="fields">Event data (no dataset values other than the patient key and UIDs).</param>
    /// <param name="reason">Why it failed (plain words, never dataset values), or null.</param>
    /// <returns>True when the entry is on disk.</returns>
    public bool Append(string eventName, string outcome, IReadOnlyDictionary<string, string>? fields = null, string? reason = null)
    {
        try
        {
            AppendRequired(eventName, outcome, fields, reason);
            return true;
        }
        catch (AuditUnavailableException e)
        {
            AppLog.Error("audit", "an audit entry could not be written (" + eventName + ")", e.InnerException ?? e);
            return false;
        }
    }

    /// <summary>Appends an entry or throws <see cref="AuditUnavailableException"/>: for the intent of a destructive action.</summary>
    /// <param name="eventName">Event.</param>
    /// <param name="outcome">An <see cref="AuditOutcome"/> value.</param>
    /// <param name="fields">Event data.</param>
    /// <param name="reason">Reason or null.</param>
    public void AppendRequired(string eventName, string outcome, IReadOnlyDictionary<string, string>? fields = null, string? reason = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(eventName);
        ArgumentException.ThrowIfNullOrEmpty(outcome);
        lock (_gate)
        {
            try
            {
                ObjectDisposedException.ThrowIf(_disposed, this);
                if (_unsettled)
                {
                    // An earlier write failed part-way: read the end of the file again and repair it first.
                    Reopen();
                }
                _unsettled = true;
                WriteEntry(eventName, outcome, fields, reason);
                if (_stream.Length >= MaxFileBytes)
                {
                    Rotate();
                }
                _unsettled = false;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ObjectDisposedException or NotSupportedException)
            {
                throw new AuditUnavailableException("The audit log could not be written.", e);
            }
        }
    }

    /// <summary>Writes a security alert ("security.alert", outcome failure) and a warning in the app log.</summary>
    /// <param name="kind">What happened, e.g. "launch-reused", "peer-refused".</param>
    /// <param name="fields">More data (never secrets or dataset values).</param>
    /// <returns>True when the entry is on disk.</returns>
    public bool Alert(string kind, IReadOnlyDictionary<string, string>? fields = null)
    {
        var data = new Dictionary<string, string>(StringComparer.Ordinal) { ["kind"] = kind };
        if (fields is not null)
        {
            foreach (KeyValuePair<string, string> kv in fields)
            {
                data[kv.Key] = kv.Value;
            }
        }
        AppLog.Warn("security", "security alert: " + kind);
        return Append(SecurityAlert, AuditOutcome.Failure, data, kind);
    }

    /// <summary>
    /// How a folder is written to the audit log (R-142): never the path itself, because folder names
    /// often hold patient names. The value is "p:" + the first 32 hex digits of HMAC-SHA256(audit
    /// key, "path:" + full path) + "/d" + the number of path components. With the key (the owner of
    /// the log) a suspected folder can be tested against an entry; without it nothing can be guessed.
    /// </summary>
    /// <param name="path">Folder or file.</param>
    /// <returns>The reference.</returns>
    public string PathReference(string? path)
    {
        if (string.IsNullOrEmpty(path))
        {
            return "";
        }
        string full;
        try
        {
            full = Path.GetFullPath(path);
        }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException)
        {
            full = path;
        }
        int depth = full.Split([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar], StringSplitOptions.RemoveEmptyEntries).Length;
        string mac = Convert.ToHexStringLower(HMACSHA256.HashData(_key, Encoding.UTF8.GetBytes("path:" + full)));
        return "p:" + mac[..32] + "/d" + depth.ToString(CultureInfo.InvariantCulture);
    }

    // ---- writing --------------------------------------------------------------------------------

    private void WriteEntry(string eventName, string outcome, IReadOnlyDictionary<string, string>? fields, string? reason)
    {
        var buffer = new MemoryStream();
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            w.WriteNumber("seq", _seq + 1);
            w.WriteString("ts", DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture));
            w.WriteString("event", eventName);
            w.WriteString("user", User);
            w.WriteString("outcome", outcome);
            if (!string.IsNullOrEmpty(reason))
            {
                w.WriteString("reason", reason);
            }
            w.WriteStartObject("data");
            if (fields is not null)
            {
                foreach (KeyValuePair<string, string> kv in fields.OrderBy(k => k.Key, StringComparer.Ordinal))
                {
                    w.WriteString(kv.Key, kv.Value);
                }
            }
            w.WriteEndObject();
            w.WriteString("prev", _prevMac);
            w.WriteEndObject();
        }
        byte[] body = buffer.ToArray();
        string mac = Convert.ToHexStringLower(HMACSHA256.HashData(_key, body));
        // body is {...}: the line is the same object with "mac" as its last member.
        var line = new byte[body.Length - 1 + MacSuffixLength + 1];
        body.AsSpan(0, body.Length - 1).CopyTo(line);
        int at = body.Length - 1;
        s_macPrefix.CopyTo(line.AsSpan(at));
        at += s_macPrefix.Length;
        at += Encoding.ASCII.GetBytes(mac, line.AsSpan(at));
        line[at++] = (byte)'"';
        line[at++] = (byte)'}';
        line[at] = (byte)'\n';
        // One write call for the whole line, then to the disk, then the anchor.
        _stream.Write(line);
        _stream.Flush(flushToDisk: true);
        _prevMac = mac;
        _seq++;
        WriteAnchor();
    }

    private void WriteAnchor()
    {
        string seq = _seq.ToString(CultureInfo.InvariantCulture);
        string tag = AnchorTag(_key, seq, _prevMac);
        AtomicFile.WriteAllBytes(AnchorPath(FilePath), Encoding.ASCII.GetBytes("{\"seq\":" + seq + ",\"mac\":\"" + _prevMac + "\",\"tag\":\"" + tag + "\"}\n"));
    }

    private static string AnchorTag(byte[] key, string seq, string mac) =>
        Convert.ToHexStringLower(HMACSHA256.HashData(key, Encoding.ASCII.GetBytes("anchor:" + seq + ":" + mac)));

    private void Reopen()
    {
        _stream.Dispose();
        _stream = OwnerOnly.OpenAppend(FilePath);
        _seq = 0;
        _prevMac = s_genesis;
        var notes = new List<(string Event, string Outcome, Dictionary<string, string> Data, string? Reason)>();
        Resume(notes);
        foreach ((string ev, string outcome, Dictionary<string, string> data, string? reason) in notes)
        {
            WriteEntry(ev, outcome, data, reason);
        }
    }

    private void Rotate()
    {
        string name = "audit." + _seq.ToString("D12", CultureInfo.InvariantCulture) + ".jsonl";
        _stream.Dispose();
        File.Move(FilePath, Path.Combine(_folder, name), overwrite: false);
        _stream = OwnerOnly.OpenAppend(FilePath);
        WriteEntry("audit.rotated", AuditOutcome.Success, new Dictionary<string, string> { ["previous"] = name }, null);
    }

    // ---- opening --------------------------------------------------------------------------------

    private static bool IsLegacy(string path)
    {
        if (!File.Exists(path))
        {
            return false;
        }
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        foreach (byte[] line in Lines(fs))
        {
            if (line.Length == 0)
            {
                continue;
            }
            // The first line of the earlier format is a JSON object with "prev" and no "mac".
            if (SplitMac(line, out _, out _))
            {
                return false;
            }
            try
            {
                using var doc = JsonDocument.Parse(line);
                return doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("prev", out _)
                    && !doc.RootElement.TryGetProperty("mac", out _);
            }
            catch (JsonException)
            {
                return false;
            }
        }
        return false;
    }

    private void SetAside(string what, List<(string, string, Dictionary<string, string>, string?)> notes, string? reason)
    {
        string stamp = DateTime.UtcNow.ToString("yyyyMMddHHmmssfff", CultureInfo.InvariantCulture);
        var files = new List<string>(RotatedFiles(_folder));
        if (File.Exists(FilePath))
        {
            files.Add(FilePath);
        }
        foreach (string f in files)
        {
            string aside = Path.Combine(_folder, "audit." + what + "-" + stamp + "-" + Path.GetFileNameWithoutExtension(f) + ".jsonl.old");
            string sha;
            using (FileStream fs = File.OpenRead(f))
            {
                sha = Convert.ToHexStringLower(SHA256.HashData(fs));
            }
            File.Move(f, aside);
            notes.Add((what == "legacy" ? "audit.migrated" : SecurityAlert, what == "legacy" ? AuditOutcome.Success : AuditOutcome.Failure,
                new Dictionary<string, string> { ["kind"] = "audit-" + what, ["file"] = Path.GetFileName(aside), ["sha256"] = sha }, reason));
        }
        File.Delete(AnchorPath(FilePath));
    }

    // Reads the anchor and the end of the newest file; repairs a torn end; records what does not add up.
    private void Resume(List<(string Event, string Outcome, Dictionary<string, string> Data, string? Reason)> notes)
    {
        (long anchorSeq, string anchorMac, bool anchorOk, bool anchorPresent) = ReadAnchor(FilePath, _key);
        string tailFile = FilePath;
        if (_stream.Length == 0)
        {
            // Nothing in the current file: the chain continues from the newest rotated file, if any.
            IReadOnlyList<string> rotated = RotatedFiles(_folder);
            if (rotated.Count == 0)
            {
                if (anchorPresent && anchorOk && anchorSeq > 0)
                {
                    _seq = anchorSeq;
                    _prevMac = s_genesis;
                    notes.Add((SecurityAlert, AuditOutcome.Failure, Gap("audit-truncated", anchorSeq, 0), "audit entries are missing from the end of the log"));
                }
                else if (anchorPresent && !anchorOk)
                {
                    notes.Add((SecurityAlert, AuditOutcome.Failure, new Dictionary<string, string> { ["kind"] = "audit-anchor-invalid" }, "the audit anchor is not signed with the audit key"));
                }
                return;
            }
            tailFile = rotated[^1];
        }
        Tail tail = ReadTail(tailFile, _key);
        _seq = tail.Seq;
        _prevMac = tail.Mac;
        if (tail.HasJunk || tail.MissingNewline)
        {
            if (tailFile != FilePath)
            {
                // A rotated file is never written again; its damage is reported, not repaired.
                notes.Add((SecurityAlert, AuditOutcome.Failure, new Dictionary<string, string> { ["kind"] = "audit-rotated-file-damaged", ["file"] = Path.GetFileName(tailFile) },
                    "a rotated audit file does not end with a signed line"));
            }
            else
            {
                if (tail.MissingNewline)
                {
                    _stream.WriteByte((byte)'\n');
                    _stream.Flush(flushToDisk: true);
                }
                if (tail.HasJunk)
                {
                    bool afterAnchor = !(anchorOk && anchorSeq > tail.Seq);
                    if (!afterAnchor)
                    {
                        // The anchor knows a newer entry than the last signed line: lines were lost or damaged.
                        _seq = anchorSeq;
                    }
                    notes.Add((afterAnchor ? RecoveredEvent : SecurityAlert, afterAnchor ? AuditOutcome.Success : AuditOutcome.Failure,
                        new Dictionary<string, string>
                        {
                            ["kind"] = afterAnchor ? "torn-write" : "audit-damaged",
                            ["tornBytes"] = tail.JunkBytes.ToString(CultureInfo.InvariantCulture),
                            ["tornSha256"] = tail.JunkSha256,
                            ["anchorSeq"] = anchorSeq.ToString(CultureInfo.InvariantCulture),
                            ["lastSignedSeq"] = tail.Seq.ToString(CultureInfo.InvariantCulture),
                        }, afterAnchor ? null : "signed audit entries were damaged after they were written"));
                    return;
                }
            }
        }
        if (!anchorPresent)
        {
            if (_seq > 0)
            {
                notes.Add((SecurityAlert, AuditOutcome.Failure, new Dictionary<string, string> { ["kind"] = "audit-anchor-missing" }, "the audit anchor file is missing"));
            }
        }
        else if (!anchorOk)
        {
            notes.Add((SecurityAlert, AuditOutcome.Failure, new Dictionary<string, string> { ["kind"] = "audit-anchor-invalid" }, "the audit anchor is not signed with the audit key"));
        }
        else if (anchorSeq > _seq)
        {
            notes.Add((SecurityAlert, AuditOutcome.Failure, Gap("audit-truncated", anchorSeq, _seq), "audit entries are missing from the end of the log"));
            _seq = anchorSeq;
        }
    }

    private static Dictionary<string, string> Gap(string kind, long anchorSeq, long fileSeq) => new()
    {
        ["kind"] = kind,
        ["anchorSeq"] = anchorSeq.ToString(CultureInfo.InvariantCulture),
        ["lastSignedSeq"] = fileSeq.ToString(CultureInfo.InvariantCulture),
    };

    private static (long Seq, string Mac, bool Ok, bool Present) ReadAnchor(string logPath, byte[] key)
    {
        string path = AnchorPath(logPath);
        if (!File.Exists(path))
        {
            return (0, s_genesis, false, false);
        }
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
            JsonElement r = doc.RootElement;
            if (r.ValueKind == JsonValueKind.Object && r.TryGetProperty("seq", out JsonElement s) && s.TryGetInt64(out long seq)
                && r.TryGetProperty("mac", out JsonElement m) && m.ValueKind == JsonValueKind.String
                && r.TryGetProperty("tag", out JsonElement t) && t.ValueKind == JsonValueKind.String)
            {
                string mac = m.GetString() ?? "";
                bool ok = CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(AnchorTag(key, seq.ToString(CultureInfo.InvariantCulture), mac)),
                    Encoding.ASCII.GetBytes(t.GetString() ?? ""));
                return (seq, mac, ok, true);
            }
        }
        catch (Exception e) when (e is JsonException or IOException)
        {
            // Reported as an invalid anchor.
        }
        return (0, s_genesis, false, true);
    }

    // HasJunk: bytes follow the last signed line. Their content (without one final line break) is what JunkBytes / JunkSha256 describe.
    private readonly record struct Tail(long Seq, string Mac, bool HasJunk, long JunkBytes, string JunkSha256, bool MissingNewline);

    // The last signed line of a file, and what follows it (a torn fragment), reading only the end.
    private static Tail ReadTail(string path, byte[] key)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        long length = fs.Length;
        if (length == 0)
        {
            return new Tail(0, s_genesis, false, 0, "", false);
        }
        long window = Math.Min(length, 64 * 1024);
        while (true)
        {
            var buf = new byte[window];
            fs.Position = length - window;
            fs.ReadExactly(buf);
            bool missingNewline = buf[^1] != (byte)'\n';
            // Walk the lines of the window from the end; the first one in a partial window may be cut.
            int end = buf.Length;
            while (end > 0)
            {
                int contentEnd = end > 0 && buf[end - 1] == (byte)'\n' ? end - 1 : end;
                int start = contentEnd == 0 ? 0 : buf.AsSpan(0, contentEnd).LastIndexOf((byte)'\n') + 1;
                bool whole = start > 0 || window == length;
                if (whole && contentEnd > start && TryReadSigned(buf.AsSpan(start, contentEnd - start), key, out long seq, out _, out string mac, out _, out _))
                {
                    int junkStart = end;
                    bool hasJunk = junkStart < buf.Length;
                    int junkEnd = missingNewline ? buf.Length : buf.Length - 1;
                    ReadOnlySpan<byte> junk = junkEnd > junkStart ? buf.AsSpan(junkStart, junkEnd - junkStart) : [];
                    return new Tail(seq, mac, hasJunk, junk.Length, hasJunk ? Convert.ToHexStringLower(SHA256.HashData(junk)) : "", missingNewline);
                }
                if (!whole)
                {
                    break;
                }
                end = start;
            }
            if (window == length)
            {
                // No signed line at all: everything is junk.
                ReadOnlySpan<byte> all = missingNewline ? buf : buf.AsSpan(0, buf.Length - 1);
                return new Tail(0, s_genesis, true, all.Length, Convert.ToHexStringLower(SHA256.HashData(all)), missingNewline);
            }
            window = Math.Min(length, window * 4);
        }
    }

    // ---- checking -------------------------------------------------------------------------------

    private static bool SplitMac(ReadOnlySpan<byte> line, out ReadOnlySpan<byte> bodyWithoutBrace, out ReadOnlySpan<byte> mac)
    {
        bodyWithoutBrace = default;
        mac = default;
        if (line.Length <= MacSuffixLength + 1 || line[0] != (byte)'{' || line[^1] != (byte)'}' || line[^2] != (byte)'"')
        {
            return false;
        }
        ReadOnlySpan<byte> suffix = line[^MacSuffixLength..];
        if (!suffix.StartsWith(s_macPrefix))
        {
            return false;
        }
        mac = suffix.Slice(s_macPrefix.Length, 64);
        bodyWithoutBrace = line[..^MacSuffixLength];
        return true;
    }

    // A line signed with the key: its numbers and names. False for anything else.
    private static bool TryReadSigned(ReadOnlySpan<byte> line, byte[] key, out long seq, out string prev, out string mac, out string eventName, out string tornSha)
    {
        seq = 0;
        prev = mac = eventName = tornSha = "";
        if (!SplitMac(line, out ReadOnlySpan<byte> head, out ReadOnlySpan<byte> macHex))
        {
            return false;
        }
        var body = new byte[head.Length + 1];
        head.CopyTo(body);
        body[^1] = (byte)'}';
        Span<byte> expected = stackalloc byte[64];
        Encoding.ASCII.GetBytes(Convert.ToHexStringLower(HMACSHA256.HashData(key, body)), expected);
        if (!CryptographicOperations.FixedTimeEquals(expected, macHex))
        {
            return false;
        }
        try
        {
            using var doc = JsonDocument.Parse(body);
            JsonElement r = doc.RootElement;
            if (!r.TryGetProperty("seq", out JsonElement s) || !s.TryGetInt64(out seq)
                || !r.TryGetProperty("prev", out JsonElement p) || p.ValueKind != JsonValueKind.String
                || !r.TryGetProperty("event", out JsonElement e) || e.ValueKind != JsonValueKind.String)
            {
                return false;
            }
            prev = p.GetString() ?? "";
            eventName = e.GetString() ?? "";
            if (r.TryGetProperty("data", out JsonElement d) && d.ValueKind == JsonValueKind.Object
                && d.TryGetProperty("tornSha256", out JsonElement t) && t.ValueKind == JsonValueKind.String)
            {
                tornSha = t.GetString() ?? "";
            }
        }
        catch (JsonException)
        {
            return false;
        }
        mac = Encoding.ASCII.GetString(macHex);
        return true;
    }

    /// <summary>Rotated files of a log folder, oldest first.</summary>
    /// <param name="folder">Folder.</param>
    /// <returns>Paths.</returns>
    public static IReadOnlyList<string> RotatedFiles(string folder)
    {
        if (!Directory.Exists(folder))
        {
            return [];
        }
        return [.. Directory.GetFiles(folder, "audit.*.jsonl")
            .Where(f => Path.GetFileName(f) is { Length: 24 } n && n.AsSpan(6, 12).IndexOfAnyExceptInRange('0', '9') < 0)
            .OrderBy(f => Path.GetFileName(f), StringComparer.Ordinal)];
    }

    /// <summary>Checks the whole trail of a log (rotated files, the current file and the anchor) with the key from the OS key store.</summary>
    /// <param name="path">audit.jsonl.</param>
    /// <returns>The result.</returns>
    public static AuditVerification Verify(string path) => Verify(path, AuditKeys.OsStore);

    /// <summary>Checks the whole trail of a log (rotated files, the current file and the anchor).</summary>
    /// <param name="path">audit.jsonl.</param>
    /// <param name="keys">Key source.</param>
    /// <returns>The result.</returns>
    public static AuditVerification Verify(string path, IAuditKeySource keys)
    {
        ArgumentNullException.ThrowIfNull(keys);
        string full = Path.GetFullPath(path);
        string folder = Path.GetDirectoryName(full) ?? ".";
        var files = new List<string>(RotatedFiles(folder));
        if (File.Exists(full))
        {
            files.Add(full);
        }
        bool anyBytes = files.Any(f => new FileInfo(f).Length > 0);
        if (!anyBytes && !File.Exists(AnchorPath(full)))
        {
            return new AuditVerification(true, 0, 0, null, 0, files.Count);
        }
        AuditKey? key;
        try
        {
            key = keys.Load(folder, create: false);
        }
        catch (CryptographicException)
        {
            key = null;
        }
        if (key is null)
        {
            return new AuditVerification(false, 0, 1, "the audit key is missing or unreadable", 0, files.Count);
        }
        byte[] keyBytes = key.Key.ToArray();
        string prev = s_genesis;
        long seq = 0;
        int n = 0, recovered = 0;
        using var junk = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        bool inJunk = false;
        int junkLine = 0;
        foreach (string file in files)
        {
            using var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            foreach (byte[] line in Lines(fs))
            {
                if (!TryReadSigned(line, keyBytes, out long s, out string p, out string mac, out string ev, out string tornSha))
                {
                    // Not a signed line: acceptable only as a torn fragment that the next entry accounts for.
                    if (inJunk)
                    {
                        junk.AppendData("\n"u8);
                    }
                    else
                    {
                        inJunk = true;
                        junkLine = n + 1;
                    }
                    junk.AppendData(line);
                    continue;
                }
                n++;
                if (inJunk)
                {
                    string sha = Convert.ToHexStringLower(junk.GetHashAndReset());
                    inJunk = false;
                    if (ev != RecoveredEvent || !string.Equals(tornSha, sha, StringComparison.Ordinal))
                    {
                        return new AuditVerification(false, n, junkLine, "a line is not signed with the audit key (changed, or written by something else)", recovered, files.Count);
                    }
                    recovered++;
                }
                if (s != seq + 1)
                {
                    return new AuditVerification(false, n, n, s > seq + 1 ? "entries are missing before this line" : "the sequence numbers go backwards", recovered, files.Count);
                }
                if (!string.Equals(p, prev, StringComparison.Ordinal))
                {
                    return new AuditVerification(false, n, n, "this line does not follow the line before it", recovered, files.Count);
                }
                seq = s;
                prev = mac;
            }
        }
        if (inJunk)
        {
            return new AuditVerification(false, n, n + 1, "the log ends with an unsigned or torn line", recovered, files.Count);
        }
        (long anchorSeq, string anchorMac, bool anchorOk, bool anchorPresent) = ReadAnchor(full, keyBytes);
        if (!anchorPresent)
        {
            return new AuditVerification(false, n, n + 1, "the anchor file is missing, so missing entries at the end cannot be ruled out", recovered, files.Count);
        }
        if (!anchorOk)
        {
            return new AuditVerification(false, n, n + 1, "the anchor file is not signed with the audit key", recovered, files.Count);
        }
        if (anchorSeq > seq)
        {
            return new AuditVerification(false, n, n + 1, "entries are missing from the end of the log (the anchor is ahead of the last line)", recovered, files.Count);
        }
        if (anchorSeq == seq && !string.Equals(anchorMac, prev, StringComparison.Ordinal))
        {
            return new AuditVerification(false, n, n, "the last line is not the one the anchor recorded", recovered, files.Count);
        }
        return new AuditVerification(true, n, 0, null, recovered, files.Count);
    }

    // The lines of a stream as bytes (without the line break); the last one also when no line break ends it.
    private static IEnumerable<byte[]> Lines(Stream s)
    {
        var line = new MemoryStream();
        var buf = new byte[64 * 1024];
        int read;
        while ((read = s.Read(buf, 0, buf.Length)) > 0)
        {
            int start = 0;
            for (int i = 0; i < read; i++)
            {
                if (buf[i] == (byte)'\n')
                {
                    line.Write(buf, start, i - start);
                    yield return line.ToArray();
                    line.SetLength(0);
                    start = i + 1;
                }
            }
            line.Write(buf, start, read - start);
        }
        if (line.Length > 0)
        {
            yield return line.ToArray();
        }
    }

    /// <summary>Event names of the trail in order, over rotated files and the current file (tests and the about page).</summary>
    /// <param name="path">audit.jsonl.</param>
    /// <returns>Event names.</returns>
    public static IReadOnlyList<string> ReadEvents(string path) => [.. ReadEntries(path).Select(e => e.Event)];

    /// <summary>The parseable entries of the trail in order (no signature check; see <see cref="Verify(string)"/>).</summary>
    /// <param name="path">audit.jsonl.</param>
    /// <returns>Entries.</returns>
    public static IReadOnlyList<AuditEntry> ReadEntries(string path)
    {
        string full = Path.GetFullPath(path);
        var files = new List<string>(RotatedFiles(Path.GetDirectoryName(full) ?? "."));
        if (File.Exists(full))
        {
            files.Add(full);
        }
        var list = new List<AuditEntry>();
        foreach (string file in files)
        {
            using var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            foreach (byte[] line in Lines(fs))
            {
                if (line.Length == 0)
                {
                    continue;
                }
                try
                {
                    using var doc = JsonDocument.Parse(line);
                    JsonElement r = doc.RootElement;
                    var data = new Dictionary<string, string>(StringComparer.Ordinal);
                    if (r.TryGetProperty("data", out JsonElement d) && d.ValueKind == JsonValueKind.Object)
                    {
                        foreach (JsonProperty p in d.EnumerateObject())
                        {
                            data[p.Name] = p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString() ?? "" : p.Value.GetRawText();
                        }
                    }
                    list.Add(new AuditEntry(r.TryGetProperty("seq", out JsonElement s) && s.TryGetInt64(out long seq) ? seq : 0,
                        Text(r, "event"), Text(r, "outcome"), Text(r, "reason"), data));
                }
                catch (JsonException)
                {
                    // A torn fragment: not an entry.
                }
            }
        }
        return list;
    }

    private static string Text(JsonElement r, string name) =>
        r.ValueKind == JsonValueKind.Object && r.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    /// <inheritdoc/>
    public void Dispose()
    {
        lock (_gate)
        {
            _disposed = true;
            _stream.Dispose();
        }
    }
}
