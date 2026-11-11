using System;
using System.IO;
using System.Text;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>DicomAttachmentPreview: the preview of untrusted attachment bytes that is part of the package.</summary>
internal static class PreviewTests
{
    private static byte[] Bytes(DicomFile f, string name)
    {
        string path = Path.Combine(TempRoot, name);
        f.Save(path, "1.2.840.10008.1.2.1");
        return File.ReadAllBytes(path);
    }

    private static byte[] MultiFrame()
    {
        using DicomFile file = DicomFile.Create();
        DicomDataset ds = file.Dataset;
        ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.7.3");
        ds.SetString(DicomTags.SOPInstanceUID, "2.25.771");
        ds.SetString(DicomTags.StudyInstanceUID, "2.25.772");
        ds.SetString(DicomTags.SeriesInstanceUID, "2.25.773");
        ds.SetString(DicomTags.Modality, "OT");
        ds.SetString(DicomTags.StudyDate, "20260115");
        ds.SetString(DicomTags.SeriesDescription, "Preview <b>series</b>");
        ds.SetString(DicomTags.PatientName, "Preview^Patient");
        ds.SetString(DicomTags.PatientID, "PV-1");
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
        for (int i = 0; i < pixels.Length / 2; i++)
        {
            int v = (i * 7) & 0x0FFF;
            pixels[i * 2] = (byte)(v & 0xFF);
            pixels[i * 2 + 1] = (byte)(v >> 8);
        }
        ds.SetBytes(DicomTags.PixelData, "OW", pixels);
        return Bytes(file, "preview-mf.dcm");
    }

    internal static void Register()
    {
        Case("preview: picture, caption, strip, PNG and JPEG with the recommended options", () =>
        {
            byte[] data = MultiFrame();
            DicomPreview p = DicomAttachmentPreview.Create(data);
            CheckEq(p.Outcome, DicomPreviewOutcome.Ok);
            Check(!p.Incomplete);
            CheckEq(p.Modality, "OT");
            CheckEq(p.StudyDate, "20260115");
            CheckEq(p.SeriesDescription, "Preview <b>series</b>");  // text from the file, not escaped: the caller's job
            CheckEq(p.TransferSyntaxUid, "1.2.840.10008.1.2.1");
            Check(p.TransferSyntaxName.Length != 0);
            CheckEq(p.Rows, 48);
            CheckEq(p.Columns, 64);
            CheckEq(p.Frames, 3);
            Require(p.Image is not null);
            CheckEq(p.Image!.Width, 64);   // never larger than the image
            CheckEq(p.Image.Height, 48);
            CheckEq(p.Image.Rgba.Length, 64 * 48 * 4);
            byte[] png = p.Image.ToPng();
            Check(png.Length > 8 && png[1] == (byte)'P' && png[2] == (byte)'N' && png[3] == (byte)'G');
            byte[] jpeg = p.Image.ToJpeg();
            Check(jpeg.Length > 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8);
            Require(p.Strip is not null);
            CheckEq(p.StripFrames, 3);
            CheckEq(p.Strip!.Width, 3 * 64);
            CheckEq(p.Strip.Height, 48);
        });

        Case("preview: options - box, strip off, caption length; the defaults are the recommended values", () =>
        {
            byte[] data = MultiFrame();
            DicomPreviewOptions d = DicomPreviewOptions.Recommended;
            CheckEq(d.MaxAttachmentBytes, 64 << 20);
            CheckEq(d.MaxBox, 512);
            CheckEq(d.MaxStripFrames, 8);
            CheckEq(d.StripBox, 128);
            CheckEq(d.MaxDepth, 16);
            CheckEq(d.MaxElements, 200_000L);
            CheckEq(d.MaxInflatedSize, 128L << 20);
            CheckEq(d.MaxFrameBytes, 64L << 20);
            CheckEq(d.FrameMemoryBudget, 256L << 20);
            CheckEq(d.MaxCaptionLength, 256);
            CheckEq(d.MaxPixels, 64_000_000L);
            CheckEq(d.MaxPixels, DicomPreviewOptions.DefaultMaxPixels);
            CheckEq(d.MaxTotalPixels, 128_000_000L);
            CheckEq(d.MaxTotalPixels, DicomPreviewOptions.DefaultMaxTotalPixels);
            CheckEq(d.MaxScans, 64);
            CheckEq(d.MaxScans, DicomPreviewOptions.DefaultMaxScans);
            CheckEq(d.MaxCropFactor, 1);
            CheckEq(d.MaxCropFactor, DicomPreviewOptions.DefaultMaxCropFactor);
            CheckEq(d.MaxValueBytes, 65_536L);
            CheckEq(d.MaxValueBytes, DicomPreviewOptions.DefaultMaxValueBytes);
            Check(d.StripControlAndFormatCharacters);
            // The untrusted preset: every limit reaches the read options.
            DicomReadOptions ro = d.ToReadOptions();
            Check(!ro.AllowRawDataset);
            Check(ro.RecoverOverlongLengths == false);
            CheckEq(ro.MaxDepth, 16);
            CheckEq(ro.MaxElements, 200_000L);
            CheckEq(ro.MaxInflatedSize, 128L << 20);
            CheckEq(ro.MaxFrameBytes, 64L << 20);
            CheckEq(ro.FrameMemoryBudget, 256L << 20);
            CheckEq(ro.MaxScans, 64);
            CheckEq(ro.MaxCropFactor, 1);
            CheckEq(new DicomReadOptions().MaxScans, 0);       // the library defaults stay what they were
            CheckEq(new DicomReadOptions().MaxCropFactor, 0);
            DicomPreview p = DicomAttachmentPreview.Create(data, d with { MaxBox = 32, MaxStripFrames = 0, MaxCaptionLength = 7 });
            CheckEq(p.Outcome, DicomPreviewOutcome.Ok);
            Require(p.Image is not null);
            CheckEq(p.Image!.Width, 32);
            CheckEq(p.Image.Height, 24);
            Check(p.Strip is null);
            CheckEq(p.SeriesDescription, "Preview");
        });

        Case("preview: not DICOM, no image, too large, cut off - never an exception", () =>
        {
            byte[] data = MultiFrame();
            byte[] text = Encoding.ASCII.GetBytes("Dear colleague, please find the images attached.".PadRight(256));
            CheckEq(DicomAttachmentPreview.Create(text).Outcome, DicomPreviewOutcome.NotDicom);
            CheckEq(DicomAttachmentPreview.Create(ReadOnlySpan<byte>.Empty).Outcome, DicomPreviewOutcome.NotDicom);
            Check(DicomAttachmentPreview.ReadPatientFields(text) is null);

            using (DicomFile sr = DicomFile.Create())
            {
                sr.Dataset.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.88.11");
                sr.Dataset.SetString(DicomTags.SOPInstanceUID, "2.25.781");
                sr.Dataset.SetString(DicomTags.Modality, "SR");
                DicomPreview np = DicomAttachmentPreview.Create(Bytes(sr, "preview-sr.dcm"));
                CheckEq(np.Outcome, DicomPreviewOutcome.NoPixelData);
                CheckEq(np.Modality, "SR");
                Check(np.Image is null);
            }

            DicomPreviewOptions o = DicomPreviewOptions.Recommended;
            DicomPreview big = DicomAttachmentPreview.Create(data, o with { MaxFrameBytes = 4 });
            CheckEq(big.Outcome, DicomPreviewOutcome.TooLarge);
            Check(big.Image is null);
            CheckEq(big.Modality, "OT");  // the caption is still there
            CheckEq(DicomAttachmentPreview.Create(data, o with { MaxAttachmentBytes = 100 }).Outcome, DicomPreviewOutcome.TooLarge);
            CheckEq(DicomAttachmentPreview.Create(data, o with { MaxElements = 3 }).Outcome, DicomPreviewOutcome.TooLarge);

            // Cut inside the last frame: flagged, the first frame is still shown.
            DicomPreview cut = DicomAttachmentPreview.Create(data.AsSpan(0, data.Length - 3));
            Check(cut.Incomplete);
            Check(cut.ReadLoss != default);
            Check(cut.Outcome != DicomPreviewOutcome.NotDicom);

            // Every prefix and a sweep of single-byte damage: an outcome, never an exception.
            for (int n = 0; n < 1200; n += 7)
            {
                DicomAttachmentPreview.Create(data.AsSpan(0, n));
            }
            for (int at = 128; at < 1200; at += 5)
            {
                byte[] bad = (byte[])data.Clone();
                bad[at] ^= 0xFF;
                DicomAttachmentPreview.Create(bad);
            }
        });

        Case("preview: MaxPixels refuses before decoding; MaxTotalPixels ends the strip; a render stays in the budget (R-191, R-192)", () =>
        {
            byte[] data = MultiFrame();  // 3 frames of 64 x 48
            DicomPreviewOptions o = DicomPreviewOptions.Recommended;
            DicomPreview at = DicomAttachmentPreview.Create(data, o with { MaxPixels = 64 * 48 });
            CheckEq(at.Outcome, DicomPreviewOutcome.Ok);
            DicomPreview over = DicomAttachmentPreview.Create(data, o with { MaxPixels = 64 * 48 - 1 });
            CheckEq(over.Outcome, DicomPreviewOutcome.TooLarge);
            Check(over.Reason.Contains("pixels", StringComparison.Ordinal));
            Check(over.Image is null);
            CheckEq(over.Rows, 48);       // the caption and the stated size are still there
            CheckEq(over.Modality, "OT");

            // The picture and one more frame fit the preview's pixel budget.
            DicomPreview two = DicomAttachmentPreview.Create(data, o with { MaxTotalPixels = 2 * 64 * 48 });
            CheckEq(two.Outcome, DicomPreviewOutcome.Ok);
            CheckEq(two.StripFrames, 1);
            DicomPreview one = DicomAttachmentPreview.Create(data, o with { MaxTotalPixels = 64 * 48 });
            CheckEq(one.Outcome, DicomPreviewOutcome.Ok);
            Check(one.Strip is null);
            CheckEq(DicomAttachmentPreview.Create(data, o with { MaxTotalPixels = 64 * 48 - 1 }).Outcome, DicomPreviewOutcome.TooLarge);

            // A 1-bit frame of 8000 x 8000: 8 MB of pixel data, 64,000,000 pixels, 256 MB as RGBA.
            using DicomFile file = DicomFile.Create();
            DicomDataset ds = file.Dataset;
            ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.7");
            ds.SetString(DicomTags.SOPInstanceUID, "2.25.791");
            ds.SetString(DicomTags.Modality, "OT");
            ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
            ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
            ds.SetUInt16(DicomTags.Rows, 8000);
            ds.SetUInt16(DicomTags.Columns, 8000);
            ds.SetUInt16(DicomTags.BitsAllocated, 1);
            ds.SetUInt16(DicomTags.BitsStored, 1);
            ds.SetUInt16(DicomTags.HighBit, 0);
            ds.SetUInt16(DicomTags.PixelRepresentation, 0);
            ds.SetBytes(DicomTags.PixelData, "OB", new byte[8000 * 8000 / 8]);
            byte[] bits = Bytes(file, "preview-1bit.dcm");
            // One pixel under the count: refused before anything is decoded.
            DicomPreview early = DicomAttachmentPreview.Create(bits, o with { MaxPixels = 63_999_999 });
            CheckEq(early.Outcome, DicomPreviewOutcome.TooLarge);
            Check(early.Reason.Contains("pixels", StringComparison.Ordinal));
            // At the default count the frame is decoded (8 MB); the render at image resolution does not fit a
            // budget of 64 MiB and the library refuses it before allocating.
            DicomPreview late = DicomAttachmentPreview.Create(bits, o with { FrameMemoryBudget = 64L << 20 });
            CheckEq(late.Outcome, DicomPreviewOutcome.TooLarge);
            Check(late.Reason.Contains("frame_memory_budget", StringComparison.Ordinal));
            Check(late.Image is null);
        });

        Case("preview: captions - control and format characters, value length cap, HTML escaping (R-193, R-199)", () =>
        {
            using DicomFile file = DicomFile.Create();
            DicomDataset ds = file.Dataset;
            ds.SetString(DicomTags.SOPClassUID, "1.2.840.10008.5.1.4.1.1.88.11");
            ds.SetString(DicomTags.SOPInstanceUID, "2.25.792");
            ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 192");
            ds.SetString(DicomTags.Modality, "SR");
            // RIGHT-TO-LEFT OVERRIDE, ZERO WIDTH SPACE, a C1 control (NEL), markup.
            const string hostile = "scan\u202Egpj.exe\u200B \u0085<img src=x onerror=alert(1)> & 'q' \"r\"";
            ds.SetString(DicomTags.SeriesDescription, hostile);
            byte[] data = Bytes(file, "preview-caption.dcm");

            DicomPreview p = DicomAttachmentPreview.Create(data);
            CheckEq(p.Outcome, DicomPreviewOutcome.NoPixelData);
            CheckEq(p.SeriesDescription, "scangpj.exe  <img src=x onerror=alert(1)> & 'q' \"r\"");
            CheckEq(DicomAttachmentPreview.HtmlEscape(p.SeriesDescription),
                    "scangpj.exe  &lt;img src=x onerror=alert(1)&gt; &amp; &#39;q&#39; &quot;r&quot;");
            // The option off: the text as the file has it.
            DicomPreview raw = DicomAttachmentPreview.Create(data, DicomPreviewOptions.Recommended with { StripControlAndFormatCharacters = false });
            CheckEq(raw.SeriesDescription, hostile);

            // A stored value longer than MaxValueBytes is not decoded: the field stays empty, the rest is there.
            Check(ds.TryGetValueLength(DicomTags.SeriesDescription, out long stored));
            Check(stored >= hostile.Length);
            Check(!ds.TryGetValueLength(DicomTags.PatientName, out _));
            DicomPreview capped = DicomAttachmentPreview.Create(data, DicomPreviewOptions.Recommended with { MaxValueBytes = stored - 1 });
            CheckEq(capped.SeriesDescription, "");
            CheckEq(capped.Modality, "SR");
            CheckEq(DicomAttachmentPreview.Create(data, DicomPreviewOptions.Recommended with { MaxValueBytes = stored }).SeriesDescription,
                    p.SeriesDescription);

            // CaptionText on its own: categories Cc and Cf, separators, lone surrogates, the cut.
            CheckEq(DicomAttachmentPreview.CaptionText("a\tb\u007Fc\u009Fd\u2028e\u2029f", 100), "a b c d e f");
            CheckEq(DicomAttachmentPreview.CaptionText("a\u200Db\u2066c\uFEFFd\u00ADe\U000E0041f", 100), "abcdef");
            CheckEq(DicomAttachmentPreview.CaptionText("a\uD83Db", 100), "a\uFFFDb");
            CheckEq(DicomAttachmentPreview.CaptionText("\u0BA4\u0BAE\u0BBF\u0BB4\u0BCD \U0001F600", 100), "\u0BA4\u0BAE\u0BBF\u0BB4\u0BCD \U0001F600");
            CheckEq(DicomAttachmentPreview.CaptionText("ab\U0001F600cd", 3), "ab");      // never half a surrogate pair
            CheckEq(DicomAttachmentPreview.CaptionText("ab\U0001F600cd", 4), "ab\U0001F600");
            CheckEq(DicomAttachmentPreview.CaptionText("a\u202Eb", 100, strip: false), "a\u202Eb");
            CheckEq(DicomAttachmentPreview.CaptionText("abc", 0), "");
            CheckEq(DicomAttachmentPreview.HtmlEscape(null), "");
            CheckEq(DicomAttachmentPreview.HtmlEscape("plain"), "plain");
            CheckEq(DicomAttachmentPreview.HtmlEscape("<a href=\"x\">&</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;");
        });

        Case("preview: patient identifiers only on request", () =>
        {
            byte[] data = MultiFrame();
            (string Name, string Id)? p = DicomAttachmentPreview.ReadPatientFields(data);
            Require(p is not null);
            CheckEq(p!.Value.Name, "Preview^Patient");
            CheckEq(p.Value.Id, "PV-1");
            Check(DicomAttachmentPreview.ReadPatientFields(data, DicomPreviewOptions.Recommended with { MaxAttachmentBytes = 100 }) is null);
        });
    }
}
