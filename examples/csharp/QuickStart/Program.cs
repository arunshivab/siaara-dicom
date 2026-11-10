// SIAARA example (C#): write and read a file, read Patient's Name by keyword and by path, read a
// pixel value, then C-ECHO / C-STORE to an SCP started here and a QIDO-RS query against the
// DICOMweb server over the same store. Everything runs on the loopback interface.
//     siaara-quickstart <empty work folder>
using System;
using System.IO;
using Siaara;
using Siaara.Network;
using Siaara.Web;

namespace Siaara.Examples;

internal static class Program
{
    private static int Main(string[] args)
    {
        string work = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "siaara-quickstart");
        if (Directory.Exists(work))
        {
            Directory.Delete(work, recursive: true);
        }
        Directory.CreateDirectory(work);
        string sample = Path.Combine(work, "sample.dcm");
        Console.WriteLine($"SIAARA {SiaaraLibrary.Version}");

        WriteSample(sample);

        // [read] Open a file; Patient's Name by keyword, by tag and by path; a pixel value.
        using (DicomFile file = DicomFile.Open(sample))
        {
            DicomDataset ds = file.Dataset;
            Console.WriteLine($"PatientName = {ds.GetString(DicomTag.Parse("PatientName"))}");
            Console.WriteLine($"PatientID   = {ds.GetString(DicomTags.PatientID)}");
            DicomPathValue? byPath = ds.GetByPath("PatientName");
            Console.WriteLine($"by path: VR {byPath?.VR}, text {byPath?.Text}");
            PixelValue pixel = file.GetPixelValue(frame: 0, column: 1, row: 0);
            Console.WriteLine($"pixel (1,0): stored {pixel.Stored}, modality value {pixel.ModalityValue}");
        }
        // [read]

        // [scp] A Storage SCP (Verification is always on) over a local store.
        using DicomStore store = DicomStore.Open(Path.Combine(work, "store"));
        using StorageScp scp = StorageScp.Start(
            new StorageScpOptions
            {
                Port = 0,                                   // ephemeral; a real archive names its port
                BindAddress = "127.0.0.1",
                AeTitle = "EXAMPLE_SCP",
                AllowedCallingAeTitles = new[] { "EXAMPLE_SCU" },
            },
            store);
        Console.WriteLine($"SCP EXAMPLE_SCP listening on 127.0.0.1:{scp.Port}");
        // [scp]

        // [echo-store] C-ECHO, then C-STORE of a file.
        var peer = new DimsePeer
        {
            Host = "127.0.0.1",
            Port = scp.Port,
            CallingAeTitle = "EXAMPLE_SCU",
            CalledAeTitle = "EXAMPLE_SCP",
        };
        DimseResult echo = Dimse.Echo(peer);
        Console.WriteLine($"C-ECHO status 0x{echo.DimseStatus:X4}");
        DimseResult stored = Dimse.StoreFiles(peer, new[] { sample });
        Console.WriteLine($"C-STORE status 0x{stored.DimseStatus:X4}, {stored.Completed} stored, {stored.Failed} failed");
        // [echo-store]

        // [dicomweb] DICOMweb over the same store, then a QIDO-RS query.
        using DicomWebServer web = DicomWebServer.Start(new DicomWebServerOptions { Port = 0 }, store);
        using var client = new DicomWebClient($"http://127.0.0.1:{web.Port}/dicomweb");
        string json = client.Search(QidoLevel.Studies, "PatientID=EX-0001");
        Console.WriteLine($"QIDO-RS studies?PatientID=EX-0001 ->\n{json}");
        // [dicomweb]

        if (echo.DimseStatus != 0 || stored.Completed != 1 || !json.Contains("EX-0001", StringComparison.Ordinal))
        {
            Console.Error.WriteLine("the example did not get the expected results");
            return 1;
        }
        return 0;
    }

    // A 2 x 2 signed 16-bit CT-like image, so the example needs no data.
    private static void WriteSample(string path)
    {
        using DicomFile file = DicomFile.Create();
        DicomDataset ds = file.Dataset;
        ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.7");
        ds.SetString(DicomTags.SOPInstanceUID, "2.25.329800735698586629295641978511506172918");
        ds.SetString(DicomTags.StudyInstanceUID, "2.25.329800735698586629295641978511506172916");
        ds.SetString(DicomTags.SeriesInstanceUID, "2.25.329800735698586629295641978511506172917");
        ds.SetString(DicomTags.Modality, "OT");
        ds.SetString(DicomTags.PatientName, "Example^Patient");
        ds.SetString(DicomTags.PatientID, "EX-0001");
        ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
        ds.SetString(DicomTags.RescaleIntercept, "-1024");
        ds.SetString(DicomTags.RescaleSlope, "1");
        ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
        ds.SetUInt16(DicomTags.Rows, 2);
        ds.SetUInt16(DicomTags.Columns, 2);
        ds.SetUInt16(DicomTags.BitsAllocated, 16);
        ds.SetUInt16(DicomTags.BitsStored, 16);
        ds.SetUInt16(DicomTags.HighBit, 15);
        ds.SetUInt16(DicomTags.PixelRepresentation, 1);
        ds.SetBytes(DicomTags.PixelData, "OW", new byte[] { 0x00, 0x04, 0x32, 0x04, 0xCE, 0x03, 0xE8, 0x07 });
        file.Save(path, "1.2.840.10008.1.2.1");
    }
}
