// F1: a no-edit read -> save in the same transfer syntax must reproduce the file byte for byte;
// edits get sensible encodings and kept group lengths are never stale.
// F3: stored-order raw bytes for big-endian files.
#include <cstdint>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "test.hpp"

using namespace siaara;

namespace {

// Builds DICOM byte streams by hand, so the tests do not depend on the writer under test.
struct Stream {
    std::vector<std::uint8_t> b;
    bool big = false;
    bool explicit_vr = true;

    void u16(std::uint16_t v) {
        if (big) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
        else { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }
    }
    void u32(std::uint32_t v) {
        if (big) { u16(static_cast<std::uint16_t>(v >> 16)); u16(static_cast<std::uint16_t>(v)); }
        else { u16(static_cast<std::uint16_t>(v)); u16(static_cast<std::uint16_t>(v >> 16)); }
    }
    void tag(Tag t) { u16(tag_group(t)); u16(tag_element(t)); }
    void raw(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
    void bytes(std::initializer_list<std::uint8_t> v) { b.insert(b.end(), v); }

    // Element header; long-form VRs per PS3.5 7.1.2.
    void hdr(Tag t, const char* vr, std::uint32_t len) {
        tag(t);
        if (!explicit_vr) { u32(len); return; }
        raw(std::string(vr, 2));
        const std::string v(vr, 2);
        if (v == "OB" || v == "OW" || v == "SQ" || v == "UN" || v == "UT" || v == "OF" || v == "UC") {
            u16(0);
            u32(len);
        } else {
            u16(static_cast<std::uint16_t>(len));
        }
    }
    void str(Tag t, const char* vr, const std::string& v) {
        hdr(t, vr, static_cast<std::uint32_t>(v.size()));
        raw(v);
    }
    void us(Tag t, std::uint16_t v) { hdr(t, "US", 2); u16(v); }
    void ul(Tag t, std::uint32_t v) { hdr(t, "UL", 4); u32(v); }
    void item(std::uint32_t len) { tag(tags::Item); u32(len); }
    void item_end() { tag(tags::ItemDelimitationItem); u32(0); }
    void seq_end() { tag(tags::SequenceDelimitationItem); u32(0); }
    std::size_t size() const { return b.size(); }
    // Patches a 32-bit length written at `at` with the byte count from `from` to now.
    void patch(std::size_t at, std::size_t from) {
        const auto n = static_cast<std::uint32_t>(b.size() - from);
        std::vector<std::uint8_t> save;
        save.swap(b);
        b.assign(save.begin(), save.begin() + static_cast<std::ptrdiff_t>(at));
        u32(n);
        b.insert(b.end(), save.begin() + static_cast<std::ptrdiff_t>(at) + 4, save.end());
    }
};

// Preamble (with non-zero bytes), "DICM" and a file meta group in explicit VR LE.
std::vector<std::uint8_t> part10(const std::string& ts_uid_padded, const std::vector<std::uint8_t>& body) {
    Stream m;
    m.hdr(0x00020001, "OB", 2);
    m.bytes({0, 1});
    m.str(0x00020002, "UI", std::string("1.2.840.10008.5.1.4.1.1.7") + '\0');
    m.str(0x00020003, "UI", "2.25.42");  // odd length, unpadded
    m.str(0x00020010, "UI", ts_uid_padded);
    m.str(0x00020012, "UI", "2.25.99");
    m.str(0x00020013, "SH", "OTHER_1 ");
    Stream f;
    for (int i = 0; i < 128; ++i) f.b.push_back(static_cast<std::uint8_t>(i == 0 ? 'I' : i == 1 ? 'I' : i));
    f.raw("DICM");
    f.ul(0x00020000, static_cast<std::uint32_t>(m.size()));
    f.b.insert(f.b.end(), m.b.begin(), m.b.end());
    f.b.insert(f.b.end(), body.begin(), body.end());
    return f.b;
}

std::vector<std::uint8_t> resave(const std::vector<std::uint8_t>& in, const WriteOptions& wo = {},
                                 const char* ts_uid = nullptr) {
    auto r = read_part10(Bytes::copy_of(in.data(), in.size()));
    if (!r) return {};
    DicomFile f = std::move(r).value();
    if (ts_uid) f.transfer_syntax = ts_uid;
    std::vector<std::uint8_t> out;
    if (!write_part10(f, out, wo)) return {};
    return out;
}

// Explicit VR LE body exercising undefined-length SQ/items, mixed modes, group lengths
// (including one in an item and one that is wrong in the source), odd lengths and an odd last
// encapsulated fragment. Built twice: once for explicit VR, once for the encapsulated TS.
std::vector<std::uint8_t> explicit_body(bool group_length_wrong) {
    Stream s;
    // Group 0008 with a group length.
    const std::size_t g8 = s.size() + 8;
    s.ul(0x00080000, 0);
    const std::size_t g8body = s.size();
    s.str(0x00080016, "UI", std::string("1.2.840.10008.5.1.4.1.1.7") + '\0');
    s.str(0x00080018, "UI", "2.25.42");          // odd, unpadded
    s.str(0x00080060, "CS", "OT");
    // (0008,1140) undefined SQ with one undefined item and one explicit item.
    s.hdr(0x00081140, "SQ", kUndefinedLength);
    s.item(kUndefinedLength);
    s.str(0x00081150, "UI", "1.2.3");             // odd
    s.item_end();
    const std::size_t il = s.size() + 4;
    s.item(0);
    const std::size_t ib = s.size();
    const std::size_t ig = s.size() + 8;
    s.ul(0x00080000, 0);                            // group length inside an item
    const std::size_t igb = s.size();
    s.str(0x00081155, "UI", "1.2.4\0");
    s.patch(ig, igb);
    s.patch(il, ib);
    s.seq_end();
    s.patch(g8, g8body);
    if (group_length_wrong) {
        // Overwrite the group length with a wrong value; a keep-as-read save must not "fix" it.
        s.b[g8] = static_cast<std::uint8_t>(s.b[g8] + 2);
    }
    // An odd OB value in a private group.
    s.str(0x00090010, "LO", "ACME 1.0");
    s.hdr(0x00091001, "OB", 3);
    s.bytes({1, 2, 3});
    // Group 0010: explicit-length SQ holding undefined-length items, odd PN.
    s.ul(0x00100000, 0);
    const std::size_t g10 = s.size() - 4;
    const std::size_t g10body = s.size();
    s.str(0x00100010, "PN", "Doe^J");               // odd
    s.hdr(0x00101002, "SQ", 0);
    const std::size_t sq = s.size() - 4;
    const std::size_t sqb = s.size();
    s.item(kUndefinedLength);
    s.str(0x00100020, "LO", "X1");
    s.item_end();
    s.patch(sq, sqb);
    s.patch(g10, g10body);
    // Group 0028 without a group length.
    s.us(0x00280002, 1);
    s.str(0x00280004, "CS", "MONOCHROME2 ");
    s.us(0x00280010, 1);
    s.us(0x00280011, 3);
    s.us(0x00280100, 8);
    s.us(0x00280101, 8);
    s.us(0x00280102, 7);
    s.us(0x00280103, 0);
    return s.b;
}

std::vector<std::uint8_t> encapsulated_pixels(const std::vector<std::uint8_t>& head) {
    Stream s;
    s.b = head;
    s.ul(0x7FE00000, 0);
    const std::size_t gp = s.size() - 4;
    const std::size_t gpb = s.size();
    s.hdr(tags::PixelData, "OB", kUndefinedLength);
    s.item(0);                                       // empty basic offset table
    s.item(5);
    s.bytes({0xFF, 0xD8, 0x01, 0x02, 0x03});         // odd last fragment
    s.seq_end();
    s.patch(gp, gpb);
    return s.b;
}

}  // namespace

TEST_CASE("fidelity: unedited explicit VR LE file round-trips byte for byte") {
    Stream s;
    s.b = explicit_body(false);
    s.hdr(tags::PixelData, "OB", 3);                 // odd native pixel data
    s.bytes({7, 8, 9});
    const auto in = part10("1.2.840.10008.1.2.1 ", s.b);  // TS padded with a space, not NUL
    const auto out = resave(in);
    REQUIRE(!out.empty());
    CHECK(out == in);
}

TEST_CASE("fidelity: wrong group length in the source is kept when the group is unchanged") {
    Stream s;
    s.b = explicit_body(true);
    const auto in = part10(std::string("1.2.840.10008.1.2.1") + '\0', s.b);
    const auto out = resave(in);
    CHECK(out == in);
}

TEST_CASE("fidelity: encapsulated pixel data keeps an odd last fragment and group 7FE0 length") {
    const auto in = part10(std::string("1.2.840.10008.1.2.4.50"), encapsulated_pixels(explicit_body(false)));
    const auto out = resave(in);
    REQUIRE(!out.empty());
    CHECK(out == in);
    // Padding on request: the fragment becomes even and its item length changes.
    WriteOptions pad;
    pad.odd_lengths = OddLengths::Pad;
    const auto padded = resave(in, pad);
    auto g = read_part10(Bytes::copy_of(padded.data(), padded.size()));
    REQUIRE(g.is_ok());
    const Element* px = g->dataset.find(tags::PixelData);
    REQUIRE(px != nullptr && px->fragments.size() == 2);
    CHECK_EQ(px->fragments[1].size(), 6u);
    CHECK_EQ(g->dataset.find(0x00100010)->value.size(), 6u);
}

TEST_CASE("fidelity: implicit VR LE with private undefined-length sequences round-trips") {
    Stream s;
    s.explicit_vr = false;
    s.ul(0x00080000, 0);
    const std::size_t g8 = s.size() - 4, g8b = s.size();
    s.str(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.7");  // odd
    s.str(0x00080060, "CS", "OT");
    s.patch(g8, g8b);
    s.str(0x00290010, "LO", "ACME");
    // Private SQ, undefined length: an implicit VR reader without the private dictionary only
    // recognises it because of the undefined length.
    s.hdr(0x00291010, "SQ", kUndefinedLength);
    s.item(kUndefinedLength);
    s.str(0x00291011, "LO", "A");                    // odd, inside a private item
    s.item_end();
    s.item(kUndefinedLength);
    s.item_end();                                     // empty item
    s.seq_end();
    // Known SQ, undefined length, explicit-length item.
    s.hdr(0x00400275, "SQ", kUndefinedLength);
    const std::size_t il = s.size() + 4;
    s.item(0);
    const std::size_t ib = s.size();
    s.str(0x00400009, "SH", "SPS1");
    s.patch(il, ib);
    s.seq_end();
    const auto in = part10(std::string("1.2.840.10008.1.2") + '\0', s.b);
    const auto out = resave(in);
    REQUIRE(!out.empty());
    CHECK(out == in);

    // The old behaviour (explicit lengths everywhere) is still available on request.
    WriteOptions ex;
    ex.sequence_lengths = SequenceLengths::Explicit;
    const auto out2 = resave(in, ex);
    CHECK(out2 != in);
    CHECK(out2.size() < in.size());
}

TEST_CASE("fidelity: explicit VR big endian round-trips byte for byte") {
    Stream s;
    s.big = true;
    s.ul(0x00080000, 0);
    const std::size_t g8 = s.size() - 4, g8b = s.size();
    s.str(0x00080016, "UI", std::string("1.2.840.10008.5.1.4.1.1.7") + '\0');
    s.str(0x00080060, "CS", "OT");
    s.patch(g8, g8b);
    s.us(0x00280010, 1);
    s.us(0x00280011, 2);
    s.us(0x00280100, 16);
    s.hdr(0x00280106, "US", 2);
    s.u16(0x0102);
    s.str(0x00291001, "UN", "abc");                  // odd UN (never swapped)
    s.hdr(tags::PixelData, "OW", 4);
    s.u16(0x1234);
    s.u16(0xABCD);
    const auto in = part10(std::string("1.2.840.10008.1.2.2") + '\0', s.b);
    const auto out = resave(in);
    REQUIRE(!out.empty());
    CHECK(out == in);
}

TEST_CASE("fidelity: UN sequences with undefined length are written back as UN") {
    Stream s;
    s.str(0x00080060, "CS", "OT");
    s.str(0x00290010, "LO", "ACME");
    s.hdr(0x00291010, "UN", kUndefinedLength);
    {
        Stream inner;
        inner.explicit_vr = false;                   // UN items are implicit VR LE (PS3.5 6.2.2)
        inner.item(kUndefinedLength);
        inner.str(0x00291011, "LO", "Q");
        inner.item_end();
        inner.seq_end();
        s.b.insert(s.b.end(), inner.b.begin(), inner.b.end());
    }
    const auto in = part10(std::string("1.2.840.10008.1.2.1") + '\0', s.b);
    const auto out = resave(in);
    CHECK(out == in);
    auto g = read_part10(Bytes::copy_of(in.data(), in.size()));
    REQUIRE(g.is_ok());
    const Element* e = g->dataset.find(0x00291010);
    REQUIRE(e != nullptr && e->is_sequence() && e->items.size() == 1);
    CHECK_EQ(e->items[0].get_string(0x00291011).value_or(""), "Q");
}

TEST_CASE("fidelity: edits recompute kept group lengths and pad edited odd values") {
    Stream s;
    s.b = explicit_body(true);  // group 0008 length wrong in the source
    const auto in = part10(std::string("1.2.840.10008.1.2.1") + '\0', s.b);
    auto r = read_part10(Bytes::copy_of(in.data(), in.size()));
    REQUIRE(r.is_ok());
    DicomFile f = std::move(r).value();
    (void)f.dataset.set_string(0x00080060, VR::CS, "MRX");  // edited: odd -> padded "MRX "
    (void)f.dataset.set_string(0x00100010, VR::PN, "Roe^A");
    std::vector<std::uint8_t> out;
    REQUIRE(write_part10(f, out).is_ok());
    auto g = read_part10(Bytes::copy_of(out.data(), out.size()));
    REQUIRE(g.is_ok());
    CHECK_EQ(g->dataset.find(0x00080060)->value.size(), 4u);
    CHECK_EQ(g->dataset.find(0x00100010)->value.size(), 6u);
    // Both kept group lengths now match the encoded groups.
    for (Tag gl : {Tag{0x00080000}, Tag{0x00100000}}) {
        const Element* e = g->dataset.find(gl);
        REQUIRE(e != nullptr);
        CHECK(e->read.group_bytes == load_le32(e->value.data()));
    }
    // Unedited odd values are still unpadded.
    CHECK_EQ(g->dataset.find(0x00080018)->value.size(), 7u);
    // Sequences keep their modes.
    CHECK(g->dataset.find(0x00081140)->undefined_length);
    CHECK(!g->dataset.find(0x00101002)->undefined_length);
}

TEST_CASE("fidelity: group length options drop, keep and recompute") {
    Stream s;
    s.b = explicit_body(false);
    const auto in = part10(std::string("1.2.840.10008.1.2.1") + '\0', s.b);
    WriteOptions drop;
    drop.group_lengths = GroupLengths::Drop;
    auto d = read_part10(Bytes::copy_of(resave(in, drop).data(), resave(in, drop).size()));
    REQUIRE(d.is_ok());
    CHECK(!d->dataset.contains(0x00080000));
    CHECK(!d->dataset.contains(0x00100000));
    CHECK(!d->dataset.find(0x00081140)->items[1].contains(0x00080000));
    WriteOptions rec;
    rec.group_lengths = GroupLengths::Recompute;
    const auto rb = resave(in, rec);
    auto r = read_part10(Bytes::copy_of(rb.data(), rb.size()));
    REQUIRE(r.is_ok());
    for (Tag gl : {Tag{0x00080000}, Tag{0x00090000}, Tag{0x00100000}, Tag{0x00280000}}) {
        const Element* e = r->dataset.find(gl);
        REQUIRE(e != nullptr);
        CHECK(e->read.group_bytes == load_le32(e->value.data()));
    }
}

TEST_CASE("fidelity: transcoding to implicit VR recomputes group lengths, private SQs undefined") {
    Stream s;
    s.ul(0x00080000, 0);
    const std::size_t g8 = s.size() - 4, g8b = s.size();
    s.str(0x00080060, "CS", "OT");
    s.patch(g8, g8b);
    s.str(0x00290010, "LO", "ACME");
    s.hdr(0x00291010, "SQ", 0);                      // explicit-length private SQ (explicit VR)
    const std::size_t sq = s.size() - 4, sqb = s.size();
    const std::size_t il = s.size() + 4;
    s.item(0);
    const std::size_t ib = s.size();
    s.str(0x00291011, "LO", "AB");
    s.patch(il, ib);
    s.patch(sq, sqb);
    const auto in = part10(std::string("1.2.840.10008.1.2.1") + '\0', s.b);
    const auto out = resave(in, {}, "1.2.840.10008.1.2");
    REQUIRE(!out.empty());
    auto g = read_part10(Bytes::copy_of(out.data(), out.size()));
    REQUIRE(g.is_ok());
    const Element* gl = g->dataset.find(0x00080000);
    REQUIRE(gl != nullptr);
    CHECK_EQ(load_le32(gl->value.data()), 10u);       // 8-byte implicit header + "OT"
    const Element* p = g->dataset.find(0x00291010);
    REQUIRE(p != nullptr && p->is_sequence());       // recognised without a private dictionary
    CHECK(p->undefined_length);
    CHECK_EQ(p->items.size(), 1u);
    // ... and back to explicit VR: same TS again keeps it as read.
    const auto back = resave(out);
    CHECK(back == out);
}

TEST_CASE("fidelity: sequences built in memory in implicit VR use undefined length when private") {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    (void)f.dataset.set_string(0x00290010, VR::LO, "ACME");
    Element seq;
    seq.tag = 0x00291010;
    seq.vr = VR::SQ;
    Dataset item;
    (void)item.set_string(0x00291011, VR::LO, "X");
    seq.items.push_back(std::move(item));
    f.dataset.put(std::move(seq));
    Element known;
    known.tag = 0x00081140;
    known.vr = VR::SQ;
    known.items.emplace_back();
    f.dataset.put(std::move(known));
    std::vector<std::uint8_t> out;
    REQUIRE(write_part10(f, out).is_ok());
    auto g = read_part10(Bytes::copy_of(out.data(), out.size()));
    REQUIRE(g.is_ok());
    CHECK(g->dataset.find(0x00291010)->undefined_length);
    CHECK(g->dataset.find(0x00291010)->is_sequence());
    CHECK(!g->dataset.find(0x00081140)->undefined_length);
    CHECK_EQ(g->dataset.find(0x00291010)->items[0].get_string(0x00291011).value_or(""), "X");
}

TEST_CASE("fidelity: stored_value_bytes returns big-endian bytes for big-endian files") {
    Stream s;
    s.big = true;
    s.us(0x00280010, 0x0102);
    s.us(0x00280100, 32);
    s.hdr(tags::PixelData, "OW", 4);
    s.bytes({0x11, 0x22, 0x33, 0x44});              // one 32-bit sample, big endian
    const auto in = part10(std::string("1.2.840.10008.1.2.2") + '\0', s.b);
    auto g = read_part10(Bytes::copy_of(in.data(), in.size()));
    REQUIRE(g.is_ok());
    const Element* rows = g->dataset.find(0x00280010);
    CHECK_EQ(rows->value.data()[0], 0x02);           // in memory: little endian
    std::vector<std::uint8_t> v;
    REQUIRE(stored_value_bytes(*rows, g->transfer_syntax, 32, v).is_ok());
    CHECK(v == (std::vector<std::uint8_t>{0x01, 0x02}));
    REQUIRE(stored_value_bytes(*g->dataset.find(tags::PixelData), g->transfer_syntax, 32, v).is_ok());
    CHECK(v == (std::vector<std::uint8_t>{0x11, 0x22, 0x33, 0x44}));
    REQUIRE(stored_value_bytes(*rows, ts::ExplicitVRLittleEndian, 0, v).is_ok());
    CHECK(v == (std::vector<std::uint8_t>{0x02, 0x01}));
}

TEST_CASE("F6b: GE private syntax 1.2.840.113619.5.2 has little-endian data set, big-endian pixels") {
    Stream s;
    s.explicit_vr = false;
    s.us(0x00280010, 1);
    s.us(0x00280011, 2);
    s.us(0x00280100, 16);
    // Icon image in a sequence: left as stored (only the main Pixel Data is big endian).
    s.hdr(0x00880200, "SQ", kUndefinedLength);
    s.item(kUndefinedLength);
    s.us(0x00280100, 16);
    s.hdr(tags::PixelData, "OW", 2);
    s.bytes({0x01, 0x02});
    s.item_end();
    s.seq_end();
    s.hdr(tags::PixelData, "OW", 4);
    s.bytes({0x12, 0x34, 0xAB, 0xCD});              // samples 0x1234, 0xABCD stored big endian
    const auto in = part10(std::string(ts::PrivateGEImplicitVRLittleEndianBigEndianPixels), s.b);
    auto g = read_part10(Bytes::copy_of(in.data(), in.size()));
    REQUIRE(g.is_ok());
    CHECK_EQ(g->dataset.get_int(0x00280011).value_or(0), 2);
    CHECK_EQ(g->dataset.get_int(tags::PixelData, 0).value_or(0), 0x1234);
    CHECK_EQ(g->dataset.get_int(tags::PixelData, 1).value_or(0), 0xABCD);
    CHECK_EQ(g->dataset.find(0x00880200)->items[0].get_int(tags::PixelData).value_or(0), 0x0201);
    std::vector<std::uint8_t> v;
    REQUIRE(stored_value_bytes(*g->dataset.find(tags::PixelData), g->transfer_syntax, 16, v).is_ok());
    CHECK(v == (std::vector<std::uint8_t>{0x12, 0x34, 0xAB, 0xCD}));
    // Saved in the same syntax: byte for byte; to explicit VR LE: little-endian pixels.
    CHECK(resave(in) == in);
    const auto le = resave(in, {}, "1.2.840.10008.1.2.1");
    auto h = read_part10(Bytes::copy_of(le.data(), le.size()));
    REQUIRE(h.is_ok());
    CHECK_EQ(h->dataset.get_int(tags::PixelData, 1).value_or(0), 0xABCD);
}
