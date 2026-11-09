// SIAARA DICOM Toolkit - base64 (RFC 4648 section 4).
#include "web/base64.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace siaara::web {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_symbol(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
bool is_space(unsigned char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}  // namespace

Status base64_encode(std::span<const std::uint8_t> data, std::string& out) {
    const std::uint64_t groups = (static_cast<std::uint64_t>(data.size()) + 2) / 3;
    std::uint64_t need = 0, total = 0;
    if (!checked_mul(groups, 4, need) || !checked_add(need, out.size(), total) || total > SIZE_MAX / 2)
        return Status(Errc::LimitExceeded, "base64 output too large");
    out.reserve(static_cast<std::size_t>(total));
    std::size_t i = 0;
    const std::size_t n = data.size();
    for (; i + 3 <= n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    if (n - i == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.append("==");
    } else if (n - i == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return Status::ok();
}

std::string base64_encode(std::string_view data) {
    std::string out;
    (void)base64_encode(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()), out);
    return out;
}

Status base64_decode(std::string_view text, std::vector<std::uint8_t>& out, std::uint64_t max_out) {
    out.clear();
    // Upper bound of the output: 3 bytes per 4 symbols.
    const std::uint64_t bound = (static_cast<std::uint64_t>(text.size()) / 4 + 1) * 3;
    if (bound <= max_out && bound <= SIZE_MAX / 2) out.reserve(static_cast<std::size_t>(bound));
    std::uint32_t acc = 0;
    int have = 0;     // symbols in the current quantum
    int pads = 0;     // '=' seen
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (is_space(c)) continue;
        if (c == '=') {
            if (have < 2) return Status(Errc::Malformed, "base64: misplaced padding", i);
            ++pads;
            ++have;
            if (have == 4) {
                // Only whitespace may follow the padded final quantum.
                for (std::size_t j = i + 1; j < text.size(); ++j)
                    if (!is_space(static_cast<unsigned char>(text[j])))
                        return Status(Errc::Malformed, "base64: data after padding", j);
                break;
            }
            continue;
        }
        if (pads) return Status(Errc::Malformed, "base64: data after padding", i);
        const int s = decode_symbol(c);
        if (s < 0) return Status(Errc::Malformed, "base64: invalid character", i);
        acc = (acc << 6) | static_cast<std::uint32_t>(s);
        if (++have == 4) {
            if (static_cast<std::uint64_t>(out.size()) + 3 > max_out)
                return Status(Errc::LimitExceeded, "base64: decoded data too large");
            out.push_back(static_cast<std::uint8_t>(acc >> 16));
            out.push_back(static_cast<std::uint8_t>(acc >> 8));
            out.push_back(static_cast<std::uint8_t>(acc));
            acc = 0;
            have = 0;
        }
    }
    const int data_syms = have - pads;
    if (have == 4 || (have > 0 && pads == 0)) {
        // Final (possibly unpadded) partial quantum.
        if (data_syms == 1) return Status(Errc::Malformed, "base64: truncated quantum");
        if (data_syms == 2 || data_syms == 3) {
            if (pads && have != 4) return Status(Errc::Malformed, "base64: truncated padding");
            const std::uint32_t v = acc << (6 * (4 - data_syms));
            const std::uint32_t mask = data_syms == 2 ? 0xFFFFu : 0xFFu;
            if ((v & mask) != 0) return Status(Errc::Malformed, "base64: non-zero padding bits");
            const std::uint64_t add = static_cast<std::uint64_t>(data_syms - 1);
            if (static_cast<std::uint64_t>(out.size()) + add > max_out)
                return Status(Errc::LimitExceeded, "base64: decoded data too large");
            out.push_back(static_cast<std::uint8_t>(v >> 16));
            if (data_syms == 3) out.push_back(static_cast<std::uint8_t>(v >> 8));
        }
    } else if (have != 0) {
        return Status(Errc::Malformed, "base64: truncated padding");
    }
    return Status::ok();
}

}  // namespace siaara::web
