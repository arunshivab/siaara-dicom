// Private-tag dictionary (src/dict/private_dictionary.cpp, docs/PRIVATE_DICTIONARY.md): lookups,
// the owner's four rules, and the implicit VR reader / keep-as-read writer. Every file here is
// synthetic, built in code.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"

using namespace siaara;

namespace {

Element raw(Tag t, VR vr, const std::string& v) {
    Element e;
    e.tag = t;
    e.vr = vr;
    e.value = Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(v.data()), v.size());
    return e;
}

std::string le16(std::uint16_t v) { return std::string{static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)}; }
std::string le32(std::uint32_t v) { return le16(static_cast<std::uint16_t>(v & 0xFFFF)) + le16(static_cast<std::uint16_t>(v >> 16)); }

// One implicit VR LE item holding (0008,0100) "ABC " : FFFE,E000 len 12, then the element.
std::string implicit_item() {
    const std::string el = le16(0x0008) + le16(0x0100) + le32(4) + "ABC ";
    return le16(0xFFFE) + le16(0xE000) + le32(static_cast<std::uint32_t>(el.size())) + el;
}

// What a foreign implicit VR writer would produce: every private element is just bytes (UN here,
// which the implicit writer does not write), including a defined-length private sequence.
Dataset implicit_source() {
    Dataset ds;
    ds.put(raw(0x00100010u, VR::PN, "Doe^Jane"));
    // Siemens: decided VRs CS / LO / OB, and (0029,1028) whose documents state no VR.
    ds.put(raw(0x00290010u, VR::LO, "SIEMENS CSA HEADER"));
    ds.put(raw(0x00291008u, VR::UN, "IMAGE NUM 4 "));
    ds.put(raw(0x00291009u, VR::UN, "20100114"));
    ds.put(raw(0x00291010u, VR::UN, std::string("\x01\x02\x03\x04", 4)));
    ds.put(raw(0x00291028u, VR::UN, "XY"));
    // Siemens AX DYNACT (0019,xx03) is US.
    ds.put(raw(0x00190010u, VR::LO, "SIEMENS AX DYNACT "));
    ds.put(raw(0x00191003u, VR::UN, le16(513)));
    // Applicare (4105,xx10): documents disagree (CS / IS / US): undecided. (4105,xx0B) is SQ.
    ds.put(raw(0x41050010u, VR::LO, "Applicare/RadWorks/Version 6.0"));
    ds.put(raw(0x4105100Bu, VR::UN, implicit_item()));
    ds.put(raw(0x41051010u, VR::UN, "12"));
    // An unknown creator: stays UN.
    ds.put(raw(0x00330010u, VR::LO, "NOT A REAL CREATOR"));
    ds.put(raw(0x00331001u, VR::UN, "ABCD"));
    return ds;
}

std::vector<std::uint8_t> write_ts(const Dataset& ds, std::string_view ts_uid) {
    std::vector<std::uint8_t> out;
    if (!write_dataset(ds, ts_uid, out).is_ok()) out.clear();
    return out;
}

Result<Dataset> read_ts(const std::vector<std::uint8_t>& b, std::string_view ts_uid, bool lenient = true) {
    ReadOptions o;
    o.lenient = lenient;
    return read_dataset(Bytes::copy_of(b.data(), b.size()), ts_uid, o);
}

}  // namespace

TEST_CASE("private dict: exact and fuzzy creator match") {
    auto e = dict::lookup_private("SIEMENS CSA HEADER", 0x0029, 0x10);
    REQUIRE(e.has_value());
    CHECK(e->match == dict::PrivateMatch::Exact);
    CHECK(e->vr == VR::OB);
    CHECK_EQ(e->keyword, std::string_view("CSAImageHeaderInfo"));
    CHECK_EQ(e->name, std::string_view("CSA Image Header Info"));
    CHECK_EQ(e->vendor, std::string_view("Siemens"));
    CHECK(e->source_count > 0);
    CHECK(!e->source.document.empty() && e->source.page > 0);
    // Trailing spaces and NULs are value padding, not a different spelling.
    auto pad = dict::lookup_private(std::string_view("SIEMENS CSA HEADER \0", 20), 0x0029, 0x10);
    REQUIRE(pad.has_value());
    CHECK(pad->match == dict::PrivateMatch::Exact);
    // Case and whitespace differences fall back to the fuzzy match, flagged.
    auto f = dict::lookup_private("siemens  csa header", 0x0029, 0x10);
    REQUIRE(f.has_value());
    CHECK(f->match == dict::PrivateMatch::Fuzzy);
    CHECK_EQ(f->creator, std::string_view("SIEMENS CSA HEADER"));
    CHECK(f->index == e->index);
    // Two spellings in the documents: each has elements the other lacks. The exact spelling is
    // used where it has the element, the variant otherwise.
    auto a = dict::lookup_private("GEMS_ADWSoft_3D1", 0x0047, 0x01);
    REQUIRE(a.has_value());
    CHECK(a->match == dict::PrivateMatch::Exact);
    auto b = dict::lookup_private("GEMS_ADWSOFT_3D1", 0x0047, 0x01);
    REQUIRE(b.has_value());
    CHECK(b->match == dict::PrivateMatch::Fuzzy);
    CHECK_EQ(b->creator, std::string_view("GEMS_ADWSoft_3D1"));
    auto c = dict::lookup_private("GEMS_ADWSOFT_3D1", 0x0047, 0xA0);
    REQUIRE(c.has_value());
    CHECK(c->match == dict::PrivateMatch::Exact);
    // Not a creator / wrong group / wrong element.
    CHECK(!dict::lookup_private("SIEMENS CSA HEADERX", 0x0029, 0x10).has_value());
    CHECK(!dict::lookup_private("SIEMENS CSA HEADER", 0x0031, 0x10).has_value());
    CHECK(!dict::lookup_private("SIEMENS CSA HEADER", 0x0029, 0xFF).has_value());
    CHECK(dict::private_entry_count() > 8000);
    CHECK(dict::private_creator_count() > 300);
}

TEST_CASE("private dict: VR conflict stays undecided with every candidate listed") {
    auto e = dict::lookup_private("Applicare/RadWorks/Version 6.0", 0x4105, 0x10);
    REQUIRE(e.has_value());
    CHECK(e->conflict);
    CHECK(e->vr == VR::None);
    CHECK(e->candidate_count >= 3);
    CHECK(dict::implicit_private_vr("Applicare/RadWorks/Version 6.0", 0x4105, 0x10) == VR::UN);
    bool cs = false, is = false, us = false;
    for (std::size_t i = 0; i < e->candidate_count; ++i) {
        dict::PrivateCandidate c;
        REQUIRE(dict::private_candidate(*e, i, c));
        CHECK(!c.source.document.empty());
        cs |= c.vr == VR::CS;
        is |= c.vr == VR::IS;
        us |= c.vr == VR::US;
    }
    CHECK(cs && is && us);
    dict::PrivateCandidate c;
    CHECK(!dict::private_candidate(*e, e->candidate_count, c));
    // A decided VR has one candidate, the primary source.
    auto d = dict::lookup_private("SIEMENS CSA HEADER", 0x0029, 0x10);
    REQUIRE(d.has_value());
    CHECK_EQ(d->candidate_count, 1u);
    REQUIRE(dict::private_candidate(*d, 0, c));
    CHECK(c.vr == VR::OB);
    CHECK_EQ(c.source.document, d->source.document);
    // Documents that state no VR: undecided, no conflict, no candidates.
    auto n = dict::lookup_private("SIEMENS CSA HEADER", 0x0029, 0x28);
    REQUIRE(n.has_value());
    CHECK(n->vr == VR::None);
    CHECK(!n->conflict);
    CHECK_EQ(n->candidate_count, 0u);
}

TEST_CASE("private dict: assumed creators only where no stated entry exists") {
    // (0019,xx01) of GEMS_ACQU_01: stated rows say UI, rows with an assumed creator say DS. The
    // stated entry wins and the assumed rows do not make it a conflict.
    auto s = dict::lookup_private("GEMS_ACQU_01", 0x0019, 0x01);
    REQUIRE(s.has_value());
    CHECK(s->confidence == dict::PrivateConfidence::Stated);
    CHECK(s->vr == VR::UI);
    CHECK(!s->conflict);
    // Only assumed rows: included, lower confidence.
    auto a = dict::lookup_private("CANON MDW NON-IMAGE", 0x7FE1, 0x10);
    REQUIRE(a.has_value());
    CHECK(a->confidence == dict::PrivateConfidence::Assumed);
    CHECK(a->vr == VR::OB);
}

TEST_CASE("private dict: rows with no creator stated are not in the lookup") {
    // (0019,xx3A), (0019,xx3C), (0021,xxA0) appear in the documents only without a creator.
    for (std::string_view c : {std::string_view(""), std::string_view("   "), std::string_view("\0", 1)}) {
        CHECK(!dict::lookup_private(c, 0x0019, 0x3A).has_value());
        CHECK(!dict::lookup_private(c, 0x0019, 0x3C).has_value());
        CHECK(!dict::lookup_private(c, 0x0021, 0xA0).has_value());
        CHECK(!dict::lookup_private(c, 0x0029, 0x10).has_value());
    }
}

TEST_CASE("private dict: implicit VR read uses decided VRs, saves byte-identical") {
    const Dataset src = implicit_source();
    const auto bytes = write_ts(src, ts::ImplicitVRLittleEndian);
    REQUIRE(!bytes.empty());
    auto r = read_ts(bytes, ts::ImplicitVRLittleEndian);
    REQUIRE(r.is_ok());
    const Dataset& ds = *r;
    auto vr_of = [&](Tag t) { const Element* e = ds.find(t); return e ? e->vr : VR::None; };
    CHECK(vr_of(0x00291008u) == VR::CS);
    CHECK(vr_of(0x00291009u) == VR::LO);
    CHECK(vr_of(0x00291010u) == VR::OB);
    CHECK(vr_of(0x00291028u) == VR::UN);  // VR not stated
    CHECK(vr_of(0x00191003u) == VR::US);
    CHECK(vr_of(0x41051010u) == VR::UN);  // conflict: undecided
    CHECK(vr_of(0x4105100Bu) == VR::SQ);
    CHECK(vr_of(0x00331001u) == VR::UN);  // unknown creator
    CHECK(ds.find(0x00291008u)->read.private_vr == VR::CS);
    CHECK(ds.find(0x41051010u)->read.private_vr == VR::None);
    CHECK_EQ(ds.get_int(0x00191003u).value_or(0), 513);
    CHECK_EQ(ds.get_string(0x00291009u).value_or(""), std::string("20100114"));
    const Element* sq = ds.find(0x4105100Bu);
    REQUIRE(sq && sq->items.size() == 1);
    CHECK_EQ(sq->items[0].get_string(0x00080100u).value_or(""), std::string("ABC"));
    // Bytes are unchanged: an implicit save is byte-identical.
    CHECK(write_ts(ds, ts::ImplicitVRLittleEndian) == bytes);

    // Saved explicit, the dictionary VRs are not written: the elements go out as read (UN), the
    // private sequence as a defined-length UN with implicit VR items.
    const auto ex = write_ts(ds, ts::ExplicitVRLittleEndian);
    REQUIRE(!ex.empty());
    auto rx = read_ts(ex, ts::ExplicitVRLittleEndian);
    REQUIRE(rx.is_ok());
    for (Tag t : {0x00291008u, 0x00291009u, 0x00291010u, 0x00191003u, 0x4105100Bu, 0x41051010u}) {
        const Element* e = rx->find(t);
        REQUIRE(e);
        CHECK(e->vr == VR::UN);
        CHECK(e->read.private_vr == VR::None);  // explicit VR in the stream: no dictionary VR
    }
    CHECK(rx->find(0x00291009u)->value.as_chars() == std::string_view("20100114"));
    CHECK(rx->find(0x4105100Bu)->value.as_chars() == std::string_view(implicit_item()));

    // An element whose VR the caller changed is written with that VR.
    Dataset edited = ds;
    edited.find(0x00291009u)->vr = VR::SH;
    auto re = read_ts(write_ts(edited, ts::ExplicitVRLittleEndian), ts::ExplicitVRLittleEndian);
    REQUIRE(re.is_ok());
    CHECK(re->find(0x00291009u)->vr == VR::SH);

    // The option turns it off.
    ReadOptions off;
    off.private_dictionary = false;
    auto ro = read_dataset(Bytes::copy_of(bytes.data(), bytes.size()), ts::ImplicitVRLittleEndian, off);
    REQUIRE(ro.is_ok());
    CHECK(ro->find(0x00291008u)->vr == VR::UN);
    CHECK(ro->find(0x4105100Bu)->vr == VR::UN);
}

TEST_CASE("private dict: explicit VR file keeps its own VR") {
    Dataset src;
    src.put(raw(0x00290010u, VR::LO, "SIEMENS CSA HEADER"));
    src.put(raw(0x00291008u, VR::SH, "IMAGE NUM 4 "));  // dictionary says CS
    src.put(raw(0x00291009u, VR::UN, "20100114"));      // dictionary says LO
    src.put(raw(0x00190010u, VR::LO, "SIEMENS AX DYNACT "));
    src.put(raw(0x00191003u, VR::OW, le16(7)));         // dictionary says US
    const auto bytes = write_ts(src, ts::ExplicitVRLittleEndian);
    REQUIRE(!bytes.empty());
    auto r = read_ts(bytes, ts::ExplicitVRLittleEndian);
    REQUIRE(r.is_ok());
    CHECK(r->find(0x00291008u)->vr == VR::SH);
    CHECK(r->find(0x00291009u)->vr == VR::UN);
    CHECK(r->find(0x00191003u)->vr == VR::OW);
    CHECK(r->find(0x00291009u)->read.private_vr == VR::None);
    CHECK(write_ts(*r, ts::ExplicitVRLittleEndian) == bytes);
}

TEST_CASE("private dict: a dictionary SQ that is not a sequence stays UN bytes, also in strict mode") {
    Dataset src;
    src.put(raw(0x41050010u, VR::LO, "Applicare/RadWorks/Version 6.0"));
    src.put(raw(0x4105100Bu, VR::UN, "ABCDEFGHIJ"));  // not an item
    src.put(raw(0x41051011u, VR::UN, "XY"));
    const auto bytes = write_ts(src, ts::ImplicitVRLittleEndian);
    for (bool lenient : {true, false}) {
        auto r = read_ts(bytes, ts::ImplicitVRLittleEndian, lenient);
        REQUIRE(r.is_ok());
        const Element* e = r->find(0x4105100Bu);
        REQUIRE(e);
        CHECK(e->vr == VR::UN);
        CHECK(e->read.private_vr == VR::None);
        CHECK(e->value.as_chars() == std::string_view("ABCDEFGHIJ"));
        CHECK(r->contains(0x41051011u));
        CHECK(write_ts(*r, ts::ImplicitVRLittleEndian) == bytes);
    }
}

TEST_CASE("private dict: creators are scoped to their data set") {
    // An item without its own creator does not inherit the parent's (PS3.5 7.8.1).
    Dataset item;
    item.put(raw(0x00291008u, VR::UN, "IMAGE NUM 4 "));
    Dataset src;
    src.put(raw(0x00290010u, VR::LO, "SIEMENS CSA HEADER"));
    Element sq;
    sq.tag = 0x00081115u;  // Referenced Series Sequence (public SQ)
    sq.vr = VR::SQ;
    sq.items.push_back(item);
    src.put(sq);
    const auto bytes = write_ts(src, ts::ImplicitVRLittleEndian);
    auto r = read_ts(bytes, ts::ImplicitVRLittleEndian);
    REQUIRE(r.is_ok());
    const Element* s = r->find(0x00081115u);
    REQUIRE(s && s->items.size() == 1);
    CHECK(s->items[0].find(0x00291008u)->vr == VR::UN);
}

TEST_CASE("private dict: JSON and XML show private names") {
    const auto bytes = write_ts(implicit_source(), ts::ImplicitVRLittleEndian);
    auto r = read_ts(bytes, ts::ImplicitVRLittleEndian);
    REQUIRE(r.is_ok());
    std::string xml;
    REQUIRE(web::dataset_to_xml(*r, xml).is_ok());
    CHECK(xml.find("tag=\"00291010\" vr=\"OB\" keyword=\"CSAImageHeaderInfo\" privateCreator=\"SIEMENS CSA HEADER\"") !=
          std::string::npos);
    std::string json;
    REQUIRE(web::dataset_to_json(*r, json).is_ok());
    CHECK(json.find("CSAImageHeaderInfo") == std::string::npos);  // off by default (PS3.18 F.2)
    web::ModelWriteOptions o;
    o.json_private_names = true;
    json.clear();
    REQUIRE(web::dataset_to_json(*r, json, o).is_ok());
    CHECK(json.find("\"00291010\":{\"vr\":\"OB\",\"keyword\":\"CSAImageHeaderInfo\",\"name\":\"CSA Image Header Info\"") !=
          std::string::npos);
    // SIAARA's own reader ignores the extra members.
    auto back = web::dataset_from_json(json);
    REQUIRE(back.is_ok());
    CHECK(back->contains(0x00291010u));
}

TEST_CASE("private dict: C API") {
    siaara_private_dict_entry e{};
    e.struct_size = sizeof e;
    const char* cr = "SIEMENS CSA HEADER";
    REQUIRE(siaara_dict_lookup_private(cr, std::strlen(cr), 0x0029, 0x10, &e) == SIAARA_OK);
    CHECK(std::string(e.vr) == "OB");
    CHECK(std::string(e.keyword) == "CSAImageHeaderInfo");
    CHECK(std::string(e.creator) == cr);
    CHECK(e.match == SIAARA_PRIVATE_EXACT);
    CHECK(e.confidence == SIAARA_PRIVATE_STATED);
    CHECK(e.conflict == 0);
    CHECK(e.group == 0x0029 && e.element == 0x10);
    CHECK(e.source_count > 0 && e.source_page > 0 && e.source_document[0] != 0);
    const char* fz = "siemens csa header";
    REQUIRE(siaara_dict_lookup_private(fz, std::strlen(fz), 0x0029, 0x10, &e) == SIAARA_OK);
    CHECK(e.match == SIAARA_PRIVATE_FUZZY);
    const char* ap = "Applicare/RadWorks/Version 6.0";
    REQUIRE(siaara_dict_lookup_private(ap, std::strlen(ap), 0x4105, 0x10, &e) == SIAARA_OK);
    CHECK(e.conflict == 1);
    CHECK(e.vr[0] == 0);
    REQUIRE(e.candidate_count >= 3);
    siaara_private_dict_candidate c{};
    c.struct_size = sizeof c;
    for (uint32_t i = 0; i < e.candidate_count; ++i) {
        REQUIRE(siaara_dict_private_candidate(ap, std::strlen(ap), 0x4105, 0x10, i, &c) == SIAARA_OK);
        CHECK(c.vr[0] != 0 && c.document[0] != 0 && c.page > 0);
    }
    CHECK(siaara_dict_private_candidate(ap, std::strlen(ap), 0x4105, 0x10, e.candidate_count, &c) == SIAARA_ERR_NOT_FOUND);
    REQUIRE(siaara_dict_lookup_private("CANON MDW NON-IMAGE", 19, 0x7FE1, 0x10, &e) == SIAARA_OK);
    CHECK(e.confidence == SIAARA_PRIVATE_ASSUMED);
    CHECK(siaara_dict_lookup_private("", 0, 0x0019, 0x3A, &e) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_dict_lookup_private(nullptr, 0, 0x0019, 0x3A, &e) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_dict_lookup_private(nullptr, 3, 0x0019, 0x3A, &e) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_private_dict_entry small{};
    small.struct_size = 4;
    CHECK(siaara_dict_lookup_private(cr, std::strlen(cr), 0x0029, 0x10, &small) == SIAARA_ERR_INVALID_ARGUMENT);

    // Through a file read from memory: the element's VR and its dictionary entry.
    DicomFile f;
    f.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    f.dataset = implicit_source();
    f.dataset.put(raw(0x00080016u, VR::UI, "1.2.840.10008.5.1.4.1.1.7"));
    f.dataset.put(raw(0x00080018u, VR::UI, "1.2.3.4"));
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_open_memory(buf.data(), buf.size(), nullptr, &file) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(file, &ds) == SIAARA_OK);
    char vr[3] = {};
    REQUIRE(siaara_dataset_vr(ds, 0x00291009u, vr) == SIAARA_OK);
    CHECK(std::string(vr) == "LO");
    REQUIRE(siaara_dict_lookup_private_element(ds, 0x00291009u, &e) == SIAARA_OK);
    CHECK(std::string(e.keyword) == "CSAImageHeaderVersion");
    CHECK(siaara_dict_lookup_private_element(ds, 0x00331001u, &e) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_dict_lookup_private_element(ds, 0x00100010u, &e) == SIAARA_ERR_NOT_FOUND);
    std::vector<char> js(1 << 16);
    size_t need = 0;
    REQUIRE(siaara_dataset_to_json(ds, SIAARA_JSON_PRIVATE_NAMES, js.data(), js.size(), &need) == SIAARA_OK);
    CHECK(std::string(js.data()).find("\"keyword\":\"CSAImageHeaderVersion\"") != std::string::npos);
    siaara_file_free(file);
}
