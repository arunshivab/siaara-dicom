// SIAARA DICOM Toolkit - access to the generated de-identification tables (deid_table.inc).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "deid/deid.hpp"

namespace siaara::deid {
namespace detail {
using A = Action;
#include "deid/deid_table.inc"
}  // namespace detail

std::span<const ProfileRow> profile_rows() noexcept { return {detail::kProfileRows, detail::kProfileRowCount}; }
const ProfileRow& private_attributes_row() noexcept { return detail::kPrivateAttributesRow; }
std::span<const SafePrivateRow> safe_private_rows() noexcept {
    return {detail::kSafePrivateRows, detail::kSafePrivateRowCount};
}
std::span<const MethodCode> method_codes() noexcept { return {detail::kCid7050, detail::kCid7050Count}; }

const ProfileRow* find_row(Tag tag) noexcept {
    // The generator sorts by (value, mask); plain tags have the full mask, so a binary search
    // over the values finds an exact row, and the few repeating-group rows are scanned.
    const auto rows = profile_rows();
    auto it = std::lower_bound(rows.begin(), rows.end(), tag,
                               [](const ProfileRow& r, Tag t) { return r.tag < t; });
    for (; it != rows.end() && it->tag == tag; ++it)
        if (it->mask == 0xFFFFFFFFu) return &*it;
    for (const ProfileRow& r : rows)
        if (r.mask != 0xFFFFFFFFu && (tag & r.mask) == r.tag) return &r;
    return nullptr;
}

const SafePrivateRow* find_safe_private(std::uint16_t group, std::string_view creator,
                                        std::uint8_t element) noexcept {
    const auto rows = safe_private_rows();
    auto key_less = [](const SafePrivateRow& r, std::uint16_t g, std::string_view c, std::uint8_t e) {
        if (r.group != g) return r.group < g;
        if (r.creator != c) return r.creator < c;
        return r.element < e;
    };
    auto it = std::lower_bound(rows.begin(), rows.end(), 0, [&](const SafePrivateRow& r, int) {
        return key_less(r, group, creator, element);
    });
    if (it != rows.end() && it->group == group && it->creator == creator && it->element == element) return &*it;
    return nullptr;
}

Action effective_action(const ProfileRow& row, std::uint32_t options) noexcept {
    bool any_c = false;
    for (std::size_t i = 0; i < kOptionCount; ++i) {
        if (!(options & (1u << i))) continue;
        if (row.option[i] == Action::K) return Action::K;
        if (row.option[i] == Action::C) any_c = true;
    }
    return any_c ? Action::C : row.basic;
}

}  // namespace siaara::deid
