// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;
using System.Globalization;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Import;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Tests;

internal static class StoreImportTests
{
    private static string WriteSmall(string dir, string name, SamplePatient p, int i)
    {
        string path = Path.Combine(dir, name + i.ToString("D4", CultureInfo.InvariantCulture) + ".dcm");
        SampleData.WriteCrNoSpacing(path, p, 8);
        return path;
    }

    private static SamplePatient Dated(string name, string id, DateOnly date) =>
        SampleData.Patient(name, id) with { StudyDate = date.ToString("yyyyMMdd", CultureInfo.InvariantCulture) };

    public static void Register()
    {
        T.Case("store: paging 50 per page, NO_TOTAL lower bound, exact count, store order", async () =>
        {
            using var env = new TestEnv("paging");
            string dir = env.Dir("in");
            var day0 = new DateOnly(2026, 1, 1);
            for (int i = 0; i < 120; i++)
            {
                WriteSmall(dir, "s", Dated("PAGE^P" + i.ToString("D3", CultureInfo.InvariantCulture), "PG" + i.ToString("D3", CultureInfo.InvariantCulture), day0.AddDays(i)), i);
            }
            ImportProgress ip = await env.ImportAsync(dir).ConfigureAwait(false);
            T.Eq(ip.Added, 120L);
            var q = new StudyQuery { Modalities = ["CR"], Limit = 50 };
            StudyPage p1 = await env.Store.QueryStudiesAsync(q).ConfigureAwait(false);
            T.Eq(p1.Rows.Count, 50);
            T.Check(p1.Total >= 50 && p1.Total <= 120);
            T.Note($"first page with NO_TOTAL: total {p1.Total} ({(p1.TotalExact ? "exact" : "lower bound")})");
            T.Eq(await env.Store.CountStudiesAsync(q).ConfigureAwait(false), 120);
            StudyPage p2 = await env.Store.QueryStudiesAsync(q with { Offset = 50 }).ConfigureAwait(false);
            StudyPage p3 = await env.Store.QueryStudiesAsync(q with { Offset = 100 }).ConfigureAwait(false);
            T.Eq(p2.Rows.Count, 50);
            T.Eq(p3.Rows.Count, 20);
            var all = p1.Rows.Concat(p2.Rows).Concat(p3.Rows).Select(r => r.StudyInstanceUid).ToList();
            T.Eq(all.Distinct().Count(), 120);
            T.Eq(p1.Rows[0].StudyDate, "20260430"); // newest first
            T.Check(string.CompareOrdinal(p1.Rows[49].StudyDate, p2.Rows[0].StudyDate) >= 0);
        });

        T.Case("store: date ranges, wildcard names, ID and accession prefixes", async () =>
        {
            using var env = new TestEnv("filters");
            string dir = env.Dir("in");
            var day0 = new DateOnly(2026, 2, 1);
            for (int i = 0; i < 20; i++)
            {
                WriteSmall(dir, "s", Dated(i % 2 == 0 ? "SMITH^ANNA" : "SMYTHE^BOB", "ID" + i.ToString("D2", CultureInfo.InvariantCulture), day0.AddDays(i)), i);
            }
            await env.ImportAsync(dir).ConfigureAwait(false);
            async Task<int> Count(StudyQuery q) => (await env.Store.QueryStudiesAsync(q with { Limit = 100 }).ConfigureAwait(false)).Rows.Count;
            T.Eq(await Count(new StudyQuery { From = day0, To = day0.AddDays(4) }).ConfigureAwait(false), 5);
            T.Eq(await Count(new StudyQuery { From = day0.AddDays(18) }).ConfigureAwait(false), 2);
            T.Eq(await Count(new StudyQuery { To = day0 }).ConfigureAwait(false), 1);
            T.Eq(await Count(new StudyQuery { Text = "smith" }).ConfigureAwait(false), 10);
            T.Eq(await Count(new StudyQuery { Text = "SM" }).ConfigureAwait(false), 20);
            T.Eq(await Count(new StudyQuery { Text = "ID1" }).ConfigureAwait(false), 10);
            T.Eq(await Count(new StudyQuery { Text = "AID0" }).ConfigureAwait(false), 10); // accession "A" + id
            T.Eq(await Count(new StudyQuery { Text = "nobody" }).ConfigureAwait(false), 0);
            T.Eq(await Count(new StudyQuery { Modalities = ["CT"] }).ConfigureAwait(false), 0);
            T.Eq(await Count(new StudyQuery { Modalities = ["CR", "DX"] }).ConfigureAwait(false), 20);
        });

        T.Case("store: live update event reaches the channel and the subscribers", async () =>
        {
            using var env = new TestEnv("live");
            string dir = env.Dir("in");
            SamplePatient p = SampleData.Patient("LIVE^ONE", "LIVE1");
            WriteSmall(dir, "l", p, 0);
            var got = new TaskCompletionSource<StudiesChangedEventArgs>(TaskCreationOptions.RunContinuationsAsynchronously);
            env.Store.StudiesChanged += (_, e) => got.TrySetResult(e);
            var raw = new TaskCompletionSource<StoreEvent>(TaskCreationOptions.RunContinuationsAsynchronously);
            env.Store.EventReceived += (_, e) => raw.TrySetResult(e.Event);
            await env.ImportAsync(dir).ConfigureAwait(false);
            StudiesChangedEventArgs ev = await got.Task.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
            T.Check(ev.Studies.Contains(p.StudyUid));
            StoreEvent r = await raw.Task.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
            T.Eq(r.Kind, TkStoreEventKind.Added);
            T.Eq(r.StudyInstanceUid, p.StudyUid);
        });

        T.Case("store: delete study and delete patient by key (audited)", async () =>
        {
            using var env = new TestEnv("delete");
            string dir = env.Dir("in");
            SamplePatient a1 = SampleData.Patient("DEL^A", "DELA");
            SamplePatient a2 = a1 with { StudyUid = SampleData.NewUid(), StudyDate = "20250101" };
            SamplePatient b = SampleData.Patient("KEEP^B", "KEEPB");
            WriteSmall(dir, "a", a1, 0);
            WriteSmall(dir, "a", a2, 1);
            WriteSmall(dir, "b", b, 2);
            await env.ImportAsync(dir).ConfigureAwait(false);
            StudyPage page = await env.Store.QueryStudiesAsync(new StudyQuery { Text = "DELA" }).ConfigureAwait(false);
            T.Require(page.Rows.Count == 2);
            string key = page.Rows[0].PatientKey;
            T.Check(key.Length > 0);
            T.Eq(page.Rows[1].PatientKey, key);
            int removed = await env.Store.DeletePatientAsync(key).ConfigureAwait(false);
            T.Eq(removed, 2);
            T.Eq((await env.Store.QueryStudiesAsync(new StudyQuery { Text = "DELA" }).ConfigureAwait(false)).Rows.Count, 0);
            StudyPage keep = await env.Store.QueryStudiesAsync(new StudyQuery { Text = "KEEPB" }).ConfigureAwait(false);
            T.Eq(keep.Rows.Count, 1);
            T.Eq(await env.Store.DeleteStudyAsync(keep.Rows[0].StudyInstanceUid).ConfigureAwait(false), 1);
            T.Eq((await env.Store.GetStatisticsAsync().ConfigureAwait(false)).Instances, 0L);
        });

        T.Case("import: folder with mixed files gives the right counts", async () =>
        {
            using var env = new TestEnv("mixed");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(Path.Combine(dir, "a", "b"), SampleData.Patient("MIX^CT", "MIX1"), 5, 16);
            File.WriteAllText(Path.Combine(dir, "readme.txt"), "not dicom");
            File.WriteAllBytes(Path.Combine(dir, "a", "garbage.dcm"), [.. Enumerable.Range(0, 3000).Select(i => (byte)(i * 31))]);
            File.WriteAllBytes(Path.Combine(dir, "empty.dcm"), []);
            ImportProgress p = await env.ImportAsync(dir).ConfigureAwait(false);
            T.Note($"seen {p.FilesSeen}, instances {p.Instances}, added {p.Added}, skipped {p.Skipped}, errors {p.Errors}");
            T.Eq(p.FilesSeen, 8L);
            T.Eq(p.Instances, 5L);
            T.Eq(p.Added, 5L);
            T.Eq(p.Skipped + p.Errors, 3L);
            T.Check(!p.UsedDicomdir && p.Finished && !p.Cancelled);
            ImportJob job = env.Import.Current!;
            foreach (ImportError e in job.Errors)
            {
                T.Check(!e.Status.Contains("MIX", StringComparison.Ordinal)); // status text only
            }
        });

        T.Case("import: DICOMDIR (any case) with a missing file", async () =>
        {
            using var env = new TestEnv("dicomdir");
            string src = env.Dir("src");
            var files = SampleData.WriteCtSeries(src, SampleData.Patient("DIR^CT", "DIR1"), 6, 16);
            string set = env.Dir("set");
            Tk.WriteDicomdir(set, files);
            string[] copied = Directory.GetFiles(Path.Combine(set, "DICOM"), "*", SearchOption.AllDirectories);
            T.Require(copied.Length == 6);
            File.Delete(copied[2]);
            if (!OperatingSystem.IsWindows())
            {
                File.Move(Path.Combine(set, "DICOMDIR"), Path.Combine(set, "dicomdir"));
            }
            T.Check(ImportService.FindDicomdir(set) is not null);
            ImportProgress p = await env.ImportAsync(set).ConfigureAwait(false);
            T.Check(p.UsedDicomdir);
            T.Eq(p.Added, 5L);
            T.Eq(p.Errors, 1L);
            ImportError e = env.Import.Current!.Errors.Single();
            T.Check(e.Status.Contains("missing", StringComparison.Ordinal));
        });

        T.Case("import: cancel stops within 200 ms", async () =>
        {
            using var env = new TestEnv("cancel");
            string dir = env.Dir("in");
            SamplePatient p = SampleData.Patient("CANCEL^X", "CAN1");
            for (int i = 0; i < 1500; i++)
            {
                WriteSmall(dir, "c", p with { StudyUid = i % 50 == 0 ? SampleData.NewUid() : p.StudyUid }, i);
            }
            ImportJob job = env.Import.Start(new ImportRequest(dir));
            var sw = Stopwatch.StartNew();
            while (job.Progress.Added < 20 && sw.ElapsedMilliseconds < 20000)
            {
                await Task.Delay(2).ConfigureAwait(false);
            }
            var stop = Stopwatch.StartNew();
            job.Cancel();
            await job.Completion.ConfigureAwait(false);
            stop.Stop();
            ImportProgress pr = job.Progress;
            T.Perf($"import cancel took {stop.Elapsed.TotalMilliseconds:F0} ms (added {pr.Added} of 1500)");
            T.Check(stop.Elapsed.TotalMilliseconds < 200);
            T.Check(pr.Cancelled && pr.Finished);
            T.Check(pr.Added < 1500);
            T.Check(AuditLog.ReadEvents(env.Paths.AuditFile).Contains("import"));
        });

        T.Case("import: a duplicate keeps the existing copy; index in place references the file", async () =>
        {
            using var env = new TestEnv("dup");
            string dir = env.Dir("in");
            SamplePatient p = SampleData.Patient("DUP^ORIGINAL", "DUP1");
            string file = WriteSmall(dir, "d", p, 0);
            ImportProgress first = await env.ImportAsync(dir).ConfigureAwait(false);
            T.Eq(first.Added, 1L);
            // Same SOP Instance UID, different name: the stored copy must win.
            string sop;
            using (TkFile f = TkFile.Open(file, headersOnly: false))
            {
                sop = f.GetString(Tags.SopInstanceUid)!;
                f.SetString(Tags.PatientName, "DUP^CHANGED");
                f.Save(file);
            }
            ImportProgress second = await env.ImportAsync(dir).ConfigureAwait(false);
            T.Eq(second.Added, 0L);
            T.Eq(second.Duplicates, 1L);
            StudyPage page = await env.Store.QueryStudiesAsync(new StudyQuery { Text = "DUP1" }).ConfigureAwait(false);
            T.Eq(page.Rows.Single().PatientName, "DUP^ORIGINAL");
            var series = await env.Store.GetSeriesAsync(page.Rows[0].StudyInstanceUid).ConfigureAwait(false);
            var inst = await env.Store.GetInstancesAsync(series[0].SeriesInstanceUid).ConfigureAwait(false);
            T.Eq(inst[0].SopInstanceUid, sop);
            T.Check(!string.Equals(Path.GetFullPath(inst[0].FilePath), Path.GetFullPath(file), StringComparison.Ordinal)); // copied
            using (TkFile stored = TkFile.Open(inst[0].FilePath, headersOnly: true))
            {
                T.Eq(stored.GetString(Tags.PatientName), "DUP^ORIGINAL");
            }

            string dir2 = env.Dir("inplace");
            string ref1 = WriteSmall(dir2, "r", SampleData.Patient("REF^ONE", "REF1"), 0);
            ImportProgress r = await env.ImportAsync(dir2, inPlace: true).ConfigureAwait(false);
            T.Eq(r.Added, 1L);
            StudyPage rp = await env.Store.QueryStudiesAsync(new StudyQuery { Text = "REF1" }).ConfigureAwait(false);
            var rs = await env.Store.GetSeriesAsync(rp.Rows[0].StudyInstanceUid).ConfigureAwait(false);
            var ri = await env.Store.GetInstancesAsync(rs[0].SeriesInstanceUid).ConfigureAwait(false);
            T.Eq(Path.GetFullPath(ri[0].FilePath), Path.GetFullPath(ref1));
        });
    }
}
