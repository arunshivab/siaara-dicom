// HTTP/1.1 message parsing and writing (RFC 9110 / 9112), media types, URLs and multipart/related
// (RFC 2387 / 2046): valid messages, every limit, malformed input, every truncation and random
// corruption, using an in-memory Transport.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "net/socket.hpp"
#include "web/http.hpp"
#include "web/multipart.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

// Serves a fixed byte string in small pieces, then reports orderly close (or a timeout).
class MemTransport : public net::Transport {
public:
    explicit MemTransport(std::string in, std::size_t piece = 7, bool timeout_at_end = false)
        : in_(std::move(in)), piece_(piece), timeout_(timeout_at_end) {}
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int) override {
        if (pos_ >= in_.size()) {
            if (timeout_) return Status(Errc::Timeout, "timeout");
            return std::size_t{0};
        }
        const std::size_t k = std::min({n, piece_, in_.size() - pos_});
        std::memcpy(buf, in_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    Status write_all(const std::uint8_t* d, std::size_t n, int) override {
        out.append(reinterpret_cast<const char*>(d), n);
        return Status::ok();
    }
    Result<bool> wait_readable(int) override { return pos_ < in_.size(); }
    void shutdown() noexcept override {}
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }
    std::string peer_address() const override { return "mem"; }
    std::string out;

private:
    std::string in_;
    std::size_t pos_ = 0;
    std::size_t piece_;
    bool timeout_;
    bool open_ = true;
};

struct Parsed {
    Result<http::Request> rq;
    int status;
    std::string written;
};

Parsed parse_request(const std::string& wire, const http::Limits& lim = {}, std::size_t piece = 7, bool timeout = false) {
    auto t = std::make_unique<MemTransport>(wire, piece, timeout);
    MemTransport* raw = t.get();
    http::Connection c(std::move(t), lim);
    auto r = c.read_request(1000);
    return {std::move(r), c.suggested_status(), raw->out};
}

}  // namespace

TEST_CASE("web/http request parsing") {
    auto p = parse_request("GET /a/b%20c?x=1&y=%2F&z HTTP/1.1\r\nHost: h\r\nAccept: */*\r\nX-Empty:\r\n\r\n");
    REQUIRE(p.rq.is_ok());
    CHECK_EQ(p.rq->method, std::string("GET"));
    CHECK_EQ(p.rq->target, std::string("/a/b%20c?x=1&y=%2F&z"));
    CHECK_EQ(*http::find_header(p.rq->headers, "accept"), std::string("*/*"));
    CHECK_EQ(*http::find_header(p.rq->headers, "X-EMPTY"), std::string(""));
    // Content-Length body, bare LF line ends, leading empty line.
    p = parse_request("\r\nPOST / HTTP/1.1\nHost: h\nContent-Length: 5\n\nhello");
    REQUIRE(p.rq.is_ok());
    CHECK_EQ(p.rq->body, std::string("hello"));
    // Chunked with extensions and trailers.
    p = parse_request("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nTrailer: x\r\n\r\n");
    REQUIRE(p.rq.is_ok());
    CHECK_EQ(p.rq->body, std::string("hello world"));
    // Expect: 100-continue gets an interim response.
    p = parse_request("PUT / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\nx");
    REQUIRE(p.rq.is_ok());
    CHECK_EQ(p.written, std::string("HTTP/1.1 100 Continue\r\n\r\n"));
    // Identical repeated Content-Length values are allowed.
    p = parse_request("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2, 2\r\n\r\nab");
    CHECK(p.rq.is_ok());
    // HTTP/1.0 without Host.
    p = parse_request("GET / HTTP/1.0\r\n\r\n");
    REQUIRE(p.rq.is_ok());
    CHECK(http::wants_close(p.rq->headers, p.rq->version_minor));
    CHECK(!http::wants_close({{"Connection", "Keep-Alive"}}, 0));
    CHECK(http::wants_close({{"Connection", "foo, close"}}, 1));
}

TEST_CASE("web/http request errors map to status codes") {
    struct Bad {
        const char* wire;
        int status;
    } cases[] = {
        {"GET / HTTP/1.1\r\n\r\n", 400},                                            // no Host
        {"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400},                      // two Hosts
        {"GET  / HTTP/1.1\r\nHost: h\r\n\r\n", 400},                                // double space
        {"GET / HTTP/2.0\r\nHost: h\r\n\r\n", 505},
        {"GET / HTTX/1.1\r\nHost: h\r\n\r\n", 400},
        {"G(T / HTTP/1.1\r\nHost: h\r\n\r\n", 400},
        {"GET /\x01 HTTP/1.1\r\nHost: h\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n", 400},                      // obs-fold
        {"GET / HTTP/1.1\r\nHost : h\r\n\r\n", 400},                                // space before colon
        {"GET / HTTP/1.1\r\nHost: h\rX: y\r\n\r\n", 400},                           // bare CR
        {"GET / HTTP/1.1\r\nHost: h\r\nX: a\x01" "b\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1, 2\r\n\r\nab", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nab\r\n0\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFF\r\n", 413},
        {"POST / HTTP/1.1\r\nHost: h\r\nExpect: magic\r\nContent-Length: 1\r\n\r\nx", 417},
        {"\r\n\r\n\r\n\r\n\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n", 400},
    };
    for (const auto& c : cases) {
        auto p = parse_request(c.wire);
        if (p.rq.is_ok() || p.status != c.status)
            std::fprintf(stderr, "  case %s: ok=%d status=%d\n", c.wire, p.rq.is_ok() ? 1 : 0, p.status);
        CHECK(!p.rq.is_ok());
        CHECK_EQ(p.status, c.status);
    }
    // Orderly close or idle timeout before a request: nothing to answer.
    auto p = parse_request("");
    CHECK(!p.rq.is_ok());
    CHECK_EQ(p.status, 0);
    p = parse_request("", {}, 7, true);
    CHECK(!p.rq.is_ok() && p.rq.error().code == Errc::Timeout);
    CHECK_EQ(p.status, 0);
    // Timeout in the middle of the headers: 408.
    p = parse_request("GET / HTTP/1.1\r\nHost: h\r\n", {}, 7, true);
    CHECK_EQ(p.status, 408);
}

TEST_CASE("web/http limits") {
    http::Limits lim;
    lim.max_request_line = 32;
    auto p = parse_request("GET /" + std::string(100, 'a') + " HTTP/1.1\r\nHost: h\r\n\r\n", lim);
    CHECK_EQ(p.status, 414);
    lim = {};
    lim.max_headers = 3;
    p = parse_request("GET / HTTP/1.1\r\nHost: h\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", lim);
    CHECK_EQ(p.status, 431);
    lim = {};
    lim.max_header_bytes = 64;
    p = parse_request("GET / HTTP/1.1\r\nHost: h\r\nA: " + std::string(100, 'x') + "\r\n\r\n", lim);
    CHECK_EQ(p.status, 431);
    lim = {};
    lim.max_body = 4;
    p = parse_request("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello", lim);
    CHECK_EQ(p.status, 413);
    p = parse_request("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n", lim);
    CHECK_EQ(p.status, 413);
    lim = {};
    lim.max_chunk_line = 8;
    p = parse_request("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3;long-extension\r\nabc\r\n0\r\n\r\n", lim);
    CHECK(!p.rq.is_ok());
    // A lying Content-Length does not allocate up front and ends as truncated.
    p = parse_request("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1000000000\r\n\r\nabc");
    CHECK(!p.rq.is_ok());
    CHECK(p.rq.error().code == Errc::Truncated);
}

TEST_CASE("web/http response parsing and writing") {
    auto t = std::make_unique<MemTransport>(
        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\nHTTP/1.0 200 OK\r\n\r\nuntil close");
    http::Connection c(std::move(t), {});
    auto r1 = c.read_response("GET");
    REQUIRE(r1.is_ok());
    CHECK_EQ(r1->status, 200);
    CHECK_EQ(r1->body, std::string("abc"));
    auto r2 = c.read_response("GET");
    REQUIRE(r2.is_ok());
    CHECK_EQ(r2->status, 204);
    auto r3 = c.read_response("GET");
    REQUIRE(r3.is_ok());
    CHECK_EQ(r3->body, std::string("until close"));
    // Writing: Content-Length is added, header injection is refused.
    auto w = std::make_unique<MemTransport>("");
    MemTransport* raw = w.get();
    http::Connection wc(std::move(w), {});
    http::Response rs;
    rs.status = 404;
    rs.set("Content-Type", "text/plain");
    rs.body = "nope";
    CHECK(wc.write_response(rs, false, true).is_ok());
    CHECK_EQ(raw->out, std::string("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnope"));
    rs.set("X-Evil", "a\r\nInjected: 1");
    CHECK(!wc.write_response(rs, false, false).is_ok());
    http::Request rq;
    rq.method = "GET";
    rq.target = "/x y";
    CHECK(!wc.write_request(rq).is_ok());
    const char* bad_status[] = {"HTTP/1.1 20 OK\r\n\r\n", "HTTP/2 200 OK\r\n\r\n", "HTTP/1.1 200OK\r\n\r\n", "garbage\r\n\r\n",
                                "HTTP/1.1 600 X\r\n\r\n"};
    for (const char* s : bad_status) {
        http::Connection bc(std::make_unique<MemTransport>(s), {});
        CHECK(!bc.read_response("GET").is_ok());
    }
}

TEST_CASE("web/http every truncation and random corruption never crash") {
    const std::string good =
        "POST /dicomweb/studies?x=%41 HTTP/1.1\r\nHost: localhost:8080\r\nContent-Type: multipart/related; type=\"application/dicom\"; boundary=b\r\n"
        "Transfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n2;x=y\r\nef\r\n0\r\nT: 1\r\n\r\n";
    REQUIRE(parse_request(good).rq.is_ok());
    for (std::size_t n = 0; n < good.size(); ++n) {
        auto p = parse_request(good.substr(0, n), {}, 3);
        CHECK(!p.rq.is_ok());
    }
    std::mt19937 rng(99);
    const char noise[] = "\r\n :;,=\"0123456789abcdefABCDEF\x01\x7F\xFF/";
    for (int iter = 0; iter < 4000; ++iter) {
        std::string m = good;
        for (int k = 0; k < 1 + static_cast<int>(rng() % 3); ++k) {
            const std::size_t pos = rng() % m.size();
            switch (rng() % 3) {
                case 0: m[pos] = noise[rng() % (sizeof noise - 1)]; break;
                case 1: m.erase(pos, 1 + rng() % 5); break;
                default: m.insert(pos, 1, noise[rng() % (sizeof noise - 1)]); break;
            }
            if (m.empty()) m = "G";
        }
        auto p = parse_request(m, {}, 1 + rng() % 16);
        if (!p.rq.is_ok()) CHECK(p.status == 0 || (p.status >= 400 && p.status < 600));
    }
    const std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX: y\r\n\r\nhello";
    for (std::size_t n = 0; n < resp.size(); ++n) {
        http::Connection c(std::make_unique<MemTransport>(resp.substr(0, n), 2), {});
        CHECK(!c.read_response("GET").is_ok());
    }
}

TEST_CASE("web/http media types and Accept") {
    auto m = http::parse_media_type("Multipart/Related; Type=\"application/dicom\"; boundary=\"a b\\\"c\"; transfer-syntax=1.2.840.10008.1.2.1");
    REQUIRE(m.is_ok());
    CHECK(m->is("multipart", "related"));
    CHECK_EQ(*m->param("type"), std::string("application/dicom"));
    CHECK_EQ(*m->param("boundary"), std::string("a b\"c"));
    CHECK_EQ(*m->param("transfer-syntax"), std::string("1.2.840.10008.1.2.1"));
    CHECK(!http::parse_media_type("text").is_ok());
    CHECK(!http::parse_media_type("*/*").is_ok());
    CHECK(!http::parse_media_type("a/b; x").is_ok());
    CHECK(!http::parse_media_type("a/b; x=\"unterminated").is_ok());
    auto acc = http::parse_accept("text/html;q=0.5, multipart/related; type=\"application/dicom\"; transfer-syntax=*, */*;q=0.1, bad, a/b;q=2");
    REQUIRE(acc.size() == 3);
    CHECK(acc[0].is("multipart", "related"));
    CHECK_EQ(*acc[0].param("transfer-syntax"), std::string("*"));
    CHECK_EQ(acc[1].weight, 500);
    CHECK_EQ(acc[2].weight, 100);
    CHECK_EQ(http::format_media_type(*m), std::string("multipart/related; type=\"application/dicom\"; boundary=\"a b\\\"c\"; transfer-syntax=1.2.840.10008.1.2.1"));
    auto list = http::header_list({{"A", "x, \"y,z\" ,w"}, {"a", "v"}}, "A");
    REQUIRE(list.size() == 4);
    CHECK_EQ(list[1], std::string("\"y,z\""));
}

TEST_CASE("web/http targets and URLs") {
    auto t = http::parse_target("/dicomweb/studies/1.2%2F3/series?PatientName=Doe%5E*&a+b=c+d&&x");
    REQUIRE(t.is_ok());
    REQUIRE(t->segments.size() == 4);
    CHECK_EQ(t->segments[2], std::string("1.2/3"));
    REQUIRE(t->query.size() == 3);
    CHECK_EQ(t->query[0].second, std::string("Doe^*"));
    CHECK_EQ(t->query[1].first, std::string("a b"));
    CHECK_EQ(t->query[1].second, std::string("c d"));
    CHECK(!http::parse_target("relative").is_ok());
    CHECK(!http::parse_target("/a%2").is_ok());
    CHECK(!http::parse_target("/a%zz").is_ok());
    CHECK(!http::parse_target("/a/../b").is_ok());
    CHECK_EQ(http::percent_encode("a b/^*~"), std::string("a%20b%2F%5E%2A~"));
    CHECK_EQ(http::percent_encode_query("application/dicom; a=b&c+d*,e#%"), std::string("application/dicom;%20a%3Db%26c%2Bd*,e%23%25"));
    auto rt = http::parse_target("/x?k=" + http::percent_encode_query("Doe^J* a+b=c&d"));
    REQUIRE(rt.is_ok() && rt->query.size() == 1);
    CHECK_EQ(rt->query[0].second, std::string("Doe^J* a+b=c&d"));
    auto u = http::parse_url("http://[::1]:8042/dicom-web/");
    REQUIRE(u.is_ok());
    CHECK_EQ(u->host, std::string("::1"));
    CHECK_EQ(u->port, 8042);
    CHECK_EQ(u->path, std::string("/dicom-web"));
    CHECK(http::parse_url("https://x/").error().code == Errc::Unsupported);
    CHECK(!http::parse_url("http://x:0/").is_ok());
    CHECK(!http::parse_url("http://x:70000/").is_ok());
    CHECK(!http::parse_url("http://user@x/").is_ok());
    CHECK(!http::parse_url("http:///").is_ok());
}

TEST_CASE("web/multipart round trip, limits and malformed input") {
    multipart::Writer w;
    w.add("application/dicom", std::string("part one\r\n--not-a-boundary\r\n"));
    w.add("application/octet-stream", std::string("\0\1\2", 3), {{"Content-Location", "x"}});
    w.add("text/plain", "");
    const std::string body = w.finish();
    CHECK(w.content_type("application/dicom").find("type=\"application/dicom\"; boundary=") != std::string::npos);
    auto parts = multipart::parse(body, w.boundary());
    REQUIRE(parts.is_ok());
    REQUIRE(parts->size() == 3);
    CHECK_EQ(std::string(parts->at(0).body), std::string("part one\r\n--not-a-boundary\r\n"));
    CHECK_EQ(std::string(parts->at(1).body), std::string("\0\1\2", 3));
    CHECK_EQ(*http::find_header(parts->at(1).headers, "content-location"), std::string("x"));
    CHECK(parts->at(2).body.empty());
    // Preamble, transport padding, epilogue, LF-only line ends, part without headers.
    auto p2 = multipart::parse("preamble\r\n--b  \r\nContent-Type: a/b\r\n\r\nX\r\n--b\n\nY\n--b--\r\nepilogue", "b");
    REQUIRE(p2.is_ok());
    REQUIRE(p2->size() == 2);
    CHECK_EQ(std::string(p2->at(0).body), std::string("X"));
    CHECK(p2->at(1).headers.empty());
    CHECK_EQ(std::string(p2->at(1).body), std::string("Y"));
    // The boundary inside content (not at a line start, or followed by other text) is data.
    auto p3 = multipart::parse("--b\r\n\r\nx--b y\r\n--bc\r\n--b--", "b");
    REQUIRE(p3.is_ok());
    CHECK_EQ(std::string(p3->at(0).body), std::string("x--b y\r\n--bc"));
    // Errors.
    CHECK(!multipart::parse(body, "").is_ok());
    CHECK(!multipart::parse(body, std::string(71, 'a')).is_ok());
    CHECK(!multipart::parse(body, "bad\"char").is_ok());
    CHECK(!multipart::parse("no delimiter", "b").is_ok());
    CHECK(!multipart::parse("--b\r\n\r\nunterminated", "b").is_ok());
    {
        auto pre = multipart::parse("--bX\r\n\r\nx\r\n--b--", "b");  // "--bX" line is preamble
        CHECK(pre.is_ok() && pre->empty());
    }
    CHECK(!multipart::parse("--b\r\nNoColon\r\n\r\nx\r\n--b--", "b").is_ok());
    CHECK(!multipart::parse("--b\r\nA: 1\r\n folded\r\n\r\nx\r\n--b--", "b").is_ok());
    multipart::Limits lim;
    lim.max_parts = 2;
    CHECK(multipart::parse(body, w.boundary(), lim).error().code == Errc::LimitExceeded);
    lim = {};
    lim.max_boundary = 8;
    CHECK(!multipart::parse(body, w.boundary(), lim).is_ok());
    lim = {};
    lim.max_part_headers = 1;
    CHECK(multipart::parse("--b\r\nA: 1\r\nB: 2\r\n\r\nx\r\n--b--", "b", lim).error().code == Errc::LimitExceeded);
    lim = {};
    lim.max_part_header_bytes = 10;
    CHECK(multipart::parse("--b\r\nA: 12345678901\r\n\r\nx\r\n--b--", "b", lim).error().code == Errc::LimitExceeded);
    // Every truncation and random corruption.
    for (std::size_t n = 0; n + 2 < body.size(); ++n) {
        auto r = multipart::parse(std::string_view(body).substr(0, n), w.boundary());
        CHECK(!r.is_ok());
    }
    std::mt19937 rng(5);
    for (int iter = 0; iter < 3000; ++iter) {
        std::string m = body;
        const std::size_t pos = rng() % m.size();
        switch (rng() % 3) {
            case 0: m[pos] = "-\r\n:ab"[rng() % 6]; break;
            case 1: m.erase(pos, 1 + rng() % 6); break;
            default: m.insert(pos, 1, "-\r\n:ab"[rng() % 6]); break;
        }
        auto r = multipart::parse(m, w.boundary());
        if (r) for (const auto& part : *r) CHECK(part.body.data() >= m.data() && part.body.data() + part.body.size() <= m.data() + m.size());
    }
}
