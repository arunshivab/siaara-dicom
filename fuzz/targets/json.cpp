// Fuzz target: JSON parser (RFC 8259). Accepted text is written back; the written form must parse
// and write identically (stable canonical form), and must be valid UTF-8.
#include "fuzz.hpp"
#include "web/json.hpp"

using namespace siaara;
using namespace siaara::web;

SIAARA_FUZZ_TARGET(json) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    json::ParseLimits lim;
    lim.max_depth = 256;
    lim.max_values = 1'000'000;
    auto v = json::parse(text, lim);
    if (!v) return 0;
    std::string w1;
    json::write(w1, *v);
    FUZZ_REQUIRE(json::is_valid_utf8(w1));
    auto again = json::parse(w1, lim);
    FUZZ_REQUIRE(again.is_ok());
    std::string w2;
    json::write(w2, *again);
    FUZZ_REQUIRE(w1 == w2);
    return 0;
}
