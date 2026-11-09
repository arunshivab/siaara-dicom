// SIAARA DICOM Toolkit - helpers shared by the fuzz targets (test tooling only).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "core/types.hpp"
#include "net/socket.hpp"

namespace siaara_fuzz {

// The input as library Bytes (an exact-size heap copy, so overreads are caught by ASan).
inline siaara::Bytes to_bytes(const std::uint8_t* d, std::size_t n) { return siaara::Bytes::copy_of(d, n); }

// A peer that sends `in` (at most `chunk` bytes per read), then closes; writes are discarded
// (counted). Never blocks: an exhausted input reads as an orderly close.
class MemTransport final : public siaara::net::Transport {
public:
    MemTransport(const std::uint8_t* d, std::size_t n, std::size_t chunk) : in_(d, d + n), chunk_(chunk ? chunk : 1) {}
    siaara::Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int) override {
        if (!open_ || pos_ >= in_.size()) return std::size_t{0};
        const std::size_t k = std::min({n, chunk_, in_.size() - pos_});
        std::memcpy(buf, in_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    siaara::Status write_all(const std::uint8_t*, std::size_t n, int) override {
        if (!open_) return siaara::Status(siaara::Errc::NetworkError, "closed");
        written_ += n;
        return siaara::Status::ok();
    }
    siaara::Result<bool> wait_readable(int) override { return true; }
    void shutdown() noexcept override {}
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }
    std::string peer_address() const override { return "fuzz"; }
    std::size_t written() const noexcept { return written_; }

private:
    std::vector<std::uint8_t> in_;
    std::size_t chunk_;
    std::size_t pos_ = 0;
    std::size_t written_ = 0;
    bool open_ = true;
};

// A per-process scratch folder (removed and recreated on first use).
inline const std::filesystem::path& scratch_dir(const char* target) {
    static std::filesystem::path dir = [target] {
        std::error_code ec;
        std::filesystem::path d = std::filesystem::temp_directory_path(ec);
        if (ec) d = ".";
        std::random_device rd;  // distinct folders for parallel fuzzing processes
        d /= std::string("siaara-fuzz-") + target + "-" + std::to_string(rd()) + std::to_string(rd());
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

}  // namespace siaara_fuzz
