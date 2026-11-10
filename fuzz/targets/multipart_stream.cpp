// Fuzz target: the incremental multipart/related parser (multipart::StreamParser, used by the
// streaming WADO-RS retrieve) against the whole-body parser (R-132). Input: one byte that seeds
// the split points, the boundary up to the first LF, then the body. Properties:
//   - feeding the body in pieces of any sizes gives exactly what feeding it at once gives (the
//     same parts in the same order, or failure in both);
//   - a body multipart::parse accepts is accepted by the StreamParser too, with the same part
//     headers and bodies.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fuzz.hpp"
#include "web/multipart.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

struct Seen {
    std::vector<std::pair<std::string, std::string>> parts;  // (Content-Type or "", body)
    bool ok = false;
};

// Runs the StreamParser over `body`, cut into pieces whose lengths come from `seed` (0: one piece).
Seen run_stream(std::string_view boundary, std::string_view body, const multipart::Limits& lim, std::uint32_t seed) {
    Seen out;
    multipart::StreamParser sp(std::string(boundary), lim, body.size() + 1, [&](const multipart::Part& p) -> Status {
        const std::string* ct = p.content_type();
        out.parts.emplace_back(ct ? *ct : std::string(), std::string(p.body));
        return Status::ok();
    });
    std::uint32_t x = seed;
    std::size_t pos = 0;
    bool failed = false;
    while (pos < body.size() && !failed) {
        std::size_t n = body.size() - pos;
        if (seed != 0) {
            x = x * 1664525u + 1013904223u;  // a small LCG: reproducible piece lengths 1..(1..64)
            const std::size_t span = 1 + ((x >> 24) & 0x3Fu);
            n = std::min<std::size_t>(n, 1 + ((x >> 8) % span));
        }
        failed = !sp.feed(body.substr(pos, n));
        pos += n;
    }
    out.ok = !failed && static_cast<bool>(sp.finish());
    return out;
}

}  // namespace

SIAARA_FUZZ_TARGET(multipart_stream) {
    siaara_fuzz::Input in(data, size);
    const std::uint32_t seed = in.u8();
    const std::string_view boundary = in.until('\n');
    const std::string_view body = in.rest();
    if (!multipart::valid_boundary(boundary)) return 0;
    multipart::Limits lim;
    lim.max_parts = 1000;
    const Seen whole = run_stream(boundary, body, lim, 0);
    for (const std::uint32_t s : {seed | 0x100u, (seed * 2654435761u) | 1u, 0x9E3779B9u ^ seed}) {
        const Seen split = run_stream(boundary, body, lim, s);
        FUZZ_REQUIRE(split.ok == whole.ok);
        if (whole.ok) FUZZ_REQUIRE(split.parts == whole.parts);
    }
    auto parsed = multipart::parse(body, boundary, lim);
    if (parsed) {
        FUZZ_REQUIRE(whole.ok);
        FUZZ_REQUIRE(whole.parts.size() == parsed->size());
        for (std::size_t i = 0; i < parsed->size(); ++i) {
            const std::string* ct = parsed.value()[i].content_type();
            FUZZ_REQUIRE(whole.parts[i].first == (ct ? *ct : std::string()));
            FUZZ_REQUIRE(whole.parts[i].second == parsed.value()[i].body);
        }
    }
    return 0;
}
