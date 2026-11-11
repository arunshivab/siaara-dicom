// SIAARA example (C#): show a DICOM file that arrived as UNTRUSTED bytes (for example a mail
// attachment) - a caption from a few safe header fields and a small PNG / JPEG of the first
// frame, or a plain reason why there is no picture. Server side, from memory, no temporary file.
// The work is done by the package: Siaara.DicomAttachmentPreview.Create(bytes, options).
// The limits bound memory and the amount of work, not time: a decode has no deadline and cannot be
// cancelled. In a product, run this in a SEPARATE PROCESS with a kill timer and a memory limit (this
// program, started once per attachment, is such a worker). Caption text is text from the file:
// DicomAttachmentPreview.HtmlEscape before it goes into a page.
//     siaara-attachment-preview <empty work folder> [file.dcm]
// docs/GETTING_STARTED.md, "Showing a DICOM attachment safely (untrusted input)".
using System;
using System.IO;
using System.Text;
using Siaara;

namespace Siaara.Examples;

internal static class Program
{
    private static int Main(string[] args)
    {
        string work = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "siaara-attachment-preview");
        Directory.CreateDirectory(work);
        Console.WriteLine($"SIAARA {SiaaraLibrary.Version}");
        DicomPreviewOptions limits = DicomPreviewOptions.Recommended;
        if (args.Length > 1)
        {
            Show(args[1], File.ReadAllBytes(args[1]), limits, Path.Combine(work, "preview"));
            return 0;
        }

        bool bad = false;
        // 1. An image: caption and picture.
        byte[] image = SampleImage(Path.Combine(work, "sample.dcm"));
        bad |= Show("image", image, limits, Path.Combine(work, "preview")).Outcome != DicomPreviewOutcome.Ok;
        // Patient identifiers are a separate, deliberate call.
        (string Name, string Id)? patient = DicomAttachmentPreview.ReadPatientFields(image, limits);
        Console.WriteLine($"  (on request) patient: {patient?.Name}, {patient?.Id}");
        bad |= patient is null;
        // 2. The same file cut off in the middle of its pixel data: flagged, never shown as whole.
        DicomPreview cut = Show("cut-off image", image.AsSpan(0, image.Length - 3), limits, Path.Combine(work, "cut"));
        bad |= cut.Outcome == DicomPreviewOutcome.NotDicom || !cut.Incomplete;
        // 3. Bytes that are not DICOM, whatever the attachment's name says.
        byte[] text = Encoding.ASCII.GetBytes("Dear colleague, please find the images attached. Regards".PadRight(256));
        bad |= Show("text named .dcm", text, limits, "unused").Outcome != DicomPreviewOutcome.NotDicom;
        // 4. DICOM without an image.
        byte[] report = SampleReport(Path.Combine(work, "report.dcm"));
        bad |= Show("structured report", report, limits, "unused").Outcome != DicomPreviewOutcome.NoPixelData;
        // 5. Limits at work: a frame larger than the caller allows is refused before anything is decoded.
        var tight = limits with { MaxFrameBytes = 4 };
        bad |= Show("image, 4-byte frame limit", image, tight, "unused").Outcome != DicomPreviewOutcome.TooLarge;
        if (bad)
        {
            Console.Error.WriteLine("attachment preview: an outcome was not the expected one");
        }
        return bad ? 1 : 0;
    }

    /// <summary>Previews one attachment, prints what a mail client would show and writes the pictures.</summary>
    private static DicomPreview Show(string label, ReadOnlySpan<byte> data, DicomPreviewOptions limits, string outPrefix)
    {
        DicomPreview p = DicomAttachmentPreview.Create(data, limits);
        Console.WriteLine($"{label} ({data.Length} bytes): {p.Outcome}{(p.Outcome == DicomPreviewOutcome.Ok ? "" : " - " + p.Reason)}");
        if (p.TransferSyntaxUid.Length != 0)
        {
            string size = p.Rows > 0 ? $", {p.Columns} x {p.Rows}, {p.Frames} frame(s)" : "";
            Console.WriteLine($"  caption: {(p.Modality.Length != 0 ? p.Modality : "?")}{(p.StudyDate.Length != 0 ? ", " + p.StudyDate : "")}" +
                              $"{(p.SeriesDescription.Length != 0 ? ", " + p.SeriesDescription : "")}{size}, " +
                              $"{(p.TransferSyntaxName.Length != 0 ? p.TransferSyntaxName : p.TransferSyntaxUid)}");
        }
        if (p.Incomplete)
        {
            Console.WriteLine($"  note: the file is incomplete ({p.ReadLoss})" + (p.Image is null ? "" : "; the picture shows what could be read"));
        }
        if (p.Image is not null)
        {
            // [encode] PNG (lossless) and JPEG of the RGBA8 picture.
            byte[] png = p.Image.ToPng();
            byte[] jpeg = p.Image.ToJpeg();
            // [encode]
            File.WriteAllBytes(outPrefix + ".png", png);
            File.WriteAllBytes(outPrefix + ".jpg", jpeg);
            Console.WriteLine($"  picture: {p.Image.Width} x {p.Image.Height} pixels, PNG {png.Length} bytes, JPEG {jpeg.Length} bytes");
            if (p.Strip is not null)
            {
                File.WriteAllBytes(outPrefix + "-strip.png", p.Strip.ToPng());
                Console.WriteLine($"  strip: {p.StripFrames} frame(s), {p.Strip.Width} x {p.Strip.Height} pixels");
            }
        }
        return p;
    }

    /// <summary>A 3-frame, 64 x 48, 12-bit image, as attachment bytes.</summary>
    private static byte[] SampleImage(string path)
    {
        using DicomFile file = DicomFile.Create();
        DicomDataset ds = file.Dataset;
        ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.7.3");  // Multi-frame Grayscale Word SC
        ds.SetString(DicomTags.SOPInstanceUID, "2.25.329800735698586629295641978511506172930");
        ds.SetString(DicomTags.StudyInstanceUID, "2.25.329800735698586629295641978511506172916");
        ds.SetString(DicomTags.SeriesInstanceUID, "2.25.329800735698586629295641978511506172931");
        ds.SetString(DicomTags.Modality, "OT");
        ds.SetString(DicomTags.StudyDate, "20260115");
        ds.SetString(DicomTags.SeriesDescription, "Example series");
        ds.SetString(DicomTags.PatientName, "Example^Patient");
        ds.SetString(DicomTags.PatientID, "EX-0001");
        ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
        ds.SetString(DicomTags.NumberOfFrames, "3");
        ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
        ds.SetUInt16(DicomTags.Rows, 48);
        ds.SetUInt16(DicomTags.Columns, 64);
        ds.SetUInt16(DicomTags.BitsAllocated, 16);
        ds.SetUInt16(DicomTags.BitsStored, 12);
        ds.SetUInt16(DicomTags.HighBit, 11);
        ds.SetUInt16(DicomTags.PixelRepresentation, 0);
        var pixels = new byte[3 * 48 * 64 * 2];
        for (int f = 0; f < 3; f++)
        {
            for (int y = 0; y < 48; y++)
            {
                for (int x = 0; x < 64; x++)
                {
                    int v = (x * 60 + y * 5 + f * 400) & 0x0FFF;  // a ramp that moves from frame to frame
                    int at = ((f * 48 + y) * 64 + x) * 2;
                    pixels[at] = (byte)(v & 0xFF);
                    pixels[at + 1] = (byte)(v >> 8);
                }
            }
        }
        ds.SetBytes(DicomTags.PixelData, "OW", pixels);
        file.Save(path, "1.2.840.10008.1.2.1");
        return File.ReadAllBytes(path);
    }

    /// <summary>A DICOM object without an image (a bare Structured Report header).</summary>
    private static byte[] SampleReport(string path)
    {
        using DicomFile file = DicomFile.Create();
        DicomDataset ds = file.Dataset;
        ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.88.11");  // Basic Text SR
        ds.SetString(DicomTags.SOPInstanceUID, "2.25.329800735698586629295641978511506172932");
        ds.SetString(DicomTags.StudyInstanceUID, "2.25.329800735698586629295641978511506172916");
        ds.SetString(DicomTags.SeriesInstanceUID, "2.25.329800735698586629295641978511506172933");
        ds.SetString(DicomTags.Modality, "SR");
        ds.SetString(DicomTags.StudyDate, "20260115");
        file.Save(path, "1.2.840.10008.1.2.1");
        return File.ReadAllBytes(path);
    }
}
