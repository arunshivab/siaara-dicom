// Named data layer tests (src/modules, src/capi/capi_named.cpp): generated module tables, the
// path API, values at a point, volumes, waveforms, RT control points, NM index vectors, SOP class /
// transfer syntax information and dictionary search. Every input is a synthetic data set built
// here; missing attributes must come back as "not found", never as a default.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/writer.hpp"
#include "modules/data.hpp"
#include "modules/modules.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

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

void s(Dataset& ds, Tag t, VR vr, std::string_view v) { (void)ds.set_string(t, vr, v); }

void u16s(Dataset& ds, Tag t, const std::vector<std::uint16_t>& v) {
    std::vector<std::uint8_t> b;
    for (auto x : v) {
        b.push_back(static_cast<std::uint8_t>(x & 0xFF));
        b.push_back(static_cast<std::uint8_t>(x >> 8));
    }
    (void)ds.set_bytes(t, VR::US, Bytes::copy_of(b.data(), b.size()));
}

siaara_file* open_mem(const DicomFile& f) {
    std::vector<std::uint8_t> bytes;
    if (!write_part10(f, bytes)) return nullptr;
    siaara_file* out = nullptr;
    if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &out) != SIAARA_OK) return nullptr;
    return out;
}

siaara_dataset* main_ds(siaara_file* f) {
    siaara_dataset* d = nullptr;
    (void)siaara_file_dataset(f, &d);
    return d;
}

std::string text_at(const siaara_dataset* ds, const char* path, siaara_path_value* pv = nullptr, siaara_status* st = nullptr) {
    siaara_path_value v{};
    v.struct_size = sizeof v;
    char buf[512] = {};
    size_t need = 0;
    const siaara_status r = siaara_get_by_path(ds, path, std::strlen(path), &v, buf, sizeof buf, &need);
    if (pv) *pv = v;
    if (st) *st = r;
    return r == SIAARA_OK ? std::string(buf) : std::string("<err>");
}

std::vector<double> doubles_at(const siaara_dataset* ds, const char* path) {
    size_t n = 0;
    if (siaara_get_doubles_by_path(ds, path, std::strlen(path), nullptr, 0, &n) != SIAARA_OK) return {};
    std::vector<double> v(n);
    if (siaara_get_doubles_by_path(ds, path, std::strlen(path), v.data(), v.size(), &n) != SIAARA_OK) return {};
    return v;
}

// ---------------------------------------------------------------- synthetic objects

DicomFile make_rtplan() {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    s(ds, 0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.481.5");
    s(ds, 0x00080018, VR::UI, "2.25.42");
    s(ds, 0x00080060, VR::CS, "RTPLAN");
    s(ds, 0x00100010, VR::PN, "Doe^John^Q^Dr^Jr=\xe5\xb1\xb1\xe7\x94\xb0^\xe5\xa4\xaa\xe9\x83\x8e=\xe3\x82\x84\xe3\x81\xbe\xe3\x81\xa0^\xe3\x81\x9f\xe3\x82\x8d\xe3\x81\x86");
    s(ds, 0x00080005, VR::CS, "ISO_IR 192");
    s(ds, 0x00080070, VR::LO, "Acme  Medical ");
    s(ds, 0x00081090, VR::LO, "LinacX 3000");
    // Fraction group: MU per beam through the Referenced Beam Sequence.
    Dataset& fg = add_item(ds, 0x300A0070);
    s(fg, 0x300A0071, VR::IS, "1");
    s(fg, 0x300A0078, VR::IS, "30");
    Dataset& rb = add_item(fg, 0x300C0004);
    s(rb, 0x300C0006, VR::IS, "1");
    s(rb, 0x300A0086, VR::DS, "123.4");
    // Beam with MLC definition and three control points.
    Dataset& beam = add_item(ds, 0x300A00B0);
    s(beam, 0x300A00C0, VR::IS, "1");
    s(beam, 0x300A00C2, VR::LO, "AP");
    s(beam, 0x300A00C4, VR::CS, "DYNAMIC");
    s(beam, 0x300A00C6, VR::CS, "PHOTON");
    s(beam, 0x300A00B2, VR::SH, "TrueBeam1");
    s(beam, 0x300A00B4, VR::DS, "1000");
    Dataset& bld = add_item(beam, 0x300A00B6);
    s(bld, 0x300A00B8, VR::CS, "MLCX");
    s(bld, 0x300A00BC, VR::IS, "2");
    s(bld, 0x300A00BE, VR::DS, "-10\\0\\10");
    Dataset& bldy = add_item(beam, 0x300A00B6);
    s(bldy, 0x300A00B8, VR::CS, "ASYMY");
    s(bldy, 0x300A00BC, VR::IS, "1");
    Dataset& cp0 = add_item(beam, 0x300A0111);
    s(cp0, 0x300A0112, VR::IS, "0");
    s(cp0, 0x300A0114, VR::DS, "6");
    s(cp0, 0x300A011E, VR::DS, "180");
    s(cp0, 0x300A011F, VR::CS, "CW");
    s(cp0, 0x300A012C, VR::DS, "0\\0\\0");
    s(cp0, 0x300A0134, VR::DS, "0");
    Dataset& p0 = add_item(cp0, 0x300A011A);
    s(p0, 0x300A00B8, VR::CS, "ASYMY");
    s(p0, 0x300A011C, VR::DS, "-50\\50");
    Dataset& p1 = add_item(cp0, 0x300A011A);
    s(p1, 0x300A00B8, VR::CS, "MLCX");
    s(p1, 0x300A011C, VR::DS, "-5\\-4\\5\\6");
    Dataset& cp1 = add_item(beam, 0x300A0111);
    s(cp1, 0x300A0112, VR::IS, "1");
    s(cp1, 0x300A011E, VR::DS, "190");
    s(cp1, 0x300A0134, VR::DS, "0.5");
    Dataset& q1 = add_item(cp1, 0x300A011A);
    s(q1, 0x300A00B8, VR::CS, "MLCX");
    s(q1, 0x300A011C, VR::DS, "-3\\-2\\3\\4");
    Dataset& cp2 = add_item(beam, 0x300A0111);
    s(cp2, 0x300A0112, VR::IS, "2");
    s(cp2, 0x300A0134, VR::DS, "1");
    // Private block in the beam: creator "ACME 1.0" at (300B,0010), element (300B,1001).
    s(beam, 0x300B0010, VR::LO, "ACME 1.0");
    s(beam, 0x300B1001, VR::LO, "private value");
    s(beam, 0x300B0011, VR::LO, "ACMERT");
    s(beam, 0x300B1101, VR::LO, "second block");
    return f;
}

DicomFile make_ct(std::uint16_t rows, std::uint16_t cols, double slope, double intercept) {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    s(ds, 0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    s(ds, 0x00080018, VR::UI, "2.25.7");
    s(ds, 0x00080060, VR::CS, "CT");
    (void)ds.set_uint16(0x00280010, rows);
    (void)ds.set_uint16(0x00280011, cols);
    (void)ds.set_uint16(0x00280002, 1);
    s(ds, 0x00280004, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(0x00280100, 16);
    (void)ds.set_uint16(0x00280101, 16);
    (void)ds.set_uint16(0x00280102, 15);
    (void)ds.set_uint16(0x00280103, 1);
    char b[64];
    std::snprintf(b, sizeof b, "%.10g", slope);
    s(ds, 0x00281053, VR::DS, b);
    std::snprintf(b, sizeof b, "%.10g", intercept);
    s(ds, 0x00281052, VR::DS, b);
    std::vector<std::uint8_t> px;
    for (std::uint32_t r = 0; r < rows; ++r)
        for (std::uint32_t c = 0; c < cols; ++c) {
            const auto v = static_cast<std::int16_t>(static_cast<int>(c) * 3 - static_cast<int>(r) * 5);
            const auto u = static_cast<std::uint16_t>(v);
            px.push_back(static_cast<std::uint8_t>(u & 0xFF));
            px.push_back(static_cast<std::uint8_t>(u >> 8));
        }
    (void)ds.set_bytes(0x7FE00010, VR::OW, Bytes::copy_of(px.data(), px.size()));
    return f;
}

}  // namespace

// ---------------------------------------------------------------- module tables

TEST_CASE("named: module tables cover PS3.3 and resolve includes") {
    uint32_t n = 0;
    REQUIRE(siaara_module_count(&n) == SIAARA_OK);
    CHECK(n > 1500);
    uint32_t modules = 0, macros = 0;
    for (uint32_t i = 0; i < n; ++i) {
        siaara_module_info mi{};
        mi.struct_size = sizeof mi;
        REQUIRE(siaara_module_get(i, &mi) == SIAARA_OK);
        modules += mi.kind == SIAARA_MODULE_KIND_MODULE;
        macros += mi.kind == SIAARA_MODULE_KIND_MACRO;
    }
    CHECK(modules > 350);
    CHECK(macros > 250);
    uint32_t idx = 0;
    REQUIRE(siaara_module_find("RT Beams", 8, &idx) == SIAARA_OK);
    siaara_module_info mi{};
    mi.struct_size = sizeof mi;
    REQUIRE(siaara_module_get(idx, &mi) == SIAARA_OK);
    CHECK(std::string(mi.name) == "RT Beams Module");
    CHECK(std::string(mi.class_name) == "RTBeamsModule");
    uint32_t idx2 = 0;
    REQUIRE(siaara_module_find(mi.table_id, std::strlen(mi.table_id), &idx2) == SIAARA_OK);
    CHECK(idx2 == idx);
    CHECK(siaara_module_find("No Such Module", 14, &idx2) == SIAARA_ERR_NOT_FOUND);
    // The Beam Sequence's item holds the Control Point Sequence, whose item holds Leaf/Jaw Positions
    // through the Beam Limiting Device Position Sequence (nested items, all generated).
    siaara_module_attribute a{};
    a.struct_size = sizeof a;
    REQUIRE(siaara_module_attribute_get(idx, 0, &a) == SIAARA_OK);
    CHECK(a.tag == 0x300A00B0u);
    CHECK(std::string(a.keyword) == "BeamSequence");
    REQUIRE(a.item_module >= 0);
    bool found_cp = false;
    siaara_module_info item{};
    item.struct_size = sizeof item;
    REQUIRE(siaara_module_get(static_cast<uint32_t>(a.item_module), &item) == SIAARA_OK);
    for (uint32_t i = 0; i < item.attribute_count; ++i) {
        siaara_module_attribute b{};
        b.struct_size = sizeof b;
        REQUIRE(siaara_module_attribute_get(static_cast<uint32_t>(a.item_module), i, &b) == SIAARA_OK);
        if (b.tag == 0x300A0111u) found_cp = b.item_module >= 0;
    }
    CHECK(found_cp);
    // Patient Module: Include rows resolved (Patient Name is direct; Issuer of Patient ID comes
    // from the Issuer of Patient ID Macro).
    REQUIRE(siaara_module_find("Patient", 7, &idx) == SIAARA_OK);
    bool name = false, issuer = false;
    const auto& flat = modules::flattened(idx);
    for (const auto& f : flat) {
        name |= f.attr->tag == 0x00100010u;
        issuer |= f.attr->tag == 0x00100021u;
    }
    CHECK(name);
    CHECK(issuer);
}

TEST_CASE("named: module present") {
    siaara_file* f = open_mem(make_rtplan());
    REQUIRE(f);
    int32_t p = -1;
    CHECK(siaara_module_present(f, "RT Beams", 8, &p) == SIAARA_OK);
    CHECK(p == 1);
    CHECK(siaara_module_present(f, "RT Brachy Application Setups", 28, &p) == SIAARA_OK);
    CHECK(p == 0);
    CHECK(siaara_module_present(f, "nonsense", 8, &p) == SIAARA_ERR_NOT_FOUND);
    siaara_file_free(f);
}

// ---------------------------------------------------------------- path API

TEST_CASE("named: path API nested sequences, indices, private tags, missing") {
    siaara_file* f = open_mem(make_rtplan());
    REQUIRE(f);
    const siaara_dataset* ds = main_ds(f);
    siaara_path_value v{};
    CHECK(text_at(ds, "BeamSequence[0].BeamName", &v) == "AP");
    CHECK(v.kind == SIAARA_VALUE_TEXT);
    CHECK(std::string(v.vr) == "LO");
    CHECK(text_at(ds, "BeamSequence[0].ControlPointSequence[0].BeamLimitingDevicePositionSequence[1].LeafJawPositions", &v) ==
          "-5\\-4\\5\\6");
    CHECK(v.kind == SIAARA_VALUE_REAL);
    CHECK(v.vm == 4);
    CHECK(v.has_real == 1 && v.real == -5.0);
    CHECK(text_at(ds, "BeamSequence[0].ControlPointSequence[0].BeamLimitingDevicePositionSequence[1].LeafJawPositions[3]",
                  &v) == "6");
    CHECK(v.real == 6.0);
    CHECK(text_at(ds, "(300A,00B0)[0].(300A,00C0)", &v) == "1");
    CHECK(v.kind == SIAARA_VALUE_INTEGER && v.integer == 1);
    CHECK(text_at(ds, "300A00B0[0].300A00C2") == "AP");
    CHECK(text_at(ds, "BeamSequence[0].ACMERT:300B,01", &v) == "second block");
    CHECK(v.tag == 0x300B1101u);
    CHECK(text_at(ds, "BeamSequence[0].\"ACME 1.0\":300B,01", &v) == "private value");
    CHECK(v.tag == 0x300B1001u);
    siaara_status st = SIAARA_OK;
    (void)text_at(ds, "BeamSequence[0].OTHER:300B,01", nullptr, &st);
    CHECK(st == SIAARA_ERR_NOT_FOUND);
    (void)text_at(ds, "BeamSequence[0].SourceAxisDistance", &v, &st);
    CHECK(st == SIAARA_OK);
    (void)text_at(ds, "BeamSequence[0].GantryAngle", nullptr, &st);  // not at beam level
    CHECK(st == SIAARA_ERR_NOT_FOUND);
    (void)text_at(ds, "BeamSequence[1].BeamName", nullptr, &st);
    CHECK(st == SIAARA_ERR_NOT_FOUND);
    (void)text_at(ds, "BeamSequence.BeamName", nullptr, &st);  // intermediate sequence needs an index
    CHECK(st == SIAARA_ERR_INVALID_ARGUMENT);
    (void)text_at(ds, "NotAKeyword", nullptr, &st);
    CHECK(st == SIAARA_ERR_NOT_FOUND);
    CHECK(text_at(ds, "BeamSequence", &v) == "");
    CHECK(v.kind == SIAARA_VALUE_SEQUENCE && v.item_count == 1);
    // Person name decoded with the UTF-8 character set; manufacturer kept exactly as stored.
    CHECK(text_at(ds, "PatientName").rfind("Doe^John^Q^Dr^Jr=", 0) == 0);
    CHECK(text_at(ds, "Manufacturer") == "Acme  Medical");  // trailing padding only is removed (PS3.5 6.2)
    // Fraction group MU per beam.
    CHECK(doubles_at(ds, "FractionGroupSequence[0].ReferencedBeamSequence[0].BeamMeterset") == std::vector<double>{123.4});
    CHECK(doubles_at(ds, "BeamSequence[0].BeamLimitingDeviceSequence[0].LeafPositionBoundaries") ==
          (std::vector<double>{-10, 0, 10}));
    // Item via path.
    siaara_dataset* root = main_ds(f);
    siaara_dataset* cp = nullptr;
    REQUIRE(siaara_dataset_by_path(root, "BeamSequence[0].ControlPointSequence[1]", 39, &cp) == SIAARA_OK);
    double g = 0;
    CHECK(siaara_dataset_get_double(cp, 0x300A011E, 0, &g) == SIAARA_OK && g == 190);
    siaara_file_free(f);
}

// ---------------------------------------------------------------- RT plan control points

TEST_CASE("named: RT plan MLC leaf positions and control point inheritance") {
    siaara_file* f = open_mem(make_rtplan());
    REQUIRE(f);
    siaara_dataset* root = main_ds(f);
    siaara_dataset* beam = nullptr;
    REQUIRE(siaara_dataset_item(root, 0x300A00B0, 0, &beam) == SIAARA_OK);
    double buf[8];
    size_t n = 0;
    uint32_t from = 99;
    REQUIRE(siaara_rt_device_positions(beam, 1, "MLCX", 4, 0, buf, 8, &n, &from) == SIAARA_OK);
    CHECK(n == 4 && buf[0] == -3 && buf[3] == 4 && from == 1);
    // Control point 2 holds no positions: as stored there is nothing, effectively control point 1's.
    CHECK(siaara_rt_device_positions(beam, 2, "MLCX", 4, 0, buf, 8, &n, &from) == SIAARA_ERR_NOT_FOUND);
    REQUIRE(siaara_rt_device_positions(beam, 2, "MLCX", 4, 1, buf, 8, &n, &from) == SIAARA_OK);
    CHECK(n == 4 && buf[0] == -3 && from == 1);
    REQUIRE(siaara_rt_device_positions(beam, 2, "ASYMY", 5, 1, buf, 8, &n, &from) == SIAARA_OK);
    CHECK(n == 2 && buf[1] == 50 && from == 0);
    CHECK(siaara_rt_device_positions(beam, 0, "MLCX", 4, 1, buf, 2, &n, &from) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(n == 4);
    uint32_t src = 0;
    REQUIRE(siaara_rt_control_point_source(beam, 2, 0x300A011E, &src) == SIAARA_OK);  // gantry angle
    CHECK(src == 1);
    REQUIRE(siaara_rt_control_point_source(beam, 2, 0x300A011F, &src) == SIAARA_OK);  // rotation direction
    CHECK(src == 0);
    CHECK(siaara_rt_control_point_source(beam, 2, 0x300A0120, &src) == SIAARA_ERR_NOT_FOUND);  // never given
    CHECK(siaara_rt_control_point_source(beam, 7, 0x300A011E, &src) == SIAARA_ERR_NOT_FOUND);
    siaara_file_free(f);
}

// ---------------------------------------------------------------- values at a point

TEST_CASE("named: HU at a pixel equals the rescale formula") {
    const double slope = 1.25, intercept = -1024;
    siaara_file* f = open_mem(make_ct(8, 10, slope, intercept));
    REQUIRE(f);
    siaara_pixel_value v{};
    v.struct_size = sizeof v;
    REQUIRE(siaara_file_pixel_value(f, 0, 7, 3, 0, &v) == SIAARA_OK);
    const int stored = 7 * 3 - 3 * 5;
    CHECK(v.stored == stored);
    CHECK(v.has_modality == 1);
    CHECK(std::fabs(v.modality_value - (stored * slope + intercept)) < 1e-9);
    CHECK(v.units_source == SIAARA_UNITS_CT_HU);
    CHECK(std::string(v.units) == "HU");
    CHECK(siaara_file_pixel_value(f, 0, 10, 0, 0, &v) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_file_pixel_value(f, 1, 0, 0, 0, &v) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(f, 0, &fr) == SIAARA_OK);
    siaara_pixel_value w{};
    w.struct_size = sizeof w;
    REQUIRE(siaara_frame_pixel_value(fr, 2, 5, 0, &w) == SIAARA_OK);
    CHECK(w.stored == 2 * 3 - 5 * 5);
    CHECK(std::fabs(w.modality_value - ((2 * 3 - 5 * 5) * slope + intercept)) < 1e-9);
    siaara_frame_free(fr);
    siaara_file_free(f);
}

TEST_CASE("named: RT Dose value uses Dose Grid Scaling and Dose Units") {
    DicomFile d = make_ct(4, 4, 1, 0);
    s(d.dataset, 0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.481.2");
    s(d.dataset, 0x00080060, VR::CS, "RTDOSE");
    (void)d.dataset.remove(0x00281053);
    (void)d.dataset.remove(0x00281052);
    s(d.dataset, 0x3004000E, VR::DS, "0.001");
    s(d.dataset, 0x30040002, VR::CS, "GY");
    siaara_file* f = open_mem(d);
    REQUIRE(f);
    siaara_pixel_value v{};
    v.struct_size = sizeof v;
    REQUIRE(siaara_file_pixel_value(f, 0, 3, 0, 0, &v) == SIAARA_OK);
    CHECK(v.stored == 9);
    CHECK(v.dose_grid_scaling == 1);
    CHECK(std::fabs(v.modality_value - 0.009) < 1e-12);
    CHECK(v.units_source == SIAARA_UNITS_DOSE_UNITS);
    CHECK(std::string(v.units) == "GY");
    siaara_file_free(f);
}

TEST_CASE("named: voxel at (25,20,12) and value at a patient coordinate") {
    const std::uint16_t cols = 32, rows = 24;
    const std::uint32_t slices = 16;
    const double ox = -20, oy = -10, oz = 5, sx = 0.5, sy = 0.75, sz = 2.0;
    auto dir = std::filesystem::temp_directory_path() / ("siaara_named_vol_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    std::vector<std::string> paths;
    for (std::uint32_t k = 0; k < slices; ++k) {
        DicomFile f = make_ct(rows, cols, 1, 0);
        Dataset& ds = f.dataset;
        char b[160];
        std::snprintf(b, sizeof b, "%.6f\\%.6f\\%.6f", ox, oy, oz + k * sz);
        s(ds, 0x00200032, VR::DS, b);
        s(ds, 0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
        std::snprintf(b, sizeof b, "%.6f\\%.6f", sy, sx);  // row spacing, column spacing
        s(ds, 0x00280030, VR::DS, b);
        s(ds, 0x00200013, VR::IS, std::to_string(k + 1));
        s(ds, 0x00080018, VR::UI, "2.25.9" + std::to_string(k));
        s(ds, 0x0020000E, VR::UI, "2.25.555");
        std::vector<std::uint8_t> px;
        for (std::uint32_t j = 0; j < rows; ++j)
            for (std::uint32_t i = 0; i < cols; ++i) {
                const auto v = static_cast<std::uint16_t>(i + 40 * j + 1000 * k);
                px.push_back(static_cast<std::uint8_t>(v & 0xFF));
                px.push_back(static_cast<std::uint8_t>(v >> 8));
            }
        (void)ds.set_bytes(0x7FE00010, VR::OW, Bytes::copy_of(px.data(), px.size()));
        paths.push_back((dir / ("s" + std::to_string(k) + ".dcm")).string());
        REQUIRE(write_file(f, paths.back()));
    }
    std::vector<const char*> cp;
    for (auto& p : paths) cp.push_back(p.c_str());
    siaara_volume* vol = nullptr;
    REQUIRE(siaara_volume_build(cp.data(), nullptr, slices, nullptr, &vol) == SIAARA_OK);
    double v = 0;
    REQUIRE(siaara_volume_voxel(vol, 25, 20, 12, &v) == SIAARA_OK);
    CHECK(v == 25 + 40 * 20 + 1000 * 12);
    CHECK(siaara_volume_voxel(vol, 32, 0, 0, &v) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_volume_sample sm{};
    sm.struct_size = sizeof sm;
    const double p[3] = {ox + 25 * sx + 0.2, oy + 20 * sy - 0.3, oz + 12 * sz + 0.9};
    REQUIRE(siaara_volume_value_at(vol, p, SIAARA_INTERP_NEAREST, &sm) == SIAARA_OK);
    CHECK(sm.inside == 1);
    CHECK(sm.voxel[0] == 25 && sm.voxel[1] == 20 && sm.voxel[2] == 12);
    CHECK(sm.value == 25 + 40 * 20 + 1000 * 12);
    const double mid[3] = {ox + 25.5 * sx, oy + 20 * sy, oz + 12 * sz};
    REQUIRE(siaara_volume_value_at(vol, mid, SIAARA_INTERP_LINEAR, &sm) == SIAARA_OK);
    CHECK(std::fabs(sm.value - (25.5 + 40 * 20 + 1000 * 12)) < 1e-3);
    const double out[3] = {ox - 5, oy, oz};
    REQUIRE(siaara_volume_value_at(vol, out, SIAARA_INTERP_NEAREST, &sm) == SIAARA_OK);
    CHECK(sm.inside == 0);
    CHECK(sm.voxel[0] == -10);
    CHECK(siaara_volume_value_at(vol, out, SIAARA_INTERP_CUBIC, &sm) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_volume_free(vol);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------- NM

TEST_CASE("named: NM energy windows and frame index vectors") {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    s(ds, 0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.20");
    s(ds, 0x00080060, VR::CS, "NM");
    Dataset& w1 = add_item(ds, 0x00540012);
    Dataset& r1 = add_item(w1, 0x00540013);
    s(r1, 0x00540014, VR::DS, "126.45");
    s(r1, 0x00540015, VR::DS, "154.55");
    s(w1, 0x00540018, VR::SH, "TC99M");
    Dataset& w2 = add_item(ds, 0x00540012);
    Dataset& r2 = add_item(w2, 0x00540013);
    s(r2, 0x00540014, VR::DS, "100");
    s(r2, 0x00540015, VR::DS, "120");
    // Frame Increment Pointer: Energy Window Vector, Detector Vector.
    const std::uint8_t fip[8] = {0x54, 0x00, 0x10, 0x00, 0x54, 0x00, 0x20, 0x00};
    (void)ds.set_bytes(0x00280009, VR::AT, Bytes::copy_of(fip, 8));
    u16s(ds, 0x00540010, {1, 1, 2, 2});
    u16s(ds, 0x00540020, {1, 2, 1, 2});
    u16s(ds, 0x00540050, {1, 1, 1, 1});  // present, but not in the pointer
    siaara_file* h = open_mem(f);
    REQUIRE(h);
    const siaara_dataset* d = main_ds(h);
    CHECK(doubles_at(d, "EnergyWindowInformationSequence[1].EnergyWindowRangeSequence[0].EnergyWindowUpperLimit") ==
          std::vector<double>{120});
    CHECK(text_at(d, "EnergyWindowInformationSequence[0].EnergyWindowName") == "TC99M");
    siaara_status st = SIAARA_OK;
    (void)text_at(d, "EnergyWindowInformationSequence[1].EnergyWindowName", nullptr, &st);
    CHECK(st == SIAARA_ERR_NOT_FOUND);
    siaara_nm_frame_index x{};
    x.struct_size = sizeof x;
    REQUIRE(siaara_nm_frame_index_get(d, 2, &x) == SIAARA_OK);
    CHECK(x.energy_window == 2 && x.detector == 1 && x.rotation == 1);
    CHECK(x.in_pointer == (SIAARA_NM_ENERGY_WINDOW | SIAARA_NM_DETECTOR));
    CHECK(x.present == (SIAARA_NM_ENERGY_WINDOW | SIAARA_NM_DETECTOR | SIAARA_NM_ROTATION));
    CHECK(x.phase == 0 && x.time_slot == 0);
    REQUIRE(siaara_nm_frame_index_get(d, 9, &x) == SIAARA_OK);  // past the vectors: 0, not invented
    CHECK(x.energy_window == 0 && x.detector == 0);
    siaara_file_free(h);
}

// ---------------------------------------------------------------- waveforms

TEST_CASE("named: ECG waveform raw and scaled samples") {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    s(ds, 0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.9.1.1");
    s(ds, 0x00080060, VR::CS, "ECG");
    Dataset& g = add_item(ds, 0x54000100);
    (void)g.set_uint16(0x003A0005, 2);
    (void)g.set_uint32(0x003A0010, 3);
    s(g, 0x003A001A, VR::DS, "500");
    s(g, 0x003A0020, VR::SH, "RHYTHM");
    (void)g.set_uint16(0x54001004, 16);
    s(g, 0x54001006, VR::CS, "SS");
    const std::int16_t pad = -32768;
    (void)g.set_bytes(0x5400100A, VR::OW, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(&pad), 2));
    Dataset& c1 = add_item(g, 0x003A0200);
    s(c1, 0x003A0203, VR::LO, "Lead I");
    s(c1, 0x003A0210, VR::DS, "2.5");
    s(c1, 0x003A0212, VR::DS, "1.1");
    s(c1, 0x003A0213, VR::DS, "-3");
    (void)c1.set_uint16(0x003A021A, 16);
    Dataset& u = add_item(c1, 0x003A0211);
    s(u, 0x00080100, VR::SH, "uV");
    s(u, 0x00080102, VR::SH, "UCUM");
    s(u, 0x00080104, VR::LO, "microvolt");
    Dataset& src = add_item(c1, 0x003A0208);
    s(src, 0x00080100, VR::SH, "5.6.3-9-1");
    s(src, 0x00080102, VR::SH, "SCPECG");
    s(src, 0x00080104, VR::LO, "Lead I (Einthoven)");
    Dataset& c2 = add_item(g, 0x003A0200);
    s(c2, 0x003A0203, VR::LO, "Marker");
    (void)c2.set_uint16(0x003A021A, 16);
    // C1S1 C2S1 C1S2 C2S2 C1S3 C2S3
    const std::int16_t data[6] = {10, 1, -20, 2, -32768, 3};
    (void)g.set_bytes(0x54001010, VR::OW, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(data), sizeof data));
    siaara_file* h = open_mem(f);
    REQUIRE(h);
    const siaara_dataset* d = main_ds(h);
    uint32_t n = 0;
    REQUIRE(siaara_waveform_group_count(d, &n) == SIAARA_OK);
    CHECK(n == 1);
    siaara_waveform_group gi{};
    gi.struct_size = sizeof gi;
    REQUIRE(siaara_waveform_group_info(d, 0, &gi) == SIAARA_OK);
    CHECK(gi.channels == 2 && gi.samples == 3 && gi.data_samples == 3 && gi.bits_allocated == 16);
    CHECK(gi.sampling_frequency == 500);
    CHECK(std::string(gi.sample_interpretation) == "SS");
    CHECK(gi.has_padding == 1 && gi.padding_value == -32768);
    siaara_waveform_channel ci{};
    ci.struct_size = sizeof ci;
    REQUIRE(siaara_waveform_channel_info(d, 0, 0, &ci) == SIAARA_OK);
    CHECK(ci.has_sensitivity == 1 && ci.sensitivity == 2.5 && ci.correction == 1.1 && ci.baseline == -3);
    char buf[64];
    size_t need = 0;
    REQUIRE(siaara_waveform_channel_string(d, 0, 0, SIAARA_WAVEFORM_LABEL, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::string(buf) == "Lead I");
    REQUIRE(siaara_waveform_channel_string(d, 0, 0, SIAARA_WAVEFORM_UNITS_CODE, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::string(buf) == "uV");
    REQUIRE(siaara_waveform_channel_string(d, 0, 0, SIAARA_WAVEFORM_SOURCE_MEANING, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::string(buf) == "Lead I (Einthoven)");
    REQUIRE(siaara_waveform_channel_string(d, 0, 0, SIAARA_WAVEFORM_GROUP_LABEL, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::string(buf) == "RHYTHM");
    CHECK(siaara_waveform_channel_string(d, 0, 1, SIAARA_WAVEFORM_UNITS_CODE, buf, sizeof buf, &need) == SIAARA_ERR_NOT_FOUND);
    int64_t raw[3];
    size_t cnt = 0;
    REQUIRE(siaara_waveform_raw_samples(d, 0, 0, raw, 3, &cnt) == SIAARA_OK);
    CHECK(cnt == 3 && raw[0] == 10 && raw[1] == -20 && raw[2] == -32768);
    double sc[3];
    REQUIRE(siaara_waveform_samples(d, 0, 0, 1, sc, 3, &cnt) == SIAARA_OK);
    CHECK(std::fabs(sc[0] - (10 * 2.5 * 1.1 - 3)) < 1e-9);
    CHECK(std::fabs(sc[1] - (-20 * 2.5 * 1.1 - 3)) < 1e-9);
    CHECK(std::isnan(sc[2]));  // padding value
    REQUIRE(siaara_waveform_samples(d, 0, 1, 0, sc, 3, &cnt) == SIAARA_OK);
    CHECK(sc[0] == 1 && sc[2] == 3);
    CHECK(siaara_waveform_samples(d, 0, 1, 1, sc, 3, &cnt) == SIAARA_ERR_NOT_FOUND);  // no sensitivity: no scaled values
    CHECK(siaara_waveform_samples(d, 0, 2, 0, sc, 3, &cnt) == SIAARA_ERR_NOT_FOUND);
    siaara_file_free(h);
}

TEST_CASE("named: G.711 expansion") {
    CHECK(modules::g711_mulaw(0xFF) == 0);
    CHECK(modules::g711_mulaw(0x00) == -8031);
    CHECK(modules::g711_mulaw(0x80) == 8031);
    CHECK(modules::g711_alaw(0xD5) == 1);   // A-law code 0xD5: smallest positive step (8 >> 3)
    CHECK(modules::g711_alaw(0xAA) == 4032);
    CHECK(modules::g711_alaw(0x2A) == -4032);
}

// ---------------------------------------------------------------- classifiers and dictionary

TEST_CASE("named: SOP class, transfer syntax and dictionary queries") {
    siaara_sop_class_info si{};
    si.struct_size = sizeof si;
    const char* plan = "1.2.840.10008.5.1.4.1.1.481.5";
    REQUIRE(siaara_sop_class_info_get(plan, std::strlen(plan), &si) == SIAARA_OK);
    CHECK(si.object_kind == SIAARA_OBJECT_RT_PLAN);
    CHECK(si.has_iod == 1);
    CHECK(std::string(si.iod_name) == "RT Plan");
    const char* ct = "1.2.840.10008.5.1.4.1.1.2";
    REQUIRE(siaara_sop_class_info_get(ct, std::strlen(ct), &si) == SIAARA_OK);
    CHECK(si.object_kind == SIAARA_OBJECT_IMAGE);
    const char* dose = "1.2.840.10008.5.1.4.1.1.481.2";
    REQUIRE(siaara_sop_class_info_get(dose, std::strlen(dose), &si) == SIAARA_OK);
    CHECK(si.object_kind == SIAARA_OBJECT_RT_DOSE);
    const char* ecg = "1.2.840.10008.5.1.4.1.1.9.1.1";
    REQUIRE(siaara_sop_class_info_get(ecg, std::strlen(ecg), &si) == SIAARA_OK);
    CHECK(si.object_kind == SIAARA_OBJECT_WAVEFORM);
    const char* ts = "1.2.840.10008.1.2.4.50";
    CHECK(siaara_sop_class_info_get(ts, std::strlen(ts), &si) == SIAARA_ERR_NOT_FOUND);
    siaara_transfer_syntax_info ti{};
    ti.struct_size = sizeof ti;
    REQUIRE(siaara_transfer_syntax_info_get(ts, std::strlen(ts), &ti) == SIAARA_OK);
    CHECK(ti.encapsulated == 1 && ti.lossy == 1 && ti.explicit_vr == 1 && ti.big_endian == 0);
    const char* defl = "1.2.840.10008.1.2.1.99";
    REQUIRE(siaara_transfer_syntax_info_get(defl, std::strlen(defl), &ti) == SIAARA_OK);
    CHECK(ti.deflated == 1 && ti.encapsulated == 0);
    uint32_t count = 0;
    REQUIRE(siaara_dict_count(&count) == SIAARA_OK);
    CHECK(count > 4000);
    siaara_dict_entry e{};
    e.struct_size = sizeof e;
    REQUIRE(siaara_dict_entry_at(0, &e) == SIAARA_OK);
    CHECK(siaara_dict_entry_at(count, &e) == SIAARA_ERR_NOT_FOUND);
    size_t hits = 0;
    uint32_t idx[64];
    REQUIRE(siaara_dict_search("leaf/jaw pos", 12, nullptr, SIAARA_DICT_MATCH_NAME, idx, 64, &hits) == SIAARA_OK);
    REQUIRE(hits >= 1);
    REQUIRE(siaara_dict_entry_at(idx[0], &e) == SIAARA_OK);
    CHECK(std::string(e.keyword) == "LeafJawPositions");
    REQUIRE(siaara_dict_search("", 0, "PN", 0, nullptr, 0, &hits) == SIAARA_OK);
    CHECK(hits > 20);
    CHECK(siaara_dict_search("", 0, "Q", 0, nullptr, 0, &hits) == SIAARA_ERR_INVALID_ARGUMENT);
}
