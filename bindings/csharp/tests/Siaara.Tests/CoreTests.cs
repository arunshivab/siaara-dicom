using System;
using System.IO;
using System.Linq;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class CoreTests
{
    internal static void Register()
    {
        Case("core: library loads from runtimes/<rid>/native and ABI matches", () =>
        {
            CheckEq(SiaaraLibrary.AbiVersion, SiaaraLibrary.ExpectedAbiVersion);
            Check(SiaaraLibrary.Version.Length > 0);
            string? p = SiaaraLibrary.NativeLibraryPath;
            Require(p is not null);
            Check(p!.Contains(Path.Combine("runtimes", SiaaraLibrary.RuntimeIdentifier, "native"), StringComparison.Ordinal));
            Check(Path.IsPathRooted(p));
        });

        Case("core: tags and dictionary", () =>
        {
            CheckEq(DicomTags.PatientName.Value, 0x00100010u);
            CheckEq(DicomTags.PatientName.ToString(), "(0010,0010)");
            CheckEq(DicomTag.Parse("(0028,0010)"), DicomTags.Rows);
            CheckEq(DicomTag.Parse("00280011"), DicomTags.Columns);
            CheckEq(DicomTag.Parse("PixelData"), DicomTags.PixelData);
            Check(!DicomTag.TryParse("NoSuchKeyword", out _));
            Check(new DicomTag(0x0009, 0x0010).IsPrivate);
            Check(DicomTags.PatientID < DicomTags.PatientName == false);
            DicomDictionaryEntry e = DicomDictionary.Lookup(DicomTags.PatientName);
            CheckEq(e.Keyword, "PatientName");
            CheckEq(e.VR, "PN");
            CheckEq(e.Name, "Patient's Name");
            CheckEq(DicomDictionary.Lookup("Rows").Tag, DicomTags.Rows);
            DicomDictionaryEntry px = DicomDictionary.Lookup(DicomTags.PixelData);
            CheckEq(px.VR, "OB");
            CheckEq(px.AlternateVR, "OW");
            Check(!DicomDictionary.TryLookup(new DicomTag(0x00091234), out _));
            CheckEq(DicomTags.PatientName.Keyword, "PatientName");
            CheckEq(DicomTransferSyntaxes.ExplicitVRLittleEndian, "1.2.840.10008.1.2.1");
            CheckEq(DicomSopClasses.CTImageStorage, "1.2.840.10008.5.1.4.1.1.2");
        });

        Case("core: create, save, open round trip (capi_smoke equivalent)", () =>
        {
            string dir = NewTempDir("roundtrip");
            string path = Path.Combine(dir, "a.dcm");
            using (DicomFile f = DicomFile.Create())
            {
                DicomDataset ds = f.Dataset;
                ds.SetRawString(DicomTags.SOPClassUID, "UI", Builders.SecondaryCapture);
                ds.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.42");
                ds.SetString(DicomTags.PatientName, "Doe^Jane");
                ds.SetUInt16(DicomTags.Rows, 512);
                ds.SetUInt32(DicomTags.SimpleFrameList, 7);
                ds.SetBytes(new DicomTag(0x00091010), "OB", new byte[] { 1, 2, 3, 4 });
                f.Save(path, DicomTransferSyntaxes.ImplicitVRLittleEndian);
            }
            using (DicomFile f = DicomFile.Open(path))
            {
                DicomDataset ds = f.Dataset;
                CheckEq(f.TransferSyntaxUid, DicomTransferSyntaxes.ImplicitVRLittleEndian);
                CheckEq(ds.GetString(DicomTags.PatientName), "Doe^Jane");
                CheckEq(ds[DicomTags.PatientName], "Doe^Jane");
                CheckEq(ds.GetInt64(DicomTags.Rows), 512L);
                CheckEq(ds.GetInt32(DicomTags.SimpleFrameList), 7);
                CheckEq(ds.GetVR(DicomTags.PatientName), "PN");
                Check(ds.Contains(DicomTags.Rows));
                Check(!ds.Contains(DicomTags.Columns));
                Check(!ds.TryGetInt64(DicomTags.Columns, out _));
                var ex = Throws<SiaaraException>(() => ds.GetInt64(DicomTags.Columns));
                CheckEq(ex?.ErrorCode, SiaaraErrorCode.NotFound);
                Check(ex is not null && ex.NativeMessage.Length > 0);
                CheckEq(ds[DicomTags.Columns], null);
                CheckEq(ds.GetBytes(new DicomTag(0x00091010)).Length, 4);
                Span<byte> small = stackalloc byte[2];
                CheckEq(ds.CopyBytes(new DicomTag(0x00091010), small), 4);
                Span<byte> big = stackalloc byte[8];
                CheckEq(ds.CopyBytes(new DicomTag(0x00091010), big), 4);
                CheckEq(big[3], (byte)4);
                var tags = ds.Tags;
                CheckEq(tags.Count, ds.Count);
                Check(tags.SequenceEqual(tags.OrderBy(t => t)));
                Check(f.FileMetaInformation.Contains(DicomTags.TransferSyntaxUID));
                CheckEq(f.FileMetaInformation.GetRawString(DicomTags.TransferSyntaxUID), DicomTransferSyntaxes.ImplicitVRLittleEndian);
                Check(ds.Remove(DicomTags.Rows));
                Check(!ds.Remove(DicomTags.Rows));
                ds[DicomTags.StudyDescription] = "Brain";
                CheckEq(ds.GetString(DicomTags.StudyDescription), "Brain");
                ds[DicomTags.StudyDescription] = null;
                Check(!ds.Contains(DicomTags.StudyDescription));
            }
            byte[] bytes = File.ReadAllBytes(path);
            using (DicomFile m = DicomFile.Open(bytes))
            {
                CheckEq(m.Dataset.GetString(DicomTags.PatientName), "Doe^Jane");
            }
            using (DicomFile h = DicomFile.Open(path, new DicomReadOptions { StopBeforePixelData = true, Strict = true }))
            {
                CheckEq(h.Dataset.GetRawString(DicomTags.SOPInstanceUID), "2.25.42");
            }
        });

        Case("core: multi-valued strings, numbers, errors", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.ImageType, "ORIGINAL\\PRIMARY\\AXIAL");
            CheckEq(ds.GetValueMultiplicity(DicomTags.ImageType), 3);
            Check(ds.GetStrings(DicomTags.ImageType).SequenceEqual(new[] { "ORIGINAL", "PRIMARY", "AXIAL" }));
            CheckEq(ds[DicomTags.ImageType], "ORIGINAL\\PRIMARY\\AXIAL");
            CheckEq(ds.GetString(DicomTags.ImageType, 2), "AXIAL");
            ds.SetString(DicomTags.PixelSpacing, "0.5\\0.25");
            CheckNear(ds.GetDouble(DicomTags.PixelSpacing, 1), 0.25, 1e-12);
            Check(ds.TryGetDouble(DicomTags.PixelSpacing, out double d0) && d0 == 0.5);
            string longText = new string('x', 5000);
            ds.SetString(DicomTags.ImageComments, "LT", longText);
            CheckEq(ds.GetString(DicomTags.ImageComments), longText);
            Throws<ArgumentException>(() => ds.SetString(DicomTags.PatientName, "pn", "x"));
            CheckEq(Throws<SiaaraException>(() => DicomFile.Open(Path.Combine(TempRoot, "missing.dcm")))?.ErrorCode, SiaaraErrorCode.IO);
            byte[] truncated = new byte[132 + 6];
            "DICM"u8.CopyTo(truncated.AsSpan(128));
            truncated[132] = 0x02; // (0002,0000 ... cut off
            Check(Throws<SiaaraException>(() => DicomFile.Open(truncated, new DicomReadOptions { Strict = true })) is not null);
        });

        Case("core: sequences and item navigation", () =>
        {
            const string json = "{\"00081115\":{\"vr\":\"SQ\",\"Value\":[" +
                "{\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"1.2.3\"]},\"00081140\":{\"vr\":\"SQ\",\"Value\":[{\"00081155\":{\"vr\":\"UI\",\"Value\":[\"9.8.7\"]}}]}}," +
                "{\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"4.5.6\"]}}]}}";
            using DicomFile f = DicomFile.FromJson(json);
            DicomDataset ds = f.Dataset;
            CheckEq(ds.GetItemCount(DicomTags.ReferencedSeriesSequence), 2);
            var items = ds.GetSequence(DicomTags.ReferencedSeriesSequence);
            CheckEq(items.Count, 2);
            CheckEq(items[1].GetRawString(DicomTags.SeriesInstanceUID), "4.5.6");
            CheckEq(items[0].Parent, ds);
            CheckEq(items[0].ItemIndex, 0);
            DicomDataset inner = items[0].GetItem(DicomTags.ReferencedImageSequence, 0);
            CheckEq(inner.GetString(DicomTags.ReferencedSOPInstanceUID), "9.8.7");
            // changing the enclosing data set does not invalidate item objects
            ds.SetString(DicomTags.PatientName, "A^B");
            ds.SetString(DicomTags.AccessionNumber, "ACC1");
            CheckEq(inner.GetString(DicomTags.ReferencedSOPInstanceUID), "9.8.7");
            CheckEq(ds.GetSequence(DicomTags.ReferencedImageSequence).Count, 0);
            CheckEq(Throws<SiaaraException>(() => ds.GetItem(DicomTags.ReferencedSeriesSequence, 5))?.ErrorCode, SiaaraErrorCode.NotFound);
            CheckEq(Throws<SiaaraException>(() => ds.GetItemCount(DicomTags.PatientName))?.ErrorCode, SiaaraErrorCode.TypeMismatch);
        });

        Case("core: use after dispose throws ObjectDisposedException", () =>
        {
            DicomFile f = Builders.Smoke();
            DicomDataset ds = f.Dataset;
            f.Dispose();
            f.Dispose();
            Check(f.IsDisposed);
            Throws<ObjectDisposedException>(() => ds.GetString(DicomTags.PatientName));
            Throws<ObjectDisposedException>(() => _ = f.FrameCount);
        });
    }
}
