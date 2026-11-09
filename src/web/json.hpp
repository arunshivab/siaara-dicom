// SIAARA DICOM Toolkit - minimal JSON (RFC 8259) parser and writer.
// Numbers are kept as their original token text, so no precision is ever lost in the parser;
// conversion to integers / doubles is the caller's choice (see dicom_json.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/status.hpp"

namespace siaara::web::json {

enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

struct Member;

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    std::string text;  // Number: the token exactly as written; String: decoded UTF-8
    std::vector<Value> array;
    std::vector<Member> object;  // members in document order (vector of an incomplete type: C++17)

    bool is_null() const noexcept { return type == Type::Null; }
    bool is_string() const noexcept { return type == Type::String; }
    bool is_number() const noexcept { return type == Type::Number; }
    bool is_array() const noexcept { return type == Type::Array; }
    bool is_object() const noexcept { return type == Type::Object; }
    // First member with this key, or nullptr.
    const Value* get(std::string_view key) const noexcept;
};

struct Member {
    std::string first;  // name
    Value second;       // value
};

struct ParseLimits {
    std::uint32_t max_depth = 512;                  // nesting of arrays + objects
    std::uint64_t max_input = 1ull << 30;           // bytes of JSON text
    std::uint64_t max_string = 1ull << 30;          // bytes of one decoded string
    std::uint64_t max_values = 50'000'000;          // total number of values in the document
    std::uint64_t max_number_length = 1024;         // characters in one number token
};

// Parses one JSON text (RFC 8259 section 2: any value, surrounded by optional whitespace).
// Strict: no comments, no trailing commas, no NaN/Infinity, no leading zeros, only valid UTF-8,
// no unescaped control characters, no lone surrogates. A leading UTF-8 BOM is rejected.
Result<Value> parse(std::string_view text, const ParseLimits& limits = {});

// True when `s` is a JSON number token (RFC 8259 section 6).
bool is_number_token(std::string_view s) noexcept;

// Appends `s` as a JSON string literal. Invalid UTF-8 sequences are replaced by U+FFFD so the
// output is always valid JSON.
void write_string(std::string& out, std::string_view utf8);

// Serialises a Value (compact form).
void write(std::string& out, const Value& v);

// Validates UTF-8 (RFC 3629: no overlongs, no surrogates, max U+10FFFF).
bool is_valid_utf8(std::string_view s) noexcept;
// Appends `s` with invalid sequences replaced by U+FFFD.
void append_utf8_sanitized(std::string& out, std::string_view s);
// Appends the UTF-8 encoding of a code point (must be a scalar value).
void append_code_point(std::string& out, std::uint32_t cp);

}  // namespace siaara::web::json
