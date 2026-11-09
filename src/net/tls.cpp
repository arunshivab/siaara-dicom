// SIAARA DICOM Toolkit - TLS transport: backend-independent parts (policy, PEM, listener, the
// engine-driven transport used by the OpenSSL and SChannel backends).
#include "net/tls.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#include "core/io.hpp"
#include "net/tls_internal.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace siaara::net {

namespace detail {

// ---------------------------------------------------------------- secrets
void secure_wipe(void* p, std::size_t n) noexcept {
    if (!p) return;
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) v[i] = 0;
}

void secure_wipe(std::string& s) noexcept {
    if (s.capacity()) secure_wipe(s.data(), s.capacity());
    s.clear();
}

void secure_wipe(std::vector<std::uint8_t>& v) noexcept {
    if (v.capacity()) secure_wipe(v.data(), v.capacity());
    v.clear();
}

Result<std::vector<std::uint8_t>> read_secret_file(const std::string& path, std::size_t max_size) {
    Result<Bytes> b = io::read_file(path);
    if (!b) return Status(Errc::IoError, "cannot read " + path);
    Bytes bytes = std::move(b).value();
    std::vector<std::uint8_t> out;
    if (bytes.size() <= max_size) out.assign(bytes.data(), bytes.data() + bytes.size());
    // io::read_file returns a private heap copy; wipe it before it is released.
    if (bytes.size()) secure_wipe(const_cast<std::uint8_t*>(bytes.data()), bytes.size());
    if (out.empty()) return Status(Errc::InvalidArgument, path + " is empty or too large");
    return out;
}

// ---------------------------------------------------------------- policy
namespace {
const CipherSuite kSuites[] = {
    {0x1301, true, false, "TLS_AES_128_GCM_SHA256"},
    {0x1302, true, false, "TLS_AES_256_GCM_SHA384"},
    {0x1303, true, false, "TLS_CHACHA20_POLY1305_SHA256"},
    {0xC02B, false, true, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"},
    {0xC02F, false, false, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256"},
    {0xC02C, false, true, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"},
    {0xC030, false, false, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"},
    {0xCCA9, false, true, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256"},
    {0xCCA8, false, false, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"},
};

void default_warning_sink(const std::string& msg) noexcept {
    const std::string line = "siaara: " + msg + "\n";
#if defined(_WIN32)
    HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        (void)::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
#else
    std::size_t done = 0;
    while (done < line.size()) {
        const ssize_t w = ::write(2, line.data() + done, line.size() - done);
        if (w <= 0) break;
        done += static_cast<std::size_t>(w);
    }
#endif
}

std::string hex4(std::uint16_t v) {
    static const char* d = "0123456789ABCDEF";
    std::string s = "0x";
    for (int shift = 12; shift >= 0; shift -= 4) s.push_back(d[(v >> shift) & 0xF]);
    return s;
}
}  // namespace

const CipherSuite* allowed_cipher_suites(std::size_t& count) noexcept {
    count = sizeof kSuites / sizeof kSuites[0];
    return kSuites;
}

const CipherSuite* find_allowed_cipher_suite(std::uint16_t id) noexcept {
    for (const CipherSuite& c : kSuites)
        if (c.id == id) return &c;
    return nullptr;
}

void tls_log(const TlsSettings& s, const std::string& msg) {
    if (s.log) s.log(msg);
}

Status check_session(const TlsSettings& s, TlsContext::Role role, const TlsSessionInfo& info, const std::string& peer) {
    const bool v13 = info.protocol == "TLSv1.3";
    if (!v13 && info.protocol != "TLSv1.2")
        return Status(Errc::NetworkError, "TLS protocol version " + info.protocol + " is not allowed");
    if (s.min_version == TlsMinVersion::Tls13 && !v13) return Status(Errc::NetworkError, "TLS 1.3 is required");
    const CipherSuite* cs = find_allowed_cipher_suite(info.cipher_suite);
    if (!cs || cs->tls13 != v13)
        return Status(Errc::NetworkError, "cipher suite " + hex4(info.cipher_suite) + " is not allowed by the PS3.15 profile");
    const char* who = role == TlsContext::Role::Client ? "server" : "client";
    std::string summary = std::string("TLS: ") + (role == TlsContext::Role::Client ? "connected to " : "accepted ") +
                          peer + ", " + info.protocol + ", " + cs->name + ", " + who + " certificate ";
    summary += !info.peer_certificate ? "not presented" : info.peer_verified ? "verified" : "NOT verified";
    tls_log(s, summary);
    if (s.insecure_skip_validation) {
        const std::string w = std::string("WARNING: TLS certificate validation is disabled (insecure_skip_validation); the ") +
                              who + " " + peer + " is not authenticated";
        if (s.log) s.log(w);
        else default_warning_sink(w);
    }
    return Status::ok();
}

std::string expected_name(const TlsSettings& s, const std::string& host) {
    return s.expected_identity.empty() ? host : s.expected_identity;
}

Status check_client_identity(const TlsSettings& s, const std::string& name) {
    // Without a name the server certificate can only be checked against the trust anchors, and
    // any certificate from any trusted CA would be accepted: fail closed on every backend.
    if (name.empty() && !s.insecure_skip_validation)
        return Status(Errc::InvalidArgument,
                      "TLS client: no host name and no expected_identity to verify the server certificate against");
    return Status::ok();
}

bool is_ip_literal(const std::string& s) noexcept {
    if (s.empty()) return false;
    if (s.find(':') != std::string::npos) return true;  // DNS names never contain ':'
    int parts = 0, digits = 0, value = 0;
    for (char c : s) {
        if (c == '.') {
            if (digits == 0) return false;
            ++parts;
            digits = value = 0;
        } else if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits > 3 || value > 255) return false;
        } else {
            return false;
        }
    }
    return digits > 0 && parts == 3;
}

// ---------------------------------------------------------------- PEM
namespace {
int b64_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool b64_decode(std::string_view in, std::vector<std::uint8_t>& out) {
    std::uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') {
            padding = true;
            continue;
        }
        const int v = b64_value(c);
        if (v < 0 || padding) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return !out.empty();
}
}  // namespace

std::vector<std::vector<std::uint8_t>> pem_blocks(const std::string& text, const char* label) {
    std::vector<std::vector<std::uint8_t>> out;
    const std::string begin = std::string("-----BEGIN ") + label + "-----";
    const std::string end = std::string("-----END ") + label + "-----";
    std::size_t pos = 0;
    while (true) {
        const std::size_t b = text.find(begin, pos);
        if (b == std::string::npos) break;
        const std::size_t body = b + begin.size();
        const std::size_t e = text.find(end, body);
        if (e == std::string::npos) break;
        std::vector<std::uint8_t> der;
        if (b64_decode(std::string_view(text).substr(body, e - body), der)) out.push_back(std::move(der));
        pos = e + end.size();
    }
    return out;
}

Result<std::vector<std::vector<std::uint8_t>>> read_pem_certificates(const std::string& path) {
    Result<Bytes> b = io::read_file(path);
    if (!b) return Status(Errc::IoError, "cannot read " + path);
    if (b->size() > (16u << 20)) return Status(Errc::LimitExceeded, path + " is too large");
    auto blocks = pem_blocks(std::string(b->as_chars()), "CERTIFICATE");
    if (blocks.empty()) return Status(Errc::InvalidArgument, "no PEM certificate in " + path);
    return blocks;
}

// ---------------------------------------------------------------- engine transport
namespace {
using Clock = std::chrono::steady_clock;

Clock::time_point deadline_after(int ms) { return Clock::now() + std::chrono::milliseconds(ms < 0 ? 0 : ms); }

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) return 0;
    return left > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(left);
}

constexpr int kAlertWriteMs = 1000;    // close_notify and failure alerts
constexpr int kControlWriteMs = 10000; // post-handshake messages produced while reading
constexpr std::size_t kReadChunk = 16384 + 512;
}  // namespace

EngineTransport::EngineTransport(std::shared_ptr<EngineContext> ctx, std::unique_ptr<Transport> inner,
                                 std::unique_ptr<TlsEngine> engine)
    : ctx_(std::move(ctx)), inner_(std::move(inner)), eng_(std::move(engine)) {
    peer_ = inner_ ? inner_->peer_address() : std::string();
}

EngineTransport::~EngineTransport() { close(); }

Status EngineTransport::send_locked(std::vector<std::uint8_t>& out, int timeout_ms) {
    if (out.empty()) return Status::ok();
    Status s = inner_->write_all(out.data(), out.size(), timeout_ms);
    out.clear();
    return s;
}

Status EngineTransport::handshake(int timeout_ms) {
    if (done_) return Status::ok();
    if (broken_) return hs_error_.code != Errc::Ok ? hs_error_ : Status(Errc::NetworkError, "TLS connection failed");
    const auto deadline = deadline_after(timeout_ms);
    auto fail = [&](Status s) {
        broken_ = true;
        s.message = "TLS handshake with " + peer_ + " failed: " + s.message;
        hs_error_ = s;
        inner_->shutdown();
        return s;
    };
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> buf(kReadChunk);
    std::lock_guard<std::mutex> lock(mu_);
    while (true) {
        Result<TlsEngine::Step> st = eng_->handshake(out);
        if (!out.empty()) {
            Status w = send_locked(out, st ? std::max(ms_left(deadline), 1) : kAlertWriteMs);
            if (!w && st) return fail(w.code == Errc::Timeout ? Status(Errc::Timeout, "timed out") : w);
        }
        if (!st) return fail(st.take_error());
        if (*st == TlsEngine::Step::Done) break;
        const int left = ms_left(deadline);
        if (left <= 0) return fail(Status(Errc::Timeout, "timed out"));
        Result<std::size_t> r = inner_->read_some(buf.data(), buf.size(), left);
        if (!r) return fail(r.error().code == Errc::Timeout ? Status(Errc::Timeout, "timed out") : r.take_error());
        if (*r == 0) return fail(Status(Errc::NetworkError, "connection closed by the peer"));
        if (Status f = eng_->feed(buf.data(), *r); !f) return fail(f);
    }
    if (Status f = eng_->finish(info_); !f) {
        out.clear();
        eng_->close_notify(out);
        (void)send_locked(out, kAlertWriteMs);
        return fail(f);
    }
    if (Status c = check_session(ctx_->settings(), ctx_->role(), info_, peer_); !c) {
        out.clear();
        eng_->close_notify(out);
        (void)send_locked(out, kAlertWriteMs);
        return fail(c);
    }
    done_ = true;
    return Status::ok();
}

Status EngineTransport::pump_decrypt(int write_timeout_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    if (plain_pos_ == plain_.size()) {
        plain_.clear();
        plain_pos_ = 0;
    }
    std::vector<std::uint8_t> out;
    bool closed = false;
    Status s = eng_->decrypt(plain_, out, closed);
    if (!out.empty()) {
        Status w = send_locked(out, write_timeout_ms);
        if (s && !w) s = w;
    }
    if (closed) peer_closed_ = true;
    if (!s) {
        broken_ = true;
        inner_->shutdown();
        return Status(Errc::NetworkError, "TLS record error from " + peer_ + ": " + s.message);
    }
    return Status::ok();
}

// Reads once from the network and decrypts. Ok(true) when anything happened.
Result<bool> EngineTransport::fill(int timeout_ms) {
    std::uint8_t buf[kReadChunk];
    Result<std::size_t> r = inner_->read_some(buf, sizeof buf, timeout_ms);
    if (!r) return r.take_error();
    if (*r == 0) {
        eof_ = true;
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        Status f = eng_->feed(buf, *r);
        if (!f) {
            broken_ = true;
            return f;
        }
    }
    if (Status p = pump_decrypt(kControlWriteMs); !p) return p;
    return true;
}

Result<std::size_t> EngineTransport::read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) {
    if (!done_) return Status(Errc::NetworkError, broken_ ? "TLS connection failed" : "TLS handshake not completed");
    if (n == 0) return std::size_t{0};
    const auto deadline = deadline_after(timeout_ms);
    bool first = true;
    while (true) {
        if (plain_pos_ < plain_.size()) {
            const std::size_t k = std::min(n, plain_.size() - plain_pos_);
            std::memcpy(buf, plain_.data() + plain_pos_, k);
            plain_pos_ += k;
            return k;
        }
        if (peer_closed_) return std::size_t{0};
        if (broken_) return Status(Errc::NetworkError, "TLS connection failed");
        if (eof_) return Status(Errc::NetworkError, "connection closed without TLS close_notify");
        if (first) {
            // Records may already be buffered (e.g. received together with the handshake).
            first = false;
            if (Status p = pump_decrypt(kControlWriteMs); !p) return p;
            continue;
        }
        Result<bool> f = fill(ms_left(deadline));
        if (!f) return f.take_error();
    }
}

Status EngineTransport::write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) {
    if (!done_ || broken_) return Status(Errc::NetworkError, broken_ ? "TLS connection failed" : "TLS handshake not completed");
    if (sent_close_) return Status(Errc::NetworkError, "TLS connection closed");
    std::lock_guard<std::mutex> lock(mu_);
    const std::size_t chunk = std::max<std::size_t>(eng_->max_plaintext(), 1);
    std::vector<std::uint8_t> out;
    std::size_t done = 0;
    while (done < n) {
        const std::size_t k = std::min(chunk, n - done);
        if (Status e = eng_->encrypt(data + done, k, out); !e) {
            broken_ = true;
            return Status(Errc::NetworkError, "TLS encryption failed: " + e.message);
        }
        done += k;
        if (out.size() >= 4 * chunk || done == n) {
            if (Status w = send_locked(out, timeout_ms); !w) {
                broken_ = true;  // a partly written record cannot be resumed
                return w;
            }
        }
    }
    return Status::ok();
}

Result<bool> EngineTransport::wait_readable(int timeout_ms) {
    if (!done_) return Status(Errc::NetworkError, "TLS handshake not completed");
    const auto deadline = deadline_after(timeout_ms);
    bool first = true;
    while (true) {
        if (plain_pos_ < plain_.size() || peer_closed_ || eof_ || broken_) return true;
        if (first) {
            first = false;
            if (Status p = pump_decrypt(kControlWriteMs); !p) return true;  // read_some reports it
            continue;
        }
        const int left = ms_left(deadline);
        Result<bool> ready = inner_->wait_readable(left);
        if (!ready) return ready.take_error();
        if (!*ready) return false;
        Result<bool> f = fill(left);
        if (!f) {
            if (f.error().code == Errc::Timeout) return false;
            return f.take_error();
        }
    }
}

void EngineTransport::shutdown() noexcept {
    if (inner_) inner_->shutdown();
}

void EngineTransport::close() noexcept {
    if (!inner_) return;
    if (inner_->is_open() && done_ && !broken_ && !eof_ && !sent_close_.exchange(true)) {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            std::vector<std::uint8_t> out;
            eng_->close_notify(out);
            (void)send_locked(out, kAlertWriteMs);
        } catch (...) {
        }
    }
    inner_->close();
}

bool EngineTransport::is_open() const noexcept { return inner_ && inner_->is_open(); }

std::string EngineTransport::peer_address() const { return peer_; }

// ---------------------------------------------------------------- engine context
namespace {
class EngineListener final : public TlsListenerImpl {
public:
    explicit EngineListener(std::shared_ptr<EngineContext> ctx) : ctx_(std::move(ctx)) {}
    Status listen(const std::string& bind_address, std::uint16_t port, int backlog) {
        return tcp_.listen(bind_address, port, backlog);
    }
    std::uint16_t port() const noexcept override { return tcp_.port(); }
    Result<std::unique_ptr<TlsTransport>> accept(int timeout_ms) override {
        Result<std::unique_ptr<Transport>> t = tcp_.accept(timeout_ms);
        if (!t) return t.take_error();
        return ctx_->wrap_server(std::move(t).value());
    }
    void close() noexcept override { tcp_.close(); }
    bool is_open() const noexcept override { return tcp_.is_open(); }

private:
    std::shared_ptr<EngineContext> ctx_;
    TcpListener tcp_;
};
}  // namespace

Result<std::unique_ptr<TlsTransport>> EngineContext::connect(const std::string& host, std::uint16_t port,
                                                             int connect_timeout_ms) {
    if (role() != Role::Client) return Status(Errc::InvalidArgument, "connect() needs a client TLS context");
    Result<std::unique_ptr<Transport>> t = tcp_connect(host, port, connect_timeout_ms);
    if (!t) return t.take_error();
    return wrap_client(std::move(t).value(), host);
}

Result<std::unique_ptr<TlsTransport>> EngineContext::wrap_client(std::unique_ptr<Transport> stream,
                                                                 const std::string& peer_name) {
    if (role() != Role::Client) return Status(Errc::InvalidArgument, "wrap_client() needs a client TLS context");
    if (!stream) return Status(Errc::InvalidArgument, "no stream");
    const std::string name = expected_name(settings_, peer_name);
    if (Status v = check_client_identity(settings_, name); !v) return v;
    Result<std::unique_ptr<TlsEngine>> e = make_engine(name);
    if (!e) return e.take_error();
    auto t = std::make_unique<EngineTransport>(shared_from_this(), std::move(stream), std::move(e).value());
    if (Status h = t->handshake(settings_.handshake_timeout_ms); !h) return h;
    return std::unique_ptr<TlsTransport>(std::move(t));
}

Result<std::unique_ptr<TlsTransport>> EngineContext::wrap_server(std::unique_ptr<Transport> stream) {
    if (role() != Role::Server) return Status(Errc::InvalidArgument, "wrap_server() needs a server TLS context");
    if (!stream) return Status(Errc::InvalidArgument, "no stream");
    Result<std::unique_ptr<TlsEngine>> e = make_engine(std::string());
    if (!e) return e.take_error();
    return std::unique_ptr<TlsTransport>(
        std::make_unique<EngineTransport>(shared_from_this(), std::move(stream), std::move(e).value()));
}

Result<std::unique_ptr<TlsListenerImpl>> EngineContext::make_listener(const std::string& bind_address,
                                                                      std::uint16_t port, int backlog) {
    auto l = std::make_unique<EngineListener>(shared_from_this());
    if (Status s = l->listen(bind_address, port, backlog); !s) return s;
    return std::unique_ptr<TlsListenerImpl>(std::move(l));
}

}  // namespace detail

// ---------------------------------------------------------------- public API
TlsContext::TlsContext(const TlsSettings& s, Role r) : settings_(s), role_(r) {}

TlsContext::~TlsContext() { detail::secure_wipe(settings_.identity.password); }

Result<std::unique_ptr<TlsTransport>> TlsContext::wrap_client(std::unique_ptr<Transport>, const std::string&) {
    return Status(Errc::Unsupported, "this TLS backend cannot wrap an existing connection");
}

Result<std::unique_ptr<TlsTransport>> TlsContext::wrap_server(std::unique_ptr<Transport>) {
    return Status(Errc::Unsupported, "this TLS backend cannot wrap an existing connection");
}

Result<std::shared_ptr<TlsContext>> TlsContext::create(const TlsSettings& settings, Role role) {
    if (settings.handshake_timeout_ms <= 0) return Status(Errc::InvalidArgument, "TLS handshake timeout must be positive");
    if (role == Role::Server && settings.identity.empty())
        return Status(Errc::InvalidArgument, "a TLS server needs a certificate and private key");
    const TlsIdentity& id = settings.identity;
    if (id.certificate_file.empty() != id.private_key_file.empty())
        return Status(Errc::InvalidArgument, "a PEM identity needs both a certificate file and a private key file");
    const int sources = (id.certificate_file.empty() ? 0 : 1) + (id.pkcs12_file.empty() ? 0 : 1) +
                        (id.windows_thumbprint.empty() ? 0 : 1) + (id.keychain_identity.empty() ? 0 : 1);
    if (sources > 1) return Status(Errc::InvalidArgument, "set exactly one TLS identity source");
    std::string why;
    if (!detail::backend_available(why)) return Status(Errc::Unsupported, "TLS is not available: " + why);
    Result<std::shared_ptr<TlsContext>> ctx = detail::backend_create_context(settings, role);
    // Every backend has loaded the identity by now: the context keeps no copy of the password
    // (settings() would expose it). The caller's own TlsSettings is the caller's to wipe.
    if (ctx) detail::secure_wipe(ctx.value()->settings_.identity.password);
    return ctx;
}

TlsListener::TlsListener() = default;
TlsListener::~TlsListener() { close(); }

Status TlsListener::listen(std::shared_ptr<TlsContext> ctx, const std::string& bind_address, std::uint16_t port,
                           int backlog) {
    close();
    if (!ctx || ctx->role() != TlsContext::Role::Server)
        return Status(Errc::InvalidArgument, "TlsListener needs a server TLS context");
    Result<std::unique_ptr<detail::TlsListenerImpl>> l = ctx->make_listener(bind_address, port, backlog);
    if (!l) return l.take_error();
    ctx_ = std::move(ctx);
    impl_ = std::move(l).value();
    return Status::ok();
}

std::uint16_t TlsListener::port() const noexcept { return impl_ ? impl_->port() : std::uint16_t{0}; }

Result<std::unique_ptr<TlsTransport>> TlsListener::accept(int timeout_ms) {
    if (!impl_) return Status(Errc::NetworkError, "listener closed");
    return impl_->accept(timeout_ms);
}

void TlsListener::close() noexcept {
    if (impl_) impl_->close();
    impl_.reset();
}

bool TlsListener::is_open() const noexcept { return impl_ && impl_->is_open(); }

Result<std::unique_ptr<TlsTransport>> tls_connect(const TlsSettings& settings, const std::string& host,
                                                  std::uint16_t port, int connect_timeout_ms) {
    Result<std::shared_ptr<TlsContext>> ctx = TlsContext::create(settings, TlsContext::Role::Client);
    if (!ctx) return ctx.take_error();
    return ctx.value()->connect(host, port, connect_timeout_ms);
}

bool tls_available(std::string* reason) {
    std::string why;
    const bool ok = detail::backend_available(why);
    if (reason) *reason = ok ? detail::backend_name() : why;
    return ok;
}

std::string tls_backend_name() { return detail::backend_name(); }

void tls_wipe_secret(std::string& s) noexcept { detail::secure_wipe(s); }

}  // namespace siaara::net
