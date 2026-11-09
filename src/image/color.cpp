// SIAARA DICOM Toolkit - colour models and palettes. Written from DICOM PS3.3 C.7.6.3.1.2,
// C.7.6.3.1.5, C.7.6.3.1.6, C.7.9.2 and PS3.6 Annex B.
#include "image/color.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <new>

#include "image/samples.hpp"

namespace siaara::image {

namespace {

#include "image/palettes_data.inc"

using Mat3 = std::array<std::array<double, 3>, 3>;

constexpr Mat3 inverse(const Mat3& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    Mat3 r{};
    r[0] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det};
    r[1] = {B / det, (a * i - c * g) / det, -(a * f - c * d) / det};
    r[2] = {C / det, -(a * h - b * g) / det, (a * e - b * d) / det};
    return r;
}

// Forward equations exactly as printed in PS3.3 C.7.6.3.1.2 (rows: Y, CB, CR; columns: R, G, B).
constexpr Mat3 kFullFwd{{{0.2990, 0.5870, 0.1140}, {-0.1687, -0.3313, 0.5000}, {0.5000, -0.4187, -0.0813}}};
constexpr Mat3 kPartialFwd{{{0.2568, 0.5041, 0.0979}, {-0.1482, -0.2910, 0.4392}, {0.4392, -0.3678, -0.0714}}};
constexpr Mat3 kIctFwd{{{0.29900, 0.58700, 0.11400}, {-0.16875, -0.33126, 0.50000}, {0.50000, -0.41869, -0.08131}}};
constexpr Mat3 kFullInv = inverse(kFullFwd);
constexpr Mat3 kPartialInv = inverse(kPartialFwd);
constexpr Mat3 kIctInv = inverse(kIctFwd);

std::uint64_t load_sample(const std::uint8_t* p, unsigned bps) noexcept {
    std::uint64_t v = 0;
    for (unsigned k = bps; k-- > 0;) v = (v << 8) | p[k];
    return v;
}
void store_sample(std::uint8_t* p, unsigned bps, std::uint64_t v) noexcept {
    for (unsigned k = 0; k < bps; ++k) p[k] = static_cast<std::uint8_t>(v >> (8 * k));
}

Status check_sizes(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint64_t pixels,
                   unsigned samples, unsigned bps) {
    std::uint64_t n = 0;
    if (!checked_mul(pixels, samples, n) || !checked_mul(n, bps, n))
        return Status(Errc::LimitExceeded, "color: size overflows");
    if (in.size() < n) return Status(Errc::Truncated, "color: input too short");
    if (out.size() < n) return Status(Errc::BufferTooSmall, "color: output too small");
    return Status::ok();
}

}  // namespace

std::uint64_t rgb_frame_bytes(const PixelInfo& pi) noexcept {
    std::uint64_t n = 0;
    const unsigned bps = pi.bits_allocated / 8u;
    if (!checked_mul(pi.pixels(), 3u * bps, n)) return 0;
    return n;
}

Status planar_to_interleaved(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint64_t pixels,
                             unsigned samples, unsigned bps) {
    if (Status s = check_sizes(in, out, pixels, samples, bps); !s) return s;
    const auto np = static_cast<std::size_t>(pixels);
    if (bps == 1) {
        for (unsigned s = 0; s < samples; ++s) {
            const std::uint8_t* src = in.data() + s * np;
            std::uint8_t* dst = out.data() + s;
            for (std::size_t i = 0; i < np; ++i) dst[i * samples] = src[i];
        }
        return Status::ok();
    }
    for (unsigned s = 0; s < samples; ++s)
        for (std::size_t i = 0; i < np; ++i)
            std::memcpy(out.data() + (i * samples + s) * bps, in.data() + (s * np + i) * bps, bps);
    return Status::ok();
}

Status interleaved_to_planar(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint64_t pixels,
                             unsigned samples, unsigned bps) {
    if (Status s = check_sizes(in, out, pixels, samples, bps); !s) return s;
    const auto np = static_cast<std::size_t>(pixels);
    for (unsigned s = 0; s < samples; ++s)
        for (std::size_t i = 0; i < np; ++i)
            std::memcpy(out.data() + (s * np + i) * bps, in.data() + (i * samples + s) * bps, bps);
    return Status::ok();
}

Status expand_422(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint32_t rows,
                  std::uint32_t columns, unsigned bps, ChromaUpsampling up) {
    if (columns % 2 != 0) return Status(Errc::Malformed, "color: 4:2:2 data needs an even number of columns");
    const std::uint64_t pixels = std::uint64_t{rows} * columns;
    if (in.size() < pixels * 2 * bps) return Status(Errc::Truncated, "color: 4:2:2 input too short");
    if (out.size() < pixels * 3 * bps) return Status(Errc::BufferTooSmall, "color: output too small");
    const std::uint32_t pairs = columns / 2;
    for (std::uint32_t r = 0; r < rows; ++r) {
        const std::uint8_t* src = in.data() + std::uint64_t{r} * columns * 2 * bps;
        std::uint8_t* dst = out.data() + std::uint64_t{r} * columns * 3 * bps;
        for (std::uint32_t k = 0; k < pairs; ++k) {
            const std::uint8_t* q = src + std::uint64_t{k} * 4 * bps;  // Y1 Y2 Cb Cr
            const std::uint64_t y1 = load_sample(q, bps), y2 = load_sample(q + bps, bps);
            const std::uint64_t cb = load_sample(q + 2 * bps, bps), cr = load_sample(q + 3 * bps, bps);
            std::uint64_t cb2 = cb, cr2 = cr;
            if (up == ChromaUpsampling::Linear && k + 1 < pairs) {
                const std::uint8_t* n = q + 4 * bps;
                cb2 = (cb + load_sample(n + 2 * bps, bps) + 1) / 2;
                cr2 = (cr + load_sample(n + 3 * bps, bps) + 1) / 2;
            }
            std::uint8_t* d = dst + std::uint64_t{k} * 6 * bps;
            store_sample(d, bps, y1);
            store_sample(d + bps, bps, cb);
            store_sample(d + 2 * bps, bps, cr);
            store_sample(d + 3 * bps, bps, y2);
            store_sample(d + 4 * bps, bps, cb2);
            store_sample(d + 5 * bps, bps, cr2);
        }
    }
    return Status::ok();
}

Status to_interleaved_full(const PixelInfo& pi, std::span<const std::uint8_t> frame, std::span<std::uint8_t> out,
                           const ColorOptions& opt) {
    if (pi.samples_per_pixel != 3 || pi.bits_allocated < 8 || pi.kind != SampleKind::Integer)
        return Status(Errc::Unsupported, "color: expected 3 integer samples per pixel of 8 bits or more");
    const unsigned bps = pi.bits_allocated / 8u;
    const std::uint64_t need = rgb_frame_bytes(pi);
    if (need == 0) return Status(Errc::LimitExceeded, "color: size overflows");
    if (out.size() < need) return Status(Errc::BufferTooSmall, "color: output too small");
    if (opt.native_layout && pi.native_subsampled())
        return expand_422(frame, out, pi.rows, pi.columns, bps, opt.upsampling);
    if (pi.planar) return planar_to_interleaved(frame, out, pi.pixels(), 3, bps);
    if (frame.size() < need) return Status(Errc::Truncated, "color: frame too short");
    std::memcpy(out.data(), frame.data(), static_cast<std::size_t>(need));
    return Status::ok();
}

Status to_rgb(const PixelInfo& pi, std::span<const std::uint8_t> frame, std::span<std::uint8_t> out,
              const ColorOptions& opt) {
    Photometric p = pi.photometric;
    if ((p == Photometric::YbrIct || p == Photometric::YbrRct) && opt.codec_applied_mct) p = Photometric::Rgb;
    switch (p) {
        case Photometric::Rgb: case Photometric::YbrFull: case Photometric::YbrFull422:
        case Photometric::YbrPartial422: case Photometric::YbrPartial420: case Photometric::YbrIct:
        case Photometric::YbrRct:
            break;
        default:
            return Status(Errc::Unsupported, "color: no RGB conversion for " + pi.photometric_text);
    }
    if (Status s = to_interleaved_full(pi, frame, out, opt); !s) return s;
    if (p == Photometric::Rgb) {
        if (pi.low_bit() == 0 && pi.bits_stored == pi.bits_allocated) return Status::ok();
    }
    const unsigned bps = pi.bits_allocated / 8u;
    const unsigned bs = pi.bits_stored;
    const unsigned shift = pi.low_bit();
    const std::uint64_t mask = bs >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bs) - 1u;
    const double maxv = static_cast<double>(mask);
    const double half = std::ldexp(1.0, static_cast<int>(bs) - 1);   // "half full scale"
    const double scale8 = std::ldexp(1.0, static_cast<int>(bs) - 8); // partial ranges are given for 8 bits
    const std::uint64_t n = pi.pixels();
    std::uint8_t* d = out.data();
    auto clamp_round = [&](double v) -> std::uint64_t {
        if (!(v > 0.0)) return 0;  // also NaN
        if (v >= maxv) return mask;
        return static_cast<std::uint64_t>(std::floor(v + 0.5));
    };
    auto sext = [&](std::uint64_t v) -> double {  // two's complement chroma (YBR_ICT/RCT with PR=1)
        if (v & (std::uint64_t{1} << (bs - 1))) return static_cast<double>(static_cast<std::int64_t>(v | ~mask));
        return static_cast<double>(v);
    };
    for (std::uint64_t i = 0; i < n; ++i, d += 3 * bps) {
        const std::uint64_t s0 = (load_sample(d, bps) >> shift) & mask;
        const std::uint64_t s1 = (load_sample(d + bps, bps) >> shift) & mask;
        const std::uint64_t s2 = (load_sample(d + 2 * bps, bps) >> shift) & mask;
        std::uint64_t r = s0, g = s1, b = s2;
        switch (p) {
            case Photometric::YbrFull: case Photometric::YbrFull422: {
                const double y = static_cast<double>(s0), cb = static_cast<double>(s1) - half,
                             cr = static_cast<double>(s2) - half;
                r = clamp_round(kFullInv[0][0] * y + kFullInv[0][1] * cb + kFullInv[0][2] * cr);
                g = clamp_round(kFullInv[1][0] * y + kFullInv[1][1] * cb + kFullInv[1][2] * cr);
                b = clamp_round(kFullInv[2][0] * y + kFullInv[2][1] * cb + kFullInv[2][2] * cr);
                break;
            }
            case Photometric::YbrPartial422: case Photometric::YbrPartial420: {
                const double y = static_cast<double>(s0) - 16.0 * scale8, cb = static_cast<double>(s1) - half,
                             cr = static_cast<double>(s2) - half;
                r = clamp_round(kPartialInv[0][0] * y + kPartialInv[0][1] * cb + kPartialInv[0][2] * cr);
                g = clamp_round(kPartialInv[1][0] * y + kPartialInv[1][1] * cb + kPartialInv[1][2] * cr);
                b = clamp_round(kPartialInv[2][0] * y + kPartialInv[2][1] * cb + kPartialInv[2][2] * cr);
                break;
            }
            case Photometric::YbrIct: {
                // Chroma centred on zero (PS3.3): signed samples, or offset by half scale when unsigned.
                const double y = pi.is_signed ? sext(s0) + half : static_cast<double>(s0);
                const double cb = pi.is_signed ? sext(s1) : static_cast<double>(s1) - half;
                const double cr = pi.is_signed ? sext(s2) : static_cast<double>(s2) - half;
                r = clamp_round(kIctInv[0][0] * y + kIctInv[0][1] * cb + kIctInv[0][2] * cr);
                g = clamp_round(kIctInv[1][0] * y + kIctInv[1][1] * cb + kIctInv[1][2] * cr);
                b = clamp_round(kIctInv[2][0] * y + kIctInv[2][1] * cb + kIctInv[2][2] * cr);
                break;
            }
            case Photometric::YbrRct: {
                // ISO/IEC 15444-1 G.2: G = Y0 - floor((Y1 + Y2) / 4), R = Y2 + G, B = Y1 + G.
                const auto y0 = static_cast<std::int64_t>(pi.is_signed ? sext(s0) + half : static_cast<double>(s0));
                const auto y1 = static_cast<std::int64_t>(pi.is_signed ? sext(s1) : static_cast<double>(s1) - half);
                const auto y2 = static_cast<std::int64_t>(pi.is_signed ? sext(s2) : static_cast<double>(s2) - half);
                const std::int64_t sum = y1 + y2;
                const std::int64_t gg = y0 - (sum >= 0 ? sum / 4 : -((-sum + 3) / 4));
                r = clamp_round(static_cast<double>(y2 + gg));
                g = clamp_round(static_cast<double>(gg));
                b = clamp_round(static_cast<double>(y1 + gg));
                break;
            }
            default:
                break;  // RGB: only the low-bit normalisation
        }
        store_sample(d, bps, r);
        store_sample(d + bps, bps, g);
        store_sample(d + 2 * bps, bps, b);
    }
    return Status::ok();
}

// ---------------------------------------------------------------- palettes

namespace {

constexpr Tag kDescTag[4] = {0x00281101, 0x00281102, 0x00281103, 0x00281104};
constexpr Tag kDataTag[4] = {0x00281201, 0x00281202, 0x00281203, 0x00281204};
constexpr Tag kSegTag[3] = {0x00281221, 0x00281222, 0x00281223};

bool binary_vr(VR vr) noexcept {
    return vr == VR::US || vr == VR::SS || vr == VR::OW || vr == VR::OB || vr == VR::UN || vr == VR::None;
}

struct Desc {
    std::uint32_t entries;
    std::int32_t first;
    std::uint16_t bits;
};

Result<Desc> read_desc(const Dataset& ds, Tag t, bool input_signed) {
    const Element* e = ds.find(t);
    if (!e) return Status(Errc::NotFound, "palette: descriptor missing", 0, t);
    if (!binary_vr(e->vr) || e->value.size() < 6) return Status(Errc::Malformed, "palette: bad descriptor", 0, t);
    const std::uint8_t* p = e->value.data();
    Desc d;
    const std::uint16_t n = load_le16(p);
    d.entries = n == 0 ? 65536u : n;
    const std::uint16_t f = load_le16(p + 2);
    d.first = input_signed ? static_cast<std::int16_t>(f) : static_cast<std::int32_t>(f);
    d.bits = load_le16(p + 4);
    return d;
}

Status read_data(const Dataset& ds, Tag t, const Desc& d, bool lenient, std::vector<std::uint16_t>& out) {
    const Element* e = ds.find(t);
    if (!e) return Status(Errc::NotFound, "palette: data missing", 0, t);
    if (!binary_vr(e->vr)) return Status(Errc::Malformed, "palette: data has a non-binary VR", 0, t);
    const std::size_t len = e->value.size();
    const std::uint8_t* p = e->value.data();
    out.resize(d.entries);
    if (d.bits == 8) {
        if (len >= std::size_t{d.entries} * 2) {
            // 8-bit entries encoded with 16 bits allocated (PS3.3 C.7.6.3.1.5 note): low byte holds the value.
            for (std::uint32_t i = 0; i < d.entries; ++i) out[i] = static_cast<std::uint16_t>(load_le16(p + 2 * i) & 0xFFu);
            return Status::ok();
        }
        if (len >= d.entries) {
            for (std::uint32_t i = 0; i < d.entries; ++i) out[i] = p[i];
            return Status::ok();
        }
        return Status(Errc::Malformed, "palette: data shorter than the descriptor says", 0, t);
    }
    if (len < std::size_t{d.entries} * 2) {
        if (!lenient || len < d.entries) return Status(Errc::Malformed, "palette: data shorter than the descriptor says", 0, t);
        // 16 in the descriptor but one byte per entry: replicate into both bytes (C.7.6.3.1.6 scaling).
        for (std::uint32_t i = 0; i < d.entries; ++i) out[i] = static_cast<std::uint16_t>(p[i] * 257u);
        return Status::ok();
    }
    for (std::uint32_t i = 0; i < d.entries; ++i) out[i] = load_le16(p + 2 * i);
    return Status::ok();
}

}  // namespace

namespace {

// Expands the discrete or linear segment at word index i (PS3.3 C.7.9.2); `next` receives the
// index after it. Indirect segments are refused here: they may not point to another indirect one.
Status expand_one(std::span<const std::uint16_t> words, std::size_t i, std::uint32_t entries,
                  std::vector<std::uint16_t>& out, std::uint64_t& work, std::size_t& next) {
    constexpr std::uint64_t kMaxWork = 1u << 24;
    if (i >= words.size() || words.size() - i < 2) return Status(Errc::Malformed, "palette: segment header truncated");
    const std::uint16_t op = words[i];
    const std::uint16_t len = words[i + 1];
    work += 1u + len;
    if (work > kMaxWork) return Status(Errc::LimitExceeded, "palette: segmented data too complex");
    if (op == 0) {  // discrete: `len` entries follow
        if (words.size() - i - 2 < len) return Status(Errc::Malformed, "palette: discrete segment truncated");
        if (out.size() + len > entries) return Status(Errc::Malformed, "palette: segments exceed the entry count");
        out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i + 2),
                   words.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
        next = i + 2 + len;
        return Status::ok();
    }
    if (op == 1) {  // linear: from the previous end point (X0, Y0) to (X0 + len, Y1)
        if (words.size() - i < 3) return Status(Errc::Malformed, "palette: linear segment truncated");
        if (out.empty()) return Status(Errc::Malformed, "palette: a linear segment cannot be first");
        if (out.size() + len > entries) return Status(Errc::Malformed, "palette: segments exceed the entry count");
        const double y0 = out.back(), y1 = words[i + 2];
        for (std::uint32_t k = 1; k <= len; ++k)
            out.push_back(static_cast<std::uint16_t>(std::floor(y0 + (y1 - y0) * k / len + 0.5)));
        next = i + 3;
        return Status::ok();
    }
    if (op == 2) return Status(Errc::Malformed, "palette: indirect segment points to an indirect segment");
    return Status(Errc::Malformed, "palette: reserved segment opcode " + std::to_string(op));
}

}  // namespace

Status expand_segmented(std::span<const std::uint16_t> words, std::uint32_t entries, std::vector<std::uint16_t>& out) {
    out.clear();
    try {
        out.reserve(entries);
        std::uint64_t work = 0;
        std::size_t i = 0;
        while (i < words.size() && out.size() < entries) {
            std::size_t next = 0;
            if (words[i] == 2) {
                // indirect: copy `count` segments starting at a byte offset from the table start
                if (words.size() - i < 4) return Status(Errc::Malformed, "palette: indirect segment truncated");
                const std::uint16_t count = words[i + 1];
                const std::uint32_t off = std::uint32_t{words[i + 2]} | (std::uint32_t{words[i + 3]} << 16);
                if (off % 2 != 0) return Status(Errc::Malformed, "palette: indirect offset is odd");
                std::size_t j = off / 2u;
                for (std::uint32_t s = 0; s < count; ++s) {
                    if (Status st = expand_one(words, j, entries, out, work, next); !st) return st;
                    j = next;
                }
                i += 4;
                continue;
            }
            if (Status st = expand_one(words, i, entries, out, work, next); !st) return st;
            i = next;
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "palette: out of memory");
    }
    if (out.size() != entries)
        return Status(Errc::Malformed, "palette: segmented data expands to " + std::to_string(out.size()) +
                                           " entries, descriptor says " + std::to_string(entries));
    return Status::ok();
}

Result<PaletteLut> read_palette(const Dataset& ds, bool input_signed, bool lenient) {
    PaletteLut lut;
    Desc d[3];
    for (int c = 0; c < 3; ++c) {
        auto r = read_desc(ds, kDescTag[c], input_signed);
        if (!r) return r.take_error();
        d[c] = *r;
        if (d[c].bits != 8 && d[c].bits != 16) {
            if (!lenient) return Status(Errc::Malformed, "palette: entry bits must be 8 or 16", 0, kDescTag[c]);
            return Status(Errc::Unsupported, "palette: entry bits " + std::to_string(d[c].bits), 0, kDescTag[c]);
        }
    }
    for (int c = 1; c < 3; ++c) {
        if (d[c].entries != d[0].entries || d[c].first != d[0].first || d[c].bits != d[0].bits) {
            if (!lenient) return Status(Errc::Malformed, "palette: red, green and blue descriptors differ", 0, kDescTag[c]);
            if (d[c].entries != d[0].entries || d[c].bits != d[0].bits)
                return Status(Errc::Malformed, "palette: red, green and blue descriptors differ", 0, kDescTag[c]);
        }
    }
    lut.entries = d[0].entries;
    lut.first_mapped = d[0].first;
    lut.bits = d[0].bits;
    std::vector<std::uint16_t>* dst[3] = {&lut.red, &lut.green, &lut.blue};
    try {
        const bool segmented = ds.contains(kSegTag[0]) && !ds.contains(kDataTag[0]);
        lut.segmented = segmented;
        for (int c = 0; c < 3; ++c) {
            if (segmented) {
                const Element* e = ds.find(kSegTag[c]);
                if (!e || !binary_vr(e->vr)) return Status(Errc::NotFound, "palette: segmented data missing", 0, kSegTag[c]);
                std::vector<std::uint16_t> words(e->value.size() / 2);
                for (std::size_t i = 0; i < words.size(); ++i) words[i] = load_le16(e->value.data() + 2 * i);
                if (Status s = expand_segmented(words, lut.entries, *dst[c]); !s) {
                    s.tag = kSegTag[c];
                    return s;
                }
                if (lut.bits == 8)
                    for (auto& v : *dst[c]) v = static_cast<std::uint16_t>(v & 0xFFu);
            } else if (Status s = read_data(ds, kDataTag[c], d[0], lenient, *dst[c]); !s) {
                return s;
            }
        }
        if (ds.contains(kDescTag[3]) && ds.contains(kDataTag[3])) {
            auto a = read_desc(ds, kDescTag[3], input_signed);
            if (a && a->entries == lut.entries && a->bits == 8) {
                if (Status s = read_data(ds, kDataTag[3], *a, lenient, lut.alpha); !s) lut.alpha.clear();
            }
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "palette: out of memory");
    }
    return lut;
}

Status apply_palette(const PixelInfo& pi, std::span<const std::uint8_t> frame, const PaletteLut& lut,
                     std::span<std::uint16_t> out) {
    if (pi.samples_per_pixel != 1 || pi.kind != SampleKind::Integer)
        return Status(Errc::InvalidArgument, "palette: needs one integer sample per pixel");
    if (lut.entries == 0 || lut.red.size() < lut.entries || lut.green.size() < lut.entries || lut.blue.size() < lut.entries)
        return Status(Errc::InvalidArgument, "palette: incomplete lookup table");
    const std::uint64_t n = pi.pixels();
    if (out.size() < n * 3) return Status(Errc::BufferTooSmall, "palette: output too small");
    if (frame.size() < pi.native_frame_bytes()) return Status(Errc::Truncated, "palette: frame too short");
    detail::SampleReader rd(pi);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint32_t k = lut.index(rd.value(frame.data(), i));
        out[3 * i] = lut.red[k];
        out[3 * i + 1] = lut.green[k];
        out[3 * i + 2] = lut.blue[k];
    }
    return Status::ok();
}

namespace {

struct StdPal {
    const std::uint16_t* desc;
    const std::uint8_t* r;
    const std::uint8_t* g;
    const std::uint8_t* b;
    std::string_view uid;
    std::string_view label;
};

const StdPal& std_pal(StandardPalette p) noexcept {
    static const StdPal pals[4] = {
        {kHotIronDescriptor, kHotIronRed, kHotIronGreen, kHotIronBlue, "1.2.840.10008.1.5.1", "HOT_IRON"},
        {kPetDescriptor, kPetRed, kPetGreen, kPetBlue, "1.2.840.10008.1.5.2", "PET"},
        {kHotMetalBlueDescriptor, kHotMetalBlueRed, kHotMetalBlueGreen, kHotMetalBlueBlue, "1.2.840.10008.1.5.3",
         "HOT_METAL_BLUE"},
        {kPet20StepDescriptor, kPet20StepRed, kPet20StepGreen, kPet20StepBlue, "1.2.840.10008.1.5.4", "PET_20_STEP"},
    };
    return pals[static_cast<int>(p)];
}

PaletteLut make_std(StandardPalette p) {
    const StdPal& s = std_pal(p);
    PaletteLut l;
    l.entries = s.desc[0] == 0 ? 65536u : s.desc[0];
    l.first_mapped = s.desc[1];
    l.bits = s.desc[2];
    l.red.assign(s.r, s.r + l.entries);
    l.green.assign(s.g, s.g + l.entries);
    l.blue.assign(s.b, s.b + l.entries);
    return l;
}

}  // namespace

const PaletteLut& standard_palette(StandardPalette p) {
    static const PaletteLut pals[4] = {make_std(StandardPalette::HotIron), make_std(StandardPalette::Pet),
                                       make_std(StandardPalette::HotMetalBlue), make_std(StandardPalette::Pet20Step)};
    return pals[static_cast<int>(p)];
}

std::string_view standard_palette_uid(StandardPalette p) noexcept { return std_pal(p).uid; }
std::string_view standard_palette_label(StandardPalette p) noexcept { return std_pal(p).label; }

}  // namespace siaara::image
