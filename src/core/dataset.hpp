// SIAARA DICOM Toolkit - in-memory data set model.
// Values are kept in little-endian byte order in memory (big-endian input is swapped on read).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara {

class Dataset;

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
        std::uint32_t group_bytes = 0;    // group length (gggg,0000): actual size of the group as read
    } read;
    // Byte offset of this element's header in the parsed stream (the file for native syntaxes; the
    // inflated stream for the deflated syntax, see DicomFile::offsets_are_file_offsets). 0 if built in memory.
    std::uint64_t offset = 0;
    Bytes value;
    std::vector<Dataset> items;
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
    std::vector<Element>& elements() noexcept { return elements_; }

    // Inserts or replaces. Keeps ascending tag order.
    Element& put(Element e);
    bool remove(Tag tag);
    // Fast path used by the parser when tags arrive in ascending order.
    void append_sorted(Element e);

    // ---- typed getters (value multiplicity index `vm`, 0-based)
    // Text value with trailing padding (spaces / NULs) removed; leading spaces kept except where
    // PS3.5 says they are insignificant (handled per VR). Returns nullopt when absent.
    std::optional<std::string_view> get_raw_string(Tag tag) const;
    std::optional<std::string> get_string(Tag tag, std::size_t vm = 0) const;
    std::optional<std::int64_t> get_int(Tag tag, std::size_t vm = 0) const;
    std::optional<double> get_double(Tag tag, std::size_t vm = 0) const;
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

private:
    std::vector<Element> elements_;
    std::uint8_t item_length_mode_ = kItemLengthNew;
};

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
    // False for the deflated transfer syntax, where Element::offset counts in the inflated stream.
    bool offsets_are_file_offsets = true;
};

// Strips trailing spaces/NULs (and leading spaces for VRs where they are insignificant).
std::string_view trim_value(std::string_view s, VR vr) noexcept;
// Splits a backslash-delimited multi-value string and returns entry `index`.
std::optional<std::string_view> split_value(std::string_view s, std::size_t index) noexcept;

}  // namespace siaara
