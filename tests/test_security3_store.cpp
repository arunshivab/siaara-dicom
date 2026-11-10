// Regression tests for the round-3 security review, local store file output (R-092): the store's
// own writes never follow a planted symbolic link, use unpredictable temporary names, and create
// owner-only folders and files unless StoreOptions::file_mode says otherwise.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "store/store.hpp"
#include "test.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace siaara;
namespace fs = std::filesystem;

#if !defined(_WIN32)
namespace {

fs::path fresh_dir(const char* name) {
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) /
                 (std::string("siaara-r092-") + name + "-" + std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

Bytes instance(int i) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.dataset.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)f.dataset.set_string(tags::SOPInstanceUID, VR::UI, "2.25.9200" + std::to_string(i));
    (void)f.dataset.set_string(tags::StudyInstanceUID, VR::UI, "2.25.92101");
    (void)f.dataset.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.92102");
    (void)f.dataset.set_string(tags::PatientID, VR::LO, "R092");
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return Bytes::copy_of(out.data(), out.size());
}

unsigned mode_of(const fs::path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) return 0xFFFFu;
    return static_cast<unsigned>(st.st_mode & 07777);
}

}  // namespace

TEST_CASE("R-092: store folders and files are owner-only by default") {
    const fs::path d = fresh_dir("mode");
    const mode_t old = ::umask(022);
    const fs::path root = d / "store";
    {
        auto s = store::Store::open(root.string());
        REQUIRE(s);
        REQUIRE((*s)->add_buffer(instance(1)));
        REQUIRE((*s)->compact());
    }
    ::umask(old);
    for (const fs::path& p : {root, root / "index", root / "data"}) CHECK_EQ(mode_of(p), 0700u);
    const fs::path file = root / store::instance_relative_path("2.25.92001");
    CHECK_EQ(mode_of(file), 0600u);
    CHECK_EQ(mode_of(file.parent_path()), 0700u);
    CHECK_EQ(mode_of(file.parent_path().parent_path()), 0700u);
    CHECK_EQ(mode_of(root / "siaara-store.lock"), 0600u);
    CHECK_EQ(mode_of(root / "index" / "journal.sxj"), 0600u);
    CHECK_EQ(mode_of(root / "index" / "snapshot.sxs"), 0600u);
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("R-092: an explicit file_mode is honoured") {
    const fs::path d = fresh_dir("filemode");
    const mode_t old = ::umask(022);
    const fs::path root = d / "store";
    store::StoreOptions o;
    o.file_mode = 0640;
    {
        auto s = store::Store::open(root.string(), o);
        REQUIRE(s);
        REQUIRE((*s)->add_buffer(instance(1)));
    }
    ::umask(old);
    CHECK_EQ(mode_of(root), 0750u);
    CHECK_EQ(mode_of(root / store::instance_relative_path("2.25.92001")), 0640u);
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("R-092: a symlink at the snapshot's old temporary name is never written through") {
    const fs::path d = fresh_dir("snap");
    const fs::path root = d / "store";
    const fs::path victim = d / "victim.txt";
    spit(victim, "KEEP");
    {
        auto s = store::Store::open(root.string());
        REQUIRE(s);
    }
    std::error_code ec;
    fs::create_symlink(victim, root / "index" / "snapshot.sxs.siaara-tmp", ec);
    REQUIRE(!ec);
    {
        auto s = store::Store::open(root.string());
        REQUIRE(s);
        REQUIRE((*s)->add_buffer(instance(1)));
        CHECK((*s)->compact());
    }
    CHECK_EQ(slurp(victim), std::string("KEEP"));
    fs::remove_all(d, ec);
}

TEST_CASE("R-092: a symlink in place of the lock file or the journal is refused") {
    const fs::path d = fresh_dir("lock");
    const fs::path root = d / "store";
    const fs::path victim = d / "victim.txt";
    spit(victim, "KEEP");
    {
        auto s = store::Store::open(root.string());
        REQUIRE(s);
    }
    std::error_code ec;
    fs::remove(root / "siaara-store.lock", ec);
    fs::create_symlink(victim, root / "siaara-store.lock", ec);
    REQUIRE(!ec);
    CHECK(!store::Store::open(root.string()));
    CHECK_EQ(slurp(victim), std::string("KEEP"));

    // Journal: a copy of the real one behind a link must not be appended to.
    fs::remove(root / "siaara-store.lock", ec);
    const fs::path journal = root / "index" / "journal.sxj";
    const std::string real = slurp(journal);
    spit(victim, real);
    fs::remove(journal, ec);
    fs::create_symlink(victim, journal, ec);
    REQUIRE(!ec);
    {
        auto s = store::Store::open(root.string());
        if (s) (void)(*s)->add_buffer(instance(2));
    }
    CHECK(slurp(victim) == real);
    fs::remove_all(d, ec);
}
#endif
