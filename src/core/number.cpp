#include "core/number.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <locale.h>
#elif defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif

namespace siaara {

namespace {

bool well_formed(std::string_view s) {
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    bool digits = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; digits = true; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; digits = true; }
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        bool exp_digits = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; exp_digits = true; }
        if (!exp_digits) return false;
    }
    return i == s.size();
}

#if defined(_WIN32)
_locale_t c_locale() {
    static _locale_t loc = _create_locale(LC_NUMERIC, "C");
    return loc;
}
#else
locale_t c_locale() {
    static locale_t loc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
    return loc;
}
#endif

}  // namespace

std::optional<double> parse_decimal_c(std::string_view token) {
    if (token.empty() || token.size() > 1024 || !well_formed(token)) return std::nullopt;
    const std::string s(token);  // NUL-terminated copy for the C library
    char* end = nullptr;
    errno = 0;
#if defined(_WIN32)
    const double v = _strtod_l(s.c_str(), &end, c_locale());
#else
    const double v = strtod_l(s.c_str(), &end, c_locale());
#endif
    if (end != s.c_str() + s.size()) return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;  // overflow gives HUGE_VAL
    return v;
}

}  // namespace siaara
