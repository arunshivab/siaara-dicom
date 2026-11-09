// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Text;

/// <summary>The DICOM attributes the viewer reads (and the ones the sample generator writes).</summary>
public static class Tags
{
#pragma warning disable CS1591 // Each field is named after its DICOM keyword; the number is the documentation.
    public static readonly TkTag ImageType = new(0x00080008);
    public static readonly TkTag SpecificCharacterSet = new(0x00080005);
    public static readonly TkTag SopClassUid = new(0x00080016);
    public static readonly TkTag SopInstanceUid = new(0x00080018);
    public static readonly TkTag StudyDate = new(0x00080020);
    public static readonly TkTag SeriesDate = new(0x00080021);
    public static readonly TkTag AcquisitionDate = new(0x00080022);
    public static readonly TkTag StudyTime = new(0x00080030);
    public static readonly TkTag SeriesTime = new(0x00080031);
    public static readonly TkTag AcquisitionTime = new(0x00080032);
    public static readonly TkTag AccessionNumber = new(0x00080050);
    public static readonly TkTag Modality = new(0x00080060);
    public static readonly TkTag Manufacturer = new(0x00080070);
    public static readonly TkTag InstitutionName = new(0x00080080);
    public static readonly TkTag ReferringPhysicianName = new(0x00080090);
    public static readonly TkTag StationName = new(0x00081010);
    public static readonly TkTag StudyDescription = new(0x00081030);
    public static readonly TkTag SeriesDescription = new(0x0008103E);
    public static readonly TkTag ManufacturerModelName = new(0x00081090);
    public static readonly TkTag RecommendedDisplayFrameRate = new(0x00082144);
    public static readonly TkTag PatientName = new(0x00100010);
    public static readonly TkTag PatientId = new(0x00100020);
    public static readonly TkTag PatientBirthDate = new(0x00100030);
    public static readonly TkTag PatientSex = new(0x00100040);
    public static readonly TkTag PatientAge = new(0x00101010);
    public static readonly TkTag PatientSize = new(0x00101020);
    public static readonly TkTag PatientWeight = new(0x00101030);
    public static readonly TkTag ContrastBolusAgent = new(0x00180010);
    public static readonly TkTag BodyPartExamined = new(0x00180015);
    public static readonly TkTag SequenceName = new(0x00180024);
    public static readonly TkTag CineRate = new(0x00180040);
    public static readonly TkTag SliceThickness = new(0x00180050);
    public static readonly TkTag Kvp = new(0x00180060);
    public static readonly TkTag RepetitionTime = new(0x00180080);
    public static readonly TkTag EchoTime = new(0x00180081);
    public static readonly TkTag MagneticFieldStrength = new(0x00180087);
    public static readonly TkTag SpacingBetweenSlices = new(0x00180088);
    public static readonly TkTag ProtocolName = new(0x00181030);
    public static readonly TkTag FrameTime = new(0x00181063);
    public static readonly TkTag RadiopharmaceuticalStartTime = new(0x00181072);
    public static readonly TkTag RadionuclideTotalDose = new(0x00181074);
    public static readonly TkTag RadionuclideHalfLife = new(0x00181075);
    public static readonly TkTag ExposureTime = new(0x00181150);
    public static readonly TkTag XRayTubeCurrent = new(0x00181151);
    public static readonly TkTag ImagerPixelSpacing = new(0x00181164);
    public static readonly TkTag FlipAngle = new(0x00181314);
    public static readonly TkTag ViewPosition = new(0x00185101);
    public static readonly TkTag StudyInstanceUid = new(0x0020000D);
    public static readonly TkTag SeriesInstanceUid = new(0x0020000E);
    public static readonly TkTag SeriesNumber = new(0x00200011);
    public static readonly TkTag InstanceNumber = new(0x00200013);
    public static readonly TkTag ImagePositionPatient = new(0x00200032);
    public static readonly TkTag ImageOrientationPatient = new(0x00200037);
    public static readonly TkTag FrameOfReferenceUid = new(0x00200052);
    public static readonly TkTag Laterality = new(0x00200060);
    public static readonly TkTag ImageLaterality = new(0x00200062);
    public static readonly TkTag SliceLocation = new(0x00201041);
    public static readonly TkTag SamplesPerPixel = new(0x00280002);
    public static readonly TkTag PhotometricInterpretation = new(0x00280004);
    public static readonly TkTag PlanarConfiguration = new(0x00280006);
    public static readonly TkTag NumberOfFrames = new(0x00280008);
    public static readonly TkTag FrameIncrementPointer = new(0x00280009);
    public static readonly TkTag Rows = new(0x00280010);
    public static readonly TkTag Columns = new(0x00280011);
    public static readonly TkTag PixelSpacing = new(0x00280030);
    public static readonly TkTag BitsAllocated = new(0x00280100);
    public static readonly TkTag BitsStored = new(0x00280101);
    public static readonly TkTag HighBit = new(0x00280102);
    public static readonly TkTag PixelRepresentation = new(0x00280103);
    public static readonly TkTag WindowCenter = new(0x00281050);
    public static readonly TkTag WindowWidth = new(0x00281051);
    public static readonly TkTag RescaleIntercept = new(0x00281052);
    public static readonly TkTag RescaleSlope = new(0x00281053);
    public static readonly TkTag RescaleType = new(0x00281054);
    public static readonly TkTag LossyImageCompression = new(0x00282110);
    public static readonly TkTag LossyImageCompressionRatio = new(0x00282112);
    public static readonly TkTag RadiopharmaceuticalInformationSequence = new(0x00540016);
    public static readonly TkTag Units = new(0x00541001);
    public static readonly TkTag DecayCorrection = new(0x00541102);
    public static readonly TkTag CorrectedImage = new(0x00280051);
    public static readonly TkTag PixelData = new(0x7FE00010);
#pragma warning restore CS1591
}
