// Regression tests for the 2026-11-09 security review, core parser findings (R-001, R-002).
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"
#include "test.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"

using namespace siaara;

namespace {

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, static_cast<std::uint16_t>(v & 0xFFFF));
    put16(b, static_cast<std::uint16_t>(v >> 16));
}
void tag(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e) {
    put16(b, g);
    put16(b, e);
}
// Explicit VR LE short-header element.
void short_el(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, const char* vr,
              const std::string& value) {
    tag(b, g, e);
    b.push_back(static_cast<std::uint8_t>(vr[0]));
    b.push_back(static_cast<std::uint8_t>(vr[1]));
    put16(b, static_cast<std::uint16_t>(value.size()));
    b.insert(b.end(), value.begin(), value.end());
}
void sq_header(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, std::uint32_t len) {
    tag(b, g, e);
    b.push_back('S');
    b.push_back('Q');
    put16(b, 0);
    put32(b, len);
}
void item_header(std::vector<std::uint8_t>& b, std::uint32_t len) {
    tag(b, 0xFFFE, 0xE000);
    put32(b, len);
}

Result<Dataset> parse(const std::vector<std::uint8_t>& b, bool lenient,
                      std::vector<std::string>* warnings = nullptr) {
    ReadOptions opt;
    opt.lenient = lenient;
    if (!warnings) return read_dataset(Bytes::copy_of(b.data(), b.size()), ts::ExplicitVRLittleEndian, opt);
    // read_part10 on a raw data set exposes the warnings.
    auto f = read_part10(Bytes::copy_of(b.data(), b.size()), opt);
    if (!f) return f.take_error();
    *warnings = f->warnings;
    return std::move(f->dataset);
}

// An item whose declared length (8) is shorter than its first element (8-byte header + 20-byte UI
// value). The UI value bytes are themselves a well-formed item carrying PatientID "EVIL": the old
// parser read the UI value past the item end and then moved pos_ backwards to the item end, so the
// same 20 bytes were parsed twice (once as a value, once as a smuggled second item).
std::vector<std::uint8_t> item_overrun_sample() {
    std::vector<std::uint8_t> inner;  // a complete item: header + (0010,0020) LO "EVIL"
    item_header(inner, 12);
    short_el(inner, 0x0010, 0x0020, "LO", "EVIL");
    std::vector<std::uint8_t> b;
    short_el(b, 0x0008, 0x0016, "UI", "1.2.3.4");
    sq_header(b, 0x0008, 0x1140, 8 + 8 + 20);  // consistent with the bytes that follow
    item_header(b, 8);                          // too short: covers only the UI element header
    short_el(b, 0x0008, 0x1155, "UI", std::string(inner.begin(), inner.end()));
    short_el(b, 0x0010, 0x0010, "PN", "AB");
    return b;
}

}  // namespace

TEST_CASE("R-001: element running past its item end is Malformed in strict mode") {
    auto r = parse(item_overrun_sample(), false);
    REQUIRE(!r);
    CHECK(r.error().code == Errc::Malformed);
}

TEST_CASE("R-001: lenient mode clamps a child to its parent end and never re-reads bytes") {
    std::vector<std::string> warnings;
    auto r = parse(item_overrun_sample(), true, &warnings);
    REQUIRE(r);
    const Element* seq = r->find(0x00081140);
    REQUIRE(seq != nullptr);
    REQUIRE(!seq->items.empty());
    const Element* uid = seq->items[0].find(0x00081155);
    REQUIRE(uid != nullptr);
    // Clamped at the item end: the value bytes belong to what follows, not to this element too.
    CHECK_EQ(uid->value.size(), 0u);
    // Every byte is parsed once: the element offsets inside the sequence strictly increase.
    std::uint64_t last = 0;
    for (const Dataset& it : seq->items)
        for (const Element& e : it.elements()) {
            CHECK(e.offset > last);
            last = e.offset;
        }
    CHECK_EQ(r->get_string(tags::PatientName).value_or(""), "AB");
    bool flagged = false;
    for (const auto& w : warnings)
        if (w.find("parent") != std::string::npos) flagged = true;
    CHECK(flagged);
}

TEST_CASE("R-001: item and nested sequence lengths may not exceed their parent") {
    // A defined-length sequence (16 bytes) holding an item that claims 100 bytes.
    std::vector<std::uint8_t> b;
    sq_header(b, 0x0008, 0x1140, 16);
    item_header(b, 100);
    short_el(b, 0x0008, 0x1155, "UI", "");      // 8 bytes: exactly fills the sequence
    short_el(b, 0x0010, 0x0010, "PN", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJ");
    auto strict = parse(b, false);
    REQUIRE(!strict);
    CHECK(strict.error().code == Errc::Malformed);
    std::vector<std::string> warnings;
    auto lenient = parse(b, true, &warnings);
    REQUIRE(lenient);
    // The item was clamped to the sequence end, so Patient Name after the sequence is still read
    // at top level instead of being swallowed into the item.
    CHECK_EQ(lenient->get_string(tags::PatientName).value_or("").size(), 82u);

    // Undefined-length item inside a defined-length sequence: the inner element overruns the
    // sequence end.
    std::vector<std::uint8_t> c;
    sq_header(c, 0x0008, 0x1140, 8 + 12);
    item_header(c, 0xFFFFFFFFu);
    short_el(c, 0x0008, 0x1155, "UI", "1.2.3.4.5.6.7.8.9.10");  // 28 bytes: past the SQ end
    tag(c, 0xFFFE, 0xE00D);
    put32(c, 0);
    short_el(c, 0x0010, 0x0010, "PN", "AB");
    auto strict2 = parse(c, false);
    REQUIRE(!strict2);
    CHECK(strict2.error().code == Errc::Malformed);
    CHECK(strict2.error().message.find("parent") != std::string::npos);
    CHECK(strict2.error().offset == 20u);  // the overrunning element, not bytes re-read after a rewind
    (void)parse(c, true);  // lenient: clamps; must not crash or re-read
}

TEST_CASE("R-001: valid nested defined-length data still parses exactly") {
    std::vector<std::uint8_t> b;
    sq_header(b, 0x0008, 0x1140, 8 + 12 + 8 + 12);
    item_header(b, 12);
    short_el(b, 0x0008, 0x1155, "UI", "1.2.");
    item_header(b, 12);
    short_el(b, 0x0008, 0x1155, "UI", "1.3.");
    short_el(b, 0x0010, 0x0010, "PN", "AB");
    for (bool lenient : {false, true}) {
        std::vector<std::string> warnings;
        auto r = parse(b, lenient, &warnings);
        REQUIRE(r);
        const Element* seq = r->find(0x00081140);
        REQUIRE(seq != nullptr);
        REQUIRE(seq->items.size() == 2u);
        CHECK_EQ(seq->items[1].get_string(0x00081155).value_or(""), "1.3.");
        CHECK_EQ(r->get_string(tags::PatientName).value_or(""), "AB");
        CHECK(warnings.empty());
    }
}

TEST_CASE("R-002: 1M elements in descending tag order parse in linear-ish time") {
    constexpr std::uint32_t kN = 1'000'000;
    std::vector<std::uint8_t> b;
    b.reserve(std::size_t{kN} * 8);
    for (std::uint32_t k = kN; k-- > 0;) {
        const std::uint32_t t = 0x00110000u + k;  // groups 0x0011..0x0020, all distinct
        tag(b, static_cast<std::uint16_t>(t >> 16), static_cast<std::uint16_t>(t & 0xFFFF));
        b.push_back('L');
        b.push_back('O');
        put16(b, 0);
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto r = parse(b, false);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r);
    CHECK_EQ(r->size(), std::size_t{kN});
    // Quadratic insertion takes hours here; sorting once takes well under a second (sanitizers: a few).
    CHECK(secs < 20.0);
    const auto& els = r->elements();
    bool ascending = true;
    for (std::size_t i = 1; i < els.size(); ++i)
        if (!(els[i - 1].tag < els[i].tag)) ascending = false;
    CHECK(ascending);
    CHECK(r->find(0x00110000u + 12345u) != nullptr);
}

TEST_CASE("R-002: duplicate tags are detected; the last occurrence wins as before") {
    std::vector<std::uint8_t> b;
    short_el(b, 0x0010, 0x0020, "LO", "ID01");
    short_el(b, 0x0010, 0x0010, "PN", "A^B");  // out of order
    short_el(b, 0x0010, 0x0020, "LO", "ID02");  // duplicate
    std::vector<std::string> warnings;
    auto r = parse(b, true, &warnings);
    REQUIRE(r);
    CHECK_EQ(r->size(), 2u);
    CHECK_EQ(r->get_string(tags::PatientID).value_or(""), "ID02");
    CHECK_EQ(r->get_string(tags::PatientName).value_or(""), "A^B");
    bool flagged = false;
    for (const auto& w : warnings)
        if (w.find("duplicate") != std::string::npos) flagged = true;
    CHECK(flagged);
}

TEST_CASE("R-002: DICOM JSON and XML with 100k descending tags decode in linear-ish time") {
    constexpr std::uint32_t kN = 100'000;
    auto hex8 = [](std::uint32_t t) {
        static const char* h = "0123456789ABCDEF";
        std::string s(8, '0');
        for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(i)] = h[(t >> (28 - 4 * i)) & 0xFu];
        return s;
    };
    std::string json = "{", xml = "<NativeDicomModel>";
    for (std::uint32_t k = kN; k-- > 0;) {
        const std::string t = hex8(0x00110000u + k);
        json += (k + 1 == kN ? "\"" : ",\"") + t + "\":{\"vr\":\"LO\"}";
        xml += "<DicomAttribute tag=\"" + t + "\" vr=\"LO\"/>";
    }
    json += "}";
    xml += "</NativeDicomModel>";
    const auto t0 = std::chrono::steady_clock::now();
    auto j = web::dataset_from_json(json);
    auto x = web::dataset_from_xml(xml);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(j);
    REQUIRE(x);
    CHECK_EQ(j->size(), std::size_t{kN});
    CHECK_EQ(x->size(), std::size_t{kN});
    CHECK(j->elements().front().tag == 0x00110000u);
    CHECK(x->find(0x00110000u + 777u) != nullptr);
    CHECK(secs < 30.0);
    // Repeated attributes are still refused, also when out of order.
    CHECK(!web::dataset_from_json("{\"00100020\":{\"vr\":\"LO\"},\"00100010\":{\"vr\":\"PN\"},\"00100020\":{\"vr\":\"LO\"}}"));
    CHECK(!web::dataset_from_xml("<NativeDicomModel><DicomAttribute tag=\"00100020\" vr=\"LO\"/><DicomAttribute tag=\"00100010\" vr=\"PN\"/><DicomAttribute tag=\"00100020\" vr=\"LO\"/></NativeDicomModel>"));
}
