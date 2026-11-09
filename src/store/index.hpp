// SIAARA DICOM Toolkit - index record extraction for viewer databases.
//
// extract_index() pulls, in one parse, every patient/study/series/instance key a viewer database
// stores (PS3.3 C.7.1.1 Patient, C.7.2.1 General Study, C.7.3.1 General Series, C.7.6.1 General
// Image, C.7.6.2 Image Plane, C.7.6.3 Image Pixel, C.12.1 SOP Common) into a fixed record: text
// as UTF-8 (decoded with the instance's Specific Character Set), numbers as numbers, plus the
// transfer syntax, file size and the byte offset of the Pixel Data value.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::store {

// String fields. The numeric values are part of the C ABI (SIAARA_IDX_* in siaara.h).
enum class Str : std::uint8_t {
    // patient
    PatientId, IssuerOfPatientId, PatientName, PatientBirthDate, PatientSex,
    // study
    StudyInstanceUid, StudyDate, StudyTime, AccessionNumber, StudyId, StudyDescription,
    ReferringPhysician,
    // series
    SeriesInstanceUid, Modality, SeriesDescription, BodyPartExamined, SeriesDate, SeriesTime,
    FrameOfReferenceUid,
    // instance
    SopClassUid, SopInstanceUid, TransferSyntaxUid, PhotometricInterpretation, ContentDate,
    ContentTime, ImageType, SpecificCharacterSet,
    // filled by the store and the DICOMDIR reader
    FilePath, ModalitiesInStudy, RecordType,
    Count
};
constexpr std::size_t kStrCount = static_cast<std::size_t>(Str::Count);

// flags
constexpr std::uint32_t kHasSeriesNumber   = 1u << 0;
constexpr std::uint32_t kHasInstanceNumber = 1u << 1;
constexpr std::uint32_t kHasPosition       = 1u << 2;   // Image Position (Patient), 3 values
constexpr std::uint32_t kHasOrientation    = 1u << 3;   // Image Orientation (Patient), 6 values
constexpr std::uint32_t kHasPixelData      = 1u << 4;
constexpr std::uint32_t kEncapsulated      = 1u << 5;
constexpr std::uint32_t kHadPreamble       = 1u << 6;   // Part 10 preamble + "DICM" present
constexpr std::uint32_t kHasPixelSpacing   = 1u << 7;
constexpr std::uint32_t kHasSliceThickness = 1u << 8;
constexpr std::uint32_t kReferenced        = 1u << 9;   // store: file indexed in place, not copied
constexpr std::uint32_t kPixelOffsetKnown  = 1u << 10;  // pixel_data_offset is a file offset

enum class Level : std::int32_t { Patient = 0, Study = 1, Series = 2, Instance = 3 };

struct IndexRecord {
    std::array<std::string, kStrCount> s;
    Level level = Level::Instance;
    std::uint32_t flags = 0;
    std::int32_t series_number = 0;
    std::int32_t instance_number = 0;
    std::uint32_t number_of_frames = 0;   // 0 when the instance has no pixel data
    std::uint32_t rows = 0, columns = 0;
    std::uint16_t bits_allocated = 0, bits_stored = 0, high_bit = 0, samples_per_pixel = 0;
    std::uint16_t pixel_representation = 0;
    std::uint32_t num_studies = 0, num_series = 0, num_instances = 0;  // query results
    std::uint64_t file_size = 0;
    std::uint64_t pixel_data_offset = 0;  // offset of the Pixel Data value field (first item for
                                          // encapsulated data), valid when kPixelOffsetKnown
    std::uint64_t pixel_data_length = 0;  // bytes of that value (through the sequence delimiter)
    double position[3] = {0, 0, 0};
    double orientation[6] = {0, 0, 0, 0, 0, 0};
    double pixel_spacing[2] = {0, 0};
    double slice_thickness = 0;
    std::uint64_t added_time = 0;         // store: seconds since 1970 when added

    std::string& operator[](Str k) noexcept { return s[static_cast<std::size_t>(k)]; }
    const std::string& operator[](Str k) const noexcept { return s[static_cast<std::size_t>(k)]; }
};

constexpr std::string_view kMediaStorageDirectoryUid = "1.2.840.10008.1.3.10";

struct ExtractOptions {
    bool allow_raw_dataset = true;  // accept files without preamble / "DICM"
};

// Parses `data` (a whole Part 10 file, or a raw data set) and fills `out`. `file_size` is stored
// as is (pass data.size() for buffers). NotDicom when the bytes are not DICOM.
Status extract_index(const Bytes& data, IndexRecord& out, const ExtractOptions& opt = {});
// Memory-maps the file and extracts it.
Status extract_index_file(const std::string& utf8_path, IndexRecord& out, const ExtractOptions& opt = {});

// Fills the key strings (and Series/Instance Number) present in `ds`, decoding text with `cs`.
// overlay = true leaves keys the data set does not contain untouched.
void read_keys(const Dataset& ds, const charset::CharacterSet& cs, IndexRecord& out, bool overlay);
// One element's whole value as UTF-8 (all values, backslash separated), padding removed.
std::string element_text(const Dataset& ds, Tag tag, const charset::CharacterSet& cs);

// Cheap test on the first bytes: preamble + "DICM", or a plausible raw data set start
// (a group 0002/0008 element with a valid explicit VR or a sane implicit length).
bool looks_like_dicom(const std::uint8_t* data, std::size_t size, bool allow_raw) noexcept;

}  // namespace siaara::store
