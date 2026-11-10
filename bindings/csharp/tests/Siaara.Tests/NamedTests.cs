using System;
using System.Linq;
using System.Runtime.InteropServices;
using Siaara.Interop;
using Siaara.Modules;
using Siaara.Volumes;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>Named data layer: generated module classes, composites, path API, values at a point, waveforms.</summary>
internal static class NamedTests
{
    private static DicomFile RtPlan()
    {
        DicomFile f = DicomFile.Create();
        DicomDataset ds = f.Dataset;
        ds.SetRawString(DicomTags.SpecificCharacterSet, "CS", "ISO_IR 192");
        ds.SetRawString(DicomTags.SOPClassUID, "UI", "1.2.840.10008.5.1.4.1.1.481.5");
        ds.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.77");
        ds.SetString(DicomTags.Modality, "RTPLAN");
        ds.SetString(DicomTags.PatientName, "Yamada^Tarou^M^Dr^III=山田^太郎=やまだ^たろう");
        ds.SetString(DicomTags.Manufacturer, "Acme  Medical");
        ds.SetString(DicomTags.ManufacturerModelName, "LinacX 3000");
        ds.SetString(DicomTags.StudyDate, "20260301");
        ds.SetString(DicomTags.StudyTime, "101530.25");
        DicomDataset fg = ds.AppendItem(DicomTags.FractionGroupSequence);
        fg.SetString(DicomTags.FractionGroupNumber, "1");
        fg.SetString(DicomTags.NumberOfFractionsPlanned, "30");
        DicomDataset rb = fg.AppendItem(DicomTags.ReferencedBeamSequence);
        rb.SetString(DicomTags.ReferencedBeamNumber, "1");
        rb.SetString(DicomTags.BeamMeterset, "123.4");
        DicomDataset beam = ds.AppendItem(DicomTags.BeamSequence);
        beam.SetString(DicomTags.BeamNumber, "1");
        beam.SetString(DicomTags.BeamName, "AP");
        beam.SetString(DicomTags.BeamType, "DYNAMIC");
        beam.SetString(DicomTags.RadiationType, "PHOTON");
        beam.SetString(DicomTags.TreatmentMachineName, "TrueBeam1");
        beam.SetString(DicomTags.SourceAxisDistance, "1000");
        DicomDataset bld = beam.AppendItem(DicomTags.BeamLimitingDeviceSequence);
        bld.SetString(DicomTags.RTBeamLimitingDeviceType, "MLCX");
        bld.SetString(DicomTags.NumberOfLeafJawPairs, "2");
        bld.SetString(DicomTags.LeafPositionBoundaries, "-10\\0\\10");
        DicomDataset cp0 = beam.AppendItem(DicomTags.ControlPointSequence);
        cp0.SetString(DicomTags.ControlPointIndex, "0");
        cp0.SetString(DicomTags.NominalBeamEnergy, "6");
        cp0.SetString(DicomTags.GantryAngle, "180");
        cp0.SetString(DicomTags.GantryRotationDirection, "CW");
        cp0.SetString(DicomTags.IsocenterPosition, "0\\0\\0");
        cp0.SetString(DicomTags.CumulativeMetersetWeight, "0");
        DicomDataset p0 = cp0.AppendItem(DicomTags.BeamLimitingDevicePositionSequence);
        p0.SetString(DicomTags.RTBeamLimitingDeviceType, "ASYMY");
        p0.SetString(DicomTags.LeafJawPositions, "-50\\50");
        DicomDataset p1 = cp0.AppendItem(DicomTags.BeamLimitingDevicePositionSequence);
        p1.SetString(DicomTags.RTBeamLimitingDeviceType, "MLCX");
        p1.SetString(DicomTags.LeafJawPositions, "-5\\-4\\5\\6");
        DicomDataset cp1 = beam.AppendItem(DicomTags.ControlPointSequence);
        cp1.SetString(DicomTags.ControlPointIndex, "1");
        cp1.SetString(DicomTags.GantryAngle, "190");
        cp1.SetString(DicomTags.CumulativeMetersetWeight, "0.5");
        DicomDataset q1 = cp1.AppendItem(DicomTags.BeamLimitingDevicePositionSequence);
        q1.SetString(DicomTags.RTBeamLimitingDeviceType, "MLCX");
        q1.SetString(DicomTags.LeafJawPositions, "-3\\-2\\3\\4");
        DicomDataset cp2 = beam.AppendItem(DicomTags.ControlPointSequence);
        cp2.SetString(DicomTags.ControlPointIndex, "2");
        cp2.SetString(DicomTags.CumulativeMetersetWeight, "1");
        beam.SetString(new DicomTag(0x300B0010u), "LO", "ACMERT");
        beam.SetString(new DicomTag(0x300B1001u), "LO", "private value");
        // Brachy: Application Setup > Channel > Brachy Control Point.
        DicomDataset setup = ds.AppendItem(DicomTags.ApplicationSetupSequence);
        setup.SetString(DicomTags.ApplicationSetupNumber, "1");
        DicomDataset ch = setup.AppendItem(DicomTags.ChannelSequence);
        ch.SetString(DicomTags.ChannelNumber, "1");
        ch.SetString(DicomTags.SourceApplicatorID, "APP1");
        DicomDataset bcp = ch.AppendItem(DicomTags.BrachyControlPointSequence);
        bcp.SetString(DicomTags.ControlPointRelativePosition, "15.5");
        bcp.SetString(DicomTags.CumulativeTimeWeight, "0.25");
        bcp.SetString(DicomTags.ControlPoint3DPosition, "1\\2\\3");
        return f;
    }

    internal static void Register()
    {
        Case("named: Patient.Name parts with all component groups", () =>
        {
            using DicomFile f = RtPlan();
            PersonName? n = f.Patient.Name;
            Require(n is not null);
            CheckEq(n!.Family, "Yamada");
            CheckEq(n.Given, "Tarou");
            CheckEq(n.Middle, "M");
            CheckEq(n.Prefix, "Dr");
            CheckEq(n.Suffix, "III");
            CheckEq(n.First, "Tarou");
            CheckEq(n.Last, "Yamada");
            CheckEq(n.Full, "Dr Tarou M Yamada III");
            CheckEq(f.Patient.FirstName, "Tarou");
            CheckEq(f.Patient.LastName, "Yamada");
            CheckEq(f.Patient.FullName, "Dr Tarou M Yamada III");
            CheckEq(n.Ideographic?.Family, "山田");
            CheckEq(n.Ideographic?.Given, "太郎");
            CheckEq(n.Phonetic?.Family, "やまだ");
            CheckEq(n.Phonetic?.Full, "たろう やまだ");
            CheckEq(n.Raw, "Yamada^Tarou^M^Dr^III=山田^太郎=やまだ^たろう");
            var only = new PersonName("Doe");
            CheckEq(only.Family, "Doe");
            Check(only.Given is null);
            Check(only.Ideographic is null);
        });

        Case("named: Equipment.MakeAndModel keeps the stored text", () =>
        {
            using DicomFile f = RtPlan();
            CheckEq(f.Equipment.MakeAndModel, "Acme  Medical LinacX 3000");
            using DicomFile empty = DicomFile.Create();
            Check(empty.Equipment.MakeAndModel is null);
            empty.Dataset.SetString(DicomTags.Manufacturer, "Acme");
            CheckEq(empty.Equipment.MakeAndModel, "Acme");
        });

        Case("named: missing attributes give null, dates keep their raw text", () =>
        {
            using DicomFile f = RtPlan();
            Check(f.Patient.PatientBirthDate is null);
            Check(f.Patient.PatientBirthDateRaw is null);
            Check(f.Patient.PatientSex is null);
            Check(f.RTDose.DoseGridScaling is null);
            CheckEq(f.RTDose.IsPresent, false);
            CheckEq(f.RTBeams.IsPresent, true);
            CheckEq(f.Study.StudyDate, new DateOnly(2026, 3, 1));
            CheckEq(f.Study.StudyDateRaw, "20260301");
            CheckEq(f.Study.StudyTime, new TimeOnly(10, 15, 30, 250));
            CheckEq(f.Study.StudyDateTime, new DateTime(2026, 3, 1, 10, 15, 30, 250));
            Check(DicomDateTimeText.ParseDateTime("2026") is null);
            CheckEq(DicomDateTimeText.ParseDateTime("20260301101530+0530"), new DateTime(2026, 3, 1, 10, 15, 30));
            Check(DicomDateTimeText.ParseDate("2026.03.01") is null);
            CheckEq(f.RTBrachyApplicationSetups.ApplicationSetupSequence.Count, 1);
            CheckEq(f.Waveforms.Count, 0);
            Check(f.Dataset.GetByPath("BeamSequence[0].GantryAngle") is null);
        });

        Case("named: path API nested sequences, indices, private tag", () =>
        {
            using DicomFile f = RtPlan();
            DicomPathValue? v = f.Dataset.GetByPath("BeamSequence[0].ControlPointSequence[0].BeamLimitingDevicePositionSequence[1].LeafJawPositions");
            Require(v is not null);
            CheckEq(v!.Text, "-5\\-4\\5\\6");
            CheckEq(v.Kind, DicomValueKind.RealNumber);
            CheckEq(v.VM, 4);
            CheckEq(v.RealValue, -5.0);
            CheckEq(f.Dataset.GetByPath("BeamSequence[0].ControlPointSequence[0].BeamLimitingDevicePositionSequence[1].LeafJawPositions[3]")!.Text, "6");
            CheckEq(f.Dataset.GetByPath("(300A,00B0)[0].(300A,00C0)")!.IntegerValue, 1L);
            DicomPathValue? p = f.Dataset.GetByPath("BeamSequence[0].ACMERT:300B,01");
            CheckEq(p?.Text, "private value");
            CheckEq(p?.Tag, new DicomTag(0x300B1001u));
            Check(f.Dataset.GetByPath("BeamSequence[0].OTHER:300B,01") is null);
            Check(f.Dataset.GetByPath("BeamSequence[4].BeamName") is null);
            Throws<SiaaraException>(() => f.Dataset.GetByPath("BeamSequence.BeamName"));
            Check(f.Dataset.GetDoublesByPath("FractionGroupSequence[0].ReferencedBeamSequence[0].BeamMeterset")!.SequenceEqual(new[] { 123.4 }));
            CheckEq(f.Dataset.GetByPath("BeamSequence")!.ItemCount, 1);
        });

        Case("named: RT plan beams, MLC leaf positions, effective control points", () =>
        {
            using DicomFile f = RtPlan();
            FractionGroupItem fg = f.RTFractionScheme.FractionGroupSequence[0];
            CheckEq(fg.NumberOfFractionsPlanned, 30L);
            CheckEq(fg.ReferencedBeamSequence[0].BeamMeterset, 123.4);
            BeamItem b = f.RTBeams.BeamSequence[0];
            CheckEq(b.BeamNumber, 1L);
            CheckEq(b.BeamName, "AP");
            CheckEq(b.BeamType, "DYNAMIC");
            CheckEq(b.RadiationType, "PHOTON");
            CheckEq(b.TreatmentMachineName, "TrueBeam1");
            CheckEq(b.SourceAxisDistance, 1000.0);
            CheckEq(b.BeamLimitingDeviceSequence[0].RTBeamLimitingDeviceType, "MLCX");
            CheckEq(b.BeamLimitingDeviceSequence[0].NumberOfLeafJawPairs, 2L);
            Check(b.BeamLimitingDeviceSequence[0].LeafPositionBoundaries!.SequenceEqual(new double[] { -10, 0, 10 }));
            var stored = b.ControlPointSequence;
            CheckEq(stored.Count, 3);
            Check(stored[0].BeamLimitingDevicePositionSequence[1].LeafJawPositions!.SequenceEqual(new double[] { -5, -4, 5, 6 }));
            Check(stored[0].IsocenterPosition!.SequenceEqual(new double[] { 0, 0, 0 }));
            Check(stored[2].GantryAngle is null);  // as stored
            var eff = b.EffectiveControlPoints;
            CheckEq(eff[2].GantryAngle, 190.0);
            CheckEq(eff[2].GantryRotationDirection, "CW");
            CheckEq(eff[2].NominalBeamEnergy, 6.0);
            CheckEq(eff[2].CumulativeMetersetWeight, 1.0);
            Check(eff[2].SourceOf(DicomTags.GantryAngle)?.ItemIndex == 1);
            Check(eff[2].EffectiveLeafJawPositions("MLCX")!.SequenceEqual(new double[] { -3, -2, 3, 4 }));
            Check(eff[2].EffectiveLeafJawPositions("ASYMY")!.SequenceEqual(new double[] { -50, 50 }));
            Check(stored[2].EffectiveLeafJawPositions("MLCX") is null);
            Check(eff[2].BeamLimitingDeviceAngle is null);  // never given
            // Brachy channel and dwell control point.
            ChannelItem ch = f.RTBrachyApplicationSetups.ApplicationSetupSequence[0].ChannelSequence[0];
            CheckEq(ch.SourceApplicatorID, "APP1");
            CheckEq(ch.BrachyControlPointSequence[0].CumulativeTimeWeight, 0.25);
            Check(ch.BrachyControlPointSequence[0].ControlPoint3DPosition!.SequenceEqual(new double[] { 1, 2, 3 }));
        });

        Case("named: module listing and presence", () =>
        {
            ModuleInfo? m = DicomModules.Find("RT Beams");
            Require(m is not null);
            CheckEq(m!.Name, "RT Beams Module");
            CheckEq(m.ClassName, nameof(RTBeamsModule));
            CheckEq(m.Attributes[0].Keyword, "BeamSequence");
            Check(DicomModules.All.Count > 1500);
            Check(DicomModules.Find("nope") is null);
            using DicomFile f = RtPlan();
            Check(f.IsModulePresent("RT Beams"));
            Check(!f.IsModulePresent("RT Dose"));
            Throws<SiaaraException>(() => f.IsModulePresent("nope"));
        });

        Case("named: HU at a pixel equals the rescale formula", () =>
        {
            using DicomFile f = Builders.Ct(2, 3, new short[] { 0, 10, 20, 1024, 1100, -5 });
            PixelValue v = f.GetPixelValue(0, 1, 1);
            CheckEq(v.Stored, 1100L);
            CheckEq(v.ModalityValue, 1100.0 * 1 - 1024);
            CheckEq(v.Units, "HU");
            CheckEq(v.UnitsSource, UnitsSource.CtHounsfield);
            using DicomFrame fr = DicomFrame.Open(f);
            CheckEq(fr.GetPixelValue(2, 1).ModalityValue, -5.0 - 1024);
            Throws<SiaaraException>(() => f.GetPixelValue(0, 3, 0));
        });

        Case("named: voxel at (25,20,12) and value at patient mm", () =>
        {
            string dir = NewTempDir("named_vol");
            string[] paths = Builders.CtSeries(dir, 16, 24, 32, (i, j, k) => (short)(i + 40 * j + 1000 * k));
            using Volume v = Volume.Build(paths);
            CheckEq(v.GetVoxel(25, 20, 12), 25.0 + 40 * 20 + 1000 * 12);
            VolumeSample s = v.ValueAt(25.3, 19.8, 24.7);
            Check(s.Inside);
            CheckEq((s.VoxelI, s.VoxelJ, s.VoxelK), (25L, 20L, 12L));
            CheckEq(s.Value, 25.0 + 40 * 20 + 1000 * 12);
            VolumeSample t = v.ValueAt(25.5, 20, 24, trilinear: true);
            CheckNear(t.Value ?? double.NaN, 25.5 + 40 * 20 + 1000 * 12, 1e-3);
            VolumeSample o = v.ValueAt(-10, 0, 0);
            Check(!o.Inside);
            Check(o.Value is null);
        });

        Case("named: NM energy windows and frame index", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.Modality, "NM");
            DicomDataset w = ds.AppendItem(DicomTags.EnergyWindowInformationSequence);
            w.SetString(DicomTags.EnergyWindowName, "TC99M");
            DicomDataset r = w.AppendItem(DicomTags.EnergyWindowRangeSequence);
            r.SetString(DicomTags.EnergyWindowLowerLimit, "126.45");
            r.SetString(DicomTags.EnergyWindowUpperLimit, "154.55");
            ds.SetBytes(DicomTags.FrameIncrementPointer, "AT", new byte[] { 0x54, 0, 0x10, 0, 0x54, 0, 0x20, 0 });
            ds.SetBytes(DicomTags.EnergyWindowVector, "US", MemoryMarshal.AsBytes(new ushort[] { 1, 1, 2, 2 }.AsSpan()));
            ds.SetBytes(DicomTags.DetectorVector, "US", MemoryMarshal.AsBytes(new ushort[] { 1, 2, 1, 2 }.AsSpan()));
            DicomDataset gi = ds.AppendItem(DicomTags.GatedInformationSequence);
            gi.SetString(DicomTags.TriggerTime, "0");
            DicomDataset di = gi.AppendItem(DicomTags.DataInformationSequence);
            di.SetString(DicomTags.FrameTime, "50");
            di.SetString(DicomTags.IntervalsAcquired, "400");
            ds.SetUInt16(DicomTags.NumberOfTimeSlots, 16);
            EnergyWindowInformationItem e = f.EnergyWindows[0];
            CheckEq(e.EnergyWindowName, "TC99M");
            CheckEq(e.EnergyWindowRangeSequence[0].EnergyWindowLowerLimit, 126.45);
            CheckEq(e.EnergyWindowRangeSequence[0].EnergyWindowUpperLimit, 154.55);
            Check(f.NMMultiFrame.FrameIncrementPointer!.SequenceEqual(new[] { new DicomTag(0x00540010u), new DicomTag(0x00540020u) }));
            CheckEq(f.NMGating.GatedInformationSequence[0].DataInformationSequence[0].IntervalsAcquired, 400L);
            CheckEq(f.NMMultiFrame.NumberOfTimeSlots, 16L);
            NMFrameIndex x = f.GetNMFrameIndex(2);
            CheckEq(x.EnergyWindow, 2);
            CheckEq(x.Detector, 1);
            Check(x.Rotation is null);
            CheckEq(x.InFrameIncrementPointer, (uint)(C.SIAARA_NM_ENERGY_WINDOW | C.SIAARA_NM_DETECTOR));
        });

        Case("named: ECG waveform scaled values", () =>
        {
            using DicomFile f = DicomFile.Create();
            DicomDataset ds = f.Dataset;
            ds.SetString(DicomTags.Modality, "ECG");
            DicomDataset g = ds.AppendItem(DicomTags.WaveformSequence);
            g.SetUInt16(DicomTags.NumberOfWaveformChannels, 2);
            g.SetUInt32(DicomTags.NumberOfWaveformSamples, 3);
            g.SetString(DicomTags.SamplingFrequency, "500");
            g.SetUInt16(DicomTags.WaveformBitsAllocated, 16);
            g.SetString(DicomTags.WaveformSampleInterpretation, "SS");
            DicomDataset c1 = g.AppendItem(DicomTags.ChannelDefinitionSequence);
            c1.SetString(DicomTags.ChannelLabel, "Lead I");
            c1.SetString(DicomTags.ChannelSensitivity, "2.5");
            c1.SetString(DicomTags.ChannelSensitivityCorrectionFactor, "1.1");
            c1.SetString(DicomTags.ChannelBaseline, "-3");
            c1.SetUInt16(DicomTags.WaveformBitsStored, 16);
            DicomDataset u = c1.AppendItem(DicomTags.ChannelSensitivityUnitsSequence);
            u.SetString(DicomTags.CodeValue, "uV");
            u.SetString(DicomTags.CodingSchemeDesignator, "UCUM");
            u.SetString(DicomTags.CodeMeaning, "microvolt");
            DicomDataset c2 = g.AppendItem(DicomTags.ChannelDefinitionSequence);
            c2.SetString(DicomTags.ChannelLabel, "Marker");
            c2.SetUInt16(DicomTags.WaveformBitsStored, 16);
            g.SetBytes(DicomTags.WaveformData, "OW", MemoryMarshal.AsBytes(new short[] { 10, 1, -20, 2, 30, 3 }.AsSpan()));
            WaveformGroup wg = f.Waveforms[0];
            CheckEq(wg.ChannelCount, 2);
            CheckEq(wg.DataSamples, 3);
            CheckEq(wg.SamplingFrequency, 500.0);
            CheckEq(wg.SampleInterpretation, "SS");
            WaveformChannel ch = wg.Channels[0];
            CheckEq(ch.Label, "Lead I");
            CheckEq(ch.UnitsCode, "uV");
            Check(ch.RawSamples().SequenceEqual(new long[] { 10, -20, 30 }));
            double[] sc = ch.ScaledSamples()!;
            CheckNear(sc[0], 10 * 2.5 * 1.1 - 3, 1e-9);
            CheckNear(sc[1], -20 * 2.5 * 1.1 - 3, 1e-9);
            CheckNear(sc[2], 30 * 2.5 * 1.1 - 3, 1e-9);
            Check(wg.Channels[1].ScaledSamples() is null);
            Check(wg.Channels[1].RawSamples().SequenceEqual(new long[] { 1, 2, 3 }));
            CheckEq(f.Waveform.WaveformSequence[0].ChannelDefinitionSequence[0].ChannelSensitivity, 2.5);
        });

        Case("named: SOP class, transfer syntax and dictionary search", () =>
        {
            SopClassInfo? s = SopClassInfo.Get("1.2.840.10008.5.1.4.1.1.481.5");
            CheckEq(s?.IodName, "RT Plan");
            CheckEq(s?.ObjectKind, C.SIAARA_OBJECT_RT_PLAN);
            Check(SopClassInfo.Get("1.2.3") is null);
            TransferSyntaxInfo? t = TransferSyntaxInfo.Get("1.2.840.10008.1.2.4.50");
            Check(t is { Lossy: true, Encapsulated: true, ExplicitVR: true });
            Check(DicomDictionary.Count > 4000);
            Check(DicomDictionary.Search("leaf/jaw pos").Any(e => e.Keyword == "LeafJawPositions"));
            Check(DicomDictionary.Search("", "PN").Count > 20);
        });
    }
}
