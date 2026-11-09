#include "core/writer.hpp"

#include "codec/deflate.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"

namespace siaara {

namespace {

struct Enc {
    bool explicit_vr;
    bool big_endian;
};

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
        for (const Element& e : ds.elements()) {
            if (tag_element(e.tag) == 0 && !is_meta && !opt_.keep_group_lengths) continue;
            if (skip_meta_group && tag_group(e.tag) == 0x0002) continue;
            if (Status s = element(e); !s) return s;
        }
        return Status::ok();
    }

private:
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

    Status element(const Element& e) {
        VR vr = e.vr;
        if (vr == VR::None) vr = VR::UN;
        if (e.is_sequence()) return sequence(e);
        if (e.is_encapsulated()) return encapsulated(e);

        const std::size_t len = e.value.size();
        const bool pad = (len & 1u) != 0;
        const std::uint64_t padded = len + (pad ? 1 : 0);
        if (enc_.explicit_vr && !vr_has_long_length(vr) && padded > 0xFFFF) {
            // Value too long for a 16-bit length: PS3.5 6.2.2 says use UN... but for strings, UC/UT.
            if (vr_is_string(vr)) vr = VR::UT;
            else vr = VR::UN;
        }
        if (padded >= kUndefinedLength) return Status(Errc::LimitExceeded, "value too large", 0, e.tag);
        header(e.tag, vr, static_cast<std::uint32_t>(padded));
        const std::size_t at = out_.size();
        out_.insert(out_.end(), e.value.data(), e.value.data() + len);
        if (pad) out_.push_back(static_cast<std::uint8_t>(vr_pad_char(vr)));
        if (enc_.big_endian) {
            if (e.tag == tags::PixelData && vr == VR::OW && (pixel_bits_ == 32 || pixel_bits_ == 64))
                swap_width(static_cast<std::size_t>(pixel_bits_ / 8), at, len);
            else
                swap_values(vr, at, len);
        }
        return Status::ok();
    }

    void swap_values(VR vr, std::size_t at, std::size_t len) {
        swap_width(vr == VR::AT ? 2 : vr_value_size(vr), at, len);
    }

    void swap_width(std::size_t w, std::size_t at, std::size_t len) {
        if (w <= 1) return;
        std::uint8_t* q = out_.data() + at;
        for (std::size_t i = 0; i + w <= len; i += w)
            for (std::size_t k = 0; k < w / 2; ++k) std::swap(q[i + k], q[i + w - 1 - k]);
    }

    // Writes a length placeholder and returns its offset so it can be patched.
    std::size_t placeholder_header(Tag t, VR vr) {
        header(t, vr, 0);
        return out_.size() - 4;
    }
    Status patch(std::size_t at, std::size_t body_start) {
        const std::uint64_t len = out_.size() - body_start;
        if (len >= kUndefinedLength) return Status(Errc::LimitExceeded, "sequence too large");
        std::uint8_t* q = out_.data() + at;
        enc_.big_endian ? store_be32(q, static_cast<std::uint32_t>(len))
                        : store_le32(q, static_cast<std::uint32_t>(len));
        return Status::ok();
    }

    Status sequence(const Element& e) {
        const bool undef = opt_.sequences_undefined_length;
        std::size_t len_at = 0;
        if (undef) header(e.tag, VR::SQ, kUndefinedLength);
        else len_at = placeholder_header(e.tag, VR::SQ);
        const std::size_t body = out_.size();
        for (const Dataset& item : e.items) {
            tag(tags::Item);
            const std::size_t ilen_at = out_.size();
            u32(undef ? kUndefinedLength : 0);
            const std::size_t ibody = out_.size();
            if (Status s = dataset(item); !s) return s;
            if (undef) {
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
        header(e.tag, e.vr == VR::None ? VR::OB : e.vr, kUndefinedLength);
        for (const Bytes& f : e.fragments) {
            tag(tags::Item);
            const std::size_t len = f.size() + (f.size() & 1u);
            if (len >= kUndefinedLength) return Status(Errc::LimitExceeded, "fragment too large");
            u32(static_cast<std::uint32_t>(len));
            out_.insert(out_.end(), f.data(), f.data() + f.size());
            if (f.size() & 1u) out_.push_back(0);
        }
        tag(tags::SequenceDelimitationItem);
        u32(0);
        return Status::ok();
    }

    std::vector<std::uint8_t>& out_;
    std::uint16_t pixel_bits_ = 0;
    Enc enc_;
    const WriteOptions& opt_;
};

}  // namespace

namespace {
Status write_body(const Dataset& ds, std::string_view transfer_syntax_uid, std::vector<std::uint8_t>& out,
                  const WriteOptions& opt, bool part10_body) {
    const TransferSyntaxInfo* t = find_transfer_syntax(transfer_syntax_uid);
    if (!t) return Status(Errc::Unsupported, "unknown transfer syntax");
    const Enc enc{t->explicit_vr, t->big_endian};
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
    std::string ts_uid = file.transfer_syntax;
    if (ts_uid.empty()) ts_uid = std::string(ts::ExplicitVRLittleEndian);

    Dataset meta = file.meta;
    {
        static constexpr std::uint8_t kVersion[2] = {0x00, 0x01};
        if (!meta.contains(tags::FileMetaInformationVersion))
            (void)meta.set_bytes(tags::FileMetaInformationVersion, VR::OB, Bytes::copy_of(kVersion, 2));
        if (!meta.contains(tags::MediaStorageSOPClassUID))
            if (auto v = file.dataset.get_string(tags::SOPClassUID))
                (void)meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, *v);
        if (!meta.contains(tags::MediaStorageSOPInstanceUID))
            if (auto v = file.dataset.get_string(tags::SOPInstanceUID))
                (void)meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, *v);
        (void)meta.set_string(tags::TransferSyntaxUID, VR::UI, ts_uid);
        if (!meta.contains(tags::ImplementationClassUID))
            (void)meta.set_string(tags::ImplementationClassUID, VR::UI, kImplementationClassUid);
        if (!meta.contains(tags::ImplementationVersionName))
            (void)meta.set_string(tags::ImplementationVersionName, VR::SH, kImplementationVersionName);
        meta.remove(tags::FileMetaInformationGroupLength);
    }

    out.assign(128, 0);
    out.insert(out.end(), {'D', 'I', 'C', 'M'});

    std::vector<std::uint8_t> meta_bytes;
    {
        Writer w(meta_bytes, Enc{true, false}, opt);
        if (Status s = w.dataset(meta, true); !s) return s;
    }
    // (0002,0000) UL group length
    const std::uint8_t gl[8] = {0x02, 0x00, 0x00, 0x00, 'U', 'L', 0x04, 0x00};
    out.insert(out.end(), gl, gl + 8);
    std::uint8_t len[4];
    store_le32(len, static_cast<std::uint32_t>(meta_bytes.size()));
    out.insert(out.end(), len, len + 4);
    out.insert(out.end(), meta_bytes.begin(), meta_bytes.end());

    return write_body(file.dataset, ts_uid, out, opt, true);
}

Status write_file(const DicomFile& file, const std::string& utf8_path, const WriteOptions& opt) {
    std::vector<std::uint8_t> buf;
    if (Status s = write_part10(file, buf, opt); !s) return s;
    return io::write_file_atomic(utf8_path, buf.data(), buf.size());
}

}  // namespace siaara
