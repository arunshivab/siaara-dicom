// SIAARA DICOM Toolkit - grayscale transformations. Written from DICOM PS3.3 C.11.1, C.11.2,
// C.11.6 and the enhanced functional groups C.7.6.16.2.9 / C.7.6.16.2.10.
#include "image/lut.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <string>

#include "image/samples.hpp"

namespace siaara::image {

namespace {

bool binary_vr(VR vr) noexcept {
    return vr == VR::US || vr == VR::SS || vr == VR::OW || vr == VR::OB || vr == VR::UN || vr == VR::None;
}

const Dataset* first_item(const Dataset& ds, Tag seq) noexcept {
    const Element* e = ds.find(seq);
    if (!e || !e->is_sequence() || e->items.empty()) return nullptr;
    return &e->items.front();
}

// The functional group macro item `macro` for a frame: per-frame first, then shared.
const Dataset* functional_group(const Dataset& ds, std::uint32_t frame, Tag macro) noexcept {
    if (const Element* pf = ds.find(tags::PerFrameFunctionalGroupsSequence); pf && pf->is_sequence() &&
                                                                             frame < pf->items.size())
        if (const Dataset* m = first_item(pf->items[frame], macro)) return m;
    if (const Dataset* sh = first_item(ds, tags::SharedFunctionalGroupsSequence))
        if (const Dataset* m = first_item(*sh, macro)) return m;
    return nullptr;
}

Status read_rescale(const Dataset& src, ModalityTransform& m, bool lenient) {
    if (src.contains(siaara::tags::RescaleSlope)) {
        auto s = src.get_double(siaara::tags::RescaleSlope);
        if (!s || !std::isfinite(*s)) {
            if (!lenient) return Status(Errc::Malformed, "modality: Rescale Slope unreadable", 0, siaara::tags::RescaleSlope);
        } else {
            m.slope = *s;
        }
    }
    if (src.contains(siaara::tags::RescaleIntercept)) {
        auto i = src.get_double(siaara::tags::RescaleIntercept);
        if (!i || !std::isfinite(*i)) {
            if (!lenient)
                return Status(Errc::Malformed, "modality: Rescale Intercept unreadable", 0, siaara::tags::RescaleIntercept);
        } else {
            m.intercept = *i;
        }
    }
    if (auto t = src.get_string(tags::RescaleType)) m.rescale_type = *t;
    return Status::ok();
}

std::optional<VoiFunction> parse_function(std::string_view s) noexcept {
    if (s.empty() || s == "LINEAR") return VoiFunction::Linear;
    if (s == "LINEAR_EXACT") return VoiFunction::LinearExact;
    if (s == "SIGMOID") return VoiFunction::Sigmoid;
    return std::nullopt;
}

Status read_windows(const Dataset& src, VoiSet& out, bool lenient) {
    if (!src.contains(tags::WindowCenter) && !src.contains(tags::WindowWidth)) return Status::ok();
    const std::size_t nc = src.value_multiplicity(tags::WindowCenter);
    const std::size_t nw = src.value_multiplicity(tags::WindowWidth);
    if (nc != nw && !lenient)
        return Status(Errc::Malformed, "voi: Window Center and Window Width have different multiplicities", 0,
                      tags::WindowCenter);
    VoiFunction fn = VoiFunction::Linear;
    if (auto f = src.get_string(tags::VoiLutFunction)) {
        auto p = parse_function(*f);
        if (!p) {
            if (!lenient) return Status(Errc::Unsupported, "voi: unknown VOI LUT Function " + *f, 0, tags::VoiLutFunction);
        } else {
            fn = *p;
        }
    }
    const std::size_t n = std::min(nc, nw);
    for (std::size_t i = 0; i < n; ++i) {
        auto c = src.get_double(tags::WindowCenter, i);
        auto w = src.get_double(tags::WindowWidth, i);
        if (!c || !w || !window_valid(*c, *w, fn)) {
            if (!lenient) return Status(Errc::Malformed, "voi: invalid window " + std::to_string(i), 0, tags::WindowWidth);
            continue;
        }
        VoiWindow vw;
        vw.center = *c;
        vw.width = *w;
        vw.function = fn;
        if (auto ex = src.get_string(tags::WindowCenterWidthExplanation, i)) vw.explanation = *ex;
        out.windows.push_back(std::move(vw));
    }
    return Status::ok();
}

Status read_voi_luts(const Dataset& src, bool first_signed, VoiSet& out, bool lenient) {
    const Element* e = src.find(tags::VoiLutSequence);
    if (!e || !e->is_sequence()) return Status::ok();
    for (const Dataset& item : e->items) {
        auto l = read_lut(item, first_signed, lenient);
        if (!l) {
            if (!lenient) return l.take_error();
            continue;
        }
        out.luts.push_back(std::move(l).value());
    }
    return Status::ok();
}

}  // namespace

Result<LutTable> read_lut(const Dataset& item, bool first_signed, bool lenient) {
    const Element* d = item.find(tags::LutDescriptor);
    if (!d || !binary_vr(d->vr) || d->value.size() < 6)
        return Status(Errc::Malformed, "lut: LUT Descriptor missing or malformed", 0, tags::LutDescriptor);
    LutTable t;
    const std::uint8_t* p = d->value.data();
    const std::uint16_t n = load_le16(p);
    t.entries = n == 0 ? 65536u : n;
    const std::uint16_t f = load_le16(p + 2);
    const bool sgn = d->vr == VR::SS ? true : d->vr == VR::US ? false : first_signed;
    t.first_mapped = sgn ? static_cast<std::int16_t>(f) : static_cast<std::int64_t>(f);
    t.bits = load_le16(p + 4);
    if (t.bits < 8 || t.bits > 16) {
        if (!lenient || t.bits < 1 || t.bits > 16)
            return Status(Errc::Malformed, "lut: bits per entry must be 8..16", 0, tags::LutDescriptor);
    }
    const Element* data = item.find(tags::LutData);
    if (!data || !binary_vr(data->vr)) return Status(Errc::Malformed, "lut: LUT Data missing", 0, tags::LutData);
    const std::size_t len = data->value.size();
    const std::uint8_t* q = data->value.data();
    try {
        t.data.resize(t.entries);
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "lut: out of memory");
    }
    if (len >= std::size_t{t.entries} * 2) {
        const std::uint32_t m = (1u << t.bits) - 1u;
        for (std::uint32_t i = 0; i < t.entries; ++i) t.data[i] = static_cast<std::uint16_t>(load_le16(q + 2 * i) & m);
    } else if (t.bits <= 8 && len >= t.entries) {
        for (std::uint32_t i = 0; i < t.entries; ++i) t.data[i] = q[i];  // 8 bits allocated (C.11.1.1.1)
    } else {
        return Status(Errc::Malformed, "lut: LUT Data shorter than the descriptor says", 0, tags::LutData);
    }
    if (auto ex = item.get_string(tags::LutExplanation)) t.explanation = *ex;
    return t;
}

void ModalityTransform::output_range(std::int64_t lo, std::int64_t hi, double& out_lo, double& out_hi) const noexcept {
    if (has_lut) {
        out_lo = 0.0;
        out_hi = lut.max_output();
        return;
    }
    const double a = static_cast<double>(lo) * slope + intercept;
    const double b = static_cast<double>(hi) * slope + intercept;
    out_lo = std::min(a, b);
    out_hi = std::max(a, b);
}

Result<ModalityTransform> modality_for_frame(const Dataset& ds, const PixelInfo& pi, std::uint32_t frame, bool lenient) {
    ModalityTransform m;
    if (!pi.is_monochrome() || pi.kind != SampleKind::Integer) return m;
    if (const Dataset* pvt = functional_group(ds, frame, tags::PixelValueTransformationSequence)) {
        m.from_functional_groups = true;
        if (const Dataset* ml = first_item(*pvt, tags::ModalityLutSequence)) {
            auto l = read_lut(*ml, pi.is_signed, lenient);
            if (!l) return l.take_error();
            m.has_lut = true;
            m.lut = std::move(l).value();
            return m;
        }
        if (Status s = read_rescale(*pvt, m, lenient); !s) return s;
        return m;
    }
    if (const Dataset* ml = first_item(ds, tags::ModalityLutSequence)) {
        auto l = read_lut(*ml, pi.is_signed, lenient);
        if (l) {
            m.has_lut = true;
            m.lut = std::move(l).value();
            if (auto t = ml->get_string(tags::ModalityLutType)) m.rescale_type = *t;
            return m;
        }
        if (!lenient) return l.take_error();
    }
    if (Status s = read_rescale(ds, m, lenient); !s) return s;
    return m;
}

Result<VoiSet> voi_for_frame(const Dataset& ds, const PixelInfo& pi, const ModalityTransform& m, std::uint32_t frame,
                             bool lenient) {
    VoiSet v;
    // C.11.2.1.1: the VOI LUT's first value mapped is signed if the modality output may be signed.
    bool first_signed = pi.is_signed;
    if (m.has_lut) {
        first_signed = false;
    } else if (!m.is_identity()) {
        double lo = 0, hi = 0;
        m.output_range(detail::min_stored(pi), detail::max_stored(pi), lo, hi);
        first_signed = lo < 0;
    }
    if (const Dataset* fv = functional_group(ds, frame, tags::FrameVoiLutSequence)) {
        v.from_functional_groups = true;
        if (Status s = read_windows(*fv, v, lenient); !s) return s;
        if (Status s = read_voi_luts(*fv, first_signed, v, lenient); !s) return s;
        if (!v.empty()) return v;
    }
    if (Status s = read_windows(ds, v, lenient); !s) return s;
    if (Status s = read_voi_luts(ds, first_signed, v, lenient); !s) return s;
    return v;
}

bool window_valid(double center, double width, VoiFunction fn) noexcept {
    if (!std::isfinite(center) || !std::isfinite(width)) return false;
    return fn == VoiFunction::Linear ? width >= 1.0 : width > 0.0;
}

double apply_window(double x, double c, double w, VoiFunction fn, double ymin, double ymax) noexcept {
    switch (fn) {
        case VoiFunction::Linear:  // C.11.2.1.2.1
            if (x <= c - 0.5 - (w - 1) / 2) return ymin;
            if (x > c - 0.5 + (w - 1) / 2) return ymax;
            return ((x - (c - 0.5)) / (w - 1) + 0.5) * (ymax - ymin) + ymin;
        case VoiFunction::LinearExact:  // C.11.2.1.3.2
            if (x <= c - w / 2) return ymin;
            if (x > c + w / 2) return ymax;
            return ((x - c) / w + 0.5) * (ymax - ymin) + ymin;
        case VoiFunction::Sigmoid:  // C.11.2.1.3.1
            return (ymax - ymin) / (1 + std::exp(-4 * (x - c) / w)) + ymin;
    }
    return ymin;
}

double apply_voi_lut(const LutTable& lut, double x) noexcept {
    if (!(x == x)) return lut.data[0];
    const double r = std::floor(x + 0.5);
    if (r <= -9.0e18) return lut.data[0];
    if (r >= 9.0e18) return lut.data[lut.entries - 1];
    return lut.lookup(static_cast<std::int64_t>(r));
}

std::optional<PresentationShape> presentation_shape(const Dataset& ds) noexcept {
    auto s = ds.get_string(tags::PresentationLutShape);
    if (!s) return std::nullopt;
    if (*s == "INVERSE") return PresentationShape::Inverse;
    if (*s == "IDENTITY") return PresentationShape::Identity;
    return std::nullopt;
}

bool output_inverted(const PixelInfo& pi, const Dataset& ds) noexcept {
    if (pi.photometric == Photometric::Monochrome1) return true;
    const auto shape = presentation_shape(ds);
    return pi.is_monochrome() && shape && *shape == PresentationShape::Inverse;
}

Result<std::optional<LutTable>> presentation_lut(const Dataset& ds, bool lenient) {
    const Dataset* item = first_item(ds, tags::PresentationLutSequence);
    if (!item) return std::optional<LutTable>{};
    auto l = read_lut(*item, false, lenient);
    if (!l) {
        if (lenient) return std::optional<LutTable>{};
        return l.take_error();
    }
    return std::optional<LutTable>{std::move(l).value()};
}

}  // namespace siaara::image
