#include <string>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "test.hpp"

using namespace siaara;

namespace {

DicomFile make_sample() {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.1234");
    (void)ds.set_string(tags::PatientName, VR::PN, "Test^Patient");
    (void)ds.set_string(tags::PatientID, VR::LO, "ID001");
    (void)ds.set_string(tags::Modality, VR::CS, "CT");
    (void)ds.set_uint16(tags::Rows, 2);
    (void)ds.set_uint16(tags::Columns, 3);
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_string(tags::RescaleSlope, VR::DS, "1.5\\2");
    const std::uint8_t px[12] = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 0xFF, 0xFF};
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px, sizeof px));

    // A nested sequence with two items.
    Element seq;
    seq.tag = 0x00081140;  // ReferencedImageSequence
    seq.vr = VR::SQ;
    for (int i = 0; i < 2; ++i) {
        Dataset item;
        (void)item.set_string(0x00081155, VR::UI, "2.25." + std::to_string(i + 1));
        seq.items.push_back(std::move(item));
    }
    ds.put(std::move(seq));
    return f;
}

void check_sample(const DicomFile& g) {
    CHECK_EQ(g.dataset.get_string(tags::PatientName).value_or(""), "Test^Patient");
    CHECK_EQ(g.dataset.get_string(tags::PatientID).value_or(""), "ID001");
    CHECK_EQ(g.dataset.get_int(tags::Rows).value_or(-1), 2);
    CHECK_EQ(g.dataset.get_int(tags::Columns).value_or(-1), 3);
    CHECK_EQ(g.dataset.get_double(tags::RescaleSlope, 1).value_or(0.0), 2.0);
    CHECK_EQ(g.dataset.value_multiplicity(tags::RescaleSlope), 2u);
    const Element* px = g.dataset.find(tags::PixelData);
    REQUIRE(px != nullptr);
    CHECK_EQ(px->value.size(), 12u);
    CHECK_EQ(g.dataset.get_int(tags::PixelData, 5).value_or(-1), 0xFFFF);
    const Element* seq = g.dataset.find(0x00081140);
    REQUIRE(seq != nullptr);
    REQUIRE(seq->items.size() == 2);
    CHECK_EQ(seq->items[1].get_string(0x00081155).value_or(""), "2.25.2");
}

}  // namespace

TEST_CASE("dictionary: generated from the standard") {
    CHECK(dict::entry_count() > 5000);
    const auto* e = dict::lookup(tags::PatientName);
    REQUIRE(e != nullptr);
    CHECK_EQ(e->keyword, "PatientName");
    CHECK(e->vr == VR::PN);
    CHECK(dict::implicit_vr(tags::PixelData) == VR::OW);
    CHECK(dict::lookup(0x60023000) != nullptr);          // repeating group 60xx
    CHECK(dict::implicit_vr(0x00091010) == VR::UN);       // private data
    CHECK(dict::implicit_vr(0x00090010) == VR::LO);       // private creator
    CHECK(dict::lookup_keyword("StudyInstanceUID") != nullptr);
    const auto* u = dict::lookup_uid("1.2.840.10008.1.2.1");
    REQUIRE(u != nullptr);
    CHECK_EQ(u->keyword, "ExplicitVRLittleEndian");
}

TEST_CASE("transfer syntax table agrees with the dictionary UID registry") {
    const char* uids[] = {"1.2.840.10008.1.2", "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.2",
                          "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.70",
                          "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.90",
                          "1.2.840.10008.1.2.4.201", "1.2.840.10008.1.2.5"};
    for (const char* uid : uids) {
        CHECK(find_transfer_syntax(uid) != nullptr);
        const auto* u = dict::lookup_uid(uid);
        CHECK(u != nullptr && u->type == "Transfer Syntax");
    }
}

TEST_CASE("round trip: explicit VR little endian") {
    DicomFile f = make_sample();
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(g.is_ok());
    CHECK(g->had_preamble);
    CHECK_EQ(g->transfer_syntax, std::string(ts::ExplicitVRLittleEndian));
    check_sample(*g);
}

TEST_CASE("round trip: implicit VR little endian") {
    DicomFile f = make_sample();
    f.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(g.is_ok());
    check_sample(*g);
}

TEST_CASE("round trip: explicit VR big endian") {
    DicomFile f = make_sample();
    f.transfer_syntax = std::string(ts::ExplicitVRBigEndian);
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(g.is_ok());
    check_sample(*g);
}

TEST_CASE("round trip: undefined-length sequences") {
    DicomFile f = make_sample();
    WriteOptions wo;
    wo.sequences_undefined_length = true;
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf, wo).is_ok());
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(g.is_ok());
    check_sample(*g);
}

TEST_CASE("encapsulated pixel data fragments") {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.5";
    Element px;
    px.tag = tags::PixelData;
    px.vr = VR::OB;
    px.undefined_length = true;
    px.fragments.push_back(Bytes{});                          // empty Basic Offset Table
    const std::uint8_t frag[5] = {1, 2, 3, 4, 5};
    px.fragments.push_back(Bytes::copy_of(frag, 5));
    f.dataset.put(std::move(px));
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(g.is_ok());
    const Element* e = g->dataset.find(tags::PixelData);
    REQUIRE(e != nullptr);
    REQUIRE(e->fragments.size() == 2);
    CHECK_EQ(e->fragments[0].size(), 0u);
    CHECK_EQ(e->fragments[1].size(), 6u);  // padded to even length
}

TEST_CASE("header-only parse stops before pixel data") {
    DicomFile f = make_sample();
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    ReadOptions opt;
    opt.stop_before_pixel_data = true;
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()), opt);
    REQUIRE(g.is_ok());
    CHECK(g->dataset.find(tags::PixelData) == nullptr);
    CHECK(g->dataset.contains(tags::Rows));
}

TEST_CASE("malformed input never crashes: every truncation of a valid file") {
    DicomFile f = make_sample();
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    WriteOptions wo;
    wo.sequences_undefined_length = true;
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf, wo).is_ok());
    for (std::size_t n = 0; n < buf.size(); ++n) {
        for (bool lenient : {true, false}) {
            ReadOptions opt;
            opt.lenient = lenient;
            auto g = read_part10(Bytes::copy_of(buf.data(), n), opt);
            (void)g;  // must return (ok or error) without crashing; ASan/UBSan check memory safety
        }
    }
    CHECK(true);
}

TEST_CASE("malformed input never crashes: single byte corruption") {
    DicomFile f = make_sample();
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    for (std::size_t i = 128; i < buf.size(); ++i) {
        for (std::uint8_t v : {std::uint8_t{0x00}, std::uint8_t{0xFF}, std::uint8_t{0x7F}}) {
            std::vector<std::uint8_t> bad = buf;
            bad[i] = v;
            auto g = read_part10(Bytes::copy_of(bad.data(), bad.size()));
            (void)g;
        }
    }
    CHECK(true);
}

TEST_CASE("sequence depth limit") {
    // Build 100 nested sequences, read with a limit of 64.
    Dataset inner;
    (void)inner.set_string(tags::PatientID, VR::LO, "X");
    for (int i = 0; i < 100; ++i) {
        Element seq;
        seq.tag = 0x00081140;
        seq.vr = VR::SQ;
        seq.items.push_back(std::move(inner));
        inner = Dataset{};
        inner.put(std::move(seq));
    }
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    f.dataset = std::move(inner);
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(!g.is_ok());
    CHECK(g.error().code == Errc::LimitExceeded);
}

TEST_CASE("decimal string parsing is exact and locale independent") {
    Dataset ds;
    const std::pair<const char*, double> good[] = {
        {"1.5", 1.5}, {"-0.25e2", -25.0}, {" 3 ", 3.0}, {".5", 0.5}, {"5.", 5.0},
        {"+7E-1", 0.7}, {"0.1", 0.1}, {"1234567890123456", 1234567890123456.0},
        {"-1024", -1024.0}, {"3.14159265358979", 3.14159265358979}};
    for (const auto& [text, value] : good) {
        (void)ds.set_string(tags::RescaleIntercept, VR::DS, text);
        auto v = ds.get_double(tags::RescaleIntercept);
        CHECK(v.has_value() && *v == value);
    }
    for (const char* bad : {"abc", "1e400", "", "1.2.3", "e5", "--1", "1e"}) {
        (void)ds.set_string(tags::RescaleIntercept, VR::DS, bad);
        CHECK(!ds.get_double(tags::RescaleIntercept).has_value());
    }
}

TEST_CASE("AT values keep (group, element) order in every byte order") {
    for (auto ts_uid : {ts::ExplicitVRLittleEndian, ts::ImplicitVRLittleEndian, ts::ExplicitVRBigEndian}) {
        DicomFile f;
        f.transfer_syntax = std::string(ts_uid);
        const std::uint8_t at[8] = {0x28, 0x00, 0x10, 0x00, 0x20, 0x00, 0x32, 0x00};  // (0028,0010) (0020,0032)
        (void)f.dataset.set_bytes(0x00209165, VR::AT, Bytes::copy_of(at, 8));  // DimensionIndexPointer
        std::vector<std::uint8_t> buf;
        REQUIRE(write_part10(f, buf).is_ok());
        auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()));
        REQUIRE(g.is_ok());
        CHECK_EQ(g->dataset.get_int(0x00209165, 0).value_or(0), 0x00280010);
        CHECK_EQ(g->dataset.get_int(0x00209165, 1).value_or(0), 0x00200032);
    }
}

TEST_CASE("element offsets and header-only stop offset point at the encoded bytes") {
    DicomFile f = make_sample();
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    ReadOptions opt;
    opt.stop_before_pixel_data = true;
    auto g = read_part10(Bytes::copy_of(buf.data(), buf.size()), opt);
    REQUIRE(g.is_ok());
    CHECK(g->offsets_are_file_offsets);
    const std::uint64_t at = g->stopped_at_offset;
    REQUIRE(at + 4 <= buf.size());
    CHECK(buf[at] == 0xE0 && buf[at + 1] == 0x7F && buf[at + 2] == 0x10 && buf[at + 3] == 0x00);
    const Element* rows = g->dataset.find(tags::Rows);
    REQUIRE(rows != nullptr);
    CHECK(buf[rows->offset] == 0x28 && buf[rows->offset + 2] == 0x10);
}
