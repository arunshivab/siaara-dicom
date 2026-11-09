// SIAARA DICOM Toolkit - 2D rendering. Written from DICOM PS3.3 C.11, C.7.6.3, C.9.2, C.7.6.11
// and PS3.4 N.2 (grayscale and colour pipelines).
#include "image/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include "image/samples.hpp"

namespace siaara::image {

namespace {

using detail::SampleReader;

inline std::uint32_t pack(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) noexcept {
    const std::uint8_t px[4] = {r, g, b, a};
    std::uint32_t v;
    std::memcpy(&v, px, 4);
    return v;
}

inline std::uint8_t to8(double y01) noexcept {
    if (!(y01 > 0.0)) return 0;
    if (y01 >= 1.0) return 255;
    return static_cast<std::uint8_t>(y01 * 255.0 + 0.5);
}

// The VOI stage chosen for a render, producing y in [0, 1].
struct Voi {
    bool use_lut = false;
    VoiWindow w;
    const LutTable* lut = nullptr;
    double operator()(double x) const noexcept {
        if (use_lut) return apply_voi_lut(*lut, x) / lut->max_output();
        return apply_window(x, w.center, w.width, w.function, 0.0, 1.0);
    }
};

// Everything after the VOI stage: inversion, Presentation LUT, 8-bit P-Value, pseudo-colour.
struct Finish {
    bool invert = false;
    const LutTable* plut = nullptr;
    const PaletteLut* pseudo = nullptr;
    std::uint32_t operator()(double y) const noexcept {
        if (invert) y = 1.0 - y;
        if (plut) {
            const double idx = std::floor(std::clamp(y, 0.0, 1.0) * (plut->entries - 1) + 0.5);
            y = plut->data[static_cast<std::size_t>(idx)] / plut->max_output();
        }
        const std::uint8_t g = to8(y);
        if (pseudo) {
            const std::uint32_t k = pseudo->index(g);
            return pack(pseudo->to8(pseudo->red[k]), pseudo->to8(pseudo->green[k]), pseudo->to8(pseudo->blue[k]));
        }
        return pack(g, g, g);
    }
};

std::uint32_t palette_rgba(const PaletteLut& pal, std::int64_t v) noexcept {
    const std::uint32_t k = pal.index(v);
    return pack(pal.to8(pal.red[k]), pal.to8(pal.green[k]), pal.to8(pal.blue[k]));
}

// Applies a stored-value LUT (index = stored value - min) to every pixel: the hot loop.
void apply_value_lut(const PixelInfo& pi, const std::uint8_t* src, const std::uint32_t* lut, std::uint8_t* dst) {
    const std::uint64_t n = pi.pixels();
    const unsigned shift = pi.low_bit();
    const std::uint32_t mask = (1u << pi.bits_stored) - 1u;
    // Two's complement to offset binary: index = value - min_stored.
    const std::uint32_t flip = pi.is_signed ? 1u << (pi.bits_stored - 1u) : 0u;
    if (pi.bits_allocated == 16) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t raw = static_cast<std::uint32_t>(src[2 * i]) | (static_cast<std::uint32_t>(src[2 * i + 1]) << 8);
            const std::uint32_t idx = ((raw >> shift) & mask) ^ flip;
            std::memcpy(dst + 4 * i, &lut[idx], 4);
        }
    } else if (pi.bits_allocated == 8) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t idx = ((static_cast<std::uint32_t>(src[i]) >> shift) & mask) ^ flip;
            std::memcpy(dst + 4 * i, &lut[idx], 4);
        }
    } else {
        const SampleReader rd(pi);
        for (std::size_t i = 0; i < n; ++i) {
            const auto idx = static_cast<std::uint32_t>((rd.raw(src, i) >> shift) & mask) ^ flip;
            std::memcpy(dst + 4 * i, &lut[idx], 4);
        }
    }
}

Status resolve_voi(const Image& img, const RenderParams& p, std::span<const std::uint8_t> frame,
                   const ModalityTransform& m, const VoiSet& set, Voi& out) {
    const PixelInfo& pi = img.info();
    switch (p.voi) {
        case VoiMode::Explicit:
            if (!window_valid(p.window_center, p.window_width, p.function))
                return Status(Errc::InvalidArgument, "render: invalid window (LINEAR needs width >= 1, others > 0)");
            out.w.center = p.window_center;
            out.w.width = p.window_width;
            out.w.function = p.function;
            return Status::ok();
        case VoiMode::File:
            if (p.voi_index < set.windows.size()) {
                out.w = set.windows[p.voi_index];
                return Status::ok();
            }
            if (p.voi_index - set.windows.size() < set.luts.size()) {
                out.use_lut = true;
                out.lut = &set.luts[p.voi_index - set.windows.size()];
                return Status::ok();
            }
            if (!set.empty()) return Status(Errc::NotFound, "render: the file has no VOI with that index");
            [[fallthrough]];
        case VoiMode::FullRange: {
            auto w = full_range_window(img, p.frame, frame, m);
            if (!w) return w.take_error();
            out.w = *w;
            return Status::ok();
        }
        case VoiMode::Identity: {
            double lo = 0, hi = 0;
            if (pi.kind != SampleKind::Integer) {  // no possible range for real values: use the present range
                auto w = full_range_window(img, p.frame, frame, m);
                if (!w) return w.take_error();
                out.w = *w;
                return Status::ok();
            }
            m.output_range(detail::min_stored(pi), detail::max_stored(pi), lo, hi);
            out.w.center = (lo + hi + 1) / 2;
            out.w.width = hi - lo + 1;
            return Status::ok();
        }
    }
    return Status(Errc::InvalidArgument, "render: unknown VOI mode");
}

}  // namespace

Result<Image> Image::open(const Dataset& ds, std::string_view ts_uid, const FrameOptions& opt) {
    auto fr = FrameReader::open(ds, ts_uid, opt);
    if (!fr) return fr.take_error();
    Image img;
    img.ds_ = &ds;
    img.frames_ = std::move(fr).value();
    const PixelInfo& pi = img.frames_.info();
    auto ov = read_overlays(ds, pi, opt.lenient);
    if (!ov) return ov.take_error();
    img.overlays_ = std::move(ov).value();
    auto sh = read_shutters(ds, opt.lenient);
    if (!sh) return sh.take_error();
    img.shutters_ = std::move(sh).value();
    if (pi.photometric == Photometric::PaletteColor) {
        auto pal = read_palette(ds, pi.is_signed, opt.lenient);
        if (!pal) return pal.take_error();
        img.palette_ = std::move(pal).value();
    } else if (pi.is_monochrome() && ds.contains(0x00281101)) {
        // Supplemental Palette Color LUT (C.7.6.19): Pixel Presentation COLOR or MIXED.
        auto pres = ds.get_string(tags::PixelPresentation);
        if (pres && (*pres == "COLOR" || *pres == "MIXED")) {
            auto pal = read_palette(ds, pi.is_signed, opt.lenient);
            if (pal) {
                img.palette_ = std::move(pal).value();
                img.supplemental_ = true;
            } else if (!opt.lenient) {
                return pal.take_error();
            }
        }
    }
    auto pl = image::presentation_lut(ds, opt.lenient);
    if (!pl) return pl.take_error();
    img.plut_ = std::move(pl).value();
    img.inverted_ = output_inverted(pi, ds);
    return img;
}

Result<ModalityTransform> Image::modality(std::uint32_t frame) const {
    return modality_for_frame(*ds_, info(), frame, frames_.options().lenient);
}

Result<VoiSet> Image::voi(std::uint32_t frame, const ModalityTransform& m) const {
    if (!info().is_monochrome()) return VoiSet{};  // C.11.2.1.2: VOI only has meaning for MONOCHROME1/2
    return voi_for_frame(*ds_, info(), m, frame, frames_.options().lenient);
}

Result<VoiWindow> full_range_window(const Image& img, std::uint32_t, std::span<const std::uint8_t> decoded,
                                    const ModalityTransform& m) {
    const PixelInfo& pi = img.info();
    if (decoded.size() < pi.native_frame_bytes()) return Status(Errc::Truncated, "render: frame too short");
    const std::uint64_t n = pi.pixels();
    VoiWindow w;
    if (pi.kind != SampleKind::Integer) {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = pi.kind == SampleKind::Float32 ? static_cast<double>(detail::load_f32(decoded.data() + 4 * i))
                                                            : detail::load_f64(decoded.data() + 8 * i);
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (!(lo <= hi)) lo = hi = 0;
        w.function = VoiFunction::LinearExact;
        w.center = lo / 2 + hi / 2;
        w.width = hi - lo;
        if (!(w.width > 0) || !std::isfinite(w.width)) w.width = std::isfinite(w.width) ? 1.0 : std::numeric_limits<double>::max();
        return w;
    }
    const SampleReader rd(pi);
    std::int64_t vmin = INT64_MAX, vmax = INT64_MIN;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t v = rd.value(decoded.data(), i);
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
    double lo = 0, hi = 0;
    if (m.has_lut) {
        lo = std::numeric_limits<double>::infinity();
        hi = -lo;
        // The LUT is not monotonic in general: look at every value the frame can reach.
        const std::int64_t a = std::max<std::int64_t>(vmin, m.lut.first_mapped - 1);
        const std::int64_t b = std::min<std::int64_t>(vmax, m.lut.first_mapped + m.lut.entries);
        for (std::int64_t v = a; v <= b; ++v) {
            const double y = m.apply(v);
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        lo = std::min({lo, m.apply(vmin), m.apply(vmax)});
        hi = std::max({hi, m.apply(vmin), m.apply(vmax)});
    } else {
        m.output_range(vmin, vmax, lo, hi);
    }
    w.function = VoiFunction::Linear;
    w.center = (lo + hi + 1) / 2;
    w.width = hi - lo + 1;
    return w;
}

Status render_2d(const Image& img, const RenderParams& p, std::span<std::uint8_t> rgba) {
    const PixelInfo& pi = img.info();
    const FrameReader& fr = img.frames();
    if (p.frame >= fr.frame_count()) return Status(Errc::InvalidArgument, "render: frame index out of range");
    const std::uint64_t n = pi.pixels();
    if (rgba.size() < n * 4) return Status(Errc::BufferTooSmall, "render: output buffer too small");
    if (pi.kind == SampleKind::Integer && pi.bits_stored > 32 && pi.samples_per_pixel == 1)
        return Status(Errc::Unsupported, "render: more than 32 bits stored");

    // ---- frame bytes (in place for native frames when possible)
    std::vector<std::uint8_t> scratch;
    std::span<const std::uint8_t> frame;
    if (auto v = fr.native_view(p.frame)) {
        frame = *v;
    } else {
        if (fr.frame_bytes() > fr.options().memory_budget)
            return Status(Errc::LimitExceeded, "render: frame exceeds the memory budget");
        try {
            scratch.resize(static_cast<std::size_t>(fr.frame_bytes()));
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "render: out of memory");
        }
        if (Status s = fr.decode(p.frame, scratch); !s) return s;
        frame = scratch;
    }
    return render_2d_decoded(img, p, frame, rgba);
}

Status render_2d_decoded(const Image& img, const RenderParams& p, std::span<const std::uint8_t> frame,
                         std::span<std::uint8_t> rgba) {
    const PixelInfo& pi = img.info();
    const FrameReader& fr = img.frames();
    if (p.frame >= fr.frame_count()) return Status(Errc::InvalidArgument, "render: frame index out of range");
    const std::uint64_t n = pi.pixels();
    if (rgba.size() < n * 4) return Status(Errc::BufferTooSmall, "render: output buffer too small");
    if (frame.size() < fr.frame_bytes()) return Status(Errc::Truncated, "render: decoded frame too short");
    if (pi.kind == SampleKind::Integer && pi.bits_stored > 32 && pi.samples_per_pixel == 1)
        return Status(Errc::Unsupported, "render: more than 32 bits stored");
    std::uint8_t* dst = rgba.data();
    const bool lut_path = pi.kind == SampleKind::Integer && pi.bits_stored <= 16 && pi.samples_per_pixel == 1;
    const SampleReader rd(pi);

    try {
        if (pi.is_monochrome()) {
            auto m = img.modality(p.frame);
            if (!m) return m.take_error();
            auto set = img.voi(p.frame, *m);
            if (!set) return set.take_error();
            Voi voi;
            if (Status s = resolve_voi(img, p, frame, *m, *set, voi); !s) return s;
            Finish fin;
            fin.invert = img.inverted() != p.invert;
            if (p.apply_presentation_lut && img.presentation_lut()) fin.plut = &*img.presentation_lut();
            if (p.pseudo_color) fin.pseudo = &standard_palette(*p.pseudo_color);
            const PaletteLut* sup = img.supplemental_palette() ? &*img.palette() : nullptr;
            if (lut_path) {
                const std::int64_t lo = detail::min_stored(pi), hi = detail::max_stored(pi);
                std::vector<std::uint32_t> lut(static_cast<std::size_t>(hi - lo + 1));
                for (std::int64_t v = lo; v <= hi; ++v) {
                    std::uint32_t c;
                    if (sup && v >= sup->first_mapped) c = palette_rgba(*sup, v);
                    else c = fin(voi(m->apply(v)));
                    lut[static_cast<std::size_t>(v - lo)] = c;
                }
                apply_value_lut(pi, frame.data(), lut.data(), dst);
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    std::uint32_t c;
                    if (pi.kind == SampleKind::Float32) {
                        c = fin(voi(static_cast<double>(detail::load_f32(frame.data() + 4 * i))));
                    } else if (pi.kind == SampleKind::Float64) {
                        c = fin(voi(detail::load_f64(frame.data() + 8 * i)));
                    } else {
                        const std::int64_t v = rd.value(frame.data(), i);
                        if (sup && v >= sup->first_mapped) c = palette_rgba(*sup, v);
                        else c = fin(voi(m->apply(v)));
                    }
                    std::memcpy(dst + 4 * i, &c, 4);
                }
            }
        } else if (pi.photometric == Photometric::PaletteColor) {
            const PaletteLut& pal = *img.palette();
            if (lut_path) {
                const std::int64_t lo = detail::min_stored(pi), hi = detail::max_stored(pi);
                std::vector<std::uint32_t> lut(static_cast<std::size_t>(hi - lo + 1));
                for (std::int64_t v = lo; v <= hi; ++v) {
                    std::uint32_t c = palette_rgba(pal, v);
                    if (p.invert) c = ~c | pack(0, 0, 0, 255);
                    lut[static_cast<std::size_t>(v - lo)] = c;
                }
                apply_value_lut(pi, frame.data(), lut.data(), dst);
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    std::uint32_t c = palette_rgba(pal, rd.value(frame.data(), i));
                    if (p.invert) c = ~c | pack(0, 0, 0, 255);
                    std::memcpy(dst + 4 * i, &c, 4);
                }
            }
        } else {
            if (pi.samples_per_pixel != 3 || pi.kind != SampleKind::Integer)
                return Status(Errc::Unsupported, "render: photometric interpretation " + pi.photometric_text);
            ColorOptions co = p.color;
            co.native_layout = !fr.encapsulated();
            co.codec_applied_mct = fr.codec_applies_color_transform();
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(rgb_frame_bytes(pi)));
            if (Status s = to_rgb(pi, frame, rgb, co); !s) return s;
            const unsigned bps = pi.bits_allocated / 8u;
            const unsigned bs = std::min<unsigned>(pi.bits_stored, 32);
            const std::uint64_t maxv = (std::uint64_t{1} << bs) - 1u;
            const std::uint8_t x = p.invert ? 255 : 0;
            if (bps == 1 && bs == 8) {
                for (std::size_t i = 0; i < n; ++i) {
                    dst[4 * i] = static_cast<std::uint8_t>(rgb[3 * i] ^ x);
                    dst[4 * i + 1] = static_cast<std::uint8_t>(rgb[3 * i + 1] ^ x);
                    dst[4 * i + 2] = static_cast<std::uint8_t>(rgb[3 * i + 2] ^ x);
                    dst[4 * i + 3] = 255;
                }
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    for (unsigned s = 0; s < 3; ++s) {
                        std::uint64_t v = 0;
                        const std::uint8_t* q = rgb.data() + (3 * i + s) * bps;
                        for (unsigned k = bps; k-- > 0;) v = (v << 8) | q[k];
                        v = std::min(v, maxv);
                        dst[4 * i + s] = static_cast<std::uint8_t>(((v * 255u + maxv / 2) / maxv) ^ x);
                    }
                    dst[4 * i + 3] = 255;
                }
            }
        }

        // ---- display shutters, then overlays on top
        if (p.shutters && img.shutters().any()) {
            std::vector<std::uint8_t> vis(static_cast<std::size_t>(n));
            if (Status s = shutter_mask(img.shutters(), pi, p.frame, img.overlays(), vis); !s) return s;
            const auto c = shutter_rgb(img.shutters());
            const std::uint32_t px = pack(c[0], c[1], c[2]);
            for (std::size_t i = 0; i < n; ++i)
                if (!vis[i]) std::memcpy(dst + 4 * i, &px, 4);
        }
        if (p.overlays && !img.overlays().empty()) {
            std::vector<std::uint8_t> mask(static_cast<std::size_t>(n), 0);
            bool anyov = false;
            for (const Overlay& ov : img.overlays()) {
                const unsigned k = (ov.group - 0x6000u) / 2u;
                if (!((p.overlay_groups >> k) & 1u)) continue;
                if (img.shutters().bitmap && ov.group == img.shutters().overlay_group) continue;  // C.7.6.15
                if (Status s = overlay_mask(ov, pi, p.frame, frame, mask); !s) return s;
                anyov = true;
            }
            if (anyov) {
                const unsigned a = p.overlay_rgba[3];
                for (std::size_t i = 0; i < n; ++i) {
                    if (!mask[i]) continue;
                    for (unsigned s = 0; s < 3; ++s)
                        dst[4 * i + s] = static_cast<std::uint8_t>((p.overlay_rgba[s] * a + dst[4 * i + s] * (255u - a) + 127u) / 255u);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "render: out of memory");
    }
    return Status::ok();
}

Result<std::vector<std::uint8_t>> render_2d(const Image& img, const RenderParams& p) {
    const std::uint64_t bytes = img.info().pixels() * 4;
    if (bytes > img.frames().options().memory_budget) return Status(Errc::LimitExceeded, "render: output exceeds the memory budget");
    std::vector<std::uint8_t> out;
    try {
        out.resize(static_cast<std::size_t>(bytes));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "render: out of memory");
    }
    if (Status s = render_2d(img, p, out); !s) return s;
    return out;
}

Status decode_modality_f32(const Image& img, std::uint32_t frame, std::span<float> out) {
    const PixelInfo& pi = img.info();
    const FrameReader& fr = img.frames();
    std::uint64_t count = 0;
    if (!checked_mul(pi.pixels(), pi.samples_per_pixel, count)) return Status(Errc::LimitExceeded, "frames: size overflows");
    if (out.size() < count) return Status(Errc::BufferTooSmall, "frames: output buffer too small");
    if (frame >= fr.frame_count()) return Status(Errc::InvalidArgument, "frames: frame index out of range");
    if (pi.kind == SampleKind::Integer && pi.bits_stored > 32)
        return Status(Errc::Unsupported, "frames: more than 32 bits stored");
    try {
        auto bytes = fr.decode(frame);
        if (!bytes) return bytes.take_error();
        const std::vector<std::uint8_t>& f = *bytes;
        const std::uint64_t n = pi.pixels();
        if (pi.kind == SampleKind::Float32) {
            for (std::size_t i = 0; i < n; ++i) out[i] = detail::load_f32(f.data() + 4 * i);
            return Status::ok();
        }
        if (pi.kind == SampleKind::Float64) {
            for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(detail::load_f64(f.data() + 8 * i));
            return Status::ok();
        }
        const SampleReader rd(pi);
        if (pi.samples_per_pixel == 1) {
            auto m = img.modality(frame);
            if (!m) return m.take_error();
            if (m->is_identity()) {
                for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(rd.value(f.data(), i));
            } else {
                for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(m->apply(rd.value(f.data(), i)));
            }
            return Status::ok();
        }
        ColorOptions co;
        co.native_layout = !fr.encapsulated();
        std::vector<std::uint8_t> full(static_cast<std::size_t>(rgb_frame_bytes(pi)));
        if (pi.samples_per_pixel != 3) {
            // 4-sample retired models: de-planarise only.
            full.resize(static_cast<std::size_t>(f.size()));
            if (pi.planar) {
                if (Status s = planar_to_interleaved(f, full, n, pi.samples_per_pixel, pi.bits_allocated / 8u); !s) return s;
            } else {
                full = f;
            }
        } else if (Status s = to_interleaved_full(pi, f, full, co); !s) {
            return s;
        }
        const unsigned bps = pi.bits_allocated / 8u;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint64_t v = 0;
            for (unsigned k = bps; k-- > 0;) v = (v << 8) | full[i * bps + k];
            v = (v >> pi.low_bit()) & rd.mask;
            out[i] = static_cast<float>(v);
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "frames: out of memory");
    }
    return Status::ok();
}

}  // namespace siaara::image
