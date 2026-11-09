// Regression tests for the 2026-11-10 02:15 security review, core parser findings (R-051).
#include <cstdint>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"
#include "test.hpp"

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
void short_el(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, const char* vr,
              const std::string& value) {
    tag(b, g, e);
    b.push_back(static_cast<std::uint8_t>(vr[0]));
    b.push_back(static_cast<std::uint8_t>(vr[1]));
    put16(b, static_cast<std::uint16_t>(value.size()));
    b.insert(b.end(), value.begin(), value.end());
}
void long_header(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, const char* vr, std::uint32_t len) {
    tag(b, g, e);
    b.push_back(static_cast<std::uint8_t>(vr[0]));
    b.push_back(static_cast<std::uint8_t>(vr[1]));
    put16(b, 0);
    put32(b, len);
}
void item_header(std::vector<std::uint8_t>& b, std::uint32_t len) {
    tag(b, 0xFFFE, 0xE000);
    put32(b, len);
}

Result<Dataset> parse(const std::vector<std::uint8_t>& b, bool lenient, std::vector<std::string>* warnings = nullptr) {
    ReadOptions opt;
    opt.lenient = lenient;
    auto f = read_part10(Bytes::copy_of(b.data(), b.size()), opt);
    if (!f) return f.take_error();
    if (warnings) *warnings = f->warnings;
    return std::move(f->dataset);
}

// (0008,1140) SQ, defined length, one defined-length item holding a private OB of undefined length
// whose bytes are not items; after the sequence a top-level Patient Name. `inner` is the OB body.
// `tail` follows Patient Name at top level.
std::vector<std::uint8_t> ob_in_item_sample(const std::vector<std::uint8_t>& inner,
                                            const std::vector<std::uint8_t>& tail = {}) {
    std::vector<std::uint8_t> item;
    long_header(item, 0x0009, 0x1001, "OB", 0xFFFFFFFFu);
    item.insert(item.end(), inner.begin(), inner.end());
    std::vector<std::uint8_t> b;
    short_el(b, 0x0008, 0x0016, "UI", "1.2.3.4");
    long_header(b, 0x0008, 0x1140, "SQ", static_cast<std::uint32_t>(8 + item.size()));
    item_header(b, static_cast<std::uint32_t>(item.size()));
    b.insert(b.end(), item.begin(), item.end());
    short_el(b, 0x0010, 0x0010, "PN", "DOE^JOHN");
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

std::vector<std::uint8_t> seq_delim() {
    std::vector<std::uint8_t> d;
    tag(d, 0xFFFE, 0xE0DD);
    put32(d, 0);
    return d;
}

void check_parent_bounded(const std::vector<std::uint8_t>& b, std::size_t inner_size) {
    std::vector<std::string> warnings;
    auto r = parse(b, true, &warnings);
    REQUIRE(r);
    // The top-level Patient Name after the sequence is still a top-level element.
    CHECK_EQ(r->get_string(tags::PatientName).value_or(""), "DOE^JOHN");
    const Element* seq = r->find(0x00081140);
    REQUIRE(seq != nullptr);
    REQUIRE(seq->items.size() == 1u);
    const Element* ob = seq->items[0].find(0x00091001);
    REQUIRE(ob != nullptr);
    std::size_t total = 0;
    for (const Bytes& f : ob->fragments) total += f.size();
    CHECK(total <= inner_size);  // nothing beyond the item end became fragment bytes
    bool flagged = false;
    for (const auto& w : warnings)
        if (w.find("parent") != std::string::npos || w.find("without items") != std::string::npos) flagged = true;
    CHECK(flagged);
    // Strict mode refuses the data set instead of guessing.
    auto strict = parse(b, false);
    REQUIRE(!strict);
    CHECK(strict.error().code == Errc::Malformed || strict.error().code == Errc::Truncated);
}

}  // namespace

TEST_CASE("R-051: undefined-length OB without items inside a defined-length item stays inside it") {
    const std::vector<std::uint8_t> inner = {'X', 'X', 'X', 'X', 'Y', 'Y', 'Y', 'Y'};
    check_parent_bounded(ob_in_item_sample(inner), inner.size());
}

TEST_CASE("R-051: a sequence delimiter beyond the parent is not used to end the OB") {
    // The only delimiter comes after the top-level Patient Name: the old scan ran to it and took
    // Patient Name as fragment bytes.
    const std::vector<std::uint8_t> inner = {'X', 'X', 'X', 'X', 'Y', 'Y', 'Y', 'Y'};
    check_parent_bounded(ob_in_item_sample(inner, seq_delim()), inner.size());
}

TEST_CASE("R-051: fragments without a delimiter and an undefined-length fragment stop at the parent") {
    {
        // One fragment item, then junk with no delimiter.
        std::vector<std::uint8_t> inner;
        item_header(inner, 0);
        item_header(inner, 4);
        inner.insert(inner.end(), {1, 2, 3, 4});
        inner.insert(inner.end(), {'J', 'U', 'N', 'K', 'J', 'U', 'N', 'K'});
        check_parent_bounded(ob_in_item_sample(inner), inner.size());
    }
    {
        // A fragment item of undefined length.
        std::vector<std::uint8_t> inner;
        item_header(inner, 0);
        item_header(inner, 0xFFFFFFFFu);
        inner.insert(inner.end(), {1, 2, 3, 4});
        check_parent_bounded(ob_in_item_sample(inner), inner.size());
    }
}

TEST_CASE("R-051: a well-formed encapsulated OB inside an item still parses") {
    std::vector<std::uint8_t> inner;
    item_header(inner, 0);
    item_header(inner, 4);
    inner.insert(inner.end(), {1, 2, 3, 4});
    const auto d = seq_delim();
    inner.insert(inner.end(), d.begin(), d.end());
    for (bool lenient : {false, true}) {
        std::vector<std::string> warnings;
        auto r = parse(ob_in_item_sample(inner), lenient, &warnings);
        REQUIRE(r);
        CHECK(warnings.empty());
        CHECK_EQ(r->get_string(tags::PatientName).value_or(""), "DOE^JOHN");
        const Element* ob = r->find(0x00081140)->items[0].find(0x00091001);
        REQUIRE(ob != nullptr);
        REQUIRE(ob->fragments.size() == 2u);
        CHECK_EQ(ob->fragments[1].size(), 4u);
    }
}
