#include "core/reader.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "codec/deflate.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"

namespace siaara {

namespace {

// "(gggg,eeee)" for messages.
std::string hex_tag(Tag t) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "(gggg,eeee)";
    for (int i = 0; i < 4; ++i) {
        out[1 + static_cast<std::size_t>(i)] = kHex[(t >> (28 - 4 * i)) & 0xFu];
        out[6 + static_cast<std::size_t>(i)] = kHex[(t >> (12 - 4 * i)) & 0xFu];
    }
    return out;
}


struct Syntax {
    bool explicit_vr = true;
    bool big_endian = false;
    bool pixel_big_endian = false;  // F6b: only top-level OW Pixel Data is big endian
};

class Parser {
public:
    Parser(const Bytes& buf, const ReadOptions& opt, std::vector<std::string>* warnings)
        : buf_(buf), p_(buf.data()), size_(buf.size()), limit_(buf.size()), opt_(opt), warnings_(warnings) {}

    std::size_t pos() const noexcept { return pos_; }
    void seek(std::size_t p) noexcept { pos_ = p; }
    std::size_t size() const noexcept { return size_; }

    // Reads elements into `ds` until `end` (absolute offset) or, when `end` is undefined,
    // until an Item Delimitation Item. `stop_group` > 0 stops before the first element whose
    // group differs (used for file meta).
    // Nothing inside may extend past the end of the innermost enclosing defined-length item or
    // sequence (`limit_`, R-001 / PARSE-10): strict mode fails with Malformed, lenient mode clamps
    // to that end and warns. The cursor never moves backwards.
    // Elements are appended in arrival order and sorted once at the end (R-002 / PARSE-21);
    // duplicates are reported and the last occurrence kept, as before.
    Status parse(Dataset& ds, Syntax syn, std::uint64_t end, bool undefined, std::uint32_t depth,
                 std::uint16_t only_group = 0) {
        const std::size_t saved_limit = limit_;
        if (!undefined) {
            assert(end <= limit_);
            if (end < limit_) limit_ = static_cast<std::size_t>(end);
        }
        // Pixel Representation is inherited by items and scoped to the data set that holds it (F8).
        const int saved_pixel_rep = pixel_rep_;
        bool ordered = true;
        Status s = parse_elements(ds, syn, end, undefined, depth, only_group, ordered);
        pixel_rep_ = saved_pixel_rep;
        if (!ordered) {
            std::vector<Tag> dups;
            ds.sort_by_tag(&dups);
            for (std::size_t i = 0; i < dups.size() && i < 16; ++i) warn("duplicate tag " + tag_text(dups[i]) + "; last kept");
        }
        limit_ = saved_limit;
        return s;
    }

    Status parse_elements(Dataset& ds, Syntax syn, std::uint64_t end, bool undefined, std::uint32_t depth,
                          std::uint16_t only_group, bool& ordered) {
        if (depth > opt_.max_depth)
            return Status(Errc::LimitExceeded, "sequence nesting too deep", pos_);
        GroupLengthTracker groups{ds, pos_};  // F1: records each group length's actual group size
        // Private Creator values of this data set (PS3.5 7.8.1: a creator's scope is the data set
        // that holds it), for the private dictionary on implicit VR elements.
        std::vector<std::pair<Tag, std::string>> creators;
        while (true) {
            groups.content_end = pos_;
            if (!undefined && pos_ >= end) return Status::ok();
            if (pos_ == limit_) {
                if (undefined && depth > 0) return lenient_or(Errc::Truncated, "missing item delimiter");
                return Status::ok();
            }
            // File meta: a short tail that does not start with the meta group is the body (an empty
            // deflated data set is only 2 bytes), not a truncated meta element.
            if (only_group != 0 && limit_ - pos_ < 8 && (limit_ - pos_ < 2 || load_le16(p_ + pos_) != only_group))
                return Status::ok();
            if (limit_ - pos_ < 8) return lenient_or(Errc::Truncated, "truncated element header");

            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            if (only_group != 0 && tag_group(tag) != only_group) {
                pos_ = start;
                return Status::ok();
            }
            if (tag == tags::ItemDelimitationItem) {
                pos_ += 4;  // skip 32-bit length (must be 0)
                if (undefined) return Status::ok();
                warn("stray item delimiter");
                continue;
            }
            if (tag == tags::SequenceDelimitationItem) {
                // Seen at data set level: stray. Skip.
                pos_ += 4;
                warn("stray sequence delimiter");
                continue;
            }
            if (opt_.stop_before_pixel_data && depth == 0 &&
                (tag == tags::PixelData || tag == tags::FloatPixelData ||
                 tag == tags::DoubleFloatPixelData)) {
                pos_ = start;
                stopped_at_pixels_ = true;
                stop_offset_ = start;
                return Status::ok();
            }

            groups.element_starts(tag);
            Element e;
            e.tag = tag;
            e.offset = start;
            e.read.parsed = true;
            std::uint32_t len = 0;
            if (Status s = read_vr_and_length(syn, tag, e.vr, len, e.read.vr_inferred); !s) return s;
            // F8: an inferred "US or SS" VR follows Pixel Representation (PS3.5 / PS3.6), so the
            // getters, DICOM JSON and the C API read the value signed when it is 1. The bytes
            // are unchanged.
            if (e.read.vr_inferred && e.vr == VR::US && pixel_rep_ == 1 && dict::us_or_ss_by_pixel_representation(tag))
                e.vr = VR::SS;
            // Private dictionary: an inferred UN on a private data element (gggg,xxEE) takes the VR
            // the dictionary decided for its creator, for interpretation only (ReadInfo::private_vr).
            // Little endian only, so the bytes need no swapping; undefined lengths keep their
            // meaning (an implicit sequence).
            if (opt_.private_dictionary && e.read.vr_inferred && e.vr == VR::UN && !syn.big_endian &&
                len != kUndefinedLength && tag_is_private(tag) && tag_element(tag) >= 0x1000) {
                const Tag ct = make_tag(tag_group(tag), static_cast<std::uint16_t>(tag_element(tag) >> 8));
                for (const auto& c : creators) {
                    if (c.first != ct) continue;
                    const VR pv = dict::implicit_private_vr(c.second, tag_group(tag),
                                                            static_cast<std::uint8_t>(tag_element(tag) & 0xFFu));
                    if (pv != VR::UN && pv != VR::None) {
                        e.vr = pv;
                        e.read.private_vr = pv;
                    }
                    break;
                }
            }

            if (++element_count_ > opt_.max_elements)
                return Status(Errc::LimitExceeded, "too many elements", start, tag);

            if (len == kUndefinedLength) {
                e.undefined_length = true;
                if (e.vr == VR::SQ || (e.vr == VR::UN && tag != tags::PixelData)) {
                    // UN with undefined length is a sequence encoded implicit VR LE (PS3.5 6.2.2).
                    Syntax inner = (e.vr == VR::UN) ? Syntax{false, false} : syn;
                    e.read.un_sequence = e.vr == VR::UN;
                    e.vr = VR::SQ;
                    if (Status s = parse_items(e, inner, 0, true, depth); !s) return s;
                } else if (tag == tags::PixelData || tag == tags::FloatPixelData ||
                           tag == tags::DoubleFloatPixelData || tag_group(tag) == 0x7FE0 ||
                           e.vr == VR::OB || e.vr == VR::OW || e.vr == VR::UN) {
                    if (Status s = parse_fragments(e, syn); !s) return s;
                    e.read.fragment_count = static_cast<std::uint32_t>(e.fragments.size());
                } else {
                    return Status(Errc::Malformed, "undefined length on a non-sequence element",
                                  start, tag);
                }
            } else {
                std::uint64_t vend = 0;
                if (!checked_add(pos_, len, vend) || vend > limit_) {
                    // Inside a defined-length item or sequence the parent's end is the bound
                    // (Malformed); at top level it is the end of the data (Truncated).
                    const bool in_parent = limit_ < size_;
                    if (!opt_.lenient)
                        return in_parent ? Status(Errc::Malformed, "value runs past end of parent item or sequence", start, tag)
                                         : Status(Errc::Truncated, "value runs past end of data", start, tag);
                    warn(in_parent ? "value runs past end of parent; clamped" : "value truncated at end of data");
                    if (!in_parent) note_truncated(start);
                    len = static_cast<std::uint32_t>(limit_ - pos_);
                    vend = limit_;
                }
                e.read.length = len;
                if (e.vr == VR::SQ) {
                    const std::size_t vstart = pos_;
                    if (Status s = parse_items(e, syn, vend, false, depth); !s) {
                        // A broken private sequence (odd group, defined length) is kept as its
                        // raw bytes (VR UN) and parsing resumes after it.
                        // A sequence only the private dictionary called SQ is a guess: it falls
                        // back to UN bytes in strict mode too.
                        const bool dict_sq = e.read.private_vr == VR::SQ;
                        if ((!opt_.lenient && !dict_sq) || (tag_group(tag) & 1u) == 0 ||
                            (s.code != Errc::Malformed && s.code != Errc::Truncated))
                            return s;
                        warn("private sequence " + hex_tag(tag) + " unreadable (" + s.message + "); kept as UN bytes");
                        e.items.clear();
                        e.vr = VR::UN;
                        e.read.private_vr = VR::None;
                        e.value = buf_.sub(vstart, static_cast<std::size_t>(vend) - vstart);
                    }
                    if (Status s = advance_to(vend, tag); !s) return s;
                } else {
                    std::size_t unit = 0;
                    // Pixel samples wider than 16 bits stored as OW are big-endian per sample, not
                    // per 16-bit word: swap by Bits Allocated so memory holds true little endian
                    // (the writer does the inverse).
                    if (syn.big_endian && depth == 0 && tag == tags::PixelData && e.vr == VR::OW &&
                        (bits_allocated_ == 32 || bits_allocated_ == 64))
                        unit = bits_allocated_ / 8u;
                    if (syn.pixel_big_endian && depth == 0 && tag == tags::PixelData && e.vr == VR::OW &&
                        (bits_allocated_ == 16 || bits_allocated_ == 32 || bits_allocated_ == 64))
                        e.value = swapped_copy(pos_, len, bits_allocated_ / 8u);
                    else
                        e.value = value_bytes(e.vr, pos_, len, syn, unit);
                    if (depth == 0 && tag == tags::BitsAllocated && e.value.size() >= 2)
                        bits_allocated_ = load_le16(e.value.data());
                    if (tag == tags::PixelRepresentation && e.value.size() >= 2)
                        pixel_rep_ = load_le16(e.value.data()) == 0 ? 0 : 1;
                    if (opt_.private_dictionary && !syn.big_endian && tag_is_private(tag) &&
                        tag_element(tag) >= 0x0010 && tag_element(tag) <= 0x00FF && vr_is_string(e.vr)) {
                        std::string v(e.value.as_chars());
                        bool found = false;
                        for (auto& c : creators)
                            if (c.first == tag) {
                                c.second = std::move(v);
                                found = true;
                                break;
                            }
                        if (!found) creators.emplace_back(tag, std::move(v));
                    }
                    if (Status s = advance_to(vend, tag); !s) return s;
                }
            }
            if (ordered && !ds.empty() && !(ds.elements().back().tag < e.tag)) ordered = false;
            ds.append_unsorted(std::move(e));
            groups.content_end = pos_;
            groups.element_done(tag, pos_);
        }
    }

    bool stopped_at_pixels() const noexcept { return stopped_at_pixels_; }
    std::uint64_t stop_offset() const noexcept { return stop_offset_; }

private:
    // Moves the cursor forward to `to`. Children are bounded by their parent (limit_), so the
    // cursor can never be past `to` here; the cursor never moves backwards (R-001). If a child did
    // overrun its parent that is a parser bug: report it as Internal in every build instead of
    // asserting only in Debug (R-051).
    [[nodiscard]] Status advance_to(std::uint64_t to, Tag tag) {
        if (pos_ > to)
            return Status(Errc::Internal, "parser cursor moved past the end of its parent", pos_, tag);
        pos_ = static_cast<std::size_t>(to);
        return Status::ok();
    }

    static std::string tag_text(Tag t) {
        static const char* hex = "0123456789ABCDEF";
        std::string s = "(gggg,eeee)";
        for (int i = 0; i < 4; ++i) {
            s[1 + static_cast<std::size_t>(i)] = hex[(t >> (28 - 4 * i)) & 0xFu];
            s[6 + static_cast<std::size_t>(i)] = hex[(t >> (12 - 4 * i)) & 0xFu];
        }
        return s;
    }
    // Records, on each group length element (gggg,0000), the number of bytes its group actually
    // occupied as read, so the writer can tell an unchanged group from an edited one (F1).
    // Looks elements up by tag, so it does not depend on how the data set stores them.
    struct GroupLengthTracker {
        Dataset& ds;
        std::size_t content_end;
        Tag pending = 0;
        std::size_t pending_index = 0;
        bool active = false;
        std::size_t body_start = 0;

        void element_starts(Tag t) {
            if (active && tag_group(t) != tag_group(pending)) finish();
        }
        void element_done(Tag t, std::size_t after) {
            if (tag_element(t) != 0 || tag_group(t) == 0xFFFE) return;
            if (active) finish();
            pending = t;
            pending_index = ds.elements().empty() ? 0 : ds.elements().size() - 1;
            active = true;
            body_start = after;
        }
        void finish() {
            active = false;
            // The reader appends unsorted (R-002), so look the element up by its position first.
            Element* g = nullptr;
            auto& els = ds.elements();
            if (pending_index < els.size() && els[pending_index].tag == pending) g = &els[pending_index];
            if (!g) g = ds.find(pending);
            if (!g || content_end < body_start) return;
            const std::size_t n = content_end - body_start;
            g->read.group_bytes = n > 0xFFFFFFFEu ? 0xFFFFFFFEu : static_cast<std::uint32_t>(n);
        }
        ~GroupLengthTracker() {
            if (active) finish();
        }
    };

    Tag read_tag(Syntax syn) {
        const std::uint8_t* q = p_ + pos_;
        const std::uint16_t g = syn.big_endian ? load_be16(q) : load_le16(q);
        const std::uint16_t el = syn.big_endian ? load_be16(q + 2) : load_le16(q + 2);
        pos_ += 4;
        return make_tag(g, el);
    }

    std::uint32_t read_u32(Syntax syn) {
        const std::uint32_t v = syn.big_endian ? load_be32(p_ + pos_) : load_le32(p_ + pos_);
        pos_ += 4;
        return v;
    }

    Status read_vr_and_length(Syntax syn, Tag tag, VR& vr, std::uint32_t& len, bool& inferred) {
        // Item-level tags (FFFE,xxxx) never carry a VR.
        if (tag_group(tag) == 0xFFFE) {
            vr = VR::None;
            len = read_u32(syn);
            return Status::ok();
        }
        if (!syn.explicit_vr) {
            vr = dict::implicit_vr(tag);
            inferred = true;
            len = read_u32(syn);
            return Status::ok();
        }
        const char a = static_cast<char>(p_[pos_]);
        const char b = static_cast<char>(p_[pos_ + 1]);
        vr = vr_from_chars(a, b);
        if (!vr_is_known(vr)) {
            // Some writers emit implicit VR inside explicit data, or junk VRs. Lenient: treat the
            // next 4 bytes as an implicit length if that fits, otherwise fail.
            if (!opt_.lenient) return Status(Errc::Malformed, "unknown VR", pos_, tag);
            warn("unknown VR; read as implicit");
            vr = dict::implicit_vr(tag);
            inferred = true;
            len = read_u32(syn);
            return Status::ok();
        }
        pos_ += 2;
        if (vr_has_long_length(vr)) {
            if (limit_ - pos_ < 6) return Status(Errc::Truncated, "truncated element header", pos_, tag);
            pos_ += 2;  // reserved
            len = read_u32(syn);
        } else {
            len = syn.big_endian ? load_be16(p_ + pos_) : load_le16(p_ + pos_);
            pos_ += 2;
        }
        return Status::ok();
    }

    Status parse_items(Element& e, Syntax syn, std::uint64_t end, bool undefined, std::uint32_t depth) {
        const std::size_t saved_limit = limit_;
        if (!undefined) {
            assert(end <= limit_);
            limit_ = static_cast<std::size_t>(end);
        }
        Status s = parse_items_in(e, syn, end, undefined, depth);
        limit_ = saved_limit;
        return s;
    }

    Status parse_items_in(Element& e, Syntax syn, std::uint64_t end, bool undefined, std::uint32_t depth) {
        while (true) {
            if (!undefined && pos_ >= end) return Status::ok();
            if (limit_ - pos_ < 8) {
                if (undefined) return lenient_or(Errc::Truncated, "missing sequence delimiter");
                return Status(Errc::Truncated, "truncated item header", pos_, e.tag);
            }
            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            const std::uint32_t len = read_u32(syn);
            if (tag == tags::SequenceDelimitationItem) {
                if (undefined) return Status::ok();
                warn("sequence delimiter in defined-length sequence");
                continue;
            }
            if (tag != tags::Item)
                return Status(Errc::Malformed, "expected item in sequence", start, e.tag);
            if (e.items.size() >= opt_.max_items_per_sequence)
                return Status(Errc::LimitExceeded, "too many items in sequence", start, e.tag);
            Dataset item;
            item.set_item_length_mode(len == kUndefinedLength ? Dataset::kItemLengthUndefined
                                                              : Dataset::kItemLengthExplicit);
            if (len == kUndefinedLength) {
                if (Status s = parse(item, syn, 0, true, depth + 1); !s) return s;
            } else {
                std::uint64_t iend = 0;
                if (!checked_add(pos_, len, iend) || iend > limit_) {
                    const bool in_parent = limit_ < size_;
                    if (!opt_.lenient)
                        return in_parent ? Status(Errc::Malformed, "item runs past end of parent sequence", start, e.tag)
                                         : Status(Errc::Truncated, "item runs past end of data", start, e.tag);
                    warn(in_parent ? "item runs past end of parent; clamped" : "item truncated at end of data");
                    if (!in_parent) note_truncated(start);
                    iend = limit_;
                }
                if (Status s = parse(item, syn, iend, false, depth + 1); !s) return s;
                if (Status s = advance_to(iend, e.tag); !s) return s;
            }
            e.items.push_back(std::move(item));
        }
    }

    // Every scan and cursor move here is bounded by limit_, the end of the innermost enclosing
    // defined-length container, not by the end of the data: an undefined-length OB inside a
    // defined-length item must never take bytes that belong to the item's parent (R-051).
    // Strict mode fails with Malformed (inside a parent) or Truncated (top level); lenient mode
    // clamps to limit_ and warns.
    Status parse_fragments(Element& e, Syntax syn) {
        const bool in_parent = limit_ < size_;
        while (true) {
            if (limit_ - pos_ < 8) return lenient_or(Errc::Truncated, "missing pixel data sequence delimiter");
            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            std::uint32_t len = read_u32(syn);
            if (tag == tags::SequenceDelimitationItem) return Status::ok();
            if (tag != tags::Item) {
                if (!opt_.lenient) return Status(Errc::Malformed, "expected fragment item", start, e.tag);
                if (e.fragments.empty()) {
                    // No item structure at all: the bytes up to the sequence delimiter become one
                    // fragment, after an empty Basic Offset Table.
                    static constexpr std::uint8_t delim[8] = {0xFE, 0xFF, 0xDD, 0xE0, 0, 0, 0, 0};
                    std::size_t d = start;
                    while (limit_ - d >= 8 && std::memcmp(p_ + d, delim, 8) != 0) ++d;
                    const bool found = limit_ - d >= 8;
                    const std::size_t stop = found ? d : limit_;
                    warn("encapsulated pixel data without items; " + std::to_string(stop - start) +
                         " bytes read as one fragment" +
                         (found ? std::string() : in_parent ? "; no delimiter before the end of the parent, clamped"
                                                            : "; no delimiter before the end of the data"));
                    if (!found && !in_parent) note_truncated(start);
                    e.fragments.push_back(Bytes());
                    e.fragments.push_back(buf_.sub(start, stop - start));
                    pos_ = found ? stop + 8 : limit_;
                    return Status::ok();
                }
                warn("pixel data fragments end without a sequence delimiter; " + std::to_string(limit_ - start) +
                     " trailing bytes ignored" + (in_parent ? " up to the end of the parent" : ""));
                note_skipped(start, limit_);
                pos_ = limit_;
                return Status::ok();
            }
            if (e.fragments.size() >= opt_.max_fragments)
                return Status(Errc::LimitExceeded, "too many pixel data fragments", start, e.tag);
            std::uint64_t fend = 0;
            if (len == kUndefinedLength || !checked_add(pos_, len, fend) || fend > limit_) {
                if (!opt_.lenient)
                    return in_parent ? Status(Errc::Malformed, "fragment runs past end of parent item", start, e.tag)
                                     : Status(Errc::Truncated, "fragment runs past end of data", start, e.tag);
                if (in_parent && len != kUndefinedLength) {
                    // A defined length past the parent: clamp this fragment and keep reading.
                    warn("fragment runs past end of parent; clamped");
                    len = static_cast<std::uint32_t>(limit_ - pos_);
                    fend = limit_;
                } else {
                    warn(in_parent ? "pixel data fragment of undefined length; clamped at end of parent"
                                   : "last pixel data fragment truncated at end of data");
                    if (!in_parent) note_truncated(start);
                    if (pos_ < limit_) e.fragments.push_back(buf_.sub(pos_, limit_ - pos_));
                    pos_ = limit_;
                    return Status::ok();
                }
            }
            e.fragments.push_back(buf_.sub(pos_, len));
            pos_ = static_cast<std::size_t>(fend);
        }
    }

    Bytes value_bytes(VR vr, std::size_t at, std::uint32_t len, Syntax syn, std::size_t unit = 0) {
        if (!syn.big_endian) return buf_.sub(at, len);
        // AT is a pair of 16-bit values (group, element), so it swaps in 16-bit units (PS3.5 6.2).
        const std::size_t w = unit ? unit : (vr == VR::AT ? 2 : vr_value_size(vr));
        if (w <= 1) return buf_.sub(at, len);
        // Big endian: swap to little endian in a private copy.
        Bytes copy = Bytes::copy_of(p_ + at, len);
        auto* q = const_cast<std::uint8_t*>(copy.data());
        for (std::size_t i = 0; i + w <= len; i += w)
            for (std::size_t k = 0; k < w / 2; ++k) std::swap(q[i + k], q[i + w - 1 - k]);
        return copy;
    }

    Bytes swapped_copy(std::size_t at, std::uint32_t len, std::size_t w) {
        Bytes copy = Bytes::copy_of(p_ + at, len);
        auto* q = const_cast<std::uint8_t*>(copy.data());
        for (std::size_t i = 0; i + w <= len; i += w)
            for (std::size_t k = 0; k < w / 2; ++k) std::swap(q[i + k], q[i + w - 1 - k]);
        return copy;
    }

    Status lenient_or(Errc code, const char* msg) {
        if (opt_.lenient) {
            warn(msg);
            // Whatever is left of the container is skipped; at the end of the data the input
            // itself ended early (P0-1: both are recorded, never only warned about).
            note_skipped(pos_, limit_);
            if (limit_ == size_) note_truncated(pos_);
            pos_ = limit_;  // the end of the innermost container: forward, never past the parent
            return Status::ok();
        }
        return Status(code, msg, pos_);
    }

    // Records input bytes [from, to) that end up in no element (DicomFile::kReadBytesSkipped).
    void note_skipped(std::size_t from, std::size_t to) noexcept {
        if (to <= from) return;
        note_loss(DicomFile::kReadBytesSkipped, from);
        bytes_skipped_ += to - from;
    }
    // The input ended inside an element, item or sequence starting at `at`.
    void note_truncated(std::size_t at) noexcept { note_loss(DicomFile::kReadInputTruncated, at); }
    void note_loss(std::uint32_t bit, std::size_t at) noexcept {
        if (loss_ == 0 || at < loss_offset_) loss_offset_ = at;
        loss_ |= bit;
    }

public:
    std::uint32_t loss() const noexcept { return loss_; }
    std::uint64_t loss_offset() const noexcept { return loss_offset_; }
    std::uint64_t bytes_skipped() const noexcept { return bytes_skipped_; }

private:
    void warn(std::string msg) {
        if (warnings_ && warnings_->size() < 1000)
            warnings_->push_back(std::move(msg) + " at offset " + std::to_string(pos_));
    }

    Bytes buf_;
    const std::uint8_t* p_;
    std::size_t size_;
    std::size_t limit_;  // end of the innermost enclosing defined-length container (R-001)
    std::size_t pos_ = 0;
    const ReadOptions& opt_;
    std::vector<std::string>* warnings_;
    std::uint64_t element_count_ = 0;
    bool stopped_at_pixels_ = false;
    std::uint64_t stop_offset_ = 0;
    std::uint16_t bits_allocated_ = 0;
    int pixel_rep_ = -1;  // Pixel Representation in scope (-1 unknown), see parse() (F8)
    std::uint32_t loss_ = 0;
    std::uint64_t loss_offset_ = 0;
    std::uint64_t bytes_skipped_ = 0;
};

Result<Syntax> syntax_for(std::string_view uid, bool lenient, const TransferSyntaxInfo** info) {
    const TransferSyntaxInfo* t = find_transfer_syntax(uid);
    *info = t;
    if (t) return Syntax{t->explicit_vr, t->big_endian, t->pixel_big_endian};
    if (!lenient) return Status(Errc::Unsupported, "unknown transfer syntax");
    // Unknown (often private) transfer syntax: PS3.5 says encapsulated ones are explicit VR LE.
    return Syntax{true, false};
}

// Looks at the first element to guess the encoding of a raw data set (no file meta).
Syntax guess_syntax(const std::uint8_t* p, std::size_t n) {
    if (n < 8) return {false, false};
    const VR vr = vr_from_chars(static_cast<char>(p[4]), static_cast<char>(p[5]));
    const bool explicit_vr = vr_is_known(vr);
    // A plausible little-endian group is small (0x0002..0x0FFF typical for the first element).
    const std::uint16_t g_le = load_le16(p);
    const std::uint16_t g_be = load_be16(p);
    const bool big = explicit_vr && g_le > 0x00FF && g_be <= 0x00FF;
    return {explicit_vr, big};
}

// Copies what the parser lost into `out` (P0-1). `total` is the size of the parsed stream.
void record_loss(const Parser& p, std::size_t total, DicomFile& out) {
    if (p.loss()) {
        if (out.read_loss == 0 || p.loss_offset() < out.read_loss_offset) out.read_loss_offset = p.loss_offset();
        out.read_loss |= p.loss();
        out.bytes_not_read += p.bytes_skipped();
    }
    if (p.stopped_at_pixels()) {
        if (out.read_loss == 0 || p.stop_offset() < out.read_loss_offset) out.read_loss_offset = p.stop_offset();
        out.read_loss |= DicomFile::kReadHeaderOnly;
        if (total > p.stop_offset()) out.bytes_not_read += total - p.stop_offset();
    }
}

// Puts one summary warning with a fixed code first in the list, so an incomplete read is the
// first thing a caller sees (P0-1).
void flag_incomplete(DicomFile& out) {
    std::string w;
    const std::uint32_t lost = out.read_loss & ~static_cast<std::uint32_t>(DicomFile::kReadHeaderOnly);
    if (lost == 0) return;  // a header-only read is the caller's own choice: no warning
    const std::string at = " (first at offset " + std::to_string(out.read_loss_offset) + ")";
    if (out.read_loss & DicomFile::kReadBytesSkipped) {
        w = std::string(kWarnParseIncomplete) + ": parsing skipped " + std::to_string(out.bytes_not_read) +
            " input bytes that are in no element" + at;
    } else {
        w = std::string(kWarnInputTruncated) + ": the data ends inside an element, item or sequence" + at;
    }
    if ((out.read_loss & DicomFile::kReadBytesSkipped) && (out.read_loss & DicomFile::kReadInputTruncated))
        w += "; the data also ends early (" + std::string(kWarnInputTruncated) + ")";
    w += "; a save of this data set is refused unless incomplete saves are allowed";
    out.warnings.insert(out.warnings.begin(), std::move(w));
}

Status parse_body(const Bytes& data, std::size_t start, std::string_view ts_uid,
                  const ReadOptions& opt, DicomFile& out) {
    const TransferSyntaxInfo* info = nullptr;
    auto syn = syntax_for(ts_uid, opt.lenient, &info);
    if (!syn) return syn.take_error();
    if (!info) out.warnings.push_back("unknown transfer syntax; read as explicit VR little endian");

    if (info && info->codec == Codec::Deflate) {
        auto store = std::make_shared<std::vector<std::uint8_t>>();
        if (Status s = codec::inflate_raw(data.data() + start, data.size() - start,
                                          opt.max_inflated_size > SIZE_MAX
                                              ? SIZE_MAX
                                              : static_cast<std::size_t>(opt.max_inflated_size),
                                          *store);
            !s)
            return s;
        const std::uint8_t* q = store->data();
        Bytes inflated(std::shared_ptr<const void>(store, store.get()), q, store->size());
        out.offsets_are_file_offsets = false;
        Parser p(inflated, opt, &out.warnings);
        Status s = p.parse(out.dataset, *syn, inflated.size(), false, 0);
        out.stopped_at_offset = p.stop_offset();
        record_loss(p, inflated.size(), out);
        return s;
    }
    Parser p(data, opt, &out.warnings);
    p.seek(start);
    Status s = p.parse(out.dataset, *syn, data.size(), false, 0);
    out.stopped_at_offset = p.stop_offset();
    record_loss(p, data.size(), out);
    return s;
}

}  // namespace

bool has_part10_header(const std::uint8_t* data, std::size_t size) noexcept {
    return size >= 132 && data[128] == 'D' && data[129] == 'I' && data[130] == 'C' && data[131] == 'M';
}

Result<DicomFile> read_part10(const Bytes& data, const ReadOptions& opt) {
    DicomFile out;
    const std::uint8_t* p = data.data();
    const std::size_t n = data.size();
    std::size_t pos = 0;

    if (has_part10_header(p, n)) {
        out.had_preamble = true;
        out.preamble = data.sub(0, 128);
        pos = 132;
    } else if (n >= 4 && p[0] == 'D' && p[1] == 'I' && p[2] == 'C' && p[3] == 'M') {
        out.warnings.push_back("file has 'DICM' but no preamble");
        pos = 4;
    } else {
        if (!opt.allow_raw_dataset) return Status(Errc::NotDicom, "no DICOM preamble");
        const Syntax g = guess_syntax(p, n);
        std::string_view uid = g.big_endian   ? ts::ExplicitVRBigEndian
                               : g.explicit_vr ? ts::ExplicitVRLittleEndian
                                               : ts::ImplicitVRLittleEndian;
        // A raw file may still start with a group 0002 meta block.
        if (n >= 8 && load_le16(p) == 0x0002) {
            Parser mp(data, opt, &out.warnings);
            if (Status s = mp.parse(out.meta, Syntax{true, false}, n, false, 0, 0x0002); !s) return s;
            record_loss(mp, n, out);
            pos = mp.pos();
            out.meta_as_read = !out.meta.empty();
            if (auto t = out.meta.get_string(tags::TransferSyntaxUID)) {
                out.transfer_syntax = *t;
                uid = out.transfer_syntax;
            }
        } else {
            out.transfer_syntax = std::string(uid);
        }
        if (Status s = parse_body(data, pos, uid, opt, out); !s) return s;
        if (out.transfer_syntax.empty()) out.transfer_syntax = std::string(uid);
        flag_incomplete(out);
        return out;
    }

    // File meta information: always explicit VR little endian (PS3.10 7.1).
    {
        Parser mp(data, opt, &out.warnings);
        mp.seek(pos);
        if (Status s = mp.parse(out.meta, Syntax{true, false}, n, false, 0, 0x0002); !s) return s;
        record_loss(mp, n, out);
        pos = mp.pos();
        out.meta_as_read = !out.meta.empty();
    }
    auto ts_uid = out.meta.get_string(tags::TransferSyntaxUID);
    if (!ts_uid || ts_uid->empty()) {
        if (!opt.lenient) return Status(Errc::Malformed, "file meta has no transfer syntax UID");
        out.warnings.push_back("missing transfer syntax; guessed from data");
        const Syntax g = guess_syntax(p + pos, n - pos);
        ts_uid = std::string(g.big_endian    ? ts::ExplicitVRBigEndian
                             : g.explicit_vr ? ts::ExplicitVRLittleEndian
                                             : ts::ImplicitVRLittleEndian);
    }
    out.transfer_syntax = *ts_uid;
    if (Status s = parse_body(data, pos, out.transfer_syntax, opt, out); !s) return s;
    flag_incomplete(out);
    return out;
}

Result<Dataset> read_dataset(const Bytes& data, std::string_view transfer_syntax_uid,
                             const ReadOptions& opt) {
    DicomFile f;
    if (Status s = parse_body(data, 0, transfer_syntax_uid, opt, f); !s) return s;
    return std::move(f.dataset);
}

Result<DicomFile> read_file(const std::string& utf8_path, const ReadOptions& opt) {
    auto bytes = io::map_file(utf8_path, !opt.stop_before_pixel_data);
    if (!bytes) return bytes.take_error();
    return read_part10(*bytes, opt);
}

}  // namespace siaara
