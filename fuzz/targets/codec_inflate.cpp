// Fuzz target: raw DEFLATE decoder (RFC 1951) with an output cap; also checks that deflating the
// input at a level chosen by its first byte inflates back to the same bytes.
#include "codec/deflate.hpp"
#include "fuzz.hpp"

using namespace siaara;

SIAARA_FUZZ_TARGET(codec_inflate) {
    std::vector<std::uint8_t> out;
    std::size_t used = 0;
    if (codec::inflate_raw(data, size, 8u << 20, out, &used)) FUZZ_REQUIRE(used <= size);
    // Bounded output: a stream that inflates past the cap must fail, not write past it.
    out.clear();
    if (codec::inflate_raw(data, size, 64, out)) FUZZ_REQUIRE(out.size() <= 64);
    if (size > 0 && size <= 65536) {
        std::vector<std::uint8_t> z, back;
        FUZZ_REQUIRE(codec::deflate_raw(data, size, data[0] % 10, z).is_ok());
        FUZZ_REQUIRE(codec::inflate_raw(z.data(), z.size(), size, back).is_ok());
        FUZZ_REQUIRE(back.size() == size && std::memcmp(back.data(), data, size) == 0);
    }
    return 0;
}
