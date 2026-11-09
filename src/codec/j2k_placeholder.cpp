// SIAARA DICOM Toolkit - TEMPORARY placeholder for the JPEG 2000 codec so the library links while
// the real codec is developed on its own branch. Delete this file when that codec is merged.
#include "codec/codec.hpp"

namespace siaara::codec {

Status decode_j2k(std::span<const std::uint8_t>, const FrameInfo&, std::span<std::uint8_t>) {
    return Status(Errc::Unsupported, "jpeg 2000: codec not available in this build");
}

Status encode_j2k(std::span<const std::uint8_t>, const FrameInfo&, const EncodeParams&, std::vector<std::uint8_t>& out) {
    out.clear();
    return Status(Errc::Unsupported, "jpeg 2000: codec not available in this build");
}

}  // namespace siaara::codec
