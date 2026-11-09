// SIAARA DICOM Toolkit - shared helpers for the clinical-object layer (src/clinical).
// Attribute tags are those of PS3.6; value parsing follows PS3.5 6.2 (DS, IS, DA, TM, DT).
// No exceptions other than std::bad_alloc; nothing here logs or returns PHI in messages.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::clinical {

namespace tags {
// identification / references
constexpr Tag InstanceCreationDate = 0x00080012, InstanceCreationTime = 0x00080013;
constexpr Tag StudyDate = 0x00080020, SeriesDate = 0x00080021, AcquisitionDate = 0x00080022, ContentDate = 0x00080023;
constexpr Tag AcquisitionDateTime = 0x0008002A;
constexpr Tag StudyTime = 0x00080030, SeriesTime = 0x00080031, AcquisitionTime = 0x00080032, ContentTime = 0x00080033;
constexpr Tag AccessionNumber = 0x00080050, Manufacturer = 0x00080070, ReferringPhysicianName = 0x00080090;
constexpr Tag CodeValue = 0x00080100, CodingSchemeDesignator = 0x00080102, CodingSchemeVersion = 0x00080103;
constexpr Tag CodeMeaning = 0x00080104, MappingResource = 0x00080105, LongCodeValue = 0x00080119, UrnCodeValue = 0x00080120;
constexpr Tag StudyDescription = 0x00081030, SeriesDescription = 0x0008103E;
constexpr Tag ReferencedPerformedProcedureStepSequence = 0x00081111;
constexpr Tag ReferencedSeriesSequence = 0x00081115, ReferencedImageSequence = 0x00081140;
constexpr Tag ReferencedSopClassUid = 0x00081150, ReferencedSopInstanceUid = 0x00081155;
constexpr Tag ReferencedFrameNumber = 0x00081160, ReferencedSopSequence = 0x00081199;
constexpr Tag SourceImageSequence = 0x00082112, DerivationImageSequence = 0x00089124;
constexpr Tag PixelPresentation = 0x00089205;
// patient
constexpr Tag PatientBirthDate = 0x00100030, PatientSex = 0x00100040;
constexpr Tag PatientSize = 0x00101020, PatientWeight = 0x00101030;
// acquisition
constexpr Tag SliceThickness = 0x00180050, SpacingBetweenSlices = 0x00180088;
constexpr Tag TriggerTime = 0x00181060, RadiopharmaceuticalStartTime = 0x00181072;
constexpr Tag RadionuclideTotalDose = 0x00181074, RadionuclideHalfLife = 0x00181075;
constexpr Tag RadiopharmaceuticalStartDateTime = 0x00181078;
constexpr Tag EstimatedRadiographicMagnificationFactor = 0x00181114, GantryDetectorTilt = 0x00181120;
constexpr Tag ImagerPixelSpacing = 0x00181164, ActualFrameDuration = 0x00181242;
constexpr Tag ShutterPresentationValue = 0x00181622, NominalScannedPixelSpacing = 0x00182010;
constexpr Tag SequenceOfUltrasoundRegions = 0x00186011;
constexpr Tag RegionSpatialFormat = 0x00186012, RegionDataType = 0x00186014;
constexpr Tag RegionLocationMinX0 = 0x00186018, RegionLocationMinY0 = 0x0018601A;
constexpr Tag RegionLocationMaxX1 = 0x0018601C, RegionLocationMaxY1 = 0x0018601E;
constexpr Tag PhysicalUnitsXDirection = 0x00186024, PhysicalUnitsYDirection = 0x00186026;
constexpr Tag PhysicalDeltaX = 0x0018602C, PhysicalDeltaY = 0x0018602E;
// relationship
constexpr Tag StudyId = 0x00200010, SeriesNumber = 0x00200011, AcquisitionNumber = 0x00200012;
constexpr Tag InstanceNumber = 0x00200013;
constexpr Tag ImagePositionPatient = 0x00200032, ImageOrientationPatient = 0x00200037;
constexpr Tag FrameOfReferenceUid = 0x00200052, TemporalPositionIdentifier = 0x00200100;
constexpr Tag SliceLocation = 0x00201041;
constexpr Tag StackId = 0x00209056, InStackPositionNumber = 0x00209057;
constexpr Tag FrameContentSequence = 0x00209111, PlanePositionSequence = 0x00209113, PlaneOrientationSequence = 0x00209116;
constexpr Tag TemporalPositionIndex = 0x00209128, NominalCardiacTriggerDelayTime = 0x00209153;
constexpr Tag FrameAcquisitionDateTime = 0x00189074, FrameReferenceDateTime = 0x00189151;
constexpr Tag DimensionIndexValues = 0x00209157;
// image
constexpr Tag PixelSpacing = 0x00280030, PixelAspectRatio = 0x00280034, CorrectedImage = 0x00280051;
constexpr Tag PixelSpacingCalibrationType = 0x00280A02, PixelSpacingCalibrationDescription = 0x00280A04;
constexpr Tag PixelMeasuresSequence = 0x00289110;
constexpr Tag SoftcopyVoiLutSequence = 0x00283110;
// PET
constexpr Tag RadiopharmaceuticalInformationSequence = 0x00540016, Units = 0x00541001;
constexpr Tag DecayCorrection = 0x00541102, FrameReferenceTime = 0x00541300, DecayFactor = 0x00541321;
constexpr Tag PatientOrientationCodeSequence = 0x00540410;
// SR
constexpr Tag MeasurementUnitsCodeSequence = 0x004008EA;
constexpr Tag RelationshipType = 0x0040A010, VerifyingObserverSequence = 0x0040A073;
constexpr Tag ValueType = 0x0040A040, ConceptNameCodeSequence = 0x0040A043, ContinuityOfContent = 0x0040A050;
constexpr Tag ReferencedWaveformChannels = 0x0040A0B0;
constexpr Tag DateTimeValue = 0x0040A120, DateValue = 0x0040A121, TimeValue = 0x0040A122;
constexpr Tag PersonNameValue = 0x0040A123, UidValue = 0x0040A124;
constexpr Tag TemporalRangeType = 0x0040A130, ReferencedSamplePositions = 0x0040A132;
constexpr Tag ReferencedTimeOffsets = 0x0040A138, ReferencedDateTime = 0x0040A13A;
constexpr Tag TextValue = 0x0040A160, FloatingPointValue = 0x0040A161;
constexpr Tag RationalNumeratorValue = 0x0040A162, RationalDenominatorValue = 0x0040A163;
constexpr Tag ConceptCodeSequence = 0x0040A168, ObservationDateTime = 0x0040A032, ObservationUid = 0x0040A171;
constexpr Tag MeasuredValueSequence = 0x0040A300, NumericValueQualifierCodeSequence = 0x0040A301;
constexpr Tag NumericValue = 0x0040A30A;
constexpr Tag CompletionFlag = 0x0040A491, VerificationFlag = 0x0040A493;
constexpr Tag ContentTemplateSequence = 0x0040A504, ContentSequence = 0x0040A730;
constexpr Tag CurrentRequestedProcedureEvidenceSequence = 0x0040A375;
constexpr Tag TemplateIdentifier = 0x0040DB00, ReferencedContentItemIdentifier = 0x0040DB73;
constexpr Tag TabulatedValuesSequence = 0x0040A801, NumberOfTableRows = 0x0040A802, NumberOfTableColumns = 0x0040A803;
constexpr Tag TableRowNumber = 0x0040A804, TableColumnNumber = 0x0040A805, CellValuesSequence = 0x0040A808;
constexpr Tag SelectorAttributeVr = 0x00720050;
constexpr Tag PixelOriginInterpretation = 0x00480301;
// presentation states
constexpr Tag GraphicAnnotationSequence = 0x00700001, GraphicLayer = 0x00700002;
constexpr Tag BoundingBoxAnnotationUnits = 0x00700003, AnchorPointAnnotationUnits = 0x00700004;
constexpr Tag GraphicAnnotationUnits = 0x00700005, UnformattedTextValue = 0x00700006;
constexpr Tag TextObjectSequence = 0x00700008, GraphicObjectSequence = 0x00700009;
constexpr Tag BoundingBoxTlhc = 0x00700010, BoundingBoxBrhc = 0x00700011, BoundingBoxJustification = 0x00700012;
constexpr Tag AnchorPoint = 0x00700014, AnchorPointVisibility = 0x00700015;
constexpr Tag GraphicDimensions = 0x00700020, NumberOfGraphicPoints = 0x00700021, GraphicData = 0x00700022;
constexpr Tag GraphicType = 0x00700023, GraphicFilled = 0x00700024;
constexpr Tag ImageHorizontalFlip = 0x00700041, ImageRotation = 0x00700042;
constexpr Tag DisplayedAreaTlhc = 0x00700052, DisplayedAreaBrhc = 0x00700053, DisplayedAreaSelectionSequence = 0x0070005A;
constexpr Tag GraphicLayerSequence = 0x00700060, GraphicLayerOrder = 0x00700062;
constexpr Tag GraphicLayerRecommendedDisplayGrayscaleValue = 0x00700066, GraphicLayerDescription = 0x00700068;
constexpr Tag ContentLabel = 0x00700080, ContentDescription = 0x00700081;
constexpr Tag PresentationCreationDate = 0x00700082, PresentationCreationTime = 0x00700083;
constexpr Tag ContentCreatorName = 0x00700084;
constexpr Tag PresentationSizeMode = 0x00700100, PresentationPixelSpacing = 0x00700101;
constexpr Tag PresentationPixelAspectRatio = 0x00700102, PresentationPixelMagnificationRatio = 0x00700103;
constexpr Tag GraphicGroupId = 0x00700295;
constexpr Tag GraphicLayerRecommendedDisplayCielabValue = 0x00700401;
constexpr Tag BlendingSequence = 0x00700402, RelativeOpacity = 0x00700403, BlendingPosition = 0x00700405;
constexpr Tag IccProfile = 0x00282000;
constexpr Tag OverlayActivationLayer = 0x60001001;  // group 60xx
// segmentation
constexpr Tag SegmentationType = 0x00620001, SegmentSequence = 0x00620002;
constexpr Tag SegmentedPropertyCategoryCodeSequence = 0x00620003, SegmentNumber = 0x00620004;
constexpr Tag SegmentLabel = 0x00620005, SegmentDescription = 0x00620006;
constexpr Tag SegmentAlgorithmType = 0x00620008, SegmentAlgorithmName = 0x00620009;
constexpr Tag SegmentIdentificationSequence = 0x0062000A, ReferencedSegmentNumber = 0x0062000B;
constexpr Tag RecommendedDisplayCielabValue = 0x0062000D, MaximumFractionalValue = 0x0062000E;
constexpr Tag SegmentedPropertyTypeCodeSequence = 0x0062000F, SegmentationFractionalType = 0x00620010;
constexpr Tag TrackingId = 0x00620020, TrackingUid = 0x00620021;
// RT structure set
constexpr Tag ReferencedFrameOfReferenceSequence = 0x30060010, RtReferencedStudySequence = 0x30060012;
constexpr Tag RtReferencedSeriesSequence = 0x30060014, ContourImageSequence = 0x30060016;
constexpr Tag StructureSetRoiSequence = 0x30060020, RoiNumber = 0x30060022;
constexpr Tag ReferencedFrameOfReferenceUid = 0x30060024, RoiName = 0x30060026, RoiDescription = 0x30060028;
constexpr Tag RoiDisplayColor = 0x3006002A, RoiGenerationAlgorithm = 0x30060036;
constexpr Tag RoiContourSequence = 0x30060039, ContourSequence = 0x30060040, ContourGeometricType = 0x30060042;
constexpr Tag NumberOfContourPoints = 0x30060046, ContourNumber = 0x30060048, ContourData = 0x30060050;
constexpr Tag RtRoiObservationsSequence = 0x30060080, ObservationNumber = 0x30060082;
constexpr Tag ReferencedRoiNumber = 0x30060084, RtRoiInterpretedType = 0x300600A4;
constexpr Tag StructureSetLabel = 0x30060002;
}  // namespace tags

// ---------------------------------------------------------------- values
// Locale-independent decimal parser (DS / IS text, PS3.5 table 6.2-1). nullopt for anything else.
std::optional<double> parse_decimal(std::string_view s) noexcept;

// All numeric values of an element: DS / IS (backslash separated, parsed in one pass), FL, FD,
// US, SS, UL, SL... Values that fail to parse make the whole result nullopt (never partially read).
std::optional<std::vector<double>> get_numbers(const Dataset& ds, Tag tag, std::size_t max_values = 1u << 26);
// Exactly n values, else nullopt.
std::optional<std::vector<double>> get_numbers_n(const Dataset& ds, Tag tag, std::size_t n);
// Trimmed text of value `vm`, nullopt when absent or empty.
std::optional<std::string> get_text(const Dataset& ds, Tag tag, std::size_t vm = 0);

// Sequence items, or an empty span when absent / not a sequence.
const std::vector<Dataset>& items_of(const Dataset& ds, Tag tag) noexcept;
const Dataset* first_item(const Dataset& ds, Tag tag) noexcept;
// Functional group macro lookup for an enhanced multi-frame object: the macro sequence `macro`
// in the Per-frame Functional Groups item `frame`, else in the Shared Functional Groups item.
// `from_per_frame` (optional) tells which one was used.
const Dataset* functional_group(const Dataset& ds, std::uint32_t frame, Tag macro, bool* from_per_frame = nullptr) noexcept;

// ---------------------------------------------------------------- dates and times (PS3.5 6.2)
// Seconds since midnight from TM "HH[MM[SS[.F{1,6}]]]" (also the retired "HH:MM:SS.frac").
std::optional<double> parse_tm(std::string_view s) noexcept;
// Days since 1970-01-01 (proleptic Gregorian) from DA "YYYYMMDD" (also the retired "YYYY.MM.DD").
std::optional<std::int64_t> parse_da(std::string_view s) noexcept;
// DT "YYYY[MM[DD[HH[MM[SS[.F]]]]]][&ZZXX]" -> seconds since 1970-01-01 00:00 of the local time
// written; `utc_offset_seconds` receives the suffix offset if present (the value is NOT shifted).
std::optional<double> parse_dt(std::string_view s, std::optional<int>* utc_offset_seconds = nullptr) noexcept;
// Combines a DA and a TM into seconds since 1970 (local time as written).
std::optional<double> combine_da_tm(std::string_view da, std::string_view tm) noexcept;

// ---------------------------------------------------------------- writing
// DS text of at most 16 characters (PS3.5 table 6.2-1), shortest that round-trips where possible.
std::string format_ds(double v);
std::string format_ds_list(const double* v, std::size_t n);
std::string format_is(std::int64_t v);
// FL / FD binary values (little endian).
Bytes encode_fl(const float* v, std::size_t n);
Bytes encode_fd(const double* v, std::size_t n);
// Current local date "YYYYMMDD" and time "HHMMSS" (std::chrono; falls back to UTC where the
// local zone is unknown).
std::string today_da();
std::string now_tm();
// A sequence element with the given items.
Element make_sequence(Tag tag, std::vector<Dataset> items);
// A code sequence item (Code Value, or Long Code Value when longer than 16 characters).
struct Code {
    std::string value;
    std::string scheme;
    std::string meaning;
    std::string scheme_version;
    bool empty() const noexcept { return value.empty() && meaning.empty(); }
    bool operator==(const Code& o) const noexcept { return value == o.value && scheme == o.scheme; }
};
Dataset code_item(const Code& c);
// Reads the first item of a code sequence (Code Value, Long Code Value or URN Code Value).
std::optional<Code> read_code(const Dataset& ds, Tag sequence_tag);
std::optional<Code> read_code_item(const Dataset& item);

// Writers keep all text in UTF-8: put_text stores the bytes and records whether any is
// non-ASCII; finish_charset then sets Specific Character Set "ISO_IR 192" when needed.
void put_text(Dataset& ds, Tag tag, VR vr, std::string_view utf8, bool& non_ascii);
void finish_charset(Dataset& ds, bool non_ascii);
// Copies the Patient and General Study module attributes from `src` (decoded to UTF-8 with its
// Specific Character Set); Type 2 attributes are written empty when absent or src is null.
void copy_patient_study(const Dataset* src, Dataset& dst, bool& non_ascii);
// CS value check: at most 16 of A-Z 0-9 '_' and space (PS3.5 table 6.2-1).
bool cs_valid(std::string_view s) noexcept;

// ---------------------------------------------------------------- UIDs
// A new UID "2.25.<integer value of a random version 4 UUID>" (PS3.5 B.2, ISO/IEC 9834-8). The
// generator is a 64-bit Mersenne Twister seeded from std::random_device; calls are thread-safe.
std::string new_uid();
// True for a syntactically valid UID (PS3.5 9.1: <= 64 chars, digits and dots, no leading zeros).
bool uid_valid(std::string_view uid) noexcept;

// ---------------------------------------------------------------- CIELab (PS3.3 C.10.7.1.1)
// DICOM-encoded CIELab (L 0..65535 -> 0..100, a/b 0..65535 -> -128..127, illuminant D50) to
// sRGB in 0..1 (unclamped components are clamped). Written from CIE 15 and IEC 61966-2-1 with
// a Bradford D50 -> D65 adaptation.
std::array<double, 3> cielab_to_rgb(std::uint16_t l, std::uint16_t a, std::uint16_t b) noexcept;
std::array<std::uint8_t, 3> cielab_to_rgb8(std::uint16_t l, std::uint16_t a, std::uint16_t b) noexcept;
// sRGB 0..255 back to DICOM CIELab (for writers).
std::array<std::uint16_t, 3> rgb8_to_cielab(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

}  // namespace siaara::clinical
