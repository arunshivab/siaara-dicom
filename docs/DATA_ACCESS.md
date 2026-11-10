# Getting data out of a DICOM object

The rule: **the toolkit returns DICOM data; applications do the calculations.** Whatever is in the
data set can be asked for by name and comes back exactly as stored (text decoded to UTF-8 with the
Specific Character Set). The toolkit interprets values only where the standard itself defines what
they mean:

| Interpretation | Standard |
|---|---|
| Modality LUT / Rescale Slope and Intercept (HU for CT), functional groups first | PS3.3 C.11.1, C.7.6.16.2.9 |
| RT Dose: stored value x Dose Grid Scaling, in Dose Units | PS3.3 C.8.8.3 |
| Waveform samples x Channel Sensitivity x Correction Factor + Channel Baseline; G.711 expansion of MB / AB | PS3.3 C.10.9.1.4.2, C.10.9.1.5 |
| Person name groups and components | PS3.5 6.2.1 |
| DA / TM / DT text to dates and times | PS3.5 Table 6.2-1 |
| RT control points after the first hold only the values that change | PS3.3 C.8.8.14.5, C.8.8.25.6 |
| NM frame k -> energy window, detector, phase, rotation, ... from the index vectors | PS3.3 C.8.4.8, C.8.4.9 |

There are no derived metrics (no SUV-, ROI-, DVH-, leaf-travel- or complexity-type values). A
missing attribute is reported as missing (null in C#, `SIAARA_ERR_NOT_FOUND` in C); nothing is
ever defaulted or fabricated. Data sets are never changed by reading.

There are four ways in, all over the same data:

1. **Generated classes** (C#): one typed class per PS3.3 module, macro and sequence item.
2. **Path API** (C, and every language through it): `"RTBeamSequence[0].ControlPointSequence[3].GantryAngle"`.
3. **Values at a point**: stored / modality value at a pixel, voxel at (i, j, k), value at a patient coordinate.
4. **Waveforms**: groups, channels, raw and scaled samples.

Plus the plain element API (`siaara_dataset_get_*`, `DicomDataset.GetString` ...) that everything
else is built on.

## 1. Generated classes (every module and macro)

`tools/gen_modules.py` reads the DocBook XML of PS3.3 (every table "Attribute Name | Tag | Type |
Description", nested '>' sequence rows, "Include Table ..." rows, the Annex A "Functional Group
Macros" tables) and PS3.6 (keyword, VR, VM). Nothing is chosen by hand, so nothing is forgotten.
From DICOM PS3.3 2026d it produces:

| | Count |
|---|---|
| Modules | 413 |
| Macros | 306 |
| Other attribute tables | 4 |
| Sequence item types | 967 |
| Distinct attributes | 4382 |
| Generated C# classes | 1691 (+ `DicomModules`) |
| Generated C# properties | 11909 typed + 622 `...Raw` (dates and times) |
| `Modules.g.cs` | 39.6 k lines, 2.9 MB; `Siaara.dll` 2.3 MB |

Two PS3.3 Include rows have no table to resolve ("Include any Private Attributes ...", DICOMDIR
"Record Selection Keys"); the items of the Shared / Per-Frame Functional Groups Sequences hold any
of the functional group macros, so their class includes every macro listed in the Annex A tables.

Regenerate after a new edition of the standard:

```
python3 -I tools/gen_modules.py part03.xml part06.xml          # writes the two generated files
python3 -I tools/gen_modules.py part03.xml part06.xml --check  # CI: ctest named_modules_match_generator
```

(the ctest runs when CMake is given `-DSIAARA_NEMA_PART03=... -DSIAARA_NEMA_PART06=...`).

### Using them

```csharp
using Siaara;
using Siaara.Modules;

using DicomFile f = DicomFile.Open("plan.dcm");
string? name   = f.Patient.Name?.Full;            // "Dr John Q Doe Jr"
string? first  = f.Patient.FirstName;             // given name
string? device = f.Equipment.MakeAndModel;        // "Acme Medical LinacX 3000"
BeamItem beam  = f.RTBeams.BeamSequence[0];
double? sad    = beam.SourceAxisDistance;
double[]? mlc  = beam.ControlPointSequence[0].BeamLimitingDevicePositionSequence[1].LeafJawPositions;
double? mu     = f.RTFractionScheme.FractionGroupSequence[0].ReferencedBeamSequence[0].BeamMeterset;
var any        = f.Modules.RTIonBeams;            // every module: f.Modules.<ModuleName>
bool present   = f.IsModulePresent("RT Beams");   // name, "C.8-50", or class name
```

* `file.Modules.<Name>` gives every module and macro (`Module` suffix dropped: `Modules.RTBeams`,
  `Modules.CodeSequenceMacro`); `dataset.Modules` does the same for any sequence item.
* Convenience roots on `DicomFile`: `Patient`, `Study`, `Series`, `Equipment`, `EnhancedEquipment`,
  `FrameOfReference`, `Image`, `ImagePlane`, `ImagePixel`, `SOPCommon`, `CTImage`, `MRImage`,
  `ModalityLut`, `VoiLut`, `RTPlan`, `RTFractionScheme`, `RTBeams`, `RTIonBeams`, `RTPrescription`,
  `RTToleranceTables`, `RTPatientSetup`, `RTDose`, `RTDvh`, `RTStructureSet`, `RTRoiContour`,
  `RTRoiObservations`, `RTImage`, `RTTreatmentRecord`, `RTBeamsTreatmentRecord`,
  `RTIonBeamsTreatmentRecord`, `RTBrachyTreatmentRecord`, `RTBrachyApplicationSetups`, `NMImage`,
  `NMMultiFrame`, `NMDetector`, `NMIsotope`, `EnergyWindows`, `NMTomoAcquisition`, `NMGating`,
  `NMPhase`, `NMReconstruction`, `Cardiac`, `Respiratory`, `PETSeries`, `PETIsotope`, `PETImage`,
  `PETGating`, `XAPositioner`, `XAAcquisition`, `Mask`, `USRegionCalibration`, `USRegions`,
  `USImage`, `Waveform`, `Waveforms`, `FunctionalGroups`, `Dimensions`, `EnhancedCTImage`,
  `EnhancedMRImage`, `SRDocument`, `SR`, `Segmentation`, `MG`, `DXImage`, `Ophthalmic`,
  `OphthalmicTomography`.
* Each class has `TableId` (the PS3.3 table), `IsPresent`, `Dataset`, an indexer for any tag
  (text), and one property per attribute in keyword PascalCase, with the tag, VR, VM and Type in
  its documentation.

Property types:

| VR | Type (VM 1) | Type (VM > 1) |
|---|---|---|
| AE AS CS LO LT SH ST UC UI UR UT | `string?` (charset-decoded, trailing padding removed) | `string[]?` |
| PN | `PersonName?` | `PersonName[]?` |
| DS FL FD | `double?` | `double[]?` |
| IS US SS UL SL SV UV | `long?` | `long[]?` |
| DA / TM / DT | `DateOnly?` / `TimeOnly?` / `DateTime?`, plus `<Keyword>Raw` (`string?`, as stored) | arrays of nullable |
| AT | `DicomTag?` | `DicomTag[]?` |
| OB OW OF OD OL OV UN, "OB or OW" | `byte[]?` (little endian) | |
| SQ | `IReadOnlyList<ItemClass>` (empty when absent) | |

A value that is present but cannot be held by the typed form (a malformed number or date) gives
null in the typed property; the indexer / `...Raw` property still has the stored text. DA needs
YYYYMMDD; TM accepts HH, HHMM, HHMMSS[.F] (omitted parts are zero in the `TimeOnly`, the text keeps
the precision); DT needs at least the full date and does not apply a UTC offset (it stays in the
text). Repeating-group attributes (60xx overlays) are read from group 6000; use the path API for the
others.

### Composites

Hand-written, documented, each only joins or selects stored values:

| Composite | Value |
|---|---|
| `PersonName.Family / Given / Middle / Prefix / Suffix` | Components of the alphabetic group, as stored (null when empty) |
| `PersonName.First` / `.Last` | `Given` / `Family` |
| `PersonName.Full`, `PersonNameGroup.Full` | Present components in the order prefix, given, middle, family, suffix, joined by one space |
| `PersonName.Ideographic`, `.Phonetic` | The other component groups, with the same members |
| `PatientModule.Name`, `.FirstName`, `.LastName`, `.FullName` | Patient's Name as above |
| `GeneralEquipmentModule.MakeAndModel`, `EnhancedGeneralEquipmentModule.MakeAndModel` | Manufacturer + " " + Manufacturer's Model Name, each as stored; the one present when only one is |
| `GeneralEquipmentModule.SoftwareVersionsText` | Software Versions values joined by one space |
| `GeneralStudyModule.StudyDateTime`, `GeneralSeriesModule.SeriesDateTime`, `GeneralImageModule.ContentDateTime`, `GeneralAcquisitionModule.AcquisitionDateAndTime` | Date + time; null unless both are present and well formed |
| `GeneralStudyModule.ReferringPhysician` | Referring Physician's Name |
| `BeamItem.EffectiveControlPoints` (also `IonBeamItem`, `TreatmentSessionBeamItem`, `TreatmentSessionIonBeamItem`) | Control points with every attribute read from the nearest control point at or before it that has it (PS3.3 rule). `ControlPointSequence` stays the as-stored view |
| `ControlPointItem.EffectiveLeafJawPositions("MLCX")` (and the Ion / delivery items) | Leaf / Jaw Positions of that device in force at the control point |

`ModuleBase.SourceOf(tag)` tells which data set (which control point) a value came from.

### Listing modules and attributes (C and C#)

```c
uint32_t m; siaara_module_find("RT Beams", 8, &m);
siaara_module_info mi = { sizeof mi }; siaara_module_get(m, &mi);        /* name, table id, class */
siaara_module_attribute a = { sizeof a }; siaara_module_attribute_get(m, 0, &a);  /* BeamSequence, item_module */
int32_t present; siaara_module_present(file, "RT Beams", 8, &present);
```

C#: `DicomModules.All`, `DicomModules.Find("RT Beams").Attributes`, `file.IsModulePresent(...)`.
A module is present when at least one of its attributes (Include rows expanded) is in the data set.

## 2. Path API (C, every language)

```
path    := segment ('.' segment)*
segment := name ['[' index ']']                0-based
name    := Keyword | (gggg,eeee) | gggg,eeee | creator:gggg,xx | "creator":gggg,xx
```

* A sequence that is not the last segment needs an item index (`BeamSequence[0].BeamName`).
* On the last segment, an index selects an item of a sequence, or one value of a multi-valued
  element (`ImagePositionPatient[2]`, `LeafJawPositions[40]`).
* Private attributes: `"SIEMENS CSA HEADER":0029,10` finds the Private Creator element
  (0029,00bb) whose value is the creator in the data set being walked and reads (0029,bb10). An
  unquoted creator cannot contain `.`, `[` or `]`.

```c
siaara_path_value v = { sizeof v };
char text[256]; size_t need;
siaara_get_by_path(ds, "BeamSequence[0].ControlPointSequence[0].GantryAngle", len, &v, text, sizeof text, &need);
/* v.kind = SIAARA_VALUE_REAL, v.real = 180, text "180", v.vr "DS", v.vm 1 */
double pos[160]; size_t n;
siaara_get_doubles_by_path(ds, "BeamSequence[0].ControlPointSequence[0]"
                               ".BeamLimitingDevicePositionSequence[1].LeafJawPositions", len, pos, 160, &n);
siaara_dataset* cp; siaara_dataset_by_path(ds, "BeamSequence[0].ControlPointSequence[3]", len, &cp);
```

The text form is all values backslash separated (or the selected one), decoded with the Specific
Character Set in force along the path; numbers as stored. The typed form gives the selected (or
first) value as `integer` / `real`, with `kind`, `vr`, `vm`, `item_count` and `byte_length`. A
missing element fails with `SIAARA_ERR_NOT_FOUND` and a message naming the segment. C#:
`dataset.GetByPath(path)` (null when absent), `dataset.GetDoublesByPath(path)`.

## 3. Values at a point

| Question | C | C# |
|---|---|---|
| Stored value and HU / modality value at (column, row) of frame f | `siaara_file_pixel_value(file, f, col, row, sample, &v)` (decodes the frame) or `siaara_frame_pixel_value(frame, col, row, sample, &v)` | `file.GetPixelValue(f, col, row)`, `frame.GetPixelValue(col, row)` |
| Voxel (i, j, k) of a volume | `siaara_volume_voxel(vol, i, j, k, &value)` | `volume.GetVoxel(i, j, k)` |
| Value at patient (x, y, z) mm | `siaara_volume_value_at(vol, xyz, SIAARA_INTERP_NEAREST, &s)` | `volume.ValueAt(x, y, z)` |

`siaara_pixel_value` gives the stored value (Pixel Representation applied), the modality value
(Modality LUT, or Rescale Slope / Intercept, per frame from the functional groups first; RT Dose:
Dose Grid Scaling), the slope and intercept used, and the units with their source: Rescale Type,
Modality LUT Type, Dose Units, PET Units, or HU for CT without Rescale Type (CT Image Module:
Rescale Type is required only when the units are not HU). Colour samples and float pixel data have
no modality value. Volumes hold modality values; the patient-coordinate call takes the nearest voxel
unless trilinear interpolation is asked for, and reports the continuous index, the nearest voxel
index and whether the point is inside (outside: `inside = 0`, no value).

## 4. Waveforms (ECG, haemodynamic, audio, ...)

```c
uint32_t groups; siaara_waveform_group_count(ds, &groups);
siaara_waveform_group g = { sizeof g };   siaara_waveform_group_info(ds, 0, &g);       /* channels, samples, Hz, SS ... */
siaara_waveform_channel c = { sizeof c }; siaara_waveform_channel_info(ds, 0, 0, &c);  /* sensitivity, correction, baseline, skew, filters */
siaara_waveform_channel_string(ds, 0, 0, SIAARA_WAVEFORM_LABEL, buf, cap, &need);     /* label, status, units, source codes */
siaara_waveform_raw_samples(ds, 0, 0, raw, cap, &n);                                   /* stored values */
siaara_waveform_samples(ds, 0, 0, 1, scaled, cap, &n);                                 /* in Channel Sensitivity Units */
```

Scaled = sample x Channel Sensitivity x Channel Sensitivity Correction Factor (1 when absent) +
Channel Baseline (0 when absent); MB / AB codes are first expanded per ITU-T G.711 (14-bit mu-law,
13-bit A-law decoder outputs); samples equal to the Waveform Padding Value are NaN. A channel
without Channel Sensitivity has arbitrary units: its scaled samples are not given
(`SIAARA_ERR_NOT_FOUND`; C# `ScaledSamples()` returns null). C#: `file.Waveforms[g].Channels[c]`.
The full Waveform Module (multiplex group label, channel source modifiers, annotations) is in the
generated `WaveformModule` / `WaveformAnnotationModule` classes.

## RT, NM and the rest

Everything below is reachable through the generated classes and the path API; the C helpers exist
where the standard defines a rule beyond "read the attribute":

* **RT Plan / Ion Plan**: fraction groups (`RTFractionScheme.FractionGroupSequence[].ReferencedBeamSequence[].BeamMeterset`),
  beams (number, name, type, radiation type, machine, SAD, dose rate and energy in the control points),
  Beam Limiting Device definitions (type, number of pairs, Leaf Position Boundaries), control points
  (gantry / collimator (`BeamLimitingDeviceAngle`) / couch angles and directions, isocenter, cumulative
  meterset weight, Beam Limiting Device Position Sequence), wedges, blocks, applicators, compensators.
  C: `siaara_rt_control_point_source(beam, cp, tag, &from)` and
  `siaara_rt_device_positions(beam, cp, "MLCX", 4, effective, buf, cap, &n, &from)` apply the
  PS3.3 inheritance rule; with `effective = 0` they give only what control point cp stores.
* **Treatment records** (RT Beams / Ion / Brachy Session Record modules): the same through
  `TreatmentSession(Ion)BeamItem` and the control point delivery sequences.
* **Brachy**: `RTBrachyApplicationSetups.ApplicationSetupSequence[].ChannelSequence[].BrachyControlPointSequence[]`
  (relative position, 3D position, cumulative time weight), source and applicator attributes.
* **NM**: `NMIsotope.EnergyWindowInformationSequence[].EnergyWindowRangeSequence[]` (lower / upper),
  radiopharmaceuticals (`RadiopharmaceuticalInformationSequence`: radionuclide code, route, start
  date-time, total dose, half-life, positron fraction), `NMMultiFrame.FrameIncrementPointer` and the
  index vectors, `NMGating.GatedInformationSequence[].DataInformationSequence[]`, Number of Time
  Slots. Frame k -> vectors: `siaara_nm_frame_index_get(ds, k, &x)` / `file.GetNMFrameIndex(k)`
  (1-based values as stored; 0 / null when a vector is absent or shorter than k).
* **PET**: `PETIsotope`, `PETImage`, `PETSeries`.
* **RT Image**: label, plane, SID, gantry / collimator / couch, `ExposureSequence`, referenced plan
  and beam (`RTImage`).
* **XA / US**: `XAPositioner` (primary / secondary angles), `Mask.MaskSubtractionSequence`,
  `USRegions` (including the Doppler fields).
* **Enhanced objects**: `FunctionalGroups.SharedFunctionalGroupsSequence[0]` /
  `PerFrameFunctionalGroupsSequence[k]` items expose every functional group macro.
* **SR, Segmentation, MG, ophthalmology** and every other module: `file.Modules.<Name>`.

### Lookups

| | C | C# |
|---|---|---|
| SOP Class UID -> name, IOD, object kind | `siaara_sop_class_info_get` (`SIAARA_OBJECT_*` from the IOD's modules) | `SopClassInfo.Get(uid)` |
| Transfer syntax -> name, explicit VR, endianness, encapsulated, lossy, deflated | `siaara_transfer_syntax_info_get` | `TransferSyntaxInfo.Get(uid)` |
| Dictionary enumerate / search by name, keyword or VR | `siaara_dict_count`, `siaara_dict_entry_at`, `siaara_dict_search` | `DicomDictionary.Count`, `EntryAt`, `Search` |

Private attributes are resolved through their Private Creator block today; when the private
dictionary (branch wip-ptags) is merged it will also supply their names and VRs.
