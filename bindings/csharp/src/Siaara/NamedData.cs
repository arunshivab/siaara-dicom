using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Siaara.Interop;
using Siaara.Modules;

namespace Siaara;

/// <summary>What a path API value is (siaara_path_value.kind).</summary>
public enum DicomValueKind
{
    /// <summary>Present with an empty value.</summary>
    Empty = C.SIAARA_VALUE_EMPTY,
    /// <summary>Text.</summary>
    Text = C.SIAARA_VALUE_TEXT,
    /// <summary>An integer VR (IS, US, SS, UL, SL, SV, UV).</summary>
    IntegerNumber = C.SIAARA_VALUE_INTEGER,
    /// <summary>A real VR (DS, FL, FD).</summary>
    RealNumber = C.SIAARA_VALUE_REAL,
    /// <summary>Bulk binary (OB, OW, OF, OD, OL, OV, UN, encapsulated).</summary>
    Binary = C.SIAARA_VALUE_BINARY,
    /// <summary>A sequence.</summary>
    Sequence = C.SIAARA_VALUE_SEQUENCE,
    /// <summary>AT.</summary>
    Tag = C.SIAARA_VALUE_TAG,
    /// <summary>A sequence item.</summary>
    Item = C.SIAARA_VALUE_ITEM,
}

/// <summary>A value found by a path (siaara_get_by_path): the text form plus a typed form.</summary>
/// <param name="Tag">The element (private tags: as resolved).</param>
/// <param name="VR">VR as stored.</param>
/// <param name="VM">Number of values (items for a sequence).</param>
/// <param name="Kind">What the value is.</param>
/// <param name="Text">All values backslash separated, or the selected one (UTF-8 decoded).</param>
/// <param name="IntegerValue">The selected (or first) value of an integer or AT element.</param>
/// <param name="RealValue">The selected (or first) value of a numeric element.</param>
/// <param name="ItemCount">Items of a sequence.</param>
/// <param name="ByteLength">Value bytes as held.</param>
public sealed record DicomPathValue(DicomTag Tag, string VR, int VM, DicomValueKind Kind, string Text, long? IntegerValue,
    double? RealValue, int ItemCount, ulong ByteLength);

/// <summary>Units source of a modality value (siaara_pixel_value.units_source).</summary>
public enum UnitsSource
{
    /// <summary>The data set does not say.</summary>
    None = C.SIAARA_UNITS_NONE,
    /// <summary>Rescale Type (0028,1054).</summary>
    RescaleType = C.SIAARA_UNITS_RESCALE_TYPE,
    /// <summary>Modality LUT Type (0028,3004).</summary>
    ModalityLutType = C.SIAARA_UNITS_MODALITY_LUT_TYPE,
    /// <summary>Dose Units (3004,0002).</summary>
    DoseUnits = C.SIAARA_UNITS_DOSE_UNITS,
    /// <summary>CT without Rescale Type: HU (PS3.3 C.8.2.1).</summary>
    CtHounsfield = C.SIAARA_UNITS_CT_HU,
    /// <summary>PET Units (0054,1001).</summary>
    PetUnits = C.SIAARA_UNITS_PET_UNITS,
}

/// <summary>A pixel's stored value and its modality value (siaara_pixel_value).</summary>
/// <param name="Frame">0-based frame.</param>
/// <param name="Column">0-based column.</param>
/// <param name="Row">0-based row.</param>
/// <param name="Sample">0-based sample.</param>
/// <param name="Stored">Integer pixel data: the stored value (null for float pixel data).</param>
/// <param name="StoredReal">The stored value as a double.</param>
/// <param name="ModalityValue">Stored value after Modality LUT / rescale / Dose Grid Scaling; null for colour or float data.</param>
/// <param name="RescaleSlope">Slope applied (1 with a LUT).</param>
/// <param name="RescaleIntercept">Intercept applied (0 with a LUT).</param>
/// <param name="FromModalityLut">The value came from a Modality LUT.</param>
/// <param name="DoseGridScaling">RT Dose: the slope is Dose Grid Scaling.</param>
/// <param name="Units">Units text as stored (empty when the data set does not say).</param>
/// <param name="UnitsSource">Where the units come from.</param>
public sealed record PixelValue(int Frame, int Column, int Row, int Sample, long? Stored, double StoredReal, double? ModalityValue,
    double RescaleSlope, double RescaleIntercept, bool FromModalityLut, bool DoseGridScaling, string Units, UnitsSource UnitsSource)
{
    internal static PixelValue From(in siaara_pixel_value v) => new((int)v.frame, (int)v.column, (int)v.row, (int)v.sample,
        v.sample_kind == C.SIAARA_SAMPLES_INTEGER ? v.stored : null, v.stored_real,
        v.has_modality != 0 ? v.modality_value : null, v.rescale_slope, v.rescale_intercept, v.modality_lut != 0,
        v.dose_grid_scaling != 0, DicomDictionary.Chars(v.units), (UnitsSource)v.units_source);
}

/// <summary>NM frame index vectors at one frame (siaara_nm_frame_index): 1-based values as stored, null when the vector is absent or too short.</summary>
/// <param name="Frame">0-based frame.</param>
/// <param name="EnergyWindow">Energy Window Vector (0054,0010).</param>
/// <param name="Detector">Detector Vector (0054,0020).</param>
/// <param name="Phase">Phase Vector (0054,0030).</param>
/// <param name="Rotation">Rotation Vector (0054,0050).</param>
/// <param name="RRInterval">R-R Interval Vector (0054,0060).</param>
/// <param name="TimeSlot">Time Slot Vector (0054,0070).</param>
/// <param name="Slice">Slice Vector (0054,0080).</param>
/// <param name="AngularView">Angular View Vector (0054,0090).</param>
/// <param name="TimeSlice">Time Slice Vector (0054,0100).</param>
/// <param name="InFrameIncrementPointer">SIAARA_NM_* bits of the vectors Frame Increment Pointer (0028,0009) lists.</param>
public sealed record NMFrameIndex(int Frame, int? EnergyWindow, int? Detector, int? Phase, int? Rotation, int? RRInterval,
    int? TimeSlot, int? Slice, int? AngularView, int? TimeSlice, uint InFrameIncrementPointer);

/// <summary>SOP Class information (siaara_sop_class_info).</summary>
/// <param name="Uid">The UID.</param>
/// <param name="Name">PS3.6 name.</param>
/// <param name="Keyword">PS3.6 keyword.</param>
/// <param name="IodName">IOD (PS3.4 B.5-1), or null.</param>
/// <param name="IodTableId">PS3.3 IOD module table, or null.</param>
/// <param name="ObjectKind">SIAARA_OBJECT_* from the IOD's modules.</param>
/// <param name="IsRetired">Retired.</param>
public sealed record SopClassInfo(string Uid, string Name, string Keyword, string? IodName, string? IodTableId, int ObjectKind, bool IsRetired)
{
    /// <summary>Information about a SOP Class UID; null when it is not a PS3.6 SOP Class.</summary>
    /// <param name="uid">The UID.</param>
    /// <returns>The information, or null.</returns>
    public static SopClassInfo? Get(string uid)
    {
        ArgumentNullException.ThrowIfNull(uid);
        var s = new siaara_sop_class_info { struct_size = StructSize.Of<siaara_sop_class_info>() };
        if (Native.siaara_sop_class_info_get(Encoding.ASCII.GetBytes(uid), ref s) != Status.Ok)
        {
            return null;
        }
        return new SopClassInfo(uid, Marshal.PtrToStringUTF8(s.name) ?? "", Marshal.PtrToStringUTF8(s.keyword) ?? "",
            s.has_iod != 0 ? Marshal.PtrToStringUTF8(s.iod_name) : null, s.has_iod != 0 ? Marshal.PtrToStringUTF8(s.iod_table_id) : null,
            s.object_kind, s.retired != 0);
    }
}

/// <summary>Transfer syntax information (siaara_transfer_syntax_info).</summary>
/// <param name="Uid">The UID.</param>
/// <param name="Name">The name.</param>
/// <param name="ExplicitVR">Explicit VR.</param>
/// <param name="BigEndian">Big endian.</param>
/// <param name="Encapsulated">Pixel data encapsulated (compressed or referenced).</param>
/// <param name="Lossy">Lossy.</param>
/// <param name="Deflated">Deflated data set.</param>
/// <param name="IsRetired">Retired.</param>
public sealed record TransferSyntaxInfo(string Uid, string Name, bool ExplicitVR, bool BigEndian, bool Encapsulated, bool Lossy,
    bool Deflated, bool IsRetired)
{
    /// <summary>Information about a transfer syntax UID; null when unknown.</summary>
    /// <param name="uid">The UID.</param>
    /// <returns>The information, or null.</returns>
    public static TransferSyntaxInfo? Get(string uid)
    {
        ArgumentNullException.ThrowIfNull(uid);
        var t = new siaara_transfer_syntax_info { struct_size = StructSize.Of<siaara_transfer_syntax_info>() };
        if (Native.siaara_transfer_syntax_info_get(Encoding.ASCII.GetBytes(uid), ref t) != Status.Ok)
        {
            return null;
        }
        return new TransferSyntaxInfo(uid, Marshal.PtrToStringUTF8(t.name) ?? "", t.explicit_vr != 0, t.big_endian != 0,
            t.encapsulated != 0, t.lossy != 0, t.deflated != 0, t.retired != 0);
    }
}

/// <summary>A waveform channel (PS3.3 C.10.9; siaara_waveform_channel and its texts).</summary>
public sealed class WaveformChannel
{
    private readonly DicomDataset _ds;

    internal WaveformChannel(DicomDataset ds, int group, int index)
    {
        _ds = ds;
        Group = group;
        Index = index;
        var c = new siaara_waveform_channel { struct_size = StructSize.Of<siaara_waveform_channel>() };
        using (var l = new HandleLease(ds.File.Handle))
        {
            Status.Check(Native.siaara_waveform_channel_info(ds.Resolve(l.Pointer), (uint)group, (uint)index, ref c));
        }
        BitsStored = (int)c.bits_stored;
        ChannelNumber = c.has_channel_number != 0 ? c.channel_number : null;
        Sensitivity = c.has_sensitivity != 0 ? c.sensitivity : null;
        CorrectionFactor = c.has_correction != 0 ? c.correction : null;
        Baseline = c.has_baseline != 0 ? c.baseline : null;
        TimeSkew = c.has_time_skew != 0 ? c.time_skew : null;
        SampleSkew = c.has_sample_skew != 0 ? c.sample_skew : null;
        Offset = c.has_offset != 0 ? c.offset : null;
        FilterLowFrequency = c.has_filter_low != 0 ? c.filter_low : null;
        FilterHighFrequency = c.has_filter_high != 0 ? c.filter_high : null;
        NotchFilterFrequency = c.has_notch != 0 ? c.notch : null;
    }

    /// <summary>Multiplex group index.</summary>
    public int Group { get; }
    /// <summary>Channel index (item of Channel Definition Sequence).</summary>
    public int Index { get; }
    /// <summary>Waveform Bits Stored.</summary>
    public int BitsStored { get; }
    /// <summary>Waveform Channel Number.</summary>
    public int? ChannelNumber { get; }
    /// <summary>Channel Sensitivity.</summary>
    public double? Sensitivity { get; }
    /// <summary>Channel Sensitivity Correction Factor.</summary>
    public double? CorrectionFactor { get; }
    /// <summary>Channel Baseline.</summary>
    public double? Baseline { get; }
    /// <summary>Channel Time Skew (s).</summary>
    public double? TimeSkew { get; }
    /// <summary>Channel Sample Skew (samples).</summary>
    public double? SampleSkew { get; }
    /// <summary>Channel Offset (ms).</summary>
    public double? Offset { get; }
    /// <summary>Filter Low Frequency (Hz).</summary>
    public double? FilterLowFrequency { get; }
    /// <summary>Filter High Frequency (Hz).</summary>
    public double? FilterHighFrequency { get; }
    /// <summary>Notch Filter Frequency (Hz).</summary>
    public double? NotchFilterFrequency { get; }
    /// <summary>Channel Label.</summary>
    public string? Label => Text(C.SIAARA_WAVEFORM_LABEL);
    /// <summary>Channel Status (values backslash separated).</summary>
    public string? ChannelStatus => Text(C.SIAARA_WAVEFORM_STATUS);
    /// <summary>Channel Sensitivity Units Sequence code value (UCUM).</summary>
    public string? UnitsCode => Text(C.SIAARA_WAVEFORM_UNITS_CODE);
    /// <summary>Channel Sensitivity Units Sequence code meaning.</summary>
    public string? UnitsMeaning => Text(C.SIAARA_WAVEFORM_UNITS_MEANING);
    /// <summary>Channel Source Sequence code value.</summary>
    public string? SourceCode => Text(C.SIAARA_WAVEFORM_SOURCE_CODE);
    /// <summary>Channel Source Sequence coding scheme designator.</summary>
    public string? SourceScheme => Text(C.SIAARA_WAVEFORM_SOURCE_SCHEME);
    /// <summary>Channel Source Sequence code meaning.</summary>
    public string? SourceMeaning => Text(C.SIAARA_WAVEFORM_SOURCE_MEANING);

    /// <summary>The stored samples (sign-extended; MB / AB: the 8-bit codes).</summary>
    /// <returns>The samples.</returns>
    public long[] RawSamples()
    {
        using var l = new HandleLease(_ds.File.Handle);
        var st = (Ds: _ds.Resolve(l.Pointer), G: (uint)Group, C: (uint)Index);
        return Status.ReadArray<long, (nint Ds, uint G, uint C)>(st, static (s, b, out n) =>
            Native.siaara_waveform_raw_samples(s.Ds, s.G, s.C, b, out n));
    }

    /// <summary>
    /// Samples in Channel Sensitivity Units: sample x Sensitivity x Correction Factor + Baseline
    /// (PS3.3 C.10.9.1.4.2; G.711 codes expanded first; NaN for the Waveform Padding Value).
    /// Null when the channel has no Channel Sensitivity (arbitrary units).
    /// </summary>
    /// <returns>The samples, or null.</returns>
    public double[]? ScaledSamples()
    {
        if (Sensitivity is null)
        {
            return null;
        }
        using var l = new HandleLease(_ds.File.Handle);
        var st = (Ds: _ds.Resolve(l.Pointer), G: (uint)Group, C: (uint)Index);
        return Status.ReadArray<double, (nint Ds, uint G, uint C)>(st, static (s, b, out n) =>
            Native.siaara_waveform_samples(s.Ds, s.G, s.C, 1, b, out n));
    }

    private string? Text(int which)
    {
        using var l = new HandleLease(_ds.File.Handle);
        var st = (Ds: _ds.Resolve(l.Pointer), G: (uint)Group, C: (uint)Index, W: which);
        return Status.TryReadString(st, static (s, b, out n) =>
            Native.siaara_waveform_channel_string(s.Ds, s.G, s.C, s.W, b, out n), out string v) == Status.Ok ? v : null;
    }
}

/// <summary>A waveform multiplex group (item of Waveform Sequence; siaara_waveform_group).</summary>
public sealed class WaveformGroup
{
    private readonly DicomDataset _ds;

    internal WaveformGroup(DicomDataset ds, int index)
    {
        _ds = ds;
        Index = index;
        var g = new siaara_waveform_group { struct_size = StructSize.Of<siaara_waveform_group>() };
        using (var l = new HandleLease(ds.File.Handle))
        {
            Status.Check(Native.siaara_waveform_group_info(ds.Resolve(l.Pointer), (uint)index, ref g));
        }
        ChannelCount = (int)g.channels;
        SampleCount = (int)g.samples;
        DataSamples = (int)g.data_samples;
        BitsAllocated = (int)g.bits_allocated;
        SamplingFrequency = g.sampling_frequency;
        SampleInterpretation = DicomDictionary.Chars(g.sample_interpretation);
        PaddingValue = g.has_padding != 0 ? g.padding_value : null;
        TimeOffset = g.has_time_offset != 0 ? g.time_offset : null;
        TriggerTimeOffset = g.has_trigger_time_offset != 0 ? g.trigger_time_offset : null;
    }

    /// <summary>Index in Waveform Sequence.</summary>
    public int Index { get; }
    /// <summary>Number of Waveform Channels.</summary>
    public int ChannelCount { get; }
    /// <summary>Number of Waveform Samples.</summary>
    public int SampleCount { get; }
    /// <summary>Samples per channel Waveform Data actually holds.</summary>
    public int DataSamples { get; }
    /// <summary>Waveform Bits Allocated.</summary>
    public int BitsAllocated { get; }
    /// <summary>Sampling Frequency (Hz).</summary>
    public double SamplingFrequency { get; }
    /// <summary>Waveform Sample Interpretation.</summary>
    public string SampleInterpretation { get; }
    /// <summary>Waveform Padding Value, as a stored sample.</summary>
    public long? PaddingValue { get; }
    /// <summary>Multiplex Group Time Offset (ms).</summary>
    public double? TimeOffset { get; }
    /// <summary>Trigger Time Offset (ms).</summary>
    public double? TriggerTimeOffset { get; }

    /// <summary>Multiplex Group Label.</summary>
    public string? Label
    {
        get
        {
            using var l = new HandleLease(_ds.File.Handle);
            var st = (Ds: _ds.Resolve(l.Pointer), G: (uint)Index);
            return Status.TryReadString(st, static (s, b, out n) =>
                Native.siaara_waveform_channel_string(s.Ds, s.G, 0, C.SIAARA_WAVEFORM_GROUP_LABEL, b, out n), out string v) == Status.Ok ? v : null;
        }
    }

    /// <summary>The channels (Channel Definition Sequence items present in the data set).</summary>
    public IReadOnlyList<WaveformChannel> Channels
    {
        get
        {
            var items = _ds.GetItem(new DicomTag(0x54000100u), Index).GetSequence(new DicomTag(0x003A0200u));
            var r = new WaveformChannel[items.Count];
            for (int i = 0; i < r.Length; i++)
            {
                r[i] = new WaveformChannel(_ds, Index, i);
            }
            return r;
        }
    }
}

/// <summary>Named data layer of a file (see docs/DATA_ACCESS.md).</summary>
public sealed partial class DicomFile
{
    /// <summary>Every PS3.3 module and macro of the main data set.</summary>
    public DicomModules Modules => new(Dataset);
    /// <summary>Patient Module (with the Name / FirstName / LastName / FullName composites).</summary>
    public PatientModule Patient => new(Dataset);
    /// <summary>General Study Module.</summary>
    public GeneralStudyModule Study => new(Dataset);
    /// <summary>General Series Module.</summary>
    public GeneralSeriesModule Series => new(Dataset);
    /// <summary>General Equipment Module (with the MakeAndModel composite).</summary>
    public GeneralEquipmentModule Equipment => new(Dataset);
    /// <summary>Enhanced General Equipment Module.</summary>
    public EnhancedGeneralEquipmentModule EnhancedEquipment => new(Dataset);
    /// <summary>Frame of Reference Module.</summary>
    public FrameOfReferenceModule FrameOfReference => new(Dataset);
    /// <summary>General Image Module.</summary>
    public GeneralImageModule Image => new(Dataset);
    /// <summary>Image Plane Module.</summary>
    public ImagePlaneModule ImagePlane => new(Dataset);
    /// <summary>Image Pixel Module.</summary>
    public ImagePixelModule ImagePixel => new(Dataset);
    /// <summary>SOP Common Module.</summary>
    public SOPCommonModule SOPCommon => new(Dataset);
    /// <summary>CT Image Module.</summary>
    public CTImageModule CTImage => new(Dataset);
    /// <summary>MR Image Module.</summary>
    public MRImageModule MRImage => new(Dataset);
    /// <summary>Modality LUT Module.</summary>
    public ModalityLUTModule ModalityLut => new(Dataset);
    /// <summary>VOI LUT Module.</summary>
    public VOILUTModule VoiLut => new(Dataset);
    /// <summary>RT General Plan Module.</summary>
    public RTGeneralPlanModule RTPlan => new(Dataset);
    /// <summary>RT Fraction Scheme Module (fraction groups, MU per beam through Referenced Beam Sequence).</summary>
    public RTFractionSchemeModule RTFractionScheme => new(Dataset);
    /// <summary>RT Beams Module (beams, devices, control points).</summary>
    public RTBeamsModule RTBeams => new(Dataset);
    /// <summary>RT Ion Beams Module.</summary>
    public RTIonBeamsModule RTIonBeams => new(Dataset);
    /// <summary>RT Prescription Module.</summary>
    public RTPrescriptionModule RTPrescription => new(Dataset);
    /// <summary>RT Tolerance Tables Module.</summary>
    public RTToleranceTablesModule RTToleranceTables => new(Dataset);
    /// <summary>RT Patient Setup Module.</summary>
    public RTPatientSetupModule RTPatientSetup => new(Dataset);
    /// <summary>RT Dose Module.</summary>
    public RTDoseModule RTDose => new(Dataset);
    /// <summary>RT DVH Module.</summary>
    public RTDVHModule RTDvh => new(Dataset);
    /// <summary>Structure Set Module.</summary>
    public StructureSetModule RTStructureSet => new(Dataset);
    /// <summary>ROI Contour Module.</summary>
    public ROIContourModule RTRoiContour => new(Dataset);
    /// <summary>RT ROI Observations Module.</summary>
    public RTROIObservationsModule RTRoiObservations => new(Dataset);
    /// <summary>RT Image Module.</summary>
    public RTImageModule RTImage => new(Dataset);
    /// <summary>RT General Treatment Record Module.</summary>
    public RTGeneralTreatmentRecordModule RTTreatmentRecord => new(Dataset);
    /// <summary>RT Beams Session Record Module (RT Beams Treatment Record IOD).</summary>
    public RTBeamsSessionRecordModule RTBeamsTreatmentRecord => new(Dataset);
    /// <summary>RT Ion Beams Session Record Module.</summary>
    public RTIonBeamsSessionRecordModule RTIonBeamsTreatmentRecord => new(Dataset);
    /// <summary>RT Brachy Session Record Module.</summary>
    public RTBrachySessionRecordModule RTBrachyTreatmentRecord => new(Dataset);
    /// <summary>RT Brachy Application Setups Module (setups, channels, sources, brachy control points).</summary>
    public RTBrachyApplicationSetupsModule RTBrachyApplicationSetups => new(Dataset);
    /// <summary>NM Image Module.</summary>
    public NMImageModule NMImage => new(Dataset);
    /// <summary>NM Multi-frame Module (Frame Increment Pointer and index vectors).</summary>
    public NMMultiFrameModule NMMultiFrame => new(Dataset);
    /// <summary>NM Detector Module.</summary>
    public NMDetectorModule NMDetector => new(Dataset);
    /// <summary>NM Isotope Module (energy windows, radiopharmaceuticals).</summary>
    public NMIsotopeModule NMIsotope => new(Dataset);
    /// <summary>Energy Window Information Sequence of the NM Isotope Module.</summary>
    public IReadOnlyList<EnergyWindowInformationItem> EnergyWindows => NMIsotope.EnergyWindowInformationSequence;
    /// <summary>NM TOMO Acquisition Module.</summary>
    public NMTomoAcquisitionModule NMTomoAcquisition => new(Dataset);
    /// <summary>NM Multi-gated Acquisition Module (Gated Information Sequence > Data Information Sequence).</summary>
    public NMMultiGatedAcquisitionModule NMGating => new(Dataset);
    /// <summary>NM Phase Module.</summary>
    public NMPhaseModule NMPhase => new(Dataset);
    /// <summary>NM Reconstruction Module.</summary>
    public NMReconstructionModule NMReconstruction => new(Dataset);
    /// <summary>Cardiac Synchronization Module.</summary>
    public CardiacSynchronizationModule Cardiac => new(Dataset);
    /// <summary>Respiratory Synchronization Module.</summary>
    public RespiratorySynchronizationModule Respiratory => new(Dataset);
    /// <summary>PET Series Module.</summary>
    public PETSeriesModule PETSeries => new(Dataset);
    /// <summary>PET Isotope Module (radiopharmaceutical information).</summary>
    public PETIsotopeModule PETIsotope => new(Dataset);
    /// <summary>PET Image Module.</summary>
    public PETImageModule PETImage => new(Dataset);
    /// <summary>PET Multi-gated Acquisition Module.</summary>
    public PETMultiGatedAcquisitionModule PETGating => new(Dataset);
    /// <summary>XA Positioner Module.</summary>
    public XAPositionerModule XAPositioner => new(Dataset);
    /// <summary>XA/XRF Acquisition Module.</summary>
    public XAXRFAcquisitionModule XAAcquisition => new(Dataset);
    /// <summary>Mask Module (Mask Subtraction Sequence).</summary>
    public MaskModule Mask => new(Dataset);
    /// <summary>US Region Calibration Module.</summary>
    public USRegionCalibrationModule USRegionCalibration => new(Dataset);
    /// <summary>Sequence of Ultrasound Regions.</summary>
    public IReadOnlyList<SequenceOfUltrasoundRegionsItem> USRegions => USRegionCalibration.SequenceOfUltrasoundRegions;
    /// <summary>US Image Module.</summary>
    public USImageModule USImage => new(Dataset);
    /// <summary>Waveform Module (raw attributes; see <see cref="Waveforms"/> for samples).</summary>
    public WaveformModule Waveform => new(Dataset);
    /// <summary>The multiplex groups with their channels and samples.</summary>
    public IReadOnlyList<WaveformGroup> Waveforms
    {
        get
        {
            DicomDataset ds = Dataset;
            var tag = new DicomTag(0x54000100u);
            int n = ds.Contains(tag) ? ds.GetItemCount(tag) : 0;
            var r = new WaveformGroup[n];
            for (int i = 0; i < n; i++)
            {
                r[i] = new WaveformGroup(ds, i);
            }
            return r;
        }
    }
    /// <summary>Multi-frame Functional Groups Module (Shared / Per-Frame Functional Groups).</summary>
    public MultiFrameFunctionalGroupsModule FunctionalGroups => new(Dataset);
    /// <summary>Multi-frame Dimension Module.</summary>
    public MultiFrameDimensionModule Dimensions => new(Dataset);
    /// <summary>Enhanced CT Image Module.</summary>
    public EnhancedCTImageModule EnhancedCTImage => new(Dataset);
    /// <summary>Enhanced MR Image Module.</summary>
    public EnhancedMRImageModule EnhancedMRImage => new(Dataset);
    /// <summary>SR Document General Module.</summary>
    public SRDocumentGeneralModule SRDocument => new(Dataset);
    /// <summary>SR Document Content Module (the content tree).</summary>
    public SRDocumentContentModule SR => new(Dataset);
    /// <summary>Segmentation Image Module.</summary>
    public SegmentationImageModule Segmentation => new(Dataset);
    /// <summary>Mammography Image Module.</summary>
    public MammographyImageModule MG => new(Dataset);
    /// <summary>DX Image Module.</summary>
    public DXImageModule DXImage => new(Dataset);
    /// <summary>Ophthalmic Photography Image Module.</summary>
    public OphthalmicPhotographyImageModule Ophthalmic => new(Dataset);
    /// <summary>Ophthalmic Tomography Image Module.</summary>
    public OphthalmicTomographyImageModule OphthalmicTomography => new(Dataset);

    /// <summary>True when at least one attribute of the named module is present (siaara_module_present).</summary>
    /// <param name="name">"RT Beams", "RT Beams Module", "C.8-50" or a class name.</param>
    /// <returns>True when present.</returns>
    public bool IsModulePresent(string name) => Modules.IsPresent(name);

    /// <summary>
    /// The stored value at (column, row) of a frame and its modality value (Modality LUT / rescale,
    /// or Dose Grid Scaling for RT Dose) with its units. Decodes the frame on every call: for many
    /// points use <see cref="DicomFrame.GetPixelValue"/>.
    /// </summary>
    /// <param name="frame">0-based frame.</param>
    /// <param name="column">0-based column.</param>
    /// <param name="row">0-based row.</param>
    /// <param name="sample">0-based sample (0 for grayscale).</param>
    /// <returns>The value.</returns>
    public PixelValue GetPixelValue(int frame, int column, int row, int sample = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        ArgumentOutOfRangeException.ThrowIfNegative(column);
        ArgumentOutOfRangeException.ThrowIfNegative(row);
        ArgumentOutOfRangeException.ThrowIfNegative(sample);
        var v = new siaara_pixel_value { struct_size = StructSize.Of<siaara_pixel_value>() };
        using var l = new HandleLease(Handle);
        Status.Check(Native.siaara_file_pixel_value(l.Pointer, (uint)frame, (uint)column, (uint)row, (uint)sample, ref v));
        return PixelValue.From(v);
    }

    /// <summary>NM index vectors at frame k (siaara_nm_frame_index_get).</summary>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The values.</returns>
    public NMFrameIndex GetNMFrameIndex(int frame)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        var x = new siaara_nm_frame_index { struct_size = StructSize.Of<siaara_nm_frame_index>() };
        using (var l = new HandleLease(Handle))
        {
            Status.Check(Native.siaara_nm_frame_index_get(Dataset.Resolve(l.Pointer), (uint)frame, ref x));
        }
        int? V(uint bit, uint v) => (x.present & bit) != 0 && v != 0 ? (int)v : null;
        return new NMFrameIndex(frame, V(C.SIAARA_NM_ENERGY_WINDOW, x.energy_window), V(C.SIAARA_NM_DETECTOR, x.detector),
            V(C.SIAARA_NM_PHASE, x.phase), V(C.SIAARA_NM_ROTATION, x.rotation), V(C.SIAARA_NM_RR_INTERVAL, x.rr_interval),
            V(C.SIAARA_NM_TIME_SLOT, x.time_slot), V(C.SIAARA_NM_SLICE, x.slice), V(C.SIAARA_NM_ANGULAR_VIEW, x.angular_view),
            V(C.SIAARA_NM_TIME_SLICE, x.time_slice), x.in_pointer);
    }
}

/// <summary>Path API and generated classes on any data set.</summary>
public sealed partial class DicomDataset
{
    /// <summary>Every PS3.3 module and macro read from this data set (also works on sequence items).</summary>
    public DicomModules Modules => new(this);

    /// <summary>
    /// Looks up a path such as "BeamSequence[0].ControlPointSequence[3].GantryAngle" (keywords,
    /// (gggg,eeee), item / value indices, private "creator:gggg,xx"). See docs/DATA_ACCESS.md.
    /// </summary>
    /// <param name="path">The path.</param>
    /// <returns>The value, or null when an element on the path is absent.</returns>
    /// <exception cref="SiaaraException">Bad path syntax (InvalidArgument) or a non-sequence used as one (TypeMismatch).</exception>
    public DicomPathValue? GetByPath(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        byte[] p = Encoding.UTF8.GetBytes(path);
        var v = new siaara_path_value { struct_size = StructSize.Of<siaara_path_value>() };
        using var l = new HandleLease(_file.Handle);
        nint ds = Resolve(l.Pointer);
        Span<byte> stack = stackalloc byte[256];
        int st = Native.siaara_get_by_path(ds, p, ref v, stack, out nuint needed);
        string text;
        if (st == Status.Ok)
        {
            text = Encoding.UTF8.GetString(stack[..(int)needed]);
        }
        else if (st == C.SIAARA_ERR_BUFFER_TOO_SMALL)
        {
            byte[] big = new byte[checked((int)needed + 1)];
            Status.Check(Native.siaara_get_by_path(ds, p, ref v, big, out needed));
            text = Encoding.UTF8.GetString(big, 0, (int)needed);
        }
        else if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return null;
        }
        else
        {
            throw Status.Error(st);
        }
        return new DicomPathValue(new DicomTag(v.tag), DicomDictionary.Chars(v.vr), (int)v.vm, (DicomValueKind)v.kind, text,
            v.has_integer != 0 ? v.integer : null, v.has_real != 0 ? v.real : null, (int)v.item_count, v.byte_length);
    }

    /// <summary>Every value of a numeric element at a path as doubles; null when absent.</summary>
    /// <param name="path">The path.</param>
    /// <returns>The values, or null.</returns>
    public double[]? GetDoublesByPath(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        byte[] p = Encoding.UTF8.GetBytes(path);
        using var l = new HandleLease(_file.Handle);
        nint ds = Resolve(l.Pointer);
        int st = Native.siaara_get_doubles_by_path(ds, p, default, out nuint n);
        if (st == C.SIAARA_ERR_NOT_FOUND)
        {
            return null;
        }
        Status.Check(st);
        var r = new double[(int)n];
        if (n > 0)
        {
            Status.Check(Native.siaara_get_doubles_by_path(ds, p, r, out _));
        }
        return r;
    }
}

/// <summary>Values at a point of a decoded frame.</summary>
public sealed partial class DicomFrame
{
    /// <summary>The stored value at (column, row) and its modality value (see <see cref="DicomFile.GetPixelValue"/>).</summary>
    /// <param name="column">0-based column.</param>
    /// <param name="row">0-based row.</param>
    /// <param name="sample">0-based sample (0 for grayscale).</param>
    /// <returns>The value.</returns>
    public PixelValue GetPixelValue(int column, int row, int sample = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(column);
        ArgumentOutOfRangeException.ThrowIfNegative(row);
        ArgumentOutOfRangeException.ThrowIfNegative(sample);
        var v = new siaara_pixel_value { struct_size = StructSize.Of<siaara_pixel_value>() };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_frame_pixel_value(l.Pointer, (uint)column, (uint)row, (uint)sample, ref v));
        return PixelValue.From(v);
    }
}

/// <summary>Dictionary enumeration and search.</summary>
public static partial class DicomDictionary
{
    /// <summary>Number of dictionary entries (siaara_dict_count).</summary>
    public static int Count
    {
        get
        {
            Status.Check(Native.siaara_dict_count(out uint n));
            return (int)n;
        }
    }

    /// <summary>Entry index (0 .. Count-1): public elements in tag order, then repeating groups.</summary>
    /// <param name="index">The index.</param>
    /// <returns>The entry.</returns>
    public static DicomDictionaryEntry EntryAt(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        var e = new siaara_dict_entry { struct_size = StructSize.Of<siaara_dict_entry>() };
        Status.Check(Native.siaara_dict_entry_at((uint)index, ref e));
        return Convert(e);
    }

    /// <summary>Entries whose name and / or keyword contain text (case-insensitive) and whose VR matches.</summary>
    /// <param name="text">Text to look for; empty matches everything.</param>
    /// <param name="vr">Two-character VR, or null for any.</param>
    /// <param name="matchName">Search names.</param>
    /// <param name="matchKeyword">Search keywords.</param>
    /// <param name="includeRetired">Include retired entries.</param>
    /// <returns>The entries.</returns>
    public static IReadOnlyList<DicomDictionaryEntry> Search(string text, string? vr = null, bool matchName = true,
        bool matchKeyword = true, bool includeRetired = false)
    {
        ArgumentNullException.ThrowIfNull(text);
        int flags = (matchName ? C.SIAARA_DICT_MATCH_NAME : 0) | (matchKeyword ? C.SIAARA_DICT_MATCH_KEYWORD : 0) |
                    (includeRetired ? C.SIAARA_DICT_INCLUDE_RETIRED : 0);
        byte[] t = Encoding.UTF8.GetBytes(text);
        byte[]? v = vr is null ? null : Encoding.ASCII.GetBytes(vr + "\0");
        Status.Check(Native.siaara_dict_search(t, v, flags, default, out nuint n));
        var idx = new uint[(int)n];
        if (n > 0)
        {
            Status.Check(Native.siaara_dict_search(t, v, flags, idx, out _));
        }
        var r = new DicomDictionaryEntry[idx.Length];
        for (int i = 0; i < idx.Length; i++)
        {
            r[i] = EntryAt((int)idx[i]);
        }
        return r;
    }
}
