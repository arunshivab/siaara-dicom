using System;
using System.Linq;
using Siaara.Clinical;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class ClinicalTests
{
    // The worked example of tests/test_suv.cpp (F-18, 370 MBq, 75 kg, 1.80 m, male, 1 h decay).
    private const string PetJson =
        "{\"00080016\":{\"vr\":\"UI\",\"Value\":[\"1.2.840.10008.5.1.4.1.1.128\"]}," +
        "\"00080018\":{\"vr\":\"UI\",\"Value\":[\"1.2.3.4.5\"]}," +
        "\"00080021\":{\"vr\":\"DA\",\"Value\":[\"20260301\"]}," +
        "\"00080022\":{\"vr\":\"DA\",\"Value\":[\"20260301\"]}," +
        "\"00080031\":{\"vr\":\"TM\",\"Value\":[\"103000\"]}," +
        "\"00080032\":{\"vr\":\"TM\",\"Value\":[\"103512\"]}," +
        "\"00100040\":{\"vr\":\"CS\",\"Value\":[\"M\"]}," +
        "\"00101020\":{\"vr\":\"DS\",\"Value\":[1.80]}," +
        "\"00101030\":{\"vr\":\"DS\",\"Value\":[75]}," +
        "\"00280051\":{\"vr\":\"CS\",\"Value\":[\"ATTN\",\"DECY\"]}," +
        "\"00541001\":{\"vr\":\"CS\",\"Value\":[\"BQML\"]}," +
        "\"00541102\":{\"vr\":\"CS\",\"Value\":[\"START\"]}," +
        "\"00540016\":{\"vr\":\"SQ\",\"Value\":[{" +
        "\"00181072\":{\"vr\":\"TM\",\"Value\":[\"093000\"]}," +
        "\"00181074\":{\"vr\":\"DS\",\"Value\":[370000000]}," +
        "\"00181075\":{\"vr\":\"DS\",\"Value\":[6586.2]}," +
        "\"00181078\":{\"vr\":\"DT\",\"Value\":[\"20260301093000\"]}}]}}";

    private static bool Rel(double a, double b, double r = 1e-9) => Math.Abs(a - b) <= r * Math.Abs(b);

    internal static void Register()
    {
        Case("clinical: SUV factors (worked example of test_suv.cpp)", () =>
        {
            using DicomFile f = DicomFile.FromJson(PetJson);
            SuvFactors s = SuvFactors.Compute(f);
            Require(s.BodyWeight is not null);
            Check(Rel(s.BodyWeight!.Value, 2.96075016126046e-4));
            Check(Rel(s.LeanBodyMassJames!.Value, 2.37956587034637e-4));
            Check(Rel(s.LeanBodyMassJanmahasatian!.Value, 2.34984195161682e-4));
            Check(Rel(s.BodySurfaceArea!.Value, 7.66797035188891e-5));
            Check(Rel(s.IdealBodyWeight!.Value, 2.96043932134852e-4));
            CheckNear(s.DecaySeconds, 3600, 1e-9);
            Check(Rel(s.DecayedDoseBq, 253314180.241641));
            f.Dataset.Remove(DicomTags.PatientWeight);
            SuvFactors none = SuvFactors.Compute(f);
            Check(none.BodyWeight is null);
            Check(none.Warnings.HasFlag(SuvWarnings.MissingWeight));
        });

        Case("clinical: RT Dose values in Gy and the dose module (testing2 P0-2)", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset d = f.Dataset;
            d.SetRawString(DicomTags.SOPClassUID, "UI", "1.2.840.10008.5.1.4.1.1.481.2");
            d.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.77");
            d.SetRawString(DicomTags.Modality, "CS", "RTDOSE");
            d.SetUInt16(DicomTags.SamplesPerPixel, 1);
            d.SetRawString(DicomTags.PhotometricInterpretation, "CS", "MONOCHROME2");
            d.SetUInt16(DicomTags.Rows, 1);
            d.SetUInt16(DicomTags.Columns, 2);
            d.SetUInt16(DicomTags.BitsAllocated, 32);
            d.SetUInt16(DicomTags.BitsStored, 32);
            d.SetUInt16(DicomTags.HighBit, 31);
            d.SetUInt16(DicomTags.PixelRepresentation, 0);
            d.SetRawString(new DicomTag(0x30040002), "CS", "GY");
            d.SetRawString(new DicomTag(0x30040004), "CS", "PHYSICAL");
            d.SetRawString(new DicomTag(0x3004000A), "CS", "PLAN");
            d.SetRawString(new DicomTag(0x3004000E), "DS", "4.574449e-05");
            byte[] px = new byte[8];
            BitConverter.TryWriteBytes(px.AsSpan(0), 1861000u);
            BitConverter.TryWriteBytes(px.AsSpan(4), 1000u);
            d.SetBytes(DicomTags.PixelData, "OW", px);
            float[] v = f.DecodeFrameModality(0);
            CheckNear(v[0], 85.13049589, 1e-4);  // 1861000 x 4.574449e-05 Gy
            CheckNear(v[1], 0.04574449, 1e-7);
            RtDoseInfo info = RtDoseInfo.Read(d);
            Check(info.IsRtDose);
            CheckEq(info.DoseGridScaling, 4.574449e-05);
            CheckEq(info.DoseUnits, "GY");
            CheckEq(info.DoseType, "PHYSICAL");
            CheckEq(info.DoseSummationType, "PLAN");
            CheckEq(info.GridFrameOffsetForm, GridFrameOffsetForm.None);
        });

        Case("clinical: ROI statistics (rectangle, circle, polygon) with area in mm2", () =>
        {
            var px = new short[4 * 4];
            for (int i = 0; i < 16; i++)
            {
                px[i] = (short)(1024 + i * 10); // HU 0, 10, ... 150
            }
            using DicomFile f = Builders.Ct(4, 4, px);
            f.Dataset.SetString(DicomTags.PixelSpacing, "0.5\\0.5");
            RoiStatistics r = RoiStatistics.Compute(f, 0, RoiShape.Rectangle, new double[] { 0, 0, 2, 2 });
            CheckEq(r.Pixels, 4L);
            CheckNear(r.Mean, (0 + 10 + 40 + 50) / 4.0, 1e-9);
            CheckNear(r.Min, 0, 1e-9);
            CheckNear(r.Max, 50, 1e-9);
            CheckNear(r.AreaPixels, 4, 1e-9);
            Require(r.AreaSquareMillimetres is not null);
            CheckNear(r.AreaSquareMillimetres!.Value, 1.0, 1e-9);
            CheckEq(r.SpacingSource, SpacingSource.PixelSpacing);
            RoiStatistics full = RoiStatistics.Compute(f, 0, RoiShape.Polygon, new double[] { 0, 0, 4, 0, 4, 4, 0, 4 });
            CheckEq(full.Pixels, 16L);
            CheckNear(full.Mean, 75, 1e-9);
            RoiStatistics c = RoiStatistics.Compute(f, 0, RoiShape.Circle, new double[] { 2, 2, 1.5 }, partialVolume: true);
            Check(c.Count > 0 && c.Count < 16);
            CheckEq(Throws<SiaaraException>(() => RoiStatistics.Compute(f, 0, RoiShape.Polygon, new double[] { 0, 0, 1, 1 }))?.ErrorCode,
                SiaaraErrorCode.InvalidArgument);
        });

        Case("clinical: frame geometry, orientation labels, series sort", () =>
        {
            string dir = NewTempDir("geom");
            string[] paths = Builders.CtSeries(dir, 4, 4, 4, (i, j, k) => 0);
            using DicomFile f0 = DicomFile.Open(paths[2]);
            PlaneGeometry g = ImageGeometry.GetFrameGeometry(f0);
            Check(g.HasPosition && g.HasOrientation);
            CheckEq(g.Position, new Point3D(0, 0, 4));
            CheckEq(g.Normal, new Point3D(0, 0, 1));
            CheckNear(g.RowSpacing, 1, 1e-12);
            CheckEq(g.SpacingSource, SpacingSource.PixelSpacing);
            OrientationLabels? labels = ImageGeometry.GetOrientationLabels(f0);
            Require(labels is not null);
            CheckEq(labels!.Left, "R");
            CheckEq(labels.Right, "L");
            CheckEq(labels.Top, "A");
            CheckEq(labels.Bottom, "P");
            using DicomFile bare = Builders.Smoke();
            Check(ImageGeometry.GetOrientationLabels(bare) is null);
            var files = new DicomFile?[] { DicomFile.Open(paths[3]), null, DicomFile.Open(paths[0]), DicomFile.Open(paths[2]), DicomFile.Open(paths[1]) };
            try
            {
                SeriesSortResult s = ImageGeometry.SortSeries(files);
                CheckEq(s.Slices.Count, 4);
                Check(s.Slices.Select(x => x.FileIndex).SequenceEqual(new[] { 2, 4, 3, 0 }));
                CheckEq(s.Warnings, SeriesSortWarnings.None);
            }
            finally
            {
                foreach (DicomFile? f in files)
                {
                    f?.Dispose();
                }
            }
        });

        Case("clinical: SR content tree", () =>
        {
            const string json =
                "{\"00080016\":{\"vr\":\"UI\",\"Value\":[\"1.2.840.10008.5.1.4.1.1.88.11\"]}," +
                "\"00080018\":{\"vr\":\"UI\",\"Value\":[\"2.25.1\"]}," +
                "\"0040A040\":{\"vr\":\"CS\",\"Value\":[\"CONTAINER\"]}," +
                "\"0040A050\":{\"vr\":\"CS\",\"Value\":[\"SEPARATE\"]}," +
                "\"0040A043\":{\"vr\":\"SQ\",\"Value\":[{\"00080100\":{\"vr\":\"SH\",\"Value\":[\"18748-4\"]},\"00080102\":{\"vr\":\"SH\",\"Value\":[\"LN\"]},\"00080104\":{\"vr\":\"LO\",\"Value\":[\"Diagnostic Imaging Report\"]}}]}," +
                "\"0040A730\":{\"vr\":\"SQ\",\"Value\":[" +
                "{\"0040A010\":{\"vr\":\"CS\",\"Value\":[\"CONTAINS\"]},\"0040A040\":{\"vr\":\"CS\",\"Value\":[\"TEXT\"]}," +
                "\"0040A043\":{\"vr\":\"SQ\",\"Value\":[{\"00080100\":{\"vr\":\"SH\",\"Value\":[\"121071\"]},\"00080102\":{\"vr\":\"SH\",\"Value\":[\"DCM\"]},\"00080104\":{\"vr\":\"LO\",\"Value\":[\"Finding\"]}}]}," +
                "\"0040A160\":{\"vr\":\"UT\",\"Value\":[\"No acute abnormality\"]}}," +
                "{\"0040A010\":{\"vr\":\"CS\",\"Value\":[\"CONTAINS\"]},\"0040A040\":{\"vr\":\"CS\",\"Value\":[\"NUM\"]}," +
                "\"0040A043\":{\"vr\":\"SQ\",\"Value\":[{\"00080100\":{\"vr\":\"SH\",\"Value\":[\"G-D705\"]},\"00080102\":{\"vr\":\"SH\",\"Value\":[\"SRT\"]},\"00080104\":{\"vr\":\"LO\",\"Value\":[\"Volume\"]}}]}," +
                "\"0040A300\":{\"vr\":\"SQ\",\"Value\":[{\"0040A30A\":{\"vr\":\"DS\",\"Value\":[12.5]},\"004008EA\":{\"vr\":\"SQ\",\"Value\":[{\"00080100\":{\"vr\":\"SH\",\"Value\":[\"cm3\"]},\"00080102\":{\"vr\":\"SH\",\"Value\":[\"UCUM\"]},\"00080104\":{\"vr\":\"LO\",\"Value\":[\"cubic centimeter\"]}}]}}]}}" +
                "]}}";
            using DicomFile f = DicomFile.FromJson(json);
            using StructuredReport sr = StructuredReport.Open(f);
            CheckEq(sr.NodeCount, 3);
            SrNode root = sr.GetNode(0);
            CheckEq(root.ValueType, SrValueType.Container);
            CheckEq(root.Parent, -1);
            CheckEq(root.ChildCount, 2);
            Check(sr.GetChildren(0).SequenceEqual(new[] { 1, 2 }));
            CheckEq(sr.GetString(0, SrField.ConceptMeaning), "Diagnostic Imaging Report");
            CheckEq(sr.GetString(1, SrField.Text), "No acute abnormality");
            SrNode num = sr.GetNode(2);
            CheckEq(num.ValueType, SrValueType.Num);
            CheckEq(num.Relationship, SrRelationship.Contains);
            CheckEq(num.Numeric, 12.5);
            CheckEq(sr.GetString(2, SrField.UnitsValue), "cm3");
            CheckEq(sr.GetGraphicData(2).Length, 0);
            Check(sr.ToText().Contains("No acute abnormality", StringComparison.Ordinal));
        });

        Case("clinical: RT Structure Set ROIs and contours", () =>
        {
            const string json =
                "{\"00080016\":{\"vr\":\"UI\",\"Value\":[\"1.2.840.10008.5.1.4.1.1.481.3\"]}," +
                "\"00080018\":{\"vr\":\"UI\",\"Value\":[\"2.25.2\"]}," +
                "\"30060020\":{\"vr\":\"SQ\",\"Value\":[{\"30060022\":{\"vr\":\"IS\",\"Value\":[7]},\"30060026\":{\"vr\":\"LO\",\"Value\":[\"PTV\"]}}]}," +
                "\"30060039\":{\"vr\":\"SQ\",\"Value\":[{\"30060084\":{\"vr\":\"IS\",\"Value\":[7]},\"3006002A\":{\"vr\":\"IS\",\"Value\":[255,0,0]}," +
                "\"30060040\":{\"vr\":\"SQ\",\"Value\":[{\"30060042\":{\"vr\":\"CS\",\"Value\":[\"CLOSED_PLANAR\"]},\"30060046\":{\"vr\":\"IS\",\"Value\":[3]}," +
                "\"30060050\":{\"vr\":\"DS\",\"Value\":[0,0,5,10,0,5,10,10,5]}}]}}]}}";
            using DicomFile f = DicomFile.FromJson(json);
            using RtStructureSet rt = RtStructureSet.Open(f);
            Require(rt.Rois.Count == 1);
            RtRoi roi = rt.Rois[0];
            CheckEq(roi.Number, 7);
            CheckEq(roi.Name, "PTV");
            CheckEq(roi.ContourCount, 1);
            CheckEq(roi.Color, ((byte)255, (byte)0, (byte)0));
            Point3D[] pts = rt.GetContourPoints(0, 0, out bool closed);
            Check(closed);
            CheckEq(pts.Length, 3);
            CheckEq(pts[1], new Point3D(10, 0, 5));
        });

        Case("clinical: SEG open errors on a non-SEG file", () =>
        {
            using DicomFile f = Builders.Smoke();
            Check(Throws<SiaaraException>(() => Segmentation.Open(f).Dispose()) is not null);
            Check(Throws<SiaaraException>(() => StructuredReport.Open(f).Dispose()) is not null);
        });
    }
}
