// Preview of a DICOM file that arrived as UNTRUSTED bytes (a mail attachment, an upload): a caption
// from a few safe header fields and a small picture of the first frame, or a plain reason why there
// is no picture. From memory, no temporary file, every limit explicit.
// docs/GETTING_STARTED.md, "Showing a DICOM attachment safely (untrusted input)".
using System;
using System.Collections.Generic;
using System.Text;
using Siaara.Volumes;

namespace Siaara;

/// <summary>What the preview ended with.</summary>
public enum DicomPreviewOutcome
{
    /// <summary>There is a picture.</summary>
    Ok,
    /// <summary>The bytes are not a DICOM Part 10 file.</summary>
    NotDicom,
    /// <summary>DICOM without an image: SR, RT Structure Set, encapsulated PDF, DICOMDIR ...</summary>
    NoPixelData,
    /// <summary>Pixel data the toolkit cannot decode: video, JPEG XL ... (Reason names it).</summary>
    Unsupported,
    /// <summary>A limit was reached: nothing was decoded (Reason names it).</summary>
    TooLarge,
    /// <summary>DICOM, but the file or its pixel data cannot be read.</summary>
    Damaged,
}

/// <summary>The options of a preview: the limits (every one protects against something) and how the
/// picture is drawn. The defaults are the recommended values for untrusted input.</summary>
public sealed record DicomPreviewOptions
{
    /// <summary>Largest attachment looked at, in bytes. Everything below is proportional to the input at worst. Default 64 MiB.</summary>
    public int MaxAttachmentBytes { get; init; } = 64 << 20;
    /// <summary>The picture fits MaxBox x MaxBox pixels, whatever Rows / Columns claim. Default 512.</summary>
    public int MaxBox { get; init; } = 512;
    /// <summary>Multi-frame: this many frames at most are ever decoded for the strip; 0 or 1: no strip. Default 8.</summary>
    public int MaxStripFrames { get; init; } = 8;
    /// <summary>Each strip tile fits StripBox x StripBox pixels. Default 128.</summary>
    public int StripBox { get; init; } = 128;
    /// <summary>Deepest sequence nesting read. Default 16.</summary>
    public int MaxDepth { get; init; } = 16;
    /// <summary>Most elements and sequence items read (millions of tiny elements). Default 200,000.</summary>
    public long MaxElements { get; init; } = 200_000;
    /// <summary>Largest inflated size of a deflated data set (a deflate bomb). Default 128 MiB.</summary>
    public long MaxInflatedSize { get; init; } = 128L << 20;
    /// <summary>Largest decoded frame (huge Rows x Columns): a larger one is refused before anything is decoded. Default 64 MiB.</summary>
    public long MaxFrameBytes { get; init; } = 64L << 20;
    /// <summary>Memory a decoder may take for one frame (a codestream that claims huge dimensions). Default 256 MiB.</summary>
    public long FrameMemoryBudget { get; init; } = 256L << 20;
    /// <summary>Longest caption field, in characters; longer text is cut. Default 256.</summary>
    public int MaxCaptionLength { get; init; } = 256;
    /// <summary>How the picture is resampled to its size. Default linear.</summary>
    public Interpolation Interpolation { get; init; } = Interpolation.Linear;
    /// <summary>Window / VOI used for the picture. Default: the file's own (window, VOI LUT, palette, MONOCHROME1).</summary>
    public VoiMode VoiMode { get; init; } = VoiMode.FromFile;
    /// <summary>Colour behind the image. Default opaque black.</summary>
    public Rgba8Color Background { get; init; } = new Rgba8Color(0, 0, 0, 255);

    /// <summary>The recommended values for untrusted input (the defaults).</summary>
    public static DicomPreviewOptions Recommended { get; } = new();

    /// <summary>The read options these limits stand for. Every limit is set explicitly: nothing is left to
    /// defaults meant for trusted local files.</summary>
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
}

/// <summary>An RGBA8 picture, rows top to bottom.</summary>
/// <param name="Width">Width in pixels.</param>
/// <param name="Height">Height in pixels.</param>
/// <param name="Rgba">Width x Height x 4 bytes.</param>
public sealed record DicomPreviewImage(int Width, int Height, byte[] Rgba)
{
    /// <summary>The picture as a PNG file (lossless).</summary>
    public byte[] ToPng() => ImageEncoder.EncodePng(Rgba, Width, Height);
    /// <summary>The picture as a JPEG file.</summary>
    public byte[] ToJpeg() => ImageEncoder.EncodeJpeg(Rgba, Width, Height);
}

/// <summary>The preview of one attachment. No patient identifiers. The caption fields are text from the
/// file: escape them before they go into HTML.</summary>
public sealed record DicomPreview
{
    /// <summary>What the preview ended with.</summary>
    public DicomPreviewOutcome Outcome { get; init; }
    /// <summary>Why there is no picture; "" when there is one.</summary>
    public string Reason { get; init; } = "";
    /// <summary>The read was not complete (truncated, bytes skipped ...): what could be decoded is still shown.</summary>
    public bool Incomplete { get; init; }
    /// <summary>What the read lost, when Incomplete.</summary>
    public DicomReadLoss ReadLoss { get; init; }
    /// <summary>Modality (0008,0060).</summary>
    public string Modality { get; init; } = "";
    /// <summary>Study Date (0008,0020), as stored (YYYYMMDD).</summary>
    public string StudyDate { get; init; } = "";
    /// <summary>Series Description (0008,103E).</summary>
    public string SeriesDescription { get; init; } = "";
    /// <summary>Transfer Syntax UID of the file.</summary>
    public string TransferSyntaxUid { get; init; } = "";
    /// <summary>Its name; "" when the toolkit does not know it.</summary>
    public string TransferSyntaxName { get; init; } = "";
    /// <summary>Rows of the image; 0 without one.</summary>
    public int Rows { get; init; }
    /// <summary>Columns of the image; 0 without one.</summary>
    public int Columns { get; init; }
    /// <summary>Number of frames the file states.</summary>
    public int Frames { get; init; }
    /// <summary>Frame 0; null without a picture.</summary>
    public DicomPreviewImage? Image { get; init; }
    /// <summary>Multi-frame: StripFrames frames spread over the run, side by side; else null.</summary>
    public DicomPreviewImage? Strip { get; init; }
    /// <summary>Frames in the strip.</summary>
    public int StripFrames { get; init; }
}

/// <summary>Previews of DICOM files that arrived as untrusted bytes (mail attachments, uploads).</summary>
public static class DicomAttachmentPreview
{
    /// <summary>The preview of untrusted bytes. Never throws for bad input and never needs more than the limits
    /// allow, whatever the bytes claim. There is no time limit in the API: the limits bound the work; run it on a
    /// worker you can abandon.</summary>
    /// <param name="data">The attachment bytes. They are only read.</param>
    /// <param name="options">Limits and drawing options; null: <see cref="DicomPreviewOptions.Recommended"/>.</param>
    public static DicomPreview Create(ReadOnlySpan<byte> data, DicomPreviewOptions? options = null)
    {
        DicomPreviewOptions limits = options ?? DicomPreviewOptions.Recommended;
        if (data.Length > limits.MaxAttachmentBytes)
        {
            return new DicomPreview { Outcome = DicomPreviewOutcome.TooLarge, Reason = "the attachment is larger than the preview limit" };
        }
        // A DICOM file starts with 128 bytes of preamble and "DICM"; nothing else is parsed.
        if (data.Length < 132 || !data.Slice(128, 4).SequenceEqual("DICM"u8))
        {
            return new DicomPreview { Outcome = DicomPreviewOutcome.NotDicom, Reason = "not a DICOM file" };
        }
        try
        {
            // From memory, with the limits.
            using DicomFile file = DicomFile.Open(data, limits.ToReadOptions());
            return FromFile(file, limits);
        }
        catch (SiaaraException e)
        {
            return new DicomPreview { Outcome = Classify(e, opening: true), Reason = e.NativeMessage };
        }
    }

    private static DicomPreview FromFile(DicomFile file, DicomPreviewOptions limits)
    {
        {
            // An incomplete read is not an error here: what was read is previewed and flagged.
            DicomReadReport report = file.ReadReport;
            // Safe header fields only.
            DicomDataset ds = file.Dataset;
            string ts = file.TransferSyntaxUid;
            var preview = new DicomPreview
            {
                Incomplete = !report.IsComplete,
                ReadLoss = report.Loss,
                Modality = Text(ds, DicomTags.Modality, limits.MaxCaptionLength),
                StudyDate = Text(ds, DicomTags.StudyDate, limits.MaxCaptionLength),
                SeriesDescription = Text(ds, DicomTags.SeriesDescription, limits.MaxCaptionLength),
                TransferSyntaxUid = ts,
                TransferSyntaxName = TransferSyntaxInfo.Get(ts)?.Name ?? "",
            };
            try
            {
                // Is there an image, and can it be decoded? Nothing is decoded yet.
                DicomPixelInfo pi = file.GetPixelInfo();
                    preview = preview with { Rows = pi.Rows, Columns = pi.Columns, Frames = pi.NumberOfFrames };
                if (pi.Rows <= 0 || pi.Columns <= 0 || pi.FrameBytes <= 0 || pi.FrameBytes > limits.MaxFrameBytes)
                {
                    return preview with { Outcome = DicomPreviewOutcome.TooLarge, Reason = "the image is larger than the preview limit" };
                }
                // Frame 0 at a bounded size, with the file's own VOI / presentation defaults.
                DicomPreviewImage image = Render(file, 0, pi, limits.MaxBox, limits);
                    preview = preview with { Outcome = DicomPreviewOutcome.Ok, Image = image };
                // Multi-frame / cine: a few frames spread over the run, never all of them.
                if (pi.NumberOfFrames > 1 && limits.MaxStripFrames > 1)
                {
                    preview = WithStrip(preview, file, pi, limits);
                }
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
    /// <returns>Null when the bytes are not a readable DICOM file within the limits.</returns>
    public static (string Name, string Id)? ReadPatientFields(ReadOnlySpan<byte> data, DicomPreviewOptions? options = null)
    {
        DicomPreviewOptions limits = options ?? DicomPreviewOptions.Recommended;
        if (data.Length > limits.MaxAttachmentBytes || data.Length < 132 || !data.Slice(128, 4).SequenceEqual("DICM"u8))
        {
            return null;
        }
        try
        {
            using DicomFile file = DicomFile.Open(data, limits.ToReadOptions(headerOnly: true));
            return (Text(file.Dataset, DicomTags.PatientName, limits.MaxCaptionLength), Text(file.Dataset, DicomTags.PatientID, limits.MaxCaptionLength));
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    private static DicomPreviewOutcome Classify(SiaaraException e, bool opening) => e.ErrorCode switch
    {
        SiaaraErrorCode.NotDicom => DicomPreviewOutcome.NotDicom,
        SiaaraErrorCode.LimitExceeded => DicomPreviewOutcome.TooLarge,
        SiaaraErrorCode.NotFound when !opening => DicomPreviewOutcome.NoPixelData,
        SiaaraErrorCode.Unsupported when !opening => DicomPreviewOutcome.Unsupported,
        _ => DicomPreviewOutcome.Damaged,
    };

    /// <summary>A text attribute, control characters replaced; "" when absent.</summary>
    private static string Text(DicomDataset ds, DicomTag tag, int maxLength)
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
        int max = Math.Max(0, maxLength);
        return sb.Length > max ? sb.ToString(0, max) : sb.ToString();
    }

    /// <summary>The size that fits box x box and keeps the aspect ratio (never larger than the image).</summary>
    private static (int Width, int Height) Fit(int columns, int rows, int box)
    {
        double scale = Math.Min(1.0, Math.Min((double)box / columns, (double)box / rows));
        return (Math.Max(1, (int)Math.Round(columns * scale)), Math.Max(1, (int)Math.Round(rows * scale)));
    }

    /// <summary>Decodes one frame (within the file's limits) and renders it into the fitted size.</summary>
    private static DicomPreviewImage Render(DicomFile file, int index, DicomPixelInfo pi, int box, DicomPreviewOptions o)
    {
        (int w, int h) = Fit(pi.Columns, pi.Rows, Math.Max(1, box));
        using DicomFrame frame = file.OpenFrame(index);
        byte[] rgba = frame.Render(new ViewportOptions
        {
            Width = w,
            Height = h,
            Format = PixelOutputFormat.Rgba8,
            Zoom = 0,                                    // fit the viewport
            Interpolation = o.Interpolation,
            VoiMode = o.VoiMode,
            Background = o.Background,
        });
        return new DicomPreviewImage(w, h, rgba);
    }

    private static DicomPreview WithStrip(DicomPreview preview, DicomFile file, DicomPixelInfo pi, DicomPreviewOptions limits)
    {
        int tiles = Math.Min(pi.NumberOfFrames, limits.MaxStripFrames);
        var done = new List<DicomPreviewImage>(tiles);
        for (int t = 0; t < tiles; t++)
        {
            try
            {
                done.Add(Render(file, (int)((long)t * pi.NumberOfFrames / tiles), pi, limits.StripBox, limits));
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
        return preview with { Strip = new DicomPreviewImage(tw * tiles, th, strip), StripFrames = done.Count };
    }
}
