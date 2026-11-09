#include "core/dataset.hpp"

#include "core/number.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace siaara {

namespace {
bool leading_space_insignificant(VR vr) noexcept {
    // PS3.5 table 6.2-1: leading spaces are insignificant for these VRs.
    switch (vr) {
        case VR::AE: case VR::CS: case VR::DS: case VR::IS: case VR::LO: case VR::SH:
        case VR::TM: case VR::DA: case VR::DT: case VR::AS: case VR::PN:
            return true;
        default:
            return false;
    }
}

// Locale-independent, correctly rounded decimal parsing for DS/IS/numeric strings (PARSE-42).
std::optional<double> parse_double(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return parse_decimal_c(s);
}

std::optional<std::int64_t> parse_int(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    if (s.empty()) return std::nullopt;
    if (s.front() == '+') s.remove_prefix(1);
    std::int64_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

auto tag_less = [](const Element& e, Tag t) { return e.tag < t; };
}  // namespace

std::string_view trim_value(std::string_view s, VR vr) noexcept {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    if (leading_space_insignificant(vr))
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}

std::optional<std::string_view> split_value(std::string_view s, std::size_t index) noexcept {
    std::size_t start = 0;
    for (std::size_t i = 0; i < index; ++i) {
        const auto pos = s.find('\\', start);
        if (pos == std::string_view::npos) return std::nullopt;
        start = pos + 1;
    }
    const auto end = s.find('\\', start);
    return s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

const Element* Dataset::find(Tag tag) const noexcept {
    auto it = std::lower_bound(elements_.begin(), elements_.end(), tag, tag_less);
    return (it != elements_.end() && it->tag == tag) ? &*it : nullptr;
}

Element* Dataset::find(Tag tag) noexcept {
    auto it = std::lower_bound(elements_.begin(), elements_.end(), tag, tag_less);
    return (it != elements_.end() && it->tag == tag) ? &*it : nullptr;
}

Element& Dataset::put(Element e) {
    auto it = std::lower_bound(elements_.begin(), elements_.end(), e.tag, tag_less);
    if (it != elements_.end() && it->tag == e.tag) {
        *it = std::move(e);
        return *it;
    }
    return *elements_.insert(it, std::move(e));
}

void Dataset::append_sorted(Element e) {
    if (elements_.empty() || elements_.back().tag < e.tag)
        elements_.push_back(std::move(e));
    else
        put(std::move(e));
}

bool Dataset::remove(Tag tag) {
    auto it = std::lower_bound(elements_.begin(), elements_.end(), tag, tag_less);
    if (it == elements_.end() || it->tag != tag) return false;
    elements_.erase(it);
    return true;
}

std::optional<std::string_view> Dataset::get_raw_string(Tag tag) const {
    const Element* e = find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    return e->value.as_chars();
}

std::size_t Dataset::value_multiplicity(Tag tag) const {
    const Element* e = find(tag);
    if (!e) return 0;
    if (e->is_sequence()) return e->items.size();
    if (e->value.empty()) return 0;
    if (vr_is_string(e->vr)) {
        if (e->vr == VR::LT || e->vr == VR::ST || e->vr == VR::UT || e->vr == VR::UR) return 1;
        const auto s = e->value.as_chars();
        return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\\')) + 1;
    }
    const std::size_t w = vr_value_size(e->vr);
    return w ? e->value.size() / w : 1;
}

std::optional<std::string> Dataset::get_string(Tag tag, std::size_t vm) const {
    const Element* e = find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    if (vr_is_string(e->vr) || e->vr == VR::UN || e->vr == VR::None) {
        std::string_view s = e->value.as_chars();
        if (e->vr != VR::LT && e->vr != VR::ST && e->vr != VR::UT && e->vr != VR::UR) {
            auto part = split_value(s, vm);
            if (!part) return std::nullopt;
            s = *part;
        } else if (vm != 0) {
            return std::nullopt;
        }
        return std::string(trim_value(s, e->vr));
    }
    // Binary VRs: format the number.
    if (auto d = get_double(tag, vm)) {
        if (e->vr == VR::FL || e->vr == VR::FD || e->vr == VR::OF || e->vr == VR::OD) {
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof buf, *d);
            return std::string(buf, r.ptr);
        }
        if (auto i = get_int(tag, vm)) return std::to_string(*i);
    }
    return std::nullopt;
}

std::optional<std::int64_t> Dataset::get_int(Tag tag, std::size_t vm) const {
    const Element* e = find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    const std::uint8_t* p = e->value.data();
    const std::size_t n = e->value.size();
    auto at = [&](std::size_t w) -> const std::uint8_t* {
        std::uint64_t off = 0;
        if (!checked_mul(vm, w, off) || off + w > n) return nullptr;
        return p + off;
    };
    switch (e->vr) {
        case VR::US: case VR::OW: { auto q = at(2); if (!q) return std::nullopt; return load_le16(q); }
        case VR::SS: { auto q = at(2); if (!q) return std::nullopt; return static_cast<std::int16_t>(load_le16(q)); }
        case VR::UL: case VR::OL: { auto q = at(4); if (!q) return std::nullopt; return load_le32(q); }
        case VR::AT: {  // (group, element) pair: returned as the tag value (group << 16) | element
            auto q = at(4); if (!q) return std::nullopt;
            return (static_cast<std::int64_t>(load_le16(q)) << 16) | load_le16(q + 2);
        }
        case VR::SL: { auto q = at(4); if (!q) return std::nullopt; return static_cast<std::int32_t>(load_le32(q)); }
        case VR::SV: { auto q = at(8); if (!q) return std::nullopt; return static_cast<std::int64_t>(load_le64(q)); }
        case VR::UV: case VR::OV: {
            auto q = at(8); if (!q) return std::nullopt;
            const std::uint64_t u = load_le64(q);
            if (u > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
            return static_cast<std::int64_t>(u);
        }
        case VR::OB: case VR::UN: { auto q = at(1); if (!q) return std::nullopt; return *q; }
        case VR::IS: {
            auto part = split_value(e->value.as_chars(), vm);
            if (!part) return std::nullopt;
            return parse_int(*part);
        }
        case VR::DS: case VR::FL: case VR::FD: case VR::OF: case VR::OD: {
            auto d = get_double(tag, vm);
            if (!d || std::trunc(*d) != *d || std::fabs(*d) > 9.2e18) return std::nullopt;
            return static_cast<std::int64_t>(*d);
        }
        default:
            return std::nullopt;
    }
}

std::optional<double> Dataset::get_double(Tag tag, std::size_t vm) const {
    const Element* e = find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    const std::uint8_t* p = e->value.data();
    const std::size_t n = e->value.size();
    switch (e->vr) {
        case VR::FL: case VR::OF: {
            std::uint64_t off = 0;
            if (!checked_mul(vm, 4, off) || off + 4 > n) return std::nullopt;
            const std::uint32_t bits = load_le32(p + off);
            float f;
            std::memcpy(&f, &bits, 4);
            return static_cast<double>(f);
        }
        case VR::FD: case VR::OD: {
            std::uint64_t off = 0;
            if (!checked_mul(vm, 8, off) || off + 8 > n) return std::nullopt;
            const std::uint64_t bits = load_le64(p + off);
            double d;
            std::memcpy(&d, &bits, 8);
            return d;
        }
        case VR::DS: case VR::IS: {
            auto part = split_value(e->value.as_chars(), vm);
            if (!part) return std::nullopt;
            return parse_double(*part);
        }
        default: {
            auto i = get_int(tag, vm);
            if (!i) return std::nullopt;
            return static_cast<double>(*i);
        }
    }
}

Status Dataset::set_string(Tag tag, VR vr, std::string_view value) {
    if (!vr_is_string(vr)) return Status(Errc::TypeMismatch, "VR is not a string VR", 0, tag);
    std::string v(value);
    if (v.size() & 1u) v.push_back(vr_pad_char(vr));
    Element e;
    e.tag = tag;
    e.vr = vr;
    e.value = Bytes::from_string(v);
    put(std::move(e));
    return Status::ok();
}

Status Dataset::set_uint16(Tag tag, std::uint16_t value) {
    std::uint8_t b[2];
    store_le16(b, value);
    Element e;
    e.tag = tag;
    e.vr = VR::US;
    e.value = Bytes::copy_of(b, 2);
    put(std::move(e));
    return Status::ok();
}

Status Dataset::set_uint32(Tag tag, std::uint32_t value) {
    std::uint8_t b[4];
    store_le32(b, value);
    Element e;
    e.tag = tag;
    e.vr = VR::UL;
    e.value = Bytes::copy_of(b, 4);
    put(std::move(e));
    return Status::ok();
}

Status Dataset::set_bytes(Tag tag, VR vr, Bytes value) {
    Element e;
    e.tag = tag;
    e.vr = vr;
    e.value = std::move(value);
    put(std::move(e));
    return Status::ok();
}

std::optional<Tag> Dataset::resolve_private(std::uint16_t group, std::string_view creator,
                                            std::uint8_t element_offset) const {
    if ((group & 1u) == 0) return std::nullopt;
    for (std::uint16_t block = 0x10; block <= 0xFF; ++block) {
        const Element* e = find(make_tag(group, block));
        if (!e) continue;
        if (trim_value(e->value.as_chars(), VR::LO) == creator)
            return make_tag(group, static_cast<std::uint16_t>((block << 8) | element_offset));
    }
    return std::nullopt;
}

}  // namespace siaara
