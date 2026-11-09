using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Siaara.AotSample;

/// <summary>Exercises the bindings end to end (native calls, callbacks, SafeHandles) under NativeAOT.</summary>
internal static class Program
{
    private static int Main()
    {
        Console.WriteLine("SIAARA " + SiaaraLibrary.Version + " (ABI " + SiaaraLibrary.AbiVersion + ") from " + SiaaraLibrary.NativeLibraryPath);
        string dir = Path.Combine(Path.GetTempPath(), "siaara-aot-" + Environment.ProcessId);
        Directory.CreateDirectory(dir);
        try
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetRawString(DicomTags.SOPClassUID, "UI", DicomSopClasses.CTImageStorage);
            ds.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.4242");
            ds.SetRawString(DicomTags.StudyInstanceUID, "UI", "2.25.4243");
            ds.SetRawString(DicomTags.SeriesInstanceUID, "UI", "2.25.4244");
            ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 192");
            ds.SetString(DicomTags.PatientName, "Ünal^Ayşe");
            ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
            ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
            ds.SetUInt16(DicomTags.Rows, 2);
            ds.SetUInt16(DicomTags.Columns, 2);
            ds.SetUInt16(DicomTags.BitsAllocated, 16);
            ds.SetUInt16(DicomTags.BitsStored, 16);
            ds.SetUInt16(DicomTags.HighBit, 15);
            ds.SetUInt16(DicomTags.PixelRepresentation, 0);
            ds.SetBytes(DicomTags.PixelData, "OW", MemoryMarshal.AsBytes(new ushort[] { 0, 100, 200, 300 }.AsSpan()));
            f.Transcode(DicomTransferSyntaxes.RLELossless);
            byte[] rgba = f.RenderRgba8(new RenderOptions { VoiMode = VoiMode.FullRange });
            string path = Path.Combine(dir, "a.dcm");
            f.Save(path);

            int events = 0;
            using (DicomStore store = DicomStore.Open(Path.Combine(dir, "store")))
            {
                store.Changed += (_, e) => events++;
                store.AddFile(path);
                var rows = store.Query(new DicomStoreQuery { Level = DicomLevel.Study, PatientName = "unal*" });
                Console.WriteLine($"store rows: {rows.Count}, events: {events}");
                if (rows.Count != 1 || events != 1)
                {
                    return 1;
                }
            }
            int scanned = FolderScanner.FindInstances(dir).Count;
            using DicomFile back = DicomFile.Open(path);
            string name = back.Dataset.GetString(DicomTags.PatientName);
            Console.WriteLine($"name: {name}, ts: {back.TransferSyntaxUid}, rgba[12]: {rgba[12]}, scanned: {scanned}");
            Console.WriteLine("json: " + back.Dataset.ToJson()[..Math.Min(60, back.Dataset.ToJson().Length)] + "...");
            bool ok = name == "Ünal^Ayşe" && rgba[12] == 255 && rgba[0] == 0 && scanned >= 1;
            Console.WriteLine(ok ? "AOT sample: OK" : "AOT sample: FAILED");
            return ok ? 0 : 1;
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }
}
