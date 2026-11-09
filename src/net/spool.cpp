// SIAARA DICOM Toolkit - spooling received data sets to disk (see net/spool.hpp).
#include "net/spool.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "core/dataset.hpp"
#include "core/io.hpp"
#include "core/writer.hpp"

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

Status rename_over(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::rename(fs_path(from), fs_path(to), ec);
    if (ec) return Status(Errc::IoError, "cannot move spool file into place");
    return Status::ok();
}

}  // namespace

// ------------------------------------------------------------------ SpoolFile

SpoolFile::~SpoolFile() {
    std::error_code ec;
    if (!pending_target_.empty()) {
        std::filesystem::rename(fs_path(path_), fs_path(pending_target_), ec);
        if (!ec) return;
    }
    if (!committed_) std::filesystem::remove(fs_path(path_), ec);
}

std::string SpoolFile::path() const {
    std::lock_guard<std::mutex> l(mu_);
    return path_;
}

bool SpoolFile::committed() const {
    std::lock_guard<std::mutex> l(mu_);
    return committed_ || !pending_target_.empty();
}

Status SpoolFile::commit(const std::string& target) {
    std::lock_guard<std::mutex> l(mu_);
    if (committed_ || !pending_target_.empty()) return Status(Errc::InvalidArgument, "spool file already committed");
    if (target.empty()) return Status(Errc::InvalidArgument, "empty target path");
#if defined(_WIN32)
    // The data set is still mapped by the receiver; rename once the mapping is released.
    pending_target_ = target;
    return Status::ok();
#else
    if (Status s = rename_over(path_, target); !s) return s;
    path_ = target;
    committed_ = true;
    return Status::ok();
#endif
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
    // From here on the SpoolFile owns (and eventually deletes) the file.
    auto file = std::make_shared<SpoolFile>(path_, prefix_size_);
    if (!good) return Status(Errc::IoError, "cannot write spool file");
    Result<Bytes> m = io::map_file(path_, true);
    if (!m) return m.take_error();
    if (m->size() != prefix_size_ + written_) return Status(Errc::IoError, "spool file size mismatch");
    // The view keeps the SpoolFile alive, and the mapping is released before the file is renamed
    // or removed (members are destroyed in reverse order).
    struct Holder {
        std::shared_ptr<SpoolFile> file;
        Bytes map;
    };
    auto h = std::make_shared<Holder>(Holder{file, m.value()});
    const std::uint8_t* base = h->map.data();
    Finished f;
    f.data = Bytes(std::shared_ptr<const void>(h, base), base ? base + prefix_size_ : nullptr,
                   static_cast<std::size_t>(written_));
    f.file = std::move(file);
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
