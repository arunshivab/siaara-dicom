using System;
using Siaara.Interop;

namespace Siaara.Volumes;

/// <summary>The value of a volume at a patient coordinate (siaara_volume_sample).</summary>
/// <param name="X">Patient x (mm, LPS), as asked.</param>
/// <param name="Y">Patient y (mm).</param>
/// <param name="Z">Patient z (mm).</param>
/// <param name="I">Continuous voxel index i.</param>
/// <param name="J">Continuous voxel index j.</param>
/// <param name="K">Continuous voxel index k.</param>
/// <param name="VoxelI">Nearest voxel i (may be outside the grid).</param>
/// <param name="VoxelJ">Nearest voxel j.</param>
/// <param name="VoxelK">Nearest voxel k.</param>
/// <param name="Inside">The point lies in the volume.</param>
/// <param name="Value">Inside: the nearest voxel's value, or the trilinear value when asked; null outside.</param>
/// <param name="Trilinear">Trilinear interpolation was asked for.</param>
public sealed record VolumeSample(double X, double Y, double Z, double I, double J, double K, long VoxelI, long VoxelJ, long VoxelK,
    bool Inside, double? Value, bool Trilinear);

/// <summary>Values of a volume at a voxel index or a patient coordinate.</summary>
public sealed partial class Volume
{
    /// <summary>Voxel (i, j, k): the modality value as built.</summary>
    /// <param name="i">Index along the first axis.</param>
    /// <param name="j">Index along the second axis.</param>
    /// <param name="k">Slice index.</param>
    /// <returns>The value.</returns>
    public double GetVoxel(int i, int j, int k)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(i);
        ArgumentOutOfRangeException.ThrowIfNegative(j);
        ArgumentOutOfRangeException.ThrowIfNegative(k);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_volume_voxel(l.Pointer, (uint)i, (uint)j, (uint)k, out double v));
        return v;
    }

    /// <summary>
    /// The value at a patient coordinate (mm): the nearest voxel by default; trilinear only when
    /// asked. Reports the voxel index and whether the point is inside the volume.
    /// </summary>
    /// <param name="x">Patient x (mm, LPS).</param>
    /// <param name="y">Patient y (mm).</param>
    /// <param name="z">Patient z (mm).</param>
    /// <param name="trilinear">Interpolate trilinearly instead of taking the nearest voxel.</param>
    /// <returns>The sample.</returns>
    public VolumeSample ValueAt(double x, double y, double z, bool trilinear = false)
    {
        var s = new siaara_volume_sample { struct_size = StructSize.Of<siaara_volume_sample>() };
        ReadOnlySpan<double> p = stackalloc double[] { x, y, z };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_volume_value_at(l.Pointer, p, trilinear ? C.SIAARA_INTERP_LINEAR : C.SIAARA_INTERP_NEAREST, ref s));
        return new VolumeSample(x, y, z, s.index[0], s.index[1], s.index[2], s.voxel[0], s.voxel[1], s.voxel[2], s.inside != 0,
            s.inside != 0 ? s.value : null, trilinear);
    }
}
