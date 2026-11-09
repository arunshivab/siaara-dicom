// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Numerics;
using System.Runtime.InteropServices;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Samples;

/// <summary>Patient and study identity of generated data (always synthetic).</summary>
/// <param name="PatientName">PN value, e.g. "SAMPLE^CT".</param>
/// <param name="PatientId">ID.</param>
/// <param name="StudyUid">Study Instance UID.</param>
/// <param name="StudyDate">DA.</param>
/// <param name="Accession">Accession Number.</param>
/// <param name="Sex">Sex.</param>
/// <param name="BirthDate">Birth date.</param>
public sealed record SamplePatient(string PatientName, string PatientId, string StudyUid, string StudyDate = "20260301", string Accession = "ACC0001",
                                   string Sex = "O", string BirthDate = "19800101");

/// <summary>
/// Generates synthetic DICOM files with the toolkit (create, setters, save). No real patient data
/// anywhere: phantoms only. Used by the tests and by "siaara-viewer --generate-samples DIR".
/// </summary>
public static class SampleData
{
    /// <summary>CT Image Storage.</summary>
    public const string CtImageStorage = "1.2.840.10008.5.1.4.1.1.2";
    /// <summary>Ultrasound Multi-frame Image Storage.</summary>
    public const string UsMultiFrameStorage = "1.2.840.10008.5.1.4.1.1.3.1";
    /// <summary>Positron Emission Tomography Image Storage.</summary>
    public const string PetImageStorage = "1.2.840.10008.5.1.4.1.1.128";
    /// <summary>Computed Radiography Image Storage.</summary>
    public const string CrImageStorage = "1.2.840.10008.5.1.4.1.1.1";

    /// <summary>A new UID under the 2.25 (UUID) root.</summary>
    /// <returns>The UID.</returns>
    public static string NewUid()
    {
        Span<byte> b = stackalloc byte[17];
        Guid.NewGuid().TryWriteBytes(b[..16]);
        b[16] = 0;
        return "2.25." + new BigInteger(b).ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>A synthetic patient with fresh UIDs.</summary>
    /// <param name="name">Name suffix (e.g. "CT").</param>
    /// <param name="id">Patient ID.</param>
    /// <returns>The patient.</returns>
    public static SamplePatient Patient(string name, string id) => new("SAMPLE^" + name, id, NewUid(), Accession: "A" + id);

    private static TkFile Base(SamplePatient p, string sopClass, string modality, string seriesUid, int seriesNumber, int instanceNumber, string seriesDescription)
    {
        TkFile f = TkFile.Create();
        try
        {
            f.SetRawString(Tags.SopClassUid, "UI", sopClass);
            f.SetRawString(Tags.SopInstanceUid, "UI", NewUid());
            f.SetRawString(Tags.StudyInstanceUid, "UI", p.StudyUid);
            f.SetRawString(Tags.SeriesInstanceUid, "UI", seriesUid);
            f.SetString(Tags.PatientName, p.PatientName);
            f.SetString(Tags.PatientId, p.PatientId);
            f.SetString(Tags.PatientSex, p.Sex);
            f.SetString(Tags.PatientBirthDate, p.BirthDate);
            f.SetString(Tags.StudyDate, p.StudyDate);
            f.SetString(Tags.StudyTime, "101500");
            f.SetString(Tags.AccessionNumber, p.Accession);
            f.SetString(Tags.StudyDescription, "SYNTHETIC " + modality + " PHANTOM");
            f.SetString(Tags.SeriesDescription, seriesDescription);
            f.SetString(Tags.Modality, modality);
            f.SetString(Tags.InstitutionName, "SIAARA TEST DATA");
            f.SetString(Tags.SeriesNumber, seriesNumber.ToString(CultureInfo.InvariantCulture));
            f.SetString(Tags.InstanceNumber, instanceNumber.ToString(CultureInfo.InvariantCulture));
            return f;
        }
        catch
        {
            f.Dispose();
            throw;
        }
    }

    private static void Gray16(TkFile f, int rows, int cols, short[] stored, bool signed)
    {
        f.SetUInt16(Tags.SamplesPerPixel, 1);
        f.SetString(Tags.PhotometricInterpretation, "MONOCHROME2");
        f.SetUInt16(Tags.Rows, (ushort)rows);
        f.SetUInt16(Tags.Columns, (ushort)cols);
        f.SetUInt16(Tags.BitsAllocated, 16);
        f.SetUInt16(Tags.BitsStored, 16);
        f.SetUInt16(Tags.HighBit, 15);
        f.SetUInt16(Tags.PixelRepresentation, (ushort)(signed ? 1 : 0));
        f.SetBytes(Tags.PixelData, "OW", MemoryMarshal.AsBytes(stored.AsSpan()));
    }

    /// <summary>HU of the CT phantom at (x, y, z): air outside a water cylinder with a bone ring and a lung-like insert.</summary>
    /// <param name="x">Column.</param>
    /// <param name="y">Row.</param>
    /// <param name="z">Slice.</param>
    /// <param name="n">Image size.</param>
    /// <returns>HU.</returns>
    public static short PhantomHu(int x, int y, int z, int n)
    {
        double cx = (x - (n / 2.0)) / (n / 2.0), cy = (y - (n / 2.0)) / (n / 2.0);
        double r = Math.Sqrt((cx * cx) + (cy * cy));
        if (r > 0.9)
        {
            return -1000;
        }
        if (r > 0.82)
        {
            return 700;
        }
        double ix = cx - 0.35, iy = cy + (0.1 * Math.Sin(z / 20.0));
        if ((ix * ix) + (iy * iy) < 0.04)
        {
            return -700;
        }
        return (short)(40 + (20 * Math.Sin((x + z) / 15.0)));
    }

    /// <summary>Writes an axial CT series.</summary>
    /// <param name="dir">Folder.</param>
    /// <param name="p">Patient.</param>
    /// <param name="slices">Slices.</param>
    /// <param name="size">Rows = columns.</param>
    /// <param name="spacingMm">Pixel spacing.</param>
    /// <param name="sliceGapMm">Distance between slices.</param>
    /// <param name="seriesNumber">Series Number.</param>
    /// <param name="positionOf">Optional slice position override (uneven spacing tests).</param>
    /// <param name="lossyFlag">Set Lossy Image Compression "01".</param>
    /// <returns>Paths.</returns>
    public static IReadOnlyList<string> WriteCtSeries(string dir, SamplePatient p, int slices, int size = 64, double spacingMm = 0.7,
        double sliceGapMm = 2.0, int seriesNumber = 1, Func<int, double>? positionOf = null, bool lossyFlag = false)
    {
        ArgumentNullException.ThrowIfNull(p);
        Directory.CreateDirectory(dir);
        string series = NewUid(), frameOfRef = NewUid();
        var paths = new string[slices];
        var px = new short[size * size];
        for (int k = 0; k < slices; k++)
        {
            for (int j = 0; j < size; j++)
            {
                for (int i = 0; i < size; i++)
                {
                    px[(j * size) + i] = (short)(PhantomHu(i, j, k, size) + 1024);
                }
            }
            using TkFile f = Base(p, CtImageStorage, "CT", series, seriesNumber, k + 1, "AXIAL " + size.ToString(CultureInfo.InvariantCulture));
            double z = positionOf?.Invoke(k) ?? (k * sliceGapMm);
            f.SetRawString(Tags.FrameOfReferenceUid, "UI", frameOfRef);
            f.SetString(Tags.ImagePositionPatient, "0\\0\\" + z.ToString("0.###", CultureInfo.InvariantCulture));
            f.SetString(Tags.ImageOrientationPatient, "1\\0\\0\\0\\1\\0");
            f.SetString(Tags.PixelSpacing, spacingMm.ToString(CultureInfo.InvariantCulture) + "\\" + spacingMm.ToString(CultureInfo.InvariantCulture));
            f.SetString(Tags.SliceThickness, sliceGapMm.ToString(CultureInfo.InvariantCulture));
            f.SetString(Tags.SliceLocation, z.ToString("0.###", CultureInfo.InvariantCulture));
            f.SetString(Tags.Kvp, "120");
            f.SetString(Tags.XRayTubeCurrent, "200");
            f.SetString(Tags.WindowCenter, "40");
            f.SetString(Tags.WindowWidth, "400");
            f.SetString(Tags.RescaleIntercept, "-1024");
            f.SetString(Tags.RescaleSlope, "1");
            f.SetString(Tags.RescaleType, "HU");
            if (lossyFlag)
            {
                f.SetString(Tags.LossyImageCompression, "01");
            }
            Gray16(f, size, size, px, signed: true);
            paths[k] = Path.Combine(dir, "CT" + seriesNumber.ToString(CultureInfo.InvariantCulture) + "_" + k.ToString("D4", CultureInfo.InvariantCulture) + ".dcm");
            f.Save(paths[k]);
        }
        return paths;
    }

    /// <summary>Writes a multi-frame 8-bit ultrasound clip with Frame Time (cine at its own rate).</summary>
    /// <param name="path">File.</param>
    /// <param name="p">Patient.</param>
    /// <param name="frames">Frames.</param>
    /// <param name="rows">Rows.</param>
    /// <param name="cols">Columns.</param>
    /// <param name="frameTimeMs">Frame Time (ms).</param>
    /// <returns>The path.</returns>
    public static string WriteUsClip(string path, SamplePatient p, int frames = 30, int rows = 120, int cols = 160, double frameTimeMs = 40)
    {
        ArgumentNullException.ThrowIfNull(p);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".");
        using TkFile f = Base(p, UsMultiFrameStorage, "US", NewUid(), 1, 1, "CINE CLIP");
        var px = new byte[frames * rows * cols];
        for (int k = 0; k < frames; k++)
        {
            double phase = 2 * Math.PI * k / frames;
            for (int j = 0; j < rows; j++)
            {
                for (int i = 0; i < cols; i++)
                {
                    double dx = i - (cols / 2.0) - (20 * Math.Cos(phase)), dy = j - (rows / 2.0) - (10 * Math.Sin(phase));
                    double v = 60 + (150 * Math.Exp(-((dx * dx) + (dy * dy)) / 200.0)) + ((i * 7 + j * 13 + k) % 23);
                    px[(k * rows * cols) + (j * cols) + i] = (byte)Math.Clamp(v, 0, 255);
                }
            }
        }
        f.SetUInt16(Tags.SamplesPerPixel, 1);
        f.SetString(Tags.PhotometricInterpretation, "MONOCHROME2");
        f.SetUInt16(Tags.Rows, (ushort)rows);
        f.SetUInt16(Tags.Columns, (ushort)cols);
        f.SetUInt16(Tags.BitsAllocated, 8);
        f.SetUInt16(Tags.BitsStored, 8);
        f.SetUInt16(Tags.HighBit, 7);
        f.SetUInt16(Tags.PixelRepresentation, 0);
        f.SetString(Tags.NumberOfFrames, frames.ToString(CultureInfo.InvariantCulture));
        f.SetString(Tags.FrameTime, frameTimeMs.ToString(CultureInfo.InvariantCulture));
        f.SetBytes(Tags.FrameIncrementPointer, "AT", [0x18, 0x00, 0x63, 0x10]);
        f.SetBytes(Tags.PixelData, "OB", px);
        f.Save(path);
        return path;
    }

    /// <summary>Writes a PET slice (Bq/ml, with or without patient weight).</summary>
    /// <param name="path">File.</param>
    /// <param name="p">Patient.</param>
    /// <param name="weightKg">Patient's Weight, or null to leave it out.</param>
    /// <param name="valueBqMl">Activity concentration of the hot region.</param>
    /// <param name="size">Rows = columns.</param>
    /// <returns>The path.</returns>
    public static string WritePet(string path, SamplePatient p, double? weightKg, double valueBqMl = 10000, int size = 32)
    {
        ArgumentNullException.ThrowIfNull(p);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".");
        using TkFile f = Base(p, PetImageStorage, "PT", NewUid(), 2, 1, "PET AC");
        var px = new short[size * size];
        for (int j = 0; j < size; j++)
        {
            for (int i = 0; i < size; i++)
            {
                bool hot = i >= size / 4 && i < 3 * size / 4 && j >= size / 4 && j < 3 * size / 4;
                px[(j * size) + i] = (short)(hot ? valueBqMl / 10 : 100);
            }
        }
        f.SetString(Tags.SeriesDate, p.StudyDate);
        f.SetString(Tags.SeriesTime, "110000");
        f.SetString(Tags.AcquisitionDate, p.StudyDate);
        f.SetString(Tags.AcquisitionTime, "110000");
        f.SetString(Tags.Units, "BQML");
        f.SetString(Tags.DecayCorrection, "START");
        f.SetString(Tags.CorrectedImage, "ATTN\\DECY");
        f.SetString(Tags.RescaleSlope, "10");
        f.SetString(Tags.RescaleIntercept, "0");
        f.SetString(Tags.PixelSpacing, "4\\4");
        f.SetString(Tags.ImagePositionPatient, "0\\0\\0");
        f.SetString(Tags.ImageOrientationPatient, "1\\0\\0\\0\\1\\0");
        f.SetString(Tags.SliceThickness, "4");
        f.SetString(Tags.PatientSize, "1.75");
        if (weightKg is double w)
        {
            f.SetString(Tags.PatientWeight, w.ToString(CultureInfo.InvariantCulture));
        }
        f.AppendItem(Tags.RadiopharmaceuticalInformationSequence,
        [
            (Tags.RadiopharmaceuticalStartTime, "100000"),
            (Tags.RadionuclideTotalDose, "370000000"),
            (Tags.RadionuclideHalfLife, "6586.2"),
        ]);
        Gray16(f, size, size, px, signed: true);
        f.Save(path);
        return path;
    }

    /// <summary>Writes a CR image without any pixel spacing (measurements in pixels).</summary>
    /// <param name="path">File.</param>
    /// <param name="p">Patient.</param>
    /// <param name="size">Rows = columns.</param>
    /// <returns>The path.</returns>
    public static string WriteCrNoSpacing(string path, SamplePatient p, int size = 64)
    {
        ArgumentNullException.ThrowIfNull(p);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".");
        using TkFile f = Base(p, CrImageStorage, "CR", NewUid(), 3, 1, "CHEST PA");
        var px = new short[size * size];
        for (int i = 0; i < px.Length; i++)
        {
            px[i] = (short)(i % size * 16);
        }
        Gray16(f, size, size, px, signed: false);
        f.Save(path);
        return path;
    }

    /// <summary>Writes a complete demo set: 500-slice CT (512x512 when full), US clip, PET, CR, plus a DICOMDIR copy.</summary>
    /// <param name="dir">Output folder.</param>
    /// <param name="full">512x512 x 500 slices; otherwise a small set.</param>
    /// <returns>Files written (DICOMDIR copy excluded).</returns>
    public static IReadOnlyList<string> WriteDemoSet(string dir, bool full)
    {
        var files = new List<string>();
        SamplePatient ct = Patient("CT^PHANTOM", "SIM0001");
        files.AddRange(WriteCtSeries(Path.Combine(dir, "folder", "ct"), ct, full ? 500 : 60, full ? 512 : 128, full ? 0.7 : 1.5, 1.0));
        files.Add(WriteUsClip(Path.Combine(dir, "folder", "us", "clip.dcm"), Patient("US^PHANTOM", "SIM0002")));
        SamplePatient pt = Patient("PET^PHANTOM", "SIM0003");
        files.Add(WritePet(Path.Combine(dir, "folder", "pet", "pet.dcm"), pt, 70));
        files.Add(WriteCrNoSpacing(Path.Combine(dir, "folder", "cr", "cr.dcm"), Patient("CR^PHANTOM", "SIM0004")));
        File.WriteAllText(Path.Combine(dir, "folder", "README.txt"), "Synthetic SIAARA sample data. Not patient data.");
        Tk.WriteDicomdir(Path.Combine(dir, "dicomdir-set"), files);
        return files;
    }
}
