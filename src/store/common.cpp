#include "store/common.hpp"

#include "core/io.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace siaara::store {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- CRC-32 (reflected, 0xEDB88320)
namespace {
struct CrcTable {
    std::uint32_t t[4][256];
    constexpr CrcTable() : t{} {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; ++i)
            for (int k = 1; k < 4; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFFu];
    }
};
constexpr CrcTable kCrc{};
}  // namespace

// Slicing-by-4: four table lookups per 32-bit word.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed) noexcept {
    std::uint32_t c = ~seed;
    while (size >= 4) {
        c ^= static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
             (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
        c = kCrc.t[3][c & 0xFFu] ^ kCrc.t[2][(c >> 8) & 0xFFu] ^ kCrc.t[1][(c >> 16) & 0xFFu] ^ kCrc.t[0][c >> 24];
        data += 4;
        size -= 4;
    }
    while (size--) c = kCrc.t[0][(c ^ *data++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)
namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32u - n)); }

void sha_block(std::uint32_t h[8], const std::uint8_t* b) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(b[4 * i]) << 24) | (static_cast<std::uint32_t>(b[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(b[4 * i + 2]) << 8) | static_cast<std::uint32_t>(b[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
        const std::uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size) noexcept {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t n = size;
    while (n >= 64) {
        sha_block(h, p);
        p += 64;
        n -= 64;
    }
    std::uint8_t tail[128] = {};
    std::memcpy(tail, p, n);
    tail[n] = 0x80;
    const std::size_t tl = (n + 9 <= 64) ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8u;
    for (int i = 0; i < 8; ++i) tail[tl - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
    sha_block(h, tail);
    if (tl == 128) sha_block(h, tail + 64);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k)
            out[static_cast<std::size_t>(4 * i + k)] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * k));
    return out;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        s.push_back(kHex[data[i] >> 4]);
        s.push_back(kHex[data[i] & 15u]);
    }
    return s;
}

std::string generate_uid() {
    static std::mutex m;
    static std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    std::uint32_t limb[4];
    {
        std::lock_guard<std::mutex> g(m);
        const std::uint64_t hi = rng(), lo = rng();
        limb[0] = static_cast<std::uint32_t>(hi >> 32);
        limb[1] = static_cast<std::uint32_t>(hi);
        limb[2] = static_cast<std::uint32_t>(lo >> 32);
        limb[3] = static_cast<std::uint32_t>(lo);
    }
    // ISO/IEC 9834-8: set version 4 and the variant bits so the value is a valid random UUID.
    limb[1] = (limb[1] & 0xFFFF0FFFu) | 0x00004000u;
    limb[2] = (limb[2] & 0x3FFFFFFFu) | 0x80000000u;
    std::string digits;
    while (limb[0] | limb[1] | limb[2] | limb[3]) {
        std::uint64_t rem = 0;
        for (auto& l : limb) {
            const std::uint64_t cur = (rem << 32) | l;
            l = static_cast<std::uint32_t>(cur / 10u);
            rem = cur % 10u;
        }
        digits.push_back(static_cast<char>('0' + rem));
    }
    if (digits.empty()) digits = "0";
    std::reverse(digits.begin(), digits.end());
    return "2.25." + digits;
}

// ---------------------------------------------------------------- UTF-8 paths
fs::path u8path(std::string_view utf8) {
    std::u8string s(utf8.size(), u8'\0');
    for (std::size_t i = 0; i < utf8.size(); ++i) s[i] = static_cast<char8_t>(utf8[i]);
    return fs::path(s);
}

std::string to_utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    std::string out(s.size(), '\0');
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<char>(s[i]);
    return out;
}

// ---------------------------------------------------------------- OsFile
OsFile::~OsFile() { close(); }

#if defined(_WIN32)
OsFile::OsFile(OsFile&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
OsFile& OsFile::operator=(OsFile&& o) noexcept {
    if (this != &o) {
        close();
        h_ = std::exchange(o.h_, nullptr);
    }
    return *this;
}
bool OsFile::is_open() const noexcept { return h_ != nullptr; }
void OsFile::close() noexcept {
    if (h_) CloseHandle(static_cast<HANDLE>(h_));
    h_ = nullptr;
}
Result<OsFile> OsFile::open(const fs::path& p, Mode mode) {
    const DWORD access = mode == Mode::ReadOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    const DWORD disp = mode == Mode::CreateTruncate ? CREATE_ALWAYS : (mode == Mode::ReadWrite ? OPEN_ALWAYS : OPEN_EXISTING);
    HANDLE h = CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status(Errc::IoError, "cannot open file");
    OsFile f;
    f.h_ = h;
    return f;
}
Result<std::uint64_t> OsFile::size() const {
    LARGE_INTEGER s{};
    if (!h_ || !GetFileSizeEx(static_cast<HANDLE>(h_), &s)) return Status(Errc::IoError, "cannot get file size");
    return static_cast<std::uint64_t>(s.QuadPart);
}
Status OsFile::read_at(std::uint64_t offset, void* buf, std::size_t n) const {
    auto* q = static_cast<std::uint8_t*>(buf);
    while (n > 0) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(h_), q, chunk, &got, &ov)) return Status(Errc::IoError, "read failed");
        if (got == 0) return Status(Errc::Truncated, "unexpected end of file");
        q += got;
        n -= got;
        offset += got;
    }
    return Status::ok();
}
Status OsFile::append(const void* data, std::size_t n) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(static_cast<HANDLE>(h_), zero, nullptr, FILE_END)) return Status(Errc::IoError, "seek failed");
    const auto* q = static_cast<const std::uint8_t*>(data);
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(static_cast<HANDLE>(h_), q, chunk, &wrote, nullptr) || wrote == 0)
            return Status(Errc::IoError, "write failed");
        q += wrote;
        n -= wrote;
    }
    return Status::ok();
}
Status OsFile::truncate(std::uint64_t sz) {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(sz);
    if (!SetFilePointerEx(static_cast<HANDLE>(h_), pos, nullptr, FILE_BEGIN) || !SetEndOfFile(static_cast<HANDLE>(h_)))
        return Status(Errc::IoError, "truncate failed");
    return Status::ok();
}
Status OsFile::sync() {
    if (!FlushFileBuffers(static_cast<HANDLE>(h_))) return Status(Errc::IoError, "flush failed");
    return Status::ok();
}
void sync_dir(const fs::path&) noexcept {}
#else
OsFile::OsFile(OsFile&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
OsFile& OsFile::operator=(OsFile&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
}
bool OsFile::is_open() const noexcept { return fd_ >= 0; }
void OsFile::close() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}
Result<OsFile> OsFile::open(const fs::path& p, Mode mode) {
    int flags = O_CLOEXEC;
    if (mode == Mode::ReadOnly) flags |= O_RDONLY;
    else if (mode == Mode::ReadWrite) flags |= O_RDWR | O_CREAT;
    else flags |= O_RDWR | O_CREAT | O_TRUNC;
    const int fd = ::open(p.c_str(), flags, 0644);
    if (fd < 0) return Status(Errc::IoError, "cannot open file");
    OsFile f;
    f.fd_ = fd;
    return f;
}
Result<std::uint64_t> OsFile::size() const {
    struct stat st {};
    if (fd_ < 0 || fstat(fd_, &st) != 0) return Status(Errc::IoError, "cannot stat file");
    return static_cast<std::uint64_t>(st.st_size);
}
Status OsFile::read_at(std::uint64_t offset, void* buf, std::size_t n) const {
    auto* q = static_cast<std::uint8_t*>(buf);
    while (n > 0) {
        const ssize_t got = ::pread(fd_, q, n, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR) continue;
            return Status(Errc::IoError, "read failed");
        }
        if (got == 0) return Status(Errc::Truncated, "unexpected end of file");
        q += got;
        n -= static_cast<std::size_t>(got);
        offset += static_cast<std::uint64_t>(got);
    }
    return Status::ok();
}
Status OsFile::append(const void* data, std::size_t n) {
    if (::lseek(fd_, 0, SEEK_END) < 0) return Status(Errc::IoError, "seek failed");
    const auto* q = static_cast<const std::uint8_t*>(data);
    while (n > 0) {
        const ssize_t w = ::write(fd_, q, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return Status(Errc::IoError, "write failed");
        }
        q += w;
        n -= static_cast<std::size_t>(w);
    }
    return Status::ok();
}
Status OsFile::truncate(std::uint64_t sz) {
    if (::ftruncate(fd_, static_cast<off_t>(sz)) != 0) return Status(Errc::IoError, "truncate failed");
    return Status::ok();
}
Status OsFile::sync() {
    if (::fsync(fd_) != 0) return Status(Errc::IoError, "fsync failed");
    return Status::ok();
}
void sync_dir(const fs::path& dir) noexcept {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }
}
#endif

#if defined(_WIN32)
namespace {
struct WinMap {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE map = nullptr;
    const void* view = nullptr;
    ~WinMap() {
        if (view) UnmapViewOfFile(view);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};
}  // namespace

Result<Bytes> map_for_header(const std::string& utf8_path) {
    const fs::path p = u8path(utf8_path);
    auto m = std::make_shared<WinMap>();
    m->file = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (m->file == INVALID_HANDLE_VALUE) return Status(Errc::IoError, "cannot open file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(m->file, &size)) return Status(Errc::IoError, "cannot get file size");
    if (size.QuadPart == 0) return Bytes{};
    if (static_cast<std::uint64_t>(size.QuadPart) > SIZE_MAX) return Status(Errc::LimitExceeded, "file too large");
    m->map = CreateFileMappingW(m->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m->map) return io::read_file(utf8_path);
    m->view = MapViewOfFile(m->map, FILE_MAP_READ, 0, 0, 0);
    if (!m->view) return io::read_file(utf8_path);
    const auto* q = static_cast<const std::uint8_t*>(m->view);
    return Bytes(std::shared_ptr<const void>(m, m->view), q, static_cast<std::size_t>(size.QuadPart));
}
#else
namespace {
struct PosixMap {
    void* addr = MAP_FAILED;
    std::size_t len = 0;
    ~PosixMap() {
        if (addr != MAP_FAILED) munmap(addr, len);
    }
};
}  // namespace

Result<Bytes> map_for_header(const std::string& utf8_path) {
    const int fd = ::open(utf8_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Status(Errc::IoError, "cannot open file");
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return Status(Errc::IoError, "not a regular file");
    }
    if (static_cast<std::uint64_t>(st.st_size) > SIZE_MAX) {
        ::close(fd);
        return Status(Errc::LimitExceeded, "file too large");
    }
    const auto len = static_cast<std::size_t>(st.st_size);
    if (len == 0) {
        ::close(fd);
        return Bytes{};
    }
    auto m = std::make_shared<PosixMap>();
    m->addr = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m->addr == MAP_FAILED) return io::read_file(utf8_path);
    m->len = len;
#if defined(MADV_RANDOM)
    (void)madvise(m->addr, len, MADV_RANDOM);
#endif
    const auto* q = static_cast<const std::uint8_t*>(m->addr);
    return Bytes(std::shared_ptr<const void>(m, m->addr), q, len);
}
#endif

Result<std::vector<std::uint8_t>> read_whole(const fs::path& p) {
    auto f = OsFile::open(p, OsFile::Mode::ReadOnly);
    if (!f) return f.take_error();
    auto sz = f->size();
    if (!sz) return sz.take_error();
    if (*sz > SIZE_MAX) return Status(Errc::LimitExceeded, "file too large");
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(*sz));
    if (!buf.empty())
        if (Status s = f->read_at(0, buf.data(), buf.size()); !s) return s;
    return buf;
}

Status write_replace(const fs::path& p, const std::uint8_t* data, std::size_t size, bool durable) {
    fs::path tmp = p;
    tmp += u8path("." + generate_uid().substr(5, 12) + ".siaara-tmp");
    {
        auto f = OsFile::open(tmp, OsFile::Mode::CreateTruncate);
        if (!f) return f.take_error();
        Status s = f->append(data, size);
        if (s && durable) s = f->sync();
        if (!s) {
            f->close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return s;
        }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        // the system message names the cause (e.g. a sharing violation), never the path
        const std::string why = ec.message();
        fs::remove(tmp, ec);
        return Status(Errc::IoError, "rename failed: " + why);
    }
    if (durable) sync_dir(p.parent_path());
    return Status::ok();
}

// ---------------------------------------------------------------- LockFile
namespace {
std::mutex g_lock_mutex;
std::set<std::string>& lock_registry() {
    static std::set<std::string> s;
    return s;
}
}  // namespace

LockFile::~LockFile() { release(); }

LockFile::LockFile(LockFile&& o) noexcept : key_(std::move(o.key_)) {
#if defined(_WIN32)
    h_ = std::exchange(o.h_, nullptr);
#else
    fd_ = std::exchange(o.fd_, -1);
#endif
    o.key_.clear();
}

LockFile& LockFile::operator=(LockFile&& o) noexcept {
    if (this != &o) {
        release();
        key_ = std::move(o.key_);
        o.key_.clear();
#if defined(_WIN32)
        h_ = std::exchange(o.h_, nullptr);
#else
        fd_ = std::exchange(o.fd_, -1);
#endif
    }
    return *this;
}

Result<LockFile> LockFile::acquire(const fs::path& p) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec) canon = p;
    LockFile l;
#if defined(_WIN32)
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION)
            return Status(Errc::Rejected, "store is in use by another process");
        return Status(Errc::IoError, "cannot open lock file");
    }
    l.h_ = h;
#else
    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return Status(Errc::IoError, "cannot open lock file");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return Status(Errc::Rejected, "store is in use by another process");
    }
    l.fd_ = fd;
#endif
    std::lock_guard<std::mutex> g(g_lock_mutex);
    const std::string key = to_utf8(canon);
    if (!lock_registry().insert(key).second) {
        // `l` has no key yet, so its destructor only closes the handle.
        return Status(Errc::Rejected, "store is already open in this process");
    }
    l.key_ = key;
    return l;
}

void LockFile::release() noexcept {
    if (!key_.empty()) {
        std::lock_guard<std::mutex> g(g_lock_mutex);
        lock_registry().erase(key_);
        key_.clear();
    }
#if defined(_WIN32)
    if (h_) CloseHandle(static_cast<HANDLE>(h_));
    h_ = nullptr;
#else
    if (fd_ >= 0) {
        (void)::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    fd_ = -1;
#endif
}

// ---------------------------------------------------------------- serialisation
void ByteWriter::u16(std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v));
    buf.push_back(static_cast<std::uint8_t>(v >> 8));
}
void ByteWriter::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void ByteWriter::u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void ByteWriter::f64(double v) {
    std::uint64_t b = 0;
    std::memcpy(&b, &v, 8);
    u64(b);
}
void ByteWriter::varint(std::uint64_t v) {
    while (v >= 0x80) {
        buf.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    buf.push_back(static_cast<std::uint8_t>(v));
}
void ByteWriter::str(std::string_view s) {
    varint(s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}

bool ByteReader::need(std::size_t k) noexcept {
    if (!ok_ || n_ - pos_ < k) {
        ok_ = false;
        return false;
    }
    return true;
}
std::uint8_t ByteReader::u8() {
    if (!need(1)) return 0;
    return p_[pos_++];
}
std::uint16_t ByteReader::u16() {
    if (!need(2)) return 0;
    const auto v = static_cast<std::uint16_t>(p_[pos_] | (p_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
}
std::uint32_t ByteReader::u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
    pos_ += 4;
    return v;
}
std::uint64_t ByteReader::u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
    pos_ += 8;
    return v;
}
double ByteReader::f64() {
    const std::uint64_t b = u64();
    double d = 0;
    std::memcpy(&d, &b, 8);
    return d;
}
std::uint64_t ByteReader::varint() {
    std::uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        const std::uint8_t b = u8();
        if (!ok_) return 0;
        v |= static_cast<std::uint64_t>(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) return v;
    }
    ok_ = false;
    return 0;
}
std::string ByteReader::str() {
    const std::uint64_t len = varint();
    if (!ok_ || len > n_ - pos_) {
        ok_ = false;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(p_ + pos_), static_cast<std::size_t>(len));
    pos_ += static_cast<std::size_t>(len);
    return s;
}

}  // namespace siaara::store
