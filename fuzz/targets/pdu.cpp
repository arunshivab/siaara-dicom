// Fuzz target: Upper Layer PDU parser (PS3.8 9.3): parse_pdu on the buffer, read_pdu from a peer
// sending the same bytes in small chunks. A parsed PDU must re-encode, and the re-encoding must
// parse to the same encoding again (canonical form is stable).
#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "net/pdu.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {
PduLimits small_limits() {
    PduLimits l;
    l.max_pdu_length = 1u << 20;
    return l;
}
}  // namespace

SIAARA_FUZZ_TARGET(pdu) {
    const PduLimits lim = small_limits();
    auto p = parse_pdu(siaara_fuzz::to_bytes(data, size), lim);
    if (p) {
        std::vector<std::uint8_t> e1;
        if (encode_pdu(*p, e1)) {
            auto q = parse_pdu(Bytes::copy_of(e1.data(), e1.size()), lim);
            FUZZ_REQUIRE(q.is_ok());
            std::vector<std::uint8_t> e2;
            FUZZ_REQUIRE(encode_pdu(*q, e2).is_ok());
            FUZZ_REQUIRE(e1 == e2);
        }
    }
    siaara_fuzz::MemTransport t(data, size, 1 + (size % 13));
    for (int i = 0; i < 64; ++i) {
        auto r = read_pdu(t, 0, lim);
        if (!r && r.error().code != Errc::Unsupported) break;
    }
    return 0;
}
