// Clinical layer: DICOM SEG (src/clinical/seg.*), RT Structure Sets (src/clinical/rtstruct.*)
// and the CIELab -> sRGB conversion used for their display colours.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/rtstruct.hpp"
#include "clinical/seg.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "image/overlay.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

constexpr std::string_view kTs = "1.2.840.10008.1.2.1";
constexpr std::string_view kCt = "1.2.840.10008.5.1.4.1.1.2";

void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }
Dataset with(Tag t, VR vr, std::string_view v) {
    Dataset d;
    put(d, t, vr, v);
    return d;
}

Dataset seg_common(std::string_view type, int rows, int cols, int frames, int bits) {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.66.4");
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.600");
    put(d, ct::SegmentationType, VR::CS, type);
    (void)d.set_uint16(siaara::tags::Rows, static_cast<std::uint16_t>(rows));
    (void)d.set_uint16(siaara::tags::Columns, static_cast<std::uint16_t>(cols));
    (void)d.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)d.set_uint16(siaara::tags::BitsAllocated, static_cast<std::uint16_t>(bits));
    (void)d.set_uint16(siaara::tags::BitsStored, static_cast<std::uint16_t>(bits));
    (void)d.set_uint16(siaara::tags::HighBit, static_cast<std::uint16_t>(bits - 1));
    (void)d.set_uint16(siaara::tags::PixelRepresentation, 0);
    put(d, siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    put(d, siaara::tags::NumberOfFrames, VR::IS, std::to_string(frames));
    put(d, ct::FrameOfReferenceUid, VR::UI, "1.2.3.4");
    return d;
}

Dataset segment_item(int number, std::string_view label, std::array<std::uint16_t, 3> lab) {
    Dataset s;
    (void)s.set_uint16(ct::SegmentNumber, static_cast<std::uint16_t>(number));
    put(s, ct::SegmentLabel, VR::LO, label);
    put(s, ct::SegmentAlgorithmType, VR::CS, "MANUAL");
    s.put(make_sequence(ct::SegmentedPropertyCategoryCodeSequence, {code_item({"123037004", "SCT", "Anatomical Structure", {}})}));
    s.put(make_sequence(ct::SegmentedPropertyTypeCodeSequence, {code_item({"10200004", "SCT", "Liver", {}})}));
    std::uint8_t b[6];
    for (int k = 0; k < 3; ++k) store_le16(b + 2 * k, lab[static_cast<std::size_t>(k)]);
    (void)s.set_bytes(ct::RecommendedDisplayCielabValue, VR::US, Bytes::copy_of(b, 6));
    return s;
}

Dataset per_frame(int segment, std::string_view source_uid, double z) {
    Dataset it;
    Dataset si;
    (void)si.set_uint16(ct::ReferencedSegmentNumber, static_cast<std::uint16_t>(segment));
    it.put(make_sequence(ct::SegmentIdentificationSequence, {si}));
    Dataset src;
    put(src, ct::ReferencedSopClassUid, VR::UI, kCt);
    put(src, ct::ReferencedSopInstanceUid, VR::UI, source_uid);
    Dataset der;
    der.put(make_sequence(ct::SourceImageSequence, {src}));
    it.put(make_sequence(ct::DerivationImageSequence, {der}));
    it.put(make_sequence(ct::PlanePositionSequence, {with(ct::ImagePositionPatient, VR::DS, "0\\0\\" + format_ds(z))}));
    return it;
}

// BINARY SEG, 3 x 3 pixels, 3 frames packed contiguously (frame k starts at bit 9k):
//   frame 0 (segment 1, source A, z 0): diagonal pixels 0, 4, 8
//   frame 1 (segment 1, source B, z 1): all nine pixels
//   frame 2 (segment 2, source A, z 0): pixel 1
Dataset binary_seg() {
    Dataset d = seg_common("BINARY", 3, 3, 3, 1);
    d.put(make_sequence(ct::SegmentSequence, {segment_item(1, "Liver", {65535, 32896, 32896}), segment_item(2, "Tumor", rgb8_to_cielab(255, 0, 0))}));
    Dataset shared;
    shared.put(make_sequence(ct::PlaneOrientationSequence, {with(ct::ImageOrientationPatient, VR::DS, "1\\0\\0\\0\\1\\0")}));
    shared.put(make_sequence(ct::PixelMeasuresSequence, {with(ct::PixelSpacing, VR::DS, "1\\1")}));
    d.put(make_sequence(0x52009229, {shared}));
    d.put(make_sequence(0x52009230, {per_frame(1, "1.2.3.A", 0), per_frame(1, "1.2.3.B", 1), per_frame(2, "1.2.3.A", 0)}));
    std::vector<int> bits(27, 0);
    bits[0] = bits[4] = bits[8] = 1;
    for (int i = 9; i < 18; ++i) bits[static_cast<std::size_t>(i)] = 1;
    bits[18 + 1] = 1;
    std::vector<std::uint8_t> packed(4, 0);
    for (int i = 0; i < 27; ++i)
        if (bits[static_cast<std::size_t>(i)]) packed[static_cast<std::size_t>(i / 8)] |= static_cast<std::uint8_t>(1u << (i % 8));
    (void)d.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(packed.data(), packed.size()));
    return d;
}

Dataset rtstruct() {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.481.3");
    put(d, ct::StructureSetLabel, VR::SH, "PLAN1");
    auto roi = [](int n, std::string_view name) {
        Dataset r;
        put(r, ct::RoiNumber, VR::IS, std::to_string(n));
        put(r, ct::RoiName, VR::LO, name);
        put(r, ct::ReferencedFrameOfReferenceUid, VR::UI, "1.2.3.4");
        put(r, ct::RoiGenerationAlgorithm, VR::CS, "MANUAL");
        return r;
    };
    d.put(make_sequence(ct::StructureSetRoiSequence, {roi(1, "PTV"), roi(2, "Cord")}));
    auto contour = [](std::string_view type, std::string_view data, int npts, std::string_view ref) {
        Dataset c;
        put(c, ct::ContourGeometricType, VR::CS, type);
        put(c, ct::NumberOfContourPoints, VR::IS, std::to_string(npts));
        put(c, ct::ContourData, VR::DS, data);
        if (!ref.empty()) {
            Dataset im;
            put(im, ct::ReferencedSopClassUid, VR::UI, kCt);
            put(im, ct::ReferencedSopInstanceUid, VR::UI, ref);
            c.put(make_sequence(ct::ContourImageSequence, {im}));
        }
        return c;
    };
    Dataset rc1;
    put(rc1, ct::ReferencedRoiNumber, VR::IS, "1");
    put(rc1, ct::RoiDisplayColor, VR::IS, "255\\0\\0");
    rc1.put(make_sequence(ct::ContourSequence, {contour("CLOSED_PLANAR", "10\\20\\0\\30\\20\\0\\30\\40\\0", 3, "1.2.3.100"),
                                                  contour("CLOSED_PLANAR", "10\\20\\2\\30\\20\\2\\30\\40\\2", 3, ""),
                                                  contour("CLOSED_PLANAR", "1\\2\\3\\4", 1, "")}));
    Dataset rc2;
    put(rc2, ct::ReferencedRoiNumber, VR::IS, "2");
    rc2.put(make_sequence(ct::ContourSequence, {contour("POINT", "5\\6\\0.2", 1, "")}));
    Dataset rc3;  // undefined ROI
    put(rc3, ct::ReferencedRoiNumber, VR::IS, "7");
    d.put(make_sequence(ct::RoiContourSequence, {rc1, rc2, rc3}));
    Dataset obs;
    put(obs, ct::ReferencedRoiNumber, VR::IS, "1");
    put(obs, ct::RtRoiInterpretedType, VR::CS, "PTV");
    d.put(make_sequence(ct::RtRoiObservationsSequence, {obs}));
    return d;
}

siaara_file* to_capi(const Dataset& ds) {
    DicomFile f;
    f.dataset = ds;
    f.transfer_syntax = std::string(kTs);
    std::vector<std::uint8_t> bytes;
    if (!write_part10(f, bytes)) return nullptr;
    siaara_file* out = nullptr;
    if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &out) != SIAARA_OK) return nullptr;
    return out;
}

}  // namespace

TEST_CASE("seg: CIELab to sRGB (PS3.3 C.10.7.1.1 encoding, D50 -> D65)") {
    CHECK((cielab_to_rgb8(65535, 32896, 32896) == std::array<std::uint8_t, 3>{255, 255, 255}));
    CHECK((cielab_to_rgb8(0, 32896, 32896) == std::array<std::uint8_t, 3>{0, 0, 0}));
    const std::uint8_t samples[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 128, 128}, {10, 200, 90}, {250, 240, 5}};
    for (const auto& s : samples) {
        const auto lab = rgb8_to_cielab(s[0], s[1], s[2]);
        const auto back = cielab_to_rgb8(lab[0], lab[1], lab[2]);
        for (int k = 0; k < 3; ++k) CHECK(std::abs(back[static_cast<std::size_t>(k)] - s[k]) <= 1);
    }
    // Neutral grey keeps a = b = 0 (0x8080).
    const auto g = rgb8_to_cielab(128, 128, 128);
    CHECK(std::abs(g[1] - 32896) <= 2 && std::abs(g[2] - 32896) <= 2);
    // Agrees with the image layer's shutter colour conversion within one level.
    for (std::uint32_t l = 0; l <= 65535; l += 13107)
        for (std::uint32_t a = 0; a <= 65535; a += 16383)
            for (std::uint32_t b = 0; b <= 65535; b += 16383) {
                const auto x = cielab_to_rgb8(static_cast<std::uint16_t>(l), static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b));
                const auto y = image::cielab_to_srgb(static_cast<std::uint16_t>(l), static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b));
                for (std::size_t k = 0; k < 3; ++k) CHECK(std::abs(x[k] - y[k]) <= 1);
            }
}

TEST_CASE("seg: BINARY segmentation, segments, frame mapping and packed masks") {
    const Dataset d = binary_seg();
    auto seg = read_segmentation(d, false);
    REQUIRE(seg.is_ok());
    CHECK(seg->type == SegmentationType::Binary);
    CHECK(seg->rows == 3 && seg->columns == 3 && seg->number_of_frames == 3);
    REQUIRE(seg->segments.size() == 2);
    const Segment* liver = seg->segment(1);
    REQUIRE(liver);
    CHECK(liver->label == "Liver" && liver->algorithm_type == "MANUAL");
    CHECK(liver->category && liver->category->value == "123037004");
    CHECK(liver->type && liver->type->meaning == "Liver");
    CHECK((liver->rgb == std::array<std::uint8_t, 3>{255, 255, 255}));
    CHECK((liver->frames == std::vector<std::uint32_t>{0, 1}));
    const Segment* tumor = seg->segment(2);
    REQUIRE(tumor && tumor->rgb);
    CHECK((*tumor->rgb)[0] >= 254 && (*tumor->rgb)[1] <= 1);
    CHECK((tumor->frames == std::vector<std::uint32_t>{2}));
    CHECK((seg->frames_for_source(1, "1.2.3.A") == std::vector<std::uint32_t>{0}));
    CHECK((seg->frames_for_source(1, "1.2.3.B", 1) == std::vector<std::uint32_t>{1}));
    CHECK(seg->frames_for_source(1, "1.2.3.B", 2).empty());
    CHECK((seg->frames_for_source(2, "1.2.3.A") == std::vector<std::uint32_t>{2}));
    CHECK(seg->frames_for_source(2, "1.2.3.B").empty());
    PlaneGeometry plane;
    plane.has_position = plane.has_orientation = true;
    plane.normal = {0, 0, 1};
    plane.position = {5, 5, 1.0};
    plane.frame_of_reference_uid = "1.2.3.4";
    CHECK((seg->frames_on_plane(1, plane) == std::vector<std::uint32_t>{1}));
    plane.position.z = 0;
    CHECK((seg->frames_on_plane(2, plane) == std::vector<std::uint32_t>{2}));
    plane.frame_of_reference_uid = "other";
    CHECK(seg->frames_on_plane(2, plane).empty());
    auto m0 = segment_mask(d, kTs, *seg, 0);
    auto m1 = segment_mask(d, kTs, *seg, 1);
    auto m2 = segment_mask(d, kTs, *seg, 2);
    REQUIRE(m0.is_ok() && m1.is_ok() && m2.is_ok());
    CHECK((*m0 == std::vector<std::uint8_t>{1, 0, 0, 0, 1, 0, 0, 0, 1}));
    CHECK((*m1 == std::vector<std::uint8_t>(9, 1)));
    CHECK((*m2 == std::vector<std::uint8_t>{0, 1, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(!segment_mask(d, kTs, *seg, 3).is_ok());
}

TEST_CASE("seg: FRACTIONAL and LABELMAP") {
    Dataset f = seg_common("FRACTIONAL", 2, 2, 1, 8);
    put(f, ct::SegmentationFractionalType, VR::CS, "PROBABILITY");
    (void)f.set_uint16(ct::MaximumFractionalValue, 255);
    f.put(make_sequence(ct::SegmentSequence, {segment_item(1, "P", {50000, 32896, 32896})}));
    f.put(make_sequence(0x52009230, {per_frame(1, "1.2.3.A", 0)}));
    const std::uint8_t v[4] = {0, 128, 255, 64};
    (void)f.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(v, 4));
    auto seg = read_segmentation(f);
    REQUIRE(seg.is_ok());
    CHECK(seg->type == SegmentationType::Fractional && seg->max_fractional == 255 && seg->fractional_type == "PROBABILITY");
    auto m = segment_mask(f, kTs, *seg, 0);
    REQUIRE(m.is_ok());
    CHECK((*m == std::vector<std::uint8_t>{0, 128, 255, 64}));
    Dataset l = seg_common("LABELMAP", 2, 2, 1, 8);
    l.put(make_sequence(ct::SegmentSequence, {segment_item(1, "A", {0, 0, 0}), segment_item(3, "B", {0, 0, 0})}));
    const std::uint8_t lv[4] = {0, 1, 3, 3};
    (void)l.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(lv, 4));
    auto ls = read_segmentation(l);
    REQUIRE(ls.is_ok());
    CHECK(ls->segment(3) && ls->segment(3)->frames.empty() && ls->frame_count(*ls->segment(3)) == 1);  // R-054
    CHECK((segment_mask(l, kTs, *ls, 0, 3).value() == std::vector<std::uint8_t>{0, 0, 1, 1}));
    CHECK((segment_mask(l, kTs, *ls, 0, 0).value() == std::vector<std::uint8_t>{0, 1, 3, 3}));
}

TEST_CASE("seg: malformed segmentations") {
    Dataset d = binary_seg();
    put(d, ct::SegmentationType, VR::CS, "WHATEVER");
    CHECK(read_segmentation(d).error().code == Errc::Unsupported);
    Dataset e = binary_seg();
    e.find(0x52009230)->items[1].remove(ct::SegmentIdentificationSequence);
    e.find(ct::SegmentSequence)->items.push_back(segment_item(1, "dup", {0, 0, 0}));
    auto s = read_segmentation(e);
    REQUIRE(s.is_ok());
    CHECK(s->warnings.size() >= 2);
    CHECK(s->frames[1].segment == 0);
    CHECK(!read_segmentation(e, false).is_ok());
    Dataset f = seg_common("FRACTIONAL", 2, 2, 1, 1);  // FRACTIONAL with 1-bit data
    const std::uint8_t b = 0xF;
    (void)f.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(&b, 1));
    auto fs = read_segmentation(f);
    REQUIRE(fs.is_ok());
    CHECK(fs->max_fractional == 255);  // assumed, with a warning
    CHECK(!fs->warnings.empty());
    CHECK(!segment_mask(f, kTs, *fs, 0).is_ok());
    Dataset g = binary_seg();
    put(g, siaara::tags::NumberOfFrames, VR::IS, "0");
    CHECK(!read_segmentation(g).is_ok());
    Dataset h = binary_seg();
    (void)h.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(&b, 1));  // too short
    auto hs = read_segmentation(h);
    REQUIRE(hs.is_ok());
    CHECK(!segment_mask(h, kTs, *hs, 2).is_ok());
}

TEST_CASE("rtstruct: ROIs, contours, colours and image mapping") {
    auto ss = read_rtstruct(rtstruct());
    REQUIRE(ss.is_ok());
    CHECK(ss->label == "PLAN1");
    REQUIRE(ss->rois.size() == 2);
    const RtRoi& ptv = ss->rois[0];
    CHECK(ptv.number == 1 && ptv.name == "PTV" && ptv.interpreted_type == "PTV" && ptv.frame_of_reference_uid == "1.2.3.4");
    CHECK((ptv.color == std::array<std::uint8_t, 3>{255, 0, 0}));
    REQUIRE(ptv.contours.size() == 2);  // the 4-value contour is skipped
    CHECK(ptv.contours[0].points.size() == 3 && ptv.contours[0].points[2].y == 40.0);
    CHECK(ptv.contours[0].images.size() == 1);
    CHECK(!ss->rois[1].color.has_value());
    CHECK(ss->warnings.size() == 2);  // bad contour data + undefined ROI
    // Axial image at z = 0, 1 mm pixels, slice thickness 1 mm -> plane tolerance 0.5 mm.
    PlaneGeometry g;
    g.rows = g.columns = 64;
    g.has_position = g.has_orientation = true;
    g.row_cosine = {1, 0, 0};
    g.column_cosine = {0, 1, 0};
    g.normal = {0, 0, 1};
    g.spacing.row = g.spacing.column = 1.0;
    g.spacing.source = SpacingSource::PixelSpacing;
    g.slice_thickness = 1.0;
    g.frame_of_reference_uid = "1.2.3.4";
    auto on = contours_on_image(*ss, g, "1.2.3.100");
    REQUIRE(on.size() == 2);
    CHECK(on[0].roi == 0 && on[0].contour == 0 && on[0].by_reference && on[0].closed);
    CHECK(on[0].points[0].x == 10.5 && on[0].points[0].y == 20.5);
    CHECK(on[1].roi == 1 && !on[1].by_reference && !on[1].closed);  // POINT at z = 0.2 by plane
    g.position.z = 2.0;
    auto on2 = contours_on_image(*ss, g, "1.2.3.101");
    REQUIRE(on2.size() == 1);
    CHECK(on2[0].contour == 1);
    RtReadOptions strict;
    strict.lenient = false;
    CHECK(!read_rtstruct(rtstruct(), strict).is_ok());
    RtReadOptions small;
    small.max_points = 3;
    CHECK(read_rtstruct(rtstruct(), small).error().code == Errc::LimitExceeded);
    CHECK(read_rtstruct(Dataset{}).error().code == Errc::NotFound);
}

TEST_CASE("seg/rtstruct: C API") {
    siaara_file* f = to_capi(binary_seg());
    REQUIRE(f != nullptr);
    siaara_seg* seg = nullptr;
    REQUIRE(siaara_seg_open(f, &seg) == SIAARA_OK);
    int32_t type = -1;
    std::uint32_t frames = 0, maxf = 0, count = 0;
    CHECK_EQ(siaara_seg_info(seg, &type, &frames, &maxf), SIAARA_OK);
    CHECK(type == SIAARA_SEG_BINARY && frames == 3);
    CHECK_EQ(siaara_seg_segment_count(seg, &count), SIAARA_OK);
    CHECK_EQ(count, 2u);
    siaara_segment_info si{};
    si.struct_size = sizeof si;
    CHECK_EQ(siaara_seg_segment(seg, 1, &si), SIAARA_OK);
    CHECK(si.number == 2 && si.frame_count == 1 && si.has_color == 1 && si.rgb[0] >= 254);
    char label[16];
    size_t need = 0;
    CHECK_EQ(siaara_seg_segment_label(seg, 0, label, sizeof label, &need), SIAARA_OK);
    CHECK(std::strcmp(label, "Liver") == 0);
    std::uint32_t fr[4];
    CHECK_EQ(siaara_seg_frames_for_source(seg, 1, "1.2.3.B", 7, 0, fr, 4, &need), SIAARA_OK);
    CHECK(need == 1 && fr[0] == 1);
    std::uint8_t mask[9];
    CHECK_EQ(siaara_seg_mask(f, seg, 0, 0, mask, 8, &need), SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(siaara_seg_mask(f, seg, 0, 0, mask, 9, &need), SIAARA_OK);
    CHECK(mask[0] == 1 && mask[1] == 0 && mask[4] == 1);
    siaara_seg_free(seg);
    siaara_file_free(f);

    siaara_file* r = to_capi(rtstruct());
    REQUIRE(r != nullptr);
    siaara_rtstruct* rt = nullptr;
    REQUIRE(siaara_rtstruct_open(r, &rt) == SIAARA_OK);
    CHECK_EQ(siaara_rtstruct_roi_count(rt, &count), SIAARA_OK);
    CHECK_EQ(count, 2u);
    siaara_rt_roi_info ri{};
    ri.struct_size = sizeof ri;
    CHECK_EQ(siaara_rtstruct_roi(rt, 0, &ri), SIAARA_OK);
    CHECK(ri.number == 1 && ri.contour_count == 2 && ri.has_color == 1 && ri.rgb[0] == 255);
    char name[16];
    CHECK_EQ(siaara_rtstruct_roi_name(rt, 1, name, sizeof name, &need), SIAARA_OK);
    CHECK(std::strcmp(name, "Cord") == 0);
    double xyz[9];
    int32_t closed = 0;
    CHECK_EQ(siaara_rtstruct_contour_points(rt, 0, 0, xyz, 9, &need, &closed), SIAARA_OK);
    CHECK(need == 9 && closed == 1 && xyz[3] == 30.0);
    CHECK_EQ(siaara_rtstruct_contour_points(rt, 0, 5, xyz, 9, &need, &closed), SIAARA_ERR_INVALID_ARGUMENT);
    siaara_rtstruct_free(rt);
    siaara_file_free(r);
}

TEST_CASE("seg/rtstruct: mutated files never crash the readers") {
    std::uint32_t seed = 7;
    auto next = [&]() { return seed = seed * 1664525u + 1013904223u; };
    for (int which = 0; which < 2; ++which) {
        DicomFile f;
        f.dataset = which ? rtstruct() : binary_seg();
        f.transfer_syntax = std::string(kTs);
        std::vector<std::uint8_t> bytes;
        REQUIRE(write_part10(f, bytes).is_ok());
        int parsed = 0;
        for (int iter = 0; iter < 300; ++iter) {
            std::vector<std::uint8_t> m = bytes;
            const int edits = 1 + static_cast<int>(next() % 8);
            for (int e = 0; e < edits; ++e) m[132 + next() % (m.size() - 132)] = static_cast<std::uint8_t>(next() >> 24);
            auto r = read_part10(Bytes::copy_of(m.data(), m.size()));
            if (!r) continue;
            if (which) {
                auto ss = read_rtstruct(r->dataset);
                if (!ss) continue;
                ++parsed;
                PlaneGeometry g;
                g.rows = g.columns = 8;
                g.has_position = g.has_orientation = true;
                g.row_cosine = {1, 0, 0};
                g.column_cosine = {0, 1, 0};
                g.normal = {0, 0, 1};
                g.spacing.row = g.spacing.column = 1;
                g.spacing.source = SpacingSource::PixelSpacing;
                (void)contours_on_image(*ss, g, "1.2.3.100");
            } else {
                auto seg = read_segmentation(r->dataset);
                if (!seg) continue;
                ++parsed;
                for (std::uint32_t k = 0; k < seg->number_of_frames && k < 8; ++k) (void)segment_mask(r->dataset, kTs, *seg, k);
            }
        }
        CHECK(parsed > 0);
    }
}
