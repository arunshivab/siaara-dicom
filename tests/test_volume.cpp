// Volume building tests (src/volume/volume.*, geometry.*): sorting, validation, per-frame
// rescale, voxel type choice, gantry tilt, 4D splitting, enhanced multi-frame, robustness, C API.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "volume/geometry.hpp"
#include "volume/volume.hpp"

using namespace siaara;
using namespace siaara::volume;

namespace {

std::string ds3(Vec3 v) {
    char b[128];
    std::snprintf(b, sizeof b, "%.6f\\%.6f\\%.6f", v.x, v.y, v.z);
    return b;
}
std::string ds6(Vec3 a, Vec3 c) {
    char b[192];
    std::snprintf(b, sizeof b, "%.8f\\%.8f\\%.8f\\%.8f\\%.8f\\%.8f", a.x, a.y, a.z, c.x, c.y, c.z);
    return b;
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

struct SliceSpec {
    std::uint16_t rows = 4, cols = 5;
    Vec3 ipp;
    Vec3 row{1, 0, 0}, col{0, 1, 0};
    double ps_row = 0.8, ps_col = 0.7;
    bool geometry = true;
    bool is_signed = true;
    std::uint16_t bits_stored = 16;
    double slope = 1.0, intercept = 0.0;
    int instance = 1;
    std::optional<int> temporal_id;
    std::optional<double> slice_thickness;
    std::vector<std::int32_t> stored;  // rows * cols stored values
};

void set_pixel_module(Dataset& ds, std::uint16_t rows, std::uint16_t cols, bool is_signed, std::uint16_t bits_stored) {
    (void)ds.set_uint16(siaara::tags::Rows, rows);
    (void)ds.set_uint16(siaara::tags::Columns, cols);
    (void)ds.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)ds.set_uint16(siaara::tags::BitsAllocated, 16);
    (void)ds.set_uint16(siaara::tags::BitsStored, bits_stored);
    (void)ds.set_uint16(siaara::tags::HighBit, static_cast<std::uint16_t>(bits_stored - 1));
    (void)ds.set_uint16(siaara::tags::PixelRepresentation, is_signed ? 1 : 0);
    (void)ds.set_string(siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
}

void set_pixels(Dataset& ds, const std::vector<std::int32_t>& stored) {
    std::vector<std::uint8_t> b;
    for (std::int32_t v : stored) {
        const auto u = static_cast<std::uint16_t>(v & 0xFFFF);
        b.push_back(static_cast<std::uint8_t>(u & 0xFF));
        b.push_back(static_cast<std::uint8_t>(u >> 8));
    }
    (void)ds.set_bytes(siaara::tags::PixelData, VR::OW, Bytes::copy_of(b.data(), b.size()));
}

DicomFile make_slice(const SliceSpec& s) {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    (void)ds.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25." + std::to_string(1000 + s.instance));
    (void)ds.set_string(siaara::tags::Modality, VR::CS, "CT");
    set_pixel_module(ds, s.rows, s.cols, s.is_signed, s.bits_stored);
    if (s.geometry) {
        (void)ds.set_string(siaara::volume::tags::ImagePositionPatient, VR::DS, ds3(s.ipp));
        (void)ds.set_string(siaara::volume::tags::ImageOrientationPatient, VR::DS, ds6(s.row, s.col));
        (void)ds.set_string(siaara::volume::tags::PixelSpacing, VR::DS, num(s.ps_row) + "\\" + num(s.ps_col));
    }
    if (s.slice_thickness) (void)ds.set_string(siaara::volume::tags::SliceThickness, VR::DS, num(*s.slice_thickness));
    (void)ds.set_string(siaara::volume::tags::InstanceNumber, VR::IS, std::to_string(s.instance));
    if (s.temporal_id) (void)ds.set_string(siaara::volume::tags::TemporalPositionIdentifier, VR::IS, std::to_string(*s.temporal_id));
    (void)ds.set_string(siaara::tags::RescaleSlope, VR::DS, num(s.slope));
    (void)ds.set_string(siaara::tags::RescaleIntercept, VR::DS, num(s.intercept));
    set_pixels(ds, s.stored);
    return f;
}

// Stored value of a synthetic series at (i, j, k).
std::int32_t pattern(std::uint32_t i, std::uint32_t j, std::uint32_t k) {
    return static_cast<std::int32_t>(i * 7 + j * 13 + k * 101) - 200;
}

std::vector<std::int32_t> slice_values(std::uint16_t rows, std::uint16_t cols, std::uint32_t k) {
    std::vector<std::int32_t> v;
    for (std::uint32_t j = 0; j < rows; ++j)
        for (std::uint32_t i = 0; i < cols; ++i) v.push_back(pattern(i, j, k));
    return v;
}

// A series of n slices along `normal` direction spacing dz; slice k has slope 1 + k % 2 and
// intercept -1024 + k. Returned in shuffled order.
std::vector<DicomFile> make_series(std::uint32_t n, double dz, std::uint32_t seed = 1, Vec3 row = {1, 0, 0},
                                   Vec3 col = {0, 1, 0}, Vec3 shift_per_slice = {}) {
    std::vector<DicomFile> files;
    const Vec3 nrm = cross(row, col);
    for (std::uint32_t k = 0; k < n; ++k) {
        SliceSpec s;
        s.row = row;
        s.col = col;
        s.ipp = Vec3{-12.5, 30.25, 100.0} + nrm * (dz * k) + shift_per_slice * k;
        s.slope = 1.0 + (k % 2);
        s.intercept = -1024.0 + k;
        s.instance = static_cast<int>(k + 1);
        s.stored = slice_values(s.rows, s.cols, k);
        files.push_back(make_slice(s));
    }
    std::mt19937 rng(seed);
    std::shuffle(files.begin(), files.end(), rng);
    return files;
}

std::vector<const DicomFile*> ptrs(const std::vector<DicomFile>& v) {
    std::vector<const DicomFile*> p;
    for (const auto& f : v) p.push_back(&f);
    return p;
}

double expected_value(std::uint32_t i, std::uint32_t j, std::uint32_t k) {
    return pattern(i, j, k) * (1.0 + (k % 2)) + (-1024.0 + k);
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool near(Vec3 a, Vec3 b, double tol) { return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol); }

std::filesystem::path temp_dir(const char* name) {
    auto p = std::filesystem::temp_directory_path() / (std::string("siaara_vol_") + name + "_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(p);
    return p;
}

}  // namespace

TEST_CASE("volume: geometry helpers (matrix inverse, functional groups)") {
    const Mat3 m = Mat3::from_columns({2, 0, 0}, {0.5, 1, 0}, {0, 0.3, 4});
    auto inv = m.inverse();
    REQUIRE(inv.has_value());
    const Vec3 v{1.5, -2, 7};
    CHECK(near((*inv) * (m * v), v, 1e-12));
    CHECK(!Mat3::from_columns({1, 0, 0}, {2, 0, 0}, {0, 0, 1}).inverse().has_value());
    CHECK(!normalized({0, 0, 0}).has_value());
    CHECK(!normalized({std::nan(""), 0, 1}).has_value());
    Grid g;
    g.dims = {2, 3, 4};
    CHECK(g.validate().is_ok());
    g.spacing[2] = 0.0;
    CHECK(g.validate().code == Errc::InvalidArgument);
    g.spacing[2] = 1.0;
    g.axes[2] = {1, 0, 0};
    CHECK(!g.validate().is_ok());
    // Malformed orientation is rejected.
    SliceSpec s;
    s.stored = slice_values(s.rows, s.cols, 0);
    s.col = {1, 0, 0};  // parallel to row
    DicomFile f = make_slice(s);
    CHECK(read_frame_plane(f.dataset, 0).error().code == Errc::Malformed);
}

TEST_CASE("volume: sorted series, geometry and per-frame rescale (int16)") {
    auto files = make_series(9, 2.5);
    auto v = build_volume(ptrs(files));
    REQUIRE(v.is_ok());
    const Grid& g = v->grid();
    CHECK(g.dims[0] == 5 && g.dims[1] == 4 && g.dims[2] == 9);
    CHECK(near(g.spacing[0], 0.7, 1e-12) && near(g.spacing[1], 0.8, 1e-12) && near(g.spacing[2], 2.5, 1e-6));
    CHECK(near(g.origin, Vec3{-12.5, 30.25, 100.0}, 1e-9));
    CHECK(near(g.axes[2], Vec3{0, 0, 1}, 1e-9));
    CHECK(v->type() == VoxelType::Int16);
    CHECK(v->flags == 0);
    CHECK(v->phase_count == 1);
    bool ok = true;
    for (std::uint32_t k = 0; k < 9; ++k)
        for (std::uint32_t j = 0; j < 4; ++j)
            for (std::uint32_t i = 0; i < 5; ++i) ok = ok && v->at(i, j, k) == static_cast<float>(expected_value(i, j, k));
    CHECK(ok);
    double mn = 1e30, mx = -1e30;
    for (std::uint32_t k = 0; k < 9; ++k)
        for (std::uint32_t j = 0; j < 4; ++j)
            for (std::uint32_t i = 0; i < 5; ++i) {
                mn = std::min(mn, expected_value(i, j, k));
                mx = std::max(mx, expected_value(i, j, k));
            }
    CHECK(v->value_min == mn && v->value_max == mx);
    std::uint64_t total = 0;
    for (auto b : v->histogram.bins) total += b;
    CHECK(total == 180);
    CHECK(v->histogram.bins.size() == 256);
    CHECK(v->histogram.bins.front() > 0 && v->histogram.bins.back() > 0);
    // Threads do not change the result.
    VolumeOptions o1;
    o1.threads = 1;
    auto v1 = build_volume(ptrs(files), o1);
    REQUIRE(v1.is_ok());
    CHECK(std::memcmp(v1->data(), v->data(), static_cast<std::size_t>(v->byte_size())) == 0);
    // Forced float32 gives the same values.
    VolumeOptions of;
    of.type = VoxelTypeRequest::Float32;
    auto vf = build_volume(ptrs(files), of);
    REQUIRE(vf.is_ok());
    CHECK(vf->type() == VoxelType::Float32);
    CHECK(vf->at(3, 2, 7) == static_cast<float>(expected_value(3, 2, 7)));
}

TEST_CASE("volume: oblique orientation and reverse stacking") {
    const double c = std::cos(0.3), s = std::sin(0.3);
    const Vec3 row{c, s, 0}, col{0, 0, -1};  // oblique coronal-ish
    auto files = make_series(5, 3.0, 7, row, col);
    auto v = build_volume(ptrs(files));
    REQUIRE(v.is_ok());
    const Vec3 n = cross(row, col);
    CHECK(near(v->grid().axes[0], row, 1e-7));
    CHECK(near(v->grid().axes[1], col, 1e-7));
    CHECK(near(v->grid().axes[2], n, 1e-7));
    CHECK(near(v->grid().index_to_patient({0, 0, 4}), Vec3{-12.5, 30.25, 100.0} + n * 12.0, 1e-5));
    CHECK(v->at(2, 1, 3) == static_cast<float>(expected_value(2, 1, 3)));
}

TEST_CASE("volume: float32 for fractional rescale, int16 overflow falls back to float32") {
    std::vector<DicomFile> files;
    for (std::uint32_t k = 0; k < 3; ++k) {
        SliceSpec s;
        s.ipp = {0, 0, 1.0 * k};
        s.slope = 0.25;
        s.intercept = 1.5;
        s.instance = static_cast<int>(k + 1);
        s.stored = slice_values(s.rows, s.cols, k);
        files.push_back(make_slice(s));
    }
    auto v = build_volume(ptrs(files));
    REQUIRE(v.is_ok());
    CHECK(v->type() == VoxelType::Float32);
    CHECK(near(v->at(4, 3, 2), pattern(4, 3, 2) * 0.25 + 1.5, 1e-6));

    // Unsigned 16 bits stored: possible range 0..65535 does not fit int16, actual values do.
    std::vector<DicomFile> small, big;
    for (std::uint32_t k = 0; k < 2; ++k) {
        SliceSpec s;
        s.is_signed = false;
        s.ipp = {0, 0, 1.0 * k};
        s.instance = static_cast<int>(k + 1);
        s.stored.assign(20, 1000 + static_cast<std::int32_t>(k));
        small.push_back(make_slice(s));
        s.stored.assign(20, 60000);
        big.push_back(make_slice(s));
    }
    auto vs = build_volume(ptrs(small));
    REQUIRE(vs.is_ok());
    CHECK(vs->type() == VoxelType::Int16);
    CHECK(vs->at(0, 0, 1) == 1001.0f);
    auto vb = build_volume(ptrs(big));
    REQUIRE(vb.is_ok());
    CHECK(vb->type() == VoxelType::Float32);
    CHECK(vb->at(1, 1, 1) == 60000.0f);
    CHECK((vb->flags & kValuesClamped) == 0);
    // Forced int16 clamps and says so.
    VolumeOptions oi;
    oi.type = VoxelTypeRequest::Int16;
    auto vc = build_volume(ptrs(big), oi);
    REQUIRE(vc.is_ok());
    CHECK(vc->at(1, 1, 1) == 32767.0f);
    CHECK((vc->flags & kValuesClamped) != 0);
}

TEST_CASE("volume: duplicates, non-uniform spacing and missing slices") {
    auto files = make_series(6, 2.0);
    auto dup = make_series(1, 2.0);  // same position as slice 0 of the series
    files.push_back(std::move(dup[0]));
    auto v = build_volume(ptrs(files));
    REQUIRE(v.is_ok());
    CHECK(v->grid().dims[2] == 6);
    CHECK((v->flags & kDuplicatePositions) != 0);
    CHECK(!v->warnings.empty());
    VolumeOptions strict;
    strict.strict = true;
    CHECK(build_volume(ptrs(files), strict).error().code == Errc::Unsupported);

    auto gap = make_series(8, 2.0);
    // Remove the slice at k = 4 (z = 108).
    gap.erase(std::remove_if(gap.begin(), gap.end(),
                             [](const DicomFile& f) { return f.dataset.get_int(siaara::volume::tags::InstanceNumber) == 5; }),
              gap.end());
    auto vg = build_volume(ptrs(gap));
    REQUIRE(vg.is_ok());
    CHECK(vg->grid().dims[2] == 7);
    CHECK((vg->flags & kMissingSlices) != 0);
    CHECK((vg->flags & kNonUniformSpacing) != 0);
    CHECK(near(vg->grid().spacing[2], 14.0 / 6.0, 1e-6));
    CHECK(build_volume(ptrs(gap), strict).error().code == Errc::Unsupported);
}

TEST_CASE("volume: mixed orientation and varying size are rejected") {
    auto files = make_series(4, 2.0);
    SliceSpec s;
    s.ipp = {0, 0, 50};
    s.row = {0, 1, 0};
    s.col = {0, 0, 1};
    s.stored = slice_values(s.rows, s.cols, 0);
    files.push_back(make_slice(s));
    CHECK(build_volume(ptrs(files)).error().code == Errc::Unsupported);
    auto f2 = make_series(4, 2.0);
    SliceSpec t;
    t.rows = 3;
    t.ipp = {-12.5, 30.25, 200};
    t.stored.assign(15, 0);
    f2.push_back(make_slice(t));
    CHECK(build_volume(ptrs(f2)).error().code == Errc::Unsupported);
}

TEST_CASE("volume: gantry tilt detection, sheared grid and de-shearing") {
    // Each slice moves 2 mm along z and 0.8 mm along +y (= one row spacing): tilt atan(0.8 / 2).
    auto files = make_series(6, 2.0, 3, {1, 0, 0}, {0, 1, 0}, {0, 0.8, 0});
    auto v = build_volume(ptrs(files));
    REQUIRE(v.is_ok());
    CHECK((v->flags & kGantryTilt) != 0);
    CHECK(near(v->tilt_degrees, std::atan(0.4) * 180.0 / 3.14159265358979323846, 1e-4));
    // Sheared: voxel (i, j, k) is at the true position of slice k's pixel (i, j).
    const Vec3 p = v->grid().index_to_patient({1, 2, 5});
    CHECK(near(p, Vec3{-12.5 + 0.7, 30.25 + 1.6 + 4.0, 110.0}, 1e-5));
    CHECK(v->at(1, 2, 5) == static_cast<float>(expected_value(1, 2, 5)));

    VolumeOptions o;
    o.deshear_tilt = true;
    auto d = build_volume(ptrs(files), o);
    REQUIRE(d.is_ok());
    CHECK((d->flags & kDesheared) != 0);
    CHECK(d->type() == VoxelType::Float32);
    CHECK(near(d->grid().axes[2], Vec3{0, 0, 1}, 1e-9));
    CHECK(near(d->grid().spacing[2], 2.0, 1e-6));
    // Orthogonal voxel (i, j, k) lies on slice k at row j - k (one row per slice), exactly.
    bool ok = true;
    for (std::uint32_t k = 0; k < 6; ++k)
        for (std::uint32_t j = 0; j < 4; ++j)
            for (std::uint32_t i = 0; i < 5; ++i) {
                const int src_j = static_cast<int>(j) - static_cast<int>(k);
                if (src_j < 0) continue;  // outside the acquired slice: fill value
                ok = ok && near(d->at(i, j, k), expected_value(i, static_cast<std::uint32_t>(src_j), k), 1e-3);
            }
    CHECK(ok);
}

TEST_CASE("volume: 4D split by Temporal Position Identifier") {
    std::vector<DicomFile> files;
    int inst = 1;
    for (int t = 1; t <= 3; ++t)
        for (std::uint32_t k = 0; k < 4; ++k) {
            SliceSpec s;
            s.ipp = {0, 0, 5.0 * k};
            s.temporal_id = t;
            s.instance = inst++;
            s.stored.assign(20, static_cast<std::int32_t>(100 * t + static_cast<int>(k)));
            files.push_back(make_slice(s));
        }
    std::mt19937 rng(5);
    std::shuffle(files.begin(), files.end(), rng);
    auto v = build_volume(ptrs(files));
    REQUIRE(v.is_ok());
    CHECK(v->phase_count == 3);
    CHECK(v->phase_key == "TemporalPositionIdentifier");
    CHECK((v->flags & kMultiPhase) != 0);
    CHECK((v->flags & kDuplicatePositions) == 0);
    CHECK(v->grid().dims[2] == 4);
    VolumeOptions o;
    o.phase = 2;
    auto v2 = build_volume(ptrs(files), o);
    REQUIRE(v2.is_ok());
    CHECK(v2->at(0, 0, 3) == 303.0f);
    CHECK(v->at(0, 0, 1) == 101.0f);
    o.phase = 3;
    CHECK(build_volume(ptrs(files), o).error().code == Errc::InvalidArgument);
}

namespace {

Element seq(Tag t, std::vector<Dataset> items) {
    Element e;
    e.tag = t;
    e.vr = VR::SQ;
    e.items = std::move(items);
    return e;
}

// Enhanced multi-frame: `phases` x `slices` frames (frame order: slices reversed, phases interleaved).
DicomFile make_enhanced(std::uint32_t phases, std::uint32_t slices) {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.4.1");
    set_pixel_module(ds, 3, 2, true, 12);
    const std::uint32_t n = phases * slices;
    (void)ds.set_string(siaara::tags::NumberOfFrames, VR::IS, std::to_string(n));
    Dataset shared;
    Dataset ori;
    (void)ori.set_string(siaara::volume::tags::ImageOrientationPatient, VR::DS, ds6({0, 1, 0}, {0, 0, -1}));
    shared.put(seq(siaara::volume::tags::PlaneOrientationSequence, {ori}));
    Dataset meas;
    (void)meas.set_string(siaara::volume::tags::PixelSpacing, VR::DS, "1.25\\1.5");
    (void)meas.set_string(siaara::volume::tags::SliceThickness, VR::DS, "3");
    shared.put(seq(siaara::volume::tags::PixelMeasuresSequence, {meas}));
    Dataset pvt;
    (void)pvt.set_string(siaara::tags::RescaleSlope, VR::DS, "2");
    (void)pvt.set_string(siaara::tags::RescaleIntercept, VR::DS, "-10");
    shared.put(seq(0x00289145, {pvt}));
    ds.put(seq(siaara::volume::tags::SharedFunctionalGroupsSequence, {shared}));
    std::vector<Dataset> per;
    std::vector<std::int32_t> stored;
    for (std::uint32_t fi = 0; fi < n; ++fi) {
        const std::uint32_t t = fi % phases, k = slices - 1 - fi / phases;
        Dataset item;
        Dataset pos;
        (void)pos.set_string(siaara::volume::tags::ImagePositionPatient, VR::DS, ds3({4.0 * k, 0, 0}));
        item.put(seq(siaara::volume::tags::PlanePositionSequence, {pos}));
        Dataset fc;
        std::uint8_t tp[4];
        store_le32(tp, t + 1);
        (void)fc.set_bytes(siaara::volume::tags::TemporalPositionIndex, VR::UL, Bytes::copy_of(tp, 4));
        (void)fc.set_string(siaara::volume::tags::StackID, VR::SH, "1");
        item.put(seq(siaara::volume::tags::FrameContentSequence, {fc}));
        per.push_back(std::move(item));
        for (int p = 0; p < 6; ++p) stored.push_back(static_cast<std::int32_t>(100 * (t + 1) + 10 * k) + p);
    }
    ds.put(seq(siaara::volume::tags::PerFrameFunctionalGroupsSequence, per));
    set_pixels(ds, stored);
    return f;
}

}  // namespace

TEST_CASE("volume: enhanced multi-frame with functional groups and temporal positions") {
    DicomFile f = make_enhanced(2, 4);
    // Round trip through the writer and reader so the functional groups come from a real stream.
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    auto rf = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(rf.is_ok());
    const DicomFile* p = &*rf;
    auto v = build_volume(std::span<const DicomFile* const>(&p, 1));
    REQUIRE(v.is_ok());
    CHECK(v->phase_count == 2);
    CHECK(v->phase_key == "TemporalPositionIndex");
    CHECK(v->grid().dims[0] == 2 && v->grid().dims[1] == 3 && v->grid().dims[2] == 4);
    CHECK(near(v->grid().spacing[0], 1.5, 1e-12) && near(v->grid().spacing[1], 1.25, 1e-12));
    // normal = (0,1,0) x (0,0,-1) = (-1, 0, 0): sorted along -x, so the first slice is at x = 12.
    CHECK(near(v->grid().axes[2], Vec3{-1, 0, 0}, 1e-9));
    CHECK(near(v->grid().origin, Vec3{12, 0, 0}, 1e-9));
    CHECK(near(v->grid().spacing[2], 4.0, 1e-9));
    CHECK(v->type() == VoxelType::Int16);
    // Slice index 0 is k = 3 (x = 12): stored 100 + 30 + p, rescale 2x - 10.
    CHECK(v->at(1, 0, 0) == static_cast<float>((131) * 2 - 10));
    CHECK(v->at(0, 2, 3) == static_cast<float>((104) * 2 - 10));
    VolumeOptions o;
    o.phase = 1;
    auto v1 = build_volume(std::span<const DicomFile* const>(&p, 1), o);
    REQUIRE(v1.is_ok());
    CHECK(v1->at(0, 0, 0) == static_cast<float>(230 * 2 - 10));
}

TEST_CASE("volume: single slice, missing geometry, budget and bad input") {
    SliceSpec s;
    s.ipp = {1, 2, 3};
    s.slice_thickness = 2.5;
    s.stored = slice_values(s.rows, s.cols, 0);
    DicomFile f = make_slice(s);
    const DicomFile* p = &f;
    auto v = build_volume(std::span<const DicomFile* const>(&p, 1));
    REQUIRE(v.is_ok());
    CHECK(v->grid().dims[2] == 1);
    CHECK((v->flags & kSingleSlice) != 0);
    CHECK(near(v->grid().spacing[2], 2.5, 1e-12));

    std::vector<DicomFile> nogeo;
    for (int k = 0; k < 3; ++k) {
        SliceSpec t;
        t.geometry = false;
        t.instance = 3 - k;
        t.stored.assign(20, k);
        nogeo.push_back(make_slice(t));
    }
    auto vn = build_volume(ptrs(nogeo));
    REQUIRE(vn.is_ok());
    CHECK((vn->flags & kSyntheticGeometry) != 0);
    CHECK(vn->at(0, 0, 0) == 2.0f);  // instance 1 first
    CHECK(vn->grid().validate().is_ok());

    auto files = make_series(4, 1.0);
    VolumeOptions o;
    o.memory_budget = 100;
    CHECK(build_volume(ptrs(files), o).error().code == Errc::LimitExceeded);
    CHECK(build_volume(std::span<const DicomFile* const>()).error().code == Errc::InvalidArgument);
    const DicomFile* null_file = nullptr;
    CHECK(build_volume(std::span<const DicomFile* const>(&null_file, 1)).error().code == Errc::InvalidArgument);
    // Colour data is refused.
    DicomFile rgb = make_slice(s);
    (void)rgb.dataset.set_uint16(siaara::tags::SamplesPerPixel, 3);
    (void)rgb.dataset.set_string(siaara::tags::PhotometricInterpretation, VR::CS, "RGB");
    const DicomFile* pr = &rgb;
    CHECK(!build_volume(std::span<const DicomFile* const>(&pr, 1)).is_ok());
    // Huge grids are refused before allocating.
    Grid g;
    g.dims = {100000, 100000, 100000};
    CHECK(Volume::allocate(g, VoxelType::Float32).error().code == Errc::LimitExceeded);
}

TEST_CASE("volume: build from files and the C API") {
    auto files = make_series(7, 1.25, 11);
    const auto dir = temp_dir("series");
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string path = (dir / ("s" + std::to_string(i) + ".dcm")).string();
        REQUIRE(write_file(files[i], path).is_ok());
        paths.push_back(path);
    }
    auto a = build_volume_from_paths(paths);
    auto b = build_volume(ptrs(files));
    REQUIRE(a.is_ok() && b.is_ok());
    CHECK(a->byte_size() == b->byte_size());
    CHECK(std::memcmp(a->data(), b->data(), static_cast<std::size_t>(a->byte_size())) == 0);
    std::vector<std::string> bad = paths;
    bad.push_back((dir / "missing.dcm").string());
    CHECK(!build_volume_from_paths(bad).is_ok());

    std::vector<const char*> cp;
    for (auto& s : paths) cp.push_back(s.c_str());
    siaara_volume* vol = nullptr;
    REQUIRE(siaara_volume_build(cp.data(), nullptr, static_cast<uint32_t>(cp.size()), nullptr, &vol) == SIAARA_OK);
    siaara_volume_properties info{};
    info.struct_size = sizeof info;
    REQUIRE(siaara_volume_info(vol, &info) == SIAARA_OK);
    CHECK(info.dims[0] == 5 && info.dims[1] == 4 && info.dims[2] == 7);
    CHECK(near(info.spacing[2], 1.25, 1e-6));
    CHECK(info.voxel_type == SIAARA_VOXEL_INT16);
    CHECK(info.direction[8] == 1.0 && info.direction[0] == 1.0 && info.direction[4] == 1.0);
    CHECK(info.value_min == a->value_min && info.value_max == a->value_max);
    std::uint64_t total = 0;
    for (uint32_t i = 0; i < info.histogram_bins; ++i) total += info.histogram[i];
    CHECK(total == 140);
    const void* data = nullptr;
    uint64_t nbytes = 0;
    REQUIRE(siaara_volume_data(vol, &data, &nbytes) == SIAARA_OK);
    CHECK(nbytes == 280);
    CHECK(std::memcmp(data, a->data(), 280) == 0);
    CHECK(siaara_volume_warning(vol, 0, nullptr, 0, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_volume_options o{};
    o.struct_size = sizeof o;
    o.voxel_type = 7;
    siaara_volume* v2 = nullptr;
    CHECK(siaara_volume_build(cp.data(), nullptr, 1, &o, &v2) == SIAARA_ERR_INVALID_ARGUMENT);
    o.voxel_type = SIAARA_VOXEL_FLOAT32;
    o.memory_budget = 10;
    CHECK(siaara_volume_build(cp.data(), nullptr, static_cast<uint32_t>(cp.size()), &o, &v2) == SIAARA_ERR_LIMIT_EXCEEDED);
    CHECK(v2 == nullptr);
    // Views through the C API.
    siaara_view* view = nullptr;
    REQUIRE(siaara_view_create(vol, 5, 4, &view) == SIAARA_OK);
    size_t need = 0;
    REQUIRE(siaara_view_render_rgba8(view, nullptr, 0, &need) == SIAARA_OK);
    CHECK(need == 80);
    std::vector<uint8_t> img(need);
    const double pt[3] = {-12.5 + 2 * 0.7, 30.25 + 1.5 * 0.8, 100.0 + 3 * 1.25};
    const double nrm[3] = {0, 0, 1}, up[3] = {0, -1, 0};
    REQUIRE(siaara_view_set_plane(view, pt, nrm, up, 0.7) == SIAARA_OK);
    REQUIRE(siaara_view_set_mode(view, SIAARA_VIEW_MPR, SIAARA_INTERP_NEAREST, SIAARA_SLAB_NONE, 0, SIAARA_QUALITY_FINAL) == SIAARA_OK);
    REQUIRE(siaara_view_set_window(view, 0, -500, 2000) == SIAARA_OK);
    REQUIRE(siaara_view_render_rgba8(view, img.data(), img.size(), &need) == SIAARA_OK);
    CHECK(siaara_view_render_rgba8(view, img.data(), 10, &need) == SIAARA_ERR_BUFFER_TOO_SMALL);
    siaara_pick_result pr{};
    pr.struct_size = sizeof pr;
    REQUIRE(siaara_view_pick(view, 2, 1.5, &pr) == SIAARA_OK);
    CHECK(pr.hit == 1 && pr.inside == 1);
    CHECK(near(pr.index[0], 2, 1e-6) && near(pr.index[2], 3, 1e-6));
    siaara_transfer_function tf{};
    tf.struct_size = sizeof tf;
    const double op[4] = {-1000, 0, 2000, 0.6};
    const double cl[8] = {-1000, 1, 0, 0, 2000, 1, 1, 1};
    tf.opacity_points = op;
    tf.opacity_count = 2;
    tf.color_points = cl;
    tf.color_count = 2;
    tf.shading = 1;
    REQUIRE(siaara_view_set_transfer_function(view, &tf) == SIAARA_OK);
    REQUIRE(siaara_view_set_mode(view, SIAARA_VIEW_VOLUME_RENDER, SIAARA_INTERP_LINEAR, 0, 0, SIAARA_QUALITY_INTERACTIVE) == SIAARA_OK);
    REQUIRE(siaara_view_render_rgba8(view, img.data(), img.size(), &need) == SIAARA_OK);
    REQUIRE(siaara_view_set_fusion_layer(view, vol, nullptr) == SIAARA_OK);
    REQUIRE(siaara_view_set_colormap(view, 1, SIAARA_COLORMAP_HOT_METAL_BLUE) == SIAARA_OK);
    CHECK(siaara_view_set_colormap(view, 1, 99) == SIAARA_ERR_INVALID_ARGUMENT);
    REQUIRE(siaara_view_set_mode(view, SIAARA_VIEW_MPR, SIAARA_INTERP_CUBIC, SIAARA_SLAB_MIP, 3.0, SIAARA_QUALITY_FINAL) == SIAARA_OK);
    REQUIRE(siaara_view_render_rgba8(view, img.data(), img.size(), &need) == SIAARA_OK);
    REQUIRE(siaara_view_pick(view, 1, 1, &pr) == SIAARA_OK);
    CHECK(pr.overlay_inside == 1);
    const double curve[6] = {-12.5, 31, 102, -9.5, 31, 102};
    REQUIRE(siaara_view_set_curve(view, curve, 2, 0) == SIAARA_OK);
    REQUIRE(siaara_view_set_mode(view, SIAARA_VIEW_CURVED_STRAIGHTENED, SIAARA_INTERP_LINEAR, 0, 0, SIAARA_QUALITY_FINAL) == SIAARA_OK);
    REQUIRE(siaara_view_render_rgba8(view, img.data(), img.size(), &need) == SIAARA_OK);
    CHECK(siaara_view_set_mode(view, 42, 0, 0, 0, 0) == SIAARA_ERR_INVALID_ARGUMENT);
    const double zero[3] = {0, 0, 0};
    CHECK(siaara_view_set_plane(view, pt, zero, up, 0) == SIAARA_ERR_INVALID_ARGUMENT);
    // The view keeps the volume alive after the handle is freed.
    siaara_volume_free(vol);
    vol = nullptr;
    REQUIRE(siaara_view_render_rgba8(view, img.data(), img.size(), &need) == SIAARA_OK);
    siaara_view_free(view);
    siaara_volume_free(vol);
    siaara_volume_free(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
