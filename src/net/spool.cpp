// SIAARA DICOM Toolkit - spooling received data sets to disk (see net/spool.hpp).
#include "net/spool.hpp"

#include <atomic>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
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

std::string unique_name() {
    static std::atomic<std::uint64_t> counter{0};
    static const std::uint64_t seed = [] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }();
    const std::uint64_t n = counter.fetch_add(1) + 1;
    static const char* hex = "0123456789abcdef";
    std::string s = "siaara-spool-";
    std::uint64_t v = seed ^ (n * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < 16; ++i) {
        s += hex[v & 15u];
        v >>= 4;
    }
    s += '-' + std::to_string(n) + ".part";
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

#endif

// Copies `from` to a temporary name next to `to`, then renames it over `to` (used when a plain
// rename is impossible, e.g. across volumes).
bool copy_into_place(const std::string& from, const std::string& to) {
    std::error_code ec;
    const std::filesystem::path target = fs_path(to);
    std::filesystem::path tmp = target;
    tmp += ".siaara-tmp";
    std::filesystem::copy_file(fs_path(from), tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
    return true;
}

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
    std::ofstream out;
};

SpoolWriter::~SpoolWriter() {
    if (impl_) {
        impl_->out.close();
        std::error_code ec;
        std::filesystem::remove(fs_path(path_), ec);
    }
}

Result<std::unique_ptr<SpoolWriter>> SpoolWriter::open(const SpoolConfig& cfg, const std::vector<std::uint8_t>& prefix) {
    std::error_code ec;
    std::filesystem::path dir = cfg.directory.empty() ? std::filesystem::temp_directory_path(ec) : fs_path(cfg.directory);
    if (ec) return Status(Errc::IoError, "no temporary directory for spooling");
    std::unique_ptr<SpoolWriter> w(new SpoolWriter());
    w->impl_ = std::make_unique<Impl>();
    const std::filesystem::path p = dir / unique_name();
    w->path_ = utf8_of(p);
    w->impl_->out.open(p, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!w->impl_->out) {
        w->impl_.reset();
        return Status(Errc::IoError, "cannot create spool file");
    }
    if (!prefix.empty()) {
        w->impl_->out.write(reinterpret_cast<const char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
        if (!w->impl_->out) return Status(Errc::IoError, "cannot write spool file");
    }
    w->prefix_size_ = prefix.size();
    return w;
}

Status SpoolWriter::write(const std::uint8_t* data, std::size_t n) {
    if (!impl_) return Status(Errc::InvalidArgument, "spool file is closed");
    if (n == 0) return Status::ok();
    impl_->out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!impl_->out) return Status(Errc::IoError, "cannot write spool file");
    written_ += n;
    return Status::ok();
}

Result<SpoolWriter::Finished> SpoolWriter::finish() {
    if (!impl_) return Status(Errc::InvalidArgument, "spool file is closed");
    impl_->out.flush();
    const bool good = static_cast<bool>(impl_->out);
    impl_->out.close();
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
