// SIAARA DICOM Toolkit - access to the generated de-identification tables (deid_table.inc).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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

namespace {
// The few rows with a repeating-group mask (50xx, 60xx, ...), in table order, bucketed by the
// top byte of the group when the mask keeps it (every row of the current table), so a tag that
// has no exact row is checked against a handful of rows instead of the whole table.
struct MaskedIndex {
    std::vector<const ProfileRow*> bucket[256];
    std::vector<const ProfileRow*> other;  // masks that do not keep the top byte
    MaskedIndex() {
        for (const ProfileRow& r : profile_rows()) {
            if (r.mask == 0xFFFFFFFFu) continue;
            if ((r.mask & 0xFF000000u) == 0xFF000000u) bucket[(r.tag >> 24) & 0xFFu].push_back(&r);
            else other.push_back(&r);
        }
    }
};
const MaskedIndex& masked_index() {
    static const MaskedIndex index;
    return index;
}
}  // namespace

const ProfileRow* find_row(Tag tag) noexcept {
    // The generator sorts by (value, mask); plain tags have the full mask, so a binary search
    // over the values finds an exact row; then the repeating-group rows (masked_index).
    const auto rows = profile_rows();
    auto it = std::lower_bound(rows.begin(), rows.end(), tag,
                               [](const ProfileRow& r, Tag t) { return r.tag < t; });
    for (; it != rows.end() && it->tag == tag; ++it)
        if (it->mask == 0xFFFFFFFFu) return &*it;
    const MaskedIndex& mi = masked_index();
    const ProfileRow* best = nullptr;  // the first matching row in table order, as before
    for (const ProfileRow* r : mi.bucket[(tag >> 24) & 0xFFu])
        if ((tag & r->mask) == r->tag) {
            best = r;
            break;
        }
    for (const ProfileRow* r : mi.other)
        if ((tag & r->mask) == r->tag && (!best || r < best)) {
            best = r;
            break;
        }
    return best;
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
