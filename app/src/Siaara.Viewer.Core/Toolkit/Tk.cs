// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Thin facade over the SIAARA C# bindings. This file is the ONLY place in the app that names a
// binding type (Siaara.*); everything else uses the app-owned types declared here. Keeping the
// mapping in one file means a binding rename touches one file, and the rest of the app can be
// read without knowing the bindings.

using System.Globalization;
using Siaara.Clinical;
using Siaara.Viewer.Core.Geometry;
using Siaara.Network;
using Siaara.Volumes;

namespace Siaara.Viewer.Core.Toolkit;

/// <summary>Toolkit status codes (mirror of the C API's siaara_status).</summary>
public enum TkError
{
    /// <summary>Success.</summary>
    None = 0,
    /// <summary>An argument was invalid.</summary>
    InvalidArgument = 1,
    /// <summary>An I/O error occurred.</summary>
    IO = 2,
    /// <summary>The input is not DICOM.</summary>
    NotDicom = 3,
    /// <summary>The input ended early.</summary>
    Truncated = 4,
    /// <summary>The input is malformed.</summary>
    Malformed = 5,
    /// <summary>Not supported.</summary>
    Unsupported = 6,
    /// <summary>Not found.</summary>
    NotFound = 7,
    /// <summary>Type mismatch.</summary>
    TypeMismatch = 8,
    /// <summary>A limit was exceeded.</summary>
    LimitExceeded = 9,
    /// <summary>Out of memory.</summary>
    OutOfMemory = 10,
    /// <summary>Network error.</summary>
    Network = 11,
    /// <summary>Timeout.</summary>
    Timeout = 12,
    /// <summary>Rejected (for a store: in use by another process).</summary>
    Rejected = 13,
    /// <summary>Cancelled.</summary>
    Cancelled = 14,
    /// <summary>Buffer too small.</summary>
    BufferTooSmall = 15,
    /// <summary>Internal error.</summary>
    Internal = 99,
}

/// <summary>A toolkit call failed. The message is plain words from the status code only: it never
/// carries dataset values, so it is safe for the UI and the logs.</summary>
public sealed class TkException : Exception
{
    /// <summary>Creates an exception.</summary>
    public TkException() : this(TkError.Internal)
    {
    }

    /// <summary>Creates an exception.</summary>
    /// <param name="message">Message.</param>
    public TkException(string message) : base(message) => Error = TkError.Internal;

    /// <summary>Creates an exception.</summary>
    /// <param name="message">Message.</param>
    /// <param name="innerException">Cause.</param>
    public TkException(string message, Exception innerException) : base(message, innerException) => Error = TkError.Internal;

    /// <summary>Creates an exception for a status.</summary>
    /// <param name="error">The status.</param>
    /// <param name="innerException">Cause, or null.</param>
    public TkException(TkError error, Exception? innerException = null) : base(Tk.Describe(error), innerException) => Error = error;

    /// <summary>The status.</summary>
    public TkError Error { get; }
}

/// <summary>Severity of a toolkit diagnostic.</summary>
public enum TkLogLevel
{
    /// <summary>Debug.</summary>
    Debug = 0,
    /// <summary>Information.</summary>
    Information = 1,
    /// <summary>Warning.</summary>
    Warning = 2,
    /// <summary>Error.</summary>
    Error = 3,
}

/// <summary>A DICOM tag as used by the app (group &lt;&lt; 16 | element).</summary>
/// <param name="Value">The tag number.</param>
public readonly record struct TkTag(uint Value)
{
    /// <summary>Group.</summary>
    public ushort Group => (ushort)(Value >> 16);

    /// <summary>Element.</summary>
    public ushort Element => (ushort)Value;

    /// <inheritdoc/>
    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"({Group:X4},{Element:X4})");

    internal DicomTag ToBinding() => new(Value);
}

/// <summary>Library-level calls: version, ABI, diagnostics, encoders, folder scan, DICOMDIR.</summary>
public static class Tk
{
    /// <summary>ABI version the app was compiled against.</summary>
    public static uint ExpectedAbiVersion => SiaaraLibrary.ExpectedAbiVersion;

    /// <summary>ABI version of the loaded native library.</summary>
    public static uint AbiVersion => SiaaraLibrary.AbiVersion;

    /// <summary>True when the native library has the ABI this build expects.</summary>
    public static bool AbiMatches => AbiVersion == ExpectedAbiVersion;

    /// <summary>Toolkit version string.</summary>
    public static string Version => SiaaraLibrary.Version;

    /// <summary>Path of the loaded native library, if known.</summary>
    public static string? NativeLibraryPath => SiaaraLibrary.NativeLibraryPath;

    /// <summary>Live native handles (debug counter).</summary>
    public static long LiveHandles => SiaaraLibrary.LiveHandleCount;

    /// <summary>Installs the diagnostics handler (level, containsPhi, message); null removes it.</summary>
    /// <param name="handler">The handler; must not throw.</param>
    public static void SetLogHandler(Action<TkLogLevel, bool, string>? handler)
    {
        if (handler is null)
        {
            SiaaraLog.SetHandler(null);
            return;
        }
        SiaaraLog.SetHandler((level, phi, msg) => handler((TkLogLevel)(int)level, phi, msg), SiaaraLogLevel.Warning);
    }

    /// <summary>Plain words for a status code (no values from any file).</summary>
    /// <param name="error">The status.</param>
    /// <returns>The text.</returns>
    public static string Describe(TkError error) => error switch
    {
        TkError.None => "OK",
        TkError.InvalidArgument => "invalid argument",
        TkError.IO => "the file could not be read or written",
        TkError.NotDicom => "not a DICOM file",
        TkError.Truncated => "the file ends early (truncated)",
        TkError.Malformed => "the file is malformed",
        TkError.Unsupported => "not supported",
        TkError.NotFound => "not found",
        TkError.TypeMismatch => "value type mismatch",
        TkError.LimitExceeded => "a size limit was exceeded",
        TkError.OutOfMemory => "out of memory",
        TkError.Network => "network error",
        TkError.Timeout => "timed out",
        TkError.Rejected => "rejected (in use or not allowed)",
        TkError.Cancelled => "cancelled",
        TkError.BufferTooSmall => "buffer too small",
        _ => "internal toolkit error",
    };

    internal static TkException Wrap(SiaaraException e) => new((TkError)(int)e.ErrorCode, e);

    internal static T Call<T>(Func<T> f)
    {
        try
        {
            return f();
        }
        catch (SiaaraException e)
        {
            throw Wrap(e);
        }
    }

    internal static void Call(Action f)
    {
        try
        {
            f();
        }
        catch (SiaaraException e)
        {
            throw Wrap(e);
        }
    }

    /// <summary>Encodes 8-bit pixels as PNG with the toolkit's encoder.</summary>
    /// <param name="pixels">width x height x (4 or 1) bytes.</param>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    /// <param name="gray">True for 1 byte per pixel.</param>
    /// <returns>The PNG bytes.</returns>
    public static byte[] EncodePng(ReadOnlySpan<byte> pixels, int width, int height, bool gray)
    {
        try
        {
            return ImageEncoder.EncodePng(pixels, width, height, gray ? PixelOutputFormat.Gray8 : PixelOutputFormat.Rgba8, 3);
        }
        catch (SiaaraException e)
        {
            throw Wrap(e);
        }
    }

    /// <summary>A standard colour map as 256 RGBA8 entries (the toolkit's table).</summary>
    /// <param name="colormap">Colour map.</param>
    /// <returns>1024 bytes.</returns>
    public static byte[] ColormapRgba8(TkColormap colormap) => Call(() => DicomFrame.GetColormapRgba8((Colormap)(int)colormap));

    /// <summary>Scans a folder for DICOM files (recursive, raw data sets allowed).</summary>
    /// <param name="root">Folder.</param>
    /// <param name="onItem">Called once per file, never concurrently.</param>
    /// <param name="reportSkipped">Also report non-DICOM files.</param>
    /// <param name="cancellationToken">Stops the scan (OperationCanceledException).</param>
    /// <returns>Totals.</returns>
    public static TkScanTotals ScanFolder(string root, Action<TkScanItem> onItem, bool reportSkipped, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(onItem);
        var opts = new FolderScanOptions { Recursive = true, AllowRaw = true, ReportSkipped = reportSkipped, Threads = 2 };
        FolderScanStatistics s = Call(() => FolderScanner.Scan(root, item =>
        {
            var kind = item.Kind switch
            {
                FolderScanItemKind.Instance => TkScanKind.Instance,
                FolderScanItemKind.Dicomdir => TkScanKind.Dicomdir,
                FolderScanItemKind.NotDicom => TkScanKind.NotDicom,
                _ => TkScanKind.Error,
            };
            onItem(new TkScanItem(item.Path, kind, (TkError)(int)item.Status));
        }, opts, cancellationToken));
        return new TkScanTotals(s.Files, s.Instances, s.Skipped, s.Errors);
    }

    /// <summary>Reads a DICOMDIR (inactive records skipped, files resolved case-insensitively).</summary>
    /// <param name="path">The DICOMDIR file.</param>
    /// <param name="onRecord">Called per record.</param>
    /// <param name="cancellationToken">Stops reading.</param>
    /// <returns>Totals.</returns>
    public static TkDicomdirTotals ReadDicomdir(string path, Action<TkDicomdirRecord> onRecord, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(onRecord);
        var opts = new DicomdirReadOptions { ResolveFiles = true, CaseInsensitive = true };
        DicomdirSummary s = Call(() => Dicomdir.Read(path, r =>
        {
            string file = r.Keys.FilePath;
            string type = r.Keys.GetString(IndexField.RecordType);
            onRecord(new TkDicomdirRecord(type, file, (TkError)(int)r.FileStatus));
        }, opts, cancellationToken));
        return new TkDicomdirTotals(s.Records, s.Files, s.MissingFiles, s.RejectedPaths);
    }

    /// <summary>Copies files under root/DICOM and writes root/DICOMDIR (General Purpose CD-R profile).</summary>
    /// <param name="root">Output folder.</param>
    /// <param name="files">Files.</param>
    /// <returns>Number of warnings.</returns>
    public static int WriteDicomdir(string root, IReadOnlyList<string> files) =>
        Call(() => Dicomdir.Write(root, files, new DicomdirWriteOptions { Overwrite = true, AllowAnyTransferSyntax = true }));
}

/// <summary>Kind of a scanned file.</summary>
public enum TkScanKind
{
    /// <summary>A DICOM instance.</summary>
    Instance,
    /// <summary>A DICOMDIR.</summary>
    Dicomdir,
    /// <summary>Not DICOM.</summary>
    NotDicom,
    /// <summary>Could not be read.</summary>
    Error,
}

/// <summary>One scanned file.</summary>
/// <param name="Path">Path.</param>
/// <param name="Kind">Kind.</param>
/// <param name="Status">Read status.</param>
public readonly record struct TkScanItem(string Path, TkScanKind Kind, TkError Status);

/// <summary>Scan totals.</summary>
/// <param name="Files">Files seen.</param>
/// <param name="Instances">DICOM instances.</param>
/// <param name="Skipped">Non-DICOM files.</param>
/// <param name="Errors">Unreadable files.</param>
public readonly record struct TkScanTotals(long Files, long Instances, long Skipped, long Errors);

/// <summary>One DICOMDIR record.</summary>
/// <param name="RecordType">Directory Record Type (PATIENT, STUDY, SERIES, IMAGE...).</param>
/// <param name="FilePath">Resolved file, or "" for records without a file.</param>
/// <param name="FileStatus">NotFound when the referenced file is missing.</param>
public readonly record struct TkDicomdirRecord(string RecordType, string FilePath, TkError FileStatus);

/// <summary>DICOMDIR totals.</summary>
/// <param name="Records">Records.</param>
/// <param name="Files">Records referencing a file.</param>
/// <param name="MissingFiles">Referenced files not found.</param>
/// <param name="RejectedPaths">Paths rejected (outside the root etc.).</param>
public readonly record struct TkDicomdirTotals(int Records, int Files, int MissingFiles, int RejectedPaths);

// ====================================================================== store

/// <summary>Record level.</summary>
public enum TkLevel
{
    /// <summary>Patient.</summary>
    Patient = 0,
    /// <summary>Study.</summary>
    Study = 1,
    /// <summary>Series.</summary>
    Series = 2,
    /// <summary>Instance.</summary>
    Instance = 3,
}

/// <summary>Store filter (C-FIND style wildcards * and ?; dates "YYYYMMDD" or ranges).</summary>
public sealed record TkStoreFilter
{
    /// <summary>Level of the rows.</summary>
    public TkLevel Level { get; init; } = TkLevel.Study;
    /// <summary>Patient's Name.</summary>
    public string? PatientName { get; init; }
    /// <summary>Patient ID.</summary>
    public string? PatientId { get; init; }
    /// <summary>Accession Number.</summary>
    public string? AccessionNumber { get; init; }
    /// <summary>Study Date or range.</summary>
    public string? StudyDate { get; init; }
    /// <summary>Modality; several separated by backslashes.</summary>
    public string? Modality { get; init; }
    /// <summary>Study Instance UID.</summary>
    public string? StudyInstanceUid { get; init; }
    /// <summary>Series Instance UID.</summary>
    public string? SeriesInstanceUid { get; init; }
    /// <summary>Rows to skip.</summary>
    public int Offset { get; init; }
    /// <summary>Maximum rows (0 = all).</summary>
    public int Limit { get; init; }
    /// <summary>Stop when Offset + Limit rows are known; Total is then a lower bound.</summary>
    public bool NoTotal { get; init; }
}

/// <summary>One store or scan row (app copy of the toolkit's index record).</summary>
public sealed class TkRecord
{
    /// <summary>Level.</summary>
    public TkLevel Level { get; init; }
    /// <summary>Patient's Name (stored value, unchanged).</summary>
    public string PatientName { get; init; } = "";
    /// <summary>Patient ID.</summary>
    public string PatientId { get; init; } = "";
    /// <summary>Patient's Sex.</summary>
    public string PatientSex { get; init; } = "";
    /// <summary>Patient's Birth Date.</summary>
    public string PatientBirthDate { get; init; } = "";
    /// <summary>Store patient key (for deleting the patient), or "".</summary>
    public string PatientKey { get; init; } = "";
    /// <summary>Study Instance UID.</summary>
    public string StudyInstanceUid { get; init; } = "";
    /// <summary>Study Date.</summary>
    public string StudyDate { get; init; } = "";
    /// <summary>Study Time.</summary>
    public string StudyTime { get; init; } = "";
    /// <summary>Accession Number.</summary>
    public string AccessionNumber { get; init; } = "";
    /// <summary>Study Description.</summary>
    public string StudyDescription { get; init; } = "";
    /// <summary>Modality (series / instance rows).</summary>
    public string Modality { get; init; } = "";
    /// <summary>Modalities in Study (study rows).</summary>
    public string ModalitiesInStudy { get; init; } = "";
    /// <summary>Series Instance UID.</summary>
    public string SeriesInstanceUid { get; init; } = "";
    /// <summary>Series Description.</summary>
    public string SeriesDescription { get; init; } = "";
    /// <summary>Series Number, or null.</summary>
    public int? SeriesNumber { get; init; }
    /// <summary>SOP Instance UID.</summary>
    public string SopInstanceUid { get; init; } = "";
    /// <summary>SOP Class UID.</summary>
    public string SopClassUid { get; init; } = "";
    /// <summary>Instance Number, or null.</summary>
    public int? InstanceNumber { get; init; }
    /// <summary>Number of Frames.</summary>
    public int NumberOfFrames { get; init; }
    /// <summary>Rows.</summary>
    public int Rows { get; init; }
    /// <summary>Columns.</summary>
    public int Columns { get; init; }
    /// <summary>The instance has pixel data.</summary>
    public bool HasPixelData { get; init; }
    /// <summary>Number of series (study rows).</summary>
    public int NumberOfSeries { get; init; }
    /// <summary>Number of instances (study / series rows).</summary>
    public int NumberOfInstances { get; init; }
    /// <summary>Absolute file path (instance rows).</summary>
    public string FilePath { get; init; } = "";

    internal static TkRecord From(DicomIndexRecord r) => new()
    {
        Level = (TkLevel)(int)r.Level,
        PatientName = r.PatientName,
        PatientId = r.PatientId,
        PatientSex = r.GetString(IndexField.PatientSex),
        PatientBirthDate = r.GetString(IndexField.PatientBirthDate),
        PatientKey = r.PatientKey ?? "",
        StudyInstanceUid = r.StudyInstanceUid,
        StudyDate = r.GetString(IndexField.StudyDate),
        StudyTime = r.GetString(IndexField.StudyTime),
        AccessionNumber = r.GetString(IndexField.AccessionNumber),
        StudyDescription = r.GetString(IndexField.StudyDescription),
        Modality = r.Modality,
        ModalitiesInStudy = r.GetString(IndexField.ModalitiesInStudy),
        SeriesInstanceUid = r.SeriesInstanceUid,
        SeriesDescription = r.GetString(IndexField.SeriesDescription),
        SeriesNumber = r.Flags.HasFlag(IndexRecordContents.SeriesNumber) ? r.SeriesNumber : null,
        SopInstanceUid = r.SopInstanceUid,
        SopClassUid = r.GetString(IndexField.SopClassUid),
        InstanceNumber = r.Flags.HasFlag(IndexRecordContents.InstanceNumber) ? r.InstanceNumber : null,
        NumberOfFrames = r.NumberOfFrames,
        Rows = r.Rows,
        Columns = r.Columns,
        HasPixelData = r.Flags.HasFlag(IndexRecordContents.PixelData),
        NumberOfSeries = r.NumberOfSeries,
        NumberOfInstances = r.NumberOfInstances,
        FilePath = r.FilePath,
    };
}

/// <summary>Rows of a store query.</summary>
/// <param name="Rows">The rows.</param>
/// <param name="Total">Total matches (a lower bound when not exact).</param>
/// <param name="TotalExact">Total is exact.</param>
public sealed record TkQueryResult(IReadOnlyList<TkRecord> Rows, int Total, bool TotalExact);

/// <summary>Result of adding a file.</summary>
public enum TkAddOutcome
{
    /// <summary>New instance.</summary>
    Added = 0,
    /// <summary>Replaced.</summary>
    Replaced = 1,
    /// <summary>Duplicate: the stored copy was kept.</summary>
    KeptExisting = 2,
}

/// <summary>Kind of store change.</summary>
public enum TkStoreEventKind
{
    /// <summary>Added.</summary>
    Added = 0,
    /// <summary>Replaced.</summary>
    Replaced = 1,
    /// <summary>Deleted.</summary>
    Deleted = 2,
    /// <summary>Index rebuilt.</summary>
    Rebuilt = 3,
}

/// <summary>A committed store change.</summary>
public sealed class TkStoreEventArgs : EventArgs
{
    /// <summary>Creates the event.</summary>
    /// <param name="kind">Kind.</param>
    /// <param name="sop">SOP Instance UID.</param>
    /// <param name="series">Series Instance UID.</param>
    /// <param name="study">Study Instance UID.</param>
    public TkStoreEventArgs(TkStoreEventKind kind, string sop, string series, string study)
    {
        Kind = kind;
        SopInstanceUid = sop;
        SeriesInstanceUid = series;
        StudyInstanceUid = study;
    }

    /// <summary>Kind.</summary>
    public TkStoreEventKind Kind { get; }
    /// <summary>SOP Instance UID.</summary>
    public string SopInstanceUid { get; }
    /// <summary>Series Instance UID.</summary>
    public string SeriesInstanceUid { get; }
    /// <summary>Study Instance UID.</summary>
    public string StudyInstanceUid { get; }
}

/// <summary>Store totals.</summary>
/// <param name="Patients">Patients.</param>
/// <param name="Studies">Studies.</param>
/// <param name="Series">Series.</param>
/// <param name="Instances">Instances.</param>
/// <param name="DataBytes">Bytes of copied data.</param>
public sealed record TkStoreStats(long Patients, long Studies, long Series, long Instances, long DataBytes);

/// <summary>The toolkit's local DICOM store (siaara_store_*).</summary>
public sealed class TkStore : IDisposable
{
    private readonly DicomStore _store;

    private TkStore(DicomStore store)
    {
        _store = store;
        _store.Changed += OnChanged;
    }

    /// <summary>Opens or creates a store. TkError.Rejected when another process has it open.</summary>
    /// <param name="root">Store folder.</param>
    /// <param name="durable">fsync every change.</param>
    /// <param name="rebuildIfCorrupt">Re-index when the index is unreadable.</param>
    /// <returns>The store.</returns>
    public static TkStore Open(string root, bool durable = true, bool rebuildIfCorrupt = true) =>
        new(Tk.Call(() => DicomStore.Open(root, new DicomStoreOptions { Create = true, Durable = durable, RebuildIfCorrupt = rebuildIfCorrupt })));

    /// <summary>Raised after each committed change, on the thread that made the change.</summary>
    public event EventHandler<TkStoreEventArgs>? Changed;

    private void OnChanged(object? sender, StoreChangedEventArgs e) =>
        Changed?.Invoke(this, new TkStoreEventArgs((TkStoreEventKind)(int)e.Kind, e.SopInstanceUid, e.SeriesInstanceUid, e.StudyInstanceUid));

    /// <summary>Adds a file, keeping an existing copy of a duplicate.</summary>
    /// <param name="path">File.</param>
    /// <param name="indexInPlace">Reference the file instead of copying it.</param>
    /// <returns>What happened.</returns>
    public TkAddOutcome AddFile(string path, bool indexInPlace) =>
        (TkAddOutcome)(int)Tk.Call(() => _store.AddFile(path, DuplicatePolicy.Keep, indexInPlace ? AddMode.Reference : AddMode.Copy));

    /// <summary>Queries the index.</summary>
    /// <param name="filter">Filter.</param>
    /// <returns>Rows.</returns>
    public TkQueryResult Query(TkStoreFilter filter)
    {
        ArgumentNullException.ThrowIfNull(filter);
        var q = new DicomStoreQuery
        {
            Level = (DicomLevel)(int)filter.Level,
            PatientName = filter.PatientName,
            PatientId = filter.PatientId,
            AccessionNumber = filter.AccessionNumber,
            StudyDate = filter.StudyDate,
            Modality = filter.Modality,
            StudyInstanceUid = filter.StudyInstanceUid,
            SeriesInstanceUid = filter.SeriesInstanceUid,
            Offset = filter.Offset,
            Limit = filter.Limit,
            NoTotal = filter.NoTotal,
        };
        DicomStoreRecordCollection r = Tk.Call(() => _store.Query(q));
        return new TkQueryResult(r.Select(TkRecord.From).ToArray(), r.Total, r.IsTotalExact);
    }

    /// <summary>Series of a study ordered by Series Number.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <returns>Rows.</returns>
    public IReadOnlyList<TkRecord> GetSeries(string studyInstanceUid) =>
        Tk.Call(() => _store.GetSeries(studyInstanceUid)).Select(TkRecord.From).ToArray();

    /// <summary>Instances of a series with file paths.</summary>
    /// <param name="seriesInstanceUid">Series.</param>
    /// <returns>Rows.</returns>
    public IReadOnlyList<TkRecord> GetInstances(string seriesInstanceUid) =>
        Tk.Call(() => _store.GetInstances(seriesInstanceUid)).Select(TkRecord.From).ToArray();

    /// <summary>Deletes a patient (by patient key), study, series or instance.</summary>
    /// <param name="level">Level.</param>
    /// <param name="key">Patient key or UID.</param>
    /// <returns>Instances removed.</returns>
    public int Delete(TkLevel level, string key) => Tk.Call(() => _store.Delete((DicomLevel)(int)level, key));

    /// <summary>Totals.</summary>
    /// <returns>The totals.</returns>
    public TkStoreStats GetStatistics()
    {
        DicomStoreStatistics s = Tk.Call(_store.GetStatistics);
        return new TkStoreStats(s.Patients, s.Studies, s.Series, s.Instances, s.DataBytes);
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        _store.Changed -= OnChanged;
        _store.Dispose();
    }
}

// ====================================================================== files

/// <summary>Pixel description of a file.</summary>
/// <param name="Rows">Rows.</param>
/// <param name="Columns">Columns.</param>
/// <param name="Frames">Number of frames.</param>
/// <param name="SamplesPerPixel">Samples per pixel.</param>
/// <param name="BitsStored">Bits stored.</param>
/// <param name="IsSigned">Signed samples.</param>
/// <param name="IsLossy">Lossy compressed now or in the past.</param>
/// <param name="IsEncapsulated">Compressed pixel data.</param>
/// <param name="Photometric">Photometric Interpretation.</param>
public sealed record TkPixelInfo(int Rows, int Columns, int Frames, int SamplesPerPixel, int BitsStored, bool IsSigned, bool IsLossy,
                                 bool IsEncapsulated, string Photometric);

/// <summary>Where a pixel spacing came from.</summary>
public enum TkSpacingSource
{
    /// <summary>Unknown: measurements are in pixels.</summary>
    None = 0,
    /// <summary>Pixel Spacing (patient plane).</summary>
    PixelSpacing = 1,
    /// <summary>Imager Pixel Spacing at the detector.</summary>
    Imager = 2,
    /// <summary>Imager Pixel Spacing corrected by the magnification factor.</summary>
    ImagerMagnified = 3,
    /// <summary>Nominal Scanned Pixel Spacing.</summary>
    NominalScanned = 4,
    /// <summary>Ultrasound region calibration.</summary>
    UltrasoundRegion = 5,
    /// <summary>Calibrated by the user (app-only value).</summary>
    Manual = 100,
}

/// <summary>Plane geometry of a frame.</summary>
/// <param name="RowSpacing">Distance between rows in mm (applies to y), 0 when unknown.</param>
/// <param name="ColumnSpacing">Distance between columns in mm (applies to x), 0 when unknown.</param>
/// <param name="Source">Spacing source.</param>
/// <param name="HasPosition">Position known.</param>
/// <param name="PositionZ">Position along the patient axis (z component).</param>
/// <param name="SliceThickness">Slice thickness (0 when absent).</param>
public sealed record TkPlane(double RowSpacing, double ColumnSpacing, TkSpacingSource Source, bool HasPosition, double PositionZ,
                             double SliceThickness)
{
    /// <summary>Rows.</summary>
    public int Rows { get; init; }
    /// <summary>Columns.</summary>
    public int Columns { get; init; }
    /// <summary>Image Orientation known.</summary>
    public bool HasOrientation { get; init; }
    /// <summary>Image Position (Patient): centre of the first pixel, mm.</summary>
    public Vec3 Position { get; init; }
    /// <summary>Direction of image x (row direction cosines).</summary>
    public Vec3 RowCosine { get; init; }
    /// <summary>Direction of image y (column direction cosines).</summary>
    public Vec3 ColumnCosine { get; init; }

    /// <summary>The patient-space plane, or null without position, orientation or spacing.</summary>
    /// <param name="frameOfReferenceUid">Frame of Reference UID of the image.</param>
    /// <returns>The plane.</returns>
    public ImagePlane? ToImagePlane(string frameOfReferenceUid) =>
        HasPosition && HasOrientation && RowSpacing > 0 && ColumnSpacing > 0 && Rows > 0 && Columns > 0
            ? new ImagePlane(frameOfReferenceUid, Position, RowCosine, ColumnCosine, ColumnSpacing, RowSpacing, Columns, Rows)
            : null;
}

/// <summary>Orientation labels at the four edges.</summary>
/// <param name="Left">Left edge.</param>
/// <param name="Right">Right edge.</param>
/// <param name="Top">Top edge.</param>
/// <param name="Bottom">Bottom edge.</param>
public sealed record TkOrientation(string Left, string Right, string Top, string Bottom);

/// <summary>ROI shape (image pixel coordinates; (0,0) = top-left corner of the first pixel).</summary>
public enum TkRoiShape
{
    /// <summary>x0 y0 x1 y1.</summary>
    Rectangle = 0,
    /// <summary>cx cy rx ry angle.</summary>
    Ellipse = 1,
    /// <summary>cx cy r.</summary>
    Circle = 2,
    /// <summary>x y pairs.</summary>
    Polygon = 3,
    /// <summary>x y pairs.</summary>
    Freehand = 4,
}

/// <summary>ROI statistics in modality units.</summary>
/// <param name="Pixels">Pixels with weight.</param>
/// <param name="Mean">Mean.</param>
/// <param name="StandardDeviation">Population SD.</param>
/// <param name="Min">Minimum.</param>
/// <param name="Max">Maximum.</param>
/// <param name="AreaPixels">Area in pixels.</param>
/// <param name="AreaMm2">Area in mm2 or null.</param>
/// <param name="Source">Spacing used for the area.</param>
public sealed record TkRoiStats(long Pixels, double Mean, double StandardDeviation, double Min, double Max, double AreaPixels, double? AreaMm2,
                                TkSpacingSource Source);

/// <summary>SUV factor of a PET frame (SUVbw = value x BodyWeightFactor).</summary>
/// <param name="BodyWeightFactor">Factor, or null when it cannot be computed.</param>
/// <param name="Warnings">Plain-word warnings.</param>
public sealed record TkSuv(double? BodyWeightFactor, IReadOnlyList<string> Warnings);

/// <summary>Series sort warnings.</summary>
[Flags]
public enum TkSortWarnings
{
    /// <summary>None.</summary>
    None = 0,
    /// <summary>Some files have no geometry.</summary>
    MissingGeometry = 1,
    /// <summary>Several stacks.</summary>
    MultipleStacks = 2,
    /// <summary>Mixed orientations.</summary>
    MixedOrientation = 4,
    /// <summary>Duplicate positions.</summary>
    DuplicatePositions = 8,
    /// <summary>Uneven spacing.</summary>
    UnevenSpacing = 16,
    /// <summary>Gantry tilt.</summary>
    GantryTilt = 32,
    /// <summary>Several frames of reference.</summary>
    MultipleFramesOfReference = 64,
    /// <summary>Split by time.</summary>
    TemporalSplit = 128,
    /// <summary>Image sizes differ.</summary>
    SizeMismatch = 256,
}

/// <summary>One sorted slice.</summary>
/// <param name="FileIndex">Index into the input.</param>
/// <param name="Frame">Frame.</param>
/// <param name="Group">Stack group.</param>
public readonly record struct TkSlice(int FileIndex, int Frame, int Group);

/// <summary>Result of sorting a series.</summary>
/// <param name="Slices">Display order.</param>
/// <param name="Warnings">Warnings.</param>
public sealed record TkSortResult(IReadOnlyList<TkSlice> Slices, TkSortWarnings Warnings);

/// <summary>
/// A data set read (and, for generated test data, written) with generic getters: the file's main
/// data set or a sequence item. Getters return null instead of throwing for absent, empty or
/// unreadable values; text comes back as the toolkit decodes it (padding removed per VR).
/// </summary>
public sealed class TkDataset
{
    private readonly DicomDataset _ds;

    internal TkDataset(DicomDataset ds) => _ds = ds;

    /// <summary>The element is present.</summary>
    /// <param name="tag">Tag.</param>
    /// <returns>True when present.</returns>
    public bool Contains(TkTag tag)
    {
        try
        {
            return _ds.Contains(tag.ToBinding());
        }
        catch (SiaaraException)
        {
            return false;
        }
    }

    /// <summary>Value <paramref name="index"/> as text, or null when absent, out of range or empty.</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="index">Value index.</param>
    /// <returns>The text.</returns>
    public string? GetString(TkTag tag, int index = 0)
    {
        try
        {
            return _ds.TryGetString(tag.ToBinding(), index, out string? v) && v.Length > 0 ? v : null;
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>Number of values (0 when absent).</summary>
    /// <param name="tag">Tag.</param>
    /// <returns>The count.</returns>
    public int ValueCount(TkTag tag)
    {
        try
        {
            return _ds.Contains(tag.ToBinding()) ? _ds.GetValueMultiplicity(tag.ToBinding()) : 0;
        }
        catch (SiaaraException)
        {
            return 0;
        }
    }

    /// <summary>Value <paramref name="index"/> as a number (DS / IS text or a binary VR), or null.</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="index">Value index.</param>
    /// <returns>The number.</returns>
    public double? GetDouble(TkTag tag, int index = 0)
    {
        try
        {
            return _ds.TryGetDouble(tag.ToBinding(), out double v, index) ? v : null;
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>Items of a sequence (0 when absent or not a sequence).</summary>
    /// <param name="sequence">Sequence tag.</param>
    /// <returns>The count.</returns>
    public int ItemCount(TkTag sequence)
    {
        try
        {
            return _ds.Contains(sequence.ToBinding()) ? _ds.GetItemCount(sequence.ToBinding()) : 0;
        }
        catch (SiaaraException)
        {
            return 0;
        }
    }

    /// <summary>Item <paramref name="index"/> of a sequence, or null.</summary>
    /// <param name="sequence">Sequence tag.</param>
    /// <param name="index">Item index.</param>
    /// <returns>The item.</returns>
    public TkDataset? Item(TkTag sequence, int index)
    {
        try
        {
            return index >= 0 && index < ItemCount(sequence) ? new TkDataset(_ds.GetItem(sequence.ToBinding(), index)) : null;
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>Sets a text value with the dictionary VR (generated data).</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="value">Value.</param>
    public void SetString(TkTag tag, string value) => Tk.Call(() => _ds.SetString(tag.ToBinding(), value));

    /// <summary>Sets a raw text value with an explicit VR (generated data, private elements).</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="vr">VR.</param>
    /// <param name="value">Value.</param>
    public void SetRawString(TkTag tag, string vr, string value) => Tk.Call(() => _ds.SetRawString(tag.ToBinding(), vr, value));

    /// <summary>Appends an empty item to a sequence (generated data).</summary>
    /// <param name="sequence">Sequence.</param>
    /// <returns>The item.</returns>
    public TkDataset AppendItem(TkTag sequence) => new(Tk.Call(() => _ds.AppendItem(sequence.ToBinding())));
}

/// <summary>A DICOM file (headers only or with pixel data). Readable from many threads.</summary>
public sealed class TkFile : IDisposable
{
    private static readonly (SeriesSortWarnings From, TkSortWarnings To)[] s_sortMap =
    [
        (SeriesSortWarnings.MissingGeometry, TkSortWarnings.MissingGeometry),
        (SeriesSortWarnings.MultipleStacks, TkSortWarnings.MultipleStacks),
        (SeriesSortWarnings.MixedOrientation, TkSortWarnings.MixedOrientation),
        (SeriesSortWarnings.DuplicatePositions, TkSortWarnings.DuplicatePositions),
        (SeriesSortWarnings.UnevenSpacing, TkSortWarnings.UnevenSpacing),
        (SeriesSortWarnings.GantryTilt, TkSortWarnings.GantryTilt),
        (SeriesSortWarnings.MultipleFramesOfReference, TkSortWarnings.MultipleFramesOfReference),
        (SeriesSortWarnings.TemporalSplit, TkSortWarnings.TemporalSplit),
        (SeriesSortWarnings.SizeMismatch, TkSortWarnings.SizeMismatch),
    ];

    private TkFile(DicomFile file, bool headersOnly)
    {
        File = file;
        HeadersOnly = headersOnly;
    }

    internal DicomFile File { get; }

    /// <summary>True after Dispose.</summary>
    public bool IsDisposed => File.IsDisposed;

    /// <summary>The main data set, for the generic getters.</summary>
    public TkDataset Dataset => new(File.Dataset);

    /// <summary>
    /// Modality values of a grayscale frame as float32 (after the Modality LUT / rescale), row by
    /// row: the input of the app's ROI statistics. Needs pixel data.
    /// </summary>
    /// <param name="frame">Frame.</param>
    /// <returns>rows x columns values.</returns>
    public float[] DecodeModalityFloat32(int frame)
    {
        byte[] bytes = Tk.Call(() => File.DecodeFrame(frame, FrameFormat.Float32Modality));
        float[] values = new float[bytes.Length / sizeof(float)];
        Buffer.BlockCopy(bytes, 0, values, 0, values.Length * sizeof(float));
        return values;
    }

    /// <summary>Opened with stop-before-pixel-data.</summary>
    public bool HeadersOnly { get; }

    /// <summary>Opens a file.</summary>
    /// <param name="path">Path.</param>
    /// <param name="headersOnly">Stop before Pixel Data.</param>
    /// <returns>The file.</returns>
    public static TkFile Open(string path, bool headersOnly) =>
        new(Tk.Call(() => DicomFile.Open(path, new DicomReadOptions { StopBeforePixelData = headersOnly, AllowRawDataset = true })), headersOnly);

    /// <summary>A new empty file (for generated test and sample data).</summary>
    /// <returns>The file.</returns>
    public static TkFile Create() => new(Tk.Call(DicomFile.Create), false);

    /// <summary>The value as text, or null when absent or empty.</summary>
    /// <param name="tag">Tag.</param>
    /// <returns>The value.</returns>
    public string? GetString(TkTag tag)
    {
        try
        {
            return File.Dataset.TryGetString(tag.ToBinding(), out string? v) && v.Length > 0 ? v : null;
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>All values as text (empty when absent).</summary>
    /// <param name="tag">Tag.</param>
    /// <returns>The values.</returns>
    public IReadOnlyList<string> GetStrings(TkTag tag)
    {
        try
        {
            return File.Dataset.Contains(tag.ToBinding()) ? File.Dataset.GetStrings(tag.ToBinding()) : [];
        }
        catch (SiaaraException)
        {
            return [];
        }
    }

    /// <summary>The value as a number, or null.</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="index">Value index.</param>
    /// <returns>The value.</returns>
    public double? GetDouble(TkTag tag, int index = 0)
    {
        try
        {
            return File.Dataset.TryGetDouble(tag.ToBinding(), out double v, index) ? v : null;
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>Items of a sequence as data sets readable with the same getters, through a callback.</summary>
    /// <param name="sequence">Sequence tag.</param>
    /// <param name="item">Item index.</param>
    /// <param name="tag">Tag inside the item.</param>
    /// <returns>The value, or null.</returns>
    public string? GetItemString(TkTag sequence, int item, TkTag tag)
    {
        try
        {
            DicomDataset ds = File.Dataset;
            if (!ds.Contains(sequence.ToBinding()) || ds.GetItemCount(sequence.ToBinding()) <= item)
            {
                return null;
            }
            return ds.GetItem(sequence.ToBinding(), item).TryGetString(tag.ToBinding(), out string? v) && v.Length > 0 ? v : null;
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>Sets a text value (VR from the dictionary).</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="value">Value.</param>
    public void SetString(TkTag tag, string value) => Tk.Call(() => File.Dataset.SetString(tag.ToBinding(), value));

    /// <summary>Sets a raw text value with an explicit VR.</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="vr">VR.</param>
    /// <param name="value">Value.</param>
    public void SetRawString(TkTag tag, string vr, string value) => Tk.Call(() => File.Dataset.SetRawString(tag.ToBinding(), vr, value));

    /// <summary>Sets a US value.</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="value">Value.</param>
    public void SetUInt16(TkTag tag, ushort value) => Tk.Call(() => File.Dataset.SetUInt16(tag.ToBinding(), value));

    /// <summary>Sets binary data.</summary>
    /// <param name="tag">Tag.</param>
    /// <param name="vr">OB / OW.</param>
    /// <param name="data">Bytes.</param>
    public void SetBytes(TkTag tag, string vr, ReadOnlySpan<byte> data)
    {
        try
        {
            File.Dataset.SetBytes(tag.ToBinding(), vr, data);
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
    }

    /// <summary>Adds a sequence item and sets text values in it.</summary>
    /// <param name="sequence">Sequence.</param>
    /// <param name="values">(tag, value) pairs set with the dictionary VR.</param>
    public void AppendItem(TkTag sequence, IReadOnlyList<(TkTag Tag, string Value)> values)
    {
        ArgumentNullException.ThrowIfNull(values);
        Tk.Call(() =>
        {
            DicomDataset item = File.Dataset.AppendItem(sequence.ToBinding());
            foreach ((TkTag t, string v) in values)
            {
                item.SetString(t.ToBinding(), v);
            }
        });
    }

    /// <summary>Saves as explicit VR little endian.</summary>
    /// <param name="path">Destination.</param>
    public void Save(string path) => Tk.Call(() => File.Save(path, DicomTransferSyntaxes.ExplicitVRLittleEndian));

    /// <summary>Transfer Syntax UID.</summary>
    public string TransferSyntaxUid => Tk.Call(() => File.TransferSyntaxUid);

    /// <summary>Pixel description, or null without valid pixel data (headers-only files report what the header says).</summary>
    public TkPixelInfo? PixelInfo =>
        File.TryGetPixelInfo(out DicomPixelInfo? p)
            ? new TkPixelInfo(p.Rows, p.Columns, p.NumberOfFrames, p.SamplesPerPixel, p.BitsStored, p.IsSigned, p.IsLossy, p.IsEncapsulated,
                p.PhotometricInterpretation)
            : null;

    /// <summary>Plane geometry of a frame.</summary>
    /// <param name="frame">Frame.</param>
    /// <returns>Geometry.</returns>
    public TkPlane GetGeometry(int frame)
    {
        PlaneGeometry g = Tk.Call(() => ImageGeometry.GetFrameGeometry(File, frame));
        return new TkPlane(g.RowSpacing, g.ColumnSpacing, (TkSpacingSource)(int)g.SpacingSource, g.HasPosition,
            g.HasPosition ? g.Position.Z : 0, g.SliceThickness)
        {
            Rows = g.Rows,
            Columns = g.Columns,
            HasOrientation = g.HasOrientation,
            Position = new Vec3(g.Position.X, g.Position.Y, g.Position.Z),
            RowCosine = new Vec3(g.RowCosine.X, g.RowCosine.Y, g.RowCosine.Z),
            ColumnCosine = new Vec3(g.ColumnCosine.X, g.ColumnCosine.Y, g.ColumnCosine.Z),
        };
    }

    /// <summary>Orientation labels, or null without orientation.</summary>
    /// <param name="frame">Frame.</param>
    /// <returns>Labels.</returns>
    public TkOrientation? GetOrientation(int frame)
    {
        try
        {
            OrientationLabels? o = ImageGeometry.GetOrientationLabels(File, frame);
            return o is null ? null : new TkOrientation(o.Left, o.Right, o.Top, o.Bottom);
        }
        catch (SiaaraException)
        {
            return null;
        }
    }

    /// <summary>ROI statistics of modality values (needs pixel data).</summary>
    /// <param name="frame">Frame.</param>
    /// <param name="shape">Shape.</param>
    /// <param name="parameters">Shape parameters.</param>
    /// <param name="partialWeighting">Weight edge pixels by covered area.</param>
    /// <returns>Statistics.</returns>
    public TkRoiStats ComputeRoi(int frame, TkRoiShape shape, ReadOnlySpan<double> parameters, bool partialWeighting)
    {
        try
        {
            RoiStatistics s = RoiStatistics.Compute(File, frame, (RoiShape)(int)shape, parameters, partialWeighting);
            return new TkRoiStats(s.Pixels, s.Mean, s.StandardDeviation, s.Min, s.Max, s.AreaPixels, s.AreaSquareMillimetres,
                (TkSpacingSource)(int)s.SpacingSource);
        }
        catch (SiaaraException e)
        {
            throw Tk.Wrap(e);
        }
    }

    /// <summary>SUV body-weight factor of a PET frame.</summary>
    /// <param name="frame">Frame.</param>
    /// <returns>Factor and warnings.</returns>
    public TkSuv ComputeSuv(int frame)
    {
        SuvFactors f = Tk.Call(() => SuvFactors.Compute(File, frame));
        var w = new List<string>();
        foreach (SuvWarnings flag in Enum.GetValues<SuvWarnings>())
        {
            if (flag != SuvWarnings.None && f.Warnings.HasFlag(flag))
            {
                w.Add(SuvWarningText(flag));
            }
        }
        return new TkSuv(f.BodyWeight, w);
    }

    private static string SuvWarningText(SuvWarnings w) => w switch
    {
        SuvWarnings.MissingWeight => "patient weight missing",
        SuvWarnings.MissingDose => "injected dose missing",
        SuvWarnings.MissingHalfLife => "half-life missing",
        SuvWarnings.MissingInjectionTime => "injection time missing",
        SuvWarnings.MissingScanTime => "scan time missing",
        SuvWarnings.InconsistentTimes => "times inconsistent",
        SuvWarnings.CountsWithoutScale => "counts without a scale factor",
        SuvWarnings.NotDecayCorrected => "not decay corrected",
        SuvWarnings.NotAttenuationCorrected => "not attenuation corrected",
        SuvWarnings.UnsupportedUnits => "units not supported",
        SuvWarnings.SeriesAfterAcquisition => "series time after acquisition",
        SuvWarnings.MissingHeight => "patient height missing",
        SuvWarnings.MissingSex => "patient sex missing",
        SuvWarnings.MidnightRollover => "midnight rollover assumed",
        SuvWarnings.InjectionDateAssumed => "injection date assumed",
        SuvWarnings.MissingUnits => "units missing",
        SuvWarnings.ImplausibleValue => "implausible value",
        SuvWarnings.AlreadySuv => "values are already SUV",
        SuvWarnings.UnknownDecayCorrection => "unknown decay correction",
        SuvWarnings.MultipleRadiopharmaceuticals => "several radiopharmaceuticals",
        SuvWarnings.TimezoneMismatch => "time zone mismatch",
        SuvWarnings.VendorFactorUsed => "vendor SUV factor used",
        SuvWarnings.PerImageScanTime => "per-image scan time used",
        _ => "SUV warning",
    };

    /// <summary>Sorts files (null entries skipped) into display order.</summary>
    /// <param name="files">Header files.</param>
    /// <param name="expandFrames">One entry per frame of multi-frame files.</param>
    /// <returns>Order and warnings.</returns>
    public static TkSortResult SortSeries(IReadOnlyList<TkFile?> files, bool expandFrames)
    {
        ArgumentNullException.ThrowIfNull(files);
        var list = files.Select(f => f?.File).ToArray();
        SeriesSortResult r = Tk.Call(() => ImageGeometry.SortSeries(list, expandFrames));
        TkSortWarnings w = TkSortWarnings.None;
        foreach ((SeriesSortWarnings from, TkSortWarnings to) in s_sortMap)
        {
            if (r.Warnings.HasFlag(from))
            {
                w |= to;
            }
        }
        return new TkSortResult(r.Slices.Select(s => new TkSlice(s.FileIndex, s.Frame, s.Group)).ToArray(), w);
    }

    /// <inheritdoc/>
    public void Dispose() => File.Dispose();
}

// ====================================================================== frames and rendering

/// <summary>How the window is chosen.</summary>
public enum TkVoiSource
{
    /// <summary>The file's n-th window / VOI LUT (full range when it has none).</summary>
    File = 0,
    /// <summary>Explicit centre / width.</summary>
    Explicit = 1,
    /// <summary>Minimum to maximum of the frame.</summary>
    FullRange = 2,
    /// <summary>The whole possible modality range.</summary>
    Identity = 3,
}

/// <summary>Standard colour maps for grayscale images (PS3.6 Annex B palettes; Gray = none).</summary>
public enum TkColormap
{
    /// <summary>No colour map.</summary>
    Gray = 0,
    /// <summary>Inverse gray.</summary>
    InverseGray = 1,
    /// <summary>Hot Iron.</summary>
    HotIron = 2,
    /// <summary>PET.</summary>
    Pet = 3,
    /// <summary>Hot Metal Blue.</summary>
    HotMetalBlue = 4,
    /// <summary>PET 20 Step.</summary>
    Pet20Step = 5,
}

/// <summary>Everything a 2D render needs (maps to the toolkit's viewport options).</summary>
public sealed record RenderSpec
{
    /// <summary>Output width.</summary>
    public int Width { get; init; }
    /// <summary>Output height.</summary>
    public int Height { get; init; }
    /// <summary>1 byte per pixel instead of RGBA.</summary>
    public bool Gray { get; init; }
    /// <summary>Output pixels per image pixel; 0 = fit.</summary>
    public double Zoom { get; init; }
    /// <summary>Pan x in output pixels.</summary>
    public double PanX { get; init; }
    /// <summary>Pan y in output pixels.</summary>
    public double PanY { get; init; }
    /// <summary>Clockwise degrees after the flips.</summary>
    public double Rotation { get; init; }
    /// <summary>Mirror left-right.</summary>
    public bool FlipHorizontal { get; init; }
    /// <summary>Mirror top-bottom.</summary>
    public bool FlipVertical { get; init; }
    /// <summary>Bilinear instead of nearest.</summary>
    public bool Linear { get; init; }
    /// <summary>Window source.</summary>
    public TkVoiSource Voi { get; init; }
    /// <summary>File VOI index.</summary>
    public int VoiIndex { get; init; }
    /// <summary>Explicit centre.</summary>
    public double WindowCenter { get; init; }
    /// <summary>Explicit width.</summary>
    public double WindowWidth { get; init; }
    /// <summary>Invert.</summary>
    public bool Invert { get; init; }
    /// <summary>Hide overlays.</summary>
    public bool NoOverlays { get; init; }
    /// <summary>Ignore shutters.</summary>
    public bool NoShutters { get; init; }
    /// <summary>Colour map for grayscale images (output must then be RGBA).</summary>
    public TkColormap Colormap { get; init; }

    internal ViewportOptions ToBinding() => new()
    {
        Width = Width,
        Height = Height,
        Format = Gray ? PixelOutputFormat.Gray8 : PixelOutputFormat.Rgba8,
        Zoom = Zoom,
        PanX = PanX,
        PanY = PanY,
        Rotation = Rotation,
        FlipHorizontal = FlipHorizontal,
        FlipVertical = FlipVertical,
        Interpolation = Linear ? Interpolation.Linear : Interpolation.Nearest,
        VoiMode = (VoiMode)(int)Voi,
        VoiIndex = VoiIndex,
        WindowCenter = WindowCenter,
        WindowWidth = WindowWidth,
        Invert = Invert,
        NoOverlays = NoOverlays,
        NoShutters = NoShutters,
        Colormap = (Colormap)(int)Colormap,
        Background = new Rgba8Color(0, 0, 0, 255),
    };
}

/// <summary>Properties of a decoded frame.</summary>
/// <param name="Rows">Rows.</param>
/// <param name="Columns">Columns.</param>
/// <param name="Frames">Frames of the image.</param>
/// <param name="Monochrome">Grayscale.</param>
/// <param name="Photometric">Photometric Interpretation.</param>
/// <param name="MinValue">Minimum modality value.</param>
/// <param name="MaxValue">Maximum modality value.</param>
/// <param name="DecodedBytes">Decoded size.</param>
/// <param name="OverlayCount">Overlays.</param>
/// <param name="HasShutter">Any display shutter.</param>
public sealed record TkFrameInfo(int Rows, int Columns, int Frames, bool Monochrome, string Photometric, double MinValue, double MaxValue,
                                 long DecodedBytes, int OverlayCount, bool HasShutter);

/// <summary>A window (or VOI LUT) stored in the file.</summary>
/// <param name="Index">VOI index.</param>
/// <param name="IsLut">VOI LUT instead of a window.</param>
/// <param name="Center">Centre.</param>
/// <param name="Width">Width.</param>
/// <param name="Explanation">Explanation text from the file.</param>
public sealed record TkVoi(int Index, bool IsLut, double Center, double Width, string Explanation);

/// <summary>A frame decoded once for repeated rendering. Immutable; renders from many threads.</summary>
public sealed class TkFrame : IDisposable
{
    private readonly DicomFrame _frame;

    private TkFrame(DicomFrame frame)
    {
        _frame = frame;
        FrameProperties p = frame.Properties;
        const FrameCharacteristics shutters = FrameCharacteristics.RectangularShutter | FrameCharacteristics.CircularShutter |
                                              FrameCharacteristics.PolygonalShutter | FrameCharacteristics.BitmapShutter;
        Info = new TkFrameInfo(p.Rows, p.Columns, p.NumberOfFrames, p.Flags.HasFlag(FrameCharacteristics.Monochrome), p.PhotometricInterpretation,
            p.MinValue, p.MaxValue, p.DecodedBytes, p.OverlayCount, (p.Flags & shutters) != 0);
        Voi = frame.VoiEntries.Select(v => new TkVoi(v.Index, v.IsLut, v.Center, v.Width, v.Explanation)).ToArray();
    }

    /// <summary>Decodes a frame of a file with pixel data.</summary>
    /// <param name="file">File.</param>
    /// <param name="frame">Frame.</param>
    /// <returns>The decoded frame.</returns>
    public static TkFrame Open(TkFile file, int frame)
    {
        ArgumentNullException.ThrowIfNull(file);
        return new TkFrame(Tk.Call(() => DicomFrame.Open(file.File, frame)));
    }

    /// <summary>Properties.</summary>
    public TkFrameInfo Info { get; }

    /// <summary>Windows and VOI LUTs in the file.</summary>
    public IReadOnlyList<TkVoi> Voi { get; }

    /// <summary>Bytes needed for a render.</summary>
    /// <param name="spec">Spec.</param>
    /// <returns>Bytes.</returns>
    public int GetRenderSize(RenderSpec spec)
    {
        ArgumentNullException.ThrowIfNull(spec);
        return Tk.Call(() => _frame.GetRenderSize(spec.ToBinding()));
    }

    /// <summary>Renders without throwing (hot path).</summary>
    /// <param name="destination">Output.</param>
    /// <param name="spec">Spec.</param>
    /// <param name="bytesWritten">Bytes written or needed.</param>
    /// <returns>Status.</returns>
    public TkError TryRender(Span<byte> destination, RenderSpec spec, out int bytesWritten)
    {
        ArgumentNullException.ThrowIfNull(spec);
        return (TkError)(int)_frame.TryRender(destination, spec.ToBinding(), out bytesWritten);
    }

    /// <summary>Renders to a new array.</summary>
    /// <param name="spec">Spec.</param>
    /// <returns>Pixels.</returns>
    public byte[] Render(RenderSpec spec)
    {
        ArgumentNullException.ThrowIfNull(spec);
        return Tk.Call(() => _frame.Render(spec.ToBinding()));
    }

    /// <summary>Viewport point to image pixel coordinates (toolkit reference mapping).</summary>
    /// <param name="spec">Spec.</param>
    /// <param name="x">X.</param>
    /// <param name="y">Y.</param>
    /// <returns>Image point.</returns>
    public (double X, double Y) ViewToImage(RenderSpec spec, double x, double y)
    {
        ArgumentNullException.ThrowIfNull(spec);
        return Tk.Call(() => _frame.ViewToImage(spec.ToBinding(), x, y));
    }

    /// <summary>Image pixel coordinates to the viewport (toolkit reference mapping).</summary>
    /// <param name="spec">Spec.</param>
    /// <param name="x">X.</param>
    /// <param name="y">Y.</param>
    /// <returns>Viewport point.</returns>
    public (double X, double Y) ImageToView(RenderSpec spec, double x, double y)
    {
        ArgumentNullException.ThrowIfNull(spec);
        return Tk.Call(() => _frame.ImageToView(spec.ToBinding(), x, y));
    }

    /// <inheritdoc/>
    public void Dispose() => _frame.Dispose();
}
