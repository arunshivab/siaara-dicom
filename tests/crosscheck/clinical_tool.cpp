// Test-only helper for tests/crosscheck/clinical_crosscheck.py (not installed).
// Prints what the clinical layer reads from a file as JSON, or writes a KOS / GSPS.
//   clinical_tool rtstruct <file>
//   clinical_tool sr <file>
//   clinical_tool seg <file>
//   clinical_tool gsps <file>
//   clinical_tool pet-inputs <file>   (stored PET data elements by path, as text; no calculation)
//   clinical_tool geometry <file> [frame]
//   clinical_tool sort <file>...
//   clinical_tool write-kos <out> <image>...
//   clinical_tool write-gsps <out> <image>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "clinical/codes.hpp"
#include "clinical/geometry.hpp"
#include "clinical/gsps.hpp"
#include "clinical/rtstruct.hpp"
#include "clinical/seg.hpp"
#include "clinical/sr.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "modules/modules.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

std::string js(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            o.push_back('\\');
            o.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o.push_back(static_cast<char>(c));
        }
    }
    return o + "\"";
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}
std::string code_js(const std::optional<Code>& c) {
    if (!c) return "null";
    return "[" + js(c->value) + "," + js(c->scheme) + "," + js(c->meaning) + "]";
}
std::string pos(const std::vector<std::uint32_t>& p) {
    std::string s;
    for (std::size_t i = 0; i < p.size(); ++i) s += (i ? "." : "") + std::to_string(p[i]);
    return s;
}

int fail(const Status& s) {
    std::fprintf(stderr, "error: %s\n", s.message.c_str());
    return 1;
}

int cmd_rtstruct(const DicomFile& f) {
    auto r = read_rtstruct(f.dataset);
    if (!r) return fail(r.error());
    std::string o = "{\"label\":" + js(r->label) + ",\"rois\":[";
    for (std::size_t i = 0; i < r->rois.size(); ++i) {
        const auto& roi = r->rois[i];
        o += (i ? "," : "") + std::string("{\"number\":") + std::to_string(roi.number) + ",\"name\":" + js(roi.name) +
             ",\"type\":" + js(roi.interpreted_type) + ",\"color\":";
        o += roi.color ? "[" + std::to_string((*roi.color)[0]) + "," + std::to_string((*roi.color)[1]) + "," +
                             std::to_string((*roi.color)[2]) + "]"
                       : std::string("null");
        o += ",\"contours\":[";
        for (std::size_t k = 0; k < roi.contours.size(); ++k) {
            const auto& c = roi.contours[k];
            o += (k ? "," : "") + std::string("{\"type\":") + js(c.geometric_type) + ",\"points\":" + std::to_string(c.points.size()) +
                 ",\"first\":[" + num(c.points[0].x) + "," + num(c.points[0].y) + "," + num(c.points[0].z) + "],\"images\":[";
            for (std::size_t m = 0; m < c.images.size(); ++m) o += (m ? "," : "") + js(c.images[m].sop_instance_uid);
            o += "]}";
        }
        o += "]}";
    }
    o += "],\"warnings\":" + std::to_string(r->warnings.size()) + "}";
    std::puts(o.c_str());
    return 0;
}

int cmd_sr(const DicomFile& f) {
    auto r = read_sr(f.dataset);
    if (!r) return fail(r.error());
    std::string o = "{\"nodes\":[";
    for (std::size_t i = 0; i < r->nodes.size(); ++i) {
        const auto& n = r->nodes[i];
        o += (i ? "," : "") + std::string("{\"pos\":") + js(pos(n.position)) + ",\"vt\":" + js(std::string(value_type_name(n.value_type))) +
             ",\"rel\":" + js(n.relationship_text) + ",\"concept\":" + code_js(n.concept_name) + ",\"text\":" + js(n.text) +
             ",\"code\":" + code_js(n.code) + ",\"num\":" + (n.numeric ? num(*n.numeric) : std::string("null")) +
             ",\"units\":" + code_js(n.units) + ",\"refs\":[";
        for (std::size_t k = 0; k < n.references.size(); ++k) o += (k ? "," : "") + js(n.references[k].sop_instance_uid);
        o += "],\"graphic\":" + std::to_string(n.graphic_data.size()) + ",\"target\":" +
             (n.referenced_node >= 0 ? js(pos(r->nodes[static_cast<std::size_t>(n.referenced_node)].position)) : std::string("null")) + "}";
    }
    o += "],\"warnings\":" + std::to_string(r->warnings.size()) + ",\"text_bytes\":" + std::to_string(sr_to_text(*r).size()) + "}";
    std::puts(o.c_str());
    return 0;
}

int cmd_seg(const DicomFile& f) {
    auto r = read_segmentation(f.dataset);
    if (!r) return fail(r.error());
    std::string o = "{\"type\":" + std::to_string(static_cast<int>(r->type)) + ",\"max_fractional\":" + std::to_string(r->max_fractional) +
                    ",\"segments\":[";
    for (std::size_t i = 0; i < r->segments.size(); ++i) {
        const auto& s = r->segments[i];
        o += (i ? "," : "") + std::string("{\"number\":") + std::to_string(s.number) + ",\"label\":" + js(s.label) +
             ",\"type\":" + code_js(s.type) + ",\"rgb\":";
        o += s.rgb ? "[" + std::to_string((*s.rgb)[0]) + "," + std::to_string((*s.rgb)[1]) + "," + std::to_string((*s.rgb)[2]) + "]"
                   : std::string("null");
        o += ",\"lab\":";
        o += s.cielab ? "[" + std::to_string((*s.cielab)[0]) + "," + std::to_string((*s.cielab)[1]) + "," + std::to_string((*s.cielab)[2]) + "]"
                      : std::string("null");
        o += ",\"frames\":" + std::to_string(s.frames.size()) + "}";
    }
    o += "],\"frames\":[";
    for (std::uint32_t k = 0; k < r->frames.size(); ++k) {
        const auto& fr = r->frames[k];
        auto m = segment_mask(f.dataset, f.transfer_syntax, *r, k);
        std::uint64_t sum = 0;
        std::string bits;
        if (m)
            for (auto v : *m) {
                sum += v;
                bits += std::to_string(v) + ",";
            }
        if (!bits.empty()) bits.pop_back();
        o += (k ? "," : "") + std::string("{\"segment\":") + std::to_string(fr.segment) + ",\"sum\":" + std::to_string(sum) +
             ",\"mask\":[" + bits + "],\"z\":" + (fr.geometry.has_position ? num(fr.geometry.position.z) : std::string("null")) + ",\"sources\":[";
        for (std::size_t s = 0; s < fr.sources.size(); ++s) o += (s ? "," : "") + js(fr.sources[s].sop_instance_uid);
        o += "]}";
    }
    o += "],\"warnings\":" + std::to_string(r->warnings.size()) + "}";
    std::puts(o.c_str());
    return 0;
}

std::string_view gt_name(GraphicType t) {
    switch (t) {
        case GraphicType::Point: return "POINT";
        case GraphicType::Polyline: return "POLYLINE";
        case GraphicType::Interpolated: return "INTERPOLATED";
        case GraphicType::Circle: return "CIRCLE";
        case GraphicType::Ellipse: return "ELLIPSE";
        case GraphicType::Multipoint: return "MULTIPOINT";
        case GraphicType::Polygon: return "POLYGON";
    }
    return "?";
}

int cmd_gsps(const DicomFile& f) {
    auto r = read_presentation_state(f.dataset);
    if (!r) return fail(r.error());
    std::string o = "{\"label\":" + js(r->content_label) + ",\"rotation\":" + std::to_string(r->rotation) +
                    ",\"flip\":" + (r->flip ? "true" : "false") + ",\"refs\":[";
    for (std::size_t i = 0; i < r->references.size(); ++i) o += (i ? "," : "") + js(r->references[i].sop_instance_uid);
    o += "],\"voi\":[";
    for (std::size_t i = 0; i < r->voi.size(); ++i)
        for (std::size_t k = 0; k < r->voi[i].windows.size(); ++k)
            o += ((i || k) ? "," : "") + std::string("[") + num(r->voi[i].windows[k].center) + "," + num(r->voi[i].windows[k].width) + "]";
    o += "],\"layers\":[";
    for (std::size_t i = 0; i < r->layers.size(); ++i) o += (i ? "," : "") + js(r->layers[i].name);
    o += "],\"annotations\":[";
    for (std::size_t i = 0; i < r->annotations.size(); ++i) {
        const auto& a = r->annotations[i];
        o += (i ? "," : "") + std::string("{\"layer\":") + js(a.layer) + ",\"graphics\":[";
        for (std::size_t k = 0; k < a.graphics.size(); ++k) {
            const auto& g = a.graphics[k];
            o += (k ? "," : "") + std::string("{\"type\":") + js(std::string(gt_name(g.type))) + ",\"units\":" +
                 js(g.units == AnnotationUnits::Pixel ? "PIXEL" : "DISPLAY") + ",\"filled\":" + (g.filled ? "true" : "false") + ",\"points\":[";
            for (std::size_t p = 0; p < g.points.size(); ++p) o += (p ? "," : "") + std::string("[") + num(g.points[p].x) + "," + num(g.points[p].y) + "]";
            o += "]}";
        }
        o += "],\"texts\":[";
        for (std::size_t k = 0; k < a.texts.size(); ++k) o += (k ? "," : "") + js(a.texts[k].text);
        o += "]}";
    }
    o += "],\"warnings\":" + std::to_string(r->warnings.size()) + "}";
    std::puts(o.c_str());
    return 0;
}

// The stored elements an application needs for SUV, read by path exactly as stored.
int cmd_pet_inputs(const DicomFile& f) {
    static const char* const kPaths[] = {
        "Units", "DecayCorrection", "CorrectedImage", "PatientWeight", "PatientSize", "PatientSex",
        "SeriesDate", "SeriesTime", "AcquisitionDate", "AcquisitionTime",
        "RadiopharmaceuticalInformationSequence[0].RadionuclideTotalDose",
        "RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife",
        "RadiopharmaceuticalInformationSequence[0].RadiopharmaceuticalStartDateTime",
        "RadiopharmaceuticalInformationSequence[0].RadiopharmaceuticalStartTime",
    };
    std::string o = "{";
    bool first = true;
    for (const char* p : kPaths) {
        auto v = modules::get_by_path(f.dataset, p);
        o += (first ? "" : ",") + js(p) + ":" + (v ? js(v->text) : std::string("null"));
        first = false;
    }
    o += "}";
    std::puts(o.c_str());
    return 0;
}

int cmd_geometry(const DicomFile& f, std::uint32_t frame) {
    auto g = frame_geometry(f.dataset, frame);
    if (!g) return fail(g.error());
    auto e = edge_labels(*g);
    const Vec3 p = g->pixel_to_patient(static_cast<double>(g->columns) - 1, static_cast<double>(g->rows) - 1);
    std::string o = "{\"position\":[" + num(g->position.x) + "," + num(g->position.y) + "," + num(g->position.z) + "],\"normal\":[" +
                    num(g->normal.x) + "," + num(g->normal.y) + "," + num(g->normal.z) + "],\"spacing\":[" + num(g->spacing.row) + "," +
                    num(g->spacing.column) + "],\"source\":" + std::to_string(static_cast<int>(g->spacing.source)) + ",\"last\":[" + num(p.x) +
                    "," + num(p.y) + "," + num(p.z) + "],\"labels\":" +
                    (e ? "[" + js(e->left) + "," + js(e->right) + "," + js(e->top) + "," + js(e->bottom) + "]" : std::string("null")) + "}";
    std::puts(o.c_str());
    return 0;
}

int cmd_sort(const std::vector<DicomFile>& files) {
    std::vector<const Dataset*> ds;
    for (const auto& f : files) ds.push_back(&f.dataset);
    auto s = sort_series(ds);
    std::string o = "{\"order\":[";
    for (std::size_t i = 0; i < s.order.size(); ++i) o += (i ? "," : "") + std::to_string(s.items[s.order[i]].instance);
    o += "],\"groups\":" + std::to_string(s.groups.size()) + ",\"warnings\":" + std::to_string(s.warnings) + "}";
    std::puts(o.c_str());
    return 0;
}

int write_out(const DicomFile& f, const char* path) {
    Status s = write_file(f, path);
    if (!s) return fail(s);
    return 0;
}

int cmd_write_kos(const char* out, const std::vector<DicomFile>& images) {
    KosWriteRequest req;
    req.source = &images[0].dataset;
    req.title = cid_code(7010, "113000");
    req.description = "Key images for review";
    for (const auto& im : images) {
        KosReference r;
        r.study_uid = im.dataset.get_string(siaara::tags::StudyInstanceUID).value_or("");
        r.series_uid = im.dataset.get_string(siaara::tags::SeriesInstanceUID).value_or("");
        r.sop_class_uid = im.dataset.get_string(siaara::tags::SOPClassUID).value_or("");
        r.sop_instance_uid = im.dataset.get_string(siaara::tags::SOPInstanceUID).value_or("");
        req.references.push_back(r);
    }
    auto f = write_kos(req);
    if (!f) return fail(f.error());
    return write_out(*f, out);
}

int cmd_write_gsps(const char* out, const DicomFile& im) {
    GspsWriteRequest req;
    req.source = &im.dataset;
    ImageReference ref;
    ref.sop_class_uid = im.dataset.get_string(siaara::tags::SOPClassUID).value_or("");
    ref.sop_instance_uid = im.dataset.get_string(siaara::tags::SOPInstanceUID).value_or("");
    ref.series_uid = im.dataset.get_string(siaara::tags::SeriesInstanceUID).value_or("");
    ref.rows = static_cast<std::uint32_t>(im.dataset.get_int(siaara::tags::Rows).value_or(0));
    ref.columns = static_cast<std::uint32_t>(im.dataset.get_int(siaara::tags::Columns).value_or(0));
    req.images.push_back(ref);
    image::VoiWindow w;
    w.center = 40;
    w.width = 400;
    req.window = w;
    req.content_description = "SIAARA crosscheck";
    req.content_creator = "Tester^Test";
    GraphicLayer l;
    l.name = "MEASUREMENTS";
    l.order = 1;
    l.cielab = rgb8_to_cielab(255, 255, 0);
    req.layers.push_back(l);
    GraphicAnnotation a;
    a.layer = "MEASUREMENTS";
    GraphicObject pl;
    pl.type = GraphicType::Polyline;
    pl.points = {{10, 10}, {40, 30}};
    a.graphics.push_back(pl);
    GraphicObject closed;
    closed.type = GraphicType::Polyline;
    closed.points = {{5, 5}, {20, 5}, {20, 20}, {5, 5}};
    a.graphics.push_back(closed);
    GraphicObject c;
    c.type = GraphicType::Circle;
    c.points = {{30, 30}, {35, 30}};
    a.graphics.push_back(c);
    GraphicObject e;
    e.type = GraphicType::Ellipse;
    e.points = {{10, 50}, {30, 50}, {20, 45}, {20, 55}};
    a.graphics.push_back(e);
    GraphicObject p;
    p.type = GraphicType::Point;
    p.units = AnnotationUnits::Display;
    p.points = {{0.5, 0.5}};
    a.graphics.push_back(p);
    TextObject t;
    t.text = "36.1 mm";
    t.anchor = Point2{40, 30};
    t.anchor_visible = true;
    t.box_tlhc = Point2{42, 25};
    t.box_brhc = Point2{60, 32};
    a.texts.push_back(t);
    req.annotations.push_back(a);
    auto f = write_gsps(req);
    if (!f) return fail(f.error());
    return write_out(*f, out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: clinical_tool <command> <args>\n");
        return 2;
    }
    const std::string cmd = argv[1];
    auto load = [](const char* p) -> Result<DicomFile> { return read_file(p); };
    if (cmd == "write-kos" || cmd == "write-gsps" || cmd == "sort") {
        std::vector<DicomFile> files;
        for (int i = cmd == "sort" ? 2 : 3; i < argc; ++i) {
            auto f = load(argv[i]);
            if (!f) return fail(f.error());
            files.push_back(std::move(f).value());
        }
        if (files.empty()) return 2;
        if (cmd == "sort") return cmd_sort(files);
        return cmd == "write-kos" ? cmd_write_kos(argv[2], files) : cmd_write_gsps(argv[2], files[0]);
    }
    auto f = load(argv[2]);
    if (!f) return fail(f.error());
    if (cmd == "rtstruct") return cmd_rtstruct(*f);
    if (cmd == "sr") return cmd_sr(*f);
    if (cmd == "seg") return cmd_seg(*f);
    if (cmd == "gsps") return cmd_gsps(*f);
    if (cmd == "pet-inputs") return cmd_pet_inputs(*f);
    if (cmd == "geometry") return cmd_geometry(*f, argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 0u);
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
