// SIAARA DICOM Toolkit - multipart/related (RFC 2387) on the RFC 2046 section 5.1 multipart syntax.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
    std::size_t max_delimiter_padding = 64;  // blanks allowed after a boundary (RFC 2046 transport padding)
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

// Incremental parsing of a multipart body that arrives in pieces: each complete part is passed to
// `on_part` (its views are valid during the call only) and dropped, so memory holds about one part.
// A part larger than max_part_bytes fails with LimitExceeded; an error from on_part stops parsing.
class StreamParser {
public:
    StreamParser(std::string boundary, const Limits& limits, std::uint64_t max_part_bytes,
                 std::function<Status(const Part&)> on_part);
    Status feed(std::string_view data);
    // After the last piece: Truncated when the close delimiter was not seen.
    Status finish();
    std::size_t parts() const noexcept { return parts_; }

private:
    enum class State { Preamble, Delimiter, Part, Done, Failed };
    Status step();
    std::string dash_;
    Limits limits_;
    std::uint64_t max_part_;
    std::function<Status(const Part&)> on_part_;
    std::string buf_;
    std::size_t scanned_ = 0;
    bool at_line_start_ = true;  // whether buf_[0] follows a line end (or is the start of the stream)
    std::size_t parts_ = 0;
    State state_ = State::Preamble;
};

// A new random boundary ("siaara-" plus 32 characters from a 62-letter alphabet): for bodies that
// are streamed, where the parts cannot be searched for it first (a clash has probability 62^-32).
std::string make_boundary();

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
