// Regression tests for the 2026-11-10 02:15 security review, HTTP findings
// (R-057, R-058, R-016 remainder, R-036 remainder).
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/socket.hpp"
#include "web/http.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

using clock_type = std::chrono::steady_clock;
long long ms_since(clock_type::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
}

bool send(net::Transport& t, const std::string& s) {
    return t.write_all(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), 2000).is_ok();
}

std::string read_reply(net::Transport& t, int ms) {
    std::string out;
    std::uint8_t buf[4096];
    for (;;) {
        auto r = t.read_some(buf, sizeof buf, ms);
        if (!r || *r == 0) break;
        out.append(reinterpret_cast<const char*>(buf), *r);
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

std::string exchange(std::uint16_t port, const std::string& wire, int ms = 3000) {
    auto t = net::tcp_connect("127.0.0.1", port, 2000);
    if (!t) return std::string();
    if (!send(**t, wire)) return std::string();
    return read_reply(**t, ms);
}

}  // namespace

TEST_CASE("R-057: request bodies are charged as their bytes arrive, not by Content-Length") {
    http::ServerConfig cfg;
    cfg.max_inflight_body_bytes = 1000;
    cfg.max_inflight_body_peer_share = 1;  // all test clients are one loopback peer
    cfg.limits.read_timeout_ms = 3000;
    http::Server s;
    REQUIRE(s.start(cfg, [](const http::Request& rq, http::Response& rs) { rs.body = std::to_string(rq.body.size()); }).is_ok());
    // A declares 900 bytes and sends 10: it holds 10, not 900.
    auto a = net::tcp_connect("127.0.0.1", s.port(), 2000);
    REQUIRE(a);
    REQUIRE(send(**a, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 900\r\n\r\n" + std::string(10, 'a')));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::string b = exchange(s.port(), "POST / HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 800\r\n\r\n" +
                                                 std::string(800, 'b'));
    CHECK(b.rfind("HTTP/1.1 200", 0) == 0);
    (*a)->close();
    s.stop();
}

TEST_CASE("R-057: one peer may hold only its share of the body budget") {
    http::BodyBudget b(1000, 4);
    CHECK(b.take(250, "p"));
    CHECK(!b.take(1, "p"));    // p's share is used up
    CHECK(b.take(250, "q"));   // another peer still gets its own
    CHECK_EQ(b.in_use(), 500u);
    b.give(250, "p");
    CHECK_EQ(b.in_use_by("p"), 0u);
    CHECK(b.take(250, "p"));
    http::BodyBudget whole(1000, 1);
    CHECK(whole.take(1000, "p"));
    CHECK(!whole.take(1, "q"));
}

TEST_CASE("R-058: refusing busy connections does not stall the accept thread") {
    http::ServerConfig cfg;
    cfg.max_connections = 1;
    cfg.max_connections_per_peer = 0;
    cfg.limits.read_timeout_ms = 5000;
    http::Server s;
    REQUIRE(s.start(cfg, [](const http::Request&, http::Response& rs) { rs.body = "ok"; }).is_ok());
    auto holder = net::tcp_connect("127.0.0.1", s.port(), 2000);
    REQUIRE(holder);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Busy connections that never send or close: each 503 lingers.
    std::vector<std::unique_ptr<net::Transport>> idle;
    for (int i = 0; i < 12; ++i) {
        auto t = net::tcp_connect("127.0.0.1", s.port(), 2000);
        if (t) idle.push_back(std::move(t).value());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    (*holder)->close();  // the slot frees
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t0 = clock_type::now();
    const std::string r = exchange(s.port(), "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    const long long ms = ms_since(t0);
    CHECK(r.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(ms < 1500);  // was 12 x 250 ms of lingering on the accept thread first
    for (auto& t : idle) t->close();
    s.stop();
}

TEST_CASE("R-058: busy connections still read a 503 before the close") {
    http::ServerConfig cfg;
    cfg.max_connections = 1;
    cfg.max_connections_per_peer = 0;
    http::Server s;
    REQUIRE(s.start(cfg, [](const http::Request&, http::Response& rs) { rs.body = "ok"; }).is_ok());
    auto holder = net::tcp_connect("127.0.0.1", s.port(), 2000);
    REQUIRE(holder);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string r = exchange(s.port(), "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK(r.rfind("HTTP/1.1 503", 0) == 0);
    (*holder)->close();
    s.stop();
}

TEST_CASE("R-016: per-peer limits group IPv6 by /64 and IPv4-mapped addresses as IPv4") {
    CHECK_EQ(net::peer_group("192.0.2.7"), std::string("192.0.2.7"));
    CHECK_EQ(net::peer_group("::ffff:192.0.2.7"), std::string("192.0.2.7"));
    CHECK_EQ(net::peer_group("2001:db8:1:2::1"), net::peer_group("2001:db8:1:2:ffff:1:2:3"));
    CHECK(net::peer_group("2001:db8:1:2::1") != net::peer_group("2001:db8:1:3::1"));
    CHECK(net::peer_group("2001:db8:1:2::1", 128) != net::peer_group("2001:db8:1:2::2", 128));
    CHECK_EQ(net::peer_group("[2001:db8:1:2::1]"), net::peer_group("2001:db8:1:2::9"));
    CHECK_EQ(net::peer_group("fe80::1%eth0"), net::peer_group("fe80::2"));
    CHECK_EQ(net::peer_group("not-an-address"), std::string("not-an-address"));
}

TEST_CASE("R-036: loopback is decided by parsing the address, not by a text prefix") {
    CHECK(http::is_loopback_address("127.0.0.1"));
    CHECK(http::is_loopback_address("127.1.2.3"));
    CHECK(http::is_loopback_address("::1"));
    CHECK(http::is_loopback_address("[::1]"));
    CHECK(http::is_loopback_address("::ffff:127.0.0.1"));
    CHECK(http::is_loopback_address("localhost"));
    CHECK(!http::is_loopback_address("127.example.com"));
    CHECK(!http::is_loopback_address("127.0.0.1.evil"));
    CHECK(!http::is_loopback_address("0.0.0.0"));
    CHECK(!http::is_loopback_address(""));
    CHECK(!http::is_loopback_address("::"));
    // The server refuses plain HTTP on such a name without allow_insecure_remote.
    http::ServerConfig cfg;
    cfg.bind_address = "127.example.com";
    http::Server s;
    CHECK(!s.start(cfg, [](const http::Request&, http::Response&) {}).is_ok());
}
