// SIAARA DICOM Toolkit - spooling received data sets to disk (see net/spool.hpp).
#include "net/spool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "core/dataset.hpp"
#include "core/writer.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace siaara::net {

namespace {

std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8_of(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string unique_name(const char* prefix = "siaara-spool-", const char* suffix = ".part") {
    static std::atomic<std::uint64_t> counter{0};
    static const std::uint64_t seed = [] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }();
    const std::uint64_t n = counter.fetch_add(1) + 1;
    static const char* hex = "0123456789abcdef";
    std::string s = prefix;
    std::uint64_t v = seed ^ (n * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < 16; ++i) {
        s += hex[v & 15u];
        v >>= 4;
    }
    s += '-' + std::to_string(n) + suffix;
    return s;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
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

    Status create(const std::string& path) {
#if defined(_WIN32)
        const std::wstring w = widen(path);
        if (w.empty()) return Status(Errc::InvalidArgument, "spool path is not valid UTF-8");
        OwnerOnlySecurity sec;
        if (!sec.get()) return Status(Errc::IoError, "cannot build an owner-only security descriptor");
        h_ = CreateFileW(w.c_str(), GENERIC_WRITE, 0, sec.get(), CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return Status(Errc::IoError, "cannot create spool file");
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd_ < 0) return Status(Errc::IoError, "cannot create spool file");
#endif
        return Status::ok();
    }
    Status write(const std::uint8_t* p, std::size_t n) {
#if defined(_WIN32)
        while (n > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
            DWORD done = 0;
            if (!WriteFile(h_, p, chunk, &done, nullptr) || done == 0) return Status(Errc::IoError, "cannot write spool file");
            p += done;
            n -= done;
        }
#else
        while (n > 0) {
            const ssize_t r = ::write(fd_, p, std::min<std::size_t>(n, 1u << 30));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return Status(Errc::IoError, "cannot write spool file");
            p += r;
            n -= static_cast<std::size_t>(r);
        }
#endif
        return Status::ok();
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

// Copies `from` to a new, exclusively created temporary name next to `to`, then renames it over
// `to` (used when a plain rename is impossible, e.g. across volumes). The temporary name is
// unpredictable and never follows an existing link (R-035).
bool copy_into_place(const std::string& from, const std::string& to) {
    std::error_code ec;
    const std::filesystem::path target = fs_path(to);
    const std::filesystem::path tmp = target.parent_path() / unique_name(".siaara-tmp-", ".part");
    const std::string tmp8 = utf8_of(tmp);
    ExclusiveFile out;
    if (!out.create(tmp8)) return false;
    bool ok = true;
#if defined(_WIN32)
    HANDLE in = CreateFileW(widen(from).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) ok = false;
    std::vector<std::uint8_t> buf(1u << 20);
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) ok = false;
        else if (got == 0) break;
        else ok = static_cast<bool>(out.write(buf.data(), got));
    }
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
#else
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) ok = false;
    std::vector<std::uint8_t> buf(1u << 20);
    while (ok) {
        const ssize_t got = ::read(in, buf.data(), buf.size());
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) ok = false;
        else if (got == 0) break;
        else ok = static_cast<bool>(out.write(buf.data(), static_cast<std::size_t>(got)));
    }
    if (in >= 0) ::close(in);
#endif
    ok = out.close() && ok;
    if (ok) {
        std::filesystem::rename(tmp, target, ec);
        ok = !ec;
    }
    if (!ok) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
    }
    return ok;
}

// Creates (or checks) the private per-user default spool directory under `base`.
Result<std::filesystem::path> private_spool_dir(const std::filesystem::path& base) {
#if defined(_WIN32)
    // %TEMP% is already per user; the subdirectory gets an owner-only DACL as well.
    const std::filesystem::path dir = base / "siaara-spool";
    const std::wstring w = dir.wstring();
    OwnerOnlySecurity sec;
    if (!sec.get()) return Status(Errc::IoError, "cannot build an owner-only security descriptor");
    if (!CreateDirectoryW(w.c_str(), sec.get()) && GetLastError() != ERROR_ALREADY_EXISTS)
        return Status(Errc::IoError, "cannot create the spool directory");
    const DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        return Status(Errc::IoError, "spool directory is not a plain directory");
    // Re-apply the owner-only DACL in case the directory existed with other permissions; this fails
    // unless we own it.
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(sec.get()->lpSecurityDescriptor, &present, &acl, &defaulted) ||
        SetNamedSecurityInfoW(const_cast<wchar_t*>(w.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
                              nullptr) != ERROR_SUCCESS)
        return Status(Errc::IoError, "cannot restrict the spool directory to its owner");
    return dir;
#else
    const std::filesystem::path dir = base / ("siaara-spool-" + std::to_string(::geteuid()));
    const std::string d = utf8_of(dir);
    if (::mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) return Status(Errc::IoError, "cannot create the spool directory");
    struct stat st {};
    if (::lstat(d.c_str(), &st) != 0) return Status(Errc::IoError, "cannot inspect the spool directory");
    if (!S_ISDIR(st.st_mode)) return Status(Errc::IoError, "spool directory is not a plain directory");
    if (st.st_uid != ::geteuid()) return Status(Errc::IoError, "spool directory is owned by another user");
    if ((st.st_mode & 077) != 0) return Status(Errc::IoError, "spool directory is accessible to other users");
    return dir;
#endif
}

std::mutex g_cleaned_mu;
std::set<std::string>& cleaned_dirs() {
    static std::set<std::string> s;
    return s;
}

std::size_t remove_stale_in(const std::filesystem::path& dir, int minutes) {
    if (minutes <= 0) return 0;
    std::size_t removed = 0;
    std::error_code ec;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(minutes);
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utf8_of(it->path().filename());
        if (name.rfind("siaara-spool-", 0) != 0 || name.size() < 5 || name.compare(name.size() - 5, 5, ".part") != 0)
            continue;
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;  // never follows links (symlink_status for links)
        if (it->is_symlink(e2)) continue;
        const auto t = it->last_write_time(e2);
        if (e2 || t > cutoff) continue;
        if (std::filesystem::remove(it->path(), e2)) ++removed;
    }
    return removed;
}

}  // namespace

Result<std::string> spool_directory(const SpoolConfig& cfg) {
    if (!cfg.directory.empty()) return cfg.directory;
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return Status(Errc::IoError, "no temporary directory for spooling");
    Result<std::filesystem::path> dir = private_spool_dir(base);
    if (!dir) return dir.take_error();
    return utf8_of(dir.value());
}

Result<std::size_t> remove_stale_spool_files(const SpoolConfig& cfg) {
    Result<std::string> dir = spool_directory(cfg);
    if (!dir) return dir.take_error();
    return remove_stale_in(fs_path(dir.value()), cfg.stale_after_minutes);
}

namespace {

}  // namespace

// ------------------------------------------------------------------ SpoolFile

#if defined(_WIN32)
struct SpoolFile::Native {
    HANDLE file = INVALID_HANDLE_VALUE;  // GENERIC_READ, shared for read and delete
    HANDLE map = nullptr;
    const void* view = nullptr;
};
#else
struct SpoolFile::Native {
    void* addr = nullptr;
    std::size_t len = 0;
};
#endif

Result<std::shared_ptr<SpoolFile>> SpoolFile::open(const std::string& path, std::uint64_t data_offset) {
    std::shared_ptr<SpoolFile> f(new SpoolFile());
    f->path_ = path;
    f->data_offset_ = data_offset;
    f->native_ = std::make_unique<Native>();
    // From here on the destructor removes the file if anything below fails.
#if defined(_WIN32)
    const std::wstring w = widen(path);
    if (w.empty()) return Status(Errc::InvalidArgument, "spool path is not valid UTF-8");
    Native& n = *f->native_;
    // FILE_SHARE_DELETE lets commit() rename the file while it is mapped. The handle itself asks only
    // for GENERIC_READ, so other readers can still open the file with the usual FILE_SHARE_READ.
    n.file = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (n.file == INVALID_HANDLE_VALUE) {
        std::error_code ec;
        std::filesystem::remove(fs_path(path), ec);
        return Status(Errc::IoError, "cannot open spool file");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(n.file, &size) || size.QuadPart <= 0) return Status(Errc::IoError, "cannot get spool file size");
    if (static_cast<std::uint64_t>(size.QuadPart) > SIZE_MAX) return Status(Errc::LimitExceeded, "spool file too large to map");
    n.map = CreateFileMappingW(n.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!n.map) return Status(Errc::IoError, "cannot map spool file");
    n.view = MapViewOfFile(n.map, FILE_MAP_READ, 0, 0, 0);
    if (!n.view) return Status(Errc::IoError, "cannot map spool file");
    f->base_ = static_cast<const std::uint8_t*>(n.view);
    f->size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Status(Errc::IoError, "cannot open spool file");
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return Status(Errc::IoError, "cannot get spool file size");
    }
    if (static_cast<std::uint64_t>(st.st_size) > SIZE_MAX) {
        ::close(fd);
        return Status(Errc::LimitExceeded, "spool file too large to map");
    }
    const auto len = static_cast<std::size_t>(st.st_size);
    void* addr = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);  // the mapping keeps the file
    if (addr == MAP_FAILED) return Status(Errc::IoError, "cannot map spool file");
    f->native_->addr = addr;
    f->native_->len = len;
    f->base_ = static_cast<const std::uint8_t*>(addr);
    f->size_ = len;
#endif
    return f;
}

SpoolFile::~SpoolFile() {
    if (!native_) return;
    const bool remove_file = !committed_ || delete_original_;
    const std::string& doomed = delete_original_ ? original_ : path_;
#if defined(_WIN32)
    Native& n = *native_;
    // The view and the section go first: a file with a mapped view cannot be deleted.
    if (n.view) UnmapViewOfFile(n.view);
    if (n.map) CloseHandle(n.map);
    const bool opened = n.file != INVALID_HANDLE_VALUE;
    if (opened) CloseHandle(n.file);
    if (opened && remove_file) {
        // Deleted only now that no view or handle of ours is left. A handle opened by someone else
        // without FILE_SHARE_DELETE (a virus scanner looking at the new file, typically) makes
        // DeleteFileW fail with a sharing violation; such holders are short-lived, so retry briefly.
        const std::wstring w = widen(doomed);
        for (int i = 0; i < 50; ++i) {
            if (DeleteFileW(w.c_str())) break;
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) break;
            if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION) break;
            Sleep(20);
        }
    }
#else
    if (native_->addr) ::munmap(native_->addr, native_->len);
    if (remove_file) ::unlink(doomed.c_str());
#endif
}

std::string SpoolFile::path() const {
    std::lock_guard<std::mutex> l(mu_);
    return path_;
}

bool SpoolFile::committed() const {
    std::lock_guard<std::mutex> l(mu_);
    return committed_;
}

Status SpoolFile::commit(const std::string& target) {
    std::lock_guard<std::mutex> l(mu_);
    if (committed_) return Status(Errc::InvalidArgument, "spool file already committed");
    if (target.empty()) return Status(Errc::InvalidArgument, "empty target path");
#if defined(_WIN32)
    const std::wstring w = widen(target);
    if (w.empty()) return Status(Errc::InvalidArgument, "target path is not valid UTF-8");
    // Our handles share FILE_SHARE_DELETE, so the mapped file can be renamed now. A rename only
    // changes the name: the mapping and every view of the data set stay valid.
    bool ok = MoveFileExW(widen(path_).c_str(), w.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        if (!copy_into_place(path_, target)) return Status(Errc::IoError, "cannot move spool file into place");
        delete_original_ = true;  // the copy is in place; the original goes with the mapping
        original_ = path_;
        path_ = target;
        committed_ = true;
        return Status::ok();
    }
#else
    if (::rename(path_.c_str(), target.c_str()) != 0) {
        if (errno != EXDEV || !copy_into_place(path_, target))
            return Status(Errc::IoError, "cannot move spool file into place");
        delete_original_ = true;
        original_ = path_;
        path_ = target;
        committed_ = true;
        return Status::ok();
    }
#endif
    path_ = target;
    committed_ = true;
    return Status::ok();
}

// ------------------------------------------------------------------ SpoolWriter

struct SpoolWriter::Impl {
    ExclusiveFile out;
    bool failed = false;
};

SpoolWriter::~SpoolWriter() {
    if (impl_) {
        impl_->out.close();
        std::error_code ec;
        std::filesystem::remove(fs_path(path_), ec);
    }
}

Result<std::unique_ptr<SpoolWriter>> SpoolWriter::open(const SpoolConfig& cfg, const std::vector<std::uint8_t>& prefix) {
    Result<std::string> dir8 = spool_directory(cfg);
    if (!dir8) return dir8.take_error();
    const std::filesystem::path dir = fs_path(dir8.value());
    {
        // First use of this directory in the process: clear leftovers of earlier runs.
        std::lock_guard<std::mutex> l(g_cleaned_mu);
        if (cleaned_dirs().insert(dir8.value()).second) (void)remove_stale_in(dir, cfg.stale_after_minutes);
    }
    std::unique_ptr<SpoolWriter> w(new SpoolWriter());
    auto impl = std::make_unique<Impl>();
    const std::filesystem::path p = dir / unique_name();
    w->path_ = utf8_of(p);
    if (Status s = impl->out.create(w->path_); !s) return s;  // nothing created: no cleanup needed
    w->impl_ = std::move(impl);
    if (!prefix.empty())
        if (Status s = w->impl_->out.write(prefix.data(), prefix.size()); !s) return s;  // destructor removes it
    w->prefix_size_ = prefix.size();
    return w;
}

Status SpoolWriter::write(const std::uint8_t* data, std::size_t n) {
    if (!impl_) return Status(Errc::InvalidArgument, "spool file is closed");
    if (n == 0) return Status::ok();
    if (Status s = impl_->out.write(data, n); !s) {
        impl_->failed = true;
        return s;
    }
    written_ += n;
    return Status::ok();
}

Result<SpoolWriter::Finished> SpoolWriter::finish() {
    if (!impl_) return Status(Errc::InvalidArgument, "spool file is closed");
    const bool good = impl_->out.close() && !impl_->failed;
    impl_.reset();
    if (!good) {
        std::error_code ec;
        std::filesystem::remove(fs_path(path_), ec);
        return Status(Errc::IoError, "cannot write spool file");
    }
    // From here on the SpoolFile owns (and eventually deletes) the file.
    Result<std::shared_ptr<SpoolFile>> file = SpoolFile::open(path_, prefix_size_);
    if (!file) return file.take_error();
    std::shared_ptr<SpoolFile> sf = std::move(file.value());
    if (sf->size() != prefix_size_ + written_) return Status(Errc::IoError, "spool file size mismatch");
    // Every view keeps the SpoolFile (and so the mapping) alive.
    Finished f;
    f.data = Bytes(std::shared_ptr<const void>(sf, sf->data()), sf->data() + prefix_size_,
                   static_cast<std::size_t>(written_));
    f.file = std::move(sf);
    return f;
}

Status part10_header(std::string_view sop_class, std::string_view sop_instance, std::string_view transfer_syntax,
                     std::string_view source_ae, std::vector<std::uint8_t>& out) {
    DicomFile f;
    f.transfer_syntax = std::string(transfer_syntax);
    if (Status s = f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, sop_class); !s) return s;
    if (Status s = f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, sop_instance); !s) return s;
    if (!source_ae.empty()) (void)f.meta.set_string(0x00020016, VR::AE, source_ae);  // Source AE Title
    out.clear();
    if (Status s = write_part10(f, out); !s) return s;
    if (out.size() < 144) return Status(Errc::Internal, "file meta too short");
    const std::uint64_t meta_end = 144ull + load_le32(out.data() + 140);
    if (meta_end > out.size()) return Status(Errc::Internal, "file meta length mismatch");
    out.resize(static_cast<std::size_t>(meta_end));
    return Status::ok();
}

}  // namespace siaara::net
