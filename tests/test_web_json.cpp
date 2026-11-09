// DICOM JSON (PS3.18 Annex F) and Native DICOM Model XML (PS3.19 Annex A): JSON/XML parsers,
// base64, round trips for every VR and their edge cases, malformed input (every truncation and
// random corruption) and limits.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "web/base64.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"
#include "web/json.hpp"
#include "web/xml.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

Element make(Tag t, VR vr, const std::string& v) {
    Element e;
    e.tag = t;
    e.vr = vr;
    e.value = Bytes::from_string(v);
    return e;
}

Element bin(Tag t, VR vr, const std::vector<std::uint8_t>& v) {
    Element e;
    e.tag = t;
    e.vr = vr;
    e.value = Bytes::copy_of(v.data(), v.size());
    return e;
}

template <class T>
std::vector<std::uint8_t> le(std::initializer_list<T> vals) {
    std::vector<std::uint8_t> out;
    for (T v : vals) {
        std::uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));  // tests run on little-endian hosts
        out.insert(out.end(), b, b + sizeof(T));
    }
    return out;
}

bool same_dataset(const Dataset& a, const Dataset& b, std::string& why);

bool same_element(const Element& x, const Element& y, std::string& why) {
    if (x.tag != y.tag || x.vr != y.vr) {
        why = "tag/vr differ at " + std::to_string(x.tag);
        return false;
    }
    if (x.is_sequence()) {
        if (x.items.size() != y.items.size()) { why = "item count differs"; return false; }
        for (std::size_t i = 0; i < x.items.size(); ++i)
            if (!same_dataset(x.items[i], y.items[i], why)) return false;
        return true;
    }
    if (x.fragments.size() != y.fragments.size()) { why = "fragment count differs"; return false; }
    for (std::size_t i = 0; i < x.fragments.size(); ++i)
        if (x.fragments[i].as_chars() != y.fragments[i].as_chars()) { why = "fragment differs"; return false; }
    if (x.value.as_chars() != y.value.as_chars()) {
        why = "value differs at tag " + std::to_string(x.tag) + ": [" + std::string(x.value.as_chars()) + "] vs [" +
              std::string(y.value.as_chars()) + "]";
        return false;
    }
    return true;
}

bool same_dataset(const Dataset& a, const Dataset& b, std::string& why) {
    if (a.size() != b.size()) {
        why = "element count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_element(a.elements()[i], b.elements()[i], why)) return false;
    return true;
}

// A data set with every VR and the edge cases the encoders must keep.
Dataset every_vr() {
    Dataset ds;
    ds.put(make(0x00080005, VR::CS, "ISO_IR 192"));
    ds.put(make(0x00080054, VR::AE, "AE_ONE\\AE_TWO"));
    ds.put(make(0x00101010, VR::AS, "042Y"));
    ds.put(bin(0x00209165, VR::AT, le<std::uint16_t>({0x0010, 0x0010, 0x7FE0, 0x0010})));
    ds.put(make(0x00080060, VR::CS, "CT"));
    ds.put(make(0x00080020, VR::DA, "20240131"));
    ds.put(make(0x00280030, VR::DS, "0.5\\-1.25e-3"));
    ds.put(make(0x00181050, VR::DS, "12345678.1234567"));        // 15 significant digits: number
    ds.put(make(0x00181060, VR::DS, "1.2345678901234567"));       // 17 digits: kept as string
    ds.put(make(0x00281052, VR::DS, "+1.5\\.5\\\\7"));            // non-JSON forms + empty value
    ds.put(make(0x0008002A, VR::DT, "20240131120000.000000+0100"));
    ds.put(bin(0x00189327, VR::FL, le<float>({1.5f, -0.1f, 3.4028235e38f, 1e-45f})));
    ds.put(bin(0x00189328, VR::FD, le<double>({0.1, -2.5e-300, 4.9e-324, 1.7976931348623157e308, -0.0})));
    ds.put(make(0x00200013, VR::IS, "42\\-7"));
    ds.put(make(0x00200012, VR::IS, "123456789012345678"));      // beyond 2^53: kept as string
    ds.put(make(0x00100020, VR::LO, "ID-\xC3\xA9\xE6\xBC\xA2"));  // UTF-8 (é, 漢)
    ds.put(make(0x00104000, VR::LT, "line one\r\nline two \\ with backslash"));
    ds.put(bin(0x00420011, VR::OB, {0x00, 0xFF, 0x10, 0x80, 0x7F, 0x01}));
    ds.put(bin(0x00720067, VR::OD, le<double>({1.0, -2.0})));
    ds.put(bin(0x00720069, VR::OF, le<float>({1.0f, 2.0f})));
    ds.put(bin(0x00720075, VR::OL, le<std::uint32_t>({1, 0xFFFFFFFFu})));
    ds.put(bin(0x00720081, VR::OV, le<std::uint64_t>({1, 0xFFFFFFFFFFFFFFFFull})));
    ds.put(bin(0x00281201, VR::OW, le<std::uint16_t>({0, 65535, 1234})));
    ds.put(make(0x00100010, VR::PN, "Doe^John^^Dr.^Jr.=\xE5\xB1\xB1\xE7\x94\xB0^\xE5\xA4\xAA\xE9\x83\x8E=\xE3\x82\x84\xE3\x81\xBE\xE3\x81\xA0^\xE3\x81\x9F\xE3\x82\x8D\xE3\x81\x86\\\\==Phonetic^Only"));
    ds.put(make(0x00081030, VR::LO, ""));                           // empty value
    ds.put(make(0x00080070, VR::LO, "\\"));                         // two empty values
    ds.put(make(0x00200011, VR::IS, ""));
    ds.put(make(0x00080021, VR::DA, ""));
    ds.put(make(0x00080080, VR::LO, "A \"quoted\" name </tag> & more"));
    ds.put(make(0x00080050, VR::SH, "ACC123"));
    ds.put(bin(0x00186020, VR::SL, le<std::int32_t>({-2147483647 - 1, 2147483647})));
    ds.put(bin(0x00280106, VR::SS, le<std::int16_t>({-32768, 32767})));
    ds.put(make(0x00321060, VR::LO, "tab\there"));
    ds.put(make(0x00400280, VR::ST, "short text"));
    ds.put(bin(0x00720082, VR::SV, le<std::int64_t>({-9007199254740991LL, std::numeric_limits<std::int64_t>::min(), 5})));
    ds.put(make(0x00080030, VR::TM, "235959.999999"));
    ds.put(make(0x00080119, VR::UC, "unlimited characters\\second"));
    ds.put(make(0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.2"));
    ds.put(make(0x0020000D, VR::UI, "2.25.1234"));
    ds.put(bin(0x00280008 + 0x100000, VR::UL, le<std::uint32_t>({0, 4294967295u})));  // (0038,0008) UL
    ds.put(bin(0x00191001, VR::UN, {1, 2, 3}));
    ds.put(make(0x00081190, VR::UR, "http://example.com/a?b=c&d=e"));
    ds.put(bin(0x00280010, VR::US, le<std::uint16_t>({512})));
    ds.put(make(0x00400275 + 0x10000000, VR::UT, "unlimited text with \\ backslash"));
    ds.put(bin(0x00720083, VR::UV, le<std::uint64_t>({9007199254740991ull, 18446744073709551615ull})));
    // Private block with its creator.
    ds.put(make(0x00090010, VR::LO, "SIAARA TEST"));
    ds.put(make(0x00091001, VR::LO, "private value"));
    ds.put(bin(0x00091002, VR::US, le<std::uint16_t>({7, 8})));
    // Sequences: empty, empty item, nested three levels.
    Element empty_sq;
    empty_sq.tag = 0x00081115;
    empty_sq.vr = VR::SQ;
    ds.put(empty_sq);
    Element sq;
    sq.tag = 0x00081140;
    sq.vr = VR::SQ;
    sq.items.emplace_back();  // empty item
    Dataset item;
    item.put(make(0x00081150, VR::UI, "1.2.3"));
    Element inner;
    inner.tag = 0x00081199;
    inner.vr = VR::SQ;
    Dataset deep;
    deep.put(make(0x00081155, VR::UI, "1.2.3.4"));
    Element deeper;
    deeper.tag = 0x0040A730;
    deeper.vr = VR::SQ;
    Dataset leaf;
    leaf.put(make(0x0040A160, VR::UT, "leaf"));
    deeper.items.push_back(leaf);
    deep.put(deeper);
    inner.items.push_back(deep);
    inner.items.push_back(deep);
    item.put(inner);
    sq.items.push_back(item);
    ds.put(sq);
    return ds;
}

}  // namespace

// ---------------------------------------------------------------- base64
TEST_CASE("web/base64 round trip and invalid input") {
    std::mt19937 rng(7);
    for (std::size_t n = 0; n < 100; ++n) {
        std::vector<std::uint8_t> d(n);
        for (auto& b : d) b = static_cast<std::uint8_t>(rng());
        std::string e;
        CHECK(base64_encode(d, e).is_ok());
        CHECK_EQ(e.size(), (n + 2) / 3 * 4);
        std::vector<std::uint8_t> back;
        CHECK(base64_decode(e, back).is_ok());
        CHECK(back == d);
        // Unpadded and line-wrapped forms decode too.
        std::string unpadded = e;
        while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
        CHECK(base64_decode(unpadded, back).is_ok() && back == d);
        std::string wrapped;
        for (std::size_t i = 0; i < e.size(); ++i) {
            if (i && i % 7 == 0) wrapped += "\r\n";
            wrapped.push_back(e[i]);
        }
        CHECK(base64_decode(wrapped, back).is_ok() && back == d);
    }
    CHECK_EQ(base64_encode("foobar"), std::string("Zm9vYmFy"));
    CHECK_EQ(base64_encode("fo"), std::string("Zm8="));
    std::vector<std::uint8_t> out;
    CHECK(!base64_decode("Zm9v!", out).is_ok());
    CHECK(!base64_decode("Z", out).is_ok());
    CHECK(!base64_decode("Zm=v", out).is_ok());
    CHECK(!base64_decode("Zm8=Zm8=", out).is_ok());
    CHECK(!base64_decode("Zm9=", out).is_ok());  // non-zero padding bits
    CHECK(!base64_decode("=", out).is_ok());
    CHECK(base64_decode("Zm9vYmFy", out, 5).code == Errc::LimitExceeded);
}

// ---------------------------------------------------------------- JSON
TEST_CASE("web/json parser accepts RFC 8259 and keeps number text") {
    auto v = json::parse(R"( {"a":[1,-0,2.50,1e400,-1.5E-7,123456789012345678901234567890],"b":"x\u00e9\ud83d\ude00\n","c":true,"d":null,"e":{}} )");
    REQUIRE(v.is_ok());
    const auto* a = v->get("a");
    REQUIRE(a && a->is_array() && a->array.size() == 6);
    CHECK_EQ(a->array[2].text, std::string("2.50"));
    CHECK_EQ(a->array[3].text, std::string("1e400"));
    CHECK_EQ(a->array[5].text, std::string("123456789012345678901234567890"));
    CHECK_EQ(v->get("b")->text, std::string("x\xC3\xA9\xF0\x9F\x98\x80\n"));
    CHECK(v->get("c")->boolean);
    CHECK(v->get("d")->is_null());
    std::string out;
    json::write(out, *v);
    auto again = json::parse(out);
    REQUIRE(again.is_ok());
    std::string out2;
    json::write(out2, *again);
    CHECK_EQ(out, out2);
    CHECK(json::parse("0").is_ok());
    CHECK(json::parse("\"\"").is_ok());
    CHECK(json::parse("[]").is_ok());
}

TEST_CASE("web/json parser rejects non-JSON") {
    const char* bad[] = {
        "NaN", "Infinity", "-Infinity", "[NaN]", "[Infinity]", "{\"a\":-Infinity}", "[1,]", "{\"a\":1,}", "01", "-01",
        "1.", ".5", "+1", "1e", "1e+", "0x10", "[1 2]", "{\"a\" 1}", "{a:1}", "'x'", "\"\\x\"", "\"\\u12\"",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"a\x01\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"",
        "\"\xF5\x80\x80\x80\"", "\xEF\xBB\xBF{}", "[", "{", "\"abc", "tru", "nul", "[1]]", "{} {}", "/*c*/1", "",
        "   ", "[\"a\"\"b\"]", "1 2", "--1", "[-]",
    };
    for (const char* s : bad) {
        auto v = json::parse(s);
        if (v.is_ok()) std::fprintf(stderr, "  accepted: %s\n", s);
        CHECK(!v.is_ok());
    }
}

TEST_CASE("web/json limits") {
    std::string deep(600, '[');
    deep += std::string(600, ']');
    CHECK(json::parse(deep).error().code == Errc::LimitExceeded);
    json::ParseLimits lim;
    lim.max_depth = 700;
    CHECK(json::parse(deep, lim).is_ok());
    lim = {};
    lim.max_input = 10;
    CHECK(json::parse("[1,2,3,4,5,6]", lim).error().code == Errc::LimitExceeded);
    lim = {};
    lim.max_string = 3;
    CHECK(json::parse("\"abcd\"", lim).error().code == Errc::LimitExceeded);
    lim = {};
    lim.max_values = 3;
    CHECK(json::parse("[1,2,3]", lim).error().code == Errc::LimitExceeded);
    lim = {};
    lim.max_number_length = 4;
    CHECK(json::parse("123456", lim).error().code == Errc::LimitExceeded);
}

TEST_CASE("web/json string escaping and UTF-8 sanitising") {
    std::string out;
    json::write_string(out, std::string("a\"b\\c\x01\x7F\xFF\xC3\xA9", 10));
    CHECK_EQ(out, std::string("\"a\\\"b\\\\c\\u0001\\u007f\xEF\xBF\xBD\xC3\xA9\""));
    auto v = json::parse(out);
    REQUIRE(v.is_ok());
    CHECK(json::is_valid_utf8(v->text));
}

// ---------------------------------------------------------------- DICOM JSON
TEST_CASE("web/dicom json round trip of every VR") {
    const Dataset ds = every_vr();
    std::string js;
    REQUIRE(dataset_to_json(ds, js).is_ok());
    REQUIRE(json::parse(js).is_ok());
    auto back = dataset_from_json(js);
    if (!back) std::fprintf(stderr, "  %s\n", back.error().message.c_str());
    REQUIRE(back.is_ok());
    std::string why;
    const bool same = same_dataset(ds, *back, why);
    if (!same) std::fprintf(stderr, "  %s\n", why.c_str());
    CHECK(same);
    // Second generation output is identical.
    std::string js2;
    REQUIRE(dataset_to_json(*back, js2).is_ok());
    CHECK_EQ(js, js2);
    // Spot checks of the encoding rules.
    auto v = json::parse(js);
    REQUIRE(v.is_ok());
    CHECK(v->get("00280030")->get("Value")->array[0].is_number());
    CHECK(v->get("00181050")->get("Value")->array[0].is_number());
    CHECK(v->get("00181060")->get("Value")->array[0].is_string());   // too many digits for a double
    CHECK(v->get("00200012")->get("Value")->array[0].is_string());   // IS beyond 2^53
    CHECK(v->get("00281052")->get("Value")->array[0].is_string());   // "+1.5"
    CHECK(v->get("00281052")->get("Value")->array[2].is_null());     // empty value in the middle
    CHECK(v->get("00081030")->get("Value") == nullptr);              // empty value: no Value
    CHECK(v->get("00080070")->get("Value")->array.size() == 2);
    CHECK(v->get("00209165")->get("Value")->array[1].text == "7FE00010");
    CHECK(v->get("00100010")->get("Value")->array[0].get("Ideographic") != nullptr);
    CHECK(v->get("00100010")->get("Value")->array[1].is_null());
    CHECK(v->get("00100010")->get("Value")->array[2].get("Alphabetic") == nullptr);
    CHECK(v->get("00100010")->get("Value")->array[2].get("Phonetic")->text == "Phonetic^Only");
    CHECK(v->get("00420011")->get("InlineBinary")->text == "AP8QgH8B");
    CHECK(v->get("00720083")->get("Value")->array[0].is_number());
    CHECK(v->get("00720083")->get("Value")->array[1].is_string());
    CHECK(v->get("00720082")->get("Value")->array[1].text == "-9223372036854775808");
    CHECK(v->get("00189328")->get("Value")->array[0].text == "0.1");
    CHECK(v->get("00189328")->get("Value")->array[4].text == "-0");
    CHECK(v->get("00081115")->get("Value") == nullptr);
    CHECK(v->get("00081140")->get("Value")->array[0].object.empty());
}

TEST_CASE("web/dicom json numbers_as_strings and IS/DS trimming") {
    Dataset ds;
    ds.put(make(0x00200013, VR::IS, " 42 "));
    ds.put(make(0x00281053, VR::DS, "1.0 "));
    ModelWriteOptions o;
    o.numbers_as_strings = true;
    std::string js;
    REQUIRE(dataset_to_json(ds, js, o).is_ok());
    CHECK_EQ(js, std::string(R"({"00200013":{"vr":"IS","Value":["42"]},"00281053":{"vr":"DS","Value":["1.0"]}})"));
    js.clear();
    REQUIRE(dataset_to_json(ds, js).is_ok());
    CHECK_EQ(js, std::string(R"({"00200013":{"vr":"IS","Value":[42]},"00281053":{"vr":"DS","Value":[1.0]}})"));
    // Huge numbers written by others are kept exactly as text.
    auto d = dataset_from_json(R"({"00200013":{"vr":"IS","Value":[123456789012345678901234567890]},"00281053":{"vr":"DS","Value":[1.00000000000000000000001e-400]}})");
    REQUIRE(d.is_ok());
    CHECK_EQ(std::string(d->find(0x00200013)->value.as_chars()), std::string("123456789012345678901234567890"));
    CHECK_EQ(std::string(d->find(0x00281053)->value.as_chars()), std::string("1.00000000000000000000001e-400"));
}

TEST_CASE("web/dicom json non-finite floats and integer edge cases") {
    Dataset ds;
    ds.put(bin(0x00189328, VR::FD, le<double>({std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                                                std::numeric_limits<double>::quiet_NaN()})));
    ds.put(bin(0x00189327, VR::FL, le<float>({std::numeric_limits<float>::infinity()})));
    std::string js;
    REQUIRE(dataset_to_json(ds, js).is_ok());
    CHECK(js.find("\"Infinity\"") != std::string::npos && js.find("\"-Infinity\"") != std::string::npos &&
          js.find("\"NaN\"") != std::string::npos);
    auto back = dataset_from_json(js);
    REQUIRE(back.is_ok());
    const auto* fd = back->find(0x00189328);
    REQUIRE(fd && fd->value.size() == 24);
    double d[3];
    std::memcpy(d, fd->value.data(), 24);
    CHECK(std::isinf(d[0]) && d[0] > 0);
    CHECK(std::isinf(d[1]) && d[1] < 0);
    CHECK(std::isnan(d[2]));
    // Bare NaN / Infinity are not JSON.
    CHECK(!dataset_from_json(R"({"00189328":{"vr":"FD","Value":[NaN]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00189328":{"vr":"FD","Value":[Infinity]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00189328":{"vr":"FD","Value":[-Infinity]}})").is_ok());
    // Overflowing doubles are rejected rather than turned into infinity.
    CHECK(!dataset_from_json(R"({"00189328":{"vr":"FD","Value":[1e400]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00189327":{"vr":"FL","Value":[1e39]}})").is_ok());
    // Integers: exponent and fraction forms that are integral are fine, others are not.
    auto i = dataset_from_json(R"({"00280010":{"vr":"US","Value":[5.0, 1e2, 0, 65535]}})");
    REQUIRE(i.is_ok());
    CHECK_EQ(i->value_multiplicity(0x00280010), 4u);
    CHECK_EQ(i->get_int(0x00280010, 1).value_or(0), 100);
    CHECK(!dataset_from_json(R"({"00280010":{"vr":"US","Value":[65536]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00280010":{"vr":"US","Value":[-1]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00280010":{"vr":"US","Value":[1.5]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00280106":{"vr":"SS","Value":[32768]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00720083":{"vr":"UV","Value":["18446744073709551616"]}})").is_ok());
    CHECK(!dataset_from_json(R"({"00720082":{"vr":"SV","Value":[-9223372036854775809]}})").is_ok());
    CHECK(dataset_from_json(R"({"00720082":{"vr":"SV","Value":["-9223372036854775808"]}})").is_ok());
    // Subnormal doubles survive.
    auto sub = dataset_from_json(R"({"00189328":{"vr":"FD","Value":[5e-324, 2.2250738585072014e-308]}})");
    REQUIRE(sub.is_ok());
    // Compare bit patterns: on x87 targets a double literal may be evaluated in excess precision.
    std::uint64_t bits[2];
    std::memcpy(bits, sub->find(0x00189328)->value.data(), 16);
    CHECK_EQ(bits[0], 1u);                        // smallest subnormal
    CHECK_EQ(bits[1], 0x0010000000000000ull);     // DBL_MIN
}

TEST_CASE("web/dicom json character sets") {
    Dataset ds;
    ds.put(make(0x00080005, VR::CS, "ISO_IR 100"));
    ds.put(make(0x00100010, VR::PN, "M\xFCller^J\xF6rg"));  // Latin-1
    std::string js;
    REQUIRE(dataset_to_json(ds, js).is_ok());
    CHECK(js.find("M\xC3\xBCller^J\xC3\xB6rg") != std::string::npos);
    auto back = dataset_from_json(js);
    REQUIRE(back.is_ok());
    CHECK_EQ(back->get_string(0x00080005).value_or(""), std::string("ISO_IR 192"));  // now UTF-8
    CHECK_EQ(back->get_string(0x00100010).value_or(""), std::string("M\xC3\xBCller^J\xC3\xB6rg"));
    // An unsupported character set: strict mode refuses, default replaces.
    Dataset other;
    other.put(make(0x00080005, VR::CS, "ISO_IR 144"));
    other.put(make(0x00100010, VR::PN, "\xC8\xD2"));
    ModelWriteOptions strict;
    strict.strict_charset = true;
    js.clear();
    CHECK(dataset_to_json(other, js, strict).code == Errc::Unsupported);
    js.clear();
    CHECK(dataset_to_json(other, js).is_ok());
    CHECK(json::parse(js).is_ok());
    // Invalid UTF-8 never produces invalid JSON.
    Dataset broken;
    broken.put(make(0x00100010, VR::PN, "\xFF\xFE"));
    js.clear();
    CHECK(dataset_to_json(broken, js).is_ok());
    CHECK(json::parse(js).is_ok());
}

TEST_CASE("web/dicom json encapsulated pixel data and bulk data URIs") {
    Dataset ds;
    Element px;
    px.tag = tags::PixelData;
    px.vr = VR::OB;
    px.undefined_length = true;
    px.fragments.push_back(Bytes());                      // empty Basic Offset Table
    px.fragments.push_back(Bytes::from_string("frame1"));
    px.fragments.push_back(Bytes::from_string("fr2"));    // odd: padded
    ds.put(px);
    std::string js;
    REQUIRE(dataset_to_json(ds, js).is_ok());
    auto back = dataset_from_json(js);
    REQUIRE(back.is_ok());
    const Element* e = back->find(tags::PixelData);
    REQUIRE(e && e->fragments.size() == 3);
    CHECK_EQ(std::string(e->fragments[1].as_chars()), std::string("frame1"));
    CHECK_EQ(std::string(e->fragments[2].as_chars()), std::string("fr2\0", 4));
    ModelWriteOptions no_inline;
    no_inline.inline_encapsulated = false;
    js.clear();
    CHECK(dataset_to_json(ds, js, no_inline).code == Errc::Unsupported);
    // BulkDataURI out and in.
    ModelWriteOptions o;
    o.bulk_data_uri = [](const Element& el, const ElementPath& path) -> std::optional<std::string> {
        if (el.tag != tags::PixelData) return std::nullopt;
        return "http://h/bulk/" + std::to_string(path.size());
    };
    js.clear();
    REQUIRE(dataset_to_json(ds, js, o).is_ok());
    CHECK(js.find("\"BulkDataURI\":\"http://h/bulk/1\"") != std::string::npos);
    std::vector<BulkReference> refs;
    ModelReadOptions ro;
    ro.unresolved = &refs;
    auto b2 = dataset_from_json(js, ro);
    REQUIRE(b2.is_ok());
    REQUIRE(refs.size() == 1);
    CHECK_EQ(refs[0].uri, std::string("http://h/bulk/1"));
    CHECK(refs[0].path == ElementPath{tags::PixelData});
    ro.resolve_bulk_data = [](std::string_view, Tag, VR) -> Result<Bytes> { return Bytes::from_string("XY"); };
    auto b3 = dataset_from_json(js, ro);
    REQUIRE(b3.is_ok());
    CHECK_EQ(std::string(b3->find(tags::PixelData)->value.as_chars()), std::string("XY"));
}

TEST_CASE("web/dicom json malformed documents") {
    const char* bad[] = {
        "[]", "1", R"({"0010001":{"vr":"PN"}})", R"({"0010001G":{"vr":"PN"}})", R"({"00100010":1})",
        R"({"00100010":{"vr":"XX"}})", R"({"00100010":{"vr":"P"}})", R"({"00100010":{"vr":5}})",
        R"({"00100010":{"vr":"PN","Value":"Doe"}})", R"({"00100010":{"vr":"PN","Value":["Doe"]}})",
        R"({"00100010":{"vr":"PN","Value":[{"Alphabetic":1}]}})", R"({"00100010":{"vr":"PN","Value":[{"Alphabetic":"a=b"}]}})",
        R"({"00100020":{"vr":"LO","Value":[1]}})", R"({"00100020":{"vr":"LO","Value":["a\\b"]}})",
        R"({"00100020":{"vr":"LO","Value":["a"],"InlineBinary":"AA=="}})", R"({"00420011":{"vr":"OB","InlineBinary":"A"}})",
        R"({"00420011":{"vr":"OB","InlineBinary":1}})", R"({"00420011":{"vr":"OB","Value":[1]}})",
        R"({"00420011":{"vr":"OB","BulkDataURI":5}})", R"({"00081140":{"vr":"SQ","Value":[1]}})",
        R"({"00081140":{"vr":"SQ","Value":[null]}})", R"({"00081140":{"vr":"SQ","InlineBinary":"AA=="}})",
        R"({"00209165":{"vr":"AT","Value":["0010"]}})", R"({"00209165":{"vr":"AT","Value":[1048592]}})",
        R"({"00280010":{"vr":"US","Value":[null]}})", R"({"00280010":{"vr":"US","Value":[true]}})",
        R"({"00280010":{"vr":"US","Value":["x"]}})", R"({"00189328":{"vr":"FD","Value":["nan"]}})",
        R"({"00100010":{"vr":"PN"},"00100010":{"vr":"PN"}})", R"({"FFFEE000":{"vr":"UN"}})",
        R"({"00104000":{"vr":"LT","Value":["a","b"]}})",
    };
    for (const char* s : bad) {
        auto d = dataset_from_json(s);
        if (d.is_ok()) std::fprintf(stderr, "  accepted: %s\n", s);
        CHECK(!d.is_ok());
    }
    // Missing vr: taken from the dictionary.
    auto d = dataset_from_json(R"({"00100010":{"Value":[{"Alphabetic":"A^B"}]}})");
    REQUIRE(d.is_ok());
    CHECK(d->find(0x00100010)->vr == VR::PN);
}

TEST_CASE("web/dicom json and xml limits") {
    // Nesting deeper than max_depth.
    std::string js;
    for (int i = 0; i < 70; ++i) js += R"({"00081140":{"vr":"SQ","Value":[)";
    js += "{}";
    for (int i = 0; i < 70; ++i) js += "]}}";
    CHECK(dataset_from_json(js).error().code == Errc::LimitExceeded);
    ModelReadOptions ro;
    ro.max_depth = 100;
    CHECK(dataset_from_json(js, ro).is_ok());
    ro = {};
    ro.max_value_bytes = 4;
    CHECK(dataset_from_json(R"({"00420011":{"vr":"OB","InlineBinary":"AAAAAAAA"}})", ro).error().code == Errc::LimitExceeded);
    CHECK(dataset_from_json(R"({"00100020":{"vr":"LO","Value":["abcdef"]}})", ro).error().code == Errc::LimitExceeded);
    ro = {};
    ro.max_elements = 2;
    CHECK(dataset_from_json(R"({"00100010":{"vr":"PN"},"00100020":{"vr":"LO"},"00100030":{"vr":"DA"}})", ro).error().code ==
          Errc::LimitExceeded);
    // Writer refuses data sets nested beyond its limit.
    Dataset ds;
    Dataset* cur = &ds;
    for (int i = 0; i < 70; ++i) {
        Element sq;
        sq.tag = 0x00081140;
        sq.vr = VR::SQ;
        sq.items.emplace_back();
        Element& put = cur->put(std::move(sq));
        cur = &put.items[0];
    }
    std::string out;
    CHECK(dataset_to_json(ds, out).code == Errc::LimitExceeded);
    out.clear();
    CHECK(dataset_to_xml(ds, out).code == Errc::LimitExceeded);
}

TEST_CASE("web/dicom json every truncation and random corruption never crash") {
    const Dataset ds = every_vr();
    std::string js;
    REQUIRE(dataset_to_json(ds, js).is_ok());
    std::size_t accepted = 0;
    for (std::size_t n = 0; n < js.size(); ++n) {
        auto d = dataset_from_json(std::string_view(js).substr(0, n));
        if (d) ++accepted;
    }
    CHECK_EQ(accepted, 0u);  // no proper prefix of an object is a complete document
    std::mt19937 rng(12345);
    const char noise[] = "{}[]\",:\\0123456789eE+-.nultrfasNIx \x01\xC3\xFF";
    int ok = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        std::string m = js;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < edits; ++k) {
            const std::size_t pos = rng() % m.size();
            switch (rng() % 3) {
                case 0: m[pos] = noise[rng() % (sizeof noise - 1)]; break;
                case 1: m.erase(pos, 1 + rng() % 8); break;
                default: m.insert(pos, 1, noise[rng() % (sizeof noise - 1)]); break;
            }
            if (m.empty()) m = "{";
        }
        auto d = dataset_from_json(m);
        if (d) {
            ++ok;
            std::string again;
            CHECK(dataset_to_json(*d, again).is_ok());
        }
    }
    CHECK(ok >= 0);
}

// ---------------------------------------------------------------- XML
TEST_CASE("web/xml parser") {
    auto n = xml::parse("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- c --><a x='1' y=\"&lt;&amp;&#65;&#x42;\">t<b/>&gt;<![CDATA[<raw>]]>\r\n<?pi x?></a>  ");
    REQUIRE(n.is_ok());
    CHECK_EQ(n->name, std::string("a"));
    CHECK_EQ(*n->attr("y"), std::string("<&AB"));
    CHECK_EQ(n->text, std::string("t><raw>\n"));
    CHECK(n->child("b") != nullptr);
    const char* bad[] = {
        "", "<a>", "<a></b>", "<a x=1/>", "<a x='1' x='2'/>", "<!DOCTYPE a><a/>",
        "<!DOCTYPE a [<!ENTITY e \"x\">]><a>&e;</a>", "<a>&e;</a>", "<a>&#0;</a>", "<a>&#xD800;</a>", "<a>\x01</a>",
        "<a/><b/>", "text<a/>", "<a><!-- -- --></a>", "<a>]]></a>", "<a x='<'/>", "<a\xFF/>", "<a>\xC0\xAF</a>",
        "<a><![CDATA[x</a>", "<a/><?xml version='1.0'?>", "<?xml version='1.0' encoding='ISO-8859-1'?><a/>",
        "<a b='1'c='2'/>", "<1a/>",
    };
    for (const char* s : bad) {
        auto r = xml::parse(s);
        if (r.is_ok()) std::fprintf(stderr, "  accepted: %s\n", s);
        CHECK(!r.is_ok());
    }
    std::string deep;
    for (int i = 0; i < 600; ++i) deep += "<a>";
    for (int i = 0; i < 600; ++i) deep += "</a>";
    CHECK(xml::parse(deep).error().code == Errc::LimitExceeded);
}

TEST_CASE("web/dicom xml round trip of every VR") {
    const Dataset ds = every_vr();
    std::string x;
    REQUIRE(dataset_to_xml(ds, x).is_ok());
    REQUIRE(xml::parse(x).is_ok());
    auto back = dataset_from_xml(x);
    if (!back) std::fprintf(stderr, "  %s\n", back.error().message.c_str());
    REQUIRE(back.is_ok());
    std::string why;
    const bool same = same_dataset(ds, *back, why);
    if (!same) std::fprintf(stderr, "  %s\n", why.c_str());
    CHECK(same);
    std::string x2;
    REQUIRE(dataset_to_xml(*back, x2).is_ok());
    CHECK_EQ(x, x2);
    CHECK(x.find("<DicomAttribute tag=\"00091001\" vr=\"LO\" privateCreator=\"SIAARA TEST\">") != std::string::npos);
    CHECK(x.find("keyword=\"PatientName\"") != std::string::npos);
    CHECK(x.find("<FamilyName>Doe</FamilyName><GivenName>John</GivenName><NamePrefix>Dr.</NamePrefix>") != std::string::npos);
    CHECK(x.find("&lt;/tag&gt; &amp; more") != std::string::npos);
    // XML and JSON agree.
    std::string j1, j2;
    REQUIRE(dataset_to_json(ds, j1).is_ok());
    REQUIRE(dataset_to_json(*back, j2).is_ok());
    CHECK_EQ(j1, j2);
}

TEST_CASE("web/dicom xml details") {
    // Value numbering with gaps, privateCreator synthesis, BulkData uuid.
    const char* doc =
        "<NativeDicomModel xmlns=\"http://dicom.nema.org/PS3.19/models/NativeDICOM\">"
        "<DicomAttribute tag=\"00080070\" vr=\"LO\"><Value number=\"3\">c</Value><Value number=\"1\">a</Value></DicomAttribute>"
        "<DicomAttribute tag=\"00291010\" vr=\"LO\" privateCreator=\"ACME 1.0\"><Value number=\"1\">p</Value></DicomAttribute>"
        "<DicomAttribute tag=\"7FE00010\" vr=\"OB\"><BulkData uuid=\"abc\"/></DicomAttribute>"
        "</NativeDicomModel>";
    std::vector<BulkReference> refs;
    ModelReadOptions ro;
    ro.unresolved = &refs;
    auto d = dataset_from_xml(doc, ro);
    REQUIRE(d.is_ok());
    CHECK_EQ(std::string(d->find(0x00080070)->value.as_chars()), std::string("a\\\\c"));
    CHECK_EQ(d->get_string(0x00290010).value_or(""), std::string("ACME 1.0"));
    REQUIRE(refs.size() == 1);
    CHECK_EQ(refs[0].uri, std::string("uuid:abc"));
    const char* bad[] = {
        "<Other/>",
        "<NativeDicomModel xmlns=\"urn:other\"/>",
        "<NativeDicomModel><Foo/></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute vr=\"LO\"/></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"0008007\" vr=\"LO\"/></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00080070\" vr=\"LO\"><Value>a</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00080070\" vr=\"LO\"><Value number=\"0\">a</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00080070\" vr=\"LO\"><Value number=\"1\">a</Value><Value number=\"1\">b</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00080070\" vr=\"LO\"><Value number=\"99999\">a</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00080070\" vr=\"LO\"><Value number=\"1\">a</Value><InlineBinary>AA==</InlineBinary></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00100010\" vr=\"PN\"><Value number=\"1\">a</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00100010\" vr=\"PN\"><PersonName number=\"1\"><Alphabetic><Foo/></Alphabetic></PersonName></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00100010\" vr=\"PN\"><PersonName number=\"1\"><Alphabetic><FamilyName>a^b</FamilyName></Alphabetic></PersonName></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00280010\" vr=\"US\"><Value number=\"1\">70000</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00280010\" vr=\"US\"><Value number=\"2\">7</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00081140\" vr=\"SQ\"><Item number=\"2\"/></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00081140\" vr=\"SQ\"><Value number=\"1\">x</Value></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00420011\" vr=\"OB\"><InlineBinary>!!</InlineBinary></DicomAttribute></NativeDicomModel>",
        "<NativeDicomModel><DicomAttribute tag=\"00100010\" vr=\"PN\"/><DicomAttribute tag=\"00100010\" vr=\"PN\"/></NativeDicomModel>",
    };
    for (const char* s : bad) {
        auto r = dataset_from_xml(s);
        if (r.is_ok()) std::fprintf(stderr, "  accepted: %s\n", s);
        CHECK(!r.is_ok());
    }
}

TEST_CASE("web/dicom xml every truncation and random corruption never crash") {
    const Dataset ds = every_vr();
    std::string x;
    REQUIRE(dataset_to_xml(ds, x).is_ok());
    std::size_t accepted = 0;
    for (std::size_t n = 0; n + 1 < x.size(); ++n)
        if (dataset_from_xml(std::string_view(x).substr(0, n))) ++accepted;
    CHECK_EQ(accepted, 0u);
    std::mt19937 rng(777);
    const char noise[] = "<>/=\"'&;#x!-[]?AZaz09 \x01\xC3\xFF";
    for (int iter = 0; iter < 3000; ++iter) {
        std::string m = x;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < edits; ++k) {
            const std::size_t pos = rng() % m.size();
            switch (rng() % 3) {
                case 0: m[pos] = noise[rng() % (sizeof noise - 1)]; break;
                case 1: m.erase(pos, 1 + rng() % 8); break;
                default: m.insert(pos, 1, noise[rng() % (sizeof noise - 1)]); break;
            }
            if (m.empty()) m = "<";
        }
        auto d = dataset_from_xml(m);
        if (d) {
            std::string again;
            CHECK(dataset_to_xml(*d, again).is_ok());
        }
    }
}
