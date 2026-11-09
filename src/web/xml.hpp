// SIAARA DICOM Toolkit - minimal XML 1.0 parser and writer, enough for the PS3.19 Native DICOM
// Model. Written from the W3C XML 1.0 recommendation.
// Supported: elements, attributes, character data, CDATA sections, comments, processing
// instructions (skipped), the five predefined entities and numeric character references.
// Rejected on purpose: DOCTYPE / DTDs (no external or custom entities, so no XXE or entity
// expansion attacks), and anything not well formed.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/status.hpp"

namespace siaara::web::xml {

struct Node {
    std::string name;                                        // qualified name as written
    std::vector<std::pair<std::string, std::string>> attrs;  // decoded values, document order
    std::vector<Node> children;                              // child elements
    std::string text;                                        // concatenated character data

    const std::string* attr(std::string_view n) const noexcept;
    const Node* child(std::string_view n) const noexcept;
};

struct ParseLimits {
    std::uint32_t max_depth = 128;  // element nesting; bounds the parser's recursion (PARSE-20)
    std::uint64_t max_input = 1ull << 30;
    std::uint64_t max_nodes = 50'000'000;
    std::uint32_t max_attributes = 64;      // per element
    std::uint64_t max_name_length = 1024;
};

// Parses a document and returns its root element.
Result<Node> parse(std::string_view text, const ParseLimits& limits = {});

// Escapes character data / attribute values. Characters not allowed in XML 1.0 (most C0 controls)
// are replaced by U+FFFD; invalid UTF-8 likewise.
void append_escaped(std::string& out, std::string_view s, bool attribute);

}  // namespace siaara::web::xml
