// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Every input of the SUV computation, read from the data set with the generic getters and with a
// record of the attribute each value came from (shown to the user next to the result). Text values
// are trimmed of DICOM padding only; nothing else is changed.

using System.Globalization;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Measure.Suv;

/// <summary>Inputs of the SUV computation as found in the data set.</summary>
public sealed class SuvInputs
{
    /// <summary>Private creator of the Philips PET private group (vendor DICOM Conformance Statement).</summary>
    public const string PhilipsCreator = "Philips PET Private Group";

    private static readonly TkTag UnitsTag = new(0x00541001);
    private static readonly TkTag DecayCorrectionTag = new(0x00541102);
    private static readonly TkTag CorrectedImageTag = new(0x00280051);
    private static readonly TkTag AcquisitionDateTimeTag = new(0x0008002A);
    private static readonly TkTag TimezoneOffsetFromUtc = new(0x00080201);
    private static readonly TkTag RadiopharmaceuticalInformation = new(0x00540016);
    private static readonly TkTag RadiopharmaceuticalStartTime = new(0x00181072);
    private static readonly TkTag RadiopharmaceuticalStartDateTime = new(0x00181078);
    private static readonly TkTag RadionuclideTotalDose = new(0x00181074);
    private static readonly TkTag RadionuclideHalfLife = new(0x00181075);
    private static readonly TkTag PerFrameFunctionalGroups = new(0x52009230);
    private static readonly TkTag SharedFunctionalGroups = new(0x52009229);
    private static readonly TkTag FrameContent = new(0x00209111);
    private static readonly TkTag FrameAcquisitionDateTimeTag = new(0x00189074);

    private readonly Dictionary<string, string> _sources = new(StringComparer.Ordinal);

    /// <summary>Units (0054,1001), or null.</summary>
    public string? Units { get; private set; }

    /// <summary>Decay Correction (0054,1102), or null.</summary>
    public string? DecayCorrection { get; private set; }

    /// <summary>Corrected Image (0028,0051) values.</summary>
    public IReadOnlyList<string> CorrectedImage { get; private set; } = [];

    /// <summary>Patient's Weight (0010,1030) in kg, or null.</summary>
    public double? WeightKg { get; private set; }

    /// <summary>Patient's Size (0010,1020) in m, or null.</summary>
    public double? HeightM { get; private set; }

    /// <summary>Patient's Sex (0010,0040), or null.</summary>
    public string? Sex { get; private set; }

    /// <summary>Items of the Radiopharmaceutical Information Sequence (0054,0016); the first is used.</summary>
    public int RadiopharmaceuticalItems { get; private set; }

    /// <summary>Radionuclide Total Dose (0018,1074) in Bq, or null.</summary>
    public double? TotalDoseBq { get; private set; }

    /// <summary>Radionuclide Half Life (0018,1075) in s, or null.</summary>
    public double? HalfLifeSeconds { get; private set; }

    /// <summary>Radiopharmaceutical Start Time (0018,1072), or null.</summary>
    public string? StartTime { get; private set; }

    /// <summary>Radiopharmaceutical Start DateTime (0018,1078), or null.</summary>
    public string? StartDateTime { get; private set; }

    /// <summary>Series Date (0008,0021), or null.</summary>
    public string? SeriesDate { get; private set; }

    /// <summary>Series Time (0008,0031), or null.</summary>
    public string? SeriesTime { get; private set; }

    /// <summary>Acquisition Date (0008,0022), or null.</summary>
    public string? AcquisitionDate { get; private set; }

    /// <summary>Acquisition Time (0008,0032), or null.</summary>
    public string? AcquisitionTime { get; private set; }

    /// <summary>Acquisition DateTime (0008,002A), or null.</summary>
    public string? AcquisitionDateTime { get; private set; }

    /// <summary>Frame Acquisition DateTime (0018,9074) of the frame's Frame Content functional group, or null.</summary>
    public string? FrameAcquisitionDateTime { get; private set; }

    /// <summary>Timezone Offset From UTC (0008,0201), or null.</summary>
    public string? TimezoneOffset { get; private set; }

    /// <summary>Philips Activity Concentration Scale Factor (7053,xx09), Bq/ml per count, or null (read for Units CNTS only).</summary>
    public double? PhilipsScaleFactor { get; private set; }

    /// <summary>Where each value came from: input name to attribute (and item path), for every value found.</summary>
    public IReadOnlyDictionary<string, string> Sources => _sources;

    // Text value trimmed of padding, null when absent or empty.
    private static string? Text(TkDataset ds, TkTag tag, int index = 0) => ds.GetString(tag, index) is string s && DecayTime.Trim(s) is { Length: > 0 } t ? t : null;

    private T? Note<T>(string input, string attribute, T? value)
    {
        if (value is not null)
        {
            _sources[input] = attribute;
        }
        return value;
    }

    private double? NoteNumber(string input, string attribute, double? value)
    {
        if (value is not null)
        {
            _sources[input] = attribute;
        }
        return value;
    }

    /// <summary>Reads the inputs for one frame.</summary>
    /// <param name="ds">The data set.</param>
    /// <param name="frame">Frame (matters for enhanced multi-frame objects).</param>
    /// <returns>The inputs.</returns>
    public static SuvInputs Read(TkDataset ds, int frame)
    {
        ArgumentNullException.ThrowIfNull(ds);
        var r = new SuvInputs();
        r.Units = r.Note("units", "Units (0054,1001)", Text(ds, UnitsTag));
        r.DecayCorrection = r.Note("decayCorrection", "Decay Correction (0054,1102)", Text(ds, DecayCorrectionTag));
        int n = ds.ValueCount(CorrectedImageTag);
        var corrected = new List<string>(n);
        for (int i = 0; i < n; i++)
        {
            if (Text(ds, CorrectedImageTag, i) is string v)
            {
                corrected.Add(v);
            }
        }
        r.CorrectedImage = corrected;
        if (corrected.Count > 0)
        {
            r._sources["correctedImage"] = "Corrected Image (0028,0051)";
        }
        r.WeightKg = r.NoteNumber("weight", "Patient's Weight (0010,1030)", ds.GetDouble(Tags.PatientWeight));
        r.HeightM = r.NoteNumber("height", "Patient's Size (0010,1020)", ds.GetDouble(Tags.PatientSize));
        r.Sex = r.Note("sex", "Patient's Sex (0010,0040)", Text(ds, Tags.PatientSex));
        r.RadiopharmaceuticalItems = ds.ItemCount(RadiopharmaceuticalInformation);
        if (ds.Item(RadiopharmaceuticalInformation, 0) is TkDataset rp)
        {
            const string In = "Radiopharmaceutical Information Sequence (0054,0016) item 1 > ";
            r.TotalDoseBq = r.NoteNumber("dose", In + "Radionuclide Total Dose (0018,1074)", rp.GetDouble(RadionuclideTotalDose));
            r.HalfLifeSeconds = r.NoteNumber("halfLife", In + "Radionuclide Half Life (0018,1075)", rp.GetDouble(RadionuclideHalfLife));
            r.StartTime = r.Note("startTime", In + "Radiopharmaceutical Start Time (0018,1072)", Text(rp, RadiopharmaceuticalStartTime));
            r.StartDateTime = r.Note("startDateTime", In + "Radiopharmaceutical Start DateTime (0018,1078)", Text(rp, RadiopharmaceuticalStartDateTime));
        }
        r.SeriesDate = r.Note("seriesDate", "Series Date (0008,0021)", Text(ds, Tags.SeriesDate));
        r.SeriesTime = r.Note("seriesTime", "Series Time (0008,0031)", Text(ds, Tags.SeriesTime));
        r.AcquisitionDate = r.Note("acquisitionDate", "Acquisition Date (0008,0022)", Text(ds, Tags.AcquisitionDate));
        r.AcquisitionTime = r.Note("acquisitionTime", "Acquisition Time (0008,0032)", Text(ds, Tags.AcquisitionTime));
        r.AcquisitionDateTime = r.Note("acquisitionDateTime", "Acquisition DateTime (0008,002A)", Text(ds, AcquisitionDateTimeTag));
        (TkDataset? content, string where) = FrameContentGroup(ds, frame);
        if (content is not null)
        {
            r.FrameAcquisitionDateTime = r.Note("frameAcquisitionDateTime", where + "Frame Acquisition DateTime (0018,9074)", Text(content, FrameAcquisitionDateTimeTag));
        }
        r.TimezoneOffset = r.Note("timezone", "Timezone Offset From UTC (0008,0201)", Text(ds, TimezoneOffsetFromUtc));
        if (string.Equals(r.Units, "CNTS", StringComparison.Ordinal) && PhilipsFactorTag(ds) is TkTag factor)
        {
            r.PhilipsScaleFactor = r.NoteNumber("philipsScaleFactor", "Activity Concentration Scale Factor " + factor + " (" + PhilipsCreator + ")", ds.GetDouble(factor));
        }
        return r;
    }

    // The Frame Content macro of the frame: in its Per-frame Functional Groups item when that item
    // has one, else in the Shared Functional Groups.
    private static (TkDataset? Content, string Where) FrameContentGroup(TkDataset ds, int frame)
    {
        if (frame < ds.ItemCount(PerFrameFunctionalGroups) && ds.Item(PerFrameFunctionalGroups, frame) is TkDataset perFrame
            && perFrame.Item(FrameContent, 0) is TkDataset fc)
        {
            return (fc, "Per-frame Functional Groups (5200,9230) item " + (frame + 1).ToString(CultureInfo.InvariantCulture) + " > Frame Content (0020,9111) > ");
        }
        if (ds.Item(SharedFunctionalGroups, 0) is TkDataset shared && shared.Item(FrameContent, 0) is TkDataset sfc)
        {
            return (sfc, "Shared Functional Groups (5200,9229) > Frame Content (0020,9111) > ");
        }
        return (null, "");
    }

    // (7053,xx09) of the block whose private creator (7053,00xx) is the Philips PET group.
    private static TkTag? PhilipsFactorTag(TkDataset ds)
    {
        for (uint block = 0x10; block <= 0xFF; block++)
        {
            var creator = new TkTag((0x7053u << 16) | block);
            if (ds.Contains(creator) && string.Equals(ds.GetString(creator), PhilipsCreator, StringComparison.Ordinal))
            {
                return new TkTag((0x7053u << 16) | (block << 8) | 0x09);
            }
        }
        return null;
    }
}
