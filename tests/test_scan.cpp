// Folder scan tests: DICOM / raw / non-DICOM detection, threads, cancellation, limits,
// DICOMDIR mode, symbolic links, C API.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/transfer_syntax.hpp"
#include "crosscheck/store_synth.hpp"
#include "siaara/siaara.h"
#include "store/dicomdir.hpp"
#include "store/scan.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::store;
using store_synth::Spec;
using store_synth::TempDir;
namespace fs = std::filesystem;

namespace {

// 12 DICOM files in nested folders (2 raw, 1 implicit VR), plus junk.
void populate(const fs::path& root) {
    for (int i = 0; i < 12; ++i) {
        Spec s;
        s.sop_uid = "1.2.99." + std::to_string(i);
        s.series_uid = "1.2.99.100." + std::to_string(i % 3);
        s.instance_number = i;
        if (i == 3 || i == 7) {
            s.preamble = false;
            s.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
        }
        if (i == 5) s.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
        const fs::path sub = root / ("d" + std::to_string(i % 4)) / ("e" + std::to_string(i % 2));
        store_synth::write_spec(sub / ("IM" + std::to_string(i)), s);
    }
    store_synth::write_bytes(root / "readme.txt", {'h', 'e', 'l', 'l', 'o', '\n'});
    store_synth::write_bytes(root / "empty", {});
    store_synth::write_bytes(root / "tiny", {0x08, 0x00});
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.resize(300, 0);
    store_synth::write_bytes(root / "d1" / "picture.png", png);
    std::vector<std::uint8_t> fake(200, 0);  // 128 zero bytes then not "DICM"
    store_synth::write_bytes(root / "d2" / "zeros.bin", fake);
    std::vector<std::uint8_t> broken(132, 0);  // preamble + DICM, then nothing usable
    broken[128] = 'D';
    broken[129] = 'I';
    broken[130] = 'C';
    broken[131] = 'M';
    broken.insert(broken.end(), {0x02, 0x00, 0x10, 0x00, 'U', 'I', 0xFF, 0x7F});
    store_synth::write_bytes(root / "d3" / "broken.dcm", broken);
}

struct Collect {
    std::mutex m;
    std::map<std::string, ScanItem> items;
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    bool operator()(const ScanItem& it) {
        const int c = ++concurrent;
        if (c > max_concurrent) max_concurrent = c;
        {
            std::lock_guard<std::mutex> g(m);
            items[it.path] = it;
        }
        std::this_thread::yield();
        --concurrent;
        return true;
    }
};

}  // namespace

TEST_CASE("scan: finds DICOM files, skips the rest, on 1 and 4 threads") {
    TempDir td("siaara-scan");
    populate(td.path());
    for (unsigned threads : {1u, 4u}) {
        Collect c;
        ScanOptions o;
        o.threads = threads;
        auto r = scan_folder(td.str(), [&](const ScanItem& it) { return c(it); }, o);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->files, 18u);
        CHECK_EQ(r->instances, 12u);
        CHECK_EQ(r->errors, 1u);   // broken.dcm
        CHECK_EQ(r->skipped, 5u);  // txt, empty, tiny, png, zeros
        CHECK_EQ(c.items.size(), 13u);
        CHECK_EQ(c.max_concurrent.load(), 1);  // callbacks are serialised
        std::set<std::string> uids;
        for (const auto& [p, it] : c.items) {
            if (it.kind == ScanKind::Instance) {
                uids.insert(it.record[Str::SopInstanceUid]);
                CHECK(it.record.file_size == fs::file_size(p));
            } else {
                CHECK(it.kind == ScanKind::Error);
                CHECK(!it.status.is_ok());
                CHECK(p.find("broken.dcm") != std::string::npos);
            }
        }
        CHECK_EQ(uids.size(), 12u);
    }
    // strict: no raw data sets
    ScanOptions strict;
    strict.allow_raw = false;
    auto r = scan_folder(td.str(), nullptr, strict);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->instances, 10u);
    // report_skipped
    Collect all;
    ScanOptions rep;
    rep.report_skipped = true;
    r = scan_folder(td.str(), [&](const ScanItem& it) { return all(it); }, rep);
    REQUIRE(r.is_ok());
    CHECK_EQ(all.items.size(), 18u);
}

TEST_CASE("scan: cancellation, file limit, non-recursive") {
    TempDir td("siaara-scan2");
    populate(td.path());
    std::atomic<int> calls{0};
    ScanOptions o;
    o.threads = 3;
    auto r = scan_folder(td.str(), [&](const ScanItem&) { return ++calls < 3; }, o);
    CHECK(!r.is_ok() && r.error().code == Errc::Cancelled);
    CHECK_EQ(calls.load(), 3);
    ScanOptions lim;
    lim.max_files = 4;
    r = scan_folder(td.str(), nullptr, lim);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->files, 4u);
    ScanOptions flat;
    flat.recursive = false;
    r = scan_folder(td.str(), nullptr, flat);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->files, 3u);  // readme.txt, empty, tiny
    CHECK_EQ(r->instances, 0u);
    CHECK(scan_folder((td / "nope").string(), nullptr).error().code == Errc::NotFound);
}

TEST_CASE("scan: DICOMDIR files are reported as such; use_dicomdir scans only listed files") {
    TempDir td("siaara-scan3");
    std::vector<std::string> files;
    for (int i = 0; i < 4; ++i) {
        Spec s;
        s.sop_uid = "1.2.98." + std::to_string(i);
        const fs::path p = td / "src" / ("f" + std::to_string(i) + ".dcm");
        store_synth::write_spec(p, s);
        files.push_back(p.string());
    }
    REQUIRE(write_dicomdir((td / "media").string(), files).is_ok());
    store_synth::write_spec(td / "media" / "EXTRA", Spec{});  // not listed in the DICOMDIR
    auto r = scan_folder((td / "media").string(), nullptr);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->dicomdirs, 1u);
    CHECK_EQ(r->instances, 5u);
    CHECK(!r->used_dicomdir);
    ScanOptions o;
    o.use_dicomdir = true;
    std::set<std::string> uids;
    std::mutex m;
    r = scan_folder((td / "media").string(), [&](const ScanItem& it) {
        std::lock_guard<std::mutex> g(m);
        if (it.kind == ScanKind::Instance) uids.insert(it.record[Str::SopInstanceUid]);
        return true;
    }, o);
    REQUIRE(r.is_ok());
    CHECK(r->used_dicomdir);
    CHECK_EQ(r->dicomdirs, 1u);
    CHECK_EQ(r->instances, 4u);
    CHECK_EQ(uids.size(), 4u);
}

TEST_CASE("scan: symbolic links are not followed unless asked; loops terminate") {
    TempDir td("siaara-scan4");
    store_synth::write_spec(td / "a" / "IM1", Spec{});
    store_synth::write_spec(td / "outside" / "IM2", Spec{});
    std::error_code ec;
    fs::create_directory_symlink(td / "outside", td / "a" / "link", ec);
    if (ec) {
        std::printf("  (symbolic links unavailable: skipped)\n");
        return;
    }
    fs::create_directory_symlink(td / "a", td / "a" / "loop", ec);
    auto r = scan_folder((td / "a").string(), nullptr);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->instances, 1u);
    ScanOptions f;
    f.follow_symlinks = true;
    f.max_files = 1000;  // the loop is cut by the iterator or, at worst, by the limit
    r = scan_folder((td / "a").string(), nullptr, f);
    if (r) CHECK(r->instances >= 2u);
}

TEST_CASE("scan: C API") {
    TempDir td("siaara-scan5");
    populate(td.path());
    struct Acc {
        int instances = 0, errors = 0, other = 0;
        bool rec_ok = true;
    } acc;
    auto cb = [](void* user, const char* path, int32_t kind, siaara_status status, const siaara_index_record* rec,
                 const char* strings) -> int32_t {
        auto* a = static_cast<Acc*>(user);
        if (kind == SIAARA_SCAN_INSTANCE) {
            ++a->instances;
            if (!rec || !strings || std::string(strings + rec->str_offset[SIAARA_IDX_SOP_INSTANCE_UID]).rfind("1.2.99.", 0) != 0)
                a->rec_ok = false;
        } else if (kind == SIAARA_SCAN_ERROR) {
            ++a->errors;
            if (status == SIAARA_OK || rec) a->rec_ok = false;
        } else {
            ++a->other;
        }
        return path ? 0 : 1;
    };
    siaara_scan_options o{};
    o.struct_size = sizeof o;
    o.threads = 2;
    o.recursive = 1;
    o.allow_raw = 1;
    siaara_scan_stats st{};
    st.struct_size = sizeof st;
    const std::string root = td.str();
    CHECK_EQ(siaara_folder_scan(root.c_str(), root.size(), &o, cb, &acc, &st), SIAARA_OK);
    CHECK_EQ(acc.instances, 12);
    CHECK_EQ(acc.errors, 1);
    CHECK_EQ(acc.other, 0);
    CHECK(acc.rec_ok);
    CHECK_EQ(st.files, 18u);
    CHECK_EQ(st.instances, 12u);
    CHECK_EQ(siaara_folder_scan(root.c_str(), root.size(), nullptr, nullptr, nullptr, nullptr), SIAARA_OK);
    auto stop = [](void*, const char*, int32_t, siaara_status, const siaara_index_record*, const char*) -> int32_t { return 1; };
    CHECK_EQ(siaara_folder_scan(root.c_str(), root.size(), nullptr, stop, nullptr, nullptr), SIAARA_ERR_CANCELLED);
    o.struct_size = 4;
    CHECK_EQ(siaara_folder_scan(root.c_str(), root.size(), &o, cb, &acc, nullptr), SIAARA_ERR_INVALID_ARGUMENT);
}
