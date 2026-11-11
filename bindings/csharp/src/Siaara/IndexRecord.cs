using System;
using System.Buffers;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>Query / record level.</summary>
public enum DicomLevel
{
    /// <summary>Patient.</summary>
    Patient = C.SIAARA_LEVEL_PATIENT,
    /// <summary>Study.</summary>
    Study = C.SIAARA_LEVEL_STUDY,
    /// <summary>Series.</summary>
    Series = C.SIAARA_LEVEL_SERIES,
    /// <summary>Instance.</summary>
    Instance = C.SIAARA_LEVEL_INSTANCE,
}

/// <summary>Text fields of an index record (SIAARA_IDX_*).</summary>
public enum IndexField
{
    /// <summary>Patient ID.</summary>
    PatientId = C.SIAARA_IDX_PATIENT_ID,
    /// <summary>Issuer of Patient ID.</summary>
    IssuerOfPatientId = C.SIAARA_IDX_ISSUER_OF_PATIENT_ID,
    /// <summary>Patient's Name.</summary>
    PatientName = C.SIAARA_IDX_PATIENT_NAME,
    /// <summary>Patient's Birth Date.</summary>
    PatientBirthDate = C.SIAARA_IDX_PATIENT_BIRTH_DATE,
    /// <summary>Patient's Sex.</summary>
    PatientSex = C.SIAARA_IDX_PATIENT_SEX,
    /// <summary>Study Instance UID.</summary>
    StudyInstanceUid = C.SIAARA_IDX_STUDY_INSTANCE_UID,
    /// <summary>Study Date.</summary>
    StudyDate = C.SIAARA_IDX_STUDY_DATE,
    /// <summary>Study Time.</summary>
    StudyTime = C.SIAARA_IDX_STUDY_TIME,
    /// <summary>Accession Number.</summary>
    AccessionNumber = C.SIAARA_IDX_ACCESSION_NUMBER,
    /// <summary>Study ID.</summary>
    StudyId = C.SIAARA_IDX_STUDY_ID,
    /// <summary>Study Description.</summary>
    StudyDescription = C.SIAARA_IDX_STUDY_DESCRIPTION,
    /// <summary>Referring Physician's Name.</summary>
    ReferringPhysician = C.SIAARA_IDX_REFERRING_PHYSICIAN,
    /// <summary>Series Instance UID.</summary>
    SeriesInstanceUid = C.SIAARA_IDX_SERIES_INSTANCE_UID,
    /// <summary>Modality.</summary>
    Modality = C.SIAARA_IDX_MODALITY,
    /// <summary>Series Description.</summary>
    SeriesDescription = C.SIAARA_IDX_SERIES_DESCRIPTION,
    /// <summary>Body Part Examined.</summary>
    BodyPartExamined = C.SIAARA_IDX_BODY_PART_EXAMINED,
    /// <summary>Series Date.</summary>
    SeriesDate = C.SIAARA_IDX_SERIES_DATE,
    /// <summary>Series Time.</summary>
    SeriesTime = C.SIAARA_IDX_SERIES_TIME,
    /// <summary>Frame of Reference UID.</summary>
    FrameOfReferenceUid = C.SIAARA_IDX_FRAME_OF_REFERENCE_UID,
    /// <summary>SOP Class UID.</summary>
    SopClassUid = C.SIAARA_IDX_SOP_CLASS_UID,
    /// <summary>SOP Instance UID.</summary>
    SopInstanceUid = C.SIAARA_IDX_SOP_INSTANCE_UID,
    /// <summary>Transfer Syntax UID.</summary>
    TransferSyntaxUid = C.SIAARA_IDX_TRANSFER_SYNTAX_UID,
    /// <summary>Photometric Interpretation.</summary>
    Photometric = C.SIAARA_IDX_PHOTOMETRIC,
    /// <summary>Content Date.</summary>
    ContentDate = C.SIAARA_IDX_CONTENT_DATE,
    /// <summary>Content Time.</summary>
    ContentTime = C.SIAARA_IDX_CONTENT_TIME,
    /// <summary>Image Type.</summary>
    ImageType = C.SIAARA_IDX_IMAGE_TYPE,
    /// <summary>Specific Character Set.</summary>
    SpecificCharacterSet = C.SIAARA_IDX_SPECIFIC_CHARACTER_SET,
    /// <summary>Absolute file path (store results; resolved file for DICOMDIR records).</summary>
    FilePath = C.SIAARA_IDX_FILE_PATH,
    /// <summary>Modalities in Study (store results).</summary>
    ModalitiesInStudy = C.SIAARA_IDX_MODALITIES_IN_STUDY,
    /// <summary>Directory Record Type (DICOMDIR).</summary>
    RecordType = C.SIAARA_IDX_RECORD_TYPE,
}

/// <summary>Validity and kind flags of an index record (SIAARA_IDXF_*).</summary>
[Flags]
public enum IndexRecordContents
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>SeriesNumber is valid.</summary>
    SeriesNumber = C.SIAARA_IDXF_SERIES_NUMBER,
    /// <summary>InstanceNumber is valid.</summary>
    InstanceNumber = C.SIAARA_IDXF_INSTANCE_NUMBER,
    /// <summary>ImagePosition is valid.</summary>
    Position = C.SIAARA_IDXF_POSITION,
    /// <summary>ImageOrientation is valid.</summary>
    Orientation = C.SIAARA_IDXF_ORIENTATION,
    /// <summary>The instance has pixel data.</summary>
    PixelData = C.SIAARA_IDXF_PIXEL_DATA,
    /// <summary>The pixel data is encapsulated.</summary>
    Encapsulated = C.SIAARA_IDXF_ENCAPSULATED,
    /// <summary>The file has a Part 10 preamble.</summary>
    Preamble = C.SIAARA_IDXF_PREAMBLE,
    /// <summary>PixelSpacing is valid.</summary>
    PixelSpacing = C.SIAARA_IDXF_PIXEL_SPACING,
    /// <summary>SliceThickness is valid.</summary>
    SliceThickness = C.SIAARA_IDXF_SLICE_THICKNESS,
    /// <summary>Store: indexed in place, not copied.</summary>
    Referenced = C.SIAARA_IDXF_REFERENCED,
    /// <summary>PixelDataOffset / PixelDataLength are valid file offsets.</summary>
    PixelOffset = C.SIAARA_IDXF_PIXEL_OFFSET,
    /// <summary>Store: the instance arrived incomplete and was kept exactly as received.</summary>
    Incomplete = C.SIAARA_IDXF_INCOMPLETE,
    /// <summary>Store: added by a receive path and indexed with the network read options (no recovery of
    /// damaged lengths); a rebuild re-reads the file the same way.</summary>
    Network = C.SIAARA_IDXF_NETWORK,
}

/// <summary>A point or direction in patient coordinates (LPS, mm).</summary>
/// <param name="X">X.</param>
/// <param name="Y">Y.</param>
/// <param name="Z">Z.</param>
public readonly record struct Point3D(double X, double Y, double Z)
{
    internal static Point3D From(ReadOnlySpan<double> v, int offset = 0) => new(v[offset], v[offset + 1], v[offset + 2]);

    internal void CopyTo(Span<double> d)
    {
        d[0] = X;
        d[1] = Y;
        d[2] = Z;
    }
}

/// <summary>
/// One flat record per instance (or per patient / study / series in store query results), for
/// viewer databases (siaara_index_record and its string table).
/// </summary>
public sealed class DicomIndexRecord
{
    private readonly string[] _strings;

    internal DicomIndexRecord(in siaara_index_record r, ReadOnlySpan<byte> table, string? patientKey = null)
    {
        Level = (DicomLevel)r.level;
        Flags = (IndexRecordContents)r.flags;
        SeriesNumber = r.series_number;
        InstanceNumber = r.instance_number;
        NumberOfFrames = (int)r.number_of_frames;
        Rows = (int)r.rows;
        Columns = (int)r.columns;
        BitsAllocated = r.bits_allocated;
        BitsStored = r.bits_stored;
        HighBit = r.high_bit;
        SamplesPerPixel = r.samples_per_pixel;
        PixelRepresentation = r.pixel_representation;
        NumberOfStudies = (int)r.num_studies;
        NumberOfSeries = (int)r.num_series;
        NumberOfInstances = (int)r.num_instances;
        FileSize = (long)r.file_size;
        PixelDataOffset = (long)r.pixel_data_offset;
        PixelDataLength = (long)r.pixel_data_length;
        siaara_index_record copy = r;
        ImagePosition = Point3D.From(copy.image_position);
        ImageOrientationRow = Point3D.From(copy.image_orientation);
        ImageOrientationColumn = Point3D.From(copy.image_orientation, 3);
        ReadOnlySpan<double> ps = copy.pixel_spacing;
        RowSpacing = ps[0];
        ColumnSpacing = ps[1];
        SliceThickness = r.slice_thickness;
        _strings = new string[C.SIAARA_IDX_COUNT];
        ReadOnlySpan<uint> off = copy.str_offset;
        ReadOnlySpan<uint> len = copy.str_length;
        for (int k = 0; k < _strings.Length; k++)
        {
            long end = (long)off[k] + len[k];
            _strings[k] = len[k] == 0 || end > table.Length
                ? string.Empty
                : Encoding.UTF8.GetString(table.Slice((int)off[k], (int)len[k]));
        }
        PatientKey = patientKey;
    }

    /// <summary>Record level.</summary>
    public DicomLevel Level { get; }
    /// <summary>Validity flags.</summary>
    public IndexRecordContents Flags { get; }
    /// <summary>Series Number (see Flags).</summary>
    public int SeriesNumber { get; }
    /// <summary>Instance Number (see Flags).</summary>
    public int InstanceNumber { get; }
    /// <summary>Number of frames; 0 without pixel data.</summary>
    public int NumberOfFrames { get; }
    /// <summary>Rows.</summary>
    public int Rows { get; }
    /// <summary>Columns.</summary>
    public int Columns { get; }
    /// <summary>Bits Allocated.</summary>
    public int BitsAllocated { get; }
    /// <summary>Bits Stored.</summary>
    public int BitsStored { get; }
    /// <summary>High Bit.</summary>
    public int HighBit { get; }
    /// <summary>Samples per Pixel.</summary>
    public int SamplesPerPixel { get; }
    /// <summary>Pixel Representation.</summary>
    public int PixelRepresentation { get; }
    /// <summary>Store results: number of studies below this row.</summary>
    public int NumberOfStudies { get; }
    /// <summary>Store results: number of series below this row.</summary>
    public int NumberOfSeries { get; }
    /// <summary>Store results: number of instances below this row.</summary>
    public int NumberOfInstances { get; }
    /// <summary>File size in bytes.</summary>
    public long FileSize { get; }
    /// <summary>File offset of the Pixel Data value (first item if encapsulated).</summary>
    public long PixelDataOffset { get; }
    /// <summary>Length of the Pixel Data value.</summary>
    public long PixelDataLength { get; }
    /// <summary>Image Position (Patient).</summary>
    public Point3D ImagePosition { get; }
    /// <summary>Image Orientation (Patient), row direction.</summary>
    public Point3D ImageOrientationRow { get; }
    /// <summary>Image Orientation (Patient), column direction.</summary>
    public Point3D ImageOrientationColumn { get; }
    /// <summary>Pixel Spacing, row spacing (mm).</summary>
    public double RowSpacing { get; }
    /// <summary>Pixel Spacing, column spacing (mm).</summary>
    public double ColumnSpacing { get; }
    /// <summary>Slice Thickness (mm).</summary>
    public double SliceThickness { get; }
    /// <summary>Store results: the key identifying the row's patient (for Delete / Move at patient level).</summary>
    public string? PatientKey { get; }

    /// <summary>A text field ("" when empty).</summary>
    /// <param name="field">The field.</param>
    /// <returns>The text.</returns>
    public string GetString(IndexField field) => _strings[(int)field];

    /// <summary>Patient ID.</summary>
    public string PatientId => GetString(IndexField.PatientId);
    /// <summary>Patient's Name.</summary>
    public string PatientName => GetString(IndexField.PatientName);
    /// <summary>Study Instance UID.</summary>
    public string StudyInstanceUid => GetString(IndexField.StudyInstanceUid);
    /// <summary>Series Instance UID.</summary>
    public string SeriesInstanceUid => GetString(IndexField.SeriesInstanceUid);
    /// <summary>SOP Instance UID.</summary>
    public string SopInstanceUid => GetString(IndexField.SopInstanceUid);
    /// <summary>Modality.</summary>
    public string Modality => GetString(IndexField.Modality);
    /// <summary>File path (store results, DICOMDIR records).</summary>
    public string FilePath => GetString(IndexField.FilePath);

    internal static siaara_index_record NewNative() =>
        new() { struct_size = StructSize.Of<siaara_index_record>() };
}

/// <summary>One-parse header extraction (siaara_extract_index).</summary>
public static class DicomIndex
{
    /// <summary>Parses a file's header and pixel data position (pixel data is not decoded).</summary>
    /// <param name="path">File path.</param>
    /// <returns>The record.</returns>
    public static DicomIndexRecord Extract(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        byte[] p = Status.Utf8(path);
        return Read(p, static (byte[] s, ref siaara_index_record r, Span<byte> b, out nuint n) =>
            Native.siaara_extract_index(s, ref r, b, out n));
    }

    /// <summary>Parses an encoded file in memory.</summary>
    /// <param name="data">The file bytes.</param>
    /// <returns>The record.</returns>
    public static DicomIndexRecord Extract(ReadOnlySpan<byte> data)
    {
        byte[] copy = data.ToArray();
        return Read(copy, static (byte[] s, ref siaara_index_record r, Span<byte> b, out nuint n) =>
            Native.siaara_extract_index_memory(s, ref r, b, out n));
    }

    internal delegate int RecordGetter<TState>(TState state, ref siaara_index_record record, Span<byte> buffer, out nuint needed);

    /// <summary>Record read: one call with a 4 KiB pooled table, a second only when that is too small.</summary>
    internal static DicomIndexRecord Read<TState>(TState state, RecordGetter<TState> getter, string? patientKey = null)
    {
        siaara_index_record r = DicomIndexRecord.NewNative();
        byte[] table = ArrayPool<byte>.Shared.Rent(4096);
        try
        {
            int st = getter(state, ref r, table, out nuint needed);
            if (st == Status.BufferTooSmall)
            {
                if (needed > int.MaxValue)
                {
                    throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "string table too large");
                }
                ArrayPool<byte>.Shared.Return(table);
                table = ArrayPool<byte>.Shared.Rent((int)needed);
                st = getter(state, ref r, table, out needed);
            }
            Status.Check(st);
            return new DicomIndexRecord(in r, table.AsSpan(0, (int)needed), patientKey);
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(table);
        }
    }
}
