// Test-only helper for tests/crosscheck/tls_crosscheck.py (not installed).
//   tls_tool https PORT CERT KEY [CA] [FILE...]
//       DICOMweb server on https://127.0.0.1:PORT/dicomweb (in-memory store preloaded with FILEs)
//       using the PEM certificate chain CERT and key KEY; when CA is not "-", clients must present
//       a certificate issued by CA (mutual TLS). Prints "listening PORT" and runs until stdin closes.
//   tls_tool info
//       Prints the TLS backend in use, or why TLS is unavailable (exit status 1).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/io.hpp"
#include "net/tls.hpp"
#include "web/server.hpp"
#include "web_memory_store.hpp"

using namespace siaara;
using namespace siaara::web;

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "info") == 0) {
        std::string why;
        const bool ok = net::tls_available(&why);
        std::printf("%s\n", why.c_str());
        return ok ? 0 : 1;
    }
    if (argc < 5 || std::strcmp(argv[1], "https") != 0) {
        std::fputs("usage: tls_tool https PORT CERT KEY [CA|-] [FILE...] | tls_tool info\n", stderr);
        return 2;
    }
    siaara_test_web::MemoryStore mem;
    for (int i = 6; i < argc; ++i) {
        auto b = io::read_file(argv[i]);
        if (!b || !mem.add_part10(*b)) std::fprintf(stderr, "skipped %s\n", argv[i]);
    }
    DicomwebServerConfig cfg;
    cfg.http.bind_address = "127.0.0.1";
    cfg.http.port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    cfg.http.tls.enabled = true;
    cfg.http.tls.identity.certificate_file = argv[3];
    cfg.http.tls.identity.private_key_file = argv[4];
    if (argc >= 6 && std::strcmp(argv[5], "-") != 0) {
        cfg.http.tls.ca_file = argv[5];
        cfg.http.tls.require_client_certificate = true;
    }
    cfg.http.log = [](const std::string& s) { std::fprintf(stderr, "tls_tool: %s\n", s.c_str()); };
    if (cfg.http.port != 0) cfg.public_base_url = "https://127.0.0.1:" + std::to_string(cfg.http.port) + "/dicomweb";
    DicomwebServer server;
    if (Status st = server.start(cfg, mem.make_store()); !st) {
        std::fprintf(stderr, "error: %s\n", st.message.c_str());
        return 1;
    }
    std::printf("listening %u\n", static_cast<unsigned>(server.port()));
    std::fflush(stdout);
    while (std::fgetc(stdin) != EOF) {
    }
    server.stop();
    return 0;
}
