// SIAARA DICOM Toolkit - generated code tables (see codes.hpp).
#include "clinical/codes.hpp"

namespace siaara::clinical {

std::span<const CodeEntry> context_group(int cid) noexcept {
    switch (cid) {
        case 7010: return codes::kCid7010;
        case 7011: return codes::kCid7011;
        default: return {};
    }
}

std::string_view context_group_version(int cid) noexcept {
    switch (cid) {
        case 7010: return codes::kCid7010Version;
        case 7011: return codes::kCid7011Version;
        default: return {};
    }
}

std::string_view context_group_uid(int cid) noexcept {
    switch (cid) {
        case 7010: return codes::kCid7010Uid;
        case 7011: return codes::kCid7011Uid;
        default: return {};
    }
}

std::optional<Code> cid_code(int cid, std::string_view value, std::string_view scheme) {
    for (const auto& e : context_group(cid))
        if (e.value == value && (scheme.empty() || e.scheme == scheme)) return e.code();
    return std::nullopt;
}

}  // namespace siaara::clinical
