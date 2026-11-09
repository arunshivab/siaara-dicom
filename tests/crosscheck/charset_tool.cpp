// Test-only helper for tests/crosscheck/charset_crosscheck.py (not installed).
// Prints every SH/LO/UC/ST/LT/UT/PN element of a file, recursing into sequences, as
//   <path> TAB <VR> TAB <hex of the UTF-8 values joined with '\'>
// using charset::get_utf8 with (0008,0005) inherited by sequence items.
#include <cstdio>
#include <string>

#include "charset/charset.hpp"
#include "core/reader.hpp"

using namespace siaara;

namespace {

void walk(const Dataset& ds, const charset::CharacterSet* inherited, const std::string& prefix) {
    const charset::CharacterSet cs = charset::character_set_of(ds, inherited);
    for (const Element& e : ds.elements()) {
        char tag[16];
        std::snprintf(tag, sizeof tag, "(%04X,%04X)", tag_group(e.tag), tag_element(e.tag));
        const std::string path = prefix + tag;
        if (e.is_sequence()) {
            for (std::size_t i = 0; i < e.items.size(); ++i)
                walk(e.items[i], &cs, path + "[" + std::to_string(i) + "].");
            continue;
        }
        if (e.vr == VR::UN) {  // e.g. private tags in implicit VR files: VR unknown to the reader
            std::printf("%s\tUN\t-\t-\n", path.c_str());
            continue;
        }
        if (!charset::vr_uses_character_set(e.vr)) continue;
        std::string joined;
        for (std::size_t k = 0;; ++k) {
            auto v = charset::get_utf8(ds, e.tag, k, inherited);
            if (!v) break;
            if (k) joined.push_back('\\');
            joined += *v;
        }
        std::string hex;
        for (char c : joined) {
            char b[3];
            std::snprintf(b, sizeof b, "%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            hex += b;
        }
        // Round trip: re-encoding the decoded value must give the original bytes.
        const charset::CharacterSet& used = cs;
        const auto kind = charset::text_kind(e.vr);
        const std::string decoded = charset::decode(e.value.as_chars(), used, kind);
        const charset::EncodeResult back = charset::encode(decoded, used, kind);
        const bool round_trip = back.ok() && back.bytes == e.value.as_chars();
        const auto vr = vr_name(e.vr);
        std::printf("%s\t%.*s\t%s\t%s\n", path.c_str(), static_cast<int>(vr.size()), vr.data(),
                    hex.empty() ? "-" : hex.c_str(), round_trip ? "rt" : "no-rt");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: charset_tool <file>\n");
        return 2;
    }
    auto r = read_file(argv[1]);
    if (!r) {
        std::fprintf(stderr, "read failed: %s\n", r.error().message.c_str());
        return 1;
    }
    walk(r->dataset, nullptr, "");
    return 0;
}
