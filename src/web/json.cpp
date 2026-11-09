// SIAARA DICOM Toolkit - minimal JSON (RFC 8259) parser and writer, written from the RFC.
#include "web/json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace siaara::web::json {

const Value* Value::get(std::string_view key) const noexcept {
    if (type != Type::Object) return nullptr;
    for (const auto& m : object)
        if (m.first == key) return &m.second;
    return nullptr;
}

// ---------------------------------------------------------------- UTF-8 helpers
namespace {

// Decodes one UTF-8 sequence at s[i]. Returns the length (1..4) and the code point, or 0 if invalid.
std::size_t utf8_decode(std::string_view s, std::size_t i, std::uint32_t& cp) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { cp = b0; return 1; }
    std::size_t len = 0;
    std::uint32_t min = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; cp = b0 & 0x1Fu; min = 0x80; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { len = 3; cp = b0 & 0x0Fu; min = 0x800; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { len = 4; cp = b0 & 0x07u; min = 0x10000; }
    else return 0;
    if (s.size() - i < len) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

}  // namespace

bool is_valid_utf8(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = 0;
        const std::size_t n = utf8_decode(s, i, cp);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

void append_code_point(std::string& out, std::uint32_t cp) {
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

void append_utf8_sanitized(std::string& out, std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = 0;
        const std::size_t n = utf8_decode(s, i, cp);
        if (n == 0) {
            append_code_point(out, 0xFFFD);
            ++i;
        } else {
            out.append(s.data() + i, n);
            i += n;
        }
    }
}

// ---------------------------------------------------------------- writer
void write_string(std::string& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            std::uint32_t cp = 0;
            const std::size_t n = utf8_decode(s, i, cp);
            if (n == 0) {
                append_code_point(out, 0xFFFD);
                ++i;
            } else {
                out.append(s.data() + i, n);
                i += n;
            }
            continue;
        }
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out.append("\\u00");
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 15]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
        ++i;
    }
    out.push_back('"');
}

void write(std::string& out, const Value& v) {
    switch (v.type) {
        case Type::Null: out.append("null"); break;
        case Type::Bool: out.append(v.boolean ? "true" : "false"); break;
        case Type::Number: out.append(is_number_token(v.text) ? v.text : std::string("null")); break;
        case Type::String: write_string(out, v.text); break;
        case Type::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < v.array.size(); ++i) {
                if (i) out.push_back(',');
                write(out, v.array[i]);
            }
            out.push_back(']');
            break;
        case Type::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < v.object.size(); ++i) {
                if (i) out.push_back(',');
                write_string(out, v.object[i].first);
                out.push_back(':');
                write(out, v.object[i].second);
            }
            out.push_back('}');
            break;
    }
}

// ---------------------------------------------------------------- number grammar
bool is_number_token(std::string_view s) noexcept {
    std::size_t i = 0;
    const std::size_t n = s.size();
    if (i < n && s[i] == '-') ++i;
    if (i >= n) return false;
    if (s[i] == '0') {
        ++i;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
    } else {
        return false;
    }
    if (i < n && s[i] == '.') {
        ++i;
        const std::size_t start = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start) return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        const std::size_t start = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start) return false;
    }
    return i == n;
}

// ---------------------------------------------------------------- parser
namespace {

class Parser {
public:
    Parser(std::string_view t, const ParseLimits& l) : s_(t), lim_(l) {}

    Status run(Value& out) {
        skip_ws();
        if (Status st = value(out, 0); !st) return st;
        skip_ws();
        if (pos_ != s_.size()) return err("unexpected data after the JSON value");
        return Status::ok();
    }

private:
    std::string_view s_;
    const ParseLimits& lim_;
    std::size_t pos_ = 0;
    std::uint64_t count_ = 0;

    Status err(const char* m) const { return Status(Errc::Malformed, std::string("json: ") + m, pos_); }

    void skip_ws() noexcept {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool literal(std::string_view w) noexcept {
        if (s_.substr(pos_, w.size()) != w) return false;
        pos_ += w.size();
        return true;
    }

    Status value(Value& v, std::uint32_t depth) {
        if (++count_ > lim_.max_values) return Status(Errc::LimitExceeded, "json: too many values", pos_);
        if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unexpected end of input", pos_);
        const char c = s_[pos_];
        switch (c) {
            case '{': return object(v, depth + 1);
            case '[': return array(v, depth + 1);
            case '"': v.type = Type::String; return string(v.text);
            case 't': if (literal("true")) { v.type = Type::Bool; v.boolean = true; return Status::ok(); } break;
            case 'f': if (literal("false")) { v.type = Type::Bool; v.boolean = false; return Status::ok(); } break;
            case 'n': if (literal("null")) { v.type = Type::Null; return Status::ok(); } break;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        }
        return err("invalid value");
    }

    Status number(Value& v) {
        const std::size_t start = pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++pos_;
            else break;
        }
        const auto tok = s_.substr(start, pos_ - start);
        if (tok.size() > lim_.max_number_length) return Status(Errc::LimitExceeded, "json: number too long", start);
        if (!is_number_token(tok)) { pos_ = start; return err("invalid number"); }
        v.type = Type::Number;
        v.text.assign(tok);
        return Status::ok();
    }

    static int hex(char c) noexcept {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(std::uint32_t& out) noexcept {
        if (s_.size() - pos_ < 4) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hex(s_[pos_ + static_cast<std::size_t>(k)]);
            if (h < 0) return false;
            out = (out << 4) | static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }

    Status string(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        for (;;) {
            if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unterminated string", pos_);
            if (out.size() > lim_.max_string) return Status(Errc::LimitExceeded, "json: string too long", pos_);
            // Fast path: copy a run of plain ASCII.
            std::size_t run = pos_;
            while (run < s_.size()) {
                const auto b = static_cast<unsigned char>(s_[run]);
                if (b == '"' || b == '\\' || b < 0x20 || b >= 0x80) break;
                ++run;
            }
            if (run > pos_) {
                out.append(s_.data() + pos_, run - pos_);
                pos_ = run;
                continue;
            }
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                if (out.size() > lim_.max_string) return Status(Errc::LimitExceeded, "json: string too long", pos_);
                return Status::ok();
            }
            if (c < 0x20) return err("control character in string");
            if (c >= 0x80) {
                std::uint32_t cp = 0;
                const std::size_t n = utf8_decode(s_, pos_, cp);
                if (n == 0) return err("invalid UTF-8 in string");
                out.append(s_.data() + pos_, n);
                pos_ += n;
                continue;
            }
            // Escape.
            ++pos_;
            if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unterminated escape", pos_);
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return err("invalid \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t lo = 0;
                        if (s_.size() - pos_ < 2 || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                            return err("lone high surrogate");
                        pos_ += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return err("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return err("lone low surrogate");
                    }
                    append_code_point(out, cp);
                    break;
                }
                default: return err("invalid escape");
            }
        }
    }

    Status array(Value& v, std::uint32_t depth) {
        if (depth > lim_.max_depth) return Status(Errc::LimitExceeded, "json: nesting too deep", pos_);
        v.type = Type::Array;
        ++pos_;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return Status::ok(); }
        for (;;) {
            skip_ws();
            v.array.emplace_back();
            if (Status st = value(v.array.back(), depth); !st) return st;
            skip_ws();
            if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unterminated array", pos_);
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return Status::ok(); }
            return err("expected ',' or ']'");
        }
    }

    Status object(Value& v, std::uint32_t depth) {
        if (depth > lim_.max_depth) return Status(Errc::LimitExceeded, "json: nesting too deep", pos_);
        v.type = Type::Object;
        ++pos_;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return Status::ok(); }
        for (;;) {
            skip_ws();
            if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unterminated object", pos_);
            if (s_[pos_] != '"') return err("expected a member name");
            v.object.emplace_back();
            auto& m = v.object.back();
            if (Status st = string(m.first); !st) return st;
            skip_ws();
            if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unterminated object", pos_);
            if (s_[pos_] != ':') return err("expected ':'");
            ++pos_;
            skip_ws();
            if (Status st = value(m.second, depth); !st) return st;
            skip_ws();
            if (pos_ >= s_.size()) return Status(Errc::Truncated, "json: unterminated object", pos_);
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return Status::ok(); }
            return err("expected ',' or '}'");
        }
    }
};

}  // namespace

Result<Value> parse(std::string_view text, const ParseLimits& limits) {
    if (static_cast<std::uint64_t>(text.size()) > limits.max_input)
        return Status(Errc::LimitExceeded, "json: document too large");
    Value v;
    Parser p(text, limits);
    if (Status st = p.run(v); !st) return st;
    return v;
}

}  // namespace siaara::web::json
