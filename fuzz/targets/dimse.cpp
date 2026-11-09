// Fuzz target: DIMSE command set decoder (PS3.7 9.3, 10.3; Implicit VR Little Endian). A decoded
// command must re-encode and decode to the same encoding.
#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "net/dimse.hpp"

using namespace siaara;
using namespace siaara::net;

SIAARA_FUZZ_TARGET(dimse) {
    const Bytes raw = siaara_fuzz::to_bytes(data, size);
    (void)command_has_dataset(raw);
    auto c = decode_command(raw);
    if (!c) return 0;
    (void)validate_command(*c);
    std::vector<std::uint8_t> e1;
    if (!encode_command(*c, e1)) return 0;
    auto d = decode_command(Bytes::copy_of(e1.data(), e1.size()));
    FUZZ_REQUIRE(d.is_ok());
    std::vector<std::uint8_t> e2;
    FUZZ_REQUIRE(encode_command(*d, e2).is_ok());
    FUZZ_REQUIRE(e1 == e2);
    if (!c->is_response()) (void)make_response(*c, 0xC000, false);
    return 0;
}
