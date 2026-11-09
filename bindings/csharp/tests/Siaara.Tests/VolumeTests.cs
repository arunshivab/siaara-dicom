using System;
using System.Linq;
using Siaara.Volumes;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class VolumeTests
{
    internal static void Register()
    {
        Case("volume: build from a CT series, properties, zero-copy voxels", () =>
        {
            string dir = NewTempDir("vol");
            string[] paths = Builders.CtSeries(dir, 6, 8, 10, (i, j, k) => (short)(i + 10 * j + 100 * k));
            using Volume v = Volume.BuildAsync(paths.Reverse().ToArray()).GetAwaiter().GetResult();
            VolumeProperties p = v.Properties;
            CheckEq(p.Dimensions, (10, 8, 6));
            CheckEq(p.Spacing, new Point3D(1, 1, 2));
            CheckEq(p.Origin, new Point3D(0, 0, 0));
            CheckEq(p.AxisI, new Point3D(1, 0, 0));
            CheckEq(p.AxisK, new Point3D(0, 0, 1));
            CheckEq(p.VoxelType, VoxelType.Signed16);
            CheckNear(p.ValueMin, 0, 1e-9);
            CheckNear(p.ValueMax, 9 + 70 + 500, 1e-9);
            CheckEq(p.Histogram.Count, 256);
            CheckEq(p.Histogram.Sum(), 480L);
            CheckEq(p.DataBytes, 480L * 2);
            CheckEq(v.Warnings.Count, p.WarningCount);
            using (VoxelBuffer<short> vox = v.LeaseSigned16Voxels())
            {
                CheckEq(vox.Span.Length, 480);
                CheckEq(vox.Span[3 + 10 * 2 + 80 * 4], (short)(3 + 20 + 400));
                CheckEq(vox.Memory.Length, 480);
            }
            Throws<InvalidOperationException>(() => v.LeaseFloat32Voxels().Dispose());
            using Volume f32 = Volume.Build(paths, new VolumeOptions { VoxelType = VoxelType.SinglePrecision, Threads = 2 });
            CheckEq(f32.Properties.VoxelType, VoxelType.SinglePrecision);
            using VoxelBuffer<float> fv = f32.LeaseFloat32Voxels();
            CheckEq(fv.Span[5], 5f);
        });

        Case("volume: the voxel lease keeps the volume alive after Dispose", () =>
        {
            string dir = NewTempDir("vol2");
            string[] paths = Builders.CtSeries(dir, 3, 4, 4, (i, j, k) => (short)k);
            Volume v = Volume.Build(paths);
            VoxelBuffer<short> vox = v.LeaseSigned16Voxels();
            v.Dispose();
            CheckEq(vox.Span[16 * 2], (short)2); // still valid: the lease holds the native volume
            vox.Dispose();
            Throws<ObjectDisposedException>(() => _ = v.Warnings);
        });

        Case("view: MPR render, window, pick, slab, MIP, volume rendering, fusion, curved", () =>
        {
            string dir = NewTempDir("view");
            string[] paths = Builders.CtSeries(dir, 8, 16, 16, (i, j, k) => (short)(i < 8 ? -1000 : 1000));
            using Volume v = Volume.Build(paths);
            using VolumeView view = VolumeView.Create(v, 32, 32);
            byte[] img = view.RenderRgba8();
            CheckEq(img.Length, 32 * 32 * 4);
            Check(img.Where((b, idx) => idx % 4 == 0).Distinct().Count() > 1); // left dark, right bright
            view.SetWindow(0, 0, 100);
            view.SetPlane(new Point3D(7.5, 7.5, 7), new Point3D(0, 0, 1), new Point3D(0, -1, 0), 0.5);
            using (PooledBuffer p = view.RenderRgba8Pooled())
            {
                CheckEq(p.Length, 32 * 32 * 4);
                byte left = p.Span[(16 * 32 + 2) * 4], right = p.Span[(16 * 32 + 29) * 4];
                CheckEq(left, (byte)0);
                CheckEq(right, (byte)255);
            }
            PickResult pick = view.Pick(16, 16);
            Check(pick.Hit && pick.Inside);
            CheckNear(pick.Patient.Z, 7, 1e-6);
            view.SetMode(ViewMode.Mpr, Interpolation.Cubic, SlabMode.Mip, 4);
            CheckEq(view.TryRenderRgba8(new byte[32 * 32 * 4], out int n), SiaaraErrorCode.Ok);
            CheckEq(n, 32 * 32 * 4);
            CheckEq(view.TryRenderRgba8(new byte[10], out _), SiaaraErrorCode.BufferTooSmall);
            view.SetMode(ViewMode.VolumeMip, Interpolation.Linear, SlabMode.None, 0, RenderQuality.Interactive);
            view.SetColormap(0, Colormap.HotIron);
            CheckEq(view.RenderRgba8().Length, 32 * 32 * 4);
            view.SetMode(ViewMode.VolumeRender);
            view.SetTransferFunction(new TransferFunction
            {
                OpacityPoints = new double[] { -1000, 0, 0, 0, 1000, 0.8 },
                ColorPoints = new double[] { -1000, 0, 0, 0, 1000, 1, 1, 1 },
                Shading = true,
            });
            using (PooledBuffer vr = view.RenderRgba8Async().GetAwaiter().GetResult())
            {
                Check(vr.Span.ToArray().Any(b => b != 0));
            }
            Throws<ArgumentException>(() => view.SetTransferFunction(new TransferFunction { OpacityPoints = new double[] { 1, 2, 3 } }));
            using Volume overlay = Volume.Build(paths);
            view.SetMode(ViewMode.Mpr);
            view.SetFusionLayer(overlay, new FusionOptions { Colormap = Colormap.Pet, Opacity = 0.4 });
            CheckEq(view.Pick(16, 16).OverlayInside, true);
            CheckEq(view.RenderRgba8().Length, 32 * 32 * 4);
            view.SetFusionLayer(null);
            view.SetCurve(new[] { new Point3D(2, 8, 2), new Point3D(8, 8, 7), new Point3D(14, 8, 12) });
            view.SetMode(ViewMode.CurvedStretched);
            CheckEq(view.RenderRgba8().Length, 32 * 32 * 4);
        });
    }
}
