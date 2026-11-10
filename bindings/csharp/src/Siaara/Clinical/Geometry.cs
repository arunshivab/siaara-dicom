using System;
using System.Collections.Generic;
using Siaara.Interop;

namespace Siaara.Clinical;

/// <summary>Where the pixel spacing came from.</summary>
public enum SpacingSource
{
    /// <summary>Unknown.</summary>
    None = C.SIAARA_SPACING_NONE,
    /// <summary>Pixel Spacing or the Pixel Measures functional group.</summary>
    PixelSpacing = C.SIAARA_SPACING_PIXEL_SPACING,
    /// <summary>Imager Pixel Spacing at the detector.</summary>
    Imager = C.SIAARA_SPACING_IMAGER,
    /// <summary>Imager Pixel Spacing / Estimated Radiographic Magnification Factor.</summary>
    ImagerMagnified = C.SIAARA_SPACING_IMAGER_MAGNIFIED,
    /// <summary>Nominal Scanned Pixel Spacing.</summary>
    NominalScanned = C.SIAARA_SPACING_NOMINAL_SCANNED,
    /// <summary>Sequence of Ultrasound Regions physical deltas.</summary>
    UltrasoundRegion = C.SIAARA_SPACING_ULTRASOUND_REGION,
}

/// <summary>Plane geometry of a frame (siaara_plane_geometry). Pixel (0,0) is centred on Position.</summary>
public sealed record PlaneGeometry
{
    /// <summary>Rows.</summary>
    public int Rows { get; init; }
    /// <summary>Columns.</summary>
    public int Columns { get; init; }
    /// <summary>Position is valid.</summary>
    public bool HasPosition { get; init; }
    /// <summary>The cosines are valid.</summary>
    public bool HasOrientation { get; init; }
    /// <summary>Spacing source.</summary>
    public SpacingSource SpacingSource { get; init; }
    /// <summary>The spacing applies at the detector plane.</summary>
    public bool SpacingAtDetector { get; init; }
    /// <summary>Image Position (Patient), mm, LPS.</summary>
    public Point3D Position { get; init; }
    /// <summary>Row direction cosine.</summary>
    public Point3D RowCosine { get; init; }
    /// <summary>Column direction cosine.</summary>
    public Point3D ColumnCosine { get; init; }
    /// <summary>Plane normal.</summary>
    public Point3D Normal { get; init; }
    /// <summary>Row spacing (mm); 0 when unknown.</summary>
    public double RowSpacing { get; init; }
    /// <summary>Column spacing (mm); 0 when unknown.</summary>
    public double ColumnSpacing { get; init; }
    /// <summary>Slice Thickness; 0 when absent.</summary>
    public double SliceThickness { get; init; }
    /// <summary>Spacing Between Slices; 0 when absent.</summary>
    public double SpacingBetweenSlices { get; init; }
    /// <summary>Magnification factor applied to the spacing (1 = none).</summary>
    public double Magnification { get; init; }
}

/// <summary>Patient orientation letters at the image edges.</summary>
/// <param name="Left">Left edge, e.g. "R".</param>
/// <param name="Right">Right edge.</param>
/// <param name="Top">Top edge.</param>
/// <param name="Bottom">Bottom edge.</param>
public sealed record OrientationLabels(string Left, string Right, string Top, string Bottom);

/// <summary>Series sort warnings (SIAARA_SORT_*).</summary>
[Flags]
public enum SeriesSortWarnings
{
    /// <summary>No warnings.</summary>
    None = 0,
    /// <summary>Some files have no geometry.</summary>
    MissingGeometry = C.SIAARA_SORT_MISSING_GEOMETRY,
    /// <summary>Several stacks.</summary>
    MultipleStacks = C.SIAARA_SORT_MULTIPLE_STACKS,
    /// <summary>Mixed orientations.</summary>
    MixedOrientation = C.SIAARA_SORT_MIXED_ORIENTATION,
    /// <summary>Duplicate positions.</summary>
    DuplicatePositions = C.SIAARA_SORT_DUPLICATE_POSITIONS,
    /// <summary>Uneven spacing.</summary>
    UnevenSpacing = C.SIAARA_SORT_UNEVEN_SPACING,
    /// <summary>Gantry tilt.</summary>
    GantryTilt = C.SIAARA_SORT_GANTRY_TILT,
    /// <summary>Several frames of reference.</summary>
    MultipleFramesOfReference = C.SIAARA_SORT_MULTIPLE_FRAMES_OF_REFERENCE,
    /// <summary>Split by temporal position.</summary>
    TemporalSplit = C.SIAARA_SORT_TEMPORAL_SPLIT,
    /// <summary>Image sizes differ.</summary>
    SizeMismatch = C.SIAARA_SORT_SIZE_MISMATCH,
}

/// <summary>One sorted slice.</summary>
/// <param name="FileIndex">Index into the input list.</param>
/// <param name="Frame">0-based frame.</param>
/// <param name="Group">Stack / temporal group number.</param>
public readonly record struct SortedSlice(int FileIndex, int Frame, int Group);

/// <summary>Result of <see cref="ImageGeometry.SortSeries"/>.</summary>
/// <param name="Slices">Slices in display order.</param>
/// <param name="Warnings">Warnings.</param>
public sealed record SeriesSortResult(IReadOnlyList<SortedSlice> Slices, SeriesSortWarnings Warnings);

/// <summary>Frame geometry, orientation labels and series sorting.</summary>
public static class ImageGeometry
{
    /// <summary>Geometry of a frame: functional groups first, then the classic attributes.</summary>
    /// <param name="file">The file.</param>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The geometry.</returns>
    public static PlaneGeometry GetFrameGeometry(DicomFile file, int frame = 0)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        var g = new siaara_plane_geometry { struct_size = StructSize.Of<siaara_plane_geometry>() };
        using (var l = new HandleLease(file.Handle))
        {
            Status.Check(Native.siaara_frame_geometry(l.Pointer, (uint)frame, ref g));
        }
        ReadOnlySpan<double> ps = g.pixel_spacing;
        return new PlaneGeometry
        {
            Rows = (int)g.rows,
            Columns = (int)g.columns,
            HasPosition = g.has_position != 0,
            HasOrientation = g.has_orientation != 0,
            SpacingSource = (SpacingSource)g.spacing_source,
            SpacingAtDetector = g.spacing_at_detector != 0,
            Position = Point3D.From(g.position),
            RowCosine = Point3D.From(g.row_cosine),
            ColumnCosine = Point3D.From(g.column_cosine),
            Normal = Point3D.From(g.normal),
            RowSpacing = ps[0],
            ColumnSpacing = ps[1],
            SliceThickness = g.slice_thickness,
            SpacingBetweenSlices = g.spacing_between_slices,
            Magnification = g.magnification,
        };
    }

    /// <summary>Orientation letters at the image edges, or null when the frame has no orientation.</summary>
    /// <param name="file">The file.</param>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The labels or null.</returns>
    public static OrientationLabels? GetOrientationLabels(DicomFile file, int frame = 0)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        var e = new siaara_edge_labels { struct_size = StructSize.Of<siaara_edge_labels>() };
        int st;
        using (var l = new HandleLease(file.Handle))
        {
            st = Native.siaara_orientation_labels(l.Pointer, (uint)frame, ref e);
        }
        if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return null;
        }
        Status.Check(st);
        return new OrientationLabels(DicomDictionary.Chars(e.left), DicomDictionary.Chars(e.right),
            DicomDictionary.Chars(e.top), DicomDictionary.Chars(e.bottom));
    }

    /// <summary>
    /// Sorts files (null entries are skipped) along the slice normal, then Instance Number and
    /// acquisition time, after splitting by stack, temporal position and orientation.
    /// </summary>
    /// <param name="files">The files.</param>
    /// <param name="expandFrames">One entry per frame of enhanced multi-frame files.</param>
    /// <returns>The order and warnings.</returns>
    public static SeriesSortResult SortSeries(IReadOnlyList<DicomFile?> files, bool expandFrames = false)
    {
        ArgumentNullException.ThrowIfNull(files);
        var ptrs = new nint[files.Count];
        var added = new bool[files.Count];
        try
        {
            for (int i = 0; i < files.Count; i++)
            {
                if (files[i] is DicomFile f)
                {
                    f.Handle.DangerousAddRef(ref added[i]);
                    ptrs[i] = f.Handle.DangerousGetHandle();
                }
            }
            uint flags = expandFrames ? C.SIAARA_SORT_EXPAND_FRAMES : 0u;
            int st = Native.siaara_series_sort(ptrs, flags, default, default, default, out nuint needed, out uint warnings);
            if (st != Status.Ok && st != Status.BufferTooSmall)
            {
                throw Status.Error(st);
            }
            int n = DicomFile.ToInt(needed);
            var inst = new uint[n];
            var frame = new uint[n];
            var group = new uint[n];
            if (n > 0)
            {
                Status.Check(Native.siaara_series_sort(ptrs, flags, inst, frame, group, out _, out warnings));
            }
            var slices = new SortedSlice[n];
            for (int k = 0; k < n; k++)
            {
                slices[k] = new SortedSlice((int)inst[k], (int)frame[k], (int)group[k]);
            }
            return new SeriesSortResult(slices, (SeriesSortWarnings)warnings);
        }
        finally
        {
            for (int i = 0; i < files.Count; i++)
            {
                if (added[i])
                {
                    files[i]!.Handle.DangerousRelease();
                }
            }
        }
    }
}
