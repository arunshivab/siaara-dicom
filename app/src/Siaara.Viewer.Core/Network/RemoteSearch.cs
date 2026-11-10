// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Text;
using System.Text.Json;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Network;

/// <summary>How much of a remote study or series the local store holds.</summary>
public enum LocalPresence
{
    /// <summary>Nothing.</summary>
    None,
    /// <summary>Some images (or the remote count is unknown, so "all" cannot be shown).</summary>
    Partial,
    /// <summary>At least as many images as the node reports.</summary>
    Complete,
}

/// <summary>A study found on a node. The values are exactly what the node sent.</summary>
public sealed class RemoteStudyRow
{
    internal RemoteStudyRow(TkRecord r, int localImages)
    {
        Study = new StudyRow(r);
        RemoteImages = r.NumberOfInstances;
        LocalImages = localImages;
    }

    /// <summary>The study's columns (same shape as a local row).</summary>
    public StudyRow Study { get; }
    /// <summary>Images on the node; -1 when the node did not say.</summary>
    public int RemoteImages { get; }
    /// <summary>Images of this study in the local store.</summary>
    public int LocalImages { get; internal set; }
    /// <summary>The Local column.</summary>
    public LocalPresence Local => RemoteSearchService.Presence(LocalImages, RemoteImages);
}

/// <summary>A series of a remote study.</summary>
/// <param name="SeriesInstanceUid">Series Instance UID.</param>
/// <param name="SeriesNumber">Series Number as received, or null.</param>
/// <param name="Modality">Modality.</param>
/// <param name="Description">Series Description as received.</param>
/// <param name="RemoteImages">Images on the node; -1 when unknown.</param>
/// <param name="LocalImages">Images in the local store.</param>
public sealed record RemoteSeriesRow(string SeriesInstanceUid, int? SeriesNumber, string Modality, string Description, int RemoteImages, int LocalImages)
{
    /// <summary>The Local column.</summary>
    public LocalPresence Local => RemoteSearchService.Presence(LocalImages, RemoteImages);
}

/// <summary>Result of a remote study search.</summary>
/// <param name="Rows">Studies, newest first.</param>
/// <param name="Truncated">More studies matched than the cap: the user should narrow the search.</param>
/// <param name="Cap">The cap that applied.</param>
public sealed record RemoteSearchResult(IReadOnlyList<RemoteStudyRow> Rows, bool Truncated, int Cap);

/// <summary>
/// Searches nodes: Study Root C-FIND or QIDO-RS at study level, series when a study is expanded. The search box means
/// "name contains, or patient ID or accession number starts with", as in the local browser, so a text search is three
/// queries whose answers are merged. Query values and answers are never written to the app log or the audit log;
/// the audit entry "search" holds the node, the level and the counts.
/// </summary>
public sealed class RemoteSearchService
{
    private readonly NodeRegistry _nodes;
    private readonly StoreService _store;
    private readonly AuditLog? _audit;

    /// <summary>Creates the service.</summary>
    /// <param name="nodes">Nodes.</param>
    /// <param name="store">Local store (for the Local column).</param>
    /// <param name="audit">Audit log, or null.</param>
    public RemoteSearchService(NodeRegistry nodes, StoreService store, AuditLog? audit)
    {
        _nodes = nodes;
        _store = store;
        _audit = audit;
    }

    internal static LocalPresence Presence(int local, int remote) =>
        local <= 0 ? LocalPresence.None : remote > 0 && local >= remote ? LocalPresence.Complete : LocalPresence.Partial;

    private static Task<T> OnThread<T>(Func<T> work) =>
        Task.Factory.StartNew(work, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);

    private static string Clean(string text) => text.Replace("*", "", StringComparison.Ordinal).Replace("?", "", StringComparison.Ordinal)
        .Replace("\\", "", StringComparison.Ordinal);

    // The search box as separate key sets (ORed by merging), times the modality chips.
    private static List<TkRemoteKeys> KeySets(StudyQuery q)
    {
        string text = Clean(q.Text.Trim());
        TkRemoteKeys[] byText = text.Length == 0
            ? [new TkRemoteKeys()]
            : [new TkRemoteKeys { PatientName = "*" + text + "*" }, new TkRemoteKeys { PatientId = text + "*" }, new TkRemoteKeys { AccessionNumber = text + "*" }];
        IReadOnlyList<string?> modalities = q.Modalities.Count == 0 ? [null] : [.. q.Modalities];
        var sets = new List<TkRemoteKeys>();
        foreach (TkRemoteKeys k in byText)
        {
            foreach (string? m in modalities)
            {
                sets.Add(k with { StudyDate = q.DateFilter, ModalitiesInStudy = m });
            }
        }
        return sets;
    }

    private void Audit(string nodeId, string kind, string level, string outcome, int matches, bool truncated, string? reason) =>
        _audit?.Append("search", outcome, new Dictionary<string, string>
        {
            ["node"] = nodeId,
            ["kind"] = kind,
            ["level"] = level,
            ["matches"] = matches.ToString(CultureInfo.InvariantCulture),
            ["truncated"] = truncated ? "yes" : "no",
        }, reason);

    /// <summary>Searches studies on a node. Cancelling the token aborts the connection.</summary>
    /// <param name="nodeId">Node id.</param>
    /// <param name="query">The filter bar (paging is not used; the cap applies).</param>
    /// <param name="cancellationToken">Cancels (a new search cancels the one before).</param>
    /// <returns>The studies with their Local column.</returns>
    /// <exception cref="NetworkException">The search failed; the message is plain words.</exception>
    public async Task<RemoteSearchResult> SearchStudiesAsync(string nodeId, StudyQuery query, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(query);
        DimseNodeSetting? dimse = _nodes.FindDimse(nodeId);
        WebNodeSetting? web = _nodes.FindWeb(nodeId);
        if (dimse is null && web is null)
        {
            throw new NetworkException("This node no longer exists.");
        }
        int cap = Math.Clamp(_nodes.Current.SearchCap, 1, 5000);
        string kind = dimse is not null ? "dimse" : "dicomweb";
        var found = new Dictionary<string, TkRecord>(StringComparer.Ordinal);
        bool truncated = false;
        try
        {
            truncated = await OnThread(() =>
            {
                bool over = false;
                if (dimse is not null)
                {
                    TkPeer peer = _nodes.PeerFor(dimse);
                    foreach (TkRemoteKeys keys in KeySets(query))
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        int n = 0;
                        TkNetResult r = TkNet.Find(peer, TkLevel.Study, keys, cap + 1, rec =>
                        {
                            n++;
                            if (rec.StudyInstanceUid.Length > 0)
                            {
                                found.TryAdd(rec.StudyInstanceUid, rec);
                            }
                        }, cancellationToken);
                        if (!r.Ok)
                        {
                            throw new NetworkException(r.DimseStatus == 0
                                ? "the node refused the connection (check both AE titles, and that this viewer is registered there)"
                                : NetworkText.DescribeStatus(r.DimseStatus));
                        }
                        over |= n > cap;
                    }
                }
                else
                {
                    using TkWebClient client = _nodes.ClientFor(web!);
                    foreach (TkRemoteKeys keys in KeySets(query))
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        string json = client.Search(TkLevel.Study, QidoQuery(keys, cap + 1), null, cancellationToken);
                        int n = 0;
                        foreach (TkRecord rec in DicomJson.Records(json, TkLevel.Study))
                        {
                            n++;
                            if (rec.StudyInstanceUid.Length > 0)
                            {
                                found.TryAdd(rec.StudyInstanceUid, rec);
                            }
                        }
                        over |= n > cap;
                    }
                }
                return over || found.Count > cap;
            }).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            Audit(nodeId, kind, "study", AuditOutcome.Cancelled, found.Count, false, null);
            throw;
        }
        catch (Exception e) when (e is TkException or NetworkException or System.Security.Cryptography.CryptographicException or JsonException)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string why = e is JsonException ? "the server's answer was not DICOM JSON" : e is System.Security.Cryptography.CryptographicException
                ? "the secret store of this computer could not be read" : NetworkText.Describe(e);
            Audit(nodeId, kind, "study", AuditOutcome.Failure, found.Count, false, why);
            AppLog.Warn("network", "remote search failed: " + why);
            throw new NetworkException("The search failed: " + why + ".", e);
        }
        List<TkRecord> ordered = [.. found.Values];
        ordered.Sort(static (a, b) =>
        {
            int c = string.CompareOrdinal(b.StudyDate, a.StudyDate);
            c = c != 0 ? c : string.CompareOrdinal(b.StudyTime, a.StudyTime);
            return c != 0 ? c : string.CompareOrdinal(a.StudyInstanceUid, b.StudyInstanceUid);
        });
        if (ordered.Count > cap)
        {
            ordered.RemoveRange(cap, ordered.Count - cap);
        }
        var rows = new List<RemoteStudyRow>(ordered.Count);
        foreach (TkRecord rec in ordered)
        {
            StudyRow? local = await _store.GetStudyAsync(rec.StudyInstanceUid, cancellationToken).ConfigureAwait(false);
            rows.Add(new RemoteStudyRow(rec, local?.Images ?? 0));
        }
        Audit(nodeId, kind, "study", AuditOutcome.Success, rows.Count, truncated, null);
        AppLog.Info("network", string.Create(CultureInfo.InvariantCulture, $"remote search: {rows.Count} studies{(truncated ? " (cap reached)" : "")}"));
        return new RemoteSearchResult(rows, truncated, cap);
    }

    /// <summary>The series of a study on a node (asked when the study row is expanded).</summary>
    /// <param name="nodeId">Node id.</param>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>Series by Series Number.</returns>
    /// <exception cref="NetworkException">The search failed.</exception>
    public async Task<IReadOnlyList<RemoteSeriesRow>> SearchSeriesAsync(string nodeId, string studyInstanceUid, CancellationToken cancellationToken)
    {
        DimseNodeSetting? dimse = _nodes.FindDimse(nodeId);
        WebNodeSetting? web = _nodes.FindWeb(nodeId);
        if (dimse is null && web is null)
        {
            throw new NetworkException("This node no longer exists.");
        }
        string kind = dimse is not null ? "dimse" : "dicomweb";
        var found = new List<TkRecord>();
        try
        {
            await OnThread(() =>
            {
                if (dimse is not null)
                {
                    TkNetResult r = TkNet.Find(_nodes.PeerFor(dimse), TkLevel.Series, new TkRemoteKeys { StudyInstanceUid = studyInstanceUid }, 5000,
                        found.Add, cancellationToken);
                    if (!r.Ok)
                    {
                        throw new NetworkException(r.DimseStatus == 0 ? "the node refused the connection" : NetworkText.DescribeStatus(r.DimseStatus));
                    }
                }
                else
                {
                    using TkWebClient client = _nodes.ClientFor(web!);
                    found.AddRange(DicomJson.Records(client.Search(TkLevel.Series, "includefield=00201209&includefield=0008103E", studyInstanceUid,
                        cancellationToken), TkLevel.Series));
                }
                return true;
            }).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception e) when (e is TkException or NetworkException or System.Security.Cryptography.CryptographicException or JsonException)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string why = e is JsonException ? "the server's answer was not DICOM JSON" : e is System.Security.Cryptography.CryptographicException
                ? "the secret store of this computer could not be read" : NetworkText.Describe(e);
            Audit(nodeId, kind, "series", AuditOutcome.Failure, 0, false, why);
            throw new NetworkException("The series could not be listed: " + why + ".", e);
        }
        IReadOnlyList<SeriesRow> local = await _store.GetSeriesAsync(studyInstanceUid, cancellationToken).ConfigureAwait(false);
        var localCount = local.ToDictionary(s => s.SeriesInstanceUid, s => s.Instances, StringComparer.Ordinal);
        Audit(nodeId, kind, "series", AuditOutcome.Success, found.Count, false, null);
        return [.. found.Where(r => r.SeriesInstanceUid.Length > 0).DistinctBy(r => r.SeriesInstanceUid)
            .OrderBy(r => r.SeriesNumber ?? int.MaxValue).ThenBy(r => r.SeriesInstanceUid, StringComparer.Ordinal)
            .Select(r => new RemoteSeriesRow(r.SeriesInstanceUid, r.SeriesNumber, r.Modality, r.SeriesDescription, r.NumberOfInstances,
                localCount.GetValueOrDefault(r.SeriesInstanceUid)))];
    }

    /// <summary>Recounts the Local column of rows already on screen (after images arrived).</summary>
    /// <param name="rows">Rows.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>Completion.</returns>
    public async Task RefreshLocalAsync(IReadOnlyList<RemoteStudyRow> rows, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(rows);
        foreach (RemoteStudyRow row in rows)
        {
            StudyRow? local = await _store.GetStudyAsync(row.Study.StudyInstanceUid, cancellationToken).ConfigureAwait(false);
            row.LocalImages = local?.Images ?? 0;
        }
    }

    // QIDO-RS puts the matching keys into the address, as the standard defines; servers and proxies may log it.
    internal static string QidoQuery(TkRemoteKeys keys, int limit)
    {
        var sb = new StringBuilder();
        void Add(string name, string? value)
        {
            if (!string.IsNullOrEmpty(value))
            {
                sb.Append(sb.Length > 0 ? "&" : "").Append(name).Append('=').Append(Uri.EscapeDataString(value));
            }
        }
        Add("PatientName", keys.PatientName);
        Add("PatientID", keys.PatientId);
        Add("AccessionNumber", keys.AccessionNumber);
        Add("StudyDate", keys.StudyDate);
        Add("ModalitiesInStudy", keys.ModalitiesInStudy);
        Add("limit", limit.ToString(CultureInfo.InvariantCulture));
        foreach (string tag in (string[])["00081030", "00100030", "00100040", "00201206", "00201208"])
        {
            Add("includefield", tag);
        }
        return sb.ToString();
    }
}

/// <summary>Reads the attributes the browser shows out of a DICOM JSON answer (PS3.18 Annex F). Values are taken as
/// they are; several values are joined with a backslash, as DICOM writes them.</summary>
internal static class DicomJson
{
    public static List<TkRecord> Records(string json, TkLevel level)
    {
        var list = new List<TkRecord>();
        if (string.IsNullOrWhiteSpace(json))
        {
            return list;
        }
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Array)
        {
            throw new JsonException("not an array");
        }
        foreach (JsonElement o in doc.RootElement.EnumerateArray())
        {
            if (o.ValueKind != JsonValueKind.Object)
            {
                continue;
            }
            list.Add(new TkRecord
            {
                Level = level,
                PatientName = Text(o, "00100010"),
                PatientId = Text(o, "00100020"),
                PatientBirthDate = Text(o, "00100030"),
                PatientSex = Text(o, "00100040"),
                StudyInstanceUid = Text(o, "0020000D"),
                StudyDate = Text(o, "00080020"),
                StudyTime = Text(o, "00080030"),
                AccessionNumber = Text(o, "00080050"),
                StudyDescription = Text(o, "00081030"),
                ModalitiesInStudy = Text(o, "00080061"),
                Modality = Text(o, "00080060"),
                SeriesInstanceUid = Text(o, "0020000E"),
                SeriesDescription = Text(o, "0008103E"),
                SeriesNumber = Number(o, "00200011"),
                NumberOfSeries = Number(o, "00201206") ?? -1,
                NumberOfInstances = Number(o, level == TkLevel.Study ? "00201208" : "00201209") ?? -1,
            });
        }
        return list;
    }

    private static string Text(JsonElement o, string tag)
    {
        if (!TryValues(o, tag, out JsonElement values))
        {
            return "";
        }
        var parts = new List<string>();
        foreach (JsonElement v in values.EnumerateArray())
        {
            parts.Add(v.ValueKind switch
            {
                JsonValueKind.String => v.GetString() ?? "",
                JsonValueKind.Number => v.GetRawText(),
                // Person names: the alphabetic group is what the browser shows.
                JsonValueKind.Object => v.TryGetProperty("Alphabetic", out JsonElement a) && a.ValueKind == JsonValueKind.String ? a.GetString() ?? "" : "",
                _ => "",
            });
        }
        return string.Join('\\', parts);
    }

    private static int? Number(JsonElement o, string tag)
    {
        if (!TryValues(o, tag, out JsonElement values) || values.GetArrayLength() == 0)
        {
            return null;
        }
        JsonElement v = values[0];
        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n))
        {
            return n;
        }
        return v.ValueKind == JsonValueKind.String && int.TryParse(v.GetString()?.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : null;
    }

    private static bool TryValues(JsonElement o, string tag, out JsonElement values)
    {
        values = default;
        if (!o.TryGetProperty(tag, out JsonElement attr) && !o.TryGetProperty(tag.ToLowerInvariant(), out attr))
        {
            return false;
        }
        return attr.ValueKind == JsonValueKind.Object && attr.TryGetProperty("Value", out values) && values.ValueKind == JsonValueKind.Array;
    }
}
