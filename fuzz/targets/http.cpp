// Fuzz target: HTTP/1.1 message parser (RFC 9112) as used by the DICOMweb server and client:
// requests (pipelined, chunked, Content-Length) or a response, chosen by the first byte, plus the
// media-type, Accept, request-target, URL and percent-decoding helpers on the header values.
#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "web/http.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {
http::Limits small_limits() {
    http::Limits l;
    l.max_body = 4u << 20;
    l.read_timeout_ms = l.write_timeout_ms = l.header_timeout_ms = 0;
    return l;
}

void helpers(const http::Headers& h) {
    for (const auto& f : h) {
        (void)http::parse_media_type(f.value);
        (void)http::parse_accept(f.value);
    }
    (void)http::header_list(h, "Accept");
    (void)http::wants_close(h, 1);
}
}  // namespace

SIAARA_FUZZ_TARGET(http) {
    if (size < 1) return 0;
    const std::uint8_t sel = data[0];
    const std::size_t chunk = 1 + (sel >> 2) * 61u;
    http::Connection c(std::make_unique<siaara_fuzz::MemTransport>(data + 1, size - 1, chunk), small_limits());
    if (sel & 1u) {
        auto r = c.read_response((sel & 2u) ? "HEAD" : "GET");
        if (r) helpers(r->headers);
        return 0;
    }
    for (int i = 0; i < 16; ++i) {
        auto r = c.read_request(0);
        if (!r) break;
        helpers(r->headers);
        auto t = http::parse_target(r->target);
        (void)t;
        (void)http::parse_url(r->target);
        (void)http::percent_decode(r->target, true);
    }
    return 0;
}
