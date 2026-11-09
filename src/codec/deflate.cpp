// Placeholder until the DEFLATE module lands (owned by the codec worker).
#include "codec/deflate.hpp"

namespace siaara::codec {

Status inflate_raw(const std::uint8_t*, std::size_t, std::size_t, std::vector<std::uint8_t>&,
                   std::size_t*) {
    return Status(Errc::Unsupported, "deflate not implemented yet");
}

Status deflate_raw(const std::uint8_t*, std::size_t, int, std::vector<std::uint8_t>&) {
    return Status(Errc::Unsupported, "deflate not implemented yet");
}

}  // namespace siaara::codec
