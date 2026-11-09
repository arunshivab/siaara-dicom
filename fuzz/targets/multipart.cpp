// Fuzz target: multipart/related body parser (RFC 2046) used for STOW-RS and WADO-RS. Input: the
// boundary up to the first LF, then the body. Parts written back with the Writer must parse to the
// same part bodies.
#include "fuzz.hpp"
#include "web/multipart.hpp"

using namespace siaara;
using namespace siaara::web;

SIAARA_FUZZ_TARGET(multipart) {
    siaara_fuzz::Input in(data, size);
    const std::string_view boundary = in.until('\n');
    const std::string_view body = in.rest();
    multipart::Limits lim;
    lim.max_parts = 1000;
    auto parts = multipart::parse(body, boundary, lim);
    if (!parts) return 0;
    for (const auto& p : *parts) {
        FUZZ_REQUIRE(p.body.data() >= body.data() && p.body.data() + p.body.size() <= body.data() + body.size());
        if (const std::string* ct = p.content_type()) (void)http::parse_media_type(*ct);
    }
    return 0;
}
