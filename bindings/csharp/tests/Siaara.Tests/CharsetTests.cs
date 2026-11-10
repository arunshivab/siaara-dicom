using System;
using System.Linq;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class CharsetTests
{
    internal static void Register()
    {
        Case("charset: Latin-1 (ISO_IR 100) encode on set, decode on get", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
            ds.SetString(DicomTags.PatientName, "Müller^Jürgen");
            byte[] raw = ds.GetBytes(DicomTags.PatientName);
            CheckEq(raw[1], (byte)0xFC); // ü in ISO 8859-1, not UTF-8
            CheckEq(ds.GetString(DicomTags.PatientName), "Müller^Jürgen");
            CheckEq(ds[DicomTags.PatientName], "Müller^Jürgen");
        });

        Case("charset: UTF-8 (ISO_IR 192), multi-byte text", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 192");
            const string name = "山田^太郎=やまだ^たろう";
            ds.SetString(DicomTags.PatientName, name);
            CheckEq(ds.GetString(DicomTags.PatientName), name);
            CheckEq(ds.GetBytes(DicomTags.PatientName).Length % 2, 0); // padded to even length
            ds.SetString(DicomTags.StudyDescription, "Ωμέγα");
            CheckEq(ds.GetString(DicomTags.StudyDescription), "Ωμέγα");
        });

        Case("charset: unrepresentable character fails and leaves the data set unchanged", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
            ds.SetString(DicomTags.PatientName, "Before");
            var e = Throws<SiaaraException>(() => ds.SetString(DicomTags.PatientName, "日本"));
            CheckEq(e?.ErrorCode, SiaaraErrorCode.Unsupported);
            CheckEq(ds.GetString(DicomTags.PatientName), "Before");
        });

        Case("charset: default repertoire and invalid bytes become U+FFFD", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetBytes(DicomTags.PatientName, "PN", new byte[] { (byte)'A', 0xFF, (byte)'B', (byte)' ' });
            string s = ds.GetString(DicomTags.PatientName);
            Check(s.StartsWith('A') && s.Contains('�', StringComparison.Ordinal) && s.EndsWith('B'));
        });

        Case("charset: sequence items inherit the enclosing data set's character set", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
            // Item text stored as Latin-1 bytes (as an encoder following PS3.5 would write it).
            const string json = "{\"00081115\":{\"vr\":\"SQ\",\"Value\":[{\"0008103E\":{\"vr\":\"LO\",\"Value\":[\"x\"]}}]}}";
            using DicomFile src = DicomFile.FromJson(json);
            DicomDataset srcItem = src.Dataset.GetItem(DicomTags.ReferencedSeriesSequence, 0);
            srcItem.SetBytes(DicomTags.SeriesDescription, "LO", new byte[] { (byte)'K', 0xF6, (byte)'r', (byte)'p' });
            // Without inheritance the item decodes with the default repertoire (U+FFFD).
            Check(srcItem.GetString(DicomTags.SeriesDescription).Contains('�', StringComparison.Ordinal));
            src.Dataset.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
            CheckEq(srcItem.GetString(DicomTags.SeriesDescription), "Körp");
            // Setting text on an item encodes with the inherited character set.
            srcItem.SetString(DicomTags.SeriesDescription, "Größe");
            Check(srcItem.GetBytes(DicomTags.SeriesDescription).Contains((byte)0xF6));
            CheckEq(srcItem.GetString(DicomTags.SeriesDescription), "Größe");
        });

        Case("charset: F15 items read from a file decode on their own (ISO_IR 192, ISO_IR 100 override)", () =>
        {
            string dir = NewTempDir("f15");
            string path = System.IO.Path.Combine(dir, "f15.dcm");
            using (DicomFile f = DicomFile.Create())
            {
                DicomDataset ds = f.Dataset;
                ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.7");
                ds.SetString(DicomTags.SOPInstanceUID, "2.25.1515");
                ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 192");
                DicomDataset a = ds.AppendItem(DicomTags.ReferencedSeriesSequence);
                a.SetString(DicomTags.SeriesDescription, "Ωμέγα");  // inherits UTF-8
                DicomDataset b = ds.AppendItem(DicomTags.ReferencedSeriesSequence);
                b.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");  // own value
                b.SetString(DicomTags.SeriesDescription, "Größe\\Körper");
                DicomDataset nested = b.AppendItem(DicomTags.ReferencedImageSequence);
                nested.SetString(DicomTags.SeriesDescription, "Müller");  // inherits Latin-1 from b
                CheckEq(nested.GetBytes(DicomTags.SeriesDescription)[1], (byte)0xFC);
                f.Save(path);
            }
            using DicomFile g = DicomFile.Open(path);
            DicomDataset ia = g.Dataset.GetItem(DicomTags.ReferencedSeriesSequence, 0);
            DicomDataset ib = g.Dataset.GetItem(DicomTags.ReferencedSeriesSequence, 1);
            DicomDataset inb = ib.GetItem(DicomTags.ReferencedImageSequence, 0);
            CheckEq(ia.GetString(DicomTags.SeriesDescription), "Ωμέγα");
            CheckEq(ib.GetString(DicomTags.SeriesDescription, 1), "Körper");
            CheckEq(string.Join("|", ib.GetStrings(DicomTags.SeriesDescription)), "Größe|Körper");
            CheckEq(inb.GetString(DicomTags.SeriesDescription), "Müller");
            CheckEq(string.Join("|", inb.GetStrings(DicomTags.SeriesDescription)), "Müller");
            // Removing the override: the nested item now inherits UTF-8, its Latin-1 byte is invalid.
            ib.Remove(DicomTags.SpecificCharacterSet);
            Check(inb.GetString(DicomTags.SeriesDescription).Contains('\uFFFD', StringComparison.Ordinal));
        });
    }
}
