using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>DicomStudyGrouping: the grouping of files that arrived together.</summary>
internal static class GroupingTests
{
    private static byte[] Bytes(DicomFile f, string name)
    {
        string path = Path.Combine(TempRoot, name);
        f.Save(path, DicomTransferSyntaxes.ExplicitVRLittleEndian);
        return File.ReadAllBytes(path);
    }

    private static string Ds(params double[] v) => string.Join('\\', v.Select(x => x.ToString("R", CultureInfo.InvariantCulture)));

    private static byte[] Slice(string sop, string series, int instance, double z, string patientId = "P1")
    {
        using DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, sop, series, "2.25.300", "Doe^Jane", patientId);
        f.Dataset.SetString(DicomTags.InstanceNumber, instance.ToString(CultureInfo.InvariantCulture));
        f.Dataset.SetString(DicomTags.ImagePositionPatient, Ds(5, -3, z));
        f.Dataset.SetString(DicomTags.ImageOrientationPatient, Ds(1, 0, 0, 0, 1, 0));
        return Bytes(f, sop + ".dcm");
    }

    internal static void Register()
    {
        Case("grouping: series ordered by position, bad bytes recorded, patient fields only when asked, names kept exactly", () =>
        {
            using var g = new DicomStudyGrouping();
            // Instance Numbers contradict the positions; shuffled.
            int[] order = { 3, 0, 4, 1, 2 };
            foreach (int k in order)
            {
                g.Add("zip/CT/IM" + k, Slice("2.25.40" + k, "2.25.60", 10 - k, 2.5 * k));
            }
            g.Add("readme.txt", Encoding.ASCII.GetBytes("not dicom"));
            g.Add("empty", ReadOnlySpan<byte>.Empty);
            g.Add("cut\u202Etxt.dcm\uD800", Slice("2.25.499", "2.25.61", 1, 0).AsSpan(0, 140));
            DicomGroupingResult r = g.Result;
            CheckEq(r.Patients.Count, 1);
            CheckEq(r.Series.Count, 1);
            DicomGroupedSeries s = r.Series[0];
            CheckEq(s.Instances.Count, 5);
            for (int i = 0; i < 5; i++)
            {
                CheckEq(s.Instances[i].SopInstanceUid.Stored, "2.25.40" + i);
                CheckEq(s.Instances[i].SortKey, DicomGroupedSortKey.Position);
                CheckEq(s.Instances[i].InstanceNumber, 10L - i);
                CheckNear(s.Instances[i].Position ?? double.NaN, 2.5 * i, 1e-9);
            }
            Check(s.Normal is not null);
            CheckEq(s.SeriesUid.Stored, "2.25.60");
            CheckEq(s.Modality.Stored, "CT");
            CheckEq(r.Patients[0].PatientId.Status, DicomGroupedTextStatus.NotRequested);
            CheckEq(s.Instances[0].PatientName.Stored, null);
            CheckEq(r.Issues.Count, 3);
            Check(r.Issues.All(i => i.Kind == DicomGroupingIssueKind.Skipped));
            // the cut file is a Part 10 header with nothing readable after it: skipped, not ungrouped
            CheckEq(r.Issues[2].SkipReason, DicomGroupingSkipReason.Unreadable);
            CheckEq(r.Ungrouped.Count, 0);
            CheckEq(s.OrderRule, DicomSeriesOrderRule.Position);
            CheckEq(r.Issues[0].Name.Stored, "readme.txt");
            CheckEq(r.Issues[0].SkipReason, DicomGroupingSkipReason.NotDicom);
            // the name exactly as given (even the lone surrogate); the display copy without the override
            DicomGroupingIssue cut = r.Issues[2];
            CheckEq(cut.Name.Stored, "cut\u202Etxt.dcm\uD800");
            CheckEq(cut.Name.Display, "cuttxt.dcm\uFFFD");
            CheckEq(r.Files, 8L);
            Check(ReferenceEquals(r, g.Result));  // built once until the next Add

            var o = new DicomGroupingOptions { IncludePatientFields = true, Order = DicomGroupingOrder.InstanceNumber };
            using var h = new DicomStudyGrouping(o);
            foreach (int k in order)
            {
                h.Add("IM" + k, Slice("2.25.40" + k, "2.25.60", 10 - k, 2.5 * k));
            }
            DicomGroupingResult q = h.Result;
            CheckEq(q.Patients[0].PatientId.Stored, "P1");
            CheckEq(q.Patients[0].PatientName.Stored, "Doe^Jane");
            CheckEq(q.Series[0].Instances[0].SopInstanceUid.Stored, "2.25.404");
            CheckEq(q.Series[0].Instances[0].SortKey, DicomGroupedSortKey.InstanceNumber);
        });

        Case("grouping: duplicates, conflicts, a DICOMDIR against what arrived", () =>
        {
            string src = NewTempDir("gsrc");
            var files = new List<string>();
            for (int i = 0; i < 3; i++)
            {
                string p = Path.Combine(src, "i" + i + ".dcm");
                File.WriteAllBytes(p, Slice("2.25.80" + i, "2.25.81", i + 1, i));
                files.Add(p);
            }
            string root = NewTempDir("gmedia");
            Dicomdir.Write(root, files);
            byte[] dd = File.ReadAllBytes(Path.Combine(root, "DICOMDIR"));
            string[] ids = Dicomdir.ReadAll(dd).Where(x => x.FileId is not null).Select(x => x.FileId!).ToArray();
            Require(ids.Length == 3);
            using var g = new DicomStudyGrouping(new DicomGroupingOptions { Duplicates = DicomDuplicatePolicy.KeepFirst });
            g.AddDicomdir(dd);
            g.Add("CD/" + ids[0].ToLowerInvariant(), File.ReadAllBytes(Path.Combine(root, ids[0])));
            g.Add("CD\\" + ids[1].Replace('/', '\\'), File.ReadAllBytes(Path.Combine(root, ids[1])));
            g.Add("again", File.ReadAllBytes(Path.Combine(root, ids[1])));
            g.Add("other", Slice("2.25.899", "2.25.81", 9, 9, patientId: "P2"));
            DicomGroupingResult r = g.Result;
            CheckEq(r.Instances.Count, 3);
            Check(r.Instances.Where(i => i.Name.Stored!.StartsWith("CD", StringComparison.Ordinal)).All(i => i.InDicomdir));
            var missing = r.Issues.Where(i => i.Kind == DicomGroupingIssueKind.ListedMissing).ToList();
            CheckEq(missing.Count, 1);
            CheckEq(missing[0].Subject.Stored, ids[2]);
            var dup = r.Issues.Single(i => i.Kind == DicomGroupingIssueKind.Duplicate);
            CheckEq(dup.Name.Stored, "again");
            CheckEq(dup.Instance, -1);
            CheckEq(r.Issues.Count(i => i.Kind == DicomGroupingIssueKind.PresentUnlisted), 1);
            CheckEq(r.Issues.Count(i => i.Kind == DicomGroupingIssueKind.StudyPatientConflict), 2);
            CheckEq(r.Patients.Count, 2);
            g.AddDicomdir(new byte[] { 1, 2, 3 });
            Check(g.Result.Issues.Any(i => i.Kind == DicomGroupingIssueKind.DicomdirSkipped && i.SkipReason == DicomGroupingSkipReason.NotDicom));
        });

        Case("grouping: options - a zero or negative limit throws; budgets; Dispose", () =>
        {
            Throws<ArgumentOutOfRangeException>(() => new DicomStudyGrouping(new DicomGroupingOptions { MaxFiles = 0 }).Dispose());
            Throws<ArgumentOutOfRangeException>(() => new DicomStudyGrouping(new DicomGroupingOptions { MaxResultBytes = -1 }).Dispose());
            Throws<ArgumentOutOfRangeException>(() => new DicomStudyGrouping(new DicomGroupingOptions { OrientationTolerance = 0 }).Dispose());
            Throws<ArgumentOutOfRangeException>(() => new DicomStudyGrouping(new DicomGroupingOptions { OrientationTolerance = double.NaN }).Dispose());
            Throws<ArgumentException>(() => new DicomStudyGrouping(new DicomGroupingOptions { Order = (DicomGroupingOrder)9 }).Dispose());
            Throws<ArgumentException>(() => new DicomStudyGrouping(new DicomGroupingOptions { TieBreak = (DicomTieBreak)9 }).Dispose());
            Throws<ArgumentOutOfRangeException>(() => new DicomStudyGrouping(new DicomGroupingOptions { MaxDicomdirs = 0 }).Dispose());
            Throws<ArgumentOutOfRangeException>(() => new DicomStudyGrouping(new DicomGroupingOptions { MaxDicomdirs = 33 }).Dispose());
            CheckEq(DicomGroupingOptions.Recommended.PositionFallback, DicomPositionFallback.PerSeries);
            CheckEq(DicomGroupingOptions.Recommended.UidValidation, DicomUidValidation.Strict);
            CheckEq(DicomGroupingOptions.DefaultMaxDicomdirs, 4);
            CheckEq(DicomGroupingOptions.DefaultMaxTotalBytes, 2L << 30);
            CheckEq(DicomGroupingOptions.DefaultMaxTextBytes, 1024);
            using var g = new DicomStudyGrouping(new DicomGroupingOptions { MaxFiles = 2 });
            for (int i = 0; i < 3; i++)
            {
                g.Add("f" + i, Slice("2.25.90" + i, "2.25.91", i, i));
            }
            DicomGroupingResult r = g.Result;
            CheckEq(r.Instances.Count, 2);
            CheckEq(r.BudgetReached, DicomGroupingLimit.MaxFiles);
            CheckEq(r.Issues[0].Limit, DicomGroupingLimit.MaxFiles);
            CheckEq(r.Issues[0].SkipReason, DicomGroupingSkipReason.TotalBudget);
            Throws<ArgumentNullException>(() => g.Add(null!, ReadOnlySpan<byte>.Empty));
            g.Dispose();
            Throws<ObjectDisposedException>(() => g.Add("x", ReadOnlySpan<byte>.Empty));
            Throws<ObjectDisposedException>(() => _ = g.Result);
        });

        Case("grouping: per-series fallback, unsafe names, no Patient ID per study, a File ID listed by two DICOMDIRs", () =>
        {
            using var g = new DicomStudyGrouping();
            g.Add("a", Slice("2.25.71", "2.25.70", 3, 10));
            using (DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, "2.25.72", "2.25.70", "2.25.300"))
            {
                f.Dataset.SetString(DicomTags.InstanceNumber, "1");
                g.Add("../b", Bytes(f, "nopos.dcm"));  // no position: the series goes by Instance Number
            }
            DicomGroupingResult r = g.Result;
            CheckEq(r.Series[0].OrderRule, DicomSeriesOrderRule.InstanceNumber);
            CheckEq(r.Series[0].Instances[0].Name.Stored, "../b");
            Check(r.Series[0].Instances[0].HasUnsafeName);
            CheckEq(r.UnsafeNames, 1L);
            CheckEq(r.Issues.Single().Kind, DicomGroupingIssueKind.UnsafeName);

            using var p = new DicomStudyGrouping(new DicomGroupingOptions { PositionFallback = DicomPositionFallback.PerInstance });
            p.Add("a", Slice("2.25.71", "2.25.70", 3, 10));
            using (DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, "2.25.72", "2.25.70", "2.25.300"))
            {
                f.Dataset.SetString(DicomTags.InstanceNumber, "1");
                p.Add("b", Bytes(f, "nopos2.dcm"));
            }
            CheckEq(p.Result.Series[0].OrderRule, DicomSeriesOrderRule.Mixed);
            CheckEq(p.Result.Series[0].Instances[0].Name.Stored, "a");

            using var n = new DicomStudyGrouping();
            foreach (string study in new[] { "2.25.81", "2.25.82" })
            {
                using DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, study + ".1", study + ".2", study);
                f.Dataset.Remove(DicomTags.PatientID);
                n.Add(study, Bytes(f, study + ".dcm"));
            }
            CheckEq(n.Result.Patients.Count, 2);
            Check(n.Result.Patients.All(x => x.NoPatientId));

            string src = NewTempDir("g2src");
            string pth = Path.Combine(src, "i.dcm");
            File.WriteAllBytes(pth, Slice("2.25.91", "2.25.92", 1, 0));
            string root = NewTempDir("g2media");
            Dicomdir.Write(root, new[] { pth });
            byte[] dd = File.ReadAllBytes(Path.Combine(root, "DICOMDIR"));
            using var two = new DicomStudyGrouping();
            two.AddDicomdir(dd);
            two.AddDicomdir(dd);
            DicomGroupingIssue missing = two.Result.Issues.Single(i => i.Kind == DicomGroupingIssueKind.ListedMissing);
            CheckEq(string.Join(",", missing.ListedBy), "0,1");
            using var damaged = new DicomStudyGrouping();
            damaged.AddDicomdir(dd.AsSpan(0, dd.Length - 10));
            DicomGroupingIssue d = damaged.Result.Issues.Single(i => i.Kind == DicomGroupingIssueKind.DicomdirDamaged);
            CheckEq(d.DamagedReason, DicomdirDamagedReason.Incomplete);
            Check(!string.IsNullOrEmpty(d.Subject.Display));
        });

        Case("grouping: series order in a study, as added (default) or by Series Number", () =>
        {
            byte[] Series(string uid, string? number)
            {
                using DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, uid + ".1", uid, "2.25.300");
                if (number is null)
                {
                    f.Dataset.Remove(DicomTags.SeriesNumber);
                }
                else
                {
                    f.Dataset.SetString(DicomTags.SeriesNumber, number);
                }
                return Bytes(f, uid + ".dcm");
            }
            var data = new[] { Series("2.25.31", "5"), Series("2.25.32", null), Series("2.25.33", "1"), Series("2.25.34", "x") };
            using var asAdded = new DicomStudyGrouping();
            using var byNumber = new DicomStudyGrouping(new DicomGroupingOptions { SeriesOrder = DicomGroupingSeriesOrder.SeriesNumber });
            for (int i = 0; i < data.Length; i++)
            {
                asAdded.Add("f" + i, data[i]);
                byNumber.Add("f" + i, data[i]);
            }
            CheckEq(string.Join(",", asAdded.Result.Studies[0].Series.Select(x => x.SeriesUid.Stored)), "2.25.31,2.25.32,2.25.33,2.25.34");
            CheckEq(string.Join(",", byNumber.Result.Studies[0].Series.Select(x => x.SeriesUid.Stored)), "2.25.33,2.25.31,2.25.32,2.25.34");
            CheckEq(DicomGroupingOptions.Recommended.SeriesOrder, DicomGroupingSeriesOrder.AsAdded);
            Throws<ArgumentException>(() => new DicomStudyGrouping(new DicomGroupingOptions { SeriesOrder = (DicomGroupingSeriesOrder)7 }).Dispose());
        });
    }
}
