using System;
using System.Linq;
using System.Runtime.InteropServices;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class PixelTests
{
    internal static void Register()
    {
        Case("pixel: info, native decode, modality float32", () =>
        {
            using DicomFile f = Builders.Smoke();
            DicomPixelInfo pi = f.GetPixelInfo();
            CheckEq(pi.Rows, 2);
            CheckEq(pi.Columns, 2);
            CheckEq(pi.BitsStored, 16);
            Check(pi.IsSigned);
            CheckEq(pi.FrameBytes, 8L);
            CheckEq(pi.PhotometricInterpretation, "MONOCHROME2");
            Check(!pi.IsEncapsulated);
            CheckEq(pi.SampleKind, PixelSampleKind.Integral);
            CheckEq(f.FrameCount, 1);
            CheckEq(f.GetFrameSize(0), 8);
            byte[] native = f.DecodeFrame(0);
            Check(native.SequenceEqual(new byte[] { 0x00, 0x04, 0x32, 0x04, 0xCE, 0x03, 0xFF, 0x7F }));
            float[] hu = f.DecodeFrameModality(0);
            Check(hu.SequenceEqual(new float[] { 0, 50, -50, 31743 }));
            Span<byte> small = stackalloc byte[4];
            CheckEq(f.TryDecodeFrame(0, small, FrameFormat.Native, out int need), SiaaraErrorCode.BufferTooSmall);
            CheckEq(need, 8);
            CheckEq(Throws<SiaaraException>(() => f.DecodeFrame(0, new byte[4]))?.ErrorCode, SiaaraErrorCode.BufferTooSmall);
            Span<byte> exact = stackalloc byte[8];
            CheckEq(f.TryDecodeFrame(0, exact, FrameFormat.Native, out int written), SiaaraErrorCode.Ok);
            CheckEq(written, 8);
            CheckEq(Throws<SiaaraException>(() => f.DecodeFrame(1))?.ErrorCode, SiaaraErrorCode.InvalidArgument);
            using (PooledBuffer pooled = f.DecodeFramePooled(0))
            {
                CheckEq(pooled.Length, 8);
                Check(pooled.Span.SequenceEqual(native));
            }
            Check(f.TryGetPixelInfo(out DicomPixelInfo? again) && again == pi);
            using DicomFile empty = DicomFile.Create();
            Check(!empty.TryGetPixelInfo(out _));
        });

        Case("pixel: render RGBA8 (file VOI, explicit window, invert, errors)", () =>
        {
            using DicomFile f = Builders.Smoke();
            byte[] rgba = f.RenderRgba8();
            CheckEq(rgba.Length, 16);
            CheckEq(rgba[4], (byte)255); // +50 HU white
            CheckEq(rgba[8], (byte)0);   // -50 HU black
            CheckEq(rgba[3], (byte)255);
            var inv = RenderOptions.Window(0, 100) with { Invert = true };
            byte[] r2 = f.RenderRgba8(inv);
            CheckEq(r2[4], (byte)0);
            CheckEq(r2[8], (byte)255);
            CheckEq(Throws<SiaaraException>(() => f.RenderRgba8(RenderOptions.Window(0, 0)))?.ErrorCode, SiaaraErrorCode.InvalidArgument);
            CheckEq(f.TryRenderRgba8(new byte[8], null, out int need), SiaaraErrorCode.BufferTooSmall);
            CheckEq(need, 16);
            using PooledBuffer p = f.RenderRgba8Pooled(new RenderOptions { VoiMode = VoiMode.FullRange });
            CheckEq(p.Length, 16);
            CheckEq(p.Span[12], (byte)255); // maximum of the frame
            CheckEq(f.GetRenderSize(), 16);
            Span<byte> dst = stackalloc byte[16];
            CheckEq(f.RenderRgba8(dst, null), 16);
            Check(dst.SequenceEqual(rgba));
        });

        Case("pixel: RGB colour frame decode and render", () =>
        {
            using DicomFile f = Builders.Rgb(3, 5);
            DicomPixelInfo pi = f.GetPixelInfo();
            CheckEq(pi.SamplesPerPixel, 3);
            byte[] native = f.DecodeFrame(0);
            CheckEq(native.Length, 45);
            byte[] rgba = f.RenderRgba8();
            CheckEq(rgba.Length, 60);
            Check(rgba[0] == native[0] && rgba[1] == native[1] && rgba[2] == native[2] && rgba[3] == 255);
            Check(rgba[4] == native[3]);
        });

        Case("pixel: multi-frame decode and per-frame render", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetRawString(DicomTags.SOPClassUID, "UI", Builders.SecondaryCapture);
            ds.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.77");
            ds.SetUInt16(DicomTags.SamplesPerPixel, 1);
            ds.SetString(DicomTags.PhotometricInterpretation, "MONOCHROME2");
            ds.SetUInt16(DicomTags.Rows, 2);
            ds.SetUInt16(DicomTags.Columns, 2);
            ds.SetUInt16(DicomTags.BitsAllocated, 8);
            ds.SetUInt16(DicomTags.BitsStored, 8);
            ds.SetUInt16(DicomTags.HighBit, 7);
            ds.SetUInt16(DicomTags.PixelRepresentation, 0);
            ds.SetString(DicomTags.NumberOfFrames, "3");
            byte[] px = Enumerable.Range(0, 12).Select(i => (byte)(i * 20)).ToArray();
            ds.SetBytes(DicomTags.PixelData, "OB", px);
            CheckEq(f.FrameCount, 3);
            for (int k = 0; k < 3; k++)
            {
                Check(f.DecodeFrame(k).SequenceEqual(px.Skip(4 * k).Take(4)));
                CheckEq(f.RenderRgba8(new RenderOptions { Frame = k, VoiMode = VoiMode.FullRange }).Length, 16);
            }
            float[] m = f.DecodeFrameModality(2);
            CheckEq(m[3], 220f);
            Check(MemoryMarshal.AsBytes(m.AsSpan()).Length == 16);
        });
    }
}
