// SIAARA DICOM Toolkit - internal helpers for files that may hold patient data: unpredictable
// temporary names, exclusive owner-only creation (no symlinks followed) and owner-only Windows
// security descriptors. Used by io::write_file_atomic (R-071) and the network spool (R-005, R-035).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace siaara::io {

// prefix + 16 hex digits from a per-process random seed + '-' + a counter + suffix.
inline std::string unique_name(std::string_view prefix, std::string_view suffix) {
    static std::atomic<std::uint64_t> counter{0};
    static const std::uint64_t seed = [] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }();
    const std::uint64_t n = counter.fetch_add(1) + 1;
    static const char* hex = "0123456789abcdef";
    std::string s(prefix);
    std::uint64_t v = seed ^ (n * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < 16; ++i) {
        s += hex[v & 15u];
        v >>= 4;
    }
    s += '-' + std::to_string(n);
    s += suffix;
    return s;
}

#if defined(_WIN32)
inline std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Renames the file open as `h` (opened with DELETE access) to `full_target`, an absolute path, with
// POSIX semantics (FileRenameInfoEx, Windows 10 1607+): an existing target is replaced even while
// others hold it open with FILE_SHARE_DELETE, and the target name never goes missing. `replace`
// false refuses an existing target (ERROR_ALREADY_EXISTS). Returns ERROR_SUCCESS or the Win32
// error; ERROR_INVALID_PARAMETER, ERROR_INVALID_FUNCTION or ERROR_NOT_SUPPORTED mean that the
// system or the file system has no POSIX rename (callers fall back to MoveFileExW).
inline DWORD posix_rename_by_handle(HANDLE h, const std::wstring& full_target, bool replace) {
    constexpr DWORD kReplaceIfExists = 0x1;  // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
    constexpr DWORD kPosixSemantics = 0x2;   // FILE_RENAME_FLAG_POSIX_SEMANTICS
    constexpr int kFileRenameInfoEx = 22;    // FILE_INFO_BY_HANDLE_CLASS FileRenameInfoEx
    const std::size_t name_bytes = full_target.size() * sizeof(WCHAR);
    if (full_target.empty() || name_bytes > 0xFFFF0000u) return ERROR_INVALID_NAME;
    const std::size_t need =
        std::max(sizeof(FILE_RENAME_INFO), offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(WCHAR));
    std::vector<std::uint64_t> buf((need + 7) / 8, 0);  // zeroed and 8-byte aligned
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    // The first member is a union of BOOLEAN ReplaceIfExists and DWORD Flags (older SDKs declare
    // only the BOOLEAN): store the flags as the DWORD it really is.
    const DWORD flags = kPosixSemantics | (replace ? kReplaceIfExists : 0);
    std::memcpy(buf.data(), &flags, sizeof flags);
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(reinterpret_cast<std::uint8_t*>(buf.data()) + offsetof(FILE_RENAME_INFO, FileName),
                full_target.data(), name_bytes);
    if (SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx), info,
                                   static_cast<DWORD>(need)))
        return ERROR_SUCCESS;
    return GetLastError();
}

// Owner-only security descriptor (the current user's SID, full control; inheritance blocked).
class OwnerOnlySecurity {
public:
    OwnerOnlySecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        DWORD len = 0;
        (void)GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        user_.resize(len);
        const bool got = len != 0 && GetTokenInformation(token, TokenUser, user_.data(), len, &len);
        CloseHandle(token);
        if (!got) return;
        PSID sid = reinterpret_cast<TOKEN_USER*>(user_.data())->User.Sid;
        const DWORD acl_len = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(sid));
        acl_.resize(acl_len);
        PACL acl = reinterpret_cast<PACL>(acl_.data());
        if (!InitializeAcl(acl, acl_len, ACL_REVISION)) return;
        if (!AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, GENERIC_ALL, sid)) return;
        if (!InitializeSecurityDescriptor(&sd_, SECURITY_DESCRIPTOR_REVISION)) return;
        if (!SetSecurityDescriptorDacl(&sd_, TRUE, acl, FALSE)) return;
        if (!SetSecurityDescriptorControl(&sd_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return;
        sa_.nLength = sizeof sa_;
        sa_.lpSecurityDescriptor = &sd_;
        sa_.bInheritHandle = FALSE;
        ok_ = true;
    }
    SECURITY_ATTRIBUTES* get() noexcept { return ok_ ? &sa_ : nullptr; }

private:
    std::vector<std::uint8_t> user_;
    std::vector<std::uint8_t> acl_;
    SECURITY_DESCRIPTOR sd_{};
    SECURITY_ATTRIBUTES sa_{};
    bool ok_ = false;
};
#endif

// Native, exclusively created, owner-only file handle (R-005): O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC
// with mode 0600 on POSIX; CREATE_NEW with an owner-only DACL on Windows.
class ExclusiveFile {
public:
    ExclusiveFile() = default;
    ~ExclusiveFile() { close(); }
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;

    // `exists` (may be null) is set when creation failed because the name is taken.
    Status create(const std::string& path, bool* exists = nullptr) {
        if (exists) *exists = false;
#if defined(_WIN32)
        const std::wstring w = widen_utf8(path);
        if (w.empty()) return Status(Errc::InvalidArgument, "path is not valid UTF-8");
        OwnerOnlySecurity sec;
        if (!sec.get()) return Status(Errc::IoError, "cannot build an owner-only security descriptor");
        // FILE_SHARE_DELETE: reopen_for_rename() below needs it (a second handle with DELETE
        // access to this same open file). Reading and writing by others stay excluded while the
        // file is written; the owner-only DACL decides who could open it at all.
        h_ = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_DELETE, sec.get(), CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (exists) *exists = e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS;
            return Status(Errc::IoError, "cannot create file");
        }
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            if (exists) *exists = errno == EEXIST;
            return Status(Errc::IoError, "cannot create file");
        }
#endif
        return Status::ok();
    }
    Status write(const std::uint8_t* p, std::size_t n) {
#if defined(_WIN32)
        while (n > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
            DWORD done = 0;
            if (!WriteFile(h_, p, chunk, &done, nullptr) || done == 0) return Status(Errc::IoError, "write failed");
            p += done;
            n -= done;
        }
#else
        while (n > 0) {
            const ssize_t r = ::write(fd_, p, std::min<std::size_t>(n, 1u << 30));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return Status(Errc::IoError, "write failed");
            p += r;
            n -= static_cast<std::size_t>(r);
        }
#endif
        return Status::ok();
    }
    // Flushes the data to the device; false on failure.
    bool sync() noexcept {
#if defined(_WIN32)
        return h_ != INVALID_HANDLE_VALUE && FlushFileBuffers(h_) != 0;
#else
        return fd_ >= 0 && ::fsync(fd_) == 0;
#endif
    }
    // Closes; false when the final flush failed.
    bool close() noexcept {
#if defined(_WIN32)
        if (h_ == INVALID_HANDLE_VALUE) return true;
        const bool ok = CloseHandle(h_) != 0;
        h_ = INVALID_HANDLE_VALUE;
        return ok;
#else
        if (fd_ < 0) return true;
        const bool ok = ::close(fd_) == 0;
        fd_ = -1;
        return ok;
#endif
    }
#if defined(_WIN32)
    // R-133: a second handle to this same open file, with DELETE access only and full sharing,
    // for renaming it into place after close(). ReOpenFile goes through the open file, not
    // through its name, so the handle cannot name another file and needs no identity check; with
    // only DELETE access and full sharing it does not keep readers from opening the target in
    // the moment it names it. INVALID_HANDLE_VALUE when the file system cannot do it (the caller
    // then opens the file by name). The caller closes the handle.
    HANDLE reopen_for_rename() const noexcept {
        if (h_ == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        return ReOpenFile(h_, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_FLAG_OPEN_REPARSE_POINT);
    }
    // The file's identity (volume serial number and file index), to recognise it when it is
    // opened again by name; false when unknown.
    bool identity(std::uint64_t& volume, std::uint64_t& index) const noexcept {
        BY_HANDLE_FILE_INFORMATION fi{};
        if (h_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h_, &fi)) return false;
        volume = fi.dwVolumeSerialNumber;
        index = (static_cast<std::uint64_t>(fi.nFileIndexHigh) << 32) | fi.nFileIndexLow;
        return true;
    }
#endif
    bool is_open() const noexcept {
#if defined(_WIN32)
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

}  // namespace siaara::io
