// Release-candidate review, small items: R-171 (the validator's per-frame loop stops at the
// findings cap), R-183 (the multipart stream parser is linear for one buffer of many small parts),
// R-164 (a value whose stated end is followed by a fitting, dictionary-known header is not
// "recovered", whatever the tag order).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/reader.hpp"
#include "core/writer.hpp"
#include "repair/validate.hpp"
#include "test.hpp"
#include "web/multipart.hpp"

using namespace siaara;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

Dataset& add_item(Dataset& ds, Tag seq) {
    Element* e = ds.find(seq);
    if (!e) {
        Element s;
        s.tag = seq;
        s.vr = VR::SQ;
        e = &ds.put(std::move(s));
    }
    return e->items.emplace_back();
}

// An Enhanced MR Image with `frames` Per-Frame Functional Groups items, each holding functional
// group macros whose mandatory attributes are missing (findings in every frame).
std::vector<std::uint8_t> enhanced_mr(std::uint32_t frames) {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.4.1");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.171001");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.171900");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.171901");
    (void)ds.set_string(tags::Modality, VR::CS, "MR");
    (void)ds.set_string(tags::NumberOfFrames, VR::IS, std::to_string(frames));
    Dataset& shared = add_item(ds, 0x52009229);
    (void)add_item(shared, 0x00289110);  // Pixel Measures Sequence: an empty item
    for (std::uint32_t i = 0; i < frames; ++i) {
        Dataset& per = add_item(ds, 0x52009230);
        (void)add_item(per, 0x00209111);  // Frame Content Sequence: an empty item
        (void)add_item(per, 0x00209113);  // Plane Position Sequence: an empty item
        (void)add_item(per, 0x00209116);  // Plane Orientation Sequence: an empty item
        (void)add_item(per, 0x00189226);  // MR Image Frame Type Sequence: an empty item
    }
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return out;
}

}  // namespace

TEST_CASE("rc1 R-171: the validator's per-frame checks stop once the findings cap is reached (100,000 frames)") {
    const std::vector<std::uint8_t> bytes = enhanced_mr(100000);
    REQUIRE(bytes.size() > 1000000);
    auto scan = repair::scan(Bytes::copy_of(bytes.data(), bytes.size()), {});
    REQUIRE(scan);
    // A cap that the checks before the per-frame ones already reach (the top level of this
    // instance lacks more than 20 mandatory attributes).
    repair::ValidateOptions capped;
    capped.max_findings = 20;
    capped.check_dictionary = false;  // the IOD checks are what is measured
    capped.check_pixels = false;
    capped.check_charset = false;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<repair::Finding> few = repair::validate(*scan, capped);
    const double capped_s = seconds_since(t0);
    CHECK_EQ(few.size(), 20u);
    // Without the cap in the way the same checks run over every frame: the reference time.
    repair::ValidateOptions all = capped;
    all.max_findings = 10000000;
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<repair::Finding> many = repair::validate(*scan, all);
    const double all_s = seconds_since(t1);
    CHECK(many.size() > 100000u);
    std::fprintf(stderr, "  validator, 100000 frames: capped at 20 findings %.3f s, all findings %.3f s\n", capped_s, all_s);
    // On the old code the capped run walked every frame of every macro all the same (as long as
    // the uncapped run). Where the cap is reached only inside the per-frame checks, the frames
    // looked at before that still cost their time: that work is not wasted.
    CHECK(capped_s < all_s / 4);
    CHECK(capped_s < 5.0);
}

TEST_CASE("net2: multipart rc1 R-183 one buffer of very many small parts is parsed in linear time") {
    const std::string boundary = "rc1boundary";
    const auto body_of = [&](std::size_t parts) {
        std::string body;
        body.reserve(parts * 64);
        for (std::size_t i = 0; i < parts; ++i) {
            body += "--" + boundary + "\r\nContent-Type: text/plain\r\n\r\n";
            body += std::to_string(i);
            body += "\r\n";
        }
        body += "--" + boundary + "--\r\n";
        return body;
    };
    const auto run = [&](std::size_t parts, std::size_t piece, double& seconds) {
        const std::string body = body_of(parts);
        std::size_t seen = 0;
        bool in_order = true;
        web::multipart::Limits lim;
        lim.max_parts = parts;
        web::multipart::StreamParser p(boundary, lim, 1u << 20, [&](const web::multipart::Part& part) -> Status {
            in_order = in_order && part.body == std::to_string(seen);
            ++seen;
            return Status::ok();
        });
        const auto t0 = std::chrono::steady_clock::now();
        Status s = Status::ok();
        for (std::size_t off = 0; s && off < body.size(); off += piece) s = p.feed(std::string_view(body).substr(off, piece));
        const Status fin = s ? p.finish() : s;
        seconds = seconds_since(t0);
        return fin && in_order && seen == parts && p.parts() == parts;
    };
    double small_s = 0, big_s = 0, pieces_s = 0;
    CHECK(run(25000, std::string::npos, small_s));
    CHECK(run(400000, std::string::npos, big_s));  // 16 times the parts in one buffer
    std::fprintf(stderr, "  multipart, one buffer: 25,000 parts %.3f s, 400,000 parts %.3f s\n", small_s, big_s);
    // Linear: about 16 times the time. Quadratic (the old erase per part): about 256 times.
    CHECK(big_s < 60 * (small_s + 0.01));
    CHECK(big_s < 20.0);
    // The same result however the body is cut (the fuzz property), also across compactions.
    for (const std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{4095}, std::size_t{4097}, std::size_t{65536}})
        CHECK(run(3000, piece, pieces_s));
}

namespace {
void put16(std::vector<std::uint8_t>& b, std::uint32_t x) {
    b.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}
// Implicit VR little endian element.
void imp(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, std::string_view v) {
    put16(b, g);
    put16(b, e);
    put16(b, static_cast<std::uint32_t>(v.size() & 0xFFFFu));
    put16(b, static_cast<std::uint32_t>(v.size() >> 16));
    b.insert(b.end(), v.begin(), v.end());
}
}  // namespace

TEST_CASE("rc1 R-164: a value ending in bytes that look like a header, before an out-of-order element, is not read short") {
    // Patient's Name ends in 10 00 20 00 - the first half of a (0010,0020) header. The next
    // element is out of order: Length to End (0008,0001), whose tag bytes read as the length
    // 0x00010008 of that would-be Patient ID. Everything is laid out so that the would-be header
    // fits and is followed by two more known headers in ascending order: the old recovery took
    // the name's stated length for a GE-style overrun and read the name four bytes short.
    const std::string name = std::string("AB") + std::string("\x10\x00\x20\x00", 4);
    std::vector<std::uint8_t> b;
    imp(b, 0x0008, 0x0016, std::string("1.2.840.10008.5.1.4.1.1.4") + '\0');
    imp(b, 0x0008, 0x0018, "2.25.164001");
    imp(b, 0x0010, 0x0010, name);
    imp(b, 0x0008, 0x0001, std::string("\x00\x00\x00\x00", 4));      // out of order
    imp(b, 0x0010, 0x4000, std::string(65528, 'c'));                 // ends where the would-be Patient ID value ends
    imp(b, 0x0020, 0x000D, "2.25.164900 ");
    imp(b, 0x0020, 0x000E, "2.25.164901 ");
    imp(b, 0x0028, 0x0010, std::string("\x02\x00", 2));
    auto f = read_dataset_report(Bytes::copy_of(b.data(), b.size()), "1.2.840.10008.1.2");
    REQUIRE(f);
    CHECK(ReadOptions{}.recover_overlong_lengths);  // the local default: the recovery is on
    int recovered = 0;
    for (const std::string& w : f->warnings) recovered += w.find("ELEMENT_LENGTH_RECOVERED") != std::string::npos ? 1 : 0;
    CHECK_EQ(recovered, 0);
    CHECK((f->read_loss & DicomFile::kReadRecovered) == 0);
    const Element* pn = f->dataset.find(0x00100010);
    REQUIRE(pn != nullptr);
    CHECK_EQ(pn->value.size(), name.size());
    CHECK(std::string(pn->value.as_chars()) == name);
    CHECK(!f->dataset.contains(0x00100020));  // there is no Patient ID in these bytes
    CHECK(f->dataset.contains(0x00080001));
    const Element* comments = f->dataset.find(0x00104000);
    REQUIRE(comments != nullptr);
    CHECK_EQ(comments->value.size(), 65528u);
    CHECK(f->dataset.get_string(0x0020000D).value_or("") == "2.25.164900");
    CHECK(f->dataset.get_string(0x0020000E).value_or("") == "2.25.164901");
}

TEST_CASE("rc2 R-164: a group length element at a value's stated end is a real header too - the value is not read short") {
    // As above, but the out-of-order element after the name is a group length element
    // (0008,0000), length 4: no attribute of the dictionary, yet six of its eight header bytes
    // are fixed. Its tag bytes read as the length 8 of the would-be Patient ID at four bytes
    // before the stated end, whose "value" is then the rest of the group length element, and
    // the UIDs that follow complete a run of three: the recovery took that for a GE overrun.
    const std::string name = std::string("AB") + std::string("\x10\x00\x20\x00", 4);
    std::vector<std::uint8_t> b;
    imp(b, 0x0008, 0x0016, std::string("1.2.840.10008.5.1.4.1.1.4") + '\0');
    imp(b, 0x0008, 0x0018, "2.25.164002");
    imp(b, 0x0010, 0x0010, name);
    imp(b, 0x0008, 0x0000, std::string("\x2A\x00\x00\x00", 4));      // out of order: a group length element
    imp(b, 0x0020, 0x000D, "2.25.164900 ");
    imp(b, 0x0020, 0x000E, "2.25.164901 ");
    imp(b, 0x0028, 0x0010, std::string("\x02\x00", 2));
    auto f = read_dataset_report(Bytes::copy_of(b.data(), b.size()), "1.2.840.10008.1.2");
    REQUIRE(f);
    int recovered = 0;
    for (const std::string& w : f->warnings) recovered += w.find("ELEMENT_LENGTH_RECOVERED") != std::string::npos ? 1 : 0;
    CHECK_EQ(recovered, 0);
    CHECK((f->read_loss & DicomFile::kReadRecovered) == 0);
    const Element* pn = f->dataset.find(0x00100010);
    REQUIRE(pn != nullptr);
    CHECK_EQ(pn->value.size(), name.size());
    CHECK(!f->dataset.contains(0x00100020));
    CHECK(f->dataset.contains(0x00080000));
    CHECK(f->dataset.get_string(0x0020000D).value_or("") == "2.25.164900");
    // The GE case itself is still recovered: a value stated 3 bytes too long right before the
    // group length element of the next group.
    std::vector<std::uint8_t> ge;
    imp(ge, 0x0008, 0x0016, std::string("1.2.840.10008.5.1.4.1.1.4") + '\0');
    imp(ge, 0x0008, 0x0018, "2.25.164003");
    imp(ge, 0x0008, 0x103E, "AX T1 POST");
    ge[ge.size() - 10 - 4] = 13;  // the stated length: 13 for 10 bytes
    imp(ge, 0x0010, 0x0000, std::string("\x12\x00\x00\x00", 4));
    imp(ge, 0x0010, 0x0010, "TEN^B ");
    imp(ge, 0x0010, 0x0020, "ID4A");
    imp(ge, 0x0020, 0x000D, "2.25.164900 ");
    auto g = read_dataset_report(Bytes::copy_of(ge.data(), ge.size()), "1.2.840.10008.1.2");
    REQUIRE(g);
    CHECK((g->read_loss & DicomFile::kReadRecovered) != 0);
    CHECK(g->dataset.get_string(0x0008103E).value_or("") == "AX T1 POST");
    CHECK(g->dataset.get_string(0x00100020).value_or("") == "ID4A");
}
