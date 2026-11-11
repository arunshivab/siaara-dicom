#include "core/display_safe.hpp"

#include <cstddef>

namespace siaara::text {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

// One step over the input: the code point at `p` and the number of bytes it takes, or n == 0
// when the byte at `p` does not start a well-formed UTF-8 sequence.
struct Step {
    std::uint32_t c;
    std::size_t n;
};

Step next(const unsigned char* p, std::size_t left) noexcept {
    std::uint32_t c = p[0];
    std::size_t n = 1;
    if (c >= 0xF0) {
        n = c <= 0xF4 ? 4 : 0;
        c &= 0x07;
    } else if (c >= 0xE0) {
        n = 3;
        c &= 0x0F;
    } else if (c >= 0xC2) {
        n = 2;
        c &= 0x1F;
    } else if (c >= 0x80) {
        n = 0;
    }
    if (n > left) return {0, 0};
    for (std::size_t k = 1; k < n; ++k) {
        if ((p[k] & 0xC0) != 0x80) return {0, 0};
        c = (c << 6) | (p[k] & 0x3Fu);
    }
    if (n == 0 || (n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF))
        return {0, 0};
    return {c, n};
}

bool becomes_space(std::uint32_t c) noexcept {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029;
}

template <class Sink>
void clean(std::string_view in, Sink&& sink) {
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    std::size_t left = in.size();
    while (left > 0) {
        const Step s = next(p, left);
        if (s.n == 0) {
            sink(kReplacement);
            ++p;
            --left;
            continue;
        }
        if (becomes_space(s.c)) {
            sink(std::string_view(" ", 1));
        } else if (!is_format_character(s.c)) {
            sink(std::string_view(reinterpret_cast<const char*>(p), s.n));
        }
        p += s.n;
        left -= s.n;
    }
}

}  // namespace

bool is_format_character(std::uint32_t c) noexcept {
    // General category Cf, Unicode 16.0 (DerivedGeneralCategory.txt).
    static constexpr std::uint32_t kRanges[][2] = {
        {0x00AD, 0x00AD},   {0x0600, 0x0605},   {0x061C, 0x061C},   {0x06DD, 0x06DD},   {0x070F, 0x070F},
        {0x0890, 0x0891},   {0x08E2, 0x08E2},   {0x180E, 0x180E},   {0x200B, 0x200F},   {0x202A, 0x202E},
        {0x2060, 0x2064},   {0x2066, 0x206F},   {0xFEFF, 0xFEFF},   {0xFFF9, 0xFFFB},   {0x110BD, 0x110BD},
        {0x110CD, 0x110CD}, {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001},
        {0xE0020, 0xE007F}};
    for (const auto& r : kRanges)
        if (c >= r[0] && c <= r[1]) return true;
    return false;
}

std::string display_safe(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    clean(utf8, [&out](std::string_view piece) { out.append(piece); });
    return out;
}

std::size_t display_safe_size(std::string_view utf8) noexcept {
    std::size_t n = 0;
    clean(utf8, [&n](std::string_view piece) noexcept { n += piece.size(); });
    return n;
}

}  // namespace siaara::text
