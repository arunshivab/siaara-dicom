// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Tests;

internal static class CoreTests
{
    public static void Register()
    {
        T.Case("interop: ABI of the native library equals the compile-time value", () =>
        {
            T.Eq(Tk.AbiVersion, Tk.ExpectedAbiVersion);
            T.Check(Tk.AbiMatches);
            T.Check(Tk.Version.Length > 0);
            T.Note("toolkit " + Tk.Version + ", ABI " + Tk.AbiVersion + ", library " + Path.GetFileName(Tk.NativeLibraryPath ?? "?"));
        });

        T.Case("interop: UTF-8 names round-trip through file, store and query unchanged", async () =>
        {
            using var env = new TestEnv("utf8");
            string dir = env.Dir("in");
            const string name = "Müller^Zoë^Ångström";
            using (TkFile f = TkFile.Create())
            {
                f.SetString(Tags.SpecificCharacterSet, "ISO_IR 192");
                f.SetRawString(Tags.SopClassUid, "UI", SampleData.CrImageStorage);
                f.SetRawString(Tags.SopInstanceUid, "UI", SampleData.NewUid());
                f.SetRawString(Tags.StudyInstanceUid, "UI", SampleData.NewUid());
                f.SetRawString(Tags.SeriesInstanceUid, "UI", SampleData.NewUid());
                f.SetString(Tags.PatientName, name);
                f.SetString(Tags.PatientId, "UTF8-1");
                f.SetString(Tags.Modality, "OT");
                f.Save(Path.Combine(dir, "u.dcm"));
            }
            using (TkFile back = TkFile.Open(Path.Combine(dir, "u.dcm"), headersOnly: true))
            {
                T.Eq(back.GetString(Tags.PatientName), name);
            }
            await env.ImportAsync(dir).ConfigureAwait(false);
            StudyPage p = await env.Store.QueryStudiesAsync(new StudyQuery { Text = "müller" }).ConfigureAwait(false);
            T.Require(p.Rows.Count == 1);
            T.Eq(p.Rows[0].PatientName, name);
            T.Eq(p.Rows[0].DisplayName, "Müller, Zoë Ångström");
            T.Eq(p.Rows[0].PatientName, name); // display formatting did not change the stored value
        });

        T.Case("display: '^' is shown as ', ' / space without changing the stored value", () =>
        {
            string stored = "Doe^Jane^Q^Dr^Jr";
            T.Eq(DisplayFormat.PersonName(stored), "Dr Doe, Jane Q Jr");
            T.Eq(stored, "Doe^Jane^Q^Dr^Jr");
            T.Eq(DisplayFormat.PersonName("^^^^"), "");
            T.Eq(DisplayFormat.PersonName("=山田^太郎"), "山田, 太郎");
            T.Eq(DisplayFormat.Date("20260301"), "2026-03-01");
            T.Eq(DisplayFormat.Date("2026"), "2026");
            T.Eq(DisplayFormat.Age("045Y", null, null), "45Y");
            T.Eq(DisplayFormat.Age(null, "19800615", "20260301"), "45Y");
        });

        T.Case("settings: JSON round-trip with atomic write; corrupt file kept aside", () =>
        {
            string dir = T.NewTempDir("settings");
            string path = Path.Combine(dir, "settings.json");
            var s = new SettingsStore(path);
            T.Check(s.Current.Presets.ContainsKey("CT"));
            T.Eq(s.Current.Presets["CT"].Count, 8);
            s.Update(p => { p.Theme = "light"; p.DisplayCalibration = 1.05; });
            var again = new SettingsStore(path);
            T.Eq(again.Current.Theme, "light");
            T.Near(again.Current.DisplayCalibration, 1.05, 1e-9);
            T.Check(Directory.GetFiles(dir, "*.tmp").Length == 0);
            File.WriteAllText(path, "{ not json");
            var bad = new SettingsStore(path);
            T.Eq(bad.Current.Theme, "dark");
            T.Check(Directory.GetFiles(dir, "settings.json.corrupt-*").Length == 1);
            string json = Encoding.UTF8.GetString(SettingsStore.Serialize(again.Current));
            T.Check(!json.Contains("secret", StringComparison.OrdinalIgnoreCase) && !json.Contains("password", StringComparison.OrdinalIgnoreCase));
        });

        T.Case("audit: append-only signed chain verifies; an edited line breaks it", () =>
        {
            string dir = T.NewTempDir("audit");
            string path = Path.Combine(dir, "audit.jsonl");
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("app.start", AuditOutcome.Success);
                a.Append("study.open", AuditOutcome.Success, new Dictionary<string, string> { ["patientKey"] = "K1", ["study"] = "1.2.3" });
            }
            using (var a = new AuditLog(path, AuditKeys.KeyFile))
            {
                a.Append("study.delete", AuditOutcome.Failure, new Dictionary<string, string> { ["patientKey"] = "K1" }, "file system error");
            }
            // Every entry carries an outcome; failures a reason.
            foreach (string line in File.ReadAllLines(path))
            {
                using var doc = System.Text.Json.JsonDocument.Parse(line);
                T.Check(doc.RootElement.TryGetProperty("outcome", out System.Text.Json.JsonElement o) && o.GetString() is "success" or "failure");
            }
            T.Check(File.ReadAllLines(path)[2].Contains("\"reason\":\"file system error\"", StringComparison.Ordinal));
            AuditVerification v = AuditLog.Verify(path, AuditKeys.KeyFile);
            T.Check(v.IsIntact);
            T.Eq(v.Lines, 3);
            T.Check(AuditLog.ReadEvents(path).SequenceEqual(["app.start", "study.open", "study.delete"]));
            string[] lines = File.ReadAllLines(path);
            lines[1] = lines[1].Replace("K1", "K2", StringComparison.Ordinal);
            TestFiles.WriteLf(path, lines);
            AuditVerification broken = AuditLog.Verify(path, AuditKeys.KeyFile);
            T.Check(!broken.IsIntact);
            // The edited line itself fails its signature (the unkeyed chain only noticed at the line after it).
            T.Eq(broken.FirstBadLine, 2);
        });

        T.Case("logging: the PHI redactor removes UIDs, names, dates, IDs and paths", () =>
        {
            string s = PhiRedactor.Redact("open 1.2.840.113619.2.55 for DOE^JANE id 12345678 at /home/u/Patients/Doe on 20260301 mail a.b@c.org /ws/view");
            T.Check(!s.Contains("1.2.840", StringComparison.Ordinal));
            T.Check(!s.Contains("DOE", StringComparison.Ordinal));
            T.Check(!s.Contains("12345678", StringComparison.Ordinal));
            T.Check(!s.Contains("Patients", StringComparison.Ordinal));
            T.Check(!s.Contains("20260301", StringComparison.Ordinal));
            T.Check(!s.Contains("a.b@c.org", StringComparison.Ordinal));
            T.Check(s.Contains("/ws/view", StringComparison.Ordinal));
        });

        T.Case("layout: defaults per series count and 1x1 to 4x4", () =>
        {
            T.Eq(LayoutModel.ForSeriesCount(1), new LayoutModel(1, 1));
            T.Eq(LayoutModel.ForSeriesCount(2), new LayoutModel(1, 2));
            T.Eq(LayoutModel.ForSeriesCount(3), new LayoutModel(2, 2));
            T.Eq(LayoutModel.ForSeriesCount(9), new LayoutModel(2, 2));
            T.Eq(LayoutModel.Of(9, 0), new LayoutModel(4, 1));
            T.Check(LayoutModel.TryParse("3x4", out LayoutModel l) && l.Count == 12);
            T.Check(!LayoutModel.TryParse("5x1", out _));
        });

        T.Case("cine: index from elapsed time (loop and bounce) is true to the rate", () =>
        {
            T.Eq(CinePlayer.IndexAt(0, 10, 10, CineMode.Loop, 0.0), 0);
            T.Eq(CinePlayer.IndexAt(0, 10, 10, CineMode.Loop, 0.95), 9);
            T.Eq(CinePlayer.IndexAt(0, 10, 10, CineMode.Loop, 1.05), 0);
            T.Eq(CinePlayer.IndexAt(0, 5, 10, CineMode.Bounce, 0.4), 4);
            T.Eq(CinePlayer.IndexAt(0, 5, 10, CineMode.Bounce, 0.6), 2);
            T.Eq(CinePlayer.IndexAt(3, 5, 25, CineMode.Loop, 0.08), 0);
        });

        T.Case("scheduler: pools sized from the cores; cancelled work never runs", async () =>
        {
            using var s = new ToolkitScheduler(8);
            T.Eq(s.GetThreadCount(WorkPool.Interactive), 4);
            T.Eq(s.GetThreadCount(WorkPool.Decode), 4);
            T.Eq(s.GetThreadCount(WorkPool.Import), 2);
            T.Eq(s.GetThreadCount(WorkPool.StoreRead), 2);
            T.Eq(await s.RunAsync(WorkPool.StoreRead, _ => 42).ConfigureAwait(false), 42);
            using var cts = new CancellationTokenSource();
            using var gate = new ManualResetEventSlim();
            Task block = s.RunAsync(WorkPool.Import, _ => gate.Wait(5000, CancellationToken.None));
            Task block2 = s.RunAsync(WorkPool.Import, _ => gate.Wait(5000, CancellationToken.None));
            bool ran = false;
            Task t = s.RunAsync(WorkPool.Import, _ => ran = true, cts.Token);
            await cts.CancelAsync().ConfigureAwait(false);
            gate.Set();
            await Task.WhenAll(block, block2).ConfigureAwait(false);
            try
            {
                await t.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
            T.Check(t.IsCanceled);
            T.Check(!ran);
        });

        T.Case("build: no PackageReference in any app project", () =>
        {
            string appDir = FindAppDir();
            var offenders = Directory.EnumerateFiles(appDir, "*.*proj", SearchOption.AllDirectories)
                .Concat(Directory.EnumerateFiles(appDir, "*.props", SearchOption.AllDirectories))
                .Concat(Directory.EnumerateFiles(appDir, "*.targets", SearchOption.AllDirectories))
                .Where(f => !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
                .Where(f => File.ReadAllText(f).Contains("<PackageReference", StringComparison.Ordinal))
                .ToArray();
            T.Check(offenders.Length == 0);
            T.Check(File.ReadAllText(Path.Combine(appDir, "Directory.Build.targets")).Contains("@(PackageReference)", StringComparison.Ordinal));
            string js = File.ReadAllText(Path.Combine(appDir, "src", "Siaara.Viewer.Web", "App.razor"));
            T.Check(!js.Contains("http://", StringComparison.Ordinal) && !js.Contains("https://", StringComparison.Ordinal)); // no CDN
        });
    }

    public static string FindAppDir()
    {
        string? d = AppContext.BaseDirectory;
        while (d is not null && !File.Exists(Path.Combine(d, "Siaara.Viewer.slnx")))
        {
            d = Path.GetDirectoryName(d);
        }
        return d ?? throw new DirectoryNotFoundException("app folder");
    }
}
