// Fuzz target: Specific Character Set parsing and text decoding (ISO 2022 escapes, UTF-8, GB18030,
// GBK, JIS, KS X 1001), then re-encoding. Input: the (0008,0005) value up to the first NUL, then
// the text bytes. Decoded text must be valid UTF-8 and delimiters must survive decoding.
#include <algorithm>

#include "charset/charset.hpp"
#include "fuzz.hpp"
#include "web/json.hpp"

using namespace siaara;

SIAARA_FUZZ_TARGET(charset) {
    siaara_fuzz::Input in(data, size);
    const std::string_view spec = in.until('\0');
    const std::string_view text = in.rest();
    const charset::CharacterSet cs = charset::CharacterSet::parse(spec);
    (void)charset::CharacterSet::parse(cs.to_string());
    const charset::TextKind kinds[] = {charset::TextKind::Text, charset::TextKind::MultiValued,
                                       charset::TextKind::PersonName};
    for (charset::TextKind k : kinds) {
        std::size_t repl = 0;
        const std::string u = charset::decode(text, cs, k, &repl);
        FUZZ_REQUIRE(web::json::is_valid_utf8(u));
        // In UTF-8 every 0x5C byte is a delimiter, and nothing else may decode to one.
        if (k == charset::TextKind::MultiValued && cs.scheme == charset::Scheme::Utf8)
            FUZZ_REQUIRE(std::count(u.begin(), u.end(), '\\') == std::count(text.begin(), text.end(), '\\'));
        const charset::EncodeResult e = charset::encode(u, cs, k);
        (void)e;
        if (k == charset::TextKind::PersonName) (void)charset::split_person_name(u);
    }
    // The input as UTF-8 to encode into the parsed set.
    (void)charset::encode(text, cs, charset::TextKind::MultiValued);
    return 0;
}
