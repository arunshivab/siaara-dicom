// Regression tests for the 2026-11-09 security review, HTTP / DICOMweb findings
// (R-006, R-007, R-008, R-016 (HTTP), R-018, R-019, R-020, R-036, R-037, R-038, R-039).
// Lines ending in "// R-0xx option" only set options introduced by the fixes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "crosscheck/web_memory_store.hpp"
#include "net/socket.hpp"
#include "web/dicom_json.hpp"
#include "web/http.hpp"
#include "web/json.hpp"
#include "web/server.hpp"
#include "web/xml.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

using clock_type = std::chrono::steady_clock;

long long ms_since(clock_type::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
}

// Serves `prefix` at once, then `rest` in pieces of `piece` bytes every `interval_ms`.
class SlowTransport : public net::Transport {
public:
    SlowTransport(std::string prefix, std::string rest, std::size_t piece, int interval_ms)
        : prefix_(std::move(prefix)), rest_(std::move(rest)), piece_(piece), interval_(interval_ms) {}
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override {
        if (n == 0) return std::size_t{0};
        if (ppos_ < prefix_.size()) {
            const std::size_t k = std::min(n, prefix_.size() - ppos_);
            std::memcpy(buf, prefix_.data() + ppos_, k);
            ppos_ += k;
            return k;
        }
        if (rpos_ >= rest_.size() || timeout_ms < interval_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(timeout_ms, 0)));
            return Status(Errc::Timeout, "timeout");
        }
        if (interval_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(interval_));
        const std::size_t k = std::min({n, piece_, rest_.size() - rpos_});
        std::memcpy(buf, rest_.data() + rpos_, k);
        rpos_ += k;
        return k;
    }
    Status write_all(const std::uint8_t* d, std::size_t n, int) override {
        out.append(reinterpret_cast<const char*>(d), n);
        return Status::ok();
    }
    Result<bool> wait_readable(int) override { return true; }
    void shutdown() noexcept override {}
    void close() noexcept override {}
    bool is_open() const noexcept override { return true; }
    std::string peer_address() const override { return "slow"; }
    std::string out;

private:
    std::string prefix_, rest_;
    std::size_t piece_;
    int interval_;
    std::size_t ppos_ = 0, rpos_ = 0;
};

struct Outcome {
    bool ok = false;
    Errc code = Errc::Ok;
    int status = 0;
    long long ms = 0;
};

Outcome read_one(std::unique_ptr<net::Transport> t, const http::Limits& lim, int first_byte_ms = 1000) {
    http::Connection c(std::move(t), lim);
    const auto t0 = clock_type::now();
    auto r = c.read_request(first_byte_ms);
    Outcome o;
    o.ms = ms_since(t0);
    o.ok = r.is_ok();
    if (!r) o.code = r.error().code;
    o.status = c.suggested_status();
    return o;
}

std::string raw_exchange(std::uint16_t port, const std::string& wire, int read_ms = 3000) {
    auto t = net::tcp_connect("127.0.0.1", port, 2000);
    if (!t) return std::string();
    (void)(*t)->write_all(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size(), 2000);
    std::string out;
    std::uint8_t buf[4096];
    for (;;) {
        auto r = (*t)->read_some(buf, sizeof buf, read_ms);
        if (!r || *r == 0) break;
        out.append(reinterpret_cast<const char*>(buf), *r);
    }
    return out;
}

http::Request get(const std::string& target) {
    http::Request rq;
    rq.method = "GET";
    rq.target = target;
    rq.headers.push_back({"Host", "h"});
    return rq;
}

// A 2-frame 8-bit native instance.
std::string two_frame_instance() {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.31");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.10");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.20");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_string(tags::NumberOfFrames, VR::IS, "2");
    (void)ds.set_uint16(tags::Rows, 16);
    (void)ds.set_uint16(tags::Columns, 16);
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(2 * 16 * 16, 7);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

}  // namespace

// ------------------------------------------------------------------ R-006

TEST_CASE("R-006: the header deadline covers a slowly dripped request line") {
    http::Limits lim;
    lim.read_timeout_ms = 1000;
    lim.header_timeout_ms = 300;
    // ~60-byte request line at 1 byte / 40 ms: each read is well inside read_timeout_ms.
    const std::string msg = "GET /" + std::string(48, 'a') + " HTTP/1.1\r\nHost: h\r\n\r\n";
    const Outcome o = read_one(std::make_unique<SlowTransport>("", msg, 1, 40), lim);
    CHECK(!o.ok);
    CHECK(o.code == Errc::Timeout);
    CHECK_EQ(o.status, 408);
    CHECK(o.ms < 1000);
}

// ------------------------------------------------------------------ R-007

TEST_CASE("R-007: request bodies below the minimum rate are cut off (Content-Length and chunked)") {
    http::Limits lim;
    lim.read_timeout_ms = 1000;
    lim.min_body_rate = 1000;        // R-007 option
    lim.body_rate_window_ms = 300;   // R-007 option
    {
        const std::string head = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1000\r\n\r\n";
        const Outcome o = read_one(std::make_unique<SlowTransport>(head, std::string(1000, 'x'), 1, 20), lim);
        CHECK(!o.ok);
        CHECK(o.code == Errc::Timeout);
        CHECK_EQ(o.status, 408);
        CHECK(o.ms < 1500);
    }
    {
        const std::string head = "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
        const std::string body = "3e8\r\n" + std::string(1000, 'x') + "\r\n0\r\n\r\n";
        const Outcome o = read_one(std::make_unique<SlowTransport>(head, body, 1, 20), lim);
        CHECK(!o.ok);
        CHECK(o.code == Errc::Timeout);
        CHECK(o.ms < 1500);
    }
}

TEST_CASE("R-007: request bodies have an overall deadline") {
    http::Limits lim;
    lim.read_timeout_ms = 1000;
    lim.min_body_rate = 0;        // R-007 option
    lim.body_timeout_ms = 400;    // R-007 option
    // 100 bytes / 50 ms: steady progress, 5 s for the whole body.
    const std::string head = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 10000\r\n\r\n";
    const Outcome o = read_one(std::make_unique<SlowTransport>(head, std::string(10000, 'x'), 100, 50), lim);
    CHECK(!o.ok);
    CHECK(o.code == Errc::Timeout);
    CHECK(o.ms < 1500);
    // A body arriving in time is read as before.
    http::Limits ok_lim;
    const Outcome good = read_one(std::make_unique<SlowTransport>(head, std::string(10000, 'x'), 4096, 0), ok_lim);
    CHECK(good.ok);
}

// ------------------------------------------------------------------ R-020

TEST_CASE("R-020: a long header line delivered one byte at a time is scanned linearly") {
    http::Limits lim;
    lim.max_header_bytes = 1u << 20;
    const std::string msg = "GET / HTTP/1.1\r\nHost: h\r\nX-Long: " + std::string(200000, 'a') + "\r\n\r\n";
    const Outcome o = read_one(std::make_unique<SlowTransport>("", msg, 1, 0), lim);
    CHECK(o.ok);
    CHECK(o.ms < 5000);
}

// ------------------------------------------------------------------ R-008

TEST_CASE("R-008: WADO-RS frame lists reject duplicates and oversized responses") {
    siaara_test_web::MemoryStore mem;
    const std::string inst = two_frame_instance();
    REQUIRE(mem.add_part10(Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(inst.data()), inst.size())).is_ok());
    const std::string base = "/dicomweb/studies/2.25.10/series/2.25.20/instances/2.25.31/frames/";
    {
        DicomwebServer s;
        REQUIRE(s.start({}, mem.make_store()).is_ok());
        http::Response rs;
        s.handle(get(base + "1,2"), rs);
        CHECK_EQ(rs.status, 200);
        rs = {};
        s.handle(get(base + "1,1"), rs);
        CHECK_EQ(rs.status, 400);
        rs = {};
        s.handle(get(base + "2,1,2,1"), rs);
        CHECK_EQ(rs.status, 400);
        s.stop();
    }
    {
        DicomwebServerConfig cfg;
        cfg.max_frames_response_bytes = 300;  // R-008 option
        DicomwebServer s;
        REQUIRE(s.start(cfg, mem.make_store()).is_ok());
        http::Response rs;
        s.handle(get(base + "1"), rs);
        CHECK_EQ(rs.status, 200);  // 256 bytes
        rs = {};
        s.handle(get(base + "1,2"), rs);
        CHECK_EQ(rs.status, 413);  // 512 bytes
        s.stop();
    }
}

// ------------------------------------------------------------------ R-016 (HTTP)

TEST_CASE("R-016: HTTP connections per peer address are limited") {
    http::ServerConfig cfg;
    cfg.max_connections_per_peer = 2;  // R-016 option
    cfg.limits.read_timeout_ms = 3000;
    http::Server s;
    REQUIRE(s.start(cfg, [](const http::Request&, http::Response& rs) { rs.body = "ok"; }).is_ok());
    auto a = net::tcp_connect("127.0.0.1", s.port(), 2000);
    auto b = net::tcp_connect("127.0.0.1", s.port(), 2000);
    REQUIRE(a);
    REQUIRE(b);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // both accepted and waiting
    const std::string third = raw_exchange(s.port(), "GET / HTTP/1.1\r\nHost: h\r\n\r\n", 1000);
    CHECK(third.rfind("HTTP/1.1 503", 0) == 0);
    (*a)->close();
    (*b)->close();
    s.stop();
}

// ------------------------------------------------------------------ R-018

TEST_CASE("R-018: request body defaults and the server-wide in-flight body budget") {
    CHECK(http::Limits{}.max_body <= (256ull << 20));
    http::ServerConfig cfg;
    cfg.max_inflight_body_bytes = 1000;  // R-018 option
    cfg.limits.read_timeout_ms = 3000;
    http::Server s;
    REQUIRE(s.start(cfg, [](const http::Request& rq, http::Response& rs) { rs.body = std::to_string(rq.body.size()); }).is_ok());
    // A holds 800 bytes of budget while its body is still arriving.
    auto a = net::tcp_connect("127.0.0.1", s.port(), 2000);
    REQUIRE(a);
    const std::string head = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 800\r\n\r\n";
    const std::string part = head + std::string(100, 'a');
    REQUIRE((*a)->write_all(reinterpret_cast<const std::uint8_t*>(part.data()), part.size(), 2000).is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // B would exceed the budget: 503.
    const std::string b = raw_exchange(s.port(), head + std::string(800, 'b'), 1500);
    CHECK(b.rfind("HTTP/1.1 503", 0) == 0);
    // A completes; afterwards the budget is free again.
    const std::string rest(700, 'a');
    REQUIRE((*a)->write_all(reinterpret_cast<const std::uint8_t*>(rest.data()), rest.size(), 2000).is_ok());
    std::string ra;
    std::uint8_t buf[512];
    for (int i = 0; i < 20 && ra.find("\r\n\r\n800") == std::string::npos; ++i) {
        auto r = (*a)->read_some(buf, sizeof buf, 1000);
        if (!r || *r == 0) break;
        ra.append(reinterpret_cast<const char*>(buf), *r);
    }
    CHECK(ra.rfind("HTTP/1.1 200", 0) == 0);
    (*a)->close();
    const std::string c = raw_exchange(s.port(), "POST / HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 800\r\n\r\n" + std::string(800, 'c'), 1500);
    CHECK(c.rfind("HTTP/1.1 200", 0) == 0);
    s.stop();
}

// ------------------------------------------------------------------ R-019

TEST_CASE("R-019: QIDO passes a capped limit so the store can stop early") {
    std::vector<std::optional<std::uint32_t>> seen;
    Store st;
    st.search = [&](const SearchQuery& q, std::vector<Dataset>& out) {
        seen.push_back(q.limit);
        for (int i = 0; i < 10; ++i) {
            Dataset d;
            (void)d.set_string(tags::StudyInstanceUID, VR::UI, "2.25." + std::to_string(i + 1));
            out.push_back(std::move(d));
        }
        return Status::ok();
    };
    DicomwebServerConfig cfg;
    cfg.max_qido_results = 3;
    DicomwebServer s;
    REQUIRE(s.start(cfg, st).is_ok());
    http::Response rs;
    s.handle(get("/dicomweb/studies?limit=5&offset=2"), rs);
    CHECK_EQ(rs.status, 200);
    CHECK(http::find_header(rs.headers, "Warning") != nullptr);  // still reported as cut
    rs = {};
    s.handle(get("/dicomweb/studies"), rs);
    CHECK_EQ(rs.status, 200);
    rs = {};
    s.handle(get("/dicomweb/studies?limit=2"), rs);
    REQUIRE(seen.size() == 3u);
    CHECK(seen[0] == std::optional<std::uint32_t>(4u));  // min(5, 3 + 1)
    CHECK(seen[1] == std::optional<std::uint32_t>(4u));  // no limit: 3 + 1
    CHECK(seen[2] == std::optional<std::uint32_t>(2u));
    auto page = datasets_from_json(rs.body);
    REQUIRE(page);
    CHECK_EQ(page->size(), 2u);
    s.stop();
}

// ------------------------------------------------------------------ R-036

TEST_CASE("R-036: plain HTTP / unauthenticated DICOMweb off loopback is refused") {
    {
        http::ServerConfig cfg;
        cfg.bind_address = "0.0.0.0";
        http::Server s;
        CHECK(s.start(cfg, [](const http::Request&, http::Response&) {}).code == Errc::InvalidArgument);
        s.stop();
    }
    {
        http::ServerConfig cfg;
        cfg.bind_address = "0.0.0.0";
        cfg.allow_insecure_remote = true;  // R-036 option
        std::vector<std::string> logs;
        cfg.log = [&](const std::string& m) { logs.push_back(m); };
        http::Server s;
        CHECK(s.start(cfg, [](const http::Request&, http::Response&) {}).is_ok());
        s.stop();
        bool warned = false;
        for (const auto& m : logs) warned = warned || m.find("warning") != std::string::npos;
        CHECK(warned);
    }
    {
        DicomwebServerConfig cfg;
        cfg.http.bind_address = "0.0.0.0";
        DicomwebServer s;
        CHECK(s.start(cfg, {}).code == Errc::InvalidArgument);
        s.stop();
    }
    {
        DicomwebServerConfig cfg;  // loopback (the default) stays fine
        DicomwebServer s;
        CHECK(s.start(cfg, {}).is_ok());
        s.stop();
    }
}

// ------------------------------------------------------------------ R-037

TEST_CASE("R-037: JSON and XML nesting defaults are bounded; DICOM nesting to 64 still works") {
    CHECK(json::ParseLimits{}.max_depth <= 128u);
    CHECK(xml::ParseLimits{}.max_depth <= 128u);
    std::string deep(200, '[');
    deep += std::string(200, ']');
    CHECK(!json::parse(deep));
    std::string x;
    for (int i = 0; i < 200; ++i) x += "<a>";
    for (int i = 0; i < 200; ++i) x += "</a>";
    CHECK(!xml::parse(x));
    // 64 nested sequences in DICOM JSON (the DICOM depth cap) still decode.
    std::string js;
    for (int i = 0; i < 63; ++i) js += R"({"00081140":{"vr":"SQ","Value":[)";
    js += "{}";
    for (int i = 0; i < 63; ++i) js += "]}}";
    CHECK(dataset_from_json(js).is_ok());
}

// ------------------------------------------------------------------ R-038

TEST_CASE("R-038: percent-decoded control characters are rejected in path and query") {
    CHECK(!http::parse_target("/a%00b"));
    CHECK(!http::parse_target("/a?x=%0a"));
    CHECK(!http::parse_target("/a?x%1F=1"));
    CHECK(!http::parse_target("/a%7F"));
    CHECK(http::parse_target("/a%20b?x=%C3%A9").is_ok());
    siaara_test_web::MemoryStore mem;
    DicomwebServer s;
    REQUIRE(s.start({}, mem.make_store()).is_ok());
    http::Response rs;
    s.handle(get("/dicomweb/studies?PatientName=%00"), rs);
    CHECK_EQ(rs.status, 400);
    s.stop();
}

// ------------------------------------------------------------------ R-039

TEST_CASE("R-039: CORS \"*\" is refused together with authorize") {
    DicomwebServerConfig cfg;
    cfg.cors_allow_origin = "*";
    cfg.authorize = [](const http::Request&) { return true; };
    {
        DicomwebServer s;
        CHECK(s.start(cfg, {}).code == Errc::InvalidArgument);
        s.stop();
    }
    cfg.cors_allow_origin = "https://viewer.example";
    {
        DicomwebServer s;
        CHECK(s.start(cfg, {}).is_ok());
        s.stop();
    }
}
