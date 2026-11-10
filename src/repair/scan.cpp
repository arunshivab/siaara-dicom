#include "repair/scan.hpp"

#include <algorithm>

#include "codec/deflate.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"

namespace siaara::repair {

namespace {

struct Syntax {
    bool explicit_vr = true;
    bool big_endian = false;
    bool encapsulated = false;  // encapsulated (or unknown) transfer syntax
};

bool is_upper(std::uint8_t c) { return c >= 'A' && c <= 'Z'; }

bool is_pixel_tag(Tag t) {
    return t == tags::PixelData || t == tags::FloatPixelData || t == tags::DoubleFloatPixelData;
}

VR implicit_vr_of(Tag tag) {
    // Private creator elements (gggg,0010-00FF) are LO (PS3.5 7.8.1).
    if (tag_is_private(tag) && tag_element(tag) >= 0x0010 && tag_element(tag) <= 0x00FF) return VR::LO;
    if (tag_element(tag) == 0x0000) return VR::UL;
    return dict::implicit_vr(tag);
}

class Scanner {
public:
    Scanner(const Bytes& buf, const ScanOptions& opt, ScanResult& out, bool file_offsets)
        : buf_(buf), p_(buf.data()), size_(buf.size()), opt_(opt), out_(out), file_offsets_(file_offsets) {}

    std::size_t pos() const noexcept { return pos_; }
    void seek(std::size_t p) noexcept { pos_ = p; }
    bool stopped() const noexcept { return stop_; }
    const Status& limit() const noexcept { return limit_; }

    // Would an element header at `at` parse as explicit VR (known VR, reserved bytes zero)?
    bool looks_explicit(std::size_t at, Syntax syn) const {
        if (size_ - at < 8) return false;
        const VR vr = vr_from_chars(static_cast<char>(p_[at + 4]), static_cast<char>(p_[at + 5]));
        if (!vr_is_known(vr)) return false;
        if (vr_has_long_length(vr)) {
            if (size_ - at < 12) return false;
            if (p_[at + 6] != 0 || p_[at + 7] != 0) return false;
        }
        const std::uint16_t g = syn.big_endian ? load_be16(p_ + at) : load_le16(p_ + at);
        const std::uint16_t el = syn.big_endian ? load_be16(p_ + at + 2) : load_le16(p_ + at + 2);
        const Tag t = make_tag(g, el);
        if (const dict::Entry* e = dict::lookup(t)) {
            if (e->vr != VR::None && vr != e->vr && vr != e->vr_alt && vr != VR::UN &&
                !(e->vr == VR::OB || e->vr == VR::OW))
                return false;
        }
        return true;
    }
    // After an unterminated pixel sequence at top level: does an element start at `at`? Its tag
    // must follow `after` (ascending order), its header must parse in the syntax, and its value
    // (defined length) must end within the data.
    bool plausible_element_after(std::size_t at, Tag after, Syntax syn) const {
        if (size_ - at < 8) return false;
        const std::uint16_t g = syn.big_endian ? load_be16(p_ + at) : load_le16(p_ + at);
        const std::uint16_t el = syn.big_endian ? load_be16(p_ + at + 2) : load_le16(p_ + at + 2);
        const Tag t = make_tag(g, el);
        if (t <= after) return false;
        std::uint64_t len = 0, header = 8;
        if (syn.explicit_vr) {
            if (!looks_explicit(at, syn)) return false;
            const VR vr = vr_from_chars(static_cast<char>(p_[at + 4]), static_cast<char>(p_[at + 5]));
            if (vr_has_long_length(vr)) {
                header = 12;
                len = syn.big_endian ? load_be32(p_ + at + 8) : load_le32(p_ + at + 8);
            } else {
                len = syn.big_endian ? load_be16(p_ + at + 6) : load_le16(p_ + at + 6);
            }
        } else {
            if (dict::lookup(t) == nullptr && !tag_is_private(t)) return false;
            len = syn.big_endian ? load_be32(p_ + at + 4) : load_le32(p_ + at + 4);
        }
        if (len == kUndefinedLength) return true;
        return header + len <= size_ - at;
    }
    bool looks_implicit(std::size_t at, Syntax syn) const {
        if (size_ - at < 8) return false;
        const std::uint32_t len = syn.big_endian ? load_be32(p_ + at + 4) : load_le32(p_ + at + 4);
        return len == kUndefinedLength || len <= size_ - at - 8;
    }

    // Parses elements into `ds`. Returns when `end` is reached (defined length), an Item
    // Delimitation Item is consumed (undefined length; *delim = true), or the input ends.
    void parse(Dataset& ds, Syntax syn, std::uint64_t end, bool undefined, std::uint32_t depth, Path& path,
               std::uint16_t only_group = 0, bool* delim = nullptr) {
        if (depth > opt_.max_depth) {
            set_limit(Status(Errc::LimitExceeded, "sequence nesting too deep", pos_));
            return;
        }
        Tag prev = 0;
        bool have_prev = false;
        GroupTrack gt;
        // Values inside a defined-length item may not run past the item (R-001).
        const std::uint64_t saved_bound = bound_;
        if (!undefined) bound_ = std::min<std::uint64_t>(end, size_);
        struct RestoreBound {
            std::uint64_t& ref;
            std::uint64_t v;
            ~RestoreBound() { ref = v; }
        } restore_bound{bound_, saved_bound};
        while (!stop_) {
            if (!undefined && pos_ >= end) {
                if (pos_ > end)
                    add(Severity::Error, "item-length", kStructural, path,
                        "elements run past the end of the item", pos_);
                break;
            }
            if (pos_ >= size_) {
                if (undefined && depth > 0) {
                    ++out_.unterminated;
                    add(Severity::Error, "unterminated-item", kStructural, path,
                        "item has no Item Delimitation Item before the end of the data", pos_);
                }
                break;
            }
            if (size_ - pos_ < 8) {
                if (all_zero(pos_, size_)) {
                    add(Severity::Warning, "trailing-bytes", kStructural, path,
                        std::to_string(size_ - pos_) + " trailing zero byte(s) after the data set", pos_);
                } else {
                    add(Severity::Error, "truncated", kStructural, path,
                        "data ends inside an element header", pos_);
                }
                pos_ = size_;
                break;
            }
            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            if (depth == 0 && only_group == 0 && tag == 0 && all_zero(start, size_)) {
                add(Severity::Warning, "trailing-bytes", kStructural, path,
                    std::to_string(size_ - start) + " trailing zero byte(s) after the data set", start);
                pos_ = size_;
                break;
            }
            if (only_group != 0 && tag_group(tag) != only_group) {
                pos_ = start;
                break;
            }
            if (tag == tags::ItemDelimitationItem) {
                pos_ += 4;
                if (undefined && depth > 0) {
                    if (delim) *delim = true;
                    break;
                }
                add(Severity::Warning, "stray-delimiter", kStructural, path, "stray Item Delimitation Item", start);
                continue;
            }
            if (tag == tags::SequenceDelimitationItem) {
                if (undefined && depth > 0) {
                    // The item ended without its delimiter; let the sequence see this one.
                    ++out_.unterminated;
                    add(Severity::Error, "unterminated-item", kStructural, path,
                        "item has no Item Delimitation Item before the Sequence Delimitation Item", start);
                    pos_ = start;
                    break;
                }
                pos_ += 4;
                add(Severity::Warning, "stray-delimiter", kStructural, path, "stray Sequence Delimitation Item", start);
                continue;
            }
            if (tag_group(tag) == 0xFFFE) {
                const std::uint32_t len = read_u32(syn);
                add(Severity::Error, "stray-item", kStructural, path, "item tag outside a sequence", start);
                if (len == kUndefinedLength || len > size_ - pos_) {
                    pos_ = size_;
                    break;
                }
                pos_ += len;
                continue;
            }

            Element e;
            e.tag = tag;
            e.offset = file_offsets_ ? start : 0;
            e.read.parsed = true;
            std::uint32_t len = 0;
            if (!read_vr_and_length(syn, tag, e.vr, len, path, start)) break;
            if (++element_count_ > opt_.max_elements) {
                set_limit(Status(Errc::LimitExceeded, "too many elements", start, tag));
                break;
            }
            // Group length bookkeeping (PS3.5 7.2).
            if (gt.active && tag_group(tag) != gt.group) finish_group(gt, start, path, ds);

            Path here = path;
            here.push_back({tag, -1});
            if (len == kUndefinedLength) {
                e.undefined_length = true;
                if (e.vr == VR::SQ || (e.vr == VR::UN && !is_pixel_tag(tag))) {
                    const Syntax inner = e.vr == VR::UN ? Syntax{false, false} : syn;
                    e.read.un_sequence = e.vr == VR::UN;
                    e.vr = VR::SQ;
                    parse_items(e, inner, 0, true, depth, here);
                } else if (is_pixel_tag(tag) || e.vr == VR::OB || e.vr == VR::OW || e.vr == VR::UN) {
                    parse_fragments(e, syn, here, start);
                    e.read.fragment_count = static_cast<std::uint32_t>(e.fragments.size());
                } else {
                    add(Severity::Error, "undefined-length", kStructural, here,
                        "undefined length on an element that is not a sequence or pixel data", start);
                    pos_ = size_;
                    break;
                }
            } else {
                std::uint64_t vend = 0;
                std::uint32_t avail_len = len;
                const std::uint64_t limit = std::min<std::uint64_t>(bound_, size_);
                const bool overrun = !checked_add(pos_, len, vend) || vend > limit;
                if (overrun) {
                    avail_len = static_cast<std::uint32_t>(limit - pos_);
                    vend = limit;
                    const bool in_parent = limit < size_;
                    add(Severity::Error, in_parent ? "item-length" : "value-truncated", in_parent ? kStructural : kRescue,
                        here,
                        "value length " + std::to_string(len) + " runs past the end of the " +
                            (in_parent ? "enclosing item" : "data") + " (" + std::to_string(avail_len) +
                            " byte(s) available)",
                        start);
                }
                e.read.length = avail_len;
                if ((len & 1u) != 0 && !overrun) {
                    ++out_.odd_lengths;
                    add(Severity::Error, "odd-length", kStructural, here,
                        "value length " + std::to_string(len) + " is odd (PS3.5 7.1.1)", start);
                }
                if (e.vr == VR::SQ) {
                    parse_items(e, syn, vend, false, depth, here);
                    pos_ = static_cast<std::size_t>(vend);
                } else if (depth == 0 && tag == tags::PixelData && syn.encapsulated && !overrun &&
                           defined_length_fragments(e, len, static_cast<std::size_t>(vend))) {
                    // Testing 10b item 5: encapsulated Pixel Data with a defined length.
                    out_.pixel.present = true;
                    out_.pixel.encapsulated = true;
                    out_.pixel.tag = tag;
                    out_.pixel.offset = start;
                    out_.pixel.declared_length = len;
                    add(Severity::Warning, "pixel-defined-length", 0, here,
                        "encapsulated Pixel Data has a defined length (" + std::to_string(len) +
                            " bytes) instead of undefined length (PS3.5 A.4); read as " +
                            std::to_string(e.fragments.size()) + " item(s), kept as read",
                        start);
                    pos_ = static_cast<std::size_t>(vend);
                } else {
                    std::size_t unit = 0;
                    if (syn.big_endian && depth == 0 && tag == tags::PixelData && e.vr == VR::OW &&
                        (bits_allocated_ == 32 || bits_allocated_ == 64))
                        unit = bits_allocated_ / 8u;
                    e.value = value_bytes(e.vr, pos_, avail_len, syn, unit);
                    if (depth == 0 && tag == tags::BitsAllocated && e.value.size() >= 2)
                        bits_allocated_ = load_le16(e.value.data());
                    pos_ = static_cast<std::size_t>(vend);
                    if (depth == 0 && is_pixel_tag(tag)) {
                        out_.pixel.present = true;
                        out_.pixel.tag = tag;
                        out_.pixel.offset = start;
                        out_.pixel.declared_length = len;
                        out_.pixel.value_truncated = overrun;
                    }
                }
                if (tag_element(tag) == 0x0000 && e.value.size() == 4 && !overrun) {
                    gt.active = true;
                    gt.group = tag_group(tag);
                    gt.declared = load_le32(e.value.data());
                    gt.start = pos_;
                    gt.offset = start;
                }
            }
            // Order and duplicates (PS3.5 7.1: ascending tag order, each tag at most once).
            if (have_prev && tag < prev) {
                ++out_.out_of_order;
                add(Severity::Error, "element-order", kStructural, here,
                    "element is out of ascending tag order (after " + tag_string(prev) + ")", start);
            }
            if (!have_prev || tag > prev) prev = tag;
            have_prev = true;
            if (Element* old = ds.find(tag)) {
                ++out_.duplicates;
                const bool old_empty = old->value.empty() && old->items.empty() && old->fragments.empty();
                const bool new_empty = e.value.empty() && e.items.empty() && e.fragments.empty();
                bool keep_new = opt_.duplicates == DuplicatePolicy::KeepLast;
                if (opt_.duplicates == DuplicatePolicy::KeepFirst && old_empty && !new_empty) keep_new = true;
                const std::uint64_t kept = keep_new ? start : old->offset;
                const std::uint64_t dropped = keep_new ? old->offset : start;
                add(Severity::Error, "duplicate-element", kStructural, here,
                    "element appears more than once; repair keeps the copy at offset " + std::to_string(kept) +
                        " and drops the copy at offset " + std::to_string(dropped),
                    start);
                if (keep_new) *old = std::move(e);
            } else {
                ds.put(std::move(e));
            }
            if (stop_) break;
        }
        if (gt.active) finish_group(gt, std::min<std::size_t>(pos_, size_), path, ds);
    }

private:
    struct GroupTrack {
        bool active = false;
        std::uint16_t group = 0;
        std::uint32_t declared = 0;
        std::size_t start = 0;
        std::uint64_t offset = 0;
    };

    void finish_group(GroupTrack& gt, std::size_t end, const Path& path, Dataset& ds) {
        GroupLengthInfo g;
        g.path = path;
        g.group = gt.group;
        g.declared = gt.declared;
        g.actual = end >= gt.start ? end - gt.start : 0;
        g.offset = gt.offset;
        if (Element* ge = ds.find(make_tag(gt.group, 0)); ge && ge->offset == (file_offsets_ ? gt.offset : 0))
            ge->read.group_bytes = g.actual > 0xFFFFFFFEu ? 0xFFFFFFFEu : static_cast<std::uint32_t>(g.actual);
        Path here = path;
        here.push_back({make_tag(gt.group, 0), -1});
        if (g.actual != g.declared) {
            add(Severity::Error, gt.group == 0x0002 ? "meta-group-length" : "group-length-mismatch", kStructural,
                here,
                "group length is " + std::to_string(g.declared) + " but the group's elements take " +
                    std::to_string(g.actual) + " byte(s)",
                gt.offset);
        } else if (gt.group != 0x0002) {
            add(Severity::Info, "group-length-present", kStructural, here,
                "group length element present (retired outside the file meta, PS3.5 7.2)", gt.offset);
        }
        out_.group_lengths.push_back(std::move(g));
        gt.active = false;
    }

    bool all_zero(std::size_t from, std::size_t to) const {
        for (std::size_t i = from; i < to; ++i)
            if (p_[i] != 0) return false;
        return true;
    }

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

    bool read_vr_and_length(Syntax syn, Tag tag, VR& vr, std::uint32_t& len, const Path& path, std::size_t start) {
        if (!syn.explicit_vr) {
            vr = implicit_vr_of(tag);
            len = read_u32(syn);
            return true;
        }
        const std::uint8_t a = p_[pos_];
        const std::uint8_t b = p_[pos_ + 1];
        vr = vr_from_chars(static_cast<char>(a), static_cast<char>(b));
        Path here = path;
        here.push_back({tag, -1});
        if (!vr_is_known(vr)) {
            if (is_upper(a) && is_upper(b)) {
                // An unknown VR: PS3.5 7.1.2 layout for future VRs (2 reserved bytes, 32-bit length).
                add(Severity::Error, "invalid-vr", kValues, here, "unknown VR; read as UN", start);
                if (size_ - pos_ < 8) {
                    add(Severity::Error, "truncated", kStructural, here, "data ends inside an element header", start);
                    pos_ = size_;
                    return false;
                }
                pos_ += 4;
                vr = VR::UN;
                len = read_u32(syn);
                return true;
            }
            add(Severity::Error, "invalid-vr", kStructural, here,
                "element has no explicit VR (implicit VR encoding inside explicit VR data); read as implicit",
                start);
            vr = implicit_vr_of(tag);
            len = read_u32(syn);
            return true;
        }
        pos_ += 2;
        if (vr_has_long_length(vr)) {
            if (size_ - pos_ < 6) {
                add(Severity::Error, "truncated", kStructural, here, "data ends inside an element header", start);
                pos_ = size_;
                return false;
            }
            pos_ += 2;
            len = read_u32(syn);
        } else {
            len = syn.big_endian ? load_be16(p_ + pos_) : load_le16(p_ + pos_);
            pos_ += 2;
        }
        return true;
    }

    void parse_items(Element& e, Syntax syn, std::uint64_t end, bool undefined, std::uint32_t depth, Path& path) {
        while (!stop_) {
            if (!undefined && pos_ >= end) {
                if (pos_ > end)
                    add(Severity::Error, "item-length", kStructural, path, "items run past the sequence length", pos_);
                return;
            }
            if (size_ - pos_ < 8) {
                ++out_.unterminated;
                add(Severity::Error, undefined ? "unterminated-sequence" : "truncated", kStructural, path,
                    undefined ? "sequence has no Sequence Delimitation Item before the end of the data"
                              : "data ends inside a sequence",
                    pos_);
                pos_ = size_;
                return;
            }
            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            const std::uint32_t len = read_u32(syn);
            if (tag == tags::SequenceDelimitationItem) {
                if (undefined) return;
                add(Severity::Warning, "stray-delimiter", kStructural, path,
                    "Sequence Delimitation Item in a defined-length sequence", start);
                continue;
            }
            if (tag != tags::Item) {
                if (undefined) {
                    // The next data element follows: the sequence was not closed.
                    ++out_.unterminated;
                    add(Severity::Error, "unterminated-sequence", kStructural, path,
                        "sequence ends without a Sequence Delimitation Item", start);
                    pos_ = start;
                    return;
                }
                add(Severity::Error, "expected-item", kStructural, path,
                    "sequence content is not an item; rest of the sequence skipped", start);
                pos_ = static_cast<std::size_t>(std::min<std::uint64_t>(end, size_));
                return;
            }
            if (e.items.size() >= opt_.max_items_per_sequence) {
                set_limit(Status(Errc::LimitExceeded, "too many items in sequence", start, e.tag));
                return;
            }
            Path item_path = path;
            item_path.back().item = static_cast<std::int32_t>(e.items.size());
            Dataset item;
            if (len == kUndefinedLength) {
                bool delim = false;
                parse(item, syn, 0, true, depth + 1, item_path, 0, &delim);
            } else {
                std::uint64_t iend = 0;
                if (!checked_add(pos_, len, iend) || iend > size_) {
                    add(Severity::Error, "value-truncated", kStructural, item_path,
                        "item length runs past the end of the data", start);
                    iend = size_;
                }
                if (!undefined && iend > end)
                    add(Severity::Error, "item-length", kStructural, item_path,
                        "item runs past the end of its sequence", start);
                parse(item, syn, iend, false, depth + 1, item_path);
                pos_ = static_cast<std::size_t>(iend);
            }
            e.items.push_back(std::move(item));
        }
    }

    // [pos_, vend) as a run of defined-length items (FFFE,E000), optionally ended by a Sequence
    // Delimitation Item at the very end: fills e.fragments (and the offsets) and returns true;
    // otherwise leaves e unchanged and returns false (the value is then read as bytes).
    bool defined_length_fragments(Element& e, std::uint32_t len, std::size_t vend) {
        if (len < 8 || vend > size_) return false;
        std::vector<Bytes> frags;
        std::vector<std::uint64_t> offsets;
        std::size_t at = pos_;
        std::uint8_t form = 1;
        while (at < vend) {
            if (vend - at < 8) return false;
            const std::uint32_t t = (std::uint32_t{load_le16(p_ + at)} << 16) | load_le16(p_ + at + 2);
            const std::uint32_t n = load_le32(p_ + at + 4);
            if (t == tags::SequenceDelimitationItem && n == 0 && at + 8 == vend && !frags.empty()) {
                form = 2;
                break;
            }
            if (t != tags::Item || n == kUndefinedLength || n > vend - at - 8) return false;
            if (frags.size() >= opt_.max_fragments) return false;
            if (!frags.empty()) offsets.push_back(at);
            frags.push_back(buf_.sub(at + 8, n));
            at += 8 + std::size_t{n};
        }
        if (frags.empty()) return false;
        e.fragments = std::move(frags);
        e.read.length = len;
        e.read.fragment_count = static_cast<std::uint32_t>(e.fragments.size());
        e.read.fragments_defined_length = form;
        if (!offsets.empty())
            for (std::uint64_t o : offsets) out_.pixel.item_offsets.push_back(o - offsets.front());
        return true;
    }

    void parse_fragments(Element& e, Syntax syn, const Path& path, std::size_t element_start) {
        PixelScan* ps = (is_pixel_tag(e.tag) && path.size() == 1) ? &out_.pixel : nullptr;
        if (ps) {
            ps->present = true;
            ps->encapsulated = true;
            ps->tag = e.tag;
            ps->offset = element_start;
        }
        std::uint64_t origin = 0;
        bool have_origin = false;
        while (!stop_) {
            if (size_ - pos_ < 8) {
                if (ps) ps->sequence_unterminated = true;
                add(Severity::Error, "pixel-sequence-unterminated", kRescue, path,
                    "encapsulated pixel data has no Sequence Delimitation Item before the end of the data", pos_);
                pos_ = size_;
                return;
            }
            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            const std::uint32_t len = read_u32(syn);
            if (tag == tags::SequenceDelimitationItem) return;
            if (tag != tags::Item) {
                if (ps) ps->sequence_unterminated = true;
                add(Severity::Error, "pixel-sequence-unterminated", kRescue, path,
                    "encapsulated pixel data ends without a Sequence Delimitation Item", start);
                pos_ = start;
                // What follows is read as elements only when it looks like one: a header that
                // parses, a tag after the pixel data, and a value that fits in the data. Junk after
                // the last fragment (gdcmData gdcm-JPEG-LossLess3a.dcm: 8 bytes) was read as an
                // element (B00C,0EB6) and written to the output, an element the input never had.
                // It is now reported as trailing bytes and not written (testing-chat repair finding).
                if (path.size() == 1 && !plausible_element_after(start, e.tag, syn)) {
                    add(Severity::Error, "trailing-bytes", kRescue, path,
                        std::to_string(size_ - start) +
                            " byte(s) after the last fragment are not an element; they are not written",
                        start);
                    out_.trailing_junk_bytes += size_ - start;
                    pos_ = size_;
                }
                return;
            }
            if (e.fragments.size() >= opt_.max_fragments) {
                set_limit(Status(Errc::LimitExceeded, "too many pixel data fragments", start, e.tag));
                return;
            }
            std::uint64_t fend = 0;
            if (len == kUndefinedLength || !checked_add(pos_, len, fend) || fend > size_) {
                if (ps) {
                    ps->last_fragment_truncated = true;
                    ps->truncated_declared = len;
                    ps->partial_fragment = buf_.sub(pos_, size_ - pos_);
                    ps->sequence_unterminated = true;
                }
                add(Severity::Error, "fragment-truncated", kRescue, path,
                    "fragment " + std::to_string(e.fragments.size()) + " runs past the end of the data (" +
                        std::to_string(size_ - pos_) + " of " + std::to_string(len) + " byte(s) present)",
                    start);
                pos_ = size_;
                return;
            }
            if ((len & 1u) != 0) {
                ++out_.odd_lengths;
                add(Severity::Error, "odd-length", kStructural, path,
                    "fragment " + std::to_string(e.fragments.size()) + " has odd length " + std::to_string(len), start);
            }
            if (!e.fragments.empty() && ps) {
                if (!have_origin) {
                    origin = start;
                    have_origin = true;
                }
                ps->item_offsets.push_back(start - origin);
            }
            e.fragments.push_back(buf_.sub(pos_, len));
            pos_ = static_cast<std::size_t>(fend);
        }
    }

    Bytes value_bytes(VR vr, std::size_t at, std::uint32_t len, Syntax syn, std::size_t unit) {
        if (!syn.big_endian) return buf_.sub(at, len);
        const std::size_t w = unit ? unit : (vr == VR::AT ? 2 : vr_value_size(vr));
        if (w <= 1) return buf_.sub(at, len);
        Bytes copy = Bytes::copy_of(p_ + at, len);
        auto* q = const_cast<std::uint8_t*>(copy.data());
        for (std::size_t i = 0; i + w <= len; i += w)
            for (std::size_t k = 0; k < w / 2; ++k) std::swap(q[i + k], q[i + w - 1 - k]);
        return copy;
    }

    void add(Severity s, const char* code, std::uint32_t cat, const Path& path, std::string msg, std::uint64_t off) {
        if (out_.findings.size() >= opt_.max_findings) return;
        Finding f;
        f.severity = s;
        f.code = code;
        f.category = cat;
        f.path = path;
        f.message = std::move(msg);
        f.offset = off;
        f.has_offset = file_offsets_;
        out_.findings.push_back(std::move(f));
    }

    void set_limit(Status s) {
        if (limit_.is_ok()) limit_ = std::move(s);
        stop_ = true;
    }

    Bytes buf_;
    const std::uint8_t* p_;
    std::size_t size_;
    std::size_t pos_ = 0;
    const ScanOptions& opt_;
    ScanResult& out_;
    bool file_offsets_;
    bool stop_ = false;
    Status limit_;
    std::uint64_t bound_ = UINT64_MAX;
    std::uint64_t element_count_ = 0;
    std::uint16_t bits_allocated_ = 0;
};

void add_file_finding(ScanResult& r, Severity s, const char* code, std::uint32_t cat, std::string msg,
                      std::uint64_t off = 0, bool has_off = false) {
    Finding f;
    f.severity = s;
    f.code = code;
    f.category = cat;
    f.message = std::move(msg);
    f.offset = off;
    f.has_offset = has_off;
    r.findings.push_back(std::move(f));
}

Syntax syntax_of(const TransferSyntaxInfo* t) {
    if (!t) return {true, false, true};
    return {t->explicit_vr, t->big_endian, t->encapsulated};
}

// Picks how the body is really encoded, starting from the transfer syntax's claim.
Syntax detect_body(const Bytes& data, std::size_t pos, Syntax claimed, const ScanOptions& opt, ScanResult& out) {
    ScanResult scratch;
    Scanner probe(data, opt, scratch, true);
    if (data.size() - pos < 8) return claimed;
    const bool exp = probe.looks_explicit(pos, claimed);
    const bool imp = probe.looks_implicit(pos, claimed);
    if (claimed.explicit_vr && !exp && imp) {
        out.encoding_mismatch = true;
        return {false, claimed.big_endian, claimed.encapsulated};
    }
    if (!claimed.explicit_vr && exp) {
        // An implicit stream whose length bytes happen to spell a VR is unlikely; also require
        // the explicit reading to agree with the next element.
        out.encoding_mismatch = true;
        return {true, claimed.big_endian, claimed.encapsulated};
    }
    if (!exp && !imp) {
        // Try the other byte order.
        const Syntax other{claimed.explicit_vr, !claimed.big_endian};
        if (probe.looks_explicit(pos, other) || probe.looks_implicit(pos, other)) {
            const std::uint16_t g = other.big_endian ? load_be16(data.data() + pos) : load_le16(data.data() + pos);
            if (g <= 0x0FFF) {
                out.encoding_mismatch = true;
                return {probe.looks_explicit(pos, other), other.big_endian, claimed.encapsulated};
            }
        }
    }
    return claimed;
}

Status parse_body(const Bytes& data, std::size_t pos, const TransferSyntaxInfo* ts, const ScanOptions& opt,
                  ScanResult& out) {
    Syntax syn = syntax_of(ts);
    if (ts && ts->codec == Codec::Deflate) {
        auto store = std::make_shared<std::vector<std::uint8_t>>();
        const std::size_t cap = opt.max_inflated_size > SIZE_MAX ? SIZE_MAX
                                                                 : static_cast<std::size_t>(opt.max_inflated_size);
        if (Status s = codec::inflate_raw(data.data() + pos, data.size() - pos, cap, *store); !s) {
            add_file_finding(out, Severity::Error, "deflate", kRescue,
                             "deflated data set could not be inflated: " + s.message, pos, true);
            return Status::ok();
        }
        Bytes inflated(std::shared_ptr<const void>(store, store.get()), store->data(), store->size());
        out.file.offsets_are_file_offsets = false;
        syn = detect_body(inflated, 0, syn, opt, out);
        out.body_explicit_vr = syn.explicit_vr;
        out.body_big_endian = syn.big_endian;
        Scanner sc(inflated, opt, out, false);
        Path path;
        sc.parse(out.file.dataset, syn, inflated.size(), false, 0, path);
        return sc.limit();
    }
    syn = detect_body(data, pos, syn, opt, out);
    out.body_explicit_vr = syn.explicit_vr;
    out.body_big_endian = syn.big_endian;
    if (out.encoding_mismatch)
        add_file_finding(out, Severity::Error, "encoding-mismatch", kStructural,
                         std::string("data set is encoded ") + (syn.explicit_vr ? "explicit" : "implicit") + " VR " +
                             (syn.big_endian ? "big" : "little") +
                             " endian, which does not match its transfer syntax",
                         pos, true);
    Scanner sc(data, opt, out, true);
    sc.seek(pos);
    Path path;
    sc.parse(out.file.dataset, syn, data.size(), false, 0, path);
    return sc.limit();
}

}  // namespace

Result<ScanResult> scan(const Bytes& data, const ScanOptions& opt) {
    ScanResult out;
    out.source = data;
    const std::uint8_t* p = data.data();
    const std::size_t n = data.size();
    if (n == 0) return Status(Errc::NotDicom, "empty input");
    std::size_t pos = 0;
    if (n >= 132 && p[128] == 'D' && p[129] == 'I' && p[130] == 'C' && p[131] == 'M') {
        out.file.had_preamble = true;
        out.had_dicm = true;
        out.file.preamble = data.sub(0, 128);
        pos = 132;
        for (std::size_t i = 0; i < 128; ++i)
            if (p[i] != 0) {
                out.preamble_nonzero = true;
                break;
            }
    } else if (n >= 4 && p[0] == 'D' && p[1] == 'I' && p[2] == 'C' && p[3] == 'M') {
        out.had_dicm = true;
        pos = 4;
        add_file_finding(out, Severity::Error, "preamble-missing", kStructural,
                         "file has \"DICM\" but no 128-byte preamble (PS3.10 7.1)", 0, true);
    } else {
        out.raw_dataset = true;
        add_file_finding(out, Severity::Error, "not-part10", kStructural,
                         "no preamble and no \"DICM\" prefix: not a Part 10 file (PS3.10 7.1)", 0, true);
    }

    // File Meta Information (always Explicit VR Little Endian, PS3.10 7.1).
    if (n - pos >= 8 && load_le16(p + pos) == 0x0002) {
        out.meta_present = true;
        ScanResult scratch;
        Scanner probe(data, opt, scratch, true);
        Syntax ms{true, false};
        if (!probe.looks_explicit(pos, ms)) {
            ms.explicit_vr = false;
            add_file_finding(out, Severity::Error, "meta-implicit-vr", kStructural,
                             "File Meta Information is not encoded Explicit VR Little Endian (PS3.10 7.1)", pos, true);
        }
        Scanner mp(data, opt, out, true);
        mp.seek(pos);
        Path path;
        mp.parse(out.file.meta, ms, n, false, 0, path, 0x0002);
        if (!mp.limit().is_ok()) return mp.limit();
        pos = mp.pos();
        out.file.meta_as_read = !out.file.meta.empty();
        if (!out.file.meta.contains(tags::FileMetaInformationGroupLength))
            add_file_finding(out, Severity::Error, "meta-group-length", kStructural,
                             "File Meta Information Group Length (0002,0000) is missing", pos, true);
    } else if (!out.raw_dataset) {
        add_file_finding(out, Severity::Error, "meta-missing", kStructural,
                         "no File Meta Information after \"DICM\"", pos, true);
    }

    const TransferSyntaxInfo* ts = nullptr;
    if (auto t = out.file.meta.get_string(tags::TransferSyntaxUID); t && !t->empty()) {
        out.declared_transfer_syntax = *t;
        ts = find_transfer_syntax(*t);
        if (!ts)
            add_file_finding(out, Severity::Error, "ts-unknown", kStructural,
                             "Transfer Syntax UID is not a known transfer syntax; data set read as explicit VR "
                             "little endian");
    } else if (out.meta_present) {
        add_file_finding(out, Severity::Error, "ts-missing", kStructural,
                         "Transfer Syntax UID (0002,0010) is missing; encoding guessed from the data");
    }
    if (!ts) {
        // Guess from the first body element.
        ScanResult scratch;
        Scanner probe(data, opt, scratch, true);
        Syntax g{true, false};
        if (n - pos >= 8) {
            // A plausible first group is small: 0x0002..0x00FF read in the right byte order.
            const std::uint16_t g_le = load_le16(p + pos), g_be = load_be16(p + pos);
            const bool be_plausible = g_le > 0x00FF && g_be <= 0x00FF;
            if (be_plausible && probe.looks_explicit(pos, {true, true})) g = {true, true};
            else if (probe.looks_explicit(pos, {true, false})) g = {true, false};
            else g = {false, false};
        }
        if (out.declared_transfer_syntax.empty() || !out.meta_present)
            ts = find_transfer_syntax(g.big_endian    ? ts::ExplicitVRBigEndian
                                      : g.explicit_vr ? ts::ExplicitVRLittleEndian
                                                      : ts::ImplicitVRLittleEndian);
    }
    out.file.transfer_syntax = ts ? std::string(ts->uid) : out.declared_transfer_syntax;
    if (Status s = parse_body(data, pos, ts, opt, out); !s) return s;
    if (out.file.dataset.empty() && out.file.meta.empty()) {
        if (out.raw_dataset) return Status(Errc::NotDicom, "no DICOM data found");
        out.fatal = true;
    }
    return out;
}

Result<ScanResult> scan_file(const std::string& utf8_path, const ScanOptions& opt) {
    auto bytes = io::map_file(utf8_path, true);
    if (!bytes) return bytes.take_error();
    return scan(*bytes, opt);
}

Status parse_implicit_items(const Bytes& bytes, std::vector<Dataset>& items, const ScanOptions& opt) {
    ScanResult out;
    // Wrap the bytes as the value of a defined-length sequence and parse its items.
    Scanner sc(bytes, opt, out, false);
    std::size_t pos = 0;
    const std::uint8_t* p = bytes.data();
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 8) return Status(Errc::Malformed, "truncated item");
        const Tag tag = make_tag(load_le16(p + pos), load_le16(p + pos + 2));
        const std::uint32_t len = load_le32(p + pos + 4);
        if (tag != tags::Item) return Status(Errc::Malformed, "not an item");
        pos += 8;
        Dataset item;
        Path ip{{0, static_cast<std::int32_t>(items.size())}};
        sc.seek(pos);
        if (len == kUndefinedLength) {
            bool delim = false;
            sc.parse(item, Syntax{false, false}, 0, true, 1, ip, 0, &delim);
            if (!delim) return Status(Errc::Malformed, "item not terminated");
            pos = sc.pos();
        } else {
            if (len > bytes.size() - pos) return Status(Errc::Malformed, "item runs past the value");
            sc.parse(item, Syntax{false, false}, pos + len, false, 1, ip);
            pos += len;
        }
        if (!sc.limit().is_ok()) return sc.limit();
        for (const Finding& f : out.findings)
            if (f.severity == Severity::Error) return Status(Errc::Malformed, "item content is malformed");
        items.push_back(std::move(item));
    }
    return Status::ok();
}

}  // namespace siaara::repair
