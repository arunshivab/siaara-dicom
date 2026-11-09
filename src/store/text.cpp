#include "store/text.hpp"

#include <cstddef>
#include <cstring>

namespace siaara::store {

namespace {

// One ASCII base letter per code point; '1' = "ij", '2' = "oe", '3' = "ae", '4' = "th",
// '5' = "ss", '.' = keep the code point unchanged.
// U+00C0..U+00FF
constexpr char kLatin1[65] =
    "aaaaaa3ceeeeiiii"   // C0-CF
    "dnooooo.ouuuuy45"   // D0-DF (D7 multiplication sign kept)
    "aaaaaa3ceeeeiiii"   // E0-EF
    "dnooooo.ouuuuy4y";  // F0-FF (F7 division sign kept)
// U+0100..U+017F
constexpr char kLatinA[129] =
    "aaaaaaccccccccdd"
    "ddeeeeeeeeeegggg"
    "gggghhhhiiiiiiii"
    "ii11jjkkklllllll"
    "lllnnnnnnnnnoooo"
    "oo22rrrrrrssssss"
    "ssttttttuuuuuuuu"
    "uuuuwwyyyzzzzzzs";
// U+1E00..U+1EFF (Latin Extended Additional)
constexpr char kLatinAdd[257] =
    "aabbbbbbccdddddd"   // 1E00-1E0F
    "ddddeeeeeeeeeeff"   // 1E10-1E1F
    "gghhhhhhhhhhiiii"   // 1E20-1E2F
    "kkkkkkllllllllmm"   // 1E30-1E3F
    "mmmmnnnnnnnnoooo"   // 1E40-1E4F
    "oooopppprrrrrrrr"   // 1E50-1E5F
    "sssssssssstttttt"   // 1E60-1E6F
    "ttuuuuuuuuuuvvvv"   // 1E70-1E7F
    "wwwwwwwwwwxxxxyy"   // 1E80-1E8F
    "zzzzzzhtwyas..5."   // 1E90-1E9F
    "aaaaaaaaaaaaaaaa"   // 1EA0-1EAF
    "aaaaaaaaeeeeeeee"   // 1EB0-1EBF
    "eeeeeeeeiiiioooo"   // 1EC0-1ECF
    "oooooooooooooooo"   // 1ED0-1EDF
    "oooouuuuuuuuuuuu"   // 1EE0-1EEF
    "uuyyyyyyyy......";  // 1EF0-1EFF

const char* expand(char c) {
    switch (c) {
        case '1': return "ij";
        case '2': return "oe";
        case '3': return "ae";
        case '4': return "th";
        case '5': return "ss";
        default: return nullptr;
    }
}

// Sparse Latin Extended-B letters in common use (Romanian, Vietnamese, pinyin, African).
char latin_b(char32_t c) {
    switch (c) {
        case 0x0180: case 0x0181: case 0x0182: case 0x0183: return 'b';
        case 0x0187: case 0x0188: return 'c';
        case 0x0189: case 0x018A: case 0x018B: case 0x018C: return 'd';
        case 0x0190: case 0x0191: case 0x0192: return 'f';
        case 0x0193: return 'g';
        case 0x0197: case 0x0196: return 'i';
        case 0x0198: case 0x0199: return 'k';
        case 0x019A: return 'l';
        case 0x019D: case 0x019E: return 'n';
        case 0x019F: case 0x01A0: case 0x01A1: return 'o';
        case 0x01A4: case 0x01A5: return 'p';
        case 0x01AB: case 0x01AC: case 0x01AD: case 0x01AE: return 't';
        case 0x01AF: case 0x01B0: return 'u';
        case 0x01B2: return 'v';
        case 0x01B3: case 0x01B4: return 'y';
        case 0x01B5: case 0x01B6: return 'z';
        case 0x01CD: case 0x01CE: return 'a';
        case 0x01CF: case 0x01D0: return 'i';
        case 0x01D1: case 0x01D2: return 'o';
        case 0x01D3: case 0x01D4: case 0x01D5: case 0x01D6: case 0x01D7: case 0x01D8:
        case 0x01D9: case 0x01DA: case 0x01DB: case 0x01DC: return 'u';
        case 0x01DE: case 0x01DF: case 0x01E0: case 0x01E1: return 'a';
        case 0x01E4: case 0x01E5: case 0x01E6: case 0x01E7: return 'g';
        case 0x01E8: case 0x01E9: return 'k';
        case 0x01EA: case 0x01EB: case 0x01EC: case 0x01ED: return 'o';
        case 0x01F0: return 'j';
        case 0x01F4: case 0x01F5: return 'g';
        case 0x01F8: case 0x01F9: return 'n';
        case 0x01FA: case 0x01FB: return 'a';
        case 0x01FE: case 0x01FF: return 'o';
        case 0x0200: case 0x0201: case 0x0202: case 0x0203: return 'a';
        case 0x0204: case 0x0205: case 0x0206: case 0x0207: return 'e';
        case 0x0208: case 0x0209: case 0x020A: case 0x020B: return 'i';
        case 0x020C: case 0x020D: case 0x020E: case 0x020F: return 'o';
        case 0x0210: case 0x0211: case 0x0212: case 0x0213: return 'r';
        case 0x0214: case 0x0215: case 0x0216: case 0x0217: return 'u';
        case 0x0218: case 0x0219: return 's';
        case 0x021A: case 0x021B: return 't';
        case 0x021E: case 0x021F: return 'h';
        case 0x0224: case 0x0225: return 'z';
        case 0x0226: case 0x0227: return 'a';
        case 0x0228: case 0x0229: return 'e';
        case 0x022A: case 0x022B: case 0x022C: case 0x022D: case 0x022E: case 0x022F:
        case 0x0230: case 0x0231: return 'o';
        case 0x0232: case 0x0233: return 'y';
        default: return 0;
    }
}

void put_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
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
}

// Decodes one code point at s[i]; advances i. Malformed input yields U+FFFD and advances 1 byte.
char32_t next_cp(std::string_view s, std::size_t& i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    std::size_t len = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1Fu; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0Fu; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07u; min = 0x10000; }
    else { ++i; return 0xFFFD; }
    if (s.size() - i < len) { ++i; return 0xFFFD; }
    for (std::size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return 0xFFFD; }
    i += len;
    return cp;
}

// Appends the folded form of one code point.
void fold_cp(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        return;
    }
    char base = 0;
    if (c >= 0xC0 && c <= 0xFF) base = kLatin1[c - 0xC0];
    else if (c >= 0x100 && c <= 0x17F) base = kLatinA[c - 0x100];
    else if (c >= 0x180 && c <= 0x24F) base = latin_b(c);
    else if (c >= 0x1E00 && c <= 0x1EFF) base = kLatinAdd[c - 0x1E00];
    if (base && base != '.') {
        if (const char* e = expand(base)) out += e;
        else out.push_back(base);
        return;
    }
    if (c >= 0x300 && c <= 0x36F) return;  // combining diacritical marks
    // Greek
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) c += 0x20;
    switch (c) {
        case 0x386: case 0x3AC: c = 0x3B1; break;
        case 0x388: case 0x3AD: c = 0x3B5; break;
        case 0x389: case 0x3AE: c = 0x3B7; break;
        case 0x38A: case 0x3AF: case 0x3CA: case 0x390: c = 0x3B9; break;
        case 0x38C: case 0x3CC: c = 0x3BF; break;
        case 0x38E: case 0x3CD: case 0x3CB: case 0x3B0: c = 0x3C5; break;
        case 0x38F: case 0x3CE: c = 0x3C9; break;
        case 0x3C2: c = 0x3C3; break;
        case 0x3AA: c = 0x3B9; break;
        case 0x3AB: c = 0x3C5; break;
        default: break;
    }
    // Cyrillic
    if (c >= 0x410 && c <= 0x42F) c += 0x20;
    else if (c >= 0x400 && c <= 0x40F) c += 0x50;
    else if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) c |= 1u;
    else if (c >= 0x4C1 && c <= 0x4CE && (c & 1u)) c += 1;
    if (c == 0x451) c = 0x435;  // yo -> ie
    // Full-width ASCII and the ideographic space
    if (c >= 0xFF01 && c <= 0xFF5E) {
        fold_cp(out, c - 0xFF01 + 0x21);
        return;
    }
    if (c == 0x3000 || c == 0xA0) {
        out.push_back(' ');
        return;
    }
    put_utf8(out, c);
}

std::string fold_impl(std::string_view s, bool caret_to_space) {
    std::string tmp;
    tmp.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const char32_t c = next_cp(s, i);
        if (caret_to_space && c == '^') tmp.push_back(' ');
        else fold_cp(tmp, c);
    }
    // collapse whitespace, trim
    std::string out;
    out.reserve(tmp.size());
    bool space = false;
    for (char ch : tmp) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(ch);
    }
    return out;
}

}  // namespace

std::string fold(std::string_view utf8) { return fold_impl(utf8, false); }

std::array<std::string, 3> name_forms(std::string_view pn) {
    std::array<std::string, 3> out;
    std::size_t g = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= pn.size() && g < 3; ++i) {
        if (i == pn.size() || pn[i] == '=') {
            out[g++] = fold_impl(pn.substr(start, i - start), true);
            start = i + 1;
        }
    }
    return out;
}

std::string fold_name_pattern(std::string_view utf8) { return fold_impl(utf8, true); }

bool has_wildcards(std::string_view p) noexcept {
    return p.find('*') != std::string_view::npos || p.find('?') != std::string_view::npos;
}

namespace {
std::size_t cp_len(std::string_view s, std::size_t i) noexcept {
    std::size_t j = i;
    (void)next_cp(s, j);
    return j - i;
}
}  // namespace

bool wildcard_match(std::string_view p, std::string_view t) noexcept {
    // Iterative matcher with single-star backtracking: O(|p| * |t|) worst case.
    std::size_t pi = 0, ti = 0;
    std::size_t star_p = std::string_view::npos, star_t = 0;
    while (ti < t.size()) {
        if (pi < p.size() && p[pi] == '*') {
            star_p = ++pi;
            star_t = ti;
            continue;
        }
        if (pi < p.size() && p[pi] == '?') {
            ++pi;
            ti += cp_len(t, ti);
            continue;
        }
        if (pi < p.size() && p[pi] == t[ti]) {
            ++pi;
            ++ti;
            continue;
        }
        if (star_p == std::string_view::npos) return false;
        pi = star_p;
        star_t += cp_len(t, star_t);
        ti = star_t;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::uint32_t date_key(std::string_view s) noexcept {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    char d[8];
    std::size_t n = 0;
    if (s.size() == 8) {
        for (char c : s) d[n++] = c;
    } else if (s.size() == 10 && s[4] == '.' && s[7] == '.') {
        for (std::size_t i = 0; i < 10; ++i)
            if (i != 4 && i != 7) d[n++] = s[i];
    } else {
        return 0;
    }
    std::uint32_t v = 0;
    for (char c : d) {
        if (c < '0' || c > '9') return 0;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return v;
}

bool parse_date_range(std::string_view r, std::uint32_t& lo, std::uint32_t& hi) noexcept {
    while (!r.empty() && r.back() == ' ') r.remove_suffix(1);
    while (!r.empty() && r.front() == ' ') r.remove_prefix(1);
    lo = 0;
    hi = 99999999u;
    if (r.empty()) return true;
    const std::size_t dash = r.find('-');
    if (dash == std::string_view::npos) {
        lo = hi = date_key(r);
        return lo != 0;
    }
    const auto a = r.substr(0, dash);
    const auto b = r.substr(dash + 1);
    if (a.empty() && b.empty()) return false;
    if (!a.empty() && (lo = date_key(a)) == 0) return false;
    if (!b.empty() && (hi = date_key(b)) == 0) return false;
    return lo <= hi;
}

}  // namespace siaara::store
