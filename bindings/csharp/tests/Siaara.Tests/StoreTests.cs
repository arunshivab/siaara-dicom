using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class StoreTests
{
    private static byte[] Encode(DicomFile f, string dir, string name)
    {
        string p = Path.Combine(dir, name);
        f.Save(p, DicomTransferSyntaxes.ExplicitVRLittleEndian);
        return File.ReadAllBytes(p);
    }

    internal static void Register()
    {
        Case("store: add, query with filters, series, instances, events, delete, stats", () =>
        {
            string dir = NewTempDir("store");
            string src = NewTempDir("store-src");
            using DicomStore store = DicomStore.Open(Path.Combine(dir, "s"));
            var events = new List<StoreChangedEventArgs>();
            object? sender = null;
            store.Changed += (s, e) =>
            {
                sender = s;
                events.Add(e);
            };
            for (int i = 0; i < 3; i++)
            {
                using DicomFile f = Builders.Ct(2, 2, new short[4], "2.25.10" + i, i < 2 ? "2.25.20" : "2.25.21", "2.25.30",
                    "Muller^Anna", "PID1");
                f.Dataset.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
                f.Dataset.SetString(DicomTags.PatientName, "Müller^Anna");
                f.Dataset.SetString(DicomTags.InstanceNumber, (3 - i).ToString(System.Globalization.CultureInfo.InvariantCulture));
                f.Dataset.SetString(DicomTags.SeriesNumber, i < 2 ? "1" : "2");
                if (i == 0)
                {
                    string path = Path.Combine(src, "a.dcm");
                    f.Save(path);
                    CheckEq(store.AddFile(path), AddOutcome.Added);
                    CheckEq(store.AddFile(path), AddOutcome.KeptExisting);
                    CheckEq(store.AddFile(path, DuplicatePolicy.Replace), AddOutcome.Replaced);
                }
                else
                {
                    CheckEq(store.AddMemory(Encode(f, src, i + ".dcm")), AddOutcome.Added);
                }
            }
            using (DicomFile other = Builders.Ct(2, 2, new short[4], "2.25.900", "2.25.901", "2.25.902", "Smith^John", "PID2"))
            {
                other.Dataset.SetString(DicomTags.Modality, "MR");
                CheckEq(store.AddMemory(Encode(other, src, "o.dcm")), AddOutcome.Added);
            }
            CheckEq(events.Count, 5);
            CheckEq(events[0].Kind, StoreChangeKind.Added);
            CheckEq(events[1].Kind, StoreChangeKind.Replaced); // KeptExisting raises no event
            CheckEq(events[0].SopInstanceUid, "2.25.100");
            CheckEq(events[0].StudyInstanceUid, "2.25.30");
            Check(ReferenceEquals(sender, store));

            DicomStoreRecordCollection studies = store.Query(new DicomStoreQuery { Level = DicomLevel.Study });
            CheckEq(studies.Count, 2);
            CheckEq(studies.Total, 2);
            Check(studies.IsTotalExact);
            var mueller = store.Query(new DicomStoreQuery { Level = DicomLevel.Study, PatientName = "muller*" });
            CheckEq(mueller.Count, 1); // case- and accent-insensitive
            CheckEq(mueller[0].PatientName, "Müller^Anna");
            CheckEq(mueller[0].StudyInstanceUid, "2.25.30");
            CheckEq(mueller[0].NumberOfSeries, 2);
            CheckEq(mueller[0].NumberOfInstances, 3);
            Check(mueller[0].PatientKey is { Length: > 0 });
            CheckEq(store.Query(new DicomStoreQuery { Level = DicomLevel.Series, Modality = "MR\\PT" }).Count, 1);
            CheckEq(store.Query(new DicomStoreQuery { Level = DicomLevel.Study, StudyDate = "20260101-20261231" }).Count, 2);
            CheckEq(store.Query(new DicomStoreQuery { Level = DicomLevel.Study, StudyDate = "-20200101" }).Count, 0);
            var paged = store.Query(new DicomStoreQuery { Level = DicomLevel.Instance, Limit = 2 });
            CheckEq(paged.Count, 2);
            CheckEq(paged.Total, 4);
            var paged2 = store.Query(new DicomStoreQuery { Level = DicomLevel.Instance, Offset = 3, Limit = 2 });
            CheckEq(paged2.Count, 1);
            var noTotal = store.QueryAsync(new DicomStoreQuery { Level = DicomLevel.Instance, Limit = 1, NoTotal = true }).GetAwaiter().GetResult();
            CheckEq(noTotal.Count, 1);

            var series = store.GetSeries("2.25.30");
            CheckEq(series.Count, 2);
            CheckEq(series[0].SeriesInstanceUid, "2.25.20");
            CheckEq(series[0].SeriesNumber, 1);
            var inst = store.GetInstances("2.25.20");
            CheckEq(inst.Count, 2);
            CheckEq(inst[0].InstanceNumber, 2); // ordered by Instance Number
            Check(File.Exists(inst[0].FilePath));
            CheckEq(inst[0].Rows, 2);
            Check(inst[0].Flags.HasFlag(IndexRecordContents.PixelData));
            using (DicomFile back = DicomFile.Open(inst[0].FilePath))
            {
                CheckEq(back.Dataset.GetString(DicomTags.PatientName), "Müller^Anna");
            }

            DicomStoreStatistics st = store.GetStatistics();
            CheckEq(st.Patients, 2L);
            CheckEq(st.Studies, 2L);
            CheckEq(st.Instances, 4L);
            events.Clear();
            CheckEq(store.Delete(DicomLevel.Series, "2.25.21"), 1);
            CheckEq(events.Count, 1);
            CheckEq(events[0].Kind, StoreChangeKind.Deleted);
            CheckEq(store.Delete(DicomLevel.Patient, mueller[0].PatientKey!), 2);
            CheckEq(store.GetStatistics().Instances, 1L);
            DicomStoreVerifyReport v = store.Verify(deep: true);
            CheckEq(v.Checked, 1);
            CheckEq(v.Missing, 0);
            store.Compact();
            store.RebuildAsync().GetAwaiter().GetResult();
            Check(events.Any(e => e.Kind == StoreChangeKind.Rebuilt));
            CheckEq(store.GetStatistics().Instances, 1L);
        });

        Case("store: second open is rejected; move between stores; reopen keeps the index", () =>
        {
            string dir = NewTempDir("store2");
            string a = Path.Combine(dir, "a"), b = Path.Combine(dir, "b");
            using (DicomStore sa = DicomStore.Open(a, new DicomStoreOptions { Durable = true }))
            {
                CheckEq(Throws<SiaaraException>(() => DicomStore.Open(a).Dispose())?.ErrorCode, SiaaraErrorCode.Rejected);
                using DicomFile f = Builders.Smoke();
                sa.AddMemory(Encode(f, dir, "x.dcm"));
                using DicomStore sb = DicomStore.Open(b);
                int moved = 0;
                sb.Changed += (_, _) => moved++;
                CheckEq(sa.Move(sb, DicomLevel.Study, "2.25.300"), 1);
                CheckEq(sa.GetStatistics().Instances, 0L);
                CheckEq(sb.GetStatistics().Instances, 1L);
                CheckEq(moved, 1);
            }
            using (DicomStore sb2 = DicomStore.Open(b, new DicomStoreOptions { Create = false }))
            {
                CheckEq(sb2.GetStatistics().Instances, 1L);
            }
            CheckEq(Throws<SiaaraException>(() => DicomStore.Open(Path.Combine(dir, "none"), new DicomStoreOptions { Create = false }))?.ErrorCode is not null, true);
        });

        Case("store: exceptions in event handlers surface on the calling thread, not in native code", () =>
        {
            string dir = NewTempDir("store3");
            using DicomStore s = DicomStore.Open(Path.Combine(dir, "s"));
            s.Changed += (_, _) => throw new InvalidOperationException("handler failed");
            using DicomFile f = Builders.Smoke();
            byte[] data = Encode(f, dir, "x.dcm");
            var e = Throws<InvalidOperationException>(() => s.AddMemory(data));
            CheckEq(e?.Message, "handler failed");
            CheckEq(s.GetStatistics().Instances, 1L); // the change itself committed
        });

        Case("store: an undisposed store with a handler is finalized (no GCHandle cycle)", () =>
        {
            string root = Path.Combine(NewTempDir("store4"), "s");
            static WeakReference Make(string r)
            {
                DicomStore s = DicomStore.Open(r);
                s.Changed += (_, _) => { };
                return new WeakReference(s);
            }
            WeakReference w = Make(root);
            for (int i = 0; i < 5 && w.IsAlive; i++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            Check(!w.IsAlive);
            using DicomStore again = DicomStore.Open(root); // the lock was released by the finalizer
            CheckEq(again.GetStatistics().Instances, 0L);
        });

        Case("scan: folder scan with records, skipped files, cancellation, exceptions", () =>
        {
            string dir = NewTempDir("scan");
            Directory.CreateDirectory(Path.Combine(dir, "sub"));
            for (int i = 0; i < 5; i++)
            {
                using DicomFile f = Builders.Ct(2, 2, new short[4], "2.25.70" + i);
                f.Save(Path.Combine(dir, i < 3 ? "" : "sub", "f" + i + ".dcm"));
            }
            File.WriteAllText(Path.Combine(dir, "readme.txt"), "not dicom");
            var items = new List<FolderScanItem>();
            FolderScanStatistics st = FolderScanner.Scan(dir, items.Add, new FolderScanOptions { ReportSkipped = true, Threads = 2 });
            CheckEq(st.Instances, 5L);
            CheckEq(st.Skipped, 1L);
            CheckEq(items.Count(i => i.Kind == FolderScanItemKind.Instance), 5);
            CheckEq(items.Count(i => i.Kind == FolderScanItemKind.NotDicom), 1);
            Check(items.Where(i => i.Record is not null).All(i => i.Record!.SopInstanceUid.StartsWith("2.25.70", StringComparison.Ordinal)));
            CheckEq(FolderScanner.FindInstances(dir, new FolderScanOptions { Recursive = false }).Count, 3);
            using var cts = new CancellationTokenSource();
            int seen = 0;
            Throws<OperationCanceledException>(() => FolderScanner.Scan(dir, _ =>
            {
                if (++seen == 2)
                {
                    cts.Cancel();
                }
            }, null, cts.Token));
            Check(seen <= 3);
            var ex = Throws<FormatException>(() => FolderScanner.Scan(dir, _ => throw new FormatException("bad")));
            CheckEq(ex?.Message, "bad");
            CheckEq(FolderScanner.ScanAsync(dir, _ => { }).GetAwaiter().GetResult().Instances, 5L);
        });

        Case("dicomdir: write and read back", () =>
        {
            string src = NewTempDir("dd-src");
            var files = new List<string>();
            for (int i = 0; i < 3; i++)
            {
                using DicomFile f = Builders.Ct(2, 2, new short[4], "2.25.80" + i, "2.25.81", "2.25.82");
                f.Dataset.SetString(DicomTags.StudyTime, "101010");
                f.Dataset.SetString(DicomTags.StudyID, "S1");
                f.Dataset.SetString(DicomTags.AccessionNumber, "A1");
                f.Dataset.SetString(DicomTags.ReferringPhysicianName, "Ref^Doc");
                f.Dataset.SetString(DicomTags.SeriesNumber, "1");
                f.Dataset.SetString(DicomTags.InstanceNumber, (i + 1).ToString(System.Globalization.CultureInfo.InvariantCulture));
                f.Dataset.SetString(DicomTags.PatientBirthDate, "19700101");
                f.Dataset.SetString(DicomTags.PatientSex, "F");
                string p = Path.Combine(src, "i" + i + ".dcm");
                f.Save(p, DicomTransferSyntaxes.ExplicitVRLittleEndian);
                files.Add(p);
            }
            string root = NewTempDir("dd");
            int warnings = Dicomdir.WriteAsync(root, files, new DicomdirWriteOptions { FileSetId = "TEST1" }).GetAwaiter().GetResult();
            Check(warnings >= 0);
            string dd = Path.Combine(root, "DICOMDIR");
            Require(File.Exists(dd));
            var recs = Dicomdir.ReadAll(dd);
            CheckEq(recs.Count(r => r.Keys.GetString(IndexField.RecordType) == "IMAGE"), 3);
            CheckEq(recs.Count(r => r.Keys.GetString(IndexField.RecordType) == "PATIENT"), 1);
            DicomdirRecord img = recs.First(r => r.Keys.GetString(IndexField.RecordType) == "IMAGE");
            Check(File.Exists(img.Keys.FilePath));
            CheckEq(img.FileStatus, SiaaraErrorCode.Ok);
            CheckEq(img.Keys.PatientId, "P1"); // inherited key
            DicomdirSummary sum = Dicomdir.Read(dd, _ => { });
            CheckEq(sum.Files, 3);
            CheckEq(sum.MissingFiles, 0);
            FolderScanStatistics st = FolderScanner.Scan(root, _ => { }, new FolderScanOptions { UseDicomdir = true });
            Check(st.UsedDicomdir);
            CheckEq(st.Instances, 3L);
        });
    }
}
