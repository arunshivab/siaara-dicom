// SIAARA DICOM Toolkit - JPEG 2000 encoder (work in progress).
#include "codec/j2k.hpp"
#include "codec/j2k_internal.hpp"

namespace siaara::codec {

Status j2k::encode(std::span<const std::uint8_t>, const FrameInfo&, const EncodeOptions&, std::vector<std::uint8_t>&) {
    return Status(Errc::Unsupported, "JPEG 2000: encoder not yet implemented");
}

Status encode_j2k(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                  std::vector<std::uint8_t>& out) {
    j2k::EncodeOptions o;
    o.lossless = p.lossless;
    return j2k::encode(in, info, o, out);
}

}  // namespace siaara::codec
