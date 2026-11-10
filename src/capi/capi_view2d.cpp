// SIAARA DICOM Toolkit - C API: decoded frames, 2D viewport rendering, VOI, overlays, shutters,
// palettes, presentation states (include/siaara/siaara.h, "viewer / PACS additions").
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "capi/capi_internal.hpp"
#include "modules/data.hpp"
#include "clinical/gsps.hpp"
#include "image/color.hpp"
#include "image/frames.hpp"
#include "image/lut.hpp"
#include "image/overlay.hpp"
#include "image/render.hpp"
#include "image/samples.hpp"
#include "image/viewport.hpp"

struct siaara_frame {
    siaara::DicomFile file;  // own copy: the Image below points into file.dataset
    std::unique_ptr<siaara::image::Image> img;
    std::vector<std::uint8_t> pixels;  // decoded frame, native layout
    std::uint32_t index = 0;
    siaara::image::ModalityTransform modality;
    siaara::image::VoiSet voi;
    double vmin = 0, vmax = 0;
    std::string sop_uid;
    siaara::capi::LiveHandle live_{};
};

struct siaara_pstate {
    siaara::clinical::PresentationState ps;
    siaara::capi::LiveHandle live_{};
};

namespace {

using siaara::Errc;
using siaara::Status;
namespace img = siaara::image;
namespace cl = siaara::clinical;
namespace capi = siaara::capi;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
siaara_status fail(const Status& s) noexcept { return capi::set_error(s); }
siaara_status ok() noexcept { return capi::clear_error(); }

template <class F>
siaara_status guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return fail(Errc::OutOfMemory, "out of memory");
    } catch (...) {
        return fail(Errc::Internal, "internal error");
    }
}

siaara_status copy_out(std::string_view s, char* buf, size_t cap, size_t* needed) {
    if (needed) *needed = s.size();
    if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (s.size() + 1 > cap) {
        std::memcpy(buf, s.data(), cap - 1);
        buf[cap - 1] = '\0';
        return fail(Errc::BufferTooSmall, "buffer too small");
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return ok();
}

// Byte buffers: buf NULL with cap 0 = size query (query = true).
siaara_status check_out(std::uint64_t need, const void* buf, size_t cap, size_t* needed, bool& query) {
    query = false;
    if (need > SIZE_MAX) return fail(Errc::LimitExceeded, "output does not fit in memory on this platform");
    if (needed) *needed = static_cast<size_t>(need);
    if (!buf && cap == 0) {
        query = true;
        return ok();
    }
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (cap < need) return fail(Errc::BufferTooSmall, "buffer too small");
    return SIAARA_OK;
}

// ---- options

struct Parsed {
    img::RenderParams rp;
    img::ViewportParams vp;
    bool gray8 = false;
    bool image_size = false;  // viewport 0 x 0
    const siaara_pstate* ps = nullptr;
};

siaara_status parse_options(const siaara_frame& f, const siaara_view2d_options* o, Parsed& out) {
    out.rp.frame = f.index;
    out.image_size = true;
    if (!o) return SIAARA_OK;
    if (o->struct_size < sizeof(siaara_view2d_options))
        return fail(Errc::InvalidArgument, "options must be set up with struct_size");
    if ((o->viewport_width == 0) != (o->viewport_height == 0))
        return fail(Errc::InvalidArgument, "viewport_width and viewport_height must both be 0 or both > 0");
    out.image_size = o->viewport_width == 0;
    out.vp.width = o->viewport_width;
    out.vp.height = o->viewport_height;
    if (o->output_format != SIAARA_PIXELS_RGBA8 && o->output_format != SIAARA_PIXELS_GRAY8)
        return fail(Errc::InvalidArgument, "unknown output_format");
    out.gray8 = o->output_format == SIAARA_PIXELS_GRAY8;
    out.vp.zoom = o->zoom;
    out.vp.pan_x = o->pan_x;
    out.vp.pan_y = o->pan_y;
    out.vp.rotation = o->rotation;
    out.vp.flip_horizontal = o->flip_horizontal != 0;
    out.vp.flip_vertical = o->flip_vertical != 0;
    switch (o->interpolation) {
        case SIAARA_INTERP_NEAREST: out.vp.interpolation = img::ViewportInterpolation::Nearest; break;
        case SIAARA_INTERP_LINEAR: out.vp.interpolation = img::ViewportInterpolation::Linear; break;
        default: return fail(Errc::Unsupported, "2D interpolation: SIAARA_INTERP_NEAREST or SIAARA_INTERP_LINEAR");
    }
    std::memcpy(out.vp.background.data(), o->background_rgba, 4);
    img::RenderParams& rp = out.rp;
    switch (o->colormap) {
        case SIAARA_COLORMAP_GRAY: break;
        case SIAARA_COLORMAP_INVERSE_GRAY: rp.invert = true; break;
        case SIAARA_COLORMAP_HOT_IRON: rp.pseudo_color = img::StandardPalette::HotIron; break;
        case SIAARA_COLORMAP_PET: rp.pseudo_color = img::StandardPalette::Pet; break;
        case SIAARA_COLORMAP_HOT_METAL_BLUE: rp.pseudo_color = img::StandardPalette::HotMetalBlue; break;
        case SIAARA_COLORMAP_PET_20_STEP: rp.pseudo_color = img::StandardPalette::Pet20Step; break;
        default: return fail(Errc::InvalidArgument, "unknown colormap");
    }
    switch (o->voi_mode) {
        case SIAARA_VOI_FILE: rp.voi = img::VoiMode::File; break;
        case SIAARA_VOI_EXPLICIT: rp.voi = img::VoiMode::Explicit; break;
        case SIAARA_VOI_FULL_RANGE: rp.voi = img::VoiMode::FullRange; break;
        case SIAARA_VOI_IDENTITY: rp.voi = img::VoiMode::Identity; break;
        default: return fail(Errc::InvalidArgument, "unknown voi_mode");
    }
    rp.voi_index = o->voi_index;
    if (rp.voi == img::VoiMode::File && !f.voi.empty() && o->voi_index >= f.voi.windows.size() + f.voi.luts.size())
        return fail(Errc::NotFound, "voi_index out of range (see siaara_voi_count)");
    rp.window_center = o->window_center;
    rp.window_width = o->window_width;
    switch (o->voi_function) {
        case SIAARA_VOI_LINEAR: rp.function = img::VoiFunction::Linear; break;
        case SIAARA_VOI_LINEAR_EXACT: rp.function = img::VoiFunction::LinearExact; break;
        case SIAARA_VOI_SIGMOID: rp.function = img::VoiFunction::Sigmoid; break;
        default: return fail(Errc::InvalidArgument, "unknown voi_function");
    }
    if (o->invert) rp.invert = !rp.invert;
    rp.overlays = o->no_overlays == 0;
    rp.shutters = o->no_shutters == 0;
    rp.overlay_groups = o->overlay_groups ? o->overlay_groups : 0xFFFFu;
    if (o->overlay_rgba[0] | o->overlay_rgba[1] | o->overlay_rgba[2] | o->overlay_rgba[3])
        std::memcpy(rp.overlay_rgba.data(), o->overlay_rgba, 4);
    out.ps = o->presentation_state;
    return SIAARA_OK;
}

// Image-resolution RGBA of the frame (or of the presentation state's displayed area).
struct Source {
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0, h = 0;
    std::optional<cl::PsRendered> ps;  // set when a presentation state was applied
};

siaara_status render_source(const siaara_frame& f, const Parsed& p, Source& s) {
    if (p.ps) {
        cl::PsRenderOptions po;
        po.overlay_color = p.rp.overlay_rgba;
        auto r = cl::render_with_presentation_state(*f.img, p.ps->ps, f.index, f.sop_uid, po);
        if (!r) return fail(r.error());
        cl::PsRendered done = std::move(r).value();
        s.w = done.width;
        s.h = done.height;
        s.rgba = std::move(done.rgba);
        done.rgba.clear();
        s.ps = std::move(done);
        return SIAARA_OK;
    }
    const img::PixelInfo& pi = f.img->info();
    s.w = pi.columns;
    s.h = pi.rows;
    s.rgba.resize(static_cast<std::size_t>(pi.pixels() * 4));
    if (Status st = img::render_2d_decoded(*f.img, p.rp, f.pixels, s.rgba); !st) return fail(st);
    return SIAARA_OK;
}

// Image point -> presentation state output and back (rotation, flip, displayed area box).
void ps_to_output(const cl::PsRendered& r, double u, double v, double& x, double& y) {
    const cl::Point2 q = r.map(cl::Point2{u, v});
    x = q.x;
    y = q.y;
}
void ps_to_image(const cl::PsRendered& r, double x, double y, double& u, double& v) {
    const double W = (r.rotation == 90 || r.rotation == 270) ? r.box_h : r.box_w;
    double ou = r.flip ? W - x : x, ov = y, a = ou, b = ov;
    switch (r.rotation) {
        case 90: a = ov; b = r.box_h - ou; break;
        case 180: a = r.box_w - ou; b = r.box_h - ov; break;
        case 270: a = r.box_w - ov; b = ou; break;
        default: break;
    }
    u = a + r.box_x0;
    v = b + r.box_y0;
}

void to_gray(const std::vector<std::uint8_t>& rgba, std::vector<std::uint8_t>& g) {
    g.resize(rgba.size() / 4);
    for (std::size_t i = 0; i < g.size(); ++i) {
        const std::uint32_t r = rgba[4 * i], gr = rgba[4 * i + 1], b = rgba[4 * i + 2];
        g[i] = static_cast<std::uint8_t>((77u * r + 150u * gr + 29u * b + 128u) >> 8);  // Rec. 601 luma
    }
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_frame_open(const siaara_file* file, uint32_t frame, siaara_frame** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = nullptr;
        auto h = std::make_unique<siaara_frame>();
        h->file = file->file;
        auto im = img::Image::open(h->file.dataset, h->file.transfer_syntax);
        if (!im) return fail(im.error());
        h->img = std::make_unique<img::Image>(std::move(im).value());
        const img::PixelInfo& pi = h->img->info();
        if (frame >= pi.number_of_frames) return fail(Errc::InvalidArgument, "frame index out of range");
        h->index = frame;
        const std::uint64_t fb = h->img->frames().frame_bytes();
        if (fb > SIZE_MAX || fb > h->img->frames().options().memory_budget)
            return fail(Errc::LimitExceeded, "frame exceeds the memory budget");
        h->pixels.resize(static_cast<std::size_t>(fb));
        if (Status s = h->img->frames().decode(frame, h->pixels); !s) return fail(s);
        h->sop_uid = h->file.dataset.get_string(siaara::tags::SOPInstanceUID).value_or(std::string());
        if (pi.is_monochrome() || pi.samples_per_pixel == 1) {
            auto m = h->img->modality(frame);
            if (!m) return fail(m.error());
            h->modality = std::move(m).value();
        }
        if (pi.is_monochrome()) {
            auto v = h->img->voi(frame, h->modality);
            if (!v) return fail(v.error());
            h->voi = std::move(v).value();
        }
        // Value range present in the frame.
        if (pi.kind != img::SampleKind::Integer) {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            const std::uint64_t n = pi.pixels();
            for (std::uint64_t i = 0; i < n; ++i) {
                const double v = pi.kind == img::SampleKind::Float32
                                     ? static_cast<double>(img::detail::load_f32(h->pixels.data() + 4 * i))
                                     : img::detail::load_f64(h->pixels.data() + 8 * i);
                if (!std::isfinite(v)) continue;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            if (!(lo <= hi)) lo = hi = 0;
            h->vmin = lo;
            h->vmax = hi;
        } else if (pi.samples_per_pixel == 1 && pi.bits_stored <= 32) {
            auto w = img::full_range_window(*h->img, frame, h->pixels, h->modality);
            if (!w) return fail(w.error());
            // LINEAR full-range window: center = (x1 + x2 + 1) / 2, width = x2 - x1 + 1.
            h->vmin = w->center - w->width / 2.0;
            h->vmax = h->vmin + w->width - 1.0;
        } else {
            h->vmin = 0;
            h->vmax = pi.bits_stored >= 63 ? 9.2e18 : static_cast<double>((std::uint64_t{1} << pi.bits_stored) - 1u);
        }
        *out = h.release();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_frame_free(siaara_frame* frame) SIAARA_NOEXCEPT { delete frame; }

SIAARA_API siaara_status SIAARA_CALL siaara_frame_info(const siaara_frame* frame, siaara_frame_properties* out) {
    return guarded([&]() -> siaara_status {
        if (!frame || !out || out->struct_size < sizeof(siaara_frame_properties))
            return fail(Errc::InvalidArgument, "frame and out (with struct_size) must be set");
        const img::PixelInfo& pi = frame->img->info();
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_frame_properties));
        out->struct_size = size;
        out->frame = frame->index;
        out->number_of_frames = pi.number_of_frames;
        out->rows = pi.rows;
        out->columns = pi.columns;
        out->samples_per_pixel = pi.samples_per_pixel;
        out->bits_allocated = pi.bits_allocated;
        out->bits_stored = pi.bits_stored;
        uint32_t fl = 0;
        if (pi.is_monochrome()) fl |= SIAARA_FRAMEF_MONOCHROME;
        if (frame->img->inverted()) fl |= SIAARA_FRAMEF_INVERTED;
        if (frame->img->palette() && !frame->img->supplemental_palette()) fl |= SIAARA_FRAMEF_PALETTE;
        if (frame->img->supplemental_palette()) fl |= SIAARA_FRAMEF_SUPPLEMENTAL_PALETTE;
        if (frame->modality.has_lut) fl |= SIAARA_FRAMEF_MODALITY_LUT;
        if (frame->img->presentation_lut()) fl |= SIAARA_FRAMEF_PRESENTATION_LUT;
        const img::Shutters& sh = frame->img->shutters();
        if (sh.rectangular) fl |= SIAARA_FRAMEF_SHUTTER_RECTANGULAR;
        if (sh.circular) fl |= SIAARA_FRAMEF_SHUTTER_CIRCULAR;
        if (sh.polygonal) fl |= SIAARA_FRAMEF_SHUTTER_POLYGONAL;
        if (sh.bitmap) fl |= SIAARA_FRAMEF_SHUTTER_BITMAP;
        if (pi.kind != img::SampleKind::Integer) fl |= SIAARA_FRAMEF_FLOAT;
        if (frame->voi.from_functional_groups) fl |= SIAARA_FRAMEF_VOI_FROM_FUNCTIONAL_GROUPS;
        if (frame->modality.dose_grid_scaling) fl |= SIAARA_FRAMEF_DOSE_GRID_SCALING;
        out->flags = fl;
        out->voi_count = static_cast<uint32_t>(frame->voi.windows.size() + frame->voi.luts.size());
        out->overlay_count = static_cast<uint32_t>(frame->img->overlays().size());
        const auto c = img::shutter_rgb(sh);
        std::memcpy(out->shutter_rgb, c.data(), 3);
        out->rescale_slope = frame->modality.has_lut ? 1.0 : frame->modality.slope;
        out->rescale_intercept = frame->modality.has_lut ? 0.0 : frame->modality.intercept;
        out->min_value = frame->vmin;
        out->max_value = frame->vmax;
        out->decoded_bytes = frame->pixels.size();
        const size_t n = std::min(pi.photometric_text.size(), sizeof(out->photometric) - 1);
        std::memcpy(out->photometric, pi.photometric_text.data(), n);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_pixels(const siaara_frame* frame, const uint8_t** out_data,
                                                         uint64_t* out_bytes) {
    return guarded([&]() -> siaara_status {
        if (!frame || !out_data) return fail(Errc::InvalidArgument, "frame and out_data must not be NULL");
        *out_data = frame->pixels.data();
        if (out_bytes) *out_bytes = frame->pixels.size();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_frame_view(const siaara_file* file, uint32_t frame,
                                                            const uint8_t** out_data, uint64_t* out_bytes) {
    return guarded([&]() -> siaara_status {
        if (!file || !out_data) return fail(Errc::InvalidArgument, "file and out_data must not be NULL");
        *out_data = nullptr;
        if (out_bytes) *out_bytes = 0;
        auto fr = img::FrameReader::open(file->file.dataset, file->file.transfer_syntax);
        if (!fr) return fail(fr.error());
        if (frame >= fr->frame_count()) return fail(Errc::InvalidArgument, "frame index out of range");
        auto v = fr->native_view(frame);
        if (!v) return fail(Errc::Unsupported, "the frame cannot be viewed in place (encapsulated, big endian or 1-bit)");
        // The view points into the data set's shared value bytes, which the file keeps alive.
        *out_data = v->data();
        if (out_bytes) *out_bytes = v->size();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_render(const siaara_frame* frame, const siaara_view2d_options* options,
                                                         uint8_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame) return fail(Errc::InvalidArgument, "frame must not be NULL");
        Parsed p;
        if (siaara_status st = parse_options(*frame, options, p); st != SIAARA_OK) return st;
        const unsigned ch = p.gray8 ? 1u : 4u;
        const img::PixelInfo& pi = frame->img->info();
        bool query = false;
        if (!p.ps) {
            if (p.image_size) {
                p.vp.width = pi.columns;
                p.vp.height = pi.rows;
            }
            const std::uint64_t need = std::uint64_t{p.vp.width} * p.vp.height * ch;
            if (siaara_status st = check_out(need, buf, cap, needed, query); st != SIAARA_OK || query) return st;
        }
        Source src;
        if (siaara_status st = render_source(*frame, p, src); st != SIAARA_OK) return st;
        if (p.ps) {
            if (p.image_size) {
                p.vp.width = src.w;
                p.vp.height = src.h;
            }
            const std::uint64_t need = std::uint64_t{p.vp.width} * p.vp.height * ch;
            if (siaara_status st = check_out(need, buf, cap, needed, query); st != SIAARA_OK || query) return st;
        }
        if (p.image_size && p.vp.zoom == 0.0) p.vp.zoom = 1.0;
        std::span<const std::uint8_t> in = src.rgba;
        std::vector<std::uint8_t> gray;
        if (p.gray8) {
            to_gray(src.rgba, gray);
            in = gray;
        }
        const std::size_t n = static_cast<std::size_t>(std::uint64_t{p.vp.width} * p.vp.height * ch);
        if (Status s = img::place_in_viewport(in, src.w, src.h, ch, p.vp, std::span<std::uint8_t>(buf, n)); !s)
            return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_map_point(const siaara_frame* frame, const siaara_view2d_options* options,
                                                            int32_t to_image, double x, double y, double* out_x,
                                                            double* out_y) {
    return guarded([&]() -> siaara_status {
        if (!frame || !out_x || !out_y) return fail(Errc::InvalidArgument, "frame, out_x and out_y must not be NULL");
        if (!std::isfinite(x) || !std::isfinite(y)) return fail(Errc::InvalidArgument, "the point must be finite");
        Parsed p;
        if (siaara_status st = parse_options(*frame, options, p); st != SIAARA_OK) return st;
        const img::PixelInfo& pi = frame->img->info();
        std::uint32_t w = pi.columns, h = pi.rows;
        std::optional<cl::PsRendered> geo;
        if (p.ps) {
            Source src;
            if (siaara_status st = render_source(*frame, p, src); st != SIAARA_OK) return st;
            w = src.w;
            h = src.h;
            geo = std::move(src.ps);
        }
        if (p.image_size) {
            p.vp.width = w;
            p.vp.height = h;
            if (p.vp.zoom == 0.0) p.vp.zoom = 1.0;
        }
        auto t = img::ViewportTransform::make(p.vp, w, h);
        if (!t) return fail(t.error());
        if (to_image) {
            double u = 0, v = 0;
            t->to_image(x, y, u, v);
            if (geo) ps_to_image(*geo, u, v, u, v);
            *out_x = u;
            *out_y = v;
        } else {
            double u = x, v = y;
            if (geo) ps_to_output(*geo, x, y, u, v);
            t->to_view(u, v, *out_x, *out_y);
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_voi_count(const siaara_frame* frame, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!frame || !out) return fail(Errc::InvalidArgument, "frame and out must not be NULL");
        *out = static_cast<uint32_t>(frame->voi.windows.size() + frame->voi.luts.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_voi_get(const siaara_frame* frame, uint32_t index, siaara_voi_info* out,
                                                    char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame || !out || out->struct_size < sizeof(siaara_voi_info))
            return fail(Errc::InvalidArgument, "frame and out (with struct_size) must be set");
        const img::VoiSet& v = frame->voi;
        if (index >= v.windows.size() + v.luts.size()) return fail(Errc::NotFound, "VOI index out of range");
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_voi_info));
        out->struct_size = size;
        out->from_functional_groups = v.from_functional_groups ? 1 : 0;
        std::string_view text;
        if (index < v.windows.size()) {
            const img::VoiWindow& w = v.windows[index];
            out->kind = SIAARA_VOI_KIND_WINDOW;
            out->function = w.function == img::VoiFunction::Linear        ? SIAARA_VOI_LINEAR
                            : w.function == img::VoiFunction::LinearExact ? SIAARA_VOI_LINEAR_EXACT
                                                                          : SIAARA_VOI_SIGMOID;
            out->center = w.center;
            out->width = w.width;
            text = w.explanation;
        } else {
            const img::LutTable& l = v.luts[index - v.windows.size()];
            out->kind = SIAARA_VOI_KIND_LUT;
            out->function = SIAARA_VOI_LINEAR;
            out->lut_entries = l.entries;
            out->lut_first_mapped = static_cast<int32_t>(std::clamp<std::int64_t>(l.first_mapped, INT32_MIN, INT32_MAX));
            out->lut_bits = l.bits;
            text = l.explanation;
        }
        if (!buf && cap == 0) {
            if (needed) *needed = text.size();
            return ok();
        }
        return copy_out(text, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_voi_lut_data(const siaara_frame* frame, uint32_t index, uint16_t* buf,
                                                         size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame) return fail(Errc::InvalidArgument, "frame must not be NULL");
        const img::VoiSet& v = frame->voi;
        if (index < v.windows.size() || index >= v.windows.size() + v.luts.size())
            return fail(Errc::NotFound, "index does not name a VOI LUT");
        const img::LutTable& l = v.luts[index - v.windows.size()];
        if (needed) *needed = l.data.size();
        if (!buf && cap == 0) return ok();
        if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
        if (cap < l.data.size()) return fail(Errc::BufferTooSmall, "buffer too small");
        std::memcpy(buf, l.data.data(), l.data.size() * sizeof(uint16_t));
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_overlay_get(const siaara_frame* frame, uint32_t index, siaara_overlay_info* out) {
    return guarded([&]() -> siaara_status {
        if (!frame || !out || out->struct_size < sizeof(siaara_overlay_info))
            return fail(Errc::InvalidArgument, "frame and out (with struct_size) must be set");
        const auto& ovs = frame->img->overlays();
        if (index >= ovs.size()) return fail(Errc::NotFound, "overlay index out of range");
        const img::Overlay& o = ovs[index];
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_overlay_info));
        out->struct_size = size;
        out->group = o.group;
        out->rows = o.rows;
        out->columns = o.columns;
        out->origin_row = o.origin_row;
        out->origin_column = o.origin_column;
        out->type = static_cast<unsigned char>(o.type);
        out->frames = o.frames;
        out->image_frame_origin = o.image_frame_origin;
        out->embedded = o.embedded ? 1 : 0;
        out->covers_frame = o.overlay_frame(frame->index) ? 1 : 0;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_overlay_text(const siaara_frame* frame, uint32_t index, int32_t field,
                                                         char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame) return fail(Errc::InvalidArgument, "frame must not be NULL");
        const auto& ovs = frame->img->overlays();
        if (index >= ovs.size()) return fail(Errc::NotFound, "overlay index out of range");
        const img::Overlay& o = ovs[index];
        switch (field) {
            case SIAARA_OVERLAY_LABEL: return copy_out(o.label, buf, cap, needed);
            case SIAARA_OVERLAY_DESCRIPTION: return copy_out(o.description, buf, cap, needed);
            case SIAARA_OVERLAY_SUBTYPE: return copy_out(o.subtype, buf, cap, needed);
            default: return fail(Errc::InvalidArgument, "unknown field");
        }
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_overlay_mask(const siaara_frame* frame, uint32_t index, uint8_t* buf,
                                                         size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame) return fail(Errc::InvalidArgument, "frame must not be NULL");
        const auto& ovs = frame->img->overlays();
        if (index >= ovs.size()) return fail(Errc::NotFound, "overlay index out of range");
        const img::PixelInfo& pi = frame->img->info();
        bool query = false;
        if (siaara_status st = check_out(pi.pixels(), buf, cap, needed, query); st != SIAARA_OK || query) return st;
        std::span<std::uint8_t> mask(buf, static_cast<std::size_t>(pi.pixels()));
        std::fill(mask.begin(), mask.end(), std::uint8_t{0});
        if (Status s = img::overlay_mask(ovs[index], pi, frame->index, frame->pixels, mask); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_shutter_mask(const siaara_frame* frame, uint8_t* buf, size_t cap,
                                                         size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame) return fail(Errc::InvalidArgument, "frame must not be NULL");
        const img::PixelInfo& pi = frame->img->info();
        bool query = false;
        if (siaara_status st = check_out(pi.pixels(), buf, cap, needed, query); st != SIAARA_OK || query) return st;
        std::span<std::uint8_t> vis(buf, static_cast<std::size_t>(pi.pixels()));
        if (!frame->img->shutters().any()) {
            std::fill(vis.begin(), vis.end(), std::uint8_t{1});
            return ok();
        }
        if (Status s = img::shutter_mask(frame->img->shutters(), pi, frame->index, frame->img->overlays(), vis); !s)
            return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_palette(const siaara_frame* frame, siaara_palette_info* info,
                                                          uint16_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!frame) return fail(Errc::InvalidArgument, "frame must not be NULL");
        if (info && info->struct_size < sizeof(siaara_palette_info))
            return fail(Errc::InvalidArgument, "info must be set up with struct_size");
        if (!frame->img->palette()) return fail(Errc::NotFound, "the image has no palette");
        const img::PaletteLut& pal = *frame->img->palette();
        if (info) {
            const uint32_t size = info->struct_size;
            std::memset(info, 0, sizeof(siaara_palette_info));
            info->struct_size = size;
            info->entries = pal.entries;
            info->first_mapped = pal.first_mapped;
            info->bits = pal.bits;
            info->has_alpha = pal.alpha.empty() ? 0 : 1;
            info->segmented = pal.segmented ? 1 : 0;
            info->supplemental = frame->img->supplemental_palette() ? 1 : 0;
        }
        const std::size_t need = std::size_t{pal.entries} * 4u;
        if (needed) *needed = need;
        if (!buf && cap == 0) return ok();
        if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
        if (cap < need) return fail(Errc::BufferTooSmall, "buffer too small");
        const std::uint16_t amax = static_cast<std::uint16_t>((1u << pal.bits) - 1u);
        for (std::size_t k = 0; k < pal.entries; ++k) {
            buf[4 * k] = pal.red[k];
            buf[4 * k + 1] = pal.green[k];
            buf[4 * k + 2] = pal.blue[k];
            // Alpha data is 8-bit (C.7.6.3.1.5): scaled to the palette precision.
            buf[4 * k + 3] = pal.alpha.size() > k
                                 ? static_cast<std::uint16_t>(pal.bits == 8 ? pal.alpha[k] : pal.alpha[k] * 257u)
                                 : amax;
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_colormap_rgba8(int32_t colormap, uint8_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (colormap < SIAARA_COLORMAP_GRAY || colormap > SIAARA_COLORMAP_PET_20_STEP)
            return fail(Errc::InvalidArgument, "unknown colormap");
        bool query = false;
        if (siaara_status st = check_out(1024, buf, cap, needed, query); st != SIAARA_OK || query) return st;
        for (unsigned g = 0; g < 256; ++g) {
            std::uint8_t r = static_cast<std::uint8_t>(g), gg = r, b = r;
            if (colormap == SIAARA_COLORMAP_INVERSE_GRAY) r = gg = b = static_cast<std::uint8_t>(255 - g);
            if (colormap >= SIAARA_COLORMAP_HOT_IRON) {
                const auto sp = static_cast<img::StandardPalette>(colormap - SIAARA_COLORMAP_HOT_IRON);
                const img::PaletteLut& pal = img::standard_palette(sp);
                const std::uint32_t k = pal.index(g);
                r = pal.to8(pal.red[k]);
                gg = pal.to8(pal.green[k]);
                b = pal.to8(pal.blue[k]);
            }
            const std::size_t o = std::size_t{g} * 4;
            buf[o] = r;
            buf[o + 1] = gg;
            buf[o + 2] = b;
            buf[o + 3] = 255;
        }
        return ok();
    });
}

// ---------------------------------------------------------------- presentation states
SIAARA_API siaara_status SIAARA_CALL siaara_pstate_open(const siaara_file* file, siaara_pstate** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = nullptr;
        auto r = cl::read_presentation_state(file->file.dataset);
        if (!r) return fail(r.error());
        auto h = std::make_unique<siaara_pstate>();
        h->ps = std::move(r).value();
        *out = h.release();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_pstate_free(siaara_pstate* ps) SIAARA_NOEXCEPT { delete ps; }

SIAARA_API siaara_status SIAARA_CALL siaara_pstate_info(const siaara_pstate* ps, siaara_pstate_properties* out) {
    return guarded([&]() -> siaara_status {
        if (!ps || !out || out->struct_size < sizeof(siaara_pstate_properties))
            return fail(Errc::InvalidArgument, "ps and out (with struct_size) must be set");
        const cl::PresentationState& p = ps->ps;
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_pstate_properties));
        out->struct_size = size;
        switch (p.kind) {
            case cl::PresentationKind::Grayscale: out->kind = SIAARA_PSTATE_GRAYSCALE; break;
            case cl::PresentationKind::Color: out->kind = SIAARA_PSTATE_COLOR; break;
            case cl::PresentationKind::PseudoColor: out->kind = SIAARA_PSTATE_PSEUDO_COLOR; break;
            case cl::PresentationKind::Blending: out->kind = SIAARA_PSTATE_BLENDING; break;
            default: out->kind = SIAARA_PSTATE_OTHER; break;
        }
        auto c = [](std::size_t v) { return static_cast<uint32_t>(std::min<std::size_t>(v, UINT32_MAX)); };
        out->has_spatial_transform = p.has_spatial_transform ? 1 : 0;
        out->rotation = p.rotation;
        out->flip = p.flip ? 1 : 0;
        out->reference_count = c(p.references.size());
        out->annotation_count = c(p.annotations.size());
        std::size_t texts = 0, graphics = 0;
        for (const auto& a : p.annotations) {
            texts += a.texts.size();
            graphics += a.graphics.size();
        }
        out->text_count = c(texts);
        out->graphic_count = c(graphics);
        out->layer_count = c(p.layers.size());
        out->voi_count = c(p.voi.size());
        out->displayed_area_count = c(p.displayed_areas.size());
        out->has_shutters = p.shutters.any() ? 1 : 0;
        out->has_modality = p.modality ? 1 : 0;
        out->warning_count = c(p.warnings.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_pstate_string(const siaara_pstate* ps, int32_t field, char* buf, size_t cap,
                                                          size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!ps) return fail(Errc::InvalidArgument, "ps must not be NULL");
        const cl::PresentationState& p = ps->ps;
        switch (field) {
            case SIAARA_PSTATE_LABEL: return copy_out(p.content_label, buf, cap, needed);
            case SIAARA_PSTATE_DESCRIPTION: return copy_out(p.content_description, buf, cap, needed);
            case SIAARA_PSTATE_CREATOR: return copy_out(p.content_creator, buf, cap, needed);
            case SIAARA_PSTATE_CREATION_DATE: return copy_out(p.creation_date, buf, cap, needed);
            case SIAARA_PSTATE_CREATION_TIME: return copy_out(p.creation_time, buf, cap, needed);
            case SIAARA_PSTATE_SOP_INSTANCE_UID: return copy_out(p.sop_instance_uid, buf, cap, needed);
            case SIAARA_PSTATE_SOP_CLASS_UID: return copy_out(p.sop_class_uid, buf, cap, needed);
            default: return fail(Errc::InvalidArgument, "unknown field");
        }
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_pstate_reference(const siaara_pstate* ps, uint32_t index, char* buf, size_t cap,
                                                             size_t* needed, uint32_t* frame_count) {
    return guarded([&]() -> siaara_status {
        if (!ps) return fail(Errc::InvalidArgument, "ps must not be NULL");
        if (index >= ps->ps.references.size()) return fail(Errc::NotFound, "reference index out of range");
        const cl::ImageReference& r = ps->ps.references[index];
        if (frame_count) *frame_count = static_cast<uint32_t>(r.frames.size());
        return copy_out(r.sop_instance_uid, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_pstate_applies(const siaara_pstate* ps, const char* sop_instance_uid,
                                                           size_t len, uint32_t frame_1based, int32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!ps || !out || (!sop_instance_uid && len)) return fail(Errc::InvalidArgument, "bad argument");
        *out = ps->ps.references_image(std::string_view(sop_instance_uid ? sop_instance_uid : "", len), frame_1based) ? 1 : 0;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_pixel_value(const siaara_frame* frame, uint32_t column, uint32_t row,
                                                              uint32_t sample, siaara_pixel_value* out) {
    return guarded([&]() -> siaara_status {
        if (!frame || !capi::pixel_value_sized(out))
            return fail(Errc::InvalidArgument, "frame and out (with struct_size) must be set");
        const img::PixelInfo& pi = frame->img->info();
        const bool modality = pi.samples_per_pixel == 1 && pi.kind == img::SampleKind::Integer;
        auto v = siaara::modules::pixel_value(frame->file.dataset, pi, frame->pixels, modality ? &frame->modality : nullptr,
                                              column, row, sample);
        if (!v) return fail(v.error());
        capi::fill_pixel_value(v.value(), frame->index, column, row, sample, out);
        return ok();
    });
}

}  // extern "C"
