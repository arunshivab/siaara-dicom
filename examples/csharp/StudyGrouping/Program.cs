// SIAARA example (C#): a ZIP of DICOM files that arrived by mail, with a DICOMDIR, grouped into
// patient / study / series, each series ordered for scrolling, and the DICOMDIR set against what is
// in the ZIP. Server side, from memory: nothing is unpacked to disk.
// The work is done by the package: Siaara.DicomStudyGrouping. The grouping shows what the files
// CLAIM, not proof that they belong together. In a product, run it in the same separate worker
// process as the attachment preview (with a kill timer and a memory limit). Every text is text from
// outside: use the display-safe copy (Display) and DicomAttachmentPreview.HtmlEscape in a page.
//     siaara-study-grouping <empty work folder> [file.zip]
// docs/GETTING_STARTED.md, "Grouping a ZIP or a DICOMDIR (untrusted input)".
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using Siaara;

namespace Siaara.Examples;

internal static class Program
{
    private static int Main(string[] args)
    {
        string work = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "siaara-study-grouping");
        Directory.CreateDirectory(work);
        Console.WriteLine($"SIAARA {SiaaraLibrary.Version}");
        byte[] zip = args.Length > 1 ? File.ReadAllBytes(args[1]) : SampleZip(work);
        DicomGroupingResult r = Group(zip, DicomGroupingOptions.Recommended);
        if (args.Length > 1)
        {
            return 0;
        }
        // The sample: one patient, one study, one CT series of 4 slices in position order, a readme
        // that is not DICOM, and one slice the DICOMDIR lists that is not in the ZIP.
        bool ok = r.Patients.Count == 1 && r.Series.Count == 1 && r.Series[0].Instances.Count == 4;
        for (int i = 1; ok && i < r.Series[0].Instances.Count; i++)
        {
            ok = r.Series[0].Instances[i].Position > r.Series[0].Instances[i - 1].Position;
        }
        ok &= r.Issues.Count == 2;
        if (!ok)
        {
            Console.Error.WriteLine("study grouping: the result was not the expected one");
        }
        return ok ? 0 : 1;
    }

    private static DicomGroupingResult Group(byte[] zipBytes, DicomGroupingOptions options)
    {
        using var grouping = new DicomStudyGrouping(options);
        using (var zip = new ZipArchive(new MemoryStream(zipBytes), ZipArchiveMode.Read))
        {
            int members = 0;
            foreach (ZipArchiveEntry entry in zip.Entries)
            {
                // Cap what you pass, as you cap the attachment's size: members and the size of each.
                if (++members > options.MaxFiles || entry.Length > options.MaxFileBytes || entry.FullName.EndsWith('/'))
                {
                    continue;
                }
                byte[] data = Read(entry);
                if (Path.GetFileName(entry.FullName).Equals("DICOMDIR", StringComparison.OrdinalIgnoreCase))
                {
                    grouping.AddDicomdir(data);
                }
                else
                {
                    grouping.Add(entry.FullName, data);  // never throws for bad bytes
                }
            }
        }
        DicomGroupingResult r = grouping.Result;
        foreach (DicomGroupedPatient p in r.Patients)
        {
            Console.WriteLine($"patient {p.Index + 1} (identifiers not shown: {p.PatientId.Status})");
            foreach (DicomGroupedStudy st in p.Studies)
            {
                Console.WriteLine($"  study {st.StudyUid.Display} of {st.StudyDate.Display}");
                foreach (DicomGroupedSeries se in st.Series)
                {
                    Console.WriteLine($"    series {se.Modality.Display} \"{se.SeriesDescription.Display}\": {se.Instances.Count} images");
                    foreach (DicomGroupedInstance i in se.Instances)
                    {
                        string pos = i.Position?.ToString("F1", CultureInfo.InvariantCulture) ?? "-";
                        Console.WriteLine($"      {i.Name.Display,-40} position {pos,6}  instance {i.InstanceNumber}  ({i.SortKey})");
                    }
                }
            }
        }
        foreach (DicomGroupingIssue issue in r.Issues)
        {
            string what = issue.Kind == DicomGroupingIssueKind.Skipped ? $" {issue.SkipReason}" : "";
            Console.WriteLine($"  issue: {issue.Kind}{what} {issue.Name.Display ?? issue.Subject.Display}");
        }
        return r;
    }

    private static byte[] Read(ZipArchiveEntry entry)
    {
        using Stream s = entry.Open();
        using var m = new MemoryStream();
        s.CopyTo(m);
        return m.ToArray();
    }

    // A ZIP as a mail client might make it from a CD: a wrapping folder, the DICOMDIR, the slices
    // in no particular order with Instance Numbers that run against their positions, a readme -
    // and one slice left out.
    private static byte[] SampleZip(string work)
    {
        string src = Path.Combine(work, "src");
        string media = Path.Combine(work, "media");
        Directory.CreateDirectory(src);
        var files = new List<string>();
        for (int k = 0; k < 5; k++)
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.2");
            ds.SetString(DicomTags.SOPInstanceUID, "2.25.6611" + k);
            ds.SetString(DicomTags.StudyInstanceUID, "2.25.6610");
            ds.SetString(DicomTags.SeriesInstanceUID, "2.25.6612");
            ds.SetString(DicomTags.Modality, "CT");
            ds.SetString(DicomTags.StudyDate, "20260115");
            ds.SetString(DicomTags.SeriesDescription, "Chest 2.5 mm");
            ds.SetString(DicomTags.PatientName, "Example^Patient");
            ds.SetString(DicomTags.PatientID, "EX-0001");
            ds.SetString(DicomTags.InstanceNumber, (10 - k).ToString(CultureInfo.InvariantCulture));
            ds.SetString(DicomTags.ImagePositionPatient, "-100\\-100\\" + (2.5 * k).ToString(CultureInfo.InvariantCulture));
            ds.SetString(DicomTags.ImageOrientationPatient, "1\\0\\0\\0\\1\\0");
            ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
            ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
            ds.SetUInt16(DicomTags.Rows, 2);
            ds.SetUInt16(DicomTags.Columns, 2);
            ds.SetUInt16(DicomTags.BitsAllocated, 16);
            ds.SetUInt16(DicomTags.BitsStored, 12);
            ds.SetUInt16(DicomTags.HighBit, 11);
            ds.SetUInt16(DicomTags.PixelRepresentation, 0);
            ds.SetBytes(DicomTags.PixelData, "OW", new byte[8]);
            string p = Path.Combine(src, "slice" + k + ".dcm");
            f.Save(p, "1.2.840.10008.1.2.1");
            files.Add(p);
        }
        if (Directory.Exists(media))
        {
            Directory.Delete(media, recursive: true);
        }
        Dicomdir.Write(media, files);
        byte[] dicomdir = File.ReadAllBytes(Path.Combine(media, "DICOMDIR"));
        var listed = new List<string>();
        foreach (DicomdirRecord rec in Dicomdir.ReadAll(dicomdir))
        {
            if (rec.FileId is not null)
            {
                listed.Add(rec.FileId);
            }
        }
        using var buffer = new MemoryStream();
        using (var zip = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
        {
            Add(zip, "PATIENT CD/DICOMDIR", dicomdir);
            foreach (int k in new[] { 3, 0, 2, 1 })  // listed[4] is left out
            {
                Add(zip, "PATIENT CD/" + listed[k], File.ReadAllBytes(Path.Combine(media, listed[k])));
            }
            Add(zip, "PATIENT CD/README.TXT", System.Text.Encoding.ASCII.GetBytes("Open DICOMDIR with any viewer."));
        }
        return buffer.ToArray();
    }

    private static void Add(ZipArchive zip, string name, byte[] data)
    {
        using Stream s = zip.CreateEntry(name).Open();
        s.Write(data);
    }
}
