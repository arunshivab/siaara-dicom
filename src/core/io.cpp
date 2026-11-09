#include "core/io.hpp"

#include "core/secure_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace siaara::io {

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

struct WinMapping {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE map = nullptr;
    const void* view = nullptr;
    ~WinMapping() {
        if (view) UnmapViewOfFile(view);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};
}  // namespace

Result<Bytes> map_file(const std::string& utf8_path, bool read_ahead) {
    const std::wstring w = widen(utf8_path);
    if (w.empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    auto m = std::make_shared<WinMapping>();
    // FILE_SHARE_DELETE lets other code rename or delete the file while it is mapped here.
    m->file = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (m->file == INVALID_HANDLE_VALUE) return Status(Errc::IoError, "cannot open file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(m->file, &size)) return Status(Errc::IoError, "cannot get file size");
    if (size.QuadPart == 0) return Bytes{};
    m->map = CreateFileMappingW(m->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m->map) return read_file(utf8_path);
    m->view = MapViewOfFile(m->map, FILE_MAP_READ, 0, 0, 0);
    if (!m->view) return read_file(utf8_path);
    const auto* p = static_cast<const std::uint8_t*>(m->view);
    return Bytes(std::shared_ptr<const void>(m, m->view), p,
                 static_cast<std::size_t>(size.QuadPart));
}

Result<Bytes> read_file(const std::string& utf8_path) {
    const std::wstring w = widen(utf8_path);
    if (w.empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    FILE* f = nullptr;
    if (_wfopen_s(&f, w.c_str(), L"rb") != 0 || !f) return Status(Errc::IoError, "cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> guard(f, &std::fclose);
    if (_fseeki64(f, 0, SEEK_END) != 0) return Status(Errc::IoError, "seek failed");
    const long long len = _ftelli64(f);
    if (len < 0) return Status(Errc::IoError, "tell failed");
    _fseeki64(f, 0, SEEK_SET);
    auto store = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(len));
    if (len > 0 && std::fread(store->data(), 1, store->size(), f) != store->size())
        return Status(Errc::IoError, "read failed");
    const std::uint8_t* p = store->data();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, store->size());
}

namespace {
// The folder part of a path ("" when there is none), with its trailing separator.
std::string folder_of(const std::string& p) {
    const std::size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}
}  // namespace

Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size, WriteMode mode) {
    const std::wstring w = widen(utf8_path);
    if (w.empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    ExclusiveFile f;
    std::string tmp;
    for (int attempt = 0;; ++attempt) {
        tmp = folder_of(utf8_path) + unique_name(".siaara-tmp-", ".part");
        bool exists = false;
        Status s = f.create(tmp, &exists);
        if (s) break;
        if (!exists || attempt >= 8) return Status(Errc::IoError, "cannot create file");
    }
    const std::wstring wtmp = widen(tmp);
    Status st = f.write(data, size);
    if (st && !f.sync()) st = Status(Errc::IoError, "flush failed");
    if (!f.close() && st) st = Status(Errc::IoError, "close failed");
    if (!st) {
        DeleteFileW(wtmp.c_str());
        return Status(Errc::IoError, "write failed");
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == WriteMode::Replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(wtmp.c_str(), w.c_str(), flags)) {
        const DWORD e = GetLastError();
        DeleteFileW(wtmp.c_str());
        if (mode == WriteMode::NoReplace && (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS))
            return Status(Errc::Rejected, "file exists");
        return Status(Errc::IoError, "rename failed");
    }
    return Status::ok();
}

#else  // POSIX

namespace {
struct PosixMapping {
    void* addr = MAP_FAILED;
    std::size_t len = 0;
    ~PosixMapping() {
        if (addr != MAP_FAILED) munmap(addr, len);
    }
};
}  // namespace

Result<Bytes> map_file(const std::string& utf8_path, bool read_ahead) {
    const int fd = ::open(utf8_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Status(Errc::IoError, "cannot open file");
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return Status(Errc::IoError, "cannot stat file");
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return Status(Errc::IoError, "not a regular file");
    }
    const auto len = static_cast<std::size_t>(st.st_size);
    if (len == 0) {
        ::close(fd);
        return Bytes{};
    }
    auto m = std::make_shared<PosixMapping>();
    m->addr = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m->addr == MAP_FAILED) return read_file(utf8_path);
    m->len = len;
#if defined(MADV_WILLNEED)
    if (read_ahead) madvise(m->addr, len, MADV_WILLNEED);
#endif
    const auto* p = static_cast<const std::uint8_t*>(m->addr);
    return Bytes(std::shared_ptr<const void>(m, m->addr), p, len);
}

Result<Bytes> read_file(const std::string& utf8_path) {
    const int fd = ::open(utf8_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Status(Errc::IoError, "cannot open file");
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return Status(Errc::IoError, "cannot stat file");
    }
    auto store = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < store->size()) {
        const ssize_t n = ::read(fd, store->data() + done, store->size() - done);
        if (n <= 0) {
            ::close(fd);
            return Status(Errc::IoError, "read failed");
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    const std::uint8_t* p = store->data();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, store->size());
}

namespace {
std::string folder_of(const std::string& p) {
    const std::size_t k = p.rfind('/');
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}

// Exclusive create of `path` itself, then write: the no-replace fallback for file systems
// without hard links. Not atomic against crashes, but never replaces an existing file.
Status write_exclusive_in_place(const std::string& path, const std::uint8_t* data, std::size_t size) {
    ExclusiveFile f;
    bool exists = false;
    if (Status s = f.create(path, &exists); !s) return exists ? Status(Errc::Rejected, "file exists") : s;
    Status st = f.write(data, size);
    if (st && !f.sync()) st = Status(Errc::IoError, "flush failed");
    if (!f.close() && st) st = Status(Errc::IoError, "close failed");
    if (!st) ::unlink(path.c_str());
    return st;
}
}  // namespace

Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size, WriteMode mode) {
    if (utf8_path.empty()) return Status(Errc::InvalidArgument, "path is empty");
    ExclusiveFile f;
    std::string tmp;
    for (int attempt = 0;; ++attempt) {
        tmp = folder_of(utf8_path) + unique_name(".siaara-tmp-", ".part");
        bool exists = false;
        Status s = f.create(tmp, &exists);
        if (s) break;
        if (!exists || attempt >= 8) return Status(Errc::IoError, "cannot create file");
    }
    Status st = f.write(data, size);
    if (st && !f.sync()) st = Status(Errc::IoError, "flush failed");
    if (!f.close() && st) st = Status(Errc::IoError, "close failed");
    if (!st) {
        ::unlink(tmp.c_str());
        return st;
    }
    if (mode == WriteMode::Replace) {
        // rename() replaces a symlink at the target itself, it never writes through it.
        if (::rename(tmp.c_str(), utf8_path.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return Status(Errc::IoError, "rename failed");
        }
        return Status::ok();
    }
    // No replace: link() fails with EEXIST if the target exists (also a dangling symlink), with
    // no window between a check and the write.
    if (::link(tmp.c_str(), utf8_path.c_str()) == 0) {
        ::unlink(tmp.c_str());
        return Status::ok();
    }
    const int e = errno;
    ::unlink(tmp.c_str());
    if (e == EEXIST) return Status(Errc::Rejected, "file exists");
    if (e == EPERM || e == ENOTSUP || e == EOPNOTSUPP || e == EMLINK || e == ENOSYS)
        return write_exclusive_in_place(utf8_path, data, size);
    return Status(Errc::IoError, "rename failed");
}

#endif

}  // namespace siaara::io
