// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Security review round 4, the audit log: R-126 (keyed signatures, anchor against cut-off ends,
// repair after a torn write, rotation with the chain carried across files, opening without
// reading the history) and R-127 (intent before a delete, a delete that fails when the intent
// cannot be written, settings changes).

using System.Security.Cryptography;
using System.Text;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Host;
using Siaara.Viewer.Web;

namespace Siaara.Viewer.Tests;

internal static class AuditTests
{
    private static readonly bool[] StudyThenPatient = [false, true];

    private static string NewLog(string name, int entries, out string dir)
    {
        dir = T.NewTempDir(name);
        string path = Path.Combine(dir, "audit.jsonl");
        using var a = new AuditLog(path, AuditKeys.KeyFile);
        for (int i = 0; i < entries; i++)
        {
            a.Append("test.event", AuditOutcome.Success, new Dictionary<string, string> { ["n"] = i.ToString(System.Globalization.CultureInfo.InvariantCulture) });
        }
        return path;
    }

    private static AuditVerification Verify(string path) => AuditLog.Verify(path, AuditKeys.KeyFile);

    public static void Register()
    {
        T.Case("audit: every line is signed with a key that is not in the log; a chain rebuilt without the key does not verify", () =>
        {
            string path = NewLog("auditkey", 4, out string dir);
            T.Check(Verify(path).IsIntact);
            T.Eq(Verify(path).Lines, 4);
            string[] lines = File.ReadAllLines(path);
            T.Check(lines.All(l => l.Contains("\"mac\":\"", StringComparison.Ordinal) && l.Contains("\"prev\":\"", StringComparison.Ordinal)));
            string key = File.ReadAllText(Path.Combine(dir, "audit.key")).Trim();
            T.Eq(key.Length, 64);
            T.Check(!File.ReadAllText(path).Contains(key, StringComparison.Ordinal));
            if (!OperatingSystem.IsWindows())
            {
                T.Eq(File.GetUnixFileMode(Path.Combine(dir, "audit.key")), UnixFileMode.UserRead | UnixFileMode.UserWrite);
                T.Eq(File.GetUnixFileMode(AuditLog.AnchorPath(path)), UnixFileMode.UserRead | UnixFileMode.UserWrite);
                T.Eq(File.GetUnixFileMode(path), UnixFileMode.UserRead | UnixFileMode.UserWrite);
            }
            // The attack the unkeyed chain allowed: change a line, then recompute every hash after it with SHA-256
            // (anybody can) so that each "prev" matches again. Without the key the new lines carry no valid mac.
            string original = File.ReadAllText(path);
            string prev = new('0', 64);
            var forged = new List<string>();
            foreach (string line in lines)
            {
                int at = line.IndexOf(",\"prev\":\"", StringComparison.Ordinal);
                string body = line[..at].Replace("\"n\":\"1\"", "\"n\":\"9\"", StringComparison.Ordinal) + ",\"prev\":\"" + prev + "\"}";
                string mac = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(body)));
                forged.Add(body[..^1] + ",\"mac\":\"" + mac + "\"}");
                prev = mac;
            }
            File.WriteAllText(path, string.Join("\n", forged) + "\n");
            AuditVerification v = Verify(path);
            T.Check(!v.IsIntact);
            T.Check(v.Problem is not null);
            // One changed character in one line.
            File.WriteAllText(path, original.Replace("\"n\":\"2\"", "\"n\":\"7\"", StringComparison.Ordinal));
            v = Verify(path);
            T.Check(!v.IsIntact);
            T.Eq(v.FirstBadLine, 3);
            // Two lines swapped, a line removed from the middle, a line duplicated.
            File.WriteAllLines(path, [lines[0], lines[2], lines[1], lines[3]]);
            T.Check(!Verify(path).IsIntact);
            File.WriteAllLines(path, [lines[0], lines[2], lines[3]]);
            T.Check(!Verify(path).IsIntact);
            File.WriteAllLines(path, [lines[0], lines[1], lines[1], lines[2], lines[3]]);
            T.Check(!Verify(path).IsIntact);
            File.WriteAllText(path, original);
            T.Check(Verify(path).IsIntact);
            // Without the key nothing can be checked, and that is said.
            string keyFile = Path.Combine(dir, "audit.key");
            File.Move(keyFile, keyFile + ".away");
            v = Verify(path);
            T.Check(!v.IsIntact && v.Problem!.Contains("key", StringComparison.Ordinal));
            // A log whose key is gone is kept aside (never continued with a new key as if nothing happened).
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("after", AuditOutcome.Success);
            }
            T.Check(Verify(path).IsIntact);
            T.Eq(Directory.GetFiles(dir, "audit.unverifiable-*.jsonl.old").Length, 1);
            T.Check(AuditLog.ReadEntries(path).Any(e => e.Event == AuditLog.SecurityAlert && e.Data["kind"] == "audit-unverifiable"));

            // The OS key store of this computer (keychain / DPAPI / key file) gives the same key back, and none after Delete.
            string osDir = T.NewTempDir("auditoskey");
            try
            {
                AuditKey made = AuditKeys.OsStore.Load(osDir, create: true)!;
                T.Check(made.Created && made.Key.Length == 32);
                AuditKey again = AuditKeys.OsStore.Load(osDir, create: false)!;
                T.Check(!again.Created && again.Key.Span.SequenceEqual(made.Key.Span));
                T.Eq(again.Protection, made.Protection);
                T.Note("audit key protection on this OS: " + made.Protection);
            }
            finally
            {
                AuditKeys.Delete(osDir);
            }
            T.Check(AuditKeys.OsStore.Load(osDir, create: false) is null);
        });

        T.Case("audit: cutting entries off the end is detected by the anchor, and recorded at the next start", () =>
        {
            string path = NewLog("audittail", 5, out string dir);
            T.Check(Verify(path).IsIntact);
            string[] lines = File.ReadAllLines(path);
            // The earlier log could not tell this file from a complete one.
            File.WriteAllLines(path, lines[..3]);
            AuditVerification v = Verify(path);
            T.Check(!v.IsIntact);
            T.Check(v.Problem!.Contains("missing from the end", StringComparison.Ordinal));
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("later", AuditOutcome.Success);
                // Numbers are not reused: the alert is entry 6, after the lost 4 and 5.
                T.Eq(a.LastSequence, 7L);
            }
            IReadOnlyList<AuditEntry> entries = AuditLog.ReadEntries(path);
            AuditEntry alert = entries.Single(e => e.Event == AuditLog.SecurityAlert);
            T.Eq(alert.Data["kind"], "audit-truncated");
            T.Eq((alert.Data["anchorSeq"], alert.Data["lastSignedSeq"]), ("5", "3"));
            // The loss stays visible for good.
            v = Verify(path);
            T.Check(!v.IsIntact);
            T.Eq(v.FirstBadLine, 4);

            // The whole file emptied, the anchor removed, the anchor replaced by an unsigned one.
            string p2 = NewLog("audittail2", 3, out _);
            File.WriteAllText(p2, "");
            T.Check(!Verify(p2).IsIntact);
            string p3 = NewLog("audittail3", 3, out _);
            File.Delete(AuditLog.AnchorPath(p3));
            T.Check(!Verify(p3).IsIntact);
            using (var a = new AuditLog(p3, AuditKeys.KeyFile))
            {
            }
            T.Check(AuditLog.ReadEntries(p3).Any(e => e.Event == AuditLog.SecurityAlert && e.Data["kind"] == "audit-anchor-missing"));
            string p4 = NewLog("audittail4", 3, out _);
            File.WriteAllText(AuditLog.AnchorPath(p4), "{\"seq\":2,\"mac\":\"" + new string('0', 64) + "\",\"tag\":\"" + new string('0', 64) + "\"}\n");
            T.Check(!Verify(p4).IsIntact);
            using (var a = new AuditLog(p4, AuditKeys.KeyFile))
            {
            }
            T.Check(AuditLog.ReadEntries(p4).Any(e => e.Event == AuditLog.SecurityAlert && e.Data["kind"] == "audit-anchor-invalid"));
            // The anchor one entry behind (a crash between the line and the anchor) is not a fault.
            string p5 = NewLog("audittail5", 3, out _);
            string anchorAt3 = File.ReadAllText(AuditLog.AnchorPath(p5));
            using (var a = new AuditLog(p5, AuditKeys.KeyFile))
            {
                a.Append("fourth", AuditOutcome.Success);
            }
            File.WriteAllText(AuditLog.AnchorPath(p5), anchorAt3);
            T.Check(Verify(p5).IsIntact);
            using (var a = new AuditLog(p5, AuditKeys.KeyFile))
            {
                a.Append("fifth", AuditOutcome.Success);
            }
            T.Check(Verify(p5).IsIntact);
            T.Check(!AuditLog.ReadEvents(p5).Contains(AuditLog.SecurityAlert));
        });

        T.Case("audit: a torn last line (crash in the middle of a write) is repaired with audit.recovered and the trail verifies again", () =>
        {
            string path = NewLog("audittorn", 3, out _);
            // Half a line, no line break: what a crash in the middle of a write leaves.
            string full = File.ReadAllLines(path)[2];
            File.AppendAllText(path, full[..(full.Length / 2)].Replace("\"seq\":3", "\"seq\":4", StringComparison.Ordinal));
            T.Check(!Verify(path).IsIntact);
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("after.crash", AuditOutcome.Success);
            }
            AuditVerification v = Verify(path);
            T.Check(v.IsIntact, v.Problem ?? "");
            T.Eq(v.Recovered, 1);
            T.Eq(v.Lines, 5);
            T.Check(AuditLog.ReadEvents(path).SequenceEqual(["test.event", "test.event", "test.event", "audit.recovered", "after.crash"]));
            AuditEntry rec = AuditLog.ReadEntries(path).Single(e => e.Event == "audit.recovered");
            T.Eq(rec.Seq, 4L);
            T.Eq(rec.Data["tornBytes"], (full.Length / 2).ToString(System.Globalization.CultureInfo.InvariantCulture));
            // The next entry did not land on the torn line.
            string[] after = File.ReadAllLines(path);
            T.Eq(after.Length, 6);
            T.Check(!after[3].EndsWith('}') && after.Skip(4).All(l => l.StartsWith("{\"seq\":", StringComparison.Ordinal) && l.EndsWith("\"}", StringComparison.Ordinal)));
            // It keeps verifying over later starts and entries.
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("next.day", AuditOutcome.Success);
            }
            T.Check(Verify(path).IsIntact);
            // A complete, signed last line that only lacks its line break is completed quietly.
            string p2 = NewLog("audittorn2", 2, out _);
            byte[] bytes = File.ReadAllBytes(p2);
            File.WriteAllBytes(p2, bytes[..^1]);
            using (var a = new AuditLog(p2, AuditKeys.KeyFile))
            {
                a.Append("third", AuditOutcome.Success);
            }
            v = Verify(p2);
            T.Check(v.IsIntact && v.Recovered == 0 && v.Lines == 3);
            // An "audit.recovered" entry does not excuse a torn fragment it does not describe: change the fragment.
            string text = File.ReadAllText(path);
            string torn = File.ReadAllLines(path)[3];
            File.WriteAllText(path, text.Replace(torn, torn + "x", StringComparison.Ordinal));
            T.Check(!Verify(path).IsIntact);
            // And a signed line damaged AFTER it was anchored is not a crash: it is reported, not excused.
            string p3 = NewLog("audittorn3", 3, out _);
            string[] l3 = File.ReadAllLines(p3);
            File.WriteAllText(p3, l3[0] + "\n" + l3[1] + "\n" + l3[2][..40]);
            using (var a = new AuditLog(p3, AuditKeys.KeyFile))
            {
            }
            T.Check(!Verify(p3).IsIntact);
            T.Check(AuditLog.ReadEntries(p3).Any(e => e.Event == AuditLog.SecurityAlert && e.Data["kind"] == "audit-damaged"));
        });

        T.Case("audit: the file is rotated by size with the chain carried across files; opening reads the end, not the history", () =>
        {
            string dir = T.NewTempDir("auditrot");
            string path = Path.Combine(dir, "audit.jsonl");
            using (var a = new AuditLog(path, AuditKeys.KeyFile, maxFileBytes: 4096))
            {
                for (int i = 0; i < 100; i++)
                {
                    a.Append("study.open", AuditOutcome.Success, new Dictionary<string, string> { ["patientKey"] = "K" + i, ["study"] = "1.2.3." + i });
                }
            }
            IReadOnlyList<string> rotated = AuditLog.RotatedFiles(dir);
            T.Check(rotated.Count >= 3);
            T.Check(rotated.All(f => new FileInfo(f).Length is >= 4096 and < 4096 + 1024));
            T.Check(new FileInfo(path).Length < 4096 + 1024);
            AuditVerification v = Verify(path);
            T.Check(v.IsIntact, v.Problem ?? "");
            T.Eq(v.Files, rotated.Count + 1);
            T.Eq(v.Lines, 100 + rotated.Count);
            T.Eq(AuditLog.ReadEvents(path).Count(e => e == "study.open"), 100);
            T.Eq(AuditLog.ReadEvents(path).Count(e => e == "audit.rotated"), rotated.Count);
            if (!OperatingSystem.IsWindows())
            {
                foreach (string f in rotated)
                {
                    T.Eq(File.GetUnixFileMode(f), UnixFileMode.UserRead | UnixFileMode.UserWrite);
                }
            }
            // A new start continues the same chain (sequence numbers go on).
            using (var a = new AuditLog(path, AuditKeys.KeyFile, maxFileBytes: 4096))
            {
                T.Eq(a.LastSequence, (long)v.Lines);
                a.Append("app.start", AuditOutcome.Success);
            }
            T.Check(Verify(path).IsIntact);
            // Opening does not read the history: with the oldest rotated file unreadable it still opens and appends.
            // The full check is what reads everything, and it notices.
            string first = rotated[0];
            byte[] keep = File.ReadAllBytes(first);
            File.WriteAllBytes(first, Encoding.ASCII.GetBytes("not an audit file\n"));
            using (var a = new AuditLog(path, AuditKeys.KeyFile, maxFileBytes: 4096))
            {
                T.Check(a.Append("app.start", AuditOutcome.Success));
            }
            T.Check(!AuditLog.ReadEvents(path).Contains(AuditLog.SecurityAlert));
            T.Check(!Verify(path).IsIntact);
            File.WriteAllBytes(first, keep);
            T.Check(Verify(path).IsIntact);
            // A rotated file removed from the middle breaks the chain.
            File.Move(rotated[1], rotated[1] + ".gone");
            v = Verify(path);
            T.Check(!v.IsIntact && v.Problem!.Contains("missing", StringComparison.Ordinal));
            File.Move(rotated[1] + ".gone", rotated[1]);
            // A crash right after the rename (current file not yet created): the chain continues from the rotated file.
            File.Move(path, Path.Combine(dir, "audit." + 999999.ToString("D12", System.Globalization.CultureInfo.InvariantCulture) + ".jsonl"));
            using (var a = new AuditLog(path, AuditKeys.KeyFile, maxFileBytes: 4096))
            {
                a.Append("after.rotation.crash", AuditOutcome.Success);
            }
            T.Check(Verify(path).IsIntact);
        });

        T.Case("audit: a log in the earlier format is kept aside with its SHA-256 recorded; writing never throws, required entries do", () =>
        {
            string dir = T.NewTempDir("auditlegacy");
            string path = Path.Combine(dir, "audit.jsonl");
            string legacy = "{\"seq\":1,\"ts\":\"2026-11-01T00:00:00.0000000Z\",\"event\":\"app.start\",\"user\":\"u\",\"outcome\":\"success\",\"data\":{},\"prev\":\"" + new string('0', 64) + "\"}\n";
            File.WriteAllText(path, legacy);
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("app.start", AuditOutcome.Success);
            }
            string aside = Directory.GetFiles(dir, "audit.legacy-*.jsonl.old").Single();
            T.Eq(File.ReadAllText(aside), legacy);
            AuditEntry migrated = AuditLog.ReadEntries(path)[0];
            T.Eq(migrated.Event, "audit.migrated");
            T.Eq(migrated.Seq, 1L);
            T.Eq(migrated.Data["sha256"], Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(legacy))));
            T.Eq(migrated.Data["file"], Path.GetFileName(aside));
            T.Check(Verify(path).IsIntact);

            // Append reports a failure instead of throwing; AppendRequired throws; nothing half-written is left behind.
            var log = new AuditLog(path, AuditKeys.KeyFile);
            log.Dispose();
            T.Check(!log.Append("late", AuditOutcome.Success));
            bool threw = false;
            try
            {
                log.AppendRequired("late", AuditOutcome.Intent);
            }
            catch (AuditUnavailableException)
            {
                threw = true;
            }
            T.Check(threw);
            T.Check(Verify(path).IsIntact);
            T.Check(Core.Logging.AppLog.RecentLines.Any(l => l.Contains("an audit entry could not be written", StringComparison.Ordinal)));
        });

        T.Case("delete: the intent is audited before anything is removed; when it cannot be written the delete fails and nothing is deleted", async () =>
        {
            string data = T.NewTempDir("delintent");
            string dir = Path.Combine(data, "in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("INTENT^CT", "INT1"), 3, 32);
            SampleData.WriteCtSeries(Path.Combine(dir, "b"), SampleData.Patient("INTENT^TWO", "INT2"), 2, 32);
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                await v.Runtime.Import.Start(new Core.Import.ImportRequest(dir)).Completion.ConfigureAwait(false);
                StudyRow first = (await v.Runtime.Store.QueryStudiesAsync(new StudyQuery { Text = "INT1" }).ConfigureAwait(false)).Rows[0];
                StudyRow second = (await v.Runtime.Store.QueryStudiesAsync(new StudyQuery { Text = "INT2" }).ConfigureAwait(false)).Rows[0];
                // A normal delete: intent, then outcome.
                T.Eq(await v.Runtime.DeleteStudyAsync(first).ConfigureAwait(false), 3);
                AuditEntry[] deletes = [.. AuditLog.ReadEntries(TestHost.AuditFile(data)).Where(e => e.Event == "study.delete")];
                T.Eq(deletes.Length, 2);
                T.Eq((deletes[0].Outcome, deletes[1].Outcome), (AuditOutcome.Intent, AuditOutcome.Success));
                T.Check(deletes[0].Seq < deletes[1].Seq && deletes[1].Data["instances"] == "3" && deletes[0].Data["patientKey"] == first.PatientKey);

                // The audit log cannot be written (closed here; a full disk or a removed folder in real life).
                long instancesBefore = (await v.Runtime.Store.GetStatisticsAsync().ConfigureAwait(false)).Instances;
                v.Runtime.Audit.Dispose();
                foreach (bool patient in StudyThenPatient)
                {
                    bool refused = false;
                    try
                    {
                        _ = patient ? await v.Runtime.DeletePatientAsync(second).ConfigureAwait(false) : await v.Runtime.DeleteStudyAsync(second).ConfigureAwait(false);
                    }
                    catch (AuditUnavailableException)
                    {
                        refused = true;
                    }
                    T.Check(refused);
                }
                T.Eq((await v.Runtime.Store.GetStatisticsAsync().ConfigureAwait(false)).Instances, instancesBefore);
                T.Eq((await v.Runtime.Store.QueryStudiesAsync(new StudyQuery { Text = "INT2" }).ConfigureAwait(false)).Rows.Count, 1);
                // Its viewer sessions were not closed either: nothing of the delete ran.
                T.Eq(instancesBefore, 2L);
            }
            T.Check(AuditLog.Verify(TestHost.AuditFile(data), AuditKeys.KeyFile).IsIntact);
        });

        T.Case("audit: settings changes are recorded by name, a changed store folder as a reference", () =>
        {
            using var env = new TestEnv("auditsettings");
            using (var rt = new ViewerRuntime(env.Paths, env.Settings, env.Scheduler, env.Store, env.Audit))
            {
                string newStore = Path.Combine(env.Root, "SMITH^ANN store");
                rt.UpdateSettings(p =>
                {
                    p.Theme = "light";
                    p.StoreRoot = newStore;
                });
                rt.UpdateSettings(p => p.Theme = "light");
                AuditEntry[] changes = [.. AuditLog.ReadEntries(env.Paths.AuditFile).Where(e => e.Event == "settings.change")];
                T.Eq(changes.Length, 1);
                T.Eq(changes[0].Data["changed"], "storeRoot,theme");
                T.Eq(changes[0].Data["storeRoot"], env.Audit.PathReference(newStore));
                T.Check(!File.ReadAllText(env.Paths.AuditFile).Contains("SMITH", StringComparison.Ordinal));
            }
        });
    }
}
