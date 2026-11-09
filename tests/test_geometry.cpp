// Clinical layer: plane geometry, pixel spacing resolution, orientation labels, reference lines
// and series sorting (src/clinical/geometry.*), with hand-computed expected values.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/geometry.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }

Dataset plane_ds(std::string_view ipp, std::string_view iop, std::string_view ps, int rows = 100, int cols = 100,
                 std::string_view forf = "1.2.3.4") {
    Dataset d;
    (void)d.set_uint16(siaara::tags::Rows, static_cast<std::uint16_t>(rows));
    (void)d.set_uint16(siaara::tags::Columns, static_cast<std::uint16_t>(cols));
    if (!ipp.empty()) put(d, ct::ImagePositionPatient, VR::DS, ipp);
    if (!iop.empty()) put(d, ct::ImageOrientationPatient, VR::DS, iop);
    if (!ps.empty()) put(d, ct::PixelSpacing, VR::DS, ps);
    if (!forf.empty()) put(d, ct::FrameOfReferenceUid, VR::UI, forf);
    return d;
}

Dataset axial(double z, int instance, std::string_view forf = "1.2.3.4", double x = 0, double y = 0) {
    Dataset d = plane_ds(format_ds(x) + "\\" + format_ds(y) + "\\" + format_ds(z), "1\\0\\0\\0\\1\\0", "1\\1", 100, 100, forf);
    put(d, ct::InstanceNumber, VR::IS, std::to_string(instance));
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.4.5." + std::to_string(instance));
    return d;
}

Element seq(Tag t, std::vector<Dataset> items) { return make_sequence(t, std::move(items)); }
Dataset item_with(Tag t, VR vr, std::string_view v) {
    Dataset d;
    put(d, t, vr, v);
    return d;
}

std::vector<const Dataset*> ptrs(const std::vector<Dataset>& v) {
    std::vector<const Dataset*> p;
    for (const auto& d : v) p.push_back(&d);
    return p;
}

std::vector<std::uint32_t> instance_order(const SeriesSort& s) {
    std::vector<std::uint32_t> out;
    for (auto i : s.order) out.push_back(s.items[i].instance);
    return out;
}

siaara_file* to_capi(const Dataset& ds) {
    DicomFile f;
    f.dataset = ds;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    std::vector<std::uint8_t> bytes;
    if (!write_part10(f, bytes)) return nullptr;
    siaara_file* out = nullptr;
    if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &out) != SIAARA_OK) return nullptr;
    return out;
}

}  // namespace

TEST_CASE("geometry: classic plane, pixel <-> patient (C.7.6.2.1.1)") {
    // Pixel Spacing 0.5\0.8: rows 0.5 mm apart, columns 0.8 mm apart.
    Dataset d = plane_ds("-100\\-120\\50", "1\\0\\0\\0\\1\\0", "0.5\\0.8", 256, 512);
    put(d, ct::SliceThickness, VR::DS, "2.5");
    auto g = frame_geometry(d, 0);
    REQUIRE(g.is_ok());
    CHECK(g->complete());
    CHECK_EQ(g->rows, 256u);
    CHECK_EQ(g->columns, 512u);
    CHECK(g->position_from == GeometrySource::Classic);
    CHECK(g->spacing.source == SpacingSource::PixelSpacing);
    CHECK(near(g->spacing.row, 0.5));
    CHECK(near(g->spacing.column, 0.8));
    CHECK(g->slice_thickness && near(*g->slice_thickness, 2.5));
    CHECK(near(g->normal.z, 1.0));
    // column 10, row 20: x = -100 + 10 * 0.8 = -92, y = -120 + 20 * 0.5 = -110, z = 50.
    const Vec3 p = g->pixel_to_patient(10, 20);
    CHECK(near(p.x, -92.0));
    CHECK(near(p.y, -110.0));
    CHECK(near(p.z, 50.0));
    double c = 0, r = 0, dist = 0;
    CHECK(g->patient_to_pixel({-92.0, -110.0, 53.0}, c, r, &dist));
    CHECK(near(c, 10.0));
    CHECK(near(r, 20.0));
    CHECK(near(dist, 3.0));
    CHECK(frame_geometry(d, 1).is_ok() == false);  // past Number of Frames
}

TEST_CASE("geometry: oblique plane round trip") {
    // Row cosine (cos30, sin30, 0), column cosine (0, 0, -1).
    const double c30 = std::sqrt(3.0) / 2.0;
    Dataset d = plane_ds("10\\20\\30", format_ds(c30) + "\\0.5\\0\\0\\0\\-1", "0.7\\0.9");
    auto g = frame_geometry(d);
    REQUIRE(g.is_ok());
    const Vec3 p = g->pixel_to_patient(3.25, 7.5);
    // P = S + X * 0.9 * 3.25 + Y * 0.7 * 7.5
    CHECK(near(p.x, 10 + c30 * 0.9 * 3.25, 1e-6));
    CHECK(near(p.y, 20 + 0.5 * 0.9 * 3.25, 1e-6));
    CHECK(near(p.z, 30 - 0.7 * 7.5, 1e-9));
    double c = 0, r = 0;
    CHECK(g->patient_to_pixel(p, c, r));
    CHECK(near(c, 3.25, 1e-6));
    CHECK(near(r, 7.5, 1e-6));
}

TEST_CASE("geometry: enhanced functional groups, per-frame before shared") {
    Dataset d;
    (void)d.set_uint16(siaara::tags::Rows, 64);
    (void)d.set_uint16(siaara::tags::Columns, 32);
    put(d, siaara::tags::NumberOfFrames, VR::IS, "2");
    put(d, ct::FrameOfReferenceUid, VR::UI, "1.2.9");
    Dataset shared;
    {
        Dataset pm;
        put(pm, ct::PixelSpacing, VR::DS, "1.5\\1.25");
        put(pm, ct::SliceThickness, VR::DS, "3");
        shared.put(seq(ct::PixelMeasuresSequence, {pm}));
        shared.put(seq(ct::PlaneOrientationSequence, {item_with(ct::ImageOrientationPatient, VR::DS, "1\\0\\0\\0\\1\\0")}));
    }
    d.put(seq(0x52009229, {shared}));
    std::vector<Dataset> per;
    for (int f = 0; f < 2; ++f) {
        Dataset it;
        it.put(seq(ct::PlanePositionSequence, {item_with(ct::ImagePositionPatient, VR::DS, "0\\0\\" + std::to_string(f * 4))}));
        if (f == 1) {
            Dataset pm;
            put(pm, ct::PixelSpacing, VR::DS, "0.5\\0.5");
            it.put(seq(ct::PixelMeasuresSequence, {pm}));
        }
        per.push_back(it);
    }
    d.put(seq(0x52009230, per));
    // Classic attributes that must be ignored in favour of the functional groups.
    put(d, ct::PixelSpacing, VR::DS, "9\\9");
    auto g0 = frame_geometry(d, 0), g1 = frame_geometry(d, 1);
    REQUIRE(g0.is_ok() && g1.is_ok());
    CHECK(g0->position_from == GeometrySource::PerFrame);
    CHECK(g0->orientation_from == GeometrySource::Shared);
    CHECK(g0->spacing.from == GeometrySource::Shared);
    CHECK(near(g0->spacing.row, 1.5) && near(g0->spacing.column, 1.25));
    CHECK(g1->spacing.from == GeometrySource::PerFrame);
    CHECK(near(g1->spacing.row, 0.5));
    CHECK(near(g1->position.z, 4.0));
    CHECK(g0->slice_thickness && near(*g0->slice_thickness, 3.0));
    CHECK_EQ(g0->frame_of_reference_uid, std::string("1.2.9"));
}

TEST_CASE("geometry: pixel spacing calibration sources") {
    {  // Imager Pixel Spacing only: detector plane.
        Dataset d;
        put(d, ct::ImagerPixelSpacing, VR::DS, "0.2\\0.2");
        auto s = resolve_pixel_spacing(d);
        CHECK(s.source == SpacingSource::ImagerPixelSpacing);
        CHECK(s.at_detector);
        CHECK(near(s.row, 0.2));
    }
    {  // Pixel Spacing equal to Imager Pixel Spacing with ERMF 1.25: 0.2 / 1.25 = 0.16 mm.
        Dataset d;
        put(d, ct::PixelSpacing, VR::DS, "0.2\\0.2");
        put(d, ct::ImagerPixelSpacing, VR::DS, "0.2\\0.2");
        put(d, ct::EstimatedRadiographicMagnificationFactor, VR::DS, "1.25");
        auto s = resolve_pixel_spacing(d);
        CHECK(s.source == SpacingSource::ImagerPixelSpacingMagnified);
        CHECK(!s.at_detector);
        CHECK(near(s.row, 0.16) && near(s.column, 0.16));
        CHECK(near(s.magnification, 1.25));
    }
    {  // Same without ERMF: detector-plane Pixel Spacing.
        Dataset d;
        put(d, ct::PixelSpacing, VR::DS, "0.2\\0.2");
        put(d, ct::ImagerPixelSpacing, VR::DS, "0.2\\0.2");
        auto s = resolve_pixel_spacing(d);
        CHECK(s.source == SpacingSource::PixelSpacing);
        CHECK(s.at_detector);
    }
    {  // Calibrated Pixel Spacing.
        Dataset d;
        put(d, ct::PixelSpacing, VR::DS, "0.15\\0.15");
        put(d, ct::ImagerPixelSpacing, VR::DS, "0.2\\0.2");
        put(d, ct::PixelSpacingCalibrationType, VR::CS, "GEOMETRY");
        auto s = resolve_pixel_spacing(d);
        CHECK(s.source == SpacingSource::PixelSpacing);
        CHECK(s.calibrated);
        CHECK_EQ(s.calibration_type, std::string("GEOMETRY"));
        CHECK(!s.at_detector);
    }
    {  // Nominal Scanned Pixel Spacing.
        Dataset d;
        put(d, ct::NominalScannedPixelSpacing, VR::DS, "0.1\\0.3");
        auto s = resolve_pixel_spacing(d);
        CHECK(s.source == SpacingSource::NominalScannedPixelSpacing);
        CHECK(near(s.row, 0.1) && near(s.column, 0.3));
    }
    {  // Ultrasound regions: deltas in cm; 0.01 cm = 0.1 mm.
        auto region = [](int x0, int y0, int x1, int y1, std::string_view dx, std::string_view dy, int units = 3) {
            Dataset r;
            (void)r.set_uint16(ct::RegionSpatialFormat, 1);
            (void)r.set_uint16(ct::RegionDataType, 1);
            (void)r.set_uint32(ct::RegionLocationMinX0, static_cast<std::uint32_t>(x0));
            (void)r.set_uint32(ct::RegionLocationMinY0, static_cast<std::uint32_t>(y0));
            (void)r.set_uint32(ct::RegionLocationMaxX1, static_cast<std::uint32_t>(x1));
            (void)r.set_uint32(ct::RegionLocationMaxY1, static_cast<std::uint32_t>(y1));
            (void)r.set_uint16(ct::PhysicalUnitsXDirection, static_cast<std::uint16_t>(units));
            (void)r.set_uint16(ct::PhysicalUnitsYDirection, static_cast<std::uint16_t>(units));
            const double x = std::stod(std::string(dx)), y = std::stod(std::string(dy));
            (void)r.set_bytes(ct::PhysicalDeltaX, VR::FD, encode_fd(&x, 1));
            (void)r.set_bytes(ct::PhysicalDeltaY, VR::FD, encode_fd(&y, 1));
            return r;
        };
        Dataset same;
        same.put(seq(ct::SequenceOfUltrasoundRegions, {region(0, 0, 99, 99, "0.01", "0.02"), region(100, 0, 199, 99, "0.01", "0.02")}));
        auto s = resolve_pixel_spacing(same);
        CHECK(s.source == SpacingSource::UltrasoundRegion);
        CHECK(near(s.column, 0.1) && near(s.row, 0.2));
        Dataset diff;
        diff.put(seq(ct::SequenceOfUltrasoundRegions, {region(0, 0, 99, 99, "0.01", "0.01"), region(100, 0, 199, 99, "0.05", "0.05"),
                                                         region(0, 100, 99, 199, "0.5", "0.5", 4 /* seconds */)}));
        CHECK(resolve_pixel_spacing(diff).source == SpacingSource::None);  // ambiguous without a point
        const double pt[2] = {150.0, 50.0};
        auto s2 = resolve_pixel_spacing(diff, 0, pt);
        CHECK(s2.source == SpacingSource::UltrasoundRegion);
        CHECK_EQ(s2.ultrasound_region, 1);
        CHECK(near(s2.row, 0.5));
        const double pt3[2] = {50.0, 150.0};  // inside the time-units region only
        CHECK(resolve_pixel_spacing(diff, 0, pt3).source == SpacingSource::None);
    }
    {  // Nothing.
        Dataset d;
        put(d, ct::PixelSpacing, VR::DS, "0\\0.5");  // invalid value: ignored
        CHECK(resolve_pixel_spacing(d).source == SpacingSource::None);
    }
}

TEST_CASE("geometry: orientation labels, including oblique") {
    CHECK_EQ(orientation_label({1, 0, 0}), std::string("L"));
    CHECK_EQ(orientation_label({-1, 0, 0}), std::string("R"));
    CHECK_EQ(orientation_label({0, 1, 0}), std::string("P"));
    CHECK_EQ(orientation_label({0, -1, 0}), std::string("A"));
    CHECK_EQ(orientation_label({0, 0, 1}), std::string("H"));
    CHECK_EQ(orientation_label({0, 0, -1}), std::string("F"));
    CHECK_EQ(orientation_label({0.8, 0, 0.6}), std::string("LH"));
    CHECK_EQ(orientation_label({0.6, -0.48, 0.64}), std::string("HLA"));
    CHECK_EQ(orientation_label({0.6, -0.48, 0.64}, 0.5), std::string("HL"));
    CHECK_EQ(orientation_label({0.6, -0.48, 0.64}, 1e-4, 1), std::string("H"));
    CHECK_EQ(orientation_label({0, 0, 0}), std::string(""));
    // Axial: right edge = patient left.
    auto ga = frame_geometry(plane_ds("0\\0\\0", "1\\0\\0\\0\\1\\0", "1\\1"));
    REQUIRE(ga.is_ok());
    auto ea = edge_labels(*ga);
    REQUIRE(ea.has_value());
    CHECK(ea->right == "L" && ea->left == "R" && ea->top == "A" && ea->bottom == "P");
    // Sagittal: row towards posterior, column towards feet.
    auto gs = frame_geometry(plane_ds("0\\0\\0", "0\\1\\0\\0\\0\\-1", "1\\1"));
    auto es = edge_labels(*gs);
    REQUIRE(es.has_value());
    CHECK(es->right == "P" && es->left == "A" && es->top == "H" && es->bottom == "F");
    // Coronal.
    auto gc = frame_geometry(plane_ds("0\\0\\0", "1\\0\\0\\0\\0\\-1", "1\\1"));
    auto ec = edge_labels(*gc);
    CHECK(ec && ec->right == "L" && ec->top == "H");
    // No orientation.
    auto gn = frame_geometry(plane_ds("0\\0\\0", "", "1\\1"));
    CHECK(!edge_labels(*gn).has_value());
}

TEST_CASE("geometry: reference lines") {
    // Target: axial at z = 0, 100 x 100, 1 mm pixels. Other: sagittal at x = 50 with rows going
    // down in z from z = 20 (40 rows), columns along +y.
    auto t = frame_geometry(plane_ds("0\\0\\0", "1\\0\\0\\0\\1\\0", "1\\1"));
    auto o = frame_geometry(plane_ds("50\\0\\20", "0\\1\\0\\0\\0\\-1", "1\\1", 40, 100));
    REQUIRE(t.is_ok() && o.is_ok());
    auto rl = reference_line(*t, *o);
    REQUIRE(rl.valid);
    CHECK(rl.same_frame_of_reference);
    CHECK(near(rl.col0, 50.0) && near(rl.col1, 50.0));
    CHECK(near(std::min(rl.row0, rl.row1), -0.5) && near(std::max(rl.row0, rl.row1), 99.5));
    // Other image shorter in y (columns 0..29): clipped to rows -0.5 .. 29.5.
    auto o2 = frame_geometry(plane_ds("50\\0\\20", "0\\1\\0\\0\\0\\-1", "1\\1", 40, 30));
    auto rl2 = reference_line(*t, *o2);
    REQUIRE(rl2.valid);
    CHECK(near(std::max(rl2.row0, rl2.row1), 29.5));
    // ... but not when only the target clip is requested.
    auto rl2b = reference_line(*t, *o2, false);
    CHECK(rl2b.valid && near(std::max(rl2b.row0, rl2b.row1), 99.5));
    // Other image entirely below z = 0: no intersection inside it.
    auto o3 = frame_geometry(plane_ds("50\\0\\-30", "0\\1\\0\\0\\0\\-1", "1\\1", 10, 100));
    CHECK(!reference_line(*t, *o3).valid);
    // Parallel planes.
    auto o4 = frame_geometry(plane_ds("0\\0\\5", "1\\0\\0\\0\\1\\0", "1\\1"));
    auto rl4 = reference_line(*t, *o4);
    CHECK(!rl4.valid && rl4.reason == "planes are parallel");
    // Different Frame of Reference.
    auto o5 = frame_geometry(plane_ds("50\\0\\20", "0\\1\\0\\0\\0\\-1", "1\\1", 40, 100, "9.9"));
    CHECK(!reference_line(*t, *o5).valid);
    CHECK(reference_line(*t, *o5, true, true).valid);
    // Oblique: plane x + z = 50 (normal (1,0,1)/sqrt2) meets z = 0 at x = 50.
    const double h = std::sqrt(0.5);
    auto o6 = frame_geometry(plane_ds("0\\0\\50", "0\\1\\0\\" + format_ds(h) + "\\0\\-" + format_ds(h), "1\\1", 200, 100));
    auto rl6 = reference_line(*t, *o6);
    REQUIRE(rl6.valid);
    CHECK(near(rl6.col0, 50.0, 1e-6) && near(rl6.col1, 50.0, 1e-6));
}

TEST_CASE("geometry: series sort along the normal, then instance number") {
    std::vector<Dataset> v{axial(4, 1), axial(0, 2), axial(8, 3), axial(2, 4), axial(6, 5)};
    auto s = sort_series(ptrs(v));
    CHECK_EQ(s.warnings, 0u);
    CHECK((instance_order(s) == std::vector<std::uint32_t>{1, 3, 0, 4, 2}));
    REQUIRE(s.groups.size() == 1);
    CHECK(near(s.groups[0].spacing, 2.0));
    CHECK(!s.groups[0].uneven && !s.groups[0].duplicates && !s.groups[0].tilted);
}

TEST_CASE("geometry: series sort warnings") {
    {  // duplicate positions: instance number breaks the tie.
        std::vector<Dataset> v{axial(4, 7), axial(0, 1), axial(4, 3), axial(2, 2)};
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortDuplicatePositions) != 0);
        CHECK((instance_order(s) == std::vector<std::uint32_t>{1, 3, 2, 0}));
    }
    {  // uneven spacing
        std::vector<Dataset> v{axial(0, 1), axial(2, 2), axial(4, 3), axial(7, 4), axial(9, 5)};
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortUnevenSpacing) != 0);
        CHECK(near(s.groups[0].spacing, 2.0));
    }
    {  // gantry tilt: y advances 0.35 mm per 2 mm in z -> atan(0.35 / 2) = 9.926 degrees.
        std::vector<Dataset> v;
        for (int k = 0; k < 5; ++k) v.push_back(axial(2.0 * k, k + 1, "1.2.3.4", 0.0, 0.35 * k));
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortGantryTilt) != 0);
        CHECK((s.warnings & kSortMultipleStacks) == 0);
        REQUIRE(s.groups.size() == 1);
        CHECK(near(s.groups[0].tilt_deg, std::atan(0.35 / 2.0) * 180.0 / 3.14159265358979323846, 1e-9));
    }
    {  // mixed orientation: 3 axial + 2 sagittal
        std::vector<Dataset> v{axial(0, 1), axial(2, 2), axial(4, 3)};
        for (int k = 0; k < 2; ++k) {
            Dataset d = plane_ds(std::to_string(10 * k) + "\\0\\0", "0\\1\\0\\0\\0\\-1", "1\\1");
            put(d, ct::InstanceNumber, VR::IS, std::to_string(10 + k));
            v.push_back(d);
        }
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortMixedOrientation) != 0);
        CHECK((s.warnings & kSortMultipleStacks) != 0);
        CHECK_EQ(s.groups.size(), 2u);
        CHECK_EQ(s.order.size(), 5u);
    }
    {  // two parallel stacks (different in-plane offsets), interleaved in z
        std::vector<Dataset> v;
        for (int k = 0; k < 3; ++k) v.push_back(axial(3.0 * k, k + 1, "1.2.3.4", 0, 0));
        for (int k = 0; k < 3; ++k) v.push_back(axial(3.0 * k + 1.5, k + 4, "1.2.3.4", 0, 150));
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortMultipleStacks) != 0);
        CHECK((s.warnings & kSortMixedOrientation) == 0);
        REQUIRE(s.groups.size() == 2);
        CHECK(near(s.groups[0].spacing, 3.0) && near(s.groups[1].spacing, 3.0));
        CHECK(!s.groups[0].uneven && !s.groups[1].uneven);
    }
    {  // missing geometry goes last; multiple frames of reference
        std::vector<Dataset> v{axial(2, 2), axial(0, 1, "7.7"), plane_ds("", "", "1\\1")};
        put(v[2], ct::InstanceNumber, VR::IS, "0");
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortMissingGeometry) != 0);
        CHECK((s.warnings & kSortMultipleFramesOfReference) != 0);
        CHECK_EQ(instance_order(s).back(), 2u);
    }
    {  // size mismatch
        std::vector<Dataset> v{axial(0, 1), axial(1, 2)};
        (void)v[1].set_uint16(siaara::tags::Rows, 50);
        CHECK((sort_series(ptrs(v)).warnings & kSortSizeMismatch) != 0);
    }
}

TEST_CASE("geometry: 4D split by temporal position and by trigger time") {
    {
        std::vector<Dataset> v;
        int n = 1;
        for (int phase = 2; phase >= 1; --phase)
            for (int z = 0; z < 3; ++z) {
                Dataset d = axial(2.0 * z, n++);
                put(d, ct::TemporalPositionIdentifier, VR::IS, std::to_string(phase));
                v.push_back(d);
            }
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortTemporalSplit) != 0);
        CHECK((s.warnings & kSortDuplicatePositions) == 0);
        REQUIRE(s.groups.size() == 2);
        CHECK(s.groups[0].temporal_key && *s.groups[0].temporal_key == 1.0);
        CHECK((instance_order(s) == std::vector<std::uint32_t>{3, 4, 5, 0, 1, 2}));
    }
    {
        std::vector<Dataset> v;
        int n = 1;
        for (int z = 0; z < 3; ++z)
            for (int t = 0; t < 2; ++t) {
                Dataset d = axial(2.0 * z, n++);
                put(d, ct::TriggerTime, VR::DS, t ? "400" : "0");
                v.push_back(d);
            }
        auto s = sort_series(ptrs(v));
        CHECK((s.warnings & kSortTemporalSplit) != 0);
        CHECK((s.warnings & kSortDuplicatePositions) == 0);
        REQUIRE(s.groups.size() == 2);
        CHECK(s.groups[0].temporal_by_trigger);
        CHECK((instance_order(s) == std::vector<std::uint32_t>{0, 2, 4, 1, 3, 5}));
    }
    {  // trigger times that differ per slice but no repeated positions: no split
        std::vector<Dataset> v;
        for (int z = 0; z < 3; ++z) {
            Dataset d = axial(2.0 * z, z + 1);
            put(d, ct::TriggerTime, VR::DS, std::to_string(100 * z));
            v.push_back(d);
        }
        auto s = sort_series(ptrs(v));
        CHECK_EQ(s.warnings, 0u);
        CHECK_EQ(s.groups.size(), 1u);
    }
}

TEST_CASE("geometry: enhanced multi-frame expansion and stack ID") {
    Dataset d;
    (void)d.set_uint16(siaara::tags::Rows, 8);
    (void)d.set_uint16(siaara::tags::Columns, 8);
    put(d, siaara::tags::NumberOfFrames, VR::IS, "4");
    Dataset shared;
    shared.put(seq(ct::PlaneOrientationSequence, {item_with(ct::ImageOrientationPatient, VR::DS, "1\\0\\0\\0\\1\\0")}));
    shared.put(seq(ct::PixelMeasuresSequence, {item_with(ct::PixelSpacing, VR::DS, "1\\1")}));
    d.put(seq(0x52009229, {shared}));
    const char* z[4] = {"4", "0", "2", "0"};
    const char* stack[4] = {"1", "1", "1", "2"};
    std::vector<Dataset> per;
    for (int f = 0; f < 4; ++f) {
        Dataset it;
        it.put(seq(ct::PlanePositionSequence, {item_with(ct::ImagePositionPatient, VR::DS, std::string("0\\0\\") + z[f])}));
        it.put(seq(ct::FrameContentSequence, {item_with(ct::StackId, VR::SH, stack[f])}));
        per.push_back(it);
    }
    d.put(seq(0x52009230, per));
    const Dataset* p = &d;
    auto s = sort_series(std::span<const Dataset* const>(&p, 1));
    REQUIRE(s.order.size() == 4);
    std::vector<std::uint32_t> frames;
    for (auto i : s.order) frames.push_back(s.items[i].frame);
    CHECK((frames == std::vector<std::uint32_t>{1, 2, 0, 3}));
    CHECK((s.warnings & kSortMultipleStacks) != 0);
    CHECK_EQ(s.groups.size(), 2u);
    SeriesSortOptions o;
    o.expand_frames = false;
    CHECK_EQ(sort_series(std::span<const Dataset* const>(&p, 1), o).order.size(), 1u);
}

TEST_CASE("geometry: malformed input never crashes") {
    std::vector<Dataset> v;
    v.push_back(plane_ds("abc\\1", "1\\0\\0\\0\\1\\0", "1\\1"));
    v.push_back(plane_ds("0\\0\\0", "0\\0\\0\\0\\0\\0", "1\\1"));
    v.push_back(plane_ds("0\\0\\0", "1\\0\\0\\1\\0\\0", "1\\1"));  // parallel cosines
    v.push_back(plane_ds("1e999\\0\\0", "1\\0\\0\\0\\1\\0", "-1\\x"));
    v.push_back(plane_ds("0\\0\\0", "1\\0\\0\\0.1\\0.9\\0", "1\\1"));  // not orthogonal / unit
    for (const auto& d : v) {
        auto g = frame_geometry(d);
        CHECK(g.is_ok());
        (void)edge_labels(*g);
    }
    auto g2 = frame_geometry(v[2]);
    CHECK(!g2->has_orientation);
    auto g4 = frame_geometry(v[4]);
    CHECK(!g4->orthogonal);
    CHECK(!g4->warnings.empty());
    auto s = sort_series(ptrs(v));
    CHECK((s.warnings & kSortMissingGeometry) != 0);
    CHECK_EQ(s.order.size(), v.size());
    Dataset bad;
    put(bad, siaara::tags::NumberOfFrames, VR::IS, "-5");
    CHECK(frame_geometry(bad, 0).is_ok());
    const Dataset* none = nullptr;
    CHECK(sort_series(std::span<const Dataset* const>(&none, 1)).order.empty());
}

TEST_CASE("geometry: C API frame geometry, labels and series sort") {
    std::vector<Dataset> v{axial(4, 1), axial(0, 2), axial(2, 3)};
    std::vector<siaara_file*> files;
    for (const auto& d : v) files.push_back(to_capi(d));
    for (auto* f : files) REQUIRE(f != nullptr);
    siaara_plane_geometry g{};
    g.struct_size = sizeof g;
    CHECK_EQ(siaara_frame_geometry(files[0], 0, &g), SIAARA_OK);
    CHECK(g.has_position == 1 && g.has_orientation == 1);
    CHECK(near(g.position[2], 4.0));
    CHECK(near(g.pixel_spacing[0], 1.0));
    CHECK_EQ(g.spacing_source, SIAARA_SPACING_PIXEL_SPACING);
    siaara_plane_geometry small{};
    small.struct_size = 8;
    CHECK_EQ(siaara_frame_geometry(files[0], 0, &small), SIAARA_ERR_INVALID_ARGUMENT);
    siaara_edge_labels e{};
    e.struct_size = sizeof e;
    CHECK_EQ(siaara_orientation_labels(files[0], 0, &e), SIAARA_OK);
    CHECK(std::strcmp(e.right, "L") == 0 && std::strcmp(e.top, "A") == 0);
    std::uint32_t inst[3] = {}, frame[3] = {}, group[3] = {}, warn = 99;
    size_t need = 0;
    CHECK_EQ(siaara_series_sort(files.data(), files.size(), 0, inst, frame, group, 1, &need, &warn), SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(need, 3u);
    CHECK_EQ(siaara_series_sort(files.data(), files.size(), 0, inst, frame, group, 3, &need, &warn), SIAARA_OK);
    CHECK(inst[0] == 1 && inst[1] == 2 && inst[2] == 0);
    CHECK_EQ(warn, 0u);
    CHECK_EQ(siaara_series_sort(nullptr, 2, 0, inst, nullptr, nullptr, 3, &need, nullptr), SIAARA_ERR_INVALID_ARGUMENT);
    for (auto* f : files) siaara_file_free(f);
}
