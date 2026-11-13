// Grouping of files that arrived together (the members of a ZIP from a mail, with or without a DICOMDIR) into
// patient / study / series, each series ordered for scrolling, and what a DICOMDIR lists set against what arrived.
// All from bytes in memory: nothing is written to disk and no file's bytes are kept. The input is UNTRUSTED.
// docs/GETTING_STARTED.md, "Grouping a ZIP or a DICOMDIR".
using System;
using System.Collections.Generic;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>How the instances of a series are ordered.</summary>
public enum DicomGroupingOrder
{
    /// <summary>Along the slice normal (dot(Image Position (Patient), row x column of Image Orientation (Patient)));
    /// files without a usable position after, by Slice Location, then by Instance Number, then as added.</summary>
    Position = C.SIAARA_GROUP_ORDER_POSITION,
    /// <summary>By Instance Number; files without one after, as added.</summary>
    InstanceNumber = C.SIAARA_GROUP_ORDER_INSTANCE_NUMBER,
    /// <summary>In the order the files were added.</summary>
    AsAdded = C.SIAARA_GROUP_ORDER_AS_ADDED,
}

/// <summary>What happens to a file whose SOP Instance UID an earlier file has.</summary>
public enum DicomDuplicatePolicy
{
    /// <summary>Both are kept; the second is marked <see cref="DicomGroupedInstance.IsDuplicate"/>.</summary>
    KeepBoth = C.SIAARA_GROUP_DUPLICATES_KEEP_BOTH,
    /// <summary>Only the first is kept; an issue names the second.</summary>
    KeepFirst = C.SIAARA_GROUP_DUPLICATES_KEEP_FIRST,
}

/// <summary>The order of the series inside each study.</summary>
public enum DicomGroupingSeriesOrder
{
    /// <summary>The order their first file was added in. The default.</summary>
    AsAdded = C.SIAARA_GROUP_SERIES_ORDER_AS_ADDED,
    /// <summary>By Series Number of each series' first file, ascending. Series without a Series Number, or with one
    /// that is not a valid IS, come after the numbered ones in the order they were added; ties keep the order they
    /// were added in. Values are not changed.</summary>
    SeriesNumber = C.SIAARA_GROUP_SERIES_ORDER_SERIES_NUMBER,
}

/// <summary>What orders a series (order Position) in which some instances have no usable position.</summary>
public enum DicomPositionFallback
{
    /// <summary>The whole series by one rule: Slice Location when every instance has one, else Instance Number when
    /// every instance has one, else as added. The default: the order of a series is one consistent rule.</summary>
    PerSeries = C.SIAARA_GROUP_FALLBACK_PER_SERIES,
    /// <summary>The positioned instances by position, the others after them by Slice Location, then Instance
    /// Number, then as added.</summary>
    PerInstance = C.SIAARA_GROUP_FALLBACK_PER_INSTANCE,
}

/// <summary>How instances with equal sort keys are ordered (a time series, diffusion, a test bolus at one
/// position). Always followed by the order the files were added in.</summary>
public enum DicomTieBreak
{
    /// <summary>Instances with an Instance Number first, by it. The default.</summary>
    InstanceNumber = C.SIAARA_GROUP_TIE_INSTANCE_NUMBER,
    /// <summary>The order the files were added in.</summary>
    AsAdded = C.SIAARA_GROUP_TIE_AS_ADDED,
    /// <summary>Acquisition Time, else Content Time; instances with one first.</summary>
    AcquisitionTime = C.SIAARA_GROUP_TIE_ACQUISITION_TIME,
}

/// <summary>How strict a "valid UID" (one that groups) is.</summary>
public enum DicomUidValidation
{
    /// <summary>PS3.5 9.1: 1 to 64 characters, components of digits separated by single dots, no empty component,
    /// no leading zero unless the component is "0". The default.</summary>
    Strict = C.SIAARA_GROUP_UID_STRICT,
    /// <summary>1 to 64 digits and dots.</summary>
    Lenient = C.SIAARA_GROUP_UID_LENIENT,
}

/// <summary>The rule a series was ordered by.</summary>
public enum DicomSeriesOrderRule
{
    /// <summary>Every instance by its position along the normal.</summary>
    Position = C.SIAARA_GSERIES_ORDER_POSITION,
    /// <summary>Every instance by Slice Location.</summary>
    SliceLocation = C.SIAARA_GSERIES_ORDER_SLICE_LOCATION,
    /// <summary>Every instance by Instance Number.</summary>
    InstanceNumber = C.SIAARA_GSERIES_ORDER_INSTANCE_NUMBER,
    /// <summary>Every instance as added.</summary>
    Added = C.SIAARA_GSERIES_ORDER_ADDED,
    /// <summary>Tiers: each instance's <see cref="DicomGroupedInstance.SortKey"/> says which key placed it.</summary>
    Mixed = C.SIAARA_GSERIES_ORDER_MIXED,
}

/// <summary>Options of a <see cref="DicomStudyGrouping"/>: the budgets of the grouping as a whole, the per-file read
/// limits of the attachment preview, and how the result is formed. The defaults are the recommended values for
/// untrusted input. A limit that is zero or negative is an error (<see cref="ArgumentOutOfRangeException"/>): to the
/// library below, 0 means "the default", and a zeroed limit must not silently become that.</summary>
public sealed record DicomGroupingOptions
{
    /// <summary>Most files read; later files are skipped. Default 10,000.</summary>
    public int MaxFiles { get; init; } = DefaultMaxFiles;
    /// <summary>A larger file is skipped. Default 64 MiB.</summary>
    public long MaxFileBytes { get; init; } = DefaultMaxFileBytes;
    /// <summary>Sizes of all files (and DICOMDIRs) read, together. Default 2 GiB.</summary>
    public long MaxTotalBytes { get; init; } = DefaultMaxTotalBytes;
    /// <summary>What deflated files inflate to while their headers are read, together. Default 2 GiB.</summary>
    public long MaxTotalInflated { get; init; } = DefaultMaxTotalInflated;
    /// <summary>All text kept: stored values plus display-safe copies (names, UIDs, text fields, File IDs). Default 64 MiB.</summary>
    public long MaxResultBytes { get; init; } = DefaultMaxResultBytes;
    /// <summary>A longer UID is not kept (<see cref="DicomGroupedTextStatus.TooLong"/>). Default 64.</summary>
    public int MaxUidChars { get; init; } = DefaultMaxUidChars;
    /// <summary>Other text (names and File IDs too): a longer one is not kept. Default 1,024 bytes (UTF-8).</summary>
    public int MaxTextBytes { get; init; } = DefaultMaxTextBytes;
    /// <summary>Per file: deepest sequence nesting read. Default 16.</summary>
    public int MaxDepth { get; init; } = DefaultMaxDepth;
    /// <summary>Per file: most elements and sequence items read. Default 200,000.</summary>
    public long MaxElements { get; init; } = DefaultMaxElements;
    /// <summary>Per file: what a deflated file may inflate to. Default 128 MiB.</summary>
    public long MaxInflatedSize { get; init; } = DefaultMaxInflatedSize;
    /// <summary>Per value: a longer stored value is not decoded (<see cref="DicomGroupedTextStatus.TooLong"/>). Default 65,536.</summary>
    public long MaxValueBytes { get; init; } = DefaultMaxValueBytes;
    /// <summary>Order within a series. Default <see cref="DicomGroupingOrder.Position"/>.</summary>
    public DicomGroupingOrder Order { get; init; } = DicomGroupingOrder.Position;
    /// <summary>Return Patient's Name and Patient ID (personal data: decide who may see them first). Default false:
    /// they are <see cref="DicomGroupedTextStatus.NotRequested"/> (patients are still told apart by Patient ID).</summary>
    public bool IncludePatientFields { get; init; }
    /// <summary>Files with a SOP Instance UID an earlier file has. Default <see cref="DicomDuplicatePolicy.KeepBoth"/>.</summary>
    public DicomDuplicatePolicy Duplicates { get; init; } = DicomDuplicatePolicy.KeepBoth;
    /// <summary>DICOMDIR matching: ASCII letters match in any case. Default true.</summary>
    public bool MatchCaseInsensitive { get; init; } = true;
    /// <summary>DICOMDIR matching: a File ID of N components matches the last N components of a name (a ZIP's
    /// wrapping folder is ignored). Off: the whole name must match. Default true.</summary>
    public bool MatchByTrailingComponents { get; init; } = true;
    /// <summary>Largest difference of any direction cosine of Image Orientation (Patient) from the series' reference
    /// orientation for which a file's position along the normal is still used. Default 1e-4.</summary>
    public double OrientationTolerance { get; init; } = DefaultOrientationTolerance;
    /// <summary>Order Position, some positions unusable. Default <see cref="DicomPositionFallback.PerSeries"/>.</summary>
    public DicomPositionFallback PositionFallback { get; init; } = DicomPositionFallback.PerSeries;
    /// <summary>Equal sort keys. Default <see cref="DicomTieBreak.InstanceNumber"/>, then as added.</summary>
    public DicomTieBreak TieBreak { get; init; } = DicomTieBreak.InstanceNumber;
    /// <summary>Which UIDs group. Default <see cref="DicomUidValidation.Strict"/>. An invalid Study / Series UID makes
    /// the instance ungrouped; the stored value is still returned untouched.</summary>
    public DicomUidValidation UidValidation { get; init; } = DicomUidValidation.Strict;
    /// <summary>Most <see cref="DicomStudyGrouping.AddDicomdir"/> calls read; later ones are refused (an issue).
    /// 1 to <see cref="MaxDicomdirsLimit"/>. Default 4.</summary>
    public int MaxDicomdirs { get; init; } = DefaultMaxDicomdirs;
    /// <summary>The order of the series inside each study. Default <see cref="DicomGroupingSeriesOrder.AsAdded"/>.</summary>
    public DicomGroupingSeriesOrder SeriesOrder { get; init; } = DicomGroupingSeriesOrder.AsAdded;

    /// <summary>Default of <see cref="MaxFiles"/>.</summary>
    public const int DefaultMaxFiles = (int)C.SIAARA_GROUPING_DEFAULT_MAX_FILES;
    /// <summary>Default of <see cref="MaxFileBytes"/>.</summary>
    public const long DefaultMaxFileBytes = C.SIAARA_GROUPING_DEFAULT_MAX_FILE_BYTES;
    /// <summary>Default of <see cref="MaxTotalBytes"/>.</summary>
    public const long DefaultMaxTotalBytes = C.SIAARA_GROUPING_DEFAULT_MAX_TOTAL_BYTES;
    /// <summary>Default of <see cref="MaxTotalInflated"/>.</summary>
    public const long DefaultMaxTotalInflated = C.SIAARA_GROUPING_DEFAULT_MAX_TOTAL_INFLATED;
    /// <summary>Default of <see cref="MaxResultBytes"/>.</summary>
    public const long DefaultMaxResultBytes = C.SIAARA_GROUPING_DEFAULT_MAX_RESULT_BYTES;
    /// <summary>Default of <see cref="MaxUidChars"/>.</summary>
    public const int DefaultMaxUidChars = (int)C.SIAARA_GROUPING_DEFAULT_MAX_UID_CHARS;
    /// <summary>Default of <see cref="MaxTextBytes"/>.</summary>
    public const int DefaultMaxTextBytes = (int)C.SIAARA_GROUPING_DEFAULT_MAX_TEXT_BYTES;
    /// <summary>Default of <see cref="MaxDepth"/>.</summary>
    public const int DefaultMaxDepth = (int)C.SIAARA_GROUPING_DEFAULT_MAX_DEPTH;
    /// <summary>Default of <see cref="MaxElements"/>.</summary>
    public const long DefaultMaxElements = C.SIAARA_GROUPING_DEFAULT_MAX_ELEMENTS;
    /// <summary>Default of <see cref="MaxInflatedSize"/>.</summary>
    public const long DefaultMaxInflatedSize = C.SIAARA_GROUPING_DEFAULT_MAX_INFLATED_SIZE;
    /// <summary>Default of <see cref="MaxValueBytes"/>.</summary>
    public const long DefaultMaxValueBytes = C.SIAARA_GROUPING_DEFAULT_MAX_VALUE_BYTES;
    /// <summary>Default of <see cref="OrientationTolerance"/>.</summary>
    public const double DefaultOrientationTolerance = 1e-4;
    /// <summary>Default of <see cref="MaxDicomdirs"/>.</summary>
    public const int DefaultMaxDicomdirs = (int)C.SIAARA_GROUPING_DEFAULT_MAX_DICOMDIRS;
    /// <summary>The most <see cref="MaxDicomdirs"/> may be.</summary>
    public const int MaxDicomdirsLimit = (int)C.SIAARA_GROUPING_MAX_DICOMDIRS_LIMIT;

    /// <summary>The recommended values (the defaults).</summary>
    public static DicomGroupingOptions Recommended { get; } = new();

    /// <summary>Throws <see cref="ArgumentOutOfRangeException"/> when a limit is zero or negative, or the tolerance
    /// is not a positive finite number; <see cref="ArgumentException"/> for an unknown enum value.</summary>
    public void Validate()
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxFiles);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxFileBytes);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxTotalBytes);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxTotalInflated);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxResultBytes);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxUidChars);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxTextBytes);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxDepth);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxElements);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxInflatedSize);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxValueBytes);
        if (!double.IsFinite(OrientationTolerance) || OrientationTolerance <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(OrientationTolerance), OrientationTolerance,
                "The tolerance must be a positive finite number.");
        }
        if (!Enum.IsDefined(Order))
        {
            throw new ArgumentException("Unknown order.", nameof(Order));
        }
        if (!Enum.IsDefined(Duplicates))
        {
            throw new ArgumentException("Unknown duplicate policy.", nameof(Duplicates));
        }
        if (!Enum.IsDefined(PositionFallback) || !Enum.IsDefined(TieBreak) || !Enum.IsDefined(UidValidation) ||
            !Enum.IsDefined(SeriesOrder))
        {
            throw new ArgumentException("Unknown position fallback, tie-break, UID validation or series order.");
        }
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(MaxDicomdirs);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(MaxDicomdirs, MaxDicomdirsLimit);
    }

    internal siaara_grouping_options ToNative()
    {
        Validate();
        return new siaara_grouping_options
        {
            struct_size = StructSize.Of<siaara_grouping_options>(),
            max_files = (uint)MaxFiles,
            max_file_bytes = (ulong)MaxFileBytes,
            max_total_bytes = (ulong)MaxTotalBytes,
            max_total_inflated = (ulong)MaxTotalInflated,
            max_result_bytes = (ulong)MaxResultBytes,
            max_uid_chars = (uint)MaxUidChars,
            max_text_bytes = (uint)MaxTextBytes,
            max_depth = (uint)MaxDepth,
            max_elements = (ulong)MaxElements,
            max_inflated_size = (ulong)MaxInflatedSize,
            max_value_bytes = (ulong)MaxValueBytes,
            order = (int)Order,
            include_patient_fields = IncludePatientFields ? 1 : 0,
            duplicates = (int)Duplicates,
            match_case_insensitive = MatchCaseInsensitive ? C.SIAARA_GROUP_SWITCH_ON : C.SIAARA_GROUP_SWITCH_OFF,
            match_by_trailing_components = MatchByTrailingComponents ? C.SIAARA_GROUP_SWITCH_ON : C.SIAARA_GROUP_SWITCH_OFF,
            orientation_tolerance = OrientationTolerance,
            position_fallback = (int)PositionFallback,
            tie_break = (int)TieBreak,
            uid_validation = (int)UidValidation,
            max_dicomdirs = (uint)MaxDicomdirs,
            series_order = (int)SeriesOrder,
        };
    }
}

/// <summary>What a <see cref="DicomGroupedText"/> holds.</summary>
public enum DicomGroupedTextStatus
{
    /// <summary>Not in the file (or empty).</summary>
    Absent = C.SIAARA_GTEXT_ABSENT,
    /// <summary>Stored and Display are set.</summary>
    Ok = C.SIAARA_GTEXT_OK,
    /// <summary>Longer than its cap: not kept (never cut and passed on as if whole).</summary>
    TooLong = C.SIAARA_GTEXT_TOO_LONG,
    /// <summary>A UID field whose value is not a valid UID (1 to 64 digits and dots): kept, not used for grouping.</summary>
    NotAUid = C.SIAARA_GTEXT_NOT_A_UID,
    /// <summary>Patient's Name / Patient ID without <see cref="DicomGroupingOptions.IncludePatientFields"/>.</summary>
    NotRequested = C.SIAARA_GTEXT_NOT_REQUESTED,
    /// <summary>A file name not kept because <see cref="DicomGroupingOptions.MaxResultBytes"/> was reached.</summary>
    NotKept = C.SIAARA_GTEXT_NOT_KEPT,
}

/// <summary>A text value: exactly as stored (a file's value decoded from its character set, padding removed; a file
/// name exactly as you gave it) and, next to it, a display-safe copy (controls and line separators a space, format
/// characters such as bidirectional overrides removed). Both are text from outside: escape them for HTML
/// (<see cref="DicomAttachmentPreview.HtmlEscape"/>).</summary>
/// <param name="Stored">The value as stored, or null unless Status is Ok or NotAUid.</param>
/// <param name="Display">The display-safe copy, or null with Stored.</param>
/// <param name="Status">What the value is.</param>
public sealed record DicomGroupedText(string? Stored, string? Display, DicomGroupedTextStatus Status)
{
    internal static readonly DicomGroupedText Absent = new(null, null, DicomGroupedTextStatus.Absent);
}

/// <summary>What placed an instance in its series.</summary>
public enum DicomGroupedSortKey
{
    /// <summary>Its position along the series' slice normal.</summary>
    Position = C.SIAARA_GSORT_POSITION,
    /// <summary>Slice Location (no usable position).</summary>
    SliceLocation = C.SIAARA_GSORT_SLICE_LOCATION,
    /// <summary>Instance Number.</summary>
    InstanceNumber = C.SIAARA_GSORT_INSTANCE_NUMBER,
    /// <summary>The order it was added in.</summary>
    Added = C.SIAARA_GSORT_ADDED,
}

/// <summary>One file that was kept, with the header fields the grouping looks at.</summary>
public sealed record DicomGroupedInstance
{
    /// <summary>Index in <see cref="DicomGroupingResult.Instances"/>.</summary>
    public int Index { get; init; }
    /// <summary>Which <see cref="DicomStudyGrouping.Add"/> call it came from (0-based).</summary>
    public int File { get; init; }
    /// <summary>Your name for the file, exactly as given, and its display-safe copy.</summary>
    public required DicomGroupedText Name { get; init; }
    /// <summary>SOP Class UID.</summary>
    public required DicomGroupedText SopClassUid { get; init; }
    /// <summary>SOP Instance UID.</summary>
    public required DicomGroupedText SopInstanceUid { get; init; }
    /// <summary>Study Instance UID.</summary>
    public required DicomGroupedText StudyUid { get; init; }
    /// <summary>Series Instance UID.</summary>
    public required DicomGroupedText SeriesUid { get; init; }
    /// <summary>Modality.</summary>
    public required DicomGroupedText Modality { get; init; }
    /// <summary>Study Date.</summary>
    public required DicomGroupedText StudyDate { get; init; }
    /// <summary>Series Description.</summary>
    public required DicomGroupedText SeriesDescription { get; init; }
    /// <summary>Patient's Name (NotRequested unless <see cref="DicomGroupingOptions.IncludePatientFields"/>).</summary>
    public required DicomGroupedText PatientName { get; init; }
    /// <summary>Patient ID (NotRequested unless <see cref="DicomGroupingOptions.IncludePatientFields"/>).</summary>
    public required DicomGroupedText PatientId { get; init; }
    /// <summary>Issuer of Patient ID (NotRequested unless <see cref="DicomGroupingOptions.IncludePatientFields"/>).</summary>
    public required DicomGroupedText IssuerOfPatientId { get; init; }
    /// <summary>Index of its series in <see cref="DicomGroupingResult.Series"/>, or -1 when ungrouped.</summary>
    public int Series { get; init; } = -1;
    /// <summary>What placed it in its series.</summary>
    public DicomGroupedSortKey SortKey { get; init; }
    /// <summary>The read-loss bits of its header read (0 = complete; see <c>SIAARA_READ_*</c>).</summary>
    public int ReadLoss { get; init; }
    /// <summary>Series Number, or null.</summary>
    public long? SeriesNumber { get; init; }
    /// <summary>Instance Number, or null.</summary>
    public long? InstanceNumber { get; init; }
    /// <summary>Number of Frames, or null.</summary>
    public long? NumberOfFrames { get; init; }
    /// <summary>Rows, or null.</summary>
    public int? Rows { get; init; }
    /// <summary>Columns, or null.</summary>
    public int? Columns { get; init; }
    /// <summary>Image Position (Patient), 3 numbers as stored (they may be NaN or infinite), or null.</summary>
    public IReadOnlyList<double>? ImagePosition { get; init; }
    /// <summary>Image Orientation (Patient), 6 numbers as stored, or null.</summary>
    public IReadOnlyList<double>? ImageOrientation { get; init; }
    /// <summary>Slice Location, or null.</summary>
    public double? SliceLocation { get; init; }
    /// <summary>Position along the series' slice normal when it is usable (it then placed the instance), else null.</summary>
    public double? Position { get; init; }
    /// <summary>An earlier file has its SOP Instance UID (<see cref="DuplicateOf"/>).</summary>
    public bool IsDuplicate { get; init; }
    /// <summary>Index of the first instance with its SOP Instance UID, or -1.</summary>
    public int DuplicateOf { get; init; } = -1;
    /// <summary>No valid Study or Series UID.</summary>
    public bool IsUngrouped { get; init; }
    /// <summary>Its name matched a DICOMDIR File ID.</summary>
    public bool InDicomdir { get; init; }
    /// <summary>Its name has a ".." component, is absolute or has a drive letter: never matched (an issue says so).</summary>
    public bool HasUnsafeName { get; init; }
    /// <summary>Acquisition Time, else Content Time, in seconds since midnight, or null.</summary>
    public double? AcquisitionTime { get; init; }
}

/// <summary>A series: its instances in order.</summary>
/// <param name="Index">Index in <see cref="DicomGroupingResult.Series"/>.</param>
/// <param name="Study">Index of its study.</param>
/// <param name="SeriesUid">Series Instance UID (of its first file).</param>
/// <param name="Modality">Modality (of its first file).</param>
/// <param name="SeriesDescription">Series Description (of its first file).</param>
/// <param name="Normal">The unit slice normal of its reference orientation, or null.</param>
/// <param name="Instances">Its instances, in order.</param>
public sealed record DicomGroupedSeries(int Index, int Study, DicomGroupedText SeriesUid, DicomGroupedText Modality,
    DicomGroupedText SeriesDescription, IReadOnlyList<double>? Normal, IReadOnlyList<DicomGroupedInstance> Instances)
{
    /// <summary>The rule the series was ordered by.</summary>
    public DicomSeriesOrderRule OrderRule { get; init; }
}

/// <summary>A study: its series.</summary>
/// <param name="Index">Index in <see cref="DicomGroupingResult.Studies"/>.</param>
/// <param name="Patient">Index of its patient.</param>
/// <param name="StudyUid">Study Instance UID.</param>
/// <param name="StudyDate">Study Date (of its first file).</param>
/// <param name="Series">Its series.</param>
public sealed record DicomGroupedStudy(int Index, int Patient, DicomGroupedText StudyUid, DicomGroupedText StudyDate,
    IReadOnlyList<DicomGroupedSeries> Series);

/// <summary>A patient: the files with the same Patient ID and Issuer of Patient ID (both as stored, compared byte
/// for byte), or the files of one study without a Patient ID (<see cref="NoPatientId"/>; never merged across
/// studies). Without <see cref="DicomGroupingOptions.IncludePatientFields"/> a patient is an index only.</summary>
/// <param name="Index">Index in <see cref="DicomGroupingResult.Patients"/>.</param>
/// <param name="PatientId">Patient ID (NotRequested unless asked for).</param>
/// <param name="PatientName">Patient's Name (NotRequested unless asked for).</param>
/// <param name="Studies">Its studies.</param>
public sealed record DicomGroupedPatient(int Index, DicomGroupedText PatientId, DicomGroupedText PatientName,
    IReadOnlyList<DicomGroupedStudy> Studies)
{
    /// <summary>The files of one study that have no Patient ID.</summary>
    public bool NoPatientId { get; init; }
    /// <summary>Issuer of Patient ID (NotRequested unless asked for).</summary>
    public DicomGroupedText IssuerOfPatientId { get; init; } = DicomGroupedText.Absent;
}

/// <summary>What a <see cref="DicomGroupingIssue"/> reports.</summary>
public enum DicomGroupingIssueKind
{
    /// <summary>A file not kept (<see cref="DicomGroupingIssue.SkipReason"/>, <see cref="DicomGroupingIssue.Limit"/>).</summary>
    Skipped = C.SIAARA_GISSUE_SKIPPED,
    /// <summary>An instance without a valid Study or Series UID (<see cref="DicomGroupingIssue.MissingUid"/>).</summary>
    Ungrouped = C.SIAARA_GISSUE_UNGROUPED,
    /// <summary>An instance (or, with KeepFirst, a file not kept) whose SOP Instance UID OtherInstance has.</summary>
    Duplicate = C.SIAARA_GISSUE_DUPLICATE,
    /// <summary>A study whose Study UID another patient's study has too (one issue per study).</summary>
    StudyPatientConflict = C.SIAARA_GISSUE_STUDY_PATIENT_CONFLICT,
    /// <summary>A series whose instance's Modality differs from that of its first file (OtherInstance).</summary>
    SeriesModalityConflict = C.SIAARA_GISSUE_SERIES_MODALITY_CONFLICT,
    /// <summary>A series whose instance's Rows x Columns differ from those of its first file (OtherInstance).</summary>
    SeriesSizeConflict = C.SIAARA_GISSUE_SERIES_SIZE_CONFLICT,
    /// <summary>A DICOMDIR that was not read.</summary>
    DicomdirSkipped = C.SIAARA_GISSUE_DICOMDIR_SKIPPED,
    /// <summary>A DICOMDIR File ID no name matches (Subject; Status Rejected when it is not well formed).</summary>
    ListedMissing = C.SIAARA_GISSUE_LISTED_MISSING,
    /// <summary>An instance whose name matches no DICOMDIR File ID.</summary>
    PresentUnlisted = C.SIAARA_GISSUE_PRESENT_UNLISTED,
    /// <summary>A File ID that two or more names match: one issue per name.</summary>
    AmbiguousMatch = C.SIAARA_GISSUE_AMBIGUOUS_MATCH,
    /// <summary>A name with a ".." component, an absolute name or one with a drive letter: never matched to a File ID.</summary>
    UnsafeName = C.SIAARA_GISSUE_UNSAFE_NAME,
    /// <summary>A DICOMDIR read incompletely or with warnings (<see cref="DicomGroupingIssue.DamagedReason"/>; Subject: the
    /// first warning). Its records are still used.</summary>
    DicomdirDamaged = C.SIAARA_GISSUE_DICOMDIR_DAMAGED,
}

/// <summary>Why a file (or DICOMDIR) was not kept.</summary>
public enum DicomGroupingSkipReason
{
    /// <summary>Not skipped.</summary>
    None = 0,
    /// <summary>Not a DICOM Part 10 file.</summary>
    NotDicom = C.SIAARA_GSKIP_NOT_DICOM,
    /// <summary>DICOM that cannot be read (<see cref="DicomGroupingIssue.Status"/>).</summary>
    Unreadable = C.SIAARA_GSKIP_UNREADABLE,
    /// <summary>Over a limit of one file (<see cref="DicomGroupingIssue.Limit"/>).</summary>
    FileLimit = C.SIAARA_GSKIP_FILE_LIMIT,
    /// <summary>Over a budget of the grouping (<see cref="DicomGroupingIssue.Limit"/>).</summary>
    TotalBudget = C.SIAARA_GSKIP_TOTAL_BUDGET,
    /// <summary>A DICOMDIR whose Media Storage SOP Class is not 1.2.840.10008.1.3.10.</summary>
    NotADicomdir = C.SIAARA_GSKIP_NOT_A_DICOMDIR,
}

/// <summary>Why a DICOMDIR is reported damaged.</summary>
public enum DicomdirDamagedReason
{
    /// <summary>Not damaged.</summary>
    None = 0,
    /// <summary>It was not read completely (truncated, bytes skipped).</summary>
    Incomplete = C.SIAARA_GDAMAGED_INCOMPLETE,
    /// <summary>It was read with warnings (cycles, links to nowhere, values too long, records dropped ...).</summary>
    Warnings = C.SIAARA_GDAMAGED_WARNINGS,
}

/// <summary>The limit or budget a skip was for.</summary>
public enum DicomGroupingLimit
{
    /// <summary>None.</summary>
    None = C.SIAARA_GLIMIT_NONE,
    /// <summary><see cref="DicomGroupingOptions.MaxFiles"/>.</summary>
    MaxFiles = C.SIAARA_GLIMIT_MAX_FILES,
    /// <summary><see cref="DicomGroupingOptions.MaxFileBytes"/>.</summary>
    FileBytes = C.SIAARA_GLIMIT_FILE_BYTES,
    /// <summary><see cref="DicomGroupingOptions.MaxTotalBytes"/>.</summary>
    TotalBytes = C.SIAARA_GLIMIT_TOTAL_BYTES,
    /// <summary><see cref="DicomGroupingOptions.MaxTotalInflated"/>.</summary>
    TotalInflated = C.SIAARA_GLIMIT_TOTAL_INFLATED,
    /// <summary><see cref="DicomGroupingOptions.MaxResultBytes"/>.</summary>
    ResultBytes = C.SIAARA_GLIMIT_RESULT_BYTES,
    /// <summary><see cref="DicomGroupingOptions.MaxDepth"/> or <see cref="DicomGroupingOptions.MaxElements"/>.</summary>
    ReadLimits = C.SIAARA_GLIMIT_READ_LIMITS,
    /// <summary><see cref="DicomGroupingOptions.MaxInflatedSize"/>.</summary>
    InflatedSize = C.SIAARA_GLIMIT_INFLATED_SIZE,
    /// <summary>A DICOMDIR's record limit.</summary>
    DicomdirRecords = C.SIAARA_GLIMIT_DICOMDIR_RECORDS,
    /// <summary><see cref="DicomGroupingOptions.MaxDicomdirs"/>.</summary>
    MaxDicomdirs = C.SIAARA_GLIMIT_MAX_DICOMDIRS,
}

/// <summary>Which UID an ungrouped instance lacks (missing or not a valid UID).</summary>
public enum DicomGroupingMissingUid
{
    /// <summary>Not an ungrouped issue.</summary>
    None = 0,
    /// <summary>Study Instance UID.</summary>
    StudyUid = C.SIAARA_GUNGROUPED_STUDY_UID,
    /// <summary>Series Instance UID.</summary>
    SeriesUid = C.SIAARA_GUNGROUPED_SERIES_UID,
    /// <summary>Both.</summary>
    Both = C.SIAARA_GUNGROUPED_BOTH,
}

/// <summary>Something the grouping reports: a file not kept, an ungrouped instance, a duplicate, an inconsistency,
/// or a difference between a DICOMDIR and what arrived. Indexes are -1 when they do not apply.</summary>
public sealed record DicomGroupingIssue
{
    /// <summary>What it reports.</summary>
    public DicomGroupingIssueKind Kind { get; init; }
    /// <summary>Skipped / DicomdirSkipped: why.</summary>
    public DicomGroupingSkipReason SkipReason { get; init; }
    /// <summary>Ungrouped: which UID.</summary>
    public DicomGroupingMissingUid MissingUid { get; init; }
    /// <summary>DicomdirDamaged: why.</summary>
    public DicomdirDamagedReason DamagedReason { get; init; }
    /// <summary>The limit or budget, for a skip over one.</summary>
    public DicomGroupingLimit Limit { get; init; }
    /// <summary>The read's status (skips) or the File ID's status (ListedMissing).</summary>
    public SiaaraErrorCode Status { get; init; }
    /// <summary>The <see cref="DicomStudyGrouping.Add"/> call.</summary>
    public int File { get; init; } = -1;
    /// <summary>The file's name (stored exactly as given, and display-safe), or Absent.</summary>
    public required DicomGroupedText Name { get; init; }
    /// <summary>The UID or File ID it is about, or Absent.</summary>
    public required DicomGroupedText Subject { get; init; }
    /// <summary>Index in <see cref="DicomGroupingResult.Instances"/>.</summary>
    public int Instance { get; init; } = -1;
    /// <summary>The other instance (the first of a duplicate, the reference of a conflict).</summary>
    public int OtherInstance { get; init; } = -1;
    /// <summary>Index in <see cref="DicomGroupingResult.Studies"/>.</summary>
    public int Study { get; init; } = -1;
    /// <summary>Index in <see cref="DicomGroupingResult.Series"/>.</summary>
    public int Series { get; init; } = -1;
    /// <summary>The <see cref="DicomStudyGrouping.AddDicomdir"/> call.</summary>
    public int Dicomdir { get; init; } = -1;
    /// <summary>The DICOMDIR record's index (of the first listing).</summary>
    public int Record { get; init; } = -1;
    /// <summary>ListedMissing, AmbiguousMatch: every <see cref="DicomStudyGrouping.AddDicomdir"/> call whose DICOMDIR
    /// lists the File ID (a File ID listed more than once is reported once).</summary>
    public IReadOnlyList<int> ListedBy { get; init; } = Array.Empty<int>();
}

/// <summary>The result of a grouping: patients / studies / series / instances, and the issues.</summary>
public sealed record DicomGroupingResult
{
    /// <summary>The patients, in the order their first file was added.</summary>
    public required IReadOnlyList<DicomGroupedPatient> Patients { get; init; }
    /// <summary>Every study, patient by patient.</summary>
    public required IReadOnlyList<DicomGroupedStudy> Studies { get; init; }
    /// <summary>Every series, study by study.</summary>
    public required IReadOnlyList<DicomGroupedSeries> Series { get; init; }
    /// <summary>Every instance: series by series, each in its order, then the ungrouped ones as added.</summary>
    public required IReadOnlyList<DicomGroupedInstance> Instances { get; init; }
    /// <summary>The ungrouped instances (no valid Study or Series UID), as added.</summary>
    public required IReadOnlyList<DicomGroupedInstance> Ungrouped { get; init; }
    /// <summary>Everything reported.</summary>
    public required IReadOnlyList<DicomGroupingIssue> Issues { get; init; }
    /// <summary>Files added.</summary>
    public long Files { get; init; }
    /// <summary>Files whose bytes were looked at.</summary>
    public long FilesRead { get; init; }
    /// <summary>The first total budget reached, or None.</summary>
    public DicomGroupingLimit BudgetReached { get; init; }
    /// <summary>Sizes of the files read, together.</summary>
    public long TotalBytes { get; init; }
    /// <summary>What deflated files inflated to, together.</summary>
    public long TotalInflated { get; init; }
    /// <summary>Text kept, in bytes.</summary>
    public long ResultBytes { get; init; }
    /// <summary>Names reported unsafe.</summary>
    public long UnsafeNames { get; init; }
}

/// <summary>
/// Groups files that arrived together (the members of a ZIP from a mail, with or without a DICOMDIR) into patient /
/// study / series, orders each series for scrolling, and sets what a DICOMDIR lists against what arrived. Everything
/// is from bytes in memory: nothing is written to disk and no file's bytes are kept.
/// <para>The input is UNTRUSTED: each file is read header-only with the attachment preview's limits, and the grouping
/// as a whole is held to the budgets of <see cref="DicomGroupingOptions"/>. <see cref="Add"/> never throws for bad
/// bytes: a file that cannot be read or is past a limit is recorded as skipped (an issue says why); ungrouped is only
/// for readable DICOM without a valid Study or Series UID. A name with a ".." component, an absolute name or one with a
/// drive letter is reported and never matched to a DICOMDIR File ID.</para>
/// <para>The grouping shows what the files CLAIM; it is not proof that they belong together or to the patient they
/// name. Reading headers has no time bound: run the grouping in the same separate worker process as the attachment
/// preview (<see cref="DicomAttachmentPreview"/>), with a kill timer and a memory limit.</para>
/// <para>Not thread-safe: use one instance from one thread at a time.</para>
/// </summary>
public sealed class DicomStudyGrouping : IDisposable
{
    private readonly GroupingHandle _handle;
    private readonly List<string> _names = new();
    private DicomGroupingResult? _result;

    /// <summary>Creates a grouping.</summary>
    /// <param name="options">Options, or null for <see cref="DicomGroupingOptions.Recommended"/>.</param>
    /// <exception cref="ArgumentOutOfRangeException">A limit is zero or negative.</exception>
    public DicomStudyGrouping(DicomGroupingOptions? options = null)
    {
        var o = (options ?? DicomGroupingOptions.Recommended).ToNative();
        Status.Check(Native.siaara_grouping_create(in o, out nint p));
        _handle = new GroupingHandle();
        _handle.Attach(p);
    }

    /// <summary>Adds one file. Never throws for bad bytes: it is recorded as an instance or as skipped.</summary>
    /// <param name="name">Your name for the file (for example the ZIP member's path); kept exactly as given and used
    /// to match DICOMDIR File IDs.</param>
    /// <param name="data">Its bytes, only read during the call (never kept).</param>
    public void Add(string name, ReadOnlySpan<byte> data)
    {
        ArgumentNullException.ThrowIfNull(name);
        ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
        byte[] n = Encoding.UTF8.GetBytes(name);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_grouping_add_file(l.Pointer, n, data));
        _names.Add(name);
        _result = null;
    }

    /// <summary>Adds a DICOMDIR (optional; more than one may be added). Never throws for bad bytes: one that cannot be
    /// read is an issue (<see cref="DicomGroupingIssueKind.DicomdirSkipped"/>).</summary>
    /// <param name="data">Its bytes, only read during the call.</param>
    public void AddDicomdir(ReadOnlySpan<byte> data)
    {
        ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_grouping_add_dicomdir(l.Pointer, data));
        _result = null;
    }

    /// <summary>The result of everything added so far (built when first read after an Add).</summary>
    public DicomGroupingResult Result
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _result ??= Build();
        }
    }

    /// <summary>Frees the native grouping.</summary>
    public void Dispose() => _handle.Dispose();

    private DicomGroupingResult Build()
    {
        using var l = new HandleLease(_handle);
        nint g = l.Pointer;
        var sum = new siaara_grouping_summary { struct_size = StructSize.Of<siaara_grouping_summary>() };
        Status.Check(Native.siaara_grouping_result(g, ref sum));

        var instances = new DicomGroupedInstance[checked((int)sum.instances)];
        var ungrouped = new List<DicomGroupedInstance>();
        for (int i = 0; i < instances.Length; i++)
        {
            var n = new siaara_grouped_instance { struct_size = StructSize.Of<siaara_grouped_instance>() };
            Status.Check(Native.siaara_grouping_instance(g, (uint)i, ref n));
            uint f = n.flags;
            bool Has(int bit) => (f & (uint)bit) != 0;
            DicomGroupedText T(int field) => Text(g, C.SIAARA_GLEVEL_INSTANCE, i, field);
            var inst = new DicomGroupedInstance
            {
                Index = i,
                File = (int)n.file,
                Name = NameText(T(C.SIAARA_GFIELD_NAME), (int)n.file),
                SopClassUid = T(C.SIAARA_GFIELD_SOP_CLASS_UID),
                SopInstanceUid = T(C.SIAARA_GFIELD_SOP_INSTANCE_UID),
                StudyUid = T(C.SIAARA_GFIELD_STUDY_UID),
                SeriesUid = T(C.SIAARA_GFIELD_SERIES_UID),
                Modality = T(C.SIAARA_GFIELD_MODALITY),
                StudyDate = T(C.SIAARA_GFIELD_STUDY_DATE),
                SeriesDescription = T(C.SIAARA_GFIELD_SERIES_DESCRIPTION),
                PatientName = T(C.SIAARA_GFIELD_PATIENT_NAME),
                PatientId = T(C.SIAARA_GFIELD_PATIENT_ID),
                IssuerOfPatientId = T(C.SIAARA_GFIELD_ISSUER_OF_PATIENT_ID),
                Series = n.series,
                SortKey = (DicomGroupedSortKey)n.sort_key,
                ReadLoss = (int)n.read_loss,
                SeriesNumber = Has(C.SIAARA_GINST_SERIES_NUMBER) ? n.series_number : null,
                InstanceNumber = Has(C.SIAARA_GINST_INSTANCE_NUMBER) ? n.instance_number : null,
                NumberOfFrames = Has(C.SIAARA_GINST_FRAMES) ? n.number_of_frames : null,
                Rows = Has(C.SIAARA_GINST_ROWS) ? (int)Math.Min(n.rows, int.MaxValue) : null,
                Columns = Has(C.SIAARA_GINST_COLUMNS) ? (int)Math.Min(n.columns, int.MaxValue) : null,
                ImagePosition = Has(C.SIAARA_GINST_POSITION) ? ToArray(n.image_position) : null,
                ImageOrientation = Has(C.SIAARA_GINST_ORIENTATION) ? ToArray(n.image_orientation) : null,
                SliceLocation = Has(C.SIAARA_GINST_SLICE_LOCATION) ? n.slice_location : null,
                Position = Has(C.SIAARA_GINST_POSITION_USABLE) ? n.position : null,
                IsDuplicate = Has(C.SIAARA_GINST_DUPLICATE),
                DuplicateOf = n.duplicate_of,
                IsUngrouped = Has(C.SIAARA_GINST_UNGROUPED),
                InDicomdir = Has(C.SIAARA_GINST_IN_DICOMDIR),
                HasUnsafeName = Has(C.SIAARA_GINST_UNSAFE_NAME),
                AcquisitionTime = Has(C.SIAARA_GINST_ACQUISITION_TIME) ? n.acquisition_time : null,
            };
            instances[i] = inst;
            if (inst.IsUngrouped)
            {
                ungrouped.Add(inst);
            }
        }

        var series = new DicomGroupedSeries[checked((int)sum.series)];
        for (int k = 0; k < series.Length; k++)
        {
            var e = new siaara_grouped_series { struct_size = StructSize.Of<siaara_grouped_series>() };
            Status.Check(Native.siaara_grouping_series(g, (uint)k, ref e));
            series[k] = new DicomGroupedSeries(k, (int)e.study,
                Text(g, C.SIAARA_GLEVEL_SERIES, k, C.SIAARA_GFIELD_SERIES_UID),
                Text(g, C.SIAARA_GLEVEL_SERIES, k, C.SIAARA_GFIELD_MODALITY),
                Text(g, C.SIAARA_GLEVEL_SERIES, k, C.SIAARA_GFIELD_SERIES_DESCRIPTION),
                e.has_normal != 0 ? ToArray(e.normal) : null,
                new ArraySegment<DicomGroupedInstance>(instances, (int)e.first_instance, (int)e.instance_count))
            {
                OrderRule = (DicomSeriesOrderRule)e.order_rule,
            };
        }
        var studies = new DicomGroupedStudy[checked((int)sum.studies)];
        for (int s = 0; s < studies.Length; s++)
        {
            var st = new siaara_grouped_study { struct_size = StructSize.Of<siaara_grouped_study>() };
            Status.Check(Native.siaara_grouping_study(g, (uint)s, ref st));
            studies[s] = new DicomGroupedStudy(s, (int)st.patient,
                Text(g, C.SIAARA_GLEVEL_STUDY, s, C.SIAARA_GFIELD_STUDY_UID),
                Text(g, C.SIAARA_GLEVEL_STUDY, s, C.SIAARA_GFIELD_STUDY_DATE),
                new ArraySegment<DicomGroupedSeries>(series, (int)st.first_series, (int)st.series_count));
        }
        var patients = new DicomGroupedPatient[checked((int)sum.patients)];
        for (int p = 0; p < patients.Length; p++)
        {
            var pt = new siaara_grouped_patient { struct_size = StructSize.Of<siaara_grouped_patient>() };
            Status.Check(Native.siaara_grouping_patient(g, (uint)p, ref pt));
            patients[p] = new DicomGroupedPatient(p,
                Text(g, C.SIAARA_GLEVEL_PATIENT, p, C.SIAARA_GFIELD_PATIENT_ID),
                Text(g, C.SIAARA_GLEVEL_PATIENT, p, C.SIAARA_GFIELD_PATIENT_NAME),
                new ArraySegment<DicomGroupedStudy>(studies, (int)pt.first_study, (int)pt.study_count))
            {
                NoPatientId = (pt.flags & (uint)C.SIAARA_GPATIENT_NO_PATIENT_ID) != 0,
                IssuerOfPatientId = Text(g, C.SIAARA_GLEVEL_PATIENT, p, C.SIAARA_GFIELD_ISSUER_OF_PATIENT_ID),
            };
        }
        var issues = new DicomGroupingIssue[checked((int)sum.issues)];
        for (int i = 0; i < issues.Length; i++)
        {
            var v = new siaara_grouping_issue { struct_size = StructSize.Of<siaara_grouping_issue>() };
            Status.Check(Native.siaara_grouping_issue_get(g, (uint)i, ref v));
            var kind = (DicomGroupingIssueKind)v.kind;
            bool skip = kind is DicomGroupingIssueKind.Skipped or DicomGroupingIssueKind.DicomdirSkipped;
            issues[i] = new DicomGroupingIssue
            {
                Kind = kind,
                SkipReason = skip ? (DicomGroupingSkipReason)v.reason : DicomGroupingSkipReason.None,
                MissingUid = kind == DicomGroupingIssueKind.Ungrouped ? (DicomGroupingMissingUid)v.reason : DicomGroupingMissingUid.None,
                DamagedReason = kind == DicomGroupingIssueKind.DicomdirDamaged ? (DicomdirDamagedReason)v.reason : DicomdirDamagedReason.None,
                Limit = (DicomGroupingLimit)v.limit,
                Status = (SiaaraErrorCode)v.status,
                File = v.file,
                Name = NameText(Text(g, C.SIAARA_GLEVEL_ISSUE, i, C.SIAARA_GFIELD_NAME), v.file),
                Subject = Text(g, C.SIAARA_GLEVEL_ISSUE, i, C.SIAARA_GFIELD_SUBJECT),
                Instance = v.instance,
                OtherInstance = v.other_instance,
                Study = v.study,
                Series = v.series,
                Dicomdir = v.dicomdir,
                Record = v.record,
                ListedBy = Bits(v.dicomdir_mask),
            };
        }
        return new DicomGroupingResult
        {
            Patients = patients,
            Studies = studies,
            Series = series,
            Instances = instances,
            Ungrouped = ungrouped,
            Issues = issues,
            Files = (long)sum.files,
            FilesRead = (long)sum.files_read,
            BudgetReached = (DicomGroupingLimit)sum.budget_reached,
            TotalBytes = (long)sum.total_bytes,
            TotalInflated = (long)sum.total_inflated,
            ResultBytes = (long)sum.result_bytes,
            UnsafeNames = (long)sum.unsafe_names,
        };
    }

    // A file name is returned exactly as the caller gave it (the C# string itself, so even a lone surrogate, which
    // UTF-8 cannot carry, comes back unchanged); the display-safe copy is the library's.
    private DicomGroupedText NameText(DicomGroupedText fromNative, int file)
    {
        if (fromNative.Status != DicomGroupedTextStatus.Ok || file < 0 || file >= _names.Count)
        {
            return fromNative;
        }
        return fromNative with { Stored = _names[file] };
    }

    private static DicomGroupedText Text(nint g, int level, int index, int field)
    {
        var t = new siaara_grouping_text { struct_size = StructSize.Of<siaara_grouping_text>() };
        Status.Check(Native.siaara_grouping_text_get(g, level, (uint)index, field, ref t));
        var status = (DicomGroupedTextStatus)t.status;
        if (t.stored == 0)
        {
            return status == DicomGroupedTextStatus.Absent ? DicomGroupedText.Absent : new DicomGroupedText(null, null, status);
        }
        string stored = Encoding.UTF8.GetString(NativeSpan.Bytes(t.stored, t.stored_len));
        string display = Encoding.UTF8.GetString(NativeSpan.Bytes(t.display, t.display_len));
        return new DicomGroupedText(stored, display, status);
    }

    private static int[] Bits(uint mask)
    {
        var list = new List<int>();
        for (int i = 0; i < 32; i++)
        {
            if ((mask & (1u << i)) != 0)
            {
                list.Add(i);
            }
        }
        return list.ToArray();
    }

    private static double[] ToArray(FixedArray3OfDouble a)
    {
        ReadOnlySpan<double> s = a;
        return s.ToArray();
    }

    private static double[] ToArray(FixedArray6OfDouble a)
    {
        ReadOnlySpan<double> s = a;
        return s.ToArray();
    }
}
