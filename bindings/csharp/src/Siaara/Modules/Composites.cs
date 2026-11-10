using System;
using System.Collections.Generic;

namespace Siaara.Modules;

// Hand-written additions to generated classes: the composites the owner named (documented in
// docs/DATA_ACCESS.md) and the PS3.3 control point inheritance. Each composite only joins or
// selects stored values; none computes anything.

/// <summary>Patient Module composites.</summary>
public sealed partial class PatientModule
{
    /// <summary>Patient's Name (0010,0010) as a <see cref="PersonName"/> (same as <see cref="PatientName"/>).</summary>
    public PersonName? Name => PatientName;

    /// <summary>Composite: the given name of Patient's Name (alphabetic group).</summary>
    public string? FirstName => PatientName?.First;

    /// <summary>Composite: the family name of Patient's Name (alphabetic group).</summary>
    public string? LastName => PatientName?.Last;

    /// <summary>Composite: <see cref="PersonName.Full"/> of Patient's Name.</summary>
    public string? FullName => PatientName?.Full;
}

/// <summary>General Equipment Module composites.</summary>
public sealed partial class GeneralEquipmentModule
{
    /// <summary>
    /// Composite: Manufacturer (0008,0070) and Manufacturer's Model Name (0008,1090) as stored,
    /// joined by one space; the one present when only one is; null when both are absent.
    /// </summary>
    public string? MakeAndModel => Composite.Join(Manufacturer, ManufacturerModelName);

    /// <summary>Composite: Software Versions (0018,1020) values joined by one space, as stored.</summary>
    public string? SoftwareVersionsText => SoftwareVersions is { Length: > 0 } v ? string.Join(' ', v) : null;
}

/// <summary>Enhanced General Equipment Module composites.</summary>
public sealed partial class EnhancedGeneralEquipmentModule
{
    /// <summary>Composite: Manufacturer and Manufacturer's Model Name joined by one space (see <see cref="GeneralEquipmentModule.MakeAndModel"/>).</summary>
    public string? MakeAndModel => Composite.Join(Manufacturer, ManufacturerModelName);
}

/// <summary>General Study Module composites.</summary>
public sealed partial class GeneralStudyModule
{
    /// <summary>Composite: Study Date + Study Time; null unless both are present and well formed.</summary>
    public DateTime? StudyDateTime => Composite.At(StudyDate, StudyTime);

    /// <summary>Referring Physician's Name as a <see cref="PersonName"/> (same as <see cref="ReferringPhysicianName"/>).</summary>
    public PersonName? ReferringPhysician => ReferringPhysicianName;
}

/// <summary>General Series Module composites.</summary>
public sealed partial class GeneralSeriesModule
{
    /// <summary>Composite: Series Date + Series Time; null unless both are present and well formed.</summary>
    public DateTime? SeriesDateTime => Composite.At(SeriesDate, SeriesTime);
}

/// <summary>General Image Module composites.</summary>
public sealed partial class GeneralImageModule
{
    /// <summary>Composite: Content Date + Content Time; null unless both are present and well formed.</summary>
    public DateTime? ContentDateTime => Composite.At(ContentDate, ContentTime);
}

/// <summary>General Acquisition Module composites.</summary>
public sealed partial class GeneralAcquisitionModule
{
    /// <summary>Composite: Acquisition Date + Acquisition Time; null unless both are present and well formed.</summary>
    public DateTime? AcquisitionDateAndTime => Composite.At(AcquisitionDate, AcquisitionTime);
}

internal static class Composite
{
    internal static string? Join(string? a, string? b) =>
        a is null ? b : b is null ? a : a + " " + b;

    internal static DateTime? At(DateOnly? d, TimeOnly? t) =>
        d is not null && t is not null ? d.Value.ToDateTime(t.Value) : null;

    // Chains for the inherited control point view: item k, k-1, ..., 0.
    internal static IReadOnlyList<T> Inherited<T>(DicomDataset beam, DicomTag sequence, Func<DicomDataset[], T> make)
    {
        IReadOnlyList<DicomDataset> cps = beam.GetSequence(sequence);
        var r = new T[cps.Count];
        for (int k = 0; k < cps.Count; k++)
        {
            var chain = new DicomDataset[k + 1];
            for (int i = 0; i <= k; i++)
            {
                chain[i] = cps[k - i];
            }
            r[k] = make(chain);
        }
        return r;
    }

    private static readonly DicomTag s_positions = new(0x300A011Au);
    private static readonly DicomTag s_deviceType = new(0x300A00B8u);
    private static readonly DicomTag s_leafJaw = new(0x300A011Cu);

    // Leaf / Jaw Positions of a device type: the nearest control point in the chain that has it.
    internal static double[]? Positions(IReadOnlyList<DicomDataset> chain, string deviceType, out int fromIndex)
    {
        ArgumentNullException.ThrowIfNull(deviceType);
        for (int i = 0; i < chain.Count; i++)
        {
            foreach (DicomDataset item in chain[i].GetSequence(s_positions))
            {
                if (item[s_deviceType] != deviceType || !item.Contains(s_leafJaw))
                {
                    continue;
                }
                int n = item.GetValueMultiplicity(s_leafJaw);
                var v = new double[n];
                for (int j = 0; j < n; j++)
                {
                    v[j] = item.GetDouble(s_leafJaw, j);
                }
                fromIndex = i;
                return v;
            }
        }
        fromIndex = -1;
        return null;
    }
}

/// <summary>RT Beams Module beam item: the inherited control point view.</summary>
public sealed partial class BeamItem
{
    /// <summary>
    /// The control points with the values in force at each one (PS3.3 C.8.8.14.5: control points
    /// after the first hold only what changes): every property reads the nearest control point at
    /// or before this one that has the attribute. A sequence attribute comes whole from the nearest
    /// control point that has it; for Leaf / Jaw Positions per device use
    /// <see cref="ControlPointItem.EffectiveLeafJawPositions"/>. <see cref="ControlPointSequence"/>
    /// is the as-stored view.
    /// </summary>
    public IReadOnlyList<ControlPointItem> EffectiveControlPoints =>
        Composite.Inherited(Dataset, new DicomTag(0x300A0111u), static c => new ControlPointItem(c));
}

/// <summary>RT Ion Beams Module beam item: the inherited control point view.</summary>
public sealed partial class IonBeamItem
{
    /// <summary>Inherited view of the Ion Control Point Sequence (see <see cref="BeamItem.EffectiveControlPoints"/>).</summary>
    public IReadOnlyList<IonControlPointItem> EffectiveControlPoints =>
        Composite.Inherited(Dataset, new DicomTag(0x300A03A8u), static c => new IonControlPointItem(c));
}

/// <summary>RT Beams Session Record beam item: the inherited control point view.</summary>
public sealed partial class TreatmentSessionBeamItem
{
    /// <summary>Inherited view of the Control Point Delivery Sequence (see <see cref="BeamItem.EffectiveControlPoints"/>).</summary>
    public IReadOnlyList<ControlPointDeliveryItem> EffectiveControlPoints =>
        Composite.Inherited(Dataset, new DicomTag(0x30080040u), static c => new ControlPointDeliveryItem(c));
}

/// <summary>RT Ion Beams Session Record beam item: the inherited control point view.</summary>
public sealed partial class TreatmentSessionIonBeamItem
{
    /// <summary>Inherited view of the Ion Control Point Delivery Sequence (see <see cref="BeamItem.EffectiveControlPoints"/>).</summary>
    public IReadOnlyList<IonControlPointDeliveryItem> EffectiveControlPoints =>
        Composite.Inherited(Dataset, new DicomTag(0x30080041u), static c => new IonControlPointDeliveryItem(c));
}

/// <summary>Control point of an RT plan beam.</summary>
public sealed partial class ControlPointItem
{
    internal ControlPointItem(DicomDataset[] chain) : base(chain) { }

    /// <summary>
    /// Leaf / Jaw Positions (300A,011C) of the device type ("MLCX", "ASYMY", ...) in force at this
    /// control point: this one's, or (in the inherited view) the nearest earlier control point's
    /// that has that device. Null when none has it.
    /// </summary>
    /// <param name="deviceType">RT Beam Limiting Device Type, as stored.</param>
    /// <returns>The positions (mm), or null.</returns>
    public double[]? EffectiveLeafJawPositions(string deviceType) => Composite.Positions(Chain, deviceType, out _);
}

/// <summary>Control point of an RT Ion plan beam.</summary>
public sealed partial class IonControlPointItem
{
    internal IonControlPointItem(DicomDataset[] chain) : base(chain) { }

    /// <summary>See <see cref="ControlPointItem.EffectiveLeafJawPositions"/>.</summary>
    /// <param name="deviceType">RT Beam Limiting Device Type, as stored.</param>
    /// <returns>The positions (mm), or null.</returns>
    public double[]? EffectiveLeafJawPositions(string deviceType) => Composite.Positions(Chain, deviceType, out _);
}

/// <summary>Control point of a treatment record beam.</summary>
public sealed partial class ControlPointDeliveryItem
{
    internal ControlPointDeliveryItem(DicomDataset[] chain) : base(chain) { }

    /// <summary>See <see cref="ControlPointItem.EffectiveLeafJawPositions"/>.</summary>
    /// <param name="deviceType">RT Beam Limiting Device Type, as stored.</param>
    /// <returns>The positions (mm), or null.</returns>
    public double[]? EffectiveLeafJawPositions(string deviceType) => Composite.Positions(Chain, deviceType, out _);
}

/// <summary>Control point of an ion treatment record beam.</summary>
public sealed partial class IonControlPointDeliveryItem
{
    internal IonControlPointDeliveryItem(DicomDataset[] chain) : base(chain) { }

    /// <summary>See <see cref="ControlPointItem.EffectiveLeafJawPositions"/>.</summary>
    /// <param name="deviceType">RT Beam Limiting Device Type, as stored.</param>
    /// <returns>The positions (mm), or null.</returns>
    public double[]? EffectiveLeafJawPositions(string deviceType) => Composite.Positions(Chain, deviceType, out _);
}
