// Fuzz target: the display-safe text cleaner (siaara_text_display_safe, core/display_safe.hpp).
// Properties: the copy is well-formed UTF-8 without controls, line / paragraph separators or
// format characters; it is at most 3 x the input (at most the input when that is well formed);
// cleaning it again changes nothing; a cut copy (small buffer) is a prefix ending on a character.
#include <string>
#include <vector>

#include "core/display_safe.hpp"
#include "fuzz.hpp"
#include "siaara/siaara.h"

namespace {

// Decodes well-formed UTF-8 or fails the run.
std::vector<std::uint32_t> decode_strict(const std::string& s) {
    std::vector<std::uint32_t> out;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint32_t c = p[i];
        std::size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        FUZZ_REQUIRE(c < 0x80 || c >= 0xC2);
        FUZZ_REQUIRE(c <= 0xF4);
        FUZZ_REQUIRE(i + n <= s.size());
        if (n > 1) c &= (0xFFu >> (n + 1));
        for (std::size_t k = 1; k < n; ++k) {
            FUZZ_REQUIRE((p[i + k] & 0xC0) == 0x80);
            c = (c << 6) | (p[i + k] & 0x3Fu);
        }
        FUZZ_REQUIRE(!(n == 3 && c < 0x800) && !(n == 4 && (c < 0x10000 || c > 0x10FFFF)));
        FUZZ_REQUIRE(c < 0xD800 || c > 0xDFFF);
        out.push_back(c);
        i += n;
    }
    return out;
}

}  // namespace

SIAARA_FUZZ_TARGET(display_text) {
    const std::string in(reinterpret_cast<const char*>(data), size);
    const std::string out = siaara::text::display_safe(in);
    FUZZ_REQUIRE(out.size() <= 3 * in.size());
    FUZZ_REQUIRE(siaara::text::display_safe_size(in) == out.size());
    for (std::uint32_t c : decode_strict(out)) {
        FUZZ_REQUIRE(c >= 0x20 && !(c >= 0x7F && c <= 0x9F) && c != 0x2028 && c != 0x2029);
        FUZZ_REQUIRE(!siaara::text::is_format_character(c));
    }
    FUZZ_REQUIRE(siaara::text::display_safe(out) == out);
    // The C API: the same copy; a small buffer gets a prefix that ends on a character boundary.
    std::vector<char> buf(out.size() + 1);
    std::size_t needed = 0;
    FUZZ_REQUIRE(siaara_text_display_safe(in.data(), in.size(), buf.data(), buf.size(), &needed) == SIAARA_OK);
    FUZZ_REQUIRE(needed == out.size() && std::string(buf.data(), needed) == out);
    if (!out.empty()) {
        const std::size_t cap = 1 + (in.empty() ? 0 : data[0] % out.size());
        std::vector<char> small(cap);
        FUZZ_REQUIRE(siaara_text_display_safe(in.data(), in.size(), small.data(), cap, &needed) ==
                     (out.size() + 1 > cap ? SIAARA_ERR_BUFFER_TOO_SMALL : SIAARA_OK));
        const std::string prefix(small.data());
        FUZZ_REQUIRE(out.compare(0, prefix.size(), prefix) == 0);
        (void)decode_strict(prefix);
    }
    return 0;
}
