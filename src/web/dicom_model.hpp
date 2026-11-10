// SIAARA DICOM Toolkit - options shared by the DICOM JSON (PS3.18 Annex F) and Native DICOM
// Model XML (PS3.19 Annex A) encoders, plus value helpers used by both.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "web/json.hpp"
#include "web/xml.hpp"

namespace siaara::web {

// Location of an element inside a data set: tag, then (item index, tag) pairs for nested ones.
using ElementPath = std::vector<std::uint32_t>;

struct BulkReference {
    ElementPath path;
    VR vr = VR::None;
    std::string uri;  // BulkDataURI (JSON) or BulkData uri / "uuid:<uuid>" (XML)
};

struct ModelWriteOptions {
    // Called for every non-sequence element; returning a URI writes BulkDataURI (JSON) or
    // <BulkData uri=...> (XML) instead of the value. Default: every value inline.
    std::function<std::optional<std::string>(const Element&, const ElementPath&)> bulk_data_uri;
    // DS and IS values are always written as JSON strings (PS3.18 F.2.3 allows either). By
    // default they are numbers, except where a JSON number could lose information: values that
    // are not JSON number tokens ("+1", ".5", "007"), IS beyond +-(2^53-1) and DS with more than
    // 15 significant digits are written as strings so their exact text is kept.
    bool numbers_as_strings = false;
    // Encapsulated pixel data without a bulk data URI is written inline as its encapsulated
    // byte stream (items + sequence delimiter, PS3.5 A.4). When false it is an error.
    bool inline_encapsulated = true;
    // Text that cannot be converted to UTF-8 from its Specific Character Set is an error
    // (Errc::Unsupported). When false, unconvertible bytes become U+FFFD.
    bool strict_charset = false;
    // XML: keyword="..." on public attributes, and on private attributes whose Private Creator and
    // element the private dictionary knows (docs/PRIVATE_DICTIONARY.md).
    bool xml_keywords = true;
    // JSON: adds "keyword" and "name" members to private attributes the private dictionary knows.
    // Off by default: they are not members of the PS3.18 F.2 attribute object (readers that follow
    // PS3.18 ignore them; SIAARA's own JSON reader does).
    bool json_private_names = false;
    std::uint32_t max_depth = 64;   // sequence nesting
};

struct ModelReadOptions {
    // A text nesting limit (max_depth) of 0 here, the default, is derived from max_depth below:
    // DICOM JSON uses 3 levels per sequence nesting (attribute, Value array, item object), DICOM
    // XML 2 (DicomAttribute, Item), plus a few for the root (PARSE-20).
    json::ParseLimits json_limits = [] {
        json::ParseLimits l;
        l.max_depth = 0;
        return l;
    }();
    xml::ParseLimits xml_limits = [] {
        xml::ParseLimits l;
        l.max_depth = 0;
        return l;
    }();
    std::uint32_t max_depth = 64;                     // sequence nesting
    std::uint64_t max_value_bytes = 1ull << 31;       // one element's value
    std::uint64_t max_elements = 10'000'000;          // whole document
    // Resolves a BulkDataURI. When unset (or when it returns NotFound), the element is created
    // with an empty value and listed in `unresolved` (if given).
    std::function<Result<Bytes>(std::string_view uri, Tag tag, VR vr)> resolve_bulk_data;
    std::vector<BulkReference>* unresolved = nullptr;
    // JSON/XML text is UTF-8. When any value is non-ASCII and the top-level Specific Character
    // Set is not ISO_IR 192, it is set to "ISO_IR 192" so the data set stays self-consistent.
    bool set_utf8_charset = true;
    // An InlineBinary Pixel Data (7FE0,0010) value that is a complete encapsulated stream
    // (items + sequence delimiter) is turned back into fragments.
    bool detect_encapsulated = true;
};

namespace detail {

// Ascii: the default repertoire (no Specific Character Set, ISO_IR 6): a byte outside it is not
// text of the declared character set and is written as U+FFFD - never as a guessed character
// (as charset::get_utf8 and the path API do; the raw bytes stay available through the raw
// accessors).
enum class Charset : std::uint8_t { Utf8, Latin1, Other, Ascii };

Charset charset_of(const Dataset& ds, Charset inherited, std::string& name_out);
bool vr_is_charset_text(VR vr) noexcept;
// Splits a string value into its values (single value for LT/ST/UT/UR), trimmed per VR.
std::vector<std::string_view> split_values(std::string_view raw, VR vr);
// Converts text to UTF-8. Returns false if the charset is unsupported and the text is not ASCII
// (strict mode); otherwise replaces undecodable bytes with U+FFFD.
bool to_utf8(std::string_view raw, Charset cs, bool strict, std::string& out);
// DS/IS value to JSON: true if it should be a number token.
bool number_is_safe(std::string_view v, VR vr) noexcept;
// Shortest text that reads back to the same float / double ("NaN", "Infinity", "-Infinity" for
// non-finite values).
std::string format_double(double v);
std::string format_float(float v);
// Locale-independent, correctly rounded decimal to binary conversion of a JSON number token.
std::optional<double> parse_double(std::string_view s);
std::optional<float> parse_float(std::string_view s);
// Exact integer from a JSON number token ("12", "-3", "1e3", "5.0"); nullopt if not integral or
// outside [min, max].
std::optional<std::int64_t> parse_int(std::string_view s, std::int64_t min, std::int64_t max);
std::optional<std::uint64_t> parse_uint(std::string_view s, std::uint64_t max);
// Encapsulated pixel data as a PS3.5 A.4 byte stream, and back.
Status encapsulated_stream(const Element& e, std::vector<std::uint8_t>& out);
bool parse_encapsulated_stream(const std::vector<std::uint8_t>& data, std::vector<Bytes>& fragments);
std::string tag_hex(Tag t);
std::optional<Tag> parse_tag_hex(std::string_view s) noexcept;
Bytes to_bytes(std::vector<std::uint8_t>&& v);
Bytes to_bytes(std::string&& s);
bool is_ascii(std::string_view s) noexcept;
// Applies the ISO_IR 192 rule of ModelReadOptions::set_utf8_charset.
void apply_utf8_charset(Dataset& ds, bool any_non_ascii);
// Encodes one numeric binary value (US, SS, UL, SL, SV, UV, FL, FD) from text. Appends LE bytes.
Status encode_number(VR vr, std::string_view text, bool is_json_string, std::vector<std::uint8_t>& out);
// Decodes value `i` of a numeric binary VR as text (shortest round trip for floats).
std::string number_text(VR vr, const std::uint8_t* p);
// True when the element's numeric binary value would be a JSON string ("NaN", big SV/UV).
bool number_needs_string(VR vr, const std::uint8_t* p);

}  // namespace detail

}  // namespace siaara::web
