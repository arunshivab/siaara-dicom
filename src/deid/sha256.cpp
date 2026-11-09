// SIAARA DICOM Toolkit - SHA-256 written from FIPS 180-4 (sections 4.1.2, 4.2.2, 5.1.1, 5.3.3,
// 6.2.2) and HMAC from RFC 2104 section 2.
#include "deid/sha256.hpp"

#include <cstring>

namespace siaara::deid {

void secure_wipe(void* p, std::size_t n) noexcept {
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) v[i] = 0;
}

namespace {

// FIPS 180-4 4.2.2: first 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// FIPS 180-4 5.3.3: initial hash value.
constexpr std::uint32_t kH0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }
constexpr std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept { return (x & y) ^ (~x & z); }
constexpr std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}
constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

}  // namespace

void Sha256::reset() noexcept {
    std::memcpy(h_, kH0, sizeof(h_));
    used_ = 0;
    total_ = 0;
}

void Sha256::block(const std::uint8_t* p) noexcept {
    std::uint32_t w[64];
    for (int t = 0; t < 16; ++t)
        w[t] = (static_cast<std::uint32_t>(p[4 * t]) << 24) | (static_cast<std::uint32_t>(p[4 * t + 1]) << 16) |
               (static_cast<std::uint32_t>(p[4 * t + 2]) << 8) | static_cast<std::uint32_t>(p[4 * t + 3]);
    for (int t = 16; t < 64; ++t)
        w[t] = small_sigma1(w[t - 2]) + w[t - 7] + small_sigma0(w[t - 15]) + w[t - 16];
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int t = 0; t < 64; ++t) {
        const std::uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + kK[t] + w[t];
        const std::uint32_t t2 = big_sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    secure_wipe(w, sizeof(w));
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += size;
    if (used_) {
        const std::size_t take = size < 64 - used_ ? size : 64 - used_;
        std::memcpy(buf_ + used_, p, take);
        used_ += take;
        p += take;
        size -= take;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    while (size >= 64) {
        block(p);
        p += 64;
        size -= 64;
    }
    if (size) {
        std::memcpy(buf_, p, size);
        used_ = size;
    }
}

Digest Sha256::finish() noexcept {
    // FIPS 180-4 5.1.1: append 1 bit, zeros to 448 mod 512, then the 64-bit big-endian bit length.
    const std::uint64_t bits = total_ * 8u;
    buf_[used_++] = 0x80;
    if (used_ > 56) {
        std::memset(buf_ + used_, 0, 64 - used_);
        block(buf_);
        used_ = 0;
    }
    std::memset(buf_ + used_, 0, 56 - used_);
    for (int i = 0; i < 8; ++i) buf_[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    block(buf_);
    Digest out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(4 * i)] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[static_cast<std::size_t>(4 * i + 1)] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[static_cast<std::size_t>(4 * i + 2)] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[static_cast<std::size_t>(4 * i + 3)] = static_cast<std::uint8_t>(h_[i]);
    }
    secure_wipe(buf_, sizeof(buf_));
    reset();
    return out;
}

Digest Sha256::hash(const void* data, std::size_t size) noexcept {
    Sha256 s;
    s.update(data, size);
    return s.finish();
}

void HmacSha256::set_key(std::span<const std::uint8_t> key) noexcept {
    // RFC 2104 section 2: keys longer than the block size B = 64 are hashed first; the key is
    // then zero-padded to B bytes and XORed with ipad (0x36) and opad (0x5C).
    std::uint8_t k[64] = {};
    if (key.size() > 64) {
        Digest d = Sha256::hash(key.data(), key.size());
        std::memcpy(k, d.data(), d.size());
        secure_wipe(d.data(), d.size());
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    std::uint8_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
    inner_.reset();
    inner_.update(pad, 64);
    for (int i = 0; i < 64; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5C);
    outer_.reset();
    outer_.update(pad, 64);
    secure_wipe(pad, sizeof(pad));
    secure_wipe(k, sizeof(k));
}

Digest HmacSha256::mac(std::string_view a, std::string_view b) const noexcept {
    Sha256 in = inner_;
    in.update(a);
    in.update(b);
    Digest inner_digest = in.finish();
    Sha256 out = outer_;
    out.update(inner_digest.data(), inner_digest.size());
    secure_wipe(inner_digest.data(), inner_digest.size());
    return out.finish();
}

Digest hmac_sha256(std::span<const std::uint8_t> key, const void* data, std::size_t size) noexcept {
    HmacSha256 h(key);
    return h.mac(std::string_view(static_cast<const char*>(data), size));
}

}  // namespace siaara::deid
