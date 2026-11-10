// SIAARA DICOM Toolkit - loading optional OS libraries (OpenSSL 3, the Vulkan loader) on POSIX
// systems: only from fixed absolute system folders, and only files that an attacker without the
// owner's rights cannot have replaced (R-021, R-063). Internal.
//
// Only absolute paths built from kLibDirs are ever passed to dlopen: LD_LIBRARY_PATH,
// DYLD_LIBRARY_PATH, the executable's RUNPATH/RPATH and the current directory play no part, so a
// planted library cannot run its constructors in this process (dlopen runs them before any check
// after it could). Before dlopen the file (after resolving symlinks) and its folder must be owned
// by root or by the effective user and must not be writable by group or others. A folder holding
// a library of the wrong ELF class fails dlopen without running anything; the next one is tried.
#pragma once

#if !defined(_WIN32)

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace siaara::sys {

#if defined(__x86_64__) && defined(__ILP32__)
#define SIAARA_LIB_TRIPLET "x86_64-linux-gnux32"
#elif defined(__x86_64__)
#define SIAARA_LIB_TRIPLET "x86_64-linux-gnu"
#elif defined(__i386__)
#define SIAARA_LIB_TRIPLET "i386-linux-gnu"
#elif defined(__aarch64__)
#define SIAARA_LIB_TRIPLET "aarch64-linux-gnu"
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
#define SIAARA_LIB_TRIPLET "arm-linux-gnueabihf"
#elif defined(__arm__)
#define SIAARA_LIB_TRIPLET "arm-linux-gnueabi"
#elif defined(__riscv) && __riscv_xlen == 64
#define SIAARA_LIB_TRIPLET "riscv64-linux-gnu"
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define SIAARA_LIB_TRIPLET "powerpc64le-linux-gnu"
#elif defined(__s390x__)
#define SIAARA_LIB_TRIPLET "s390x-linux-gnu"
#elif defined(__loongarch64)
#define SIAARA_LIB_TRIPLET "loongarch64-linux-gnu"
#endif

inline const char* const kLibDirs[] = {
#if defined(SIAARA_LIB_TRIPLET)
    "/usr/lib/" SIAARA_LIB_TRIPLET "/",
    "/lib/" SIAARA_LIB_TRIPLET "/",
#endif
#if UINTPTR_MAX > 0xFFFFFFFFu
    "/usr/lib64/",
    "/lib64/",
#else
    "/usr/lib32/",
#endif
    "/usr/lib/",
    "/lib/",
    "/usr/local/lib64/",  // only when root- (or own-) owned and not group/world writable
    "/usr/local/lib/",
#if defined(__APPLE__)
    "/opt/homebrew/lib/",
#endif
};


// True when `path` (resolved) and its folder are owned by root or the effective user and not
// writable by group or others.
inline bool trusted_file(const std::string& path, std::string& resolved) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) return false;
    resolved = buf;
    const uid_t me = ::geteuid();
    auto ok = [me](const struct stat& st) {
        return (st.st_uid == 0 || st.st_uid == me) && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
    };
    struct stat f {};
    if (::stat(buf, &f) != 0 || !S_ISREG(f.st_mode) || !ok(f)) return false;
    const std::string::size_type slash = resolved.rfind('/');
    const std::string dir = slash == 0 ? std::string("/") : resolved.substr(0, slash);
    struct stat d {};
    return ::stat(dir.c_str(), &d) == 0 && S_ISDIR(d.st_mode) && ok(d);
}

// dlopen of `soname` from the first system folder that has a trusted, loadable copy, or null.
// `path` receives the path opened.
inline void* open_system_library(const char* soname, std::string& path) {
    for (const char* dir : kLibDirs) {
        const std::string p = std::string(dir) + soname;
        std::string resolved;
        if (!trusted_file(p, resolved)) continue;
        if (void* h = ::dlopen(resolved.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            path = resolved;
            return h;
        }
    }
    return nullptr;
}

}  // namespace siaara::sys

#endif
