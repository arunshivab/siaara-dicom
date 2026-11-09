// SIAARA DICOM Toolkit - softcopy presentation states (see gsps.hpp for the references).
#include "clinical/gsps.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "charset/charset.hpp"
#include "clinical/common.hpp"
#include "core/transfer_syntax.hpp"
#include "image/samples.hpp"

namespace siaara::clinical {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr Tag kRescaleIntercept = 0x00281052, kRescaleSlope = 0x00281053, kRescaleType = 0x00281054;
constexpr Tag kModalityLutSequence = 0x00283000, kVoiLutSequence = 0x00283010;
constexpr Tag kWindowCenter = 0x00281050, kWindowWidth = 0x00281051, kWindowExplanation = 0x00281055;
constexpr Tag kVoiLutFunction = 0x00281056, kPresentationLutShape = 0x20500020;
constexpr Tag kStudyInstanceUid = 0x0020000D, kSeriesInstanceUid = 0x0020000E;

std::optional<AnnotationUnits> parse_units(std::string_view s) {
    if (s == "PIXEL") return AnnotationUnits::Pixel;
    if (s == "DISPLAY") return AnnotationUnits::Display;
    if (s == "MATRIX") return AnnotationUnits::Matrix;
    return std::nullopt;
}
std::string_view units_name(AnnotationUnits u) {
    switch (u) {
        case AnnotationUnits::Pixel: return "PIXEL";
        case AnnotationUnits::Display: return "DISPLAY";
        case AnnotationUnits::Matrix: return "MATRIX";
    }
    return "PIXEL";
}
std::optional<GraphicType> parse_type(std::string_view s) {
    if (s == "POINT") return GraphicType::Point;
    if (s == "POLYLINE") return GraphicType::Polyline;
    if (s == "INTERPOLATED") return GraphicType::Interpolated;
    if (s == "CIRCLE") return GraphicType::Circle;
    if (s == "ELLIPSE") return GraphicType::Ellipse;
    if (s == "MULTIPOINT") return GraphicType::Multipoint;
    if (s == "POLYGON") return GraphicType::Polygon;
    return std::nullopt;
}
std::string_view type_name(GraphicType t) {
    switch (t) {
        case GraphicType::Point: return "POINT";
        case GraphicType::Polyline: return "POLYLINE";
        case GraphicType::Interpolated: return "INTERPOLATED";
        case GraphicType::Circle: return "CIRCLE";
        case GraphicType::Ellipse: return "ELLIPSE";
        case GraphicType::Multipoint: return "MULTIPOINT";
        case GraphicType::Polygon: return "POLYGON";
    }
    return "POLYLINE";
}
// Point count rules of C.10.5.1.2.
bool point_count_ok(GraphicType t, std::size_t n) {
    switch (t) {
        case GraphicType::Point: return n == 1;
        case GraphicType::Polyline:
        case GraphicType::Interpolated: return n >= 2;
        case GraphicType::Circle: return n == 2;
        case GraphicType::Ellipse: return n == 4;
        case GraphicType::Multipoint: return n >= 1;
        case GraphicType::Polygon: return n >= 3;
    }
    return false;
}

std::optional<Point2> point_of(const Dataset& d, Tag t) {
    auto v = get_numbers_n(d, t, 2);
    if (!v || !std::isfinite((*v)[0]) || !std::isfinite((*v)[1])) return std::nullopt;
    return Point2{(*v)[0], (*v)[1]};
}

std::vector<ImageReference> image_refs(const Dataset& item, std::string_view series_uid = {}) {
    std::vector<ImageReference> out;
    for (const Dataset& r : items_of(item, tags::ReferencedImageSequence)) {
        ImageReference ref;
        if (auto v = get_text(r, tags::ReferencedSopClassUid)) ref.sop_class_uid = *v;
        if (auto v = get_text(r, tags::ReferencedSopInstanceUid)) ref.sop_instance_uid = *v;
        ref.series_uid = std::string(series_uid);
        if (auto f = get_numbers(r, tags::ReferencedFrameNumber, 1u << 20))
            for (double x : *f)
                if (x >= 1.0 && x <= 2147483647.0) ref.frames.push_back(static_cast<std::int32_t>(x));
        out.push_back(std::move(ref));
    }
    return out;
}

std::vector<ImageReference> series_refs(const Dataset& ds) {
    std::vector<ImageReference> out;
    for (const Dataset& s : items_of(ds, tags::ReferencedSeriesSequence)) {
        const std::string suid = get_text(s, kSeriesInstanceUid).value_or("");
        auto refs = image_refs(s, suid);
        out.insert(out.end(), std::make_move_iterator(refs.begin()), std::make_move_iterator(refs.end()));
    }
    return out;
}

bool scope_applies(const std::vector<ImageReference>& refs, std::string_view sop, std::uint32_t f) {
    if (refs.empty()) return true;
    for (const auto& r : refs)
        if (r.applies_to(sop, f)) return true;
    return false;
}

std::optional<image::ModalityTransform> read_modality(const Dataset& d, std::vector<std::string>& warnings, bool lenient) {
    if (const Dataset* m = first_item(d, kModalityLutSequence)) {
        auto lut = image::read_lut(*m, false, lenient);
        if (lut) {
            image::ModalityTransform t;
            t.has_lut = true;
            t.lut = std::move(lut).value();
            return t;
        }
        warnings.emplace_back("Modality LUT Sequence item unreadable; ignored");
    }
    auto slope = d.get_double(kRescaleSlope), icpt = d.get_double(kRescaleIntercept);
    if (slope && icpt && std::isfinite(*slope) && std::isfinite(*icpt)) {
        image::ModalityTransform t;
        t.slope = *slope;
        t.intercept = *icpt;
        if (auto rt = get_text(d, kRescaleType)) t.rescale_type = *rt;
        return t;
    }
    return std::nullopt;
}

std::vector<SoftcopyVoi> read_voi(const Dataset& d, std::vector<std::string>& warnings, bool lenient) {
    std::vector<SoftcopyVoi> out;
    for (const Dataset& item : items_of(d, tags::SoftcopyVoiLutSequence)) {
        SoftcopyVoi v;
        v.images = image_refs(item);
        auto c = get_numbers(item, kWindowCenter, 64), w = get_numbers(item, kWindowWidth, 64);
        image::VoiFunction fn = image::VoiFunction::Linear;
        if (auto f = get_text(item, kVoiLutFunction)) {
            if (*f == "LINEAR_EXACT") fn = image::VoiFunction::LinearExact;
            else if (*f == "SIGMOID") fn = image::VoiFunction::Sigmoid;
        }
        if (c && w) {
            const std::size_t n = std::min(c->size(), w->size());
            for (std::size_t k = 0; k < n; ++k) {
                if (!image::window_valid((*c)[k], (*w)[k], fn)) {
                    warnings.emplace_back("Softcopy VOI window with an invalid width ignored");
                    continue;
                }
                image::VoiWindow win;
                win.center = (*c)[k];
                win.width = (*w)[k];
                win.function = fn;
                if (auto e = item.get_string(kWindowExplanation, k)) win.explanation = *e;
                v.windows.push_back(std::move(win));
            }
        }
        for (const Dataset& l : items_of(item, kVoiLutSequence)) {
            auto lut = image::read_lut(l, false, lenient);
            if (lut) v.luts.push_back(std::move(lut).value());
            else warnings.emplace_back("Softcopy VOI LUT item unreadable; ignored");
        }
        out.push_back(std::move(v));
    }
    return out;
}

Status read_annotations(const Dataset& ds, PresentationState& ps, bool lenient) {
    std::size_t k = 0;
    for (const Dataset& item : items_of(ds, tags::GraphicAnnotationSequence)) {
        ++k;
        GraphicAnnotation a;
        a.layer = get_text(item, tags::GraphicLayer).value_or("");
        a.images = image_refs(item);
        for (const Dataset& t : items_of(item, tags::TextObjectSequence)) {
            TextObject to;
            const charset::CharacterSet cs = charset::character_set_of(ds);
            if (auto v = charset::get_utf8(t, tags::UnformattedTextValue, 0, &cs)) to.text = *v;
            if (auto u = get_text(t, tags::BoundingBoxAnnotationUnits)) to.box_units = parse_units(*u);
            if (auto u = get_text(t, tags::AnchorPointAnnotationUnits)) to.anchor_units = parse_units(*u);
            to.box_tlhc = point_of(t, tags::BoundingBoxTlhc);
            to.box_brhc = point_of(t, tags::BoundingBoxBrhc);
            to.anchor = point_of(t, tags::AnchorPoint);
            to.anchor_visible = get_text(t, tags::AnchorPointVisibility).value_or("N") == "Y";
            to.justification = get_text(t, tags::BoundingBoxJustification).value_or("");
            const bool box = to.box_tlhc && to.box_brhc && to.box_units;
            const bool anchor = to.anchor && to.anchor_units;
            if (!box) to.box_tlhc.reset(), to.box_brhc.reset();
            if (!anchor) to.anchor.reset();
            if (!box && !anchor) {
                if (!lenient) return Status(Errc::Malformed, "text object without a bounding box or anchor point");
                ps.warnings.push_back("text object without position skipped");
                continue;
            }
            a.texts.push_back(std::move(to));
        }
        for (const Dataset& g : items_of(item, tags::GraphicObjectSequence)) {
            GraphicObject go;
            auto u = get_text(g, tags::GraphicAnnotationUnits);
            auto ty = get_text(g, tags::GraphicType);
            auto pu = u ? parse_units(*u) : std::nullopt;
            auto pt = ty ? parse_type(*ty) : std::nullopt;
            const auto dims = g.get_int(tags::GraphicDimensions).value_or(2);
            auto data = get_numbers(g, tags::GraphicData, 1u << 24);
            const auto npts = g.get_int(tags::NumberOfGraphicPoints);
            bool ok = pu && pt && dims == 2 && data && data->size() % 2 == 0;
            if (ok && npts && static_cast<std::size_t>(*npts) * 2 != data->size()) ok = false;
            if (ok) {
                for (std::size_t i = 0; i + 1 < data->size(); i += 2) {
                    if (!std::isfinite((*data)[i]) || !std::isfinite((*data)[i + 1])) ok = false;
                    go.points.push_back({(*data)[i], (*data)[i + 1]});
                }
                ok = ok && point_count_ok(*pt, go.points.size());
            }
            if (!ok) {
                if (!lenient) return Status(Errc::Malformed, "graphic object " + std::to_string(k) + " is malformed");
                ps.warnings.push_back("malformed graphic object skipped");
                continue;
            }
            go.units = *pu;
            go.type = *pt;
            go.filled = get_text(g, tags::GraphicFilled).value_or("N") == "Y";
            go.group_id = g.get_int(tags::GraphicGroupId);
            a.graphics.push_back(std::move(go));
        }
        ps.annotations.push_back(std::move(a));
    }
    return Status::ok();
}

}  // namespace

bool ImageReference::applies_to(std::string_view sop_instance, std::uint32_t f) const noexcept {
    if (sop_instance != sop_instance_uid) return false;
    if (frames.empty()) return true;
    for (auto x : frames)
        if (static_cast<std::uint32_t>(x) == f) return true;
    return false;
}

bool PresentationState::references_image(std::string_view sop, std::uint32_t f) const noexcept {
    for (const auto& r : references)
        if (r.applies_to(sop, f)) return true;
    return false;
}
const SoftcopyVoi* PresentationState::voi_for(std::string_view sop, std::uint32_t f) const noexcept {
    for (const auto& v : voi)
        if (scope_applies(v.images, sop, f)) return &v;
    return nullptr;
}
const DisplayedArea* PresentationState::displayed_area_for(std::string_view sop, std::uint32_t f) const noexcept {
    for (const auto& d : displayed_areas)
        if (scope_applies(d.images, sop, f)) return &d;
    return nullptr;
}
const GraphicLayer* PresentationState::layer(std::string_view name) const noexcept {
    for (const auto& l : layers)
        if (l.name == name) return &l;
    return nullptr;
}

Result<PresentationState> read_presentation_state(const Dataset& ds, bool lenient) {
    PresentationState ps;
    ps.sop_class_uid = get_text(ds, siaara::tags::SOPClassUID).value_or("");
    if (ps.sop_class_uid == sop::GrayscaleSoftcopyPresentationState) ps.kind = PresentationKind::Grayscale;
    else if (ps.sop_class_uid == sop::ColorSoftcopyPresentationState) ps.kind = PresentationKind::Color;
    else if (ps.sop_class_uid == sop::PseudoColorSoftcopyPresentationState) ps.kind = PresentationKind::PseudoColor;
    else if (ps.sop_class_uid == sop::BlendingSoftcopyPresentationState) ps.kind = PresentationKind::Blending;
    else if (!ds.contains(tags::ReferencedSeriesSequence) && !ds.contains(tags::BlendingSequence))
        return Status(Errc::Unsupported, "not a softcopy presentation state");
    ps.sop_instance_uid = get_text(ds, siaara::tags::SOPInstanceUID).value_or("");
    ps.content_label = get_text(ds, tags::ContentLabel).value_or("");
    const charset::CharacterSet cs = charset::character_set_of(ds);
    if (auto v = charset::get_utf8(ds, tags::ContentDescription, 0, &cs)) ps.content_description = *v;
    if (auto v = charset::get_utf8(ds, tags::ContentCreatorName, 0, &cs)) ps.content_creator = *v;
    ps.creation_date = get_text(ds, tags::PresentationCreationDate).value_or("");
    ps.creation_time = get_text(ds, tags::PresentationCreationTime).value_or("");
    ps.instance_number = ds.get_int(tags::InstanceNumber);
    ps.references = series_refs(ds);
    ps.modality = read_modality(ds, ps.warnings, lenient);
    ps.voi = read_voi(ds, ps.warnings, lenient);
    ps.plut_shape = image::presentation_shape(ds);
    if (auto pl = image::presentation_lut(ds, lenient); pl && pl->has_value()) ps.plut = std::move(**pl);
    else if (!pl) ps.warnings.push_back("Presentation LUT Sequence unreadable; ignored");
    if (auto sh = image::read_shutters(ds, lenient)) ps.shutters = std::move(sh).value();
    else if (!lenient) return sh.take_error();
    {
        image::PixelInfo dummy;
        dummy.rows = 1;
        dummy.columns = 1;
        dummy.bits_allocated = 16;
        dummy.bits_stored = 16;
        dummy.high_bit = 15;
        if (auto ov = image::read_overlays(ds, dummy, lenient)) ps.overlays = std::move(ov).value();
    }
    for (std::uint16_t g = 0x6000; g <= 0x601E; g = static_cast<std::uint16_t>(g + 2)) {
        const Tag t = (static_cast<Tag>(g) << 16) | 0x1001u;
        if (ds.contains(t)) ps.activations.push_back({g, get_text(ds, t).value_or("")});
    }
    for (const Dataset& l : items_of(ds, tags::GraphicLayerSequence)) {
        GraphicLayer gl;
        gl.name = get_text(l, tags::GraphicLayer).value_or("");
        gl.order = l.get_int(tags::GraphicLayerOrder).value_or(0);
        if (auto g = l.get_int(tags::GraphicLayerRecommendedDisplayGrayscaleValue); g && *g >= 0 && *g <= 0xFFFF)
            gl.gray = static_cast<std::uint16_t>(*g);
        if (auto lab = get_numbers_n(l, tags::GraphicLayerRecommendedDisplayCielabValue, 3))
            gl.cielab = std::array<std::uint16_t, 3>{static_cast<std::uint16_t>((*lab)[0]), static_cast<std::uint16_t>((*lab)[1]),
                                                     static_cast<std::uint16_t>((*lab)[2])};
        if (auto v = charset::get_utf8(l, tags::GraphicLayerDescription, 0, &cs)) gl.description = *v;
        ps.layers.push_back(std::move(gl));
    }
    std::stable_sort(ps.layers.begin(), ps.layers.end(), [](const GraphicLayer& a, const GraphicLayer& b) { return a.order < b.order; });
    if (Status s = read_annotations(ds, ps, lenient); !s) return s;
    if (ds.contains(tags::ImageRotation) || ds.contains(tags::ImageHorizontalFlip)) {
        ps.has_spatial_transform = true;
        const auto rot = ds.get_int(tags::ImageRotation).value_or(0);
        if (rot == 0 || rot == 90 || rot == 180 || rot == 270) ps.rotation = static_cast<int>(rot);
        else if (!lenient) return Status(Errc::Malformed, "Image Rotation is not 0, 90, 180 or 270");
        else ps.warnings.push_back("invalid Image Rotation ignored");
        ps.flip = get_text(ds, tags::ImageHorizontalFlip).value_or("N") == "Y";
    }
    for (const Dataset& d : items_of(ds, tags::DisplayedAreaSelectionSequence)) {
        DisplayedArea da;
        da.images = image_refs(d);
        auto tl = get_numbers_n(d, tags::DisplayedAreaTlhc, 2), br = get_numbers_n(d, tags::DisplayedAreaBrhc, 2);
        if (!tl || !br || std::fabs((*tl)[0]) > 1e9 || std::fabs((*tl)[1]) > 1e9 || std::fabs((*br)[0]) > 1e9 ||
            std::fabs((*br)[1]) > 1e9) {
            if (!lenient) return Status(Errc::Malformed, "displayed area corners missing");
            ps.warnings.push_back("displayed area without corners skipped");
            continue;
        }
        da.tlhc_column = static_cast<std::int32_t>((*tl)[0]);
        da.tlhc_row = static_cast<std::int32_t>((*tl)[1]);
        da.brhc_column = static_cast<std::int32_t>((*br)[0]);
        da.brhc_row = static_cast<std::int32_t>((*br)[1]);
        da.size_mode = get_text(d, tags::PresentationSizeMode).value_or("");
        if (auto sp = get_numbers_n(d, tags::PresentationPixelSpacing, 2)) da.pixel_spacing = std::array<double, 2>{(*sp)[0], (*sp)[1]};
        if (auto ar = get_numbers_n(d, tags::PresentationPixelAspectRatio, 2))
            da.aspect_ratio = std::array<std::int64_t, 2>{static_cast<std::int64_t>((*ar)[0]), static_cast<std::int64_t>((*ar)[1])};
        da.magnification = d.get_double(tags::PresentationPixelMagnificationRatio);
        ps.displayed_areas.push_back(std::move(da));
    }
    // Colour, pseudo-colour and blending states.
    ps.has_icc_profile = ds.contains(tags::IccProfile);
    if (ds.contains(0x00281101)) {
        if (auto pal = image::read_palette(ds, false, lenient)) ps.palette = std::move(pal).value();
        else ps.warnings.push_back("palette colour lookup table unreadable");
    }
    for (const Dataset& b : items_of(ds, tags::BlendingSequence)) {
        BlendingInput in;
        in.position = get_text(b, tags::BlendingPosition).value_or("");
        in.study_uid = get_text(b, kStudyInstanceUid).value_or("");
        in.images = series_refs(b);
        in.voi = read_voi(b, ps.warnings, lenient);
        in.modality = read_modality(b, ps.warnings, lenient);
        ps.blending.push_back(std::move(in));
    }
    ps.relative_opacity = ds.get_double(tags::RelativeOpacity);
    return ps;
}

// ---------------------------------------------------------------- rendering
Point2 PsRendered::map(Point2 p) const noexcept {
    double u = p.x - box_x0, v = p.y - box_y0;
    double ou = u, ov = v, W = box_w;
    switch (rotation) {
        case 90: ou = box_h - v; ov = u; W = box_h; break;
        case 180: ou = box_w - u; ov = box_h - v; break;
        case 270: ou = v; ov = box_w - u; W = box_h; break;
        default: break;
    }
    if (flip) ou = W - ou;
    return {ou, ov};
}

namespace {

struct Canvas {
    std::uint8_t* px;
    std::int64_t w, h;
    void set(std::int64_t x, std::int64_t y, const std::array<std::uint8_t, 4>& c) const noexcept {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        std::uint8_t* p = px + 4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x));
        p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = c[3];
    }
    void line(Point2 a, Point2 b, const std::array<std::uint8_t, 4>& c) const noexcept {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double steps = std::min(1e6, std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
        const auto n = static_cast<std::int64_t>(steps);
        for (std::int64_t i = 0; i <= n; ++i) {
            const double t = n ? static_cast<double>(i) / static_cast<double>(n) : 0.0;
            set(static_cast<std::int64_t>(std::floor(a.x + t * dx)), static_cast<std::int64_t>(std::floor(a.y + t * dy)), c);
        }
    }
    void fill(const std::vector<Point2>& poly, const std::array<std::uint8_t, 4>& c) const {
        if (w <= 0 || h <= 0 || w > 65536 || h > 65536) return;
        auto m = roi_mask(static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(w), Roi::polygon(poly));
        if (!m) return;
        for (std::size_t i = 0; i < m->size(); ++i)
            if ((*m)[i] > 0.0f) {
                std::uint8_t* p = px + 4 * i;
                p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = c[3];
            }
    }
};

std::vector<Point2> ellipse_points(Point2 c, double a, double b, double ang, std::size_t n = 180) {
    std::vector<Point2> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        const double ex = a * std::cos(t), ey = b * std::sin(t);
        v[i] = {c.x + ex * std::cos(ang) - ey * std::sin(ang), c.y + ex * std::sin(ang) + ey * std::cos(ang)};
    }
    return v;
}

// Draws one graphic object whose points are already in canvas coordinates.
void draw_graphic(const Canvas& cv, GraphicType type, const std::vector<Point2>& p, bool filled,
                  const std::array<std::uint8_t, 4>& c) {
    auto closed = [&](const std::vector<Point2>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) cv.line(v[i], v[(i + 1) % v.size()], c);
    };
    switch (type) {
        case GraphicType::Point:
        case GraphicType::Multipoint:
            for (const auto& q : p) {
                cv.line({q.x - 2, q.y}, {q.x + 2, q.y}, c);
                cv.line({q.x, q.y - 2}, {q.x, q.y + 2}, c);
            }
            break;
        case GraphicType::Polyline:
        case GraphicType::Interpolated:
        case GraphicType::Polygon: {
            const bool is_closed = type == GraphicType::Polygon ||
                                   (p.size() > 2 && p.front().x == p.back().x && p.front().y == p.back().y);
            if (filled && is_closed && p.size() >= 3) cv.fill(p, c);
            for (std::size_t i = 0; i + 1 < p.size(); ++i) cv.line(p[i], p[i + 1], c);
            if (type == GraphicType::Polygon && p.size() > 2) cv.line(p.back(), p.front(), c);
            break;
        }
        case GraphicType::Circle: {
            const double r = std::hypot(p[1].x - p[0].x, p[1].y - p[0].y);
            auto v = ellipse_points(p[0], r, r, 0.0);
            if (filled) cv.fill(v, c);
            closed(v);
            break;
        }
        case GraphicType::Ellipse: {
            const Point2 ctr{(p[0].x + p[1].x) / 2, (p[0].y + p[1].y) / 2};
            const double a = std::hypot(p[1].x - p[0].x, p[1].y - p[0].y) / 2;
            const double b = std::hypot(p[3].x - p[2].x, p[3].y - p[2].y) / 2;
            auto v = ellipse_points(ctr, a, b, std::atan2(p[1].y - p[0].y, p[1].x - p[0].x));
            if (filled) cv.fill(v, c);
            closed(v);
            break;
        }
    }
}

std::array<std::uint8_t, 4> layer_color(const PresentationState& ps, const std::string& name, const std::array<std::uint8_t, 4>& def) {
    if (const GraphicLayer* l = ps.layer(name)) {
        if (l->cielab) {
            const auto c = cielab_to_rgb8((*l->cielab)[0], (*l->cielab)[1], (*l->cielab)[2]);
            return {c[0], c[1], c[2], 255};
        }
        if (l->gray) {
            const auto g = static_cast<std::uint8_t>((std::uint32_t{*l->gray} * 255u + 32767u) / 65535u);
            return {g, g, g, 255};
        }
    }
    return def;
}

Status grayscale_pass(const image::Image& img, const PresentationState& ps, std::uint32_t frame, std::string_view sop,
                      std::span<const std::uint8_t> decoded, std::uint8_t* rgba) {
    const image::PixelInfo& pi = img.info();
    image::ModalityTransform m;
    if (ps.modality) {
        m = *ps.modality;
    } else {
        auto im = img.modality(frame);
        if (!im) return im.take_error();
        m = std::move(im).value();
    }
    const image::detail::SampleReader rd(pi);
    const std::uint64_t n = pi.pixels();
    auto value = [&](std::uint64_t i) -> double {
        switch (pi.kind) {
            case image::SampleKind::Float32: return static_cast<double>(image::detail::load_f32(decoded.data() + 4 * i));
            case image::SampleKind::Float64: return image::detail::load_f64(decoded.data() + 8 * i);
            default: return m.apply(rd.value(decoded.data(), i));
        }
    };
    const SoftcopyVoi* voi = ps.voi_for(sop, frame + 1);
    const image::VoiWindow* win = voi && !voi->windows.empty() ? &voi->windows.front() : nullptr;
    const image::LutTable* vlut = !win && voi && !voi->luts.empty() ? &voi->luts.front() : nullptr;
    // No Softcopy VOI LUT: identity VOI over the range of the modality output (PS3.4 N.2).
    double lo = 0.0, hi = 1.0;
    if (!win && !vlut) {
        lo = std::numeric_limits<double>::infinity();
        hi = -lo;
        for (std::uint64_t i = 0; i < n; ++i) {
            const double v = value(i);
            if (std::isnan(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (!(hi > lo)) hi = lo + 1.0;
    }
    const bool inverse = ps.plut_shape && *ps.plut_shape == image::PresentationShape::Inverse;
    for (std::uint64_t i = 0; i < n; ++i) {
        const double x = value(i);
        double y;
        if (win) y = image::apply_window(x, win->center, win->width, win->function, 0.0, 1.0);
        else if (vlut) y = image::apply_voi_lut(*vlut, x) / vlut->max_output();
        else y = (x - lo) / (hi - lo);
        if (std::isnan(y)) y = 0.0;
        y = std::clamp(y, 0.0, 1.0);
        if (ps.plut) {
            const double idx = std::round(y * (ps.plut->entries - 1.0));
            y = ps.plut->lookup(ps.plut->first_mapped + static_cast<std::int64_t>(idx)) / ps.plut->max_output();
        } else if (inverse) {
            y = 1.0 - y;
        }
        const auto g = static_cast<std::uint8_t>(std::lround(std::clamp(y, 0.0, 1.0) * 255.0));
        std::uint8_t* p = rgba + 4 * i;
        p[0] = p[1] = p[2] = g;
        p[3] = 255;
    }
    return Status::ok();
}

}  // namespace

Result<PsRendered> render_with_presentation_state(const image::Image& img, const PresentationState& ps,
                                                  std::uint32_t frame, std::string_view sop, const PsRenderOptions& opt) {
    const image::PixelInfo& pi = img.info();
    if (frame >= img.frames().frame_count()) return Status(Errc::InvalidArgument, "frame index out of range");
    const std::uint32_t f1 = frame + 1;
    const std::uint64_t n = pi.pixels();
    std::vector<std::uint8_t> base(static_cast<std::size_t>(n) * 4);
    auto decoded_r = img.frames().decode(frame);
    if (!decoded_r) return decoded_r.take_error();
    const std::vector<std::uint8_t>& decoded = *decoded_r;
    if (pi.is_monochrome() && pi.samples_per_pixel == 1) {
        if (Status s = grayscale_pass(img, ps, frame, sop, decoded, base.data()); !s) return s;
    } else {
        image::RenderParams p;
        p.frame = frame;
        p.shutters = false;
        p.overlays = false;
        if (Status s = image::render_2d(img, p, base); !s) return s;
    }
    const auto W = static_cast<std::int64_t>(pi.columns), H = static_cast<std::int64_t>(pi.rows);
    const Canvas image_cv{base.data(), W, H};
    // Shutters of the presentation state (the image's own are not applied).
    if (ps.shutters.any()) {
        std::vector<std::uint8_t> visible(static_cast<std::size_t>(n));
        if (Status s = image::shutter_mask(ps.shutters, pi, frame, ps.overlays, visible); s) {
            const auto c = image::shutter_rgb(ps.shutters);
            for (std::size_t i = 0; i < visible.size(); ++i)
                if (!visible[i]) {
                    base[4 * i] = c[0], base[4 * i + 1] = c[1], base[4 * i + 2] = c[2], base[4 * i + 3] = 255;
                }
        }
    }
    // Activated overlays: the presentation state's own plane for that group, else the image's.
    for (const auto& act : ps.activations) {
        const image::Overlay* ov = nullptr;
        for (const auto& o : ps.overlays)
            if (o.group == act.group) ov = &o;
        const bool own = ov != nullptr;
        if (!ov)
            for (const auto& o : img.overlays())
                if (o.group == act.group) ov = &o;
        if (!ov) continue;
        std::vector<std::uint8_t> mask(static_cast<std::size_t>(n), 0);
        if (Status s = image::overlay_mask(*ov, pi, own ? 0u : frame, decoded, mask); !s) continue;
        const auto c = layer_color(ps, act.layer, opt.overlay_color);
        for (std::size_t i = 0; i < mask.size(); ++i)
            if (mask[i]) base[4 * i] = c[0], base[4 * i + 1] = c[1], base[4 * i + 2] = c[2], base[4 * i + 3] = 255;
    }
    // Pre-spatial-transformation annotations (PIXEL units).
    std::vector<const GraphicAnnotation*> applicable;
    for (const auto& a : ps.annotations)
        if (scope_applies(a.images, sop, f1)) applicable.push_back(&a);
    std::stable_sort(applicable.begin(), applicable.end(), [&](const GraphicAnnotation* a, const GraphicAnnotation* b) {
        const GraphicLayer* la = ps.layer(a->layer);
        const GraphicLayer* lb = ps.layer(b->layer);
        return (la ? la->order : 0) < (lb ? lb->order : 0);
    });
    if (opt.annotations)
        for (const auto* a : applicable) {
            const auto c = layer_color(ps, a->layer, opt.default_color);
            for (const auto& g : a->graphics)
                if (g.units == AnnotationUnits::Pixel) draw_graphic(image_cv, g.type, g.points, g.filled, c);
        }
    // Spatial transformation and displayed area.
    PsRendered out;
    out.rotation = opt.spatial ? ps.rotation : 0;
    out.flip = opt.spatial && ps.flip;
    out.box_x0 = 0;
    out.box_y0 = 0;
    out.box_w = static_cast<double>(W);
    out.box_h = static_cast<double>(H);
    if (const DisplayedArea* da = ps.displayed_area_for(sop, f1)) {
        out.size_mode = da->size_mode;
        out.presentation_pixel_spacing = da->pixel_spacing;
        out.presentation_aspect_ratio = da->aspect_ratio;
        out.magnification = da->magnification;
        if (opt.displayed_area) {
            const std::int64_t c0 = std::min(da->tlhc_column, da->brhc_column), c1 = std::max(da->tlhc_column, da->brhc_column);
            const std::int64_t r0 = std::min(da->tlhc_row, da->brhc_row), r1 = std::max(da->tlhc_row, da->brhc_row);
            out.box_x0 = static_cast<double>(c0 - 1);
            out.box_y0 = static_cast<double>(r0 - 1);
            out.box_w = static_cast<double>(c1 - c0 + 1);
            out.box_h = static_cast<double>(r1 - r0 + 1);
        }
    }
    const bool swap = out.rotation == 90 || out.rotation == 270;
    const double ow = swap ? out.box_h : out.box_w, oh = swap ? out.box_w : out.box_h;
    if (!(ow >= 1.0 && oh >= 1.0) || ow > 65536.0 || oh > 65536.0 || ow * oh > static_cast<double>(1u << 28))
        return Status(Errc::LimitExceeded, "displayed area too large or empty");
    out.width = static_cast<std::uint32_t>(ow);
    out.height = static_cast<std::uint32_t>(oh);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    for (std::uint32_t oy = 0; oy < out.height; ++oy)
        for (std::uint32_t ox = 0; ox < out.width; ++ox) {
            double up = ox + 0.5, vp = oy + 0.5;
            if (out.flip) up = ow - up;
            double u = up, v = vp;
            switch (out.rotation) {
                case 90: u = vp; v = out.box_h - up; break;
                case 180: u = out.box_w - up; v = out.box_h - vp; break;
                case 270: u = out.box_w - vp; v = up; break;
                default: break;
            }
            const auto x = static_cast<std::int64_t>(std::floor(u + out.box_x0));
            const auto y = static_cast<std::int64_t>(std::floor(v + out.box_y0));
            std::uint8_t* d = out.rgba.data() + 4 * (static_cast<std::size_t>(oy) * out.width + ox);
            if (x < 0 || y < 0 || x >= W || y >= H) {
                d[3] = 255;
                continue;
            }
            const std::uint8_t* s = base.data() + 4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x));
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = s[3];
        }
    // Post-transformation annotations (DISPLAY units) and text placement.
    const Canvas out_cv{out.rgba.data(), out.width, out.height};
    auto to_out = [&](AnnotationUnits u, Point2 p) {
        if (u == AnnotationUnits::Display) return Point2{p.x * ow, p.y * oh};
        return out.map(p);
    };
    for (const auto* a : applicable) {
        const auto c = layer_color(ps, a->layer, opt.default_color);
        if (opt.annotations)
            for (const auto& g : a->graphics)
                if (g.units == AnnotationUnits::Display) {
                    std::vector<Point2> pts;
                    for (const auto& p : g.points) pts.push_back(to_out(g.units, p));
                    draw_graphic(out_cv, g.type, pts, g.filled, c);
                }
        for (const auto& t : a->texts) {
            PlacedText pt;
            pt.text = t.text;
            pt.layer = a->layer;
            pt.justification = t.justification;
            pt.anchor_visible = t.anchor_visible;
            pt.rgb = {c[0], c[1], c[2]};
            if (t.anchor && t.anchor_units) pt.anchor = to_out(*t.anchor_units, *t.anchor);
            if (t.box_tlhc && t.box_brhc && t.box_units) {
                const Point2 p0 = to_out(*t.box_units, *t.box_tlhc), p1 = to_out(*t.box_units, *t.box_brhc);
                pt.box_tlhc = Point2{std::min(p0.x, p1.x), std::min(p0.y, p1.y)};
                pt.box_brhc = Point2{std::max(p0.x, p1.x), std::max(p0.y, p1.y)};
            }
            out.texts.push_back(std::move(pt));
        }
    }
    return out;
}

// ---------------------------------------------------------------- writing
Result<DicomFile> write_gsps(const GspsWriteRequest& req) {
    if (req.images.empty()) return Status(Errc::InvalidArgument, "GSPS needs at least one referenced image");
    for (const auto& r : req.images)
        if (!uid_valid(r.sop_class_uid) || !uid_valid(r.sop_instance_uid) || !uid_valid(r.series_uid) || r.rows == 0 ||
            r.columns == 0)
            return Status(Errc::InvalidArgument, "referenced image needs SOP class, instance, series UID, rows and columns");
    if (!cs_valid(req.content_label)) return Status(Errc::InvalidArgument, "Content Label must be a CS value");
    if (req.rotation != 0 && req.rotation != 90 && req.rotation != 180 && req.rotation != 270)
        return Status(Errc::InvalidArgument, "rotation must be 0, 90, 180 or 270");
    if (req.window && !image::window_valid(req.window->center, req.window->width, req.window->function))
        return Status(Errc::InvalidArgument, "invalid window");
    if (!req.series_uid.empty() && !uid_valid(req.series_uid)) return Status(Errc::InvalidArgument, "invalid series UID");
    if (!req.sop_instance_uid.empty() && !uid_valid(req.sop_instance_uid)) return Status(Errc::InvalidArgument, "invalid SOP instance UID");

    DicomFile file;
    file.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = file.dataset;
    bool non_ascii = false;
    const std::string uid = req.sop_instance_uid.empty() ? new_uid() : req.sop_instance_uid;
    const std::string date = today_da(), time = now_tm();
    // SOP Common
    (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, sop::GrayscaleSoftcopyPresentationState);
    (void)ds.set_string(siaara::tags::SOPInstanceUID, VR::UI, uid);
    (void)ds.set_string(tags::InstanceCreationDate, VR::DA, date);
    (void)ds.set_string(tags::InstanceCreationTime, VR::TM, time);
    // Patient, General Study
    copy_patient_study(req.source, ds, non_ascii);
    if (!ds.contains(siaara::tags::StudyInstanceUID)) return Status(Errc::InvalidArgument, "source has no Study Instance UID");
    // General Series, Presentation Series, General Equipment
    (void)ds.set_string(siaara::tags::Modality, VR::CS, "PR");
    (void)ds.set_string(siaara::tags::SeriesInstanceUID, VR::UI, req.series_uid.empty() ? new_uid() : req.series_uid);
    (void)ds.set_string(tags::SeriesNumber, VR::IS, format_is(req.series_number));
    put_text(ds, tags::Manufacturer, VR::LO, req.manufacturer, non_ascii);
    // Presentation State Identification
    (void)ds.set_string(tags::InstanceNumber, VR::IS, format_is(req.instance_number));
    (void)ds.set_string(tags::ContentLabel, VR::CS, req.content_label);
    put_text(ds, tags::ContentDescription, VR::LO, req.content_description, non_ascii);
    (void)ds.set_string(tags::PresentationCreationDate, VR::DA, date);
    (void)ds.set_string(tags::PresentationCreationTime, VR::TM, time);
    put_text(ds, tags::ContentCreatorName, VR::PN, req.content_creator, non_ascii);
    // Presentation State Relationship: Referenced Series Sequence grouped by series.
    auto ref_item = [](const ImageReference& r) {
        Dataset d;
        (void)d.set_string(tags::ReferencedSopClassUid, VR::UI, r.sop_class_uid);
        (void)d.set_string(tags::ReferencedSopInstanceUid, VR::UI, r.sop_instance_uid);
        if (!r.frames.empty()) {
            std::string f;
            for (std::size_t i = 0; i < r.frames.size(); ++i) {
                if (i) f.push_back('\\');
                f += std::to_string(r.frames[i]);
            }
            (void)d.set_string(tags::ReferencedFrameNumber, VR::IS, f);
        }
        return d;
    };
    {
        std::vector<std::string> order;
        for (const auto& r : req.images)
            if (std::find(order.begin(), order.end(), r.series_uid) == order.end()) order.push_back(r.series_uid);
        std::vector<Dataset> series;
        for (const auto& s : order) {
            Dataset item;
            std::vector<Dataset> imgs;
            for (const auto& r : req.images)
                if (r.series_uid == s) imgs.push_back(ref_item(r));
            item.put(make_sequence(tags::ReferencedImageSequence, std::move(imgs)));
            (void)item.set_string(siaara::tags::SeriesInstanceUID, VR::UI, s);
            series.push_back(std::move(item));
        }
        ds.put(make_sequence(tags::ReferencedSeriesSequence, std::move(series)));
    }
    // Displayed Area: one item per image size / spacing (with references unless it covers all).
    {
        struct Group {
            std::uint32_t rows, cols;
            std::optional<std::array<double, 2>> spacing;
            std::vector<const ImageReference*> refs;
        };
        std::vector<Group> groups;
        for (const auto& r : req.images) {
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
                return g.rows == r.rows && g.cols == r.columns && g.spacing == r.pixel_spacing;
            });
            if (it == groups.end()) groups.push_back({r.rows, r.columns, r.pixel_spacing, {&r}});
            else it->refs.push_back(&r);
        }
        std::vector<Dataset> items;
        for (const auto& g : groups) {
            Dataset d;
            if (groups.size() > 1) {
                std::vector<Dataset> refs;
                for (const auto* r : g.refs) refs.push_back(ref_item(*r));
                d.put(make_sequence(tags::ReferencedImageSequence, std::move(refs)));
            }
            (void)d.set_bytes(tags::DisplayedAreaTlhc, VR::SL, Bytes::copy_of("\x01\0\0\0\x01\0\0\0", 8));
            std::uint8_t br[8];
            store_le32(br, g.cols);
            store_le32(br + 4, g.rows);
            (void)d.set_bytes(tags::DisplayedAreaBrhc, VR::SL, Bytes::copy_of(br, 8));
            (void)d.set_string(tags::PresentationSizeMode, VR::CS, "SCALE TO FIT");
            if (g.spacing && (*g.spacing)[0] > 0 && (*g.spacing)[1] > 0)
                (void)d.set_string(tags::PresentationPixelSpacing, VR::DS, format_ds_list(g.spacing->data(), 2));
            else
                (void)d.set_string(tags::PresentationPixelAspectRatio, VR::IS, "1\\1");
            items.push_back(std::move(d));
        }
        ds.put(make_sequence(tags::DisplayedAreaSelectionSequence, std::move(items)));
    }
    // Graphic Annotation and Graphic Layer.
    std::vector<GraphicLayer> layers = req.layers;
    for (const auto& l : layers)
        if (!cs_valid(l.name)) return Status(Errc::InvalidArgument, "Graphic Layer names must be CS values");
    if (!req.annotations.empty()) {
        std::vector<Dataset> anns;
        for (const auto& a : req.annotations) {
            if (!cs_valid(a.layer)) return Status(Errc::InvalidArgument, "annotation layer must be a CS value");
            if (std::none_of(layers.begin(), layers.end(), [&](const GraphicLayer& l) { return l.name == a.layer; })) {
                GraphicLayer l;
                l.name = a.layer;
                l.order = static_cast<std::int64_t>(layers.size()) + 1;
                layers.push_back(std::move(l));
            }
            if (a.texts.empty() && a.graphics.empty()) return Status(Errc::InvalidArgument, "empty annotation");
            Dataset item;
            if (!a.images.empty()) {
                std::vector<Dataset> refs;
                for (const auto& r : a.images) {
                    if (!uid_valid(r.sop_class_uid) || !uid_valid(r.sop_instance_uid))
                        return Status(Errc::InvalidArgument, "annotation image reference needs SOP class and instance");
                    refs.push_back(ref_item(r));
                }
                item.put(make_sequence(tags::ReferencedImageSequence, std::move(refs)));
            }
            (void)item.set_string(tags::GraphicLayer, VR::CS, a.layer);
            if (!a.texts.empty()) {
                std::vector<Dataset> texts;
                for (const auto& t : a.texts) {
                    Dataset d;
                    const bool box = t.box_tlhc && t.box_brhc;
                    const bool anchor = t.anchor.has_value();
                    if (!box && !anchor) return Status(Errc::InvalidArgument, "text object needs a bounding box or an anchor point");
                    if (box) (void)d.set_string(tags::BoundingBoxAnnotationUnits, VR::CS, units_name(t.box_units.value_or(AnnotationUnits::Pixel)));
                    if (anchor) (void)d.set_string(tags::AnchorPointAnnotationUnits, VR::CS, units_name(t.anchor_units.value_or(AnnotationUnits::Pixel)));
                    put_text(d, tags::UnformattedTextValue, VR::ST, t.text, non_ascii);
                    if (box) {
                        const float tl[2] = {static_cast<float>(t.box_tlhc->x), static_cast<float>(t.box_tlhc->y)};
                        const float br[2] = {static_cast<float>(t.box_brhc->x), static_cast<float>(t.box_brhc->y)};
                        (void)d.set_bytes(tags::BoundingBoxTlhc, VR::FL, encode_fl(tl, 2));
                        (void)d.set_bytes(tags::BoundingBoxBrhc, VR::FL, encode_fl(br, 2));
                        const std::string j = t.justification.empty() ? "LEFT" : t.justification;
                        if (j != "LEFT" && j != "RIGHT" && j != "CENTER") return Status(Errc::InvalidArgument, "bad text justification");
                        (void)d.set_string(tags::BoundingBoxJustification, VR::CS, j);
                    }
                    if (anchor) {
                        const float ap[2] = {static_cast<float>(t.anchor->x), static_cast<float>(t.anchor->y)};
                        (void)d.set_bytes(tags::AnchorPoint, VR::FL, encode_fl(ap, 2));
                        (void)d.set_string(tags::AnchorPointVisibility, VR::CS, t.anchor_visible ? "Y" : "N");
                    }
                    texts.push_back(std::move(d));
                }
                item.put(make_sequence(tags::TextObjectSequence, std::move(texts)));
            }
            if (!a.graphics.empty()) {
                std::vector<Dataset> gr;
                for (const auto& g : a.graphics) {
                    if (g.units == AnnotationUnits::Matrix) return Status(Errc::InvalidArgument, "MATRIX units are not written");
                    if (g.type == GraphicType::Multipoint || g.type == GraphicType::Polygon)
                        return Status(Errc::InvalidArgument, "graphic type not allowed in a GSPS");
                    if (!point_count_ok(g.type, g.points.size()) || g.points.size() > 0xFFFF)
                        return Status(Errc::InvalidArgument, "wrong number of graphic points");
                    Dataset d;
                    (void)d.set_string(tags::GraphicAnnotationUnits, VR::CS, units_name(g.units));
                    (void)d.set_uint16(tags::GraphicDimensions, 2);
                    (void)d.set_uint16(tags::NumberOfGraphicPoints, static_cast<std::uint16_t>(g.points.size()));
                    std::vector<float> pts;
                    for (const auto& p : g.points) {
                        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return Status(Errc::InvalidArgument, "non-finite graphic point");
                        pts.push_back(static_cast<float>(p.x));
                        pts.push_back(static_cast<float>(p.y));
                    }
                    (void)d.set_bytes(tags::GraphicData, VR::FL, encode_fl(pts.data(), pts.size()));
                    (void)d.set_string(tags::GraphicType, VR::CS, type_name(g.type));
                    const bool closed = g.points.size() > 2 && g.points.front().x == g.points.back().x &&
                                        g.points.front().y == g.points.back().y;
                    if (g.type == GraphicType::Circle || g.type == GraphicType::Ellipse || closed)
                        (void)d.set_string(tags::GraphicFilled, VR::CS, g.filled ? "Y" : "N");
                    gr.push_back(std::move(d));
                }
                item.put(make_sequence(tags::GraphicObjectSequence, std::move(gr)));
            }
            anns.push_back(std::move(item));
        }
        ds.put(make_sequence(tags::GraphicAnnotationSequence, std::move(anns)));
    }
    if (!layers.empty()) {
        std::vector<Dataset> items;
        for (const auto& l : layers) {
            Dataset d;
            (void)d.set_string(tags::GraphicLayer, VR::CS, l.name);
            (void)d.set_string(tags::GraphicLayerOrder, VR::IS, format_is(l.order));
            if (l.gray) (void)d.set_uint16(tags::GraphicLayerRecommendedDisplayGrayscaleValue, *l.gray);
            if (l.cielab) {
                std::uint8_t b[6];
                for (int k = 0; k < 3; ++k) store_le16(b + 2 * k, (*l.cielab)[static_cast<std::size_t>(k)]);
                (void)d.set_bytes(tags::GraphicLayerRecommendedDisplayCielabValue, VR::US, Bytes::copy_of(b, 6));
            }
            if (!l.description.empty()) put_text(d, tags::GraphicLayerDescription, VR::LO, l.description, non_ascii);
            items.push_back(std::move(d));
        }
        ds.put(make_sequence(tags::GraphicLayerSequence, std::move(items)));
    }
    // Spatial Transformation
    if (req.rotation != 0 || req.flip) {
        (void)ds.set_uint16(tags::ImageRotation, static_cast<std::uint16_t>(req.rotation));
        (void)ds.set_string(tags::ImageHorizontalFlip, VR::CS, req.flip ? "Y" : "N");
    }
    // Softcopy VOI LUT
    if (req.window) {
        Dataset d;
        (void)d.set_string(kWindowCenter, VR::DS, format_ds(req.window->center));
        (void)d.set_string(kWindowWidth, VR::DS, format_ds(req.window->width));
        if (!req.window->explanation.empty()) put_text(d, kWindowExplanation, VR::LO, req.window->explanation, non_ascii);
        if (req.window->function == image::VoiFunction::LinearExact) (void)d.set_string(kVoiLutFunction, VR::CS, "LINEAR_EXACT");
        else if (req.window->function == image::VoiFunction::Sigmoid) (void)d.set_string(kVoiLutFunction, VR::CS, "SIGMOID");
        std::vector<Dataset> items;
        items.push_back(std::move(d));
        ds.put(make_sequence(tags::SoftcopyVoiLutSequence, std::move(items)));
    }
    // Softcopy Presentation LUT
    (void)ds.set_string(kPresentationLutShape, VR::CS, req.invert ? "INVERSE" : "IDENTITY");
    finish_charset(ds, non_ascii);
    return file;
}

}  // namespace siaara::clinical
