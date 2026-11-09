// SIAARA DICOM Toolkit - TLS internals shared by the per-OS backends (not a public header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/tls.hpp"

namespace siaara::net::detail {

// ---------------------------------------------------------------- secrets
// Overwrites memory in a way the optimiser cannot remove.
void secure_wipe(void* p, std::size_t n) noexcept;
void secure_wipe(std::string& s) noexcept;                // wipes the whole capacity, then clears
void secure_wipe(std::vector<std::uint8_t>& v) noexcept;  // same

// Reads a whole (small) file holding secrets; the caller wipes the result.
Result<std::vector<std::uint8_t>> read_secret_file(const std::string& path, std::size_t max_size = 1u << 20);

// ---------------------------------------------------------------- policy
// Allowed cipher suites (IANA numbers) in preference order: TLS 1.3 suites, then TLS 1.2
// ECDHE + AEAD suites (PS3.15 B.12 / B.13, RFC 9325 section 4.2).
struct CipherSuite {
    std::uint16_t id;
    bool tls13;
    bool ecdsa;  // TLS 1.2 suites: ECDSA (true) or RSA (false) authentication
    const char* name;
};
const CipherSuite* allowed_cipher_suites(std::size_t& count) noexcept;
const CipherSuite* find_allowed_cipher_suite(std::uint16_t id) noexcept;

// Checks the negotiated protocol and suite against the policy, logs the per-connection summary
// and, with insecure_skip_validation, the warning.
Status check_session(const TlsSettings& s, TlsContext::Role role, const TlsSessionInfo& info, const std::string& peer);
void tls_log(const TlsSettings& s, const std::string& msg);

// The name the client checks in the server certificate.
std::string expected_name(const TlsSettings& s, const std::string& host);
// Client side, before any handshake: InvalidArgument when `name` (the result of expected_name)
// is empty and certificate validation is on. Every backend calls it.
Status check_client_identity(const TlsSettings& s, const std::string& name);
// True for IPv4 dotted quads and IPv6 literals (no brackets).
bool is_ip_literal(const std::string& s) noexcept;

// PEM "CERTIFICATE" blocks of a file, DER-decoded. Fails when the file holds none.
Result<std::vector<std::vector<std::uint8_t>>> read_pem_certificates(const std::string& path);
// Decodes every block with the given label ("CERTIFICATE") from PEM text.
std::vector<std::vector<std::uint8_t>> pem_blocks(const std::string& text, const char* label);

// ---------------------------------------------------------------- backend hooks
// Exactly one backend file provides these for the target OS.
Result<std::shared_ptr<TlsContext>> backend_create_context(const TlsSettings& s, TlsContext::Role role);
bool backend_available(std::string& reason);
std::string backend_name();

class TlsListenerImpl {
public:
    virtual ~TlsListenerImpl() = default;
    virtual std::uint16_t port() const noexcept = 0;
    virtual Result<std::unique_ptr<TlsTransport>> accept(int timeout_ms) = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;
};

// ---------------------------------------------------------------- record engines
// A TLS state machine that never touches the network (OpenSSL over memory BIOs, SChannel over
// SSPI buffers). EngineTransport moves the bytes and applies the timeouts.
class TlsEngine {
public:
    virtual ~TlsEngine() = default;
    enum class Step : std::uint8_t { NeedInput, Done };
    // Takes ciphertext received from the peer.
    virtual Status feed(const std::uint8_t* p, std::size_t n) = 0;
    // Advances the handshake. Records to send are appended to `out`, also when it fails (alerts).
    virtual Result<Step> handshake(std::vector<std::uint8_t>& out) = 0;
    // After Step::Done: fills `info` and validates the peer per the settings.
    virtual Status finish(TlsSessionInfo& info) = 0;
    // Decrypts the buffered records: plaintext is appended to `plain`, records to send (post-
    // handshake messages) to `out`. `closed` is set when the peer's close_notify was seen.
    virtual Status decrypt(std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& out, bool& closed) = 0;
    // Encrypts at most max_plaintext() bytes per call into records appended to `out`.
    virtual Status encrypt(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) = 0;
    virtual std::size_t max_plaintext() const noexcept { return 16384; }
    // Appends the close_notify alert.
    virtual void close_notify(std::vector<std::uint8_t>& out) noexcept = 0;
};

class EngineContext : public TlsContext, public std::enable_shared_from_this<EngineContext> {
public:
    Result<std::unique_ptr<TlsTransport>> connect(const std::string& host, std::uint16_t port,
                                                  int connect_timeout_ms) override;
    Result<std::unique_ptr<TlsTransport>> wrap_client(std::unique_ptr<Transport> stream,
                                                      const std::string& peer_name) override;
    Result<std::unique_ptr<TlsTransport>> wrap_server(std::unique_ptr<Transport> stream) override;

protected:
    EngineContext(const TlsSettings& s, Role r) : TlsContext(s, r) {}
    // `peer_name`: client only, the name to verify (and send as SNI when it is a DNS name).
    virtual Result<std::unique_ptr<TlsEngine>> make_engine(const std::string& peer_name) = 0;

private:
    Result<std::unique_ptr<detail::TlsListenerImpl>> make_listener(const std::string& bind_address,
                                                                   std::uint16_t port, int backlog) override;
};

// A TlsTransport over any connected Transport, driven by a TlsEngine.
class EngineTransport final : public TlsTransport {
public:
    EngineTransport(std::shared_ptr<EngineContext> ctx, std::unique_ptr<Transport> inner,
                    std::unique_ptr<TlsEngine> engine);
    ~EngineTransport() override;

    Status handshake(int timeout_ms) override;
    const TlsSessionInfo& session() const noexcept override { return info_; }

    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override;
    Status write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) override;
    Result<bool> wait_readable(int timeout_ms) override;
    void shutdown() noexcept override;
    void close() noexcept override;
    bool is_open() const noexcept override;
    std::string peer_address() const override;

private:
    std::shared_ptr<EngineContext> ctx_;
    std::unique_ptr<Transport> inner_;
    std::unique_ptr<TlsEngine> eng_;
    std::string peer_;
    TlsSessionInfo info_;
    std::mutex mu_;  // serialises engine calls and the ciphertext they produce
    std::vector<std::uint8_t> plain_;
    std::size_t plain_pos_ = 0;
    bool done_ = false;
    bool broken_ = false;       // handshake or record failure: no further I/O
    bool peer_closed_ = false;  // close_notify received
    bool eof_ = false;          // TCP closed without close_notify
    bool sent_close_ = false;
    Status hs_error_;

    Status pump_decrypt(int deadline_left_ms);
    Status send_locked(std::vector<std::uint8_t>& out, int timeout_ms);
    Result<bool> fill(int timeout_ms);
};

}  // namespace siaara::net::detail
