// Local store tests: index extraction, add/dedupe/replace, every query filter, ordering,
// delete/move/verify/rebuild, crash recovery at every journal byte, lock file, C API.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "core/reader.hpp"
#include "core/types.hpp"
#include "crosscheck/store_synth.hpp"
#include "siaara/siaara.h"
#include "store/common.hpp"
#include "store/index.hpp"
#include "store/store.hpp"
#include "store/text.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::store;
using store_synth::Spec;
using store_synth::TempDir;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Store> open_store(const fs::path& root, StoreOptions opt = {}) {
    auto r = Store::open(root.string(), opt);
    if (!r) {
        std::fprintf(stderr, "  open failed: %s\n", r.error().message.c_str());
        return nullptr;
    }
    return std::move(r).value();
}

Spec spec(int patient, int study, int series, int inst) {
    Spec s;
    s.patient_id = "P" + std::to_string(patient);
    s.patient_name = "Patient" + std::to_string(patient) + "^Test";
    s.study_uid = "1.2.826.0.1." + std::to_string(patient) + "." + std::to_string(study);
    s.series_uid = s.study_uid + "." + std::to_string(series);
    s.sop_uid = s.series_uid + "." + std::to_string(inst);
    s.series_number = series;
    s.instance_number = inst;
    s.z = inst * 2.5;
    s.accession = "A" + std::to_string(patient) + std::to_string(study);
    return s;
}

std::string add_spec(Store& st, const fs::path& dir, const Spec& s, DuplicatePolicy pol = DuplicatePolicy::Keep,
                     AddOutcome expect = AddOutcome::Added) {
    const fs::path p = dir / (std::to_string(std::hash<std::string>{}(s.sop_uid)) + ".dcm");
    store_synth::write_spec(p, s);
    auto r = st.add_file(p.string(), pol);
    CHECK(r.is_ok());
    if (r) CHECK(*r == expect);
    return p.string();
}

std::size_t count(const Store& st, const QueryFilter& f) {
    auto r = st.query(f);
    CHECK(r.is_ok());
    return r ? r->rows.size() : 9999;
}

QueryFilter at(Level l) {
    QueryFilter f;
    f.level = l;
    return f;
}

}  // namespace

// ================================================================ text folding and matching
TEST_CASE("store: fold strips Latin accents and case-folds") {
    CHECK_EQ(fold("M\xC3\xBCller"), "muller");                        // Müller
    CHECK_EQ(fold("\xC5\x81\xC3\x93\x44\xC5\xB9"), "lodz");            // ŁÓDŹ
    CHECK_EQ(fold("Stra\xC3\x9F" "e"), "strasse");                     // Straße
    CHECK_EQ(fold("Nguy\xE1\xBB\x85n"), "nguyen");                     // Nguyễn
    CHECK_EQ(fold("\xE1\xB9\xA2" "ankar"), "sankar");                  // Ṣankar
    CHECK_EQ(fold("\xC3\x86sir"), "aesir");                            // Æsir
    CHECK_EQ(fold("Ren\x65\xCC\x81"), "rene");                         // René (combining acute)
    CHECK_EQ(fold("\xD0\x81\xD0\x96"), "\xD0\xB5\xD0\xB6");            // ЁЖ -> еж
    CHECK_EQ(fold("\xCE\x86\xCE\xA3"), "\xCE\xB1\xCF\x83");            // ΆΣ -> ασ
    CHECK_EQ(fold("\xEF\xBC\xA1\xEF\xBD\x82"), "ab");                  // full-width Ab
    CHECK_EQ(fold("  a \t  b  "), "a b");
    CHECK_EQ(fold("\xE5\xB1\xB1\xE7\x94\xB0"), "\xE5\xB1\xB1\xE7\x94\xB0");  // 山田 unchanged
    CHECK_EQ(fold("\xFF" "a"), "\xEF\xBF\xBD" "a");                    // malformed -> U+FFFD
}

TEST_CASE("store: name forms split component groups") {
    const auto f = name_forms("Yamada^Tarou=\xE5\xB1\xB1\xE7\x94\xB0^\xE5\xA4\xAA\xE9\x83\x8E=\xE3\x82\x84\xE3\x81\xBE\xE3\x81\xA0^\xE3\x81\x9F\xE3\x82\x8D\xE3\x81\x86");
    CHECK_EQ(f[0], "yamada tarou");
    CHECK_EQ(f[1], "\xE5\xB1\xB1\xE7\x94\xB0 \xE5\xA4\xAA\xE9\x83\x8E");
    CHECK_EQ(f[2], "\xE3\x82\x84\xE3\x81\xBE\xE3\x81\xA0 \xE3\x81\x9F\xE3\x82\x8D\xE3\x81\x86");
    const auto g = name_forms("Doe^^^^");
    CHECK_EQ(g[0], "doe");
    CHECK(g[1].empty() && g[2].empty());
}

TEST_CASE("store: wildcard matching and date ranges") {
    CHECK(wildcard_match("doe*", "doe jane"));
    CHECK(wildcard_match("*jane", "doe jane"));
    CHECK(wildcard_match("d?e*", "doe"));
    CHECK(!wildcard_match("d?e", "de"));
    CHECK(wildcard_match("*", ""));
    CHECK(wildcard_match("a*b*c", "axxbyyc"));
    CHECK(!wildcard_match("a*b*c", "axxbyy"));
    CHECK(wildcard_match("?", "\xE5\xB1\xB1"));  // '?' takes a whole UTF-8 character
    CHECK(!wildcard_match("??", "\xE5\xB1\xB1"));
    CHECK(wildcard_match("\xE5\xB1\xB1*", "\xE5\xB1\xB1\xE7\x94\xB0"));
    std::uint32_t lo = 0, hi = 0;
    CHECK(parse_date_range("20200101-20201231", lo, hi) && lo == 20200101u && hi == 20201231u);
    CHECK(parse_date_range("-20191231", lo, hi) && lo == 0u && hi == 20191231u);
    CHECK(parse_date_range("20210101-", lo, hi) && lo == 20210101u && hi == 99999999u);
    CHECK(parse_date_range("20200105", lo, hi) && lo == 20200105u && hi == 20200105u);
    CHECK(!parse_date_range("2020-01-01", lo, hi));
    CHECK(!parse_date_range("20201231-20200101", lo, hi));
    CHECK(!parse_date_range("-", lo, hi));
    CHECK_EQ(date_key("2020.01.15"), 20200115u);
    CHECK_EQ(date_key("garbage!"), 0u);
}

TEST_CASE("store: SHA-256 and CRC-32 known answers") {
    const auto h = sha256("abc", 3);
    CHECK_EQ(to_hex(h.data(), 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto e = sha256("", 0);
    CHECK_EQ(to_hex(e.data(), 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string m(1000, 'a');  // multi-block
    const auto k = sha256(m.data(), m.size());
    CHECK_EQ(to_hex(k.data(), 32), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    const char* nine = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const std::uint8_t*>(nine), 9), 0xCBF43926u);
    const std::string uid = generate_uid();
    CHECK(uid.rfind("2.25.", 0) == 0 && uid.size() <= 64 && uid.size() > 10);
    CHECK(generate_uid() != uid);
}

// ================================================================ extraction
TEST_CASE("store: extract_index reads every key, the pixel data offset and UTF-8 names") {
    Spec s;
    s.charset = "ISO_IR 100";
    s.patient_name = "M\xC3\xBCller^J\xC3\xBCrgen";
    s.study_desc = "Th\xC3\xB6rax";
    const auto bytes = store_synth::make_bytes(s);
    IndexRecord r;
    const Bytes b = Bytes::copy_of(bytes.data(), bytes.size());
    REQUIRE(extract_index(b, r).is_ok());
    CHECK_EQ(r[Str::PatientName], s.patient_name);
    CHECK_EQ(r[Str::StudyDescription], s.study_desc);
    CHECK_EQ(r[Str::PatientId], "PID1");
    CHECK_EQ(r[Str::PatientBirthDate], "19700101");
    CHECK_EQ(r[Str::PatientSex], "F");
    CHECK_EQ(r[Str::StudyInstanceUid], s.study_uid);
    CHECK_EQ(r[Str::SeriesInstanceUid], s.series_uid);
    CHECK_EQ(r[Str::SopInstanceUid], s.sop_uid);
    CHECK_EQ(r[Str::SopClassUid], s.sop_class);
    CHECK_EQ(r[Str::Modality], "CT");
    CHECK_EQ(r[Str::BodyPartExamined], "CHEST");
    CHECK_EQ(r[Str::AccessionNumber], "ACC1");
    CHECK_EQ(r[Str::ReferringPhysician], "Ref^Doc");
    CHECK_EQ(r[Str::StudyDate], "20200115");
    CHECK_EQ(r[Str::StudyTime], "101500");
    CHECK_EQ(r[Str::ImageType], "ORIGINAL\\PRIMARY\\AXIAL");
    CHECK_EQ(r[Str::TransferSyntaxUid], std::string(ts::ExplicitVRLittleEndian));
    CHECK_EQ(r[Str::PhotometricInterpretation], "MONOCHROME2");
    CHECK_EQ(r[Str::SpecificCharacterSet], "ISO_IR 100");
    CHECK_EQ(r[Str::FrameOfReferenceUid], s.study_uid + ".9");
    CHECK(r.flags & kHasSeriesNumber);
    CHECK(r.flags & kHasInstanceNumber);
    CHECK(r.flags & kHadPreamble);
    CHECK_EQ(r.series_number, 1);
    CHECK_EQ(r.instance_number, 1);
    CHECK_EQ(r.rows, 4u);
    CHECK_EQ(r.columns, 4u);
    CHECK_EQ(r.bits_allocated, 16u);
    CHECK_EQ(r.bits_stored, 12u);
    CHECK_EQ(r.high_bit, 11u);
    CHECK_EQ(r.samples_per_pixel, 1u);
    CHECK_EQ(r.number_of_frames, 1u);
    CHECK(r.flags & kHasPosition);
    CHECK(r.flags & kHasOrientation);
    CHECK(r.position[0] == -10.0 && r.position[1] == -20.0 && r.position[2] == 0.0);
    CHECK(r.orientation[0] == 1.0 && r.orientation[4] == 1.0);
    CHECK((r.flags & kHasPixelSpacing) && r.pixel_spacing[0] == 0.5 && r.pixel_spacing[1] == 0.6);
    CHECK((r.flags & kHasSliceThickness) && r.slice_thickness == 1.5);
    CHECK_EQ(r.file_size, bytes.size());
    REQUIRE((r.flags & kPixelOffsetKnown) != 0);
    CHECK_EQ(r.pixel_data_length, 32u);
    REQUIRE(r.pixel_data_offset + 32 == bytes.size());
    CHECK_EQ(bytes[static_cast<std::size_t>(r.pixel_data_offset)], 0xA0);
    // explicit VR OW header: tag (4), "OW", 2 reserved bytes, 32-bit length
    CHECK_EQ(load_le32(bytes.data() + r.pixel_data_offset - 12), 0x00107FE0u);
    CHECK_EQ(load_le32(bytes.data() + r.pixel_data_offset - 4), 32u);
}

TEST_CASE("store: extract_index handles raw data sets, encapsulated pixel data and junk") {
    Spec s;
    s.preamble = false;
    s.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    const auto raw = store_synth::make_bytes(s);
    IndexRecord r;
    REQUIRE(extract_index(Bytes::copy_of(raw.data(), raw.size()), r).is_ok());
    CHECK_EQ(r[Str::SopInstanceUid], s.sop_uid);
    CHECK(!(r.flags & kHadPreamble));
    ExtractOptions strict;
    strict.allow_raw_dataset = false;
    CHECK(extract_index(Bytes::copy_of(raw.data(), raw.size()), r, strict).code == Errc::NotDicom);

    // encapsulated: empty offset table + two fragments
    DicomFile f = store_synth::make_file(Spec{});
    f.transfer_syntax = "1.2.840.10008.1.2.5";  // RLE Lossless (encapsulated)
    Element* pd = f.dataset.find(tags::PixelData);
    REQUIRE(pd != nullptr);
    pd->vr = VR::OB;
    pd->value = Bytes{};
    pd->undefined_length = true;
    const std::uint8_t frag[6] = {1, 2, 3, 4, 5, 6};
    pd->fragments = {Bytes::copy_of(frag, 0), Bytes::copy_of(frag, 6)};
    std::vector<std::uint8_t> enc;
    Status ws = write_part10(f, enc);
    REQUIRE(ws.is_ok());
    REQUIRE(extract_index(Bytes::copy_of(enc.data(), enc.size()), r).is_ok());
    CHECK(r.flags & kEncapsulated);
    REQUIRE(r.flags & kPixelOffsetKnown);
    CHECK_EQ(load_le32(enc.data() + r.pixel_data_offset), 0xE000FFFEu);  // first item tag
    CHECK_EQ(r.pixel_data_offset + r.pixel_data_length, enc.size());      // through the delimiter
    CHECK_EQ(load_le32(enc.data() + enc.size() - 8), 0xE0DDFFFEu);

    const std::uint8_t junk[200] = {'P', 'K', 3, 4};
    CHECK(extract_index(Bytes::copy_of(junk, sizeof junk), r).code == Errc::NotDicom);
    CHECK(extract_index(Bytes{}, r).code == Errc::NotDicom);
    CHECK(!looks_like_dicom(junk, sizeof junk, true));
}

// ================================================================ add / dedupe / replace
TEST_CASE("store: add, de-duplicate by SOP Instance UID, replace") {
    TempDir td("siaara-store");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    Spec a = spec(1, 1, 1, 1);
    add_spec(*st, td.path(), a);
    add_spec(*st, td.path(), a, DuplicatePolicy::Keep, AddOutcome::KeptExisting);
    a.study_desc = "Changed";
    add_spec(*st, td.path(), a, DuplicatePolicy::Keep, AddOutcome::KeptExisting);
    auto q = st->query(at(Level::Study));
    REQUIRE(q.is_ok() && q->rows.size() == 1);
    CHECK_EQ(q->rows[0][Str::StudyDescription], "CT Chest");
    add_spec(*st, td.path(), a, DuplicatePolicy::Replace, AddOutcome::Replaced);
    q = st->query(at(Level::Study));
    REQUIRE(q.is_ok() && q->rows.size() == 1);
    CHECK_EQ(q->rows[0][Str::StudyDescription], "Changed");
    CHECK_EQ(st->stats().instances, 1u);

    // file layout: hashed name, no UID or patient data in the path
    auto inst = st->instance(a.sop_uid);
    REQUIRE(inst.is_ok());
    const std::string path = (*inst)[Str::FilePath];
    CHECK(fs::exists(path));
    CHECK(path.find(a.sop_uid) == std::string::npos);
    CHECK(path.find("Patient") == std::string::npos);
    CHECK(path.find(instance_relative_path(a.sop_uid).substr(5)) != std::string::npos);
    CHECK_EQ(instance_relative_path(a.sop_uid).size(), std::string("data/ab/cd/").size() + 32 + 4);

    // replacing with an instance that moved to another series
    Spec moved = a;
    moved.series_uid = a.study_uid + ".77";
    add_spec(*st, td.path(), moved, DuplicatePolicy::Replace, AddOutcome::Replaced);
    CHECK_EQ(st->stats().series, 1u);
    CHECK(st->instances_of(a.series_uid)->empty());
    CHECK_EQ(st->instances_of(moved.series_uid)->size(), 1u);

    // DICOMDIRs and files without SOP Instance UID are refused
    Spec dd;
    dd.sop_class = std::string(kMediaStorageDirectoryUid);
    dd.sop_uid = "1.2.3.999";
    store_synth::write_spec(td / "dd.dcm", dd);
    CHECK(st->add_file((td / "dd.dcm").string()).error().code == Errc::Rejected);
    Spec nouid;
    nouid.sop_uid = "";
    store_synth::write_spec(td / "nouid.dcm", nouid);
    CHECK(st->add_file((td / "nouid.dcm").string()).error().code == Errc::Malformed);
    CHECK(st->add_file((td / "missing.dcm").string()).error().code == Errc::IoError);
}

TEST_CASE("store: add_buffer stores raw data sets as Part 10; reference mode keeps files in place") {
    TempDir td("siaara-store");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    Spec raw = spec(1, 1, 1, 1);
    raw.preamble = false;
    raw.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    const auto b = store_synth::make_bytes(raw);
    auto r = st->add_buffer(Bytes::copy_of(b.data(), b.size()));
    REQUIRE(r.is_ok());
    auto inst = st->instance(raw.sop_uid);
    REQUIRE(inst.is_ok());
    const auto stored = store_synth::read_bytes((*inst)[Str::FilePath]);
    CHECK(has_part10_header(stored.data(), stored.size()));
    CHECK((*inst).flags & kHadPreamble);
    CHECK_EQ((*inst).file_size, stored.size());

    Spec ref = spec(2, 1, 1, 1);
    const fs::path ext = td / "external" / "file.dcm";
    store_synth::write_spec(ext, ref);
    auto rr = st->add_file(ext.string(), DuplicatePolicy::Keep, AddMode::Reference);
    REQUIRE(rr.is_ok());
    auto ri = st->instance(ref.sop_uid);
    REQUIRE(ri.is_ok());
    CHECK((*ri).flags & kReferenced);
    CHECK(fs::equivalent((*ri)[Str::FilePath], ext));
    auto n = st->remove(Level::Instance, ref.sop_uid);
    CHECK(n.is_ok() && *n == 1);
    CHECK(fs::exists(ext));  // referenced files are never deleted
}

// ================================================================ queries
namespace {

struct QueryFixture {
    TempDir td{"siaara-query"};
    std::unique_ptr<Store> st;
    QueryFixture() {
        st = open_store(td / "s");
        if (!st) return;
        // patient 1: Müller^Jürgen (Latin-1), 2 studies
        Spec m = spec(1, 1, 1, 1);
        m.charset = "ISO_IR 100";
        m.patient_id = "MUL001";
        m.patient_name = "M\xC3\xBCller^J\xC3\xBCrgen";
        m.birth = "19601231";
        m.sex = "M";
        m.study_date = "20200310";
        m.study_desc = "CT THORAX mit KM";
        m.accession = "ACC-100";
        m.referring = "Schmidt^Anna";
        add_spec(*st, td.path(), m);
        m.sop_uid += ".2";
        m.instance_number = 2;
        add_spec(*st, td.path(), m);
        Spec m2 = m;
        m2.study_uid = "1.2.826.0.1.1.2";
        m2.series_uid = m2.study_uid + ".1";
        m2.sop_uid = m2.series_uid + ".1";
        m2.study_date = "20210620";
        m2.study_desc = "MR Knee";
        m2.modality = "MR";
        m2.series_desc = "T1 sag";
        m2.body_part = "KNEE";
        m2.accession = "ACC-200";
        add_spec(*st, td.path(), m2);
        // second series in study 2: SR
        Spec sr = m2;
        sr.series_uid = m2.study_uid + ".2";
        sr.sop_uid = sr.series_uid + ".1";
        sr.modality = "SR";
        sr.series_desc = "Report";
        sr.series_number = 99;
        sr.pixels = false;
        sr.sop_class = "1.2.840.10008.5.1.4.1.1.88.33";
        add_spec(*st, td.path(), sr);
        // patient 2: Yamada (UTF-8, three groups)
        Spec y = spec(2, 1, 1, 1);
        y.charset = "ISO_IR 192";
        y.patient_id = "JP0002";
        y.patient_name = "Yamada^Tarou=\xE5\xB1\xB1\xE7\x94\xB0^\xE5\xA4\xAA\xE9\x83\x8E=\xE3\x82\x84\xE3\x81\xBE\xE3\x81\xA0^\xE3\x81\x9F\xE3\x82\x8D\xE3\x81\x86";
        y.birth = "19851105";
        y.sex = "M";
        y.study_date = "20191201";
        y.study_desc = "Chest X-ray";
        y.modality = "CR";
        y.body_part = "CHEST";
        y.accession = "JPACC9";
        add_spec(*st, td.path(), y);
        // patient 3: no patient ID (keyed by name + birth date)
        Spec n = spec(3, 1, 1, 1);
        n.patient_id = "";
        n.patient_name = "O'Brien^Siobh\xC3\xA1n";
        n.charset = "ISO_IR 192";
        n.birth = "20000229";
        n.study_date = "";
        n.study_desc = "";
        n.accession = "";
        add_spec(*st, td.path(), n);
    }
};

}  // namespace

TEST_CASE("store: query filters on every key") {
    QueryFixture fx;
    REQUIRE(fx.st != nullptr);
    Store& st = *fx.st;
    CHECK_EQ(count(st, at(Level::Patient)), 3u);
    CHECK_EQ(count(st, at(Level::Study)), 4u);
    CHECK_EQ(count(st, at(Level::Series)), 5u);
    CHECK_EQ(count(st, at(Level::Instance)), 6u);

    auto q = at(Level::Patient);
    q.patient_name = "muller*";
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "M\xC3\x9CLLER^J\xC3\x9CRGEN";  // MÜLLER^JÜRGEN exact, other case and accents
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "*jurgen";
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "mueller*";
    CHECK_EQ(count(st, q), 0u);
    q.patient_name = "yamada*";
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "\xE5\xB1\xB1\xE7\x94\xB0*";  // ideographic group
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "*\xE3\x81\x9F\xE3\x82\x8D\xE3\x81\x86";  // phonetic group, leading wildcard
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "o'brien siobhan";
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "?'brien*";
    CHECK_EQ(count(st, q), 1u);
    q.patient_name = "*";
    CHECK_EQ(count(st, q), 3u);

    q = at(Level::Patient);
    q.patient_id = "MUL001";
    CHECK_EQ(count(st, q), 1u);
    q.patient_id = "mul001";  // IDs are case-sensitive
    CHECK_EQ(count(st, q), 0u);
    q.patient_id = "*0002";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Patient);
    q.patient_birth_date = "19600101-19861231";
    CHECK_EQ(count(st, q), 2u);
    q.patient_birth_date = "20000229";
    CHECK_EQ(count(st, q), 1u);
    q.patient_birth_date = "bad";
    CHECK(st.query(q).error().code == Errc::InvalidArgument);
    q = at(Level::Patient);
    q.patient_sex = "M";
    CHECK_EQ(count(st, q), 2u);

    q = at(Level::Study);
    q.study_date = "20200101-20201231";
    CHECK_EQ(count(st, q), 1u);
    q.study_date = "-20200310";
    CHECK_EQ(count(st, q), 2u);
    q.study_date = "20200311-";
    CHECK_EQ(count(st, q), 1u);
    q.study_date = "20191201";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Study);
    q.accession_number = "ACC-200";
    CHECK_EQ(count(st, q), 1u);
    q.accession_number = "ACC-*";
    CHECK_EQ(count(st, q), 2u);
    q = at(Level::Study);
    q.study_description = "*thorax*";
    CHECK_EQ(count(st, q), 1u);
    q.study_description = "chest*";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Study);
    q.referring_physician = "schmidt*";
    CHECK_EQ(count(st, q), 2u);
    q = at(Level::Study);
    q.modality = "MR";
    CHECK_EQ(count(st, q), 1u);
    q.modality = "CT\\CR";
    CHECK_EQ(count(st, q), 3u);  // patient 3 has CT too
    q.modality = "S?";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Series);
    q.series_description = "t1*";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Series);
    q.body_part = "CHEST";
    CHECK_EQ(count(st, q), 3u);
    q = at(Level::Instance);
    q.study_uid = "1.2.826.0.1.1.2";
    CHECK_EQ(count(st, q), 2u);
    q.series_uid = "1.2.826.0.1.1.2.2";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Instance);
    q.sop_instance_uid = "1.2.826.0.1.1.1.1.1.2";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Series);
    q.sop_instance_uid = "1.2.826.0.1.1.1.1.1.2";
    CHECK_EQ(count(st, q), 1u);
    q = at(Level::Patient);
    q.modality = "SR";  // patient level, filtered through series
    CHECK_EQ(count(st, q), 1u);
    q.study_uid = "no-such-uid";
    CHECK_EQ(count(st, q), 0u);

    // combined filters across levels
    q = at(Level::Instance);
    q.patient_name = "m*";
    q.modality = "CT";
    q.study_date = "2020*";  // not a valid range -> error
    CHECK(st.query(q).error().code == Errc::InvalidArgument);
    q.study_date = "20200310-20200310";
    CHECK_EQ(count(st, q), 2u);
}

TEST_CASE("store: query results carry counts, modalities, paging and order") {
    QueryFixture fx;
    REQUIRE(fx.st != nullptr);
    Store& st = *fx.st;
    auto p = st.query(at(Level::Patient));
    REQUIRE(p.is_ok() && p->rows.size() == 3);
    // ordered by folded name: "m..." < "o'..." < "yamada..."
    CHECK_EQ(p->rows[0][Str::PatientId], "MUL001");
    CHECK_EQ(p->rows[0].num_studies, 2u);
    CHECK_EQ(p->rows[0].num_series, 3u);
    CHECK_EQ(p->rows[0].num_instances, 4u);
    CHECK_EQ(p->rows[0][Str::PatientName], "M\xC3\xBCller^J\xC3\xBCrgen");
    CHECK(p->rows[0].level == Level::Patient);

    auto s = st.query(at(Level::Study));
    REQUIRE(s.is_ok() && s->rows.size() == 4);
    CHECK_EQ(s->rows[0][Str::StudyDate], "20210620");  // newest first
    CHECK_EQ(s->rows[0][Str::ModalitiesInStudy], "MR\\SR");
    CHECK_EQ(s->rows[0].num_series, 2u);
    CHECK_EQ(s->rows[3][Str::StudyDate], "");           // undated last

    auto paged = at(Level::Study);
    paged.offset = 1;
    paged.limit = 2;
    auto pg = st.query(paged);
    REQUIRE(pg.is_ok());
    CHECK_EQ(pg->total, 4u);
    REQUIRE(pg->rows.size() == 2);
    CHECK_EQ(pg->rows[0][Str::StudyInstanceUid], s->rows[1][Str::StudyInstanceUid]);
    CHECK_EQ(pg->rows[1][Str::StudyInstanceUid], s->rows[2][Str::StudyInstanceUid]);
    paged.offset = 10;
    CHECK_EQ(count(st, paged), 0u);

    auto series = st.series_of("1.2.826.0.1.1.2");
    REQUIRE(series.is_ok() && series->size() == 2);
    CHECK_EQ((*series)[0].series_number, 1);
    CHECK_EQ((*series)[1].series_number, 99);
    CHECK_EQ((*series)[1][Str::Modality], "SR");
    CHECK_EQ((*series)[0].num_instances, 1u);
}

TEST_CASE("store: instance order by number, then slice position, then UID") {
    TempDir td("siaara-order");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    const int numbers[] = {3, 1, 2, 10};
    for (int n : numbers) {
        Spec s = spec(1, 1, 1, n);
        add_spec(*st, td.path(), s);
    }
    // two without an instance number, ordered by position along the normal
    for (int k = 0; k < 2; ++k) {
        Spec s = spec(1, 1, 1, 100 + k);
        s.has_instance_number = false;
        s.z = k == 0 ? 50.0 : -50.0;
        add_spec(*st, td.path(), s);
    }
    auto v = st->instances_of(spec(1, 1, 1, 1).series_uid);
    REQUIRE(v.is_ok() && v->size() == 6);
    CHECK_EQ((*v)[0].instance_number, 1);
    CHECK_EQ((*v)[1].instance_number, 2);
    CHECK_EQ((*v)[2].instance_number, 3);
    CHECK_EQ((*v)[3].instance_number, 10);
    CHECK((*v)[4].position[2] == -50.0);
    CHECK((*v)[5].position[2] == 50.0);
    for (const auto& r : *v) CHECK(fs::exists(r[Str::FilePath]));
}

// ================================================================ delete / move / verify / rebuild
TEST_CASE("store: delete at every level removes files and empty parents") {
    TempDir td("siaara-del");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    for (int p = 1; p <= 2; ++p)
        for (int s = 1; s <= 2; ++s)
            for (int e = 1; e <= 2; ++e)
                for (int i = 1; i <= 2; ++i) add_spec(*st, td.path(), spec(p, s, e, i));
    CHECK_EQ(st->stats().instances, 16u);
    std::vector<Event> events;
    st->set_callback([&](const Event& e) { events.push_back(e); });
    const std::string victim = instance_relative_path(spec(1, 1, 1, 1).sop_uid);
    REQUIRE(fs::exists(td / "s" / victim));
    auto n = st->remove(Level::Instance, spec(1, 1, 1, 1).sop_uid);
    CHECK(n.is_ok() && *n == 1);
    CHECK(!fs::exists(td / "s" / victim));
    CHECK_EQ(events.size(), 1u);
    CHECK(events.back().kind == EventKind::Deleted);
    n = st->remove(Level::Series, spec(1, 1, 2, 1).series_uid);
    CHECK(n.is_ok() && *n == 2);
    n = st->remove(Level::Study, spec(1, 2, 1, 1).study_uid);
    CHECK(n.is_ok() && *n == 4);
    CHECK_EQ(st->stats().studies, 3u);
    // what is left of patient 1: study 1 series 1 instance 2
    auto pq = st->query(at(Level::Patient));
    REQUIRE(pq.is_ok() && pq->rows.size() == 2);
    n = st->remove(Level::Patient, Store::patient_key(pq->rows[0]));
    CHECK(n.is_ok() && *n == 1);
    CHECK_EQ(st->stats().patients, 1u);
    n = st->remove(Level::Study, "nope");
    CHECK(n.is_ok() && *n == 0);
    CHECK_EQ(st->stats().instances, 8u);
    CHECK_EQ(events.size(), 8u);
    // persists
    st.reset();
    st = open_store(td / "s");
    REQUIRE(st != nullptr);
    CHECK_EQ(st->stats().instances, 8u);
    CHECK_EQ(st->stats().patients, 1u);
}

TEST_CASE("store: move a study to another store") {
    TempDir td("siaara-move");
    auto a = open_store(td / "a");
    auto b = open_store(td / "b");
    REQUIRE(a != nullptr && b != nullptr);
    for (int i = 1; i <= 3; ++i) add_spec(*a, td.path(), spec(1, 1, 1, i));
    add_spec(*a, td.path(), spec(1, 2, 1, 1));
    auto n = a->move_to(*b, Level::Study, spec(1, 1, 1, 1).study_uid);
    CHECK(n.is_ok() && *n == 3);
    CHECK_EQ(a->stats().instances, 1u);
    CHECK_EQ(b->stats().instances, 3u);
    auto v = b->instances_of(spec(1, 1, 1, 1).series_uid);
    REQUIRE(v.is_ok() && v->size() == 3);
    for (const auto& r : *v) CHECK(fs::exists(r[Str::FilePath]));
    CHECK(a->move_to(*a, Level::Study, "x").error().code == Errc::InvalidArgument);
}

TEST_CASE("store: verify finds missing, changed and orphan files; repair fixes them") {
    TempDir td("siaara-verify");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    for (int i = 1; i <= 5; ++i) add_spec(*st, td.path(), spec(1, 1, 1, i));
    auto rep = st->verify(true, false);
    REQUIRE(rep.is_ok());
    CHECK(rep->ok());
    CHECK_EQ(rep->checked, 5u);
    const fs::path root = td / "s";
    fs::remove(root / instance_relative_path(spec(1, 1, 1, 1).sop_uid));  // missing
    {
        // changed size, same instance (re-encoded with an extra element): reindexed by repair
        Spec s = spec(1, 1, 1, 2);
        s.series_desc = "A much longer series description";
        store_synth::write_spec(root / instance_relative_path(s.sop_uid), s);
    }
    {
        // same size, other instance (deep check only)
        Spec s = spec(1, 1, 1, 3);
        Spec other = s;
        other.sop_uid = s.sop_uid.substr(0, s.sop_uid.size() - 1) + "9";
        store_synth::write_spec(root / instance_relative_path(s.sop_uid), other);
    }
    store_synth::write_bytes(root / "data" / "00" / "00" / "stray.dcm", {1, 2, 3});
    store_synth::write_bytes(root / "data" / "00" / "00" / "x.dcm.123.siaara-tmp", {1});
    rep = st->verify(false, false);
    REQUIRE(rep.is_ok());
    CHECK_EQ(rep->missing, 1u);
    CHECK_EQ(rep->size_mismatch, 1u);
    CHECK_EQ(rep->content_mismatch, 0u);
    CHECK_EQ(rep->orphans, 2u);
    CHECK(rep->missing_uids.size() == 1 && rep->missing_uids[0] == spec(1, 1, 1, 1).sop_uid);
    for (const auto& o : rep->orphan_paths) CHECK(o.rfind("data/00/00/", 0) == 0);
    rep = st->verify(true, true);
    REQUIRE(rep.is_ok());
    CHECK_EQ(rep->content_mismatch, 1u);
    CHECK(rep->repaired >= 4u);
    rep = st->verify(true, false);
    REQUIRE(rep.is_ok());
    CHECK(rep->ok());
    CHECK_EQ(st->stats().instances, 3u);
    auto r2 = st->instance(spec(1, 1, 1, 2).sop_uid);
    REQUIRE(r2.is_ok());
    CHECK_EQ((*r2)[Str::SeriesDescription], "A much longer series description");
}

TEST_CASE("store: rebuild re-indexes the data folder and referenced files") {
    TempDir td("siaara-rebuild");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    for (int i = 1; i <= 4; ++i) add_spec(*st, td.path(), spec(1, 1, 1, i));
    Spec ref = spec(2, 1, 1, 1);
    store_synth::write_spec(td / "ext.dcm", ref);
    REQUIRE(st->add_file((td / "ext.dcm").string(), DuplicatePolicy::Keep, AddMode::Reference).is_ok());
    bool rebuilt = false;
    st->set_callback([&](const Event& e) { rebuilt = rebuilt || e.kind == EventKind::Rebuilt; });
    const auto gen = st->stats().generation;
    REQUIRE(st->rebuild().is_ok());
    CHECK(rebuilt);
    CHECK_EQ(st->stats().instances, 5u);
    CHECK_EQ(st->stats().journal_records, 0u);
    CHECK(st->stats().generation > gen);
    st.reset();
    // lose the whole index: rebuild_if_corrupt recovers the copied files
    std::error_code ec;
    store_synth::write_bytes(td / "s" / "index" / "snapshot.sxs", {'j', 'u', 'n', 'k', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    auto bad = Store::open((td / "s").string());
    CHECK(!bad.is_ok() && bad.error().code == Errc::Malformed);
    StoreOptions o;
    o.rebuild_if_corrupt = true;
    st = open_store(td / "s", o);
    REQUIRE(st != nullptr);
    CHECK_EQ(st->stats().instances, 4u);
    st.reset();
    st = open_store(td / "s");
    REQUIRE(st != nullptr);
    CHECK_EQ(st->stats().instances, 4u);
}

// ================================================================ persistence and crash recovery
TEST_CASE("store: reopen, compaction and stale journals") {
    TempDir td("siaara-compact");
    StoreOptions o;
    o.compact_min_records = 5;
    o.compact_ratio = 0;
    auto st = open_store(td / "s", o);
    REQUIRE(st != nullptr);
    for (int i = 1; i <= 12; ++i) add_spec(*st, td.path(), spec(1 + i % 3, 1, 1, i));
    const auto s1 = st->stats();
    CHECK(s1.generation >= 2u);           // compacted at least twice
    CHECK(s1.journal_records <= 5u);
    CHECK(s1.snapshot_bytes > 0u);
    REQUIRE(st->compact().is_ok());
    // keep a copy of the (empty) journal of this generation, add more, compact again,
    // then put the old journal back: it is older than the snapshot and must be ignored
    const auto old_journal = store_synth::read_bytes(td / "s" / "index" / "journal.sxj");
    add_spec(*st, td.path(), spec(9, 1, 1, 1));
    REQUIRE(st->compact().is_ok());
    st.reset();
    store_synth::write_bytes(td / "s" / "index" / "journal.sxj", old_journal);
    st = open_store(td / "s", o);
    REQUIRE(st != nullptr);
    CHECK_EQ(st->stats().instances, 13u);
    CHECK_EQ(st->stats().patients, 4u);
    auto q = st->query(at(Level::Instance));
    REQUIRE(q.is_ok() && q->rows.size() == 13);
    for (const auto& r : q->rows) {
        CHECK(fs::exists(r[Str::FilePath]));
        CHECK(!r[Str::PatientName].empty());
        CHECK(r.flags & kHasPixelData);
    }
    // a journal newer than the snapshot means the snapshot was lost: refuse
    st.reset();
    auto j = store_synth::read_bytes(td / "s" / "index" / "journal.sxj");
    REQUIRE(j.size() >= 16);
    j[12] = static_cast<std::uint8_t>(j[12] + 5);
    store_synth::write_bytes(td / "s" / "index" / "journal.sxj", j);
    auto bad = Store::open((td / "s").string());
    CHECK(!bad.is_ok() && bad.error().code == Errc::Malformed);
}

TEST_CASE("store: crash recovery at every journal byte position") {
    TempDir td("siaara-crash");
    const fs::path root = td / "s";
    struct Op {
        bool add;
        std::string uid;
    };
    std::vector<Op> ops;
    {
        auto st = open_store(root);
        REQUIRE(st != nullptr);
        for (int i = 1; i <= 5; ++i) {
            Spec s = spec(1 + (i & 1), 1, 1, i);
            add_spec(*st, td.path(), s);
            ops.push_back({true, s.sop_uid});
        }
        REQUIRE(st->remove(Level::Instance, spec(2, 1, 1, 1).sop_uid).is_ok());
        ops.push_back({false, spec(2, 1, 1, 1).sop_uid});
        Spec again = spec(1, 1, 1, 2);
        again.study_desc = "replaced";
        add_spec(*st, td.path(), again, DuplicatePolicy::Replace, AddOutcome::Replaced);
        ops.push_back({true, again.sop_uid});
        REQUIRE(st->remove(Level::Instance, spec(2, 1, 1, 3).sop_uid).is_ok());
        ops.push_back({false, spec(2, 1, 1, 3).sop_uid});
    }
    const fs::path jp = root / "index" / "journal.sxj";
    const auto full = store_synth::read_bytes(jp);
    // record boundaries
    std::vector<std::size_t> ends;
    for (std::size_t pos = 16; pos + 9 <= full.size();) {
        pos += 9 + load_le32(full.data() + pos);
        ends.push_back(pos);
    }
    REQUIRE(ends.size() == ops.size());
    REQUIRE(ends.back() == full.size());
    auto expected_after = [&](std::size_t nops) {
        std::set<std::string> live;
        for (std::size_t i = 0; i < nops; ++i) {
            if (ops[i].add) live.insert(ops[i].uid);
            else live.erase(ops[i].uid);
        }
        return live.size();
    };
    std::size_t failures = 0;
    for (std::size_t k = 0; k <= full.size(); ++k) {
        store_synth::write_bytes(jp, std::vector<std::uint8_t>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(k)));
        auto r = Store::open(root.string());
        if (!r) {
            ++failures;
            continue;
        }
        const std::size_t complete = static_cast<std::size_t>(std::count_if(ends.begin(), ends.end(), [&](std::size_t e) { return e <= k; }));
        const auto stt = (*r)->stats();
        if (stt.instances != expected_after(complete) || stt.replayed_records != complete) ++failures;
        const std::size_t good = complete ? ends[complete - 1] : 16;
        if (k >= 16 && stt.recovered_bytes != k - good) ++failures;
        if (k >= 16 && fs::file_size(jp) != good) ++failures;  // tail truncated on open
        if (k < 16 && fs::file_size(jp) != 16) ++failures;     // torn header rewritten
    }
    CHECK_EQ(failures, 0u);
    // after recovery from a torn tail the store keeps working
    store_synth::write_bytes(jp, std::vector<std::uint8_t>(full.begin(), full.end() - 3));
    {
        auto st = open_store(root);
        REQUIRE(st != nullptr);
        CHECK_EQ(st->stats().instances, expected_after(ops.size() - 1));
        add_spec(*st, td.path(), spec(5, 1, 1, 1));
    }
    {
        auto st = open_store(root);
        REQUIRE(st != nullptr);
        CHECK_EQ(st->stats().instances, expected_after(ops.size() - 1) + 1);
    }
    // a flipped bit in record 3 ends the replay there
    auto flipped = full;
    flipped[ends[2] + 20] ^= 0x10;
    store_synth::write_bytes(jp, flipped);
    {
        auto st = open_store(root);
        REQUIRE(st != nullptr);
        CHECK_EQ(st->stats().replayed_records, 3u);
        CHECK_EQ(st->stats().instances, expected_after(3));
    }
}

// ================================================================ lock and concurrency
TEST_CASE("store: lock file stops a second open (same process and another process)") {
    TempDir td("siaara-lock");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    CHECK(fs::exists(td / "s" / "siaara-store.lock"));
    auto second = Store::open((td / "s").string());
    CHECK(!second.is_ok() && second.error().code == Errc::Rejected);
#if !defined(_WIN32)
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        // child: the parent's lock must be visible across processes
        auto r = Store::open((td / "s").string());
        _exit(!r.is_ok() && r.error().code == Errc::Rejected ? 0 : 1);
    }
    REQUIRE(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    st.reset();
    auto third = Store::open((td / "s").string());
    CHECK(third.is_ok());
}

TEST_CASE("store: concurrent adders and readers; callbacks may re-enter") {
    TempDir td("siaara-mt");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    // pre-generate files
    std::vector<std::string> files;
    for (int t = 0; t < 4; ++t)
        for (int i = 1; i <= 25; ++i) {
            Spec s = spec(t, 1, 1, i);
            const fs::path p = td / ("in" + std::to_string(t)) / (std::to_string(i) + ".dcm");
            store_synth::write_spec(p, s);
            files.push_back(p.string());
        }
    std::atomic<int> events{0};
    std::atomic<int> nested_ok{0};
    st->set_callback([&](const Event& e) {
        ++events;
        if (st->instance(e.sop_instance_uid).is_ok()) ++nested_ok;  // re-entrant query
    });
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto q = st->query(at(Level::Study));
            if (!q) ++errors;
        }
    });
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t)
        writers.emplace_back([&, t] {
            for (int i = 0; i < 25; ++i) {
                // each file twice: the second add must be a no-op
                for (int k = 0; k < 2; ++k)
                    if (!st->add_file(files[static_cast<std::size_t>(t * 25 + i)])) ++errors;
            }
        });
    for (auto& w : writers) w.join();
    stop.store(true);
    reader.join();
    CHECK_EQ(errors.load(), 0);
    CHECK_EQ(st->stats().instances, 100u);
    CHECK_EQ(st->stats().patients, 4u);
    CHECK_EQ(events.load(), 100);
    CHECK_EQ(nested_ok.load(), 100);
}

TEST_CASE("store: add_records bulk-indexes referenced files") {
    TempDir td("siaara-bulk");
    auto st = open_store(td / "s");
    REQUIRE(st != nullptr);
    std::vector<IndexRecord> recs;
    for (int i = 0; i < 100; ++i) {
        IndexRecord r;
        r[Str::PatientId] = "B" + std::to_string(i % 10);
        r[Str::PatientName] = "Bulk^" + std::to_string(i % 10);
        r[Str::StudyInstanceUid] = "9.1." + std::to_string(i % 10);
        r[Str::SeriesInstanceUid] = "9.1." + std::to_string(i % 10) + ".1";
        r[Str::SopInstanceUid] = "9.1." + std::to_string(i % 10) + ".1." + std::to_string(i);
        r[Str::FilePath] = (td / "nowhere" / (std::to_string(i) + ".dcm")).string();
        recs.push_back(r);
    }
    auto n = st->add_records(recs);
    CHECK(n.is_ok() && *n == 100);
    CHECK_EQ(st->stats().studies, 10u);
    recs[0][Str::FilePath].clear();
    CHECK(st->add_records(recs).error().code == Errc::InvalidArgument);
    auto rep = st->verify(false, false);
    REQUIRE(rep.is_ok());
    CHECK_EQ(rep->missing, 100u);
}

// ================================================================ C API
namespace {
struct CEvents {
    int added = 0, deleted = 0;
};
void SIAARA_CALL c_event(void* user, int32_t event, const char* sop, const char*, const char*) {
    auto* e = static_cast<CEvents*>(user);
    if (event == SIAARA_EVENT_ADDED && sop && *sop) ++e->added;
    if (event == SIAARA_EVENT_DELETED) ++e->deleted;
}
const char* str_of(const siaara_index_record& r, const char* tab, int k) { return tab + r.str_offset[k]; }
}  // namespace

TEST_CASE("store: C API extract, store, query, delete, verify, stats") {
    TempDir td("siaara-capi");
    Spec s = spec(1, 1, 1, 1);
    s.charset = "ISO_IR 192";
    s.patient_name = "\xC3\x89lise^Dupont";
    const fs::path f = td / "a.dcm";
    store_synth::write_spec(f, s);
    const std::string fp = f.string();

    siaara_index_record rec{};
    rec.struct_size = sizeof rec;
    size_t needed = 0;
    CHECK_EQ(siaara_extract_index(fp.c_str(), fp.size(), &rec, nullptr, 0, &needed), SIAARA_OK);
    CHECK(needed > 100);
    std::vector<char> tab(needed);
    CHECK_EQ(siaara_extract_index(fp.c_str(), fp.size(), &rec, tab.data(), 10, &needed), SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(rec.rows, 4u);  // numbers are filled even when the strings do not fit
    CHECK_EQ(siaara_extract_index(fp.c_str(), fp.size(), &rec, tab.data(), tab.size(), &needed), SIAARA_OK);
    CHECK_EQ(std::string(str_of(rec, tab.data(), SIAARA_IDX_PATIENT_NAME)), s.patient_name);
    CHECK_EQ(rec.str_length[SIAARA_IDX_PATIENT_NAME], s.patient_name.size());
    CHECK_EQ(std::string(str_of(rec, tab.data(), SIAARA_IDX_SOP_INSTANCE_UID)), s.sop_uid);
    CHECK_EQ(std::string(str_of(rec, tab.data(), SIAARA_IDX_RECORD_TYPE)), "");
    CHECK(rec.flags & SIAARA_IDXF_PIXEL_OFFSET);
    CHECK_EQ(rec.image_position[2], 2.5);
    siaara_index_record small{};
    small.struct_size = 8;
    CHECK_EQ(siaara_extract_index(fp.c_str(), fp.size(), &small, nullptr, 0, &needed), SIAARA_ERR_INVALID_ARGUMENT);
    const auto bytes = store_synth::read_bytes(f);
    CHECK_EQ(siaara_extract_index_memory(bytes.data(), bytes.size(), &rec, tab.data(), tab.size(), &needed), SIAARA_OK);
    CHECK_EQ(rec.file_size, bytes.size());

    const std::string root = (td / "store").string();
    siaara_store* st = nullptr;
    REQUIRE(siaara_store_open(root.c_str(), root.size(), nullptr, &st) == SIAARA_OK);
    siaara_store* st2 = nullptr;
    CHECK_EQ(siaara_store_open(root.c_str(), root.size(), nullptr, &st2), SIAARA_ERR_REJECTED);
    CEvents ev;
    CHECK_EQ(siaara_store_set_callback(st, &c_event, &ev), SIAARA_OK);
    int32_t outcome = -1;
    CHECK_EQ(siaara_store_add_file(st, fp.c_str(), fp.size(), SIAARA_DUP_KEEP, SIAARA_ADD_COPY, &outcome), SIAARA_OK);
    CHECK_EQ(outcome, SIAARA_ADD_ADDED);
    CHECK_EQ(siaara_store_add_memory(st, bytes.data(), bytes.size(), SIAARA_DUP_KEEP, &outcome), SIAARA_OK);
    CHECK_EQ(outcome, SIAARA_ADD_KEPT_EXISTING);
    CHECK_EQ(siaara_store_add_memory(st, bytes.data(), bytes.size(), SIAARA_DUP_REPLACE, &outcome), SIAARA_OK);
    CHECK_EQ(outcome, SIAARA_ADD_REPLACED);
    CHECK_EQ(siaara_store_add_file(st, fp.c_str(), fp.size(), 7, SIAARA_ADD_COPY, &outcome), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(ev.added, 1);

    siaara_store_filter q{};
    q.struct_size = sizeof q;
    q.level = SIAARA_LEVEL_PATIENT;
    q.patient_name = "elise*";
    siaara_store_result* res = nullptr;
    REQUIRE(siaara_store_query(st, &q, &res) == SIAARA_OK);
    uint32_t n = 0, total = 0;
    CHECK_EQ(siaara_store_result_count(res, &n, &total), SIAARA_OK);
    CHECK_EQ(n, 1u);
    CHECK_EQ(total, 1u);
    siaara_index_record row{};
    row.struct_size = sizeof row;
    std::vector<char> rt(4096);
    CHECK_EQ(siaara_store_result_get(res, 0, &row, rt.data(), rt.size(), &needed), SIAARA_OK);
    CHECK_EQ(row.level, SIAARA_LEVEL_PATIENT);
    CHECK_EQ(row.num_instances, 1u);
    CHECK_EQ(siaara_store_result_get(res, 1, &row, rt.data(), rt.size(), &needed), SIAARA_ERR_NOT_FOUND);
    char key[256];
    CHECK_EQ(siaara_store_patient_key(res, 0, key, sizeof key, &needed), SIAARA_OK);
    const std::string pkey(key, needed);
    siaara_store_result_free(res);
    q.level = 9;
    CHECK_EQ(siaara_store_query(st, &q, &res), SIAARA_ERR_INVALID_ARGUMENT);

    REQUIRE(siaara_store_get_instances(st, s.series_uid.c_str(), s.series_uid.size(), &res) == SIAARA_OK);
    CHECK_EQ(siaara_store_result_count(res, &n, nullptr), SIAARA_OK);
    CHECK_EQ(n, 1u);
    CHECK_EQ(siaara_store_result_get(res, 0, &row, rt.data(), rt.size(), &needed), SIAARA_OK);
    CHECK(fs::exists(str_of(row, rt.data(), SIAARA_IDX_FILE_PATH)));
    siaara_store_result_free(res);
    REQUIRE(siaara_store_get_series(st, s.study_uid.c_str(), s.study_uid.size(), &res) == SIAARA_OK);
    CHECK_EQ(siaara_store_result_count(res, &n, nullptr), SIAARA_OK);
    CHECK_EQ(n, 1u);
    siaara_store_result_free(res);

    siaara_store_stats stats{};
    stats.struct_size = sizeof stats;
    CHECK_EQ(siaara_store_stats_get(st, &stats), SIAARA_OK);
    CHECK_EQ(stats.instances, 1u);
    CHECK_EQ(stats.patients, 1u);
    siaara_store_verify_report vr{};
    vr.struct_size = sizeof vr;
    CHECK_EQ(siaara_store_verify(st, 1, 0, &vr), SIAARA_OK);
    CHECK_EQ(vr.checked, 1u);
    CHECK_EQ(vr.missing + vr.orphans + vr.size_mismatch + vr.content_mismatch, 0u);
    CHECK_EQ(siaara_store_compact(st), SIAARA_OK);
    CHECK_EQ(siaara_store_rebuild(st), SIAARA_OK);

    const std::string root2 = (td / "store2").string();
    siaara_store* other = nullptr;
    REQUIRE(siaara_store_open(root2.c_str(), root2.size(), nullptr, &other) == SIAARA_OK);
    uint32_t moved = 0;
    CHECK_EQ(siaara_store_move(st, other, SIAARA_LEVEL_PATIENT, pkey.c_str(), pkey.size(), &moved), SIAARA_OK);
    CHECK_EQ(moved, 1u);
    CHECK_EQ(ev.deleted, 1);
    uint32_t removed = 0;
    CHECK_EQ(siaara_store_delete(other, SIAARA_LEVEL_STUDY, s.study_uid.c_str(), s.study_uid.size(), &removed), SIAARA_OK);
    CHECK_EQ(removed, 1u);
    CHECK_EQ(siaara_store_set_callback(st, nullptr, nullptr), SIAARA_OK);
    siaara_store_free(other);
    siaara_store_free(st);
    siaara_store_free(nullptr);
    siaara_store_result_free(nullptr);
}
