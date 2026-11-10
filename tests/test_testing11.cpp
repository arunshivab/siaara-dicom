// Validator tests for the independent testing re-run of 2026-11-11 (section 3.3) and security
// review finding R-155: permissive condition wording, several "Required if" sentences, module
// selection for float pixel data, the severity of present-condition-unmet, Functional Group
// Macro checks inside Shared / Per-Frame items, and the missed Dimension Index Sequence, Frame
// Increment Pointer and Patient Position cases. Every input is synthetic.
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "repair/iod.hpp"
#include "repair/report.hpp"
#include "repair/validate.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::repair;

namespace {

Dataset& add_item(Dataset& ds, Tag seq) {
    Element* e = ds.find(seq);
    if (!e) {
        Element ne;
        ne.tag = seq;
        ne.vr = VR::SQ;
        e = &ds.put(std::move(ne));
    }
    return e->items.emplace_back();
}

void str(Dataset& ds, Tag t, VR vr, std::string_view v) { (void)ds.set_string(t, vr, v); }

// Patient / study / series / SOP common attributes and an 8-bit 2 x 2 image.
DicomFile image_file(const char* sop_class, const char* modality, bool integer_pixels = true) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    str(ds, tags::SOPClassUID, VR::UI, sop_class);
    str(ds, tags::SOPInstanceUID, VR::UI, "2.25.4110");
    str(ds, tags::StudyInstanceUID, VR::UI, "2.25.4111");
    str(ds, tags::SeriesInstanceUID, VR::UI, "2.25.4112");
    str(ds, tags::PatientName, VR::PN, "T11^Val");
    str(ds, tags::PatientID, VR::LO, "T11");
    str(ds, tags::Modality, VR::CS, modality);
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    str(ds, tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 2);
    (void)ds.set_uint16(tags::Columns, 2);
    if (integer_pixels) {
        (void)ds.set_uint16(tags::BitsAllocated, 8);
        (void)ds.set_uint16(tags::BitsStored, 8);
        (void)ds.set_uint16(tags::HighBit, 7);
        (void)ds.set_uint16(tags::PixelRepresentation, 0);
        (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>("\1\2\3\4"), 4));
    }
    return f;
}

std::vector<Finding> iod_findings(const DicomFile& f) {
    ValidateOptions o;
    return check_iod(f, o);
}

const Finding* find(const std::vector<Finding>& f, std::string_view code, Tag last, std::string_view path_has = {}) {
    for (const Finding& x : f)
        if (x.code == code && !x.path.empty() && x.path.back().tag == last &&
            (path_has.empty() || path_string(x.path).find(path_has) != std::string::npos))
            return &x;
    return nullptr;
}

void dump(const std::vector<Finding>& f) {
    for (const Finding& x : f)
        std::printf("    %s %s %s [%s] %s\n", std::string(severity_name(x.severity)).c_str(), x.code.c_str(),
                    path_string(x.path).c_str(), x.module.c_str(), x.message.c_str());
}

#define EXPECT_NONE(findings, code, tag)                 \
    do {                                                 \
        if (find(findings, code, tag) != nullptr) {      \
            dump(findings);                              \
            CHECK(find(findings, code, tag) == nullptr); \
        }                                                \
    } while (0)
#define EXPECT_ONE(findings, code, tag, path)                  \
    do {                                                       \
        if (find(findings, code, tag, path) == nullptr) {      \
            dump(findings);                                    \
            CHECK(find(findings, code, tag, path) != nullptr); \
        }                                                      \
    } while (0)

// An Enhanced MR image with two frames: Shared and Per-Frame Functional Groups.
DicomFile enhanced_mr() {
    DicomFile f = image_file("1.2.840.10008.5.1.4.1.1.4.1", "MR");
    Dataset& ds = f.dataset;
    str(ds, 0x00280008, VR::IS, "2");
    (void)add_item(ds, 0x52009229);
    (void)add_item(ds, 0x52009230);
    (void)add_item(ds, 0x52009230);
    return f;
}

Dataset& shared(DicomFile& f) { return f.dataset.find(0x52009229)->items[0]; }
Dataset& frame(DicomFile& f, std::size_t i) { return f.dataset.find(0x52009230)->items[i]; }

}  // namespace

TEST_CASE("testing11 3.3: SR Concept Name Code Sequence on the root CONTAINER is not flagged") {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    str(ds, tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.88.11");  // Basic Text SR
    str(ds, tags::SOPInstanceUID, VR::UI, "2.25.4113");
    str(ds, tags::Modality, VR::CS, "SR");
    str(ds, 0x0040A040, VR::CS, "CONTAINER");
    Dataset& title = add_item(ds, 0x0040A043);
    str(title, 0x00080100, VR::SH, "18748-4");
    str(title, 0x00080102, VR::SH, "LN");
    str(title, 0x00080104, VR::LO, "Diagnostic Imaging Report");
    str(ds, 0x0040A050, VR::CS, "SEPARATE");
    const auto r = iod_findings(f);
    EXPECT_NONE(r, "present-condition-unmet", 0x0040A043);
    // The three "Required if" sentences are not all machine-checkable: the condition as a whole
    // is not evaluated, so a TEXT item without a concept name is not called an error either.
    const std::vector<ReqNode>* doc = module_attributes("C.17-5");
    REQUIRE(doc != nullptr);
    for (const ReqNode& n : *doc)
        if (n.tag == 0x0040A043u) CHECK(n.condition == nullptr);
}

TEST_CASE("testing11 3.3: 'May also be present if ...' allows Window Center together with VOI LUT Sequence (DX)") {
    DicomFile f = image_file("1.2.840.10008.5.1.4.1.1.1.1", "DX");
    Dataset& ds = f.dataset;
    str(ds, 0x00080068, VR::CS, "FOR PRESENTATION");
    str(ds, 0x00281050, VR::DS, "128");
    str(ds, 0x00281051, VR::DS, "256");
    Dataset& lut = add_item(ds, 0x00283010);
    const std::uint16_t desc[3] = {4, 0, 8};
    (void)lut.set_bytes(0x00283002, VR::US, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(desc), sizeof desc));
    const std::uint16_t data[4] = {0, 80, 160, 255};
    (void)lut.set_bytes(0x00283006, VR::OW, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(data), sizeof data));
    const auto r = iod_findings(f);
    EXPECT_NONE(r, "present-condition-unmet", 0x00281050);
    EXPECT_NONE(r, "present-condition-unmet", 0x00283010);
    // Neither present: both are required for FOR PRESENTATION.
    (void)ds.remove(0x00281050);
    (void)ds.remove(0x00283010);
    const auto missing = iod_findings(f);
    EXPECT_ONE(missing, "missing-type1", 0x00281050, "");
    // Every permissive wording of PS3.3 reads as "may be present": no row of the DX Image Module
    // for the two attributes forbids presence.
    const std::vector<ReqNode>* dx = module_attributes("C.8-70");
    REQUIRE(dx != nullptr);
    for (const ReqNode& n : *dx)
        if (n.tag == 0x00281050u || n.tag == 0x00283010u) CHECK(!n.absent_unless_required);
}

TEST_CASE("testing11 3.3: a float parametric map is not checked against the Image Pixel Module") {
    DicomFile f = image_file("1.2.840.10008.5.1.4.1.1.30", "OT", false);  // Parametric Map Storage
    Dataset& ds = f.dataset;
    (void)ds.set_uint16(tags::BitsAllocated, 32);
    const float px[4] = {0.5f, 1.5f, 2.5f, 3.5f};
    (void)ds.set_bytes(tags::FloatPixelData, VR::OF, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(px), sizeof px));
    const auto r = iod_findings(f);
    for (const Tag t : {tags::BitsStored, tags::HighBit, tags::PixelRepresentation, tags::PixelData}) EXPECT_NONE(r, "missing-type1", t);
    // The same object with integer pixel data and no Bits Stored is still an error.
    DicomFile g = image_file("1.2.840.10008.5.1.4.1.1.30", "OT");
    (void)g.dataset.remove(tags::BitsStored);
    const auto ri = iod_findings(g);
    EXPECT_ONE(ri, "missing-type1", tags::BitsStored, "");
}

TEST_CASE("testing11 3.3: present-condition-unmet is a warning unless the description forbids presence") {
    // Planar Configuration with Samples per Pixel 1: "Required if Samples per Pixel has a value
    // greater than 1", nothing said about otherwise.
    DicomFile f = image_file("1.2.840.10008.5.1.4.1.1.7", "OT");
    (void)f.dataset.set_uint16(0x00280006, 0);
    const auto r = iod_findings(f);
    const Finding* x = find(r, "present-condition-unmet", 0x00280006);
    if (!x) dump(r);
    REQUIRE(x != nullptr);
    CHECK(x->severity == Severity::Warning);
    // No table row is an error-level rule unless its description forbids presence in so many words.
    int forbidden = 0, plain = 0;
    for (const char* id : {"C.7-11c", "C.7-9", "C.8-70", "C.17-5", "C.7-5a", "C.7-14", "C.7.6.17-1"})
        if (const std::vector<ReqNode>* m = module_attributes(id))
            for (const ReqNode& n : *m) {
                forbidden += n.forbidden_otherwise ? 1 : 0;
                plain += n.absent_unless_required && !n.forbidden_otherwise ? 1 : 0;
                CHECK(!n.forbidden_otherwise || n.absent_unless_required);
            }
    CHECK(plain > 0);
    (void)forbidden;
}

TEST_CASE("testing11 R-155: Functional Group Macros are checked inside the Shared and every Per-Frame item") {
    DicomFile f = enhanced_mr();
    // Frame 0: MR Image Frame Type item without Frame Type (Type 1); Frame Content with a Stack ID
    // but no In-Stack Position Number (1C: "Required if Stack ID ... is present").
    (void)add_item(frame(f, 0), 0x00189226);
    Dataset& fc0 = add_item(frame(f, 0), 0x00209111);
    str(fc0, 0x00209056, VR::SH, "1");
    (void)add_item(frame(f, 0), 0x00289110);
    // Frame 1: complete frame type; Frame Content without a Stack ID.
    Dataset& ft1 = add_item(frame(f, 1), 0x00189226);
    str(ft1, 0x00089007, VR::CS, "ORIGINAL\\PRIMARY\\M\\NONE");
    str(ft1, 0x00089205, VR::CS, "MONOCHROME");
    str(ft1, 0x00089206, VR::CS, "VOLUME");
    str(ft1, 0x00089207, VR::CS, "NONE");
    str(ft1, 0x00089208, VR::CS, "MAGNITUDE");
    str(ft1, 0x00089209, VR::CS, "NONE");
    (void)add_item(frame(f, 1), 0x00209111);
    (void)add_item(frame(f, 1), 0x00289110);
    // Shared: a Frame Anatomy item without Frame Laterality (Type 1).
    (void)add_item(shared(f), 0x00209071);
    const auto r = iod_findings(f);
    // Type 1 inside a per-frame item, only in the frame that lacks it.
    EXPECT_ONE(r, "missing-type1", 0x00089007, "(5200,9230)[0](0018,9226)[0]");
    if (find(r, "missing-type1", 0x00089007, "(5200,9230)[1]") != nullptr) {
        dump(r);
        CHECK(false);
    }
    // Type 1C whose condition attribute (Stack ID) is in the same frame's item: required in
    // frame 0, not in frame 1.
    EXPECT_ONE(r, "missing-type1", 0x00209057, "(5200,9230)[0](0020,9111)[0]");
    if (find(r, "missing-type1", 0x00209057, "(5200,9230)[1]") != nullptr) {
        dump(r);
        CHECK(false);
    }
    // A mandatory macro that is nowhere: Plane Position (Patient).
    EXPECT_ONE(r, "missing-functional-group", 0x00209113, "(5200,9230)[0]");
    // Present in the shared item: checked there, and not "missing" from the frames.
    CHECK(find(r, "missing-functional-group", 0x00209071) == nullptr);
    EXPECT_ONE(r, "missing-type1", 0x00209072, "(5200,9229)[0](0020,9071)[0]");
    // Macros that are in every frame are not reported as missing.
    CHECK(find(r, "missing-functional-group", 0x00289110) == nullptr);
    CHECK(find(r, "missing-functional-group", 0x00189226) == nullptr);
    CHECK(find(r, "missing-functional-group", 0x00209111) == nullptr);
    // A macro that is in one frame only is missing from the other.
    (void)frame(f, 1).remove(0x00289110);
    const auto r2 = iod_findings(f);
    EXPECT_ONE(r2, "missing-functional-group", 0x00289110, "(5200,9230)[1]");
    // IODs without functional groups have none.
    const IodReq* mr = iod_for_sop_class("1.2.840.10008.5.1.4.1.1.4");
    REQUIRE(mr != nullptr);
    CHECK(mr->functional_groups.empty());
    const IodReq* emr = iod_for_sop_class("1.2.840.10008.5.1.4.1.1.4.1");
    REQUIRE(emr != nullptr);
    CHECK(emr->functional_groups.size() > 20);
}

TEST_CASE("testing11 3.3: Dimension Index Sequence, Frame Increment Pointer and Patient Position are required where PS3.3 says") {
    // Enhanced MR without Dimension Index Sequence: required unless Dimension Organization Type is TILED_FULL.
    DicomFile e = enhanced_mr();
    auto r = iod_findings(e);
    EXPECT_ONE(r, "missing-type1", 0x00209222, "");
    str(e.dataset, 0x00209311, VR::CS, "TILED_FULL");
    r = iod_findings(e);
    EXPECT_NONE(r, "missing-type1", 0x00209222);
    str(e.dataset, 0x00209311, VR::CS, "3D");
    r = iod_findings(e);
    EXPECT_ONE(r, "missing-type1", 0x00209222, "");

    // NM (Multi-frame Module is mandatory) and a multi-frame US image without Frame Increment Pointer.
    DicomFile nm = image_file("1.2.840.10008.5.1.4.1.1.20", "NM");
    str(nm.dataset, 0x00280008, VR::IS, "2");
    r = iod_findings(nm);
    EXPECT_ONE(r, "missing-type1", 0x00280009, "");
    DicomFile us = image_file("1.2.840.10008.5.1.4.1.1.3.1", "US");  // Ultrasound Multi-frame Image
    str(us.dataset, 0x00280008, VR::IS, "2");
    r = iod_findings(us);
    EXPECT_ONE(r, "missing-type1", 0x00280009, "");

    // Patient Position: Type 2C, required for CT and MR when there is no Patient Orientation Code Sequence.
    DicomFile mr = image_file("1.2.840.10008.5.1.4.1.1.4", "MR");
    r = iod_findings(mr);
    EXPECT_ONE(r, "missing-type2", 0x00185100, "");
    str(mr.dataset, 0x00185100, VR::CS, "HFS");
    r = iod_findings(mr);
    EXPECT_NONE(r, "missing-type2", 0x00185100);
    // Not required for Secondary Capture.
    DicomFile sc = image_file("1.2.840.10008.5.1.4.1.1.7", "OT");
    r = iod_findings(sc);
    EXPECT_NONE(r, "missing-type2", 0x00185100);
}
