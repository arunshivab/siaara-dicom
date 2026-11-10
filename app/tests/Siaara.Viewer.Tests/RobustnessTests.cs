// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// m1-review fixes: single-use launch secrets and the owner-only instance file, ref-counted cache
// leases, observed background tasks, released header handles, no-store on PHI responses, audit
// outcomes (failed deletes too).

using System.Net;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Samples;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;
using Siaara.Viewer.Host;

namespace Siaara.Viewer.Tests;

internal static class RobustnessTests
{
    private static HttpClient Client(RunningViewer v, bool cookie) => TestHost.Client(v, cookie);

    private static async Task<bool> WaitUntilAsync(Func<bool> probe, int timeoutMs = 5000)
    {
        for (int i = 0; i < timeoutMs / 10; i++)
        {
            if (probe())
            {
                return true;
            }
            await Task.Delay(10).ConfigureAwait(false);
        }
        return probe();
    }

    public static void Register()
    {
        T.Case("security: the instance file is owner-only from creation and always holds a fresh single-use launch URL", async () =>
        {
            string data = T.NewTempDir("instfile");
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                string lockFile = Path.Combine(data, "test.lock"), urlFile = Path.Combine(data, "test.url");
                using InstanceLock? instance = InstanceLock.TryAcquire(lockFile, urlFile);
                T.Require(instance is not null);
                instance!.KeepFresh(v.Security);
                if (!OperatingSystem.IsWindows())
                {
                    T.Eq(File.GetUnixFileMode(urlFile), UnixFileMode.UserRead | UnixFileMode.UserWrite);
                }
                // No temporary file left behind.
                T.Eq(Directory.GetFiles(data, "*.tmp").Length + Directory.GetFiles(data, ".*.tmp").Length, 0);
                InstanceEntry first = InstanceLock.Read(urlFile)!;
                T.Eq(first.Endpoint.AbsoluteUri, v.Security.Origin + "/launch");
                using HttpClient anon = Client(v, cookie: false);
                using (HttpResponseMessage ok = await TestHost.PostLaunchAsync(anon, first.Secret).ConfigureAwait(false))
                {
                    T.Eq(ok.StatusCode, HttpStatusCode.OK);
                    T.Check(ok.Headers.Contains("Set-Cookie"));
                }
                // Used once: the file now holds a different secret, and the old one is refused.
                T.Check(await WaitUntilAsync(() => InstanceLock.Read(urlFile) is InstanceEntry e && e.Secret != first.Secret).ConfigureAwait(false));
                InstanceEntry second = InstanceLock.Read(urlFile)!;
                using (HttpResponseMessage again = await TestHost.PostLaunchAsync(anon, first.Secret).ConfigureAwait(false))
                {
                    T.Eq(again.StatusCode, HttpStatusCode.Forbidden);
                }
                using (HttpResponseMessage next = await TestHost.PostLaunchAsync(anon, second.Secret).ConfigureAwait(false))
                {
                    T.Eq(next.StatusCode, HttpStatusCode.OK);
                }
                if (!OperatingSystem.IsWindows())
                {
                    T.Eq(File.GetUnixFileMode(urlFile), UnixFileMode.UserRead | UnixFileMode.UserWrite);
                }
                // A second start reads it and brings the window forward with that fresh secret.
                T.Check(InstanceLock.Read(urlFile)!.Secret != second.Secret);
                if (!OperatingSystem.IsWindows())
                {
                    // An instance file that others can read is not trusted by a second start.
                    File.SetUnixFileMode(urlFile, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead);
                    T.Check(InstanceLock.Read(urlFile) is null);
                }
            }
        });

        T.Case("security: PHI responses (pages, thumbnails, API) are sent no-store", async () =>
        {
            string data = T.NewTempDir("nostore");
            string dir = Path.Combine(data, "in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("NOSTORE^CT", "NST1"), 3, 32);
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                await v.Runtime.Import.Start(new Core.Import.ImportRequest(dir)).Completion.ConfigureAwait(false);
                StudyPage page = await v.Runtime.Store.QueryStudiesAsync(new StudyQuery { Text = "NST1" }).ConfigureAwait(false);
                IReadOnlyList<SeriesRow> series = await v.Runtime.Store.GetSeriesAsync(page.Rows[0].StudyInstanceUid).ConfigureAwait(false);
                using HttpClient c = Client(v, cookie: true);
                foreach (string path in new[] { "/api/thumb?series=" + v.Runtime.Ids.For(series[0].SeriesInstanceUid), "/api/health", "/" })
                {
                    using HttpResponseMessage r = await c.GetAsync(new Uri(path, UriKind.Relative)).ConfigureAwait(false);
                    T.Eq(r.StatusCode, HttpStatusCode.OK);
                    T.Check(r.Headers.CacheControl?.NoStore == true);
                    T.Check(r.Headers.CacheControl?.MaxAge is null || r.Headers.CacheControl.MaxAge == TimeSpan.Zero);
                }
                // Refusals too.
                using HttpClient anon = Client(v, cookie: false);
                using HttpResponseMessage refused = await anon.GetAsync(new Uri("/", UriKind.Relative)).ConfigureAwait(false);
                T.Check(refused.Headers.CacheControl?.NoStore == true);
            }
        });

        T.Case("cache: an evicted frame or file stays usable while leased and is released after the last lease", async () =>
        {
            using var env = new TestEnv("lease");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("LEASE^CT", "LEA1"), 4, 64);
            await env.ImportAsync(dir).ConfigureAwait(false);
            var paths = Directory.GetFiles(dir).Order(StringComparer.Ordinal).ToArray();
            var images = paths.Select(p => new SeriesImage(ImageInstance.OpenHeader(p), 0)).ToArray();
            // Budget below one frame: every new decode evicts the previous one.
            using var cache = new RenderCache(env.Scheduler, 1, 1);
            var spec = new RenderSpec { Width = 32, Height = 32, Gray = true };
            FrameLease first = await cache.GetFrameAsync(images[0], WorkPriority.Normal, CancellationToken.None).ConfigureAwait(false);
            using (FrameLease second = await cache.GetFrameAsync(images[1], WorkPriority.Normal, CancellationToken.None).ConfigureAwait(false))
            {
                T.Eq(cache.DecodedFrames, 1);
                // The first frame was evicted, but the lease keeps it alive.
                T.Eq(first.Frame.Render(spec).Length, 32 * 32);
                T.Eq(second.Frame.Render(spec).Length, 32 * 32);
            }
            long frameBytes = first.Frame.Info.DecodedBytes;
            TkFrame orphan = first.Frame;
            first.Dispose();
            first.Dispose();
            bool disposed = false;
            try
            {
                orphan.Render(spec);
            }
            catch (ObjectDisposedException)
            {
                disposed = true;
            }
            T.Check(disposed);
            // Full file handles: evicted while leased, still readable.
            using (FileLease f0 = cache.GetFullFile(images[0].Instance))
            {
                using FileLease f1 = cache.GetFullFile(images[1].Instance);
                T.Eq(cache.FullHandles, 1);
                T.Check(!f0.File.IsDisposed);
                T.Check(f0.File.PixelInfo is not null);
            }
            // Many threads rendering while others force evictions (room for less than one frame):
            // no disposed-frame failures.
            using var shared = new RenderCache(env.Scheduler, frameBytes / 2, 1);
            int failures = 0;
            var workers = Enumerable.Range(0, 8).Select(w => Task.Run(async () =>
            {
                for (int i = 0; i < 40; i++)
                {
                    try
                    {
                        using FrameLease l = await shared.GetFrameAsync(images[(w + i) % images.Length], WorkPriority.Normal, CancellationToken.None)
                            .ConfigureAwait(false);
                        l.Frame.Render(spec);
                    }
                    catch (ObjectDisposedException)
                    {
                        Interlocked.Increment(ref failures);
                    }
                }
            })).ToArray();
            await Task.WhenAll(workers).ConfigureAwait(false);
            T.Eq(failures, 0);
            foreach (SeriesImage i in images)
            {
                i.Instance.Dispose();
            }
        });

        T.Case("session: header handles are released on close; a study whose files are gone ends in a message, not Loading", async () =>
        {
            using var env = new TestEnv("handles");
            string dir = env.Dir("in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("HANDLE^CT", "HDL1"), 5, 32);
            await env.ImportAsync(dir).ConfigureAwait(false);
            string study = Gen.StudyOf(env, "HDL1");
            (ViewerSession s, FakeSink sink) = await env.OpenAsync(study, 64, 64).ConfigureAwait(false);
            await ImagingTests.FirstFrameAsync(sink).ConfigureAwait(false);
            ImageInstance[] instances = [.. s.Series.SelectMany(x => x.Images).Select(i => i.Instance).Distinct()];
            T.Eq(instances.Length, 5);
            long before = Tk.LiveHandles;
            s.Dispose();
            T.Check(instances.All(i => i.Header.IsDisposed));
            T.Check(Tk.LiveHandles <= before - 5);
            s.Dispose();

            // Copied files deleted behind the store's back: the viewer says so instead of "Loading...".
            string studyDir = env.Dir("in2");
            SampleData.WriteCtSeries(studyDir, SampleData.Patient("GONE^CT", "GON1"), 3, 32);
            await env.ImportAsync(studyDir).ConfigureAwait(false);
            string gone = Gen.StudyOf(env, "GON1");
            foreach (SeriesRow row in await env.Store.GetSeriesAsync(gone).ConfigureAwait(false))
            {
                foreach (InstanceRow i in await env.Store.GetInstancesAsync(row.SeriesInstanceUid).ConfigureAwait(false))
                {
                    File.Delete(i.FilePath);
                }
            }
            (ViewerSession s2, FakeSink sink2) = await env.OpenAsync(gone, 64, 64).ConfigureAwait(false);
            using (s2)
            {
                T.Check(s2.Loaded);
                T.Check(s2.LoadError is not null);
                ViewportStateMessage? st = null;
                for (int i = 0; i < 200 && st is null; i++)
                {
                    st = sink2.LastState(0);
                    await Task.Delay(10).ConfigureAwait(false);
                }
                T.Check(st?.Message is string m && !m.StartsWith("Loading", StringComparison.Ordinal));
            }

            // A failing background task is shown, not swallowed.
            (ViewerSession s3, _) = await env.OpenAsync(study, 64, 64).ConfigureAwait(false);
            using (s3)
            {
                int failures = s3.BackgroundFailures;
                s3.Observe(Task.FromException(new IOException("disk")), "saving the measurement");
                s3.Observe(Task.FromCanceled(new CancellationToken(true)), "nothing");
                T.Check(await WaitUntilAsync(() => s3.BackgroundFailures == failures + 1).ConfigureAwait(false));
                T.Eq(s3.BackgroundError, "Saving the measurement failed: file system error.");
            }
        });

        T.Case("delete: sessions of the study close and cached handles are released before the store delete; failures are audited", async () =>
        {
            string data = T.NewTempDir("delrel");
            string dir = Path.Combine(data, "in");
            SampleData.WriteCtSeries(dir, SampleData.Patient("DELREL^CT", "DRL1"), 4, 32);
            RunningViewer v = await TestHost.StartAsync(data).ConfigureAwait(false);
            await using (v.ConfigureAwait(false))
            {
                await v.Runtime.Import.Start(new Core.Import.ImportRequest(dir)).Completion.ConfigureAwait(false);
                StudyRow row = (await v.Runtime.Store.QueryStudiesAsync(new StudyQuery { Text = "DRL1" }).ConfigureAwait(false)).Rows[0];
                ViewerSession s = await v.Runtime.OpenStudyAsync(row.StudyInstanceUid).ConfigureAwait(false);
                T.Require(await WaitUntilAsync(() => s.Loaded && s.Series.Count > 0).ConfigureAwait(false));
                ImageInstance[] headers = [.. s.Series.SelectMany(x => x.Images).Select(i => i.Instance).Distinct()];
                using (FileLease _ = v.Runtime.Cache.GetFullFile(headers[0]))
                {
                }
                T.Check(v.Runtime.Cache.FullHandles >= 1);
                int removed = await v.Runtime.DeleteStudyAsync(row).ConfigureAwait(false);
                T.Eq(removed, 4);
                T.Check(v.Runtime.Sessions.Get(s.Id) is null);
                T.Check(headers.All(h => h.Header.IsDisposed));
                T.Eq(v.Runtime.Cache.FullHandles, 0);
                // A delete that fails is audited with outcome failure and a reason.
                StudyRow other = row;
                v.Runtime.Store.Dispose();
                bool threw = false;
                try
                {
                    await v.Runtime.DeleteStudyAsync(other).ConfigureAwait(false);
                }
                catch (ObjectDisposedException)
                {
                    threw = true;
                }
                T.Check(threw);
            }
            string[] lines = File.ReadAllLines(Path.Combine(data, "audit", "audit.jsonl"));
            // Each delete: the intent first, then its outcome.
            string[] all = [.. lines.Where(l => l.Contains("\"study.delete\"", StringComparison.Ordinal))];
            T.Eq(all.Length, 4);
            T.Check(all[0].Contains("\"outcome\":\"intent\"", StringComparison.Ordinal) && all[2].Contains("\"outcome\":\"intent\"", StringComparison.Ordinal));
            string[] deletes = [all[1], all[3]];
            T.Check(deletes[0].Contains("\"outcome\":\"success\"", StringComparison.Ordinal));
            T.Check(deletes[1].Contains("\"outcome\":\"failure\"", StringComparison.Ordinal) && deletes[1].Contains("\"reason\":", StringComparison.Ordinal));
            T.Check(lines.All(l => l.Contains("\"outcome\":", StringComparison.Ordinal)));
            T.Check(AuditLog.Verify(Path.Combine(data, "audit", "audit.jsonl"), AuditKeys.KeyFile).IsIntact);
        });
    }
}
