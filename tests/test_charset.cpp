// Character set tests: PS3.5 Annex H/I/J/K examples, table round trips, malformed input.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::charset;

namespace {

CharacterSet cs_of(std::string_view s) { return CharacterSet::parse(s); }

std::string dec(std::string_view bytes, std::string_view cs, TextKind kind = TextKind::PersonName,
                std::size_t* bad = nullptr) {
    return decode(bytes, cs_of(cs), kind, bad);
}

std::string enc(std::string_view utf8, std::string_view cs, TextKind kind = TextKind::PersonName) {
    EncodeResult r = encode(utf8, cs_of(cs), kind);
    CHECK(r.ok());
    return r.bytes;
}

// True when s is well-formed UTF-8 (strict).
bool valid_utf8(std::string_view s) {
    std::size_t bad = 99;
    const std::string again = decode(s, cs_of("ISO_IR 192"), TextKind::Text, &bad);
    return bad == 0 && again == s;
}

std::string utf8_of(char32_t cp) {
    std::string out;
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

const std::string kReplacementUtf8 = "\xEF\xBF\xBD";

std::size_t count_of(std::string_view hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string_view::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

// PS3.5 H.3.1: Specific Character Set "\ISO 2022 IR 87".
const std::string kH31 =
    "Yamada^Tarou=\x1b$B;3ED\x1b(B^\x1b$BB@O:\x1b(B=\x1b$B$d$^$@\x1b(B^\x1b$B$?$m$&\x1b(B";
// PS3.5 H.3.2: "ISO 2022 IR 13\ISO 2022 IR 87".
const std::string kH32 =
    "\xd4\xcf\xc0\xde^\xc0\xdb\xb3=\x1b$B;3ED\x1b(J^\x1b$BB@O:\x1b(J=\x1b$B$d$^$@\x1b(J^\x1b$B$?$m$&\x1b(J";
// PS3.5 I.2: "\ISO 2022 IR 149".
const std::string kI2 =
    "Hong^Gildong=\x1b$)C\xfb\xf3^\x1b$)C\xd1\xce\xd4\xd7=\x1b$)C\xc8\xab^\x1b$)C\xb1\xe6\xb5\xbf";
// PS3.5 J (GB18030) and K (ISO 2022 IR 58) person names.
const std::string kJ1 = "Wang^XiaoDong=\xcd\xf5^\xd0\xa1\xb6\xab=";
const std::string kK1 = "Zhang^XiaoDong=\x1b$)A\xd5\xc5^\x1b$)A\xd0\xa1\xb6\xab=";

}  // namespace

TEST_CASE("charset: parse Specific Character Set") {
    CharacterSet a = cs_of("");
    CHECK(a.scheme == Scheme::Iso2022);
    CHECK(a.terms.size() == 1 && a.terms[0] == Term::IR6);
    CHECK(!a.extensions);
    CharacterSet b = cs_of("\\ISO 2022 IR 87 ");
    CHECK(b.extensions);
    CHECK(b.terms.size() == 2 && b.terms[0] == Term::IR6 && b.terms[1] == Term::IR87);
    CHECK_EQ(b.to_string(), "\\ISO 2022 IR 87");
    CharacterSet c = cs_of("ISO 2022 IR 13\\ISO 2022 IR 87");
    CHECK(c.terms.size() == 2 && c.terms[0] == Term::IR13);
    CHECK_EQ(c.to_string(), "ISO 2022 IR 13\\ISO 2022 IR 87");
    CHECK(cs_of("ISO_IR 192").scheme == Scheme::Utf8);
    CHECK(cs_of("GB18030").scheme == Scheme::Gb18030);
    CHECK(cs_of("GBK").scheme == Scheme::Gbk);
    CHECK(!cs_of("ISO_IR 100").extensions);
    CHECK(cs_of("ISO 2022 IR 100").extensions);
    CharacterSet d = cs_of("ISO-IR 100");  // common misspelling
    CHECK(d.terms[0] == Term::IR100 && d.lenient == 1);
    CharacterSet e = cs_of("KLINGON");
    CHECK(e.terms[0] == Term::IR6 && e.unknown == 1);
    CHECK_EQ(cs_of("ISO_IR 144").to_string(), "ISO_IR 144");
}

TEST_CASE("charset: PS3.5 H.3.1 Japanese ISO 2022 IR 87") {
    std::size_t bad = 9;
    CHECK_EQ(dec(kH31, "\\ISO 2022 IR 87", TextKind::PersonName, &bad),
             "Yamada^Tarou=山田^太郎=やまだ^たろう");
    CHECK_EQ(bad, 0u);
    CHECK_EQ(enc("Yamada^Tarou=山田^太郎=やまだ^たろう", "\\ISO 2022 IR 87"), kH31);
    PersonName pn = split_person_name("Yamada^Tarou=山田^太郎=やまだ^たろう");
    CHECK_EQ(pn.groups[1], "山田^太郎");
    CHECK_EQ(pn.components[2][0], "やまだ");
    CHECK_EQ(pn.components[2][1], "たろう");
    CHECK_EQ(pn.components[0][1], "Tarou");
}

TEST_CASE("charset: PS3.5 H.3.2 Japanese ISO 2022 IR 13 + IR 87") {
    std::size_t bad = 9;
    CHECK_EQ(dec(kH32, "ISO 2022 IR 13\\ISO 2022 IR 87", TextKind::PersonName, &bad),
             "ﾔﾏﾀﾞ^ﾀﾛｳ=山田^太郎=やまだ^たろう");
    CHECK_EQ(bad, 0u);
    CHECK_EQ(enc("ﾔﾏﾀﾞ^ﾀﾛｳ=山田^太郎=やまだ^たろう", "ISO 2022 IR 13\\ISO 2022 IR 87"), kH32);
    // Single-valued ISO_IR 13: katakana in GR, JIS X 0201 Romaji in GL.
    CHECK_EQ(dec("\xd4\xcf\xc0\xde^\xc0\xdb\xb3", "ISO_IR 13"), "ﾔﾏﾀﾞ^ﾀﾛｳ");
    // Romaji 0x5C is YEN SIGN and 0x7E OVERLINE inside text, a delimiter in multi-valued VRs.
    CHECK_EQ(dec("1\\2~", "ISO_IR 13", TextKind::Text), "1¥2‾");
    CHECK_EQ(dec("1\\2", "ISO_IR 13", TextKind::MultiValued), "1\\2");
    CHECK_EQ(enc("¥‾", "ISO_IR 13", TextKind::Text), "\\~");
}

TEST_CASE("charset: PS3.5 I Korean ISO 2022 IR 149") {
    std::size_t bad = 9;
    CHECK_EQ(dec(kI2, "\\ISO 2022 IR 149", TextKind::PersonName, &bad), "Hong^Gildong=洪^吉洞=홍^길동");
    CHECK_EQ(bad, 0u);
    CHECK_EQ(enc("Hong^Gildong=洪^吉洞=홍^길동", "\\ISO 2022 IR 149"), kI2);
    // Multi-line text: G1 returns to value 1 (none) at each CR LF, so each line re-designates.
    const std::string lt = "\x1b$)C\xc7\xd1\xb1\xdb\r\n\x1b$)C\xc7\xd1\xb1\xdb abc";
    CHECK_EQ(dec(lt, "\\ISO 2022 IR 149", TextKind::Text), "한글\r\n한글 abc");
    CHECK_EQ(enc("한글\r\n한글 abc", "\\ISO 2022 IR 149", TextKind::Text), lt);
    // Without the second designation the GR bytes after CR LF are undecodable.
    bad = 0;
    (void)dec("\x1b$)C\xc7\xd1\r\n\xc7\xd1", "\\ISO 2022 IR 149", TextKind::Text, &bad);
    CHECK_EQ(bad, 2u);
}

TEST_CASE("charset: PS3.5 J Chinese GB18030") {
    std::size_t bad = 9;
    CHECK_EQ(dec(kJ1, "GB18030", TextKind::PersonName, &bad), "Wang^XiaoDong=王^小东=");
    CHECK_EQ(bad, 0u);
    CHECK_EQ(enc("Wang^XiaoDong=王^小东=", "GB18030"), kJ1);
    const std::string lt = "The first line includes\xd6\xd0\xce\xc4.\r\nThe second line includes\xd6\xd0\xce\xc4, too.\r\nThe third line.";
    const std::string u = "The first line includes中文.\r\nThe second line includes中文, too.\r\nThe third line.";
    CHECK_EQ(dec(lt, "GB18030", TextKind::Text), u);
    CHECK_EQ(enc(u, "GB18030", TextKind::Text), lt);
    // Four-byte sequences (WHATWG index-gb18030-ranges).
    CHECK_EQ(dec("\x81\x30\x81\x30", "GB18030"), "\xc2\x80");
    CHECK_EQ(enc("\xf0\x90\x80\x80", "GB18030"), "\x90\x30\x81\x30");          // U+10000
    CHECK_EQ(dec(enc("😀", "GB18030"), "GB18030"), "😀");
    CHECK_EQ(dec(enc("\xee\x9f\x87", "GB18030"), "GB18030"), "\xee\x9f\x87");  // U+E7C7 (pointer 7457)
    CHECK_EQ(enc("\xee\x9f\x87", "GB18030"), "\x81\x35\xf4\x37");
    // A trail byte 0x5C belongs to the character, not a value delimiter.
    const std::string two = dec("\x81\x5c\\x", "GB18030", TextKind::MultiValued);
    CHECK_EQ(count_of(two, "\\"), 1u);
    CHECK(two.size() > 3 && two.back() == 'x');
}

TEST_CASE("charset: GBK") {
    CHECK_EQ(dec(kJ1, "GBK"), "Wang^XiaoDong=王^小东=");
    CHECK_EQ(enc("€王", "GBK"), "\x80\xcd\xf5");
    EncodeResult r = encode("😀", cs_of("GBK"), TextKind::Text);  // needs a four-byte code
    CHECK(!r.ok());
    CHECK(r.failures.size() == 1 && r.failures[0].code_point == 0x1F600);
}

TEST_CASE("charset: PS3.5 K Chinese ISO 2022 IR 58") {
    std::size_t bad = 9;
    CHECK_EQ(dec(kK1, "\\ISO 2022 IR 58", TextKind::PersonName, &bad), "Zhang^XiaoDong=张^小东=");
    CHECK_EQ(bad, 0u);
    CHECK_EQ(enc("Zhang^XiaoDong=张^小东=", "\\ISO 2022 IR 58"), kK1);
    const std::string lt = "1. \x1b$)A\xb5\xda\xd2\xbb\xd0\xd0\xce\xc4\xd7\xd6\xa1\xa3\r\n2. \x1b$)A\xb5\xda\xb6\xfe\xd0\xd0\xce\xc4\xd7\xd6\xa1\xa3\r\n";
    const std::string u = "1. 第一行文字。\r\n2. 第二行文字。\r\n";
    CHECK_EQ(dec(lt, "\\ISO 2022 IR 58", TextKind::Text), u);
    CHECK_EQ(enc(u, "\\ISO 2022 IR 58", TextKind::Text), lt);
}

TEST_CASE("charset: JIS X 0212 (ISO 2022 IR 159)") {
    const std::string b = "\x1b$(D\x30\x21\x1b(B";
    CHECK_EQ(dec(b, "\\ISO 2022 IR 87\\ISO 2022 IR 159"), "丂");
    CHECK_EQ(enc("丂", "\\ISO 2022 IR 87\\ISO 2022 IR 159"), b);
    // The value 1 G0 set is restored before SPACE.
    CHECK_EQ(enc("山 a", "\\ISO 2022 IR 87", TextKind::Text), "\x1b$B;3\x1b(B a");
    // A character in both sets is taken from the first listed set that has it.
    CHECK_EQ(enc("山", "\\ISO 2022 IR 87\\ISO 2022 IR 159"), "\x1b$B;3\x1b(B");
    // Double-byte G0: '^' (0x5E) and '\' (0x5C) bytes are halves of characters, not delimiters.
    std::size_t bad = 9;
    const std::string s = dec("\x1b$B\x30\x5c$^\x1b(B\\x", "\\ISO 2022 IR 87", TextKind::PersonName, &bad);
    CHECK_EQ(bad, 0u);
    CHECK_EQ(count_of(s, "\\"), 1u);
    CHECK_EQ(count_of(s, "^"), 0u);
}

TEST_CASE("charset: single-byte sets, full high half round trip") {
    struct Set { const char* name; std::size_t defined; std::uint8_t byte; char32_t cp; };
    const Set sets[] = {
        {"ISO_IR 100", 96, 0xE9, 0x00E9}, {"ISO_IR 101", 96, 0xA3, 0x0141}, {"ISO_IR 109", 89, 0xA1, 0x0126},
        {"ISO_IR 110", 96, 0xA1, 0x0104}, {"ISO_IR 144", 96, 0xD0, 0x0430}, {"ISO_IR 127", 51, 0xC7, 0x0627},
        {"ISO_IR 126", 93, 0xC1, 0x0391}, {"ISO_IR 138", 60, 0xE0, 0x05D0}, {"ISO_IR 148", 96, 0xD0, 0x011E},
        {"ISO_IR 203", 96, 0xA4, 0x20AC}, {"ISO_IR 166", 88, 0xA1, 0x0E01},
    };
    for (const Set& st : sets) {
        for (int form = 0; form < 2; ++form) {
            std::string name = st.name;
            if (form == 1) name = "ISO 2022 IR " + name.substr(7);
            const CharacterSet cs = cs_of(name);
            std::size_t defined = 0;
            for (unsigned b = 0xA0; b <= 0xFF; ++b) {
                const std::string in(1, static_cast<char>(b));
                std::size_t bad = 0;
                const std::string u = decode(in, cs, TextKind::Text, &bad);
                CHECK(valid_utf8(u));
                if (bad) { CHECK_EQ(u, kReplacementUtf8); continue; }
                ++defined;
                EncodeResult r = encode(u, cs, TextKind::Text);
                CHECK(r.ok());
                CHECK_EQ(r.bytes, in);
            }
            if (defined != st.defined) std::fprintf(stderr, "  %s: %zu defined\n", name.c_str(), defined);
            CHECK_EQ(defined, st.defined);
            CHECK_EQ(decode(std::string(1, static_cast<char>(st.byte)), cs, TextKind::Text), utf8_of(st.cp));
            if (decode(std::string(1, static_cast<char>(st.byte)), cs, TextKind::Text) != utf8_of(st.cp))
                std::fprintf(stderr, "  spot check failed for %s\n", name.c_str());
            // ASCII is unchanged; C1 bytes decode to C1 controls.
            CHECK_EQ(decode("AZaz09^=", cs, TextKind::Text), "AZaz09^=");
        }
    }
    // Default repertoire: high bytes are not defined.
    std::size_t bad = 0;
    CHECK_EQ(dec("A\xe9", "", TextKind::Text, &bad), "A" + kReplacementUtf8);
    CHECK_EQ(bad, 1u);
}

TEST_CASE("charset: double-byte sets, every cell round trips") {
    struct Set { const char* cs; const char* esc; bool g1; std::size_t expected; };
    const Set sets[] = {
        {"\\ISO 2022 IR 87", "\x1b$B", false, 6879},
        {"\\ISO 2022 IR 159", "\x1b$(D", false, 6067},
        {"\\ISO 2022 IR 149", "\x1b$)C", true, 8224},
        {"\\ISO 2022 IR 58", "\x1b$)A", true, 7445},
    };
    for (const Set& st : sets) {
        const CharacterSet cs = cs_of(st.cs);
        std::size_t defined = 0;
        for (unsigned r = 0; r < 94; ++r) {
            for (unsigned c = 0; c < 94; ++c) {
                const unsigned hi = st.g1 ? 0x80u : 0u;
                std::string in = st.esc;
                in.push_back(static_cast<char>((0x21 + r) | hi));
                in.push_back(static_cast<char>((0x21 + c) | hi));
                if (!st.g1) in += "\x1b(B";
                std::size_t bad = 0;
                const std::string u = decode(in, cs, TextKind::Text, &bad);
                if (bad) { CHECK_EQ(u, kReplacementUtf8); continue; }
                ++defined;
                CHECK(static_cast<unsigned char>(u[0]) >= 0x80);  // never ASCII
                EncodeResult e = encode(u, cs, TextKind::Text);
                CHECK(e.ok());
                CHECK_EQ(e.bytes, in);
            }
        }
        CHECK_EQ(defined, st.expected);
    }
}

TEST_CASE("charset: GB18030 two-byte index round trip") {
    const CharacterSet cs = cs_of("GB18030");
    std::size_t exact = 0, total = 0;
    for (unsigned lead = 0x81; lead <= 0xFE; ++lead) {
        for (unsigned trail = 0x40; trail <= 0xFE; ++trail) {
            if (trail == 0x7F) continue;
            std::string in;
            in.push_back(static_cast<char>(lead));
            in.push_back(static_cast<char>(trail));
            std::size_t bad = 0;
            const std::string u = decode(in, cs, TextKind::Text, &bad);
            CHECK_EQ(bad, 0u);
            ++total;
            EncodeResult e = encode(u, cs, TextKind::Text);
            if (e.ok() && e.bytes == in) { ++exact; continue; }
            // Not canonical: the encoding must still decode to the same character.
            if (e.ok()) CHECK_EQ(decode(e.bytes, cs, TextKind::Text), u);
        }
    }
    CHECK_EQ(total, 23940u);
    std::fprintf(stderr, "  GB18030 two-byte: %zu of %zu round trip exactly\n", exact, total);
    CHECK(exact + 40 >= total);
}

TEST_CASE("charset: malformed input yields U+FFFD and never over-reads") {
    std::size_t bad = 0;
    CHECK_EQ(dec("A\x1b", "\\ISO 2022 IR 87", TextKind::Text, &bad), "A" + kReplacementUtf8);
    CHECK_EQ(bad, 1u);
    CHECK_EQ(dec("\x1b$", "\\ISO 2022 IR 87", TextKind::Text, &bad), kReplacementUtf8);
    CHECK_EQ(dec("\x1b$)Zx", "\\ISO 2022 IR 87", TextKind::Text, &bad), kReplacementUtf8 + "x");
    CHECK_EQ(dec("\x1b\x01x", "\\ISO 2022 IR 87", TextKind::Text, &bad), kReplacementUtf8 + "\x01x");
    CHECK_EQ(dec("\x1b(((((((((B", "\\ISO 2022 IR 87", TextKind::Text, &bad).substr(0, 3), kReplacementUtf8);
    CHECK_EQ(dec("\x1b$B;", "\\ISO 2022 IR 87", TextKind::Text, &bad), kReplacementUtf8);       // truncated
    CHECK_EQ(dec("\x1b$B;\r", "\\ISO 2022 IR 87", TextKind::Text, &bad), kReplacementUtf8 + "\r");
    CHECK_EQ(dec("\x1b$)C\xb0", "\\ISO 2022 IR 149", TextKind::Text, &bad), kReplacementUtf8);
    CHECK_EQ(dec("\x1b$)C\xb0" "A", "\\ISO 2022 IR 149", TextKind::Text, &bad), kReplacementUtf8 + "A");
    CHECK_EQ(dec("\x1b)Bx\xc1", "\\ISO 2022 IR 87", TextKind::Text, &bad), kReplacementUtf8 + "x" + kReplacementUtf8);
    CHECK_EQ(dec("\xc3", "ISO_IR 192", TextKind::Text, &bad), kReplacementUtf8);
    CHECK_EQ(dec("\xe0\x80\x80", "ISO_IR 192", TextKind::Text, &bad), kReplacementUtf8 + kReplacementUtf8 + kReplacementUtf8);
    CHECK_EQ(bad, 3u);
    CHECK_EQ(dec("\xed\xa0\x80", "ISO_IR 192", TextKind::Text, &bad).size(), 9u);  // surrogate
    CHECK_EQ(dec("\x81", "GB18030", TextKind::Text, &bad), kReplacementUtf8);
    CHECK_EQ(dec("\x81\x30", "GB18030", TextKind::Text, &bad), kReplacementUtf8 + "0");
    CHECK_EQ(dec("\x81\x30\x81", "GB18030", TextKind::Text, &bad), kReplacementUtf8 + "0" + kReplacementUtf8);
    CHECK_EQ(dec("\x84\x31\xa5\x30", "GB18030", TextKind::Text, &bad), kReplacementUtf8);  // pointer > 39419
    CHECK_EQ(dec("\x81 \\", "GB18030", TextKind::MultiValued, &bad), kReplacementUtf8 + " \\");
    CHECK_EQ(dec("\xff", "GB18030", TextKind::Text, &bad), kReplacementUtf8);

    // Deterministic random bytes in every scheme: output is always valid UTF-8.
    const char* sets[] = {"", "ISO_IR 100", "ISO 2022 IR 13\\ISO 2022 IR 87\\ISO 2022 IR 159",
                          "\\ISO 2022 IR 149", "\\ISO 2022 IR 58", "ISO_IR 192", "GB18030", "GBK",
                          "ISO 2022 IR 166\\ISO 2022 IR 100", "ISO_IR 13"};
    std::uint32_t seed = 12345;
    for (const char* s : sets) {
        const CharacterSet cs = cs_of(s);
        for (int iter = 0; iter < 400; ++iter) {
            std::string in;
            const std::size_t len = (seed >> 7) % 48;
            for (std::size_t k = 0; k < len; ++k) {
                seed = seed * 1103515245u + 12345u;
                const auto v = static_cast<std::uint8_t>(seed >> 16);
                in.push_back(static_cast<char>((seed & 0x300u) == 0 ? 0x1B : v));
            }
            seed = seed * 1103515245u + 12345u;
            for (TextKind k : {TextKind::Text, TextKind::MultiValued, TextKind::PersonName}) {
                const std::string out = decode(in, cs, k);
                CHECK(valid_utf8(out));
                EncodeResult e = encode(out, cs, k);  // must not crash; U+FFFD may be unrepresentable
                (void)e;
            }
        }
    }
}

TEST_CASE("charset: multi-valued character sets") {
    const char* cs = "ISO 2022 IR 100\\ISO 2022 IR 144\\ISO 2022 IR 126";
    const std::string u = "Jérôme Люк Διον";
    const std::string b = enc(u, cs, TextKind::Text);
    CHECK_EQ(b, "J\xe9r\xf4me \x1b-L\xbb\xee\xda \x1b-F\xc4\xe9\xef\xed\x1b-A");
    CHECK_EQ(dec(b, cs, TextKind::Text), u);
    // Restored to value 1 (Latin-1 in G1) at the delimiter.
    CHECK_EQ(enc("Люк^é", cs), "\x1b-L\xbb\xee\xda\x1b-A^\xe9");
    CHECK_EQ(dec("\x1b-L\xbb^\xe9", cs), "Л^é");
    // Characters outside every listed set are reported.
    EncodeResult r = encode("Jé山x", cs_of(cs), TextKind::Text);
    CHECK(!r.ok());
    CHECK(r.failures.size() == 1 && r.failures[0].code_point == 0x5C71 && r.failures[0].offset == 3);
    CHECK_EQ(r.bytes, "J\xe9?x");
    // Without code extensions only value 1 is usable.
    CHECK(!encode("Люк", cs_of("ISO_IR 100"), TextKind::Text).ok());
    // Thai through ISO 2022 IR 166.
    CHECK_EQ(enc("ก", "ISO 2022 IR 166"), "\xa1");
    CHECK_EQ(enc("กé", "ISO 2022 IR 166\\ISO 2022 IR 100"), "\xa1\x1b-A\xe9\x1b-T");
    // UTF-8 ignores the other values.
    CHECK(cs_of("ISO_IR 192\\ISO 2022 IR 87").scheme == Scheme::Utf8);
}

TEST_CASE("charset: get_utf8 / set_utf8 on a data set") {
    Dataset ds;
    (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "\\ISO 2022 IR 87");
    (void)ds.set_bytes(tags::PatientName, VR::PN, Bytes::from_string(kH31));
    CHECK_EQ(get_utf8(ds, tags::PatientName).value_or(""), "Yamada^Tarou=山田^太郎=やまだ^たろう");
    // Two PN values; the first contains a JIS X 0208 character whose second byte is 0x5C.
    (void)ds.set_bytes(0x00101001, VR::PN, Bytes::from_string("\x1b$B\x30\x5c\x1b(B\\B^C "));
    auto v0 = get_utf8(ds, 0x00101001, 0);
    auto v1 = get_utf8(ds, 0x00101001, 1);
    REQUIRE(v0 && v1);
    CHECK(v0->size() == 3 && static_cast<unsigned char>((*v0)[0]) >= 0xE0);
    CHECK_EQ(*v1, "B^C");
    CHECK(!get_utf8(ds, 0x00101001, 2));
    CHECK(!get_utf8(ds, 0x00100020));
    // set_utf8 encodes in the data set's character set.
    CHECK(set_utf8(ds, 0x00080090, VR::PN, "やまだ^たろう").is_ok());
    CHECK_EQ(ds.get_raw_string(0x00080090).value_or(""), "\x1b$B$d$^$@\x1b(B^\x1b$B$?$m$&\x1b(B ");
    CHECK_EQ(get_utf8(ds, 0x00080090).value_or(""), "やまだ^たろう");
    std::vector<Unrepresentable> fails;
    Status s = set_utf8(ds, 0x00080090, VR::PN, "홍길동", nullptr, &fails);
    CHECK(s.code == Errc::Unsupported);
    CHECK_EQ(fails.size(), 3u);
    CHECK(s.message.find("3 character(s)") != std::string::npos);
    CHECK(s.message.find("\\ISO 2022 IR 87") != std::string::npos);
    CHECK_EQ(get_utf8(ds, 0x00080090).value_or(""), "やまだ^たろう");  // unchanged
    // Default-repertoire VRs ignore (0008,0005).
    (void)ds.set_string(tags::Modality, VR::CS, "OT");
    CHECK_EQ(get_utf8(ds, tags::Modality).value_or(""), "OT");
    CHECK(set_utf8(ds, tags::Modality, VR::CS, "山").code == Errc::Unsupported);
    CHECK(set_utf8(ds, tags::Rows, VR::US, "1").code == Errc::TypeMismatch);
    // Text VRs: backslash is a character, one value.
    (void)ds.set_bytes(0x00104000, VR::LT, Bytes::from_string("a\\b\r\n\x1b$B;3\x1b(B "));
    CHECK_EQ(get_utf8(ds, 0x00104000).value_or(""), "a\\b\r\n山");
    CHECK(!get_utf8(ds, 0x00104000, 1));

    // Sequence items inherit the enclosing character set unless they have their own.
    Dataset item;
    (void)item.set_bytes(tags::PatientName, VR::PN, Bytes::from_string(kH31));
    const CharacterSet parent = character_set_of(ds);
    CHECK_EQ(get_utf8(item, tags::PatientName, 0, &parent).value_or(""), "Yamada^Tarou=山田^太郎=やまだ^たろう");
    (void)item.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    (void)item.set_bytes(tags::PatientName, VR::PN, Bytes::from_string("山田"));
    CHECK_EQ(get_utf8(item, tags::PatientName, 0, &parent).value_or(""), "山田");
    CHECK(table_bytes() > 100000);
}

TEST_CASE("charset: C API get/set UTF-8") {
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_create(&f) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x00080005, "CS", "ISO_IR 100", 10) == SIAARA_OK);
    const std::string name = "Buc^Jérôme";
    CHECK(siaara_dataset_set_utf8(ds, 0x00100010, "PN", name.data(), name.size()) == SIAARA_OK);
    const std::uint8_t* raw = nullptr;
    std::size_t raw_size = 0;
    CHECK(siaara_dataset_get_raw(ds, 0x00100010, &raw, &raw_size) == SIAARA_OK);
    CHECK(raw_size == 10 && std::memcmp(raw, "Buc^J\xe9r\xf4me", 10) == 0);
    char buf[64];
    std::size_t needed = 0;
    CHECK(siaara_dataset_get_utf8(ds, 0x00100010, 0, buf, sizeof buf, &needed) == SIAARA_OK);
    CHECK_EQ(std::string(buf), name);
    CHECK_EQ(needed, name.size());
    CHECK(siaara_dataset_get_utf8(ds, 0x00100010, 0, buf, 4, &needed) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(siaara_dataset_get_utf8(ds, 0x00100020, 0, buf, sizeof buf, &needed) == SIAARA_ERR_NOT_FOUND);
    const std::string kanji = "山田";
    CHECK(siaara_dataset_set_utf8(ds, 0x00100010, "PN", kanji.data(), kanji.size()) == SIAARA_ERR_UNSUPPORTED);
    CHECK(std::string(siaara_last_error_message()).find("cannot be represented") != std::string::npos);
    CHECK(siaara_dataset_set_utf8(ds, 0x00100010, "XX", kanji.data(), kanji.size()) == SIAARA_ERR_INVALID_ARGUMENT);
    // Explicit inherited character set (for sequence items without (0008,0005)).
    CHECK(siaara_dataset_remove(ds, 0x00080005) == SIAARA_OK);
    const char* cs = "\\ISO 2022 IR 87";
    CHECK(siaara_dataset_set_utf8_cs(ds, 0x00100010, "PN", kanji.data(), kanji.size(), cs, std::strlen(cs)) == SIAARA_OK);
    CHECK(siaara_dataset_get_utf8_cs(ds, 0x00100010, 0, cs, std::strlen(cs), buf, sizeof buf, &needed) == SIAARA_OK);
    CHECK_EQ(std::string(buf), kanji);
    CHECK(siaara_dataset_get_utf8(nullptr, 0x00100010, 0, buf, sizeof buf, &needed) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file_free(f);
}
