#include "core/reader.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "codec/deflate.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"

namespace siaara {

namespace {

struct Syntax {
    bool explicit_vr = true;
    bool big_endian = false;
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
        bool ordered = true;
        Status s = parse_elements(ds, syn, end, undefined, depth, only_group, ordered);
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
        while (true) {
            if (!undefined && pos_ >= end) return Status::ok();
            if (pos_ == limit_) {
                if (undefined && depth > 0) return lenient_or(Errc::Truncated, "missing item delimiter");
                return Status::ok();
            }
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

            Element e;
            e.tag = tag;
            e.offset = start;
            std::uint32_t len = 0;
            if (Status s = read_vr_and_length(syn, tag, e.vr, len); !s) return s;

            if (++element_count_ > opt_.max_elements)
                return Status(Errc::LimitExceeded, "too many elements", start, tag);

            if (len == kUndefinedLength) {
                e.undefined_length = true;
                if (e.vr == VR::SQ || (e.vr == VR::UN && tag != tags::PixelData)) {
                    // UN with undefined length is a sequence encoded implicit VR LE (PS3.5 6.2.2).
                    Syntax inner = (e.vr == VR::UN) ? Syntax{false, false} : syn;
                    e.vr = VR::SQ;
                    if (Status s = parse_items(e, inner, 0, true, depth); !s) return s;
                } else if (tag == tags::PixelData || tag == tags::FloatPixelData ||
                           tag == tags::DoubleFloatPixelData || tag_group(tag) == 0x7FE0 ||
                           e.vr == VR::OB || e.vr == VR::OW || e.vr == VR::UN) {
                    if (Status s = parse_fragments(e, syn); !s) return s;
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
                    len = static_cast<std::uint32_t>(limit_ - pos_);
                    vend = limit_;
                }
                if (e.vr == VR::SQ) {
                    if (Status s = parse_items(e, syn, vend, false, depth); !s) return s;
                    advance_to(vend);
                } else {
                    std::size_t unit = 0;
                    // Pixel samples wider than 16 bits stored as OW are big-endian per sample, not
                    // per 16-bit word: swap by Bits Allocated so memory holds true little endian
                    // (the writer does the inverse).
                    if (syn.big_endian && depth == 0 && tag == tags::PixelData && e.vr == VR::OW &&
                        (bits_allocated_ == 32 || bits_allocated_ == 64))
                        unit = bits_allocated_ / 8u;
                    e.value = value_bytes(e.vr, pos_, len, syn, unit);
                    if (depth == 0 && tag == tags::BitsAllocated && e.value.size() >= 2)
                        bits_allocated_ = load_le16(e.value.data());
                    advance_to(vend);
                }
            }
            if (ordered && !ds.empty() && !(ds.elements().back().tag < e.tag)) ordered = false;
            ds.append_unsorted(std::move(e));
        }
    }

    bool stopped_at_pixels() const noexcept { return stopped_at_pixels_; }
    std::uint64_t stop_offset() const noexcept { return stop_offset_; }

private:
    // Moves the cursor forward to `to`. Children are bounded by their parent (limit_), so the
    // cursor can never be past `to` here; the max() keeps it monotonic regardless (R-001).
    void advance_to(std::uint64_t to) noexcept {
        assert(pos_ <= to);
        if (to > pos_) pos_ = static_cast<std::size_t>(to);
    }

    static std::string tag_text(Tag t) {
        static const char* hex = "0123456789ABCDEF";
        std::string s = "(gggg,eeee)";
        for (int i = 0; i < 4; ++i) {
            s[static_cast<std::size_t>(1 + i)] = hex[(t >> (28 - 4 * i)) & 0xFu];
            s[static_cast<std::size_t>(6 + i)] = hex[(t >> (12 - 4 * i)) & 0xFu];
        }
        return s;
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

    Status read_vr_and_length(Syntax syn, Tag tag, VR& vr, std::uint32_t& len) {
        // Item-level tags (FFFE,xxxx) never carry a VR.
        if (tag_group(tag) == 0xFFFE) {
            vr = VR::None;
            len = read_u32(syn);
            return Status::ok();
        }
        if (!syn.explicit_vr) {
            vr = dict::implicit_vr(tag);
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
                    iend = limit_;
                }
                if (Status s = parse(item, syn, iend, false, depth + 1); !s) return s;
                advance_to(iend);
            }
            e.items.push_back(std::move(item));
        }
    }

    Status parse_fragments(Element& e, Syntax syn) {
        while (true) {
            if (limit_ - pos_ < 8) return lenient_or(Errc::Truncated, "missing pixel data sequence delimiter");
            const std::size_t start = pos_;
            const Tag tag = read_tag(syn);
            std::uint32_t len = read_u32(syn);
            if (tag == tags::SequenceDelimitationItem) return Status::ok();
            if (tag != tags::Item)
                return Status(Errc::Malformed, "expected fragment item", start, e.tag);
            if (e.fragments.size() >= opt_.max_fragments)
                return Status(Errc::LimitExceeded, "too many pixel data fragments", start, e.tag);
            std::uint64_t fend = 0;
            if (len == kUndefinedLength || !checked_add(pos_, len, fend) || (fend > limit_ && limit_ == size_))
                return Status(Errc::Truncated, "fragment runs past end of data", start, e.tag);
            if (fend > limit_) {
                if (!opt_.lenient)
                    return Status(Errc::Malformed, "fragment runs past end of parent item", start, e.tag);
                warn("fragment runs past end of parent; clamped");
                len = static_cast<std::uint32_t>(limit_ - pos_);
                fend = limit_;
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

    Status lenient_or(Errc code, const char* msg) {
        if (opt_.lenient) {
            warn(msg);
            pos_ = limit_;  // the end of the innermost container: forward, never past the parent
            return Status::ok();
        }
        return Status(code, msg, pos_);
    }

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
};

Result<Syntax> syntax_for(std::string_view uid, bool lenient, const TransferSyntaxInfo** info) {
    const TransferSyntaxInfo* t = find_transfer_syntax(uid);
    *info = t;
    if (t) return Syntax{t->explicit_vr, t->big_endian};
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
        return s;
    }
    Parser p(data, opt, &out.warnings);
    p.seek(start);
    Status s = p.parse(out.dataset, *syn, data.size(), false, 0);
    out.stopped_at_offset = p.stop_offset();
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
            pos = mp.pos();
            if (auto t = out.meta.get_string(tags::TransferSyntaxUID)) {
                out.transfer_syntax = *t;
                uid = out.transfer_syntax;
            }
        } else {
            out.transfer_syntax = std::string(uid);
        }
        if (Status s = parse_body(data, pos, uid, opt, out); !s) return s;
        if (out.transfer_syntax.empty()) out.transfer_syntax = std::string(uid);
        return out;
    }

    // File meta information: always explicit VR little endian (PS3.10 7.1).
    {
        Parser mp(data, opt, &out.warnings);
        mp.seek(pos);
        if (Status s = mp.parse(out.meta, Syntax{true, false}, n, false, 0, 0x0002); !s) return s;
        pos = mp.pos();
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
