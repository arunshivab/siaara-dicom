// The display-safe text cleaner (core/display_safe.hpp, siaara_text_display_safe): controls,
// separators, format characters, malformed UTF-8, the C API buffer rules.
#include <cstdint>
#include <cstring>
#include <string>

#include "core/display_safe.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

std::string clean(const std::string& s) {
    char buf[512];
    size_t needed = 0;
    if (siaara_text_display_safe(s.data(), s.size(), buf, sizeof buf, &needed) != SIAARA_OK) return "<error>";
    return std::string(buf, needed);
}

}  // namespace

TEST_CASE("display text: display-safe copy - controls, separators, format characters, bad UTF-8") {
    // C0, DEL, C1 (U+0085), the line and paragraph separators: a space each.
    CHECK(clean("A\x01" "B\x7F" "C\xC2\x85" "D\xE2\x80\xA8" "E\xE2\x80\xA9" "F") == "A B C D E F");
    CHECK(clean(std::string("a\0b", 3)) == "a b");
    // Format characters (Cf) removed: RLO, ZWSP, ZWJ, BOM, soft hyphen, isolates, tag characters, Arabic number sign.
    CHECK(clean("x\xE2\x80\xAE" "gpj.exe\xE2\x80\x8B\xE2\x80\x8D\xEF\xBB\xBF\xC2\xAD\xE2\x81\xA6\xF3\xA0\x81\x81\xD8\x80" "y") ==
          "xgpj.exey");
    // Every byte not in a well-formed UTF-8 sequence: U+FFFD each.
    CHECK(clean("H\xFF" "I\xC3" " ") == "H\xEF\xBF\xBDI\xEF\xBF\xBD ");
    CHECK(clean("\xE0\x80\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // overlong
    CHECK(clean("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // an encoded surrogate
    CHECK(clean("\xF4\x90\x80\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // above U+10FFFF
    CHECK(clean("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    // Everything else stays: Latin, Tamil, an emoji, U+FFFD itself.
    const std::string keep = "M\xC3\xBCller \xE0\xAE\xA4\xE0\xAE\xAE\xE0\xAE\xBF\xE0\xAE\xB4\xE0\xAF\x8D \xF0\x9F\x98\x80 \xEF\xBF\xBD";
    CHECK(clean(keep) == keep);
    CHECK(clean("") == "");
    // The C API: NULL text with length 0; buffer rules; a cut never splits a character.
    size_t needed = 7;
    char one[1] = {'x'};
    CHECK_EQ(siaara_text_display_safe(nullptr, 0, one, sizeof one, &needed), SIAARA_OK);
    CHECK_EQ(needed, 0u);
    CHECK(one[0] == '\0');
    CHECK_EQ(siaara_text_display_safe(nullptr, 3, one, sizeof one, &needed), SIAARA_ERR_INVALID_ARGUMENT);
    const std::string e = "\xC3\xA9\xC3\xA9";  // two two-byte characters
    char small[4];
    CHECK_EQ(siaara_text_display_safe(e.data(), e.size(), small, sizeof small, &needed), SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(needed, 4u);
    CHECK(std::string(small) == "\xC3\xA9");
    CHECK_EQ(siaara_text_display_safe(e.data(), e.size(), nullptr, 0, &needed), SIAARA_OK);
    CHECK_EQ(needed, 4u);
    // The size function agrees with the copy, and growth is bounded by 3 x.
    for (const std::string& s : {std::string("\xFF\xFF\xFF"), keep, std::string("a\xE2\x80\xAE" "b")}) {
        CHECK_EQ(text::display_safe_size(s), text::display_safe(s).size());
        CHECK(text::display_safe(s).size() <= 3 * s.size());
    }
}

TEST_CASE("display text: every format character of Unicode 16 is removed, every other code point is kept or a space") {
    for (std::uint32_t c = 0; c <= 0x10FFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        std::string u;
        if (c < 0x80) {
            u.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            u.push_back(static_cast<char>(0xC0 | (c >> 6)));
            u.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            u.push_back(static_cast<char>(0xE0 | (c >> 12)));
            u.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            u.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            u.push_back(static_cast<char>(0xF0 | (c >> 18)));
            u.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            u.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            u.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        const std::string out = text::display_safe(u);
        const bool space = c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029;
        if (space) {
            if (out != " ") CHECK(out == " ");
        } else if (text::is_format_character(c)) {
            if (!out.empty()) CHECK(out.empty());
        } else if (out != u) {
            CHECK(out == u);
        }
    }
    CHECK(text::is_format_character(0x202E));
    CHECK(text::is_format_character(0xE007F));
    CHECK(!text::is_format_character(0x200A));  // a space character (Zs), not Cf
    CHECK(!text::is_format_character(0x2065));  // unassigned
}

