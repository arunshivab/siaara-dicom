// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Facade over the networking part of the SIAARA C# bindings (Siaara.Network, Siaara.Web). With
// Tk.cs these are the only files of the app that name a binding type. The toolkit moves DICOM
// data; what to ask, when, and what to tell the user is decided by the app (Core/Network).

using System.Globalization;
using Siaara.Network;
using Siaara.Web;

namespace Siaara.Viewer.Core.Toolkit;

/// <summary>TLS settings of a connection. There is deliberately no "skip certificate checks" member.</summary>
public sealed record TkTls
{
    /// <summary>PEM file of trusted certificate authorities; null = the operating system's trust store.</summary>
    public string? CaFile { get; init; }
    /// <summary>Our identity as a PKCS#12 file (every OS), or null.</summary>
    public string? Pkcs12File { get; init; }
    /// <summary>Password of the PKCS#12 file.</summary>
    public string? Password { get; init; }
    /// <summary>Name or address the other side's certificate must carry; null = the host we connect to.</summary>
    public string? ExpectedIdentity { get; init; }
    /// <summary>Refuse TLS 1.2.</summary>
    public bool Tls13Only { get; init; }

    /// <summary>Hides the password.</summary>
    /// <returns>Text without the password.</returns>
    public override string ToString() => "TkTls";

    internal TlsOptions ToBinding() => new()
    {
        CaFile = Empty(CaFile),
        Pkcs12File = Empty(Pkcs12File),
        Password = Empty(Password),
        ExpectedIdentity = Empty(ExpectedIdentity),
        Tls13Only = Tls13Only,
    };

    private static string? Empty(string? s) => string.IsNullOrEmpty(s) ? null : s;
}

/// <summary>A remote DIMSE application entity and how to talk to it.</summary>
public sealed record TkPeer
{
    /// <summary>Host name or address.</summary>
    public required string Host { get; init; }
    /// <summary>Port.</summary>
    public required int Port { get; init; }
    /// <summary>The remote AE title.</summary>
    public required string CalledAe { get; init; }
    /// <summary>Our AE title.</summary>
    public required string CallingAe { get; init; }
    /// <summary>TLS, or null for plain TCP.</summary>
    public TkTls? Tls { get; init; }
    /// <summary>TCP connect timeout.</summary>
    public TimeSpan? ConnectTimeout { get; init; }
    /// <summary>Association negotiation timeout.</summary>
    public TimeSpan? AssociationTimeout { get; init; }
    /// <summary>Wait for each DIMSE response.</summary>
    public TimeSpan? DimseTimeout { get; init; }
    /// <summary>Largest PDU we accept (0 = the toolkit's default).</summary>
    public int MaxPdu { get; init; }
    /// <summary>C-STORE may compress lossily when the other side accepts nothing else.</summary>
    public bool AllowLossyTranscode { get; init; }
    /// <summary>Send files whose read was not complete.</summary>
    public bool AllowIncomplete { get; init; }

    internal DimsePeer ToBinding() => new()
    {
        Host = Host,
        Port = Port,
        CalledAeTitle = CalledAe,
        CallingAeTitle = CallingAe,
        Tls = Tls?.ToBinding(),
        ConnectTimeout = ConnectTimeout,
        AssociationTimeout = AssociationTimeout,
        DimseTimeout = DimseTimeout,
        MaxPdu = MaxPdu,
        AllowLossyTranscode = AllowLossyTranscode,
        AllowIncomplete = AllowIncomplete,
    };
}

/// <summary>Outcome of a DIMSE operation as the other side decided it.</summary>
/// <param name="Ok">Success or warning.</param>
/// <param name="DimseStatus">Final DIMSE status (0 = success).</param>
/// <param name="Completed">Sub-operations or files done.</param>
/// <param name="Failed">Failed ones.</param>
/// <param name="Warning">Ones with a warning.</param>
/// <param name="Remaining">Remaining as reported.</param>
/// <param name="Matches">C-FIND matches.</param>
public readonly record struct TkNetResult(bool Ok, int DimseStatus, int Completed, int Failed, int Warning, int Remaining, int Matches)
{
    internal static TkNetResult From(DimseResult r) =>
        new(r.IsSuccess, r.DimseStatus, r.Completed, r.Failed, r.Warning, r.Remaining, r.Matches);
}

/// <summary>Matching keys of a remote query (C-FIND wildcards and ranges; null = any).</summary>
public sealed record TkRemoteKeys
{
    /// <summary>Patient's Name.</summary>
    public string? PatientName { get; init; }
    /// <summary>Patient ID.</summary>
    public string? PatientId { get; init; }
    /// <summary>Accession Number.</summary>
    public string? AccessionNumber { get; init; }
    /// <summary>Study Date or range.</summary>
    public string? StudyDate { get; init; }
    /// <summary>Modalities in Study.</summary>
    public string? ModalitiesInStudy { get; init; }
    /// <summary>Study Instance UID.</summary>
    public string? StudyInstanceUid { get; init; }
    /// <summary>Series Instance UID.</summary>
    public string? SeriesInstanceUid { get; init; }

    /// <summary>Never prints the values (they identify patients).</summary>
    /// <returns>A fixed text.</returns>
    public override string ToString() => "TkRemoteKeys";
}

/// <summary>What happened to one file of a C-STORE.</summary>
/// <param name="Sent">The other side stored it (status success or warning).</param>
/// <param name="Error">Why it was not sent (None when a request went out).</param>
/// <param name="DimseStatus">The C-STORE status, 0xFFFF when nothing was sent.</param>
/// <param name="IncompleteInput">Not sent because the file was not read completely.</param>
/// <param name="UidChanged">A lossy transcode gave the instance a new SOP Instance UID.</param>
/// <param name="Lossy">The transfer compressed lossily.</param>
/// <param name="SopClassUid">SOP Class UID.</param>
/// <param name="SopInstanceUid">SOP Instance UID as sent.</param>
/// <param name="OriginalSopInstanceUid">The file's own SOP Instance UID.</param>
public sealed record TkStoreOutcome(bool Sent, TkError Error, int DimseStatus, bool IncompleteInput, bool UidChanged, bool Lossy,
                                    string SopClassUid, string SopInstanceUid, string OriginalSopInstanceUid);

/// <summary>Result of a Storage Commitment request.</summary>
/// <param name="Reported">The other side answered within the wait.</param>
/// <param name="FailureReasons">Per instance: 0 committed, 0xFFFF not reported, else the DICOM failure reason.</param>
public sealed record TkCommitResult(bool Reported, IReadOnlyList<int> FailureReasons);

/// <summary>DIMSE service class user calls. Each call opens one association, blocks the calling thread, and is
/// cancelled through the token (the association is aborted). Failures decided by the other side come back in the
/// result; network failures, timeouts and refusals of the connection throw <see cref="TkException"/>.</summary>
public static class TkNet
{
    private static readonly DicomTag s_patientName = new(0x00100010);
    private static readonly DicomTag s_patientId = new(0x00100020);
    private static readonly DicomTag s_patientBirthDate = new(0x00100030);
    private static readonly DicomTag s_patientSex = new(0x00100040);
    private static readonly DicomTag s_studyUid = new(0x0020000D);
    private static readonly DicomTag s_studyDate = new(0x00080020);
    private static readonly DicomTag s_studyTime = new(0x00080030);
    private static readonly DicomTag s_accession = new(0x00080050);
    private static readonly DicomTag s_studyDescription = new(0x00081030);
    private static readonly DicomTag s_modalitiesInStudy = new(0x00080061);
    private static readonly DicomTag s_studySeries = new(0x00201206);
    private static readonly DicomTag s_studyInstances = new(0x00201208);
    private static readonly DicomTag s_seriesUid = new(0x0020000E);
    private static readonly DicomTag s_modality = new(0x00080060);
    private static readonly DicomTag s_seriesNumber = new(0x00200011);
    private static readonly DicomTag s_seriesDescription = new(0x0008103E);
    private static readonly DicomTag s_seriesInstances = new(0x00201209);

    private static T Run<T>(Func<T> call, CancellationToken token)
    {
        try
        {
            return call();
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
            throw new OperationCanceledException(token);
        }
    }

    /// <summary>C-ECHO.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static TkNetResult Echo(TkPeer peer, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        return Run(() => TkNetResult.From(Dimse.Echo(peer.ToBinding(), cancellationToken)), cancellationToken);
    }

    /// <summary>Study Root C-FIND at study or series level. Each match is handed over as a row with the values
    /// exactly as received.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="level">Study or Series.</param>
    /// <param name="keys">Matching keys.</param>
    /// <param name="maxMatches">Stop (C-CANCEL) after this many matches; 0 = all.</param>
    /// <param name="onMatch">Called per match on the calling thread.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static TkNetResult Find(TkPeer peer, TkLevel level, TkRemoteKeys keys, int maxMatches, Action<TkRecord> onMatch,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        ArgumentNullException.ThrowIfNull(keys);
        ArgumentNullException.ThrowIfNull(onMatch);
        if (level is not (TkLevel.Study or TkLevel.Series))
        {
            throw new ArgumentOutOfRangeException(nameof(level));
        }
        return Run(() =>
        {
            using DicomFile id = Dimse.CreateQueryIdentifier(level == TkLevel.Study ? QueryLevel.Study : QueryLevel.Series, new QueryKeys
            {
                PatientName = keys.PatientName,
                PatientId = keys.PatientId,
                AccessionNumber = keys.AccessionNumber,
                StudyDate = keys.StudyDate,
                ModalitiesInStudy = keys.ModalitiesInStudy,
                StudyInstanceUid = keys.StudyInstanceUid,
                SeriesInstanceUid = keys.SeriesInstanceUid,
            });
            // Return keys the browser shows; the counts are optional keys many archives answer.
            DicomTag[] wanted = level == TkLevel.Study
                ? [s_patientName, s_patientId, s_patientBirthDate, s_patientSex, s_studyUid, s_studyDate, s_studyTime, s_accession,
                   s_studyDescription, s_modalitiesInStudy, s_studySeries, s_studyInstances]
                : [s_studyUid, s_seriesUid, s_modality, s_seriesNumber, s_seriesDescription, s_seriesInstances];
            foreach (DicomTag t in wanted)
            {
                if (!id.Dataset.Contains(t))
                {
                    id.Dataset.SetString(t, "");
                }
            }
            int seen = 0;
            DimseResult r = Dimse.Find(peer.ToBinding(), QueryRetrieveModel.StudyRoot, id.Dataset, d =>
            {
                onMatch(RecordFrom(d, level));
                seen++;
                return maxMatches <= 0 || seen < maxMatches;
            }, null, cancellationToken);
            return TkNetResult.From(r) with { Matches = seen };
        }, cancellationToken);
    }

    private static string Str(DicomDataset d, DicomTag t)
    {
        try
        {
            return d.TryGetString(t, out string? v) ? v : "";
        }
        catch (SiaaraException)
        {
            return "";
        }
    }

    private static string Joined(DicomDataset d, DicomTag t)
    {
        try
        {
            return d.Contains(t) ? string.Join('\\', d.GetStrings(t)) : "";
        }
        catch (SiaaraException)
        {
            return "";
        }
    }

    private static int? Int(DicomDataset d, DicomTag t) =>
        int.TryParse(Str(d, t).Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? v : null;

    private static TkRecord RecordFrom(DicomDataset d, TkLevel level) => new()
    {
        Level = level,
        PatientName = Str(d, s_patientName),
        PatientId = Str(d, s_patientId),
        PatientSex = Str(d, s_patientSex),
        PatientBirthDate = Str(d, s_patientBirthDate),
        StudyInstanceUid = Str(d, s_studyUid),
        StudyDate = Str(d, s_studyDate),
        StudyTime = Str(d, s_studyTime),
        AccessionNumber = Str(d, s_accession),
        StudyDescription = Str(d, s_studyDescription),
        ModalitiesInStudy = Joined(d, s_modalitiesInStudy),
        Modality = Str(d, s_modality),
        SeriesInstanceUid = Str(d, s_seriesUid),
        SeriesDescription = Str(d, s_seriesDescription),
        SeriesNumber = Int(d, s_seriesNumber),
        NumberOfSeries = Int(d, s_studySeries) ?? -1,
        NumberOfInstances = Int(d, level == TkLevel.Study ? s_studyInstances : s_seriesInstances) ?? -1,
    };

    private static DicomFile RetrieveIdentifier(string studyInstanceUid, string? seriesInstanceUid) =>
        Dimse.CreateQueryIdentifier(string.IsNullOrEmpty(seriesInstanceUid) ? QueryLevel.Study : QueryLevel.Series, new QueryKeys
        {
            ReturnKeys = false,
            StudyInstanceUid = studyInstanceUid,
            SeriesInstanceUid = string.IsNullOrEmpty(seriesInstanceUid) ? null : seriesInstanceUid,
        });

    private sealed class Relay(Action<TkNetResult>? progress) : IProgress<DimseResult>
    {
        public void Report(DimseResult value) => progress?.Invoke(TkNetResult.From(value));
    }

    /// <summary>C-GET of a study or a series into the store (existing instances are kept).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null for the study.</param>
    /// <param name="store">Destination.</param>
    /// <param name="progress">After every sub-operation (on the calling thread).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static TkNetResult Get(TkPeer peer, string studyInstanceUid, string? seriesInstanceUid, TkStore store, Action<TkNetResult>? progress,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        ArgumentNullException.ThrowIfNull(store);
        return Run(() =>
        {
            using DicomFile id = RetrieveIdentifier(studyInstanceUid, seriesInstanceUid);
            return TkNetResult.From(Dimse.Get(peer.ToBinding(), QueryRetrieveModel.StudyRoot, id.Dataset,
                new DimseGetOptions { Store = store.Binding }, new Relay(progress), null, cancellationToken));
        }, cancellationToken);
    }

    /// <summary>C-MOVE of a study or a series to a destination AE (our own Storage SCP).</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null for the study.</param>
    /// <param name="destinationAe">Move destination AE title.</param>
    /// <param name="progress">After every pending response (on the calling thread).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome.</returns>
    public static TkNetResult Move(TkPeer peer, string studyInstanceUid, string? seriesInstanceUid, string destinationAe,
        Action<TkNetResult>? progress, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        return Run(() =>
        {
            using DicomFile id = RetrieveIdentifier(studyInstanceUid, seriesInstanceUid);
            return TkNetResult.From(Dimse.Move(peer.ToBinding(), QueryRetrieveModel.StudyRoot, id.Dataset, destinationAe, new Relay(progress), null,
                cancellationToken));
        }, cancellationToken);
    }

    /// <summary>C-STORE of files over one association, with what happened to each file.</summary>
    /// <param name="peer">The peer (its AllowIncomplete and AllowLossyTranscode apply).</param>
    /// <param name="paths">Files.</param>
    /// <param name="progress">After every file (on the calling thread).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The outcome and one entry per path.</returns>
    public static (TkNetResult Result, IReadOnlyList<TkStoreOutcome> Files) StoreFiles(TkPeer peer, IReadOnlyList<string> paths,
        Action<TkNetResult>? progress, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        return Run(() =>
        {
            (DimseResult r, IReadOnlyList<StoreOutcome> files) = Dimse.StoreFilesWithOutcomes(peer.ToBinding(), paths, new Relay(progress), cancellationToken);
            var list = new TkStoreOutcome[files.Count];
            for (int i = 0; i < list.Length; i++)
            {
                StoreOutcome o = files[i];
                bool stored = o.Sent && o.Error == SiaaraErrorCode.Ok && (o.DimseStatus == 0 || (o.DimseStatus & 0xF000) == 0xB000);
                list[i] = new TkStoreOutcome(stored, (TkError)(int)o.Error, o.DimseStatus, o.IncompleteInput, o.UidChanged, o.Lossy, o.SopClassUid,
                    o.SopInstanceUid, o.OriginalSopInstanceUid);
            }
            return (TkNetResult.From(r), (IReadOnlyList<TkStoreOutcome>)list);
        }, cancellationToken);
    }

    /// <summary>Storage Commitment (push model) for instances already sent. With a receiver (our running SCP) the report
    /// may also arrive on a new association.</summary>
    /// <param name="peer">The peer.</param>
    /// <param name="instances">SOP Class and SOP Instance UIDs as sent.</param>
    /// <param name="wait">Total wait for the report.</param>
    /// <param name="receiver">Our SCP, or null (report on the same association only).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The per-instance result; Reported is false when no report came in time.</returns>
    public static TkCommitResult Commit(TkPeer peer, IReadOnlyList<(string SopClassUid, string SopInstanceUid)> instances, TimeSpan wait,
        TkScp? receiver, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        ArgumentNullException.ThrowIfNull(instances);
        try
        {
            CommitmentResult r = Dimse.Commit(peer.ToBinding(), instances, new CommitmentOptions { Wait = wait, Receiver = receiver?.Binding },
                cancellationToken);
            return new TkCommitResult(r.FailureReasons.Any(x => x != 0xFFFF), r.FailureReasons);
        }
        catch (SiaaraException e) when (e.ErrorCode == SiaaraErrorCode.Timeout)
        {
            return new TkCommitResult(false, [.. Enumerable.Repeat(0xFFFF, instances.Count)]);
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
    }
}

/// <summary>Kind of an event of our Storage SCP.</summary>
public enum TkScpEventKind
{
    /// <summary>An instance was stored.</summary>
    Stored,
    /// <summary>An instance could not be stored.</summary>
    StoreFailed,
    /// <summary>C-ECHO.</summary>
    Echo,
    /// <summary>C-FIND answered from the store.</summary>
    Find,
    /// <summary>C-MOVE or C-GET served from the store.</summary>
    Retrieve,
    /// <summary>A request was refused (not allowed for that AE).</summary>
    Refused,
    /// <summary>A Storage Commitment report arrived.</summary>
    CommitmentReport,
    /// <summary>Anything else.</summary>
    Other,
}

/// <summary>An event of our Storage SCP (no dataset values).</summary>
/// <param name="Kind">Kind.</param>
/// <param name="CallingAe">The other side's AE title.</param>
/// <param name="PeerAddress">The other side's address.</param>
/// <param name="DimseStatus">Status answered.</param>
/// <param name="Completed">Matches or sub-operations done.</param>
/// <param name="Failed">Failed sub-operations.</param>
public readonly record struct TkScpEvent(TkScpEventKind Kind, string CallingAe, string PeerAddress, int DimseStatus, int Completed, int Failed);

/// <summary>Counters of our Storage SCP.</summary>
/// <param name="ActiveAssociations">Associations now.</param>
/// <param name="Associations">Handled so far.</param>
/// <param name="Rejected">Associations rejected.</param>
/// <param name="Stored">Instances stored.</param>
/// <param name="StoreFailures">Instances refused or failed.</param>
/// <param name="Echoes">C-ECHO requests.</param>
/// <param name="Bytes">Bytes of stored instances.</param>
/// <param name="Refused">Requests refused.</param>
/// <param name="EventsDropped">Events not delivered.</param>
public readonly record struct TkScpStats(long ActiveAssociations, long Associations, long Rejected, long Stored, long StoreFailures, long Echoes,
                                         long Bytes, long Refused, long EventsDropped);

/// <summary>A remote AE our SCP may open associations to (C-MOVE destination, commitment report receiver).</summary>
/// <param name="AeTitle">AE title.</param>
/// <param name="Host">Host.</param>
/// <param name="Port">Port.</param>
public sealed record TkAeEntry(string AeTitle, string Host, int Port);

/// <summary>Options of our Storage SCP.</summary>
public sealed record TkScpOptions
{
    /// <summary>Our AE title.</summary>
    public required string AeTitle { get; init; }
    /// <summary>Port; 0 = any free port.</summary>
    public int Port { get; init; }
    /// <summary>Bind address; null or "" = every interface.</summary>
    public string? BindAddress { get; init; }
    /// <summary>Calling AE titles accepted.</summary>
    public IReadOnlyList<string> AllowedCallingAes { get; init; } = [];
    /// <summary>Accept every calling AE (only read when the list is empty).</summary>
    public bool AcceptAnyCallingAe { get; init; }
    /// <summary>TLS identity; null = plain TCP.</summary>
    public TkTls? Tls { get; init; }
    /// <summary>Also answer C-FIND / C-GET / C-MOVE from the store.</summary>
    public bool ServeQueryRetrieve { get; init; }
    /// <summary>C-MOVE destinations and commitment report receivers.</summary>
    public IReadOnlyList<TkAeEntry> AeTable { get; init; } = [];
}

/// <summary>Our Storage SCP (also Verification; optionally Query/Retrieve from the store). The only listener of the
/// app that is reachable from the network.</summary>
public sealed class TkScp : IDisposable
{
    private readonly StorageScp _scp;

    private TkScp(StorageScp scp)
    {
        _scp = scp;
        _scp.Received += OnReceived;
    }

    internal StorageScp Binding => _scp;

    /// <summary>Starts the SCP. Instances whose data set is incomplete are refused by the toolkit; C-STORE commands whose
    /// UIDs differ from the data set's are refused; compressed transfer syntaxes are accepted and kept as received.</summary>
    /// <param name="options">Options.</param>
    /// <param name="store">The store (must stay open while the SCP runs).</param>
    /// <returns>The running SCP.</returns>
    public static TkScp Start(TkScpOptions options, TkStore store)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(store);
        var o = new StorageScpOptions
        {
            AeTitle = options.AeTitle,
            Port = options.Port,
            BindAddress = string.IsNullOrEmpty(options.BindAddress) ? null : options.BindAddress,
            AllowedCallingAeTitles = options.AllowedCallingAes,
            AllowAnyCallingAeTitle = options.AcceptAnyCallingAe && options.AllowedCallingAes.Count == 0,
            AcceptCompressed = true,
            StrictStoreUids = true,
            Tls = options.Tls?.ToBinding(),
        };
        var s = new ScpServiceOptions
        {
            Storage = true,
            Find = options.ServeQueryRetrieve,
            Get = options.ServeQueryRetrieve,
            Move = options.ServeQueryRetrieve,
            CommitmentReports = true,
            AeTable = [.. options.AeTable.Select(a => new ScpAeEntry(a.AeTitle, a.Host, a.Port))],
        };
        return new TkScp(Tk.Call(() => StorageScp.Start(o, s, store.Binding)));
    }

    /// <summary>The listening port.</summary>
    public int Port => _scp.Port;

    /// <summary>Raised per event on the SCP's dispatcher thread; handlers must be quick and must not throw.</summary>
    public event EventHandler<TkScpEventArgs>? Received;

    private void OnReceived(object? sender, ScpEvent e)
    {
        TkScpEventKind kind = e.Kind switch
        {
            ScpEventKind.Stored => TkScpEventKind.Stored,
            ScpEventKind.StoreFailed => TkScpEventKind.StoreFailed,
            ScpEventKind.Echo => TkScpEventKind.Echo,
            ScpEventKind.Find => TkScpEventKind.Find,
            ScpEventKind.Move or ScpEventKind.Get => TkScpEventKind.Retrieve,
            ScpEventKind.Refused => TkScpEventKind.Refused,
            ScpEventKind.CommitmentReport => TkScpEventKind.CommitmentReport,
            _ => TkScpEventKind.Other,
        };
        Received?.Invoke(this, new TkScpEventArgs(new TkScpEvent(kind, e.CallingAeTitle, e.PeerAddress, e.DimseStatus, e.Completed, e.Failed)));
    }

    /// <summary>Counters.</summary>
    /// <returns>The counters.</returns>
    public TkScpStats GetStatistics()
    {
        StorageScpStatistics s = Tk.Call(_scp.GetStatistics);
        return new TkScpStats(s.ActiveAssociations, s.AssociationsHandled, s.AssociationsRejected, s.InstancesStored, s.StoreFailures, s.Echoes,
            s.BytesReceived, s.Refused, s.EventsDropped);
    }

    /// <summary>Stops listening and aborts running associations.</summary>
    public void Dispose()
    {
        _scp.Received -= OnReceived;
        _scp.Dispose();
    }
}

/// <summary>An SCP event.</summary>
public sealed class TkScpEventArgs : EventArgs
{
    /// <summary>Creates the event.</summary>
    /// <param name="event">The event.</param>
    public TkScpEventArgs(TkScpEvent @event) => Event = @event;

    /// <summary>The event.</summary>
    public TkScpEvent Event { get; }
}

/// <summary>Progress of a streamed DICOMweb retrieve.</summary>
/// <param name="Parts">Instances kept so far.</param>
/// <param name="Bytes">Bytes received.</param>
/// <param name="TotalBytes">Total when the server announced it, else 0.</param>
public readonly record struct TkWebProgress(int Parts, long Bytes, long TotalBytes);

/// <summary>A DICOMweb client (QIDO-RS, WADO-RS, STOW-RS) for one server. One request at a time.</summary>
public sealed class TkWebClient : IDisposable
{
    private readonly DicomWebClient _client;

    /// <summary>Creates a client.</summary>
    /// <param name="baseAddress">http:// or https:// base URL of the DICOMweb service.</param>
    /// <param name="tls">TLS settings for https, or null (operating system trust store).</param>
    /// <param name="authorization">Value of the Authorization header, or null.</param>
    /// <param name="timeout">Timeout of each wait for the server.</param>
    public TkWebClient(string baseAddress, TkTls? tls, string? authorization, TimeSpan? timeout = null)
    {
        _client = Tk.Call(() => tls is null ? new DicomWebClient(baseAddress, timeout) : new DicomWebClient(baseAddress, tls.ToBinding(), timeout));
        try
        {
            if (!string.IsNullOrEmpty(authorization))
            {
                Tk.Call(() => _client.SetAuthorization(authorization));
            }
            // A streamed study retrieve may run longer than the default whole-body deadline.
            Tk.Call(() => _client.SetLimits(new DicomWebLimits { NoBodyTimeout = true }));
        }
        catch
        {
            _client.Dispose();
            throw;
        }
    }

    /// <summary>QIDO-RS search; the DICOM JSON answer as text (values as the server sent them).</summary>
    /// <param name="level">Study or Series.</param>
    /// <param name="query">Query string without '?', or null.</param>
    /// <param name="studyInstanceUid">Study for a series search, or null.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The JSON array ("" or "[]" when nothing matched).</returns>
    public string Search(TkLevel level, string? query, string? studyInstanceUid, CancellationToken cancellationToken)
    {
        try
        {
            return _client.SearchAsync(level == TkLevel.Series ? QidoLevel.Series : QidoLevel.Studies, query, studyInstanceUid, null,
                cancellationToken).GetAwaiter().GetResult();
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
    }

    /// <summary>WADO-RS retrieve of a study or series, streamed part by part into the store.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <param name="seriesInstanceUid">Series, or null.</param>
    /// <param name="store">Destination.</param>
    /// <param name="progress">After every part (on the worker thread).</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>(stored, failed) parts.</returns>
    public (int Stored, int Failed) RetrieveTo(string studyInstanceUid, string? seriesInstanceUid, TkStore store, Action<TkWebProgress>? progress,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(store);
        try
        {
            return _client.RetrieveToAsync(studyInstanceUid, string.IsNullOrEmpty(seriesInstanceUid) ? null : seriesInstanceUid, null, store.Binding, null,
                false, null, new WebRelay(progress), cancellationToken).GetAwaiter().GetResult();
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
    }

    private sealed class WebRelay(Action<TkWebProgress>? progress) : IProgress<DicomWebTransferProgress>
    {
        public void Report(DicomWebTransferProgress value) => progress?.Invoke(new TkWebProgress(value.Parts, value.Bytes, value.TotalBytes));
    }

    /// <summary>STOW-RS of one file. A file whose read was not complete is not sent unless allowed
    /// (TkError.Truncated is thrown).</summary>
    /// <param name="path">The file.</param>
    /// <param name="allowIncomplete">Send it even when its read was not complete.</param>
    /// <param name="cancellationToken">Cancels.</param>
    public void Store(string path, bool allowIncomplete, CancellationToken cancellationToken)
    {
        try
        {
            using DicomFile f = DicomFile.Open(path);
            if (!allowIncomplete && !f.ReadReport.IsComplete)
            {
                throw new TkException(TkError.Truncated);
            }
            // The call blocks until the request has ended, so the file outlives it.
#pragma warning disable CA2025
            _client.StoreAsync(f, null, cancellationToken).GetAwaiter().GetResult();
#pragma warning restore CA2025
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
    }

    /// <inheritdoc/>
    public void Dispose() => _client.Dispose();
}

/// <summary>The toolkit's DICOMweb server over a store, on the loopback interface. The viewer does not offer it to
/// users; it is the counterpart the app's own DICOMweb tests run against.</summary>
public sealed class TkWebServer : IDisposable
{
    private readonly DicomWebServer _server;

    private TkWebServer(DicomWebServer server) => _server = server;

    /// <summary>Starts a server on 127.0.0.1 at a free port.</summary>
    /// <param name="store">The store it serves.</param>
    /// <param name="authorize">Checks the Authorization header of every request, or null for no check.</param>
    /// <returns>The server.</returns>
    public static TkWebServer StartLoopback(TkStore store, Func<string, bool>? authorize = null)
    {
        ArgumentNullException.ThrowIfNull(store);
        return new TkWebServer(Tk.Call(() => DicomWebServer.Start(new DicomWebServerOptions
        {
            Port = 0,
            Authorize = authorize is null ? null : (_, _, header) => authorize(header),
        }, store.Binding)));
    }

    /// <summary>Base URL of the service.</summary>
    public string BaseAddress => string.Create(CultureInfo.InvariantCulture, $"http://127.0.0.1:{_server.Port}/dicomweb");

    /// <inheritdoc/>
    public void Dispose() => _server.Dispose();
}
