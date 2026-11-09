using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using Siaara.Network;
using Siaara.Volumes;
using Siaara.Web;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>Storage SCP, DIMSE SCU, logging, DICOMweb additions, cancellable long calls.</summary>
internal static class NetworkTests
{
    private sealed class SyncProgress<TValue> : IProgress<TValue>
    {
        internal List<TValue> Seen { get; } = new();

        public void Report(TValue value)
        {
            lock (Seen)
            {
                Seen.Add(value);
            }
        }
    }

    private static DimsePeer Peer(int port, string calling = "CS_SCU") => new()
    {
        Host = "127.0.0.1",
        Port = port,
        CallingAeTitle = calling,
        CalledAeTitle = "CS_SCP",
        ConnectTimeout = TimeSpan.FromSeconds(3),
        AssociationTimeout = TimeSpan.FromSeconds(3),
        DimseTimeout = TimeSpan.FromSeconds(5),
    };

    internal static void Register()
    {
        Case("network: Storage SCP, C-ECHO, allowed peers, C-STORE files and memory, events, statistics, logging", () =>
        {
            var logs = new ConcurrentQueue<(SiaaraLogLevel, bool, string)>();
            SiaaraLog.SetHandler((l, phi, m) => logs.Enqueue((l, phi, m)), SiaaraLogLevel.Debug);
            string dir = NewTempDir("scp");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            using StorageScp scp = StorageScp.Start(new StorageScpOptions
            {
                AeTitle = "CS_SCP",
                BindAddress = "127.0.0.1",
                AllowedCallingAeTitles = new[] { "CS_SCU" },
            }, store);
            var events = new ConcurrentQueue<ScpEvent>();
            scp.Received += (_, e) => events.Enqueue(e);
            Check(scp.Port > 0);
            DimseResult echo = Dimse.EchoAsync(Peer(scp.Port)).GetAwaiter().GetResult();
            Check(echo.IsSuccess);
            CheckEq(echo.DimseStatus, 0);
            DimseResult stranger = Dimse.EchoAsync(Peer(scp.Port, "STRANGER")).GetAwaiter().GetResult();
            CheckEq(stranger.Outcome, SiaaraErrorCode.Rejected);

            var paths = new List<string>();
            for (int i = 0; i < 3; i++)
            {
                using DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sopUid: "2.25.71" + i);
                if (i == 1)
                {
                    f.Transcode(DicomTransferSyntaxes.RLELossless);
                }
                string p = Path.Combine(dir, "f" + i + ".dcm");
                f.Save(p);
                paths.Add(p);
            }
            paths.Add(Path.Combine(dir, "missing.dcm"));
            var progress = new SyncProgress<DimseResult>();
            DimseResult stored = Dimse.StoreFilesAsync(Peer(scp.Port), paths, progress).GetAwaiter().GetResult();
            CheckEq(stored.Outcome, SiaaraErrorCode.Rejected);
            CheckEq(stored.Completed, 3);
            CheckEq(stored.Failed, 1);
            CheckEq(progress.Seen.Count, 4);
            using (DicomFile mem = Builders.Ct(2, 2, new short[] { 5, 6, 7, 8 }, sopUid: "2.25.7199"))
            {
                Check(Dimse.StoreAsync(Peer(scp.Port), mem).GetAwaiter().GetResult().IsSuccess);
            }
            CheckEq(store.GetStatistics().Instances, 4L);
            StorageScpStatistics st = scp.GetStatistics();
            CheckEq(st.InstancesStored, 4L);
            CheckEq(st.Echoes, 1L);
            Check(st.AssociationsRejected >= 1);
            Check(events.Count(e => e.Kind == ScpEventKind.Stored) == 4);
            Check(events.Any(e => e.Kind == ScpEventKind.Echo && e.CallingAeTitle == "CS_SCU"));
            scp.Stop();
            Throws<SiaaraException>(() => Dimse.EchoAsync(Peer(scp.Port)).GetAwaiter().GetResult());
            SiaaraLog.SetHandler(null);
            Check(!logs.IsEmpty);
            Throws<ArgumentException>(() => StorageScp.Start(new StorageScpOptions(), null!));
            CheckEq(Throws<SiaaraException>(() => StorageScp.Start(new StorageScpOptions(), store))?.ErrorCode, SiaaraErrorCode.InvalidArgument);
        });

        Case("network: cancellation of DIMSE and DICOMweb calls, worklist query", () =>
        {
            using (var cts = new CancellationTokenSource())
            {
                cts.Cancel();
                Throws<OperationCanceledException>(() => Dimse.EchoAsync(Peer(1), cts.Token).GetAwaiter().GetResult());
            }
            using (DicomFile wl = Dimse.CreateWorklistQuery("CT01", "CT", "20260101-20261231"))
            {
                CheckEq(wl.Dataset.GetItemCount(new DicomTag(0x00400100u)), 1);
            }
            // A server that never answers: cancellation aborts the running request.
            using var server = new LoopbackHttpServer((m, p, b) =>
            {
                Thread.Sleep(4000);
                return (200, "application/dicom+json", "[]"u8.ToArray());
            });
            using var client = new DicomWebClient("http://127.0.0.1:" + server.Port + "/dicom-web", TimeSpan.FromSeconds(10));
            using var cts2 = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
            var sw = Stopwatch.StartNew();
            Throws<OperationCanceledException>(() => client.RetrieveMetadataAsync("2.25.1", cancellationToken: cts2.Token).GetAwaiter().GetResult());
            Check(sw.ElapsedMilliseconds < 3000);
        });

        Case("network: DICOMweb metadata and WADO-URI", () =>
        {
            byte[] part10;
            using (DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sopUid: "2.25.8801", seriesUid: "2.25.8802", studyUid: "2.25.8803"))
            {
                string p = Path.Combine(NewTempDir("wado"), "x.dcm");
                f.Save(p);
                part10 = File.ReadAllBytes(p);
            }
            using var server = new LoopbackHttpServer((m, p, b) =>
            {
                if (p.EndsWith("/metadata", StringComparison.Ordinal) && p.Contains("2.25.8803", StringComparison.Ordinal))
                {
                    return (200, "application/dicom+json", Encoding.UTF8.GetBytes("[{\"00080018\":{\"vr\":\"UI\",\"Value\":[\"2.25.8801\"]}}]"));
                }
                if (p.Contains("requestType=WADO", StringComparison.Ordinal))
                {
                    return (200, "application/dicom", part10);
                }
                return (404, "text/plain", "no"u8.ToArray());
            });
            using var client = new DicomWebClient("http://127.0.0.1:" + server.Port + "/dicom-web", TimeSpan.FromSeconds(10));
            string json = client.RetrieveMetadataAsync("2.25.8803", "2.25.8802").GetAwaiter().GetResult();
            Check(json.Contains("2.25.8801", StringComparison.Ordinal));
            Check(server.Requests.Any(r => r.Contains("/studies/2.25.8803/series/2.25.8802/metadata", StringComparison.Ordinal)));
            CheckEq(Throws<SiaaraException>(() => client.RetrieveMetadata("2.25.9"))?.ErrorCode, SiaaraErrorCode.NotFound);
            byte[] body = client.WadoUri("2.25.8803", "2.25.8802", "2.25.8801");
            CheckEq(body.Length, part10.Length);
            using DicomFile back = new DicomWebPart("application/dicom", body).ToDicomFile();
            CheckEq(back.Dataset.GetRawString(DicomTags.SOPInstanceUID), "2.25.8801");
        });

        Case("network: volume build, store verify / rebuild, DICOMDIR write with progress and cancellation", () =>
        {
            string dir = NewTempDir("long");
            string[] paths = Builders.CtSeries(dir, 4, 4, 4, (i, j, k) => (short)k);
            var progress = new SyncProgress<OperationProgress>();
            using (Volume v = Volume.BuildAsync(paths, new VolumeOptions { Threads = 1 }, progress).GetAwaiter().GetResult())
            {
                CheckEq(v.Properties.Dimensions.K, 4);
            }
            CheckEq(progress.Seen.Count, 4);
            CheckEq(progress.Seen[^1], new OperationProgress(4, 4));
            using (var cts = new CancellationTokenSource())
            {
                cts.Cancel();
                Throws<OperationCanceledException>(() => Volume.BuildAsync(paths, null, null, cts.Token).GetAwaiter().GetResult());
            }
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "store"));
            foreach (string p in paths)
            {
                store.AddFile(p);
            }
            var vp = new SyncProgress<OperationProgress>();
            DicomStoreVerifyReport rep = store.VerifyAsync(true, false, vp).GetAwaiter().GetResult();
            CheckEq(rep.Checked, 4);
            CheckEq(vp.Seen.Count, 4);
            var rp = new SyncProgress<OperationProgress>();
            store.RebuildAsync(rp).GetAwaiter().GetResult();
            CheckEq(rp.Seen.Count, 4);
            CheckEq(store.GetStatistics().Instances, 4L);
            var dp = new SyncProgress<OperationProgress>();
            Dicomdir.WriteAsync(Path.Combine(dir, "media"), paths, null, dp).GetAwaiter().GetResult();
            CheckEq(dp.Seen.Count, 8);
            Check(File.Exists(Path.Combine(dir, "media", "DICOMDIR")));
        });
    }
}
