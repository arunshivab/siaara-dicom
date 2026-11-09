using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

namespace Siaara.Tests;

/// <summary>Synthetic test objects (the repository has no binary test data).</summary>
internal static class Builders
{
    internal const string CtImageStorage = "1.2.840.10008.5.1.4.1.1.2";
    internal const string SecondaryCapture = "1.2.840.10008.5.1.4.1.1.7";

    /// <summary>A signed 16-bit MONOCHROME2 image with rescale -1024 and window 0/100.</summary>
    internal static DicomFile Ct(int rows, int columns, short[] stored, string sopUid = "2.25.100", string seriesUid = "2.25.200",
        string studyUid = "2.25.300", string patientName = "Doe^Jane", string patientId = "P1")
    {
        DicomFile f = DicomFile.Create();
        try
        {
            DicomDataset ds = f.Dataset;
            ds.SetRawString(DicomTags.SOPClassUID, "UI", CtImageStorage);
            ds.SetRawString(DicomTags.SOPInstanceUID, "UI", sopUid);
            ds.SetRawString(DicomTags.StudyInstanceUID, "UI", studyUid);
            ds.SetRawString(DicomTags.SeriesInstanceUID, "UI", seriesUid);
            ds.SetString(DicomTags.PatientName, patientName);
            ds.SetString(DicomTags.PatientID, patientId);
            ds.SetString(DicomTags.Modality, "CT");
            ds.SetString(DicomTags.StudyDate, "20260301");
            ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
            ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
            ds.SetUInt16(DicomTags.Rows, (ushort)rows);
            ds.SetUInt16(DicomTags.Columns, (ushort)columns);
            ds.SetUInt16(DicomTags.BitsAllocated, 16);
            ds.SetUInt16(DicomTags.BitsStored, 16);
            ds.SetUInt16(DicomTags.HighBit, 15);
            ds.SetUInt16(DicomTags.PixelRepresentation, 1);
            ds.SetString(DicomTags.WindowCenter, "0");
            ds.SetString(DicomTags.WindowWidth, "100");
            ds.SetString(DicomTags.RescaleIntercept, "-1024");
            ds.SetString(DicomTags.RescaleSlope, "1");
            ds.SetBytes(DicomTags.PixelData, "OW", MemoryMarshal.AsBytes(stored.AsSpan()));
            return f;
        }
        catch
        {
            f.Dispose();
            throw;
        }
    }

    /// <summary>The 2x2 image of tests/capi_smoke.c: stored 1024, 1074, 974, 32767 (HU 0, 50, -50, 31743).</summary>
    internal static DicomFile Smoke() => Ct(2, 2, new short[] { 1024, 1074, 974, 32767 });

    /// <summary>An 8-bit RGB secondary capture.</summary>
    internal static DicomFile Rgb(int rows, int columns)
    {
        DicomFile f = DicomFile.Create();
        try
        {
            DicomDataset ds = f.Dataset;
            ds.SetRawString(DicomTags.SOPClassUID, "UI", SecondaryCapture);
            ds.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.900");
            ds.SetUInt16(DicomTags.SamplesPerPixel, 3);
            ds.SetString(DicomTags.PhotometricInterpretation, "RGB");
            ds.SetUInt16(DicomTags.PlanarConfiguration, 0);
            ds.SetUInt16(DicomTags.Rows, (ushort)rows);
            ds.SetUInt16(DicomTags.Columns, (ushort)columns);
            ds.SetUInt16(DicomTags.BitsAllocated, 8);
            ds.SetUInt16(DicomTags.BitsStored, 8);
            ds.SetUInt16(DicomTags.HighBit, 7);
            ds.SetUInt16(DicomTags.PixelRepresentation, 0);
            byte[] px = new byte[rows * columns * 3];
            for (int i = 0; i < px.Length; i++)
            {
                px[i] = (byte)(i * 7 % 251);
            }
            ds.SetBytes(DicomTags.PixelData, "OB", px);
            return f;
        }
        catch
        {
            f.Dispose();
            throw;
        }
    }

    /// <summary>Writes a CT series of `slices` axial slices (rows x cols, 1 mm pixels, 2 mm apart) to dir.</summary>
    internal static string[] CtSeries(string dir, int slices, int rows, int cols, Func<int, int, int, short> hu,
        string seriesUid = "2.25.5000", string forUid = "2.25.5001")
    {
        var paths = new string[slices];
        for (int k = 0; k < slices; k++)
        {
            var px = new short[rows * cols];
            for (int j = 0; j < rows; j++)
            {
                for (int i = 0; i < cols; i++)
                {
                    px[j * cols + i] = (short)(hu(i, j, k) + 1024);
                }
            }
            using DicomFile f = Ct(rows, cols, px, "2.25.5000." + (k + 1).ToString(CultureInfo.InvariantCulture), seriesUid);
            DicomDataset ds = f.Dataset;
            ds.SetRawString(DicomTags.FrameOfReferenceUID, "UI", forUid);
            ds.SetString(DicomTags.ImagePositionPatient, "0\\0\\" + (2.0 * k).ToString(CultureInfo.InvariantCulture));
            ds.SetString(DicomTags.ImageOrientationPatient, "1\\0\\0\\0\\1\\0");
            ds.SetString(DicomTags.PixelSpacing, "1\\1");
            ds.SetString(DicomTags.SliceThickness, "2");
            ds.SetString(DicomTags.InstanceNumber, (k + 1).ToString(CultureInfo.InvariantCulture));
            ds.SetString(DicomTags.SeriesNumber, "1");
            paths[k] = Path.Combine(dir, "ct" + k.ToString("D3", CultureInfo.InvariantCulture) + ".dcm");
            f.Save(paths[k], DicomTransferSyntaxes.ExplicitVRLittleEndian);
        }
        return paths;
    }
}
