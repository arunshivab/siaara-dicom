// C API tests for the viewer additions: decoded frames, 2D viewport rendering, VOI, overlays,
// shutters, palettes, sequence items, multi-valued strings and the live-handle counter.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "siaara/siaara.h"
#include "test.hpp"

namespace {

using Bytes8 = std::vector<std::uint8_t>;

void set_s(siaara_dataset* ds, uint32_t tag, const char* vr, const std::string& v) {
    (void)siaara_dataset_set_string(ds, tag, vr, v.c_str(), v.size());
}
void set_u16s(siaara_dataset* ds, uint32_t tag, const char* vr, std::initializer_list<int> vals) {
    Bytes8 b;
    for (int v : vals) {
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    (void)siaara_dataset_set_bytes(ds, tag, vr, b.data(), b.size());
}

// A native MONOCHROME2 (or other) image built through the C API.
siaara_file* make_image(uint32_t rows, uint32_t cols, uint16_t bits, bool is_signed, const Bytes8& px,
                        const char* photometric = "MONOCHROME2", uint16_t samples = 1) {
    siaara_file* f = nullptr;
    if (siaara_file_create(&f) != SIAARA_OK) return nullptr;
    siaara_dataset* ds = nullptr;
    (void)siaara_file_dataset(f, &ds);
    set_s(ds, 0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.7");
    set_s(ds, 0x00080018, "UI", "2.25.1234");
    (void)siaara_dataset_set_uint16(ds, 0x00280002, samples);
    set_s(ds, 0x00280004, "CS", photometric);
    (void)siaara_dataset_set_uint16(ds, 0x00280010, static_cast<uint16_t>(rows));
    (void)siaara_dataset_set_uint16(ds, 0x00280011, static_cast<uint16_t>(cols));
    (void)siaara_dataset_set_uint16(ds, 0x00280100, bits);
    (void)siaara_dataset_set_uint16(ds, 0x00280101, bits);
    (void)siaara_dataset_set_uint16(ds, 0x00280102, static_cast<uint16_t>(bits - 1));
    (void)siaara_dataset_set_uint16(ds, 0x00280103, is_signed ? 1 : 0);
    if (samples == 3) (void)siaara_dataset_set_uint16(ds, 0x00280006, 0);
    (void)siaara_dataset_set_bytes(ds, 0x7FE00010, bits > 8 ? "OW" : "OB", px.data(), px.size());
    return f;
}

// 2x2 8-bit image A=10 B=20 / C=30 D=40.
siaara_file* make_2x2() { return make_image(2, 2, 8, false, {10, 20, 30, 40}); }

siaara_view2d_options opts(uint32_t w, uint32_t h) {
    siaara_view2d_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.viewport_width = w;
    o.viewport_height = h;
    o.voi_mode = SIAARA_VOI_IDENTITY;
    o.background_rgba[3] = 255;
    o.background_rgba[0] = 7;
    return o;
}

Bytes8 render_gray(const siaara_frame* fr, const siaara_view2d_options& o) {
    siaara_view2d_options g = o;
    g.output_format = SIAARA_PIXELS_GRAY8;
    size_t need = 0;
    if (siaara_frame_render(fr, &g, nullptr, 0, &need) != SIAARA_OK) return {};
    Bytes8 out(need);
    if (siaara_frame_render(fr, &g, out.data(), out.size(), &need) != SIAARA_OK) return {};
    return out;
}

int64_t live() {
    int64_t n = -1;
    (void)siaara_debug_live_handles(&n);
    return n;
}

}  // namespace

TEST_CASE("capi/view2d: frame open, info, pixels, render equals siaara_render_rgba8") {
    const int64_t before = live();
    Bytes8 px;
    for (int v : {-1000, -24, 40, 1000, 3000, 0}) {
        px.push_back(static_cast<std::uint8_t>(v & 0xFF));
        px.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    siaara_file* f = make_image(2, 3, 16, true, px);
    REQUIRE(f);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    set_s(ds, 0x00281052, "DS", "-1024");
    set_s(ds, 0x00281053, "DS", "1");
    set_s(ds, 0x00281050, "DS", "40\\-600");
    set_s(ds, 0x00281051, "DS", "400\\1500");
    set_s(ds, 0x00281055, "LO", "SOFT\\LUNG");
    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(f, 0, &fr) == SIAARA_OK);
    CHECK(live() == before + 2);
    siaara_frame_properties fp;
    std::memset(&fp, 0, sizeof fp);
    fp.struct_size = sizeof fp;
    REQUIRE(siaara_frame_info(fr, &fp) == SIAARA_OK);
    CHECK(fp.rows == 2 && fp.columns == 3 && fp.voi_count == 2 && fp.overlay_count == 0);
    CHECK((fp.flags & SIAARA_FRAMEF_MONOCHROME) != 0);
    CHECK(fp.rescale_slope == 1.0 && fp.rescale_intercept == -1024.0);
    CHECK(fp.min_value == -2024.0 && fp.max_value == 1976.0);
    CHECK(std::strcmp(fp.photometric, "MONOCHROME2") == 0);
    CHECK(fp.decoded_bytes == 12);
    const uint8_t* data = nullptr;
    uint64_t bytes = 0;
    REQUIRE(siaara_frame_pixels(fr, &data, &bytes) == SIAARA_OK);
    CHECK(bytes == 12 && std::memcmp(data, px.data(), 12) == 0);

    // The frame survives the file.
    Bytes8 expect(24), got(24);
    size_t need = 0;
    REQUIRE(siaara_render_rgba8(f, nullptr, expect.data(), expect.size(), &need) == SIAARA_OK);
    siaara_file_free(f);
    REQUIRE(siaara_frame_render(fr, nullptr, got.data(), got.size(), &need) == SIAARA_OK);
    CHECK(need == 24 && got == expect);

    // VOI list and choice by index.
    uint32_t n = 0;
    REQUIRE(siaara_voi_count(fr, &n) == SIAARA_OK);
    CHECK(n == 2);
    siaara_voi_info vi;
    std::memset(&vi, 0, sizeof vi);
    vi.struct_size = sizeof vi;
    char text[32];
    REQUIRE(siaara_voi_get(fr, 1, &vi, text, sizeof text, &need) == SIAARA_OK);
    CHECK(vi.kind == SIAARA_VOI_KIND_WINDOW && vi.center == -600 && vi.width == 1500);
    CHECK(std::strcmp(text, "LUNG") == 0 && need == 4);
    CHECK(siaara_voi_get(fr, 2, &vi, nullptr, 0, &need) == SIAARA_ERR_NOT_FOUND);
    siaara_view2d_options o = opts(0, 0);
    o.voi_mode = SIAARA_VOI_FILE;
    o.voi_index = 1;
    REQUIRE(siaara_frame_render(fr, &o, got.data(), got.size(), &need) == SIAARA_OK);
    siaara_render_options ro;
    std::memset(&ro, 0, sizeof ro);
    ro.struct_size = sizeof ro;
    ro.voi_mode = SIAARA_VOI_EXPLICIT;
    ro.window_center = -600;
    ro.window_width = 1500;
    siaara_file* f2 = make_image(2, 3, 16, true, px);
    REQUIRE(siaara_file_dataset(f2, &ds) == SIAARA_OK);
    set_s(ds, 0x00281052, "DS", "-1024");
    set_s(ds, 0x00281053, "DS", "1");
    REQUIRE(siaara_render_rgba8(f2, &ro, expect.data(), expect.size(), &need) == SIAARA_OK);
    CHECK(got == expect);
    o.voi_index = 2;
    CHECK(siaara_frame_render(fr, &o, got.data(), got.size(), &need) == SIAARA_ERR_NOT_FOUND);
    // Explicit window with the SIGMOID function, and an invalid LINEAR width.
    o.voi_mode = SIAARA_VOI_EXPLICIT;
    o.window_center = 0;
    o.window_width = 0.5;
    CHECK(siaara_frame_render(fr, &o, got.data(), got.size(), &need) == SIAARA_ERR_INVALID_ARGUMENT);
    o.voi_function = SIAARA_VOI_SIGMOID;
    CHECK(siaara_frame_render(fr, &o, got.data(), got.size(), &need) == SIAARA_OK);
    siaara_file_free(f2);
    siaara_frame_free(fr);
    siaara_frame_free(nullptr);
    CHECK(live() == before);
}

TEST_CASE("capi/view2d: zoom, rotation, flips, pan, background, interpolation, gray output, map_point") {
    siaara_file* f = make_2x2();
    REQUIRE(f);
    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(f, 0, &fr) == SIAARA_OK);
    siaara_view2d_options o = opts(4, 4);
    o.zoom = 2;
    CHECK(render_gray(fr, o) == Bytes8({10, 10, 20, 20, 10, 10, 20, 20, 30, 30, 40, 40, 30, 30, 40, 40}));
    o.zoom = 0;  // fit: also 2
    CHECK(render_gray(fr, o) == Bytes8({10, 10, 20, 20, 10, 10, 20, 20, 30, 30, 40, 40, 30, 30, 40, 40}));
    o = opts(2, 2);
    o.rotation = 90;
    CHECK(render_gray(fr, o) == Bytes8({30, 10, 40, 20}));
    o.rotation = -270;
    CHECK(render_gray(fr, o) == Bytes8({30, 10, 40, 20}));
    o.rotation = 180;
    CHECK(render_gray(fr, o) == Bytes8({40, 30, 20, 10}));
    o.rotation = 0;
    o.flip_horizontal = 1;
    CHECK(render_gray(fr, o) == Bytes8({20, 10, 40, 30}));
    o.flip_horizontal = 0;
    o.flip_vertical = 1;
    CHECK(render_gray(fr, o) == Bytes8({30, 40, 10, 20}));
    o = opts(4, 2);
    o.zoom = 1;
    CHECK(render_gray(fr, o) == Bytes8({7, 10, 20, 7, 7, 30, 40, 7}));
    o.pan_x = 1;
    CHECK(render_gray(fr, o) == Bytes8({7, 7, 10, 20, 7, 7, 30, 40}));
    o = opts(2, 2);
    o.interpolation = SIAARA_INTERP_LINEAR;
    CHECK(render_gray(fr, o) == Bytes8({10, 20, 30, 40}));
    o = opts(3, 3);
    o.zoom = 1.5;
    o.interpolation = SIAARA_INTERP_LINEAR;
    const Bytes8 lin = render_gray(fr, o);
    REQUIRE(lin.size() == 9);
    CHECK(lin[0] == 10 && lin[2] == 20 && lin[6] == 30 && lin[8] == 40);
    CHECK(lin[4] == 25);  // centre: mean of the four
    o.interpolation = SIAARA_INTERP_CUBIC;
    CHECK(siaara_frame_render(fr, &o, nullptr, 0, nullptr) == SIAARA_ERR_UNSUPPORTED);

    // RGBA output and the background colour.
    o = opts(4, 2);
    o.zoom = 1;
    o.background_rgba[1] = 9;
    Bytes8 rgba(32);
    size_t need = 0;
    REQUIRE(siaara_frame_render(fr, &o, rgba.data(), rgba.size(), &need) == SIAARA_OK);
    CHECK(need == 32 && rgba[0] == 7 && rgba[1] == 9 && rgba[3] == 255);
    CHECK(rgba[4] == 10 && rgba[5] == 10 && rgba[6] == 10 && rgba[7] == 255);
    CHECK(siaara_frame_render(fr, &o, rgba.data(), 31, &need) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(siaara_frame_render(fr, &o, nullptr, 0, &need) == SIAARA_OK && need == 32);

    // Viewport <-> image coordinates.
    o = opts(4, 4);
    o.zoom = 2;
    double x = 0, y = 0;
    REQUIRE(siaara_frame_map_point(fr, &o, 1, 1, 1, &x, &y) == SIAARA_OK);
    CHECK(x == 0.5 && y == 0.5);
    o.rotation = 90;
    REQUIRE(siaara_frame_map_point(fr, &o, 0, 0, 0, &x, &y) == SIAARA_OK);
    CHECK(std::fabs(x - 4) < 1e-12 && std::fabs(y - 0) < 1e-12);  // top left corner goes to the top right
    double u = 0, v = 0;
    REQUIRE(siaara_frame_map_point(fr, &o, 1, x, y, &u, &v) == SIAARA_OK);
    CHECK(std::fabs(u) < 1e-12 && std::fabs(v) < 1e-12);

    // Argument checks.
    o.struct_size = 8;
    CHECK(siaara_frame_render(fr, &o, nullptr, 0, &need) == SIAARA_ERR_INVALID_ARGUMENT);
    o = opts(4, 0);
    CHECK(siaara_frame_render(fr, &o, nullptr, 0, &need) == SIAARA_ERR_INVALID_ARGUMENT);
    o = opts(2, 2);
    o.zoom = -1;
    CHECK(siaara_frame_render(fr, &o, rgba.data(), rgba.size(), &need) == SIAARA_ERR_INVALID_ARGUMENT);
    o = opts(2, 2);
    o.colormap = 99;
    CHECK(siaara_frame_render(fr, &o, nullptr, 0, &need) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_frame_render(nullptr, nullptr, nullptr, 0, &need) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_frame_open(f, 1, &fr) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_frame_free(fr);
    siaara_file_free(f);
}

TEST_CASE("capi/view2d: colour maps, palettes, zero-copy frame view") {
    // Grayscale with a standard colour map = the colour map applied to the gray levels.
    Bytes8 ramp(256);
    for (int i = 0; i < 256; ++i) ramp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    siaara_file* f = make_image(16, 16, 8, false, ramp);
    REQUIRE(f);
    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(f, 0, &fr) == SIAARA_OK);
    Bytes8 map(1024);
    size_t need = 0;
    for (int cm = SIAARA_COLORMAP_GRAY; cm <= SIAARA_COLORMAP_PET_20_STEP; ++cm) {
        REQUIRE(siaara_colormap_rgba8(cm, map.data(), map.size(), &need) == SIAARA_OK);
        siaara_view2d_options o = opts(0, 0);
        o.colormap = cm;
        Bytes8 out(1024);
        REQUIRE(siaara_frame_render(fr, &o, out.data(), out.size(), &need) == SIAARA_OK);
        CHECK(out == map);
    }
    CHECK(siaara_colormap_rgba8(SIAARA_COLORMAP_INVERSE_GRAY, map.data(), map.size(), &need) == SIAARA_OK && map[0] == 255);
    CHECK(siaara_colormap_rgba8(-1, map.data(), map.size(), &need) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_frame_palette(fr, nullptr, nullptr, 0, &need) == SIAARA_ERR_NOT_FOUND);

    // Zero-copy view of native pixel data.
    const uint8_t* view = nullptr;
    uint64_t bytes = 0;
    REQUIRE(siaara_file_frame_view(f, 0, &view, &bytes) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    const uint8_t* raw = nullptr;
    size_t raw_size = 0;
    REQUIRE(siaara_dataset_get_raw(ds, 0x7FE00010, &raw, &raw_size) == SIAARA_OK);
    CHECK(view == raw && bytes == 256);
    CHECK(siaara_file_frame_view(f, 1, &view, &bytes) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_frame_free(fr);
    siaara_file_free(f);

    // PALETTE COLOR image: 4 entries of 16 bits, first mapped 1.
    f = make_image(1, 4, 8, false, {0, 1, 2, 4}, "PALETTE COLOR");
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    for (uint32_t k = 0; k < 3; ++k) {
        set_u16s(ds, 0x00281101 + k, "US", {4, 1, 16});
        const int base = static_cast<int>(k) * 0x1000;
        set_u16s(ds, 0x00281201 + k, "OW", {base, base + 0x4000, base + 0x8000, 0xFFFF});
    }
    REQUIRE(siaara_frame_open(f, 0, &fr) == SIAARA_OK);
    siaara_palette_info pinfo;
    std::memset(&pinfo, 0, sizeof pinfo);
    pinfo.struct_size = sizeof pinfo;
    REQUIRE(siaara_frame_palette(fr, &pinfo, nullptr, 0, &need) == SIAARA_OK);
    CHECK(pinfo.entries == 4 && pinfo.first_mapped == 1 && pinfo.bits == 16 && need == 16);
    std::vector<uint16_t> pal(need);
    REQUIRE(siaara_frame_palette(fr, &pinfo, pal.data(), pal.size(), &need) == SIAARA_OK);
    CHECK(pal[0] == 0 && pal[1] == 0x1000 && pal[2] == 0x2000 && pal[3] == 0xFFFF);
    CHECK(pal[12] == 0xFFFF && pal[13] == 0xFFFF);
    CHECK(siaara_frame_palette(fr, &pinfo, pal.data(), 3, &need) == SIAARA_ERR_BUFFER_TOO_SMALL);
    siaara_frame_properties fp;
    std::memset(&fp, 0, sizeof fp);
    fp.struct_size = sizeof fp;
    REQUIRE(siaara_frame_info(fr, &fp) == SIAARA_OK);
    CHECK((fp.flags & SIAARA_FRAMEF_PALETTE) != 0 && (fp.flags & SIAARA_FRAMEF_MONOCHROME) == 0);
    Bytes8 out(16);
    REQUIRE(siaara_frame_render(fr, nullptr, out.data(), out.size(), &need) == SIAARA_OK);
    CHECK(out[0] == 0 && out[4] == 0 && out[8] == 64 && out[12] == 255);  // values below first mapped -> entry 0
    siaara_frame_free(fr);
    siaara_file_free(f);
}

TEST_CASE("capi/view2d: overlay and shutter masks, VOI LUT data, sequence items") {
    siaara_file* f = make_image(2, 4, 8, false, {0, 50, 100, 150, 200, 250, 25, 75});
    REQUIRE(f);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    // Overlay group 6002: 2x4 plane, bits set at pixels 0 and 5.
    (void)siaara_dataset_set_uint16(ds, 0x60020010, 2);
    (void)siaara_dataset_set_uint16(ds, 0x60020011, 4);
    set_s(ds, 0x60020040, "CS", "G");
    set_u16s(ds, 0x60020050, "SS", {1, 1});
    (void)siaara_dataset_set_uint16(ds, 0x60020100, 1);
    (void)siaara_dataset_set_uint16(ds, 0x60020102, 0);
    set_s(ds, 0x60021500, "LO", "MARK");
    const uint8_t bits[2] = {0x21, 0x00};
    (void)siaara_dataset_set_bytes(ds, 0x60023000, "OW", bits, 2);
    // Rectangular shutter: columns 2..3, rows 1..2 visible.
    set_s(ds, 0x00181600, "CS", "RECTANGULAR");
    set_s(ds, 0x00181602, "IS", "2");
    set_s(ds, 0x00181604, "IS", "3");
    set_s(ds, 0x00181606, "IS", "1");
    set_s(ds, 0x00181608, "IS", "2");
    // VOI LUT Sequence with one 4-entry 8-bit LUT, built with siaara_dataset_append_item.
    siaara_dataset* item = nullptr;
    uint32_t idx = 99;
    REQUIRE(siaara_dataset_append_item(ds, 0x00283010, &item, &idx) == SIAARA_OK);
    CHECK(idx == 0);
    set_u16s(item, 0x00283002, "US", {4, 0, 8});
    set_u16s(item, 0x00283006, "OW", {0, 100, 200, 255});
    set_s(item, 0x00283003, "LO", "CURVE");

    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(f, 0, &fr) == SIAARA_OK);
    siaara_frame_properties fp;
    std::memset(&fp, 0, sizeof fp);
    fp.struct_size = sizeof fp;
    REQUIRE(siaara_frame_info(fr, &fp) == SIAARA_OK);
    CHECK(fp.overlay_count == 1 && fp.voi_count == 1 && (fp.flags & SIAARA_FRAMEF_SHUTTER_RECTANGULAR));
    siaara_overlay_info oi;
    std::memset(&oi, 0, sizeof oi);
    oi.struct_size = sizeof oi;
    REQUIRE(siaara_overlay_get(fr, 0, &oi) == SIAARA_OK);
    CHECK(oi.group == 0x6002 && oi.rows == 2 && oi.columns == 4 && oi.type == 'G' && oi.covers_frame == 1);
    char label[16];
    size_t need = 0;
    REQUIRE(siaara_overlay_text(fr, 0, SIAARA_OVERLAY_LABEL, label, sizeof label, &need) == SIAARA_OK);
    CHECK(std::strcmp(label, "MARK") == 0);
    Bytes8 mask(8);
    REQUIRE(siaara_overlay_mask(fr, 0, mask.data(), mask.size(), &need) == SIAARA_OK);
    CHECK(mask == Bytes8({1, 0, 0, 0, 0, 1, 0, 0}));
    CHECK(siaara_overlay_mask(fr, 1, mask.data(), mask.size(), &need) == SIAARA_ERR_NOT_FOUND);
    REQUIRE(siaara_shutter_mask(fr, mask.data(), mask.size(), &need) == SIAARA_OK);
    CHECK(mask == Bytes8({0, 1, 1, 0, 0, 1, 1, 0}));

    siaara_voi_info vi;
    std::memset(&vi, 0, sizeof vi);
    vi.struct_size = sizeof vi;
    char text[16];
    REQUIRE(siaara_voi_get(fr, 0, &vi, text, sizeof text, &need) == SIAARA_OK);
    CHECK(vi.kind == SIAARA_VOI_KIND_LUT && vi.lut_entries == 4 && vi.lut_bits == 8 && std::strcmp(text, "CURVE") == 0);
    uint16_t lut[4] = {};
    REQUIRE(siaara_voi_lut_data(fr, 0, lut, 4, &need) == SIAARA_OK);
    CHECK(need == 4 && lut[1] == 100 && lut[3] == 255);
    CHECK(siaara_voi_lut_data(fr, 0, lut, 3, &need) == SIAARA_ERR_BUFFER_TOO_SMALL);
    // Render: shutter black outside, overlays white on top.
    siaara_view2d_options o = opts(0, 0);
    o.voi_mode = SIAARA_VOI_IDENTITY;
    Bytes8 g = render_gray(fr, o);
    CHECK(g == Bytes8({255, 50, 100, 0, 0, 255, 25, 0}));  // overlays are drawn over the shutter
    o.no_overlays = 1;
    o.no_shutters = 1;
    CHECK(render_gray(fr, o) == Bytes8({0, 50, 100, 150, 200, 250, 25, 75}));
    o.no_overlays = 0;
    o.overlay_groups = 1;  // only 6000: the overlay in 6002 is hidden
    CHECK(render_gray(fr, o) == Bytes8({0, 50, 100, 150, 200, 250, 25, 75}));
    siaara_frame_free(fr);

    // More items, removal, and the error cases.
    REQUIRE(siaara_dataset_append_item(ds, 0x00283010, &item, &idx) == SIAARA_OK);
    CHECK(idx == 1);
    uint32_t count = 0;
    REQUIRE(siaara_dataset_item_count(ds, 0x00283010, &count) == SIAARA_OK);
    CHECK(count == 2);
    REQUIRE(siaara_dataset_remove_item(ds, 0x00283010, 0) == SIAARA_OK);
    CHECK(siaara_dataset_item_count(ds, 0x00283010, &count) == SIAARA_OK && count == 1);
    CHECK(siaara_dataset_remove_item(ds, 0x00283010, 5) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_dataset_append_item(ds, 0x00080018, &item, nullptr) == SIAARA_ERR_TYPE_MISMATCH);
    CHECK(siaara_dataset_append_item(ds, 0x7FE00010, &item, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file_free(f);
}

TEST_CASE("capi/view2d: multi-valued strings and the live-handle counter") {
    const int64_t before = live();
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_create(&f) == SIAARA_OK);
    CHECK(live() == before + 1);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    set_s(ds, 0x00080005, "CS", "ISO_IR 100");
    set_s(ds, 0x00080008, "CS", "ORIGINAL\\PRIMARY\\AXIAL");
    const char latin1[] = {'M', static_cast<char>(0xFC), 'l', 'l', 'e', 'r', '^', 'J', '\\', 'B', 0};
    REQUIRE(siaara_dataset_set_bytes(ds, 0x00100010, "PN", reinterpret_cast<const uint8_t*>(latin1), 10) == SIAARA_OK);
    char buf[64];
    size_t need = 0;
    uint32_t count = 0;
    REQUIRE(siaara_dataset_get_strings(ds, 0x00080008, 0, buf, sizeof buf, &need, &count) == SIAARA_OK);
    CHECK(count == 3 && need == 23);
    CHECK(std::memcmp(buf, "ORIGINAL\0PRIMARY\0AXIAL\0", 23) == 0);
    REQUIRE(siaara_dataset_get_strings(ds, 0x00100010, 1, buf, sizeof buf, &need, &count) == SIAARA_OK);
    CHECK(count == 2 && std::strcmp(buf, "M\xC3\xBCller^J") == 0 && std::strcmp(buf + 10, "B") == 0);
    CHECK(siaara_dataset_get_strings(ds, 0x00080008, 0, nullptr, 0, &need, &count) == SIAARA_OK && need == 23);
    CHECK(siaara_dataset_get_strings(ds, 0x00080008, 0, buf, 10, &need, &count) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(siaara_dataset_get_strings(ds, 0x00080009, 0, buf, sizeof buf, &need, &count) == SIAARA_ERR_NOT_FOUND);
    siaara_file_free(f);
    CHECK(live() == before);
    CHECK(siaara_debug_live_handles(nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------- presentation states
#include "clinical/gsps.hpp"
#include "core/writer.hpp"

TEST_CASE("capi/view2d: presentation state read, info, applies, render with rotation and window") {
    // 2x3 8-bit image and a GSPS that rotates by 90 degrees with window 100/101.
    siaara_file* img = make_image(2, 3, 8, false, {0, 50, 100, 150, 200, 250});
    REQUIRE(img);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(img, &ds) == SIAARA_OK);
    set_s(ds, 0x0020000E, "UI", "2.25.77");
    set_s(ds, 0x0020000D, "UI", "2.25.78");
    siaara::clinical::GspsWriteRequest req;
    siaara::Dataset src;
    (void)src.set_string(siaara::tags::StudyInstanceUID, siaara::VR::UI, "2.25.78");
    req.source = &src;
    siaara::clinical::ImageReference ref;
    ref.sop_class_uid = "1.2.840.10008.5.1.4.1.1.7";
    ref.sop_instance_uid = "2.25.1234";
    ref.series_uid = "2.25.77";
    ref.rows = 2;
    ref.columns = 3;
    req.images.push_back(ref);
    siaara::image::VoiWindow w;
    w.center = 100;
    w.width = 101;
    req.window = w;
    req.rotation = 90;
    req.content_label = "VIEW";
    auto gs = siaara::clinical::write_gsps(req);
    REQUIRE(gs.is_ok());
    std::vector<std::uint8_t> bytes;
    REQUIRE(siaara::write_part10(*gs, bytes).is_ok());
    siaara_file* psf = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &psf) == SIAARA_OK);
    siaara_pstate* ps = nullptr;
    REQUIRE(siaara_pstate_open(psf, &ps) == SIAARA_OK);
    siaara_file_free(psf);
    siaara_pstate_properties pp;
    std::memset(&pp, 0, sizeof pp);
    pp.struct_size = sizeof pp;
    REQUIRE(siaara_pstate_info(ps, &pp) == SIAARA_OK);
    CHECK(pp.kind == SIAARA_PSTATE_GRAYSCALE && pp.rotation == 90 && pp.reference_count == 1 && pp.voi_count == 1);
    char buf[64];
    size_t need = 0;
    REQUIRE(siaara_pstate_string(ps, SIAARA_PSTATE_LABEL, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK(std::strcmp(buf, "VIEW") == 0);
    uint32_t frames = 9;
    REQUIRE(siaara_pstate_reference(ps, 0, buf, sizeof buf, &need, &frames) == SIAARA_OK);
    CHECK(std::strcmp(buf, "2.25.1234") == 0 && frames == 0);
    int32_t applies = 0;
    REQUIRE(siaara_pstate_applies(ps, "2.25.1234", 9, 1, &applies) == SIAARA_OK);
    CHECK(applies == 1);
    REQUIRE(siaara_pstate_applies(ps, "2.25.9", 6, 1, &applies) == SIAARA_OK);
    CHECK(applies == 0);
    siaara_pstate* not_ps = nullptr;
    CHECK(siaara_pstate_open(img, &not_ps) == SIAARA_ERR_UNSUPPORTED && not_ps == nullptr);

    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(img, 0, &fr) == SIAARA_OK);
    siaara_view2d_options o = opts(0, 0);
    o.presentation_state = ps;
    const Bytes8 with_ps = render_gray(fr, o);
    siaara_view2d_options plain = opts(2, 3);
    plain.zoom = 1;
    plain.rotation = 90;
    plain.voi_mode = SIAARA_VOI_EXPLICIT;
    plain.window_center = 100;
    plain.window_width = 101;
    const Bytes8 expect = render_gray(fr, plain);
    CHECK(with_ps.size() == 6 && with_ps == expect);
    // The point mapping goes through the presentation state's rotation.
    double x = 0, y = 0;
    REQUIRE(siaara_frame_map_point(fr, &o, 0, 0, 0, &x, &y) == SIAARA_OK);
    CHECK(std::fabs(x - 2) < 1e-9 && std::fabs(y) < 1e-9);
    double u = 0, v = 0;
    REQUIRE(siaara_frame_map_point(fr, &o, 1, x, y, &u, &v) == SIAARA_OK);
    CHECK(std::fabs(u) < 1e-9 && std::fabs(v) < 1e-9);
    siaara_frame_free(fr);
    siaara_pstate_free(ps);
    siaara_file_free(img);
}
