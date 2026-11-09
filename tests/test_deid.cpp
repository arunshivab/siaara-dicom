// De-identification tests (PS3.15 Annex E): generated tables, every Table E.1-1 row under every
// option, nested sequences, PHI leakage in written bytes, UID consistency, date shifting,
// private attributes, burned-in annotation reporting, pixel blanking, SHA-256/HMAC vectors.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "deid/deid.hpp"
#include "deid/sha256.hpp"
#include "dict/dictionary.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::deid;

namespace {

// SHA-256 of src/deid/deid_table.inc as produced by tools/gen_deid.py from the 2026d XML.
// tests/crosscheck/deid_table_check.py regenerates the file and compares it byte for byte.
constexpr std::string_view kTableSha256 = "afef23cccc489baa1677b11632ff1e0fb1da5f1a658e13bdb12a054859ca372e";

std::string hex(const Digest& d, std::size_t n = 32) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(kHex[d[i] >> 4]);
        s.push_back(kHex[d[i] & 15]);
    }
    return s;
}

std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

const std::vector<std::uint8_t> kKey = bytes_of("0123456789abcdef-test-key");
const std::vector<std::uint8_t> kKey2 = bytes_of("another-secret-key-0000");

Options make_options(std::uint32_t bits, const std::vector<std::uint8_t>& key = kKey) {
    Options o;
    o.options = bits;
    o.key = key;
    return o;
}

std::string value_of(const Dataset& ds, Tag tag) {
    const Element* e = ds.find(tag);
    if (!e) return {};
    return std::string(trim_value(e->value.as_chars(), e->vr));
}

Element make_element(Tag tag, VR vr, std::string_view v) {
    Element e;
    e.tag = tag;
    e.vr = vr;
    std::string s(v);
    if (s.size() & 1u) s.push_back(vr_pad_char(vr));
    e.value = Bytes::from_string(s);
    return e;
}

std::string marker(Tag t) {
    char b[16];
    std::snprintf(b, sizeof b, "PHI%08X", t);
    return b;
}
std::string seq_marker(Tag t) {
    char b[16];
    std::snprintf(b, sizeof b, "PHS%08X", t);
    return b;
}

constexpr std::string_view kDate = "19870615";
constexpr std::string_view kDateTime = "19870615103000";
constexpr std::string_view kTime = "103000";

// Concrete tag for a (possibly repeating-group) row.
Tag concrete(const ProfileRow& r) {
    if (r.mask == 0xFF000000u) return r.tag | 0x3000u;  // (50xx,xxxx): Curve Data (50xx,3000)
    return r.tag;
}

bool skipped_group(Tag t) {
    const std::uint16_t g = tag_group(t);
    return g == 0x0000 || g == 0x0002 || g == 0x0004;
}

VR row_vr(Tag t) {
    const dict::Entry* e = dict::lookup(t);
    return e ? e->vr : VR::None;
}

// Plants a recognisable value for `tag`. `uid_index` makes each UID distinct.
void plant(Dataset& ds, Tag tag, VR vr, std::size_t uid_index) {
    const std::string m = marker(tag);
    switch (vr) {
        case VR::AE: case VR::CS: case VR::SH: case VR::LO: case VR::LT: case VR::ST: case VR::UT:
        case VR::UC: case VR::PN:
            ds.put(make_element(tag, vr, m));
            break;
        case VR::UR: ds.put(make_element(tag, vr, "http://h/" + m)); break;
        case VR::AS: ds.put(make_element(tag, vr, "042Y")); break;
        case VR::DA: ds.put(make_element(tag, vr, kDate)); break;
        case VR::DT: ds.put(make_element(tag, vr, kDateTime)); break;
        case VR::TM: ds.put(make_element(tag, vr, kTime)); break;
        case VR::DS: ds.put(make_element(tag, vr, "12.5")); break;
        case VR::IS: ds.put(make_element(tag, vr, "42")); break;
        case VR::UI: ds.put(make_element(tag, vr, "1.2.999." + std::to_string(uid_index + 1))); break;
        case VR::SQ: {
            Element e;
            e.tag = tag;
            e.vr = VR::SQ;
            Dataset item;
            item.put(make_element(tags::PatientName, VR::PN, seq_marker(tag)));
            item.put(make_element(0x00081155, VR::UI, "1.2.999.77." + std::to_string(uid_index + 1)));
            e.items.push_back(std::move(item));
            ds.put(std::move(e));
            break;
        }
        case VR::OB: case VR::OW: case VR::OF: case VR::OD: case VR::OL: case VR::OV: case VR::UN: {
            std::string v = m + "!";  // 12 bytes, a multiple of 2, 4 and... 12 is not of 8: pad
            if (vr == VR::OD || vr == VR::OV) v += "....";
            Element e;
            e.tag = tag;
            e.vr = vr;
            e.value = Bytes::from_string(v);
            ds.put(std::move(e));
            break;
        }
        default: {
            std::size_t n = vr == VR::AT ? 4 : vr_value_size(vr);
            if (n == 0) n = 4;
            std::vector<std::uint8_t> b(n, 0x5A);
            Element e;
            e.tag = tag;
            e.vr = vr;
            e.value = Bytes::copy_of(b.data(), b.size());
            ds.put(std::move(e));
        }
    }
}

// The planted data set: every plantable row at the top level and again inside an item of VOI LUT
// Sequence (0028,3010), which Table E.1-1 does not list (so it is kept and walked).
constexpr Tag kCarrier = 0x00283010;

Dataset planted_dataset(std::size_t* planted = nullptr) {
    Dataset ds;
    Dataset inner;
    std::size_t n = 0, idx = 0;
    for (const ProfileRow& r : profile_rows()) {
        const Tag t = concrete(r);
        ++idx;
        if (skipped_group(t)) continue;
        const VR vr = row_vr(t);
        if (vr == VR::None) continue;
        plant(ds, t, vr, idx);
        plant(inner, t, vr, idx + 1000);
        ++n;
    }
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    Element carrier;
    carrier.tag = kCarrier;
    carrier.vr = VR::SQ;
    carrier.items.push_back(std::move(inner));
    ds.put(std::move(carrier));
    if (planted) *planted = n;
    return ds;
}

// Expected action, computed here from the row independently of the engine.
Action expected_action(const ProfileRow& r, std::uint32_t bits) {
    bool c = false;
    for (std::size_t i = 0; i < kOptionCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if (r.option[i] == Action::K) return Action::K;
        if (r.option[i] == Action::C) c = true;
    }
    Action a = c ? Action::C : r.basic;
    switch (a) {  // CombinedPolicy::Conformant
        case Action::XZ: return Action::Z;
        case Action::XD: case Action::XZD: case Action::ZD: return Action::D;
        case Action::XZU: return Action::U;
        default: return a;
    }
}

bool is_text(VR vr) {
    switch (vr) {
        case VR::AE: case VR::CS: case VR::SH: case VR::LO: case VR::LT: case VR::ST: case VR::UT:
        case VR::UC: case VR::PN: case VR::UR:
            return true;
        default:
            return false;
    }
}
bool is_bulk(VR vr) {
    return vr == VR::OB || vr == VR::OW || vr == VR::OF || vr == VR::OD || vr == VR::OL || vr == VR::OV ||
           vr == VR::UN;
}

struct RowCheck {
    std::size_t checked = 0;
    std::size_t failed = 0;
};

// Checks one row's outcome in `out` (the processed data set at one level) against `before`.
void check_row(const ProfileRow& r, const Dataset& before, const Dataset& out, std::uint32_t bits, RowCheck& rc,
               const char* where) {
    const Tag t = concrete(r);
    const Element* orig = before.find(t);
    if (!orig) return;
    const VR vr = row_vr(t);
    const Element* e = out.find(t);
    const Action a = expected_action(r, bits);
    bool ok = true;
    const std::string ov(trim_value(orig->value.as_chars(), vr));
    const std::string nv = e ? std::string(trim_value(e->value.as_chars(), vr)) : std::string();
    switch (a) {
        case Action::X:
            ok = e == nullptr;
            break;
        case Action::Z:
            ok = e && e->value.empty() && e->items.empty();
            break;
        case Action::D:
            if (!e) { ok = false; break; }
            if (vr == VR::SQ) ok = e->items.size() == 1 && e->items[0].empty();
            else if (vr == VR::UI) ok = nv.rfind("2.25.", 0) == 0;
            else ok = !e->value.empty() && (nv != ov || !is_text(vr)) && nv.find("PHI") == std::string::npos;
            if (vr == VR::DA || vr == VR::DT) ok = ok && nv != ov;
            break;
        case Action::K:
            if (!e) { ok = false; break; }
            ok = vr == VR::SQ ? e->items.size() == orig->items.size() : (e->value.span().size() == orig->value.span().size() &&
                                                                         std::equal(e->value.span().begin(), e->value.span().end(),
                                                                                    orig->value.span().begin()));
            break;
        case Action::U:
            if (!e) { ok = false; break; }
            if (vr == VR::SQ) ok = e->items.size() == orig->items.size();
            else ok = nv.rfind("2.25.", 0) == 0 && nv != ov;
            break;
        case Action::C:
            if (!e) { ok = false; break; }
            if (vr == VR::SQ) ok = e->items.size() == orig->items.size();
            else if (vr == VR::DA || vr == VR::DT) {
                std::string tmp;
                ok = nv != ov && !nv.empty() && shift_date(nv, vr, 0, tmp);
            } else if (vr == VR::TM || vr == VR::DS || vr == VR::IS || vr == VR::AS) ok = nv == ov;
            else if (vr == VR::UI) ok = nv.rfind("2.25.", 0) == 0;
            else if (is_text(vr)) ok = nv == "CLEANED";
            else if (is_bulk(vr)) ok = e->value.size() == orig->value.size() &&
                                       std::all_of(e->value.span().begin(), e->value.span().end(),
                                                   [](std::uint8_t b) { return b == 0; });
            else ok = nv == ov;
            break;
        default:
            ok = false;
    }
    ++rc.checked;
    if (!ok) {
        ++rc.failed;
        if (rc.failed <= 10)
            std::fprintf(stderr, "  row (%04X,%04X) %.*s [%s] options %03X: expected action %d not applied\n",
                         tag_group(t), tag_element(t), static_cast<int>(r.name.size()), r.name.data(), where, bits,
                         static_cast<int>(a));
    }
}

const std::vector<std::uint32_t>& option_combinations() {
    static const std::vector<std::uint32_t> combos = [] {
        std::vector<std::uint32_t> v{0};
        for (std::size_t i = 0; i < kOptionCount; ++i) v.push_back(1u << i);
        v.push_back(kAllOptionBits & ~RetainLongitudinalFullDates);
        v.push_back(kAllOptionBits & ~RetainLongitudinalModifiedDates);
        v.push_back(CleanDescriptors | CleanStructuredContent | CleanGraphics | RetainLongitudinalModifiedDates);
        return v;
    }();
    return combos;
}

DicomFile file_of(Dataset ds) {
    DicomFile f;
    f.dataset = std::move(ds);
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    return f;
}

bool contains(const std::vector<std::uint8_t>& hay, std::string_view needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

}  // namespace

// ---------------------------------------------------------------- crypto vectors

TEST_CASE("deid: SHA-256 FIPS 180-4 example vectors") {
    CHECK_EQ(hex(Sha256::hash("abc", 3)), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(hex(Sha256::hash("", 0)), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(hex(Sha256::hash(m448.data(), m448.size())),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string m896 =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    CHECK_EQ(hex(Sha256::hash(m896.data(), m896.size())),
             "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    // One million 'a', fed in uneven pieces to exercise buffering.
    Sha256 s;
    const std::string chunk(997, 'a');
    std::size_t left = 1000000;
    while (left) {
        const std::size_t n = std::min(left, chunk.size());
        s.update(chunk.data(), n);
        left -= n;
    }
    CHECK_EQ(hex(s.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Every length around the padding boundary agrees between one-shot and byte-wise hashing.
    for (std::size_t len = 50; len < 140; ++len) {
        const std::string m(len, 'x');
        Sha256 b;
        for (char c : m) b.update(&c, 1);
        CHECK(b.finish() == Sha256::hash(m.data(), m.size()));
    }
}

TEST_CASE("deid: HMAC-SHA-256 RFC 4231 test cases") {
    struct V {
        std::vector<std::uint8_t> key;
        std::string data;
        std::string mac;
        std::size_t bytes;
    };
    const std::vector<std::uint8_t> k25 = [] {
        std::vector<std::uint8_t> k;
        for (int i = 1; i <= 25; ++i) k.push_back(static_cast<std::uint8_t>(i));
        return k;
    }();
    const std::vector<V> vs = {
        {std::vector<std::uint8_t>(20, 0x0b), "Hi There",
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32},
        {bytes_of("Jefe"), "what do ya want for nothing?",
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32},
        {std::vector<std::uint8_t>(20, 0xaa), std::string(50, '\xdd'),
         "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32},
        {k25, std::string(50, '\xcd'), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 32},
        {std::vector<std::uint8_t>(20, 0x0c), "Test With Truncation", "a3b6167473100ee06e0c796c2955552b", 16},
        {std::vector<std::uint8_t>(131, 0xaa), "Test Using Larger Than Block-Size Key - Hash Key First",
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32},
        {std::vector<std::uint8_t>(131, 0xaa),
         "This is a test using a larger than block-size key and a larger than block-size data. The key needs "
         "to be hashed before being used by the HMAC algorithm.",
         "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", 32},
    };
    for (const V& v : vs) {
        CHECK_EQ(hex(hmac_sha256(v.key, v.data.data(), v.data.size()), v.bytes), v.mac);
        HmacSha256 h(v.key);
        // Split input gives the same MAC, and the precomputed key state is reusable.
        const std::size_t half = v.data.size() / 2;
        CHECK_EQ(hex(h.mac(std::string_view(v.data).substr(0, half), std::string_view(v.data).substr(half)), v.bytes),
                 v.mac);
        CHECK_EQ(hex(h.mac(v.data), v.bytes), v.mac);
    }
}

TEST_CASE("deid: secure_wipe clears memory") {
    std::uint8_t b[32];
    std::memset(b, 0x77, sizeof b);
    secure_wipe(b, sizeof b);
    CHECK(std::all_of(std::begin(b), std::end(b), [](std::uint8_t x) { return x == 0; }));
}

// ---------------------------------------------------------------- generated tables

TEST_CASE("deid: generated table matches the generator output (hash)") {
#ifdef SIAARA_DEID_TABLE_PATH
    std::FILE* f = std::fopen(SIAARA_DEID_TABLE_PATH, "rb");
    REQUIRE(f != nullptr);
    std::string data;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    std::fclose(f);
    CHECK_EQ(hex(Sha256::hash(data.data(), data.size())), std::string(kTableSha256));
#endif
    CHECK_EQ(profile_rows().size(), 656u);
    CHECK_EQ(safe_private_rows().size(), 479u);
    CHECK_EQ(method_codes().size(), 13u);
}

TEST_CASE("deid: table lookups") {
    const ProfileRow* r = find_row(tags::PatientName);
    REQUIRE(r != nullptr);
    CHECK(r->basic == Action::Z);
    r = find_row(tags::PatientID);
    REQUIRE(r != nullptr);
    CHECK(r->basic == Action::ZD);
    r = find_row(0x60123000);  // Overlay Data in group 6012
    REQUIRE(r != nullptr);
    CHECK(r->tag == 0x60003000u);
    r = find_row(0x50041234);  // Curve Data (50xx,xxxx)
    REQUIRE(r != nullptr);
    CHECK(r->mask == 0xFF000000u);
    CHECK(find_row(kCarrier) == nullptr);
    CHECK(find_row(tags::SOPClassUID) == nullptr);
    r = find_row(tags::SOPInstanceUID);
    REQUIRE(r != nullptr);
    CHECK(r->basic == Action::U);
    CHECK(effective_action(*r, RetainUids) == Action::K);
    CHECK(private_attributes_row().basic == Action::X);
    CHECK(private_attributes_row().option[0] == Action::C);
    // Sorted and unique (binary search relies on it).
    const auto rows = profile_rows();
    for (std::size_t i = 1; i < rows.size(); ++i)
        CHECK(rows[i - 1].tag < rows[i].tag || (rows[i - 1].tag == rows[i].tag && rows[i - 1].mask < rows[i].mask));
    // Safe private: lower-case hex in the standard's XML is normalised by the generator.
    CHECK(find_safe_private(0x0299, "NQRight", 0x0A) != nullptr);
    CHECK(find_safe_private(0x0019, "GEMS_ACQU_01", 0x23) != nullptr);
    CHECK(find_safe_private(0x0019, "GEMS_ACQU_01", 0x22) == nullptr);
    CHECK(find_safe_private(0x0019, "gems_acqu_01", 0x23) == nullptr);
    const auto sp = safe_private_rows();
    for (std::size_t i = 1; i < sp.size(); ++i) {
        const auto& a = sp[i - 1];
        const auto& b = sp[i];
        CHECK(a.group < b.group || (a.group == b.group && (a.creator < b.creator ||
                                                           (a.creator == b.creator && a.element < b.element))));
    }
    bool has_basic = false;
    for (const MethodCode& c : method_codes())
        if (c.value == "113100" && c.scheme == "DCM") has_basic = true;
    CHECK(has_basic);
}

// ---------------------------------------------------------------- every row, every option

TEST_CASE("deid: every Table E.1-1 row under every option, top level and nested") {
    std::size_t planted = 0;
    const Dataset before = planted_dataset(&planted);
    CHECK(planted > 600);
    const Dataset& inner_before = before.find(kCarrier)->items[0];
    RowCheck rc;
    for (std::uint32_t bits : option_combinations()) {
        DicomFile f = file_of(before);
        Report rep;
        REQUIRE(deidentify(f, make_options(bits), &rep).is_ok());
        const Element* carrier = f.dataset.find(kCarrier);
        REQUIRE(carrier != nullptr);
        REQUIRE(carrier->items.size() == 1);
        for (const ProfileRow& r : profile_rows()) {
            check_row(r, before, f.dataset, bits, rc, "top");
            check_row(r, inner_before, carrier->items[0], bits, rc, "nested");
        }
        CHECK(rep.items_visited > 0);
    }
    std::printf("  rows checked: %zu (%zu failed) over %zu option combinations\n", rc.checked, rc.failed,
                option_combinations().size());
    CHECK_EQ(rc.failed, 0u);
}

TEST_CASE("deid: rows of groups 0000, 0002 and 0004 are removed from the data set") {
    Dataset ds;
    ds.put(make_element(0x00041511, VR::UI, "1.2.999.1"));
    ds.put(make_element(0x00041130, VR::CS, "PHIFILESET"));
    ds.put(make_element(0x00020003, VR::UI, "1.2.999.2"));
    ds.put(make_element(0x00001000, VR::UI, "1.2.999.3"));
    DicomFile f = file_of(ds);
    REQUIRE(deidentify(f, make_options(0)).is_ok());
    CHECK(!f.dataset.contains(0x00041511));
    CHECK(!f.dataset.contains(0x00041130));
    CHECK(!f.dataset.contains(0x00020003));
    CHECK(!f.dataset.contains(0x00001000));
}

TEST_CASE("deid: no planted PHI survives in the written bytes") {
    for (std::uint32_t bits : std::initializer_list<std::uint32_t>{0u, RetainLongitudinalModifiedDates,
                               CleanDescriptors | CleanStructuredContent | CleanGraphics | RetainLongitudinalModifiedDates,
                               RetainSafePrivate}) {
        Dataset ds = planted_dataset();
        // Private data of an unknown creator, a private sequence, and PHI deep in nested items.
        ds.put(make_element(0x00290010, VR::LO, "ACME PRIVATE"));
        ds.put(make_element(0x00291001, VR::LO, "PHIPRIVATE1"));
        {
            Element sq;
            sq.tag = 0x00291002;
            sq.vr = VR::SQ;
            Dataset item;
            item.put(make_element(tags::PatientName, VR::PN, "PHIPRIVSEQ"));
            sq.items.push_back(std::move(item));
            ds.put(std::move(sq));
        }
        {
            Dataset level3;
            level3.put(make_element(tags::PatientName, VR::PN, "PHIDEEP3"));
            level3.put(make_element(0x00081155, VR::UI, "1.2.999.3.3"));
            Element s3; s3.tag = kCarrier; s3.vr = VR::SQ; s3.items.push_back(std::move(level3));
            Dataset level2; level2.put(std::move(s3));
            Element s2; s2.tag = 0x00540016; s2.vr = VR::SQ; s2.items.push_back(std::move(level2));
            Dataset level1; level1.put(std::move(s2));
            Element s1; s1.tag = 0x00189346; s1.vr = VR::SQ; s1.items.push_back(std::move(level1));
            ds.put(std::move(s1));
        }
        DicomFile f = file_of(std::move(ds));
        f.meta.put(make_element(0x00020016, VR::AE, "PHISOURCEAE"));
        f.meta.put(make_element(0x00020100, VR::UI, "1.2.999.100"));
        f.meta.put(make_element(0x00020102, VR::OB, "PHIMETAPRIV0"));
        const Dataset original = f.dataset;
        REQUIRE(deidentify(f, make_options(bits)).is_ok());
        std::vector<std::uint8_t> bytes;
        REQUIRE(write_part10(f, bytes).is_ok());
        std::size_t leaks = 0;
        auto expect_absent = [&](std::string_view s) {
            if (contains(bytes, s)) {
                if (++leaks <= 10) std::fprintf(stderr, "  leaked: %.*s (options %03X)\n", static_cast<int>(s.size()), s.data(), bits);
            }
        };
        for (const ProfileRow& r : profile_rows()) {
            const Tag t = concrete(r);
            if (skipped_group(t) || !original.contains(t)) continue;
            if (expected_action(r, bits) == Action::K) continue;
            expect_absent(marker(t));
        }
        for (const ProfileRow& r : profile_rows()) expect_absent(seq_marker(concrete(r)));
        expect_absent("PHIPRIVATE1");
        expect_absent("PHIPRIVSEQ");
        expect_absent("PHIDEEP3");
        expect_absent("PHISOURCEAE");
        expect_absent("PHIMETAPRIV");
        expect_absent("1.2.999.");  // every planted UID is replaced (no Retain UIDs here)
        expect_absent(kDate);       // all dates removed, dummied or shifted
        CHECK_EQ(leaks, 0u);
        // The written file reads back.
        auto back = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
        CHECK(back.is_ok());
    }
}

// ---------------------------------------------------------------- UIDs

TEST_CASE("deid: UID replacement is consistent within and across runs, and key dependent") {
    auto make_pair_of = [] {
        Dataset a;
        (void)a.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
        (void)a.set_string(tags::SOPInstanceUID, VR::UI, "1.2.826.0.1.3680043.9.1.1");
        (void)a.set_string(tags::StudyInstanceUID, VR::UI, "1.2.826.0.1.3680043.9.1");
        (void)a.set_string(tags::SeriesInstanceUID, VR::UI, "1.2.826.0.1.3680043.9.1.0");
        Dataset b = a;
        (void)b.set_string(tags::SOPInstanceUID, VR::UI, "1.2.826.0.1.3680043.9.1.2");
        // b references a in Source Image Sequence and, two levels down, in a nested reference.
        Dataset ref;
        (void)ref.set_string(0x00081150, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
        (void)ref.set_string(0x00081155, VR::UI, "1.2.826.0.1.3680043.9.1.1");
        Element src; src.tag = 0x00082112; src.vr = VR::SQ; src.items.push_back(ref);
        b.put(src);
        Dataset inner; inner.put(src);
        Element outer; outer.tag = 0x00081115; outer.vr = VR::SQ; outer.items.push_back(inner);
        b.put(outer);
        return std::make_pair(file_of(a), file_of(b));
    };
    auto [a1, b1] = make_pair_of();
    auto [a2, b2] = make_pair_of();
    REQUIRE(deidentify(a1, make_options(0)).is_ok());
    REQUIRE(deidentify(b1, make_options(0)).is_ok());
    REQUIRE(deidentify(a2, make_options(0)).is_ok());
    const std::string a_uid = value_of(a1.dataset, tags::SOPInstanceUID);
    CHECK(a_uid.rfind("2.25.", 0) == 0);
    CHECK(a_uid.size() <= 64);
    CHECK(a_uid.find_first_not_of("0123456789.") == std::string::npos);
    CHECK(a_uid[5] != '0');
    CHECK_EQ(a_uid, value_of(a2.dataset, tags::SOPInstanceUID));  // same key, separate run
    CHECK_EQ(a_uid, replacement_uid(kKey, "1.2.826.0.1.3680043.9.1.1"));
    CHECK_EQ(value_of(a1.dataset, tags::StudyInstanceUID), value_of(b1.dataset, tags::StudyInstanceUID));
    CHECK(value_of(a1.dataset, tags::SOPInstanceUID) != value_of(b1.dataset, tags::SOPInstanceUID));
    // References inside sequences follow the referenced instance.
    const Element* src = b1.dataset.find(0x00082112);
    REQUIRE(src && src->items.size() == 1);
    CHECK_EQ(value_of(src->items[0], 0x00081155), a_uid);
    CHECK_EQ(value_of(src->items[0], 0x00081150), "1.2.840.10008.5.1.4.1.1.2");  // SOP Class kept
    const Element* outer = b1.dataset.find(0x00081115);
    REQUIRE(outer && outer->items.size() == 1);
    const Element* nested = outer->items[0].find(0x00082112);
    REQUIRE(nested && nested->items.size() == 1);
    CHECK_EQ(value_of(nested->items[0], 0x00081155), a_uid);
    // File meta follows the new SOP Instance UID.
    CHECK_EQ(value_of(a1.meta, tags::MediaStorageSOPInstanceUID), a_uid);
    CHECK_EQ(value_of(a1.meta, tags::MediaStorageSOPClassUID), "1.2.840.10008.5.1.4.1.1.2");
    // A different key gives different UIDs.
    auto [a3, b3] = make_pair_of();
    REQUIRE(deidentify(a3, make_options(0, kKey2)).is_ok());
    CHECK(value_of(a3.dataset, tags::SOPInstanceUID) != a_uid);
    // Retain UIDs keeps them.
    auto [a4, b4] = make_pair_of();
    REQUIRE(deidentify(a4, make_options(RetainUids)).is_ok());
    CHECK_EQ(value_of(a4.dataset, tags::SOPInstanceUID), "1.2.826.0.1.3680043.9.1.1");
    CHECK_EQ(value_of(a4.meta, tags::MediaStorageSOPInstanceUID), "1.2.826.0.1.3680043.9.1.1");
    // Padding does not change the mapping; distinct UIDs map to distinct UIDs.
    CHECK_EQ(replacement_uid(kKey, std::string("1.2.3") + '\0'), replacement_uid(kKey, "1.2.3"));
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i) seen.insert(replacement_uid(kKey, "1.2.3." + std::to_string(i)));
    CHECK_EQ(seen.size(), 2000u);
    for (const std::string& u : seen) {
        CHECK(u.size() <= 64);
        CHECK(u.size() > 5 && u[5] != '0');
    }
}

TEST_CASE("deid: unlisted UIDs are replaced unless well known") {
    Dataset ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.4");
    constexpr Tag kUnlistedUi = 0x00080062;  // SOP Classes in Study
    REQUIRE(find_row(kUnlistedUi) == nullptr);
    REQUIRE(dict::lookup(kUnlistedUi) && dict::lookup(kUnlistedUi)->vr == VR::UI);
    (void)ds.set_string(kUnlistedUi, VR::UI, "1.2.999.5\\1.2.840.10008.5.1.4.1.1.4");
    DicomFile f = file_of(ds);
    REQUIRE(deidentify(f, make_options(0)).is_ok());
    CHECK_EQ(value_of(f.dataset, tags::SOPClassUID), "1.2.840.10008.5.1.4.1.1.4");
    const std::string v = value_of(f.dataset, kUnlistedUi);
    CHECK(v.rfind("2.25.", 0) == 0);
    CHECK(v.find("\\1.2.840.10008.5.1.4.1.1.4") != std::string::npos);
}

// ---------------------------------------------------------------- dates

TEST_CASE("deid: modified dates keep intervals per patient") {
    auto make = [](std::string_view pid, std::string_view study, std::string_view acq, std::string_view dt) {
        Dataset ds;
        (void)ds.set_string(tags::PatientID, VR::LO, pid);
        (void)ds.set_string(0x00080020, VR::DA, study);
        (void)ds.set_string(0x00080022, VR::DA, acq);
        (void)ds.set_string(0x0008002A, VR::DT, dt);
        (void)ds.set_string(0x00080030, VR::TM, "235959.5");
        (void)ds.set_string(0x00080032, VR::TM, "000001");
        return file_of(ds);
    };
    auto days = [](std::string_view da) {
        // days since 0000-03-01 style count, enough for differences
        const int y = std::stoi(std::string(da.substr(0, 4)));
        const int m = std::stoi(std::string(da.substr(4, 2)));
        const int d = std::stoi(std::string(da.substr(6, 2)));
        const int yy = m <= 2 ? y - 1 : y;
        const int era = yy / 400, yoe = yy - era * 400, mp = (m + 9) % 12;
        const int doy = (153 * mp + 2) / 5 + d - 1;
        return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
    };
    DicomFile a = make("PAT-A", "20200228", "20200301", "20200229120000.25+0530");
    DicomFile b = make("PAT-A", "20210110", "20210110", "2021");
    DicomFile c = make("PAT-B", "20200228", "20200301", "20200229");
    for (DicomFile* f : {&a, &b, &c}) REQUIRE(deidentify(*f, make_options(RetainLongitudinalModifiedDates)).is_ok());
    const std::string s1 = value_of(a.dataset, 0x00080020), q1 = value_of(a.dataset, 0x00080022);
    const std::string s2 = value_of(b.dataset, 0x00080020);
    CHECK(s1 != "20200228");
    CHECK_EQ(days(q1) - days(s1), days("20200301") - days("20200228"));      // across a leap day
    CHECK_EQ(days(s2) - days(s1), days("20210110") - days("20200228"));      // across files, same patient
    const int off = days(s1) - days("20200228");
    CHECK_EQ(off, date_offset_days(kKey, "PAT-A"));
    CHECK(off <= -1);
    CHECK(off >= -3652);
    const std::string dt = value_of(a.dataset, 0x0008002A);
    CHECK_EQ(dt.substr(8), "120000.25+0530");
    CHECK_EQ(days(dt.substr(0, 8)) - days("20200229"), off);
    CHECK_EQ(value_of(a.dataset, 0x00080030), "235959.5");  // times kept
    CHECK_EQ(value_of(b.dataset, 0x0008002A).size(), 4u);  // precision kept
    CHECK_EQ(value_of(a.dataset, 0x00280303), "MODIFIED");
    // Another patient gets its own offset (these two keys/IDs are known to differ).
    CHECK(date_offset_days(kKey, "PAT-B") != date_offset_days(kKey, "PAT-A"));
    CHECK_EQ(days(value_of(c.dataset, 0x00080020)) - days("20200228"), date_offset_days(kKey, "PAT-B"));
    // Full Dates keeps them and says so.
    DicomFile d = make("PAT-A", "20200228", "20200301", "2020");
    REQUIRE(deidentify(d, make_options(RetainLongitudinalFullDates)).is_ok());
    CHECK_EQ(value_of(d.dataset, 0x00080020), "20200228");
    CHECK_EQ(value_of(d.dataset, 0x00280303), "UNMODIFIED");
    // Basic profile: dates go (X/Z -> Z with the conformant policy).
    DicomFile e = make("PAT-A", "20200228", "20200301", "2020");
    REQUIRE(deidentify(e, make_options(0)).is_ok());
    CHECK(value_of(e.dataset, 0x00080020).empty());
    // Both date options together are refused and the file is unchanged.
    DicomFile g = make("PAT-A", "20200228", "20200301", "2020");
    CHECK(deidentify(g, make_options(RetainLongitudinalFullDates | RetainLongitudinalModifiedDates)).code ==
          Errc::InvalidArgument);
    CHECK_EQ(value_of(g.dataset, 0x00080020), "20200228");
}

TEST_CASE("deid: shift_date edge cases") {
    std::string out;
    CHECK(shift_date("20200301", VR::DA, -1, out) && out == "20200229");
    CHECK(shift_date("20210301", VR::DA, -1, out) && out == "20210228");
    CHECK(shift_date("20000229", VR::DA, 366, out) && out == "20010301");
    CHECK(shift_date("19000301", VR::DA, -1, out) && out == "19000228");  // 1900 is not a leap year
    CHECK(shift_date("2020.03.01", VR::DA, -1, out) && out == "20200229");
    CHECK(shift_date("20200101-20200201", VR::DA, -1, out) && out == "20191231-20200131");
    CHECK(shift_date("-20200201", VR::DA, -1, out) && out == "-20200131");
    CHECK(!shift_date("20200230", VR::DA, -1, out));
    CHECK(!shift_date("2020013", VR::DA, -1, out));
    CHECK(!shift_date("2O200101", VR::DA, -1, out));
    CHECK(!shift_date("00010101", VR::DA, -1, out));  // before year 1
    CHECK(shift_date("202003", VR::DT, -1, out) && out == "202002");
    CHECK(shift_date("20200101000000-0500", VR::DT, -1, out) && out == "20191231000000-0500");
    CHECK(!shift_date("2020010", VR::DT, -1, out));
    CHECK(!shift_date("2020010199", VR::DT, -1, out));
    CHECK(shift_date("235959.123456", VR::TM, -1, out) && out == "235959.123456");
    CHECK(shift_date("23:59:59", VR::TM, -1, out));
    CHECK(!shift_date("2500", VR::TM, -1, out));
}

TEST_CASE("deid: unparseable dates are emptied under Modified Dates") {
    Dataset ds;
    (void)ds.set_string(0x00080020, VR::DA, "PHIBADDATE");
    DicomFile f = file_of(ds);
    Report rep;
    REQUIRE(deidentify(f, make_options(RetainLongitudinalModifiedDates), &rep).is_ok());
    CHECK(value_of(f.dataset, 0x00080020).empty());
    CHECK(rep.flags & UnparseableValuesRemoved);
}

// ---------------------------------------------------------------- private attributes

TEST_CASE("deid: private attributes removed unless safe and Retain Safe Private") {
    auto make = [] {
        Dataset ds;
        ds.put(make_element(0x00190010, VR::LO, "GEMS_ACQU_01"));
        ds.put(make_element(0x00191023, VR::DS, "1.375"));       // safe: Table Speed
        ds.put(make_element(0x00191050, VR::LO, "PHIGEUNSAFE"));  // not in Table E.3.10-1
        ds.put(make_element(0x00190011, VR::LO, "SOMEONE ELSE"));
        ds.put(make_element(0x00191123, VR::DS, "9"));            // same offset, other creator
        ds.put(make_element(0x00290010, VR::LO, "UNKNOWN"));
        ds.put(make_element(0x00291001, VR::LO, "PHIUNKNOWN"));
        ds.put(make_element(0x00311001, VR::LO, "PHINOCREATOR"));   // no creator at all
        ds.put(make_element(0x00070010, VR::LO, "PHIILLEGALGROUP"));  // odd group below 0009
        ds.put(make_element(0x00090001, VR::LO, "PHIRESERVED"));      // (gggg,0001-000F)
        return file_of(ds);
    };
    DicomFile basic = make();
    Report rb;
    REQUIRE(deidentify(basic, make_options(0), &rb).is_ok());
    for (const Element& e : basic.dataset.elements()) CHECK(!(tag_group(e.tag) & 1u));
    CHECK(!basic.dataset.contains(0x00070010));
    CHECK(rb.private_removed >= 9);

    DicomFile safe = make();
    Report rs;
    REQUIRE(deidentify(safe, make_options(RetainSafePrivate), &rs).is_ok());
    CHECK(safe.dataset.contains(0x00190010));
    CHECK_EQ(value_of(safe.dataset, 0x00191023), "1.375");
    CHECK(!safe.dataset.contains(0x00191050));
    CHECK(!safe.dataset.contains(0x00190011));  // creator of a block that kept nothing
    CHECK(!safe.dataset.contains(0x00191123));
    CHECK(!safe.dataset.contains(0x00290010));
    CHECK(!safe.dataset.contains(0x00291001));
    CHECK(!safe.dataset.contains(0x00311001));
    CHECK(!safe.dataset.contains(0x00090001));
    CHECK_EQ(rs.private_kept, 1u);
    bool coded = false;
    if (const Element* seq = safe.dataset.find(0x00120064))
        for (const Dataset& item : seq->items)
            if (value_of(item, 0x00080100) == "113111") coded = true;
    CHECK(coded);
}

TEST_CASE("deid: safe private sequences are walked, unsafe content inside is handled") {
    const SafePrivateRow* sq = nullptr;
    for (const SafePrivateRow& r : safe_private_rows())
        if (r.is_sequence) { sq = &r; break; }
    if (!sq) return;  // the current table lists no sequence
    const Tag creator = make_tag(sq->group, 0x0010);
    const Tag elem = make_tag(sq->group, static_cast<std::uint16_t>(0x1000u | sq->element));
    Dataset ds;
    ds.put(make_element(creator, VR::LO, sq->creator));
    Element e; e.tag = elem; e.vr = VR::SQ;
    Dataset item;
    item.put(make_element(tags::PatientName, VR::PN, "PHIINSIDE"));
    item.put(make_element(0x00081155, VR::UI, "1.2.999.4"));
    e.items.push_back(item);
    ds.put(e);
    DicomFile f = file_of(ds);
    REQUIRE(deidentify(f, make_options(RetainSafePrivate)).is_ok());
    const Element* out = f.dataset.find(elem);
    REQUIRE(out && out->items.size() == 1);
    CHECK(value_of(out->items[0], tags::PatientName).empty());
    CHECK(value_of(out->items[0], 0x00081155).rfind("2.25.", 0) == 0);
}

// ---------------------------------------------------------------- burned-in annotation, pixels

namespace {
DicomFile image(std::uint16_t rows, std::uint16_t cols, std::string_view photometric, std::uint16_t spp,
                std::uint16_t bits, std::uint32_t frames = 1, bool is_signed = false) {
    Dataset ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "1.2.999.50");
    (void)ds.set_uint16(tags::Rows, rows);
    (void)ds.set_uint16(tags::Columns, cols);
    (void)ds.set_uint16(tags::SamplesPerPixel, spp);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, photometric);
    (void)ds.set_uint16(tags::BitsAllocated, bits);
    (void)ds.set_uint16(tags::BitsStored, bits == 16 ? 12 : bits);
    (void)ds.set_uint16(tags::HighBit, static_cast<std::uint16_t>((bits == 16 ? 12 : bits) - 1));
    (void)ds.set_uint16(tags::PixelRepresentation, is_signed ? 1 : 0);
    if (spp > 1) (void)ds.set_uint16(tags::PlanarConfiguration, 0);
    if (frames > 1) (void)ds.set_string(tags::NumberOfFrames, VR::IS, std::to_string(frames));
    std::size_t n = std::size_t{rows} * cols * spp * frames * (bits / 8u);
    if (bits == 1) n = (std::size_t{rows} * cols * frames + 7) / 8;
    if (n & 1u) ++n;
    std::vector<std::uint8_t> px(n, 0x11);
    if (bits == 1) std::fill(px.begin(), px.end(), 0xFF);
    (void)ds.set_bytes(tags::PixelData, bits > 8 ? VR::OW : VR::OB, Bytes::copy_of(px.data(), px.size()));
    return file_of(ds);
}
}  // namespace

TEST_CASE("deid: burned-in annotation reporting") {
    {
        DicomFile f = image(4, 4, "MONOCHROME2", 1, 8);  // (0028,0301) absent
        Report r;
        REQUIRE(deidentify(f, make_options(0), &r).is_ok());
        CHECK(r.flags & BurnedInAnnotationRemains);
        CHECK(r.flags & NotFullyDeidentified);
    }
    {
        DicomFile f = image(4, 4, "MONOCHROME2", 1, 8);
        (void)f.dataset.set_string(0x00280301, VR::CS, "YES");
        Report r;
        REQUIRE(deidentify(f, make_options(0), &r).is_ok());
        CHECK(r.flags & BurnedInAnnotationRemains);
    }
    {
        DicomFile f = image(4, 4, "MONOCHROME2", 1, 8);
        (void)f.dataset.set_string(0x00280301, VR::CS, "NO");
        Report r;
        REQUIRE(deidentify(f, make_options(0), &r).is_ok());
        CHECK(!(r.flags & NotFullyDeidentified));
    }
    {
        DicomFile f = image(4, 4, "MONOCHROME2", 1, 8);
        (void)f.dataset.set_string(0x00280301, VR::CS, "NO");
        (void)f.dataset.set_string(0x00280302, VR::CS, "YES");
        Report r;
        REQUIRE(deidentify(f, make_options(0), &r).is_ok());
        CHECK(r.flags & RecognizableFeaturesRemain);
        CHECK(r.flags & NotFullyDeidentified);
        CHECK(!(r.flags & BurnedInAnnotationRemains));
    }
    {
        // Regions applied but not asserted complete: still reported.
        DicomFile f = image(4, 4, "MONOCHROME2", 1, 8);
        Options o = make_options(0);
        o.pixel_regions.push_back({kAllFrames, 0, 0, 2, 2});
        Report r;
        REQUIRE(deidentify(f, o, &r).is_ok());
        CHECK(r.flags & PixelDataCleaned);
        CHECK(r.flags & BurnedInAnnotationRemains);
    }
    {
        // Cleaned and asserted: Burned In Annotation NO and code 113101.
        DicomFile f = image(4, 4, "MONOCHROME2", 1, 8);
        (void)f.dataset.set_string(0x00280301, VR::CS, "YES");
        Options o = make_options(0);
        o.pixel_regions.push_back({0, 1, 1, 2, 2});
        o.pixel_data_clean = true;
        Report r;
        REQUIRE(deidentify(f, o, &r).is_ok());
        CHECK(!(r.flags & NotFullyDeidentified));
        CHECK_EQ(value_of(f.dataset, 0x00280301), "NO");
        bool coded = false;
        for (const Dataset& item : f.dataset.find(0x00120064)->items)
            if (value_of(item, 0x00080100) == "113101") coded = true;
        CHECK(coded);
        const Element* px = f.dataset.find(tags::PixelData);
        const std::uint8_t* p = px->value.data();
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const bool in = x >= 1 && x <= 2 && y >= 1 && y <= 2;
                CHECK_EQ(p[y * 4 + x], in ? 0 : 0x11);
            }
    }
    {
        // No pixel data: nothing to report.
        Dataset ds;
        (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.88.11");
        DicomFile f = file_of(ds);
        Report r;
        REQUIRE(deidentify(f, make_options(0), &r).is_ok());
        CHECK(!(r.flags & NotFullyDeidentified));
    }
}

TEST_CASE("deid: pixel blanking per photometric interpretation and layout") {
    {
        DicomFile f = image(2, 4, "MONOCHROME1", 1, 16, 2);
        PixelRegion r{1, 0, 0, 1, 1};
        REQUIRE(blank_pixel_regions(f.dataset, std::span<const PixelRegion>(&r, 1)).is_ok());
        const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
        CHECK_EQ(load_le16(p), 0x1111);              // frame 0 untouched
        CHECK_EQ(load_le16(p + 16), 0x0FFF);         // frame 1 pixel 0: 12-bit maximum
        CHECK_EQ(load_le16(p + 18), 0x1111);
    }
    {
        DicomFile f = image(2, 2, "MONOCHROME2", 1, 16, 1, true);
        PixelRegion r{kAllFrames, 1, 1, 5, 5};  // clipped to the image
        REQUIRE(blank_pixel_regions(f.dataset, std::span<const PixelRegion>(&r, 1)).is_ok());
        const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
        CHECK_EQ(load_le16(p + 6), 0x0800);  // 12-bit two's complement minimum
        CHECK_EQ(load_le16(p + 4), 0x1111);
    }
    {
        DicomFile f = image(2, 2, "YBR_FULL", 3, 8);
        PixelRegion r{kAllFrames, 0, 0, 1, 1};
        REQUIRE(blank_pixel_regions(f.dataset, std::span<const PixelRegion>(&r, 1)).is_ok());
        const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
        CHECK(p[0] == 0 && p[1] == 128 && p[2] == 128 && p[3] == 0x11);
    }
    {
        DicomFile f = image(2, 2, "RGB", 3, 8);
        (void)f.dataset.set_uint16(tags::PlanarConfiguration, 1);
        PixelRegion r{kAllFrames, 1, 0, 1, 1};
        REQUIRE(blank_pixel_regions(f.dataset, std::span<const PixelRegion>(&r, 1)).is_ok());
        const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
        CHECK(p[1] == 0 && p[5] == 0 && p[9] == 0 && p[0] == 0x11 && p[4] == 0x11);
    }
    {
        DicomFile f = image(1, 4, "MONOCHROME2", 1, 1);
        PixelRegion r{kAllFrames, 1, 0, 2, 1};
        REQUIRE(blank_pixel_regions(f.dataset, std::span<const PixelRegion>(&r, 1)).is_ok());
        CHECK_EQ(f.dataset.find(tags::PixelData)->value.data()[0], 0xF9);  // bits 1 and 2 cleared
    }
    {
        DicomFile f = image(2, 2, "MONOCHROME2", 1, 8);
        PixelRegion r{5, 0, 0, 1, 1};
        CHECK(blank_pixel_regions(f.dataset, std::span<const PixelRegion>(&r, 1)).code == Errc::InvalidArgument);
    }
    {
        // Encapsulated pixel data cannot be blanked; deidentify fails and leaves the file alone.
        DicomFile f = image(2, 2, "MONOCHROME2", 1, 8);
        Element* px = f.dataset.find(tags::PixelData);
        px->undefined_length = true;
        px->fragments = {Bytes{}, Bytes::from_string("JPEGDATA")};
        px->value = {};
        f.transfer_syntax = "1.2.840.10008.1.2.4.50";
        (void)f.dataset.set_string(tags::PatientName, VR::PN, "PHIKEEPONFAIL");
        Options o = make_options(0);
        o.pixel_regions.push_back({kAllFrames, 0, 0, 1, 1});
        CHECK(deidentify(f, o).code == Errc::Unsupported);
        CHECK_EQ(value_of(f.dataset, tags::PatientName), "PHIKEEPONFAIL");
    }
}

TEST_CASE("deid: embedded overlay bits are cleared and overlay groups removed") {
    DicomFile f = image(1, 2, "MONOCHROME2", 1, 16);
    std::uint8_t px[4] = {0x00, 0x80, 0x01, 0x80};  // bit 15 set (embedded overlay)
    (void)f.dataset.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px, 4));
    (void)f.dataset.set_uint16(0x60000010, 1);
    (void)f.dataset.set_uint16(0x60000011, 2);
    (void)f.dataset.set_uint16(0x60000100, 16);
    (void)f.dataset.set_uint16(0x60000102, 15);
    f.dataset.put(make_element(0x60000022, VR::LO, "PHIOVERLAYDESC"));
    // A separate overlay with its own data: removed with its whole group by the Basic Profile.
    (void)f.dataset.set_uint16(0x60020010, 1);
    f.dataset.put(make_element(0x60021500, VR::LO, "PHIOVERLAYLABEL"));
    (void)f.dataset.set_bytes(0x60023000, VR::OW, Bytes::copy_of(px, 4));
    REQUIRE(deidentify(f, make_options(0)).is_ok());
    const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
    CHECK(p[1] == 0x00 && p[3] == 0x00 && p[2] == 0x01);
    for (const Element& e : f.dataset.elements()) CHECK((tag_group(e.tag) & 0xFF00u) != 0x6000u);
}

TEST_CASE("deid: Clean Graphics keeps overlays with their data zeroed and text cleaned") {
    DicomFile f = image(1, 2, "MONOCHROME2", 1, 8);
    (void)f.dataset.set_uint16(0x60000010, 1);
    f.dataset.put(make_element(0x60001500, VR::LO, "PHIOVERLAYLABEL"));
    std::uint8_t ov[2] = {0x03, 0x00};
    (void)f.dataset.set_bytes(0x60003000, VR::OW, Bytes::copy_of(ov, 2));
    REQUIRE(deidentify(f, make_options(CleanGraphics)).is_ok());
    CHECK(f.dataset.contains(0x60000010));
    CHECK_EQ(value_of(f.dataset, 0x60001500), "CLEANED");
    CHECK_EQ(f.dataset.find(0x60003000)->value.data()[0], 0);
}

// ---------------------------------------------------------------- other rules

TEST_CASE("deid: method attributes, file meta and conformant dummies") {
    Dataset ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "1.2.999.60");
    (void)ds.set_string(tags::PatientID, VR::LO, "MRN12345");
    (void)ds.set_string(0x00080080, VR::LO, "PHIHOSPITAL");  // Institution Name X/Z/D
    DicomFile f = file_of(ds);
    f.meta.put(make_element(0x00020013, VR::SH, "OLDIMPL"));
    REQUIRE(deidentify(f, make_options(CleanDescriptors | RetainPatientCharacteristics)).is_ok());
    CHECK_EQ(value_of(f.dataset, 0x00120062), "YES");
    const std::string method = value_of(f.dataset, 0x00120063);
    CHECK(method.find("Basic Application Confidentiality Profile") != std::string::npos);
    CHECK(method.find("Clean Descriptors Option") != std::string::npos);
    std::set<std::string> codes;
    for (const Dataset& item : f.dataset.find(0x00120064)->items) {
        codes.insert(value_of(item, 0x00080100));
        CHECK_EQ(value_of(item, 0x00080102), "DCM");
        CHECK(!value_of(item, 0x00080104).empty());
    }
    CHECK(codes == std::set<std::string>({"113100", "113105", "113108"}));
    CHECK(!f.meta.contains(0x00020013));  // rebuilt; the writer adds this toolkit's identity
    const std::string pid = value_of(f.dataset, tags::PatientID);
    CHECK(pid.size() == 18 && pid.rfind("ID", 0) == 0);
    DicomFile g = file_of(ds);
    REQUIRE(deidentify(g, make_options(0)).is_ok());
    CHECK_EQ(value_of(g.dataset, tags::PatientID), pid);  // consistent pseudonym
    CHECK_EQ(value_of(g.dataset, 0x00080080), "DEIDENTIFIED");
}

TEST_CASE("deid: combined actions follow the attribute type callback and the remove policy") {
    Dataset ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    (void)ds.set_string(0x00080080, VR::LO, "PHIHOSPITAL");  // X/Z/D
    (void)ds.set_string(0x00080022, VR::DA, "20200101");      // X/Z
    (void)ds.set_string(0x00081040, VR::LO, "PHIDEPT");       // X
    (void)ds.set_string(tags::PatientID, VR::LO, "MRN1");      // Z/D
    auto run = [&](const Options& opts) {
        DicomFile f = file_of(ds);
        CHECK(deidentify(f, opts).is_ok());
        return f;
    };
    Options o = make_options(0);
    std::vector<std::string> seen_classes;
    o.attribute_type = [&](std::string_view sop, std::span<const Tag> path, Tag tag) {
        seen_classes.emplace_back(sop);
        CHECK(path.empty());
        if (tag == 0x00080080) return 3;
        if (tag == 0x00080022) return 2;
        if (tag == tags::PatientID) return 1;
        return 0;
    };
    DicomFile a = run(o);
    CHECK(!a.dataset.contains(0x00080080));
    CHECK(a.dataset.contains(0x00080022) && value_of(a.dataset, 0x00080022).empty());
    CHECK(!value_of(a.dataset, tags::PatientID).empty());
    CHECK(!seen_classes.empty() && seen_classes[0] == "1.2.840.10008.5.1.4.1.1.2");
    Options r = make_options(0);
    r.combined = CombinedPolicy::Remove;
    DicomFile b = run(r);
    CHECK(!b.dataset.contains(0x00080080));
    CHECK(!b.dataset.contains(0x00080022));
    CHECK(b.dataset.contains(tags::PatientID) && value_of(b.dataset, tags::PatientID).empty());  // Z/D -> Z
}

TEST_CASE("deid: clean action keeps only safe words; structured content is cleaned") {
    Dataset ds;
    (void)ds.set_string(0x00081030, VR::LO, "CT chest abdomen pelvis - 55F Dr. Smith");
    // SR content: free text inside a Content Sequence, a standard code and a local code.
    Dataset code;
    (void)code.set_string(0x00080100, VR::SH, "121071");
    (void)code.set_string(0x00080102, VR::SH, "DCM");
    (void)code.set_string(0x00080104, VR::LO, "Finding");
    Dataset local = code;
    (void)local.set_string(0x00080102, VR::SH, "99LOCAL");
    (void)local.set_string(0x00080104, VR::LO, "PHILOCALMEANING");
    Dataset item;
    Element cn; cn.tag = 0x0040A043; cn.vr = VR::SQ; cn.items.push_back(code);
    item.put(cn);
    Element cc; cc.tag = 0x0040A168; cc.vr = VR::SQ; cc.items.push_back(local);
    item.put(cc);
    (void)item.set_string(0x0040A160, VR::UT, "PHITEXT patient John");
    (void)item.set_string(0x0040A040, VR::CS, "TEXT");
    Element content; content.tag = 0x0040A730; content.vr = VR::SQ; content.items.push_back(item);
    ds.put(content);
    Options o = make_options(CleanDescriptors | CleanStructuredContent);
    o.safe_words = {"CT", "chest", "abdomen", "pelvis"};
    DicomFile f = file_of(ds);
    REQUIRE(deidentify(f, o).is_ok());
    CHECK_EQ(value_of(f.dataset, 0x00081030), "CT chest abdomen pelvis");
    const Element* cs = f.dataset.find(0x0040A730);
    REQUIRE(cs && cs->items.size() == 1);
    const Dataset& it = cs->items[0];
    CHECK_EQ(value_of(it, 0x0040A160), "CLEANED");
    CHECK_EQ(value_of(it, 0x0040A040), "TEXT");
    CHECK_EQ(value_of(it.find(0x0040A043)->items[0], 0x00080104), "Finding");
    CHECK_EQ(value_of(it.find(0x0040A168)->items[0], 0x00080104), "CLEANED");
    // Without the option the Basic Profile replaces Content Sequence with a dummy (D).
    DicomFile g = file_of(ds);
    REQUIRE(deidentify(g, make_options(0)).is_ok());
    const Element* gs = g.dataset.find(0x0040A730);
    REQUIRE(gs && gs->items.size() == 1);
    CHECK(gs->items[0].empty());
    CHECK(!g.dataset.contains(0x00081030));
}

TEST_CASE("deid: sequences stored as UN are parsed and walked") {
    // Referenced Image Sequence (X/Z/U*) encoded as UN with defined length, implicit VR LE.
    std::vector<std::uint8_t> v;
    auto u16 = [&](std::uint16_t x) { v.push_back(static_cast<std::uint8_t>(x)); v.push_back(static_cast<std::uint8_t>(x >> 8)); };
    auto u32 = [&](std::uint32_t x) { u16(static_cast<std::uint16_t>(x)); u16(static_cast<std::uint16_t>(x >> 16)); };
    const std::string uid = "1.2.999.70";  // even length
    u16(0xFFFE); u16(0xE000); u32(static_cast<std::uint32_t>(8 + uid.size()));
    u16(0x0008); u16(0x1155); u32(static_cast<std::uint32_t>(uid.size()));
    v.insert(v.end(), uid.begin(), uid.end());
    Element e;
    e.tag = 0x00081140;
    e.vr = VR::UN;
    e.value = Bytes::copy_of(v.data(), v.size());
    Dataset ds;
    ds.put(e);
    Element bad = e;
    bad.tag = 0x00082112;
    std::vector<std::uint8_t> junk = {1, 2, 3, 4};
    bad.value = Bytes::copy_of(junk.data(), junk.size());
    ds.put(bad);
    DicomFile f = file_of(ds);
    REQUIRE(deidentify(f, make_options(0)).is_ok());
    const Element* out = f.dataset.find(0x00081140);
    REQUIRE(out && out->vr == VR::SQ && out->items.size() == 1);
    CHECK_EQ(value_of(out->items[0], 0x00081155), replacement_uid(kKey, uid));
    CHECK(!f.dataset.contains(0x00082112));  // unparseable: removed
}

TEST_CASE("deid: argument checks and DICOMDIR refusal") {
    Dataset ds;
    DicomFile f = file_of(ds);
    std::vector<std::uint8_t> short_key(15, 1);
    CHECK(deidentify(f, make_options(0, short_key)).code == Errc::InvalidArgument);
    CHECK(deidentify(f, make_options(1u << 20)).code == Errc::InvalidArgument);
    Options o = make_options(0);
    o.max_shift_days = 0;
    CHECK(deidentify(f, o).code == Errc::InvalidArgument);
    DicomFile dir;
    (void)dir.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, "1.2.840.10008.1.3.10");
    CHECK(deidentify(dir, make_options(0)).code == Errc::Unsupported);
}

TEST_CASE("deid: C API") {
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_create(&file) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(file, &ds) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(ds, tags::PatientName, "PN", "Doe^John", 8) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(ds, tags::SOPInstanceUID, "UI", "1.2.999.80", 10) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(ds, 0x00081030, "LO", "CT head Smith", 13) == SIAARA_OK);
    std::vector<std::uint16_t> px(4, 7);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::Rows, 2) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::Columns, 2) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::SamplesPerPixel, 1) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(ds, tags::PhotometricInterpretation, "CS", "MONOCHROME2", 11) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::BitsAllocated, 16) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::BitsStored, 16) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::HighBit, 15) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_uint16(ds, tags::PixelRepresentation, 0) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_bytes(ds, tags::PixelData, "OW", reinterpret_cast<const uint8_t*>(px.data()), 8) ==
            SIAARA_OK);

    siaara_deid_options o{};
    o.struct_size = 8;  // too small
    uint32_t flags = 99;
    CHECK(siaara_deidentify(file, &o, &flags) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(flags, 0u);
    o.struct_size = sizeof o;
    o.flags = SIAARA_DEID_CLEAN_DESCRIPTORS;
    o.key = kKey.data();
    o.key_len = kKey.size();
    o.safe_words = "ct head";
    o.safe_words_len = 7;
    CHECK(siaara_deidentify(file, &o, &flags) == SIAARA_OK);
    CHECK(flags & SIAARA_DEID_BURNED_IN_ANNOTATION_REMAINS);
    CHECK(flags & SIAARA_DEID_NOT_FULLY_DEIDENTIFIED);
    char buf[128];
    size_t need = 0;
    CHECK(siaara_dataset_get_string(ds, 0x00081030, 0, buf, sizeof buf, &need) == SIAARA_OK);
    CHECK_EQ(std::string(buf), "CT head");
    CHECK(siaara_dataset_get_string(ds, tags::SOPInstanceUID, 0, buf, sizeof buf, &need) == SIAARA_OK);
    char expect[128];
    CHECK(siaara_deid_replacement_uid(kKey.data(), kKey.size(), "1.2.999.80", 10, expect, sizeof expect, &need) ==
          SIAARA_OK);
    CHECK_EQ(std::string(buf), std::string(expect));
    CHECK(siaara_deid_replacement_uid(kKey.data(), 4, "1", 1, expect, sizeof expect, &need) ==
          SIAARA_ERR_INVALID_ARGUMENT);

    // Second run: blank the image and assert it clean.
    siaara_deid_region region{SIAARA_DEID_ALL_FRAMES, 0, 0, 2, 1};
    o.regions = &region;
    o.region_count = 1;
    o.pixel_data_clean = 1;
    CHECK(siaara_deidentify(file, &o, &flags) == SIAARA_OK);
    CHECK(flags & SIAARA_DEID_PIXEL_DATA_CLEANED);
    CHECK(!(flags & SIAARA_DEID_NOT_FULLY_DEIDENTIFIED));
    const uint8_t* raw = nullptr;
    size_t raw_size = 0;
    CHECK(siaara_dataset_get_raw(ds, tags::PixelData, &raw, &raw_size) == SIAARA_OK);
    CHECK(raw_size == 8 && load_le16(raw) == 0 && load_le16(raw + 4) == 7);
    o.combined_policy = 7;
    CHECK(siaara_deidentify(file, &o, &flags) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file_free(file);
}
