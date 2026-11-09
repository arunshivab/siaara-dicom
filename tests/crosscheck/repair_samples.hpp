// Test-only: deliberately broken DICOM files for the repair / validation tests
// (tests/test_repair.cpp, tests/test_validate.cpp) and tests/crosscheck/repair_crosscheck.py
// (through repair_samples.cpp). Every file is built here byte by byte or with the SIAARA writer;
// nothing is taken from other projects.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"

namespace repair_samples {

using siaara::Bytes;
using siaara::Dataset;
using siaara::Element;
using siaara::Tag;
using siaara::VR;
using Buf = std::vector<std::uint8_t>;

constexpr const char* kScClass = "1.2.840.10008.5.1.4.1.1.7";
constexpr const char* kPatientName = "Testpatient^Repair";  // must never appear in non-detailed reports

inline void put_str(Dataset& ds, Tag t, VR vr, const std::string& v) { (void)ds.set_string(t, vr, v); }

// A Secondary Capture Image (PS3.3 A.8.1) that meets every Type 1 / Type 2 requirement.
inline Dataset base_sc(const std::string& sop_uid = "1.2.826.0.1.3680043.10.1408.1.1", std::uint16_t rows = 8,
                       std::uint16_t cols = 8, std::uint16_t bits = 8, std::uint32_t frames = 1) {
    Dataset ds;
    put_str(ds, 0x00080008, VR::CS, "DERIVED\\SECONDARY");
    put_str(ds, 0x00080016, VR::UI, kScClass);
    put_str(ds, 0x00080018, VR::UI, sop_uid);
    put_str(ds, 0x00080020, VR::DA, "20240102");
    put_str(ds, 0x00080030, VR::TM, "101500");
    put_str(ds, 0x00080050, VR::SH, "ACC1");
    put_str(ds, 0x00080060, VR::CS, "OT");
    put_str(ds, 0x00080064, VR::CS, "WSD");
    put_str(ds, 0x00080090, VR::PN, "Referrer^Test");
    put_str(ds, 0x00100010, VR::PN, kPatientName);
    put_str(ds, 0x00100020, VR::LO, "PID-REPAIR-1");
    put_str(ds, 0x00100030, VR::DA, "19700101");
    put_str(ds, 0x00100040, VR::CS, "O");
    put_str(ds, 0x0020000D, VR::UI, "1.2.826.0.1.3680043.10.1408.2.1");
    put_str(ds, 0x0020000E, VR::UI, "1.2.826.0.1.3680043.10.1408.3.1");
    put_str(ds, 0x00200010, VR::SH, "S1");
    put_str(ds, 0x00200011, VR::IS, "1");
    put_str(ds, 0x00200013, VR::IS, "1");
    put_str(ds, 0x00200020, VR::CS, "");
    put_str(ds, 0x00200060, VR::CS, "");
    (void)ds.set_uint16(0x00280002, 1);
    put_str(ds, 0x00280004, VR::CS, "MONOCHROME2");
    if (frames != 1) put_str(ds, 0x00280008, VR::IS, std::to_string(frames));
    (void)ds.set_uint16(0x00280010, rows);
    (void)ds.set_uint16(0x00280011, cols);
    (void)ds.set_uint16(0x00280100, bits);
    (void)ds.set_uint16(0x00280101, bits);
    (void)ds.set_uint16(0x00280102, static_cast<std::uint16_t>(bits - 1));
    (void)ds.set_uint16(0x00280103, 0);
    const std::size_t n = std::size_t{rows} * cols * (bits / 8u) * frames;
    Buf px(n);
    for (std::size_t i = 0; i < n; ++i) px[i] = static_cast<std::uint8_t>((i * 7u + i / 64u) & 0x7Fu);
    (void)ds.set_bytes(0x7FE00010, bits > 8 ? VR::OW : VR::OB, Bytes::copy_of(px.data(), px.size()));
    return ds;
}

inline Buf part10(const Dataset& ds, std::string_view ts = siaara::ts::ExplicitVRLittleEndian) {
    siaara::DicomFile f;
    f.dataset = ds;
    f.transfer_syntax = std::string(ts);
    Buf out;
    (void)siaara::write_part10(f, out);
    return out;
}

// ---------------------------------------------------------------- raw explicit VR LE emitter
struct Raw {
    Buf b;
    void u16(std::uint16_t v) {
        b.push_back(static_cast<std::uint8_t>(v));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void tag(Tag t) {
        u16(siaara::tag_group(t));
        u16(siaara::tag_element(t));
    }
    void header(Tag t, VR vr, std::uint32_t len) {
        tag(t);
        const auto v = static_cast<std::uint16_t>(vr);
        b.push_back(static_cast<std::uint8_t>(v >> 8));
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        if (siaara::vr_has_long_length(vr)) {
            u16(0);
            u32(len);
        } else {
            u16(static_cast<std::uint16_t>(len));
        }
    }
    void value(Tag t, VR vr, const std::uint8_t* p, std::size_t n) {
        header(t, vr, static_cast<std::uint32_t>(n));
        b.insert(b.end(), p, p + n);
    }
    void str(Tag t, VR vr, const std::string& s) {  // no padding added
        value(t, vr, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }
    void implicit_str(Tag t, const std::string& s) {  // tag + 32-bit length, no VR
        tag(t);
        u32(static_cast<std::uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
    void element(const Element& e) {
        if (e.is_sequence()) {
            header(e.tag, VR::SQ, siaara::kUndefinedLength);
            for (const Dataset& it : e.items) {
                tag(0xFFFEE000);
                u32(siaara::kUndefinedLength);
                for (const Element& c : it.elements()) element(c);
                tag(0xFFFEE00D);
                u32(0);
            }
            tag(0xFFFEE0DD);
            u32(0);
            return;
        }
        if (e.is_encapsulated()) {
            header(e.tag, VR::OB, siaara::kUndefinedLength);
            for (const Bytes& f : e.fragments) {
                tag(0xFFFEE000);
                u32(static_cast<std::uint32_t>(f.size()));
                b.insert(b.end(), f.data(), f.data() + f.size());
            }
            tag(0xFFFEE0DD);
            u32(0);
            return;
        }
        value(e.tag, e.vr, e.value.data(), e.value.size());
    }
    // Emits every element of `ds`; `hook` may emit something else instead (returns true).
    void dataset(const Dataset& ds, const std::function<bool(const Element&, Raw&)>& hook = {}) {
        for (const Element& e : ds.elements())
            if (!hook || !hook(e, *this)) element(e);
    }
};

// Preamble + "DICM" + a meta group for `ds` + `body`.
inline Buf with_meta(const Dataset& ds, const Buf& body, std::string_view ts = siaara::ts::ExplicitVRLittleEndian,
                     bool preamble = true, const std::string& ms_instance_override = {}, bool impl_uid = true) {
    Raw m;
    const std::string cls = ds.get_string(0x00080016).value_or("");
    std::string inst = ms_instance_override.empty() ? ds.get_string(0x00080018).value_or("") : ms_instance_override;
    auto pad_ui = [](std::string s) {
        if (s.size() & 1u) s.push_back('\0');
        return s;
    };
    const std::uint8_t ver[2] = {0, 1};
    m.value(0x00020001, VR::OB, ver, 2);
    m.str(0x00020002, VR::UI, pad_ui(cls));
    m.str(0x00020003, VR::UI, pad_ui(inst));
    m.str(0x00020010, VR::UI, pad_ui(std::string(ts)));
    if (impl_uid) m.str(0x00020012, VR::UI, pad_ui("1.2.826.0.1.3680043.10.1408.99"));
    Raw gl;
    gl.header(0x00020000, VR::UL, 4);
    gl.u32(static_cast<std::uint32_t>(m.b.size()));
    Buf out;
    if (preamble) out.assign(128, 0);
    out.insert(out.end(), {'D', 'I', 'C', 'M'});
    out.insert(out.end(), gl.b.begin(), gl.b.end());
    out.insert(out.end(), m.b.begin(), m.b.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

inline Buf body_of(const Dataset& ds, const std::function<bool(const Element&, Raw&)>& hook = {}) {
    Raw r;
    r.dataset(ds, hook);
    return r.b;
}

struct Sample {
    std::string name;
    Buf bytes;
    std::vector<std::string> expect;      // finding codes the validator must report
    std::string fix;                      // categories that repair them
    std::vector<std::string> gone_after;  // codes that must be gone after the fix
};

inline Dataset encapsulate(Dataset ds, const std::vector<Buf>& frames, bool bot = true) {
    Element px;
    px.tag = 0x7FE00010;
    px.vr = VR::OB;
    px.undefined_length = true;
    Buf table;
    std::uint32_t at = 0;
    for (const Buf& f : frames) {
        for (int i = 0; i < 4; ++i) table.push_back(static_cast<std::uint8_t>(at >> (8 * i)));
        at += 8 + static_cast<std::uint32_t>(f.size() + (f.size() & 1u));
    }
    px.fragments.push_back(bot ? Bytes::copy_of(table.data(), table.size()) : Bytes());
    for (Buf f : frames) {
        if (f.size() & 1u) f.push_back(0);
        px.fragments.push_back(Bytes::copy_of(f.data(), f.size()));
    }
    ds.put(std::move(px));
    return ds;
}

inline std::vector<Buf> rle_frames(const Dataset& native, std::uint32_t frames, std::uint16_t rows, std::uint16_t cols) {
    siaara::codec::FrameInfo fi;
    fi.rows = rows;
    fi.columns = cols;
    fi.samples_per_pixel = 1;
    fi.bits_allocated = 8;
    fi.bits_stored = 8;
    const Element* px = native.find(0x7FE00010);
    std::vector<Buf> out;
    const std::size_t fb = std::size_t{rows} * cols;
    for (std::uint32_t k = 0; k < frames; ++k) {
        Buf enc;
        (void)siaara::codec::encode_rle(std::span<const std::uint8_t>(px->value.data() + k * fb, fb), fi, {}, enc);
        out.push_back(std::move(enc));
    }
    return out;
}

inline std::vector<Sample> all_samples() {
    std::vector<Sample> s;
    using siaara::ts::ExplicitVRLittleEndian;
    const Dataset base = base_sc();

    s.push_back({"valid_sc", part10(base), {}, "", {}});

    {  // 16-bit samples, Bits Allocated says 8
        Dataset ds = base_sc("1.2.826.0.1.3680043.10.1408.1.2", 8, 8, 16);
        (void)ds.set_uint16(0x00280100, 8);
        (void)ds.set_uint16(0x00280101, 8);
        (void)ds.set_uint16(0x00280102, 7);
        s.push_back({"pixel_bits_allocated", part10(ds), {"pixel-length"}, "pixels", {"pixel-length"}});
    }
    {  // three frames of data, Number of Frames 2
        Dataset ds = base_sc("1.2.826.0.1.3680043.10.1408.1.3", 8, 8, 8, 3);
        (void)ds.set_string(0x00280008, VR::IS, "2");
        s.push_back({"pixel_number_of_frames", part10(ds), {"pixel-length"}, "pixels", {"pixel-length"}});
    }
    {  // JPEG 2000 lossless, 12-bit unsigned 16x16; header says Rows 15, Bits Stored 16, signed
        Dataset ds = base_sc("1.2.826.0.1.3680043.10.1408.1.4", 16, 16, 16);
        const Element* px = ds.find(0x7FE00010);
        Buf vals(px->value.data(), px->value.data() + px->value.size());
        for (std::size_t i = 0; i + 1 < vals.size(); i += 2) {
            const std::uint16_t v = static_cast<std::uint16_t>((i * 13u) & 0x0FFFu);
            vals[i] = static_cast<std::uint8_t>(v);
            vals[i + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        siaara::codec::FrameInfo fi;
        fi.rows = 16;
        fi.columns = 16;
        fi.bits_allocated = 16;
        fi.bits_stored = 12;
        Buf enc;
        (void)siaara::codec::encode_j2k(vals, fi, {}, enc);
        Dataset d2 = encapsulate(ds, {enc});
        (void)d2.set_uint16(0x00280010, 15);
        (void)d2.set_uint16(0x00280101, 16);
        (void)d2.set_uint16(0x00280102, 15);
        (void)d2.set_uint16(0x00280103, 1);
        s.push_back({"pixel_j2k_header", part10(d2, "1.2.840.10008.1.2.4.90"),
                     {"rows-codestream", "precision-codestream", "signedness-codestream"}, "pixels",
                     {"rows-codestream", "precision-codestream", "signedness-codestream"}});
    }
    {  // odd value length, wrong group length
        const Buf body = body_of(base, [](const Element& e, Raw& r) {
            if (e.tag == 0x00080008) {
                Raw g;
                g.str(0x00080008, VR::CS, "DERIVED\\SECONDARY");
                r.header(0x00080000, VR::UL, 4);
                r.u32(12345);  // wrong
                r.b.insert(r.b.end(), g.b.begin(), g.b.end());
                return true;
            }
            if (e.tag == 0x00200010) {
                r.str(0x00200010, VR::SH, "ABC");  // odd length
                return true;
            }
            return false;
        });
        s.push_back({"struct_odd_and_group_length", with_meta(base, body), {"odd-length", "group-length-mismatch"},
                     "structural", {"odd-length", "group-length-mismatch"}});
    }
    {  // out of order and duplicated elements
        const Buf body = body_of(base, [](const Element& e, Raw& r) {
            if (e.tag == 0x00080060) {
                r.element(e);
                r.str(0x00080020, VR::DA, "20240102");  // out of order and a duplicate
                return true;
            }
            return false;
        });
        s.push_back({"struct_order_duplicate", with_meta(base, body), {"element-order", "duplicate-element"},
                     "structural", {"element-order", "duplicate-element"}});
    }
    {  // undefined-length sequence never closed (the data ends inside its item)
        Dataset ds = base;
        ds.remove(0x7FE00010);
        Raw r;
        r.dataset(ds);
        r.header(0x00400275, VR::SQ, siaara::kUndefinedLength);  // Request Attributes Sequence
        r.tag(0xFFFEE000);
        r.u32(siaara::kUndefinedLength);
        r.str(0x00401001, VR::SH, "RP1 ");
        s.push_back({"struct_unterminated_sequence", with_meta(ds, r.b), {"unterminated-item", "unterminated-sequence"},
                     "structural", {"unterminated-item", "unterminated-sequence"}});
    }
    {  // "DICM" without preamble, meta instance UID differs, no Implementation Class UID
        s.push_back({"struct_meta", with_meta(base, body_of(base), ExplicitVRLittleEndian, false,
                                              "1.2.826.0.1.3680043.10.1408.1.999", false),
                     {"preamble-missing", "meta-mismatch", "meta-missing-element"}, "structural",
                     {"preamble-missing", "meta-mismatch", "meta-missing-element"}});
    }
    {  // raw data set, no Part 10 header
        s.push_back({"struct_raw_dataset", body_of(base), {"not-part10"}, "structural", {"not-part10"}});
    }
    {  // element without VR inside explicit VR data
        const Buf body = body_of(base, [](const Element& e, Raw& r) {
            if (e.tag == 0x00200010) {
                r.implicit_str(0x00200010, "S1");
                return true;
            }
            return false;
        });
        s.push_back({"struct_implicit_element", with_meta(base, body), {"invalid-vr"}, "structural", {"invalid-vr"}});
    }
    {  // RLE, 3 frames, Basic Offset Table wrong
        Dataset ds = base_sc("1.2.826.0.1.3680043.10.1408.1.5", 8, 8, 8, 3);
        Dataset enc = encapsulate(ds, rle_frames(ds, 3, 8, 8));
        Element* px = enc.find(0x7FE00010);
        Buf bad = {0, 0, 0, 0, 5, 0, 0, 0, 9, 0, 0, 0};
        px->fragments[0] = Bytes::copy_of(bad.data(), bad.size());
        s.push_back({"struct_bad_offset_table", part10(enc, "1.2.840.10008.1.2.5"), {"bot-invalid"}, "structural",
                     {"bot-invalid"}});
    }
    {  // value formats
        Dataset ds = base;
        put_str(ds, 0x00080020, VR::DA, "2024.01.02");
        put_str(ds, 0x00080030, VR::TM, "10:15:00");
        put_str(ds, 0x00080023, VR::DA, "01/02/2024");  // ambiguous: left as is
        put_str(ds, 0x00200011, VR::IS, "7.0");
        put_str(ds, 0x00101010, VR::AS, "045y");
        put_str(ds, 0x00181050, VR::DS, "1.25000000000000000");
        const Buf body = body_of(ds, [](const Element& e, Raw& r) {
            if (e.tag == 0x0020000E) {
                r.str(0x0020000E, VR::UI, "1.2.826.0.1.3680043.10.1408.3.12 ");  // space padding
                return true;
            }
            return false;
        });
        s.push_back({"values_formats", with_meta(ds, body), {"value-invalid", "ui-padding"}, "values", {"ui-padding"}});
    }
    {  // wrong VRs: Series Number as LO, Modality as UN, Rows as SS
        const Buf body = body_of(base, [](const Element& e, Raw& r) {
            if (e.tag == 0x00200011) {
                r.str(0x00200011, VR::LO, "1 ");
                return true;
            }
            if (e.tag == 0x00080060) {
                r.str(0x00080060, VR::UN, "OT");
                return true;
            }
            if (e.tag == 0x00280010) {
                r.value(0x00280010, VR::SS, e.value.data(), e.value.size());
                return true;
            }
            return false;
        });
        s.push_back({"values_vr", with_meta(base, body), {"vr-mismatch", "vr-un"}, "values", {"vr-mismatch", "vr-un"}});
    }
    {  // invalid UID, referenced elsewhere
        Dataset ds = base;
        put_str(ds, 0x0020000D, VR::UI, "1.2.03.4");
        Dataset item;
        put_str(item, 0x00081150, VR::UI, "1.2.840.10008.3.1.2.3.1");
        put_str(item, 0x00081155, VR::UI, "1.2.03.4");
        Element sq;
        sq.tag = 0x00081110;
        sq.vr = VR::SQ;
        sq.items.push_back(item);
        ds.put(std::move(sq));
        s.push_back({"uids_invalid", part10(ds), {"value-invalid"}, "uids", {"value-invalid"}});
    }
    {  // UTF-8 text, no Specific Character Set
        Dataset ds = base;
        (void)ds.set_bytes(0x00100010, VR::PN, Bytes::from_string("M\xC3\xBCller^J\xC3\xBCrgen "));
        s.push_back({"charset_utf8_undeclared", part10(ds), {"charset-undeclared"}, "charset", {"charset-undeclared"}});
    }
    {  // Latin-1 text, no Specific Character Set: ambiguous without --charset
        Dataset ds = base;
        (void)ds.set_bytes(0x00100010, VR::PN, Bytes::from_string("M\xFCller^J\xFCrgen "));
        s.push_back({"charset_latin1_undeclared", part10(ds), {"charset-undeclared"}, "", {}});
    }
    {  // missing attributes
        Dataset ds = base;
        ds.remove(0x00080020);  // Study Date, Type 2
        ds.remove(0x00080090);  // Referring Physician's Name, Type 2
        ds.remove(0x0020000E);  // Series Instance UID, Type 1
        ds.remove(0x00080060);  // Modality, Type 1 (not repairable)
        s.push_back({"attributes_missing", part10(ds), {"missing-type1", "missing-type2"}, "attributes", {"missing-type2"}});
    }
    {  // native multi-frame truncated inside frame 3 of 4
        Dataset ds = base_sc("1.2.826.0.1.3680043.10.1408.1.6", 8, 8, 8, 4);
        Buf f = part10(ds);
        f.resize(f.size() - 64 - 20);
        s.push_back({"rescue_native_truncated", f, {"value-truncated"}, "rescue", {"pixel-truncated", "pixel-length"}});
    }
    {  // RLE, 4 frames: frame 1 corrupt, file cut inside frame 3
        Dataset ds = base_sc("1.2.826.0.1.3680043.10.1408.1.7", 8, 8, 8, 4);
        auto frames = rle_frames(ds, 4, 8, 8);
        frames[1][0] = 99;  // 99 segments: not a valid RLE header
        Dataset enc = encapsulate(ds, frames, false);
        Buf f = part10(enc, "1.2.840.10008.1.2.5");
        f.resize(f.size() - 8 - frames[3].size() / 2);
        s.push_back({"rescue_rle_damaged", f, {"fragment-truncated"}, "rescue", {"frames-missing"}});
    }
    return s;
}

// Four instances of one series with study-level problems.
inline std::vector<Buf> study_set() {
    std::vector<Buf> out;
    const char* uids[4] = {"1.2.826.0.1.3680043.10.1408.1.10", "1.2.826.0.1.3680043.10.1408.1.11",
                           "1.2.826.0.1.3680043.10.1408.1.11", "1.2.826.0.1.3680043.10.1408.1.13"};
    const char* numbers[4] = {"1", "2", "2", "5"};
    for (int i = 0; i < 4; ++i) {
        Dataset ds = base_sc(uids[i]);
        put_str(ds, 0x00200013, VR::IS, numbers[i]);
        put_str(ds, 0x00200032, VR::DS, "0\\0\\" + std::to_string(i * 2));
        put_str(ds, 0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
        if (i == 1) put_str(ds, 0x00100010, VR::PN, "Otherpatient^Wrong");
        if (i == 2) put_str(ds, 0x00080064, VR::CS, "SI");  // different content, same SOP Instance UID
        if (i == 3) ds.remove(0x0020000D);
        out.push_back(part10(ds));
    }
    return out;
}

}  // namespace repair_samples
