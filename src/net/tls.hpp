// SIAARA DICOM Toolkit - TLS transport (DICOM PS3.15 Annex B.12 / B.13, BCP 195: RFC 8996, RFC 9325).
//
// The toolkit has no third-party dependency for TLS: each operating system's own TLS stack is used.
//   Windows : SChannel (SSPI) through secur32/crypt32.
//   macOS   : Network.framework (identities and trust through Security.framework).
//   Linux   : the system OpenSSL 3.x (libssl.so.3 / libcrypto.so.3), loaded at run time with
//             dlopen when TLS is first used; never linked at build time.
//
// Policy, on every backend:
//   - TLS 1.2 minimum, TLS 1.3 preferred (RFC 8996 removes 1.0/1.1). No SSL 2/3, no compression,
//     no renegotiation.
//   - TLS 1.2 cipher suites: ECDHE key exchange with an AEAD cipher only (AES-GCM, ChaCha20-Poly1305).
//     TLS 1.3: TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256.
//     The negotiated suite is checked again after the handshake; anything else is refused.
//   - Peer certificates are validated by default: chain to a trust anchor (the OS store, or the
//     PEM file in TlsSettings::ca_file), validity dates, and on the client the host name or IP
//     address (or TlsSettings::expected_identity). insecure_skip_validation turns this off and
//     logs a warning on every connection.
//   - There is never a fallback to plain TCP. Every handshake and every read has a timeout.
//   - Connections are closed with a TLS close_notify alert.
//
// PS3.15 requires TLS on a different port from unsecured DICOM; port 2762 ("dicom-tls") is the
// registered port for DIMSE over TLS.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/status.hpp"
#include "net/socket.hpp"

namespace siaara::net {

// Where a local certificate and its private key come from. Set exactly one source.
struct TlsIdentity {
    // PEM certificate chain (leaf first, then intermediates) and PEM private key
    // (PKCS#8, optionally encrypted with `password`). OpenSSL backend only; on Windows and macOS
    // use a PKCS#12 file or the OS store.
    std::string certificate_file;
    std::string private_key_file;
    // PKCS#12 (.p12 / .pfx) file holding the key, the certificate and optionally its chain.
    std::string pkcs12_file;
    // Password of the PKCS#12 file or of an encrypted PEM key. Never logged. The toolkit's own
    // copies are wiped as soon as the identity is loaded: TlsContext::settings() returns it empty.
    std::string password;
    // Windows: SHA-1 thumbprint (hex, spaces allowed) of a certificate with a private key in the
    // "MY" store of the current user (or of the local machine when windows_local_machine is set).
    std::string windows_thumbprint;
    bool windows_local_machine = false;
    // macOS: label of an identity (certificate + private key) in the user's keychains.
    std::string keychain_identity;

    bool empty() const noexcept {
        return certificate_file.empty() && private_key_file.empty() && pkcs12_file.empty() &&
               windows_thumbprint.empty() && keychain_identity.empty();
    }
};

enum class TlsMinVersion : std::uint8_t {
    Tls12 = 0,  // TLS 1.2 or 1.3 (1.3 preferred); the PS3.15 B.12 / B.13 profiles
    Tls13 = 1,  // TLS 1.3 only
};

struct TlsSettings {
    // When false, the owner of these settings uses plain TCP (the separate, unsecured port).
    bool enabled = false;
    // Server: required. Client: sent when the server asks for a certificate (mutual TLS).
    TlsIdentity identity;
    // PEM file of trust anchors used instead of the operating system's trust store.
    std::string ca_file;
    // Server: ask for a client certificate and refuse the connection without a valid one.
    bool require_client_certificate = false;
    // Client: the DNS name or IP address the server certificate must carry. Empty: the host
    // name or address that was connected to. Also sent as SNI when it is a DNS name.
    std::string expected_identity;
    // DANGEROUS: accept any peer certificate (no chain, name or date checks). For tests and
    // closed lab networks only; a warning is logged on every connection.
    bool insecure_skip_validation = false;
    TlsMinVersion min_version = TlsMinVersion::Tls12;
    int handshake_timeout_ms = 15000;
    // Warnings and a one-line summary per connection (protocol, cipher suite, peer address).
    // Never key material, passwords or application data.
    std::function<void(const std::string&)> log;
};

struct TlsSessionInfo {
    std::string protocol;            // "TLSv1.3", "TLSv1.2"
    std::uint16_t cipher_suite = 0;  // IANA cipher suite number
    std::string cipher_name;         // IANA name, e.g. "TLS_AES_128_GCM_SHA256"
    bool peer_certificate = false;   // the peer presented a certificate
    bool peer_verified = false;      // ... and it was validated (false with insecure_skip_validation)
};

// A Transport protected by TLS. Transports from TlsContext::connect() have completed the
// handshake; transports from TlsListener::accept() complete it in handshake(), which servers call
// on the worker thread so a slow client cannot stall the accept loop.
class TlsTransport : public Transport {
public:
    // Runs the handshake to completion within `timeout_ms`. A no-op once it has completed.
    virtual Status handshake(int timeout_ms) = 0;
    virtual const TlsSessionInfo& session() const noexcept = 0;
};

namespace detail {
class TlsListenerImpl;
}

// Configuration loaded once (identity, trust anchors) and shared by many connections.
// Thread safe: connect() and listeners may be used from several threads at once.
class TlsContext {
public:
    enum class Role : std::uint8_t { Client, Server };
    static Result<std::shared_ptr<TlsContext>> create(const TlsSettings& settings, Role role);

    virtual ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    Role role() const noexcept { return role_; }
    const TlsSettings& settings() const noexcept { return settings_; }

    // Client: opens a TCP connection to host:port within `connect_timeout_ms` and completes the
    // TLS handshake within settings().handshake_timeout_ms.
    virtual Result<std::unique_ptr<TlsTransport>> connect(const std::string& host, std::uint16_t port,
                                                          int connect_timeout_ms) = 0;
    // Wraps an already connected stream (client side) and completes the handshake. `peer_name`
    // is the name checked when settings().expected_identity is empty; with both empty the call
    // fails with Errc::InvalidArgument (unless insecure_skip_validation). Not available on macOS,
    // where Network.framework owns the TCP connection (Errc::Unsupported).
    virtual Result<std::unique_ptr<TlsTransport>> wrap_client(std::unique_ptr<Transport> stream,
                                                              const std::string& peer_name);
    // Server side: wraps an accepted stream; the handshake runs in handshake(). Unsupported on macOS.
    virtual Result<std::unique_ptr<TlsTransport>> wrap_server(std::unique_ptr<Transport> stream);

protected:
    TlsContext(const TlsSettings& s, Role r);
    TlsSettings settings_;
    Role role_;

private:
    friend class TlsListener;
    virtual Result<std::unique_ptr<detail::TlsListenerImpl>> make_listener(const std::string& bind_address,
                                                                           std::uint16_t port, int backlog) = 0;
};

// Listens for TLS connections (the counterpart of TcpListener).
class TlsListener {
public:
    TlsListener();
    ~TlsListener();
    TlsListener(const TlsListener&) = delete;
    TlsListener& operator=(const TlsListener&) = delete;

    // `ctx` must be a server context. Empty `bind_address`: all interfaces. Port 0: ephemeral.
    Status listen(std::shared_ptr<TlsContext> ctx, const std::string& bind_address, std::uint16_t port,
                  int backlog = 64);
    std::uint16_t port() const noexcept;
    // Waits up to `timeout_ms` for a connection (Errc::Timeout when none arrived). The returned
    // transport has not done its handshake yet: call handshake() before using it.
    Result<std::unique_ptr<TlsTransport>> accept(int timeout_ms);
    void close() noexcept;
    bool is_open() const noexcept;

private:
    std::shared_ptr<TlsContext> ctx_;
    std::unique_ptr<detail::TlsListenerImpl> impl_;
};

// Client convenience: creates a client context from `settings` and connects.
Result<std::unique_ptr<TlsTransport>> tls_connect(const TlsSettings& settings, const std::string& host,
                                                  std::uint16_t port, int connect_timeout_ms);

// True when this build and this machine can do TLS. On Linux this loads the system OpenSSL 3
// (once). On failure `reason` says why (e.g. "libssl.so.3 not found").
bool tls_available(std::string* reason = nullptr);

// Overwrites a secret (e.g. TlsIdentity::password) so the optimiser cannot skip it, then clears it.
void tls_wipe_secret(std::string& s) noexcept;

// Name of the TLS implementation in use: "OpenSSL 3.0.13", "SChannel", "Network.framework".
std::string tls_backend_name();

}  // namespace siaara::net
