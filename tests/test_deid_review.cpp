// Regression tests for the de-identification findings of the 2026-11-09 security review:
// R-028 (per-purpose keys), R-044 (embedded overlays), R-045 (issuer of the patient ID),
// R-046 (cleaning ISO 2022 text), R-047 (table lookups), R-048 (visual features hook).
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "deid/deid.hpp"
#include "dict/dictionary.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::deid;

namespace {

std::vector<std::uint8_t> key_of(std::string_view s) { return {s.begin(), s.end()}; }
const std::vector<std::uint8_t> kKey = key_of("review-regression-key-0123456789");

Options opts(std::uint32_t bits = 0) {
    Options o;
    o.options = bits;
    o.key = kKey;
    return o;
}

std::string value_of(const Dataset& ds, Tag tag) {
    const Element* e = ds.find(tag);
    if (!e) return {};
    return std::string(trim_value(e->value.as_chars(), e->vr));
}

DicomFile file_with(Dataset ds) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    f.dataset = std::move(ds);
    return f;
}

Dataset patient(std::string_view id, std::string_view issuer) {
    Dataset ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "1.2.999.77.1");
    (void)ds.set_string(tags::PatientID, VR::LO, id);
    if (!issuer.empty()) (void)ds.set_string(0x00100021, VR::LO, issuer);
    (void)ds.set_string(0x00080020, VR::DA, "20200301");  // Study Date
    return ds;
}

// 16-bit image with 12 bits stored; pixels 0x0811 (bit 11 set inside the stored bits).
DicomFile image16(bool encapsulated = false) {
    Dataset ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "1.2.999.77.2");
    (void)ds.set_uint16(tags::Rows, 1);
    (void)ds.set_uint16(tags::Columns, 2);
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_uint16(tags::BitsStored, 12);
    (void)ds.set_uint16(tags::HighBit, 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    (void)ds.set_string(0x00280301, VR::CS, "NO");  // Burned In Annotation: only the overlay matters
    const std::uint8_t px[4] = {0x11, 0x88, 0x11, 0x88};  // bit 15 (overlay) and bit 11 (image)
    if (encapsulated) {
        Element e;
        e.tag = tags::PixelData;
        e.vr = VR::OB;
        e.undefined_length = true;
        e.fragments.push_back(Bytes());
        e.fragments.push_back(Bytes::copy_of(px, 4));
        ds.put(std::move(e));
    } else {
        (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px, 4));
    }
    return file_with(std::move(ds));
}

void add_embedded_overlay(Dataset& ds, std::uint16_t group, std::uint16_t bit) {
    (void)ds.set_uint16(static_cast<Tag>(group) << 16 | 0x0010, 1);
    (void)ds.set_uint16(static_cast<Tag>(group) << 16 | 0x0011, 2);
    (void)ds.set_uint16(static_cast<Tag>(group) << 16 | 0x0100, 16);
    (void)ds.set_uint16(static_cast<Tag>(group) << 16 | 0x0102, bit);
}

bool has_group(const Dataset& ds, std::uint16_t g) {
    for (const Element& e : ds.elements())
        if (tag_group(e.tag) == g) return true;
    return false;
}

// "2.25.<decimal>" back to the 16 bytes.
std::array<std::uint8_t, 16> uid_bytes(const std::string& uid) {
    std::array<std::uint8_t, 16> n{};
    for (char c : uid.substr(5)) {
        unsigned carry = static_cast<unsigned>(c - '0');
        for (int i = 15; i >= 0; --i) {
            const unsigned v = n[static_cast<std::size_t>(i)] * 10u + carry;
            n[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v & 0xFFu);
            carry = v >> 8;
        }
    }
    return n;
}

}  // namespace

// R-028: a crafted UI value equal to the date-offset MAC input used to come back as a UID made
// from the same MAC, which gives away the patient's date offset (the UUID version bits hide
// only 4 of its 64 bits). With per-purpose sub-keys the two MACs are unrelated.
TEST_CASE("deid review/R-028 a crafted UID does not reveal the date offset") {
    const std::string pid = "PATIENT-28";
    const std::int32_t offset = date_offset_days(kKey, pid);
    // The old date-offset MAC input: "SIAARA-DEID-DATE-OFFSET\0" + Patient ID.
    const std::string crafted = std::string("SIAARA-DEID-DATE-OFFSET", 23) + '\0' + pid;
    const std::string uid = replacement_uid(kKey, crafted);
    const std::array<std::uint8_t, 16> b = uid_bytes(uid);
    int matches = 0;
    for (unsigned nib = 0; nib < 16; ++nib) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            std::uint8_t x = b[static_cast<std::size_t>(i)];
            if (i == 6) x = static_cast<std::uint8_t>((x & 0x0Fu) | (nib << 4));
            v = (v << 8) | x;
        }
        if (-static_cast<std::int32_t>(1u + static_cast<std::uint32_t>(v % 3652u)) == offset) ++matches;
    }
    std::printf("  offset %d, candidates from the crafted UID matching it: %d\n", offset, matches);
    CHECK_EQ(matches, 0);
    // The same through deidentify(): an unlisted UI attribute holding the crafted value.
    Dataset ds = patient(pid, "");
    Element e;
    e.tag = 0x0040A124;  // UID (Table E.1-1: U)
    e.vr = VR::UI;
    std::string v = crafted;
    if (v.size() & 1u) v.push_back('\0');
    e.value = Bytes::from_string(v);
    ds.put(std::move(e));
    DicomFile f = file_with(std::move(ds));
    REQUIRE(deidentify(f, opts(RetainLongitudinalModifiedDates)).is_ok());
    CHECK(value_of(f.dataset, 0x0040A124) == uid);
}

// R-045: the date offset and the Patient ID pseudonym depend on the Issuer of Patient ID too:
// the same ID from two issuers is two patients.
TEST_CASE("deid review/R-045 issuer of patient ID separates patients") {
    DicomFile a = file_with(patient("12345", "HOSPITAL-A"));
    DicomFile b = file_with(patient("12345", "HOSPITAL-B"));
    DicomFile a2 = file_with(patient("12345", "HOSPITAL-A"));
    const Options o = opts(RetainLongitudinalModifiedDates);
    REQUIRE(deidentify(a, o).is_ok());
    REQUIRE(deidentify(b, o).is_ok());
    REQUIRE(deidentify(a2, o).is_ok());
    std::printf("  shifted study dates: A %s, B %s\n", value_of(a.dataset, 0x00080020).c_str(),
                value_of(b.dataset, 0x00080020).c_str());
    CHECK(value_of(a.dataset, 0x00080020) != value_of(b.dataset, 0x00080020));
    // Same patient, same issuer: consistent.
    CHECK_EQ(value_of(a.dataset, 0x00080020), value_of(a2.dataset, 0x00080020));
}

// R-044: an embedded overlay whose bits cannot be cleared (encapsulated pixel data, or an
// overlay bit inside the stored image bits) is not hidden: its group stays and the report says
// the instance is not fully de-identified. Several groups are cleared in one pass.
TEST_CASE("deid review/R-044 embedded overlays that cannot be cleared are reported") {
    {
        DicomFile f = image16(true);
        add_embedded_overlay(f.dataset, 0x6000, 15);
        Report r;
        REQUIRE(deidentify(f, opts(), &r).is_ok());
        CHECK(r.flags & NotFullyDeidentified);
        CHECK(has_group(f.dataset, 0x6000));
    }
    {
        // Bit 11 is an image bit (BitsStored 12, HighBit 11): clearing it would damage the image.
        DicomFile f = image16(false);
        add_embedded_overlay(f.dataset, 0x6000, 11);
        Report r;
        REQUIRE(deidentify(f, opts(), &r).is_ok());
        const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
        CHECK(p[1] == 0x88 && p[3] == 0x88);  // pixel data untouched
        CHECK(r.flags & NotFullyDeidentified);
    }
    {
        // Two overlays in unused bits 12 and 15: both cleared, both groups removed, image bits kept.
        DicomFile f = image16(false);
        std::uint8_t px[4] = {0x11, 0x98, 0x11, 0x98};  // bits 15, 12 and 11
        (void)f.dataset.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px, 4));
        add_embedded_overlay(f.dataset, 0x6000, 15);
        add_embedded_overlay(f.dataset, 0x6002, 12);
        Report r;
        REQUIRE(deidentify(f, opts(), &r).is_ok());
        const std::uint8_t* p = f.dataset.find(tags::PixelData)->value.data();
        CHECK(p[0] == 0x11 && p[1] == 0x08 && p[3] == 0x08);
        CHECK(!has_group(f.dataset, 0x6000) && !has_group(f.dataset, 0x6002));
        CHECK(!(r.flags & NotFullyDeidentified));
    }
}

// R-046: Clean Descriptors on ISO 2022 IR 87 text: the JIS X 0208 bytes of a kanji (0x41 0x42
// reads "AB") must not be mistaken for an ASCII safe word.
TEST_CASE("deid review/R-046 cleaning ISO 2022 multi-byte text works on characters") {
    Dataset ds = patient("P46", "");
    (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "\\ISO 2022 IR 87");
    // ESC $ B, 0x30 0x21 (a kanji whose bytes read "0!"), 0x41 0x42 (one reading "AB"), ESC ( B.
    const std::string jis = std::string("\x1b$B0!AB\x1b(B", 10) + " CT";
    (void)ds.set_string(0x00081030, VR::LO, jis);  // Study Description
    DicomFile f = file_with(std::move(ds));
    Options o = opts(CleanDescriptors);
    o.safe_words = {"AB", "CT"};
    REQUIRE(deidentify(f, o).is_ok());
    const std::string out = value_of(f.dataset, 0x00081030);
    std::printf("  cleaned: %s\n", out.c_str());
    CHECK_EQ(out, "CT");
}

// R-047: rows with repeating-group masks are found through an index, not by scanning every row
// of Table E.1-1 for each attribute that has no row of its own.
TEST_CASE("deid review/R-047 table lookups of unlisted tags are fast and exact") {
    const auto rows = profile_rows();
    // Exactness against the plain definition on a spread of tags.
    int checked = 0;
    for (std::uint32_t g = 0x0008; g <= 0x7FE0; g += 0x0102) {
        for (std::uint32_t el : {0x0010u, 0x1001u, 0x3000u, 0x4000u, 0x0102u}) {
            const Tag tag = (g << 16) | el;
            const ProfileRow* want = nullptr;
            for (const ProfileRow& r : rows)
                if (r.mask == 0xFFFFFFFFu && r.tag == tag) want = &r;
            if (!want)
                for (const ProfileRow& r : rows)
                    if (r.mask != 0xFFFFFFFFu && (tag & r.mask) == r.tag) {
                        want = &r;
                        break;
                    }
            CHECK(find_row(tag) == want);
            ++checked;
        }
    }
    CHECK(find_row(0x60123000) != nullptr);
    CHECK(find_row(0x50103000) != nullptr);
    // Speed: unlisted tags (no exact row) used to scan all rows each time. Compared with that
    // scan done here, per lookup, so the bound holds for any build type.
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::size_t found = 0;
    constexpr std::uint32_t kFast = 400000, kSlow = 20000;
    for (std::uint32_t i = 0; i < kFast; ++i) found += find_row(0x00191000u + (i & 0xFFFu)) != nullptr;
    const double fast = std::chrono::duration<double>(Clock::now() - t0).count() / kFast;
    t0 = Clock::now();
    std::size_t slow_found = 0;
    for (std::uint32_t i = 0; i < kSlow; ++i) {
        const Tag tag = 0x00191000u + (i & 0xFFFu);
        for (const ProfileRow& r : rows)
            if (r.mask != 0xFFFFFFFFu && (tag & r.mask) == r.tag) {
                ++slow_found;
                break;
            }
    }
    const double slow = std::chrono::duration<double>(Clock::now() - t0).count() / kSlow;
    std::printf("  %d exact checks; unlisted lookup %.3f us, full-table scan %.3f us\n", checked, fast * 1e6, slow * 1e6);
    CHECK_EQ(found, 0u);
    CHECK_EQ(slow_found, 0u);
    CHECK(fast * 5 < slow);
}

// R-028 / R-045: report flags for UI values outside [0-9.] and for a missing Patient ID; the
// public helpers give what deidentify() uses.
TEST_CASE("deid review/R-028 R-045 flags and helper consistency") {
    {
        Dataset ds = patient("PAT-X", "ISSUER-X");
        (void)ds.set_string(0x0040A124, VR::UI, "1.2.3.ABC");
        DicomFile f = file_with(std::move(ds));
        Report r;
        REQUIRE(deidentify(f, opts(RetainLongitudinalModifiedDates), &r).is_ok());
        CHECK(r.flags & InvalidUidValues);
        CHECK(!(r.flags & EmptyPatientId));
        CHECK_EQ(value_of(f.dataset, 0x0040A124), replacement_uid(kKey, "1.2.3.ABC"));
        // The shifted date is the helper's offset for (Patient ID, Issuer of Patient ID).
        std::string expect;
        REQUIRE(shift_date("20200301", VR::DA, date_offset_days(kKey, "PAT-X", "ISSUER-X"), expect));
        CHECK_EQ(value_of(f.dataset, 0x00080020), expect);
        CHECK(date_offset_days(kKey, "PAT-X", "ISSUER-X") != date_offset_days(kKey, "PAT-X"));
    }
    {
        DicomFile f = file_with(patient("", ""));
        Report r;
        REQUIRE(deidentify(f, opts(), &r).is_ok());
        CHECK(r.flags & EmptyPatientId);
        CHECK(!(r.flags & InvalidUidValues));
    }
}

// R-048: the Clean Recognizable Visual Features hook.
TEST_CASE("deid review/R-048 clean recognizable visual features hook") {
    auto with_features = [] {
        DicomFile f = image16(false);
        (void)f.dataset.set_string(0x00280302, VR::CS, "YES");
        return f;
    };
    auto has_code = [](const Dataset& ds, std::string_view code) {
        const Element* seq = ds.find(0x00120064);
        if (!seq) return false;
        for (const Dataset& item : seq->items)
            if (value_of(item, 0x00080100) == code) return true;
        return false;
    };
    {
        // Without the hook: unsupported, reported.
        DicomFile f = with_features();
        Report r;
        REQUIRE(deidentify(f, opts(), &r).is_ok());
        CHECK(r.flags & RecognizableFeaturesRemain);
        CHECK(!has_code(f.dataset, "113102"));
    }
    {
        DicomFile f = with_features();
        int calls = 0;
        Options o = opts();
        o.clean_recognizable_visual_features = [&](Dataset& ds) {
            ++calls;
            CHECK(ds.contains(tags::PixelData));
            return Status::ok();
        };
        Report r;
        REQUIRE(deidentify(f, o, &r).is_ok());
        CHECK_EQ(calls, 1);
        CHECK(!(r.flags & RecognizableFeaturesRemain));
        CHECK(!(r.flags & NotFullyDeidentified));
        CHECK(has_code(f.dataset, "113102"));
        CHECK_EQ(value_of(f.dataset, 0x00280302), "NO");
    }
    {
        // A failing hook fails de-identification and leaves the file unchanged.
        DicomFile f = with_features();
        const std::string before = value_of(f.dataset, tags::SOPInstanceUID);
        Options o = opts();
        o.clean_recognizable_visual_features = [](Dataset&) { return Status(Errc::Unsupported, "no defacer"); };
        CHECK_EQ(deidentify(f, o).code, Errc::Unsupported);
        CHECK_EQ(value_of(f.dataset, tags::SOPInstanceUID), before);
        CHECK_EQ(value_of(f.dataset, 0x00280302), "YES");
    }
}
