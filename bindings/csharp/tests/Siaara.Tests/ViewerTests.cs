using System;
using System.Linq;
using System.Runtime.InteropServices;
using Siaara.Volumes;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>DicomFrame (decode once, viewport rendering), VOI, overlays, palettes, sequences, live handles.</summary>
internal static class ViewerTests
{
    private static DicomFile Gray8(int rows, int columns, byte[] pixels, string photometric = "MONOCHROME2")
    {
        DicomFile f = DicomFile.Create();
        DicomDataset ds = f.Dataset;
        ds.SetRawString(DicomTags.SOPClassUID, "UI", Builders.SecondaryCapture);
        ds.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.4242");
        ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
        ds.SetString(DicomTags.PhotometricInterpretation, photometric);
        ds.SetUInt16(DicomTags.Rows, (ushort)rows);
        ds.SetUInt16(DicomTags.Columns, (ushort)columns);
        ds.SetUInt16(DicomTags.BitsAllocated, 8);
        ds.SetUInt16(DicomTags.BitsStored, 8);
        ds.SetUInt16(DicomTags.HighBit, 7);
        ds.SetUInt16(DicomTags.PixelRepresentation, 0);
        ds.SetBytes(DicomTags.PixelData, "OB", pixels);
        return f;
    }

    private static ViewportOptions Gray(int w, int h) =>
        new() { Width = w, Height = h, Format = PixelOutputFormat.Gray8, VoiMode = VoiMode.Identity, Background = new(7, 0, 0) };

    internal static void Register()
    {
        Case("viewer: frame decode once, properties, render equals RenderRgba8", () =>
        {
            long before = SiaaraLibrary.LiveHandleCount;
            using DicomFile f = Builders.Smoke();
            f.Dataset.SetString(DicomTags.WindowCenter, "0\\40");
            f.Dataset.SetString(DicomTags.WindowWidth, "100\\400");
            f.Dataset.SetString(DicomTags.WindowCenterWidthExplanation, "A\\SOFT");
            byte[] expect = f.RenderRgba8();
            using DicomFrame fr = f.OpenFrame();
            CheckEq(SiaaraLibrary.LiveHandleCount, before + 2);
            FrameProperties p = fr.Properties;
            CheckEq(p.Rows, 2);
            CheckEq(p.Columns, 2);
            Check(p.Flags.HasFlag(FrameCharacteristics.Monochrome));
            CheckEq(p.RescaleIntercept, -1024.0);
            CheckEq(p.MinValue, -50.0);
            CheckEq(p.MaxValue, 31743.0);
            CheckEq(p.VoiCount, 2);
            CheckEq(p.PhotometricInterpretation, "MONOCHROME2");
            Check(fr.Render().SequenceEqual(expect));
            var voi = fr.VoiEntries;
            CheckEq(voi.Count, 2);
            CheckEq(voi[1].Explanation, "SOFT");
            CheckEq(voi[1].Center, 40.0);
            Check(!voi[1].IsLut);
            byte[] second = fr.Render(new ViewportOptions { VoiIndex = 1 });
            Check(second.SequenceEqual(f.RenderRgba8(RenderOptions.Window(40, 400))));
            byte[] explicitW = fr.Render(new ViewportOptions().WithWindow(40, 400));
            Check(explicitW.SequenceEqual(second));
            using (var pixels = fr.LeasePixels())
            {
                CheckEq(pixels.Memory.Length, 8);
                Check(pixels.Memory.Span.SequenceEqual(f.DecodeFrame(0)));
            }
            using (var view = f.LeaseNativeFrame(0))
            {
                Check(view.Memory.Span.SequenceEqual(f.DecodeFrame(0)));
            }
            CheckEq(Throws<SiaaraException>(() => fr.Render(new ViewportOptions { VoiIndex = 5 }))?.ErrorCode, SiaaraErrorCode.NotFound);
            CheckEq(Throws<SiaaraException>(() => fr.Render(new ViewportOptions().WithWindow(0, 0.5)))?.ErrorCode,
                SiaaraErrorCode.InvalidArgument);
            Span<byte> small = stackalloc byte[4];
            CheckEq(fr.TryRender(small, null, out int need), SiaaraErrorCode.BufferTooSmall);
            CheckEq(need, 16);
            CheckEq(Throws<SiaaraException>(() => DicomFrame.Open(f, 1))?.ErrorCode, SiaaraErrorCode.InvalidArgument);
        });

        Case("viewer: zoom, rotation, flips, pan, interpolation and coordinate mapping", () =>
        {
            using DicomFile f = Gray8(2, 2, new byte[] { 10, 20, 30, 40 });
            using DicomFrame fr = DicomFrame.Open(f);
            Check(fr.Render(Gray(4, 4) with { Zoom = 2 }).SequenceEqual(new byte[] { 10, 10, 20, 20, 10, 10, 20, 20, 30, 30, 40, 40, 30, 30, 40, 40 }));
            Check(fr.Render(Gray(2, 2) with { Rotation = 90 }).SequenceEqual(new byte[] { 30, 10, 40, 20 }));
            Check(fr.Render(Gray(2, 2) with { FlipHorizontal = true }).SequenceEqual(new byte[] { 20, 10, 40, 30 }));
            Check(fr.Render(Gray(2, 2) with { FlipVertical = true }).SequenceEqual(new byte[] { 30, 40, 10, 20 }));
            Check(fr.Render(Gray(4, 2) with { Zoom = 1, PanX = 1 }).SequenceEqual(new byte[] { 7, 7, 10, 20, 7, 7, 30, 40 }));
            byte[] lin = fr.Render(Gray(3, 3) with { Zoom = 1.5, Interpolation = Interpolation.Linear });
            CheckEq(lin[4], (byte)25);
            CheckEq(Throws<SiaaraException>(() => fr.Render(Gray(2, 2) with { Interpolation = Interpolation.Cubic }))?.ErrorCode,
                SiaaraErrorCode.Unsupported);
            byte[] rgba = fr.Render(new ViewportOptions { Width = 4, Height = 2, Zoom = 1, VoiMode = VoiMode.Identity, Background = new(1, 2, 3, 4) });
            CheckEq(rgba.Length, 32);
            Check(rgba.Take(8).SequenceEqual(new byte[] { 1, 2, 3, 4, 10, 10, 10, 255 }));
            var o = Gray(4, 4) with { Zoom = 2, Rotation = 90 };
            (double x, double y) = fr.ImageToView(o, 0, 0);
            CheckNear(x, 4, 1e-12);
            CheckNear(y, 0, 1e-12);
            (double u, double v) = fr.ViewToImage(o, x, y);
            CheckNear(u, 0, 1e-12);
            CheckNear(v, 0, 1e-12);
            CheckEq(fr.GetRenderSize(Gray(5, 3)), 15);
        });

        Case("viewer: colour maps, palette colour, overlays, shutters", () =>
        {
            byte[] ramp = Enumerable.Range(0, 256).Select(i => (byte)i).ToArray();
            using (DicomFile f = Gray8(16, 16, ramp))
            using (DicomFrame fr = f.OpenFrame())
            {
                foreach (Colormap cm in Enum.GetValues<Colormap>())
                {
                    byte[] map = DicomFrame.GetColormapRgba8(cm);
                    Check(fr.Render(new ViewportOptions { Colormap = cm, VoiMode = VoiMode.Identity }).SequenceEqual(map));
                }
                CheckEq(fr.GetPalette(), null);
            }
            using (DicomFile f = Gray8(1, 4, new byte[] { 0, 1, 2, 4 }, "PALETTE COLOR"))
            {
                for (uint k = 0; k < 3; k++)
                {
                    f.Dataset.SetBytes(new DicomTag(0x00281101u + k), "US", MemoryMarshal.AsBytes(new ushort[] { 4, 1, 16 }.AsSpan()));
                    ushort b = (ushort)(k * 0x1000);
                    f.Dataset.SetBytes(new DicomTag(0x00281201u + k), "OW",
                        MemoryMarshal.AsBytes(new ushort[] { b, (ushort)(b + 0x4000), (ushort)(b + 0x8000), 0xFFFF }.AsSpan()));
                }
                using DicomFrame fr = f.OpenFrame();
                ColorPalette? pal = fr.GetPalette();
                Require(pal is not null);
                CheckEq(pal!.Entries, 4);
                CheckEq(pal.FirstMapped, 1);
                CheckEq(pal.Bits, 16);
                CheckEq(pal.Rgba[1], (ushort)0x1000);
                CheckEq(pal.Rgba[3], (ushort)0xFFFF);
                Check(fr.Properties.Flags.HasFlag(FrameCharacteristics.Palette));
            }
            using (DicomFile f = Gray8(2, 4, new byte[] { 0, 50, 100, 150, 200, 250, 25, 75 }))
            {
                DicomDataset ds = f.Dataset;
                ds.SetUInt16(new DicomTag(0x60000010u), 2);
                ds.SetUInt16(new DicomTag(0x60000011u), 4);
                ds.SetRawString(new DicomTag(0x60000040u), "CS", "G");
                ds.SetBytes(new DicomTag(0x60000050u), "SS", MemoryMarshal.AsBytes(new short[] { 1, 1 }.AsSpan()));
                ds.SetUInt16(new DicomTag(0x60000100u), 1);
                ds.SetUInt16(new DicomTag(0x60000102u), 0);
                ds.SetBytes(new DicomTag(0x60003000u), "OW", new byte[] { 0x21, 0x00 });
                ds.SetString(DicomTags.ShutterShape, "RECTANGULAR");
                ds.SetString(DicomTags.ShutterLeftVerticalEdge, "2");
                ds.SetString(DicomTags.ShutterRightVerticalEdge, "3");
                ds.SetString(DicomTags.ShutterUpperHorizontalEdge, "1");
                ds.SetString(DicomTags.ShutterLowerHorizontalEdge, "2");
                using DicomFrame fr = f.OpenFrame();
                var ovs = fr.Overlays;
                CheckEq(ovs.Count, 1);
                CheckEq(ovs[0].Group, 0x6000);
                CheckEq(ovs[0].Type, 'G');
                Check(ovs[0].CoversFrame);
                Check(fr.GetOverlayMask(0).SequenceEqual(new byte[] { 1, 0, 0, 0, 0, 1, 0, 0 }));
                Check(fr.GetShutterMask().SequenceEqual(new byte[] { 0, 1, 1, 0, 0, 1, 1, 0 }));
                Check(fr.Properties.Flags.HasFlag(FrameCharacteristics.RectangularShutter));
                Check(fr.Render(Gray(0, 0) with { NoOverlays = true }).SequenceEqual(new byte[] { 0, 50, 100, 0, 0, 250, 25, 0 }));
                byte[] red = fr.Render(new ViewportOptions { VoiMode = VoiMode.Identity, NoShutters = true, OverlayColor = new(255, 0, 0) });
                Check(red.Take(4).SequenceEqual(new byte[] { 255, 0, 0, 255 }));
            }
        });

        Case("viewer: sequence items, multi-valued strings, live handles", () =>
        {
            long before = SiaaraLibrary.LiveHandleCount;
            using (DicomFile f = DicomFile.Create())
            {
                DicomDataset ds = f.Dataset;
                DicomDataset a = ds.AppendItem(DicomTags.ReferencedImageSequence);
                a.SetRawString(DicomTags.ReferencedSOPInstanceUID, "UI", "2.25.1");
                DicomDataset b = ds.AppendItem(DicomTags.ReferencedImageSequence);
                b.SetRawString(DicomTags.ReferencedSOPInstanceUID, "UI", "2.25.2");
                CheckEq(b.ItemIndex, 1);
                CheckEq(ds.GetItemCount(DicomTags.ReferencedImageSequence), 2);
                CheckEq(ds.GetItem(DicomTags.ReferencedImageSequence, 0).GetRawString(DicomTags.ReferencedSOPInstanceUID), "2.25.1");
                ds.RemoveItem(DicomTags.ReferencedImageSequence, 0);
                CheckEq(ds.GetItem(DicomTags.ReferencedImageSequence, 0).GetRawString(DicomTags.ReferencedSOPInstanceUID), "2.25.2");
                CheckEq(Throws<SiaaraException>(() => ds.RemoveItem(DicomTags.ReferencedImageSequence, 3))?.ErrorCode, SiaaraErrorCode.NotFound);
                DicomDataset nested = a.Parent!.AppendItem(DicomTags.ReferencedImageSequence).AppendItem(DicomTags.PurposeOfReferenceCodeSequence);
                nested.SetString(DicomTags.CodeValue, "121311");
                CheckEq(ds.GetItem(DicomTags.ReferencedImageSequence, 1).GetItemCount(DicomTags.PurposeOfReferenceCodeSequence), 1);
                ds.SetString(DicomTags.ImageType, "ORIGINAL\\PRIMARY\\AXIAL");
                Check(ds.GetStrings(DicomTags.ImageType).SequenceEqual(new[] { "ORIGINAL", "PRIMARY", "AXIAL" }));
                Check(ds.GetRawStrings(DicomTags.ImageType).SequenceEqual(new[] { "ORIGINAL", "PRIMARY", "AXIAL" }));
                ds.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
                ds.SetString(DicomTags.PatientName, "Müller^Jürgen\\Second");
                Check(ds.GetStrings(DicomTags.PatientName).SequenceEqual(new[] { "Müller^Jürgen", "Second" }));
                CheckEq(SiaaraLibrary.LiveHandleCount, before + 1);
            }
            CheckEq(SiaaraLibrary.LiveHandleCount, before);
        });
    }
}
