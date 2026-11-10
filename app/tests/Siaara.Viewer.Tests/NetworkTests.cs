// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Network;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Tests;

/// <summary>M2 networking. Every counterpart is our own toolkit in this process: a second store with its own
/// receiver (Storage, Query/Retrieve) or the toolkit's DICOMweb server. No outside tools.</summary>
internal static class NetworkTests
{
    private const string ViewerAe = "VIEWER";
    private const string ArchiveAe = "ARCHIVE";

    internal static int FreePort()
    {
        using var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        return ((IPEndPoint)l.LocalEndpoint).Port;
    }

    private static async Task<bool> UntilAsync(Func<bool> condition, int timeoutMs = 15000)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs)
        {
            if (condition())
            {
                return true;
            }
            await Task.Delay(25).ConfigureAwait(false);
        }
        return condition();
    }

    private static List<AuditEntry> Audit(TestEnv env, string name) => [.. AuditLog.ReadEntries(env.Paths.AuditFile).Where(e => e.Event == name)];

    // A receiver on 127.0.0.1 at a free port, allowing the given senders.
    internal static void StartReceiver(TestEnv env, string ae, IEnumerable<string> allowed, bool acceptAny = false, bool queryRetrieve = false)
    {
        var s = new ScpSetting { Enabled = true, AeTitle = ae, Port = FreePort(), BindAddress = "127.0.0.1", AcceptAnyCallingAe = acceptAny, ServeQueryRetrieve = queryRetrieve };
        foreach (string a in allowed)
        {
            s.AllowedCallingAes.Add(a);
        }
        env.Network.Scp.Apply(s);
    }

    internal static string AddNode(TestEnv env, string name, string calledAe, int port, string callingAe = ViewerAe, RetrieveMethod retrieve = RetrieveMethod.Get,
        bool commitment = false, int associationTimeoutSeconds = 10) =>
        env.Network.Nodes.Save(new DimseNodeSetting
        {
            Name = name,
            Host = "127.0.0.1",
            Port = port,
            CalledAe = calledAe,
            CallingAe = callingAe,
            Retrieve = retrieve,
            RequestCommitment = commitment,
            ConnectTimeoutSeconds = 5,
            AssociationTimeoutSeconds = associationTimeoutSeconds,
            DimseTimeoutSeconds = 20,
            CommitmentWaitSeconds = 5,
        });

    // An "archive": a store with a CT study (2 series) and a CR study, its receiver serving Query/Retrieve.
    private static async Task<(TestEnv Env, SamplePatient Ct, SamplePatient Cr)> ArchiveAsync(string name, int slices = 12)
    {
        var env = new TestEnv(name);
        try
        {
            SamplePatient ct = SampleData.Patient("REMOTE^CT", "RMT100") with { StudyDate = "20260301" };
            SamplePatient cr = SampleData.Patient("REMOTE^CR", "RMT200") with { StudyDate = "20260302" };
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(Path.Combine(dir, "ct1"), ct, slices, 32, seriesNumber: 1);
            SampleData.WriteCtSeries(Path.Combine(dir, "ct2"), ct, 4, 32, seriesNumber: 2);
            SampleData.WriteCrNoSpacing(Path.Combine(dir, "cr.dcm"), cr, 32);
            await env.ImportAsync(dir).ConfigureAwait(false);
            return (env, ct, cr);
        }
        catch
        {
            env.Dispose();
            throw;
        }
    }

    public static void Register()
    {
        T.Case("network: node settings round-trip, validation, and no secret in settings.json", () =>
        {
            using var env = new TestEnv("nodes");
            NodeRegistry reg = env.Network.Nodes;
            string id = reg.Save(new DimseNodeSetting { Name = " Main PACS ", Host = "pacs.example.org", Port = 11112, CalledAe = "PACS", CallingAe = "VIEWER", Retrieve = RetrieveMethod.Move, MaxPdu = 16384, AllowLossyTranscode = true, RequestCommitment = true });
            const string Token = "tok-SECRET-9f8e7d";
            const string TlsPass = "p12-SECRET-1a2b3c";
            string p12 = Path.Combine(env.Root, "id.p12");
            File.WriteAllBytes(p12, [1, 2, 3]);
            var web = new WebNodeSetting { Name = "Web archive", Address = "https://web.example.org/dicomweb/", Auth = WebAuthKind.Bearer };
            web.Tls.Pkcs12File = p12;
            string wid = reg.Save(web, Token, TlsPass);
            // A fresh read of the file gives the same nodes.
            UserPrefs again = SettingsStore.Load(env.Paths.SettingsFile);
            DimseNodeSetting d = again.Network.DimseNodes.Single();
            T.Eq(d.Id, id);
            T.Eq(d.Name, "Main PACS");
            T.Eq(d.Port, 11112);
            T.Eq(d.Retrieve, RetrieveMethod.Move);
            T.Eq(d.MaxPdu, 16384);
            T.Check(d.AllowLossyTranscode && d.RequestCommitment);
            WebNodeSetting w = again.Network.WebNodes.Single();
            T.Eq(w.Address, "https://web.example.org/dicomweb");
            T.Eq(w.Auth, WebAuthKind.Bearer);
            T.Check(!again.Network.Scp.Enabled);
            T.Eq(again.Network.Scp.Port, 11112);
            // Secrets: in the secret store, nowhere in settings.json, the audit log or the app log.
            string settingsText = TestFiles.ReadSharedText(env.Paths.SettingsFile);
            T.Check(!settingsText.Contains(Token, StringComparison.Ordinal) && !settingsText.Contains(TlsPass, StringComparison.Ordinal));
            T.Check(settingsText.Contains("\"retrieve\": \"Move\"", StringComparison.Ordinal));
            T.Check(reg.HasSecret(NodeRegistry.PasswordSecret(wid)) && reg.HasSecret(NodeRegistry.TlsSecret(wid)));
            string auditText = TestFiles.ReadSharedText(env.Paths.AuditFile);
            T.Check(!auditText.Contains(Token, StringComparison.Ordinal) && !auditText.Contains(TlsPass, StringComparison.Ordinal));
            T.Check(AppLog.RecentLines.All(l => !l.Contains(Token, StringComparison.Ordinal) && !l.Contains(TlsPass, StringComparison.Ordinal)));
            foreach (string f in Directory.EnumerateFiles(env.Paths.SecretsFolder))
            {
                T.Check(!TestFiles.ReadSharedText(f).Contains(Token, StringComparison.Ordinal));
                if (!OperatingSystem.IsWindows())
                {
                    T.Check(OwnerOnly.IsOwnerOnly(File.GetUnixFileMode(f)));
                }
            }
            T.Eq(Audit(env, "node.change").Count, 2);
            T.Eq(Audit(env, "node.change")[1].Data["secret"], "set");

            // Validation: nothing is saved for a bad node.
            void Bad(Action a, string part)
            {
                try
                {
                    a();
                    T.Check(false, "expected a validation error: " + part);
                }
                catch (NodeValidationException e)
                {
                    T.Check(e.Problems.Any(p => p.Contains(part, StringComparison.OrdinalIgnoreCase)), part);
                }
            }
            DimseNodeSetting Good() => new() { Name = "N2", Host = "h", Port = 104, CalledAe = "A", CallingAe = "B" };
            Bad(() => { DimseNodeSetting n = Good(); n.CalledAe = "SEVENTEEN_CHARS_X"; reg.Save(n); }, "AE title");
            Bad(() => { DimseNodeSetting n = Good(); n.CallingAe = ""; reg.Save(n); }, "Our AE title");
            Bad(() => { DimseNodeSetting n = Good(); n.CalledAe = "A\\B"; reg.Save(n); }, "AE title");
            Bad(() => { DimseNodeSetting n = Good(); n.Port = 0; reg.Save(n); }, "Port");
            Bad(() => { DimseNodeSetting n = Good(); n.Port = 65536; reg.Save(n); }, "Port");
            Bad(() => { DimseNodeSetting n = Good(); n.Host = "two words"; reg.Save(n); }, "Host");
            Bad(() => { DimseNodeSetting n = Good(); n.Name = "main pacs"; reg.Save(n); }, "already has this name");
            Bad(() => reg.Save(new WebNodeSetting { Name = "W2", Address = "ftp://x/y" }), "http");
            Bad(() => reg.Save(new WebNodeSetting { Name = "W2", Address = "https://user:pw@x/y" }), "user name or password");
            Bad(() => reg.Save(new WebNodeSetting { Name = "W2", Address = "http://far.example.org/dw", Auth = WebAuthKind.Bearer }, "t"), "only sent over https");
            Bad(() => reg.Save(new WebNodeSetting { Name = "W2", Address = "https://x/dw", Auth = WebAuthKind.Basic, UserName = "u" }), "password");
            Bad(() => reg.SaveScp(new ScpSetting { AeTitle = "", Port = 11112 }), "AE title");
            Bad(() => reg.SaveScp(new ScpSetting { AeTitle = "X", Port = 70000 }), "Port");
            T.Check(NodeRegistry.IsValidAe("SIXTEEN_CHARS_OK") && !NodeRegistry.IsValidAe(" LEAD") && !NodeRegistry.IsValidAe("TAB\tX"));
            T.Eq(reg.List().Count, 2);
            // http with credentials is fine on this computer only.
            reg.Save(new WebNodeSetting { Name = "Local web", Address = "http://127.0.0.1:8042/dicom-web", Auth = WebAuthKind.Basic, UserName = "u" }, "pw");
            // Updating keeps the stored secret when none is given; deleting removes node and secrets.
            w = reg.FindWeb(wid)!;
            w.Name = "Web archive 2";
            reg.Save(w);
            T.Check(reg.HasSecret(NodeRegistry.PasswordSecret(wid)));
            T.Check(reg.Delete(wid));
            T.Check(!reg.HasSecret(NodeRegistry.PasswordSecret(wid)) && !reg.HasSecret(NodeRegistry.TlsSecret(wid)));
            T.Check(reg.FindWeb(wid) is null && !reg.Delete(wid));
            T.Check(AuditLog.Verify(env.Paths.AuditFile, AuditKeys.KeyFile).IsIntact);
        });

        T.Case("network: secret store round-trip on this OS", () =>
        {
            string dir = Path.Combine(T.NewTempDir("secrets"), "s");
            ISecretStore s = SecretStores.ForCurrentOs(dir);
            T.Note("protection: " + s.Protection);
            const string Value = "pässwörd ☃ with spaces:and;punctuation";
            T.Check(s.Read("node1.password") is null);
            s.Write("node1.password", Value);
            s.Write("node1.tls", "other");
            T.Eq(s.Read("node1.password"), Value);
            T.Eq(SecretStores.ForCurrentOs(dir).Read("node1.password"), Value);
            s.Write("node1.password", "changed");
            T.Eq(s.Read("node1.password"), "changed");
            T.Eq(s.Read("node1.tls"), "other");
            s.Delete("node1.password");
            T.Check(s.Read("node1.password") is null);
            s.Delete("node1.password");
            s.Delete("node1.tls");
            foreach (string bad in (string[])["", "../x", "A", "a/b", ".hidden", new string('a', 81)])
            {
                try
                {
                    s.Write(bad, "v");
                    T.Check(false, "bad id accepted");
                }
                catch (ArgumentException)
                {
                }
            }
            // The encrypted-file form (Linux): a secret copied under another id, or changed, does not open.
            string dir2 = Path.Combine(T.NewTempDir("secrets2"), "s");
            ISecretStore f = SecretStores.EncryptedFiles(dir2);
            f.Write("a.password", Value);
            T.Eq(f.Read("a.password"), Value);
            T.Eq(File.ReadAllBytes(Path.Combine(dir2, "a.password.sec")).AsSpan().IndexOf("with spaces"u8), -1);
            File.Copy(Path.Combine(dir2, "a.password.sec"), Path.Combine(dir2, "b.password.sec"));
            try
            {
                f.Read("b.password");
                T.Check(false, "a moved secret opened");
            }
            catch (System.Security.Cryptography.CryptographicException)
            {
            }
            if (!OperatingSystem.IsWindows())
            {
                T.Check(OwnerOnly.IsOwnerOnly(File.GetUnixFileMode(Path.Combine(dir2, "secret.key"))));
                T.Check(OwnerOnly.IsOwnerOnly(File.GetUnixFileMode(dir2)));
            }
        });

        T.Case("network: receiver off by default; start, echo, store, activity row, allow-list, accept-any, stop and start again", async () =>
        {
            using var env = new TestEnv("scp");
            using var sender = new TestEnv("scp-sender");
            ScpService scp = env.Network.Scp;
            T.Check(!scp.IsRunning && !env.Network.Nodes.Current.Scp.Enabled);
            env.Network.StartConfigured();
            T.Check(!scp.IsRunning);
            // No sender listed and no accept-any tick: it does not start.
            try
            {
                StartReceiver(env, ViewerAe, []);
                T.Check(false, "started without any allowed sender");
            }
            catch (NetworkException e)
            {
                T.Check(e.Message.Contains("at least one sender", StringComparison.Ordinal));
            }
            T.Check(!scp.IsRunning);
            StartReceiver(env, ViewerAe, ["MODALITY"]);
            T.Require(scp.IsRunning && scp.Port > 0);
            T.Check(scp.Accepts("MODALITY") && !scp.Accepts("OTHER") && !scp.AcceptsAny);

            // The sender's view: two nodes for the same receiver, one with an AE title that is not allowed.
            string ok = AddNode(sender, "viewer", ViewerAe, scp.Port, "MODALITY");
            string stranger = AddNode(sender, "viewer as stranger", ViewerAe, scp.Port, "STRANGER");
            NodeTestResult echo = sender.Network.Nodes.Test(ok, CancellationToken.None);
            T.Check(echo.Ok, echo.Message);
            NodeTestResult refused = sender.Network.Nodes.Test(stranger, CancellationToken.None);
            T.Check(!refused.Ok);
            T.Note("unknown AE: " + refused.Message);
            T.Check(refused.Message.Contains("refused", StringComparison.Ordinal) || refused.Message.Contains("AE title", StringComparison.Ordinal));
            NodeTestResult dead = sender.Network.Nodes.Test(AddNode(sender, "nobody", "X", FreePort()), CancellationToken.None);
            T.Check(!dead.Ok && dead.Message.Contains("could not connect", StringComparison.Ordinal), dead.Message);

            SamplePatient p = SampleData.Patient("SEND^CT", "SND100");
            SampleData.WriteCtSeries(sender.Dir("in"), p, 10, 32);
            await sender.ImportAsync(Path.Combine(sender.Root, "in")).ConfigureAwait(false);
            string study = Gen.StudyOf(sender, "SND100");
            NetworkJob refusedSend = sender.Network.Transfers.Send(stranger, new SendRequest(study, null, "t"));
            await refusedSend.Completion.ConfigureAwait(false);
            T.Eq(refusedSend.State, NetworkJobState.Failed);
            T.Note("send as stranger: " + refusedSend.Error + " | " + string.Join(" | ", refusedSend.Notes.Take(2)));
            NetworkJob send = sender.Network.Transfers.Send(ok, new SendRequest(study, null, "t"));
            await send.Completion.ConfigureAwait(false);
            T.Eq(send.State, NetworkJobState.Completed);
            T.Eq(send.Completed, 10);
            T.Eq(send.Failed, 0);
            T.Check(await UntilAsync(() => env.Store.GetStatisticsAsync().GetAwaiter().GetResult().Instances == 10).ConfigureAwait(false));
            T.Eq((await env.Store.GetStudyAsync(study).ConfigureAwait(false))?.Images ?? 0, 10);
            // One activity row for the burst, counters live, and the row closes by itself.
            NetworkJob row = env.Network.Jobs.Snapshot().Single(j => j.Kind == NetworkJobKind.Receive);
            T.Eq(row.NodeName, "MODALITY");
            T.Eq(row.Completed, 10);
            T.Check(await UntilAsync(() => scp.Stats.Stored == 10 && scp.Stats.Rejected >= 2 && scp.Stats.Echoes >= 1, 5000).ConfigureAwait(false));
            T.Check(await UntilAsync(() => row.IsFinished, 12000).ConfigureAwait(false));
            T.Eq(row.State, NetworkJobState.Completed);
            AuditEntry rec = Audit(env, "scp.receive").Single();
            T.Eq(rec.Data["stored"], "10");
            T.Eq(rec.Data["ae"], "MODALITY");

            // Stop, then accept-any: the stranger gets in, and the choice is in the audit log before the start.
            scp.Stop();
            T.Check(!scp.IsRunning && scp.Port == 0);
            T.Check(!sender.Network.Nodes.Test(ok, CancellationToken.None).Ok);
            StartReceiver(env, ViewerAe, [], acceptAny: true);
            T.Check(scp.IsRunning && scp.AcceptsAny);
            foreach (string n in (string[])[ok, stranger])
            {
                DimseNodeSetting d = sender.Network.Nodes.FindDimse(n)!;
                d.Port = scp.Port;
                sender.Network.Nodes.Save(d);
            }
            T.Check(sender.Network.Nodes.Test(stranger, CancellationToken.None).Ok);
            scp.Stop();
            List<AuditEntry> all = [.. AuditLog.ReadEntries(env.Paths.AuditFile)];
            int any = all.FindIndex(e => e.Event == "scp.accept-any" && e.Outcome == AuditOutcome.Intent && e.Data["acceptAny"] == "on");
            int started = all.FindLastIndex(e => e.Event == "scp.start" && e.Outcome == AuditOutcome.Success && e.Data["acceptAny"] == "yes");
            T.Check(any >= 0 && started > any);
            T.Eq(all.Count(e => e.Event == "scp.start" && e.Outcome == AuditOutcome.Intent), 2);
            T.Eq(all.Count(e => e.Event == "scp.stop"), 2);
            T.Check(all.Any(e => e.Event == "scp.rejected"));
            // A port that is taken: plain words, and the receiver stays off.
            using var taken = new TcpListener(IPAddress.Loopback, 0);
            taken.Start();
            try
            {
                var s = new ScpSetting { Enabled = true, AeTitle = ViewerAe, Port = ((IPEndPoint)taken.LocalEndpoint).Port, BindAddress = "127.0.0.1" };
                s.AllowedCallingAes.Add("MODALITY");
                scp.Apply(s);
                T.Check(false, "started on a port in use");
            }
            catch (NetworkException e)
            {
                T.Note("port in use: " + e.Message);
                T.Check(e.Message.Contains("port is in use", StringComparison.Ordinal));
            }
            T.Check(!scp.IsRunning && scp.LastError is not null);
        });
        T.Case("network: search, C-GET and C-MOVE against our own receiver; Local column; cap; cancel; audit without query values", async () =>
        {
            (TestEnv archive, SamplePatient ct, SamplePatient cr) = await ArchiveAsync("archive").ConfigureAwait(false);
            using TestEnv _ = archive;
            using var env = new TestEnv("viewer");
            StartReceiver(archive, ArchiveAe, [ViewerAe], queryRetrieve: true);
            string get = AddNode(env, "Archive (get)", ArchiveAe, archive.Network.Scp.Port);
            string move = AddNode(env, "Archive (move)", ArchiveAe, archive.Network.Scp.Port, retrieve: RetrieveMethod.Move);

            // Study search: everything, by name part, by ID prefix, by date, by modality.
            RemoteSearchResult all = await env.Network.Search.SearchStudiesAsync(get, new StudyQuery(), CancellationToken.None).ConfigureAwait(false);
            T.Eq(all.Rows.Count, 2);
            T.Check(!all.Truncated);
            T.Eq(all.Rows[0].Study.PatientId, "RMT200"); // newest first
            RemoteStudyRow ctRow = all.Rows.Single(r => r.Study.PatientId == "RMT100");
            T.Eq(ctRow.Study.PatientName, ct.PatientName); // exactly as stored on the other side
            T.Eq(ctRow.Study.StudyInstanceUid, ct.StudyUid);
            T.Eq(ctRow.RemoteImages, 16);
            T.Eq(ctRow.Local, LocalPresence.None);
            T.Eq((await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { Text = "MOTE^C" }, CancellationToken.None).ConfigureAwait(false)).Rows.Count, 2);
            T.Eq((await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { Text = "RMT1" }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT100");
            T.Eq((await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { Text = "A" + "RMT2" }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT200");
            T.Eq((await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { From = new DateOnly(2026, 3, 2), To = new DateOnly(2026, 3, 2) }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT200");
            T.Eq((await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { Modalities = ["CR", "DX"] }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT200");
            T.Eq((await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { Text = "NOBODY" }, CancellationToken.None).ConfigureAwait(false)).Rows.Count, 0);
            // The cap: one study comes back and the result says "narrow the search".
            env.Settings.Update(p => p.Network.SearchCap = 1);
            RemoteSearchResult capped = await env.Network.Search.SearchStudiesAsync(get, new StudyQuery(), CancellationToken.None).ConfigureAwait(false);
            T.Check(capped.Rows.Count == 1 && capped.Truncated && capped.Cap == 1);
            env.Settings.Update(p => p.Network.SearchCap = 200);
            // A cancelled search ends as cancelled, not as an error.
            using (var cts = new CancellationTokenSource())
            {
                await cts.CancelAsync().ConfigureAwait(false);
                try
                {
                    await env.Network.Search.SearchStudiesAsync(get, new StudyQuery { Text = "X" }, cts.Token).ConfigureAwait(false);
                    T.Check(false, "a cancelled search returned");
                }
                catch (OperationCanceledException)
                {
                }
            }

            // Series on expand.
            IReadOnlyList<RemoteSeriesRow> series = await env.Network.Search.SearchSeriesAsync(get, ct.StudyUid, CancellationToken.None).ConfigureAwait(false);
            T.Eq(series.Count, 2);
            T.Eq(series[0].SeriesNumber, 1);
            T.Eq(series[0].RemoteImages, 12);
            T.Eq(series[1].RemoteImages, 4);
            T.Eq(series[0].Local, LocalPresence.None);

            // C-GET of one series: Local becomes partial for the study, complete for the series.
            NetworkJob j1 = env.Network.Transfers.Retrieve(get, ct.StudyUid, series[1].SeriesInstanceUid, "series 2", series[1].RemoteImages);
            await j1.Completion.ConfigureAwait(false);
            T.Eq(j1.State, NetworkJobState.Completed);
            T.Eq(j1.Completed, 4);
            T.Eq(j1.Fraction, 1.0);
            await env.Network.Search.RefreshLocalAsync(all.Rows, CancellationToken.None).ConfigureAwait(false);
            T.Eq(ctRow.Local, LocalPresence.Partial);
            series = await env.Network.Search.SearchSeriesAsync(get, ct.StudyUid, CancellationToken.None).ConfigureAwait(false);
            T.Eq(series[1].Local, LocalPresence.Complete);
            T.Eq(series[0].Local, LocalPresence.None);
            // C-GET of the study: complete; what arrived is what the archive holds, byte for byte.
            NetworkJob j2 = env.Network.Transfers.Retrieve(get, ct.StudyUid, null, "study", ctRow.RemoteImages);
            await j2.Completion.ConfigureAwait(false);
            T.Eq(j2.State, NetworkJobState.Completed);
            T.Eq(j2.Completed + j2.Failed, 16);
            await env.Network.Search.RefreshLocalAsync(all.Rows, CancellationToken.None).ConfigureAwait(false);
            T.Eq(ctRow.Local, LocalPresence.Complete);
            T.Eq(ctRow.LocalImages, 16);
            StudyRow? localCt = await env.Store.GetStudyAsync(ct.StudyUid).ConfigureAwait(false);
            T.Eq(localCt?.PatientName, ct.PatientName);
            IReadOnlyList<SeriesRow> remoteSeries = await archive.Store.GetSeriesAsync(ct.StudyUid).ConfigureAwait(false);
            foreach (SeriesRow sr in remoteSeries)
            {
                var theirs = (await archive.Store.GetInstancesAsync(sr.SeriesInstanceUid).ConfigureAwait(false)).ToDictionary(i => i.SopInstanceUid, i => i.FilePath);
                foreach (InstanceRow mine in await env.Store.GetInstancesAsync(sr.SeriesInstanceUid).ConfigureAwait(false))
                {
                    using TkFile a = TkFile.Open(mine.FilePath, headersOnly: false);
                    using TkFile b = TkFile.Open(theirs[mine.SopInstanceUid], headersOnly: false);
                    T.Check(a.GetString(Core.Text.Tags.PatientName) == b.GetString(Core.Text.Tags.PatientName));
                    T.Check(a.DecodeModalityFloat32(0).AsSpan().SequenceEqual(b.DecodeModalityFloat32(0)));
                }
            }

            // C-MOVE: a clear message while our receiver is off or would refuse the archive.
            try
            {
                env.Network.Transfers.Retrieve(move, cr.StudyUid, null, "cr");
                T.Check(false, "C-MOVE started without a receiver");
            }
            catch (NetworkException e)
            {
                T.Check(e.Message.Contains("receiver is off", StringComparison.Ordinal) && e.Message.Contains("C-GET", StringComparison.Ordinal));
            }
            StartReceiver(env, ViewerAe, ["SOMEONE"]);
            try
            {
                env.Network.Transfers.Retrieve(move, cr.StudyUid, null, "cr");
                T.Check(false, "C-MOVE started although the receiver would refuse the archive");
            }
            catch (NetworkException e)
            {
                T.Check(e.Message.Contains("allowed senders", StringComparison.Ordinal));
            }
            // The archive does not know our receiver yet: its answer is turned into plain words.
            StartReceiver(env, ViewerAe, [ArchiveAe]);
            NetworkJob unknown = env.Network.Transfers.Retrieve(move, cr.StudyUid, null, "cr");
            await unknown.Completion.ConfigureAwait(false);
            T.Eq(unknown.State, NetworkJobState.Failed);
            T.Note("unknown move destination: " + unknown.Error);
            T.Check(unknown.Error!.Contains("destination", StringComparison.Ordinal));
            T.Check(unknown.CanRetry);
            // Register our receiver at the archive (its AE table comes from its nodes) and retry.
            AddNode(archive, "the viewer", ViewerAe, env.Network.Scp.Port, ArchiveAe);
            archive.Network.Scp.Stop();
            archive.Network.Scp.Start();
            foreach (string n in (string[])[get, move])
            {
                DimseNodeSetting d = env.Network.Nodes.FindDimse(n)!;
                d.Port = archive.Network.Scp.Port;
                env.Network.Nodes.Save(d);
            }
            NetworkJob j3 = unknown.Retry()!;
            await j3.Completion.ConfigureAwait(false);
            T.Eq(j3.State, NetworkJobState.Completed);
            T.Eq(j3.Completed, 1);
            T.Check(await UntilAsync(() => env.Store.GetStudyAsync(cr.StudyUid).GetAwaiter().GetResult()?.Images == 1).ConfigureAwait(false));
            T.Check(env.Network.Jobs.Snapshot().Any(j => j.Kind == NetworkJobKind.Receive && j.NodeName == ArchiveAe));

            // Audit: searches with node, level and counts; retrieves with method and outcome; no query value anywhere.
            List<AuditEntry> searches = Audit(env, "search");
            T.Check(searches.Count >= 9);
            T.Check(searches.All(e => e.Data.Keys.Order().SequenceEqual((string[])["kind", "level", "matches", "node", "truncated"])));
            T.Check(searches.Any(e => e.Outcome == AuditOutcome.Cancelled) && searches.Any(e => e.Data["level"] == "series"));
            List<AuditEntry> retrieves = Audit(env, "retrieve");
            T.Eq(retrieves.Count, 4);
            T.Check(retrieves.Select(e => e.Data["method"]).SequenceEqual((string[])["c-get", "c-get", "c-move", "c-move"]));
            T.Eq(retrieves[2].Outcome, AuditOutcome.Failure);
            T.Eq(retrieves[3].Outcome, AuditOutcome.Success);
            string auditText = TestFiles.ReadSharedText(env.Paths.AuditFile) + TestFiles.ReadSharedText(archive.Paths.AuditFile);
            string logText = string.Join("\n", AppLog.RecentLines);
            foreach (string phi in (string[])["MOTE^C", "RMT1", "ARMT2", "NOBODY", "REMOTE", "RMT100", "RMT200"])
            {
                T.Check(!auditText.Contains(phi, StringComparison.Ordinal), "audit: " + phi);
                T.Check(!logText.Contains(phi, StringComparison.Ordinal), "log: " + phi);
            }
            T.Check(AuditLog.Verify(env.Paths.AuditFile, AuditKeys.KeyFile).IsIntact);
        });
        T.Case("network: send over one association; truncated and damaged files fail; allow-incomplete is audited; cancel mid-way; commitment", async () =>
        {
            using var archive = new TestEnv("send-archive");
            using var env = new TestEnv("send-viewer");
            // The archive: a receiver that also answers Storage Commitment (a facade option the viewer itself never sets).
            int port = FreePort();
            using TkScp archiveScp = TkScp.Start(new TkScpOptions { AeTitle = ArchiveAe, Port = port, BindAddress = "127.0.0.1", AllowedCallingAes = [ViewerAe], ServeCommitment = true },
                archive.Store.Store);
            string node = AddNode(env, "Archive", ArchiveAe, port, commitment: true);
            string plain = AddNode(env, "Archive, no confirmation asked", ArchiveAe, port);

            SamplePatient p = SampleData.Patient("SEND^BAD", "SNB100");
            IReadOnlyList<string> files = SampleData.WriteCtSeries(env.Dir("ref"), p, 8, 32);
            await env.ImportAsync(Path.Combine(env.Root, "ref"), inPlace: true).ConfigureAwait(false);
            // After indexing in place, one file loses its second half and one is overwritten with noise.
            byte[] whole = File.ReadAllBytes(files[2]);
            File.WriteAllBytes(files[2], whole.AsSpan(0, whole.Length / 2).ToArray());
            File.WriteAllBytes(files[5], System.Security.Cryptography.RandomNumberGenerator.GetBytes(4096));
            NetworkJob j = env.Network.Transfers.Send(node, new SendRequest(p.StudyUid, null, "SEND^BAD"));
            await j.Completion.ConfigureAwait(false);
            T.Eq(j.State, NetworkJobState.CompletedWithErrors);
            T.Eq(j.Completed, 6);
            T.Eq(j.Failed, 2);
            T.Eq(j.Total, 8);
            foreach (string n in j.Notes)
            {
                T.Note(n);
            }
            T.Check(j.Notes.Any(n => n.StartsWith("Not sent:", StringComparison.Ordinal) && n.Contains("not read completely", StringComparison.Ordinal)));
            T.Eq(j.Notes.Count(n => n.StartsWith("Not sent:", StringComparison.Ordinal)), 2);
            T.Check(j.Notes.Any(n => n.Contains("confirmed storage of 6", StringComparison.Ordinal)));
            T.Check(j.Notes.All(n => !n.Contains("delete", StringComparison.OrdinalIgnoreCase) && !n.Contains("safe", StringComparison.OrdinalIgnoreCase)));
            T.Check(await UntilAsync(() => archive.Store.GetStatisticsAsync().GetAwaiter().GetResult().Instances == 6).ConfigureAwait(false));
            T.Eq(Audit(env, "send").Count(e => e.Outcome == AuditOutcome.Intent), 0);
            AuditEntry first = Audit(env, "send").Single();
            T.Eq(first.Outcome, AuditOutcome.Failure);
            T.Eq(first.Data["sent"], "6");
            T.Eq(first.Data["failed"], "2");
            T.Eq(first.Data["commitment"], "confirmed");
            T.Eq(first.Data["allowIncomplete"], "no");

            // The user allows incomplete files for this one send: the intent is audited first, the truncated file goes
            // (the noise still cannot be read).
            NetworkJob j2 = env.Network.Transfers.Send(plain, new SendRequest(p.StudyUid, null, "SEND^BAD") { AllowIncomplete = true });
            await j2.Completion.ConfigureAwait(false);
            T.Eq(j2.Completed, 7);
            T.Eq(j2.Failed, 1);
            T.Eq(j2.State, NetworkJobState.CompletedWithErrors);
            List<AuditEntry> sends = Audit(env, "send");
            T.Eq(sends.Count, 3);
            T.Check(sends[1].Outcome == AuditOutcome.Intent && sends[1].Data["allowIncomplete"] == "yes");
            T.Eq(sends[2].Data["commitment"], "not requested");

            // "k not committed": the archive is asked about the six it holds and two it never got.
            IReadOnlyList<SeriesRow> series = await archive.Store.GetSeriesAsync(p.StudyUid).ConfigureAwait(false);
            IReadOnlyList<InstanceRow> held = await archive.Store.GetInstancesAsync(series[0].SeriesInstanceUid).ConfigureAwait(false);
            var ask = held.Take(6).Select(i => (SampleData.CtImageStorage, i.SopInstanceUid)).ToList();
            ask.Add((SampleData.CtImageStorage, SampleData.NewUid()));
            ask.Add((SampleData.CtImageStorage, SampleData.NewUid()));
            NetworkJob c = env.Network.Transfers.CommitOnly(node, ask);
            await c.Completion.ConfigureAwait(false);
            T.Eq(c.State, NetworkJobState.CompletedWithErrors);
            T.Check(c.Notes.Any(n => n.StartsWith("2 of 8 image(s) not committed", StringComparison.Ordinal)), string.Join(" | ", c.Notes));
            T.Eq(c.Notes.Count(n => n.StartsWith("Not committed:", StringComparison.Ordinal)), 2);

            // A node that does not answer confirmation requests: "did not confirm", and the row is not a clean success.
            using var silent = new TestEnv("send-silent");
            StartReceiver(silent, "QUIET", [ViewerAe]);
            string quiet = AddNode(env, "Quiet", "QUIET", silent.Network.Scp.Port, commitment: true);
            NetworkJob j3 = env.Network.Transfers.Send(quiet, new SendRequest(p.StudyUid, files.Count > 0 ? null : "", "x"));
            await j3.Completion.ConfigureAwait(false);
            T.Eq(j3.State, NetworkJobState.CompletedWithErrors);
            T.Eq(j3.Completed, 6);
            string? said = j3.Notes.FirstOrDefault(n => n.StartsWith("The PACS did not confirm storage", StringComparison.Ordinal));
            T.Note("no confirmation: " + said);
            T.Check(said is not null);
            T.Check(j3.Notes.All(n => !n.Contains("safe", StringComparison.OrdinalIgnoreCase) && !n.Contains("delete", StringComparison.OrdinalIgnoreCase)));

            // Cancel in the middle of a long send: the row is "Cancelled" within a second and the rest is not sent.
            SamplePatient big = SampleData.Patient("SEND^BIG", "SNB200");
            SampleData.WriteCtSeries(env.Dir("big"), big, 400, 128);
            await env.ImportAsync(Path.Combine(env.Root, "big")).ConfigureAwait(false);
            NetworkJob j4 = env.Network.Transfers.Send(plain, new SendRequest(big.StudyUid, null, "big"));
            T.Require(await UntilAsync(() => j4.Completed >= 20 || j4.IsFinished).ConfigureAwait(false));
            T.Check(!j4.IsFinished, "the send finished before it could be cancelled");
            var sw = Stopwatch.StartNew();
            j4.Cancel();
            await j4.Completion.ConfigureAwait(false);
            T.Check(sw.ElapsedMilliseconds < 1000, "cancel took " + sw.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms");
            T.Note("cancelled after " + j4.Completed.ToString(CultureInfo.InvariantCulture) + " of 400 in " + sw.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms");
            T.Eq(j4.State, NetworkJobState.Cancelled);
            T.Check(j4.Completed < 400 && j4.CanRetry);
            T.Check(await UntilAsync(() => Audit(env, "send").Any(e => e.Outcome == AuditOutcome.Cancelled)).ConfigureAwait(false));
            await Task.Delay(300).ConfigureAwait(false);
            long there = (await archive.Store.GetStudyAsync(big.StudyUid).ConfigureAwait(false))?.Images ?? 0;
            T.Check(there < 400, "everything arrived although the send was cancelled");
        });

        T.Case("network: DICOMweb search, streamed retrieve and STOW against the toolkit's server; bearer token; plain-word errors", async () =>
        {
            (TestEnv archive, SamplePatient ct, SamplePatient cr) = await ArchiveAsync("web-archive", slices: 40).ConfigureAwait(false);
            using TestEnv _ = archive;
            using var env = new TestEnv("web-viewer");
            const string Token = "web-token-51d3";
            var seen = new System.Collections.Concurrent.ConcurrentQueue<string>();
            using TkWebServer server = TkWebServer.StartLoopback(archive.Store.Store, header =>
            {
                seen.Enqueue(header);
                return header == "Bearer " + Token;
            });
            string web = env.Network.Nodes.Save(new WebNodeSetting { Name = "Web", Address = server.BaseAddress, Auth = WebAuthKind.Bearer, TimeoutSeconds = 20 }, Token);
            string wrong = env.Network.Nodes.Save(new WebNodeSetting { Name = "Web, wrong token", Address = server.BaseAddress, Auth = WebAuthKind.Bearer, TimeoutSeconds = 20 }, "nope");
            string dead = env.Network.Nodes.Save(new WebNodeSetting { Name = "Web, nobody there", Address = "http://127.0.0.1:" + FreePort().ToString(CultureInfo.InvariantCulture) + "/dicomweb", TimeoutSeconds = 5 });
            NodeTestResult t = env.Network.Nodes.Test(web, CancellationToken.None);
            T.Check(t.Ok, t.Message);
            NodeTestResult tw = env.Network.Nodes.Test(wrong, CancellationToken.None);
            T.Note("wrong token: " + tw.Message);
            T.Check(!tw.Ok);
            NodeTestResult td = env.Network.Nodes.Test(dead, CancellationToken.None);
            T.Note("no server: " + td.Message);
            T.Check(!td.Ok && td.Message.Contains("could not connect", StringComparison.Ordinal));
            T.Check(seen.Contains("Bearer " + Token) && seen.Contains("Bearer nope"));

            RemoteSearchResult all = await env.Network.Search.SearchStudiesAsync(web, new StudyQuery(), CancellationToken.None).ConfigureAwait(false);
            T.Eq(all.Rows.Count, 2);
            RemoteStudyRow ctRow = all.Rows.Single(r => r.Study.PatientId == "RMT100");
            T.Eq(ctRow.Study.PatientName, ct.PatientName);
            T.Eq(ctRow.RemoteImages, 44);
            T.Eq(ctRow.Study.StudyDate, "20260301");
            T.Eq((await env.Network.Search.SearchStudiesAsync(web, new StudyQuery { Text = "MOTE^CR" }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT200");
            T.Eq((await env.Network.Search.SearchStudiesAsync(web, new StudyQuery { Text = "RMT1" }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT100");
            T.Eq((await env.Network.Search.SearchStudiesAsync(web, new StudyQuery { Modalities = ["CT"], From = new DateOnly(2026, 3, 1), To = new DateOnly(2026, 3, 1) }, CancellationToken.None).ConfigureAwait(false)).Rows.Single().Study.PatientId, "RMT100");
            env.Settings.Update(p => p.Network.SearchCap = 1);
            T.Check((await env.Network.Search.SearchStudiesAsync(web, new StudyQuery(), CancellationToken.None).ConfigureAwait(false)) is { Truncated: true, Rows.Count: 1 });
            env.Settings.Update(p => p.Network.SearchCap = 200);
            try
            {
                await env.Network.Search.SearchStudiesAsync(wrong, new StudyQuery(), CancellationToken.None).ConfigureAwait(false);
                T.Check(false, "search with a wrong token worked");
            }
            catch (NetworkException e)
            {
                T.Check(e.Message.StartsWith("The search failed:", StringComparison.Ordinal));
            }
            IReadOnlyList<RemoteSeriesRow> series = await env.Network.Search.SearchSeriesAsync(web, ct.StudyUid, CancellationToken.None).ConfigureAwait(false);
            T.Eq(series.Count, 2);
            T.Eq(series[0].RemoteImages, 40);
            T.Eq(series[1].SeriesNumber, 2);

            // WADO-RS: one series, then the study, streamed into the store with byte progress.
            NetworkJob j1 = env.Network.Transfers.Retrieve(web, ct.StudyUid, series[1].SeriesInstanceUid, "s2", 4);
            await j1.Completion.ConfigureAwait(false);
            T.Eq(j1.State, NetworkJobState.Completed);
            T.Eq(j1.Completed, 4);
            T.Check(j1.Bytes > 4 * 1024);
            NetworkJob j2 = env.Network.Transfers.Retrieve(web, ct.StudyUid, null, "study", 44);
            await j2.Completion.ConfigureAwait(false);
            T.Eq(j2.State, NetworkJobState.Completed);
            T.Eq(j2.Completed, 44);
            await env.Network.Search.RefreshLocalAsync(all.Rows, CancellationToken.None).ConfigureAwait(false);
            T.Eq(ctRow.Local, LocalPresence.Complete);
            T.Eq((await env.Store.GetStudyAsync(ct.StudyUid).ConfigureAwait(false))?.PatientName, ct.PatientName);
            NetworkJob jw = env.Network.Transfers.Retrieve(wrong, cr.StudyUid, null, "cr");
            await jw.Completion.ConfigureAwait(false);
            T.Eq(jw.State, NetworkJobState.Failed);
            T.Note("retrieve with a wrong token: " + jw.Error);
            T.Eq(Audit(env, "retrieve").Count(e => e.Data["method"] == "wado-rs"), 3);

            // STOW-RS of a local study; a truncated file is not sent.
            SamplePatient mine = SampleData.Patient("LOCAL^UP", "LCL300");
            IReadOnlyList<string> files = SampleData.WriteCtSeries(env.Dir("up"), mine, 5, 32);
            await env.ImportAsync(Path.Combine(env.Root, "up"), inPlace: true).ConfigureAwait(false);
            byte[] whole = File.ReadAllBytes(files[1]);
            File.WriteAllBytes(files[1], whole.AsSpan(0, whole.Length / 2).ToArray());
            NetworkJob up = env.Network.Transfers.Send(web, new SendRequest(mine.StudyUid, null, "up"));
            await up.Completion.ConfigureAwait(false);
            T.Eq(up.State, NetworkJobState.CompletedWithErrors);
            T.Eq(up.Completed, 4);
            T.Eq(up.Failed, 1);
            T.Check(up.Notes.Single(n => n.StartsWith("Not sent:", StringComparison.Ordinal)).Contains("not read completely", StringComparison.Ordinal));
            T.Eq((await archive.Store.GetStudyAsync(mine.StudyUid).ConfigureAwait(false))?.Images ?? 0, 4);
            T.Eq(Audit(env, "send").Single().Data["method"], "stow-rs");
            // The token is in the secret store only.
            string everything = TestFiles.ReadSharedText(env.Paths.SettingsFile) + TestFiles.ReadSharedText(env.Paths.AuditFile) + string.Join("\n", AppLog.RecentLines);
            T.Check(!everything.Contains(Token, StringComparison.Ordinal));
        });

        T.Case("network: at most four transfers at once, cancel within a second, updates at most ten a second, last 200 rows kept", async () =>
        {
            using var archive = new TestEnv("jobs-archive");
            using var env = new TestEnv("jobs-viewer");
            var studies = new List<string>();
            for (int i = 0; i < 10; i++)
            {
                SamplePatient p = SampleData.Patient("JOB^" + i.ToString(CultureInfo.InvariantCulture), "JOB" + i.ToString(CultureInfo.InvariantCulture));
                SampleData.WriteCtSeries(Path.Combine(archive.Dir("in"), "s" + i.ToString(CultureInfo.InvariantCulture)), p, 60, 128);
                studies.Add(p.StudyUid);
            }
            await archive.ImportAsync(Path.Combine(archive.Root, "in")).ConfigureAwait(false);
            StartReceiver(archive, ArchiveAe, [ViewerAe], queryRetrieve: true);
            string node = AddNode(env, "Archive", ArchiveAe, archive.Network.Scp.Port);
            NetworkJobs jobs = env.Network.Jobs;
            long before = jobs.Notifications;
            var sw = Stopwatch.StartNew();
            NetworkJob[] started = [.. studies.Select(s => env.Network.Transfers.Retrieve(node, s, null, "job", 60))];
            T.Check(started.Count(j => j.State == NetworkJobState.Queued) >= 6 || jobs.PeakRunning <= NetworkJobs.MaxTransfers);
            await Task.WhenAll(started.Select(j => j.Completion)).ConfigureAwait(false);
            double seconds = sw.Elapsed.TotalSeconds;
            T.Check(started.All(j => j.State == NetworkJobState.Completed && j.Completed == 60));
            T.Note("peak transfers at once: " + jobs.PeakRunning.ToString(CultureInfo.InvariantCulture));
            T.Check(jobs.PeakRunning <= NetworkJobs.MaxTransfers && jobs.PeakRunning >= 2);
            long notes = jobs.Notifications - before;
            T.Note(string.Create(CultureInfo.InvariantCulture, $"{notes} updates for 600 images in {seconds:F2} s"));
            T.Check(notes <= (seconds * 10) + 2);
            T.Check(notes >= 1);
            T.Eq((await env.Store.GetStatisticsAsync().ConfigureAwait(false)).Instances, 600L);

            // A node that accepts the connection and then says nothing: the job hangs in the handshake. The toolkit does
            // not interrupt that wait, so the row turns "Cancelled" by the one-second rule and the abandoned connection
            // keeps its transfer slot until the handshake times out (never more than four connections).
            using var mute = new TcpListener(IPAddress.Loopback, 0);
            mute.Start();
            string stuck = AddNode(env, "Mute", "MUTE", ((IPEndPoint)mute.LocalEndpoint).Port, associationTimeoutSeconds: 1);
            NetworkJob hang = env.Network.Transfers.Retrieve(stuck, studies[0], null, "hang");
            T.Require(await UntilAsync(() => hang.State == NetworkJobState.Running).ConfigureAwait(false));
            await Task.Delay(300).ConfigureAwait(false);
            sw.Restart();
            hang.Cancel();
            await hang.Completion.ConfigureAwait(false);
            T.Check(sw.ElapsedMilliseconds < 1000, "cancel took " + sw.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms");
            T.Eq(hang.State, NetworkJobState.Cancelled);
            // Queued jobs cancel at once.
            NetworkJob[] many = [.. Enumerable.Range(0, 8).Select(_ => env.Network.Transfers.Retrieve(stuck, studies[0], null, "queued"))];
            T.Require(await UntilAsync(() => many.Count(j => j.State == NetworkJobState.Running) == 4).ConfigureAwait(false));
            T.Eq(many.Count(j => j.State == NetworkJobState.Queued), 4);
            NetworkJob queued = many.First(j => j.State == NetworkJobState.Queued);
            sw.Restart();
            queued.Cancel();
            await queued.Completion.ConfigureAwait(false);
            T.Check(sw.ElapsedMilliseconds < 1000);
            foreach (NetworkJob j in many)
            {
                j.Cancel();
            }
            await Task.WhenAll(many.Select(j => j.Completion)).ConfigureAwait(false);
            T.Check(many.All(j => j.State == NetworkJobState.Cancelled));

            // 230 quick failures (nobody listens): the list keeps the newest 200.
            string nobody = AddNode(env, "Nobody", "NOBODY", FreePort());
            NetworkJob[] fails = [.. Enumerable.Range(0, 230).Select(_ => env.Network.Transfers.Retrieve(nobody, studies[0], null, "fail"))];
            await Task.WhenAll(fails.Select(j => j.Completion)).ConfigureAwait(false);
            T.Check(fails.All(j => j.State == NetworkJobState.Failed && j.Error!.Contains("could not connect", StringComparison.Ordinal)), fails[0].Error ?? "");
            IReadOnlyList<NetworkJob> kept = jobs.Snapshot();
            T.Eq(kept.Count, NetworkJobs.MaxKept);
            T.Check(ReferenceEquals(kept[0], fails[^1]));
            T.Check(jobs.PeakRunning <= NetworkJobs.MaxTransfers);
        });

        T.Case("network: an open viewer grows while images arrive and keeps the image it shows", async () =>
        {
            (TestEnv archive, SamplePatient ct, SamplePatient _) = await ArchiveAsync("grow-archive", slices: 30).ConfigureAwait(false);
            using TestEnv __ = archive;
            using var env = new TestEnv("grow-viewer");
            StartReceiver(archive, ArchiveAe, [ViewerAe], queryRetrieve: true);
            string node = AddNode(env, "Archive", ArchiveAe, archive.Network.Scp.Port);
            IReadOnlyList<RemoteSeriesRow> series = await env.Network.Search.SearchSeriesAsync(node, ct.StudyUid, CancellationToken.None).ConfigureAwait(false);
            // The small series first; the viewer opens on it.
            await env.Network.Transfers.Retrieve(node, ct.StudyUid, series[1].SeriesInstanceUid, "s2", 4).Completion.ConfigureAwait(false);
            (Core.Viewing.ViewerSession session, FakeSink sink) = await env.OpenAsync(ct.StudyUid).ConfigureAwait(false);
            using Core.Viewing.ViewerSession ___ = session;
            await sink.SettleAsync().ConfigureAwait(false);
            T.Eq(session.Series.Count, 1);
            T.Eq(session.Series[0].Images.Count, 4);
            // Scroll to the third image and remember which one it is.
            session.OnInput(new Core.Viewing.InputEvent(0, 2, Core.Viewing.InputKind.Wheel, 10, 10, 0, 1, 0, Core.Viewing.InputModifiers.None, 0, 0));
            session.OnInput(new Core.Viewing.InputEvent(0, 3, Core.Viewing.InputKind.Wheel, 10, 10, 0, 1, 0, Core.Viewing.InputModifiers.None, 0, 0));
            await sink.SettleAsync().ConfigureAwait(false);
            Core.Viewing.Viewport vp = session.GetViewport(0);
            Core.Imaging.FrameKey shown = vp.CurrentImage!.Key;
            T.Check(vp.ImageIndex > 0);
            int uiEvents = 0;
            session.UiChanged += (_, _) => Interlocked.Increment(ref uiEvents);
            // The other series arrives while the viewer is open: it appears as a second series.
            NetworkJob big = env.Network.Transfers.Retrieve(node, ct.StudyUid, series[0].SeriesInstanceUid, "s1", 30);
            await big.Completion.ConfigureAwait(false);
            T.Eq(big.State, NetworkJobState.Completed);
            T.Check(await UntilAsync(() => session.Series.Count == 2 && session.Series.Sum(s => s.Images.Count) == 34).ConfigureAwait(false));
            T.Eq(session.ImagesArrived, 30L);
            T.Check(uiEvents >= 1);
            T.Eq(vp.CurrentImage!.Key, shown);
            T.Check(session.LoadError is null);

            // A series that grows under the viewport: slices arrive in a shuffled order, the stack is re-sorted each
            // time and the viewport stays on the same image.
            SamplePatient live = SampleData.Patient("LIVE^GROW", "LIVE1");
            IReadOnlyList<string> files = SampleData.WriteCtSeries(env.Dir("live"), live, 24, 32);
            foreach (int i in (int[])[10, 11, 12])
            {
                env.Store.Store.AddFile(files[i], indexInPlace: false);
            }
            (Core.Viewing.ViewerSession s2, FakeSink sink2) = await env.OpenAsync(live.StudyUid).ConfigureAwait(false);
            using Core.Viewing.ViewerSession ____ = s2;
            await sink2.SettleAsync().ConfigureAwait(false);
            Core.Viewing.Viewport v2 = s2.GetViewport(0);
            T.Eq(v2.Series!.Images.Count, 3);
            s2.OnInput(new Core.Viewing.InputEvent(0, 2, Core.Viewing.InputKind.Wheel, 10, 10, 0, 1, 0, Core.Viewing.InputModifiers.None, 0, 0));
            await sink2.SettleAsync().ConfigureAwait(false);
            Core.Imaging.FrameKey middle = v2.CurrentImage!.Key;
            T.Eq(v2.ImageIndex, 1);
            sink2.Drain();
            int[] order = [.. Enumerable.Range(0, 24).Where(i => i is < 10 or > 12).OrderBy(i => (i * 7) % 24)];
            foreach (int i in order)
            {
                env.Store.Store.AddFile(files[i], indexInPlace: false);
                await Task.Delay(20).ConfigureAwait(false);
            }
            T.Check(await UntilAsync(() => v2.Series!.Images.Count == 24).ConfigureAwait(false));
            T.Eq(v2.CurrentImage!.Key, middle);
            // Slices 0..9 sort before it (or after, by the stack direction): its position moved by ten.
            T.Check(v2.ImageIndex is 11 or 12, "index " + v2.ImageIndex.ToString(CultureInfo.InvariantCulture));
            T.Eq(s2.Series.Count, 1);
            T.Eq(s2.Series[0].Images.Select(i => i.Key).Distinct().Count(), 24);
            await sink2.SettleAsync().ConfigureAwait(false);
            T.Eq(sink2.LastState(0)?.ImageCount ?? 0, 24);
            // An empty study opened before anything arrived shows its first images when they come.
            SamplePatient later = SampleData.Patient("LIVE^LATER", "LIVE2");
            IReadOnlyList<string> lf = SampleData.WriteCtSeries(env.Dir("later"), later, 3, 32);
            (Core.Viewing.ViewerSession s3, FakeSink sink3) = await env.OpenAsync(later.StudyUid).ConfigureAwait(false);
            using Core.Viewing.ViewerSession _____ = s3;
            T.Check(s3.LoadError is not null && s3.Series.Count == 0);
            foreach (string f in lf)
            {
                env.Store.Store.AddFile(f, indexInPlace: false);
            }
            T.Check(await UntilAsync(() => s3.Series.Count == 1 && s3.Series[0].Images.Count == 3 && s3.LoadError is null).ConfigureAwait(false));
            T.Check(s3.GetViewport(0).Series is not null && s3.Study is not null);
            CapturedFrame fr = await sink3.NextFrameAsync().ConfigureAwait(false);
            T.Check(fr.Width > 0);
        });
        T.Case("network: in the real host the pages stay on 127.0.0.1 with the receiver on; receiver restarts with the app; logs hold no patient data; audit trail", async () =>
        {
            (TestEnv archive, SamplePatient ct, SamplePatient _) = await ArchiveAsync("host-archive").ConfigureAwait(false);
            using TestEnv __ = archive;
            string data = T.NewTempDir("host-net");
            int scpPort = FreePort();
            int kestrelPort;
            Host.RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                NetworkServices net = v.Runtime.Network;
                T.Check(!net.Scp.IsRunning);
                // The receiver on every interface: it is the only thing other computers can reach.
                var s = new ScpSetting { Enabled = true, AeTitle = ViewerAe, Port = scpPort };
                s.AllowedCallingAes.Add(ArchiveAe);
                net.Scp.Apply(s);
                T.Require(net.Scp.IsRunning);
                kestrelPort = v.Port;
                ICollection<string> addresses = v.App.Services.GetRequiredService<Microsoft.AspNetCore.Hosting.Server.IServer>()
                    .Features.Get<Microsoft.AspNetCore.Hosting.Server.Features.IServerAddressesFeature>()!.Addresses;
                T.Check(addresses.Count == 1 && addresses.All(a => new Uri(a).Host == "127.0.0.1"));
                IPAddress? lan = System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces()
                    .Where(n => n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up && n.NetworkInterfaceType != System.Net.NetworkInformation.NetworkInterfaceType.Loopback)
                    .SelectMany(n => n.GetIPProperties().UnicastAddresses).Select(a => a.Address)
                    .FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork);
                if (lan is null)
                {
                    T.Note("no LAN interface: the address check is the listening addresses only");
                }
                else
                {
                    async Task<bool> Connects(int port)
                    {
                        using var c = new TcpClient();
                        try
                        {
                            await c.ConnectAsync(lan, port).WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false);
                            return true;
                        }
                        catch (Exception e) when (e is SocketException or TimeoutException)
                        {
                            return false;
                        }
                    }
                    T.Check(!await Connects(kestrelPort).ConfigureAwait(false), "the pages answer on the LAN address");
                    T.Check(await Connects(scpPort).ConfigureAwait(false), "the receiver does not answer on the LAN address");
                }

                // The archive sends a study to the host's receiver; the host searches the archive and fetches a series.
                StartReceiver(archive, ArchiveAe, [ViewerAe], queryRetrieve: true);
                string toViewer = AddNode(archive, "viewer", ViewerAe, scpPort, ArchiveAe);
                await archive.Network.Transfers.Send(toViewer, new SendRequest(ct.StudyUid, null, "x")).Completion.ConfigureAwait(false);
                T.Check(await UntilAsync(() => v.Runtime.Store.GetStudyAsync(ct.StudyUid).GetAwaiter().GetResult()?.Images == 16).ConfigureAwait(false));
                string node = net.Nodes.Save(new DimseNodeSetting { Name = "Archive", Host = "127.0.0.1", Port = archive.Network.Scp.Port, CalledAe = ArchiveAe, CallingAe = ViewerAe });
                RemoteSearchResult r = await net.Search.SearchStudiesAsync(node, new StudyQuery { Text = "REMOTE^CR" }, CancellationToken.None).ConfigureAwait(false);
                T.Eq(r.Rows.Count, 1);
                T.Eq(r.Rows[0].Local, LocalPresence.None);
                NetworkJob j = net.Transfers.Retrieve(node, r.Rows[0].Study.StudyInstanceUid, null, r.Rows[0].Study.DisplayName, r.Rows[0].RemoteImages);
                await j.Completion.ConfigureAwait(false);
                T.Eq(j.State, NetworkJobState.Completed);
                T.Check(await UntilAsync(() => net.Jobs.Snapshot().Any(x => x.Kind == NetworkJobKind.Receive && x.IsFinished), 12000).ConfigureAwait(false));
            }
            // After the stop: the receiver's port is free again, and scp.stop is in the trail before app.stop.
            using (var probe = new TcpListener(IPAddress.Loopback, scpPort))
            {
                probe.Start();
            }
            List<string> events = [.. AuditLog.ReadEvents(TestHost.AuditFile(data))];
            foreach (string e in (string[])["scp.settings", "scp.start", "scp.receive", "node.change", "search", "retrieve", "scp.stop", "app.stop"])
            {
                T.Check(events.Contains(e), "audit: " + e);
            }
            T.Check(events.LastIndexOf("scp.stop") < events.LastIndexOf("app.stop"));
            T.Check(AuditLog.Verify(TestHost.AuditFile(data), AuditKeys.KeyFile).IsIntact);

            // No patient value and no search text in the app log (memory and files) or the audit trail.
            string logs = string.Join("\n", AppLog.RecentLines) + string.Concat(Directory.EnumerateFiles(Path.Combine(data, "logs")).Select(ReadShared))
                + TestFiles.ReadSharedText(TestHost.AuditFile(data));
            foreach (string phi in (string[])["REMOTE", "RMT100", "RMT200", "ARMT", "SAMPLE^"])
            {
                T.Check(!logs.Contains(phi, StringComparison.Ordinal), "found in a log: " + phi);
            }
            T.Check(logs.Contains("receiver started", StringComparison.Ordinal) && logs.Contains("remote search: 1 studies", StringComparison.Ordinal));

            // The next start brings the receiver back by itself, on the same port; the pages are again on 127.0.0.1 only.
            Host.RunningViewer v2 = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v2.ConfigureAwait(false))
            {
                T.Check(v2.Runtime.Network.Scp.IsRunning);
                T.Eq(v2.Runtime.Network.Scp.Port, scpPort);
                T.Check(archive.Network.Nodes.Test(archive.Network.Nodes.List().Single(n => n.Name == "viewer").Id, CancellationToken.None).Ok);
                // Switching it off in the settings stops it and keeps it off.
                ScpSetting off = v2.Runtime.Network.Nodes.Current.Scp;
                off.Enabled = false;
                v2.Runtime.Network.Scp.Apply(off);
                T.Check(!v2.Runtime.Network.Scp.IsRunning);
            }
        });
    }

    private static string ReadShared(string path)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var r = new StreamReader(fs);
        return r.ReadToEnd();
    }
}
