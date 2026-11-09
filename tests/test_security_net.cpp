// Regression tests for the 2026-11-09 security review, DIMSE network findings
// (R-003, R-004, R-005, R-015, R-016, R-017, R-034, R-035).
// Lines ending in "// R-0xx option" only set options introduced by the fixes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/transfer_syntax.hpp"
#include "net/association.hpp"
#include "net/pdu.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/socket.hpp"
#include "net/spool.hpp"
#include "test.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace siaara;
using namespace siaara::net;

namespace {

using clock_type = std::chrono::steady_clock;

long long ms_since(clock_type::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
}

// Delivers `data` one byte every `interval_ms` (a slow-drip peer), then reports a timeout.
class DripTransport : public Transport {
public:
    DripTransport(std::vector<std::uint8_t> d, int interval_ms) : data_(std::move(d)), interval_(interval_ms) {}
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override {
        if (n == 0) return std::size_t{0};
        if (pos_ >= data_.size() || timeout_ms < interval_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(timeout_ms, 0)));
            return Status(Errc::Timeout, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_));
        buf[0] = data_[pos_++];
        return std::size_t{1};
    }
    Status write_all(const std::uint8_t*, std::size_t, int) override { return Status::ok(); }
    Result<bool> wait_readable(int) override { return true; }
    void shutdown() noexcept override {}
    void close() noexcept override {}
    bool is_open() const noexcept override { return true; }
    std::string peer_address() const override { return "drip"; }

private:
    std::vector<std::uint8_t> data_;
    int interval_;
    std::size_t pos_ = 0;
};

// Delivers `data` at once, then holds the read open (timeouts) until released.
class HoldTransport : public Transport {
public:
    explicit HoldTransport(std::vector<std::uint8_t> d) : data_(std::move(d)) {}
    std::atomic<bool> released{false};
    std::atomic<bool> drained{false};
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override {
        if (pos_ < data_.size()) {
            const std::size_t k = std::min(n, data_.size() - pos_);
            std::copy(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                      data_.begin() + static_cast<std::ptrdiff_t>(pos_ + k), buf);
            pos_ += k;
            return k;
        }
        drained = true;
        const auto t0 = clock_type::now();
        while (!released && ms_since(t0) < timeout_ms) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (released) return std::size_t{0};
        return Status(Errc::Timeout, "timeout");
    }
    Status write_all(const std::uint8_t*, std::size_t, int) override { return Status::ok(); }
    Result<bool> wait_readable(int) override { return true; }
    void shutdown() noexcept override {}
    void close() noexcept override {}
    bool is_open() const noexcept override { return true; }
    std::string peer_address() const override { return "hold"; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_ = 0;
};

AssociateRq echo_rq(const std::string& called = "SEC_SCP", const std::string& calling = "SEC_SCU") {
    AssociateRq rq;
    rq.called_ae = called;
    rq.calling_ae = calling;
    rq.contexts.push_back({1, "1.2.840.10008.1.1", {"1.2.840.10008.1.2"}});
    rq.user.max_length = 16384;
    rq.user.implementation_class_uid = "1.2.3.4";
    return rq;
}

std::vector<std::uint8_t> encoded(const Pdu& p) {
    std::vector<std::uint8_t> b;
    (void)encode_pdu(p, b);
    return b;
}

Timeouts quick() {
    Timeouts t;
    t.connect_ms = 3000;
    t.acse_ms = 3000;
    t.dimse_ms = 3000;
    t.artim_ms = 2000;
    t.idle_ms = 10000;
    t.write_ms = 3000;
    return t;
}

ScpConfig scp(std::size_t max_assoc) {
    ScpConfig c;
    c.ae_title = "SEC_SCP";
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.max_associations = max_assoc;
    c.allow_any_calling_ae = true;  // R-017 option
    c.timeouts = quick();
    return c;
}

ScuConfig scu(std::uint16_t port) {
    ScuConfig c;
    c.calling_ae = "SEC_SCU";
    c.called_ae = "SEC_SCP";
    c.host = "127.0.0.1";
    c.port = port;
    c.timeouts = quick();
    return c;
}

#if defined(__linux__)
// Resident set size in bytes (Linux).
std::uint64_t rss_bytes() {
    std::ifstream f("/proc/self/statm");
    std::uint64_t pages = 0, resident = 0;
    f >> pages >> resident;
    return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
}
#endif

}  // namespace

// ------------------------------------------------------------------ R-003

TEST_CASE("R-003: a slow-drip PDU is cut off at the whole-PDU deadline") {
    // A valid A-ASSOCIATE-RQ (~200 bytes) dripped at 1 byte / 20 ms: every single read makes
    // progress well inside the 300 ms timeout, so only an absolute deadline stops it.
    DripTransport t(encoded(echo_rq()), 20);
    const auto t0 = clock_type::now();
    Result<Pdu> r = read_pdu(t, 300);
    const long long ms = ms_since(t0);
    REQUIRE(!r);
    CHECK(r.error().code == Errc::Timeout);
    CHECK(ms < 1000);
    // read_exact has the same absolute deadline.
    DripTransport t2(std::vector<std::uint8_t>(200, 0x41), 20);
    std::vector<std::uint8_t> buf(200);
    const auto t1 = clock_type::now();
    CHECK(read_exact(t2, buf.data(), buf.size(), 300).code == Errc::Timeout);
    CHECK(ms_since(t1) < 1000);
}

TEST_CASE("R-003: a dripping requestor is dropped at ARTIM while setting up the association") {
    ScpConfig cfg = scp(2);
    cfg.timeouts.artim_ms = 600;
    ScpServer server(cfg, {});
    REQUIRE(server.start().is_ok());
    auto t = tcp_connect("127.0.0.1", server.port(), 2000);
    REQUIRE(t);
    const std::vector<std::uint8_t> rq = encoded(echo_rq());
    const auto t0 = clock_type::now();
    bool closed = false;
    for (std::size_t i = 0; i < rq.size() && ms_since(t0) < 6000; ++i) {
        if (!(*t)->write_all(&rq[i], 1, 500)) {
            closed = true;
            break;
        }
        // Peer closed (or sent an abort) yet?
        Result<bool> rd = (*t)->wait_readable(100);
        if (!rd || *rd) {
            closed = true;
            break;
        }
    }
    const long long ms = ms_since(t0);
    CHECK(closed);
    CHECK(ms < 2500);
    server.stop();
}

TEST_CASE("R-003: a silent connection to a full server does not stall the accept thread") {
    ScpServer server(scp(1), {});
    REQUIRE(server.start().is_ok());
    Scu first(scu(server.port()));
    REQUIRE(first.connect().is_ok());  // the only slot is taken
    // A connection that never sends anything...
    auto silent = tcp_connect("127.0.0.1", server.port(), 2000);
    REQUIRE(silent);
    // ...must not delay the rejection of the next requestor.
    const auto t0 = clock_type::now();
    Scu second(scu(server.port()));
    AssociateRj rj;
    CHECK(second.connect(&rj).code == Errc::Rejected);
    CHECK(ms_since(t0) < 1000);
    CHECK_EQ(rj.reason, rj::PresentationLocalLimitExceeded);
    // The silent connection got an A-ASSOCIATE-RJ promptly as well, without sending a byte.
    std::uint8_t b = 0;
    Result<std::size_t> got = (*silent)->read_some(&b, 1, 1500);
    CHECK(got.is_ok() && *got == 1 && b == 0x03);
    CHECK(first.release().is_ok());
    server.stop();
}

// ------------------------------------------------------------------ R-004

TEST_CASE("R-004: A-ASSOCIATE and fixed-size PDUs are capped by type before allocation") {
    {
        std::vector<std::uint8_t> hdr = {0x01, 0, 0x00, 0x10, 0x00, 0x00};  // RQ claiming 1 MB
        HoldTransport t(hdr);
        t.released = true;
        Result<Pdu> r = read_pdu(t, 500);
        REQUIRE(!r);
        CHECK(r.error().code == Errc::LimitExceeded);
        CHECK(!t.drained.load());  // refused from the header alone, no body read attempted
    }
    {
        std::vector<std::uint8_t> hdr = {0x05, 0, 0x00, 0x00, 0x01, 0x00};  // A-RELEASE-RQ claiming 256 bytes
        HoldTransport t(hdr);
        t.released = true;
        Result<Pdu> r = read_pdu(t, 500);
        REQUIRE(!r);
        CHECK(r.error().code == Errc::Malformed);
        CHECK(!t.drained.load());
    }
    {
        // A normal A-ASSOCIATE-RQ and A-ABORT still read fine.
        HoldTransport t(encoded(echo_rq()));
        CHECK(read_pdu(t, 500).is_ok());
        HoldTransport a(encoded(Abort{abort_src::User, 0}));
        CHECK(read_pdu(a, 500).is_ok());
    }
}

TEST_CASE("R-004: P-DATA buffers grow with the bytes received (memory bound)") {
#if defined(__linux__)
    // Eight peers each claim a 16 MB P-DATA-TF and send 100 bytes. Allocating the claimed size up
    // front would make ~128 MB resident; growing with the data keeps it at a few 64 KB steps.
    constexpr int kPeers = 8;
    std::vector<std::unique_ptr<HoldTransport>> ts;
    for (int i = 0; i < kPeers; ++i) {
        std::vector<std::uint8_t> b = {0x04, 0, 0x00, 0xFF, 0xFF, 0x00};  // length 0x00FFFF00
        b.resize(6 + 100, 0x11);
        ts.push_back(std::make_unique<HoldTransport>(b));
    }
    const std::uint64_t before = rss_bytes();
    std::vector<std::thread> readers;
    for (int i = 0; i < kPeers; ++i)
        readers.emplace_back([&, i] { (void)read_pdu(*ts[static_cast<std::size_t>(i)], 10000); });
    for (int i = 0; i < 400; ++i) {
        bool all = true;
        for (auto& t : ts) all = all && t->drained.load();
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t during = rss_bytes();
    for (auto& t : ts) t->released = true;
    for (auto& r : readers) r.join();
    const std::uint64_t grew = during > before ? during - before : 0;
    std::printf("    R-004: RSS growth with %d pending 16 MB claims: %llu KB\n", kPeers,
                static_cast<unsigned long long>(grew / 1024));
    CHECK(grew < (32ull << 20));
#endif
}

// ------------------------------------------------------------------ R-005, R-035 (POSIX)

#if !defined(_WIN32)
namespace {
struct TmpdirOverride {
    std::string old;
    bool had = false;
    explicit TmpdirOverride(const std::string& dir) {
        if (const char* v = std::getenv("TMPDIR")) {
            old = v;
            had = true;
        }
        ::setenv("TMPDIR", dir.c_str(), 1);
    }
    ~TmpdirOverride() {
        if (had) ::setenv("TMPDIR", old.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

std::filesystem::path fresh_dir(const std::string& name) {
    const std::filesystem::path p = std::filesystem::path("/tmp") / ("siaara_sec_" + name + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

unsigned mode_of(const std::filesystem::path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) return 0xFFFF;
    return static_cast<unsigned>(st.st_mode & 07777);
}
}  // namespace

TEST_CASE("R-005: default spool goes to a private 0700 directory with 0600 files") {
    const auto base = fresh_dir("spool_default");
    TmpdirOverride env(base.string());
    SpoolConfig cfg;  // no directory: the default
    const std::vector<std::uint8_t> prefix(132, 0);
    auto w = SpoolWriter::open(cfg, prefix);
    REQUIRE(w);
    const std::uint8_t data[4] = {1, 2, 3, 4};
    REQUIRE((*w)->write(data, 4).is_ok());
    auto fin = (*w)->finish();
    REQUIRE(fin);
    const std::filesystem::path file = fin->file->path();
    const std::filesystem::path dir = file.parent_path();
    CHECK(dir != base);                                        // not straight into the shared temp dir
    CHECK(dir.parent_path() == base);
    CHECK_EQ(mode_of(dir), 0700u);
    CHECK_EQ(mode_of(file), 0600u);
    fin->file.reset();
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("R-005: a planted or loosened default spool directory is refused") {
    const auto base = fresh_dir("spool_planted");
    TmpdirOverride env(base.string());
    const std::filesystem::path elsewhere = base / "attacker";
    std::filesystem::create_directories(elsewhere);
    ::chmod(elsewhere.c_str(), 0777);
    const std::filesystem::path priv = base / ("siaara-spool-" + std::to_string(::geteuid()));
    std::filesystem::create_directory_symlink(elsewhere, priv);
    const std::vector<std::uint8_t> prefix(132, 0);
    {
        auto w = SpoolWriter::open(SpoolConfig{}, prefix);
        CHECK(!w);
        std::size_t n = 0;
        for (auto it = std::filesystem::directory_iterator(elsewhere); it != std::filesystem::directory_iterator(); ++it) ++n;
        CHECK_EQ(n, 0u);  // nothing written through the link
    }
    std::filesystem::remove(priv);
    std::filesystem::create_directory(priv);
    ::chmod(priv.c_str(), 0755);
    CHECK(!SpoolWriter::open(SpoolConfig{}, prefix));  // readable by others: refused
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("R-005: stale spool files are removed on first use of a directory") {
    const auto dir = fresh_dir("spool_stale");
    auto touch = [&](const std::string& name, int minutes_old) {
        const auto p = dir / name;
        std::ofstream(p) << "x";
        std::filesystem::last_write_time(
            p, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(minutes_old));
        return p;
    };
    const auto stale = touch("siaara-spool-0123456789abcdef-1.part", 120);
    const auto fresh = touch("siaara-spool-fedcba9876543210-2.part", 0);
    const auto other = touch("unrelated.part", 120);
    SpoolConfig cfg;
    cfg.directory = dir.string();
    auto w = SpoolWriter::open(cfg, std::vector<std::uint8_t>(132, 0));
    REQUIRE(w);
    CHECK(!std::filesystem::exists(stale));
    CHECK(std::filesystem::exists(fresh));
    CHECK(std::filesystem::exists(other));
    w->reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("R-035: cross-volume commit never follows a planted temporary name") {
    struct stat a {}, b {};
    if (::stat("/dev/shm", &b) != 0 || ::stat("/tmp", &a) != 0 || a.st_dev == b.st_dev) {
        std::printf("    R-035: skipped (no second file system at /dev/shm)\n");
        return;
    }
    const auto src_dir = fresh_dir("spool_xdev");
    const std::filesystem::path dst_dir = std::filesystem::path("/dev/shm") / ("siaara_sec_dst_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dst_dir, ec);
    std::filesystem::create_directories(dst_dir);
    const std::filesystem::path victim = dst_dir / "victim";
    std::ofstream(victim) << "precious";
    const std::filesystem::path target = dst_dir / "received.dcm";
    // The temporary name the old code used, planted as a link to the victim.
    std::filesystem::create_symlink(victim, dst_dir / "received.dcm.siaara-tmp");
    SpoolConfig cfg;
    cfg.directory = src_dir.string();
    auto w = SpoolWriter::open(cfg, std::vector<std::uint8_t>(132, 0));
    REQUIRE(w);
    const std::uint8_t data[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    REQUIRE((*w)->write(data, 8).is_ok());
    auto fin = (*w)->finish();
    REQUIRE(fin);
    CHECK(fin->file->commit(target.string()).is_ok());
    std::string v;
    std::getline(std::ifstream(victim), v);
    CHECK_EQ(v, "precious");
    CHECK(std::filesystem::is_regular_file(std::filesystem::symlink_status(target)));
    CHECK_EQ(std::filesystem::file_size(target), 140u);
    CHECK_EQ(mode_of(target) & 077u, 0u);
    fin->file.reset();
    std::filesystem::remove_all(dst_dir, ec);
    std::filesystem::remove_all(src_dir, ec);
}
#endif

// ------------------------------------------------------------------ R-015

TEST_CASE("R-015: in-memory data sets default to a 256 MB cap") {
    CHECK_EQ(AssociationLimits{}.max_dataset_size, 256ull << 20);
}

// ------------------------------------------------------------------ R-016, R-017

TEST_CASE("R-017: an SCP with no calling AE policy refuses to start") {
    ScpConfig c;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    {
        ScpServer s(c, {});
        Status st = s.start();
        CHECK(st.code == Errc::InvalidArgument);
        s.stop();
    }
    c.allowed_calling_aes = {"SOME_SCU"};
    {
        ScpServer s(c, {});
        CHECK(s.start().is_ok());
        s.stop();
    }
}

TEST_CASE("R-017: explicit allow-any still accepts every calling AE") {
    ScpConfig c = scp(2);
    c.allow_any_calling_ae = true;  // R-017 option
    ScpServer s(c, {});
    REQUIRE(s.start().is_ok());
    Scu u(scu(s.port()));
    CHECK(u.connect().is_ok());
    CHECK(u.echo().is_ok());
    CHECK(u.release().is_ok());
    s.stop();
}

TEST_CASE("R-016: concurrent associations per peer address are limited") {
    ScpConfig c = scp(8);
    c.max_associations_per_ip = 2;  // R-016 option
    ScpServer s(c, {});
    REQUIRE(s.start().is_ok());
    Scu a(scu(s.port())), b(scu(s.port())), d(scu(s.port()));
    REQUIRE(a.connect().is_ok());
    REQUIRE(b.connect().is_ok());
    AssociateRj rj;
    CHECK(d.connect(&rj).code == Errc::Rejected);
    CHECK_EQ(rj.reason, rj::PresentationLocalLimitExceeded);
    CHECK(a.release().is_ok());
    // The slot is given back once an association ends.
    bool ok = false;
    for (int i = 0; i < 50 && !ok; ++i) {
        Scu again(scu(s.port()));
        ok = again.connect().is_ok() && again.release().is_ok();
        if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(ok);
    CHECK(b.release().is_ok());
    s.stop();
}

TEST_CASE("R-016: association lifetime and operation count are limited") {
    {
        ScpConfig c = scp(2);
        c.max_association_lifetime_ms = 400;  // R-016 option
        ScpServer s(c, {});
        REQUIRE(s.start().is_ok());
        Scu u(scu(s.port()));
        REQUIRE(u.connect().is_ok());
        CHECK(u.echo().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        CHECK(!u.echo().is_ok());  // aborted by the SCP at the lifetime limit
        s.stop();
    }
    {
        ScpConfig c = scp(2);
        c.max_operations_per_association = 3;  // R-016 option
        ScpServer s(c, {});
        REQUIRE(s.start().is_ok());
        Scu u(scu(s.port()));
        REQUIRE(u.connect().is_ok());
        int ok = 0;
        for (int i = 0; i < 5; ++i) ok += u.echo().is_ok() ? 1 : 0;
        CHECK_EQ(ok, 3);
        s.stop();
    }
}

TEST_CASE("R-016: default limits") {
    ScpConfig c;
    CHECK(c.max_associations_per_ip > 0);  // R-016 option
    CHECK(c.max_association_lifetime_ms > 0);  // R-016 option
    CHECK(c.max_operations_per_association > 0);  // R-016 option
}

// ------------------------------------------------------------------ R-034

namespace {
// Acceptor with max length 16384; a raw requestor sends one 60000-byte P-DATA-TF. Returns the
// acceptor's receive() status and the oversized count.
struct OversizeResult {
    bool received = false;
    std::uint64_t oversized = 0;
};
OversizeResult oversize_exchange(bool enforce) {
    OversizeResult out;
    TcpListener l;
    if (!l.listen("127.0.0.1", 0)) return out;
    std::thread requestor([&] {
        auto t = tcp_connect("127.0.0.1", l.port(), 2000);
        if (!t) return;
        const auto rq = encoded(echo_rq());
        if (!(*t)->write_all(rq.data(), rq.size(), 2000)) return;
        Result<Pdu> ac = read_pdu(**t, 3000);
        if (!ac) return;
        PDataTf pd;
        std::vector<std::uint8_t> big(60000, 0);
        pd.pdvs.push_back({1, true, true, Bytes::copy_of(big.data(), big.size())});
        const auto b = encoded(pd);
        (void)(*t)->write_all(b.data(), b.size(), 3000);
        std::uint8_t sink[256];
        (void)(*t)->read_some(sink, sizeof sink, 2000);
    });
    auto t = l.accept(3000);
    if (t) {
        AssociationLimits lim;
        lim.max_receive_pdu = 16384;
        lim.enforce_max_receive_pdu = enforce;  // R-034 option
        auto a = Association::receive_request(std::move(*t), quick(), lim);
        if (a) {
            AssociateAc ac;
            std::vector<AcceptedSyntax> policy = {{"1.2.840.10008.1.1", {"1.2.840.10008.1.2"}}};
            auto ctx = negotiate_contexts((*a)->request_pdu(), policy, nullptr, ac);
            if ((*a)->accept(ac, ctx)) {
                auto r = (*a)->receive(3000, [](const Bytes&) -> Result<bool> { return false; });
                out.received = r.is_ok();
                out.oversized = (*a)->oversized_pdus();  // R-034 option
                (*a)->abort();
            }
        }
    }
    requestor.join();
    return out;
}
}  // namespace

TEST_CASE("R-034: strict mode aborts on P-DATA longer than our advertised maximum") {
    CHECK(!oversize_exchange(true).received);
}

TEST_CASE("R-034: lenient mode accepts oversized P-DATA but flags it") {
    const OversizeResult r = oversize_exchange(false);
    CHECK(r.received);
    CHECK_EQ(r.oversized, 1u);
}
