// Clinical layer: softcopy presentation states (src/clinical/gsps.*): reading, the rendering
// helper (VOI, Presentation LUT, shutters, annotations, rotation / flip, displayed area), the GSPS
// writer round trip, robustness; plus the UID generator and DS formatting from common.*.
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/gsps.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "image/render.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

constexpr std::string_view kTs = "1.2.840.10008.1.2.1";
constexpr std::string_view kCt = "1.2.840.10008.5.1.4.1.1.2";

void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }

// 3 columns x 2 rows, 8-bit MONOCHROME2, value = 10 x row + column.
Dataset small_image() {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, kCt);
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.100");
    put(d, siaara::tags::StudyInstanceUID, VR::UI, "1.2.3.1");
    put(d, siaara::tags::SeriesInstanceUID, VR::UI, "1.2.3.2");
    put(d, siaara::tags::PatientName, VR::PN, "Doe^Jane");
    put(d, siaara::tags::PatientID, VR::LO, "ID1");
    (void)d.set_uint16(siaara::tags::Rows, 2);
    (void)d.set_uint16(siaara::tags::Columns, 3);
    (void)d.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)d.set_uint16(siaara::tags::BitsAllocated, 8);
    (void)d.set_uint16(siaara::tags::BitsStored, 8);
    (void)d.set_uint16(siaara::tags::HighBit, 7);
    (void)d.set_uint16(siaara::tags::PixelRepresentation, 0);
    put(d, siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    const std::uint8_t px[6] = {0, 1, 2, 10, 11, 12};
    (void)d.set_bytes(siaara::tags::PixelData, VR::OB, Bytes::copy_of(px, 6));
    return d;
}

PresentationState window_ps(double c = 128, double w = 256) {
    PresentationState ps;
    ps.kind = PresentationKind::Grayscale;
    SoftcopyVoi v;
    image::VoiWindow win;
    win.center = c;
    win.width = w;
    v.windows.push_back(win);
    ps.voi.push_back(v);
    return ps;
}

std::uint8_t gray(const PsRendered& r, std::uint32_t x, std::uint32_t y) { return r.rgba[4 * (y * r.width + x)]; }

ImageReference ref_small() {
    ImageReference r;
    r.sop_class_uid = std::string(kCt);
    r.sop_instance_uid = "1.2.3.100";
    r.series_uid = "1.2.3.2";
    r.rows = 2;
    r.columns = 3;
    return r;
}

}  // namespace

TEST_CASE("common: 2.25 UID generator and DS formatting") {
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i) {
        const std::string u = new_uid();
        CHECK(u.rfind("2.25.", 0) == 0);
        CHECK(u.size() <= 44);  // 5 + at most 39 digits of a 128-bit value
        CHECK(uid_valid(u));
        seen.insert(u);
    }
    CHECK_EQ(seen.size(), 2000u);
    CHECK(uid_valid("1.2.840.10008.1.2"));
    CHECK(!uid_valid("1.02.3"));
    CHECK(!uid_valid("1..2"));
    CHECK(!uid_valid("1.2."));
    CHECK(!uid_valid("1.2.a"));
    CHECK(!uid_valid(std::string(65, '1')));
    CHECK_EQ(format_ds(0.5), std::string("0.5"));
    CHECK_EQ(format_ds(-1024), std::string("-1024"));
    const double vals[] = {1.0 / 3.0, 123456.789012345, 6.02214076e23, -1e-300, 3.14159265358979};
    for (double v : vals) {
        const std::string s = format_ds(v);
        CHECK(s.size() <= 16);
        auto back = parse_decimal(s);
        CHECK(back && std::fabs(*back - v) <= 1e-9 * std::fabs(v));
    }
    CHECK(cs_valid("ANNOTATIONS") && !cs_valid("lower") && !cs_valid("") && !cs_valid("ABCDEFGHIJKLMNOPQ"));
}

TEST_CASE("gsps: rendering applies VOI, Presentation LUT, rotation, flip, displayed area") {
    const Dataset ds = small_image();
    auto img = image::Image::open(ds, kTs);
    REQUIRE(img.is_ok());
    PsRenderOptions o;
    o.annotations = false;
    {  // Window c = 128, w = 256 on 8-bit data is the identity: gray == stored value.
        auto r = render_with_presentation_state(*img, window_ps(), 0, "1.2.3.100", o);
        REQUIRE(r.is_ok());
        CHECK(r->width == 3 && r->height == 2);
        CHECK(gray(*r, 0, 0) == 0 && gray(*r, 2, 0) == 2 && gray(*r, 1, 1) == 11);
    }
    {  // Rotation 90 clockwise: output is 2 x 3, out(x, y) = image(column y, row H-1-x).
        PresentationState ps = window_ps();
        ps.rotation = 90;
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        REQUIRE(r.is_ok());
        CHECK(r->width == 2 && r->height == 3);
        CHECK(gray(*r, 0, 0) == 10 && gray(*r, 1, 0) == 0 && gray(*r, 0, 2) == 12 && gray(*r, 1, 2) == 2);
        // Image point (0,0) (top left corner) maps to the top right corner of the output.
        const Point2 m = r->map({0, 0});
        CHECK(m.x == 2.0 && m.y == 0.0);
        // Rotation 90 then flip: the flip happens after the rotation (C.10.6.1.1).
        ps.flip = true;
        auto rf = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*rf, 0, 0) == 0 && gray(*rf, 1, 0) == 10);
    }
    {  // Flip only.
        PresentationState ps = window_ps();
        ps.flip = true;
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*r, 0, 0) == 2 && gray(*r, 2, 1) == 10);
    }
    {  // Rotation 180 and 270.
        PresentationState ps = window_ps();
        ps.rotation = 180;
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*r, 0, 0) == 12 && gray(*r, 2, 1) == 0);
        ps.rotation = 270;
        auto r2 = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(r2->width == 2 && gray(*r2, 0, 0) == 2 && gray(*r2, 1, 2) == 10);
    }
    {  // Displayed area TLHC (2,1) .. BRHC (3,2), 1-based: columns 1..2 of both rows.
        PresentationState ps = window_ps();
        DisplayedArea da;
        da.tlhc_column = 2, da.tlhc_row = 1, da.brhc_column = 3, da.brhc_row = 2;
        da.size_mode = "SCALE TO FIT";
        ps.displayed_areas.push_back(da);
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        REQUIRE(r.is_ok());
        CHECK(r->width == 2 && r->height == 2);
        CHECK(gray(*r, 0, 0) == 1 && gray(*r, 1, 1) == 12);
        CHECK_EQ(r->size_mode, std::string("SCALE TO FIT"));
        // Area larger than the image: padding is black.
        ps.displayed_areas[0].tlhc_column = 0;
        ps.displayed_areas[0].tlhc_row = 0;
        ps.displayed_areas[0].brhc_column = 4;
        auto r2 = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(r2->width == 5 && r2->height == 3 && gray(*r2, 0, 0) == 0 && gray(*r2, 2, 2) == 11);
        ps.displayed_areas[0].brhc_column = 2000000000;
        CHECK(!render_with_presentation_state(*img, ps, 0, "1.2.3.100", o).is_ok());
    }
    {  // INVERSE Presentation LUT Shape.
        PresentationState ps = window_ps();
        ps.plut_shape = image::PresentationShape::Inverse;
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*r, 0, 0) == 255 && gray(*r, 2, 1) == 243);
    }
    {  // No Softcopy VOI LUT: identity over the modality range 0..12; 1 -> round(255 / 12) = 21.
        PresentationState ps;
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*r, 0, 0) == 0 && gray(*r, 2, 1) == 255 && gray(*r, 1, 0) == 21);
    }
    {  // VOI scoped to another image does not apply.
        PresentationState ps = window_ps(6, 2);
        ImageReference other = ref_small();
        other.sop_instance_uid = "9.9";
        ps.voi[0].images.push_back(other);
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*r, 1, 0) == 21);
    }
    {  // Modality LUT of the presentation state replaces the image's: x 2 + 100.
        PresentationState ps = window_ps(110.5, 20);
        image::ModalityTransform m;
        m.slope = 2;
        m.intercept = 100;
        ps.modality = m;
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        // x = 0 -> 100 -> ((100 - 110) / 19 + 0.5) * 255 = -6.7 -> 0;  x = 12 -> 124 -> 255
        CHECK(gray(*r, 0, 0) == 0 && gray(*r, 2, 1) == 255);
        // x = 10 -> 120: ((120 - 110) / 19 + 0.5) = 1.026 -> 255; x = 2 -> 104 -> ((104-110)/19+0.5) * 255 = 46.97 -> 47
        CHECK(gray(*r, 2, 0) == 47);
    }
    {  // Rectangular shutter (columns 2..3, rows 1..2, 1-based) with P-Value 65535 (white).
        PresentationState ps = window_ps();
        ps.shutters.rectangular = true;
        ps.shutters.left = 2, ps.shutters.right = 3, ps.shutters.upper = 1, ps.shutters.lower = 2;
        ps.shutters.presentation_value = std::uint16_t{65535};
        auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100", o);
        CHECK(gray(*r, 0, 0) == 255 && gray(*r, 0, 1) == 255 && gray(*r, 1, 0) == 1);
    }
}

TEST_CASE("gsps: annotations are drawn with the layer colour and text is placed") {
    const Dataset ds = small_image();
    auto img = image::Image::open(ds, kTs);
    REQUIRE(img.is_ok());
    PresentationState ps = window_ps();
    GraphicLayer layer;
    layer.name = "MEAS";
    layer.cielab = rgb8_to_cielab(255, 0, 0);
    ps.layers.push_back(layer);
    GraphicAnnotation a;
    a.layer = "MEAS";
    GraphicObject line;
    line.units = AnnotationUnits::Pixel;
    line.type = GraphicType::Polyline;
    line.points = {{0.5, 0.5}, {2.5, 0.5}};  // across the centres of row 0
    a.graphics.push_back(line);
    TextObject t;
    t.text = "12 mm";
    t.anchor = Point2{1.5, 0.5};
    t.anchor_units = AnnotationUnits::Pixel;
    t.anchor_visible = true;
    a.texts.push_back(t);
    ps.annotations.push_back(a);
    ps.rotation = 90;
    auto r = render_with_presentation_state(*img, ps, 0, "1.2.3.100");
    REQUIRE(r.is_ok());
    // Row 0 of the image is the right column of the rotated output.
    for (std::uint32_t y = 0; y < 3; ++y) {
        const std::uint8_t* p = r->rgba.data() + 4 * (y * r->width + 1);
        CHECK(p[0] >= 254 && p[1] <= 1 && p[2] <= 1);
    }
    CHECK(gray(*r, 0, 0) == 10);  // untouched
    REQUIRE(r->texts.size() == 1);
    CHECK(r->texts[0].anchor && r->texts[0].anchor->x == 1.5 && r->texts[0].anchor->y == 1.5);
    CHECK(r->texts[0].rgb[0] >= 254);
    // DISPLAY units: a point at (0.5, 0.5) of the 3 x 2 output lands at (1.5, 1).
    PresentationState ps2 = window_ps();
    GraphicAnnotation b;
    b.layer = "X";
    GraphicObject pt;
    pt.units = AnnotationUnits::Display;
    pt.type = GraphicType::Point;
    pt.points = {{0.5, 0.5}};
    b.graphics.push_back(pt);
    ps2.annotations.push_back(b);
    auto r2 = render_with_presentation_state(*img, ps2, 0, "1.2.3.100");
    REQUIRE(r2.is_ok());
    const std::uint8_t* p = r2->rgba.data() + 4 * (1 * r2->width + 1);
    CHECK(p[0] == 255 && p[1] == 255 && p[2] == 0);  // default yellow
}

TEST_CASE("gsps: writer round trip") {
    const Dataset src = small_image();
    GspsWriteRequest req;
    req.source = &src;
    ImageReference ref = ref_small();
    ref.pixel_spacing = std::array<double, 2>{0.5, 0.5};
    req.images.push_back(ref);
    image::VoiWindow w;
    w.center = 40;
    w.width = 400;
    w.explanation = "SOFT TISSUE";
    req.window = w;
    req.rotation = 90;
    req.flip = true;
    req.content_label = "MEASUREMENTS";
    req.content_description = "Distance \xC3\xA9tude";  // non-ASCII: ISO_IR 192
    req.content_creator = "Shiva^Arun";
    GraphicLayer l;
    l.name = "LAYER1";
    l.order = 1;
    l.cielab = std::array<std::uint16_t, 3>{65535, 32896, 32896};
    l.gray = std::uint16_t{65535};
    req.layers.push_back(l);
    GraphicAnnotation a;
    a.layer = "LAYER1";
    GraphicObject g;
    g.type = GraphicType::Polyline;
    g.points = {{0.5, 0.5}, {2.5, 1.5}};
    a.graphics.push_back(g);
    GraphicObject c;
    c.type = GraphicType::Circle;
    c.points = {{1.5, 1.0}, {2.0, 1.0}};
    c.filled = true;
    a.graphics.push_back(c);
    GraphicObject e;
    e.type = GraphicType::Ellipse;
    e.units = AnnotationUnits::Display;
    e.points = {{0.1, 0.5}, {0.9, 0.5}, {0.5, 0.4}, {0.5, 0.6}};
    a.graphics.push_back(e);
    TextObject t;
    t.text = "len 2.2 mm";
    t.box_tlhc = Point2{0, 0};
    t.box_brhc = Point2{2, 1};
    t.box_units = AnnotationUnits::Pixel;
    t.justification = "CENTER";
    a.texts.push_back(t);
    req.annotations.push_back(a);
    GraphicAnnotation a2;  // layer not listed: added automatically
    a2.layer = "NOTES";
    a2.images.push_back(ref_small());
    TextObject t2;
    t2.text = "note";
    t2.anchor = Point2{1, 1};
    a2.texts.push_back(t2);
    req.annotations.push_back(a2);
    auto file = write_gsps(req);
    REQUIRE(file.is_ok());
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(*file, bytes).is_ok());
    auto back = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(back.is_ok());
    auto ps = read_presentation_state(back->dataset, false);
    REQUIRE(ps.is_ok());
    CHECK(ps->kind == PresentationKind::Grayscale);
    CHECK(uid_valid(ps->sop_instance_uid));
    CHECK_EQ(ps->content_label, std::string("MEASUREMENTS"));
    CHECK_EQ(ps->content_description, req.content_description);
    CHECK_EQ(ps->content_creator, std::string("Shiva^Arun"));
    CHECK_EQ(back->dataset.get_string(siaara::tags::SpecificCharacterSet).value_or(""), std::string("ISO_IR 192"));
    CHECK_EQ(back->dataset.get_string(siaara::tags::PatientName).value_or(""), std::string("Doe^Jane"));
    REQUIRE(ps->references.size() == 1);
    CHECK(ps->references[0].sop_instance_uid == "1.2.3.100" && ps->references[0].series_uid == "1.2.3.2");
    CHECK(ps->references_image("1.2.3.100"));
    REQUIRE(ps->voi.size() == 1 && ps->voi[0].windows.size() == 1);
    CHECK(ps->voi[0].windows[0].center == 40 && ps->voi[0].windows[0].width == 400);
    CHECK_EQ(ps->voi[0].windows[0].explanation, std::string("SOFT TISSUE"));
    CHECK(ps->plut_shape && *ps->plut_shape == image::PresentationShape::Identity);
    CHECK(ps->has_spatial_transform && ps->rotation == 90 && ps->flip);
    REQUIRE(ps->displayed_areas.size() == 1);
    CHECK(ps->displayed_areas[0].brhc_column == 3 && ps->displayed_areas[0].brhc_row == 2);
    CHECK(ps->displayed_areas[0].pixel_spacing && (*ps->displayed_areas[0].pixel_spacing)[0] == 0.5);
    REQUIRE(ps->layers.size() == 2);
    CHECK(ps->layers[0].name == "LAYER1" && ps->layers[0].cielab && (*ps->layers[0].cielab)[0] == 65535);
    CHECK(ps->layers[0].gray == 65535);
    REQUIRE(ps->annotations.size() == 2);
    const auto& ra = ps->annotations[0];
    REQUIRE(ra.graphics.size() == 3 && ra.texts.size() == 1);
    CHECK(ra.graphics[0].type == GraphicType::Polyline && ra.graphics[0].points.size() == 2);
    CHECK(ra.graphics[0].points[1].x == 2.5 && ra.graphics[0].points[1].y == 1.5);
    CHECK(ra.graphics[1].type == GraphicType::Circle && ra.graphics[1].filled);
    CHECK(ra.graphics[2].units == AnnotationUnits::Display && ra.graphics[2].points.size() == 4);
    CHECK(std::fabs(ra.graphics[2].points[0].x - 0.1) < 1e-7);  // FL precision
    CHECK(ra.texts[0].text == "len 2.2 mm" && ra.texts[0].justification == "CENTER");
    CHECK(ra.texts[0].box_brhc && ra.texts[0].box_brhc->x == 2.0);
    CHECK(ps->annotations[1].layer == "NOTES" && ps->annotations[1].images.size() == 1);
    CHECK(ps->annotations[1].texts[0].anchor && !ps->annotations[1].texts[0].anchor_visible);
    // The written state renders.
    auto img = image::Image::open(src, kTs);
    REQUIRE(img.is_ok());
    auto r = render_with_presentation_state(*img, *ps, 0, "1.2.3.100");
    REQUIRE(r.is_ok());
    CHECK(r->width == 2 && r->height == 3);
    CHECK(r->texts.size() == 2);
}

TEST_CASE("gsps: writer rejects invalid requests") {
    const Dataset src = small_image();
    GspsWriteRequest base;
    base.source = &src;
    base.images.push_back(ref_small());
    CHECK(write_gsps(base).is_ok());
    {
        GspsWriteRequest r = base;
        r.images.clear();
        CHECK(!write_gsps(r).is_ok());
    }
    {
        GspsWriteRequest r = base;
        r.content_label = "lower case";
        CHECK(!write_gsps(r).is_ok());
    }
    {
        GspsWriteRequest r = base;
        r.rotation = 45;
        CHECK(!write_gsps(r).is_ok());
    }
    {
        GspsWriteRequest r = base;
        r.images[0].rows = 0;
        CHECK(!write_gsps(r).is_ok());
    }
    {
        GspsWriteRequest r = base;
        GraphicAnnotation a;
        a.layer = "L";
        GraphicObject g;
        g.type = GraphicType::Circle;
        g.points = {{0, 0}, {1, 1}, {2, 2}};
        a.graphics.push_back(g);
        r.annotations.push_back(a);
        CHECK(!write_gsps(r).is_ok());
    }
    {
        GspsWriteRequest r = base;
        GraphicAnnotation a;
        a.layer = "L";
        TextObject t;
        t.text = "floating";
        a.texts.push_back(t);
        r.annotations.push_back(a);
        CHECK(!write_gsps(r).is_ok());  // no box, no anchor
    }
    {
        GspsWriteRequest r = base;
        r.source = nullptr;  // no study UID to copy
        CHECK(!write_gsps(r).is_ok());
    }
}

TEST_CASE("gsps: malformed presentation states") {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, sop::GrayscaleSoftcopyPresentationState);
    Dataset g;
    put(g, ct::GraphicAnnotationUnits, VR::CS, "PIXEL");
    put(g, ct::GraphicType, VR::CS, "POLYLINE");
    const float pts[3] = {1, 2, 3};
    (void)g.set_bytes(ct::GraphicData, VR::FL, encode_fl(pts, 3));
    Dataset bad_units = g;
    put(bad_units, ct::GraphicAnnotationUnits, VR::CS, "INCHES");
    Dataset ok_g = g;
    (void)ok_g.set_bytes(ct::GraphicData, VR::FL, encode_fl(pts, 2));
    put(ok_g, ct::GraphicType, VR::CS, "POINT");
    Dataset ann;
    put(ann, ct::GraphicLayer, VR::CS, "L");
    ann.put(make_sequence(ct::GraphicObjectSequence, {g, bad_units, ok_g}));
    Dataset txt;
    put(txt, ct::UnformattedTextValue, VR::ST, "no position");
    ann.put(make_sequence(ct::TextObjectSequence, {txt}));
    d.put(make_sequence(ct::GraphicAnnotationSequence, {ann}));
    (void)d.set_uint16(ct::ImageRotation, 45);
    auto lenient = read_presentation_state(d, true);
    REQUIRE(lenient.is_ok());
    REQUIRE(lenient->annotations.size() == 1);
    CHECK_EQ(lenient->annotations[0].graphics.size(), 1u);
    CHECK(lenient->annotations[0].texts.empty());
    CHECK(lenient->rotation == 0);
    CHECK(lenient->warnings.size() >= 3);
    CHECK(!read_presentation_state(d, false).is_ok());
    Dataset not_ps;
    put(not_ps, siaara::tags::SOPClassUID, VR::UI, kCt);
    CHECK(read_presentation_state(not_ps).error().code == Errc::Unsupported);
}

TEST_CASE("gsps: blending and colour states are parsed") {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, sop::BlendingSoftcopyPresentationState);
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.9");
    std::vector<Dataset> items;
    for (int k = 0; k < 2; ++k) {
        Dataset b;
        put(b, ct::BlendingPosition, VR::CS, k ? "SUPERIMPOSED" : "UNDERLYING");
        put(b, 0x0020000D, VR::UI, "1.2.3.1");
        Dataset ri;
        put(ri, ct::ReferencedSopClassUid, VR::UI, kCt);
        put(ri, ct::ReferencedSopInstanceUid, VR::UI, "1.2.3.10" + std::to_string(k));
        Dataset rs;
        rs.put(make_sequence(ct::ReferencedImageSequence, {ri}));
        put(rs, siaara::tags::SeriesInstanceUID, VR::UI, "1.2.3.2" + std::to_string(k));
        b.put(make_sequence(ct::ReferencedSeriesSequence, {rs}));
        if (k) {
            Dataset v;
            put(v, 0x00281050, VR::DS, "3");
            put(v, 0x00281051, VR::DS, "6");
            b.put(make_sequence(ct::SoftcopyVoiLutSequence, {v}));
            put(b, 0x00281052, VR::DS, "0");
            put(b, 0x00281053, VR::DS, "0.001");
        }
        items.push_back(b);
    }
    d.put(make_sequence(ct::BlendingSequence, items));
    const float op = 0.4f;
    (void)d.set_bytes(ct::RelativeOpacity, VR::FL, encode_fl(&op, 1));
    auto ps = read_presentation_state(d);
    REQUIRE(ps.is_ok());
    CHECK(ps->kind == PresentationKind::Blending);
    REQUIRE(ps->blending.size() == 2);
    CHECK(ps->blending[0].position == "UNDERLYING" && ps->blending[1].position == "SUPERIMPOSED");
    CHECK(ps->blending[1].images.size() == 1 && ps->blending[1].images[0].series_uid == "1.2.3.21");
    CHECK(ps->blending[1].voi.size() == 1 && ps->blending[1].voi[0].windows[0].width == 6.0);
    CHECK(ps->blending[1].modality && std::fabs(ps->blending[1].modality->slope - 0.001) < 1e-15);
    CHECK(ps->relative_opacity && std::fabs(*ps->relative_opacity - 0.4) < 1e-7);
    put(d, siaara::tags::SOPClassUID, VR::UI, sop::ColorSoftcopyPresentationState);
    const std::uint8_t icc[4] = {1, 2, 3, 4};
    (void)d.set_bytes(ct::IccProfile, VR::OB, Bytes::copy_of(icc, 4));
    auto cps = read_presentation_state(d);
    CHECK(cps.is_ok() && cps->kind == PresentationKind::Color && cps->has_icc_profile);
}

TEST_CASE("gsps: mutated files never crash the reader") {
    const Dataset src = small_image();
    GspsWriteRequest req;
    req.source = &src;
    req.images.push_back(ref_small());
    GraphicAnnotation a;
    a.layer = "L";
    GraphicObject g;
    g.points = {{0, 0}, {1, 1}, {2, 0}, {0, 0}};
    a.graphics.push_back(g);
    TextObject t;
    t.text = "x";
    t.anchor = Point2{1, 1};
    a.texts.push_back(t);
    req.annotations.push_back(a);
    image::VoiWindow w;
    w.center = 1;
    w.width = 3;
    req.window = w;
    auto f = write_gsps(req);
    REQUIRE(f.is_ok());
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(*f, bytes).is_ok());
    auto img = image::Image::open(src, kTs);
    REQUIRE(img.is_ok());
    std::mt19937 rng(1234);
    int parsed = 0;
    for (int iter = 0; iter < 300; ++iter) {
        std::vector<std::uint8_t> m = bytes;
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e) m[132 + rng() % (m.size() - 132)] = static_cast<std::uint8_t>(rng());
        auto r = read_part10(Bytes::copy_of(m.data(), m.size()));
        if (!r) continue;
        auto ps = read_presentation_state(r->dataset);
        if (!ps) continue;
        ++parsed;
        (void)render_with_presentation_state(*img, *ps, 0, "1.2.3.100");
    }
    CHECK(parsed > 0);
}
