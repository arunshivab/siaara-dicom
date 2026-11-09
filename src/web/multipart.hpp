// SIAARA DICOM Toolkit - multipart/related (RFC 2387) on the RFC 2046 section 5.1 multipart syntax.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "web/http.hpp"

namespace siaara::web::multipart {

struct Limits {
    std::size_t max_parts = 100000;
    std::size_t max_boundary = 70;          // RFC 2046 maximum
    std::size_t max_part_headers = 32;
    std::size_t max_part_header_bytes = 16384;
};

struct Part {
    http::Headers headers;
    std::string_view body;  // view into the parsed message body
    const std::string* content_type() const noexcept { return http::find_header(headers, "Content-Type"); }
};

// True when `b` is a valid RFC 2046 boundary (1..70 bchars, not ending in a space).
bool valid_boundary(std::string_view b, std::size_t max_len = 70) noexcept;

// Splits a multipart body. The returned views point into `body`.
Result<std::vector<Part>> parse(std::string_view body, std::string_view boundary, const Limits& limits = {});

// Builds a body. The boundary is chosen so that it occurs in no part.
class Writer {
public:
    Writer();
    void add(std::string_view content_type, std::string_view data, http::Headers extra = {});
    // The finished body; boundary() is valid afterwards.
    std::string finish();
    const std::string& boundary() const noexcept { return boundary_; }
    // Content-Type for the message: multipart/related; type="<root type>"; boundary=...
    std::string content_type(std::string_view root_type) const;

private:
    struct Item {
        http::Headers headers;
        std::string data;
    };
    std::vector<Item> items_;
    std::string boundary_;
};

}  // namespace siaara::web::multipart
