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

bool is_lut_descriptor(Tag tag) noexcept {
    return tag == 0x00283002u || (tag >= 0x00281100u && tag <= 0x00281103u) || (tag >= 0x00281111u && tag <= 0x00281113u);
}

bool us_or_ss_by_pixel_representation(Tag tag) noexcept {
    switch (tag) {
        case 0x00189810u:  // Zero Velocity Pixel Value
        case 0x00221452u:  // Mapped Pixel Value
        case 0x00280071u:  // Perimeter Value (retired)
        case 0x00280104u: case 0x00280105u:  // Smallest / Largest Valid Pixel Value (retired)
        case 0x00280106u: case 0x00280107u:  // Smallest / Largest Image Pixel Value
        case 0x00280108u: case 0x00280109u:  // Smallest / Largest Pixel Value in Series
        case 0x00280110u: case 0x00280111u:  // Smallest / Largest Image Pixel Value in Plane (retired)
        case 0x00280120u: case 0x00280121u:  // Pixel Padding Value / Range Limit
        case 0x00409211u: case 0x00409216u:  // Real World Value Last / First Value Mapped
        case 0x00603004u: case 0x00603006u:  // Histogram First / Last Bin Value
            return true;
        default:
            return is_lut_descriptor(tag);
    }
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

const Entry* entry_at(std::size_t index) noexcept {
    if (index < std::size(kEntries)) return &kEntries[index];
    index -= std::size(kEntries);
    return index < std::size(kMaskedEntries) ? &kMaskedEntries[index] : nullptr;
}

const UidEntry* lookup_uid(std::string_view uid) noexcept {
    auto it = std::lower_bound(std::begin(kUids), std::end(kUids), uid,
                               [](const UidEntry& e, std::string_view u) { return e.uid < u; });
    if (it != std::end(kUids) && it->uid == uid) return &*it;
    return nullptr;
}

std::size_t uid_count() noexcept { return std::size(kUids); }

}  // namespace siaara::dict
