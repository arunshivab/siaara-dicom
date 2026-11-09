// DICOMDIR tests: write/read round trip, record types, profiles, broken links and cycles,
// inactive records, path sanitisation attacks, and (when installed) DCMTK dcmmkdir as an oracle.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/types.hpp"
#include "core/writer.hpp"
#include "crosscheck/store_synth.hpp"
#include "siaara/siaara.h"
#include "store/dicomdir.hpp"
#include "store/path.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::store;
using store_synth::Spec;
using store_synth::TempDir;
namespace fs = std::filesystem;

namespace {

std::vector<DicomdirRecord> read_all(const fs::path& p, DicomdirSummary* sum = nullptr, DicomdirReadOptions opt = {}) {
    std::vector<DicomdirRecord> out;
    auto r = read_dicomdir(p.string(), [&](const DicomdirRecord& rec) {
        out.push_back(rec);
        return true;
    }, opt);
    CHECK(r.is_ok());
    if (r && sum) *sum = *r;
    return out;
}

std::size_t count_type(const std::vector<DicomdirRecord>& v, const std::string& t) {
    return static_cast<std::size_t>(std::count_if(v.begin(), v.end(), [&](const DicomdirRecord& r) { return r.keys[Str::RecordType] == t; }));
}

// Test file set: patient A (Latin-1 name) with a CT study of 2 series x 3 images and an RT
// dose, patient B with an SR in its own study, patient C with one MR image.
std::vector<std::string> make_set(const fs::path& dir, std::map<std::string, std::string>* uid_to_file = nullptr) {
    std::vector<std::string> files;
    auto add = [&](const Spec& s) {
        const fs::path p = dir / ("f" + std::to_string(files.size()) + ".dcm");
        store_synth::write_spec(p, s);
        files.push_back(p.string());
        if (uid_to_file) (*uid_to_file)[s.sop_uid] = p.string();
    };
    for (int e = 1; e <= 2; ++e)
        for (int i = 1; i <= 3; ++i) {
            Spec s;
            s.charset = "ISO_IR 100";
            s.patient_id = "PA";
            s.patient_name = "M\xC3\xBCller^Anna";
            s.study_uid = "1.2.40.1";
            s.series_uid = "1.2.40.1." + std::to_string(e);
            s.sop_uid = s.series_uid + "." + std::to_string(i);
            s.series_number = e;
            s.instance_number = i;
            add(s);
        }
    Spec dose;
    dose.patient_id = "PA";
    dose.patient_name = "M\xC3\xBCller^Anna";
    dose.charset = "ISO_IR 100";
    dose.study_uid = "1.2.40.1";
    dose.series_uid = "1.2.40.1.9";
    dose.sop_uid = "1.2.40.1.9.1";
    dose.modality = "RTDOSE";
    dose.sop_class = "1.2.840.10008.5.1.4.1.1.481.2";
    dose.extra = {{0x3004000A, VR::CS, "PLAN"}};
    add(dose);
    Spec sr;
    sr.patient_id = "PB";
    sr.patient_name = "Brown^Bob";
    sr.study_uid = "1.2.40.2";
    sr.series_uid = "1.2.40.2.1";
    sr.sop_uid = "1.2.40.2.1.1";
    sr.modality = "SR";
    sr.pixels = false;
    sr.sop_class = "1.2.840.10008.5.1.4.1.1.88.33";
    sr.extra = {{0x00080023, VR::DA, "20200102"}, {0x00080033, VR::TM, "120000"},
                {0x0040A491, VR::CS, "COMPLETE"}, {0x0040A493, VR::CS, "UNVERIFIED"}};
    sr.concept_name = true;
    add(sr);
    Spec mr;
    mr.patient_id = "PC";
    mr.patient_name = "Chen^Li";
    mr.study_uid = "1.2.40.3";
    mr.series_uid = "1.2.40.3.1";
    mr.sop_uid = "1.2.40.3.1.1";
    mr.modality = "MR";
    mr.sop_class = "1.2.840.10008.5.1.4.1.1.4";
    add(mr);
    return files;
}

// A DICOMDIR built by hand with no offsets (root offset 0: read in sequence order).
fs::path handmade(const fs::path& root, const std::vector<std::string>& file_ids) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, kMediaStorageDirectoryUid);
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, "1.2.3.4.5");
    (void)f.dataset.set_string(0x00041130, VR::CS, "");
    (void)f.dataset.set_uint32(0x00041200, 0);
    (void)f.dataset.set_uint32(0x00041202, 0);
    (void)f.dataset.set_uint16(0x00041212, 0);
    Element seq;
    seq.tag = 0x00041220;
    seq.vr = VR::SQ;
    auto rec = [](const char* type) {
        Dataset d;
        (void)d.set_uint32(0x00041400, 0);
        (void)d.set_uint16(0x00041410, 0xFFFF);
        (void)d.set_uint32(0x00041420, 0);
        (void)d.set_string(0x00041430, VR::CS, type);
        return d;
    };
    Dataset p = rec("PATIENT");
    (void)p.set_string(0x00100020, VR::LO, "HAND");
    seq.items.push_back(p);
    seq.items.push_back(rec("STUDY"));
    seq.items.push_back(rec("SERIES"));
    for (const auto& id : file_ids) {
        Dataset im = rec("IMAGE");
        (void)im.set_bytes(0x00041500, VR::CS, Bytes::from_string(id.size() % 2 ? id + " " : id));
        seq.items.push_back(im);
    }
    f.dataset.put(std::move(seq));
    const fs::path p2 = root / "DICOMDIR";
    (void)write_file(f, p2.string());
    return p2;
}

bool has_dcmmkdir() { return std::system("command -v dcmmkdir > /dev/null 2>&1") == 0; }

}  // namespace

TEST_CASE("dicomdir: path components are sanitised") {
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "C:", "x:y", "CON", "con", "Aux.dcm", "NUL.txt", "COM1",
                            "lpt9", "COM\xC2\xB9", "CONIN$", "file.", "file ", "a*b", "a?b", "a|b", "a<b", "a\"b",
                            "a\x01" "b", "a\x7F"})
        CHECK(!path_component_ok(bad));
    for (const char* good : {"IM000001", "DICOM", "a.dcm", "COM10", "CONSOLE", "LPT", "x y", "\xC3\xA9t\xC3\xA9"})
        CHECK(path_component_ok(good));
    const auto parts = split_file_id("DICOM\\P0000001\\IM1 ");
    REQUIRE(parts.size() == 3);
    CHECK_EQ(parts[2], "IM1");
    TempDir td("siaara-path");
    CHECK(safe_join(td.path(), {"A", "..", "B"}).error().code == Errc::Rejected);
    CHECK(safe_join(td.path(), {}).error().code == Errc::Rejected);
    CHECK(safe_join(td.path(), std::vector<std::string>(40, "A")).error().code == Errc::Rejected);
    CHECK(safe_join(td.path(), {"A", "B"}).is_ok());
    CHECK(is_within(td.path(), td.path() / "a" / "b"));
    CHECK(!is_within(td.path() / "a", td.path() / "a" / ".." / "b"));
    CHECK(!is_within(td.path(), td.path().parent_path()));
}

TEST_CASE("dicomdir: write then read round trip") {
    TempDir td("siaara-dd");
    std::map<std::string, std::string> src;
    const auto files = make_set(td / "src", &src);
    DicomdirWriteOptions wo;
    wo.file_set_id = "SIAARA TEST";
    auto w = write_dicomdir((td / "media").string(), files, wo);
    REQUIRE(w.is_ok());
    CHECK_EQ(w->files, 9u);
    CHECK_EQ(w->patients, 3u);
    CHECK_EQ(w->studies, 3u);
    CHECK_EQ(w->series, 5u);
    CHECK(w->warnings.empty());
    for (const auto& m : w->warnings) std::printf("  warning: %s\n", m.c_str());

    DicomdirSummary sum;
    const auto recs = read_all(td / "media" / "DICOMDIR", &sum);
    CHECK_EQ(sum.file_set_id, "SIAARA TEST");
    CHECK(!sum.sequential_fallback);
    CHECK_EQ(sum.unreachable, 0u);
    CHECK_EQ(sum.records, 9u + 3 + 3 + 5);
    CHECK_EQ(sum.reported, sum.records);
    CHECK_EQ(sum.files, 9u);
    CHECK_EQ(sum.missing_files + sum.rejected_paths, 0u);
    CHECK_EQ(count_type(recs, "PATIENT"), 3u);
    CHECK_EQ(count_type(recs, "STUDY"), 3u);
    CHECK_EQ(count_type(recs, "SERIES"), 5u);
    CHECK_EQ(count_type(recs, "IMAGE"), 7u);
    CHECK_EQ(count_type(recs, "RT DOSE"), 1u);
    CHECK_EQ(count_type(recs, "SR DOCUMENT"), 1u);
    std::map<std::size_t, const DicomdirRecord*> by_index;
    for (const auto& r : recs) by_index[r.index] = &r;
    std::set<std::string> seen;
    for (const auto& r : recs) {
        const std::string& type = r.keys[Str::RecordType];
        if (type == "PATIENT") {
            CHECK_EQ(r.depth, 0u);
            CHECK_EQ(r.parent, -1);
            continue;
        }
        REQUIRE(r.parent >= 0);
        const DicomdirRecord* parent = by_index[static_cast<std::size_t>(r.parent)];
        REQUIRE(parent != nullptr);
        CHECK_EQ(parent->depth + 1, r.depth);
        if (type == "STUDY") CHECK_EQ(parent->keys[Str::RecordType], "PATIENT");
        if (type == "SERIES") CHECK_EQ(parent->keys[Str::RecordType], "STUDY");
        if (r.depth == 3) {
            CHECK_EQ(parent->keys[Str::RecordType], "SERIES");
            // keys inherited from the ancestors
            CHECK(!r.keys[Str::PatientId].empty());
            CHECK(!r.keys[Str::StudyInstanceUid].empty());
            CHECK(!r.keys[Str::SeriesInstanceUid].empty());
            const std::string uid = r.keys[Str::SopInstanceUid];
            REQUIRE(src.count(uid) == 1);
            seen.insert(uid);
            CHECK(r.file_status.is_ok());
            CHECK(store_synth::read_bytes(r.keys[Str::FilePath]) == store_synth::read_bytes(src[uid]));
            CHECK_EQ(r.keys[Str::TransferSyntaxUid], std::string(ts::ExplicitVRLittleEndian));
            if (r.keys[Str::PatientId] == "PA") CHECK_EQ(r.keys[Str::PatientName], "M\xC3\xBCller^Anna");
            REQUIRE(r.file_id.size() == 5);
            CHECK_EQ(r.file_id[0], "DICOM");
            for (const auto& c : r.file_id) {
                CHECK(c.size() <= 8);
                for (char ch : c) CHECK((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
            }
            if (type == "IMAGE" && r.keys[Str::PatientId] == "PA") CHECK(r.keys.flags & kHasInstanceNumber);
        }
    }
    CHECK_EQ(seen.size(), 9u);

    // the output folder already has a DICOMDIR
    CHECK(write_dicomdir((td / "media").string(), files, wo).error().code == Errc::Rejected);
    wo.overwrite = true;
    CHECK(write_dicomdir((td / "media").string(), files, wo).is_ok());
    wo.file_set_id = "lower case";
    CHECK(write_dicomdir((td / "media2").string(), files, wo).error().code == Errc::InvalidArgument);
    CHECK(write_dicomdir((td / "media3").string(), {}, {}).error().code == Errc::InvalidArgument);

    // stopping from the callback
    int n = 0;
    auto stop = read_dicomdir((td / "media" / "DICOMDIR").string(), [&](const DicomdirRecord&) { return ++n < 3; });
    CHECK(!stop.is_ok() && stop.error().code == Errc::Cancelled);
    CHECK_EQ(n, 3);
}

TEST_CASE("dicomdir: media profiles restrict transfer syntaxes") {
    TempDir td("siaara-prof");
    Spec imp;
    imp.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    store_synth::write_spec(td / "imp.dcm", imp);
    DicomFile rle = store_synth::make_file(Spec{});
    rle.transfer_syntax = "1.2.840.10008.1.2.5";
    Element* pd = rle.dataset.find(tags::PixelData);
    REQUIRE(pd != nullptr);
    pd->vr = VR::OB;
    pd->value = Bytes{};
    pd->undefined_length = true;
    const std::uint8_t frag[4] = {1, 2, 3, 4};
    pd->fragments = {Bytes::copy_of(frag, 0), Bytes::copy_of(frag, 4)};
    REQUIRE(write_file(rle, (td / "rle.dcm").string()).is_ok());

    DicomdirWriteOptions cd;
    CHECK(write_dicomdir((td / "a").string(), {(td / "imp.dcm").string()}, cd).error().code == Errc::Unsupported);
    CHECK(write_dicomdir((td / "b").string(), {(td / "rle.dcm").string()}, cd).error().code == Errc::Unsupported);
    DicomdirWriteOptions usb;
    usb.profile = MediaProfile::GeneralUsb;
    CHECK(write_dicomdir((td / "c").string(), {(td / "imp.dcm").string()}, usb).error().code == Errc::Unsupported);
    CHECK(write_dicomdir((td / "d").string(), {(td / "rle.dcm").string()}, usb).is_ok());
    cd.allow_any_transfer_syntax = true;
    auto w = write_dicomdir((td / "e").string(), {(td / "imp.dcm").string()}, cd);
    REQUIRE(w.is_ok());
    CHECK(!w->warnings.empty());
    const auto recs = read_all(td / "d" / "DICOMDIR");
    CHECK_EQ(recs.size(), 4u);
    CHECK_EQ(recs.back().keys[Str::TransferSyntaxUid], "1.2.840.10008.1.2.5");
}

TEST_CASE("dicomdir: broken links, cycles and inactive records are contained") {
    TempDir td("siaara-links");
    const auto files = make_set(td / "src");
    REQUIRE(write_dicomdir((td / "m").string(), files, {}).is_ok());
    const fs::path dd = td / "m" / "DICOMDIR";
    const auto orig = store_synth::read_bytes(dd);
    const auto recs = read_all(dd);
    REQUIRE(recs.size() == 20);
    std::vector<const DicomdirRecord*> patients;
    for (const auto& r : recs)
        if (r.keys[Str::RecordType] == "PATIENT") patients.push_back(&r);
    REQUIRE(patients.size() == 3);
    auto patch32 = [](std::vector<std::uint8_t>& b, std::uint64_t at, std::uint32_t v) {
        store_le32(b.data() + at, v);
    };
    // record layout (explicit VR LE): item header (8), (0004,1400) UL value at +16,
    // (0004,1410) US value at +28, (0004,1420) UL value at +38
    for (const auto* p : patients) {
        CHECK_EQ(load_le32(orig.data() + p->offset), 0xE000FFFEu);
        CHECK_EQ(load_le32(orig.data() + p->offset + 8), 0x14000004u);
    }
    {  // the first patient's "next" points at itself: cycle
        auto b = orig;
        patch32(b, patients[0]->offset + 16, static_cast<std::uint32_t>(patients[0]->offset));
        store_synth::write_bytes(dd, b);
        DicomdirSummary s;
        const auto v = read_all(dd, &s);
        CHECK_EQ(count_type(v, "PATIENT"), 1u);
        CHECK(s.unreachable > 0);
        CHECK(!s.warnings.empty());
    }
    {  // the second patient's lower link points into the middle of nowhere
        auto b = orig;
        patch32(b, patients[1]->offset + 38, 12345);
        store_synth::write_bytes(dd, b);
        DicomdirSummary s;
        const auto v = read_all(dd, &s);
        CHECK_EQ(count_type(v, "PATIENT"), 3u);
        CHECK_EQ(s.unreachable, 3u);  // its study, series and SR
    }
    {  // the second patient is inactive: it and its subtree are skipped (unless asked)
        auto b = orig;
        b[static_cast<std::size_t>(patients[1]->offset + 28)] = 0;
        b[static_cast<std::size_t>(patients[1]->offset + 29)] = 0;
        store_synth::write_bytes(dd, b);
        const auto v = read_all(dd);
        CHECK_EQ(v.size(), 16u);
        DicomdirReadOptions all;
        all.include_inactive = true;
        const auto w = read_all(dd, nullptr, all);
        CHECK_EQ(w.size(), 20u);
        CHECK(std::any_of(w.begin(), w.end(), [](const DicomdirRecord& r) { return !r.in_use; }));
    }
    {  // root offset broken: sequence order with record-type nesting
        auto b = orig;
        const std::uint8_t pat[8] = {0x04, 0x00, 0x00, 0x12, 'U', 'L', 4, 0};
        auto it = std::search(b.begin(), b.end(), pat, pat + 8);
        REQUIRE(it != b.end());
        const auto at = static_cast<std::uint64_t>(it - b.begin()) + 8;
        patch32(b, at, 7);
        store_synth::write_bytes(dd, b);
        DicomdirSummary s;
        const auto v = read_all(dd, &s);
        CHECK(s.sequential_fallback);
        CHECK_EQ(v.size(), 20u);
        for (const auto& r : v) {
            const std::string& t = r.keys[Str::RecordType];
            const std::uint32_t want = t == "PATIENT" ? 0u : t == "STUDY" ? 1u : t == "SERIES" ? 2u : 3u;
            CHECK_EQ(r.depth, want);
        }
        CHECK_EQ(s.files, 9u);
    }
    {  // truncated file fails cleanly
        std::vector<std::uint8_t> b(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(orig.size() / 2));
        store_synth::write_bytes(dd, b);
        std::size_t n = 0;
        auto r = read_dicomdir(dd.string(), [&](const DicomdirRecord&) {
            ++n;
            return true;
        });
        // lenient parsing keeps what it could read; links into the missing half are cut
        if (r) CHECK(r->reported == n && n < 20);
    }
}

TEST_CASE("dicomdir: Referenced File IDs cannot escape the media root") {
    TempDir td("siaara-attack");
    const fs::path media = td / "media";
    store_synth::write_spec(media / "dicom" / "im1", Spec{});  // lower case on disk
    store_synth::write_spec(td / "outside" / "SECRET", Spec{});
    std::error_code ec;
    fs::create_directory_symlink(td / "outside", media / "LINK", ec);
    const bool have_link = !ec;
    const std::vector<std::string> ids = {
        "DICOM\\IM1",            // ok (case-insensitive)
        "..\\..\\etc\\passwd",  "/etc/passwd", "C:\\WINDOWS\\X", "\\\\server\\share\\x", "CON", "AUX.DCM",
        "COM1", "LPT9.TXT", "DICOM\\..\\..\\X", "FILE.", std::string("A\0B", 3), "LINK\\SECRET",
        "DICOM\\NOPE",           // missing
    };
    const fs::path dd = handmade(media, ids);
    DicomdirSummary s;
    const auto v = read_all(dd, &s);
    CHECK(s.sequential_fallback);
    REQUIRE(v.size() == 3 + ids.size());
    CHECK(v[3].file_status.is_ok());
    CHECK(fs::equivalent(v[3].keys[Str::FilePath], media / "dicom" / "im1"));
    for (std::size_t i = 4; i < 3 + ids.size() - 1; ++i) {
        if (ids[i - 3] == "LINK\\SECRET" && !have_link) continue;
        CHECK(v[i].file_status.code == Errc::Rejected);
        CHECK(v[i].keys[Str::FilePath].empty());
    }
    CHECK(v.back().file_status.code == Errc::NotFound);
    CHECK_EQ(s.files, 1u);
    CHECK_EQ(s.missing_files, 1u);
    CHECK_EQ(s.rejected_paths, have_link ? 12u : 11u);
    DicomdirReadOptions exact;
    exact.case_insensitive = false;
    const auto w = read_all(dd, nullptr, exact);
    CHECK(w[3].file_status.code == Errc::NotFound);
}

TEST_CASE("dicomdir: C API read and write") {
    TempDir td("siaara-ddc");
    const auto files = make_set(td / "src");
    std::vector<const char*> ptrs;
    std::vector<size_t> lens;
    for (const auto& f : files) {
        ptrs.push_back(f.c_str());
        lens.push_back(f.size());
    }
    const std::string root = (td / "m").string();
    siaara_dicomdir_write_options wo{};
    wo.struct_size = sizeof wo;
    wo.profile = SIAARA_PROFILE_GENERAL_CD;
    wo.file_set_id = "CAPI";
    uint32_t warnings = 99;
    CHECK_EQ(siaara_dicomdir_write(root.c_str(), root.size(), ptrs.data(), lens.data(), ptrs.size(), &wo, &warnings), SIAARA_OK);
    CHECK(warnings < 99);
    wo.profile = 5;
    CHECK_EQ(siaara_dicomdir_write(root.c_str(), root.size(), ptrs.data(), lens.data(), ptrs.size(), &wo, nullptr),
             SIAARA_ERR_INVALID_ARGUMENT);
    struct Acc {
        int images = 0;
        int with_file = 0;
        bool names_ok = true;
    } acc;
    auto cb = [](void* user, const siaara_dicomdir_entry* e, const siaara_index_record* k, const char* s) -> int32_t {
        auto* a = static_cast<Acc*>(user);
        const std::string type = s + k->str_offset[SIAARA_IDX_RECORD_TYPE];
        if (type == "IMAGE") ++a->images;
        if (e->file_status == SIAARA_OK && k->str_length[SIAARA_IDX_FILE_PATH] > 0) ++a->with_file;
        if (e->depth == 3 && std::string(s + k->str_offset[SIAARA_IDX_PATIENT_ID]).empty()) a->names_ok = false;
        return 0;
    };
    const std::string dd = (td / "m" / "DICOMDIR").string();
    siaara_dicomdir_summary sum{};
    sum.struct_size = sizeof sum;
    CHECK_EQ(siaara_dicomdir_read(dd.c_str(), dd.size(), nullptr, cb, &acc, &sum), SIAARA_OK);
    CHECK_EQ(acc.images, 7);
    CHECK_EQ(acc.with_file, 9);
    CHECK(acc.names_ok);
    CHECK_EQ(sum.records, 20u);
    CHECK_EQ(sum.files, 9u);
    CHECK_EQ(sum.sequential_fallback, 0);
    auto stop = [](void*, const siaara_dicomdir_entry*, const siaara_index_record*, const char*) -> int32_t { return 1; };
    CHECK_EQ(siaara_dicomdir_read(dd.c_str(), dd.size(), nullptr, stop, nullptr, nullptr), SIAARA_ERR_CANCELLED);
    CHECK_EQ(siaara_dicomdir_read("/no/such", 8, nullptr, cb, &acc, nullptr), SIAARA_ERR_IO);
}

TEST_CASE("dicomdir: DCMTK dcmmkdir output reads correctly (oracle, skipped when absent)") {
    if (!has_dcmmkdir()) {
        std::printf("  (dcmmkdir not installed: skipped)\n");
        return;
    }
    TempDir td("siaara-dcmtk");
    std::map<std::string, std::string> src;
    std::vector<std::string> made = make_set(td / "tmp", &src);
    // dcmmkdir wants ISO 9660 style names under the file-set root
    std::map<std::string, std::string> uid_to_media;
    int k = 0;
    for (const auto& [uid, path] : src) {
        const std::string rel = "IMAGES/IM" + std::to_string(100000 + k++);
        std::error_code ec;
        fs::create_directories(td / "fs" / "IMAGES", ec);
        fs::copy_file(path, td / "fs" / rel, ec);
        uid_to_media[uid] = rel;
    }
    const std::string cmd = "cd '" + (td / "fs").string() + "' && dcmmkdir --recurse --invent +F TESTSET IMAGES > dcmmkdir.log 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::printf("  dcmmkdir failed (rc %d), log follows\n", rc);
        const int shown = std::system(("cat '" + (td / "fs" / "dcmmkdir.log").string() + "'").c_str());
        (void)shown;
    }
    REQUIRE(rc == 0);
    DicomdirSummary s;
    const auto v = read_all(td / "fs" / "DICOMDIR", &s);
    if (s.files != 9) {
        const int shown = std::system(("cat '" + (td / "fs" / "dcmmkdir.log").string() + "'").c_str());
        (void)shown;
    }
    CHECK(!s.sequential_fallback);
    CHECK_EQ(s.unreachable, 0u);
    CHECK_EQ(s.file_set_id, "TESTSET");
    CHECK_EQ(count_type(v, "PATIENT"), 3u);
    CHECK_EQ(count_type(v, "STUDY"), 3u);
    CHECK_EQ(count_type(v, "SERIES"), 5u);
    CHECK_EQ(s.files, 9u);
    std::size_t matched = 0;
    for (const auto& r : v) {
        if (r.depth != 3) continue;
        const std::string uid = r.keys[Str::SopInstanceUid];
        REQUIRE(uid_to_media.count(uid) == 1);
        CHECK(fs::equivalent(r.keys[Str::FilePath], td / "fs" / uid_to_media[uid]));
        CHECK(!r.keys[Str::PatientId].empty());
        CHECK(!r.keys[Str::SeriesInstanceUid].empty());
        ++matched;
    }
    CHECK_EQ(matched, 9u);
    CHECK_EQ(count_type(v, "RT DOSE"), 1u);
    CHECK_EQ(count_type(v, "SR DOCUMENT"), 1u);
}

TEST_CASE("dicomdir: missing Type 1 record keys are written empty and warned about") {
    TempDir td("siaara-type1");
    Spec dose;
    dose.sop_class = "1.2.840.10008.5.1.4.1.1.481.2";  // RT Dose without Dose Summation Type
    dose.modality = "RTDOSE";
    store_synth::write_spec(td / "dose.dcm", dose);
    auto w = write_dicomdir((td / "m").string(), {(td / "dose.dcm").string()});
    REQUIRE(w.is_ok());
    REQUIRE(w->warnings.size() == 1);
    CHECK(w->warnings[0].find("(3004,000A)") != std::string::npos);
    CHECK(w->warnings[0].find(dose.patient_name) == std::string::npos);  // no patient data
    const auto v = read_all(td / "m" / "DICOMDIR");
    CHECK_EQ(count_type(v, "RT DOSE"), 1u);
}
