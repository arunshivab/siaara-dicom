// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// The synthetic inputs of the SUV / ROI golden file (Golden/suv-roi-golden.json): every case is
// built in memory, deterministically, so the golden values (written once with the toolkit's
// siaara_suv_factor / siaara_roi_stats_compute before those functions left the toolkit) can be
// compared with the app's own C# implementation on exactly the same data sets.

using System.Globalization;
using System.Runtime.InteropServices;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Tests;

internal sealed record SuvCase(string Name, int Frame, Action<TkFile> Build);

internal sealed record RoiImage(string Name, Action<TkFile> Build);

internal sealed record RoiCase(string Name, string Image, int Frame, int Shape, double[] Parameters, bool Partial);

internal static class SuvRoiCases
{
    private static readonly TkTag Units = new(0x00541001);
    private static readonly TkTag DecayCorrection = new(0x00541102);
    private static readonly TkTag CorrectedImage = new(0x00280051);
    private static readonly TkTag AcquisitionDateTime = new(0x0008002A);
    private static readonly TkTag TimezoneOffset = new(0x00080201);
    private static readonly TkTag Radiopharmaceutical = new(0x00540016);
    private static readonly TkTag StartTime = new(0x00181072);
    private static readonly TkTag StartDateTime = new(0x00181078);
    private static readonly TkTag TotalDose = new(0x00181074);
    private static readonly TkTag HalfLife = new(0x00181075);
    private static readonly TkTag PerFrame = new(0x52009230);
    private static readonly TkTag Shared = new(0x52009229);
    private static readonly TkTag FrameContent = new(0x00209111);
    private static readonly TkTag FrameAcquisitionDateTime = new(0x00189074);
    private static readonly TkTag PixelMeasures = new(0x00289110);

    private sealed record Pet
    {
        public string? Units { get; init; } = "BQML";
        public string? Decay { get; init; } = "START";
        public string? Corrected { get; init; } = "DECY\\ATTN";
        public string? Weight { get; init; } = "70";
        public string? Size { get; init; } = "1.75";
        public string? Sex { get; init; } = "M";
        public string? SeriesDate { get; init; } = "20260301";
        public string? SeriesTime { get; init; } = "101500";
        public string? AcqDate { get; init; } = "20260301";
        public string? AcqTime { get; init; } = "101600";
        public string? AcqDateTime { get; init; }
        public string? Timezone { get; init; }
        public bool Radiopharm { get; init; } = true;
        public string? Dose { get; init; } = "370000000";
        public string? Half { get; init; } = "6586.2";
        public string? Start { get; init; } = "091500";
        public string? StartDt { get; init; }
        public int ExtraRadiopharm { get; init; }
        public (int Block, string Value)? Philips { get; init; }
        public string[]? FrameAcq { get; init; }
        public string? SharedFrameAcq { get; init; }
    }

    private static void Set(TkFile f, TkTag tag, string vr, string? value)
    {
        if (value is not null)
        {
            f.SetRawString(tag, vr, value);
        }
    }

    private static Action<TkFile> Build(Pet p) => f =>
    {
        f.SetRawString(Tags.SopClassUid, "UI", "1.2.840.10008.5.1.4.1.1.128");
        f.SetRawString(Tags.Modality, "CS", "PT");
        Set(f, Units, "CS", p.Units);
        Set(f, DecayCorrection, "CS", p.Decay);
        Set(f, CorrectedImage, "CS", p.Corrected);
        Set(f, Tags.PatientWeight, "DS", p.Weight);
        Set(f, Tags.PatientSize, "DS", p.Size);
        Set(f, Tags.PatientSex, "CS", p.Sex);
        Set(f, Tags.SeriesDate, "DA", p.SeriesDate);
        Set(f, Tags.SeriesTime, "TM", p.SeriesTime);
        Set(f, Tags.AcquisitionDate, "DA", p.AcqDate);
        Set(f, Tags.AcquisitionTime, "TM", p.AcqTime);
        Set(f, AcquisitionDateTime, "DT", p.AcqDateTime);
        Set(f, TimezoneOffset, "SH", p.Timezone);
        TkDataset ds = f.Dataset;
        if (p.Radiopharm)
        {
            for (int k = 0; k <= p.ExtraRadiopharm; k++)
            {
                TkDataset item = ds.AppendItem(Radiopharmaceutical);
                if (p.Dose is not null)
                {
                    item.SetRawString(TotalDose, "DS", k == 0 ? p.Dose : "1000");
                }
                if (p.Half is not null)
                {
                    item.SetRawString(HalfLife, "DS", p.Half);
                }
                if (p.Start is not null)
                {
                    item.SetRawString(StartTime, "TM", p.Start);
                }
                if (p.StartDt is not null)
                {
                    item.SetRawString(StartDateTime, "DT", p.StartDt);
                }
            }
        }
        if (p.Philips is var (block, value))
        {
            var creator = new TkTag((uint)((0x7053 << 16) | block));
            var factor = new TkTag((uint)((0x7053 << 16) | (block << 8) | 0x09));
            f.SetRawString(creator, "LO", "Philips PET Private Group");
            f.SetRawString(factor, "DS", value);
        }
        if (p.FrameAcq is not null)
        {
            f.SetRawString(Tags.NumberOfFrames, "IS", p.FrameAcq.Length.ToString(CultureInfo.InvariantCulture));
            foreach (string dt in p.FrameAcq)
            {
                TkDataset frame = ds.AppendItem(PerFrame);
                TkDataset content = frame.AppendItem(FrameContent);
                if (dt.Length > 0)
                {
                    content.SetRawString(FrameAcquisitionDateTime, "DT", dt);
                }
            }
        }
        if (p.SharedFrameAcq is not null)
        {
            TkDataset shared = ds.AppendItem(Shared);
            shared.AppendItem(FrameContent).SetRawString(FrameAcquisitionDateTime, "DT", p.SharedFrameAcq);
        }
    };

    public static IReadOnlyList<SuvCase> Suv()
    {
        var b = new Pet();
        var list = new List<(string, int, Pet)>
        {
            ("base: BQML START, start time only (date assumed)", 0, b),
            ("start date-time", 0, b with { StartDt = "20260301091500" }),
            ("start date-time with fraction", 0, b with { StartDt = "20260301091500.123456" }),
            ("start date-time and (0008,0201) both +0530", 0, b with { StartDt = "20260301091500+0530", Timezone = "+0530" }),
            ("start date-time offset, scan without: mismatch", 0, b with { StartDt = "20260301091500-0300" }),
            ("start date-time offset -0300, (0008,0201) +0100", 0, b with { StartDt = "20260301091500-0300", Timezone = "+0100" }),
            ("acquisition date-time with offset and start without", 0, b with { SeriesDate = null, SeriesTime = null, AcqDateTime = "20260301101600+0100", StartDt = "20260301091500" }),
            ("invalid start date-time falls back to the start time", 0, b with { StartDt = "2026XX01" }),
            ("invalid (0008,0201) ignored", 0, b with { StartDt = "20260301091500+0000", Timezone = "0530" }),
            ("retired TM and DA forms", 0, b with { SeriesDate = "2026.03.01", SeriesTime = "10:15:00.5", Start = "09:15:00" }),
            ("CNTS without scale", 0, b with { Units = "CNTS" }),
            ("CNTS with Philips factor, block 10", 0, b with { Units = "CNTS", Philips = (0x10, "2.5") }),
            ("CNTS with Philips factor, block 11", 0, b with { Units = "CNTS", Philips = (0x11, "0.125") }),
            ("CNTS with Philips factor 0", 0, b with { Units = "CNTS", Philips = (0x10, "0") }),
            ("BQML ignores the Philips factor", 0, b with { Philips = (0x10, "2.5") }),
            ("GML with weight, height, sex", 0, b with { Units = "GML" }),
            ("GML without weight", 0, b with { Units = "GML", Weight = null }),
            ("GML female", 0, b with { Units = "GML", Sex = "F" }),
            ("units missing", 0, b with { Units = null }),
            ("units unsupported", 0, b with { Units = "PROPCNTS" }),
            ("units with padding", 0, b with { Units = "BQML " }),
            ("ADMIN with DECY, no half-life", 0, b with { Decay = "ADMIN", Half = null }),
            ("ADMIN without DECY", 0, b with { Decay = "ADMIN", Corrected = "ATTN" }),
            ("NONE: acquisition time", 0, b with { Decay = "NONE", Corrected = "ATTN" }),
            ("NONE without acquisition time", 0, b with { Decay = "NONE", AcqDate = null, AcqTime = null }),
            ("decay correction missing", 0, b with { Decay = null }),
            ("decay correction unknown value", 0, b with { Decay = "OTHER" }),
            ("START without DECY", 0, b with { Corrected = "ATTN" }),
            ("ATTN missing", 0, b with { Corrected = "DECY" }),
            ("corrected image missing", 0, b with { Corrected = null }),
            ("weight missing", 0, b with { Weight = null }),
            ("weight zero", 0, b with { Weight = "0" }),
            ("weight implausible", 0, b with { Weight = "2000" }),
            ("weight not a number", 0, b with { Weight = "seventy" }),
            ("height missing", 0, b with { Size = null }),
            ("height in cm (implausible)", 0, b with { Size = "175" }),
            ("height too small", 0, b with { Size = "0.2" }),
            ("sex other", 0, b with { Sex = "O" }),
            ("sex missing", 0, b with { Sex = null }),
            ("female", 0, b with { Sex = "F", Weight = "58.5", Size = "1.62" }),
            ("James LBM negative at high BMI", 0, b with { Weight = "200", Size = "1.5" }),
            ("IBW negative for a short female", 0, b with { Sex = "F", Weight = "20", Size = "1.0" }),
            ("dose missing", 0, b with { Dose = null }),
            ("dose negative", 0, b with { Dose = "-5" }),
            ("half-life missing", 0, b with { Half = null }),
            ("half-life zero", 0, b with { Half = "0" }),
            ("no radiopharmaceutical sequence", 0, b with { Radiopharm = false }),
            ("several radiopharmaceuticals", 0, b with { ExtraRadiopharm = 2 }),
            ("series after acquisition: acquisition used", 0, b with { SeriesTime = "120000" }),
            ("series date-time missing: acquisition used", 0, b with { SeriesDate = null, SeriesTime = null }),
            ("series missing, acquisition date-time", 0, b with { SeriesDate = null, SeriesTime = null, AcqDate = null, AcqTime = null, AcqDateTime = "20260301101700" }),
            ("no scan time at all", 0, b with { SeriesDate = null, SeriesTime = null, AcqDate = null, AcqTime = null }),
            ("no scan time, start date-time", 0, b with { SeriesDate = null, SeriesTime = null, AcqDate = null, AcqTime = null, StartDt = "20260301091500" }),
            ("injection after the scan (date assumed)", 0, b with { Start = "120000" }),
            ("injection after the scan (date-time)", 0, b with { StartDt = "20260301120000" }),
            ("more than 10 half-lives", 0, b with { StartDt = "20260228091500" }),
            ("new year: scan just after midnight", 0, b with { SeriesDate = "20260101", SeriesTime = "000500", AcqDate = "20260101", AcqTime = "000600", Start = "235500" }),
            ("leap day", 0, b with { SeriesDate = "20280229", AcqDate = "20280229", StartDt = "20280229091500" }),
            ("invalid series date", 0, b with { SeriesDate = "20260230" }),
            ("enhanced: per-frame acquisition, frame 1, series after", 1, b with { SeriesTime = "120000", AcqDate = null, AcqTime = null, FrameAcq = ["20260301101600", "20260301101800.25"] }),
            ("enhanced: per-frame item without the date-time, shared one used", 0, b with { SeriesTime = "120000", AcqDate = null, AcqTime = null, FrameAcq = [""], SharedFrameAcq = "20260301101900" }),
            ("enhanced: frame past the per-frame items, shared", 2, b with { SeriesTime = "120000", AcqDate = null, AcqTime = null, FrameAcq = ["20260301101600", "20260301101800"], SharedFrameAcq = "20260301102000" }),
            ("NONE with per-frame acquisition", 1, b with { Decay = "NONE", Corrected = "ATTN", AcqDate = null, AcqTime = null, FrameAcq = ["20260301101600", "20260301104000"] }),
            ("empty data set", 0, new Pet
            {
                Units = null, Decay = null, Corrected = null, Weight = null, Size = null, Sex = null, SeriesDate = null, SeriesTime = null, AcqDate = null,
                AcqTime = null, Radiopharm = false,
            }),
        };
        return [.. list.Select(t => new SuvCase(t.Item1, t.Item2, Build(t.Item3)))];
    }

    private static void Gray16(TkFile f, int rows, int cols, int frames, Func<int, int, int, int> stored, string slope, string intercept)
    {
        f.SetRawString(Tags.SopClassUid, "UI", "1.2.840.10008.5.1.4.1.1.2");
        f.SetRawString(Tags.Modality, "CS", "CT");
        f.SetUInt16(Tags.SamplesPerPixel, 1);
        f.SetString(Tags.PhotometricInterpretation, "MONOCHROME2");
        f.SetUInt16(Tags.Rows, (ushort)rows);
        f.SetUInt16(Tags.Columns, (ushort)cols);
        f.SetUInt16(Tags.BitsAllocated, 16);
        f.SetUInt16(Tags.BitsStored, 16);
        f.SetUInt16(Tags.HighBit, 15);
        f.SetUInt16(Tags.PixelRepresentation, 1);
        if (frames > 1)
        {
            f.SetRawString(Tags.NumberOfFrames, "IS", frames.ToString(CultureInfo.InvariantCulture));
        }
        f.SetRawString(Tags.RescaleSlope, "DS", slope);
        f.SetRawString(Tags.RescaleIntercept, "DS", intercept);
        var px = new short[rows * cols * frames];
        for (int k = 0; k < frames; k++)
        {
            for (int r = 0; r < rows; r++)
            {
                for (int c = 0; c < cols; c++)
                {
                    px[(k * rows * cols) + (r * cols) + c] = (short)stored(k, r, c);
                }
            }
        }
        f.SetBytes(Tags.PixelData, "OW", MemoryMarshal.AsBytes(px.AsSpan()));
    }

    public static IReadOnlyList<RoiImage> RoiImages() =>
    [
        new("a: 16x20, slope 0.37, spacing 0.8\\0.6", f =>
        {
            Gray16(f, 16, 20, 1, (_, r, c) => (((r * 37) + (c * 101)) % 997) - 300 + (r * c), "0.37", "-12.5");
            f.SetRawString(Tags.PixelSpacing, "DS", "0.8\\0.6");
        }),
        new("b: 12x12 without spacing", f => Gray16(f, 12, 12, 1, (_, r, c) => (r * 12) + c, "1", "0")),
        new("c: 2 frames 8x10, imager pixel spacing", f =>
        {
            Gray16(f, 8, 10, 2, (k, r, c) => (k * 1000) + (r * r * 7) - (c * 13), "2.5", "1000");
            f.SetRawString(Tags.ImagerPixelSpacing, "DS", "0.143\\0.143");
        }),
        new("d: 6x6 per-frame pixel measures", f =>
        {
            Gray16(f, 6, 6, 1, (_, r, c) => (r - c) * 1000, "0.001", "0");
            f.Dataset.AppendItem(new TkTag(0x52009229)).AppendItem(PixelMeasures).SetRawString(Tags.PixelSpacing, "DS", "1.25\\2.5");
        }),
    ];

    public static IReadOnlyList<RoiCase> Roi()
    {
        var shapes = new List<(string Name, int Shape, double[] P)>
        {
            ("rect aligned", 0, [2, 3, 7, 9]),
            ("rect fractional", 0, [1.3, 2.7, 6.45, 8.05]),
            ("rect reversed corners", 0, [6.45, 8.05, 1.3, 2.7]),
            ("rect one pixel", 0, [4, 4, 5, 5]),
            ("rect partly outside", 0, [-2.5, -1.25, 3.5, 4.75]),
            ("rect outside", 0, [40, 40, 50, 50]),
            ("rect degenerate", 0, [3, 3, 3, 8]),
            ("rect whole image and more", 0, [-1, -1, 100, 100]),
            ("circle centred", 2, [6, 6, 3.2]),
            ("circle on pixel centres", 2, [5.5, 5.5, 2]),
            ("circle fractional", 2, [4.3, 7.9, 2.65]),
            ("circle at the corner", 2, [0, 0, 3.7]),
            ("circle tiny", 2, [3.5, 3.5, 0.3]),
            ("circle zero radius", 2, [3, 3, 0]),
            ("ellipse axis aligned", 1, [6, 5, 4, 2.5, 0]),
            ("ellipse rotated 0.3", 1, [6.2, 5.1, 4.4, 1.9, 0.3]),
            ("ellipse rotated 1.2", 1, [5.7, 4.6, 3.3, 1.4, 1.2]),
            ("ellipse rotated -0.7", 1, [4.9, 5.3, 3.8, 2.2, -0.7]),
            ("ellipse rotated pi/2", 1, [5, 5, 4, 1, Math.PI / 2]),
            ("ellipse partly outside", 1, [1, 9, 5, 3, 0.45]),
            ("ellipse degenerate", 1, [5, 5, 0, 2, 0]),
            ("triangle", 3, [1.5, 1.5, 8.2, 2.1, 4.4, 7.7]),
            ("concave polygon", 3, [1, 1, 9, 1, 9, 9, 5, 4, 1, 9]),
            ("polygon partly outside", 3, [-3, 2, 6, -2.5, 8, 7, 2, 10]),
            ("polygon two vertices", 3, [1, 1, 5, 5]),
            ("bow tie (no area)", 3, [1, 1, 5, 5, 5, 1, 1, 5]),
            ("polygon odd parameter count", 3, [1, 1, 5, 5, 3]),
            ("freehand ring", 4, [.. Enumerable.Range(0, 37).SelectMany(k => new[] { 5.5 + (3.7 * Math.Cos(k * Math.PI / 18.5)) + (0.4 * Math.Sin(k)), 5.5 + (2.9 * Math.Sin(k * Math.PI / 18.5)) })]),
            ("freehand with vertices on pixel edges", 4, [2, 2, 2, 6, 3, 6, 3, 7, 7, 7, 7, 2]),
        };
        var list = new List<RoiCase>();
        foreach (RoiImage img in RoiImages())
        {
            foreach ((string name, int shape, double[] p) in shapes)
            {
                foreach (bool partial in new[] { false, true })
                {
                    list.Add(new RoiCase(name + (partial ? " (partial)" : " (centres)"), img.Name, 0, shape, p, partial));
                }
            }
        }
        list.Add(new RoiCase("frame 1 rect (partial)", "c: 2 frames 8x10, imager pixel spacing", 1, 0, [1.5, 1.5, 7.25, 5.75], true));
        list.Add(new RoiCase("frame 1 ellipse (centres)", "c: 2 frames 8x10, imager pixel spacing", 1, 1, [5, 4, 3, 2, 0.6], false));
        list.Add(new RoiCase("frame out of range", "c: 2 frames 8x10, imager pixel spacing", 2, 0, [1, 1, 4, 4], false));
        return list;
    }
}
