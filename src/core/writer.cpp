#include "core/writer.hpp"

#include "codec/deflate.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"

namespace siaara {

namespace {

struct Enc {
    bool explicit_vr;
    bool big_endian;
    bool pixel_big_endian = false;  // F6b: top-level OW Pixel Data big endian, rest little endian
};

// Byte-swap unit for a value in a big-endian syntax: per sample for OW pixel data wider than 16
// bits (PS3.5 A.3 / 8.1.1), 16-bit halves for AT (PS3.5 6.2), else the VR's value size.
std::size_t swap_unit(Tag t, VR vr, std::uint16_t bits_allocated) noexcept {
    if (t == tags::PixelData && vr == VR::OW && (bits_allocated == 32 || bits_allocated == 64))
        return static_cast<std::size_t>(bits_allocated / 8u);
    return vr == VR::AT ? 2 : vr_value_size(vr);
}

void swap_in_place(std::uint8_t* q, std::size_t len, std::size_t w) noexcept {
    if (w <= 1) return;
    for (std::size_t i = 0; i + w <= len; i += w)
        for (std::size_t k = 0; k < w / 2; ++k) std::swap(q[i + k], q[i + w - 1 - k]);
}

class Writer {
public:
    Writer(std::vector<std::uint8_t>& out, Enc enc, const WriteOptions& opt)
        : out_(out), enc_(enc), opt_(opt) {}

    // `skip_meta_group`: the top level of a Part 10 body, where group 0002 must not appear
    // (PS3.10 7.1); file meta elements left in the data set would otherwise follow the real file
    // meta and override it when the file is read back.
    Status dataset(const Dataset& ds, bool is_meta = false, bool skip_meta_group = false) {
        // Pixel samples wider than 16 bits stored as OW must be byte-reversed per sample, not per
        // 16-bit word, in big-endian syntaxes (PS3.5 A.3 / 8.1.1), so remember Bits Allocated.
        const std::uint16_t saved_bits = pixel_bits_;
        if (auto b = ds.get_int(tags::BitsAllocated)) pixel_bits_ = static_cast<std::uint16_t>(*b);
        struct Restore { std::uint16_t& ref; std::uint16_t v; ~Restore() { ref = v; } } restore{pixel_bits_, saved_bits};
        if (is_meta) {
            for (const Element& e : ds.elements())
                if (Status s = element(e); !s) return s;
            return Status::ok();
        }
        GroupPatch pending;
        std::uint32_t group = 0x10000;  // no group yet
        for (const Element& e : ds.elements()) {
            if (skip_meta_group && tag_group(e.tag) == 0x0002) continue;
            const std::uint16_t g = tag_group(e.tag);
            if (g != group) {
                if (Status s = finish_group(pending); !s) return s;
                group = g;
                if (opt_.group_lengths == GroupLengths::Recompute && g != 0xFFFE &&
                    tag_element(e.tag) != 0) {
                    // Add a group length for a group that has none.
                    header(make_tag(g, 0), VR::UL, 4);
                    pending = GroupPatch{true, out_.size(), out_.size() + 4, nullptr};
                    u32(0);  // value patched by finish_group
                }
            }
            if (is_group_length(e)) {
                if (opt_.group_lengths == GroupLengths::Drop) continue;
                header(e.tag, e.vr == VR::None ? VR::UL : e.vr, 4);
                pending = GroupPatch{true, out_.size(), out_.size() + 4, &e};
                u32(0);  // value patched by finish_group
                continue;
            }
            if (Status s = element(e); !s) return s;
        }
        return finish_group(pending);
    }

private:
    // A group length whose value is patched once the group has been written.
    struct GroupPatch {
        bool active = false;
        std::size_t value_at = 0;
        std::size_t body_start = 0;
        const Element* source = nullptr;  // the element as read, or null when added
    };

    static bool is_group_length(const Element& e) noexcept {
        return tag_element(e.tag) == 0 && tag_group(e.tag) != 0xFFFE && tag_group(e.tag) != 0x0002 &&
               !e.is_sequence() && !e.is_encapsulated() && e.value.size() == 4;
    }

    Status finish_group(GroupPatch& p) {
        if (!p.active) return Status::ok();
        p.active = false;
        const std::uint64_t n = out_.size() - p.body_start;
        if (n > 0xFFFFFFFFu) return Status(Errc::LimitExceeded, "group too large for a group length");
        std::uint32_t v = static_cast<std::uint32_t>(n);
        // Keep: a group whose size is what it was on read is unchanged, so the value as read is
        // written back verbatim (even if the source file had it wrong); otherwise it is recomputed.
        const Element* e = p.source;
        if (opt_.group_lengths == GroupLengths::Keep && e && e->read.parsed && e->read.length == 4 &&
            e->read.group_bytes == v)
            v = load_le32(e->value.data());
        std::uint8_t* q = out_.data() + p.value_at;
        enc_.big_endian ? store_be32(q, v) : store_le32(q, v);
        return Status::ok();
    }

    void u16(std::uint16_t v) {
        std::uint8_t b[2];
        enc_.big_endian ? store_be16(b, v) : store_le16(b, v);
        out_.insert(out_.end(), b, b + 2);
    }
    void u32(std::uint32_t v) {
        std::uint8_t b[4];
        enc_.big_endian ? store_be32(b, v) : store_le32(b, v);
        out_.insert(out_.end(), b, b + 4);
    }
    void tag(Tag t) {
        u16(tag_group(t));
        u16(tag_element(t));
    }

    void header(Tag t, VR vr, std::uint32_t len) {
        tag(t);
        if (enc_.explicit_vr) {
            const auto v = static_cast<std::uint16_t>(vr);
            out_.push_back(static_cast<std::uint8_t>(v >> 8));
            out_.push_back(static_cast<std::uint8_t>(v & 0xFF));
            if (vr_has_long_length(vr)) {
                out_.push_back(0);
                out_.push_back(0);
                u32(len);
            } else {
                u16(static_cast<std::uint16_t>(len));
            }
        } else {
            u32(len);
        }
    }

    // True when an odd length read from the stream is written unchanged.
    bool keep_odd(const Element& e) const noexcept {
        return opt_.odd_lengths == OddLengths::Keep && e.read.parsed && e.value.size() == e.read.length;
    }

    Status element(const Element& e) {
        VR vr = e.vr;
        if (vr == VR::None) vr = VR::UN;
        if (e.is_sequence()) return sequence(e);
        if (e.is_encapsulated()) return encapsulated(e);

        const std::size_t len = e.value.size();
        const bool pad = (len & 1u) != 0 && !keep_odd(e);
        const std::uint64_t padded = len + (pad ? 1 : 0);
        if (enc_.explicit_vr && !vr_has_long_length(vr) && padded > 0xFFFF) {
            // Value too long for a 16-bit length: PS3.5 6.2.2 says use UN... but for strings, UC/UT.
            if (vr_is_string(vr)) vr = VR::UT;
            else vr = VR::UN;
        }
        if (padded >= kUndefinedLength) return Status(Errc::LimitExceeded, "value too large", 0, e.tag);
        // A VR the private dictionary gave an implicit VR element is for interpretation only: the
        // element is saved as it was read, VR UN, in explicit VR syntaxes (ReadInfo::private_vr).
        const VR pad_vr = vr;
        if (dictionary_vr_only(e)) vr = VR::UN;
        header(e.tag, vr, static_cast<std::uint32_t>(padded));
        const std::size_t at = out_.size();
        out_.insert(out_.end(), e.value.data(), e.value.data() + len);
        if (pad) out_.push_back(static_cast<std::uint8_t>(vr_pad_char(pad_vr)));
        if (enc_.big_endian) swap_in_place(out_.data() + at, len, swap_unit(e.tag, vr, pixel_bits_));
        else if (enc_.pixel_big_endian && depth_ == 0 && e.tag == tags::PixelData && vr == VR::OW &&
                 (pixel_bits_ == 16 || pixel_bits_ == 32 || pixel_bits_ == 64))
            swap_in_place(out_.data() + at, len, pixel_bits_ / 8u);
        return Status::ok();
    }

    Status patch(std::size_t at, std::size_t body_start) {
        const std::uint64_t len = out_.size() - body_start;
        if (len >= kUndefinedLength) return Status(Errc::LimitExceeded, "sequence too large");
        std::uint8_t* q = out_.data() + at;
        enc_.big_endian ? store_be32(q, static_cast<std::uint32_t>(len))
                        : store_le32(q, static_cast<std::uint32_t>(len));
        return Status::ok();
    }

    // The element's VR came from the private dictionary on an implicit VR read and is unchanged.
    static bool dictionary_vr_only(const Element& e) noexcept {
        return e.read.parsed && e.read.private_vr != VR::None && e.vr == e.read.private_vr;
    }

    // Undefined length for this SQ? (see SequenceLengths in writer.hpp)
    bool sequence_undefined(const Element& e) const noexcept {
        switch (opt_.sequence_lengths) {
            case SequenceLengths::Explicit: return false;
            case SequenceLengths::Undefined: return true;
            case SequenceLengths::KeepAsRead: break;
        }
        // In implicit VR a reader needs the dictionary to see an explicit-length SQ as a sequence.
        const bool unknown_in_implicit = !enc_.explicit_vr && dict::implicit_vr(e.tag) != VR::SQ;
        // A defined-length sequence found through the private dictionary keeps its length as read.
        if (e.read.parsed && dictionary_vr_only(e)) return e.undefined_length;
        if (e.read.parsed) return e.undefined_length || unknown_in_implicit;
        return unknown_in_implicit || (!enc_.explicit_vr && tag_is_private(e.tag));
    }

    bool item_undefined(const Dataset& item, bool sq_undefined) const noexcept {
        switch (opt_.sequence_lengths) {
            case SequenceLengths::Explicit: return false;
            case SequenceLengths::Undefined: return true;
            case SequenceLengths::KeepAsRead: break;
        }
        switch (item.item_length_mode()) {
            case Dataset::kItemLengthExplicit: return false;
            case Dataset::kItemLengthUndefined: return true;
            default: return sq_undefined;
        }
    }

    Status sequence(const Element& e) {
        // A sequence read as UN with undefined length goes back out the same way: VR UN in
        // explicit VR syntaxes, items always implicit VR little endian (PS3.5 6.2.2). Not in
        // Explicit VR Big Endian: pydicom and GDCM read such a value with big-endian item tags
        // and cannot find its end (gdcmData CT-SIEMENS-MissingPixelDataInIconSQ.dcm), so there
        // it is written as a normal SQ in the file's own encoding (testing-chat BE finding).
        const bool as_un = e.read.parsed && e.read.un_sequence && !enc_.big_endian &&
                           opt_.sequence_lengths == SequenceLengths::KeepAsRead;
        // A defined-length implicit VR sequence found through the private dictionary goes out as it
        // was read: in explicit VR little endian as UN of defined length with implicit VR items.
        const bool as_un_defined = !as_un && dictionary_vr_only(e) && !e.undefined_length && enc_.explicit_vr &&
                                   !enc_.big_endian && opt_.sequence_lengths == SequenceLengths::KeepAsRead;
        const bool undef = as_un || (!as_un_defined && sequence_undefined(e));
        std::size_t len_at = 0;
        header(e.tag, (as_un || as_un_defined) ? VR::UN : VR::SQ, undef ? kUndefinedLength : 0);
        if (!undef) len_at = out_.size() - 4;
        const Enc outer = enc_;
        if (as_un || as_un_defined) enc_ = Enc{false, false};
        struct RestoreEnc { Enc& ref; Enc v; ~RestoreEnc() { ref = v; } } restore{enc_, outer};
        const std::size_t body = out_.size();
        ++depth_;
        struct Depth { unsigned& d; ~Depth() { --d; } } depth_guard{depth_};
        for (const Dataset& item : e.items) {
            const bool iundef = item_undefined(item, undef);
            tag(tags::Item);
            const std::size_t ilen_at = out_.size();
            u32(iundef ? kUndefinedLength : 0);
            const std::size_t ibody = out_.size();
            if (Status s = dataset(item); !s) return s;
            if (iundef) {
                tag(tags::ItemDelimitationItem);
                u32(0);
            } else if (Status s = patch(ilen_at, ibody); !s) {
                return s;
            }
        }
        if (undef) {
            tag(tags::SequenceDelimitationItem);
            u32(0);
            return Status::ok();
        }
        return patch(len_at, body);
    }

    Status encapsulated(const Element& e) {
        // Fragments read with an odd length (typically the last one) are kept that way unless
        // padding is asked for or the fragment list was changed.
        const bool keep = opt_.odd_lengths == OddLengths::Keep && e.read.parsed &&
                          e.read.fragment_count == e.fragments.size();
        // Read with a defined length (testing 10b item 5): written back the same way while the
        // fragments are as read and lengths are kept as read, so the file is reproduced.
        if (e.read.fragments_defined_length && keep && opt_.sequence_lengths == SequenceLengths::KeepAsRead) {
            std::uint64_t total = e.read.fragments_defined_length == 2 ? 8u : 0u;
            for (const Bytes& f : e.fragments) total += 8u + f.size();
            if (total >= kUndefinedLength) return Status(Errc::LimitExceeded, "pixel data too large");
            header(e.tag, e.vr == VR::None ? VR::OB : e.vr, static_cast<std::uint32_t>(total));
            for (const Bytes& f : e.fragments) {
                tag(tags::Item);
                u32(static_cast<std::uint32_t>(f.size()));
                out_.insert(out_.end(), f.data(), f.data() + f.size());
            }
            if (e.read.fragments_defined_length == 2) {
                tag(tags::SequenceDelimitationItem);
                u32(0);
            }
            return Status::ok();
        }
        header(e.tag, e.vr == VR::None ? VR::OB : e.vr, kUndefinedLength);
        for (const Bytes& f : e.fragments) {
            tag(tags::Item);
            const bool pad = (f.size() & 1u) != 0 && !keep;
            const std::size_t len = f.size() + (pad ? 1u : 0u);
            if (len >= kUndefinedLength) return Status(Errc::LimitExceeded, "fragment too large");
            u32(static_cast<std::uint32_t>(len));
            out_.insert(out_.end(), f.data(), f.data() + f.size());
            if (pad) out_.push_back(0);
        }
        tag(tags::SequenceDelimitationItem);
        u32(0);
        return Status::ok();
    }

    std::vector<std::uint8_t>& out_;
    std::uint16_t pixel_bits_ = 0;
    unsigned depth_ = 0;  // sequence nesting
    Enc enc_;
    const WriteOptions& opt_;
};

}  // namespace

Status stored_value_bytes(const Element& e, std::string_view transfer_syntax_uid,
                          std::uint16_t bits_allocated, std::vector<std::uint8_t>& out, bool top_level) {
    if (e.is_sequence() || e.is_encapsulated())
        return Status(Errc::TypeMismatch, "element has no single value (sequence or encapsulated)", 0, e.tag);
    const TransferSyntaxInfo* t = find_transfer_syntax(transfer_syntax_uid);
    out.assign(e.value.data(), e.value.data() + e.value.size());
    if (t && t->big_endian)
        swap_in_place(out.data(), out.size(), swap_unit(e.tag, e.vr == VR::None ? VR::UN : e.vr, bits_allocated));
    else if (t && t->pixel_big_endian && top_level && e.tag == tags::PixelData && e.vr == VR::OW &&
             (bits_allocated == 16 || bits_allocated == 32 || bits_allocated == 64))
        swap_in_place(out.data(), out.size(), bits_allocated / 8u);
    return Status::ok();
}

namespace {
Status write_body(const Dataset& ds, std::string_view transfer_syntax_uid, std::vector<std::uint8_t>& out,
                  const WriteOptions& opt, bool part10_body) {
    const TransferSyntaxInfo* t = find_transfer_syntax(transfer_syntax_uid);
    if (!t) return Status(Errc::Unsupported, "unknown transfer syntax");
    const Enc enc{t->explicit_vr, t->big_endian, t->pixel_big_endian};
    if (t->codec == Codec::Deflate) {
        std::vector<std::uint8_t> plain;
        Writer w(plain, enc, opt);
        if (Status s = w.dataset(ds, false, part10_body); !s) return s;
        std::vector<std::uint8_t> packed;
        if (Status s = codec::deflate_raw(plain.data(), plain.size(), opt.deflate_level, packed); !s)
            return s;
        out.insert(out.end(), packed.begin(), packed.end());
        if (packed.size() & 1u) out.push_back(0);
        return Status::ok();
    }
    Writer w(out, enc, opt);
    return w.dataset(ds, false, part10_body);
}
}  // namespace

Status write_dataset(const Dataset& ds, std::string_view transfer_syntax_uid,
                     std::vector<std::uint8_t>& out, const WriteOptions& opt) {
    return write_body(ds, transfer_syntax_uid, out, opt, false);
}

Status write_part10(const DicomFile& file, std::vector<std::uint8_t>& out, const WriteOptions& opt) {
    if (!file.complete() && !opt.allow_incomplete) {
        const char* why = (file.read_loss & DicomFile::kReadBytesSkipped)     ? "input bytes were skipped"
                          : (file.read_loss & DicomFile::kReadInputTruncated) ? "the input was truncated"
                          : (file.read_loss & DicomFile::kReadRecovered)
                              ? "damaged input was recovered by a heuristic, see the read warnings"
                              : "it was read header-only";
        return Status(Errc::Rejected,
                      std::string("refusing to save a data set that was not read completely (") + why +
                          "; first at offset " + std::to_string(file.read_loss_offset) +
                          "): the file would hold less than was read; allow incomplete saves to write it anyway",
                      file.read_loss_offset);
    }
    std::string ts_uid = file.transfer_syntax;
    if (ts_uid.empty()) ts_uid = std::string(ts::ExplicitVRLittleEndian);

    Dataset meta = file.meta;
    // A meta group read from a stream is kept as it was: missing elements are not added (that
    // would change the user's file). A meta group built in memory gets the required elements.
    const bool meta_as_read = file.meta_as_read;
    const Element* read_gl = file.meta.find(tags::FileMetaInformationGroupLength);
    if (!meta_as_read) {
        static constexpr std::uint8_t kVersion[2] = {0x00, 0x01};
        if (!meta.contains(tags::FileMetaInformationVersion))
            (void)meta.set_bytes(tags::FileMetaInformationVersion, VR::OB, Bytes::copy_of(kVersion, 2));
        if (!meta.contains(tags::MediaStorageSOPClassUID))
            if (auto v = file.dataset.get_string(tags::SOPClassUID))
                (void)meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, *v);
        if (!meta.contains(tags::MediaStorageSOPInstanceUID))
            if (auto v = file.dataset.get_string(tags::SOPInstanceUID))
                (void)meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, *v);
        if (!meta.contains(tags::ImplementationClassUID)) {
            (void)meta.set_string(tags::ImplementationClassUID, VR::UI, kImplementationClassUid);
            // Version Name (type 3) names this implementation only alongside its class UID.
            if (!meta.contains(tags::ImplementationVersionName))
                (void)meta.set_string(tags::ImplementationVersionName, VR::SH, kImplementationVersionName);
        }
    }
    // Rewritten only when it changes, so an unchanged element keeps its bytes (padding included).
    if (meta.get_string(tags::TransferSyntaxUID).value_or(std::string()) != ts_uid)
        (void)meta.set_string(tags::TransferSyntaxUID, VR::UI, ts_uid);
    meta.remove(tags::FileMetaInformationGroupLength);
    // The group length is written unless a meta group read without one is kept as read.
    const bool write_gl = !(meta_as_read && !read_gl && opt.group_lengths == GroupLengths::Keep);

    // The preamble as read is kept (it may hold e.g. a TIFF header, PS3.10 7.5); otherwise zeros.
    if (file.had_preamble && file.preamble.size() == 128)
        out.assign(file.preamble.data(), file.preamble.data() + 128);
    else
        out.assign(128, 0);
    out.insert(out.end(), {'D', 'I', 'C', 'M'});

    std::vector<std::uint8_t> meta_bytes;
    {
        Writer w(meta_bytes, Enc{true, false}, opt);
        if (Status s = w.dataset(meta, true); !s) return s;
    }
    if (meta_bytes.size() > 0xFFFFFFFEu) return Status(Errc::LimitExceeded, "file meta too large");
    if (write_gl) {
        // (0002,0000) UL group length: as read when the group's size is unchanged, else recomputed.
        auto v = static_cast<std::uint32_t>(meta_bytes.size());
        if (opt.group_lengths == GroupLengths::Keep && read_gl && read_gl->read.parsed &&
            read_gl->value.size() == 4 && read_gl->read.group_bytes == v)
            v = load_le32(read_gl->value.data());
        const std::uint8_t gl[8] = {0x02, 0x00, 0x00, 0x00, 'U', 'L', 0x04, 0x00};
        out.insert(out.end(), gl, gl + 8);
        std::uint8_t len[4];
        store_le32(len, v);
        out.insert(out.end(), len, len + 4);
    }
    out.insert(out.end(), meta_bytes.begin(), meta_bytes.end());

    return write_body(file.dataset, ts_uid, out, opt, true);
}

Status write_file(const DicomFile& file, const std::string& utf8_path, const WriteOptions& opt) {
    std::vector<std::uint8_t> buf;
    if (Status s = write_part10(file, buf, opt); !s) return s;
    return io::write_file_atomic(utf8_path, buf.data(), buf.size());
}

}  // namespace siaara
