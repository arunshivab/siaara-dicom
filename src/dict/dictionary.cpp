#include "dict/dictionary.hpp"

#include <algorithm>
#include <array>

namespace siaara::dict {

namespace {
#include "dict/dictionary_data.inc"  // defines kEntries (sorted by tag), kMaskedEntries, kUids (sorted by uid)
}  // namespace

const Entry* lookup(Tag tag) noexcept {
    // Group length (gggg,0000) is UL for every group.
    static constexpr Entry kGroupLength{0x00000000, 0x0000FFFF, VR::UL, VR::None,
                                        "GenericGroupLength", "Generic Group Length", "1", false};
    auto it = std::lower_bound(std::begin(kEntries), std::end(kEntries), tag,
                               [](const Entry& e, Tag t) { return e.tag < t; });
    if (it != std::end(kEntries) && it->tag == tag) return &*it;
    for (const Entry& e : kMaskedEntries)
        if ((tag & e.mask) == e.tag) return &e;
    if (tag_element(tag) == 0 && !tag_is_private(tag)) return &kGroupLength;
    return nullptr;
}

const Entry* lookup_keyword(std::string_view keyword) noexcept {
    for (const Entry& e : kEntries)
        if (e.keyword == keyword) return &e;
    for (const Entry& e : kMaskedEntries)
        if (e.keyword == keyword) return &e;
    return nullptr;
}

VR implicit_vr(Tag tag) noexcept {
    if (tag_is_private(tag)) {
        // Private creator elements (gggg,0010-00FF) are LO by definition (PS3.5 7.8.1).
        const std::uint16_t el = tag_element(tag);
        if (el >= 0x0010 && el <= 0x00FF) return VR::LO;
        if (el == 0) return VR::UL;
        return VR::UN;
    }
    const Entry* e = lookup(tag);
    if (!e || e->vr == VR::None) return VR::UN;
    // "OB or OW" elements (Pixel Data, Overlay Data, ...) are OW in implicit VR (PS3.5 A.1).
    if (e->vr == VR::OB && e->vr_alt == VR::OW) return VR::OW;
    return e->vr;
}

std::size_t entry_count() noexcept { return std::size(kEntries) + std::size(kMaskedEntries); }

const UidEntry* lookup_uid(std::string_view uid) noexcept {
    auto it = std::lower_bound(std::begin(kUids), std::end(kUids), uid,
                               [](const UidEntry& e, std::string_view u) { return e.uid < u; });
    if (it != std::end(kUids) && it->uid == uid) return &*it;
    return nullptr;
}

std::size_t uid_count() noexcept { return std::size(kUids); }

}  // namespace siaara::dict
