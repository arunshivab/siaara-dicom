// Repair tests: every category on deliberately broken files. Each test checks that the finding is
// repaired, that the change is logged, and that the input file is byte-identical afterwards.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <random>
#include <set>
#include <string>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "crosscheck/repair_samples.hpp"
#include "deid/sha256.hpp"
#include "image/frames.hpp"
#include "repair/repair.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::repair;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> n{0};
        std::random_device rd;
        path = fs::temp_directory_path() / ("siaara-repair-test-" + std::to_string(rd()) + "-" + std::to_string(++n));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return (path / name).string(); }
};

std::string hash_file(const std::string& p) {
    auto b = io::read_file(p);
    if (!b) return "unreadable";
    return sha256_hex(b->data(), b->size());
}

bool write(const std::string& p, const std::vector<std::uint8_t>& b) { return io::write_file_atomic(p, b.data(), b.size()).is_ok(); }

bool has_code(const std::vector<Finding>& f, std::string_view code) {
    return std::any_of(f.begin(), f.end(), [&](const Finding& x) { return x.code == code; });
}
bool has_change(const FileReport& r, std::uint32_t cat, std::string_view action) {
    return std::any_of(r.changes.begin(), r.changes.end(),
                       [&](const Change& c) { return c.category == cat && (action.empty() || c.action == action); });
}

// Writes `bytes` as an input, repairs it into a new file, and checks the owner's rules.
struct Run {
    TempDir dir;
    std::string in, out;
    Report rep;
    bool ok = false;
    Run(const std::vector<std::uint8_t>& bytes, std::uint32_t fix, RepairOptions opt = {}) {
        in = dir.file("input.dcm");
        out = dir.file("out/repaired.dcm");
        if (!write(in, bytes)) return;
        fs::create_directories(dir.path / "out");
        const std::string before = hash_file(in);
        opt.fix = fix;
        opt.write = true;
        if (opt.uid_seed.empty()) opt.uid_seed = "unit-test";
        auto r = repair_file(in, out, opt);
        if (!r) return;
        rep = std::move(r).value();
        ok = rep.files.size() == 1 && hash_file(in) == before && rep.files[0].input_unchanged &&
             rep.files[0].input_sha256 == before;
    }
    const FileReport& file() const { return rep.files[0]; }
};

}  // namespace

TEST_CASE("repair: every sample is repaired into a new file, input unchanged") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.fix.empty()) continue;
        std::uint32_t cats = 0;
        REQUIRE(parse_categories(s.fix, cats));
        Run run(s.bytes, cats);
        if (!run.ok) std::fprintf(stderr, "    %s: run failed\n", s.name.c_str());
        REQUIRE(run.ok);
        const FileReport& f = run.file();
        CHECK(f.written);
        CHECK(f.error.empty());
        CHECK(fs::exists(run.out));
        CHECK(!f.changes.empty());
        for (const std::string& code : s.gone_after) {
            const bool still = has_code(f.findings_after, code);
            if (still) std::fprintf(stderr, "    %s: %s still reported after the fix\n", s.name.c_str(), code.c_str());
            CHECK(!still);
        }
        // The output parses strictly with the core reader.
        ReadOptions strict;
        strict.lenient = false;
        auto back = read_file(run.out, strict);
        if (!back) std::fprintf(stderr, "    %s: strict read failed: %s\n", s.name.c_str(), back.error().message.c_str());
        CHECK(back.is_ok());
        CHECK(count(f.findings_after, Severity::Error) <= count(f.findings, Severity::Error));
    }
}

TEST_CASE("repair: dry run writes nothing and plans the changes") {
    const auto samples = repair_samples::all_samples();
    TempDir dir;
    const std::string in = dir.file("in.dcm"), out = dir.file("out.dcm");
    REQUIRE(write(in, samples[1].bytes));  // pixel_bits_allocated
    RepairOptions opt;
    opt.fix = kPixels;
    opt.write = false;
    auto r = repair_file(in, out, opt);
    REQUIRE(r.is_ok());
    CHECK(!fs::exists(out));
    CHECK(r->files[0].dry_run);
    CHECK(!r->files[0].written);
    CHECK(has_change(r->files[0], kPixels, "set"));
    CHECK(r->files[0].after_computed);
    CHECK(!has_code(r->files[0].findings_after, "pixel-length"));
    // Report only (no categories): no changes at all.
    opt.fix = 0;
    auto r2 = repair_file(in, "", opt);
    REQUIRE(r2.is_ok());
    CHECK(r2->files[0].changes.empty());
    CHECK(has_code(r2->files[0].findings, "pixel-length"));
}

TEST_CASE("repair: refuses to overwrite the input or an existing output") {
    TempDir dir;
    const std::string in = dir.file("in.dcm");
    REQUIRE(write(in, repair_samples::part10(repair_samples::base_sc())));
    const std::string before = hash_file(in);
    RepairOptions opt;
    opt.fix = kAllCategories;
    opt.write = true;
    CHECK(!repair_file(in, in, opt).is_ok());
    CHECK(!repair_file(in, (dir.path / "." / "in.dcm").string(), opt).is_ok());
    const std::string inputs[] = {in};
    CHECK(!repair_files(inputs, dir.path.string(), opt).is_ok());  // output name == input
    CHECK(hash_file(in) == before);
    const std::string out = dir.file("exists.dcm");
    REQUIRE(write(out, {1, 2, 3}));
    auto r = repair_file(in, out, opt);
    REQUIRE(r.is_ok());
    CHECK(!r->files[0].written);
    CHECK(!r->files[0].error.empty());
    CHECK(hash_file(out) == sha256_hex(std::vector<std::uint8_t>{1, 2, 3}.data(), 3));
    opt.overwrite = true;
    auto r2 = repair_file(in, out, opt);
    REQUIRE(r2.is_ok());
    CHECK(r2->files[0].written);
    CHECK(hash_file(in) == before);
}

TEST_CASE("repair: a clean file is written as read (keep-as-read)") {
    const auto bytes = repair_samples::part10(repair_samples::base_sc());
    Run run(bytes, kValues | kUids | kCharset | kAttributes | kPixels | kRescue);
    REQUIRE(run.ok);
    CHECK(run.file().changes.empty());
    auto out = io::read_file(run.out);
    REQUIRE(out.is_ok());
    CHECK(out->size() == bytes.size() && std::equal(bytes.begin(), bytes.end(), out->data()));
}

TEST_CASE("repair: structural fixes are logged in detail") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name == "struct_order_duplicate") {
            Run run(s.bytes, kStructural);
            REQUIRE(run.ok);
            const auto& ch = run.file().changes;
            const bool kept = std::any_of(ch.begin(), ch.end(), [](const Change& c) {
                return c.detail.find("keeps the copy at offset") != std::string::npos;
            });
            CHECK(kept);
        }
        if (s.name == "struct_meta") {
            Run run(s.bytes, kStructural);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->had_preamble);
            CHECK(f->meta.get_string(tags::MediaStorageSOPInstanceUID) == f->dataset.get_string(tags::SOPInstanceUID));
            CHECK(f->meta.contains(tags::ImplementationClassUID));
            // UIDs are listed in PS3.15 E.1-1: their values are withheld from the log.
            for (const Change& c : run.file().changes)
                if (!c.path.empty() && c.path.back().tag == tags::MediaStorageSOPInstanceUID) CHECK(c.values_withheld);
        }
        if (s.name == "struct_odd_and_group_length") {
            Run run(s.bytes, kStructural);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(!f->dataset.contains(0x00080000));  // dropped (retired)
            const Element* sid = f->dataset.find(0x00200010);
            REQUIRE(sid != nullptr);
            CHECK(sid->value.size() % 2 == 0);
            RepairOptions rec;
            rec.recompute_group_lengths = true;
            Run run2(s.bytes, kStructural, rec);
            REQUIRE(run2.ok);
            CHECK(!has_code(run2.file().findings_after, "group-length-mismatch"));
        }
    }
}

TEST_CASE("repair: values are normalised only when unambiguous") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name != "values_formats") continue;
        Run run(s.bytes, kValues);
        REQUIRE(run.ok);
        auto f = read_file(run.out);
        REQUIRE(f.is_ok());
        CHECK(f->dataset.get_string(0x00080020) == std::optional<std::string>("20240102"));
        CHECK(f->dataset.get_string(0x00080030) == std::optional<std::string>("101500"));
        CHECK(f->dataset.get_string(0x00200011) == std::optional<std::string>("7"));
        CHECK(f->dataset.get_string(0x00101010) == std::optional<std::string>("045Y"));
        CHECK(f->dataset.get_string(0x00181050) == std::optional<std::string>("1.25"));
        CHECK(f->dataset.get_string(0x00080023) == std::optional<std::string>("01/02/2024"));  // left alone
        const Element* se = f->dataset.find(tags::SeriesInstanceUID);
        REQUIRE(se != nullptr);
        CHECK(se->value.as_chars().find(' ') == std::string_view::npos);
        // Dates are not loggable: withheld.
        for (const Change& c : run.file().changes)
            if (!c.path.empty() && c.path.back().tag == 0x00080020) CHECK(c.values_withheld);
        // A detailed report shows them and is marked as containing PHI.
        RepairOptions d;
        d.detailed_report = true;
        Run run2(s.bytes, kValues, d);
        REQUIRE(run2.ok);
        CHECK(run2.rep.detailed);
        bool shown = false;
        for (const Change& c : run2.file().changes)
            if (!c.path.empty() && c.path.back().tag == 0x00080020) shown = c.old_value == "2024.01.02" && c.new_value == "20240102";
        CHECK(shown);
        CHECK(to_json(run2.rep).find("\"contains_phi\":true") != std::string::npos);
    }
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name != "values_vr") continue;
        Run run(s.bytes, kValues);
        REQUIRE(run.ok);
        auto f = read_file(run.out);
        REQUIRE(f.is_ok());
        CHECK(f->dataset.find(0x00200011)->vr == VR::IS);
        CHECK(f->dataset.find(0x00080060)->vr == VR::CS);
        CHECK(f->dataset.find(0x00280010)->vr == VR::US);
        CHECK(has_change(run.file(), kValues, "re-encode"));
    }
}

TEST_CASE("repair: invalid UIDs get consistent replacements") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name != "uids_invalid") continue;
        Run run(s.bytes, kUids);
        REQUIRE(run.ok);
        auto f = read_file(run.out);
        REQUIRE(f.is_ok());
        const auto study = f->dataset.get_string(tags::StudyInstanceUID);
        REQUIRE(study.has_value());
        CHECK(study->rfind("2.25.", 0) == 0);
        const Element* sq = f->dataset.find(0x00081110);
        REQUIRE(sq != nullptr);
        REQUIRE(sq->items.size() == 1);
        CHECK(sq->items[0].get_string(0x00081155) == study);       // reference kept consistent
        CHECK(sq->items[0].get_string(0x00081150) == std::optional<std::string>("1.2.840.10008.3.1.2.3.1"));  // class UID untouched
    }
}

TEST_CASE("repair: Specific Character Set") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name == "charset_utf8_undeclared") {
            Run run(s.bytes, kCharset);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->dataset.get_string(tags::SpecificCharacterSet) == std::optional<std::string>("ISO_IR 192"));
        }
        if (s.name == "charset_latin1_undeclared") {
            Run run(s.bytes, kCharset);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(!f->dataset.contains(tags::SpecificCharacterSet));  // ambiguous: flagged only
            RepairOptions o;
            o.charset = "ISO_IR 100";
            Run run2(s.bytes, kCharset, o);
            REQUIRE(run2.ok);
            auto g = read_file(run2.out);
            REQUIRE(g.is_ok());
            CHECK(g->dataset.get_string(tags::SpecificCharacterSet) == std::optional<std::string>("ISO_IR 100"));
            CHECK(!has_code(run2.file().findings_after, "charset-undeclared"));
        }
    }
}

TEST_CASE("repair: missing attributes, never clinical values") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name != "attributes_missing") continue;
        Run run(s.bytes, kAttributes);
        REQUIRE(run.ok);
        auto f = read_file(run.out);
        REQUIRE(f.is_ok());
        const Element* sd = f->dataset.find(0x00080020);
        REQUIRE(sd != nullptr);
        CHECK(sd->value.empty());
        REQUIRE(f->dataset.contains(0x00080090));
        auto series = f->dataset.get_string(tags::SeriesInstanceUID);
        REQUIRE(series.has_value());
        CHECK(series->rfind("2.25.", 0) == 0);
        CHECK(!f->dataset.contains(0x00080064));  // Type 1 clinical value: not invented
        CHECK(has_code(run.file().findings_after, "missing-type1"));
    }
}

TEST_CASE("repair: header-to-pixel consistency") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name == "pixel_bits_allocated") {
            Run run(s.bytes, kPixels);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->dataset.get_int(tags::BitsAllocated) == std::optional<std::int64_t>(16));
            CHECK(f->dataset.get_int(tags::BitsStored) == std::optional<std::int64_t>(8));
            for (const Change& c : run.file().changes)
                if (c.path.back().tag == tags::BitsAllocated) CHECK(c.old_value == "8" && c.new_value == "16");
        }
        if (s.name == "pixel_number_of_frames") {
            Run run(s.bytes, kPixels);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->dataset.get_int(tags::NumberOfFrames) == std::optional<std::int64_t>(3));
        }
        if (s.name == "pixel_j2k_header") {
            Run run(s.bytes, kPixels);
            REQUIRE(run.ok);
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->dataset.get_int(tags::Rows) == std::optional<std::int64_t>(16));
            CHECK(f->dataset.get_int(tags::BitsStored) == std::optional<std::int64_t>(12));
            CHECK(f->dataset.get_int(tags::HighBit) == std::optional<std::int64_t>(11));
            CHECK(f->dataset.get_int(tags::PixelRepresentation) == std::optional<std::int64_t>(0));
            auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
            REQUIRE(fr.is_ok());
            CHECK(fr->decode(0).is_ok());
        }
    }
}

TEST_CASE("repair: pixel rescue reports exactly which frames were recovered") {
    for (const auto& s : repair_samples::all_samples()) {
        if (s.name == "rescue_native_truncated") {
            Run run(s.bytes, kRescue);
            REQUIRE(run.ok);
            const FileReport& r = run.file();
            CHECK(r.rescue_attempted);
            CHECK(r.frames_declared == 4);
            CHECK((r.frames_recovered == std::vector<std::uint32_t>{0, 1}));
            CHECK((r.frames_lost == std::vector<std::uint32_t>{2, 3}));
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->dataset.get_int(tags::NumberOfFrames) == std::optional<std::int64_t>(2));
            auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
            REQUIRE(fr.is_ok());
            CHECK(fr->frame_count() == 2);
            CHECK(fr->decode(1).is_ok());
        }
        if (s.name == "rescue_rle_damaged") {
            Run run(s.bytes, kRescue);
            REQUIRE(run.ok);
            const FileReport& r = run.file();
            CHECK(r.frames_declared == 4);
            CHECK((r.frames_recovered == std::vector<std::uint32_t>{0, 2}));
            CHECK((r.frames_lost == std::vector<std::uint32_t>{1, 3}));
            auto f = read_file(run.out);
            REQUIRE(f.is_ok());
            CHECK(f->dataset.get_int(tags::NumberOfFrames) == std::optional<std::int64_t>(2));
            // Rescue + decompress: native output that opens everywhere.
            Run run2(s.bytes, kRescue | kDecompress);
            REQUIRE(run2.ok);
            auto g = read_file(run2.out);
            REQUIRE(g.is_ok());
            CHECK(g->transfer_syntax == std::string(ts::ExplicitVRLittleEndian));
            const Element* px = g->dataset.find(tags::PixelData);
            REQUIRE(px != nullptr);
            CHECK(px->value.size() == 2u * 8u * 8u);
            CHECK(has_change(run2.file(), kDecompress, "decompress"));
        }
    }
}

TEST_CASE("repair: study / series consistency across files") {
    TempDir dir;
    const auto files = repair_samples::study_set();
    std::vector<std::string> inputs, before;
    for (std::size_t i = 0; i < files.size(); ++i) {
        inputs.push_back(dir.file("in" + std::to_string(i) + ".dcm"));
        REQUIRE(write(inputs.back(), files[i]));
        before.push_back(hash_file(inputs.back()));
    }
    RepairOptions opt;
    opt.fix = kStudy | kAttributes;
    opt.write = true;
    opt.uid_seed = "study-test";
    auto r = repair_files(inputs, (dir.path / "out").string(), opt);
    REQUIRE(r.is_ok());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        CHECK(hash_file(inputs[i]) == before[i]);
        CHECK(r->files[i].input_unchanged);
    }
    const auto& sf = r->study_findings;
    CHECK(has_code(sf, "level-conflict"));
    CHECK(has_code(sf, "duplicate-sop-instance-uid"));
    CHECK(has_code(sf, "instance-number"));
    CHECK(has_code(sf, "study-uid-missing"));
    // Findings never name patients.
    CHECK(to_json(*r).find("Otherpatient") == std::string::npos);
    CHECK(to_json(*r).find(repair_samples::kPatientName) == std::string::npos);
    std::vector<DicomFile> out;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto f = read_file((dir.path / "out" / ("in" + std::to_string(i) + ".dcm")).string());
        REQUIRE(f.is_ok());
        out.push_back(std::move(f).value());
    }
    std::set<std::string> sops, studies;
    std::vector<std::int64_t> numbers;
    for (const auto& f : out) {
        CHECK(f.dataset.get_string(tags::PatientName) == std::optional<std::string>(repair_samples::kPatientName));
        sops.insert(f.dataset.get_string(tags::SOPInstanceUID).value_or(""));
        studies.insert(f.dataset.get_string(tags::StudyInstanceUID).value_or(""));
        numbers.push_back(f.dataset.get_int(0x00200013).value_or(-1));
        CHECK(f.meta.get_string(tags::MediaStorageSOPInstanceUID) == f.dataset.get_string(tags::SOPInstanceUID));
    }
    CHECK(sops.size() == 4);
    CHECK(studies.size() == 1);
    std::sort(numbers.begin(), numbers.end());
    CHECK((numbers == std::vector<std::int64_t>{1, 2, 3, 4}));
}

TEST_CASE("repair: in-memory repair") {
    const auto samples = repair_samples::all_samples();
    std::vector<std::uint8_t> out;
    RepairOptions opt;
    opt.fix = kStructural;
    opt.write = true;
    for (const auto& s : samples)
        if (s.name == "struct_raw_dataset") {
            const Report rep = repair_buffer(Bytes::copy_of(s.bytes.data(), s.bytes.size()), &out, opt);
            REQUIRE(rep.files.size() == 1);
            CHECK(rep.files[0].written);
            CHECK(out.size() > 132 && out[128] == 'D' && out[131] == 'M');
        }
}

#include "siaara/siaara.h"

TEST_CASE("repair: C API validate and repair") {
    TempDir dir;
    const auto samples = repair_samples::all_samples();
    const std::string in = dir.file("in.dcm");
    REQUIRE(write(in, samples[1].bytes));  // pixel_bits_allocated
    const std::string before = hash_file(in);
    size_t needed = 0;
    CHECK(siaara_validate_json(in.data(), in.size(), nullptr, 0, &needed) == SIAARA_OK);
    REQUIRE(needed > 0);
    std::string json(needed + 1, '\0');
    CHECK(siaara_validate_json(in.data(), in.size(), json.data(), json.size(), &needed) == SIAARA_OK);
    CHECK(json.find("pixel-length") != std::string::npos);
    char small[8];
    CHECK(siaara_validate_json(in.data(), in.size(), small, sizeof small, &needed) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(siaara_validate_memory_json(samples[0].bytes.data(), samples[0].bytes.size(), nullptr, 0, &needed) == SIAARA_OK);

    siaara_repair_options o{};
    o.struct_size = sizeof o;
    o.fix = SIAARA_REPAIR_PIXELS;
    o.write = 1;
    o.uid_seed = "capi";
    o.uid_seed_len = 4;
    const std::string out = dir.file("out.dcm");
    CHECK(siaara_repair_file(in.data(), in.size(), in.data(), in.size(), &o, nullptr, 0, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    std::string report(1 << 16, '\0');
    CHECK(siaara_repair_file(in.data(), in.size(), out.data(), out.size(), &o, report.data(), report.size(), &needed) ==
          SIAARA_OK);
    CHECK(report.find("\"written\":true") != std::string::npos);
    CHECK(report.find("\"input_unchanged\":true") != std::string::npos);
    CHECK(fs::exists(out));
    CHECK(hash_file(in) == before);
    // The output exists now: refused without overwrite (the report is still filled in).
    CHECK(siaara_repair_file(in.data(), in.size(), out.data(), out.size(), &o, report.data(), report.size(), &needed) ==
          SIAARA_ERR_REJECTED);
    o.fix = 1u << 20;
    CHECK(siaara_repair_file(in.data(), in.size(), out.data(), out.size(), &o, nullptr, 0, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    // Dry run with an old, shorter struct (only struct_size and fix): nothing written.
    siaara_repair_options old{};
    old.struct_size = static_cast<uint32_t>(offsetof(siaara_repair_options, write));
    old.fix = SIAARA_REPAIR_PIXELS;
    const std::string out2 = dir.file("out2.dcm");
    CHECK(siaara_repair_file(in.data(), in.size(), out2.data(), out2.size(), &old, nullptr, 0, nullptr) == SIAARA_OK);
    CHECK(!fs::exists(out2));
    CHECK(hash_file(in) == before);
}

TEST_CASE("repair: junk after an unterminated pixel sequence is not turned into an element (testing2)") {
    // gdcmData gdcm-JPEG-LossLess3a.dcm: 8 bytes after the last fragment, no Sequence Delimitation
    // Item. They were read as an element (B00C,0EB6) and written to the output.
    auto u16 = [](std::vector<std::uint8_t>& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&](std::vector<std::uint8_t>& b, std::uint32_t v) { u16(b, v & 0xFFFFu); u16(b, v >> 16); };
    auto el = [&](std::vector<std::uint8_t>& b, Tag t, const char* vr, std::string v) {
        if (v.size() & 1u) v.push_back(' ');
        u16(b, tag_group(t)); u16(b, t & 0xFFFFu);
        b.push_back(static_cast<std::uint8_t>(vr[0])); b.push_back(static_cast<std::uint8_t>(vr[1]));
        u16(b, static_cast<unsigned>(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    };
    std::vector<std::uint8_t> b(128, 0);
    b.insert(b.end(), {'D', 'I', 'C', 'M'});
    std::vector<std::uint8_t> meta;
    el(meta, 0x00020010, "UI", std::string("1.2.840.10008.1.2.4.70") + '\0');
    u16(b, 2); u16(b, 0); b.push_back('U'); b.push_back('L'); u16(b, 4); u32(b, static_cast<std::uint32_t>(meta.size()));
    b.insert(b.end(), meta.begin(), meta.end());
    el(b, 0x00080016, "UI", std::string("1.2.840.10008.5.1.4.1.1.7") + '\0');
    el(b, 0x00080018, "UI", "2.25.31337");
    el(b, 0x00100010, "PN", "DOE^JANE");
    u16(b, 0x7FE0); u16(b, 0x0010); b.push_back('O'); b.push_back('B'); u16(b, 0); u32(b, 0xFFFFFFFFu);
    u16(b, 0xFFFE); u16(b, 0xE000); u32(b, 0);
    u16(b, 0xFFFE); u16(b, 0xE000); u32(b, 4);
    b.insert(b.end(), {0xFF, 0xD8, 0xFF, 0xD9});
    b.insert(b.end(), {0x0C, 0xB0, 0xB6, 0x0E, 0x50, 0xD1, 0x02, 0x0E});
    Run run(b, kStructural | kRescue);
    REQUIRE(run.ok);
    bool reported = false;
    for (const auto& f : run.file().findings)
        if (f.code == "trailing-bytes") reported = true;
    CHECK(reported);
    auto out = read_file(run.out);
    REQUIRE(out.is_ok());
    CHECK(!out->dataset.contains(0xB00C0EB6));
    CHECK(out->dataset.contains(tags::PixelData));
    CHECK_EQ(out->dataset.get_string(tags::PatientName).value_or(""), std::string("DOE^JANE"));
}
