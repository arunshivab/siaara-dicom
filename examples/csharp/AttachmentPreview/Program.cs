// SIAARA example (C#): show a DICOM file that arrived as UNTRUSTED bytes (for example a mail
// attachment) - a caption from a few safe header fields and a small PNG / JPEG of the first
// frame, or a plain reason why there is no picture. Server side, from memory, no temporary file.
//     siaara-attachment-preview <empty work folder> [file.dcm]
// docs/GETTING_STARTED.md, "Showing a DICOM attachment safely (untrusted input)".
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Siaara;
using Siaara.Volumes;

namespace Siaara.Examples;

/// <summary>What the preview ended with.</summary>
internal enum PreviewOutcome
{
    Ok,            // there is a picture
    NotDicom,      // the bytes are not a DICOM Part 10 file
    NoPixelData,   // DICOM without an image: SR, RT Structure Set, encapsulated PDF, DICOMDIR ...
    Unsupported,   // pixel data the toolkit cannot decode: video, JPEG XL ... (Reason names it)
    TooLarge,      // a limit was reached: nothing was decoded (Reason names it)
    Damaged,       // DICOM, but the file or its pixel data cannot be read
}

/// <summary>The limits of a preview. Every one protects against something (see Recommended).</summary>
internal sealed record PreviewLimits
{
    public int MaxAttachmentBytes { get; init; } = 64 << 20;      // everything below is proportional to the input at worst
    public int MaxBox { get; init; } = 512;                       // the picture fits MaxBox x MaxBox, whatever Rows / Columns claim
    public int MaxStripFrames { get; init; } = 8;                 // multi-frame: this many frames at most are ever decoded
    public int StripBox { get; init; } = 128;
    public int MaxDepth { get; init; } = 16;                      // deeply nested sequences
    public long MaxElements { get; init; } = 200_000;             // millions of tiny elements or empty items
    public long MaxInflatedSize { get; init; } = 128L << 20;      // a deflate bomb
    public long MaxFrameBytes { get; init; } = 64L << 20;         // huge Rows x Columns
    public long FrameMemoryBudget { get; init; } = 256L << 20;    // a codestream that claims huge dimensions

    public static PreviewLimits Recommended { get; } = new();

    // [limits] Every limit explicitly: nothing is left to defaults meant for trusted local files.
    public DicomReadOptions ToReadOptions(bool headerOnly = false) => new()
    {
        AllowRawDataset = false,          // Part 10 only
        Strict = false,                   // lenient: real-world files are rarely perfect ...
        RecoverOverlongLengths = false,   // ... but no guessing at damaged lengths
        StopBeforePixelData = headerOnly,
        MaxDepth = MaxDepth,
        MaxElements = MaxElements,
        MaxInflatedSize = MaxInflatedSize,
        MaxFrameBytes = MaxFrameBytes,           // these two belong to the opened file:
        FrameMemoryBudget = FrameMemoryBudget,   // every decode and render on it is held to them
    };
    // [limits]
}

/// <summary>An RGBA8 picture, rows top to bottom.</summary>
internal sealed record PreviewImage(int Width, int Height, byte[] Rgba)
{
    public byte[] ToPng() => ImageEncoder.EncodePng(Rgba, Width, Height);
    public byte[] ToJpeg() => ImageEncoder.EncodeJpeg(Rgba, Width, Height);
}

/// <summary>The preview of one attachment. No patient identifiers. The caption fields are text from the
/// file: escape them before they go into HTML.</summary>
internal sealed record Preview
{
    public PreviewOutcome Outcome { get; init; }
    public string Reason { get; init; } = "";
    /// <summary>The read was not complete (truncated, bytes skipped ...): what could be decoded is still shown.</summary>
    public bool Incomplete { get; init; }
    public DicomReadLoss ReadLoss { get; init; }
    public string Modality { get; init; } = "";
    public string StudyDate { get; init; } = "";
    public string SeriesDescription { get; init; } = "";
    public string TransferSyntaxUid { get; init; } = "";
    public string TransferSyntaxName { get; init; } = "";
    public int Rows { get; init; }
    public int Columns { get; init; }
    public int Frames { get; init; }
    public PreviewImage? Image { get; init; }   // frame 0
    public PreviewImage? Strip { get; init; }   // multi-frame: StripFrames frames side by side
    public int StripFrames { get; init; }
}

internal static class AttachmentPreview
{
    /// <summary>The preview of untrusted bytes. Never throws for bad input and never needs more than the limits
    /// allow, whatever the bytes claim. There is no time limit in the API: the limits bound the work; run it on a
    /// worker you can abandon.</summary>
    public static Preview Create(ReadOnlySpan<byte> data, PreviewLimits limits)
    {
        if (data.Length > limits.MaxAttachmentBytes)
        {
            return new Preview { Outcome = PreviewOutcome.TooLarge, Reason = "the attachment is larger than the preview limit" };
        }
        // A DICOM file starts with 128 bytes of preamble and "DICM"; nothing else is parsed.
        if (data.Length < 132 || !data.Slice(128, 4).SequenceEqual("DICM"u8))
        {
            return new Preview { Outcome = PreviewOutcome.NotDicom, Reason = "not a DICOM file" };
        }
        DicomFile file;
        try
        {
            // [open] From memory, with the limits.
            file = DicomFile.Open(data, limits.ToReadOptions());
            // [open]
        }
        catch (SiaaraException e)
        {
            return new Preview { Outcome = Classify(e, opening: true), Reason = e.NativeMessage };
        }
        using (file)
        {
            // An incomplete read is not an error here: what was read is previewed and flagged.
            DicomReadReport report = file.ReadReport;
            // [caption] Safe header fields only.
            DicomDataset ds = file.Dataset;
            string ts = file.TransferSyntaxUid;
            var preview = new Preview
            {
                Incomplete = !report.IsComplete,
                ReadLoss = report.Loss,
                Modality = Text(ds, DicomTags.Modality),
                StudyDate = Text(ds, DicomTags.StudyDate),
                SeriesDescription = Text(ds, DicomTags.SeriesDescription),
                TransferSyntaxUid = ts,
                TransferSyntaxName = TransferSyntaxInfo.Get(ts)?.Name ?? "",
            };
            // [caption]
            try
            {
                // [pixels] Is there an image, and can it be decoded? Nothing is decoded yet.
                DicomPixelInfo pi = file.GetPixelInfo();
                // [pixels]
                preview = preview with { Rows = pi.Rows, Columns = pi.Columns, Frames = pi.NumberOfFrames };
                if (pi.Rows <= 0 || pi.Columns <= 0 || pi.FrameBytes <= 0 || pi.FrameBytes > limits.MaxFrameBytes)
                {
                    return preview with { Outcome = PreviewOutcome.TooLarge, Reason = "the image is larger than the preview limit" };
                }
                // [render] Frame 0 at a bounded size, with the file's own VOI / presentation defaults.
                PreviewImage image = Render(file, 0, pi, limits.MaxBox);
                // [render]
                preview = preview with { Outcome = PreviewOutcome.Ok, Image = image };
                // [strip] Multi-frame / cine: a few frames spread over the run, never all of them.
                if (pi.NumberOfFrames > 1 && limits.MaxStripFrames > 1)
                {
                    preview = WithStrip(preview, file, pi, limits);
                }
                // [strip]
                return preview;
            }
            catch (SiaaraException e)
            {
                return preview with { Outcome = Classify(e, opening: false), Reason = e.ErrorCode == SiaaraErrorCode.NotFound ? "no image in this DICOM object" : e.NativeMessage };
            }
        }
    }

    /// <summary>Patient's Name and Patient ID, only when the caller asks (personal data: decide who may see them
    /// first). Opens the bytes again, header only, with the same limits.</summary>
    public static (string Name, string Id)? PatientFields(ReadOnlySpan<byte> data, PreviewLimits limits)
    {
        if (data.Length > limits.MaxAttachmentBytes || data.Length < 132 || !data.Slice(128, 4).SequenceEqual("DICM"u8))
        {
            return null;
        }
        try
        {
            using DicomFile file = DicomFile.Open(data, limits.ToReadOptions(headerOnly: true));
            return (Text(file.Dataset, DicomTags.PatientName), Text(file.Dataset, DicomTags.PatientID));
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    private static PreviewOutcome Classify(SiaaraException e, bool opening) => e.ErrorCode switch
    {
        SiaaraErrorCode.NotDicom => PreviewOutcome.NotDicom,
        SiaaraErrorCode.LimitExceeded => PreviewOutcome.TooLarge,
        SiaaraErrorCode.NotFound when !opening => PreviewOutcome.NoPixelData,
        SiaaraErrorCode.Unsupported when !opening => PreviewOutcome.Unsupported,
        _ => PreviewOutcome.Damaged,
    };

    /// <summary>A text attribute, control characters replaced; "" when absent.</summary>
    private static string Text(DicomDataset ds, DicomTag tag)
    {
        if (!ds.TryGetString(tag, out string? value))
        {
            return "";
        }
        var sb = new StringBuilder(value.Length);
        foreach (char c in value)
        {
            sb.Append(char.IsControl(c) ? ' ' : c);
        }
        return sb.Length > 256 ? sb.ToString(0, 256) : sb.ToString();
    }

    /// <summary>The size that fits box x box and keeps the aspect ratio (never larger than the image).</summary>
    private static (int Width, int Height) Fit(int columns, int rows, int box)
    {
        double scale = Math.Min(1.0, Math.Min((double)box / columns, (double)box / rows));
        return (Math.Max(1, (int)Math.Round(columns * scale)), Math.Max(1, (int)Math.Round(rows * scale)));
    }

    /// <summary>Decodes one frame (within the file's limits) and renders it into the fitted size.</summary>
    private static PreviewImage Render(DicomFile file, int index, DicomPixelInfo pi, int box)
    {
        (int w, int h) = Fit(pi.Columns, pi.Rows, box);
        using DicomFrame frame = file.OpenFrame(index);
        byte[] rgba = frame.Render(new ViewportOptions
        {
            Width = w,
            Height = h,
            Format = PixelOutputFormat.Rgba8,
            Zoom = 0,                                    // fit the viewport
            Interpolation = Interpolation.Linear,
            VoiMode = VoiMode.FromFile,                  // the file's window / VOI LUT, palette, MONOCHROME1 ...
            Background = new Rgba8Color(0, 0, 0, 255),
        });
        return new PreviewImage(w, h, rgba);
    }

    private static Preview WithStrip(Preview preview, DicomFile file, DicomPixelInfo pi, PreviewLimits limits)
    {
        int tiles = Math.Min(pi.NumberOfFrames, limits.MaxStripFrames);
        var done = new List<PreviewImage>(tiles);
        for (int t = 0; t < tiles; t++)
        {
            try
            {
                done.Add(Render(file, (int)((long)t * pi.NumberOfFrames / tiles), pi, limits.StripBox));
            }
            catch (SiaaraException)
            {
                break;  // a frame that cannot be decoded ends the strip; the main picture stays
            }
        }
        if (done.Count == 0)
        {
            return preview;
        }
        int tw = done[0].Width, th = done[0].Height;
        var strip = new byte[tw * tiles * th * 4];  // tiles not reached stay transparent
        for (int t = 0; t < done.Count; t++)
        {
            for (int y = 0; y < th; y++)
            {
                Buffer.BlockCopy(done[t].Rgba, y * tw * 4, strip, (y * tw * tiles + t * tw) * 4, tw * 4);
            }
        }
        return preview with { Strip = new PreviewImage(tw * tiles, th, strip), StripFrames = done.Count };
    }
}

internal static class Program
{
    private static int Main(string[] args)
    {
        string work = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "siaara-attachment-preview");
        Directory.CreateDirectory(work);
        Console.WriteLine($"SIAARA {SiaaraLibrary.Version}");
        PreviewLimits limits = PreviewLimits.Recommended;
        if (args.Length > 1)
        {
            Show(args[1], File.ReadAllBytes(args[1]), limits, Path.Combine(work, "preview"));
            return 0;
        }

        bool bad = false;
        // 1. An image: caption and picture.
        byte[] image = SampleImage(Path.Combine(work, "sample.dcm"));
        bad |= Show("image", image, limits, Path.Combine(work, "preview")).Outcome != PreviewOutcome.Ok;
        // Patient identifiers are a separate, deliberate call.
        (string Name, string Id)? patient = AttachmentPreview.PatientFields(image, limits);
        Console.WriteLine($"  (on request) patient: {patient?.Name}, {patient?.Id}");
        bad |= patient is null;
        // 2. The same file cut off in the middle of its pixel data: flagged, never shown as whole.
        Preview cut = Show("cut-off image", image.AsSpan(0, image.Length - 3), limits, Path.Combine(work, "cut"));
        bad |= cut.Outcome == PreviewOutcome.NotDicom || !cut.Incomplete;
        // 3. Bytes that are not DICOM, whatever the attachment's name says.
        byte[] text = Encoding.ASCII.GetBytes("Dear colleague, please find the images attached. Regards".PadRight(256));
        bad |= Show("text named .dcm", text, limits, "unused").Outcome != PreviewOutcome.NotDicom;
        // 4. DICOM without an image.
        byte[] report = SampleReport(Path.Combine(work, "report.dcm"));
        bad |= Show("structured report", report, limits, "unused").Outcome != PreviewOutcome.NoPixelData;
        // 5. Limits at work: a frame larger than the caller allows is refused before anything is decoded.
        var tight = limits with { MaxFrameBytes = 4 };
        bad |= Show("image, 4-byte frame limit", image, tight, "unused").Outcome != PreviewOutcome.TooLarge;
        if (bad)
        {
            Console.Error.WriteLine("attachment preview: an outcome was not the expected one");
        }
        return bad ? 1 : 0;
    }

    /// <summary>Previews one attachment, prints what a mail client would show and writes the pictures.</summary>
    private static Preview Show(string label, ReadOnlySpan<byte> data, PreviewLimits limits, string outPrefix)
    {
        Preview p = AttachmentPreview.Create(data, limits);
        Console.WriteLine($"{label} ({data.Length} bytes): {p.Outcome}{(p.Outcome == PreviewOutcome.Ok ? "" : " - " + p.Reason)}");
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
