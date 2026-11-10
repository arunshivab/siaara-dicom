// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Network;

/// <summary>What to send.</summary>
/// <param name="StudyInstanceUid">Study in the local store.</param>
/// <param name="SeriesInstanceUid">One series of it, or null for the whole study.</param>
/// <param name="Title">Text for the activity row (screen only).</param>
public sealed record SendRequest(string StudyInstanceUid, string? SeriesInstanceUid, string Title)
{
    /// <summary>Also send files that were not read completely (truncated or damaged). Off unless the user ticks it for
    /// this one send; audited before anything is sent.</summary>
    public bool AllowIncomplete { get; init; }
    /// <summary>Ask the node to confirm storage; null = the node's setting.</summary>
    public bool? RequestCommitment { get; init; }
}

/// <summary>
/// Retrieve (C-GET, C-MOVE, WADO-RS) and send (C-STORE, STOW-RS). Each call makes one activity row; at most four
/// transfers run at once. Images go straight into the local store as received, so a viewer that has the study open
/// sees them arrive. Nothing here tells the user that data may be deleted anywhere.
/// </summary>
public sealed class TransferService
{
    private readonly NodeRegistry _nodes;
    private readonly StoreService _store;
    private readonly NetworkJobs _jobs;
    private readonly ScpService _scp;
    private readonly AuditLog? _audit;

    /// <summary>Creates the service.</summary>
    /// <param name="nodes">Nodes.</param>
    /// <param name="store">Local store.</param>
    /// <param name="jobs">Activity list.</param>
    /// <param name="scp">Our receiver (C-MOVE destination, commitment reports).</param>
    /// <param name="audit">Audit log, or null.</param>
    public TransferService(NodeRegistry nodes, StoreService store, NetworkJobs jobs, ScpService scp, AuditLog? audit)
    {
        _nodes = nodes;
        _store = store;
        _jobs = jobs;
        _scp = scp;
        _audit = audit;
    }

    private static string N(long n) => n.ToString(CultureInfo.InvariantCulture);

    private static string Outcome(NetworkJobState s) => s switch
    {
        NetworkJobState.Completed => AuditOutcome.Success,
        NetworkJobState.Cancelled => AuditOutcome.Cancelled,
        _ => AuditOutcome.Failure,
    };

    private static NetworkException Refusal(TkNetResult r) => new(r.DimseStatus == 0
        ? "the node refused the connection (check both AE titles, and that this viewer is registered there)"
        : NetworkText.DescribeStatus(r.DimseStatus));

    // ------------------------------------------------------------------ retrieve

    /// <summary>Starts fetching a study or one series from a node into the local store.</summary>
    /// <param name="nodeId">Node id.</param>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null for the study.</param>
    /// <param name="title">Text for the activity row (screen only, never logged).</param>
    /// <param name="expectedImages">Images the node reported, 0 or less when unknown.</param>
    /// <returns>The job.</returns>
    /// <exception cref="NetworkException">It cannot start as configured (for C-MOVE: the receiver is off or would not
    /// accept the node); the message says what to change.</exception>
    public NetworkJob Retrieve(string nodeId, string studyInstanceUid, string? seriesInstanceUid, string title, int expectedImages = 0)
    {
        ArgumentException.ThrowIfNullOrEmpty(studyInstanceUid);
        DimseNodeSetting? dimse = _nodes.FindDimse(nodeId);
        WebNodeSetting? web = _nodes.FindWeb(nodeId);
        if (dimse is null && web is null)
        {
            throw new NetworkException("This node no longer exists.");
        }
        string method = web is not null ? "wado-rs" : dimse!.Retrieve == RetrieveMethod.Move ? "c-move" : "c-get";
        string destination = "";
        if (method == "c-move")
        {
            if (!_scp.IsRunning)
            {
                throw new NetworkException("This node is set to C-MOVE, which sends the images to this viewer's receiver, and the receiver is off. "
                    + "Switch the receiver on in Network settings, or set the node's retrieve method to C-GET.");
            }
            if (!_scp.Accepts(dimse!.CalledAe))
            {
                throw new NetworkException("This node is set to C-MOVE, but the receiver would refuse its images: add the node's AE title \""
                    + dimse.CalledAe + "\" to the receiver's allowed senders in Network settings.");
            }
            destination = dimse.MoveDestinationAe.Length > 0 ? dimse.MoveDestinationAe : _scp.AeTitle;
        }
        string nodeName = dimse?.Name ?? web!.Name;
        string? series = string.IsNullOrEmpty(seriesInstanceUid) ? null : seriesInstanceUid;
        return _jobs.Start(NetworkJobKind.Retrieve, nodeName, title, studyInstanceUid, Math.Max(0, expectedImages), (job, ct) =>
        {
            NetworkJobState end = NetworkJobState.Failed;
            string? why = null;
            try
            {
                end = web is not null ? RetrieveWeb(job, web, studyInstanceUid, series, ct)
                    : RetrieveDimse(job, dimse!, method, destination, studyInstanceUid, series, ct);
                return end;
            }
            catch (Exception e)
            {
                end = e is OperationCanceledException || ct.IsCancellationRequested ? NetworkJobState.Cancelled : NetworkJobState.Failed;
                why = end == NetworkJobState.Failed ? NetworkText.Describe(e) : null;
                throw;
            }
            finally
            {
                var fields = new Dictionary<string, string>
                {
                    ["node"] = nodeId,
                    ["method"] = method,
                    ["study"] = studyInstanceUid,
                    ["completed"] = N(job.Completed),
                    ["failed"] = N(job.Failed),
                };
                if (series is not null)
                {
                    fields["series"] = series;
                }
                _audit?.Append("retrieve", Outcome(end), fields, why ?? (end == NetworkJobState.CompletedWithErrors ? "some images were not transferred" : null));
                AppLog.Info("network", string.Create(CultureInfo.InvariantCulture, $"retrieve ({method}) ended: {end}, {job.Completed} done, {job.Failed} failed"));
            }
        }, () => Retrieve(nodeId, studyInstanceUid, seriesInstanceUid, title, expectedImages));
    }

    private NetworkJobState RetrieveDimse(NetworkJob job, DimseNodeSetting node, string method, string destination, string study, string? series,
        CancellationToken ct)
    {
        TkPeer peer = _nodes.PeerFor(node);
        void Progress(TkNetResult p) => job.Report(p.Completed + p.Warning, p.Failed, p.Warning, p.Completed + p.Warning + p.Failed + p.Remaining);
        TkNetResult r = method == "c-move"
            ? TkNet.Move(peer, study, series, destination, Progress, ct)
            : TkNet.Get(peer, study, series, _store.Store, Progress, ct);
        job.Report(r.Completed + r.Warning, r.Failed, r.Warning);
        if (!r.Ok && r.Completed + r.Warning == 0)
        {
            throw Refusal(r);
        }
        if (!r.Ok || r.Failed > 0)
        {
            job.Note(N(r.Failed) + " image(s) were not transferred" + (r.Ok ? "." : ": " + NetworkText.DescribeStatus(r.DimseStatus) + "."));
            return NetworkJobState.CompletedWithErrors;
        }
        if (r.Completed + r.Warning == 0)
        {
            job.Note("The node sent no images for this request.");
            return NetworkJobState.CompletedWithErrors;
        }
        return NetworkJobState.Completed;
    }

    private NetworkJobState RetrieveWeb(NetworkJob job, WebNodeSetting node, string study, string? series, CancellationToken ct)
    {
        using TkWebClient client = _nodes.ClientFor(node);
        // Streamed by the toolkit part by part into the store (memory: about one image), so there is no size limit
        // to work around with per-series or per-image requests.
        (int stored, int failed) = client.RetrieveTo(study, series, _store.Store, p =>
        {
            job.Report(p.Parts, job.Failed, 0);
            job.ReportBytes(p.Bytes, p.TotalBytes);
        }, ct);
        job.Report(stored, failed, 0);
        if (failed > 0)
        {
            job.Note(N(failed) + " image(s) of the answer could not be stored (damaged or not DICOM).");
            return NetworkJobState.CompletedWithErrors;
        }
        if (stored == 0)
        {
            job.Note("The server sent no images for this request.");
            return NetworkJobState.CompletedWithErrors;
        }
        return NetworkJobState.Completed;
    }

    // ------------------------------------------------------------------ send

    /// <summary>Starts sending a local study or series to a node: C-STORE over one association for a DIMSE node, STOW-RS
    /// for a DICOMweb node. With <see cref="SendRequest.AllowIncomplete"/> the intent is audited first; when that
    /// cannot be written nothing is sent.</summary>
    /// <param name="nodeId">Node id.</param>
    /// <param name="request">What to send.</param>
    /// <returns>The job.</returns>
    /// <exception cref="NetworkException">The node does not exist.</exception>
    /// <exception cref="AuditUnavailableException">An incomplete-files send could not be audited; nothing was sent.</exception>
    public NetworkJob Send(string nodeId, SendRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        DimseNodeSetting? dimse = _nodes.FindDimse(nodeId);
        WebNodeSetting? web = _nodes.FindWeb(nodeId);
        if (dimse is null && web is null)
        {
            throw new NetworkException("This node no longer exists.");
        }
        string method = web is not null ? "stow-rs" : "c-store";
        var fields = new Dictionary<string, string>
        {
            ["node"] = nodeId,
            ["method"] = method,
            ["study"] = request.StudyInstanceUid,
            ["allowIncomplete"] = request.AllowIncomplete ? "yes" : "no",
        };
        if (!string.IsNullOrEmpty(request.SeriesInstanceUid))
        {
            fields["series"] = request.SeriesInstanceUid;
        }
        if (request.AllowIncomplete)
        {
            // Sending files known to be incomplete is the one risky send: on record before the first byte leaves.
            _audit?.AppendRequired("send", AuditOutcome.Intent, fields);
        }
        return _jobs.Start(NetworkJobKind.Send, dimse?.Name ?? web!.Name, request.Title, request.StudyInstanceUid, 0, (job, ct) =>
        {
            NetworkJobState end = NetworkJobState.Failed;
            string? why = null;
            try
            {
                List<(string Path, string Sop)> files = CollectFiles(request, ct);
                if (files.Count == 0)
                {
                    throw new NetworkException("there is nothing to send: the study is not in the local store");
                }
                job.Report(0, 0, 0, files.Count);
                fields["images"] = N(files.Count);
                end = web is not null ? SendWeb(job, web, files, request, ct) : SendDimse(job, dimse!, files, request, fields, ct);
                return end;
            }
            catch (Exception e)
            {
                end = e is OperationCanceledException || ct.IsCancellationRequested ? NetworkJobState.Cancelled : NetworkJobState.Failed;
                why = end == NetworkJobState.Failed ? NetworkText.Describe(e) : null;
                throw;
            }
            finally
            {
                fields["sent"] = N(job.Completed);
                fields["failed"] = N(job.Failed);
                _audit?.Append("send", Outcome(end), fields, why ?? (end == NetworkJobState.CompletedWithErrors ? "some images were not sent or not confirmed" : null));
                AppLog.Info("network", string.Create(CultureInfo.InvariantCulture, $"send ({method}) ended: {end}, {job.Completed} sent, {job.Failed} failed"));
            }
        }, () => Send(nodeId, request));
    }

    private List<(string Path, string Sop)> CollectFiles(SendRequest request, CancellationToken ct)
    {
        var files = new List<(string, string)>();
        foreach (TkRecord s in _store.Store.GetSeries(request.StudyInstanceUid))
        {
            ct.ThrowIfCancellationRequested();
            if (!string.IsNullOrEmpty(request.SeriesInstanceUid) && s.SeriesInstanceUid != request.SeriesInstanceUid)
            {
                continue;
            }
            files.AddRange(_store.Store.GetInstances(s.SeriesInstanceUid).Where(i => i.FilePath.Length > 0).Select(i => (i.FilePath, i.SopInstanceUid)));
        }
        return files;
    }

    private const string IncompleteText = "the file was not read completely (truncated or damaged); such files are only sent when you allow it for the send";

    private NetworkJobState SendDimse(NetworkJob job, DimseNodeSetting node, List<(string Path, string Sop)> files, SendRequest request,
        Dictionary<string, string> fields, CancellationToken ct)
    {
        TkPeer peer = _nodes.PeerFor(node, request.AllowIncomplete);
        (TkNetResult r, IReadOnlyList<TkStoreOutcome> outcomes) = TkNet.StoreFiles(peer, [.. files.Select(f => f.Path)],
            p => job.Report(p.Completed + p.Warning, p.Failed, p.Warning), ct);
        int sent = 0, failed = 0, changed = 0;
        var committed = new List<(string, string)>();
        for (int i = 0; i < outcomes.Count; i++)
        {
            TkStoreOutcome o = outcomes[i];
            string uid = o.OriginalSopInstanceUid.Length > 0 ? o.OriginalSopInstanceUid : files[i].Sop;
            if (o.Sent)
            {
                sent++;
                committed.Add((o.SopClassUid, o.SopInstanceUid));
                if (o.UidChanged)
                {
                    changed++;
                    // The node knows this image under the new identifier only.
                    job.Note("Sent with lossy compression under a new identifier: " + uid + " was sent as " + o.SopInstanceUid + ".");
                }
                continue;
            }
            failed++;
            string reason = o.IncompleteInput ? IncompleteText
                : o.Error != TkError.None && o.DimseStatus is 0xFFFF or 0 ? Tk.Describe(o.Error)
                : NetworkText.DescribeStatus(o.DimseStatus);
            job.Note("Not sent: image " + uid + ": " + reason + ".");
        }
        job.Report(sent, failed, 0, files.Count);
        fields["uidChanged"] = N(changed);
        if (sent == 0)
        {
            throw outcomes.All(o => o.IncompleteInput) ? new NetworkException("no image was sent: " + IncompleteText)
                : r.DimseStatus == 0 && outcomes.All(o => o.Error is TkError.Rejected or TkError.Network) ? Refusal(r)
                : new NetworkException("no image was sent (see the lines below)");
        }
        bool problems = failed > 0;
        if (request.RequestCommitment ?? node.RequestCommitment)
        {
            problems |= !Commit(job, node, peer, committed, fields, ct);
        }
        else
        {
            fields["commitment"] = "not requested";
        }
        return problems ? NetworkJobState.CompletedWithErrors : NetworkJobState.Completed;
    }

    // Storage Commitment for what was just sent. True only when the node confirmed every image.
    private bool Commit(NetworkJob job, DimseNodeSetting node, TkPeer peer, List<(string SopClassUid, string SopInstanceUid)> sent,
        Dictionary<string, string> fields, CancellationToken ct)
    {
        TkCommitResult c;
        try
        {
            // With our receiver running the report may come on a new association; otherwise only on this one.
            c = TkNet.Commit(peer, sent, TimeSpan.FromSeconds(node.CommitmentWaitSeconds), _scp.Running, ct);
        }
        catch (TkException e)
        {
            fields["commitment"] = "failed";
            job.Note("The PACS did not confirm storage: the confirmation request failed (" + NetworkText.Describe(e) + ").");
            return false;
        }
        if (!c.Reported)
        {
            fields["commitment"] = "unconfirmed";
            job.Note("The PACS did not confirm storage (no answer within " + N(node.CommitmentWaitSeconds) + " s).");
            return false;
        }
        int not = c.FailureReasons.Count(x => x != 0);
        fields["commitment"] = not == 0 ? "confirmed" : "partial";
        fields["notCommitted"] = N(not);
        if (not == 0)
        {
            job.Note("The PACS confirmed storage of " + N(sent.Count) + " image(s).");
            return true;
        }
        job.Note(N(not) + " of " + N(sent.Count) + " image(s) not committed by the PACS.");
        for (int i = 0; i < c.FailureReasons.Count && i < sent.Count; i++)
        {
            if (c.FailureReasons[i] != 0)
            {
                job.Note("Not committed: image " + sent[i].SopInstanceUid + ": " + NetworkText.DescribeCommitReason(c.FailureReasons[i]) + ".");
            }
        }
        return false;
    }

    private NetworkJobState SendWeb(NetworkJob job, WebNodeSetting node, List<(string Path, string Sop)> files, SendRequest request, CancellationToken ct)
    {
        using TkWebClient client = _nodes.ClientFor(node);
        int sent = 0, failed = 0;
        TkException? first = null;
        foreach ((string path, string sop) in files)
        {
            ct.ThrowIfCancellationRequested();
            try
            {
                client.Store(path, request.AllowIncomplete, ct);
                sent++;
            }
            catch (TkException e) when (!ct.IsCancellationRequested)
            {
                failed++;
                first ??= e;
                job.Note("Not sent: image " + sop + ": " + (e.Error == TkError.Truncated ? IncompleteText : NetworkText.Describe(e)) + ".");
                if (sent == 0 && failed >= 3 && e.Error is TkError.Network or TkError.Timeout or TkError.Rejected)
                {
                    // The server is not reachable or refuses us: do not walk through the whole study.
                    job.Report(sent, failed, 0, files.Count);
                    throw new NetworkException(NetworkText.Describe(e), e);
                }
            }
            job.Report(sent, failed, 0, files.Count);
        }
        if (sent == 0)
        {
            throw new NetworkException("no image was sent: " + (first is null ? "nothing to send" : first.Error == TkError.Truncated ? IncompleteText : NetworkText.Describe(first)));
        }
        return failed > 0 ? NetworkJobState.CompletedWithErrors : NetworkJobState.Completed;
    }
}

/// <summary>The networking services of one running app, created together and disposed before the store closes.</summary>
public sealed class NetworkServices : IDisposable
{
    /// <summary>Creates the services (the receiver is not started; see <see cref="StartConfigured"/>).</summary>
    /// <param name="settings">Settings.</param>
    /// <param name="secrets">Secret store.</param>
    /// <param name="store">Local store.</param>
    /// <param name="audit">Audit log, or null.</param>
    public NetworkServices(Settings.SettingsStore settings, Platform.ISecretStore secrets, StoreService store, AuditLog? audit)
    {
        Nodes = new NodeRegistry(settings, secrets, audit);
        Jobs = new NetworkJobs();
        Scp = new ScpService(Nodes, store, Jobs, audit);
        Search = new RemoteSearchService(Nodes, store, audit);
        Transfers = new TransferService(Nodes, store, Jobs, Scp, audit);
    }

    /// <summary>Nodes and their secrets.</summary>
    public NodeRegistry Nodes { get; }
    /// <summary>Activity list.</summary>
    public NetworkJobs Jobs { get; }
    /// <summary>Our receiver.</summary>
    public ScpService Scp { get; }
    /// <summary>Remote search.</summary>
    public RemoteSearchService Search { get; }
    /// <summary>Retrieve and send.</summary>
    public TransferService Transfers { get; }

    /// <summary>Starts the receiver when the settings say so. A failure is logged and kept in
    /// <see cref="ScpService.LastError"/> for the settings page; the app starts regardless.</summary>
    public void StartConfigured()
    {
        if (!Nodes.Current.Scp.Enabled)
        {
            return;
        }
        try
        {
            Scp.Start();
        }
        catch (NetworkException e)
        {
            AppLog.Warn("network", "the receiver is enabled in the settings but did not start: " + e.Message);
        }
    }

    /// <summary>Cancels transfers and stops the receiver. Call before the store closes.</summary>
    public void Dispose()
    {
        Jobs.Dispose();
        Scp.Dispose();
    }
}
