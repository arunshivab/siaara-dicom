#include "repair/values.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

#include "core/number.hpp"
#include "deid/sha256.hpp"

namespace siaara::repair {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), digit);
}
int num(std::string_view s) {
    int v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}
bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in(int y, int m) {
    static constexpr int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

bool control_ok(VR vr, unsigned char c) {
    if (c == 0x1B) return vr != VR::AE && vr != VR::CS && vr != VR::UI;  // ESC for code extensions
    if (vr == VR::ST || vr == VR::LT || vr == VR::UT)
        return c == 0x0A || c == 0x0C || c == 0x0D || c == 0x09;
    return false;
}

ValueCheck check_da(std::string_view v) {
    if (v.size() != 8) return all_digits(v) ? ValueCheck::BadFormat : ValueCheck::BadCharacter;
    if (!all_digits(v)) return ValueCheck::BadCharacter;
    const int y = num(v.substr(0, 4)), m = num(v.substr(4, 2)), d = num(v.substr(6, 2));
    if (m < 1 || m > 12 || d < 1 || d > days_in(y, m)) return ValueCheck::BadDate;
    return ValueCheck::Ok;
}

ValueCheck check_tm(std::string_view v) {
    std::string_view whole = v, frac;
    if (auto dot = v.find('.'); dot != std::string_view::npos) {
        whole = v.substr(0, dot);
        frac = v.substr(dot + 1);
        if (whole.size() != 6 || frac.empty() || frac.size() > 6 || !all_digits(frac))
            return std::all_of(v.begin(), v.end(), [](char c) { return digit(c) || c == '.'; })
                       ? ValueCheck::BadFormat
                       : ValueCheck::BadCharacter;
    }
    if (!all_digits(whole)) return ValueCheck::BadCharacter;
    if (whole.size() != 2 && whole.size() != 4 && whole.size() != 6) return ValueCheck::BadFormat;
    if (num(whole.substr(0, 2)) > 23) return ValueCheck::BadTime;
    if (whole.size() >= 4 && num(whole.substr(2, 2)) > 59) return ValueCheck::BadTime;
    if (whole.size() >= 6 && num(whole.substr(4, 2)) > 60) return ValueCheck::BadTime;
    return ValueCheck::Ok;
}

ValueCheck check_dt(std::string_view v) {
    std::string_view body = v;
    if (auto z = v.find_first_of("+-"); z != std::string_view::npos) {
        const std::string_view off = v.substr(z + 1);
        if (off.size() != 4 || !all_digits(off)) return ValueCheck::BadFormat;
        const int hh = num(off.substr(0, 2)), mm = num(off.substr(2, 2));
        if (hh > 14 || mm > 59) return ValueCheck::BadTime;
        body = v.substr(0, z);
    }
    std::string_view whole = body, frac;
    if (auto dot = body.find('.'); dot != std::string_view::npos) {
        whole = body.substr(0, dot);
        frac = body.substr(dot + 1);
        if (whole.size() != 14 || frac.empty() || frac.size() > 6 || !all_digits(frac)) return ValueCheck::BadFormat;
    }
    if (!all_digits(whole)) return ValueCheck::BadCharacter;
    const std::size_t n = whole.size();
    if (n != 4 && n != 6 && n != 8 && n != 10 && n != 12 && n != 14) return ValueCheck::BadFormat;
    const int y = num(whole.substr(0, 4));
    if (n >= 6) {
        const int m = num(whole.substr(4, 2));
        if (m < 1 || m > 12) return ValueCheck::BadDate;
        if (n >= 8) {
            const int d = num(whole.substr(6, 2));
            if (d < 1 || d > days_in(y, m)) return ValueCheck::BadDate;
        }
    }
    if (n >= 10 && num(whole.substr(8, 2)) > 23) return ValueCheck::BadTime;
    if (n >= 12 && num(whole.substr(10, 2)) > 59) return ValueCheck::BadTime;
    if (n >= 14 && num(whole.substr(12, 2)) > 60) return ValueCheck::BadTime;
    return ValueCheck::Ok;
}

bool ds_syntax(std::string_view v) {
    std::size_t i = 0;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
    std::size_t int_digits = 0, frac_digits = 0;
    while (i < v.size() && digit(v[i])) ++i, ++int_digits;
    if (i < v.size() && v[i] == '.') {
        ++i;
        while (i < v.size() && digit(v[i])) ++i, ++frac_digits;
    }
    if (int_digits + frac_digits == 0) return false;
    if (i < v.size() && (v[i] == 'e' || v[i] == 'E')) {
        ++i;
        if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
        std::size_t e = 0;
        while (i < v.size() && digit(v[i])) ++i, ++e;
        if (e == 0) return false;
    }
    return i == v.size();
}

bool is_syntax(std::string_view v, std::int64_t* out = nullptr) {
    std::string_view s = v;
    if (!s.empty() && (s.front() == '+' || s.front() == '-')) s.remove_prefix(1);
    if (!all_digits(s)) return false;
    std::int64_t x = 0;
    std::string_view t = v;
    if (!t.empty() && t.front() == '+') t.remove_prefix(1);
    const auto r = std::from_chars(t.data(), t.data() + t.size(), x);
    if (r.ec != std::errc() || r.ptr != t.data() + t.size()) return false;
    if (out) *out = x;
    return x >= -2147483648LL && x <= 2147483647LL;
}

std::string_view trim_spaces(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return s;
}

std::size_t utf8_chars(std::string_view s) {
    std::size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

}  // namespace

VrLimit vr_limit(VR vr) noexcept {
    switch (vr) {
        case VR::AE: return {16, false, false};
        case VR::AS: return {4, true, false};
        case VR::AT: return {4, true, false};
        case VR::CS: return {16, false, false};
        case VR::DA: return {8, false, false};
        case VR::DS: return {16, false, false};
        case VR::DT: return {26, false, false};
        case VR::FL: return {4, true, false};
        case VR::FD: return {8, true, false};
        case VR::IS: return {12, false, false};
        case VR::LO: return {64, false, true};
        case VR::LT: return {10240, false, true};
        case VR::PN: return {64, false, true};  // per component group
        case VR::SH: return {16, false, true};
        case VR::SL: return {4, true, false};
        case VR::SS: return {2, true, false};
        case VR::ST: return {1024, false, true};
        case VR::SV: return {8, true, false};
        case VR::TM: return {14, false, false};
        case VR::UI: return {64, false, false};
        case VR::UL: return {4, true, false};
        case VR::US: return {2, true, false};
        case VR::UV: return {8, true, false};
        default: return {};
    }
}

std::string_view value_check_name(ValueCheck c) noexcept {
    switch (c) {
        case ValueCheck::Ok: return "ok";
        case ValueCheck::BadCharacter: return "character not allowed for the VR";
        case ValueCheck::BadFormat: return "value does not follow the VR's format";
        case ValueCheck::TooLong: return "value too long for the VR";
        case ValueCheck::BadDate: return "date out of range";
        case ValueCheck::BadTime: return "time out of range";
    }
    return "";
}

ValueCheck check_value(VR vr, std::string_view v) noexcept {
    for (char ch : v) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 && !control_ok(vr, c)) return ValueCheck::BadCharacter;
        if (c == 0x7F) return ValueCheck::BadCharacter;
    }
    const VrLimit lim = vr_limit(vr);
    if (lim.max && !lim.in_characters && v.size() > lim.max) return ValueCheck::TooLong;
    if (lim.max && lim.in_characters && vr != VR::PN && utf8_chars(v) > lim.max) return ValueCheck::TooLong;
    if (v.empty()) return ValueCheck::Ok;
    switch (vr) {
        case VR::AE:
            for (char c : v)
                if (c == '\\' || static_cast<unsigned char>(c) >= 0x80) return ValueCheck::BadCharacter;
            return ValueCheck::Ok;
        case VR::CS:
            for (char c : v)
                if (!((c >= 'A' && c <= 'Z') || digit(c) || c == ' ' || c == '_')) return ValueCheck::BadCharacter;
            return ValueCheck::Ok;
        case VR::AS:
            if (v.size() != 4) return ValueCheck::BadFormat;
            if (!all_digits(v.substr(0, 3))) return ValueCheck::BadCharacter;
            if (v[3] != 'D' && v[3] != 'W' && v[3] != 'M' && v[3] != 'Y')
                return (v[3] >= 'a' && v[3] <= 'z') || (v[3] >= 'A' && v[3] <= 'Z') ? ValueCheck::BadFormat
                                                                                      : ValueCheck::BadCharacter;
            return ValueCheck::Ok;
        case VR::DA: return check_da(v);
        case VR::TM: return check_tm(v);
        case VR::DT: return check_dt(v);
        case VR::DS:
            for (char c : v)
                if (!(digit(c) || c == '+' || c == '-' || c == 'e' || c == 'E' || c == '.' || c == ' '))
                    return ValueCheck::BadCharacter;
            return ds_syntax(v) ? ValueCheck::Ok : ValueCheck::BadFormat;
        case VR::IS:
            for (char c : v)
                if (!(digit(c) || c == '+' || c == '-' || c == ' ')) return ValueCheck::BadCharacter;
            return is_syntax(v) ? ValueCheck::Ok : ValueCheck::BadFormat;
        case VR::UI:
            for (char c : v)
                if (!(digit(c) || c == '.')) return ValueCheck::BadCharacter;
            return uid_is_valid(v) ? ValueCheck::Ok : ValueCheck::BadFormat;
        case VR::PN: {
            // At most 64 characters per component group, at most 5 components, 3 groups.
            std::size_t groups = 1, start = 0;
            for (std::size_t i = 0; i <= v.size(); ++i) {
                if (i == v.size() || v[i] == '=') {
                    const std::string_view g = v.substr(start, i - start);
                    if (utf8_chars(g) > 64) return ValueCheck::TooLong;
                    if (static_cast<std::size_t>(std::count(g.begin(), g.end(), '^')) > 4) return ValueCheck::BadFormat;
                    start = i + 1;
                    if (i < v.size()) ++groups;
                }
            }
            return groups > 3 ? ValueCheck::BadFormat : ValueCheck::Ok;
        }
        case VR::UR:
            for (char c : v)
                if (c == ' ' || static_cast<unsigned char>(c) >= 0x80) return ValueCheck::BadCharacter;
            return ValueCheck::Ok;
        default:
            return ValueCheck::Ok;
    }
}

bool uid_is_valid(std::string_view uid) noexcept {
    if (uid.empty() || uid.size() > 64) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= uid.size(); ++i) {
        if (i == uid.size() || uid[i] == '.') {
            const std::string_view comp = uid.substr(start, i - start);
            if (comp.empty() || !all_digits(comp)) return false;
            if (comp.size() > 1 && comp[0] == '0') return false;
            start = i + 1;
        }
    }
    return true;
}

std::optional<std::string> normalise_value(VR vr, std::string_view raw) {
    const std::string_view v = trim_spaces(raw);
    switch (vr) {
        case VR::DA: {
            // YYYY.MM.DD or YYYY-MM-DD
            if (v.size() == 10 && (v[4] == '.' || v[4] == '-') && v[7] == v[4] && all_digits(v.substr(0, 4)) &&
                all_digits(v.substr(5, 2)) && all_digits(v.substr(8, 2))) {
                std::string out = std::string(v.substr(0, 4)) + std::string(v.substr(5, 2)) + std::string(v.substr(8, 2));
                if (check_da(out) == ValueCheck::Ok) return out;
            }
            if (v != raw && check_da(v) == ValueCheck::Ok) return std::string(v);
            return std::nullopt;
        }
        case VR::TM: {
            std::string out;
            std::string_view s = v;
            std::string_view frac;
            if (auto dot = s.find('.'); dot != std::string_view::npos) {
                frac = s.substr(dot);
                s = s.substr(0, dot);
            }
            if ((s.size() == 5 || s.size() == 8) && s[2] == ':' && (s.size() == 5 || s[5] == ':')) {
                out = std::string(s.substr(0, 2)) + std::string(s.substr(3, 2));
                if (s.size() == 8) out += std::string(s.substr(6, 2));
                else if (!frac.empty()) return std::nullopt;
                out += std::string(frac);
                if (check_tm(out) == ValueCheck::Ok) return out;
                return std::nullopt;
            }
            if (v != raw && check_tm(v) == ValueCheck::Ok) return std::string(v);
            return std::nullopt;
        }
        case VR::DS: {
            if (!ds_syntax(v)) return std::nullopt;
            const auto d = parse_decimal_c(v);
            if (!d || !std::isfinite(*d)) return std::nullopt;
            if (v.size() <= 16 && v != raw) return std::string(v);
            if (v.size() <= 16) return std::nullopt;
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof buf, *d);
            if (r.ec != std::errc() || r.ptr - buf > 16) return std::nullopt;
            std::string out(buf, r.ptr);
            const auto back = parse_decimal_c(out);
            if (!back || *back != *d) return std::nullopt;
            return out;
        }
        case VR::IS: {
            std::string_view s = v;
            if (auto dot = s.find('.'); dot != std::string_view::npos) {
                const std::string_view frac = s.substr(dot + 1);
                if (!std::all_of(frac.begin(), frac.end(), [](char c) { return c == '0'; })) return std::nullopt;
                s = s.substr(0, dot);
            }
            std::int64_t x = 0;
            if (!is_syntax(s, &x)) return std::nullopt;
            std::string out = std::to_string(x);
            if (out == raw) return std::nullopt;
            return out;
        }
        case VR::UI: {
            std::string_view s = raw;
            while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
            while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
            if (s.size() != raw.size() && uid_is_valid(s) && !(raw.size() == s.size() + 1 && raw.back() == '\0'))
                return std::string(s);
            return std::nullopt;
        }
        case VR::AS: {
            if (v.size() == 4 && all_digits(v.substr(0, 3)) && (v[3] == 'd' || v[3] == 'w' || v[3] == 'm' || v[3] == 'y')) {
                std::string out(v);
                out[3] = static_cast<char>(out[3] - 'a' + 'A');
                return out;
            }
            return std::nullopt;
        }
        case VR::CS: {
            std::string out(v);
            bool changed = false;
            for (char& c : out)
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                    changed = true;
                }
            if (changed && check_value(VR::CS, out) == ValueCheck::Ok) return out;
            return std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

bool vm_allows(std::string_view vm, std::size_t n) noexcept {
    if (vm.empty()) return true;
    // Forms: "k", "a-b", "a-n", "a-kn"
    auto parse_uint = [](std::string_view s, std::size_t& out) {
        if (s.empty()) return false;
        std::size_t v = 0;
        for (char c : s) {
            if (!digit(c)) return false;
            v = v * 10 + static_cast<std::size_t>(c - '0');
        }
        out = v;
        return true;
    };
    const auto dash = vm.find('-');
    std::size_t lo = 0;
    if (dash == std::string_view::npos) {
        if (!parse_uint(vm, lo)) return true;
        return n == lo;
    }
    if (!parse_uint(vm.substr(0, dash), lo)) return true;
    std::string_view hi = vm.substr(dash + 1);
    if (hi == "n") return n >= lo;
    if (!hi.empty() && hi.back() == 'n') {
        std::size_t k = 0;
        if (!parse_uint(hi.substr(0, hi.size() - 1), k) || k == 0) return true;
        return n >= lo && n % k == 0;
    }
    std::size_t h = 0;
    if (!parse_uint(hi, h)) return true;
    return n >= lo && n <= h;
}

std::string uid_from_seed(std::string_view seed) {
    deid::Digest d = deid::Sha256::hash(seed.data(), seed.size());
    std::uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = d[static_cast<std::size_t>(i)];
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0Fu) | 0x80u);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3Fu) | 0x80u);
    std::string digits;
    bool nonzero = true;
    while (nonzero) {
        unsigned rem = 0;
        nonzero = false;
        for (int i = 0; i < 16; ++i) {
            const unsigned cur = (rem << 8) | b[i];
            b[i] = static_cast<std::uint8_t>(cur / 10u);
            rem = cur % 10u;
            if (b[i]) nonzero = true;
        }
        digits.push_back(static_cast<char>('0' + rem));
    }
    std::reverse(digits.begin(), digits.end());
    return "2.25." + digits;
}

std::size_t split_count(std::string_view s) noexcept {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\\')) + 1;
}

}  // namespace siaara::repair
