// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Store;

/// <summary>A study row of the browser. Stored values are kept as stored; Display* members format for the screen only.</summary>
public sealed class StudyRow
{
    internal StudyRow(TkRecord r)
    {
        StudyInstanceUid = r.StudyInstanceUid;
        PatientName = r.PatientName;
        PatientId = r.PatientId;
        PatientSex = r.PatientSex;
        PatientBirthDate = r.PatientBirthDate;
        PatientKey = r.PatientKey;
        StudyDate = r.StudyDate;
        StudyTime = r.StudyTime;
        Modalities = r.ModalitiesInStudy.Length > 0 ? r.ModalitiesInStudy : r.Modality;
        Description = r.StudyDescription;
        AccessionNumber = r.AccessionNumber;
        Images = r.NumberOfInstances;
        SeriesCount = r.NumberOfSeries;
    }

    /// <summary>Study Instance UID.</summary>
    public string StudyInstanceUid { get; }
    /// <summary>Patient's Name as stored.</summary>
    public string PatientName { get; }
    /// <summary>Patient ID.</summary>
    public string PatientId { get; }
    /// <summary>Patient's Sex.</summary>
    public string PatientSex { get; }
    /// <summary>Patient's Birth Date.</summary>
    public string PatientBirthDate { get; }
    /// <summary>Store patient key.</summary>
    public string PatientKey { get; }
    /// <summary>Study Date.</summary>
    public string StudyDate { get; }
    /// <summary>Study Time.</summary>
    public string StudyTime { get; }
    /// <summary>Modalities in Study (backslash separated).</summary>
    public string Modalities { get; }
    /// <summary>Study Description.</summary>
    public string Description { get; }
    /// <summary>Accession Number.</summary>
    public string AccessionNumber { get; }
    /// <summary>Number of instances.</summary>
    public int Images { get; }
    /// <summary>Number of series.</summary>
    public int SeriesCount { get; }

    /// <summary>Name for display ('^' shown as ", " / space).</summary>
    public string DisplayName => DisplayFormat.PersonName(PatientName);
    /// <summary>Sex / age for display.</summary>
    public string DisplaySexAge => DisplayFormat.SexAge(PatientSex, DisplayFormat.Age(null, PatientBirthDate, StudyDate));
    /// <summary>Study date for display.</summary>
    public string DisplayDate => DisplayFormat.Date(StudyDate);
    /// <summary>Modalities for display.</summary>
    public string DisplayModalities => Modalities.Replace('\\', ' ');
}

/// <summary>A series row.</summary>
public sealed class SeriesRow
{
    internal SeriesRow(TkRecord r)
    {
        Record = r;
    }

    internal TkRecord Record { get; }
    /// <summary>Series Instance UID.</summary>
    public string SeriesInstanceUid => Record.SeriesInstanceUid;
    /// <summary>Series Number.</summary>
    public int? SeriesNumber => Record.SeriesNumber;
    /// <summary>Series Description.</summary>
    public string Description => Record.SeriesDescription;
    /// <summary>Modality.</summary>
    public string Modality => Record.Modality;
    /// <summary>Instances.</summary>
    public int Instances => Record.NumberOfInstances;
}

/// <summary>An instance row with its file.</summary>
/// <param name="SopInstanceUid">SOP Instance UID.</param>
/// <param name="FilePath">File path.</param>
/// <param name="InstanceNumber">Instance Number.</param>
/// <param name="Frames">Frames.</param>
/// <param name="HasPixelData">Has pixel data.</param>
public sealed record InstanceRow(string SopInstanceUid, string FilePath, int? InstanceNumber, int Frames, bool HasPixelData);

/// <summary>A page of studies.</summary>
/// <param name="Rows">Rows.</param>
/// <param name="Total">Total (lower bound when not exact).</param>
/// <param name="TotalExact">Exact total.</param>
public sealed record StudyPage(IReadOnlyList<StudyRow> Rows, int Total, bool TotalExact);

/// <summary>Browser filter.</summary>
public sealed record StudyQuery
{
    /// <summary>Free text: matches name (contains), patient ID or accession (prefix).</summary>
    public string Text { get; init; } = "";
    /// <summary>First study date (inclusive) or null.</summary>
    public DateOnly? From { get; init; }
    /// <summary>Last study date (inclusive) or null.</summary>
    public DateOnly? To { get; init; }
    /// <summary>Modalities (any of); empty = all. "CR/DX" chips pass both.</summary>
    public IReadOnlyList<string> Modalities { get; init; } = [];
    /// <summary>Rows to skip.</summary>
    public int Offset { get; init; }
    /// <summary>Page size.</summary>
    public int Limit { get; init; } = 50;

    internal string? DateFilter
    {
        get
        {
            if (From is null && To is null)
            {
                return null;
            }
            string f = From?.ToString("yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture) ?? "";
            string t = To?.ToString("yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture) ?? "";
            return f == t ? f : f + "-" + t;
        }
    }

    internal string? ModalityFilter => Modalities.Count == 0 ? null : string.Join('\\', Modalities);
}

/// <summary>A store change as posted by the store callback.</summary>
/// <param name="Kind">Kind.</param>
/// <param name="SopInstanceUid">Instance.</param>
/// <param name="SeriesInstanceUid">Series.</param>
/// <param name="StudyInstanceUid">Study.</param>
public readonly record struct StoreEvent(TkStoreEventKind Kind, string SopInstanceUid, string SeriesInstanceUid, string StudyInstanceUid);

/// <summary>Studies changed (batched store events).</summary>
public sealed class StudiesChangedEventArgs : EventArgs
{
    /// <summary>Creates the event.</summary>
    /// <param name="studies">Changed Study Instance UIDs.</param>
    /// <param name="rebuilt">The index was rebuilt.</param>
    public StudiesChangedEventArgs(IReadOnlySet<string> studies, bool rebuilt)
    {
        Studies = studies;
        Rebuilt = rebuilt;
    }

    /// <summary>Changed studies.</summary>
    public IReadOnlySet<string> Studies { get; }
    /// <summary>Rebuilt.</summary>
    public bool Rebuilt { get; }
}
