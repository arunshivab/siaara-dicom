// SIAARA DICOM Toolkit - multipart parsing and writing (RFC 2046 5.1, RFC 2387).
#include "web/multipart.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace siaara::web::multipart {

namespace {

bool is_bchar(char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '\'': case '(': case ')': case '+': case '_': case ',': case '-': case '.': case '/':
        case ':': case '=': case '?': case ' ':
            return true;
        default:
            return false;
    }
}

bool is_token_char(char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

// Position of the next delimiter "CRLF--boundary" at or after `from` whose boundary is followed by
// "--", LWSP or a line end; npos if none.
std::size_t find_delimiter(std::string_view body, std::string_view dash_boundary, std::size_t from) {
    for (;;) {
        const auto p = body.find(dash_boundary, from);
        if (p == std::string_view::npos) return p;
        const bool line_start = p == 0 || (p >= 2 && body[p - 2] == '\r' && body[p - 1] == '\n') ||
                                (p >= 1 && body[p - 1] == '\n');
        const std::size_t after = p + dash_boundary.size();
        const bool ends_ok = after >= body.size() || body[after] == '-' || body[after] == ' ' ||
                             body[after] == '\t' || body[after] == '\r' || body[after] == '\n';
        if (line_start && ends_ok) return p;
        from = p + 1;
    }
}

}  // namespace

bool valid_boundary(std::string_view b, std::size_t max_len) noexcept {
    if (b.empty() || b.size() > max_len || b.size() > 70 || b.back() == ' ') return false;
    for (char c : b)
        if (!is_bchar(c)) return false;
    return true;
}

Result<std::vector<Part>> parse(std::string_view body, std::string_view boundary, const Limits& limits) {
    if (!valid_boundary(boundary, limits.max_boundary)) return Status(Errc::Malformed, "multipart: invalid boundary");
    const std::string dash = "--" + std::string(boundary);
    std::vector<Part> parts;
    std::size_t pos = find_delimiter(body, dash, 0);
    if (pos == std::string_view::npos) return Status(Errc::Malformed, "multipart: no boundary delimiter");
    for (;;) {
        std::size_t after = pos + dash.size();
        if (body.substr(after, 2) == "--") return parts;  // close delimiter; the epilogue is ignored
        while (after < body.size() && (body[after] == ' ' || body[after] == '\t')) ++after;  // transport padding
        if (body.substr(after, 2) == "\r\n") after += 2;
        else if (body.substr(after, 1) == "\n") after += 1;
        else return Status(after >= body.size() ? Errc::Truncated : Errc::Malformed, "multipart: bad delimiter line");
        const std::size_t next = find_delimiter(body, dash, after);
        if (next == std::string_view::npos) return Status(Errc::Truncated, "multipart: missing close delimiter");
        // The CRLF before the delimiter belongs to the delimiter.
        std::size_t end = next;
        if (end >= after + 2 && body[end - 2] == '\r' && body[end - 1] == '\n') end -= 2;
        else if (end >= after + 1 && body[end - 1] == '\n') end -= 1;
        if (parts.size() >= limits.max_parts) return Status(Errc::LimitExceeded, "multipart: too many parts");
        Part part;
        std::string_view content = body.substr(after, end - after);
        // Part headers, up to an empty line (a part may also have no headers at all).
        std::size_t hp = 0, header_bytes = 0;
        for (;;) {
            auto nl = content.find('\n', hp);
            if (nl == std::string_view::npos) {
                if (hp == content.size()) break;  // headers only, empty body, no blank line
                return Status(Errc::Malformed, "multipart: part header section not terminated");
            }
            std::string_view line = content.substr(hp, nl - hp);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            hp = nl + 1;
            header_bytes += line.size() + 2;
            if (header_bytes > limits.max_part_header_bytes) return Status(Errc::LimitExceeded, "multipart: part headers too large");
            if (line.empty()) break;
            if (line[0] == ' ' || line[0] == '\t') return Status(Errc::Malformed, "multipart: folded part header");
            if (part.headers.size() >= limits.max_part_headers) return Status(Errc::LimitExceeded, "multipart: too many part headers");
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) return Status(Errc::Malformed, "multipart: bad part header");
            for (std::size_t k = 0; k < colon; ++k)
                if (!is_token_char(line[k])) return Status(Errc::Malformed, "multipart: bad part header name");
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            for (char c : value)
                if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7F)
                    return Status(Errc::Malformed, "multipart: control character in part header");
            part.headers.push_back({std::string(line.substr(0, colon)), std::string(value)});
        }
        part.body = content.substr(std::min(hp, content.size()));
        parts.push_back(std::move(part));
        pos = next;
    }
}

Writer::Writer() = default;

void Writer::add(std::string_view content_type, std::string_view data, http::Headers extra) {
    Item it;
    it.headers.push_back({"Content-Type", std::string(content_type)});
    for (auto& h : extra) it.headers.push_back(std::move(h));
    it.data.assign(data);
    items_.push_back(std::move(it));
}

std::string Writer::finish() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                        (counter.fetch_add(1) * 0x9E3779B97F4A7C15ull));
    static constexpr char kChars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::string b = "siaara-";
        for (int i = 0; i < 32; ++i) b.push_back(kChars[gen() % 62]);
        bool clash = false;
        for (const auto& it : items_)
            if (it.data.find(b) != std::string::npos) { clash = true; break; }
        if (!clash) { boundary_ = std::move(b); break; }
    }
    std::string out;
    std::size_t total = 0;
    for (const auto& it : items_) total += it.data.size() + 128;
    out.reserve(total + 64);
    for (const auto& it : items_) {
        out += "--" + boundary_ + "\r\n";
        (void)http::append_headers(out, it.headers);
        out += "\r\n";
        out += it.data;
        out += "\r\n";
    }
    out += "--" + boundary_ + "--\r\n";
    return out;
}

std::string Writer::content_type(std::string_view root_type) const {
    return "multipart/related; type=" + http::quote_if_needed(root_type) + "; boundary=" + http::quote_if_needed(boundary_);
}

}  // namespace siaara::web::multipart
