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
    }
}
