// Independent testing verdict on the release candidate, smaller items: P3-5 (validator:
// Segmentation functional groups), P3-6 (DICOM JSON / XML and string VRs the character set does
// not apply to), P3-7 (Image Type of a lossy derived image), P3-8 (the HU default is the CT IODs'),
// P3-10 (ACR-NEMA volumes: Planes).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "image/frames.hpp"
#include "repair/validate.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"

using namespace siaara;

namespace {

Dataset& add_item(Dataset& ds, Tag seq) {
    Element* e = ds.find(seq);
    if (!e) {
        Element s;
        s.tag = seq;
        s.vr = VR::SQ;
        e = &ds.put(std::move(s));
    }
    return e->items.emplace_back();
}

// A per-frame functional group item of a segmentation frame.
void seg_frame(Dataset& ds, bool whole) {
    Dataset& per = add_item(ds, 0x52009230);
    Dataset& derivation = add_item(per, 0x00189807);  // something else, so the item is never empty
    (void)derivation.set_string(0x00189808, VR::CS, "X");
    if (!whole) return;
    Dataset& content = add_item(per, 0x00209111);  // Frame Content Sequence
    (void)content.set_string(0x00209157, VR::UL, "1");
    Dataset& position = add_item(per, 0x00209113);  // Plane Position Sequence
    (void)position.set_string(0x00200032, VR::DS, "0\\0\\0");
    Dataset& segment = add_item(per, 0x0062000A);  // Segment Identification Sequence
    (void)segment.set_uint16(0x0062000B, 1);
}

std::vector<repair::Finding> validate_seg(const char* sop_class, const char* segmentation_type, bool second_frame_whole) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, sop_class);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.9500001");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.9500900");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.9500901");
    (void)ds.set_string(tags::Modality, VR::CS, "SEG");
    (void)ds.set_string(0x00200052, VR::UI, "2.25.9500902");  // Frame of Reference UID
    (void)ds.set_string(0x00620001, VR::CS, segmentation_type);
    (void)ds.set_string(tags::NumberOfFrames, VR::IS, "2");
    Dataset& shared = add_item(ds, 0x52009229);
    Dataset& orientation = add_item(shared, 0x00209116);
    (void)orientation.set_string(0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
    Dataset& measures = add_item(shared, 0x00289110);
    (void)measures.set_string(0x00280030, VR::DS, "1\\1");
    seg_frame(ds, true);
    seg_frame(ds, second_frame_whole);
    std::vector<std::uint8_t> bytes;
    (void)write_part10(f, bytes);
    repair::ValidateOptions vo;
    vo.check_dictionary = false;
    vo.check_pixels = false;
    auto r = repair::validate_bytes(Bytes::copy_of(bytes.data(), bytes.size()), vo);
    return r ? *r : std::vector<repair::Finding>{};
}

int missing_groups(const std::vector<repair::Finding>& fs, const char* macro) {
    int n = 0;
    for (const repair::Finding& f : fs)
        if (f.code == "missing-functional-group" && f.module.find(macro) == 0) ++n;
    return n;
}

DicomFile small_image(const char* sop_class, const char* modality) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    if (sop_class) (void)ds.set_string(tags::SOPClassUID, VR::UI, sop_class);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.9800001");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.9800900");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.9800901");
    (void)ds.set_string(tags::Modality, VR::CS, modality);
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 8);
    (void)ds.set_uint16(tags::Columns, 8);
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(64);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>(k * 4u);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    return f;
}

int32_t units_source_of(const DicomFile& f, std::string* units = nullptr) {
    std::vector<std::uint8_t> bytes;
    (void)write_part10(f, bytes);
    siaara_file* file = nullptr;
    if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &file) != SIAARA_OK) return -1;
    siaara_pixel_value pv;
    std::memset(&pv, 0, sizeof pv);
    pv.struct_size = sizeof pv;
    const siaara_status st = siaara_file_pixel_value(file, 0, 1, 1, 0, &pv);
    siaara_file_free(file);
    if (st != SIAARA_OK) return -2;
    if (units) *units = pv.units;
    return pv.units_source;
}

}  // namespace

TEST_CASE("rc2 P3-5: the validator reports a Segmentation frame without Segment Identification and plane position") {
    const char* kSeg = "1.2.840.10008.5.1.4.1.1.66.4";
    // Both frames whole: nothing to report about functional groups.
    const auto whole = validate_seg(kSeg, "BINARY", true);
    CHECK_EQ(missing_groups(whole, "Segmentation"), 0);
    CHECK_EQ(missing_groups(whole, "Plane Position (Patient)"), 0);
    // The second frame without Frame Content, Plane Position and Segment Identification.
    const auto cut = validate_seg(kSeg, "BINARY", false);
    CHECK_EQ(missing_groups(cut, "Segmentation"), 1);              // on the old code: no finding at all
    CHECK_EQ(missing_groups(cut, "Plane Position (Patient)"), 1);
    CHECK_EQ(missing_groups(cut, "Plane Orientation (Patient)"), 0);  // it is in the shared item
    CHECK_EQ(missing_groups(cut, "Pixel Measures"), 0);
    for (const repair::Finding& f : cut)
        if (f.code == "missing-functional-group") {
            CHECK(f.severity == repair::Severity::Error);
            CHECK(f.message.find("1 of 2") != std::string::npos);
        }
    // A label map needs no Segment Identification (Segmentation Type LABELMAP): not reported.
    const auto label = validate_seg(kSeg, "LABELMAP", false);
    CHECK_EQ(missing_groups(label, "Segmentation"), 0);
    CHECK_EQ(missing_groups(label, "Plane Position (Patient)"), 1);
}

TEST_CASE("rc2 P3-6: DICOM JSON and XML give U+FFFD for a byte above 0x7F in a VR the character set does not apply to") {
    Dataset ds;
    (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
    (void)ds.set_bytes(0x00080060, VR::CS, Bytes::copy_of("M\xFCR ", 4));       // Modality: CS, no character set
    (void)ds.set_bytes(0x00100010, VR::PN, Bytes::copy_of("M\xFCller", 6));     // Patient's Name: Latin-1 applies
    (void)ds.set_bytes(0x0008103E, VR::LO, Bytes::copy_of("K\xFChl", 4));    // Series Description: LO
    // The C API's text getter is the reference.
    char buf[64];
    size_t need = 0;
    const siaara_dataset* h = reinterpret_cast<const siaara_dataset*>(&ds);
    REQUIRE(siaara_dataset_get_utf8(h, 0x00080060, 0, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::string(buf) == "M\xEF\xBF\xBDR");
    REQUIRE(siaara_dataset_get_utf8(h, 0x00100010, 0, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::string(buf) == "M\xC3\xBCller");
    std::string json;
    REQUIRE(web::dataset_to_json(ds, json));
    CHECK(json.find("M\xEF\xBF\xBDR") != std::string::npos);  // was "M\xC3\xBCR" (decoded as Latin-1)
    CHECK(json.find("M\xC3\xBCR") == std::string::npos);
    CHECK(json.find("M\xC3\xBCller") != std::string::npos);    // PN and LO still use the character set
    CHECK(json.find("K\xC3\xBChl") != std::string::npos);
    std::string xml;
    REQUIRE(web::dataset_to_xml(ds, xml));
    CHECK(xml.find("M\xEF\xBF\xBDR") != std::string::npos);
    CHECK(xml.find("M\xC3\xBCller") != std::string::npos);
    // The stored bytes are untouched.
    CHECK(std::memcmp(ds.find(0x00080060)->value.data(), "M\xFCR ", 4) == 0);
}

TEST_CASE("rc2 P3-7: a lossy derived image gets Image Type DERIVED\\SECONDARY when the source has fewer than two values") {
    struct Row {
        const char* source;    // nullptr: absent
        const char* expected;
        bool note;
    };
    const Row rows[] = {
        {nullptr, "DERIVED\\SECONDARY", true},
        {"", "DERIVED\\SECONDARY", true},
        {"ORIGINAL", "DERIVED\\SECONDARY", true},  // was "DERIVED" alone: one value, not allowed
        {"ORIGINAL\\PRIMARY", "DERIVED\\PRIMARY", false},
        {"ORIGINAL\\PRIMARY\\AXIAL", "DERIVED\\PRIMARY\\AXIAL", false},
    };
    for (const Row& row : rows) {
        DicomFile f = small_image("1.2.840.10008.5.1.4.1.1.7", "OT");
        if (row.source) (void)f.dataset.set_string(0x00080008, VR::CS, row.source);
        codec::TranscodeParams p;
        p.lossy = true;
        codec::TranscodeReport rep;
        REQUIRE(codec::transcode_file(f, "1.2.840.10008.1.2.4.50", p, &rep));
        CHECK(rep.lossy_applied);
        std::string_view image_type = f.dataset.get_raw_string(0x00080008).value_or("");
        while (!image_type.empty() && image_type.back() == ' ') image_type.remove_suffix(1);  // even-length padding
        CHECK_EQ(image_type, std::string_view(row.expected));
        CHECK_EQ(rep.image_type_added, row.note);
        CHECK_EQ(rep.derivation_note.rfind("IMAGE_TYPE_ADDED", 0) == 0, row.note);
    }
}

TEST_CASE("rc2 P3-8: the HU default is keyed on the CT SOP classes, not on Modality CT") {
    std::string units;
    CHECK_EQ(units_source_of(small_image("1.2.840.10008.5.1.4.1.1.2", "CT"), &units), SIAARA_UNITS_CT_HU);
    CHECK(units == "HU");
    CHECK_EQ(units_source_of(small_image("1.2.840.10008.5.1.4.1.1.2.1", "CT")), SIAARA_UNITS_CT_HU);
    // A Secondary Capture of a CT: the unit is unknown ...
    CHECK_EQ(units_source_of(small_image("1.2.840.10008.5.1.4.1.1.7", "CT"), &units), SIAARA_UNITS_NONE);
    CHECK(units.empty());
    CHECK_EQ(units_source_of(small_image("1.2.840.10008.5.1.4.1.1.7.2", "CT")), SIAARA_UNITS_NONE);
    // ... unless Rescale Type says so.
    DicomFile sc = small_image("1.2.840.10008.5.1.4.1.1.7", "CT");
    (void)sc.dataset.set_string(0x00281052, VR::DS, "-1024");
    (void)sc.dataset.set_string(0x00281053, VR::DS, "1");
    (void)sc.dataset.set_string(0x00281054, VR::LO, "HU");
    CHECK_EQ(units_source_of(sc, &units), SIAARA_UNITS_RESCALE_TYPE);
    CHECK(units == "HU");
    // No SOP Class UID at all (old data): judged by Modality, as before.
    CHECK_EQ(units_source_of(small_image(nullptr, "CT")), SIAARA_UNITS_CT_HU);
    CHECK_EQ(units_source_of(small_image("1.2.840.10008.5.1.4.1.1.4", "MR")), SIAARA_UNITS_NONE);
}

TEST_CASE("rc2 P3-10: an ACR-NEMA volume without Number of Frames is read as Planes frames when the pixel data fits exactly") {
    DicomFile f = small_image("1.2.840.10008.5.1.4.1.1.7", "OT");
    std::vector<std::uint8_t> px(64 * 5);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>(k / 64);
    (void)f.dataset.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    (void)f.dataset.set_uint16(0x00280012, 5);  // Planes (retired)
    auto fr = image::FrameReader::open(f.dataset, f.transfer_syntax);
    REQUIRE(fr);
    CHECK_EQ(fr->frame_count(), 5u);
    bool said = false;
    for (const std::string& w : fr->warnings()) said = said || w.rfind("ACR_NEMA_PLANES", 0) == 0;
    CHECK(said);
    auto last = fr->decode(4);
    REQUIRE(last);
    CHECK_EQ((*last)[0], 4u);
    // Strict mode, a Planes value the data does not fit, or a Number of Frames: no such reading.
    image::FrameOptions strict;
    strict.lenient = false;
    auto sfr = image::FrameReader::open(f.dataset, f.transfer_syntax, strict);
    CHECK(!sfr || sfr->frame_count() == 1);
    (void)f.dataset.set_uint16(0x00280012, 4);
    auto misfit = image::FrameReader::open(f.dataset, f.transfer_syntax);
    CHECK(!misfit || misfit->frame_count() == 1);
    (void)f.dataset.set_uint16(0x00280012, 5);
    (void)f.dataset.set_string(tags::NumberOfFrames, VR::IS, "1");
    auto stated = image::FrameReader::open(f.dataset, f.transfer_syntax);
    CHECK(!stated || stated->frame_count() == 1);
}
