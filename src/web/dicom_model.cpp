// SIAARA DICOM Toolkit - value helpers shared by the DICOM JSON and XML encoders.
#include "web/dicom_model.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <ios>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace siaara::web::detail {

std::string tag_hex(Tag t) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string s(8, '0');
    for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(7 - i)] = kHex[(t >> (4 * i)) & 15u];
    return s;
}

std::optional<Tag> parse_tag_hex(std::string_view s) noexcept {
    if (s.size() != 8) return std::nullopt;
    Tag t = 0;
    for (char c : s) {
        std::uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
        else return std::nullopt;
        t = (t << 4) | d;
    }
    return t;
}

Bytes to_bytes(std::vector<std::uint8_t>&& v) {
    if (v.empty()) return Bytes();
    auto store = std::make_shared<std::vector<std::uint8_t>>(std::move(v));
    const std::uint8_t* p = store->data();
    const std::size_t n = store->size();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, n);
}

Bytes to_bytes(std::string&& s) {
    if (s.empty()) return Bytes();
    auto store = std::make_shared<std::string>(std::move(s));
    const auto* p = reinterpret_cast<const std::uint8_t*>(store->data());
    const std::size_t n = store->size();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, n);
}

bool is_ascii(std::string_view s) noexcept {
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    return true;
}

Charset charset_of(const Dataset& ds, Charset inherited, std::string& name_out) {
    const auto raw = ds.get_raw_string(tags::SpecificCharacterSet);
    if (!raw) return inherited;
    const auto values = split_values(*raw, VR::CS);
    name_out.clear();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) name_out.push_back('\\');
        name_out.append(values[i]);
    }
    if (values.empty()) return Charset::Utf8;  // default repertoire (ASCII)
    if (values.size() == 1) {
        const auto v = values[0];
        if (v.empty() || v == "ISO_IR 6" || v == "ISO_IR 192" || v == "ISO 2022 IR 6") return Charset::Utf8;
        if (v == "ISO_IR 100" || v == "ISO 2022 IR 100") return Charset::Latin1;
        return Charset::Other;
    }
    // Code extensions: only the trivial "ISO 2022 IR 6" combinations are handled.
    for (const auto v : values)
        if (!v.empty() && v != "ISO 2022 IR 6") return Charset::Other;
    return Charset::Utf8;
}

bool vr_is_charset_text(VR vr) noexcept {
    return vr == VR::SH || vr == VR::LO || vr == VR::ST || vr == VR::LT || vr == VR::UC || vr == VR::UT ||
           vr == VR::PN;
}

std::vector<std::string_view> split_values(std::string_view raw, VR vr) {
    std::vector<std::string_view> out;
    if (vr == VR::LT || vr == VR::ST || vr == VR::UT || vr == VR::UR) {
        // Single valued; only trailing padding is insignificant (UR: leading too, PS3.5 6.2).
        auto v = trim_value(raw, vr);
        if (vr == VR::UR) while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        if (!v.empty()) out.push_back(v);
        return out;
    }
    if (trim_value(raw, vr).empty() && raw.find('\\') == std::string_view::npos) return out;
    std::size_t start = 0;
    for (;;) {
        const auto pos = raw.find('\\', start);
        const auto part = raw.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
        out.push_back(trim_value(part, vr));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

bool to_utf8(std::string_view raw, Charset cs, bool strict, std::string& out) {
    switch (cs) {
        case Charset::Utf8:
            if (strict && !json::is_valid_utf8(raw)) return false;
            json::append_utf8_sanitized(out, raw);
            return true;
        case Charset::Latin1:
            for (char c : raw) json::append_code_point(out, static_cast<unsigned char>(c));
            return true;
        case Charset::Other:
            if (is_ascii(raw)) { out.append(raw); return true; }
            if (strict) return false;
            for (char c : raw) {
                if (static_cast<unsigned char>(c) < 0x80) out.push_back(c);
                else json::append_code_point(out, 0xFFFD);
            }
            return true;
    }
    return false;
}

namespace {

struct Decimal {
    bool neg = false;
    std::string digits;   // significant digits, no leading zeros (empty = zero)
    std::int64_t exp10 = 0;  // value = digits * 10^exp10
};

// Parses a JSON number token into its exact decimal form.
bool decompose(std::string_view s, Decimal& d) {
    if (!json::is_number_token(s)) return false;
    std::size_t i = 0;
    if (s[i] == '-') { d.neg = true; ++i; }
    std::int64_t frac = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
        if (!(d.digits.empty() && s[i] == '0')) d.digits.push_back(s[i]);
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            ++frac;
            if (!(d.digits.empty() && s[i] == '0')) d.digits.push_back(s[i]);
        }
    }
    std::int64_t e = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (s[i] == '+' || s[i] == '-') eneg = s[i++] == '-';
        for (; i < s.size(); ++i)
            if (e < 1'000'000'000) e = e * 10 + (s[i] - '0');
        if (eneg) e = -e;
    }
    // Remove trailing zeros of the significand into the exponent.
    std::int64_t tz = 0;
    while (!d.digits.empty() && d.digits.back() == '0') { d.digits.pop_back(); ++tz; }
    d.exp10 = e - frac + tz;
    return true;
}

}  // namespace

std::optional<std::uint64_t> parse_uint(std::string_view s, std::uint64_t max) {
    Decimal d;
    if (!decompose(s, d)) return std::nullopt;
    if (d.digits.empty()) return 0;
    if (d.neg || d.exp10 < 0) return std::nullopt;
    if (static_cast<std::int64_t>(d.digits.size()) + d.exp10 > 20) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : d.digits)
        if (!checked_mul(v, 10, v) || !checked_add(v, static_cast<std::uint64_t>(c - '0'), v)) return std::nullopt;
    for (std::int64_t k = 0; k < d.exp10; ++k)
        if (!checked_mul(v, 10, v)) return std::nullopt;
    if (v > max) return std::nullopt;
    return v;
}

std::optional<std::int64_t> parse_int(std::string_view s, std::int64_t min, std::int64_t max) {
    Decimal d;
    if (!decompose(s, d)) return std::nullopt;
    if (d.digits.empty()) return 0;
    if (d.exp10 < 0) return std::nullopt;
    if (static_cast<std::int64_t>(d.digits.size()) + d.exp10 > 20) return std::nullopt;
    std::uint64_t mag = 0;
    for (char c : d.digits)
        if (!checked_mul(mag, 10, mag) || !checked_add(mag, static_cast<std::uint64_t>(c - '0'), mag)) return std::nullopt;
    for (std::int64_t k = 0; k < d.exp10; ++k)
        if (!checked_mul(mag, 10, mag)) return std::nullopt;
    std::int64_t v = 0;
    if (d.neg) {
        if (mag > static_cast<std::uint64_t>(INT64_MAX) + 1u) return std::nullopt;
        v = mag == static_cast<std::uint64_t>(INT64_MAX) + 1u ? INT64_MIN : -static_cast<std::int64_t>(mag);
    } else {
        if (mag > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
        v = static_cast<std::int64_t>(mag);
    }
    if (v < min || v > max) return std::nullopt;
    return v;
}

bool number_is_safe(std::string_view v, VR vr) noexcept {
    if (!json::is_number_token(v)) return false;
    // Count significant digits and find the exponent.
    std::size_t sig = 0;
    bool started = false, any_frac_or_exp = false;
    std::size_t pending_zeros = 0;
    std::int64_t e = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '.') { any_frac_or_exp = true; continue; }
        if (c == 'e' || c == 'E') {
            any_frac_or_exp = true;
            std::size_t j = i + 1;
            bool neg = false;
            if (j < v.size() && (v[j] == '+' || v[j] == '-')) neg = v[j++] == '-';
            for (; j < v.size(); ++j)
                if (e < 100000) e = e * 10 + (v[j] - '0');
            if (neg) e = -e;
            break;
        }
        if (c < '0' || c > '9') continue;
        if (c == '0') {
            if (started) ++pending_zeros;
            continue;
        }
        started = true;
        sig += pending_zeros + 1;
        pending_zeros = 0;
    }
    if (vr == VR::IS) {
        if (any_frac_or_exp) return false;
        return sig + pending_zeros <= 15;
    }
    return sig <= 15 && e >= -300 && e <= 300;
}

namespace {

template <class F>
std::optional<F> parse_float_t(std::string_view s) {
    if (!json::is_number_token(s)) return std::nullopt;
    std::istringstream in{std::string(s)};
    in.imbue(std::locale::classic());
    F v{};
    in >> v;
    if (in.fail()) {
        // Some standard libraries flag underflow to a subnormal / zero as a failure. The token is
        // grammatically valid, so a tiny magnitude means the value is (close to) zero: retry the
        // decision with the exact decimal form.
        Decimal d;
        if (!decompose(s, d)) return std::nullopt;
        if (d.digits.empty()) return d.neg ? -F(0) : F(0);
        const std::int64_t mag = static_cast<std::int64_t>(d.digits.size()) + d.exp10;
        if (mag < std::numeric_limits<F>::min_exponent10 - 30) return d.neg ? -F(0) : F(0);
        return std::nullopt;  // overflow or genuinely unparseable
    }
    if (in.peek() != std::char_traits<char>::eof()) return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

template <class F>
std::string format_float_t(F v, int max_precision) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    for (int p = 1; p <= max_precision; ++p) {
        out.str(std::string());
        out << std::setprecision(p) << v;
        auto back = parse_float_t<F>(out.str());
        if (back && (*back == v) && (std::signbit(*back) == std::signbit(v))) return out.str();
    }
    out.str(std::string());
    out << std::setprecision(max_precision) << v;
    return out.str();
}

}  // namespace

std::optional<double> parse_double(std::string_view s) { return parse_float_t<double>(s); }
std::optional<float> parse_float(std::string_view s) { return parse_float_t<float>(s); }
std::string format_double(double v) { return format_float_t<double>(v, 17); }
std::string format_float(float v) { return format_float_t<float>(v, 9); }

Status encapsulated_stream(const Element& e, std::vector<std::uint8_t>& out) {
    out.clear();
    std::uint64_t total = 8;
    for (const auto& f : e.fragments) {
        std::uint64_t t = 0;
        if (!checked_add(total, 9, t) || !checked_add(t, f.size(), total))
            return Status(Errc::LimitExceeded, "encapsulated data too large", 0, e.tag);
    }
    if (total > SIZE_MAX / 2) return Status(Errc::LimitExceeded, "encapsulated data too large", 0, e.tag);
    out.reserve(static_cast<std::size_t>(total));
    auto put32 = [&](std::uint32_t v) {
        std::uint8_t b[4];
        store_le32(b, v);
        out.insert(out.end(), b, b + 4);
    };
    auto put_tag = [&](Tag t) {
        std::uint8_t b[4];
        store_le16(b, tag_group(t));
        store_le16(b + 2, tag_element(t));
        out.insert(out.end(), b, b + 4);
    };
    for (const auto& f : e.fragments) {
        const std::uint64_t len = f.size() + (f.size() & 1u);
        if (len >= kUndefinedLength) return Status(Errc::LimitExceeded, "fragment too large", 0, e.tag);
        put_tag(tags::Item);
        put32(static_cast<std::uint32_t>(len));
        out.insert(out.end(), f.data(), f.data() + f.size());
        if (f.size() & 1u) out.push_back(0);
    }
    put_tag(tags::SequenceDelimitationItem);
    put32(0);
    return Status::ok();
}

bool parse_encapsulated_stream(const std::vector<std::uint8_t>& data, std::vector<Bytes>& fragments) {
    fragments.clear();
    const std::size_t n = data.size();
    if (n < 16) return false;
    auto shared = std::make_shared<std::vector<std::uint8_t>>(data);
    std::shared_ptr<const void> owner(shared, shared.get());
    const std::uint8_t* p = shared->data();
    std::size_t pos = 0;
    while (n - pos >= 8) {
        const Tag t = (static_cast<Tag>(load_le16(p + pos)) << 16) | load_le16(p + pos + 2);
        const std::uint32_t len = load_le32(p + pos + 4);
        pos += 8;
        if (t == tags::SequenceDelimitationItem) return len == 0 && pos == n && !fragments.empty();
        if (t != tags::Item || len == kUndefinedLength || len > n - pos) return false;
        fragments.emplace_back(owner, p + pos, len);
        pos += len;
    }
    return false;
}

void apply_utf8_charset(Dataset& ds, bool any_non_ascii) {
    if (!any_non_ascii) return;
    const auto raw = ds.get_raw_string(tags::SpecificCharacterSet);
    if (raw && trim_value(*raw, VR::CS) == "ISO_IR 192") return;
    (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
}

Status encode_number(VR vr, std::string_view text, bool is_json_string, std::vector<std::uint8_t>& out) {
    // Strings may carry surrounding spaces (e.g. XML text); numbers never do.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    std::uint8_t b[8];
    auto bad = [&]() { return Status(Errc::Malformed, std::string("invalid ") + std::string(vr_name(vr)) + " value"); };
    switch (vr) {
        case VR::US: { auto v = parse_uint(text, 0xFFFF); if (!v) return bad(); store_le16(b, static_cast<std::uint16_t>(*v)); out.insert(out.end(), b, b + 2); break; }
        case VR::SS: { auto v = parse_int(text, INT16_MIN, INT16_MAX); if (!v) return bad(); store_le16(b, static_cast<std::uint16_t>(static_cast<std::int16_t>(*v))); out.insert(out.end(), b, b + 2); break; }
        case VR::UL: { auto v = parse_uint(text, 0xFFFFFFFFu); if (!v) return bad(); store_le32(b, static_cast<std::uint32_t>(*v)); out.insert(out.end(), b, b + 4); break; }
        case VR::SL: { auto v = parse_int(text, INT32_MIN, INT32_MAX); if (!v) return bad(); store_le32(b, static_cast<std::uint32_t>(static_cast<std::int32_t>(*v))); out.insert(out.end(), b, b + 4); break; }
        case VR::UV: {
            auto v = parse_uint(text, UINT64_MAX); if (!v) return bad();
            store_le32(b, static_cast<std::uint32_t>(*v)); store_le32(b + 4, static_cast<std::uint32_t>(*v >> 32));
            out.insert(out.end(), b, b + 8); break;
        }
        case VR::SV: {
            auto v = parse_int(text, INT64_MIN, INT64_MAX); if (!v) return bad();
            const auto u = static_cast<std::uint64_t>(*v);
            store_le32(b, static_cast<std::uint32_t>(u)); store_le32(b + 4, static_cast<std::uint32_t>(u >> 32));
            out.insert(out.end(), b, b + 8); break;
        }
        case VR::FL: {
            float f = 0;
            if (is_json_string && text == "NaN") f = std::numeric_limits<float>::quiet_NaN();
            else if (is_json_string && text == "Infinity") f = std::numeric_limits<float>::infinity();
            else if (is_json_string && text == "-Infinity") f = -std::numeric_limits<float>::infinity();
            else { auto v = parse_float(text); if (!v) return bad(); f = *v; }
            std::uint32_t u = 0;
            std::memcpy(&u, &f, 4);
            store_le32(b, u);
            out.insert(out.end(), b, b + 4);
            break;
        }
        case VR::FD: {
            double d = 0;
            if (is_json_string && text == "NaN") d = std::numeric_limits<double>::quiet_NaN();
            else if (is_json_string && text == "Infinity") d = std::numeric_limits<double>::infinity();
            else if (is_json_string && text == "-Infinity") d = -std::numeric_limits<double>::infinity();
            else { auto v = parse_double(text); if (!v) return bad(); d = *v; }
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            store_le32(b, static_cast<std::uint32_t>(u)); store_le32(b + 4, static_cast<std::uint32_t>(u >> 32));
            out.insert(out.end(), b, b + 8);
            break;
        }
        default: return Status(Errc::Internal, "not a numeric VR");
    }
    return Status::ok();
}

std::string number_text(VR vr, const std::uint8_t* p) {
    switch (vr) {
        case VR::US: return std::to_string(load_le16(p));
        case VR::SS: return std::to_string(static_cast<std::int16_t>(load_le16(p)));
        case VR::UL: return std::to_string(load_le32(p));
        case VR::SL: return std::to_string(static_cast<std::int32_t>(load_le32(p)));
        case VR::UV: return std::to_string(load_le64(p));
        case VR::SV: return std::to_string(static_cast<std::int64_t>(load_le64(p)));
        case VR::FL: { const std::uint32_t u = load_le32(p); float f; std::memcpy(&f, &u, 4); return format_float(f); }
        case VR::FD: { const std::uint64_t u = load_le64(p); double d; std::memcpy(&d, &u, 8); return format_double(d); }
        default: return std::string();
    }
}

bool number_needs_string(VR vr, const std::uint8_t* p) {
    constexpr std::uint64_t kMaxSafe = (1ull << 53) - 1;
    switch (vr) {
        case VR::UV: return load_le64(p) > kMaxSafe;
        case VR::SV: {
            const auto v = static_cast<std::int64_t>(load_le64(p));
            return v > static_cast<std::int64_t>(kMaxSafe) || v < -static_cast<std::int64_t>(kMaxSafe);
        }
        case VR::FL: { const std::uint32_t u = load_le32(p); float f; std::memcpy(&f, &u, 4); return !std::isfinite(f); }
        case VR::FD: { const std::uint64_t u = load_le64(p); double d; std::memcpy(&d, &u, 8); return !std::isfinite(d); }
        default: return false;
    }
}

}  // namespace siaara::web::detail
