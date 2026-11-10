// Regression tests for the 2026-11-10 02:15 security review, OS library loading (R-021, R-063).
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/system_library.hpp"
#include "test.hpp"

namespace fs = std::filesystem;

TEST_CASE("R-021: libraries are loaded only from owner-controlled, non-writable files and folders") {
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / ("siaara-r021-" + std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path f = d / "libfake.so.1";
    std::ofstream(f) << "x";
    std::string resolved;
    ::chmod(d.c_str(), 0755);
    ::chmod(f.c_str(), 0644);
    CHECK(siaara::sys::trusted_file(f.string(), resolved));
    ::chmod(f.c_str(), 0664);  // group writable file
    CHECK(!siaara::sys::trusted_file(f.string(), resolved));
    ::chmod(f.c_str(), 0644);
    ::chmod(d.c_str(), 0777);  // world writable folder (like /tmp)
    CHECK(!siaara::sys::trusted_file(f.string(), resolved));
    ::chmod(d.c_str(), 0755);
    // A symlink is judged by what it points to.
    const fs::path link = d / "libalias.so.1";
    fs::create_symlink(f, link, ec);
    CHECK(siaara::sys::trusted_file(link.string(), resolved));
    CHECK_EQ(resolved, fs::canonical(f).string());
    CHECK(!siaara::sys::trusted_file((d / "missing.so").string(), resolved));
    std::string path;
    CHECK(siaara::sys::open_system_library("libsiaara-does-not-exist.so.9", path) == nullptr);
    fs::remove_all(d, ec);
}

TEST_CASE("R-063: the system libc loads through the hardened path from an absolute folder") {
    std::string path;
    void* h = siaara::sys::open_system_library("libc.so.6", path);
#if defined(__linux__)
    REQUIRE(h != nullptr);
    CHECK(!path.empty() && path[0] == '/');
#endif
    if (h) ::dlclose(h);
}
#endif
