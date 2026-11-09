// SIAARA DICOM Toolkit - SHA-256 (FIPS 180-4 section 6.2) and HMAC-SHA-256 (RFC 2104).
// Used by the de-identification engine for keyed UID replacement and date offsets.
// Every object that has held key material wipes it in its destructor.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace siaara::deid {

using Digest = std::array<std::uint8_t, 32>;

// Overwrites memory in a way the compiler may not remove as a dead store.
void secure_wipe(void* p, std::size_t n) noexcept;

class Sha256 {
public:
    Sha256() noexcept { reset(); }
    ~Sha256() { secure_wipe(this, sizeof(*this)); }
    Sha256(const Sha256&) = default;
    Sha256& operator=(const Sha256&) = default;

    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view s) noexcept { update(s.data(), s.size()); }
    Digest finish() noexcept;  // also resets

    static Digest hash(const void* data, std::size_t size) noexcept;

private:
    void block(const std::uint8_t* p) noexcept;
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;  // bytes
};

// HMAC-SHA-256 with the key schedule computed once (inner and outer padded states).
class HmacSha256 {
public:
    HmacSha256() noexcept = default;
    explicit HmacSha256(std::span<const std::uint8_t> key) noexcept { set_key(key); }
    ~HmacSha256() { secure_wipe(this, sizeof(*this)); }
    HmacSha256(const HmacSha256&) = delete;
    HmacSha256& operator=(const HmacSha256&) = delete;

    void set_key(std::span<const std::uint8_t> key) noexcept;
    // MAC of the concatenation of the given parts.
    Digest mac(std::string_view a, std::string_view b = {}) const noexcept;

private:
    Sha256 inner_, outer_;
};

Digest hmac_sha256(std::span<const std::uint8_t> key, const void* data, std::size_t size) noexcept;

}  // namespace siaara::deid
