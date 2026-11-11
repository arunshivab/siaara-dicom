// Preview of a DICOM file that arrived as UNTRUSTED bytes (a mail attachment, an upload): a caption
// from a few safe header fields and a small picture of the first frame, or a plain reason why there
// is no picture. From memory, no temporary file, every limit explicit.
// The limits bound MEMORY and the amount of work, not TIME: a decode cannot be cancelled and has no
// deadline. Run previews of untrusted input in a SEPARATE PROCESS with a kill timer and a memory limit.
// docs/GETTING_STARTED.md, "Showing a DICOM attachment safely (untrusted input)".
using System;
using System.Collections.Generic;
using System.Globalization;
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
    /// <summary>A limit was reached: there is no picture (Reason names the limit).</summary>
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
    /// <summary>Largest inflated size of a deflated data set (a deflate bomb). Default 128 MiB. While a data set
    /// inflates, the process can hold about three times this limit (measured with 128 MiB on Linux: about 380 MiB
    /// resident, both for 127 MiB of data and for a deflate bomb refused at the limit; freed growth steps stay in
    /// the process heap). Give the preview worker about 1 GiB.</summary>
    public long MaxInflatedSize { get; init; } = 128L << 20;
    /// <summary>Largest decoded frame (huge Rows x Columns): a larger one is refused before anything is decoded. Default 64 MiB.</summary>
    public long MaxFrameBytes { get; init; } = 64L << 20;
    /// <summary>Memory a decoder may take for one frame (a codestream that claims huge dimensions). Default 256 MiB.</summary>
    public long FrameMemoryBudget { get; init; } = 256L << 20;
    /// <summary>Most pixels (Rows x Columns) of one frame: a larger image is refused before anything is decoded.
    /// A frame is rendered at 4 bytes per pixel whatever its bit depth (a 1-bit frame of 64 MiB is 537 million
    /// pixels), so <see cref="MaxFrameBytes"/> alone does not bound it. Default 64,000,000.</summary>
    public long MaxPixels { get; init; } = DefaultMaxPixels;
    /// <summary>Most pixels decoded for one preview, all frames together (the picture and the strip): the strip
    /// ends when the next frame would pass it. Bounds the work a multi-frame file can ask for.
    /// Default 128,000,000.</summary>
    public long MaxTotalPixels { get; init; } = DefaultMaxTotalPixels;
    /// <summary>Most JPEG scans decoded per frame (a progressive JPEG with hundreds of scans is slow by design).
    /// Default 64.</summary>
    public int MaxScans { get; init; } = DefaultMaxScans;
    /// <summary>A codestream larger than Rows x Columns is cropped only when at most this many times larger per
    /// axis. Default 1: it must have the stated size (nothing larger is decoded to be cropped).</summary>
    public int MaxCropFactor { get; init; } = DefaultMaxCropFactor;
    /// <summary>Longest stored value, in bytes, read for a caption field: a longer one is not decoded at all and
    /// the field stays "". Default 65,536.</summary>
    public long MaxValueBytes { get; init; } = DefaultMaxValueBytes;
    /// <summary>Longest caption field, in characters; longer text is cut. Default 256.</summary>
    public int MaxCaptionLength { get; init; } = 256;
    /// <summary>Caption text: control characters (Unicode category Cc: C0, DEL, C1) and line / paragraph
    /// separators become a space, format characters (category Cf: bidirectional overrides such as U+202E,
    /// zero-width characters, the byte order mark) are removed, a lone surrogate becomes U+FFFD. Default true.
    /// Zero-width joiners are format characters too: turn this off (and filter yourself) if captions in scripts
    /// that need them must keep their exact shaping.</summary>
    public bool StripControlAndFormatCharacters { get; init; } = true;

    /// <summary>Default of <see cref="MaxPixels"/>.</summary>
    public const long DefaultMaxPixels = 64_000_000;
    /// <summary>Default of <see cref="MaxTotalPixels"/>.</summary>
    public const long DefaultMaxTotalPixels = 128_000_000;
    /// <summary>Default of <see cref="MaxScans"/>.</summary>
    public const int DefaultMaxScans = 64;
    /// <summary>Default of <see cref="MaxCropFactor"/>.</summary>
    public const int DefaultMaxCropFactor = 1;
    /// <summary>Default of <see cref="MaxValueBytes"/>.</summary>
    public const long DefaultMaxValueBytes = 65_536;
    /// <summary>How the picture is resampled to its size. Default linear.</summary>
    public Interpolation Interpolation { get; init; } = Interpolation.Linear;
    /// <summary>Window / VOI used for the picture. Default: the file's own (window, VOI LUT, palette, MONOCHROME1).</summary>
    public VoiMode VoiMode { get; init; } = VoiMode.FromFile;
    /// <summary>Colour behind the image. Default opaque black.</summary>
    public Rgba8Color Background { get; init; } = new Rgba8Color(0, 0, 0, 255);

    /// <summary>The recommended values for untrusted input (the defaults).</summary>
    public static DicomPreviewOptions Recommended { get; } = new();

    /// <summary>The read options these limits stand for: the toolkit's preset for untrusted input. Every limit is
    /// set explicitly: nothing is left to defaults meant for trusted local files. Use it for your own reads of
    /// untrusted bytes too (<c>DicomFile.Open(bytes, DicomPreviewOptions.Recommended.ToReadOptions())</c>).</summary>
    /// <exception cref="ArgumentOutOfRangeException">A limit is zero or negative. (To the library below, 0
    /// means "the default for trusted files"; a zeroed limit must not silently become that.)</exception>
    public DicomReadOptions ToReadOptions(bool headerOnly = false)
    {
        Validate();
        return ReadOptionsUnchecked(headerOnly);
    }

    /// <summary>Throws <see cref="ArgumentOutOfRangeException"/> when a limit is zero or negative.
    /// <c>MaxStripFrames</c> may be 0 (no strip) and <c>MaxCaptionLength</c> may be 0 (empty captions).</summary>
    public void Validate()
    {
        Positive(MaxAttachmentBytes, nameof(MaxAttachmentBytes));
        Positive(MaxBox, nameof(MaxBox));
        Positive(StripBox, nameof(StripBox));
        Positive(MaxDepth, nameof(MaxDepth));
        Positive(MaxElements, nameof(MaxElements));
        Positive(MaxInflatedSize, nameof(MaxInflatedSize));
        Positive(MaxFrameBytes, nameof(MaxFrameBytes));
        Positive(FrameMemoryBudget, nameof(FrameMemoryBudget));
        Positive(MaxPixels, nameof(MaxPixels));
        Positive(MaxTotalPixels, nameof(MaxTotalPixels));
        Positive(MaxScans, nameof(MaxScans));
        Positive(MaxCropFactor, nameof(MaxCropFactor));
        Positive(MaxValueBytes, nameof(MaxValueBytes));
        if (MaxStripFrames < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(MaxStripFrames), MaxStripFrames, "a preview limit cannot be negative");
        }
        if (MaxCaptionLength < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(MaxCaptionLength), MaxCaptionLength, "a preview limit cannot be negative");
        }
    }

    private static void Positive(long value, string name)
    {
        if (value <= 0)
        {
            throw new ArgumentOutOfRangeException(name, value, "a preview limit must be greater than zero");
        }
    }

    private DicomReadOptions ReadOptionsUnchecked(bool headerOnly) => new()
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
        MaxScans = MaxScans,
        MaxCropFactor = MaxCropFactor,
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
/// file: escape them before they go into HTML (<see cref="DicomAttachmentPreview.HtmlEscape"/>).</summary>
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
    /// <summary>The preview of untrusted bytes. Never throws for bad input and never needs more memory than the
    /// limits allow, whatever the bytes claim (reading, decoding and rendering are all held to them).
    /// <para>There is NO bound on the time a decode takes and it cannot be cancelled: a thread cannot be
    /// abandoned. For untrusted input call this in a SEPARATE PROCESS that the caller kills after a timeout and
    /// that runs under a memory limit (a job object, a cgroup, ulimit); the limits here keep the work
    /// proportional to the input, they are not a deadline.</para></summary>
    /// <param name="data">The attachment bytes. They are only read.</param>
    /// <param name="options">Limits and drawing options; null: <see cref="DicomPreviewOptions.Recommended"/>.</param>
    public static DicomPreview Create(ReadOnlySpan<byte> data, DicomPreviewOptions? options = null)
    {
        DicomPreviewOptions limits = options ?? DicomPreviewOptions.Recommended;
        limits.Validate();  // the caller's options, not the input: a zeroed limit is an error, not "no limit"
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
        catch (Exception e) when (e is OverflowException or ArgumentException or InvalidOperationException or IndexOutOfRangeException)
        {
            // Whatever the bytes are, the answer is an outcome and never an exception.
            return new DicomPreview { Outcome = DicomPreviewOutcome.Damaged, Reason = "the file could not be previewed" };
        }
    }

    /// <summary>A transfer syntax UID as far as it is one: digits and dots, at most 64 characters; else "". The
    /// file meta is text from the file like any other, and a field called "UID" is the one most likely to be
    /// trusted.</summary>
    private static string CleanUid(string? uid)
    {
        if (string.IsNullOrEmpty(uid) || uid.Length > 64)
        {
            return "";
        }
        foreach (char c in uid)
        {
            if (c != '.' && (c < '0' || c > '9'))
            {
                return "";
            }
        }
        return uid;
    }

    private static DicomPreview FromFile(DicomFile file, DicomPreviewOptions limits)
    {
        {
            // An incomplete read is not an error here: what was read is previewed and flagged.
            DicomReadReport report = file.ReadReport;
            // Safe header fields only.
            DicomDataset ds = file.Dataset;
            string ts = CleanUid(file.TransferSyntaxUid);
            var preview = new DicomPreview
            {
                Incomplete = !report.IsComplete,
                ReadLoss = report.Loss,
                Modality = Text(ds, DicomTags.Modality, limits),
                StudyDate = Text(ds, DicomTags.StudyDate, limits),
                SeriesDescription = Text(ds, DicomTags.SeriesDescription, limits),
                TransferSyntaxUid = ts,
                TransferSyntaxName = ts.Length == 0 ? "" : TransferSyntaxInfo.Get(ts)?.Name ?? "",
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
                // Pixels, not bytes: the render works at 4 bytes per pixel whatever the bit depth.
                long pixels = (long)pi.Rows * pi.Columns;
                if (pixels > limits.MaxPixels || pixels > limits.MaxTotalPixels)
                {
                    return preview with { Outcome = DicomPreviewOutcome.TooLarge, Reason = "the image has more pixels than the preview limit" };
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
        limits.Validate();
        if (data.Length > limits.MaxAttachmentBytes || data.Length < 132 || !data.Slice(128, 4).SequenceEqual("DICM"u8))
        {
            return null;
        }
        try
        {
            using DicomFile file = DicomFile.Open(data, limits.ToReadOptions(headerOnly: true));
            return (Text(file.Dataset, DicomTags.PatientName, limits), Text(file.Dataset, DicomTags.PatientID, limits));
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

    /// <summary>A text attribute for a caption; "" when absent or when its stored value is longer than
    /// MaxValueBytes (then nothing of it is decoded).</summary>
    private static string Text(DicomDataset ds, DicomTag tag, DicomPreviewOptions o)
    {
        // The stored length first: a value of many megabytes is never decoded to find out it is too long.
        if (!ds.TryGetValueLength(tag, out long stored) || stored > Math.Max(0, o.MaxValueBytes))
        {
            return "";
        }
        if (!ds.TryGetString(tag, out string? value))
        {
            return "";
        }
        return CaptionText(value, o.MaxCaptionLength, o.StripControlAndFormatCharacters);
    }

    /// <summary>Caption text from a file made safe to show as plain text: cut to maxLength characters (never
    /// inside a surrogate pair) and, when strip is true, control characters and line / paragraph separators
    /// replaced by a space, format characters (bidirectional overrides, zero-width characters) removed and a lone
    /// surrogate replaced by U+FFFD. It is still text from the file: escape it for HTML with
    /// <see cref="HtmlEscape"/>.</summary>
    /// <param name="value">The text.</param>
    /// <param name="maxLength">Most characters (UTF-16 code units) kept.</param>
    /// <param name="strip">The <see cref="DicomPreviewOptions.StripControlAndFormatCharacters"/> behaviour.</param>
    public static string CaptionText(string value, int maxLength, bool strip = true)
    {
        ArgumentNullException.ThrowIfNull(value);
        int max = Math.Max(0, maxLength);
        var sb = new StringBuilder(Math.Min(value.Length, max));
        for (int i = 0; i < value.Length && sb.Length < max;)
        {
            if (!Rune.TryGetRuneAt(value, i, out Rune rune))
            {
                // A lone surrogate.
                sb.Append(strip ? '\uFFFD' : value[i]);
                i++;
                continue;
            }
            i += rune.Utf16SequenceLength;
            if (strip)
            {
                switch (Rune.GetUnicodeCategory(rune))
                {
                    case UnicodeCategory.Control:
                    case UnicodeCategory.LineSeparator:
                    case UnicodeCategory.ParagraphSeparator:
                        sb.Append(' ');
                        continue;
                    case UnicodeCategory.Format:
                        continue;
                }
            }
            if (sb.Length + rune.Utf16SequenceLength > max)
            {
                break;  // never half a surrogate pair
            }
            sb.Append(rune.ToString());
        }
        return sb.ToString();
    }

    /// <summary>Text escaped for HTML element content and quoted attribute values: &amp;, &lt;, &gt;, the double
    /// quote and the apostrophe become character references. Every caption field of a preview is text from the
    /// file and must go through this (or your template engine's escaping) before it is put into a page.</summary>
    /// <param name="text">The text; null gives "".</param>
    public static string HtmlEscape(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return "";
        }
        var sb = new StringBuilder(text.Length + 16);
        foreach (char c in text)
        {
            switch (c)
            {
                case '&': sb.Append("&amp;"); break;
                case '<': sb.Append("&lt;"); break;
                case '>': sb.Append("&gt;"); break;
                case '"': sb.Append("&quot;"); break;
                case '\'': sb.Append("&#39;"); break;
                default: sb.Append(c); break;
            }
        }
        return sb.ToString();
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
        long framePixels = (long)pi.Rows * pi.Columns;
        long decoded = framePixels;  // the main picture
        for (int t = 0; t < tiles; t++)
        {
            if (decoded + framePixels > limits.MaxTotalPixels)
            {
                break;  // the preview's pixel budget is spent: the strip ends here
            }
            decoded += framePixels;
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
        long stripBytes = (long)tw * tiles * th * 4;
        if (stripBytes <= 0 || stripBytes > limits.FrameMemoryBudget || stripBytes > int.MaxValue)
        {
            return preview;  // a strip that does not fit the budget is left out; the main picture stays
        }
        var strip = new byte[stripBytes];  // tiles not reached stay transparent
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
