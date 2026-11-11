// SIAARA DICOM Toolkit - softcopy presentation states.
// Written from DICOM PS3.3 A.33 (Grayscale, Color, Pseudo-Color and Blending Softcopy
// Presentation State IODs), C.11.10/C.11.11 (Presentation State Identification / Relationship),
// C.11.12 (Presentation State Shutter), C.10.4 (Displayed Area), C.10.5 (Graphic Annotation),
// C.10.6 (Spatial Transformation), C.10.7 (Graphic Layer), C.11.7 (Overlay Activation), C.11.8
// (Softcopy VOI LUT), C.11.6 (Softcopy Presentation LUT), C.11.14/C.11.15 (Presentation State
// Blending) and PS3.4 N.2 (the order in which the transformations are applied).
//
// Coordinates of PIXEL annotation units: x = column, y = row, (0,0) = top left corner of the top
// left pixel (C.10.5.1.2). DISPLAY units: 0..1 across / down the displayed area.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clinical/geometry.hpp"
#include "clinical/measure.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/lut.hpp"
#include "image/overlay.hpp"
#include "image/render.hpp"

namespace siaara::clinical {

namespace sop {
constexpr std::string_view GrayscaleSoftcopyPresentationState = "1.2.840.10008.5.1.4.1.1.11.1";
constexpr std::string_view ColorSoftcopyPresentationState = "1.2.840.10008.5.1.4.1.1.11.2";
constexpr std::string_view PseudoColorSoftcopyPresentationState = "1.2.840.10008.5.1.4.1.1.11.3";
constexpr std::string_view BlendingSoftcopyPresentationState = "1.2.840.10008.5.1.4.1.1.11.4";
}  // namespace sop

enum class AnnotationUnits : std::uint8_t { Pixel, Display, Matrix };
enum class GraphicType : std::uint8_t { Point, Polyline, Interpolated, Circle, Ellipse, Multipoint, Polygon };

struct ImageReference {
    std::string sop_class_uid;
    std::string sop_instance_uid;
    std::string series_uid;           // from the enclosing Referenced Series Sequence item
    std::vector<std::int32_t> frames; // 1-based Referenced Frame Number; empty = all frames
    // Writer only: image size (Displayed Area) and pixel spacing (row, column) if known.
    std::uint32_t rows = 0, columns = 0;
    std::optional<std::array<double, 2>> pixel_spacing;
    bool applies_to(std::string_view sop_instance, std::uint32_t frame_1based) const noexcept;
};

struct GraphicObject {
    AnnotationUnits units = AnnotationUnits::Pixel;
    GraphicType type = GraphicType::Polyline;
    std::vector<Point2> points;
    bool filled = false;
    std::optional<std::int64_t> group_id;
};

struct TextObject {
    std::string text;  // UTF-8
    std::optional<AnnotationUnits> box_units, anchor_units;
    std::optional<Point2> box_tlhc, box_brhc, anchor;
    bool anchor_visible = false;
    std::string justification;  // LEFT / RIGHT / CENTER
};

struct GraphicAnnotation {
    std::string layer;
    std::vector<ImageReference> images;  // empty = every referenced image
    std::vector<TextObject> texts;
    std::vector<GraphicObject> graphics;
};

struct GraphicLayer {
    std::string name;
    std::int64_t order = 0;
    std::optional<std::uint16_t> gray;                    // P-Value
    std::optional<std::array<std::uint16_t, 3>> cielab;   // DICOM-encoded CIELab
    std::string description;
};

struct DisplayedArea {
    std::vector<ImageReference> images;  // empty = all
    std::int32_t tlhc_column = 1, tlhc_row = 1, brhc_column = 0, brhc_row = 0;  // 1-based, pre-transform
    std::string size_mode;               // SCALE TO FIT / TRUE SIZE / MAGNIFY
    std::optional<std::array<double, 2>> pixel_spacing;  // row, column (mm)
    std::optional<std::array<std::int64_t, 2>> aspect_ratio;  // vertical, horizontal
    std::optional<double> magnification;
};

struct SoftcopyVoi {
    std::vector<ImageReference> images;  // empty = all
    std::vector<image::VoiWindow> windows;
    std::vector<image::LutTable> luts;
};

struct OverlayActivation {
    std::uint16_t group = 0x6000;
    std::string layer;
};

struct BlendingInput {
    std::string position;  // UNDERLYING / SUPERIMPOSED
    std::string study_uid;
    std::vector<ImageReference> images;
    std::vector<SoftcopyVoi> voi;
    std::optional<image::ModalityTransform> modality;
};

enum class PresentationKind : std::uint8_t { Grayscale, Color, PseudoColor, Blending, Other };

struct PresentationState {
    PresentationKind kind = PresentationKind::Other;
    std::string sop_class_uid, sop_instance_uid;
    std::string content_label, content_description, content_creator;
    std::string creation_date, creation_time;
    std::optional<std::int64_t> instance_number;
    std::vector<ImageReference> references;   // Presentation State Relationship
    std::optional<image::ModalityTransform> modality;
    std::vector<SoftcopyVoi> voi;
    std::optional<image::PresentationShape> plut_shape;
    std::optional<image::LutTable> plut;
    image::Shutters shutters;
    std::vector<image::Overlay> overlays;      // overlay planes held in the presentation state
    std::vector<OverlayActivation> activations;
    std::vector<GraphicLayer> layers;
    std::vector<GraphicAnnotation> annotations;
    bool has_spatial_transform = false;
    int rotation = 0;                          // clockwise degrees: 0, 90, 180, 270
    bool flip = false;                         // horizontal flip after rotation
    std::vector<DisplayedArea> displayed_areas;
    // Colour / pseudo-colour / blending (parsed, not applied).
    bool has_icc_profile = false;
    std::optional<image::PaletteLut> palette;
    std::vector<BlendingInput> blending;
    std::optional<double> relative_opacity;
    std::vector<std::string> warnings;

    bool references_image(std::string_view sop_instance, std::uint32_t frame_1based = 1) const noexcept;
    const SoftcopyVoi* voi_for(std::string_view sop_instance, std::uint32_t frame_1based) const noexcept;
    const DisplayedArea* displayed_area_for(std::string_view sop_instance, std::uint32_t frame_1based) const noexcept;
    const GraphicLayer* layer(std::string_view name) const noexcept;
};

// Reads any softcopy presentation state. Fails with Unsupported for other SOP classes and with
// Malformed for structurally invalid annotations (strict) - in lenient mode those items are
// skipped and recorded in warnings.
Result<PresentationState> read_presentation_state(const Dataset& ds, bool lenient = true);

// ---------------------------------------------------------------- applying (PS3.4 N.2)
struct PsRenderOptions {
    bool annotations = true;          // rasterise graphic objects (text is returned, not drawn)
    bool spatial = true;              // rotation / flip
    bool displayed_area = true;       // crop / pad to the displayed area
    std::array<std::uint8_t, 4> default_color{255, 255, 0, 255};  // layers without a colour
    std::array<std::uint8_t, 4> overlay_color{255, 255, 255, 255};
    // Work (points, line steps, scan-line edge visits, filled pixels) one render may spend on
    // graphic annotations; beyond it the render fails with LimitExceeded (R-056). 0 = default.
    std::uint64_t max_draw_work = 0;
    static constexpr std::uint64_t kDefaultMaxDrawWork = std::uint64_t{1} << 27;
    // The most bytes the rendered output (displayed area, 4 bytes per pixel) may take; a larger
    // one is LimitExceeded before it is allocated (R-191). 0 = kDefaultMaxOutputBytes: only the
    // renderer's own bound of 2^28 pixels applies.
    std::uint64_t max_output_bytes = 0;
    static constexpr std::uint64_t kDefaultMaxOutputBytes = 0;
};

struct PlacedText {
    std::string text, layer, justification;
    std::optional<Point2> anchor, box_tlhc, box_brhc;  // output pixel coordinates
    bool anchor_visible = false;
    std::array<std::uint8_t, 3> rgb{255, 255, 0};
};

struct PsRendered {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;    // width x height x 4
    std::vector<PlacedText> texts;
    std::string size_mode;
    std::optional<std::array<double, 2>> presentation_pixel_spacing;
    std::optional<std::array<std::int64_t, 2>> presentation_aspect_ratio;
    std::optional<double> magnification;
    // Maps image PIXEL-unit coordinates to output coordinates (rotation, flip, displayed area).
    int rotation = 0;
    bool flip = false;
    double box_x0 = 0, box_y0 = 0, box_w = 0, box_h = 0;  // displayed area, pre-transform, continuous
    Point2 map(Point2 image_point) const noexcept;
};

// Renders frame `frame` (0-based) of `img` as the presentation state says: modality (the
// state's Modality LUT, if any, replaces the image's), Softcopy VOI LUT, Presentation LUT (the
// image's own MONOCHROME1 inversion and Presentation LUT are replaced), presentation state
// shutters, activated overlays, PIXEL annotations, spatial transformation, displayed area, then
// DISPLAY annotations. Grayscale images go through the state's grayscale pipeline; colour
// images are rendered with render_2d and then shuttered / annotated / transformed.
Result<PsRendered> render_with_presentation_state(const image::Image& img, const PresentationState& ps,
                                                  std::uint32_t frame, std::string_view sop_instance_uid,
                                                  const PsRenderOptions& opt = {});

// ---------------------------------------------------------------- writing
struct GspsWriteRequest {
    const Dataset* source = nullptr;      // Patient / General Study attributes are copied from it
    std::vector<ImageReference> images;   // series_uid, SOP class/instance, rows and columns required
    std::vector<GraphicLayer> layers;     // layers named by annotations but not listed are added
    std::vector<GraphicAnnotation> annotations;
    std::optional<image::VoiWindow> window;  // applies to all referenced images
    bool invert = false;                  // Presentation LUT Shape INVERSE (else IDENTITY)
    int rotation = 0;                     // 0 / 90 / 180 / 270; with flip, writes the Spatial Transformation
    bool flip = false;
    std::string content_label = "ANNOTATIONS";  // CS: A-Z 0-9 _ and space, 16 characters at most
    std::string content_description;
    std::string content_creator;          // PN, UTF-8
    std::string series_uid, sop_instance_uid;  // generated when empty
    std::int32_t series_number = 9900, instance_number = 1;
    std::string manufacturer = "SIAARA";
};

// Builds a Grayscale Softcopy Presentation State (explicit VR little endian). Fails with
// InvalidArgument for missing references, unknown units, wrong point counts or a bad label.
Result<DicomFile> write_gsps(const GspsWriteRequest& req);

}  // namespace siaara::clinical
