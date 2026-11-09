using System;
using System.Collections.Generic;
using System.Text;
using Siaara.Interop;

namespace Siaara.Clinical;

/// <summary>SR content item value type.</summary>
public enum SrValueType
{
    /// <summary>CONTAINER.</summary>
    Container = C.SIAARA_SR_CONTAINER,
    /// <summary>TEXT.</summary>
    Text = C.SIAARA_SR_TEXT,
    /// <summary>CODE.</summary>
    Code = C.SIAARA_SR_CODE,
    /// <summary>NUM.</summary>
    Num = C.SIAARA_SR_NUM,
    /// <summary>DATETIME.</summary>
    DateTime = C.SIAARA_SR_DATETIME,
    /// <summary>DATE.</summary>
    Date = C.SIAARA_SR_DATE,
    /// <summary>TIME.</summary>
    Time = C.SIAARA_SR_TIME,
    /// <summary>UIDREF.</summary>
    UidRef = C.SIAARA_SR_UIDREF,
    /// <summary>PNAME.</summary>
    PersonName = C.SIAARA_SR_PNAME,
    /// <summary>COMPOSITE.</summary>
    Composite = C.SIAARA_SR_COMPOSITE,
    /// <summary>IMAGE.</summary>
    Image = C.SIAARA_SR_IMAGE,
    /// <summary>WAVEFORM.</summary>
    Waveform = C.SIAARA_SR_WAVEFORM,
    /// <summary>SCOORD.</summary>
    SpatialCoordinates = C.SIAARA_SR_SCOORD,
    /// <summary>SCOORD3D.</summary>
    SpatialCoordinates3D = C.SIAARA_SR_SCOORD3D,
    /// <summary>TCOORD.</summary>
    TemporalCoordinates = C.SIAARA_SR_TCOORD,
    /// <summary>TABLE.</summary>
    Table = C.SIAARA_SR_TABLE,
    /// <summary>By-reference relationship.</summary>
    Reference = C.SIAARA_SR_REFERENCE,
    /// <summary>Unknown.</summary>
    Unknown = C.SIAARA_SR_UNKNOWN,
}

/// <summary>SR relationship type.</summary>
public enum SrRelationship
{
    /// <summary>None (root).</summary>
    None = C.SIAARA_REL_NONE,
    /// <summary>CONTAINS.</summary>
    Contains = C.SIAARA_REL_CONTAINS,
    /// <summary>HAS PROPERTIES.</summary>
    HasProperties = C.SIAARA_REL_HAS_PROPERTIES,
    /// <summary>HAS OBS CONTEXT.</summary>
    HasObservationContext = C.SIAARA_REL_HAS_OBS_CONTEXT,
    /// <summary>HAS ACQ CONTEXT.</summary>
    HasAcquisitionContext = C.SIAARA_REL_HAS_ACQ_CONTEXT,
    /// <summary>INFERRED FROM.</summary>
    InferredFrom = C.SIAARA_REL_INFERRED_FROM,
    /// <summary>SELECTED FROM.</summary>
    SelectedFrom = C.SIAARA_REL_SELECTED_FROM,
    /// <summary>HAS CONCEPT MOD.</summary>
    HasConceptModifier = C.SIAARA_REL_HAS_CONCEPT_MOD,
    /// <summary>Unknown.</summary>
    Unknown = C.SIAARA_REL_UNKNOWN,
}

/// <summary>Text fields of an SR node.</summary>
public enum SrField
{
    /// <summary>Concept name code value.</summary>
    ConceptValue = C.SIAARA_SRF_CONCEPT_VALUE,
    /// <summary>Concept name coding scheme.</summary>
    ConceptScheme = C.SIAARA_SRF_CONCEPT_SCHEME,
    /// <summary>Concept name code meaning.</summary>
    ConceptMeaning = C.SIAARA_SRF_CONCEPT_MEANING,
    /// <summary>TEXT, DATETIME, DATE, TIME, UIDREF, PNAME value.</summary>
    Text = C.SIAARA_SRF_TEXT,
    /// <summary>CODE value.</summary>
    CodeValue = C.SIAARA_SRF_CODE_VALUE,
    /// <summary>CODE scheme.</summary>
    CodeScheme = C.SIAARA_SRF_CODE_SCHEME,
    /// <summary>CODE meaning.</summary>
    CodeMeaning = C.SIAARA_SRF_CODE_MEANING,
    /// <summary>NUM value as text.</summary>
    NumericText = C.SIAARA_SRF_NUMERIC_TEXT,
    /// <summary>Units code value.</summary>
    UnitsValue = C.SIAARA_SRF_UNITS_VALUE,
    /// <summary>Units scheme.</summary>
    UnitsScheme = C.SIAARA_SRF_UNITS_SCHEME,
    /// <summary>Units meaning.</summary>
    UnitsMeaning = C.SIAARA_SRF_UNITS_MEANING,
    /// <summary>First referenced SOP Class.</summary>
    ReferencedSopClass = C.SIAARA_SRF_REFERENCED_SOP_CLASS,
    /// <summary>First referenced SOP Instance.</summary>
    ReferencedSopInstance = C.SIAARA_SRF_REFERENCED_SOP_INSTANCE,
    /// <summary>Graphic Type.</summary>
    GraphicType = C.SIAARA_SRF_GRAPHIC_TYPE,
    /// <summary>Value Type as text.</summary>
    ValueType = C.SIAARA_SRF_VALUE_TYPE,
    /// <summary>Relationship as text.</summary>
    Relationship = C.SIAARA_SRF_RELATIONSHIP,
    /// <summary>Continuity of Content.</summary>
    Continuity = C.SIAARA_SRF_CONTINUITY,
    /// <summary>Template ID.</summary>
    TemplateId = C.SIAARA_SRF_TEMPLATE_ID,
    /// <summary>Position, e.g. "1.2.3".</summary>
    Position = C.SIAARA_SRF_POSITION,
}

/// <summary>One SR content tree node (siaara_sr_node_info).</summary>
/// <param name="Index">Node index.</param>
/// <param name="Parent">Parent index, -1 for the root.</param>
/// <param name="Depth">Depth.</param>
/// <param name="ChildCount">Number of children.</param>
/// <param name="ValueType">Value type.</param>
/// <param name="Relationship">Relationship to the parent.</param>
/// <param name="ReferencedNode">REFERENCE target, -1 if unresolved.</param>
/// <param name="Numeric">NUM value, or null.</param>
/// <param name="ReferenceCount">COMPOSITE / IMAGE / WAVEFORM references.</param>
/// <param name="GraphicPointValues">SCOORD / SCOORD3D Graphic Data value count.</param>
public sealed record SrNode(int Index, int Parent, int Depth, int ChildCount, SrValueType ValueType, SrRelationship Relationship,
    int ReferencedNode, double? Numeric, int ReferenceCount, int GraphicPointValues);

/// <summary>An SR content tree (siaara_sr).</summary>
public sealed class StructuredReport : IDisposable
{
    private readonly SrHandle _handle;

    private StructuredReport(SrHandle h) => _handle = h;

    /// <summary>Reads the content tree of an SR file.</summary>
    /// <param name="file">The file.</param>
    /// <returns>The report.</returns>
    public static StructuredReport Open(DicomFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        nint p;
        using (var l = new HandleLease(file.Handle))
        {
            Status.Check(Native.siaara_sr_open(l.Pointer, out p));
        }
        var h = new SrHandle();
        h.Attach(p);
        return new StructuredReport(h);
    }

    /// <summary>Number of nodes (node 0 is the root).</summary>
    public int NodeCount
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_sr_node_count(l.Pointer, out uint n));
            return (int)n;
        }
    }

    /// <summary>A node.</summary>
    /// <param name="index">Node index.</param>
    /// <returns>The node.</returns>
    public SrNode GetNode(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        var n = new siaara_sr_node_info { struct_size = StructSize.Of<siaara_sr_node_info>() };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_sr_node(l.Pointer, (uint)index, ref n));
        return new SrNode(index, n.parent, (int)n.depth, (int)n.child_count, (SrValueType)n.value_type, (SrRelationship)n.relationship,
            n.referenced_node, n.has_numeric != 0 ? n.numeric : null, (int)n.reference_count, (int)n.graphic_point_values);
    }

    /// <summary>Child node indices.</summary>
    /// <param name="index">Node index.</param>
    /// <returns>The children.</returns>
    public int[] GetChildren(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_handle);
        uint[] c = Status.ReadArray((Sr: l.Pointer, I: (uint)index), static ((nint Sr, uint I) s, Span<uint> b, out nuint n) =>
            Native.siaara_sr_node_children(s.Sr, s.I, b, out n));
        return Array.ConvertAll(c, x => (int)x);
    }

    /// <summary>A text field of a node ("" when absent).</summary>
    /// <param name="index">Node index.</param>
    /// <param name="field">Field.</param>
    /// <returns>The text.</returns>
    public string GetString(int index, SrField field)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_handle);
        return Status.ReadString((Sr: l.Pointer, I: (uint)index, F: (int)field), static (s, b, out n) =>
            Native.siaara_sr_node_string(s.Sr, s.I, s.F, b, out n));
    }

    /// <summary>SCOORD / SCOORD3D Graphic Data values.</summary>
    /// <param name="index">Node index.</param>
    /// <returns>The values.</returns>
    public double[] GetGraphicData(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        using var l = new HandleLease(_handle);
        return Status.ReadArray((Sr: l.Pointer, I: (uint)index), static ((nint Sr, uint I) s, Span<double> b, out nuint n) =>
            Native.siaara_sr_node_graphic_data(s.Sr, s.I, b, out n));
    }

    /// <summary>Plain-text rendering of the whole tree.</summary>
    /// <returns>The text.</returns>
    public string ToText()
    {
        using var l = new HandleLease(_handle);
        return Status.ReadString(l.Pointer, static (nint s, Span<byte> b, out nuint n) => Native.siaara_sr_to_text(s, b, out n));
    }

    /// <summary>Frees the tree.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}

/// <summary>Segmentation type.</summary>
public enum SegmentationType
{
    /// <summary>BINARY.</summary>
    Binary = C.SIAARA_SEG_BINARY,
    /// <summary>FRACTIONAL.</summary>
    Fractional = C.SIAARA_SEG_FRACTIONAL,
    /// <summary>LABELMAP.</summary>
    LabelMap = C.SIAARA_SEG_LABELMAP,
}

/// <summary>A segment (siaara_segment_info).</summary>
/// <param name="Number">Segment Number.</param>
/// <param name="FrameCount">SEG frames holding the segment.</param>
/// <param name="Label">Segment Label.</param>
/// <param name="Rgb">Display colour (from CIELab), or null.</param>
/// <param name="CieLab">Recommended Display CIELab Value (L, a, b as stored).</param>
public sealed record SegmentInfo(int Number, int FrameCount, string Label, (byte R, byte G, byte B)? Rgb, (ushort L, ushort A, ushort B) CieLab);

/// <summary>A DICOM SEG object (siaara_seg). Keeps its source file for mask access.</summary>
public sealed class Segmentation : IDisposable
{
    private readonly SegHandle _handle;
    private readonly DicomFile _file;

    private Segmentation(SegHandle h, DicomFile file)
    {
        _handle = h;
        _file = file;
    }

    /// <summary>Reads the segmentation description. The file must stay open while masks are read.</summary>
    /// <param name="file">The SEG file.</param>
    /// <returns>The segmentation.</returns>
    public static Segmentation Open(DicomFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        nint p;
        using (var l = new HandleLease(file.Handle))
        {
            Status.Check(Native.siaara_seg_open(l.Pointer, out p));
        }
        var h = new SegHandle();
        h.Attach(p);
        return new Segmentation(h, file);
    }

    /// <summary>Segmentation Type.</summary>
    public SegmentationType Type => Info().Type;

    /// <summary>Number of Frames.</summary>
    public int FrameCount => Info().Frames;

    /// <summary>Maximum Fractional Value.</summary>
    public int MaxFractionalValue => Info().Max;

    private (SegmentationType Type, int Frames, int Max) Info()
    {
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_seg_info(l.Pointer, out int type, out uint frames, out uint max));
        return ((SegmentationType)type, (int)frames, (int)max);
    }

    /// <summary>The segments.</summary>
    public IReadOnlyList<SegmentInfo> Segments
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_seg_segment_count(l.Pointer, out uint n));
            var list = new SegmentInfo[n];
            for (uint i = 0; i < n; i++)
            {
                var s = new siaara_segment_info { struct_size = StructSize.Of<siaara_segment_info>() };
                Status.Check(Native.siaara_seg_segment(l.Pointer, i, ref s));
                string label = Status.ReadString((Seg: l.Pointer, I: i), static (st, b, out nn) =>
                    Native.siaara_seg_segment_label(st.Seg, st.I, b, out nn));
                ReadOnlySpan<byte> rgb = s.rgb;
                ReadOnlySpan<ushort> lab = s.cielab;
                list[i] = new SegmentInfo((int)s.number, (int)s.frame_count, label,
                    s.has_color != 0 ? (rgb[0], rgb[1], rgb[2]) : null, (lab[0], lab[1], lab[2]));
            }
            return list;
        }
    }

    /// <summary>SEG frames (0-based) of a segment derived from a source instance and 1-based frame (0 = any).</summary>
    /// <param name="segmentNumber">Segment Number.</param>
    /// <param name="sourceSopInstanceUid">Source SOP Instance UID.</param>
    /// <param name="sourceFrame">1-based source frame, 0 = any.</param>
    /// <returns>The SEG frames.</returns>
    public int[] GetFramesForSource(int segmentNumber, string sourceSopInstanceUid, int sourceFrame = 0)
    {
        ArgumentException.ThrowIfNullOrEmpty(sourceSopInstanceUid);
        using var l = new HandleLease(_handle);
        uint[] f = Status.ReadArray((Seg: l.Pointer, N: (uint)segmentNumber, Uid: Encoding.ASCII.GetBytes(sourceSopInstanceUid), F: (uint)sourceFrame),
            static ((nint Seg, uint N, byte[] Uid, uint F) s, Span<uint> b, out nuint n) =>
                Native.siaara_seg_frames_for_source(s.Seg, s.N, s.Uid, s.F, b, out n));
        return Array.ConvertAll(f, x => (int)x);
    }

    /// <summary>Mask of a SEG frame (rows x columns bytes): BINARY 0/1, FRACTIONAL the stored value, LABELMAP 1 for the segment.</summary>
    /// <param name="frame">0-based SEG frame.</param>
    /// <param name="segmentNumber">Segment Number.</param>
    /// <returns>The mask.</returns>
    public byte[] GetMask(int frame, int segmentNumber)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        using var f = new HandleLease(_file.Handle);
        using var l = new HandleLease(_handle);
        int st = Native.siaara_seg_mask(f.Pointer, l.Pointer, (uint)frame, (uint)segmentNumber, default, out nuint needed);
        if (st != Status.Ok && st != Status.BufferTooSmall)
        {
            throw Status.Error(st);
        }
        byte[] mask = new byte[DicomFile.ToInt(needed)];
        if (mask.Length > 0)
        {
            Status.Check(Native.siaara_seg_mask(f.Pointer, l.Pointer, (uint)frame, (uint)segmentNumber, mask, out _));
        }
        return mask;
    }

    /// <summary>Frees the description (not the file).</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}

/// <summary>An RT Structure Set ROI (siaara_rt_roi_info).</summary>
/// <param name="Index">Index in the structure set.</param>
/// <param name="Number">ROI Number.</param>
/// <param name="Name">ROI Name.</param>
/// <param name="ContourCount">Contours.</param>
/// <param name="Color">ROI Display Color, or null.</param>
public sealed record RtRoi(int Index, int Number, string Name, int ContourCount, (byte R, byte G, byte B)? Color);

/// <summary>An RT Structure Set (siaara_rtstruct).</summary>
public sealed class RtStructureSet : IDisposable
{
    private readonly RtStructHandle _handle;

    private RtStructureSet(RtStructHandle h) => _handle = h;

    /// <summary>Reads the structure set of a file.</summary>
    /// <param name="file">The RTSTRUCT file.</param>
    /// <returns>The structure set.</returns>
    public static RtStructureSet Open(DicomFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        nint p;
        using (var l = new HandleLease(file.Handle))
        {
            Status.Check(Native.siaara_rtstruct_open(l.Pointer, out p));
        }
        var h = new RtStructHandle();
        h.Attach(p);
        return new RtStructureSet(h);
    }

    /// <summary>The ROIs.</summary>
    public IReadOnlyList<RtRoi> Rois
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_rtstruct_roi_count(l.Pointer, out uint n));
            var list = new RtRoi[n];
            for (uint i = 0; i < n; i++)
            {
                var r = new siaara_rt_roi_info { struct_size = StructSize.Of<siaara_rt_roi_info>() };
                Status.Check(Native.siaara_rtstruct_roi(l.Pointer, i, ref r));
                string name = Status.ReadString((Rt: l.Pointer, I: i), static (s, b, out nn) =>
                    Native.siaara_rtstruct_roi_name(s.Rt, s.I, b, out nn));
                ReadOnlySpan<byte> rgb = r.rgb;
                list[i] = new RtRoi((int)i, r.number, name, (int)r.contour_count, r.has_color != 0 ? (rgb[0], rgb[1], rgb[2]) : null);
            }
            return list;
        }
    }

    /// <summary>Contour points in patient coordinates (mm).</summary>
    /// <param name="roiIndex">ROI index (not number).</param>
    /// <param name="contour">Contour index.</param>
    /// <param name="closed">True for CLOSED_PLANAR.</param>
    /// <returns>The points.</returns>
    public Point3D[] GetContourPoints(int roiIndex, int contour, out bool closed)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(roiIndex);
        ArgumentOutOfRangeException.ThrowIfNegative(contour);
        using var l = new HandleLease(_handle);
        int st = Native.siaara_rtstruct_contour_points(l.Pointer, (uint)roiIndex, (uint)contour, default, out nuint needed, out int c);
        if (st != Status.Ok && st != Status.BufferTooSmall)
        {
            throw Status.Error(st);
        }
        var xyz = new double[DicomFile.ToInt(needed)];
        if (xyz.Length > 0)
        {
            Status.Check(Native.siaara_rtstruct_contour_points(l.Pointer, (uint)roiIndex, (uint)contour, xyz, out _, out c));
        }
        closed = c != 0;
        var points = new Point3D[xyz.Length / 3];
        for (int i = 0; i < points.Length; i++)
        {
            points[i] = Point3D.From(xyz, 3 * i);
        }
        return points;
    }

    /// <summary>Frees the structure set.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}
