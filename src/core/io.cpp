#include "core/io.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

Result<Bytes> map_file(const std::string& utf8_path) {
    const std::wstring w = widen(utf8_path);
    if (w.empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    auto m = std::make_shared<WinMapping>();
    m->file = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
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
    FILE* f = _wfopen(w.c_str(), L"rb");
    if (!f) return Status(Errc::IoError, "cannot open file");
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

Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size) {
    const std::wstring w = widen(utf8_path);
    if (w.empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    const std::wstring tmp = w + L".siaara-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status(Errc::IoError, "cannot create file");
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, data + done, chunk, &wrote, nullptr) || wrote == 0) {
            CloseHandle(h);
            DeleteFileW(tmp.c_str());
            return Status(Errc::IoError, "write failed");
        }
        done += wrote;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!MoveFileExW(tmp.c_str(), w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
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

Result<Bytes> map_file(const std::string& utf8_path) {
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
#if defined(MADV_SEQUENTIAL)
    madvise(m->addr, len, MADV_WILLNEED);
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

Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size) {
    const std::string tmp = utf8_path + ".siaara-tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Status(Errc::IoError, "cannot create file");
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, data + done, size - done);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return Status(Errc::IoError, "write failed");
        }
        done += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), utf8_path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return Status(Errc::IoError, "rename failed");
    }
    return Status::ok();
}

#endif

}  // namespace siaara::io
