using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;

namespace Siaara.Volumes;

/// <summary>Voxel type.</summary>
public enum VoxelType
{
    /// <summary>int16 when every modality value is an integer that fits, else float32 (requests only).</summary>
    Automatic = C.SIAARA_VOXEL_AUTO,
    /// <summary>Signed 16-bit.</summary>
    Signed16 = C.SIAARA_VOXEL_INT16,
    /// <summary>32-bit float.</summary>
    SinglePrecision = C.SIAARA_VOXEL_FLOAT32,
}

/// <summary>Volume build flags (SIAARA_VOLUME_*).</summary>
[Flags]
public enum VolumeConditions
{
    /// <summary>None.</summary>
    None = 0,
    /// <summary>Non-uniform slice spacing.</summary>
    NonUniformSpacing = C.SIAARA_VOLUME_NON_UNIFORM_SPACING,
    /// <summary>Missing slices.</summary>
    MissingSlices = C.SIAARA_VOLUME_MISSING_SLICES,
    /// <summary>Duplicate positions.</summary>
    DuplicatePositions = C.SIAARA_VOLUME_DUPLICATE_POSITIONS,
    /// <summary>Gantry tilt.</summary>
    GantryTilt = C.SIAARA_VOLUME_GANTRY_TILT,
    /// <summary>Desheared onto an orthogonal grid.</summary>
    Desheared = C.SIAARA_VOLUME_DESHEARED,
    /// <summary>Multi-phase (4D) series.</summary>
    MultiPhase = C.SIAARA_VOLUME_MULTI_PHASE,
    /// <summary>Synthetic geometry.</summary>
    SyntheticGeometry = C.SIAARA_VOLUME_SYNTHETIC_GEOMETRY,
    /// <summary>Single slice.</summary>
    SingleSlice = C.SIAARA_VOLUME_SINGLE_SLICE,
    /// <summary>Values were clamped.</summary>
    ValuesClamped = C.SIAARA_VOLUME_VALUES_CLAMPED,
    /// <summary>Spacing taken from the header.</summary>
    SpacingFromHeader = C.SIAARA_VOLUME_SPACING_FROM_HEADER,
}

/// <summary>Volume build options (siaara_volume_options).</summary>
public sealed record VolumeOptions
{
    /// <summary>Voxel type request.</summary>
    public VoxelType VoxelType { get; init; }
    /// <summary>Phase of a 4D series, 0-based.</summary>
    public int Phase { get; init; }
    /// <summary>Threads; 0 = hardware concurrency.</summary>
    public int Threads { get; init; }
    /// <summary>Resample a gantry-tilted stack onto an orthogonal grid.</summary>
    public bool DeshearTilt { get; init; }
    /// <summary>Fail on non-uniform spacing, missing slices, duplicates.</summary>
    public bool Strict { get; init; }
    /// <summary>Voxel buffer limit in bytes; 0 = 4 GiB.</summary>
    public long MemoryBudget { get; init; }
    /// <summary>Relative spacing tolerance; 0 = 0.01.</summary>
    public double SpacingTolerance { get; init; }
}

/// <summary>Volume properties (siaara_volume_properties).</summary>
public sealed record VolumeProperties
{
    /// <summary>Dimensions (i, j, k).</summary>
    public (int I, int J, int K) Dimensions { get; init; }
    /// <summary>Spacing (mm) along i, j, k.</summary>
    public Point3D Spacing { get; init; }
    /// <summary>Centre of voxel (0,0,0), LPS mm.</summary>
    public Point3D Origin { get; init; }
    /// <summary>Patient direction of index axis i.</summary>
    public Point3D AxisI { get; init; }
    /// <summary>Patient direction of index axis j.</summary>
    public Point3D AxisJ { get; init; }
    /// <summary>Patient direction of index axis k.</summary>
    public Point3D AxisK { get; init; }
    /// <summary>Voxel type (Signed16 or SinglePrecision).</summary>
    public VoxelType VoxelType { get; init; }
    /// <summary>Conditions found while building.</summary>
    public VolumeConditions Conditions { get; init; }
    /// <summary>Minimum value.</summary>
    public double ValueMin { get; init; }
    /// <summary>Maximum value.</summary>
    public double ValueMax { get; init; }
    /// <summary>Phases of the series.</summary>
    public int PhaseCount { get; init; }
    /// <summary>Phase built.</summary>
    public int PhaseIndex { get; init; }
    /// <summary>Gantry tilt (degrees).</summary>
    public double TiltDegrees { get; init; }
    /// <summary>Number of warnings (see <see cref="Volume.Warnings"/>).</summary>
    public int WarningCount { get; init; }
    /// <summary>Histogram lower bound.</summary>
    public double HistogramMin { get; init; }
    /// <summary>Histogram upper bound.</summary>
    public double HistogramMax { get; init; }
    /// <summary>Equal-width histogram (256 bins) over [HistogramMin, HistogramMax].</summary>
    public IReadOnlyList<long> Histogram { get; init; } = Array.Empty<long>();
    /// <summary>Bytes of the voxel buffer.</summary>
    public long DataBytes { get; init; }
}

/// <summary>
/// A dense voxel grid built from one series (siaara_volume), immutable and usable from many threads.
/// Voxel (i, j, k), i fastest, is at Origin + i*Spacing.X*AxisI + j*Spacing.Y*AxisJ + k*Spacing.Z*AxisK.
/// </summary>
public sealed class Volume : IDisposable
{
    private readonly VolumeHandle _handle;
    private VolumeProperties? _properties;

    private Volume(VolumeHandle h) => _handle = h;

    internal VolumeHandle Handle => _handle;

    /// <summary>Builds a volume from the files of one series (blocking and CPU heavy; see <see cref="BuildAsync"/>).</summary>
    /// <param name="paths">File paths.</param>
    /// <param name="options">Options, or null.</param>
    /// <returns>The volume.</returns>
    public static Volume Build(IReadOnlyList<string> paths, VolumeOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(paths);
        if (paths.Count == 0)
        {
            throw new ArgumentException("At least one path is needed.", nameof(paths));
        }
        using var mem = new NativeAllocations();
        var ptrs = new nint[paths.Count];
        var lens = new nuint[paths.Count];
        for (int i = 0; i < paths.Count; i++)
        {
            ArgumentException.ThrowIfNullOrEmpty(paths[i], nameof(paths));
            ptrs[i] = mem.Utf8(paths[i]);
            lens[i] = (nuint)System.Text.Encoding.UTF8.GetByteCount(paths[i]);
        }
        int st;
        nint v;
        if (options is null)
        {
            st = Native.siaara_volume_build(ptrs, lens, (uint)paths.Count, in Unsafe.NullRef<siaara_volume_options>(), out v);
        }
        else
        {
            var o = new siaara_volume_options
            {
                struct_size = StructSize.Of<siaara_volume_options>(),
                voxel_type = (int)options.VoxelType,
                phase = (uint)Math.Max(0, options.Phase),
                threads = (uint)Math.Max(0, options.Threads),
                deshear_tilt = options.DeshearTilt ? 1 : 0,
                strict = options.Strict ? 1 : 0,
                memory_budget = (ulong)Math.Max(0, options.MemoryBudget),
                spacing_tolerance = options.SpacingTolerance,
            };
            st = Native.siaara_volume_build(ptrs, lens, (uint)paths.Count, in o, out v);
        }
        Status.Check(st);
        var h = new VolumeHandle();
        h.Attach(v);
        return new Volume(h);
    }

    /// <summary>Build on a thread-pool thread (reads and decodes the whole series).</summary>
    /// <param name="paths">File paths.</param>
    /// <param name="options">Options, or null.</param>
    /// <param name="cancellationToken">Cancels before the build starts.</param>
    /// <returns>The volume.</returns>
    public static Task<Volume> BuildAsync(IReadOnlyList<string> paths, VolumeOptions? options = null, CancellationToken cancellationToken = default) =>
        Task.Run(() => Build(paths, options), cancellationToken);

    /// <summary>Properties (read once; the volume is immutable).</summary>
    public VolumeProperties Properties => _properties ??= ReadProperties();

    private VolumeProperties ReadProperties()
    {
        var p = new siaara_volume_properties { struct_size = StructSize.Of<siaara_volume_properties>() };
        using (var l = new HandleLease(_handle))
        {
            Status.Check(Native.siaara_volume_info(l.Pointer, ref p));
        }
        ReadOnlySpan<uint> dims = p.dims;
        ReadOnlySpan<double> dir = p.direction;
        ReadOnlySpan<ulong> hist = p.histogram;
        var h = new long[Math.Min((int)p.histogram_bins, hist.Length)];
        for (int i = 0; i < h.Length; i++)
        {
            h[i] = (long)hist[i];
        }
        return new VolumeProperties
        {
            Dimensions = ((int)dims[0], (int)dims[1], (int)dims[2]),
            Spacing = Point3D.From(p.spacing),
            Origin = Point3D.From(p.origin),
            // row-major 3x3: column c is the patient direction of index axis c
            AxisI = new Point3D(dir[0], dir[3], dir[6]),
            AxisJ = new Point3D(dir[1], dir[4], dir[7]),
            AxisK = new Point3D(dir[2], dir[5], dir[8]),
            VoxelType = (VoxelType)p.voxel_type,
            Conditions = (VolumeConditions)p.flags,
            ValueMin = p.value_min,
            ValueMax = p.value_max,
            PhaseCount = (int)p.phase_count,
            PhaseIndex = (int)p.phase_index,
            TiltDegrees = p.tilt_degrees,
            WarningCount = (int)p.warning_count,
            HistogramMin = p.histogram_min,
            HistogramMax = p.histogram_max,
            Histogram = h,
            DataBytes = (long)p.data_bytes,
        };
    }

    /// <summary>Build warnings as text.</summary>
    public IReadOnlyList<string> Warnings
    {
        get
        {
            int n = Properties.WarningCount;
            var list = new string[n];
            using var l = new HandleLease(_handle);
            for (int i = 0; i < n; i++)
            {
                list[i] = Status.ReadString((V: l.Pointer, I: (uint)i), static (s, b, out nn) => Native.siaara_volume_warning(s.V, s.I, b, out nn));
            }
            return list;
        }
    }

    /// <summary>
    /// Zero-copy access to the int16 voxels. The returned owner keeps the volume alive until it is
    /// disposed (dispose it; the native buffer is freed only after that).
    /// </summary>
    /// <returns>The voxel memory.</returns>
    /// <exception cref="InvalidOperationException">The volume holds float32 voxels.</exception>
    public VoxelBuffer<short> LeaseSigned16Voxels() => Lease<short>(VoxelType.Signed16);

    /// <summary>Zero-copy access to the float32 voxels (see <see cref="LeaseSigned16Voxels"/>).</summary>
    /// <returns>The voxel memory.</returns>
    public VoxelBuffer<float> LeaseFloat32Voxels() => Lease<float>(VoxelType.SinglePrecision);

    private VoxelBuffer<T> Lease<T>(VoxelType expected) where T : unmanaged
    {
        if (Properties.VoxelType != expected)
        {
            throw new InvalidOperationException("The volume holds " + Properties.VoxelType + " voxels.");
        }
        nint data;
        ulong bytes;
        using (var l = new HandleLease(_handle))
        {
            Status.Check(Native.siaara_volume_data(l.Pointer, out data, out bytes));
        }
        ulong count = bytes / (ulong)Unsafe.SizeOf<T>();
        if (count > int.MaxValue)
        {
            throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "volume larger than a span can address");
        }
        return new VoxelBuffer<T>(new BorrowedMemoryManager<T>(_handle, data, (int)count));
    }

    /// <summary>Frees the volume (views keep their own reference).</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}

/// <summary>Read-only, zero-copy view of a volume's voxels; Dispose releases the volume reference.</summary>
/// <typeparam name="T">short or float.</typeparam>
public sealed class VoxelBuffer<T> : IDisposable where T : unmanaged
{
    private readonly MemoryManager<T> _manager;

    internal VoxelBuffer(MemoryManager<T> manager) => _manager = manager;

    /// <summary>The voxels, i fastest.</summary>
    public ReadOnlyMemory<T> Memory => _manager.Memory;

    /// <summary>The voxels as a span.</summary>
    public ReadOnlySpan<T> Span => _manager.GetSpan();

    /// <summary>Releases the volume reference.</summary>
    public void Dispose()
    {
        ((IDisposable)_manager).Dispose();
        GC.SuppressFinalize(this);
    }
}
