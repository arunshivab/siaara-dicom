// SIAARA DICOM Toolkit - character set conversion (see charset.hpp).
//
// The ISO 2022 model used here is the DICOM profile of PS3.5 6.1.2.5: 8-bit code, GL holds G0
// and GR holds G1 permanently (no locking or single shifts), sets are switched only by
// designation escape sequences, and at every delimiter the G0/G1 designations of value 1
// become active again.
#include "charset/charset.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace siaara::charset {

namespace {

#include "charset/charset_tables.inc"

constexpr char32_t kReplacement = 0xFFFD;
constexpr std::uint8_t ESC = 0x1B;

// ------------------------------------------------------------------ graphic sets
enum class GSet : std::uint8_t {
    None, Ascii, JisRoman, JisKatakana,
    Latin1, Latin2, Latin3, Latin4, Cyrillic, Arabic, Greek, Hebrew, Latin5, Latin9, Thai,
    Jis0208, Jis0212, Ksx1001, Gb2312,
};
enum class Shape : std::uint8_t { None, S94, S96, S94x94 };

struct GSetInfo {
    Shape shape;
    char final_byte;              // ISO 2022 final byte of the designation
    const std::uint16_t* table;   // S94: 94 cells; S96: 96 cells; S94x94: 8836 cells
    const std::uint16_t* rev;     // S94x94: cell indices sorted by code point
    std::size_t rev_len;
};

const GSetInfo& info(GSet g) noexcept {
    static const GSetInfo kInfo[] = {
        {Shape::None, 0, nullptr, nullptr, 0},
        {Shape::S94, 'B', nullptr, nullptr, 0},  // ASCII is computed, not tabled
        {Shape::S94, 'J', kJisRoman, nullptr, 0},
        {Shape::S94, 'I', kJisKatakana, nullptr, 0},
        {Shape::S96, 'A', k8859_1, nullptr, 0},
        {Shape::S96, 'B', k8859_2, nullptr, 0},
        {Shape::S96, 'C', k8859_3, nullptr, 0},
        {Shape::S96, 'D', k8859_4, nullptr, 0},
        {Shape::S96, 'L', k8859_5, nullptr, 0},
        {Shape::S96, 'G', k8859_6, nullptr, 0},
        {Shape::S96, 'F', k8859_7, nullptr, 0},
        {Shape::S96, 'H', k8859_8, nullptr, 0},
        {Shape::S96, 'M', k8859_9, nullptr, 0},
        {Shape::S96, 'b', k8859_15, nullptr, 0},
        {Shape::S96, 'T', k8859_11, nullptr, 0},
        {Shape::S94x94, 'B', kJIS0208, kJIS0208Rev, std::size(kJIS0208Rev)},
        {Shape::S94x94, 'D', kJIS0212, kJIS0212Rev, std::size(kJIS0212Rev)},
        {Shape::S94x94, 'C', kKSX1001, kKSX1001Rev, std::size(kKSX1001Rev)},
        {Shape::S94x94, 'A', kGB2312, kGB2312Rev, std::size(kGB2312Rev)},
    };
    return kInfo[static_cast<std::size_t>(g)];
}

// Code point of cell `index` (0-based within the set); 0 when unassigned.
char32_t cell(GSet g, std::size_t index) noexcept {
    if (g == GSet::Ascii) return index < 94 ? static_cast<char32_t>(0x21 + index) : 0;
    const GSetInfo& i = info(g);
    const std::size_t n = i.shape == Shape::S94 ? 94 : i.shape == Shape::S96 ? 96 : 8836;
    return (i.table && index < n) ? i.table[index] : 0;
}

// Reverse lookup: cell index of `cp` in `g`, or -1.
long find_cell(GSet g, char32_t cp) noexcept {
    const GSetInfo& i = info(g);
    switch (i.shape) {
        case Shape::None:
            return -1;
        case Shape::S94:
        case Shape::S96: {
            if (g == GSet::Ascii) return (cp >= 0x21 && cp <= 0x7E) ? static_cast<long>(cp - 0x21) : -1;
            const std::size_t n = i.shape == Shape::S94 ? 94 : 96;
            for (std::size_t k = 0; k < n; ++k)
                if (i.table[k] != 0 && i.table[k] == cp) return static_cast<long>(k);
            return -1;
        }
        case Shape::S94x94: {
            if (cp > 0xFFFF) return -1;
            const std::uint16_t* first = i.rev;
            const std::uint16_t* last = i.rev + i.rev_len;
            const std::uint16_t* it = std::lower_bound(first, last, cp, [&](std::uint16_t idx, char32_t v) {
                return static_cast<char32_t>(i.table[idx]) < v;
            });
            if (it != last && i.table[*it] == cp) return static_cast<long>(*it);
            return -1;
        }
    }
    return -1;
}

GSet set_94(char f) noexcept {
    switch (f) {
        case 'B': return GSet::Ascii;
        case 'J': return GSet::JisRoman;
        case 'I': return GSet::JisKatakana;
        default: return GSet::None;
    }
}
GSet set_96(char f) noexcept {
    switch (f) {
        case 'A': return GSet::Latin1;
        case 'B': return GSet::Latin2;
        case 'C': return GSet::Latin3;
        case 'D': return GSet::Latin4;
        case 'L': return GSet::Cyrillic;
        case 'G': return GSet::Arabic;
        case 'F': return GSet::Greek;
        case 'H': return GSet::Hebrew;
        case 'M': return GSet::Latin5;
        case 'b': return GSet::Latin9;
        case 'T': return GSet::Thai;
        default: return GSet::None;
    }
}
GSet set_94x94(char f) noexcept {
    switch (f) {
        case '@':  // JIS C 6226-1978, accepted as JIS X 0208
        case 'B': return GSet::Jis0208;
        case 'A': return GSet::Gb2312;
        case 'C': return GSet::Ksx1001;
        case 'D': return GSet::Jis0212;
        default: return GSet::None;
    }
}

// ------------------------------------------------------------------ defined terms
struct TermInfo {
    Term term;
    std::string_view ir_name;     // "ISO_IR nnn" form ("" when there is none)
    std::string_view iso2022_name;
    GSet g0, g1;
};
constexpr TermInfo kTerms[] = {
    {Term::IR6, "", "ISO 2022 IR 6", GSet::Ascii, GSet::None},
    {Term::IR100, "ISO_IR 100", "ISO 2022 IR 100", GSet::Ascii, GSet::Latin1},
    {Term::IR101, "ISO_IR 101", "ISO 2022 IR 101", GSet::Ascii, GSet::Latin2},
    {Term::IR109, "ISO_IR 109", "ISO 2022 IR 109", GSet::Ascii, GSet::Latin3},
    {Term::IR110, "ISO_IR 110", "ISO 2022 IR 110", GSet::Ascii, GSet::Latin4},
    {Term::IR144, "ISO_IR 144", "ISO 2022 IR 144", GSet::Ascii, GSet::Cyrillic},
    {Term::IR127, "ISO_IR 127", "ISO 2022 IR 127", GSet::Ascii, GSet::Arabic},
    {Term::IR126, "ISO_IR 126", "ISO 2022 IR 126", GSet::Ascii, GSet::Greek},
    {Term::IR138, "ISO_IR 138", "ISO 2022 IR 138", GSet::Ascii, GSet::Hebrew},
    {Term::IR148, "ISO_IR 148", "ISO 2022 IR 148", GSet::Ascii, GSet::Latin5},
    {Term::IR203, "ISO_IR 203", "ISO 2022 IR 203", GSet::Ascii, GSet::Latin9},
    {Term::IR13, "ISO_IR 13", "ISO 2022 IR 13", GSet::JisRoman, GSet::JisKatakana},
    {Term::IR166, "ISO_IR 166", "ISO 2022 IR 166", GSet::Ascii, GSet::Thai},
    {Term::IR87, "", "ISO 2022 IR 87", GSet::Jis0208, GSet::None},
    {Term::IR159, "", "ISO 2022 IR 159", GSet::Jis0212, GSet::None},
    {Term::IR149, "", "ISO 2022 IR 149", GSet::None, GSet::Ksx1001},
    {Term::IR58, "", "ISO 2022 IR 58", GSet::None, GSet::Gb2312},
    {Term::IR192, "ISO_IR 192", "", GSet::None, GSet::None},
    {Term::GB18030, "GB18030", "", GSet::None, GSet::None},
    {Term::GBK, "GBK", "", GSet::None, GSet::None},
};
const TermInfo& term_info(Term t) noexcept { return kTerms[static_cast<std::size_t>(t)]; }

// Upper-case letters and digits only: "ISO_IR 100", "ISO-IR100", "iso ir 100" all -> "ISOIR100".
std::string squash(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c >= 'a' && c <= 'z') out.push_back(static_cast<char>(c - 'a' + 'A'));
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out.push_back(c);
    }
    return out;
}

// Returns true and sets `t` when `v` names a term; `exact` tells whether the spelling was exact.
bool lookup_term(std::string_view v, Term& t, bool& iso2022, bool& exact) {
    for (const TermInfo& ti : kTerms) {
        if (!ti.ir_name.empty() && v == ti.ir_name) { t = ti.term; iso2022 = false; exact = true; return true; }
        if (!ti.iso2022_name.empty() && v == ti.iso2022_name) { t = ti.term; iso2022 = true; exact = true; return true; }
    }
    if (v == "ISO_IR 6") { t = Term::IR6; iso2022 = false; exact = true; return true; }  // common, harmless
    const std::string q = squash(v);
    if (q.empty()) return false;
    for (const TermInfo& ti : kTerms) {
        if (!ti.ir_name.empty() && q == squash(ti.ir_name)) { t = ti.term; iso2022 = false; exact = false; return true; }
        if (!ti.iso2022_name.empty() && q == squash(ti.iso2022_name)) { t = ti.term; iso2022 = true; exact = false; return true; }
    }
    if (q == "ISOIR6" || q == "UTF8") {
        t = q == "UTF8" ? Term::IR192 : Term::IR6;
        iso2022 = false; exact = false;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ UTF-8 helpers
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

// Reads one code point at s[i] (Unicode "maximal subpart" error handling, as the WHATWG UTF-8
// decoder). Advances i; returns kReplacement and sets bad on error.
char32_t next_utf8(std::string_view s, std::size_t& i, bool& bad) noexcept {
    bad = false;
    const auto b0 = static_cast<std::uint8_t>(s[i++]);
    if (b0 < 0x80) return b0;
    std::size_t need;
    char32_t cp;
    std::uint8_t lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; cp = b0 & 0x1Fu; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2; cp = b0 & 0x0Fu;
        if (b0 == 0xE0) lo = 0xA0;
        if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3; cp = b0 & 0x07u;
        if (b0 == 0xF0) lo = 0x90;
        if (b0 == 0xF4) hi = 0x8F;
    } else {
        bad = true;
        return kReplacement;
    }
    for (std::size_t k = 0; k < need; ++k) {
        if (i >= s.size()) { bad = true; return kReplacement; }
        const auto b = static_cast<std::uint8_t>(s[i]);
        if (b < lo || b > hi) { bad = true; return kReplacement; }  // b is not consumed
        lo = 0x80; hi = 0xBF;
        cp = (cp << 6) | (b & 0x3Fu);
        ++i;
    }
    return cp;
}

bool is_line_delimiter(std::uint8_t b) noexcept { return b == 0x0D || b == 0x0A || b == 0x0C || b == 0x09; }
bool is_value_delimiter(std::uint8_t b, TextKind kind) noexcept {
    if (kind == TextKind::Text) return false;
    if (b == '\\') return true;
    return kind == TextKind::PersonName && (b == '^' || b == '=');
}

// ------------------------------------------------------------------ ISO 2022 decoder
struct Initial {
    GSet g0 = GSet::Ascii;
    GSet g1 = GSet::None;
};

Initial initial_state(const CharacterSet& cs) noexcept {
    Initial st;
    const TermInfo& v1 = term_info(cs.terms.empty() ? Term::IR6 : cs.terms.front());
    // A multi-byte G0 set is never the initial G0 (delimiters must stay recognisable); a G1
    // multi-byte set in value 1 is not conformant but harmless, so it is honoured.
    if (v1.g0 == GSet::Ascii || v1.g0 == GSet::JisRoman) st.g0 = v1.g0;
    if (v1.g1 != GSet::None) st.g1 = v1.g1;
    return st;
}

std::string decode_iso2022(std::string_view s, const CharacterSet& cs, TextKind kind, std::size_t& bad) {
    std::string out;
    out.reserve(s.size() + s.size() / 2);
    const Initial init = initial_state(cs);
    GSet g0 = init.g0, g1 = init.g1;
    const std::size_t n = s.size();
    auto byte = [&](std::size_t k) { return static_cast<std::uint8_t>(s[k]); };
    auto replace = [&]() { put_utf8(out, kReplacement); ++bad; };
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t b = byte(i);
        if (b == ESC) {
            // ESC I* F: intermediates 0x20..0x2F, final 0x30..0x7E.
            std::size_t j = i + 1;
            while (j < n && j - i <= 4 && byte(j) >= 0x20 && byte(j) <= 0x2F) ++j;
            if (j >= n || byte(j) < 0x30 || byte(j) > 0x7E) {
                replace();  // truncated or broken escape: drop ESC and intermediates
                i = j;
                continue;
            }
            const std::string_view inter = s.substr(i + 1, j - i - 1);
            const char f = static_cast<char>(byte(j));
            GSet g = GSet::None;
            int target = -1;
            if (inter == "(") { g = set_94(f); target = 0; }
            else if (inter == ")") { g = set_94(f); target = 1; }
            else if (inter == "-") { g = set_96(f); target = 1; }
            else if (inter == "$" && (f == '@' || f == 'A' || f == 'B')) { g = set_94x94(f); target = 0; }
            else if (inter == "$(") { g = set_94x94(f); target = 0; }
            else if (inter == "$)") { g = set_94x94(f); target = 1; }
            // Single-byte 94-sets that contain ASCII letters are not allowed in G1: their GR
            // bytes would decode to ASCII delimiters.
            if (target == 1 && (g == GSet::Ascii || g == GSet::JisRoman)) g = GSet::None;
            if (g == GSet::None) replace();
            else if (target == 0) g0 = g;
            else g1 = g;
            i = j + 1;
            continue;
        }
        if (b < 0x20 || b == 0x7F) {
            out.push_back(static_cast<char>(b));
            if (is_line_delimiter(b)) { g0 = init.g0; g1 = init.g1; }
            ++i;
            continue;
        }
        if (b == 0x20) {
            out.push_back(' ');
            ++i;
            continue;
        }
        if (b < 0x80) {  // GL: G0
            const Shape sh = info(g0).shape;
            if (sh == Shape::S94x94) {
                if (i + 1 < n && byte(i + 1) >= 0x21 && byte(i + 1) <= 0x7E) {
                    const char32_t cp = cell(g0, static_cast<std::size_t>(b - 0x21) * 94 + (byte(i + 1) - 0x21u));
                    if (cp) put_utf8(out, cp); else replace();
                    i += 2;
                } else {
                    replace();  // truncated double-byte character
                    ++i;
                }
                continue;
            }
            if (is_value_delimiter(b, kind)) {
                out.push_back(static_cast<char>(b));
                g0 = init.g0;
                g1 = init.g1;
                ++i;
                continue;
            }
            const char32_t cp = cell(g0, b - 0x21u);
            if (cp) put_utf8(out, cp); else replace();
            ++i;
            continue;
        }
        // GR: G1
        const Shape sh = info(g1).shape;
        if (sh == Shape::None) { replace(); ++i; continue; }
        if (b < 0xA0) { put_utf8(out, b); ++i; continue; }  // C1 control
        if (sh == Shape::S96) {
            const char32_t cp = cell(g1, b - 0xA0u);
            if (cp) put_utf8(out, cp); else replace();
            ++i;
            continue;
        }
        if (b == 0xA0 || b == 0xFF) { replace(); ++i; continue; }
        if (sh == Shape::S94) {
            const char32_t cp = cell(g1, b - 0xA1u);
            if (cp) put_utf8(out, cp); else replace();
            ++i;
            continue;
        }
        if (i + 1 < n && byte(i + 1) >= 0xA1 && byte(i + 1) <= 0xFE) {
            const char32_t cp = cell(g1, static_cast<std::size_t>(b - 0xA1) * 94 + (byte(i + 1) - 0xA1u));
            if (cp) put_utf8(out, cp); else replace();
            i += 2;
        } else {
            replace();
            ++i;
        }
    }
    return out;
}

std::string decode_utf8(std::string_view s, std::size_t& bad) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        bool err = false;
        const char32_t cp = next_utf8(s, i, err);
        if (err) ++bad;
        put_utf8(out, cp);
    }
    return out;
}

// ------------------------------------------------------------------ GB18030 / GBK
// WHATWG Encoding Standard 10.2: "index gb18030 ranges code point" (0 = null).
char32_t gb_ranges_code_point(std::uint32_t pointer) noexcept {
    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) return 0;
    if (pointer == 7457) return 0xE7C7;
    const std::uint32_t* first = kGb18030RangePtr;
    const std::uint32_t* last = first + std::size(kGb18030RangePtr);
    const std::uint32_t* it = std::upper_bound(first, last, pointer);
    if (it == first) return 0;
    const std::size_t k = static_cast<std::size_t>(it - first) - 1;
    return static_cast<char32_t>(kGb18030RangeCp[k] + (pointer - kGb18030RangePtr[k]));
}

std::uint32_t gb_ranges_pointer(char32_t cp) noexcept {
    if (cp == 0xE7C7) return 7457;
    const std::uint32_t* first = kGb18030RangeCp;
    const std::uint32_t* last = first + std::size(kGb18030RangeCp);
    const std::uint32_t* it = std::upper_bound(first, last, static_cast<std::uint32_t>(cp));
    const std::size_t k = it == first ? 0 : static_cast<std::size_t>(it - first) - 1;
    return kGb18030RangePtr[k] + (static_cast<std::uint32_t>(cp) - kGb18030RangeCp[k]);
}

// The gb18030 decoder of WHATWG 10.2.1 (also used for GBK, as that standard specifies).
std::string decode_gb18030(std::string_view s, std::size_t& bad) {
    std::string out;
    out.reserve(s.size() + s.size() / 2);
    const std::size_t n = s.size();
    auto byte = [&](std::size_t k) { return static_cast<std::uint8_t>(s[k]); };
    auto replace = [&]() { put_utf8(out, kReplacement); ++bad; };
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t b = byte(i);
        if (b < 0x80) { out.push_back(static_cast<char>(b)); ++i; continue; }
        if (b == 0x80) { put_utf8(out, 0x20AC); ++i; continue; }
        if (b == 0xFF) { replace(); ++i; continue; }
        if (i + 1 >= n) { replace(); ++i; continue; }
        const std::uint8_t b2 = byte(i + 1);
        if (b2 >= 0x30 && b2 <= 0x39) {
            if (i + 3 < n && byte(i + 2) >= 0x81 && byte(i + 2) <= 0xFE && byte(i + 3) >= 0x30 &&
                byte(i + 3) <= 0x39) {
                const std::uint32_t p = ((b - 0x81u) * 10u + (b2 - 0x30u)) * 1260u +
                                        (byte(i + 2) - 0x81u) * 10u + (byte(i + 3) - 0x30u);
                const char32_t cp = gb_ranges_code_point(p);
                if (cp) put_utf8(out, cp); else replace();
                i += 4;
            } else {
                replace();  // the following bytes are processed again
                ++i;
            }
            continue;
        }
        if ((b2 >= 0x40 && b2 <= 0x7E) || (b2 >= 0x80 && b2 <= 0xFE)) {
            const std::size_t p = static_cast<std::size_t>(b - 0x81) * 190 + (b2 - (b2 < 0x7F ? 0x40u : 0x41u));
            put_utf8(out, kGb18030Two[p]);  // every two-byte pointer is assigned
            i += 2;
            continue;
        }
        replace();
        i += b2 < 0x80 ? 1 : 2;  // an ASCII trail byte is processed again
    }
    return out;
}

// ------------------------------------------------------------------ encoders
struct Encoder {
    const CharacterSet& cs;
    TextKind kind;
    EncodeResult r;

    void fail(char32_t cp, std::size_t off) {
        r.failures.push_back({cp, off});
        r.bytes.push_back('?');
    }
};

void encode_utf8(Encoder& e, std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        bool bad = false;
        (void)next_utf8(s, i, bad);
        if (bad) e.fail(kReplacement, start);
        else e.r.bytes.append(s.substr(start, i - start));
    }
}

void encode_gb18030(Encoder& e, std::string_view s, bool gbk) {
    std::string& out = e.r.bytes;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        bool bad = false;
        const char32_t cp = next_utf8(s, i, bad);
        if (bad) { e.fail(kReplacement, start); continue; }
        if (cp < 0x80) { out.push_back(static_cast<char>(cp)); continue; }
        if (cp == 0xE5E5) { e.fail(cp, start); continue; }
        if (gbk && cp == 0x20AC) { out.push_back(static_cast<char>(0x80)); continue; }
        bool done = false;
        for (std::size_t k = 0; k < std::size(kGb18030EncodeExtraCp); ++k) {
            if (kGb18030EncodeExtraCp[k] == cp) {
                out.push_back(static_cast<char>(kGb18030EncodeExtraBytes[k] >> 8));
                out.push_back(static_cast<char>(kGb18030EncodeExtraBytes[k] & 0xFF));
                done = true;
                break;
            }
        }
        if (done) continue;
        if (cp <= 0xFFFF) {
            const std::uint16_t* first = kGb18030TwoRev;
            const std::uint16_t* last = first + std::size(kGb18030TwoRev);
            const std::uint16_t* it = std::lower_bound(first, last, cp, [](std::uint16_t idx, char32_t v) {
                return static_cast<char32_t>(kGb18030Two[idx]) < v;
            });
            if (it != last && kGb18030Two[*it] == cp) {
                const unsigned lead = *it / 190u + 0x81u;
                const unsigned trail = *it % 190u;
                out.push_back(static_cast<char>(lead));
                out.push_back(static_cast<char>(trail + (trail < 0x3F ? 0x40u : 0x41u)));
                continue;
            }
        }
        if (gbk) { e.fail(cp, start); continue; }
        std::uint32_t p = gb_ranges_pointer(cp);
        const std::uint32_t b1 = p / 12600u;
        p %= 12600u;
        const std::uint32_t b2 = p / 1260u;
        p %= 1260u;
        out.push_back(static_cast<char>(b1 + 0x81u));
        out.push_back(static_cast<char>(b2 + 0x30u));
        out.push_back(static_cast<char>(p / 10u + 0x81u));
        out.push_back(static_cast<char>(p % 10u + 0x30u));
    }
}

void put_escape(std::string& out, GSet g, int target) {
    const GSetInfo& i = info(g);
    out.push_back(static_cast<char>(ESC));
    if (i.shape == Shape::S94) out.push_back(target == 0 ? '(' : ')');
    else if (i.shape == Shape::S96) out.push_back('-');
    else {
        out.push_back('$');
        if (!(target == 0 && g == GSet::Jis0208)) out.push_back(target == 0 ? '(' : ')');
    }
    out.push_back(i.final_byte);
}

void put_cell(std::string& out, GSet g, int target, long index) {
    const auto idx = static_cast<unsigned>(index);
    const unsigned high = target == 0 ? 0x00u : 0x80u;
    switch (info(g).shape) {
        case Shape::S94: out.push_back(static_cast<char>((0x21u + idx) | high)); break;
        case Shape::S96: out.push_back(static_cast<char>(0xA0u + idx)); break;
        case Shape::S94x94:
            out.push_back(static_cast<char>((0x21u + idx / 94u) | high));
            out.push_back(static_cast<char>((0x21u + idx % 94u) | high));
            break;
        case Shape::None: break;
    }
}

void encode_iso2022(Encoder& e, std::string_view s) {
    std::string& out = e.r.bytes;
    const Initial init = initial_state(e.cs);
    GSet g0 = init.g0, g1 = init.g1;
    // Candidate designations, in preference order after the current sets.
    struct Cand { GSet g; int target; };
    std::vector<Cand> cands;
    auto add = [&](GSet g, int t) {
        if (g == GSet::None) return;
        for (const Cand& c : cands) if (c.g == g && c.target == t) return;
        cands.push_back({g, t});
    };
    add(init.g0, 0);
    add(init.g1, 1);
    if (e.cs.extensions)
        for (Term t : e.cs.terms) { add(term_info(t).g0, 0); add(term_info(t).g1, 1); }

    auto restore = [&]() {
        if (g0 != init.g0) { put_escape(out, init.g0, 0); g0 = init.g0; }
        if (g1 != init.g1) {
            if (init.g1 != GSet::None) put_escape(out, init.g1, 1);
            g1 = init.g1;
        }
    };
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        bool bad = false;
        const char32_t cp = next_utf8(s, i, bad);
        if (bad) { e.fail(kReplacement, start); continue; }
        if (cp < 0x20 || cp == 0x7F || cp == 0x20) {
            if (cp != 0x20 && is_line_delimiter(static_cast<std::uint8_t>(cp))) restore();
            if (cp == ESC) { e.fail(cp, start); continue; }  // would be read as an escape
            out.push_back(static_cast<char>(cp));
            continue;
        }
        if (cp < 0x80 && is_value_delimiter(static_cast<std::uint8_t>(cp), e.kind)) {
            restore();
            out.push_back(static_cast<char>(cp));
            continue;
        }
        if (cp >= 0x80 && cp < 0xA0) {  // C1 control: representable only through a G1 set
            if (info(g1).shape != Shape::None) out.push_back(static_cast<char>(cp));
            else e.fail(cp, start);
            continue;
        }
        long idx = find_cell(g0, cp);
        if (idx >= 0) { put_cell(out, g0, 0, idx); continue; }
        idx = find_cell(g1, cp);
        if (idx >= 0) { put_cell(out, g1, 1, idx); continue; }
        bool done = false;
        for (const Cand& c : cands) {
            idx = find_cell(c.g, cp);
            if (idx < 0) continue;
            put_escape(out, c.g, c.target);
            (c.target == 0 ? g0 : g1) = c.g;
            put_cell(out, c.g, c.target, idx);
            done = true;
            break;
        }
        if (!done) e.fail(cp, start);
    }
    restore();
}

}  // namespace

// ------------------------------------------------------------------ public API
CharacterSet CharacterSet::parse(std::string_view value) {
    CharacterSet cs;
    cs.terms.clear();
    std::size_t start = 0;
    std::size_t index = 0;
    bool any_2022 = false;
    constexpr std::size_t kMaxValues = 16;
    while (start <= value.size() && index < kMaxValues) {
        std::size_t end = value.find('\\', start);
        if (end == std::string_view::npos) end = value.size();
        std::string_view v = value.substr(start, end - start);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\0')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.remove_suffix(1);
        Term t{};
        bool iso2022 = false, exact = true;
        if (v.empty()) {
            if (index == 0) cs.terms.push_back(Term::IR6);  // value 1 empty: default repertoire
        } else if (lookup_term(v, t, iso2022, exact)) {
            if (!exact) ++cs.lenient;
            if (std::find(cs.terms.begin(), cs.terms.end(), t) == cs.terms.end()) cs.terms.push_back(t);
            any_2022 = any_2022 || iso2022;
        } else {
            ++cs.unknown;
            if (index == 0) cs.terms.push_back(Term::IR6);
        }
        ++index;
        if (end == value.size()) break;
        start = end + 1;
    }
    if (cs.terms.empty()) cs.terms.push_back(Term::IR6);
    // UTF-8, GB18030 and GBK have no code extensions; when listed, they govern the whole value.
    for (Term t : cs.terms) {
        if (t == Term::IR192) { cs.scheme = Scheme::Utf8; break; }
        if (t == Term::GB18030) { cs.scheme = Scheme::Gb18030; break; }
        if (t == Term::GBK) { cs.scheme = Scheme::Gbk; break; }
    }
    if (cs.scheme != Scheme::Iso2022) {
        const Term keep = cs.scheme == Scheme::Utf8 ? Term::IR192 : cs.scheme == Scheme::Gb18030 ? Term::GB18030 : Term::GBK;
        cs.terms.assign(1, keep);
    }
    cs.extensions = cs.scheme == Scheme::Iso2022 && (any_2022 || cs.terms.size() > 1);
    return cs;
}

std::string CharacterSet::to_string() const {
    std::string out;
    for (std::size_t k = 0; k < terms.size(); ++k) {
        if (k) out.push_back('\\');
        const TermInfo& ti = term_info(terms[k]);
        if (k == 0 && terms[k] == Term::IR6) continue;  // empty value 1
        out += (extensions && !ti.iso2022_name.empty()) || ti.ir_name.empty() ? ti.iso2022_name : ti.ir_name;
    }
    return out;
}

TextKind text_kind(VR vr) noexcept {
    switch (vr) {
        case VR::PN: return TextKind::PersonName;
        case VR::ST: case VR::LT: case VR::UT: case VR::UR: return TextKind::Text;
        default: return TextKind::MultiValued;
    }
}

bool vr_uses_character_set(VR vr) noexcept {
    switch (vr) {
        case VR::SH: case VR::LO: case VR::UC: case VR::ST: case VR::LT: case VR::UT: case VR::PN:
            return true;
        default:
            return false;
    }
}

std::string decode(std::string_view bytes, const CharacterSet& cs, TextKind kind, std::size_t* replacements) {
    std::size_t bad = 0;
    std::string out;
    switch (cs.scheme) {
        case Scheme::Iso2022: out = decode_iso2022(bytes, cs, kind, bad); break;
        case Scheme::Utf8: out = decode_utf8(bytes, bad); break;
        case Scheme::Gb18030:
        case Scheme::Gbk: out = decode_gb18030(bytes, bad); break;
    }
    if (replacements) *replacements = bad;
    return out;
}

EncodeResult encode(std::string_view utf8, const CharacterSet& cs, TextKind kind) {
    Encoder e{cs, kind, {}};
    e.r.bytes.reserve(utf8.size() + 8);
    switch (cs.scheme) {
        case Scheme::Iso2022: encode_iso2022(e, utf8); break;
        case Scheme::Utf8: encode_utf8(e, utf8); break;
        case Scheme::Gb18030: encode_gb18030(e, utf8, false); break;
        case Scheme::Gbk: encode_gb18030(e, utf8, true); break;
    }
    return std::move(e.r);
}

PersonName split_person_name(std::string_view v) {
    PersonName pn;
    std::size_t g = 0, c = 0;
    for (char ch : v) {
        if (ch == '=') {
            if (++g >= 3) break;
            c = 0;
            continue;
        }
        pn.groups[g].push_back(ch);
        if (ch == '^') {
            ++c;
            continue;
        }
        if (c < 5) pn.components[g][c].push_back(ch);
    }
    return pn;
}

std::size_t table_bytes() noexcept { return kTableBytes; }

}  // namespace siaara::charset
