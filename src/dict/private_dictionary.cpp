// SIAARA DICOM Toolkit - private-tag dictionary (generated from vendor DICOM Conformance Statements
// by tools/gen_private_dict.py; see docs/PRIVATE_DICTIONARY.md).
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>

#include "core/dataset.hpp"
#include "dict/dictionary.hpp"

namespace siaara::dict {

namespace {

// Compact table: strings live in one pool and rows hold 32-bit pool offsets and small indices, so
// the table needs no relocations and no per-row std::string.
struct PrivCreator {
    std::uint32_t name;   // pool offset
    std::uint32_t fold;   // pool offset of the case/whitespace fold
    std::uint16_t first;  // first entry
    std::uint16_t count;  // entries
};

struct PrivEntry {
    std::uint32_t name;    // pool offset
    std::uint32_t keyword; // pool offset
    std::uint16_t creator; // index into kPrivCreators
    std::uint16_t group;
    VR vr;                 // VR::None: undecided
    std::uint16_t doc;     // primary source: index into kPrivDocs
    std::uint16_t page;
    std::uint16_t sources;
    std::uint16_t cand_first;  // kPrivCandidates (VR conflicts only)
    std::uint16_t cand_count;
    std::uint8_t element;
    std::uint8_t vm;      // index into kPrivVms
    std::uint8_t flags;   // kFlag*
    std::uint8_t vendor;  // index into kPrivVendors
};
static_assert(sizeof(PrivEntry) == 28, "compact private dictionary row");

struct PrivCandidate {
    VR vr;
    std::uint16_t doc;
    std::uint16_t page;
};

// Keep in step with tools/gen_private_dict.py.
constexpr std::uint8_t kFlagAssumed = 1;
constexpr std::uint8_t kFlagVrConflict = 2;
constexpr std::uint8_t kFlagVmConflict = 4;
constexpr std::uint8_t kFlagRetired = 8;

#include "dict/private_dictionary_data.inc"  // kPrivPool, kPrivVendors, kPrivVms, kPrivDocs, kPrivCreators, kPrivFoldOrder, kPrivEntries, kPrivCandidates

static_assert(std::size(kPrivEntries) <= 0xFFFFu, "entry indices are 16-bit");

const char* pool(std::uint32_t off) noexcept { return kPrivPool[off / kPrivPoolRow] + (off % kPrivPoolRow); }
std::string_view pool_sv(std::uint32_t off) noexcept { return std::string_view(pool(off)); }

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// Trailing spaces and NULs are value padding (PS3.5 6.2), not part of the creator.
std::string_view trim_creator(std::string_view s) noexcept {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return s;
}

// Same fold as tools/gen_private_dict.py: ASCII upper case, ASCII whitespace removed. Returns
// false when the folded creator would not fit (no dictionary creator is that long).
bool fold(std::string_view s, std::array<char, 128>& buf, std::size_t& n) noexcept {
    n = 0;
    for (char c : s) {
        if (is_space(c)) continue;
        if (n == buf.size()) return false;
        buf[n++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return true;
}

// Byte-wise comparison (the generator sorts by UTF-8 bytes).
bool bytes_less(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(a[i]);
        const auto y = static_cast<unsigned char>(b[i]);
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

const PrivEntry* find_in_creator(std::size_t ci, std::uint16_t group, std::uint8_t el) noexcept {
    const PrivCreator& c = kPrivCreators[ci];
    const PrivEntry* b = kPrivEntries + c.first;
    const PrivEntry* e = b + c.count;
    const std::uint32_t key = (static_cast<std::uint32_t>(group) << 8) | el;
    const PrivEntry* it = std::lower_bound(b, e, key, [](const PrivEntry& x, std::uint32_t k) {
        return ((static_cast<std::uint32_t>(x.group) << 8) | x.element) < k;
    });
    if (it != e && it->group == group && it->element == el) return it;
    return nullptr;
}

// Index of the creator spelled exactly `s`, or -1.
long find_creator(std::string_view s) noexcept {
    const PrivCreator* b = std::begin(kPrivCreators);
    const PrivCreator* e = std::end(kPrivCreators);
    const PrivCreator* it = std::lower_bound(b, e, s, [](const PrivCreator& c, std::string_view v) {
        return bytes_less(pool_sv(c.name), v);
    });
    if (it != e && pool_sv(it->name) == s) return static_cast<long>(it - b);
    return -1;
}

// Preferred of two fuzzy candidates: stated over assumed, then more documents, then table order.
bool better(const PrivEntry* a, const PrivEntry* b) noexcept {
    if (!b) return true;
    const bool sa = (a->flags & kFlagAssumed) == 0;
    const bool sb = (b->flags & kFlagAssumed) == 0;
    if (sa != sb) return sa;
    return a->sources > b->sources;
}

PrivateEntry make_entry(const PrivEntry& e, PrivateMatch match) noexcept {
    PrivateEntry out;
    out.creator = pool_sv(kPrivCreators[e.creator].name);
    out.group = e.group;
    out.element = e.element;
    out.name = pool_sv(e.name);
    out.keyword = pool_sv(e.keyword);
    out.vr = e.vr;
    out.vm = pool_sv(kPrivVms[e.vm]);
    out.retired = (e.flags & kFlagRetired) != 0;
    out.conflict = (e.flags & kFlagVrConflict) != 0;
    out.vm_conflict = (e.flags & kFlagVmConflict) != 0;
    out.confidence = (e.flags & kFlagAssumed) ? PrivateConfidence::Assumed : PrivateConfidence::Stated;
    out.match = match;
    out.vendor = pool_sv(kPrivVendors[e.vendor]);
    out.source = PrivateSource{pool_sv(kPrivDocs[e.doc]), e.page};
    out.source_count = e.sources;
    out.candidate_count = out.conflict ? e.cand_count : (e.vr != VR::None ? 1u : 0u);
    out.index = static_cast<std::uint32_t>(&e - kPrivEntries);
    return out;
}

}  // namespace

std::optional<PrivateEntry> lookup_private(std::string_view creator, std::uint16_t group,
                                           std::uint8_t element_low_byte) noexcept {
    creator = trim_creator(creator);
    const long exact = find_creator(creator);
    if (exact >= 0) {
        if (const PrivEntry* e = find_in_creator(static_cast<std::size_t>(exact), group, element_low_byte))
            return make_entry(*e, PrivateMatch::Exact);
    }
    std::array<char, 128> buf{};
    std::size_t n = 0;
    if (!fold(creator, buf, n) || n == 0) return std::nullopt;
    const std::string_view f(buf.data(), n);
    const std::uint16_t* b = std::begin(kPrivFoldOrder);
    const std::uint16_t* e = std::end(kPrivFoldOrder);
    const std::uint16_t* it = std::lower_bound(b, e, f, [](std::uint16_t ci, std::string_view v) {
        return bytes_less(pool_sv(kPrivCreators[ci].fold), v);
    });
    const PrivEntry* best = nullptr;
    for (; it != e && pool_sv(kPrivCreators[*it].fold) == f; ++it) {
        if (static_cast<long>(*it) == exact) continue;
        const PrivEntry* x = find_in_creator(*it, group, element_low_byte);
        if (x && better(x, best)) best = x;
    }
    if (!best) return std::nullopt;
    return make_entry(*best, PrivateMatch::Fuzzy);
}

bool private_candidate(const PrivateEntry& entry, std::size_t i, PrivateCandidate& out) noexcept {
    if (entry.index >= std::size(kPrivEntries) || i >= entry.candidate_count) return false;
    const PrivEntry& e = kPrivEntries[entry.index];
    if (e.flags & kFlagVrConflict) {
        if (i >= e.cand_count) return false;
        const PrivCandidate& c = kPrivCandidates[e.cand_first + i];
        out = PrivateCandidate{c.vr, PrivateSource{pool_sv(kPrivDocs[c.doc]), c.page}};
        return true;
    }
    if (e.vr == VR::None || i != 0) return false;
    out = PrivateCandidate{e.vr, PrivateSource{pool_sv(kPrivDocs[e.doc]), e.page}};
    return true;
}

VR implicit_private_vr(std::string_view creator, std::uint16_t group, std::uint8_t element_low_byte) noexcept {
    const auto e = lookup_private(creator, group, element_low_byte);
    if (!e || e->vr == VR::None) return VR::UN;
    return e->vr;
}

std::optional<PrivateEntry> lookup_private_element(const Dataset& ds, Tag tag) noexcept {
    const std::uint16_t g = tag_group(tag);
    const std::uint16_t el = tag_element(tag);
    if (!tag_is_private(tag) || el < 0x1000) return std::nullopt;
    const Element* c = ds.find(make_tag(g, static_cast<std::uint16_t>(el >> 8)));
    if (!c || c->is_sequence() || c->is_encapsulated()) return std::nullopt;
    return lookup_private(c->value.as_chars(), g, static_cast<std::uint8_t>(el & 0xFFu));
}

std::size_t private_entry_count() noexcept { return std::size(kPrivEntries); }
std::size_t private_creator_count() noexcept { return std::size(kPrivCreators); }

}  // namespace siaara::dict
