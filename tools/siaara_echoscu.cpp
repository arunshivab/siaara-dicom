// siaara-echoscu: sends C-ECHO requests (Verification SOP Class, PS3.4 Annex A) to an SCP.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net/scu.hpp"
#include "net/tls.hpp"
#include "tool_help.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

// ---- TLS options (PS3.15 B.12/B.13; DIMSE over TLS uses its own port, 2762 is registered)
std::string env_value(const char* name) {
#if defined(_MSC_VER)
    char* v = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) return std::string();
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

const char* const kTlsUsage =
    "TLS options:\n"
    "  --tls                 use TLS (default port for DIMSE over TLS: 2762)\n"
    "  --tls-ca FILE         PEM trust anchors (default: the OS trust store)\n"
    "  --tls-cert FILE       PEM certificate chain (Linux)\n"
    "  --tls-key FILE        PEM private key (Linux)\n"
    "  --tls-p12 FILE        PKCS#12 identity (all systems)\n"
    "  --tls-password-env V  read the PKCS#12 / key password from environment variable V\n"
    "  --tls-os-identity ID  Windows: certificate thumbprint in CurrentUser\\MY; macOS: keychain label\n"
    "  --tls13               TLS 1.3 only (default: TLS 1.2 or 1.3)\n"
    "  --tls-insecure        DANGEROUS: do not validate the peer certificate (warned per connection)\n";

// Returns 1 when argv[i] was a TLS option (i advanced past its value), 0 when it is not one,
// -1 when its value is missing or unusable.
int parse_tls_option(int argc, char** argv, int& i, siaara::net::TlsSettings& t) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    auto value = [&](std::string& dst) {
        if (!more) return -1;
        dst = argv[++i];
        return 1;
    };
    if (a == "--tls") {
        t.enabled = true;
        return 1;
    }
    if (a == "--tls-ca") return value(t.ca_file);
    if (a == "--tls-cert") return value(t.identity.certificate_file);
    if (a == "--tls-key") return value(t.identity.private_key_file);
    if (a == "--tls-p12") return value(t.identity.pkcs12_file);
    if (a == "--tls-name") return value(t.expected_identity);
    if (a == "--tls-os-identity") {
#if defined(_WIN32)
        return value(t.identity.windows_thumbprint);
#else
        return value(t.identity.keychain_identity);
#endif
    }
    if (a == "--tls-password-env") {
        std::string name;
        if (value(name) < 0) return -1;
        t.identity.password = env_value(name.c_str());
        return t.identity.password.empty() ? -1 : 1;
    }
    if (a == "--tls13") {
        t.min_version = siaara::net::TlsMinVersion::Tls13;
        return 1;
    }
    if (a == "--tls-insecure") {
        t.insecure_skip_validation = true;
        return 1;
    }
    if (a == "--tls-require-client-cert") {
        t.require_client_certificate = true;
        return 1;
    }
    return 0;
}

void usage() {
    std::fprintf(siaara_tools::usage_stream(),
                 "usage: siaara-echoscu [options] host port\n"
                 "  -aet AE        calling AE title (default SIAARA_ECHO)\n"
                 "  -aec AE        called AE title (default ANY-SCP)\n"
                 "  -t MS          timeout for connect/ACSE/DIMSE in milliseconds (default 15000)\n"
                 "  -r N           number of C-ECHO requests on the association (default 1)\n"
                 "  --max-pdu N    maximum PDU length to receive (default 65536)\n"
                 "  -q             quiet\n"
                 "  --tls-name N   name or IP the server certificate must carry (default: host)\n"
                 "  -v             log TLS details to siaara_tools::usage_stream()\n");
    std::fputs(kTlsUsage, siaara_tools::usage_stream());
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
    if (siaara_tools::wants_help(argc, argv)) {
        usage();
        return 0;
    }
    ScuConfig cfg;
    cfg.calling_ae = "SIAARA_ECHO";
    unsigned long repeat = 1, timeout = 15000, max_pdu = 65536;
    bool quiet = false, verbose = false;
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        const int tls_opt = std::strcmp(a, "--tls-require-client-cert") ? parse_tls_option(argc, argv, i, cfg.tls) : -1;
        if (tls_opt < 0) {
            usage();
            return 2;
        }
        if (tls_opt > 0) continue;
        if (!std::strcmp(a, "-aet") && more) cfg.calling_ae = argv[++i];
        else if (!std::strcmp(a, "-aec") && more) cfg.called_ae = argv[++i];
        else if (!std::strcmp(a, "-t") && more && parse_uint(argv[++i], 3600000, timeout)) {}
        else if (!std::strcmp(a, "-r") && more && parse_uint(argv[++i], 100000, repeat)) {}
        else if (!std::strcmp(a, "--max-pdu") && more && parse_uint(argv[++i], 0xFFFFFFFFul, max_pdu)) {}
        else if (!std::strcmp(a, "-q")) quiet = true;
        else if (!std::strcmp(a, "-v")) verbose = true;
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
    cfg.tls.handshake_timeout_ms = t;
    cfg.tls.log = [verbose](const std::string& s) {
        if (verbose || s.rfind("WARNING", 0) == 0) std::fprintf(stderr, "siaara-echoscu: %s\n", s.c_str());
    };

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
