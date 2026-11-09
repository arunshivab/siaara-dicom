// SIAARA DICOM Toolkit - minimal XML 1.0 parser and writer (see xml.hpp for the supported subset).
#include "web/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "web/json.hpp"

namespace siaara::web::xml {

const std::string* Node::attr(std::string_view n) const noexcept {
    for (const auto& a : attrs)
        if (a.first == n) return &a.second;
    return nullptr;
}

const Node* Node::child(std::string_view n) const noexcept {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

namespace {

bool is_xml_char(std::uint32_t cp) noexcept {
    return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
           (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

bool name_start(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80;
}
bool name_char(unsigned char c) noexcept {
    return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}
bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

class Parser {
public:
    Parser(std::string_view s, const ParseLimits& l) : s_(s), lim_(l) {}

    Status run(Node& root) {
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") pos_ = 3;
        if (!json::is_valid_utf8(s_)) return Status(Errc::Malformed, "xml: invalid UTF-8");
        if (Status st = misc(true); !st) return st;
        if (pos_ >= s_.size() || s_[pos_] != '<') return err("expected the root element");
        if (Status st = element(root, 1); !st) return st;
        if (Status st = misc(false); !st) return st;
        if (pos_ != s_.size()) return err("content after the root element");
        return Status::ok();
    }

private:
    std::string_view s_;
    const ParseLimits& lim_;
    std::size_t pos_ = 0;
    std::uint64_t nodes_ = 0;

    Status err(const char* m) const { return Status(Errc::Malformed, std::string("xml: ") + m, pos_); }
    Status trunc() const { return Status(Errc::Truncated, "xml: unexpected end of input", pos_); }
    bool starts(std::string_view w) const noexcept { return s_.substr(pos_, w.size()) == w; }
    void skip_ws() noexcept { while (pos_ < s_.size() && is_space(s_[pos_])) ++pos_; }

    // Comments, PIs and whitespace outside the root element.
    Status misc(bool prolog) {
        bool first = true;
        for (;;) {
            skip_ws();
            if (starts("<?")) {
                if (Status st = pi(prolog && first); !st) return st;
            } else if (starts("<!--")) {
                if (Status st = comment(); !st) return st;
            } else if (starts("<!")) {
                return err("DOCTYPE and other declarations are not supported");
            } else {
                return Status::ok();
            }
            first = false;
        }
    }

    Status pi(bool allow_decl) {
        const std::size_t start = pos_;
        pos_ += 2;
        const auto end = s_.find("?>", pos_);
        if (end == std::string_view::npos) return trunc();
        const auto target = s_.substr(pos_, end - pos_);
        if (target.size() >= 3 && (target[0] | 0x20) == 'x' && (target[1] | 0x20) == 'm' &&
            (target[2] | 0x20) == 'l' && (target.size() == 3 || is_space(target[3]))) {
            if (!allow_decl || start != (s_.substr(0, 3) == "\xEF\xBB\xBF" ? 3u : 0u))
                return err("misplaced XML declaration");
            // Only UTF-8 (or no encoding declaration) is accepted.
            const auto enc = target.find("encoding");
            if (enc != std::string_view::npos) {
                auto rest = target.substr(enc + 8);
                std::size_t i = 0;
                while (i < rest.size() && (is_space(rest[i]) || rest[i] == '=')) ++i;
                if (i < rest.size() && (rest[i] == '"' || rest[i] == '\'')) {
                    const char q = rest[i];
                    const auto close = rest.find(q, i + 1);
                    if (close == std::string_view::npos) return err("bad XML declaration");
                    std::string e(rest.substr(i + 1, close - i - 1));
                    for (char& c : e) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    if (e != "UTF-8" && e != "UTF8" && e != "US-ASCII")
                        return Status(Errc::Unsupported, "xml: only UTF-8 documents are supported");
                }
            }
        }
        pos_ = end + 2;
        return Status::ok();
    }

    Status comment() {
        pos_ += 4;
        const auto end = s_.find("--", pos_);
        if (end == std::string_view::npos) return trunc();
        if (end + 2 >= s_.size()) return trunc();
        if (s_[end + 2] != '>') return err("'--' inside a comment");
        pos_ = end + 3;
        return Status::ok();
    }

    Status name(std::string& out) {
        const std::size_t start = pos_;
        if (pos_ >= s_.size()) return trunc();
        if (!name_start(static_cast<unsigned char>(s_[pos_]))) return err("invalid name");
        while (pos_ < s_.size() && name_char(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        if (pos_ - start > lim_.max_name_length) return Status(Errc::LimitExceeded, "xml: name too long", start);
        out.assign(s_.substr(start, pos_ - start));
        return Status::ok();
    }

    // Decodes a reference at s_[pos_] == '&' and appends it.
    Status reference(std::string& out) {
        const auto semi = s_.find(';', pos_);
        if (semi == std::string_view::npos) return trunc();
        if (semi - pos_ > 12) return err("invalid entity reference");
        const auto ref = s_.substr(pos_ + 1, semi - pos_ - 1);
        if (ref == "lt") out.push_back('<');
        else if (ref == "gt") out.push_back('>');
        else if (ref == "amp") out.push_back('&');
        else if (ref == "quot") out.push_back('"');
        else if (ref == "apos") out.push_back('\'');
        else if (ref.size() >= 2 && ref[0] == '#') {
            std::uint32_t cp = 0;
            const bool hexa = ref[1] == 'x';
            const auto digits = ref.substr(hexa ? 2 : 1);
            if (digits.empty()) return err("invalid character reference");
            for (char c : digits) {
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hexa && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hexa && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0) return err("invalid character reference");
                cp = cp * (hexa ? 16u : 10u) + static_cast<std::uint32_t>(d);
                if (cp > 0x10FFFF) return err("invalid character reference");
            }
            if (!is_xml_char(cp)) return err("character reference to a non-XML character");
            json::append_code_point(out, cp);
        } else {
            return err("unknown entity (only predefined entities are supported)");
        }
        pos_ = semi + 1;
        return Status::ok();
    }

    // Validates XML Char at pos_ and appends one (possibly multi-byte) character.
    Status char_data(std::string& out) {
        const auto c = static_cast<unsigned char>(s_[pos_]);
        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return err("control character");
            if (c == '\r') {  // end-of-line normalisation (XML 1.0 section 2.11)
                out.push_back('\n');
                ++pos_;
                if (pos_ < s_.size() && s_[pos_] == '\n') ++pos_;
                return Status::ok();
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
            return Status::ok();
        }
        // Valid UTF-8 was checked up front; check the XML Char range (U+FFFE/U+FFFF).
        std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        if (len == 3 && c == 0xEF && pos_ + 2 < s_.size() && static_cast<unsigned char>(s_[pos_ + 1]) == 0xBF &&
            (static_cast<unsigned char>(s_[pos_ + 2]) == 0xBE || static_cast<unsigned char>(s_[pos_ + 2]) == 0xBF))
            return err("non-character");
        out.append(s_.data() + pos_, len);
        pos_ += len;
        return Status::ok();
    }

    Status attribute_value(std::string& out) {
        if (pos_ >= s_.size()) return trunc();
        const char q = s_[pos_];
        if (q != '"' && q != '\'') return err("expected a quoted attribute value");
        ++pos_;
        for (;;) {
            if (pos_ >= s_.size()) return trunc();
            const char c = s_[pos_];
            if (c == q) { ++pos_; return Status::ok(); }
            if (c == '<') return err("'<' in attribute value");
            if (c == '&') {
                if (Status st = reference(out); !st) return st;
                continue;
            }
            if (c == '\t' || c == '\n' || c == '\r') {  // attribute-value normalisation
                if (c == '\r' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '\n') ++pos_;
                out.push_back(' ');
                ++pos_;
                continue;
            }
            if (Status st = char_data(out); !st) return st;
        }
    }

    Status element(Node& n, std::uint32_t depth) {
        if (depth > lim_.max_depth) return Status(Errc::LimitExceeded, "xml: nesting too deep", pos_);
        if (++nodes_ > lim_.max_nodes) return Status(Errc::LimitExceeded, "xml: too many nodes", pos_);
        ++pos_;  // '<'
        if (Status st = name(n.name); !st) return st;
        for (;;) {
            const bool had_space = pos_ < s_.size() && is_space(s_[pos_]);
            skip_ws();
            if (pos_ >= s_.size()) return trunc();
            if (s_[pos_] == '/') {
                if (pos_ + 1 >= s_.size()) return trunc();
                if (s_[pos_ + 1] != '>') return err("expected '/>'");
                pos_ += 2;
                return Status::ok();
            }
            if (s_[pos_] == '>') { ++pos_; break; }
            if (!had_space) return err("expected whitespace before an attribute");
            if (n.attrs.size() >= lim_.max_attributes) return Status(Errc::LimitExceeded, "xml: too many attributes", pos_);
            std::string an, av;
            if (Status st = name(an); !st) return st;
            skip_ws();
            if (pos_ >= s_.size()) return trunc();
            if (s_[pos_] != '=') return err("expected '='");
            ++pos_;
            skip_ws();
            if (Status st = attribute_value(av); !st) return st;
            for (const auto& a : n.attrs)
                if (a.first == an) return err("duplicate attribute");
            n.attrs.emplace_back(std::move(an), std::move(av));
        }
        // Content.
        for (;;) {
            if (pos_ >= s_.size()) return trunc();
            const char c = s_[pos_];
            if (c == '<') {
                if (starts("</")) {
                    pos_ += 2;
                    std::string end;
                    if (Status st = name(end); !st) return st;
                    if (end != n.name) return err("mismatched end tag");
                    skip_ws();
                    if (pos_ >= s_.size()) return trunc();
                    if (s_[pos_] != '>') return err("expected '>'");
                    ++pos_;
                    return Status::ok();
                }
                if (starts("<!--")) {
                    if (Status st = comment(); !st) return st;
                    continue;
                }
                if (starts("<![CDATA[")) {
                    pos_ += 9;
                    const auto end = s_.find("]]>", pos_);
                    if (end == std::string_view::npos) return trunc();
                    while (pos_ < end)
                        if (Status st = char_data(n.text); !st) return st;
                    pos_ = end + 3;
                    continue;
                }
                if (starts("<?")) {
                    if (Status st = pi(false); !st) return st;
                    continue;
                }
                if (starts("<!")) return err("declaration inside an element");
                n.children.emplace_back();
                if (Status st = element(n.children.back(), depth + 1); !st) return st;
                continue;
            }
            if (c == '&') {
                if (Status st = reference(n.text); !st) return st;
                continue;
            }
            if (c == '>' && pos_ >= 2 && s_[pos_ - 1] == ']' && s_[pos_ - 2] == ']') return err("']]>' in content");
            if (Status st = char_data(n.text); !st) return st;
        }
    }
};

}  // namespace

Result<Node> parse(std::string_view text, const ParseLimits& limits) {
    if (static_cast<std::uint64_t>(text.size()) > limits.max_input)
        return Status(Errc::LimitExceeded, "xml: document too large");
    Node root;
    Parser p(text, limits);
    if (Status st = p.run(root); !st) return st;
    return root;
}

void append_escaped(std::string& out, std::string_view s, bool attribute) {
    std::string clean;
    json::append_utf8_sanitized(clean, s);
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const auto c = static_cast<unsigned char>(clean[i]);
        switch (c) {
            case '<': out.append("&lt;"); break;
            case '>': out.append("&gt;"); break;
            case '&': out.append("&amp;"); break;
            case '"': if (attribute) out.append("&quot;"); else out.push_back('"'); break;
            case '\r': out.append("&#13;"); break;
            case '\t': if (attribute) out.append("&#9;"); else out.push_back('\t'); break;
            case '\n': if (attribute) out.append("&#10;"); else out.push_back('\n'); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    if (c < 0x20) out.append("\xEF\xBF\xBD");  // not representable in XML 1.0
                    else out.push_back(static_cast<char>(c));
                } else if (c == 0xEF && i + 2 < clean.size() && static_cast<unsigned char>(clean[i + 1]) == 0xBF &&
                           (static_cast<unsigned char>(clean[i + 2]) == 0xBE ||
                            static_cast<unsigned char>(clean[i + 2]) == 0xBF)) {
                    out.append("\xEF\xBF\xBD");
                    i += 2;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
}

}  // namespace siaara::web::xml
