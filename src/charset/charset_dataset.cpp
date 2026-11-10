// SIAARA DICOM Toolkit - data set text helpers (charset::get_utf8 / set_utf8).
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "charset/charset.hpp"

namespace siaara::charset {

namespace {

const CharacterSet& default_set() {
    static const CharacterSet kDefault;
    return kDefault;
}

// Returns piece `index` of `s` split on `delim`.
std::optional<std::string_view> piece(std::string_view s, char delim, std::size_t index) noexcept {
    std::size_t start = 0;
    for (std::size_t k = 0; k < index; ++k) {
        const std::size_t pos = s.find(delim, start);
        if (pos == std::string_view::npos) return std::nullopt;
        start = pos + 1;
    }
    const std::size_t end = s.find(delim, start);
    return s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

}  // namespace

CharacterSet character_set_of(const Dataset& ds, const CharacterSet* inherited) {
    if (auto raw = ds.get_raw_string(tags::SpecificCharacterSet)) return CharacterSet::parse(*raw);
    if (inherited) return *inherited;
    // An item read on its own: the enclosing data set's value stored on it (F15).
    if (!ds.inherited_charset().empty()) return CharacterSet::parse(ds.inherited_charset());
    return default_set();
}

std::optional<std::string> get_utf8(const Dataset& ds, Tag tag, std::size_t vm, const CharacterSet* inherited) {
    const Element* e = ds.find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    if (!vr_is_string(e->vr)) return ds.get_string(tag, vm);
    const TextKind kind = text_kind(e->vr);
    const CharacterSet cs = vr_uses_character_set(e->vr) ? character_set_of(ds, inherited) : default_set();
    const std::string all = decode(e->value.as_chars(), cs, kind);
    std::string_view v = all;
    if (kind == TextKind::Text) {
        if (vm != 0) return std::nullopt;
    } else {
        // Safe: decode() emits '\\' only for real value delimiters.
        auto p = piece(v, '\\', vm);
        if (!p) return std::nullopt;
        v = *p;
    }
    return std::string(trim_value(v, e->vr));
}

Status set_utf8(Dataset& ds, Tag tag, VR vr, std::string_view utf8, const CharacterSet* inherited,
                std::vector<Unrepresentable>* failures) {
    if (failures) failures->clear();
    if (!vr_is_string(vr)) return Status(Errc::TypeMismatch, "VR is not a string VR", 0, tag);
    const CharacterSet cs = vr_uses_character_set(vr) ? character_set_of(ds, inherited) : default_set();
    EncodeResult r = encode(utf8, cs, text_kind(vr));
    if (!r.ok()) {
        // The message carries no characters of the value (it may be PHI): only counts and offsets.
        const std::string cs_name = cs.to_string();
        const std::string name = cs_name.empty() ? std::string("the default repertoire (ISO_IR 6)")
                                                 : "Specific Character Set '" + cs_name + "'";
        std::string msg = std::to_string(r.failures.size()) + " character(s) cannot be represented in " +
                          name + "; first at UTF-8 byte offset " + std::to_string(r.failures.front().offset);
        if (failures) *failures = std::move(r.failures);
        return Status(Errc::Unsupported, std::move(msg), 0, tag);
    }
    return ds.set_string(tag, vr, r.bytes);
}

}  // namespace siaara::charset
