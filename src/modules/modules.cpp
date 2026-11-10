// SIAARA DICOM Toolkit - named data layer (see modules.hpp).
#include "modules/modules.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <new>
#include <stdexcept>

#include "charset/charset.hpp"
#include "dict/dictionary.hpp"

namespace siaara::modules {

namespace detail {
#include "modules/module_tables.inc"
}  // namespace detail

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool iequals_suffixed(std::string_view full, std::string_view name, std::string_view suffix) noexcept {
    return full.size() == name.size() + suffix.size() && iequals(full.substr(0, name.size()), name) &&
           iequals(full.substr(name.size()), suffix);
}

void flatten_into(std::uint32_t index, std::vector<std::uint32_t>& stack, std::vector<FlatAttr>& out,
                  std::vector<std::uint64_t>& seen) {
    if (std::find(stack.begin(), stack.end(), index) != stack.end()) return;
    stack.push_back(index);
    const ModNode& n = detail::kModNodes[index];
    for (const ModEntry& e : entries(n)) {
        if (e.attr < 0) {
            if (e.node >= 0) flatten_into(static_cast<std::uint32_t>(e.node), stack, out, seen);
            continue;
        }
        const ModAttr& a = detail::kModAttrs[e.attr];
        const std::uint64_t key = (std::uint64_t{a.tag} << 32) | a.mask;
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(FlatAttr{&a, e.type, e.node});
    }
    stack.pop_back();
}

}  // namespace

std::span<const ModNode> nodes() noexcept { return detail::kModNodes; }

std::span<const ModEntry> entries(const ModNode& n) noexcept {
    return std::span<const ModEntry>(detail::kModEntries).subspan(n.first, n.count);
}

const ModAttr* attribute(std::int32_t index) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= std::size(detail::kModAttrs)) return nullptr;
    return &detail::kModAttrs[index];
}

std::span<const ModAttr> attributes() noexcept { return detail::kModAttrs; }

const std::vector<FlatAttr>& flattened(std::uint32_t index) {
    static const std::vector<std::vector<FlatAttr>> all = [] {
        std::vector<std::vector<FlatAttr>> v(std::size(detail::kModNodes));
        std::vector<std::uint32_t> stack;
        std::vector<std::uint64_t> seen;
        for (std::uint32_t i = 0; i < v.size(); ++i) {
            seen.clear();
            flatten_into(i, stack, v[i], seen);
        }
        return v;
    }();
    static const std::vector<FlatAttr> empty;
    return index < all.size() ? all[index] : empty;
}

std::optional<std::uint32_t> find_node(std::string_view name) noexcept {
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    if (name.empty()) return std::nullopt;
    std::string_view id = name;
    if (id.size() > 6 && (iequals(id.substr(0, 6), "table_") || iequals(id.substr(0, 6), "table "))) id.remove_prefix(6);
    const auto& ns = detail::kModNodes;
    for (std::uint32_t i = 0; i < std::size(ns); ++i) {
        const ModNode& n = ns[i];
        if (iequals(n.name, name) || (!n.table_id.empty() && iequals(n.table_id, id)) || iequals(n.class_name, name))
            return i;
    }
    // A bare name: the Module of that name first, then the Macro (several names have both, e.g.
    // Cardiac Synchronization, Respiratory Synchronization, Real World Value Mapping).
    for (const char* suffix : {" Module", " Macro"})
        for (std::uint32_t i = 0; i < std::size(ns); ++i) {
            const ModNode& n = ns[i];
            if (n.kind == static_cast<std::int32_t>(NodeKind::Item)) continue;
            if (iequals_suffixed(n.name, name, suffix)) return i;
        }
    return std::nullopt;
}

bool node_present(const Dataset& ds, std::uint32_t index) {
    for (const FlatAttr& f : flattened(index)) {
        if (f.attr->mask == 0xFFFFFFFFu) {
            if (ds.contains(f.attr->tag)) return true;
            continue;
        }
        for (const Element& e : ds.elements())
            if ((e.tag & f.attr->mask) == f.attr->tag) return true;
    }
    return false;
}

// ---------------------------------------------------------------- path API

namespace {

struct Segment {
    Tag tag = 0;
    bool is_private = false;
    std::string creator;
    std::uint16_t group = 0;
    std::uint8_t offset = 0;
    std::int64_t index = -1;
    std::string text;  // for messages
};

bool hex4(std::string_view s, std::uint16_t& out) noexcept {
    if (s.size() != 4) return false;
    unsigned v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
        else return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

Status bad(std::string_view what, std::string_view seg) {
    std::string m(what);
    if (!seg.empty()) {
        m += ": ";
        m.append(seg.substr(0, 80));
    }
    return Status(Errc::InvalidArgument, std::move(m));
}

// Parses the path into segments.
Result<std::vector<Segment>> parse_path(std::string_view p) {
    std::vector<Segment> out;
    std::size_t i = 0;
    if (p.size() > 4096) return bad("path too long", {});
    while (true) {
        if (i >= p.size()) return bad("empty path segment", p);
        Segment s;
        const std::size_t start = i;
        std::string_view name;
        if (p[i] == '"') {
            const std::size_t q = p.find('"', i + 1);
            if (q == std::string_view::npos || q + 1 >= p.size() || p[q + 1] != ':') return bad("bad quoted private creator", p.substr(i));
            s.creator.assign(p.substr(i + 1, q - i - 1));
            s.is_private = true;
            i = q + 2;
            const std::size_t e = p.find_first_of(".[", i);
            name = p.substr(i, (e == std::string_view::npos ? p.size() : e) - i);
            i += name.size();
        } else if (p[i] == '(') {
            const std::size_t q = p.find(')', i);
            if (q == std::string_view::npos) return bad("missing ')'", p.substr(i));
            name = p.substr(i, q - i + 1);
            i = q + 1;
        } else {
            const std::size_t e = p.find_first_of(".[", i);
            name = p.substr(i, (e == std::string_view::npos ? p.size() : e) - i);
            i += name.size();
            const std::size_t colon = name.rfind(':');
            if (colon != std::string_view::npos) {
                s.creator.assign(name.substr(0, colon));
                s.is_private = true;
                name = name.substr(colon + 1);
            }
        }
        if (s.is_private) {
            // gggg,xx
            std::uint16_t g = 0;
            if (name.size() != 7 || name[4] != ',' || !hex4(name.substr(0, 4), g)) return bad("private tag must be creator:gggg,xx", p.substr(start, i - start));
            std::uint16_t x = 0;
            std::string four = "00";
            four.append(name.substr(5, 2));
            if (!hex4(four, x)) return bad("private tag must be creator:gggg,xx", p.substr(start, i - start));
            if ((g & 1u) == 0) return bad("private tags have an odd group", p.substr(start, i - start));
            if (s.creator.empty()) return bad("empty private creator", p.substr(start, i - start));
            s.group = g;
            s.offset = static_cast<std::uint8_t>(x);
        } else {
            std::string_view t = name;
            if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = t.substr(1, t.size() - 2);
            std::uint16_t g = 0, el = 0;
            const bool with_comma = t.size() == 9 && t[4] == ',' && hex4(t.substr(0, 4), g) && hex4(t.substr(5, 4), el);
            const bool plain = !with_comma && t.size() == 8 && name.front() != '(' && hex4(t.substr(0, 4), g) &&
                               hex4(t.substr(4, 4), el);
            if (with_comma || plain) {
                s.tag = make_tag(g, el);
            } else {
                const auto* d = dict::lookup_keyword(t);
                // Repeating groups (Overlay 60xx, Curve 50xx): the keyword names the first group
                // (6000 / 5000); another group is addressed by its tag, e.g. (6002,0010).
                if (!d || (d->mask != 0xFFFFFFFFu && d->mask != 0xFF00FFFFu)) {
                    std::string m = "unknown keyword: " + std::string(t.substr(0, 80));
                    if (d) m += " (a repeating-element keyword: use the tag)";
                    // R-157: creator:gggg,xx with a '.', '[' or ']' in an unquoted creator ends
                    // up here, cut at that character.
                    if (p.find(':', start) != std::string_view::npos)
                        m += " (a private creator that contains '.', '[' or ']' must be quoted: \"creator\":gggg,xx)";
                    return Status(Errc::NotFound, std::move(m));
                }
                s.tag = d->tag;
            }
        }
        if (i < p.size() && p[i] == '[') {
            const std::size_t q = p.find(']', i);
            if (q == std::string_view::npos) return bad("missing ']'", p.substr(start));
            std::string_view num = p.substr(i + 1, q - i - 1);
            std::uint32_t v = 0;
            auto r = std::from_chars(num.data(), num.data() + num.size(), v);
            if (num.empty() || r.ec != std::errc() || r.ptr != num.data() + num.size()) return bad("bad index", p.substr(start, q + 1 - start));
            s.index = v;
            i = q + 1;
        }
        s.text.assign(p.substr(start, i - start));
        out.push_back(std::move(s));
        if (i == p.size()) break;
        if (p[i] != '.') return bad("expected '.'", p.substr(i));
        ++i;
        if (out.size() > 64) return bad("path too deep", {});
    }
    return out;
}

charset::CharacterSet cs_of(const Dataset& ds, const charset::CharacterSet* parent) {
    return charset::character_set_of(ds, parent);
}

struct Walk {
    const Dataset* parent = nullptr;
    const Element* element = nullptr;
    const Dataset* item = nullptr;
    std::int64_t index = -1;
    charset::CharacterSet cs;
};

Result<Walk> walk(const Dataset& root, std::string_view path) {
    auto segs = parse_path(path);
    if (!segs) return segs.error();
    Walk w;
    const Dataset* cur = &root;
    charset::CharacterSet cs = cs_of(root, nullptr);
    const auto& v = segs.value();
    for (std::size_t k = 0; k < v.size(); ++k) {
        const Segment& s = v[k];
        Tag tag = s.tag;
        if (s.is_private) {
            auto t = cur->resolve_private(s.group, s.creator, s.offset);
            if (!t) return Status(Errc::NotFound, "private creator block not present: " + s.text);
            tag = *t;
        }
        const Element* e = cur->find(tag);
        if (!e) return Status(Errc::NotFound, "not present: " + s.text);
        const bool last = k + 1 == v.size();
        if (!last) {
            if (!e->is_sequence()) return Status(Errc::TypeMismatch, "not a sequence: " + s.text);
            if (s.index < 0) return bad("a sequence needs an item index", s.text);
            if (static_cast<std::uint64_t>(s.index) >= e->items.size())
                return Status(Errc::NotFound, "item index out of range: " + s.text);
            const Dataset& item = e->items[static_cast<std::size_t>(s.index)];
            cs = cs_of(item, &cs);
            cur = &item;
            continue;
        }
        w.parent = cur;
        w.element = e;
        w.index = s.index;
        w.cs = cs;
        if (e->is_sequence() && s.index >= 0) {
            if (static_cast<std::uint64_t>(s.index) >= e->items.size())
                return Status(Errc::NotFound, "item index out of range: " + s.text);
            w.item = &e->items[static_cast<std::size_t>(s.index)];
        }
    }
    return w;
}

bool vr_is_binary_numeric(VR vr) noexcept {
    switch (vr) {
        case VR::US: case VR::SS: case VR::UL: case VR::SL: case VR::FL: case VR::FD: case VR::SV: case VR::UV:
            return true;
        default:
            return false;
    }
}

bool vr_is_bulk(VR vr) noexcept {
    switch (vr) {
        case VR::OB: case VR::OW: case VR::OD: case VR::OF: case VR::OL: case VR::OV: case VR::UN:
            return true;
        default:
            return false;
    }
}

std::string format_tag(Tag t) {
    char b[16];
    std::snprintf(b, sizeof b, "(%04X,%04X)", static_cast<unsigned>(t >> 16), static_cast<unsigned>(t & 0xFFFFu));
    return b;
}

}  // namespace

Result<PathTarget> resolve_path(const Dataset& root, std::string_view path) {
    auto w = walk(root, path);
    if (!w) return w.error();
    PathTarget t;
    t.parent = w.value().parent;
    t.element = w.value().element;
    t.item = w.value().item;
    t.index = w.value().index;
    t.charset = w.value().cs.to_string();
    return t;
}

namespace {
Result<PathValue> get_by_path_impl(const Dataset& root, std::string_view path);
}  // namespace

Result<PathValue> get_by_path(const Dataset& root, std::string_view path) {
    try {
        return get_by_path_impl(root, path);
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "out of memory");
    } catch (const std::length_error&) {  // more elements than a vector can hold (32-bit builds)
        return Status(Errc::LimitExceeded, "result exceeds the memory budget");
    }
}

namespace {
Result<PathValue> get_by_path_impl(const Dataset& root, std::string_view path) {
    auto wr = walk(root, path);
    if (!wr) return wr.take_error();
    const Walk& w = wr.value();
    if (w.element == nullptr || w.parent == nullptr) return Status(Errc::Internal, "path did not resolve");
    const Element& e = *w.element;
    const Dataset& ds = *w.parent;
    PathValue v;
    v.tag = e.tag;
    v.vr = e.vr;
    v.value_index = w.index;
    if (e.is_sequence()) {
        v.item_count = static_cast<std::uint32_t>(e.items.size());
        v.vm = v.item_count;
        v.kind = w.item ? ValueKind::Item : ValueKind::Sequence;
        return v;
    }
    if (e.is_encapsulated()) {
        v.kind = ValueKind::Binary;
        v.vm = 1;
        for (const auto& f : e.fragments) v.byte_length += f.size();
        return v;
    }
    v.byte_length = e.value.size();
    if (e.value.size() == 0) {
        if (w.index >= 0) return Status(Errc::NotFound, "value index out of range (empty value)");
        v.kind = ValueKind::Empty;
        return v;
    }
    v.vm = static_cast<std::uint32_t>(ds.value_multiplicity(e.tag));
    if (w.index >= 0 && static_cast<std::uint64_t>(w.index) >= v.vm)
        return Status(Errc::NotFound, "value index out of range");
    const std::size_t first = w.index >= 0 ? static_cast<std::size_t>(w.index) : 0;
    const std::size_t end = w.index >= 0 ? first + 1 : v.vm;
    if (e.vr == VR::AT) {
        v.kind = ValueKind::Tag;
        const std::uint8_t* p = e.value.data();
        for (std::size_t i = first; i < end && (i + 1) * 4 <= e.value.size(); ++i) {
            const Tag t = make_tag(static_cast<std::uint16_t>(p[i * 4] | (p[i * 4 + 1] << 8)),
                                   static_cast<std::uint16_t>(p[i * 4 + 2] | (p[i * 4 + 3] << 8)));
            if (i == first) {
                v.has_integer = true;
                v.integer = t;
            }
            if (i != first) v.text += '\\';
            v.text += format_tag(t);
        }
        return v;
    }
    if (vr_is_bulk(e.vr)) {
        v.kind = ValueKind::Binary;
        return v;
    }
    const bool numeric = vr_is_binary_numeric(e.vr) || e.vr == VR::DS || e.vr == VR::IS;
    const bool integer = e.vr != VR::FL && e.vr != VR::FD && e.vr != VR::DS;
    // One pass over the element whatever its value count (R-111): text is decoded once and split
    // once; fetching value i with get_string / get_utf8 for every i rescans it each time.
    std::size_t replaced = 0;
    if (w.index >= 0) {
        std::optional<std::string> s = numeric ? ds.get_string(e.tag, first) : charset::get_utf8(ds, e.tag, first, &w.cs, &replaced);
        if (s) v.text = std::move(*s);
    } else if (vr_is_string(e.vr) && !numeric) {
        if (auto s = charset::get_utf8_all(ds, e.tag, &w.cs, &replaced)) v.text = std::move(*s);
    } else if (vr_is_string(e.vr) || !vr_is_binary_numeric(e.vr)) {
        // DS / IS, and a VR that is none of the above (read like text, as get_string does).
        const std::string_view raw = e.value.as_chars();
        v.text.reserve(raw.size());
        for_each_value(raw, [&](std::size_t i, std::string_view piece) {
            if (i) v.text += '\\';
            v.text += trim_value(piece, e.vr);
            return true;
        });
    } else {
        // Fixed-size binary values: each is read in constant time.
        for (std::size_t i = first; i < end; ++i) {
            if (i != first) v.text += '\\';
            if (auto s = ds.get_string(e.tag, i)) v.text += *s;
        }
    }
    v.undecodable = static_cast<std::uint32_t>(std::min<std::size_t>(replaced, 0xFFFFFFFFu));
    if (numeric) {
        v.kind = integer ? ValueKind::Integer : ValueKind::Real;
        if (auto d = ds.get_double(e.tag, first)) {
            v.has_real = true;
            v.real = *d;
        }
        if (integer) {
            if (auto n = ds.get_int(e.tag, first)) {
                v.has_integer = true;
                v.integer = *n;
            }
        }
    } else {
        v.kind = ValueKind::Text;
    }
    return v;
}
}  // namespace

namespace {

// The numeric element a path names, and the range of its values the path selects.
struct NumericTarget {
    const Dataset* parent = nullptr;
    Tag tag = 0;
    std::size_t first = 0, count = 0;
};
Result<NumericTarget> numeric_target(const Dataset& root, std::string_view path) {
    auto wr = walk(root, path);
    if (!wr) return wr.take_error();
    const Walk& w = wr.value();
    if (w.element == nullptr || w.parent == nullptr) return Status(Errc::Internal, "path did not resolve");
    const Element& e = *w.element;
    if (e.is_sequence() || e.is_encapsulated() || e.vr == VR::AT) return Status(Errc::TypeMismatch, "not a numeric element");
    const bool numeric = vr_is_binary_numeric(e.vr) || e.vr == VR::DS || e.vr == VR::IS || e.vr == VR::OW ||
                         e.vr == VR::OF || e.vr == VR::OD || e.vr == VR::OL || e.vr == VR::OV;
    if (!numeric) return Status(Errc::TypeMismatch, "not a numeric element");
    const std::size_t vm = e.value.size() == 0 ? 0 : w.parent->value_multiplicity(e.tag);
    if (w.index >= 0 && static_cast<std::uint64_t>(w.index) >= vm) return Status(Errc::NotFound, "value index out of range");
    NumericTarget t;
    t.parent = w.parent;
    t.tag = e.tag;
    t.first = w.index >= 0 ? static_cast<std::size_t>(w.index) : 0;
    t.count = w.index >= 0 ? 1 : vm;
    return t;
}

}  // namespace

Result<std::size_t> doubles_by_path_count(const Dataset& root, std::string_view path) {
    auto t = numeric_target(root, path);
    if (!t) return t.take_error();
    return t.value().count;
}

Status doubles_by_path_into(const Dataset& root, std::string_view path, std::span<double> out) {
    auto t = numeric_target(root, path);
    if (!t) return t.take_error();
    if (out.size() != t.value().count) return Status(Errc::BufferTooSmall, "buffer does not hold the value count");
    if (!t.value().parent->get_doubles(t.value().tag, t.value().first, out))
        return Status(Errc::TypeMismatch, "value is not a number");
    return Status::ok();
}

Result<std::vector<double>> doubles_by_path(const Dataset& root, std::string_view path) {
    try {
        auto t = numeric_target(root, path);
        if (!t) return t.take_error();
        if (!fits_named_data_budget(t.value().count, sizeof(double)))
            return Status(Errc::LimitExceeded, "values exceed the memory budget");
        std::vector<double> out(t.value().count);
        if (!t.value().parent->get_doubles(t.value().tag, t.value().first, out))
            return Status(Errc::TypeMismatch, "value is not a number");
        return out;
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "out of memory");
    } catch (const std::length_error&) {  // more elements than a vector can hold (32-bit builds)
        return Status(Errc::LimitExceeded, "result exceeds the memory budget");
    }
}

}  // namespace siaara::modules
