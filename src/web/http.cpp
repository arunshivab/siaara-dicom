// SIAARA DICOM Toolkit - HTTP/1.1 client and server, written from RFC 9110 and RFC 9112.
#include "web/http.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace siaara::web::http {

namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool is_tchar(char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-':
        case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}
bool is_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s)
        if (!is_tchar(c)) return false;
    return true;
}
bool is_ows(char c) noexcept { return c == ' ' || c == '\t'; }
std::string_view trim_ows(std::string_view s) noexcept {
    while (!s.empty() && is_ows(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ows(s.back())) s.remove_suffix(1);
    return s;
}
std::string to_lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = lower(c);
    return r;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Strict decimal Content-Length (RFC 9110 8.6): 1*DIGIT, checked.
bool parse_decimal(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty() || s.size() > 19) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        if (!checked_mul(v, 10, v) || !checked_add(v, static_cast<std::uint64_t>(c - '0'), v)) return false;
    }
    out = v;
    return true;
}

}  // namespace

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

const std::string* find_header(const Headers& h, std::string_view name) noexcept {
    for (const auto& f : h)
        if (iequals(f.name, name)) return &f.value;
    return nullptr;
}

std::vector<std::string> header_list(const Headers& h, std::string_view name) {
    std::vector<std::string> out;
    for (const auto& f : h) {
        if (!iequals(f.name, name)) continue;
        std::string_view v = f.value;
        bool quoted = false;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= v.size(); ++i) {
            if (i < v.size() && v[i] == '"') quoted = !quoted;
            if (i < v.size() && v[i] == '\\' && quoted) { ++i; continue; }
            if (i == v.size() || (v[i] == ',' && !quoted)) {
                const auto item = trim_ows(v.substr(start, i - start));
                if (!item.empty()) out.emplace_back(item);
                start = i + 1;
            }
        }
    }
    return out;
}

void Response::set(std::string_view name, std::string value) {
    for (auto& f : headers)
        if (iequals(f.name, name)) { f.value = std::move(value); return; }
    headers.push_back({std::string(name), std::move(value)});
}

std::string_view reason_phrase(int status) noexcept {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------- media types
const std::string* MediaType::param(std::string_view name) const noexcept {
    for (const auto& p : params)
        if (p.first == name) return &p.second;
    return nullptr;
}

namespace {

// Parses ";name=value" parameters starting at s[i]. Handles quoted-string (RFC 9110 5.6.4).
bool parse_params(std::string_view s, std::size_t& i, std::vector<std::pair<std::string, std::string>>& out,
                  int* weight) {
    for (;;) {
        while (i < s.size() && is_ows(s[i])) ++i;
        if (i >= s.size()) return true;
        if (s[i] != ';') return false;
        ++i;
        while (i < s.size() && is_ows(s[i])) ++i;
        if (i >= s.size()) return true;  // tolerate a trailing ';'
        const std::size_t ns = i;
        while (i < s.size() && is_tchar(s[i])) ++i;
        const auto name = s.substr(ns, i - ns);
        if (name.empty() || i >= s.size() || s[i] != '=') return false;
        ++i;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            ++i;
            bool closed = false;
            while (i < s.size()) {
                const char c = s[i];
                if (c == '"') { ++i; closed = true; break; }
                if (c == '\\') {
                    if (i + 1 >= s.size()) return false;
                    value.push_back(s[i + 1]);
                    i += 2;
                    continue;
                }
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return false;
                value.push_back(c);
                ++i;
            }
            if (!closed) return false;
        } else {
            const std::size_t vs = i;
            while (i < s.size() && is_tchar(s[i])) ++i;
            value.assign(s.substr(vs, i - vs));
            if (value.empty()) return false;
        }
        std::string lname = to_lower(name);
        if (weight && lname == "q") {
            // qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] )
            int w = 0;
            if (value.empty() || (value[0] != '0' && value[0] != '1') || value.size() > 5) return false;
            w = (value[0] - '0') * 1000;
            if (value.size() > 1) {
                if (value[1] != '.') return false;
                int scale = 100;
                for (std::size_t k = 2; k < value.size(); ++k) {
                    if (value[k] < '0' || value[k] > '9') return false;
                    w += (value[k] - '0') * scale;
                    scale /= 10;
                }
            }
            if (w > 1000) return false;
            *weight = w;
            weight = nullptr;  // later parameters are accept-ext
            continue;
        }
        if (out.size() >= 32) return false;
        out.emplace_back(std::move(lname), std::move(value));
    }
}

bool parse_one_media(std::string_view s, MediaType& m, bool accept) {
    s = trim_ows(s);
    std::size_t i = 0;
    while (i < s.size() && is_tchar(s[i])) ++i;
    m.type = to_lower(s.substr(0, i));
    if (m.type.empty() || i >= s.size() || s[i] != '/') return false;
    const std::size_t ss = ++i;
    while (i < s.size() && is_tchar(s[i])) ++i;
    m.subtype = to_lower(s.substr(ss, i - ss));
    if (m.subtype.empty()) return false;
    if (!accept && (m.type == "*" || m.subtype == "*")) return false;
    if (m.type == "*" && m.subtype != "*") return false;
    return parse_params(s, i, m.params, accept ? &m.weight : nullptr);
}

}  // namespace

Result<MediaType> parse_media_type(std::string_view s) {
    MediaType m;
    if (s.size() > 4096 || !parse_one_media(s, m, false)) return Status(Errc::Malformed, "invalid media type");
    return m;
}

std::vector<MediaType> parse_accept(std::string_view s) {
    std::vector<MediaType> out;
    if (s.size() > 16384) return out;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] == '"') quoted = !quoted;
        if (i < s.size() && s[i] == '\\' && quoted) { ++i; continue; }
        if (i == s.size() || (s[i] == ',' && !quoted)) {
            const auto item = trim_ows(s.substr(start, i - start));
            start = i + 1;
            if (item.empty() || out.size() >= 64) continue;
            MediaType m;
            if (parse_one_media(item, m, true)) out.push_back(std::move(m));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const MediaType& a, const MediaType& b) { return a.weight > b.weight; });
    return out;
}

std::string quote_if_needed(std::string_view v) {
    if (is_token(v)) return std::string(v);
    std::string q = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') q.push_back('\\');
        q.push_back(c);
    }
    q.push_back('"');
    return q;
}

std::string format_media_type(const MediaType& m) {
    std::string s = m.type + "/" + m.subtype;
    for (const auto& p : m.params) {
        s += "; ";
        s += p.first;
        s += "=";
        s += quote_if_needed(p.second);
    }
    return s;
}

// ---------------------------------------------------------------- URLs
std::string percent_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string percent_encode_query(std::string_view s) {
    static constexpr std::string_view kKeep = "/:@!$'()*,;?";
    std::string out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && kKeep.find(s[i]) == std::string_view::npos) continue;
        out += percent_encode(s.substr(start, i - start));
        if (i < s.size()) out.push_back(s[i]);
        start = i + 1;
    }
    return out;
}

Result<std::string> percent_decode(std::string_view s, bool plus_is_space) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) return Status(Errc::Malformed, "bad percent-encoding");
            const int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a < 0 || b < 0) return Status(Errc::Malformed, "bad percent-encoding");
            out.push_back(static_cast<char>(a * 16 + b));
            i += 2;
        } else if (plus_is_space && s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

Result<Target> parse_target(std::string_view target) {
    Target t;
    if (target.empty() || target[0] != '/') return Status(Errc::Malformed, "request target is not in origin-form");
    const auto frag = target.find('#');
    if (frag != std::string_view::npos) target = target.substr(0, frag);
    const auto q = target.find('?');
    const auto raw_path = target.substr(0, q);
    auto p = percent_decode(raw_path, false);
    if (!p) return p.take_error();
    t.path = std::move(p).value();
    // Segments are split before decoding so an encoded '/' stays inside its segment.
    std::size_t start = 1;
    while (start <= raw_path.size()) {
        auto end = raw_path.find('/', start);
        if (end == std::string_view::npos) end = raw_path.size();
        if (end > start) {
            auto seg = percent_decode(raw_path.substr(start, end - start), false);
            if (!seg) return seg.take_error();
            if (seg.value() == "." || seg.value() == "..") return Status(Errc::Malformed, "dot segment in path");
            t.segments.push_back(std::move(seg).value());
        }
        start = end + 1;
    }
    if (q != std::string_view::npos) {
        const auto qs = target.substr(q + 1);
        std::size_t s = 0;
        while (s <= qs.size()) {
            auto e = qs.find('&', s);
            if (e == std::string_view::npos) e = qs.size();
            const auto pair = qs.substr(s, e - s);
            if (!pair.empty()) {
                if (t.query.size() >= 256) return Status(Errc::LimitExceeded, "too many query parameters");
                const auto eq = pair.find('=');
                auto k = percent_decode(pair.substr(0, eq), true);
                auto v = percent_decode(eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1), true);
                if (!k || !v) return Status(Errc::Malformed, "bad percent-encoding in query");
                t.query.emplace_back(std::move(k).value(), std::move(v).value());
            }
            s = e + 1;
        }
    }
    return t;
}

Result<Url> parse_url(std::string_view url) {
    Url u;
    const auto sep = url.find("://");
    if (sep == std::string_view::npos) return Status(Errc::InvalidArgument, "URL has no scheme");
    u.scheme = to_lower(url.substr(0, sep));
    if (u.scheme != "http" && u.scheme != "https") return Status(Errc::Unsupported, "only http:// and https:// URLs are supported");
    if (u.scheme == "https") u.port = 443;
    auto rest = url.substr(sep + 3);
    const auto slash = rest.find('/');
    auto authority = rest.substr(0, slash);
    u.path = slash == std::string_view::npos ? std::string() : std::string(rest.substr(slash));
    while (!u.path.empty() && u.path.back() == '/') u.path.pop_back();
    if (authority.find('@') != std::string_view::npos) return Status(Errc::InvalidArgument, "credentials in URLs are not supported");
    std::string_view host = authority, port;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return Status(Errc::InvalidArgument, "bad IPv6 literal");
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return Status(Errc::InvalidArgument, "bad authority");
            port = authority.substr(close + 2);
        }
    } else if (const auto c = authority.rfind(':'); c != std::string_view::npos) {
        host = authority.substr(0, c);
        port = authority.substr(c + 1);
    }
    if (host.empty()) return Status(Errc::InvalidArgument, "URL has no host");
    u.host = std::string(host);
    if (!port.empty()) {
        std::uint64_t p = 0;
        if (!parse_decimal(port, p) || p == 0 || p > 65535) return Status(Errc::InvalidArgument, "bad port");
        u.port = static_cast<std::uint16_t>(p);
    }
    return u;
}

// ---------------------------------------------------------------- connection
Connection::Connection(std::unique_ptr<net::Transport> t, const Limits& limits) : t_(std::move(t)), lim_(limits) {}

void Connection::close() noexcept {
    if (t_) {
        t_->shutdown();
        t_->close();
    }
}

int Connection::timeout_now(int base) const {
    if (deadline_ms_ == 0) return base;
    const std::int64_t left = deadline_ms_ - now_ms();
    if (left <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(left, base));
}

Status Connection::fill(int timeout_ms) {
    if (!t_) return Status(Errc::NetworkError, "connection closed");
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 65536 && pos_ * 2 > buf_.size()) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    std::uint8_t tmp[16384];
    auto r = t_->read_some(tmp, sizeof tmp, timeout_ms);
    if (!r) return r.take_error();
    if (*r == 0) return Status(Errc::NetworkError, "connection closed");
    buf_.append(reinterpret_cast<const char*>(tmp), *r);
    return Status::ok();
}

// Reads one line ending in LF (CRLF or bare LF); the terminator is removed. A bare CR is an error.
Status Connection::read_line(std::size_t cap, std::string& line, int first_timeout_ms, bool& got_any) {
    line.clear();
    bool first = true;
    for (;;) {
        const auto nl = buf_.find('\n', pos_);
        if (nl != std::string::npos) {
            if (nl - pos_ > cap + 1) return Status(Errc::LimitExceeded, "line too long");
            std::string_view l(buf_.data() + pos_, nl - pos_);
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            if (l.size() > cap) return Status(Errc::LimitExceeded, "line too long");
            if (l.find('\r') != std::string_view::npos) return Status(Errc::Malformed, "bare CR in message");
            line.assign(l);
            pos_ = nl + 1;
            got_any = true;
            return Status::ok();
        }
        if (buf_.size() - pos_ > cap + 1) return Status(Errc::LimitExceeded, "line too long");
        // No line of the protocol carries C0 controls other than HTAB: fail fast on binary
        // input (e.g. a TLS handshake sent to a plain HTTP port) instead of waiting for a LF.
        for (std::size_t i = pos_; i < buf_.size(); ++i) {
            const auto b = static_cast<unsigned char>(buf_[i]);
            if ((b < 0x20 && b != '\t' && b != '\r') || b == 0x7F) return Status(Errc::Malformed, "control character in message");
        }
        if (buf_.size() > pos_) got_any = true;
        if (Status st = fill(timeout_now(first && !got_any ? first_timeout_ms : lim_.read_timeout_ms)); !st) {
            if (deadline_ms_ && now_ms() >= deadline_ms_ && st.code == Errc::Timeout)
                return Status(Errc::Timeout, "header section timed out");
            return st;
        }
        got_any = true;
        first = false;
    }
}

Status Connection::read_headers(Headers& h) {
    std::size_t total = 0;
    std::string line;
    for (;;) {
        bool any = true;
        const std::size_t cap = lim_.max_header_bytes > total ? lim_.max_header_bytes - total : 0;
        if (Status st = read_line(cap, line, lim_.read_timeout_ms, any); !st) {
            if (st.code == Errc::LimitExceeded) return Status(Errc::LimitExceeded, "header section too large");
            return st;
        }
        total += line.size() + 2;
        if (total > lim_.max_header_bytes) return Status(Errc::LimitExceeded, "header section too large");
        if (line.empty()) return Status::ok();
        if (is_ows(line[0])) return Status(Errc::Malformed, "obsolete line folding");
        if (h.size() >= lim_.max_headers) return Status(Errc::LimitExceeded, "too many header fields");
        const auto colon = line.find(':');
        if (colon == std::string::npos) return Status(Errc::Malformed, "header field without ':'");
        const std::string_view name(line.data(), colon);
        if (!is_token(name)) return Status(Errc::Malformed, "invalid header field name");
        const auto value = trim_ows(std::string_view(line).substr(colon + 1));
        for (char c : value)
            if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7F)
                return Status(Errc::Malformed, "control character in header field value");
        h.push_back({std::string(name), std::string(value)});
    }
}

Status Connection::read_exact(std::uint64_t n, std::string& out) {
    if (n > lim_.max_body) return Status(Errc::LimitExceeded, "body too large");
    if (n > SIZE_MAX / 2) return Status(Errc::LimitExceeded, "body too large");
    const auto want = static_cast<std::size_t>(n);
    if (want - std::min(want, out.size()) > 0 && out.capacity() < out.size() + want) {
        // Reserve gradually so a lying Content-Length cannot make us allocate up front.
        out.reserve(out.size() + std::min<std::size_t>(want, 16u << 20));
    }
    std::size_t left = want;
    while (left > 0) {
        if (pos_ < buf_.size()) {
            const std::size_t take = std::min(left, buf_.size() - pos_);
            out.append(buf_, pos_, take);
            pos_ += take;
            left -= take;
            continue;
        }
        if (Status st = fill(lim_.read_timeout_ms); !st) {
            if (st.message == "connection closed") return Status(Errc::Truncated, "connection closed inside the body");
            return st;
        }
    }
    return Status::ok();
}

Status Connection::read_chunked(std::string& body) {
    std::string line;
    for (;;) {
        bool any = true;
        if (Status st = read_line(lim_.max_chunk_line, line, lim_.read_timeout_ms, any); !st) return st;
        std::size_t i = 0;
        std::uint64_t size = 0;
        while (i < line.size()) {
            const char c = line[i];
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) break;
            if (i >= 15) return Status(Errc::LimitExceeded, "chunk size too large");
            size = size * 16 + static_cast<std::uint64_t>(d);
            ++i;
        }
        if (i == 0) return Status(Errc::Malformed, "invalid chunk size");
        const auto rest = trim_ows(std::string_view(line).substr(i));
        if (!rest.empty() && rest[0] != ';') return Status(Errc::Malformed, "invalid chunk size");
        if (size == 0) break;
        std::uint64_t total = 0;
        if (!checked_add(body.size(), size, total) || total > lim_.max_body)
            return Status(Errc::LimitExceeded, "body too large");
        if (Status st = read_exact(size, body); !st) return st;
        if (Status st = read_line(lim_.max_chunk_line, line, lim_.read_timeout_ms, any); !st) return st;
        if (!line.empty()) return Status(Errc::Malformed, "missing CRLF after chunk data");
    }
    // Trailer section: parsed with the header limits, then discarded.
    Headers trailers;
    return read_headers(trailers);
}

Status Connection::read_to_close(std::string& body) {
    for (;;) {
        if (pos_ < buf_.size()) {
            const std::size_t take = buf_.size() - pos_;
            std::uint64_t total = 0;
            if (!checked_add(body.size(), take, total) || total > lim_.max_body)
                return Status(Errc::LimitExceeded, "body too large");
            body.append(buf_, pos_, take);
            pos_ = buf_.size();
        }
        Status st = fill(lim_.read_timeout_ms);
        if (!st) {
            if (st.message == "connection closed") return Status::ok();
            return st;
        }
    }
}

namespace {

// Message body framing shared by requests and responses (RFC 9112 6.3).
enum class Framing { None, Length, Chunked, Close };

Status framing_of(const Headers& h, bool is_request, Framing& f, std::uint64_t& length, int& status_hint) {
    const auto te = header_list(h, "Transfer-Encoding");
    bool has_cl = false;
    std::uint64_t cl = 0;
    for (const auto& fld : h) {
        if (!iequals(fld.name, "Content-Length")) continue;
        // A list of identical values is allowed (RFC 9110 8.6); anything else is an error.
        std::string_view v = fld.value;
        std::size_t s = 0;
        while (s <= v.size()) {
            auto e = v.find(',', s);
            if (e == std::string_view::npos) e = v.size();
            std::uint64_t one = 0;
            if (!parse_decimal(trim_ows(v.substr(s, e - s)), one)) { status_hint = 400; return Status(Errc::Malformed, "invalid Content-Length"); }
            if (has_cl && one != cl) { status_hint = 400; return Status(Errc::Malformed, "conflicting Content-Length values"); }
            has_cl = true;
            cl = one;
            s = e + 1;
        }
    }
    if (!te.empty()) {
        if (has_cl) { status_hint = 400; return Status(Errc::Malformed, "both Transfer-Encoding and Content-Length"); }
        if (te.size() != 1 || !iequals(te.back(), "chunked")) {
            status_hint = 501;
            return Status(Errc::Unsupported, "unsupported transfer coding");
        }
        f = Framing::Chunked;
        return Status::ok();
    }
    if (has_cl) {
        f = Framing::Length;
        length = cl;
        return Status::ok();
    }
    f = is_request ? Framing::None : Framing::Close;
    return Status::ok();
}

}  // namespace

Result<Request> Connection::read_request(int first_byte_timeout_ms) {
    suggested_ = 0;
    Request rq;
    std::string line;
    bool got_any = false;
    deadline_ms_ = 0;
    // Ignore empty lines before the request-line (RFC 9112 2.2); at most a few.
    for (int empties = 0;; ++empties) {
        Status st = read_line(lim_.max_request_line, line, first_byte_timeout_ms, got_any);
        if (!st) {
            if (st.code == Errc::LimitExceeded) suggested_ = 414;
            else if (got_any && st.code == Errc::Malformed) suggested_ = 400;
            else if (got_any && st.code == Errc::Timeout) suggested_ = 408;
            return st;
        }
        if (deadline_ms_ == 0 && lim_.header_timeout_ms > 0) deadline_ms_ = now_ms() + lim_.header_timeout_ms;
        if (!line.empty()) break;
        if (empties >= 4) { suggested_ = 400; return Status(Errc::Malformed, "empty lines instead of a request"); }
    }
    if (deadline_ms_ == 0 && lim_.header_timeout_ms > 0) deadline_ms_ = now_ms() + lim_.header_timeout_ms;
    // request-line = method SP request-target SP HTTP-version
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || line.find(' ', sp2 + 1) != std::string::npos) {
        suggested_ = 400;
        deadline_ms_ = 0;
        return Status(Errc::Malformed, "invalid request line");
    }
    rq.method = line.substr(0, sp1);
    rq.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = line.substr(sp2 + 1);
    if (!is_token(rq.method) || rq.target.empty()) {
        suggested_ = 400;
        deadline_ms_ = 0;
        return Status(Errc::Malformed, "invalid request line");
    }
    for (char c : rq.target)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
            suggested_ = 400;
            deadline_ms_ = 0;
            return Status(Errc::Malformed, "invalid request target");
        }
    if (version.size() != 8 || version.compare(0, 5, "HTTP/") != 0 || version[6] != '.' || version[5] < '0' ||
        version[5] > '9' || version[7] < '0' || version[7] > '9') {
        suggested_ = 400;
        deadline_ms_ = 0;
        return Status(Errc::Malformed, "invalid HTTP version");
    }
    if (version[5] != '1') {
        suggested_ = 505;
        deadline_ms_ = 0;
        return Status(Errc::Unsupported, "HTTP version not supported");
    }
    rq.version_minor = version[7] - '0';
    Status hs = read_headers(rq.headers);
    deadline_ms_ = 0;
    if (!hs) {
        suggested_ = hs.code == Errc::LimitExceeded ? 431 : hs.code == Errc::Timeout ? 408 : hs.code == Errc::Malformed ? 400 : 0;
        return hs;
    }
    if (rq.version_minor >= 1) {
        const std::string* host = find_header(rq.headers, "Host");
        int hosts = 0;
        for (const auto& f : rq.headers) hosts += iequals(f.name, "Host") ? 1 : 0;
        if (!host || hosts != 1) {
            suggested_ = 400;
            return Status(Errc::Malformed, "missing or repeated Host header");
        }
    }
    Framing f = Framing::None;
    std::uint64_t len = 0;
    int hint = 400;
    if (Status st = framing_of(rq.headers, true, f, len, hint); !st) {
        suggested_ = hint;
        return st;
    }
    if (f == Framing::Length && len > lim_.max_body) {
        suggested_ = 413;
        return Status(Errc::LimitExceeded, "body too large");
    }
    if (f != Framing::None && !(f == Framing::Length && len == 0)) {
        if (const std::string* ex = find_header(rq.headers, "Expect")) {
            if (!iequals(*ex, "100-continue")) {
                suggested_ = 417;
                return Status(Errc::Unsupported, "unsupported expectation");
            }
            if (rq.version_minor >= 1) {
                if (Status st = write_raw("HTTP/1.1 100 Continue\r\n\r\n"); !st) return st;
            }
        }
    }
    Status bs = Status::ok();
    if (f == Framing::Length) bs = read_exact(len, rq.body);
    else if (f == Framing::Chunked) bs = read_chunked(rq.body);
    if (!bs) {
        suggested_ = bs.code == Errc::LimitExceeded ? 413 : bs.code == Errc::Malformed ? 400 : bs.code == Errc::Timeout ? 408 : 0;
        return bs;
    }
    return rq;
}

Result<Response> Connection::read_response(std::string_view request_method) {
    for (int interim = 0;; ++interim) {
        Response rs;
        std::string line;
        bool got_any = false;
        deadline_ms_ = 0;
        if (Status st = read_line(lim_.max_request_line, line, lim_.read_timeout_ms, got_any); !st) return st;
        if (lim_.header_timeout_ms > 0) deadline_ms_ = now_ms() + lim_.header_timeout_ms;
        // status-line = HTTP-version SP status-code SP [ reason-phrase ]
        if (line.size() < 12 || line.compare(0, 7, "HTTP/1.") != 0 || line[7] < '0' || line[7] > '9' || line[8] != ' ' ||
            line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9' ||
            (line.size() > 12 && line[12] != ' ')) {
            deadline_ms_ = 0;
            return Status(Errc::Malformed, "invalid status line");
        }
        rs.version_minor = line[7] - '0';
        rs.status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        rs.reason = line.size() > 13 ? line.substr(13) : std::string();
        Status hs = read_headers(rs.headers);
        deadline_ms_ = 0;
        if (!hs) return hs;
        if (rs.status >= 100 && rs.status < 200) {
            if (interim >= 8) return Status(Errc::Malformed, "too many interim responses");
            continue;  // 100 Continue and other interim responses are skipped
        }
        if (iequals(request_method, "HEAD") || rs.status == 204 || rs.status == 304) return rs;
        Framing f = Framing::None;
        std::uint64_t len = 0;
        int hint = 0;
        if (Status st = framing_of(rs.headers, false, f, len, hint); !st) return st;
        Status bs = Status::ok();
        if (f == Framing::Length) bs = read_exact(len, rs.body);
        else if (f == Framing::Chunked) bs = read_chunked(rs.body);
        else if (f == Framing::Close) bs = read_to_close(rs.body);
        if (!bs) return bs;
        if (f == Framing::Close) close();
        return rs;
    }
}

Status append_headers(std::string& out, const Headers& h) {
    for (const auto& f : h) {
        if (!is_token(f.name)) return Status(Errc::InvalidArgument, "invalid header field name");
        for (char c : f.value)
            if (c == '\r' || c == '\n' || c == '\0') return Status(Errc::InvalidArgument, "invalid header field value");
        out += f.name;
        out += ": ";
        out += f.value;
        out += "\r\n";
    }
    return Status::ok();
}

Status Connection::write_raw(std::string_view data) {
    if (!t_) return Status(Errc::NetworkError, "connection closed");
    return t_->write_all(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), lim_.write_timeout_ms);
}

Status Connection::write_request(const Request& rq) {
    if (!is_token(rq.method)) return Status(Errc::InvalidArgument, "invalid method");
    for (char c : rq.target)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return Status(Errc::InvalidArgument, "invalid request target");
    std::string head = rq.method + " " + rq.target + " HTTP/1.1\r\n";
    if (Status st = append_headers(head, rq.headers); !st) return st;
    const bool body_allowed = !rq.body.empty() || rq.method == "POST" || rq.method == "PUT";
    if (body_allowed && !find_header(rq.headers, "Content-Length"))
        head += "Content-Length: " + std::to_string(rq.body.size()) + "\r\n";
    head += "\r\n";
    if (rq.body.size() < 65536) {
        head += rq.body;
        return write_raw(head);
    }
    if (Status st = write_raw(head); !st) return st;
    return write_raw(rq.body);
}

Status Connection::write_response(const Response& rs, bool head, bool close_after) {
    if (rs.status < 100 || rs.status > 599) return Status(Errc::InvalidArgument, "invalid status code");
    std::string reason = rs.reason.empty() ? std::string(reason_phrase(rs.status)) : rs.reason;
    for (char& c : reason)
        if (c == '\r' || c == '\n' || c == '\0') c = ' ';
    std::string out = "HTTP/1.1 " + std::to_string(rs.status) + " " + reason + "\r\n";
    if (Status st = append_headers(out, rs.headers); !st) return st;
    const bool no_body = rs.status == 204 || rs.status == 304 || rs.status < 200;
    if (!no_body && !find_header(rs.headers, "Content-Length"))
        out += "Content-Length: " + std::to_string(rs.body.size()) + "\r\n";
    if (close_after) out += "Connection: close\r\n";
    out += "\r\n";
    if (head || no_body) return write_raw(out);
    if (rs.body.size() < 65536) {
        out += rs.body;
        return write_raw(out);
    }
    if (Status st = write_raw(out); !st) return st;
    return write_raw(rs.body);
}

bool wants_close(const Headers& h, int version_minor) noexcept {
    bool close = version_minor == 0;
    for (const auto& f : h) {
        if (!iequals(f.name, "Connection")) continue;
        std::string_view v = f.value;
        std::size_t s = 0;
        while (s <= v.size()) {
            auto e = v.find(',', s);
            if (e == std::string_view::npos) e = v.size();
            const auto tok = trim_ows(v.substr(s, e - s));
            if (iequals(tok, "close")) return true;
            if (iequals(tok, "keep-alive")) close = false;
            s = e + 1;
        }
    }
    return close;
}

// ---------------------------------------------------------------- client
Client::Client(std::string host, std::uint16_t port, const Limits& limits, int connect_timeout_ms)
    : host_(std::move(host)), port_(port), lim_(limits), connect_timeout_ms_(connect_timeout_ms) {}

void Client::close() noexcept {
    if (conn_) conn_->close();
    conn_.reset();
}

Result<Response> Client::attempt(const Request& rq, bool& reused, bool& got_bytes) {
    reused = conn_ && conn_->is_open();
    got_bytes = false;
    if (!reused) {
        auto t = connector ? connector(host_, port_, connect_timeout_ms_) : net::tcp_connect(host_, port_, connect_timeout_ms_);
        if (!t) return t.take_error();
        conn_ = std::make_unique<Connection>(std::move(t).value(), lim_);
    }
    Request full = rq;
    if (!find_header(full.headers, "Host")) {
        std::string h = host_.find(':') != std::string::npos ? "[" + host_ + "]" : host_;
        if (port_ != default_port) h += ":" + std::to_string(port_);
        full.headers.insert(full.headers.begin(), {"Host", h});
    }
    if (Status st = conn_->write_request(full); !st) {
        close();
        return st;
    }
    auto rs = conn_->read_response(rq.method);
    if (!rs) {
        // A stale keep-alive connection fails before any response byte; tell the caller.
        got_bytes = rs.error().code != Errc::NetworkError;
        close();
        return rs.take_error();
    }
    if (wants_close(rs->headers, rs->version_minor) || !conn_->is_open()) close();
    return rs;
}

Result<Response> Client::send(const Request& rq) {
    bool reused = false, got_bytes = false;
    auto r = attempt(rq, reused, got_bytes);
    if (!r && reused && !got_bytes) return attempt(rq, reused, got_bytes);
    return r;
}

// ---------------------------------------------------------------- server
Server::~Server() { stop(); }

Status Server::start(const ServerConfig& cfg, Handler handler) {
    if (accept_thread_.joinable()) return Status(Errc::InvalidArgument, "server already started");
    if (!handler) return Status(Errc::InvalidArgument, "handler must be set");
    cfg_ = cfg;
    handler_ = std::move(handler);
    stopping_ = false;
    if (cfg_.tls.enabled) {
        net::TlsSettings ts = cfg_.tls;
        if (!ts.log && cfg_.log) ts.log = cfg_.log;
        auto ctx = net::TlsContext::create(ts, net::TlsContext::Role::Server);
        net::tls_wipe_secret(ts.identity.password);
        net::tls_wipe_secret(cfg_.tls.identity.password);  // the context keeps (and later wipes) its own copy
        if (!ctx) return ctx.take_error();
        if (Status st = tls_listener_.listen(std::move(ctx).value(), cfg_.bind_address, cfg_.port); !st) return st;
    } else if (Status st = listener_.listen(cfg_.bind_address, cfg_.port); !st) {
        return st;
    }
    accept_thread_ = std::thread([this] { accept_loop(); });
    return Status::ok();
}

void Server::reap(bool all) {
    std::list<Worker> done;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (all || it->done.load()) {
                auto next = std::next(it);
                done.splice(done.end(), workers_, it);
                it = next;
            } else {
                ++it;
            }
        }
    }
    for (auto& w : done)
        if (w.thread.joinable()) w.thread.join();
}

void Server::stop() noexcept {
    stopping_ = true;
    if (accept_thread_.joinable()) accept_thread_.join();
    listener_.close();
    tls_listener_.close();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& w : workers_)
            if (w.transport) w.transport->shutdown();
    }
    try {
        reap(true);
    } catch (...) {
    }
}

void Server::accept_loop() {
    while (!stopping_.load()) {
        Result<std::unique_ptr<net::Transport>> t = Status(Errc::Timeout, "");
        if (cfg_.tls.enabled) {
            auto s = tls_listener_.accept(200);
            if (s) t = std::unique_ptr<net::Transport>(std::move(s).value());
            else t = s.take_error();
        } else {
            t = listener_.accept(200);
        }
        if (!t) {
            if (t.error().code == Errc::Timeout) {
                reap(false);
                continue;
            }
            if (stopping_.load()) break;
            if (cfg_.log) cfg_.log("http: accept failed: " + t.error().message);
            continue;
        }
        reap(false);
        std::size_t active = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            active = workers_.size();
        }
        if (active >= cfg_.max_connections && cfg_.tls.enabled) {
            // A 503 would need a TLS handshake on the accept thread; just close.
            t.value()->close();
            if (cfg_.log) cfg_.log("http: connection refused (busy)");
            continue;
        }
        if (active >= cfg_.max_connections) {
            Connection c(std::move(t).value(), cfg_.limits);
            Response rs;
            rs.status = 503;
            rs.set("Content-Type", "text/plain");
            rs.set("Retry-After", "1");
            rs.body = "server busy\n";
            (void)c.write_response(rs, false, true);
            c.close();
            if (cfg_.log) cfg_.log("http: connection refused (busy)");
            continue;
        }
        std::lock_guard<std::mutex> lk(mu_);
        workers_.emplace_back();
        Worker* w = &workers_.back();
        net::Transport* raw = t->get();
        w->transport = raw;
        auto owned = std::make_shared<std::unique_ptr<net::Transport>>(std::move(t).value());
        w->thread = std::thread([this, w, owned]() mutable {
            if (cfg_.tls.enabled) {
                // Handshake on this thread; stop() shuts the socket down through w->transport.
                Status hs = static_cast<net::TlsTransport*>(owned->get())->handshake(cfg_.tls.handshake_timeout_ms);
                if (!hs) {
                    if (cfg_.log) cfg_.log("http: " + hs.message);
                    {
                        std::lock_guard<std::mutex> lk2(mu_);
                        w->transport = nullptr;
                    }
                    owned->reset();
                    w->done = true;
                    return;
                }
            }
            const std::string peer = (*owned)->peer_address();
            Connection c(std::move(*owned), cfg_.limits);
            serve(c, peer);
            {
                std::lock_guard<std::mutex> lk2(mu_);
                w->transport = nullptr;  // stop() no longer touches it
            }
            c.close();
            w->done = true;
        });
    }
}

void Server::serve_connection(std::unique_ptr<net::Transport> t) {
    const std::string peer = t->peer_address();
    Connection c(std::move(t), cfg_.limits);
    serve(c, peer);
    c.close();
}

void Server::serve(Connection& c, const std::string& peer) {
    for (std::uint32_t n = 0; n < cfg_.max_requests_per_connection && !stopping_.load(); ++n) {
        auto rq = c.read_request(n == 0 ? cfg_.limits.read_timeout_ms : cfg_.idle_timeout_ms);
        if (!rq) {
            const int st = c.suggested_status();
            if (st != 0) {
                Response rs;
                rs.status = st;
                rs.set("Content-Type", "text/plain");
                rs.body = std::string(reason_phrase(st)) + "\n";  // fixed text: never echo input
                (void)c.write_response(rs, false, true);
                if (cfg_.log) cfg_.log("http: " + peer + " rejected request: " + std::to_string(st));
            }
            break;
        }
        Response rs;
        try {
            handler_(*rq, rs);
        } catch (...) {
            rs = Response{};
            rs.status = 500;
            rs.set("Content-Type", "text/plain");
            rs.body = "internal error\n";
        }
        const bool close_after = wants_close(rq->headers, rq->version_minor) || wants_close(rs.headers, 1) ||
                                 n + 1 >= cfg_.max_requests_per_connection || stopping_.load();
        if (Status st = c.write_response(rs, iequals(rq->method, "HEAD"), close_after); !st) break;
        if (cfg_.log) cfg_.log("http: " + peer + " " + rq->method + " -> " + std::to_string(rs.status));
        if (close_after) break;
    }
}

}  // namespace siaara::web::http
