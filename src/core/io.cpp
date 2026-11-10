#include "core/io.hpp"

#include "core/secure_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

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

namespace {
// Writes the pieces in order, in chunks; `observe` sees each chunk first.
Status write_slices(ExclusiveFile& f, const Slice* parts, std::size_t part_count,
                    const std::function<void(const std::uint8_t*, std::size_t)>& observe) {
    for (std::size_t i = 0; i < part_count; ++i) {
        const std::uint8_t* p = parts[i].data;
        std::size_t left = parts[i].size;
        while (left > 0) {
            const std::size_t n = std::min(left, kWriteChunk);
            if (observe) observe(p, n);
            if (Status s = f.write(p, n); !s) return s;
            p += n;
            left -= n;
        }
    }
    return Status::ok();
}
}  // namespace

// ---------------------------------------------------------------- live mappings (R-160)
namespace {
struct MappingEntry {
    std::size_t len = 0;
    std::string path;
    std::uint64_t id_a = 0, id_b = 0;
    void* handle = nullptr;
};
struct MappingRegistry {
    std::mutex mu;
    std::map<const std::uint8_t*, MappingEntry> live;
};
// Never destroyed: mappings may be released during static destruction.
MappingRegistry& mappings() {
    static MappingRegistry* r = new MappingRegistry;
    return *r;
}
}  // namespace

namespace detail {
void register_mapping(const void* addr, std::size_t len, const std::string& utf8_path, std::uint64_t id_a,
                      std::uint64_t id_b, void* handle) {
    if (!addr || len == 0) return;
    MappingRegistry& r = mappings();
    const std::lock_guard<std::mutex> l(r.mu);
    r.live[static_cast<const std::uint8_t*>(addr)] = MappingEntry{len, utf8_path, id_a, id_b, handle};
}
void unregister_mapping(const void* addr) noexcept {
    if (!addr) return;
    MappingRegistry& r = mappings();
    const std::lock_guard<std::mutex> l(r.mu);
    r.live.erase(static_cast<const std::uint8_t*>(addr));
}
}  // namespace detail

Status check_mapping(const std::uint8_t* p) noexcept {
    if (!p) return Status::ok();
    try {
        MappingRegistry& r = mappings();
        std::unique_lock<std::mutex> l(r.mu);
        auto it = r.live.upper_bound(p);
        if (it == r.live.begin()) return Status::ok();
        --it;
        if (static_cast<std::size_t>(p - it->first) >= it->second.len) return Status::ok();
#if defined(_WIN32)
        // Under the lock: the handle is open for as long as the mapping is registered.
        const MappingEntry& e = it->second;
        LARGE_INTEGER size{};
        if (!e.handle || !GetFileSizeEx(static_cast<HANDLE>(e.handle), &size)) return Status::ok();
        if (size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) < e.len)
            return Status(Errc::Truncated, "the file was shortened after it was memory-mapped");
#else
        const MappingEntry e = it->second;
        l.unlock();  // the stat may be slow (network folders)
        struct stat st {};
        // By name: the mapping holds no descriptor. Another file under the name (replaced by
        // rename) or no file at all leaves the mapped one intact.
        if (::stat(e.path.c_str(), &st) != 0) return Status::ok();
        if (static_cast<std::uint64_t>(st.st_dev) != e.id_a || static_cast<std::uint64_t>(st.st_ino) != e.id_b)
            return Status::ok();
        if (st.st_size >= 0 && static_cast<std::uint64_t>(st.st_size) < e.len)
            return Status(Errc::Truncated, "the file was shortened after it was memory-mapped");
#endif
        return Status::ok();
    } catch (...) {
        return Status(Errc::Internal, "mapping check failed");
    }
}

Result<Bytes> open_file(const std::string& utf8_path, const OpenOptions& opt) {
    if (opt.mode == OpenMode::Read) return read_file(utf8_path, opt.read_max_bytes);
    if (opt.mode == OpenMode::Auto) {
        // Small enough: owned memory. Larger (the only case read_file answers LimitExceeded): mapped.
        auto r = read_file(utf8_path, opt.read_threshold);
        if (r || r.error().code != Errc::LimitExceeded) return r;
    }
    auto m = map_file(utf8_path, opt.read_ahead);
    if (!m) return m;
    if (Status s = check_mapping(m->data()); !s) return s;
    return m;
}

Status write_file_atomic(const std::string& utf8_path, const std::uint8_t* data, std::size_t size, WriteMode mode) {
    const Slice one{data, size};
    return write_parts_atomic(utf8_path, &one, 1, {}, [&utf8_path]() -> Result<std::string> { return utf8_path; }, mode);
}

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
        detail::unregister_mapping(view);
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
    detail::register_mapping(m->view, static_cast<std::size_t>(size.QuadPart), utf8_path, 0, 0, m->file);
    return Bytes(std::shared_ptr<const void>(m, m->view), p,
                 static_cast<std::size_t>(size.QuadPart));
}

Result<Bytes> read_file(const std::string& utf8_path, std::uint64_t max_bytes) {
    const std::wstring w = widen(utf8_path);
    if (w.empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    // Full sharing, FILE_SHARE_DELETE included: reading a file never stops write_file_atomic (or
    // anyone else) from renaming a new version over it (R-071).
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status(Errc::IoError, "cannot open file");
    std::unique_ptr<void, decltype(&CloseHandle)> guard(h, &CloseHandle);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) return Status(Errc::IoError, "cannot get file size");
    if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes)
        return Status(Errc::LimitExceeded, "file is larger than the read limit");
    if (static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>(SIZE_MAX))
        return Status(Errc::OutOfMemory, "file too large for memory");
    const std::size_t len = static_cast<std::size_t>(size.QuadPart);
    if (len == 0) return Bytes{};
    std::shared_ptr<std::uint8_t[]> store(new (std::nothrow) std::uint8_t[len]);
    if (!store) return Status(Errc::OutOfMemory, "out of memory reading the file");
    std::size_t got = 0;
    while (got < len) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(len - got, 1u << 30));
        DWORD n = 0;
        if (!ReadFile(h, store.get() + got, chunk, &n, nullptr)) return Status(Errc::IoError, "read failed");
        if (n == 0) break;  // the file shrank while being read
        got += n;
    }
    if (got != len) return Status(Errc::IoError, "read failed");
    const std::uint8_t* p = store.get();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, len);
}

namespace {
// The folder part of a path ("" when there is none), with its trailing separator.
std::string folder_of(const std::string& p) {
    const std::size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}

// The absolute form of a path (SetFileInformationByHandle renames want one); empty on failure.
std::wstring full_path_of(const std::wstring& w) {
    const DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return {};
    std::wstring full(n, L'\0');
    const DWORD m = GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
    if (m == 0 || m >= n) return {};
    full.resize(m);
    return full;
}

// How long one write_file_atomic call keeps retrying a replace that fails with a sharing error
// (R-133: it may run on an association thread, so the wait is short).
constexpr DWORD kReplaceRetryBudgetMs = 1000;

// A target with the read-only attribute cannot be replaced: that ERROR_ACCESS_DENIED does not go
// away, so it is not retried.
bool target_is_read_only(const std::wstring& full) {
    const DWORD a = GetFileAttributesW(full.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) != 0;
}

// Opens the finished temporary file again for the rename: DELETE access only and full sharing, so
// that while this handle exists (it ends up naming the target for a moment) readers that share
// delete can still open the target. Reparse points are not followed, and the file must be the
// one this writer created (same volume and file index): another writer's or a planted file is
// never renamed.
HANDLE reopen_for_rename(const std::wstring& wtmp, std::uint64_t volume, std::uint64_t index) {
    HANDLE h = CreateFileW(wtmp.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    BY_HANDLE_FILE_INFORMATION fi{};
    const bool same = GetFileInformationByHandle(h, &fi) && fi.dwVolumeSerialNumber == volume &&
                      ((static_cast<std::uint64_t>(fi.nFileIndexHigh) << 32) | fi.nFileIndexLow) == index &&
                      (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (!same) {
        CloseHandle(h);
        SetLastError(ERROR_FILE_INVALID);  // not ours: never renamed, never deleted
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

bool is_sharing_error(DWORD e) {
    return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ||
           e == ERROR_USER_MAPPED_FILE;
}

bool posix_rename_unsupported(DWORD e) {
    return e == ERROR_INVALID_PARAMETER || e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED ||
           e == ERROR_CALL_NOT_IMPLEMENTED;
}
}  // namespace

Status write_parts_atomic(const std::string& folder_path, const Slice* parts, std::size_t part_count,
                          const std::function<void(const std::uint8_t*, std::size_t)>& observe,
                          const std::function<Result<std::string>()>& final_path, WriteMode mode) {
    if (widen(folder_path).empty()) return Status(Errc::InvalidArgument, "path is empty or not valid UTF-8");
    if (full_path_of(widen(folder_path)).empty()) return Status(Errc::InvalidArgument, "cannot resolve the target path");
    ExclusiveFile f;
    std::string tmp;
    for (int attempt = 0;; ++attempt) {
        tmp = folder_of(folder_path) + unique_name(".siaara-tmp-", ".part");
        bool exists = false;
        Status s = f.create(tmp, &exists);
        if (s) break;
        if (!exists || attempt >= 8) return Status(Errc::IoError, "cannot create file");
    }
    const std::wstring wtmp = widen(tmp);
    std::uint64_t volume = 0, index = 0;
    Status st = write_slices(f, parts, part_count, observe);
    if (st && !f.sync()) st = Status(Errc::IoError, "flush failed");
    const bool have_identity = st && f.identity(volume, index);
    // R-133: the handle the rename goes through is taken from the open file itself (no second
    // lookup by name, so no identity check that FAT / exFAT / some network shares fail), before
    // the writing handle is closed. INVALID_HANDLE_VALUE when the file system cannot do it.
    HANDLE rename_handle = st ? f.reopen_for_rename() : INVALID_HANDLE_VALUE;
    if (!f.close() && st) st = Status(Errc::IoError, "close failed");
    const auto close_rename_handle = [&rename_handle] {
        if (rename_handle != INVALID_HANDLE_VALUE) CloseHandle(rename_handle);
        rename_handle = INVALID_HANDLE_VALUE;
    };
    if (!st) {
        close_rename_handle();
        DeleteFileW(wtmp.c_str());
        return Status(Errc::IoError, "write failed");
    }
    // The target is known only now (it may depend on the content just written).
    std::wstring full;
    {
        Result<std::string> target = final_path();
        Status ts = target ? Status::ok() : target.error();
        if (ts) {
            const std::wstring w = widen(*target);
            full = w.empty() ? std::wstring() : full_path_of(w);
            if (full.empty()) ts = Status(Errc::InvalidArgument, "cannot resolve the target path");
        }
        if (!ts) {
            close_rename_handle();
            DeleteFileW(wtmp.c_str());
            return ts;
        }
    }
    const bool replace = mode == WriteMode::Replace;
    // POSIX-semantics rename through a handle first: the target is replaced even while readers hold
    // it open with FILE_SHARE_DELETE (as every reader in the toolkit does), and the target name
    // always names one whole file. Without POSIX renames (before Windows 10 1607, FAT, some network
    // shares) MoveFileExW does the replace. Either way another writer's rename at the same moment,
    // or a reader that did not share delete, makes the replace fail briefly with a sharing error:
    // retry, within a short bounded time. Whatever happens, the temporary file (which may hold
    // patient data) is removed when the write fails.
    bool posix = true;
    DWORD e = ERROR_SUCCESS;
    DWORD waited_ms = 0;
    for (unsigned attempt = 0;; ++attempt) {
        if (posix) {
            HANDLE h = rename_handle;
            bool by_name = false;
            if (h == INVALID_HANDLE_VALUE) {
                // No handle from the open file: open the temporary file by name and make sure it
                // is the one written above. Where the file system gives no usable identity the
                // rename goes by name instead (the name is unpredictable and was created
                // exclusively by this call).
                h = have_identity ? reopen_for_rename(wtmp, volume, index) : INVALID_HANDLE_VALUE;
                by_name = true;
                if (h == INVALID_HANDLE_VALUE) {
                    posix = false;
                    continue;
                }
            }
            e = posix_rename_by_handle(h, full, replace);
            if (by_name) CloseHandle(h);
            if (posix_rename_unsupported(e)) {
                posix = false;
                continue;
            }
        } else {
            close_rename_handle();
            const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
            e = MoveFileExW(wtmp.c_str(), full.c_str(), flags) ? ERROR_SUCCESS : GetLastError();
        }
        if (e == ERROR_SUCCESS) {
            close_rename_handle();
            return Status::ok();
        }
        if (e == ERROR_ACCESS_DENIED && target_is_read_only(full)) break;  // will not go away
        if (!is_sharing_error(e) || waited_ms >= kReplaceRetryBudgetMs) break;
        const DWORD pause = std::min<DWORD>(DWORD{1} << std::min(attempt, 5u), 25);  // 1, 2, 4 ... 25 ms
        Sleep(pause);
        waited_ms += pause;
    }
    close_rename_handle();
    DeleteFileW(wtmp.c_str());  // always: a failed write leaves nothing behind
    if (!replace && (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)) return Status(Errc::Rejected, "file exists");
    if (e == ERROR_ACCESS_DENIED && target_is_read_only(full)) return Status(Errc::IoError, "the target file is read-only");
    return Status(Errc::IoError, "rename failed");
}

#else  // POSIX

namespace {
struct PosixMapping {
    void* addr = MAP_FAILED;
    std::size_t len = 0;
    ~PosixMapping() {
        if (addr != MAP_FAILED) {
            detail::unregister_mapping(addr);
            munmap(addr, len);
        }
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
    detail::register_mapping(m->addr, len, utf8_path, static_cast<std::uint64_t>(st.st_dev),
                             static_cast<std::uint64_t>(st.st_ino), nullptr);
    return Bytes(std::shared_ptr<const void>(m, m->addr), p, len);
}

Result<Bytes> read_file(const std::string& utf8_path, std::uint64_t max_bytes) {
    const int fd = ::open(utf8_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Status(Errc::IoError, "cannot open file");
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return Status(Errc::IoError, "cannot stat file");
    }
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_bytes) {
        ::close(fd);
        return Status(Errc::LimitExceeded, "file is larger than the read limit");
    }
    if (static_cast<std::uint64_t>(st.st_size) > SIZE_MAX) {
        ::close(fd);
        return Status(Errc::OutOfMemory, "file too large for memory");
    }
    const std::size_t len = static_cast<std::size_t>(st.st_size);
    if (len == 0) {
        ::close(fd);
        return Bytes{};
    }
    std::shared_ptr<std::uint8_t[]> store(new (std::nothrow) std::uint8_t[len]);
    if (!store) {
        ::close(fd);
        return Status(Errc::OutOfMemory, "out of memory reading the file");
    }
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::read(fd, store.get() + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {  // an error, or the file became shorter while it was read
            ::close(fd);
            return Status(Errc::IoError, "read failed");
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    const std::uint8_t* p = store.get();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, len);
}

namespace {
std::string folder_of(const std::string& p) {
    const std::size_t k = p.rfind('/');
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}

// Exclusive create of `path` itself, then write: the no-replace fallback for file systems
// without hard links. Not atomic against crashes, but never replaces an existing file.
Status write_exclusive_slices(const std::string& path, const Slice* parts, std::size_t part_count) {
    ExclusiveFile f;
    bool exists = false;
    if (Status s = f.create(path, &exists); !s) return exists ? Status(Errc::Rejected, "file exists") : s;
    Status st = write_slices(f, parts, part_count, {});
    if (st && !f.sync()) st = Status(Errc::IoError, "flush failed");
    if (!f.close() && st) st = Status(Errc::IoError, "close failed");
    if (!st) ::unlink(path.c_str());
    return st;
}
}  // namespace

Status write_parts_atomic(const std::string& folder_path, const Slice* parts, std::size_t part_count,
                          const std::function<void(const std::uint8_t*, std::size_t)>& observe,
                          const std::function<Result<std::string>()>& final_path, WriteMode mode) {
    if (folder_path.empty()) return Status(Errc::InvalidArgument, "path is empty");
    ExclusiveFile f;
    std::string tmp;
    for (int attempt = 0;; ++attempt) {
        tmp = folder_of(folder_path) + unique_name(".siaara-tmp-", ".part");
        bool exists = false;
        Status s = f.create(tmp, &exists);
        if (s) break;
        if (!exists || attempt >= 8) return Status(Errc::IoError, "cannot create file");
    }
    Status st = write_slices(f, parts, part_count, observe);
    if (st && !f.sync()) st = Status(Errc::IoError, "flush failed");
    if (!f.close() && st) st = Status(Errc::IoError, "close failed");
    if (!st) {
        ::unlink(tmp.c_str());
        return st;
    }
    // The target is known only now (it may depend on the content just written).
    Result<std::string> target = final_path();
    if (!target || target->empty()) {
        ::unlink(tmp.c_str());
        return target ? Status(Errc::InvalidArgument, "path is empty") : target.error();
    }
    const std::string& utf8_path = *target;
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
    bool no_links = e == EPERM || e == EOPNOTSUPP || e == EMLINK || e == ENOSYS;
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
    no_links = no_links || e == ENOTSUP;
#endif
    if (no_links)
        return write_exclusive_slices(utf8_path, parts, part_count);
    return Status(Errc::IoError, "rename failed");
}

#endif

}  // namespace siaara::io
