#include "store/path.hpp"

#include <cstddef>
#include <system_error>

#include "store/common.hpp"

namespace siaara::store {

namespace fs = std::filesystem;

namespace {
char upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

bool iequal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

bool reserved_device(std::string_view c) noexcept {
    // The device name applies to the part before the first dot ("CON.txt" is also reserved).
    std::string_view stem = c.substr(0, c.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
    static constexpr std::string_view kNames[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"};
    for (auto n : kNames)
        if (iequal(stem, n)) return true;
    if (stem.size() == 4 && (iequal(stem.substr(0, 3), "COM") || iequal(stem.substr(0, 3), "LPT"))) {
        const char d = stem[3];
        if (d >= '0' && d <= '9') return true;
    }
    // COM and LPT with superscript digits (U+00B9, U+00B2, U+00B3) are reserved too.
    if (stem.size() == 5 && (iequal(stem.substr(0, 3), "COM") || iequal(stem.substr(0, 3), "LPT")) &&
        static_cast<unsigned char>(stem[3]) == 0xC2) {
        const auto d = static_cast<unsigned char>(stem[4]);
        if (d == 0xB9 || d == 0xB2 || d == 0xB3) return true;
    }
    return false;
}
}  // namespace

bool path_component_ok(std::string_view c) noexcept {
    if (c.empty() || c.size() > 255) return false;
    if (c == "." || c == "..") return false;
    for (char ch : c) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7F) return false;
        switch (ch) {
            case '/': case '\\': case ':': case '<': case '>': case '"': case '|': case '?': case '*':
                return false;
            default: break;
        }
    }
    if (c.back() == '.' || c.back() == ' ') return false;
    if (reserved_device(c)) return false;
    return true;
}

std::vector<std::string> split_file_id(std::string_view v) {
    std::vector<std::string> out;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.remove_suffix(1);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= v.size(); ++i) {
        if (i == v.size() || v[i] == '\\') {
            std::string_view part = v.substr(start, i - start);
            while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
            while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
            out.emplace_back(part);
            start = i + 1;
        }
    }
    return out;
}

Result<fs::path> safe_join(const fs::path& root, const std::vector<std::string>& comps,
                           std::size_t max_components) {
    if (comps.empty()) return Status(Errc::Rejected, "empty file ID");
    if (comps.size() > max_components) return Status(Errc::Rejected, "file ID has too many components");
    fs::path p = root;
    for (const auto& c : comps) {
        if (!path_component_ok(c)) return Status(Errc::Rejected, "unsafe file ID component");
        p /= u8path(c);
    }
    return p;
}

bool is_within(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    const fs::path r = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const fs::path q = fs::weakly_canonical(p, ec);
    if (ec) return false;
    auto ri = r.begin();
    auto qi = q.begin();
    for (; ri != r.end(); ++ri, ++qi) {
        if (ri->empty()) continue;  // trailing separator
        if (qi == q.end() || *ri != *qi) return false;
    }
    return true;
}

Result<fs::path> resolve_file_id(const fs::path& root, const std::vector<std::string>& comps,
                                 bool case_insensitive) {
    auto joined = safe_join(root, comps);
    if (!joined) return joined.take_error();
    std::error_code ec;
    fs::path found;
    if (fs::is_regular_file(*joined, ec)) {
        found = *joined;
    } else if (case_insensitive) {
        fs::path cur = root;
        for (std::size_t i = 0; i < comps.size(); ++i) {
            fs::path direct = cur / u8path(comps[i]);
            if (fs::exists(direct, ec)) {
                cur = direct;
                continue;
            }
            bool hit = false;
            for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
                if (iequal(to_utf8(it->path().filename()), comps[i])) {
                    cur = it->path();
                    hit = true;
                    break;
                }
            }
            if (!hit) return Status(Errc::NotFound, "referenced file not found");
        }
        if (!fs::is_regular_file(cur, ec)) return Status(Errc::NotFound, "referenced file not found");
        found = cur;
    } else {
        return Status(Errc::NotFound, "referenced file not found");
    }
    if (!is_within(root, found)) return Status(Errc::Rejected, "file ID resolves outside the root");
    return found;
}

}  // namespace siaara::store
