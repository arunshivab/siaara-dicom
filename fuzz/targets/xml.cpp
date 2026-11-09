// Fuzz target: XML 1.0 parser used for the DICOM Native model (PS3.19 A.1).
#include "fuzz.hpp"
#include "web/xml.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {
std::size_t walk(const xml::Node& n, std::size_t depth) {
    std::size_t c = 1;
    for (const auto& k : n.children) c += walk(k, depth + 1);
    (void)n.attr("keyword");
    (void)n.child("Value");
    return c;
}
}  // namespace

SIAARA_FUZZ_TARGET(xml) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    xml::ParseLimits lim;
    lim.max_depth = 256;
    lim.max_nodes = 1'000'000;
    auto n = xml::parse(text, lim);
    if (!n) return 0;
    FUZZ_REQUIRE(walk(*n, 0) <= lim.max_nodes + 1);
    std::string esc;
    xml::append_escaped(esc, n->text, false);
    xml::append_escaped(esc, n->name, true);
    return 0;
}
