// TLS (PS3.15 B.12 / B.13) over 127.0.0.1 with throwaway certificates made at test time by the
// `openssl` command line tool (skipped with a message when it is not installed; no key is ever
// committed). DIMSE echo/store with server authentication and mutual TLS, host name and trust
// failures, the insecure flag, plaintext peers, timeouts, protocol/cipher refusal with
// `openssl s_client` as a negative oracle, DICOMweb over https, and the C API.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "crosscheck/web_memory_store.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/socket.hpp"
#include "net/tls.hpp"
#include "siaara/siaara.h"
#include "web/client.hpp"
#include "web/server.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

namespace fs = std::filesystem;
constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";
constexpr const char* kP12Password = "siaara-test-p12";
// PKCS#12 encryption every OS importer reads (OpenSSL 3 defaults to AES/PBKDF2, which older
// Windows and macOS importers reject).
constexpr const char* kP12Algorithms = " -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1";

#if defined(_WIN32)
constexpr const char* kQuiet = " >NUL 2>&1";
constexpr const char* kNoInput = " <NUL";
#else
constexpr const char* kQuiet = " >/dev/null 2>&1";
constexpr const char* kNoInput = " </dev/null";
#endif

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

bool run(const std::string& cmd) { return std::system((cmd + kQuiet).c_str()) == 0; }

bool write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
    return static_cast<bool>(f);
}

// Throwaway PKI in a temporary folder, removed when the process exits.
struct Pki {
    bool ok = false;
    std::string why;
    fs::path dir;
    fs::path ca, ca_key, server, server_key, server_p12, client, client_key, client_p12, client_key_enc;
    fs::path self_signed, self_signed_key, self_signed_p12, expired, expired_key, expired_p12;
    fs::path other_ca;

    Pki() {
        if (!run("openssl version")) {
            why = "the openssl command line tool is not installed";
            return;
        }
        std::error_code ec;
        dir = fs::temp_directory_path(ec) / ("siaara_tls_" + std::to_string(static_cast<unsigned long long>(
                                                  std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(dir, ec);
        if (ec) {
            why = "cannot create a temporary folder";
            return;
        }
        auto p = [&](const char* n) { return dir / n; };
        ca = p("ca.pem");
        ca_key = p("ca.key");
        other_ca = p("other_ca.pem");
        // A private request config: the system openssl.cnf is not used. (LibreSSL, the macOS
        // `openssl`, adds its default v3_ca extensions and appends -addext ones, producing
        // duplicate basicConstraints that macOS rejects as "not standards compliant".)
        if (!write_text(p("req.cnf"), "[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=placeholder\n"
                                      "[ca_ext]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
                                      "subjectKeyIdentifier=hash\n"
                                      "[self_ext]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                                      "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n"
                                      "subjectKeyIdentifier=hash\n")) {
            why = "cannot write the request config";
            return;
        }
        const std::string ec_key =
            " -config " + q(p("req.cnf")) + " -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -sha256";
        const std::string ca_ext = " -extensions ca_ext";
        if (!run("openssl req -x509" + ec_key + " -keyout " + q(ca_key) + " -out " + q(ca) +
                 " -days 3 -subj \"/CN=SIAARA Test CA\"" + ca_ext) ||
            !run("openssl req -x509" + ec_key + " -keyout " + q(p("other_ca.key")) + " -out " + q(other_ca) +
                 " -days 3 -subj \"/CN=SIAARA Other CA\"" + ca_ext)) {
            why = "openssl req failed";
            return;
        }
        if (!write_text(p("server.ext"), "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                                         "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n"
                                         "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n") ||
            !write_text(p("client.ext"), "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                                         "extendedKeyUsage=clientAuth\nsubjectAltName=DNS:siaara-scu.test\n"
                                         "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n")) {
            why = "cannot write extension files";
            return;
        }
        auto leaf = [&](const char* name, const char* cn, const char* ext, int serial, fs::path& cert, fs::path& key,
                        fs::path& p12) {
            cert = dir / (std::string(name) + ".pem");
            key = dir / (std::string(name) + ".key");
            p12 = dir / (std::string(name) + ".p12");
            const fs::path csr = dir / (std::string(name) + ".csr");
            return run("openssl req" + ec_key + " -keyout " + q(key) + " -out " + q(csr) + " -subj \"/CN=" + cn + "\"") &&
                   run("openssl x509 -req -sha256 -in " + q(csr) + " -CA " + q(ca) + " -CAkey " + q(ca_key) + " -set_serial " +
                       std::to_string(serial) + " -days 3 -out " + q(cert) + " -extfile " + q(dir / ext)) &&
                   run("openssl pkcs12 -export -in " + q(cert) + " -inkey " + q(key) + " -certfile " + q(ca) +
                       " -out " + q(p12) + " -passout pass:" + kP12Password + kP12Algorithms);
        };
        if (!leaf("server", "localhost", "server.ext", 2, server, server_key, server_p12) ||
            !leaf("client", "SIAARA_SCU", "client.ext", 3, client, client_key, client_p12)) {
            why = "issuing the test certificates failed";
            return;
        }
        client_key_enc = p("client_enc.key");
        (void)run("openssl pkcs8 -topk8 -in " + q(client_key) + " -out " + q(client_key_enc) +
                  " -v2 aes-256-cbc -passout pass:key-password");
        // Self-signed server certificate (not issued by the test CA).
        self_signed = p("self.pem");
        self_signed_key = p("self.key");
        self_signed_p12 = p("self.p12");
        if (!run("openssl req -x509" + ec_key + " -keyout " + q(self_signed_key) + " -out " + q(self_signed) +
                 " -days 3 -subj \"/CN=localhost\" -extensions self_ext") ||
            !run("openssl pkcs12 -export -in " + q(self_signed) + " -inkey " + q(self_signed_key) + " -out " +
                 q(self_signed_p12) + " -passout pass:" + kP12Password + kP12Algorithms)) {
            why = "making the self-signed certificate failed";
            return;
        }
        // Expired server certificate issued by the test CA (openssl ca sets past dates).
        expired = p("expired.pem");
        expired_key = p("expired.key");
        expired_p12 = p("expired.p12");
        const std::string d = dir.generic_string();
        (void)write_text(p("index.txt"), "");
        (void)write_text(p("serial"), "10\n");
        (void)write_text(p("ca.cnf"), "[ca]\ndefault_ca=d\n[d]\ndatabase=" + d + "/index.txt\nnew_certs_dir=" + d +
                                          "\nserial=" + d + "/serial\ndefault_md=sha256\npolicy=p\n"
                                          "unique_subject=no\n[p]\ncommonName=supplied\n");
        if (!run("openssl req" + ec_key + " -keyout " + q(expired_key) + " -out " + q(p("expired.csr")) +
                 " -subj \"/CN=localhost\"") ||
            !run("openssl ca -batch -notext -config " + q(p("ca.cnf")) + " -cert " + q(ca) + " -keyfile " + q(ca_key) +
                 " -in " + q(p("expired.csr")) + " -out " + q(expired) +
                 " -startdate 20200101000000Z -enddate 20200201000000Z -extfile " + q(p("server.ext"))) ||
            !run("openssl pkcs12 -export -in " + q(expired) + " -inkey " + q(expired_key) + " -out " + q(expired_p12) +
                 " -passout pass:" + kP12Password + kP12Algorithms)) {
            why = "making the expired certificate failed";
            return;
        }
        ok = true;
    }
    ~Pki() {
        if (!dir.empty()) {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    }
};

Pki& pki() {
    static Pki p;
    return p;
}

// Prints why a test is skipped (once per test) and returns true when it must be skipped.
bool skip(const char* test) {
    std::string why;
    if (!tls_available(&why)) {
        std::printf("  SKIPPED %s: TLS is not available (%s)\n", test, why.c_str());
        return true;
    }
    if (!pki().ok) {
        std::printf("  SKIPPED %s: %s\n", test, pki().why.c_str());
        return true;
    }
    return false;
}

// PEM files with OpenSSL; PKCS#12 on Windows and macOS (their backends do not read PEM keys).
TlsIdentity identity(const fs::path& cert, const fs::path& key, const fs::path& p12) {
    TlsIdentity id;
#if defined(_WIN32) || defined(__APPLE__)
    (void)cert;
    (void)key;
    id.pkcs12_file = p12.string();
    id.password = kP12Password;
#else
    (void)p12;
    id.certificate_file = cert.string();
    id.private_key_file = key.string();
#endif
    return id;
}

struct LogSink {
    std::mutex mu;
    std::vector<std::string> lines;
    std::function<void(const std::string&)> fn() {
        return [this](const std::string& s) {
            std::lock_guard<std::mutex> l(mu);
            lines.push_back(s);
        };
    }
    bool contains(const std::string& needle) {
        std::lock_guard<std::mutex> l(mu);
        for (const std::string& s : lines)
            if (s.find(needle) != std::string::npos) return true;
        return false;
    }
    std::size_t count(const std::string& needle) {
        std::lock_guard<std::mutex> l(mu);
        std::size_t n = 0;
        for (const std::string& s : lines)
            if (s.find(needle) != std::string::npos) ++n;
        return n;
    }
};

Timeouts fast() {
    Timeouts t;
    t.connect_ms = 5000;
    t.acse_ms = 5000;
    t.dimse_ms = 5000;
    t.artim_ms = 3000;
    t.idle_ms = 10000;
    return t;
}

struct TlsScp {
    LogSink log;
    std::atomic<int> stored{0};
    std::vector<std::uint8_t> last_pixels;
    std::mutex mu;
    std::unique_ptr<ScpServer> server;

    explicit TlsScp(TlsSettings tls, bool use_tls = true) {
        ScpConfig c;
        c.ae_title = "TLS_SCP";
        c.bind_address = "127.0.0.1";
        c.port = 0;
        c.max_associations = 4;
        c.timeouts = fast();
        c.tls = std::move(tls);
        c.tls.enabled = use_tls;
        c.tls.handshake_timeout_ms = 3000;
        ScpHandlers h;
        h.log = log.fn();
        h.on_store = [this](const StoreRequest& rq) -> std::uint16_t {
            if (const Element* e = rq.dataset.find(tags::PixelData)) {
                std::lock_guard<std::mutex> l(mu);
                last_pixels.assign(e->value.data(), e->value.data() + e->value.size());
            }
            stored.fetch_add(1);
            return dimse_status::Success;
        };
        server = std::make_unique<ScpServer>(c, h);
    }
    Status start() { return server->start(); }
    std::uint16_t port() const { return server->port(); }
};

TlsSettings server_tls() {
    TlsSettings s;
    s.enabled = true;
    s.identity = identity(pki().server, pki().server_key, pki().server_p12);
    return s;
}

TlsSettings client_tls() {
    TlsSettings s;
    s.enabled = true;
    s.ca_file = pki().ca.string();
    s.handshake_timeout_ms = 3000;
    return s;
}

ScuConfig scu(std::uint16_t port, TlsSettings tls, const std::string& host = "127.0.0.1") {
    ScuConfig c;
    c.calling_ae = "TLS_SCU";
    c.called_ae = "TLS_SCP";
    c.host = host;
    c.port = port;
    c.timeouts = fast();
    c.tls = std::move(tls);
    return c;
}

Status echo(const ScuConfig& c) {
    Scu u(c);
    u.add_verification();
    if (Status s = u.connect(); !s) return s;
    Result<std::uint16_t> r = u.echo();
    if (!r) return r.take_error();
    if (*r != 0) return Status(Errc::Rejected, "echo status");
    return u.release();
}

DicomFile instance(std::size_t pixel_bytes) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kCT);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.4242");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.4240");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.4241");
    (void)ds.set_string(tags::PatientID, VR::LO, "TLS1");
    (void)ds.set_uint16(tags::Rows, 1);
    (void)ds.set_uint16(tags::Columns, static_cast<std::uint16_t>(pixel_bytes / 2 > 0xFFFF ? 0xFFFF : pixel_bytes / 2));
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    std::vector<std::uint8_t> px(pixel_bytes);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>((k * 131) ^ (k >> 8));
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    return f;
}

// `openssl s_client` against a port; returns its exit status == 0 (handshake completed).
bool s_client(std::uint16_t port, const std::string& args) {
    return std::system(("openssl s_client -connect 127.0.0.1:" + std::to_string(port) + " " + args + kNoInput + kQuiet)
                           .c_str()) == 0;
}

}  // namespace

TEST_CASE("tls/availability and settings validation") {
    std::string why;
    const bool avail = tls_available(&why);
    std::printf("  TLS backend: %s (%s)\n", tls_backend_name().c_str(), avail ? "available" : why.c_str());
    CHECK_EQ(siaara_tls_available(), avail ? 1 : 0);
    if (!avail) {
        CHECK(std::strlen(siaara_last_error_message()) > 0);
        return;
    }
    // A server needs an identity; contradictory identity sources are refused.
    TlsSettings s;
    s.enabled = true;
    CHECK_EQ(TlsContext::create(s, TlsContext::Role::Server).error().code, Errc::InvalidArgument);
    s.identity.certificate_file = "x.pem";
    CHECK_EQ(TlsContext::create(s, TlsContext::Role::Client).error().code, Errc::InvalidArgument);
    s.identity.private_key_file = "x.key";
    s.identity.pkcs12_file = "x.p12";
    CHECK_EQ(TlsContext::create(s, TlsContext::Role::Client).error().code, Errc::InvalidArgument);
    s = {};
    s.handshake_timeout_ms = 0;
    CHECK_EQ(TlsContext::create(s, TlsContext::Role::Client).error().code, Errc::InvalidArgument);
    // Missing files are reported, not ignored.
    s = {};
    s.ca_file = "/nonexistent/siaara-ca.pem";
    CHECK(!TlsContext::create(s, TlsContext::Role::Client).is_ok());
    // TLS settings with an http:// DICOMweb URL: refused (no silent plaintext).
    web::DicomwebClientConfig wc;
    wc.base_url = "http://127.0.0.1:1/dicomweb";
    wc.tls.enabled = true;
    CHECK_EQ(web::DicomwebClient(wc).status().code, Errc::InvalidArgument);
    auto u = web::http::parse_url("https://example.test/dicomweb");
    REQUIRE(u.is_ok());
    CHECK_EQ(u->port, 443);
    CHECK_EQ(u->scheme, std::string("https"));
}

TEST_CASE("tls/dimse echo and store with server authentication") {
    if (skip("tls/dimse echo")) return;
    TlsScp scp(server_tls());
    REQUIRE(scp.start().is_ok());
    // Address in the IP subjectAltName.
    LogSink clog;
    TlsSettings ct = client_tls();
    ct.log = clog.fn();
    Status st = echo(scu(scp.port(), ct));
    if (!st) {
        std::printf("  echo: %s\n", st.message.c_str());
        // Diagnostics for the next CI run: the oracle's version and the test CA as it was made.
        std::fflush(stdout);
        const int v = std::system("openssl version");
        const int x = std::system(("openssl x509 -noout -text -in " + q(pki().ca)).c_str());
        std::printf("  (diagnostic commands exited %d, %d)\n", v, x);
        std::fflush(stdout);
    }
    CHECK(st.is_ok());
    CHECK(clog.contains("TLSv1.3"));
    CHECK(clog.contains("server certificate verified"));
    // DNS name in the subjectAltName, checked through expected_identity.
    ct.expected_identity = "localhost";
    CHECK(echo(scu(scp.port(), ct)).is_ok());
    // TLS 1.3 only, and TLS 1.2 forced by the server.
    ct.min_version = TlsMinVersion::Tls13;
    CHECK(echo(scu(scp.port(), ct)).is_ok());
    // C-STORE of 3 MB (many records and PDUs) arrives intact.
    Scu u(scu(scp.port(), client_tls()));
    u.add_context(kCT, {std::string(ts::ExplicitVRLittleEndian)});
    REQUIRE(u.connect().is_ok());
    const DicomFile f = instance(3u << 20);
    Result<std::uint16_t> r = u.store(f);
    REQUIRE(r.is_ok());
    CHECK_EQ(*r, 0);
    CHECK(u.release().is_ok());
    CHECK_EQ(scp.stored.load(), 1);
    {
        std::lock_guard<std::mutex> l(scp.mu);
        const Element* e = f.dataset.find(tags::PixelData);
        REQUIRE(e != nullptr);
        CHECK(scp.last_pixels.size() == e->value.size() &&
              std::memcmp(scp.last_pixels.data(), e->value.data(), e->value.size()) == 0);
    }
    CHECK(scp.log.contains("TLS: accepted"));
}

TEST_CASE("tls/dimse TLS 1.2 and TLS 1.3 clients (openssl s_client)") {
    if (skip("tls/versions")) return;
    TlsScp scp(server_tls());
    REQUIRE(scp.start().is_ok());
    CHECK(s_client(scp.port(), "-tls1_2 -CAfile " + q(pki().ca) + " -verify_return_error"));
    CHECK(s_client(scp.port(), "-tls1_3 -CAfile " + q(pki().ca) + " -verify_return_error"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(scp.log.contains("TLSv1.2, TLS_ECDHE_ECDSA_WITH_"));
    CHECK(scp.log.contains("TLSv1.3, TLS_"));
}

TEST_CASE("tls/dimse protocol and cipher refusal (openssl s_client negative oracle)") {
    if (skip("tls/refusal")) return;
    TlsScp scp(server_tls());
    REQUIRE(scp.start().is_ok());
    // TLS 1.1 and 1.0 are refused (RFC 8996). SECLEVEL=0 so the oracle itself offers them.
    CHECK(!s_client(scp.port(), "-tls1_1 -cipher DEFAULT@SECLEVEL=0"));
    CHECK(!s_client(scp.port(), "-tls1 -cipher DEFAULT@SECLEVEL=0"));
    // TLS 1.2 with only non-AEAD (CBC) or non-ECDHE suites: refused.
    CHECK(!s_client(scp.port(), "-tls1_2 -cipher ECDHE-ECDSA-AES128-SHA256"));
    CHECK(!s_client(scp.port(), "-tls1_2 -cipher ECDHE-ECDSA-AES128-SHA"));
    CHECK(!s_client(scp.port(), "-tls1_2 -cipher AES128-GCM-SHA256"));
    // The server keeps serving after the refusals.
    CHECK(echo(scu(scp.port(), client_tls())).is_ok());
}

TEST_CASE("tls/dimse mutual TLS") {
    if (skip("tls/dimse mutual TLS")) return;
    TlsSettings st = server_tls();
    st.require_client_certificate = true;
    st.ca_file = pki().ca.string();
    TlsScp scp(st);
    REQUIRE(scp.start().is_ok());
    // No client certificate: refused.
    Status no_cert = echo(scu(scp.port(), client_tls()));
    CHECK(!no_cert.is_ok());
    // With the client certificate: accepted and verified.
    TlsSettings ct = client_tls();
    ct.identity = identity(pki().client, pki().client_key, pki().client_p12);
    Status with_cert = echo(scu(scp.port(), ct));
    if (!with_cert) std::printf("  mutual TLS echo: %s\n", with_cert.message.c_str());
    CHECK(with_cert.is_ok());
    CHECK(scp.log.contains("client certificate verified"));
    // A client certificate from another CA: refused.
    TlsSettings other = client_tls();
    other.identity = identity(pki().self_signed, pki().self_signed_key, pki().self_signed_p12);
    CHECK(!echo(scu(scp.port(), other)).is_ok());
    // The oracle client without a certificate is refused, with one it is accepted. (TLS 1.2: with
    // TLS 1.3 the client finishes its side of the handshake before the server's alert arrives.)
    if (run("openssl version")) {
        CHECK(!s_client(scp.port(), "-tls1_2 -CAfile " + q(pki().ca) + " -verify_return_error"));
        CHECK(s_client(scp.port(), "-CAfile " + q(pki().ca) + " -cert " + q(pki().client) + " -key " + q(pki().client_key)));
    }
}

TEST_CASE("tls/dimse host name and trust failures, insecure flag") {
    if (skip("tls/validation")) return;
    TlsScp good(server_tls());
    REQUIRE(good.start().is_ok());
    // Wrong expected host name.
    TlsSettings ct = client_tls();
    ct.expected_identity = "pacs.other.example";
    Status wrong = echo(scu(good.port(), ct));
    CHECK(!wrong.is_ok());
    std::printf("  wrong host name: %s\n", wrong.message.c_str());
    // Trust anchors that did not issue the server certificate.
    ct = client_tls();
    ct.ca_file = pki().other_ca.string();
    CHECK(!echo(scu(good.port(), ct)).is_ok());
    // Self-signed server certificate.
    TlsSettings ss;
    ss.enabled = true;
    ss.identity = identity(pki().self_signed, pki().self_signed_key, pki().self_signed_p12);
    TlsScp self(ss);
    REQUIRE(self.start().is_ok());
    Status untrusted = echo(scu(self.port(), client_tls()));
    CHECK(!untrusted.is_ok());
    std::printf("  self-signed: %s\n", untrusted.message.c_str());
    // Expired server certificate.
    TlsSettings ex;
    ex.enabled = true;
    ex.identity = identity(pki().expired, pki().expired_key, pki().expired_p12);
    TlsScp expired(ex);
    REQUIRE(expired.start().is_ok());
    Status old = echo(scu(expired.port(), client_tls()));
    CHECK(!old.is_ok());
    std::printf("  expired: %s\n", old.message.c_str());
    // insecure_skip_validation: accepted, and a warning is logged for every connection.
    LogSink warn;
    TlsSettings insecure = client_tls();
    insecure.insecure_skip_validation = true;
    insecure.log = warn.fn();
    CHECK(echo(scu(self.port(), insecure)).is_ok());
    CHECK(echo(scu(expired.port(), insecure)).is_ok());
    insecure.expected_identity = "pacs.other.example";
    CHECK(echo(scu(good.port(), insecure)).is_ok());
    CHECK_EQ(warn.count("WARNING: TLS certificate validation is disabled"), 3u);
    CHECK(warn.contains("server certificate NOT verified"));
}

TEST_CASE("tls/dimse plaintext peers, no fallback, timeouts") {
    if (skip("tls/plaintext")) return;
    TlsScp tls_scp(server_tls());
    REQUIRE(tls_scp.start().is_ok());
    // Plain DIMSE client to the TLS port: fails cleanly and quickly.
    const auto t0 = std::chrono::steady_clock::now();
    TlsSettings off;
    Status plain = echo(scu(tls_scp.port(), off));
    CHECK(!plain.is_ok());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6));
    // TLS client to a plain port: fails, never falls back to plain TCP.
    TlsScp plain_scp(TlsSettings{}, false);
    REQUIRE(plain_scp.start().is_ok());
    Status no_fallback = echo(scu(plain_scp.port(), client_tls()));
    CHECK(!no_fallback.is_ok());
    std::printf("  TLS client to plain port: %s\n", no_fallback.message.c_str());
    CHECK_EQ(plain_scp.server->associations_handled(), 0u);
    // A peer that accepts TCP but never answers: the handshake times out.
    TcpListener silent;
    REQUIRE(silent.listen("127.0.0.1", 0).is_ok());
    TlsSettings ct = client_tls();
    ct.handshake_timeout_ms = 500;
    const auto t1 = std::chrono::steady_clock::now();
    Result<std::unique_ptr<TlsTransport>> hung = tls_connect(ct, "127.0.0.1", silent.port(), 2000);
    const auto took = std::chrono::steady_clock::now() - t1;
    CHECK(!hung.is_ok());
    if (!hung) CHECK_EQ(hung.error().code, Errc::Timeout);
    CHECK(took < std::chrono::seconds(3));
    // A client that connects to the TLS port and says nothing: the server gives up after its
    // handshake timeout and keeps serving.
    Result<std::unique_ptr<Transport>> idle = tcp_connect("127.0.0.1", tls_scp.port(), 2000);
    REQUIRE(idle.is_ok());
    CHECK(echo(scu(tls_scp.port(), client_tls())).is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    CHECK(tls_scp.log.contains("timed out"));
    // Stopping the server while a handshake is pending returns promptly.
    Result<std::unique_ptr<Transport>> idle2 = tcp_connect("127.0.0.1", tls_scp.port(), 2000);
    REQUIRE(idle2.is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t2 = std::chrono::steady_clock::now();
    tls_scp.server->stop();
    CHECK(std::chrono::steady_clock::now() - t2 < std::chrono::seconds(2));
}

TEST_CASE("tls/transport close_notify and orderly close") {
    if (skip("tls/close_notify")) return;
    // Server side by hand: accept, handshake, echo bytes back, then close (sends close_notify).
    Result<std::shared_ptr<TlsContext>> sctx = TlsContext::create(server_tls(), TlsContext::Role::Server);
    REQUIRE(sctx.is_ok());
    TlsListener l;
    REQUIRE(l.listen(sctx.value(), "127.0.0.1", 0).is_ok());
    std::atomic<bool> server_ok{false};
    std::thread th([&] {
        Result<std::unique_ptr<TlsTransport>> a = l.accept(5000);
        if (!a) return;
        std::unique_ptr<TlsTransport> t = std::move(a).value();
        if (!t->handshake(5000)) return;
        std::vector<std::uint8_t> buf(70000);
        if (!read_exact(*t, buf.data(), buf.size(), 5000)) return;
        if (!t->write_all(buf.data(), buf.size(), 5000)) return;
        t->close();
        server_ok = true;
    });
    Result<std::unique_ptr<TlsTransport>> c = tls_connect(client_tls(), "127.0.0.1", l.port(), 5000);
    REQUIRE(c.is_ok());
    std::unique_ptr<TlsTransport> t = std::move(c).value();
    CHECK(t->session().peer_verified);
    CHECK(t->session().cipher_suite != 0);
    std::vector<std::uint8_t> out(70000), back(70000);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(i * 7);
    REQUIRE(t->write_all(out.data(), out.size(), 5000).is_ok());
    CHECK(read_exact(*t, back.data(), back.size(), 5000).is_ok());
    CHECK(out == back);
    // The peer's close_notify reads as an orderly close (0), not an error.
    Result<bool> ready = t->wait_readable(5000);
    CHECK(ready.is_ok() && *ready);
    std::uint8_t one = 0;
    Result<std::size_t> eof = t->read_some(&one, 1, 5000);
    CHECK(eof.is_ok() && *eof == 0);
    th.join();
    CHECK(server_ok.load());
    t->close();
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("tls/openssl identities: PKCS#12 and encrypted PEM key") {
    if (skip("tls/identities")) return;
    TlsSettings st;
    st.enabled = true;
    st.identity.pkcs12_file = pki().server_p12.string();
    st.identity.password = kP12Password;
    st.require_client_certificate = true;
    st.ca_file = pki().ca.string();
    TlsScp scp(st);
    REQUIRE(scp.start().is_ok());
    TlsSettings ct = client_tls();
    ct.identity.certificate_file = pki().client.string();
    ct.identity.private_key_file = pki().client_key_enc.string();
    ct.identity.password = "key-password";
    CHECK(echo(scu(scp.port(), ct)).is_ok());
    ct.identity.password = "wrong";
    CHECK(!TlsContext::create(ct, TlsContext::Role::Client).is_ok());
    st.identity.password = "wrong";
    CHECK(!TlsContext::create(st, TlsContext::Role::Server).is_ok());
    TlsSettings store;
    store.identity.windows_thumbprint = "00";
    CHECK_EQ(TlsContext::create(store, TlsContext::Role::Client).error().code, Errc::Unsupported);
}
#endif

TEST_CASE("tls/dicomweb over https") {
    if (skip("tls/https")) return;
    siaara_test_web::MemoryStore mem;
    web::DicomwebServer server;
    web::DicomwebServerConfig sc;
    sc.http.bind_address = "127.0.0.1";
    sc.http.port = 0;
    sc.http.tls = server_tls();
    sc.http.tls.handshake_timeout_ms = 3000;
    LogSink slog;
    sc.http.log = slog.fn();
    REQUIRE(server.start(sc, mem.make_store()).is_ok());
    const std::string base = "https://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";

    web::DicomwebClientConfig cc;
    cc.base_url = base;
    cc.limits.read_timeout_ms = 10000;
    cc.tls.ca_file = pki().ca.string();
    web::DicomwebClient c(cc);
    REQUIRE(c.status().is_ok());
    const DicomFile f = instance(200000);
    std::vector<std::uint8_t> part10;
    REQUIRE(write_part10(f, part10).is_ok());
    auto stow = c.store({std::string(part10.begin(), part10.end())});
    REQUIRE(stow.is_ok());
    CHECK_EQ(stow->http_status, 200);
    auto studies = c.search_studies();
    REQUIRE(studies.is_ok());
    CHECK_EQ(studies->size(), 1u);
    auto got = c.retrieve_raw("2.25.4240", "2.25.4241", "2.25.4242", {"*"});
    REQUIRE(got.is_ok());
    REQUIRE(got->size() == 1u);
    CHECK(got->front() == std::string(part10.begin(), part10.end()));
    // Metadata RetrieveURLs and BulkDataURIs point at https://.
    auto meta = c.request("GET", "/studies/2.25.4240/metadata", {{"Accept", "application/dicom+json"}});
    REQUIRE(meta.is_ok());
    CHECK(meta->body.find("https://127.0.0.1:") != std::string::npos);
    CHECK(slog.contains("TLS: accepted"));

    // Untrusted server certificate: refused.
    web::DicomwebClientConfig bad = cc;
    bad.tls.ca_file = pki().other_ca.string();
    web::DicomwebClient b(bad);
    REQUIRE(b.status().is_ok());
    CHECK(!b.search_studies().is_ok());
    // Plain http:// to the https port: fails cleanly.
    web::DicomwebClientConfig plain;
    plain.base_url = "http://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";
    plain.limits.read_timeout_ms = 3000;
    web::DicomwebClient p(plain);
    CHECK(!p.search_studies().is_ok());
    // The oracle as an HTTPS client.
    if (run("openssl version")) CHECK(s_client(server.port(), "-CAfile " + q(pki().ca) + " -verify_return_error"));

    // C API: https client with options.
    siaara_tls_options o{};
    o.struct_size = sizeof o;
    const std::string ca = pki().ca.string();
    o.ca_file = ca.c_str();
    siaara_web_client* wc = nullptr;
    REQUIRE(siaara_web_client_create_tls(base.data(), base.size(), 10000, &o, &wc) == SIAARA_OK);
    std::vector<char> buf(1 << 16);
    std::size_t needed = 0;
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, nullptr, 0, buf.data(), buf.size(), &needed) ==
          SIAARA_OK);
    CHECK(std::string(buf.data(), needed).find("2.25.4240") != std::string::npos);
    siaara_web_client_free(wc);
    wc = nullptr;
    const std::string other = pki().other_ca.string();
    o.ca_file = other.c_str();
    REQUIRE(siaara_web_client_create_tls(base.data(), base.size(), 10000, &o, &wc) == SIAARA_OK);
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, nullptr, 0, buf.data(), buf.size(), &needed) !=
          SIAARA_OK);
    siaara_web_client_free(wc);
    o.struct_size = 4;
    CHECK(siaara_web_client_create_tls(base.data(), base.size(), 10000, &o, &wc) == SIAARA_ERR_INVALID_ARGUMENT);
    server.stop();
}
