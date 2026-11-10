// F15: a sequence item without its own Specific Character Set (0008,0005) carries the value in
// force in its enclosing data set, so its text decodes correctly when the item is used on its own
// (a copy in C++, a borrowed item handle in the C API). PS3.5 7.5.3.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "web/dicom_json.hpp"

using namespace siaara;

namespace {

constexpr Tag kPatientName = 0x00100010;
constexpr Tag kInstitutionName = 0x00080080;
constexpr Tag kSeqA = 0x0040A730;  // Content Sequence
constexpr Tag kSeqB = 0x00081115;  // Referenced Series Sequence

// "Müller^Jürgen" in ISO 8859-1 and in UTF-8.
const std::string kLatin1 = "M\xFCller^J\xFCrgen";
const std::string kUtf8 = "M\xC3\xBCller^J\xC3\xBCrgen";
// "Ωmega" in UTF-8 (not representable in Latin-1).
const std::string kOmega = "\xCE\xA9mega";

void put_text(Dataset& d, Tag t, VR vr, std::string_view bytes) { (void)d.set_string(t, vr, bytes); }

Element sequence(Tag t, std::vector<Dataset> items) {
    Element e;
    e.tag = t;
    e.vr = VR::SQ;
    for (auto& d : items) e.items.push_back(std::move(d));
    return e;
}

// Top: ISO_IR 100. Content Sequence item 0: no charset, Latin-1 PN, with a nested item (no
// charset, Latin-1 LO). Item 1: own ISO_IR 192 (override), UTF-8 PN, with a nested item that
// inherits ISO_IR 192.
Dataset build() {
    Dataset nested0;
    put_text(nested0, kInstitutionName, VR::LO, kLatin1);
    Dataset item0;
    put_text(item0, kPatientName, VR::PN, kLatin1);
    item0.put(sequence(kSeqB, {nested0}));

    Dataset nested1;
    put_text(nested1, kInstitutionName, VR::LO, kOmega);
    Dataset item1;
    put_text(item1, tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    put_text(item1, kPatientName, VR::PN, kUtf8);
    item1.put(sequence(kSeqB, {nested1}));

    Dataset top;
    (void)top.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)top.set_string(tags::SOPInstanceUID, VR::UI, "1.2.3.4.15");
    put_text(top, tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
    put_text(top, kPatientName, VR::PN, kLatin1);
    top.put(sequence(kSeqA, {item0, item1}));
    return top;
}

std::vector<std::uint8_t> to_part10(const Dataset& ds) {
    DicomFile f;
    f.dataset = ds;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    std::vector<std::uint8_t> bytes;
    CHECK(write_part10(f, bytes).is_ok());
    return bytes;
}

const Dataset& item(const Dataset& d, Tag seq, std::size_t i) { return d.find(seq)->items[i]; }

// Every item decodes on its own (a copy, no inherited argument).
void check_items_alone(const Dataset& top) {
    const Dataset i0 = item(top, kSeqA, 0);  // copies
    const Dataset i1 = item(top, kSeqA, 1);
    const Dataset n0 = item(i0, kSeqB, 0);
    const Dataset n1 = item(i1, kSeqB, 0);
    CHECK_EQ(i0.inherited_charset(), "ISO_IR 100");
    CHECK_EQ(n0.inherited_charset(), "ISO_IR 100");
    CHECK_EQ(n1.inherited_charset(), "ISO_IR 192");
    CHECK_EQ(charset::get_utf8(i0, kPatientName).value_or(""), kUtf8);
    CHECK_EQ(charset::get_utf8(n0, kInstitutionName).value_or(""), kUtf8);
    CHECK_EQ(charset::get_utf8(i1, kPatientName).value_or(""), kUtf8);  // own ISO_IR 192 wins
    CHECK_EQ(charset::get_utf8(n1, kInstitutionName).value_or(""), kOmega);
}

}  // namespace

TEST_CASE("F15: items built in memory carry the enclosing character set") { check_items_alone(build()); }

TEST_CASE("F15: items read from a Part 10 file carry the enclosing character set (ISO_IR 100, 192 override)") {
    const auto bytes = to_part10(build());
    auto f = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(f.is_ok());
    check_items_alone(f->dataset);
    // An explicit inherited value still overrides the recorded one (siaara_dataset_get_utf8_cs).
    const charset::CharacterSet def = charset::CharacterSet::parse("");
    CHECK_EQ(charset::get_utf8(item(f->dataset, kSeqA, 0), kPatientName, 0, &def).value_or(""),
             "M\xEF\xBF\xBDller^J\xEF\xBF\xBDrgen");  // U+FFFD for the Latin-1 bytes
}

TEST_CASE("F15: changing or removing (0008,0005) updates the items") {
    Dataset top = build();
    put_text(top, tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    CHECK_EQ(item(top, kSeqA, 0).inherited_charset(), "ISO_IR 192");
    CHECK_EQ(item(item(top, kSeqA, 0), kSeqB, 0).inherited_charset(), "ISO_IR 192");
    CHECK_EQ(item(item(top, kSeqA, 1), kSeqB, 0).inherited_charset(), "ISO_IR 192");  // item 1's own
    top.remove(tags::SpecificCharacterSet);
    CHECK_EQ(item(top, kSeqA, 0).inherited_charset(), "");
    CHECK_EQ(item(item(top, kSeqA, 1), kSeqB, 0).inherited_charset(), "ISO_IR 192");
    // An item's own value removed: its children inherit from above it again.
    Dataset t2 = build();
    Element& seq = *t2.find(kSeqA);
    seq.items[1].remove(tags::SpecificCharacterSet);
    CHECK_EQ(item(seq.items[1], kSeqB, 0).inherited_charset(), "ISO_IR 100");
}

TEST_CASE("F15: DICOM JSON input records the inherited character set") {
    // JSON text is UTF-8; an item with its own ISO_IR 100 keeps it, the items below inherit it.
    const std::string json =
        "{\"00080005\":{\"vr\":\"CS\",\"Value\":[\"ISO_IR 192\"]},"
        "\"0040A730\":{\"vr\":\"SQ\",\"Value\":[{\"00100010\":{\"vr\":\"PN\",\"Value\":[{\"Alphabetic\":\"" + kUtf8 + "\"}]}}]}}";
    auto d = web::dataset_from_json(json);
    REQUIRE(d.is_ok());
    const Dataset i0 = item(*d, kSeqA, 0);
    CHECK_EQ(i0.inherited_charset(), "ISO_IR 192");
    CHECK_EQ(charset::get_utf8(i0, kPatientName).value_or(""), kUtf8);
}

TEST_CASE("F15: C API item handles decode and encode with the inherited character set") {
    const auto bytes = to_part10(build());
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &file) == SIAARA_OK);
    siaara_dataset* top = nullptr;
    REQUIRE(siaara_file_dataset(file, &top) == SIAARA_OK);
    char buf[64];
    size_t need = 0;
    auto text = [&](siaara_dataset* d, uint32_t t) {
        buf[0] = 0;
        CHECK_EQ(siaara_dataset_get_utf8(d, t, 0, buf, sizeof buf, &need), SIAARA_OK);
        return std::string(buf);
    };
    siaara_dataset *i0 = nullptr, *i1 = nullptr, *n0 = nullptr, *n1 = nullptr;
    REQUIRE(siaara_dataset_item(top, kSeqA, 0, &i0) == SIAARA_OK);
    REQUIRE(siaara_dataset_item(top, kSeqA, 1, &i1) == SIAARA_OK);
    REQUIRE(siaara_dataset_item(i0, kSeqB, 0, &n0) == SIAARA_OK);
    const std::string path = "ContentSequence[1].ReferencedSeriesSequence[0]";
    REQUIRE(siaara_dataset_by_path(top, path.data(), path.size(), &n1) == SIAARA_OK);
    CHECK_EQ(text(i0, kPatientName), kUtf8);
    CHECK_EQ(text(n0, kInstitutionName), kUtf8);
    CHECK_EQ(text(i1, kPatientName), kUtf8);
    CHECK_EQ(text(n1, kInstitutionName), kOmega);
    // All values in one call, decoded likewise.
    CHECK_EQ(siaara_dataset_get_strings(i0, kPatientName, 1, buf, sizeof buf, &need, nullptr), SIAARA_OK);
    CHECK_EQ(std::string(buf), kUtf8);
    // The explicit form still overrides.
    CHECK_EQ(siaara_dataset_get_utf8_cs(i0, kPatientName, 0, "ISO_IR 192", 10, buf, sizeof buf, &need), SIAARA_OK);
    CHECK(std::string(buf) != kUtf8);
    // Encoding into an inheriting item uses Latin-1: Ω cannot be represented, ü can.
    CHECK_EQ(siaara_dataset_set_utf8(i0, kInstitutionName, "LO", kOmega.data(), kOmega.size()), SIAARA_ERR_UNSUPPORTED);
    REQUIRE(siaara_dataset_set_utf8(i0, kInstitutionName, "LO", kUtf8.data(), kUtf8.size()) == SIAARA_OK);
    const uint8_t* raw = nullptr;
    size_t raw_n = 0;
    REQUIRE(siaara_dataset_get_raw(i0, kInstitutionName, &raw, &raw_n) == SIAARA_OK);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(raw), kLatin1.size()), kLatin1);
    // A new item appended through the C API inherits too.
    siaara_dataset* added = nullptr;
    REQUIRE(siaara_dataset_append_item(top, kSeqA, &added, nullptr) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_bytes(added, kPatientName, "PN", reinterpret_cast<const uint8_t*>(kLatin1.data()), kLatin1.size()) == SIAARA_OK);
    CHECK_EQ(text(added, kPatientName), kUtf8);
    // Changing the top-level value through the C API reaches the items.
    REQUIRE(siaara_dataset_set_string(top, tags::SpecificCharacterSet, "CS", "ISO_IR 192", 10) == SIAARA_OK);
    REQUIRE(siaara_dataset_item(top, kSeqA, 0, &i0) == SIAARA_OK);
    CHECK(text(i0, kPatientName) != kUtf8);  // Latin-1 bytes are no longer valid UTF-8
    siaara_file_free(file);
}
