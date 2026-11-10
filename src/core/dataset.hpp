// SIAARA DICOM Toolkit - in-memory data set model.
// Values are kept in little-endian byte order in memory (big-endian input is swapped on read).
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara {

class Dataset;

// The items of a sequence. Each item has its own heap allocation, so a reference (or a borrowed
// C API handle) to an item stays valid while sibling items are added or removed and while the
// element itself moves (e.g. when its parent data set grows), until that item is removed or the
// sequence is replaced (R-033, CAPI-05). Vector-like interface over Dataset references.
class ItemList {
    using Store = std::vector<std::unique_ptr<Dataset>>;

public:
    template <bool Const>
    class Iter {
        using Base = std::conditional_t<Const, Store::const_iterator, Store::iterator>;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Dataset;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const Dataset&, Dataset&>;
        using pointer = std::conditional_t<Const, const Dataset*, Dataset*>;
        Iter() = default;
        explicit Iter(Base b) : b_(b) {}
        template <bool C = Const, class = std::enable_if_t<C>>
        Iter(const Iter<false>& o) : b_(o.base()) {}  // NOLINT(google-explicit-constructor)
        reference operator*() const { return **b_; }
        pointer operator->() const { return b_->get(); }
        reference operator[](difference_type n) const { return *b_[n]; }
        Iter& operator++() { ++b_; return *this; }
        Iter operator++(int) { Iter t = *this; ++b_; return t; }
        Iter& operator--() { --b_; return *this; }
        Iter operator--(int) { Iter t = *this; --b_; return t; }
        Iter& operator+=(difference_type n) { b_ += n; return *this; }
        Iter& operator-=(difference_type n) { b_ -= n; return *this; }
        friend Iter operator+(Iter a, difference_type n) { return a += n; }
        friend Iter operator+(difference_type n, Iter a) { return a += n; }
        friend Iter operator-(Iter a, difference_type n) { return a -= n; }
        friend difference_type operator-(const Iter& a, const Iter& b) { return a.b_ - b.b_; }
        friend bool operator==(const Iter& a, const Iter& b) { return a.b_ == b.b_; }
        friend auto operator<=>(const Iter& a, const Iter& b) { return a.b_ <=> b.b_; }
        Base base() const { return b_; }

    private:
        Base b_{};
    };
    using value_type = Dataset;
    using size_type = std::size_t;
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;
    using reference = Dataset&;
    using const_reference = const Dataset&;

    ItemList() = default;
    ItemList(const ItemList& o);
    ItemList(ItemList&&) noexcept = default;
    ItemList& operator=(const ItemList& o);
    ItemList& operator=(ItemList&&) noexcept = default;
    ~ItemList() = default;
    ItemList(std::vector<Dataset>&& v);       // NOLINT(google-explicit-constructor)
    ItemList(const std::vector<Dataset>& v);  // NOLINT(google-explicit-constructor)
    ItemList(std::initializer_list<Dataset> v);

    std::size_t size() const noexcept { return v_.size(); }
    bool empty() const noexcept { return v_.empty(); }
    void clear() noexcept { v_.clear(); }
    void reserve(std::size_t n) { v_.reserve(n); }
    Dataset& operator[](std::size_t i) { return *v_[i]; }
    const Dataset& operator[](std::size_t i) const { return *v_[i]; }
    Dataset& front() { return *v_.front(); }
    const Dataset& front() const { return *v_.front(); }
    Dataset& back() { return *v_.back(); }
    const Dataset& back() const { return *v_.back(); }
    iterator begin() noexcept { return iterator(v_.begin()); }
    iterator end() noexcept { return iterator(v_.end()); }
    const_iterator begin() const noexcept { return const_iterator(v_.begin()); }
    const_iterator end() const noexcept { return const_iterator(v_.end()); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }
    void push_back(const Dataset& d);
    void push_back(Dataset&& d);
    template <class... A>
    Dataset& emplace_back(A&&... a);
    void pop_back() { v_.pop_back(); }
    void assign(std::size_t n, const Dataset& d);
    iterator erase(const_iterator pos) { return iterator(v_.erase(pos.base())); }
    iterator erase(const_iterator first, const_iterator last) { return iterator(v_.erase(first.base(), last.base())); }

private:
    Store v_;
};

// One data element. Exactly one of value / items / fragments is used:
//  - normal elements: value
//  - SQ:              items
//  - encapsulated pixel data (undefined length OB/OW): fragments (first one is the Basic Offset Table)
struct Element {
    Tag tag = 0;
    VR vr = VR::None;
    bool undefined_length = false;   // was encoded with undefined length
    // How the reader found the element encoded. The writer's "keep as read" options use this to
    // reproduce unedited elements byte for byte (F1). Elements built in memory leave it zeroed;
    // see ReadInfo for what counts as an edit.
    struct ReadInfo {
        bool parsed = false;              // came from a parsed stream
        bool un_sequence = false;         // SQ stored as UN with undefined length (items implicit VR LE)
        std::uint32_t length = 0;         // stored value length (defined-length values)
        std::uint32_t fragment_count = 0; // encapsulated: fragments as read (including the offset table)
        // Encapsulated Pixel Data read with a defined length (lenient, PS3.5 A.4 asks for undefined
        // length): 1 items only, 2 items and a Sequence Delimitation Item inside the length. The
        // writer keeps that form while the fragments are unchanged. 0 otherwise.
        std::uint8_t fragments_defined_length = 0;
        std::uint32_t group_bytes = 0;    // group length (gggg,0000): actual size of the group as read
        // The VR was not in the stream (implicit VR, or an unknown VR read as implicit): it came
        // from the dictionary, and for "US or SS" elements from Pixel Representation (F8).
        bool vr_inferred = false;
        // The inferred VR came from the private dictionary (dict::lookup_private) through the
        // element's Private Creator. It is for interpretation only: the stream had no VR, so
        // while vr still equals this value the writer saves the element as it was read (UN in
        // explicit VR syntaxes; a defined-length SQ stays defined length). VR::None otherwise.
        VR private_vr = VR::None;
    } read;
    // Byte offset of this element's header in the parsed stream (the file for native syntaxes; the
    // inflated stream for the deflated syntax, see DicomFile::offsets_are_file_offsets). 0 if built in memory.
    std::uint64_t offset = 0;
    Bytes value;
    // Stable item addresses (see ItemList). Items added here directly, in an element that is
    // already in a data set, do not get the enclosing Specific Character Set: call
    // propagate_inherited_charset() on that data set afterwards, or put() the element (R-136).
    ItemList items;
    std::vector<Bytes> fragments;

    bool is_sequence() const noexcept { return vr == VR::SQ; }
    bool is_encapsulated() const noexcept { return !fragments.empty() || (undefined_length && vr != VR::SQ); }
};

// A data set: elements kept sorted by tag (PS3.5 7.1 requires ascending order).
class Dataset {
public:
    Dataset() = default;

    // ---- element access
    const Element* find(Tag tag) const noexcept;
    Element* find(Tag tag) noexcept;
    bool contains(Tag tag) const noexcept { return find(tag) != nullptr; }
    std::size_t size() const noexcept { return elements_.size(); }
    bool empty() const noexcept { return elements_.empty(); }
    const std::vector<Element>& elements() const noexcept { return elements_; }
    // Direct access for editing in place. The data set does not see such edits: after adding
    // items to Element::items directly, or changing the bytes of Specific Character Set
    // (0008,0005) in place, call propagate_inherited_charset() on this data set (or store the
    // element again with put()), or the items keep the character set they inherited before and
    // their text decodes with the wrong one (R-136). Keep the elements in ascending tag order.
    std::vector<Element>& elements() noexcept { return elements_; }

    // Inserts or replaces. Keeps ascending tag order.
    Element& put(Element e);
    bool remove(Tag tag);
    // Fast path used by the parser when tags arrive in ascending order.
    void append_sorted(Element e);
    // Bulk building (parsers): append in arrival order, then call sort_by_tag() once. Between the
    // two calls the data set may be out of order and lookups are not valid.
    void append_unsorted(Element e) { elements_.push_back(std::move(e)); }
    // Stable sort by tag; of duplicate tags the last appended is kept (as put() would) and the
    // duplicate tags are reported in `duplicates` when given. O(n log n).
    void sort_by_tag(std::vector<Tag>* duplicates = nullptr);

    // ---- typed getters (value multiplicity index `vm`, 0-based)
    // Text value with trailing padding (spaces / NULs) removed; leading spaces kept except where
    // PS3.5 says they are insignificant (handled per VR). Returns nullopt when absent.
    std::optional<std::string_view> get_raw_string(Tag tag) const;
    std::optional<std::string> get_string(Tag tag, std::size_t vm = 0) const;
    std::optional<std::int64_t> get_int(Tag tag, std::size_t vm = 0) const;
    std::optional<double> get_double(Tag tag, std::size_t vm = 0) const;
    // Values [first, first + out.size()) as doubles in one pass over the element (DS / IS text is
    // scanned once, not once per value). False when the element is absent or not numeric, the
    // range is past the last value, or a value in the range is not a number; `out` may then be
    // partly written. Fetching many values with get_double(tag, i) in a loop is quadratic for
    // DS / IS: use this (R-111).
    bool get_doubles(Tag tag, std::size_t first, std::span<double> out) const;
    // Every value as a double (as get_double would give each); nullopt as above.
    std::optional<std::vector<double>> get_doubles(Tag tag) const;
    std::size_t value_multiplicity(Tag tag) const;

    // ---- setters (convenience; encode in little endian)
    Status set_string(Tag tag, VR vr, std::string_view value);
    Status set_uint16(Tag tag, std::uint16_t value);
    Status set_uint32(Tag tag, std::uint32_t value);
    Status set_bytes(Tag tag, VR vr, Bytes value);

    // Private creator resolution: returns the tag for (group, creator, element-offset).
    std::optional<Tag> resolve_private(std::uint16_t group, std::string_view creator,
                                       std::uint8_t element_offset) const;

    // When this data set is a sequence item: how its length was encoded on read
    // (0 = built in memory, 1 = explicit length, 2 = undefined length + delimiter). Used by the
    // writer's "keep as read" sequence length mode.
    enum : std::uint8_t { kItemLengthNew = 0, kItemLengthExplicit = 1, kItemLengthUndefined = 2 };
    std::uint8_t item_length_mode() const noexcept { return item_length_mode_; }
    void set_item_length_mode(std::uint8_t m) noexcept { item_length_mode_ = m; }

    // When this data set is a sequence item: the Specific Character Set (0008,0005) value in force
    // in the enclosing data set (its own value, or the one it inherited), as stored with trailing
    // padding removed; empty for the default repertoire. PS3.5 7.5.3: an item without its own
    // (0008,0005) uses the enclosing data set's, so text in an item accessed on its own (a C API
    // item handle, a copy) still decodes correctly (F15). Kept current by the readers, by put()
    // and remove() of (0008,0005) or of a sequence, and by propagate_inherited_charset().
    const std::string& inherited_charset() const noexcept { return inherited_charset_; }
    void set_inherited_charset(std::string_view raw) { inherited_charset_.assign(raw); }

private:
    std::vector<Element> elements_;
    std::string inherited_charset_;
    std::uint8_t item_length_mode_ = kItemLengthNew;
};

// The Specific Character Set value in force for `ds`: its own (0008,0005) (padding removed), else
// the inherited one (Dataset::inherited_charset); empty for the default repertoire.
std::string_view charset_in_force(const Dataset& ds);
// Sets Dataset::inherited_charset of every item below `ds` (all depths) from the value in force in
// its enclosing data set, starting from charset_in_force(ds). Iterative: any depth is safe.
// The readers, Dataset::put() and Dataset::remove() call it; code that edits Element::items or
// the bytes of (0008,0005) through elements() / find() must call it itself (R-136).
void propagate_inherited_charset(Dataset& ds);

// ---- ItemList members that need the complete Dataset
inline ItemList::ItemList(const ItemList& o) {
    v_.reserve(o.v_.size());
    for (const auto& p : o.v_) v_.push_back(std::make_unique<Dataset>(*p));
}
inline ItemList& ItemList::operator=(const ItemList& o) {
    if (this != &o) {
        ItemList t(o);
        v_ = std::move(t.v_);
    }
    return *this;
}
inline ItemList::ItemList(std::vector<Dataset>&& v) {
    v_.reserve(v.size());
    for (auto& d : v) v_.push_back(std::make_unique<Dataset>(std::move(d)));
}
inline ItemList::ItemList(const std::vector<Dataset>& v) {
    v_.reserve(v.size());
    for (const auto& d : v) v_.push_back(std::make_unique<Dataset>(d));
}
inline ItemList::ItemList(std::initializer_list<Dataset> v) {
    v_.reserve(v.size());
    for (const auto& d : v) v_.push_back(std::make_unique<Dataset>(d));
}
inline void ItemList::push_back(const Dataset& d) { v_.push_back(std::make_unique<Dataset>(d)); }
inline void ItemList::push_back(Dataset&& d) { v_.push_back(std::make_unique<Dataset>(std::move(d))); }
inline void ItemList::assign(std::size_t n, const Dataset& d) {
    Store t;
    t.reserve(n);
    for (std::size_t i = 0; i < n; ++i) t.push_back(std::make_unique<Dataset>(d));
    v_ = std::move(t);
}
template <class... A>
Dataset& ItemList::emplace_back(A&&... a) {
    v_.push_back(std::make_unique<Dataset>(std::forward<A>(a)...));
    return *v_.back();
}

// Result of reading a Part 10 file or a raw data set.
struct DicomFile {
    Dataset meta;                 // group 0002
    Dataset dataset;              // everything else
    std::string transfer_syntax;  // UID string, padding removed
    bool had_preamble = false;
    Bytes preamble;               // the 128-byte preamble as read (written back unless cleared)
    // True when `meta` was parsed from a stream: the writer then keeps it as read (it does not add
    // missing meta elements). Clear it after building a new meta group.
    bool meta_as_read = false;
    std::vector<std::string> warnings;  // lenient-mode findings (never contain PHI)
    // Where a header-only parse stopped (offset of the Pixel Data element), or 0.
    std::uint64_t stopped_at_offset = 0;
    // Whether the data set holds everything the input held (testing-chat P0-1). Any bit set
    // means a save would write less than the input, or would turn damaged input into a file
    // that looks whole: the writer refuses such a file unless WriteOptions::allow_incomplete is
    // set. The first warning then starts with kWarnParseIncomplete, kWarnInputTruncated or
    // kWarnReadRecovered.
    enum ReadLoss : std::uint32_t {
        kReadBytesSkipped = 1u << 0,    // input bytes that are in no element (lenient skip)
        kReadInputTruncated = 1u << 1,  // the input ended inside an element, item or sequence
        kReadHeaderOnly = 1u << 2,      // ReadOptions::stop_before_pixel_data: the rest not read
        // Lenient reading recovered damaged input with a heuristic (an element length corrected,
        // an encoding detected without file meta; testing 10b item 7): the data set is the
        // reader's interpretation, so a save (or a C-STORE) would write something the input did
        // not hold literally. Every recovery is also a read warning.
        kReadRecovered = 1u << 3,
    };
    std::uint32_t read_loss = 0;          // ReadLoss bits
    std::uint64_t read_loss_offset = 0;   // offset of the first loss (parsed-stream offset)
    std::uint64_t bytes_not_read = 0;     // input bytes in no element (skipped or after a header-only stop)
    bool complete() const noexcept { return read_loss == 0; }
    // False for the deflated transfer syntax, where Element::offset counts in the inflated stream.
    bool offsets_are_file_offsets = true;
};

// Fixed codes that start the first warning of an incomplete read (DicomFile::read_loss).
inline constexpr std::string_view kWarnParseIncomplete = "PARSE_INCOMPLETE";
inline constexpr std::string_view kWarnInputTruncated = "INPUT_TRUNCATED";
inline constexpr std::string_view kWarnReadRecovered = "READ_RECOVERED";

// Strips trailing spaces/NULs (and leading spaces for VRs where they are insignificant).
std::string_view trim_value(std::string_view s, VR vr) noexcept;
// Splits a backslash-delimited multi-value string and returns entry `index`. One scan from the
// start per call: to visit every value use for_each_value() or split_all_values(), never a loop
// over `index` (quadratic, R-111 / R-112).
std::optional<std::string_view> split_value(std::string_view s, std::size_t index) noexcept;
// One pass over a backslash-delimited multi-value string: fn(index, piece) for every value in
// order (an empty string is one empty value). fn returns bool: false stops. Returns the number of
// values visited.
template <class Fn>
std::size_t for_each_value(std::string_view s, Fn&& fn) {
    std::size_t start = 0, index = 0;
    while (true) {
        const std::size_t end = s.find('\\', start);
        const bool last = end == std::string_view::npos;
        if (!fn(index, s.substr(start, last ? std::string_view::npos : end - start))) return index + 1;
        ++index;
        if (last) return index;
        start = end + 1;
    }
}
// Every value of a backslash-delimited string in one pass, as views into `s`; at most `max`.
std::vector<std::string_view> split_all_values(std::string_view s, std::size_t max = static_cast<std::size_t>(-1));

}  // namespace siaara
