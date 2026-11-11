using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using Siaara.Network;
using Siaara.Web;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>The full service set: SCP services, synchronous and callback DIMSE calls, DICOMweb server.</summary>
internal static class Network2Tests
{
    private static DimsePeer Peer(int port, string called, string calling = "N2_SCU", bool lossy = false) => new()
    {
        Host = "127.0.0.1",
        Port = port,
        CallingAeTitle = calling,
        CalledAeTitle = called,
        ConnectTimeout = TimeSpan.FromSeconds(3),
        AssociationTimeout = TimeSpan.FromSeconds(3),
        DimseTimeout = TimeSpan.FromSeconds(5),
        AllowLossyTranscode = lossy,
    };

    private static StorageScpOptions Options(string ae) => new()
    {
        AeTitle = ae,
        BindAddress = "127.0.0.1",
        AllowAnyCallingAeTitle = true,
    };

    private static void Fill(DicomStore store, int n, string study, string series, string pid)
    {
        for (int i = 0; i < n; i++)
        {
            using DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sopUid: series + "." + (i + 1), seriesUid: series,
                studyUid: study, patientId: pid);
            string p = Path.Combine(Path.GetTempPath(), "siaara_n2_" + series + "_" + i + ".dcm");
            f.Save(p);
            store.AddFile(p);
            File.Delete(p);
        }
    }

    internal static void Register()
    {
        Case("network2: testing 10b - per-file outcomes (UID sent, lossy), commitment of them, incomplete input refused", () =>
        {
            string dir = NewTempDir("n2t10b");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            using StorageScp scp = StorageScp.Start(Options("T10B"), new ScpServiceOptions
            {
                Storage = true,
                Commitment = true,
                CommitmentReportMode = CommitmentReportMode.Same,
                TransferSyntaxes = new[] { DicomTransferSyntaxes.JPEGBaseline8Bit },
            }, store);
            string good = Path.Combine(dir, "rgb.dcm");
            using (DicomFile f = Builders.Rgb(8, 8))
            {
                f.Save(good);
            }
            byte[] all = File.ReadAllBytes(good);
            string truncated = Path.Combine(dir, "truncated.dcm");
            File.WriteAllBytes(truncated, all.AsSpan(0, all.Length - 30).ToArray());

            (DimseResult r, IReadOnlyList<StoreOutcome> files) =
                Dimse.StoreFilesWithOutcomes(Peer(scp.Port, "T10B", lossy: true), new[] { good, truncated });
            CheckEq(r.Outcome, SiaaraErrorCode.Rejected);
            CheckEq(r.Completed, 1);
            CheckEq(r.Failed, 1);
            CheckEq(files.Count, 2);
            Check(files[0].Sent && files[0].Lossy && files[0].UidChanged);
            CheckEq(files[0].LossyHistory, LossyHistory.Aligned);
            CheckEq(files[1].LossyHistory, LossyHistory.NotApplicable);
            CheckEq(files[0].OriginalSopInstanceUid, "2.25.900");
            Check(files[0].SopInstanceUid != "2.25.900");
            CheckEq(files[0].TransferSyntaxUid, DicomTransferSyntaxes.JPEGBaseline8Bit);
            CheckEq(files[1].Error, SiaaraErrorCode.Rejected);
            Check(files[1].IncompleteInput && !files[1].Sent);
            CheckEq(files[1].DimseStatus, 0xFFFF);

            CommitmentResult cr = Dimse.Commit(Peer(scp.Port, "T10B"), new[] { (files[0].SopClassUid, files[0].SopInstanceUid) });
            Check(cr.Result.IsSuccess);
            CheckEq(cr.Result.Completed, 1);

            var keep = new DimsePeer
            {
                Host = "127.0.0.1", Port = scp.Port, CallingAeTitle = "N2_SCU", CalledAeTitle = "T10B",
                AllowLossyTranscode = true, KeepUidOnLossy = true, AllowIncomplete = true,
            };
            (DimseResult r2, IReadOnlyList<StoreOutcome> files2) = Dimse.StoreFilesWithOutcomes(keep, new[] { good });
            Check(r2.IsSuccess);
            Check(files2[0].Lossy && !files2[0].UidChanged);
            CheckEq(files2[0].SopInstanceUid, "2.25.900");
            using (DicomFile mem = Builders.Rgb(4, 4))
            {
                (DimseResult r3, StoreOutcome one) = Dimse.StoreWithOutcome(Peer(scp.Port, "T10B", lossy: true), mem);
                Check(r3.IsSuccess);
                Check(one.UidChanged);
                CheckEq(one.OriginalSopInstanceUid, "2.25.900");
            }
        });

        Case("network2: Q/R from the store - synchronous Find, per-match callback, Move, Get with options, events", () =>
        {
            string dir = NewTempDir("n2qr");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            using DicomStore destStore = DicomStore.Open(Path.Combine(dir, "dest"));
            Fill(store, 2, "2.25.3100", "2.25.3101", "QR1");
            Fill(store, 1, "2.25.3200", "2.25.3201", "QR2");
            using StorageScp dest = StorageScp.Start(Options("DEST"), destStore);
            var events = new ConcurrentQueue<ScpEvent>();
            using StorageScp scp = StorageScp.Start(Options("QR_SCP"), new ScpServiceOptions
            {
                Find = true,
                Move = true,
                Get = true,
                Relational = true,
                AeTable = new[] { new ScpAeEntry("DEST", "127.0.0.1", dest.Port) },
            }, store);
            scp.Received += (_, e) => events.Enqueue(e);
            DimsePeer peer = Peer(scp.Port, "QR_SCP");
            Check(Dimse.Echo(peer).IsSuccess);

            using (DicomFile id = Dimse.CreateQueryIdentifier(QueryLevel.Study))
            {
                (DimseResult r, IReadOnlyList<DicomFile> matches) = Dimse.Find(peer, QueryRetrieveModel.StudyRoot, id.Dataset);
                Check(r.IsSuccess);
                CheckEq(matches.Count, 2);
                foreach (DicomFile m in matches)
                {
                    m.Dispose();
                }
            }
            using (DicomFile id = Dimse.CreateQueryIdentifier(QueryLevel.Image, new QueryKeys { PatientId = "QR1" }))
            {
                var uids = new List<string>();
                DimseResult r = Dimse.Find(peer, QueryRetrieveModel.StudyRoot, id.Dataset, m =>
                {
                    uids.Add(m.GetRawString(DicomTags.SOPInstanceUID) ?? string.Empty);
                    return true;
                }, new QueryOptions { Negotiation = QueryNegotiation.Relational });
                Check(r.IsSuccess);
                Check(r.AcceptedNegotiation.HasFlag(QueryNegotiation.Relational));
                CheckEq(uids.Count, 2);
                int seen = 0;
                DimseResult stopped = Dimse.FindAsync(peer, QueryRetrieveModel.StudyRoot, id.Dataset, _ => ++seen < 1,
                    new QueryOptions { Negotiation = QueryNegotiation.Relational }).GetAwaiter().GetResult();
                Check(stopped.IsSuccess);
                CheckEq(seen, 1);
            }
            using (DicomFile id = Dimse.CreateQueryIdentifier(QueryLevel.Study, new QueryKeys { StudyInstanceUid = "2.25.3100", ReturnKeys = false }))
            {
                DimseResult moved = Dimse.Move(peer, QueryRetrieveModel.StudyRoot, id.Dataset, "DEST");
                Check(moved.IsSuccess);
                CheckEq(moved.Completed, 2);
                CheckEq(destStore.GetStatistics().Instances, 2L);
                string folder = Path.Combine(dir, "got");
                Directory.CreateDirectory(folder);
                DimseResult got = Dimse.Get(peer, QueryRetrieveModel.StudyRoot, id.Dataset, new DimseGetOptions
                {
                    Folder = folder,
                    NativeOnly = true,
                    StrictStoreUids = true,
                    SopClasses = new[] { Builders.CtImageStorage },
                });
                Check(got.IsSuccess);
                CheckEq(got.Completed, 2);
                CheckEq(Directory.GetFiles(folder).Length, 2);
            }
            SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Get), 5000);
            Check(SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Move && e.DestinationAeTitle == "DEST" && e.Completed == 2), 10000));
            Check(SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Find && e.DatasetJson is not null), 10000));
            StorageScpStatistics st = scp.GetStatistics();
            Check(st.Finds >= 3);
            CheckEq(st.Retrieves, 2L);
            CheckEq(st.SubOperations, 4L);
        });

        Case("network2: worklist provider, MPPS, allow-lists, Storage Commitment with a receiver, UPS, events", () =>
        {
            string dir = NewTempDir("n2svc");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            using DicomStore rstore = DicomStore.Open(Path.Combine(dir, "recv"));
            Fill(store, 1, "2.25.3300", "2.25.3301", "C1");
            using DicomFile item = DicomFile.FromJson("{\"00100020\":{\"vr\":\"LO\",\"Value\":[\"W1\"]},\"00400100\":{\"vr\":\"SQ\",\"Value\":[{\"00080060\":{\"vr\":\"CS\",\"Value\":[\"CT\"]},\"00400001\":{\"vr\":\"AE\",\"Value\":[\"CT01\"]}}]}}");
            var revents = new ConcurrentQueue<ScpEvent>();
            using StorageScp receiver = StorageScp.Start(Options("RECV"), new ScpServiceOptions { Storage = false, CommitmentReports = true }, rstore);
            receiver.Received += (_, e) => revents.Enqueue(e);
            var events = new ConcurrentQueue<ScpEvent>();
            string? queried = null;
            using StorageScp scp = StorageScp.Start(Options("SVC_SCP"), new ScpServiceOptions
            {
                Worklist = true,
                WorklistProvider = q =>
                {
                    queried = q.CallingAeTitle;
                    return new[] { item.Dataset };
                },
                Mpps = true,
                Commitment = true,
                CommitmentReportMode = CommitmentReportMode.New,
                Ups = true,
                AeTable = new[] { new ScpAeEntry("RECV", "127.0.0.1", receiver.Port) },
                AllowLists = new Dictionary<ScpService, IReadOnlyList<string>> { [ScpService.Mpps] = new[] { "MODALITY" } },
            }, store);
            scp.Received += (_, e) => events.Enqueue(e);

            using (DicomFile q = Dimse.CreateWorklistQuery(modality: "CT"))
            {
                (DimseResult r, IReadOnlyList<DicomFile> m) = Dimse.Find(Peer(scp.Port, "SVC_SCP"), QueryRetrieveModel.Worklist, q.Dataset);
                Check(r.IsSuccess);
                CheckEq(m.Count, 1);
                CheckEq(m[0].Dataset.GetRawString(DicomTags.PatientID), "W1");
                CheckEq(queried, "N2_SCU");
                foreach (DicomFile f in m)
                {
                    f.Dispose();
                }
            }
            using (DicomFile q = Dimse.CreateWorklistQuery(modality: "MR"))
            {
                (DimseResult r, IReadOnlyList<DicomFile> m) = Dimse.Find(Peer(scp.Port, "SVC_SCP"), QueryRetrieveModel.Worklist, q.Dataset);
                Check(r.IsSuccess);
                CheckEq(m.Count, 0);
            }

            using DicomFile mpps = DicomFile.Create();
            mpps.Dataset.SetString(new DicomTag(0x00400252u), "CS", "IN PROGRESS");
            (DimseResult created, string uid) = Dimse.MppsCreate(Peer(scp.Port, "SVC_SCP", "MODALITY"), mpps.Dataset);
            Check(created.IsSuccess);
            Check(uid.Length > 5);
            using DicomFile done = DicomFile.Create();
            done.Dataset.SetString(new DicomTag(0x00400252u), "CS", "COMPLETED");
            Check(Dimse.MppsSetAsync(Peer(scp.Port, "SVC_SCP", "MODALITY"), uid, done.Dataset).GetAwaiter().GetResult().IsSuccess);
            using (DicomFile? state = scp.GetMpps(uid))
            {
                CheckEq(state?.Dataset.GetRawString(new DicomTag(0x00400252u)), "COMPLETED");
            }
            Check(scp.GetMpps("2.25.1") is null);
            (DimseResult refused, _) = Dimse.MppsCreate(Peer(scp.Port, "SVC_SCP", "OTHER"), mpps.Dataset);
            CheckEq(refused.Outcome, SiaaraErrorCode.Rejected);
            CheckEq(refused.DimseStatus, 0x0124);

            CommitmentResult cr = Dimse.Commit(Peer(scp.Port, "SVC_SCP", "RECV"),
                new[] { (Builders.CtImageStorage, "2.25.3301.1"), (Builders.CtImageStorage, "2.25.9.9") },
                new CommitmentOptions { Receiver = receiver, Wait = TimeSpan.FromSeconds(8), SameAssociationWait = TimeSpan.Zero });
            CheckEq(cr.Result.Completed, 1);
            CheckEq(cr.Result.Failed, 1);
            CheckEq(cr.FailureReasons[1], 0x0112);
            Check(cr.TransactionUid.Length > 5);
            SpinWait.SpinUntil(() => revents.Any(e => e.Kind == ScpEventKind.CommitmentReport), 5000);
            Check(SpinWait.SpinUntil(() => revents.Any(e => e.Kind == ScpEventKind.CommitmentReport && e.TransactionUid == cr.TransactionUid), 10000));

            using DicomFile ups = DicomFile.Create();
            ups.Dataset.SetString(new DicomTag(0x00741000u), "CS", "SCHEDULED");
            NResponse nr = Dimse.SendNRequest(Peer(scp.Port, "SVC_SCP"), new NRequest(NCommand.Create, "1.2.840.10008.5.1.4.34.6.1")
            {
                SopInstanceUid = "2.25.3401",
                Dataset = ups.Dataset,
            });
            Check(nr.Result.IsSuccess);
            CheckEq(nr.SopInstanceUid, "2.25.3401");
            string local = scp.CreateUps(ups.Dataset);
            Check(local.Length > 5);
            using (DicomFile? got = scp.GetUps(local))
            {
                Check(got is not null);
            }
            SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Ups), 5000);
            Check(SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.MppsCreate && e.SopInstanceUid == uid && e.DatasetJson is not null), 10000));
            Check(SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Refused && e.CallingAeTitle == "OTHER"), 10000));
            Check(SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Worklist && e.Completed == 1), 10000));
            Check(SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.CommitmentRequest && e.Outcome == 1), 10000));
            StorageScpStatistics st = scp.GetStatistics();
            CheckEq(st.Refused, 1L);
            CheckEq(st.Commitments, 1L);
        });

        Case("network2: R-093 lossy C-STORE reports the new SOP Instance UID; StorageScpOptions R3 fields", () =>
        {
            string dir = NewTempDir("n2lossy");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            var events = new ConcurrentQueue<ScpEvent>();
            using StorageScp scp = StorageScp.Start(Options("LOSSY_SCP") with
            {
                StrictStoreUids = true,
                SpoolDirectory = Path.Combine(dir, "spool"),
                SpoolThreshold = 1 << 20,
                EventQueueLimit = 16,
            }, new ScpServiceOptions
            {
                StorageSopClasses = new[] { Builders.SecondaryCapture },
                TransferSyntaxes = new[] { "1.2.840.10008.1.2.4.50" },
            }, store);
            scp.Received += (_, e) => events.Enqueue(e);
            using DicomFile rgb = Builders.Rgb(16, 16);
            rgb.Dataset.SetRawString(DicomTags.StudyInstanceUID, "UI", "2.25.3500");
            rgb.Dataset.SetRawString(DicomTags.SeriesInstanceUID, "UI", "2.25.3501");
            var changes = new List<UidChange>();
            DimseResult r = Dimse.Store(Peer(scp.Port, "LOSSY_SCP", lossy: true), rgb, changes.Add);
            Check(r.IsSuccess);
            CheckEq(changes.Count, 1);
            CheckEq(changes[0].OriginalSopInstanceUid, "2.25.900");
            Check(changes[0].NewSopInstanceUid != "2.25.900");
            SpinWait.SpinUntil(() => events.Any(e => e.Kind == ScpEventKind.Stored), 5000);
            ScpEvent stored = events.First(e => e.Kind == ScpEventKind.Stored);
            CheckEq(stored.SopInstanceUid, changes[0].NewSopInstanceUid);
            Check(!stored.UidMismatch);
            Check(stored.PeerAddress.Length > 0);
            CheckEq(stored.Outcome, 0);
            CheckEq(scp.GetStatistics().EventsDropped, 0L);
        });

        Case("network2: DICOMweb server over a store, streamed retrieve, client limits, authorize", () =>
        {
            string dir = NewTempDir("n2web");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            using DicomStore copy = DicomStore.Open(Path.Combine(dir, "copy"));
            Fill(store, 3, "2.25.3600", "2.25.3601", "WEB");
            using DicomWebServer server = DicomWebServer.Start(new DicomWebServerOptions
            {
                Authorize = (method, _, auth) => method != "POST" || auth == "Bearer ok",
            }, store);
            using var client = new DicomWebClient("http://127.0.0.1:" + server.Port + "/dicomweb", TimeSpan.FromSeconds(10));
            string json = client.Search(QidoLevel.Studies, "PatientID=WEB");
            Check(json.Contains("2.25.3600", StringComparison.Ordinal));
            var progress = new List<DicomWebTransferProgress>();
            Directory.CreateDirectory(Path.Combine(dir, "files"));
            (int stored, int failed) = client.RetrieveTo("2.25.3600", null, null, copy, Path.Combine(dir, "files"), progress: new Collect(progress));
            CheckEq(stored, 3);
            CheckEq(failed, 0);
            CheckEq(copy.GetStatistics().Instances, 3L);
            CheckEq(progress.Count, 3);
            client.SetLimits(new DicomWebLimits { MaxBodyBytes = 100 });
            Throws<SiaaraException>(() => client.Retrieve("2.25.3600"));
            client.SetLimits(new DicomWebLimits { MaxBodyBytes = 256L << 20, MaxPartBytes = 1 << 20 });
            (stored, _) = client.RetrieveToAsync("2.25.3600", "2.25.3601", null, copy, null).GetAwaiter().GetResult();
            CheckEq(stored, 3);
            using DicomFile extra = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sopUid: "2.25.3601.9", seriesUid: "2.25.3601",
                studyUid: "2.25.3600", patientId: "WEB");
            CheckEq(Throws<SiaaraException>(() => client.Store(extra))?.ErrorCode, SiaaraErrorCode.Rejected);
            client.SetAuthorization("Bearer ok");
            client.Store(extra);
            CheckEq(store.GetStatistics().Instances, 4L);

            // STOW-RS does not send a file whose read was not complete, unless allowed.
            using DicomFile whole = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sopUid: "2.25.3601.10", seriesUid: "2.25.3601",
                studyUid: "2.25.3600", patientId: "WEB");
            string wholePath = Path.Combine(dir, "whole.dcm");
            whole.Save(wholePath);
            byte[] all = File.ReadAllBytes(wholePath);
            using DicomFile cut = DicomFile.Open(all.AsSpan(0, all.Length - 3));
            SiaaraException? refused = Throws<SiaaraException>(() => client.Store(cut));
            CheckEq(refused?.ErrorCode, SiaaraErrorCode.Rejected);
            Check(refused?.Message.Contains("INCOMPLETE_INPUT", StringComparison.Ordinal) == true);
            CheckEq(store.GetStatistics().Instances, 4L);
            client.Store(cut, null, allowIncomplete: true);
            CheckEq(store.GetStatistics().Instances, 5L);
        });

        Case("network2: ScpServiceOptions.AllowIncomplete decides C-GET sub-operations for a truncated stored file", () =>
        {
            string dir = NewTempDir("n2inc");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            Fill(store, 1, "2.25.3700", "2.25.3701", "INC");
            using (DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sopUid: "2.25.3701.77", seriesUid: "2.25.3701",
                studyUid: "2.25.3700", patientId: "INC"))
            {
                string p = Path.Combine(dir, "whole.dcm");
                f.Save(p);
                byte[] all = File.ReadAllBytes(p);
                string t = Path.Combine(dir, "truncated.dcm");
                File.WriteAllBytes(t, all.AsSpan(0, all.Length - 3).ToArray());
                store.AddFile(t);
            }
            CheckEq(store.GetStatistics().Instances, 2L);
            foreach (bool allow in new[] { false, true })
            {
                using StorageScp scp = StorageScp.Start(Options("INC_SCP"), new ScpServiceOptions
                {
                    Get = true,
                    AllowIncomplete = allow,
                }, store);
                using DicomFile id = Dimse.CreateQueryIdentifier(QueryLevel.Study,
                    new QueryKeys { StudyInstanceUid = "2.25.3700", ReturnKeys = false });
                string folder = Path.Combine(dir, allow ? "got1" : "got0");
                Directory.CreateDirectory(folder);
                DimseResult got = Dimse.Get(Peer(scp.Port, "INC_SCP"), QueryRetrieveModel.StudyRoot, id.Dataset, new DimseGetOptions
                {
                    Folder = folder,
                    NativeOnly = true,
                    SopClasses = new[] { Builders.CtImageStorage },
                });
                CheckEq(got.Completed, allow ? 2 : 1);
                CheckEq(got.Failed, allow ? 0 : 1);
                CheckEq(Directory.GetFiles(folder).Length, allow ? 2 : 1);
            }
        });
    }

    private sealed class Collect : IProgress<DicomWebTransferProgress>
    {
        private readonly List<DicomWebTransferProgress> _list;

        internal Collect(List<DicomWebTransferProgress> list) => _list = list;

        public void Report(DicomWebTransferProgress value) => _list.Add(value);
    }
}
