// Regression tests for the 2026-11-10 02:15 security review, file output (R-071).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/io.hpp"
#include "test.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace siaara;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / (std::string("siaara-r071-") + name + "-" + std::to_string(
#if defined(_WIN32)
                                                    0
#else
                                                    static_cast<long>(::getpid())
#endif
                                                    ));
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

Status write(const fs::path& p, const std::string& s, io::WriteMode m = io::WriteMode::Replace) {
    return io::write_file_atomic(p.string(), reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), m);
}

std::size_t entries(const fs::path& d) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); ++it) ++n;
    return n;
}

}  // namespace

#if !defined(_WIN32)
TEST_CASE("R-071: write_file_atomic never writes through a symlink planted at the temporary name") {
    const fs::path d = fresh_dir("link");
    const fs::path victim = d / "victim.txt";
    spit(victim, "KEEP");
    const fs::path target = d / "out.dcm";
    // The old fixed temporary name, and a symlink at the target itself.
    std::error_code ec;
    fs::create_symlink(victim, d / "out.dcm.siaara-tmp", ec);
    REQUIRE(!ec);
    CHECK(write(target, "PHI-DATA"));
    CHECK_EQ(slurp(victim), std::string("KEEP"));
    CHECK_EQ(slurp(target), std::string("PHI-DATA"));
    fs::remove(target, ec);
    fs::create_symlink(victim, target, ec);
    REQUIRE(!ec);
    CHECK(write(target, "PHI-2"));
    CHECK_EQ(slurp(victim), std::string("KEEP"));
    CHECK(!fs::is_symlink(fs::symlink_status(target)));
    CHECK_EQ(slurp(target), std::string("PHI-2"));
    fs::remove_all(d, ec);
}

TEST_CASE("R-071: files written by write_file_atomic are owner-only") {
    const fs::path d = fresh_dir("mode");
    const mode_t old = ::umask(022);
    const fs::path target = d / "out.dcm";
    CHECK(write(target, "PHI"));
    ::umask(old);
    struct stat st {};
    REQUIRE(::stat(target.c_str(), &st) == 0);
    CHECK((st.st_mode & 077) == 0);
    std::error_code ec;
    fs::remove_all(d, ec);
}
#endif

TEST_CASE("R-071: concurrent writers of one target never clobber each other's temporary file") {
    const fs::path d = fresh_dir("race");
    const fs::path target = d / "out.dcm";
    constexpr int kThreads = 6, kRounds = 30;
    std::vector<std::string> payloads;
    for (int t = 0; t < kThreads; ++t) payloads.emplace_back(std::size_t{1} << 18, static_cast<char>('A' + t));
    std::atomic<int> failures{0}, torn{0}, unreadable{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t)
        pool.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r) {
                if (!write(target, payloads[static_cast<std::size_t>(t)])) ++failures;
                // Half the threads read like the toolkit (io::read_file shares read, write and
                // delete: never refused), half like other programs (std::ifstream; MSVC's opens
                // without FILE_SHARE_DELETE). On Windows such an open is refused while any rename
                // handle of the file exists: retried, as that is not a torn file.
                std::string now;
                if (t % 2 == 0) {
                    Result<Bytes> b = io::read_file(target.string());
                    if (!b) ++unreadable;
                    else now.assign(reinterpret_cast<const char*>(b.value().data()), b.value().size());
                } else {
                    bool opened = false;
                    for (int i = 0; i < 1000 && !opened; ++i) {
                        std::ifstream in(target, std::ios::binary);
                        if (!in.is_open()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }
                        opened = true;
                        now.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                    }
                    if (!opened) ++unreadable;
                }
                bool whole = false;
                for (const auto& p : payloads) whole = whole || now == p;
                if (!whole) ++torn;
            }
        });
    for (auto& th : pool) th.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(unreadable.load(), 0);
    CHECK_EQ(torn.load(), 0);
    CHECK_EQ(entries(d), 1u);  // no temporary files left behind
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("R-071: WriteMode::NoReplace keeps an existing file") {
    const fs::path d = fresh_dir("noreplace");
    const fs::path target = d / "out.dcm";
    CHECK(write(target, "FIRST", io::WriteMode::NoReplace));
    const Status s = write(target, "SECOND", io::WriteMode::NoReplace);
    CHECK(s.code == Errc::Rejected);
    CHECK_EQ(slurp(target), std::string("FIRST"));
    CHECK(write(target, "THIRD"));
    CHECK_EQ(slurp(target), std::string("THIRD"));
    CHECK_EQ(entries(d), 1u);
    std::error_code ec;
    fs::remove_all(d, ec);
}
