// SIAARA DICOM Toolkit - overlay planes and display shutters. Written from DICOM PS3.3 C.9.2,
// C.7.6.11, C.7.6.15 and C.10.7.1.1 (CIELab encoding).
#include "image/overlay.hpp"

#include "image/raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>

#include "image/samples.hpp"

namespace siaara::image {

namespace {

Tag ot(std::uint16_t group, std::uint16_t element) noexcept { return make_tag(group, element); }

// Integer attribute, reading the bytes directly when the VR is unknown (implicit VR without a
// dictionary entry gives UN, which Dataset::get_int would read as bytes).
std::optional<std::int64_t> get_i16(const Dataset& ds, Tag t, std::size_t vm, bool is_signed) {
    const Element* e = ds.find(t);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    if (e->vr == VR::UN || e->vr == VR::None || e->vr == VR::OB || e->vr == VR::OW) {
        if (e->value.size() < 2 * (vm + 1)) return std::nullopt;
        const std::uint16_t v = load_le16(e->value.data() + 2 * vm);
        return is_signed ? static_cast<std::int64_t>(static_cast<std::int16_t>(v)) : static_cast<std::int64_t>(v);
    }
    return ds.get_int(t, vm);
}

}  // namespace

std::optional<std::uint32_t> Overlay::overlay_frame(std::uint32_t image_frame) const noexcept {
    if (!multi_frame) return 0u;  // C.9.2.1.4: applies to all frames
    const std::int64_t f = static_cast<std::int64_t>(image_frame) - (static_cast<std::int64_t>(image_frame_origin) - 1);
    if (f < 0 || f >= static_cast<std::int64_t>(frames)) return std::nullopt;
    return static_cast<std::uint32_t>(f);
}

Result<std::vector<Overlay>> read_overlays(const Dataset& ds, const PixelInfo& pi, bool lenient) {
    std::vector<Overlay> out;
    try {
        for (std::uint16_t g = 0x6000; g <= 0x601E; g = static_cast<std::uint16_t>(g + 2)) {
            const bool has_data = ds.contains(ot(g, 0x3000));
            if (!has_data && !ds.contains(ot(g, 0x0010))) continue;
            auto fail = [&](const char* why) -> Status {
                return Status(Errc::Malformed, std::string("overlay: ") + why, 0, ot(g, 0x0010));
            };
            Overlay ov;
            ov.group = g;
            const auto rows = get_i16(ds, ot(g, 0x0010), 0, false);
            const auto cols = get_i16(ds, ot(g, 0x0011), 0, false);
            const auto ba = get_i16(ds, ot(g, 0x0100), 0, false);
            if (!rows || !cols || *rows <= 0 || *cols <= 0) {
                if (!lenient) return fail("rows/columns missing or zero");
                continue;
            }
            ov.rows = static_cast<std::uint32_t>(*rows);
            ov.columns = static_cast<std::uint32_t>(*cols);
            const auto r0 = get_i16(ds, ot(g, 0x0050), 0, true);
            const auto c0 = get_i16(ds, ot(g, 0x0050), 1, true);
            if (r0 && c0) {
                ov.origin_row = static_cast<std::int32_t>(*r0);
                ov.origin_column = static_cast<std::int32_t>(*c0);
            } else if (!lenient) {
                return fail("Overlay Origin missing");
            }
            if (auto t = ds.get_string(ot(g, 0x0040)); t && !t->empty()) ov.type = (*t)[0];
            if (auto s = ds.get_string(ot(g, 0x0022))) ov.description = *s;
            if (auto s = ds.get_string(ot(g, 0x1500))) ov.label = *s;
            if (auto s = ds.get_string(ot(g, 0x0045))) ov.subtype = *s;
            if (ds.contains(ot(g, 0x0015))) {
                const auto nf = ds.get_int(ot(g, 0x0015));
                if (!nf || *nf < 1 || *nf > 0x7FFFFFFF) {
                    if (!lenient) return fail("Number of Frames in Overlay invalid");
                    continue;
                }
                ov.multi_frame = true;
                ov.frames = static_cast<std::uint32_t>(*nf);
                if (auto o = get_i16(ds, ot(g, 0x0051), 0, false); o && *o >= 1) ov.image_frame_origin = static_cast<std::uint32_t>(*o);
            }
            const std::uint64_t bits_per_frame = std::uint64_t{ov.rows} * ov.columns;
            if (has_data) {
                ov.data = ds.find(ot(g, 0x3000))->value;
                if (ba && *ba != 1 && !lenient) return fail("Overlay Bits Allocated must be 1 for Overlay Data");
                const std::uint64_t have_frames = ov.data.size() * 8 / bits_per_frame;
                if (have_frames < ov.frames) {
                    if (!lenient || have_frames == 0) {
                        if (!lenient) return fail("Overlay Data shorter than its rows, columns and frames");
                        continue;
                    }
                    ov.frames = static_cast<std::uint32_t>(have_frames);
                }
            } else {
                // Retired embedded overlay (PS3.3-2004): a bit of each Pixel Data container.
                const auto bp = get_i16(ds, ot(g, 0x0102), 0, false);
                if (!bp || pi.bits_allocated < 8 || *bp >= pi.bits_allocated || pi.samples_per_pixel != 1 ||
                    pi.kind != SampleKind::Integer || ov.rows != pi.rows || ov.columns != pi.columns) {
                    if (!lenient) return fail("embedded overlay does not match the pixel data");
                    continue;
                }
                if (*bp <= pi.high_bit && *bp >= static_cast<std::int64_t>(pi.low_bit())) {
                    if (!lenient) return fail("embedded overlay bit lies inside the stored pixel bits");
                    continue;
                }
                ov.embedded = true;
                ov.bit_position = static_cast<std::uint16_t>(*bp);
            }
            out.push_back(std::move(ov));
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "overlay: out of memory");
    }
    return out;
}

Status overlay_mask(const Overlay& ov, const PixelInfo& pi, std::uint32_t frame, std::span<const std::uint8_t> frame_pixels,
                    std::span<std::uint8_t> mask) {
    if (mask.size() < pi.pixels()) return Status(Errc::BufferTooSmall, "overlay: mask too small");
    const auto of = ov.overlay_frame(frame);
    if (!of) return Status::ok();
    if (ov.embedded) {
        if (frame_pixels.size() < pi.native_frame_bytes()) return Status(Errc::Truncated, "overlay: frame too short");
        const unsigned bytes = pi.bits_allocated / 8u;
        for (std::size_t i = 0; i < pi.pixels(); ++i) {
            const std::uint8_t* p = frame_pixels.data() + i * bytes;
            const unsigned byte = ov.bit_position / 8u, bit = ov.bit_position % 8u;
            if ((static_cast<unsigned>(p[byte]) >> bit) & 1u) mask[i] = 1;
        }
        return Status::ok();
    }
    const std::uint64_t base = std::uint64_t{*of} * ov.rows * ov.columns;
    const std::uint8_t* bits = ov.data.data();
    const std::int64_t r_off = static_cast<std::int64_t>(ov.origin_row) - 1;
    const std::int64_t c_off = static_cast<std::int64_t>(ov.origin_column) - 1;
    // Visible part of the overlay in overlay coordinates.
    const std::int64_t r_lo = std::max<std::int64_t>(0, -r_off);
    const std::int64_t r_hi = std::min<std::int64_t>(ov.rows, static_cast<std::int64_t>(pi.rows) - r_off);
    const std::int64_t c_lo = std::max<std::int64_t>(0, -c_off);
    const std::int64_t c_hi = std::min<std::int64_t>(ov.columns, static_cast<std::int64_t>(pi.columns) - c_off);
    for (std::int64_t r = r_lo; r < r_hi; ++r) {
        std::uint64_t bit = base + static_cast<std::uint64_t>(r) * ov.columns + static_cast<std::uint64_t>(c_lo);
        std::uint8_t* m = mask.data() + static_cast<std::uint64_t>(r + r_off) * pi.columns + static_cast<std::uint64_t>(c_lo + c_off);
        for (std::int64_t c = c_lo; c < c_hi; ++c, ++bit, ++m)
            if ((static_cast<unsigned>(bits[bit >> 3]) >> (bit & 7u)) & 1u) *m = 1;
    }
    return Status::ok();
}

// ---------------------------------------------------------------- shutters

namespace {
constexpr Tag kShutterShape = 0x00181600;
constexpr Tag kLeft = 0x00181602, kRight = 0x00181604, kUpper = 0x00181606, kLower = 0x00181608;
constexpr Tag kCenter = 0x00181610, kRadius = 0x00181612, kVertices = 0x00181620;
constexpr Tag kPresentationValue = 0x00181622, kOverlayGroup = 0x00181623, kLab = 0x00181624;
constexpr std::size_t kMaxVertices = 4096;  // R-084
}  // namespace

Result<Shutters> read_shutters(const Dataset& ds, bool lenient) {
    Shutters sh;
    const std::size_t n = ds.value_multiplicity(kShutterShape);
    for (std::size_t i = 0; i < n; ++i) {
        auto s = ds.get_string(kShutterShape, i);
        if (!s) continue;
        if (*s == "RECTANGULAR") sh.rectangular = true;
        else if (*s == "CIRCULAR") sh.circular = true;
        else if (*s == "POLYGONAL") sh.polygonal = true;
        else if (*s == "BITMAP") sh.bitmap = true;
        else if (!lenient) return Status(Errc::Malformed, "shutter: unknown shape " + *s, 0, kShutterShape);
    }
    auto drop = [&](bool& flag, const char* why) -> Status {
        if (!lenient) return Status(Errc::Malformed, std::string("shutter: ") + why, 0, kShutterShape);
        flag = false;
        return Status::ok();
    };
    if (sh.rectangular) {
        auto l = ds.get_int(kLeft), r = ds.get_int(kRight), u = ds.get_int(kUpper), lo = ds.get_int(kLower);
        if (!l || !r || !u || !lo || std::llabs(*l) > 1000000 || std::llabs(*r) > 1000000 || std::llabs(*u) > 1000000 ||
            std::llabs(*lo) > 1000000) {
            if (Status s = drop(sh.rectangular, "rectangular edges missing"); !s) return s;
        } else {
            sh.left = static_cast<std::int32_t>(*l);
            sh.right = static_cast<std::int32_t>(*r);
            sh.upper = static_cast<std::int32_t>(*u);
            sh.lower = static_cast<std::int32_t>(*lo);
        }
    }
    if (sh.circular) {
        auto cr = ds.get_double(kCenter, 0), cc = ds.get_double(kCenter, 1), rad = ds.get_double(kRadius);
        if (!cr || !cc || !rad || !std::isfinite(*cr) || !std::isfinite(*cc) || !std::isfinite(*rad) || *rad < 0) {
            if (Status s = drop(sh.circular, "circular shutter attributes missing"); !s) return s;
        } else {
            sh.center_row = *cr;
            sh.center_column = *cc;
            sh.radius = *rad;
        }
    }
    if (sh.polygonal) {
        const std::size_t vm = ds.value_multiplicity(kVertices);
        if (vm < 6 || vm % 2 != 0 || vm / 2 > kMaxVertices) {
            if (Status s = drop(sh.polygonal, vm / 2 > kMaxVertices ? "polygonal shutter has more than 4096 vertices"
                                                                     : "polygonal shutter needs at least three vertices");
                !s)
                return s;
        } else {
            try {
                sh.vertices.reserve(vm / 2);
            } catch (const std::bad_alloc&) {
                return Status(Errc::OutOfMemory, "shutter: out of memory");
            }
            for (std::size_t i = 0; i + 1 < vm; i += 2) {
                auto r = ds.get_double(kVertices, i), c = ds.get_double(kVertices, i + 1);
                if (!r || !c || !std::isfinite(*r) || !std::isfinite(*c)) {
                    sh.vertices.clear();
                    if (Status s = drop(sh.polygonal, "polygonal shutter vertex unreadable"); !s) return s;
                    break;
                }
                sh.vertices.emplace_back(*r, *c);
            }
        }
    }
    if (sh.bitmap) {
        auto g = ds.get_int(kOverlayGroup);
        if (!g || *g < 0x6000 || *g > 0x601E || (*g & 1)) {
            if (Status s = drop(sh.bitmap, "Shutter Overlay Group invalid"); !s) return s;
        } else {
            sh.overlay_group = static_cast<std::uint16_t>(*g);
        }
    }
    if (auto v = ds.get_int(kPresentationValue); v && *v >= 0 && *v <= 0xFFFF)
        sh.presentation_value = static_cast<std::uint16_t>(*v);
    if (ds.value_multiplicity(kLab) >= 3) {
        auto l = ds.get_int(kLab, 0), a = ds.get_int(kLab, 1), b = ds.get_int(kLab, 2);
        if (l && a && b && *l >= 0 && *l <= 0xFFFF && *a >= 0 && *a <= 0xFFFF && *b >= 0 && *b <= 0xFFFF)
            sh.presentation_lab = std::array<std::uint16_t, 3>{static_cast<std::uint16_t>(*l), static_cast<std::uint16_t>(*a),
                                                               static_cast<std::uint16_t>(*b)};
    }
    return sh;
}

namespace {

// Pixels inside the polygon (row coordinate y = 1..rows, column x = 1..cols, centres at integer
// coordinates), even-odd rule, boundary pixels included. An edge-table scan (R-084): cost is the
// edges plus the active edges per row, not rows x vertices, charged to `budget` with the pixels
// marked; false when the budget ran out. All double -> integer conversions are clamped (R-083).
bool polygon_inside(const std::vector<std::pair<double, double>>& v, std::uint32_t rows, std::uint32_t cols,
                    std::vector<std::uint8_t>& in, raster::WorkBudget& budget) {
    const std::size_t n = v.size();
    std::vector<raster::Edge> edges;
    edges.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = v[i];
        const auto& b = v[(i + 1) % n];
        edges.push_back({a.second, a.first, b.second, b.first});
    }
    const double dc = static_cast<double>(cols);
    bool ok = raster::scan_polygon(std::move(edges), 0, static_cast<std::int64_t>(rows) - 1, 1.0, budget,
                                   [&](std::int64_t r, const std::vector<double>& xs) {
                                       for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                                           const std::int64_t c_lo = std::max<std::int64_t>(1, raster::clamp_to_i64(std::ceil(xs[k]), 0.0, dc + 1.0));
                                           const std::int64_t c_hi = std::min<std::int64_t>(static_cast<std::int64_t>(cols), raster::clamp_to_i64(std::floor(xs[k + 1]), 0.0, dc + 1.0));
                                           if (c_hi < c_lo || !budget.charge(static_cast<std::uint64_t>(c_hi - c_lo + 1))) continue;
                                           for (std::int64_t c = c_lo; c <= c_hi; ++c)
                                               in[static_cast<std::size_t>(r) * cols + static_cast<std::size_t>(c) - 1] = 1;
                                       }
                                   });
    // Boundary pixels count as inside: mark them by stepping along each (clipped) edge.
    for (std::size_t i = 0; ok && i < n; ++i) {
        double r0 = v[i].first, c0 = v[i].second, r1 = v[(i + 1) % n].first, c1 = v[(i + 1) % n].second;
        if (!raster::clip_segment(c0, r0, c1, r1, 0.5, cols + 0.5, 0.5, rows + 0.5)) continue;
        const double steps = std::ceil(std::max(std::fabs(r1 - r0), std::fabs(c1 - c0)) * 2) + 1;
        if (!budget.charge(static_cast<std::uint64_t>(steps) + 1)) {
            ok = false;
            break;
        }
        for (double st = 0; st <= steps; st += 1.0) {
            const double t = steps > 0 ? st / steps : 0;
            const std::int64_t rr = raster::clamp_to_i64(std::floor(r0 + (r1 - r0) * t + 0.5), 0.0, rows + 1.0);
            const std::int64_t cc = raster::clamp_to_i64(std::floor(c0 + (c1 - c0) * t + 0.5), 0.0, cols + 1.0);
            if (rr >= 1 && rr <= rows && cc >= 1 && cc <= cols)
                in[static_cast<std::size_t>(rr - 1) * cols + static_cast<std::size_t>(cc - 1)] = 1;
        }
    }
    return ok && !budget.exceeded;
}

}  // namespace

Status shutter_mask(const Shutters& sh, const PixelInfo& pi, std::uint32_t frame, const std::vector<Overlay>& overlays,
                    std::span<std::uint8_t> visible) {
    const std::uint64_t n = pi.pixels();
    if (visible.size() < n) return Status(Errc::BufferTooSmall, "shutter: mask too small");
    std::fill(visible.begin(), visible.begin() + static_cast<std::ptrdiff_t>(n), std::uint8_t{1});
    const std::uint32_t rows = pi.rows, cols = pi.columns;
    if (sh.rectangular) {
        for (std::uint32_t r = 0; r < rows; ++r) {
            const std::int64_t row = r + 1;
            for (std::uint32_t c = 0; c < cols; ++c) {
                const std::int64_t col = c + 1;
                if (col < sh.left || col > sh.right || row < sh.upper || row > sh.lower) visible[std::size_t{r} * cols + c] = 0;
            }
        }
    }
    if (sh.circular) {
        const double r2 = sh.radius * sh.radius;
        for (std::uint32_t r = 0; r < rows; ++r) {
            const double dr = (r + 1.0) - sh.center_row;
            for (std::uint32_t c = 0; c < cols; ++c) {
                const double dc = (c + 1.0) - sh.center_column;
                if (dr * dr + dc * dc > r2) visible[std::size_t{r} * cols + c] = 0;
            }
        }
    }
    try {
        if (sh.polygonal && sh.vertices.size() >= 3) {
            std::vector<std::uint8_t> inside(static_cast<std::size_t>(n), 0);
            raster::WorkBudget budget;
            budget.remaining = std::max<std::uint64_t>(std::uint64_t{1} << 26, 4 * n);
            if (!polygon_inside(sh.vertices, rows, cols, inside, budget))
                return Status(Errc::LimitExceeded, "shutter: polygon exceeds the work budget");
            for (std::size_t i = 0; i < n; ++i)
                if (!inside[i]) visible[i] = 0;
        }
        if (sh.bitmap) {
            for (const Overlay& ov : overlays) {
                if (ov.group != sh.overlay_group) continue;
                std::vector<std::uint8_t> m(static_cast<std::size_t>(n), 0);
                if (Status s = overlay_mask(ov, pi, frame, {}, m); !s) return s;
                for (std::size_t i = 0; i < n; ++i)
                    if (m[i]) visible[i] = 0;
            }
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "shutter: out of memory");
    }
    return Status::ok();
}

std::array<std::uint8_t, 3> cielab_to_srgb(std::uint16_t L16, std::uint16_t a16, std::uint16_t b16) noexcept {
    // PS3.3 C.10.7.1.1: L* = 0..100 and a*, b* = -128..127 scaled linearly to 0..FFFF; PCS white D50.
    const double L = L16 * 100.0 / 65535.0;
    const double a = a16 * 255.0 / 65535.0 - 128.0;
    const double b = b16 * 255.0 / 65535.0 - 128.0;
    const double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto finv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double X = 0.9642 * finv(fx), Y = finv(fy), Z = 0.8249 * finv(fz);
    // D50 -> linear sRGB (D65) with Bradford adaptation (combined matrix).
    double rgb[3] = {3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z, -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                     0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z};
    std::array<std::uint8_t, 3> out{};
    for (int i = 0; i < 3; ++i) {
        double v = std::clamp(rgb[i], 0.0, 1.0);
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::floor(v * 255 + 0.5));
    }
    return out;
}

std::array<std::uint8_t, 3> shutter_rgb(const Shutters& sh) noexcept {
    if (sh.presentation_lab) return cielab_to_srgb((*sh.presentation_lab)[0], (*sh.presentation_lab)[1], (*sh.presentation_lab)[2]);
    if (sh.presentation_value) {
        const auto g = static_cast<std::uint8_t>((std::uint32_t{*sh.presentation_value} * 255u + 32767u) / 65535u);
        return {g, g, g};
    }
    return {0, 0, 0};
}

}  // namespace siaara::image
