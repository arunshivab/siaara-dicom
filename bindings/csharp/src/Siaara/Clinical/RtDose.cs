using System;
using Siaara.Interop;

namespace Siaara.Clinical;

/// <summary>Form of the RT Dose Grid Frame Offset Vector (3004,000C).</summary>
public enum GridFrameOffsetForm
{
    /// <summary>No Grid Frame Offset Vector.</summary>
    None = C.SIAARA_GFOV_NONE,
    /// <summary>First value 0: offsets from Image Position (Patient) along the plane normal.</summary>
    Relative = C.SIAARA_GFOV_RELATIVE,
    /// <summary>First value = Image Position (Patient) z: the z coordinates of transverse planes.</summary>
    Absolute = C.SIAARA_GFOV_ABSOLUTE,
}

/// <summary>
/// RT Dose module values (siaara_rt_dose_info). For an RT Dose object every modality value the
/// library produces (float frames, volumes, fusion) is the stored value times
/// <see cref="DoseGridScaling"/>, a dose in <see cref="DoseUnits"/>.
/// </summary>
public sealed record RtDoseInfo
{
    /// <summary>SOP Class RT Dose or Modality RTDOSE.</summary>
    public bool IsRtDose { get; init; }
    /// <summary>Dose Grid Scaling (3004,000E), or null when absent or not a positive number.</summary>
    public double? DoseGridScaling { get; init; }
    /// <summary>Dose Units (3004,0002): GY or RELATIVE.</summary>
    public string DoseUnits { get; init; } = "";
    /// <summary>Dose Type (3004,0004): PHYSICAL, EFFECTIVE or ERROR.</summary>
    public string DoseType { get; init; } = "";
    /// <summary>Dose Summation Type (3004,000A): PLAN, MULTI_PLAN, FRACTION, BEAM, ...</summary>
    public string DoseSummationType { get; init; } = "";
    /// <summary>Form of the Grid Frame Offset Vector.</summary>
    public GridFrameOffsetForm GridFrameOffsetForm { get; init; }
    /// <summary>Number of values in the Grid Frame Offset Vector.</summary>
    public int GridFrameOffsetCount { get; init; }

    /// <summary>Reads the RT Dose values of a data set (any data set: IsRtDose is false when it is not RT Dose).</summary>
    /// <param name="dataset">The data set.</param>
    /// <returns>The values.</returns>
    public static RtDoseInfo Read(DicomDataset dataset)
    {
        ArgumentNullException.ThrowIfNull(dataset);
        using var l = new HandleLease(dataset.File.Handle);
        var r = new siaara_rt_dose_info { struct_size = StructSize.Of<siaara_rt_dose_info>() };
        Status.Check(Native.siaara_rt_dose_info_get(dataset.Resolve(l.Pointer), ref r));
        return new RtDoseInfo
        {
            IsRtDose = r.is_rt_dose != 0,
            DoseGridScaling = r.has_dose_grid_scaling != 0 ? r.dose_grid_scaling : null,
            DoseUnits = DicomDictionary.Chars(r.dose_units),
            DoseType = DicomDictionary.Chars(r.dose_type),
            DoseSummationType = DicomDictionary.Chars(r.dose_summation_type),
            GridFrameOffsetForm = (GridFrameOffsetForm)r.grid_frame_offset_form,
            GridFrameOffsetCount = (int)r.grid_frame_offset_count,
        };
    }
}
