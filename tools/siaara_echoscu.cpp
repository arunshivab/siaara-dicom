// siaara-echoscu: sends C-ECHO requests (Verification SOP Class, PS3.4 Annex A) to an SCP.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net/scu.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: siaara-echoscu [options] host port\n"
                 "  -aet AE        calling AE title (default SIAARA_ECHO)\n"
                 "  -aec AE        called AE title (default ANY-SCP)\n"
                 "  -t MS          timeout for connect/ACSE/DIMSE in milliseconds (default 15000)\n"
                 "  -r N           number of C-ECHO requests on the association (default 1)\n"
                 "  --max-pdu N    maximum PDU length to receive (default 65536)\n"
                 "  -q             quiet\n");
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!s[0] || *end || v > max) return false;
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ScuConfig cfg;
    cfg.calling_ae = "SIAARA_ECHO";
    unsigned long repeat = 1, timeout = 15000, max_pdu = 65536;
    bool quiet = false;
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        if (!std::strcmp(a, "-aet") && more) cfg.calling_ae = argv[++i];
        else if (!std::strcmp(a, "-aec") && more) cfg.called_ae = argv[++i];
        else if (!std::strcmp(a, "-t") && more && parse_uint(argv[++i], 3600000, timeout)) {}
        else if (!std::strcmp(a, "-r") && more && parse_uint(argv[++i], 100000, repeat)) {}
        else if (!std::strcmp(a, "--max-pdu") && more && parse_uint(argv[++i], 0xFFFFFFFFul, max_pdu)) {}
        else if (!std::strcmp(a, "-q")) quiet = true;
        else if (a[0] != '-' && npos < 2) pos[npos++] = a;
        else {
            usage();
            return 2;
        }
    }
    unsigned long port = 0;
    if (npos != 2 || !parse_uint(pos[1], 65535, port)) {
        usage();
        return 2;
    }
    cfg.host = pos[0];
    cfg.port = static_cast<std::uint16_t>(port);
    const int t = static_cast<int>(timeout);
    cfg.timeouts.connect_ms = t;
    cfg.timeouts.acse_ms = t;
    cfg.timeouts.dimse_ms = t;
    cfg.limits.max_receive_pdu = static_cast<std::uint32_t>(max_pdu);

    Scu scu(cfg);
    scu.add_verification();
    AssociateRj rj;
    if (Status s = scu.connect(&rj); !s) {
        if (s.code == Errc::Rejected)
            std::fprintf(stderr, "association rejected: result %u source %u reason %u\n", rj.result, rj.source, rj.reason);
        else
            std::fprintf(stderr, "association failed: %s\n", s.message.c_str());
        return 1;
    }
    int failures = 0;
    for (unsigned long i = 0; i < repeat; ++i) {
        Result<std::uint16_t> r = scu.echo();
        if (!r) {
            std::fprintf(stderr, "C-ECHO failed: %s\n", r.error().message.c_str());
            return 1;
        }
        if (*r != 0) ++failures;
        if (!quiet) std::printf("C-ECHO-RSP status 0x%04X\n", *r);
    }
    if (Status s = scu.release(); !s) {
        std::fprintf(stderr, "release failed: %s\n", s.message.c_str());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
