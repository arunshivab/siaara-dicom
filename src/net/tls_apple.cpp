// SIAARA DICOM Toolkit - TLS backend for macOS: Network.framework (connections and TLS),
// Security.framework (identities, trust evaluation) and CoreFoundation. All are OS frameworks.
//
// Network.framework owns its TCP connections, so this backend implements TlsContext::connect()
// and the listener directly (wrap_client/wrap_server are not available). The deprecated Secure
// Transport API is not used. The C API of Network.framework takes blocks; Apple clang compiles
// them in C++ (the blocks extension is on by default for Apple targets), so no Objective-C is
// needed. Objects are reference counted by hand (nw_release / sec_release / CFRelease) because
// this is C++, not Objective-C with ARC.
//
// Policy (as on the other backends): TLS 1.2 minimum, 1.3 preferred; only the cipher suites of
// detail::allowed_cipher_suites() are offered; peer certificates are evaluated by the toolkit's
// verify block (SecPolicyCreateSSL with the expected name, the configured anchors only when
// ca_file is set, dates, key sizes). Identities: PKCS#12 files or a keychain identity by label.
#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <Network/Network.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "net/tls_internal.hpp"

#if !defined(__BLOCKS__)
#error "tls_apple.cpp needs the clang blocks extension (on by default with Apple clang; else add -fblocks)"
#endif

namespace siaara::net::detail {

namespace {

using Clock = std::chrono::steady_clock;

std::string cf_string(CFStringRef s) {
    if (!s) return std::string();
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(max > 0 ? max : 1), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return std::string();
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string os_status_text(OSStatus st) {
    std::string text = "OSStatus " + std::to_string(static_cast<long>(st));
    if (CFStringRef m = SecCopyErrorMessageString(st, nullptr)) {
        text += " (" + cf_string(m) + ")";
        CFRelease(m);
    }
    return text;
}

std::string describe(nw_error_t e) {
    if (!e) return "unknown network error";
    const nw_error_domain_t dom = nw_error_get_error_domain(e);
    const int code = nw_error_get_error_code(e);
    if (dom == nw_error_domain_tls) return "TLS error " + os_status_text(static_cast<OSStatus>(code));
    if (dom == nw_error_domain_posix) return "network error (errno " + std::to_string(code) + ")";
    if (dom == nw_error_domain_dns) return "name resolution failed (" + std::to_string(code) + ")";
    return "network error " + std::to_string(code);
}

// Shared between a transport and the blocks Network.framework calls on its queue.
struct ConnState {
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    bool failed = false;
    bool cancelled = false;
    std::string error;
    std::string verify_error;
    std::vector<std::uint8_t> rx;
    std::size_t rx_pos = 0;
    bool eof = false;
    bool recv_pending = false;
    std::string rx_error;
    int sends_pending = 0;
    std::string tx_error;
};

class AppleContext;

class AppleTransport final : public TlsTransport {
public:
    AppleTransport(std::shared_ptr<AppleContext> ctx, nw_connection_t c, std::shared_ptr<ConnState> st);
    ~AppleTransport() override;

    Status handshake(int timeout_ms) override;
    const TlsSessionInfo& session() const noexcept override { return info_; }
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override;
    Status write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) override;
    Result<bool> wait_readable(int timeout_ms) override;
    void shutdown() noexcept override;
    void close() noexcept override;
    bool is_open() const noexcept override;
    std::string peer_address() const override { return peer_; }

private:
    std::shared_ptr<AppleContext> ctx_;
    nw_connection_t conn_;
    std::shared_ptr<ConnState> st_;
    dispatch_queue_t queue_;
    TlsSessionInfo info_;
    std::string peer_;
    bool started_ = false;
    bool done_ = false;
    bool closed_ = false;

    void start_receive_locked();
    void read_session();
};

class AppleContext final : public TlsContext, public std::enable_shared_from_this<AppleContext> {
public:
    AppleContext(const TlsSettings& s, Role r) : TlsContext(s, r) {}
    ~AppleContext() override {
        if (identity_) CFRelease(identity_);
        if (anchors_) CFRelease(anchors_);
    }
    Status init();

    Result<std::unique_ptr<TlsTransport>> connect(const std::string& host, std::uint16_t port,
                                                  int connect_timeout_ms) override;
    // TLS-over-TCP parameters. `st` (client) receives the verification error text.
    nw_parameters_t make_parameters(const std::string& peer_name, std::shared_ptr<ConnState> st);

private:
    SecIdentityRef identity_ = nullptr;
    CFArrayRef anchors_ = nullptr;

    Status load_identity();
    Status load_anchors();
    Result<std::unique_ptr<detail::TlsListenerImpl>> make_listener(const std::string& bind_address,
                                                                   std::uint16_t port, int backlog) override;
};

// ---------------------------------------------------------------- context
Status AppleContext::load_identity() {
    const TlsIdentity& id = settings_.identity;
    if (!id.certificate_file.empty())
        return Status(Errc::Unsupported, "PEM private keys are not supported by the macOS backend; use a PKCS#12 "
                                         "file or a keychain identity");
    if (!id.windows_thumbprint.empty()) return Status(Errc::Unsupported, "certificate thumbprints are for Windows");
    if (!id.pkcs12_file.empty()) {
        Result<std::vector<std::uint8_t>> der = read_secret_file(id.pkcs12_file);
        if (!der) return der.take_error();
        std::vector<std::uint8_t> bytes = std::move(der).value();
        CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
        secure_wipe(bytes);
        CFStringRef pw = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(id.password.data()),
                                                 static_cast<CFIndex>(id.password.size()), kCFStringEncodingUTF8, false);
        if (!data || !pw) {
            if (data) CFRelease(data);
            if (pw) CFRelease(pw);
            return Status(Errc::OutOfMemory, "cannot read the PKCS#12 file");
        }
        const void* keys[2] = {kSecImportExportPassphrase, nullptr};
        const void* values[2] = {pw, nullptr};
        CFIndex n = 1;
#if defined(__MAC_15_0)
        // Keep the imported key in memory instead of adding it to the default keychain.
        if (__builtin_available(macOS 15.0, *)) {
            keys[1] = kSecImportToMemoryOnly;
            values[1] = kCFBooleanTrue;
            n = 2;
        }
#endif
        CFDictionaryRef opts = CFDictionaryCreate(nullptr, keys, values, n, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
        CFArrayRef items = nullptr;
        const OSStatus st = SecPKCS12Import(data, opts, &items);
        CFRelease(opts);
        CFRelease(pw);
        CFRelease(data);
        if (st != errSecSuccess || !items || CFArrayGetCount(items) < 1) {
            if (items) CFRelease(items);
            return Status(Errc::InvalidArgument, "cannot import the PKCS#12 file " + id.pkcs12_file + ": " + os_status_text(st));
        }
        CFDictionaryRef first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, 0));
        const void* ident = first ? CFDictionaryGetValue(first, kSecImportItemIdentity) : nullptr;
        if (ident) identity_ = static_cast<SecIdentityRef>(const_cast<void*>(CFRetain(ident)));
        CFRelease(items);
        if (!identity_) return Status(Errc::InvalidArgument, "the PKCS#12 file holds no identity");
        return Status::ok();
    }
    if (!id.keychain_identity.empty()) {
        CFStringRef label = CFStringCreateWithCString(nullptr, id.keychain_identity.c_str(), kCFStringEncodingUTF8);
        if (!label) return Status(Errc::InvalidArgument, "invalid keychain identity label");
        const void* keys[4] = {kSecClass, kSecAttrLabel, kSecReturnRef, kSecMatchLimit};
        const void* values[4] = {kSecClassIdentity, label, kCFBooleanTrue, kSecMatchLimitOne};
        CFDictionaryRef q = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
        CFTypeRef found = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &found);
        CFRelease(q);
        CFRelease(label);
        if (st != errSecSuccess || !found)
            return Status(Errc::NotFound, "no keychain identity labelled " + id.keychain_identity + ": " + os_status_text(st));
        identity_ = static_cast<SecIdentityRef>(const_cast<void*>(found));
        return Status::ok();
    }
    return Status::ok();
}

Status AppleContext::load_anchors() {
    if (settings_.ca_file.empty()) return Status::ok();
    Result<std::vector<std::vector<std::uint8_t>>> certs = read_pem_certificates(settings_.ca_file);
    if (!certs) return certs.take_error();
    CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    if (!arr) return Status(Errc::OutOfMemory, "out of memory");
    for (const std::vector<std::uint8_t>& der : *certs) {
        CFDataRef d = CFDataCreate(nullptr, der.data(), static_cast<CFIndex>(der.size()));
        SecCertificateRef c = d ? SecCertificateCreateWithData(nullptr, d) : nullptr;
        if (d) CFRelease(d);
        if (!c) {
            CFRelease(arr);
            return Status(Errc::InvalidArgument, "invalid certificate in " + settings_.ca_file);
        }
        CFArrayAppendValue(arr, c);
        CFRelease(c);
    }
    anchors_ = arr;
    return Status::ok();
}

Status AppleContext::init() {
    if (Status s = load_identity(); !s) return s;
    return load_anchors();
}

nw_parameters_t AppleContext::make_parameters(const std::string& peer_name, std::shared_ptr<ConnState> st) {
    std::shared_ptr<AppleContext> self = shared_from_this();
    const bool server = role_ == Role::Server;
    const std::string name = peer_name;
    nw_parameters_configure_protocol_block_t configure_tls = ^(nw_protocol_options_t tls_options) {
        sec_protocol_options_t sec = nw_tls_copy_sec_protocol_options(tls_options);
        const TlsSettings& s = self->settings_;
        const bool only13 = s.min_version == TlsMinVersion::Tls13;
        sec_protocol_options_set_min_tls_protocol_version(sec, only13 ? tls_protocol_version_TLSv13
                                                                      : tls_protocol_version_TLSv12);
        sec_protocol_options_set_max_tls_protocol_version(sec, tls_protocol_version_TLSv13);
        std::size_t count = 0;
        const CipherSuite* suites = allowed_cipher_suites(count);
        for (std::size_t i = 0; i < count; ++i)
            if (suites[i].tls13 || !only13)
                sec_protocol_options_append_tls_ciphersuite(sec, static_cast<tls_ciphersuite_t>(suites[i].id));
        if (self->identity_) {
            sec_identity_t sid = sec_identity_create(self->identity_);
            if (sid) {
                sec_protocol_options_set_local_identity(sec, sid);
                sec_release(sid);
            }
        }
        if (server && s.require_client_certificate) sec_protocol_options_set_peer_authentication_required(sec, true);
        if (!server && !name.empty() && !is_ip_literal(name))
            sec_protocol_options_set_tls_server_name(sec, name.c_str());
        sec_protocol_options_set_verify_block(
            sec,
            ^(sec_protocol_metadata_t, sec_trust_t trust_ref, sec_protocol_verify_complete_t complete) {
                const TlsSettings& vs = self->settings_;
                if (vs.insecure_skip_validation) {
                    complete(true);
                    return;
                }
                SecTrustRef trust = sec_trust_copy_ref(trust_ref);
                std::string why;
                bool ok = false;
                if (trust) {
                    CFStringRef host = nullptr;
                    if (!server && !name.empty())
                        host = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
                    SecPolicyRef policy = SecPolicyCreateSSL(!server, host);
                    if (host) CFRelease(host);
                    if (policy) {
                        (void)SecTrustSetPolicies(trust, policy);
                        CFRelease(policy);
                    }
                    if (self->anchors_) {
                        (void)SecTrustSetAnchorCertificates(trust, self->anchors_);
                        (void)SecTrustSetAnchorCertificatesOnly(trust, true);
                    }
                    CFErrorRef err = nullptr;
                    ok = SecTrustEvaluateWithError(trust, &err);
                    if (!ok) {
                        if (err) {
                            CFStringRef d = CFErrorCopyDescription(err);
                            why = cf_string(d) + " (code " + std::to_string(static_cast<long>(CFErrorGetCode(err))) + ")";
                            if (d) CFRelease(d);
                        }
                        if (why.empty()) why = "trust evaluation failed";
                    }
                    if (err) CFRelease(err);
                    if (ok) {
                        // PS3.15 key sizes: RSA >= 2048 bits, ECC >= 256 bits.
                        if (SecKeyRef key = SecTrustCopyKey(trust)) {
                            if (CFDictionaryRef a = SecKeyCopyAttributes(key)) {
                                CFTypeRef type = CFDictionaryGetValue(a, kSecAttrKeyType);
                                CFNumberRef bits_ref = static_cast<CFNumberRef>(CFDictionaryGetValue(a, kSecAttrKeySizeInBits));
                                int bits = 0;
                                if (bits_ref) (void)CFNumberGetValue(bits_ref, kCFNumberIntType, &bits);
                                if (type && CFEqual(type, kSecAttrKeyTypeRSA) && bits < 2048) {
                                    ok = false;
                                    why = "peer RSA key shorter than 2048 bits";
                                } else if (type && CFEqual(type, kSecAttrKeyTypeECSECPrimeRandom) && bits < 256) {
                                    ok = false;
                                    why = "peer EC key shorter than 256 bits";
                                }
                                CFRelease(a);
                            }
                            CFRelease(key);
                        }
                    }
                    CFRelease(trust);
                } else {
                    why = "no peer certificate";
                }
                if (!ok) {
                    if (st) {
                        std::lock_guard<std::mutex> l(st->mu);
                        st->verify_error = why;
                    }
                    tls_log(vs, "TLS: peer certificate rejected: " + why);
                }
                complete(ok);
            },
            dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
        sec_release(sec);
    };
    return nw_parameters_create_secure_tcp(configure_tls, NW_PARAMETERS_DEFAULT_CONFIGURATION);
}

Result<std::unique_ptr<TlsTransport>> AppleContext::connect(const std::string& host, std::uint16_t port,
                                                            int connect_timeout_ms) {
    if (role_ != Role::Client) return Status(Errc::InvalidArgument, "connect() needs a client TLS context");
    if (host.empty()) return Status(Errc::InvalidArgument, "empty host");
    if (Status v = check_client_identity(settings_, expected_name(settings_, host)); !v) return v;
    const std::string port_text = std::to_string(port);
    nw_endpoint_t ep = nw_endpoint_create_host(host.c_str(), port_text.c_str());
    if (!ep) return Status(Errc::InvalidArgument, "invalid host");
    auto st = std::make_shared<ConnState>();
    nw_parameters_t params = make_parameters(expected_name(settings_, host), st);
    nw_connection_t c = params ? nw_connection_create(ep, params) : nullptr;
    nw_release(ep);
    if (params) nw_release(params);
    if (!c) return Status(Errc::NetworkError, "cannot create a connection");
    auto t = std::make_unique<AppleTransport>(shared_from_this(), c, st);
    const long total = static_cast<long>(connect_timeout_ms < 0 ? 0 : connect_timeout_ms) +
                       static_cast<long>(settings_.handshake_timeout_ms);
    if (Status h = t->handshake(total > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(total)); !h) return h;
    return std::unique_ptr<TlsTransport>(std::move(t));
}

// ---------------------------------------------------------------- transport
AppleTransport::AppleTransport(std::shared_ptr<AppleContext> ctx, nw_connection_t c, std::shared_ptr<ConnState> st)
    : ctx_(std::move(ctx)), conn_(c), st_(std::move(st)) {
    queue_ = dispatch_queue_create("siaara.tls", DISPATCH_QUEUE_SERIAL);
    if (nw_endpoint_t ep = nw_connection_copy_endpoint(conn_)) {
        if (nw_endpoint_get_type(ep) == nw_endpoint_type_address) {
            if (char* a = nw_endpoint_copy_address_string(ep)) {
                peer_ = a;
                std::free(a);
            }
        } else if (nw_endpoint_get_type(ep) == nw_endpoint_type_host) {
            if (const char* h = nw_endpoint_get_hostname(ep)) peer_ = h;
        }
        nw_release(ep);
    }
}

AppleTransport::~AppleTransport() {
    close();
    nw_release(conn_);
    dispatch_release(queue_);
}

Status AppleTransport::handshake(int timeout_ms) {
    if (done_) return Status::ok();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    std::shared_ptr<ConnState> st = st_;
    if (!started_) {
        started_ = true;
        nw_connection_set_queue(conn_, queue_);
        nw_connection_set_state_changed_handler(conn_, ^(nw_connection_state_t state, nw_error_t error) {
            std::lock_guard<std::mutex> l(st->mu);
            if (state == nw_connection_state_ready) {
                st->ready = true;
            } else if (state == nw_connection_state_failed || state == nw_connection_state_waiting) {
                // "waiting" (e.g. connection refused) would retry later; this transport fails at once.
                if (!st->failed) st->error = describe(error);
                st->failed = true;
            } else if (state == nw_connection_state_cancelled) {
                st->cancelled = true;
            }
            st->cv.notify_all();
        });
        nw_connection_start(conn_);
    }
    std::unique_lock<std::mutex> l(st_->mu);
    st_->cv.wait_until(l, deadline, [&] { return st_->ready || st_->failed || st_->cancelled; });
    const bool ok = st_->ready && !st_->failed;
    std::string why = !st_->verify_error.empty() ? "certificate verification failed: " + st_->verify_error
                      : st_->failed                ? st_->error
                      : st_->cancelled             ? std::string("connection cancelled")
                                                   : std::string("timed out");
    const bool timed_out = !st_->ready && !st_->failed && !st_->cancelled;
    l.unlock();
    if (!ok) {
        nw_connection_cancel(conn_);
        closed_ = true;
        return Status(timed_out ? Errc::Timeout : Errc::NetworkError, "TLS handshake with " + peer_ + " failed: " + why);
    }
    read_session();
    if (Status c = check_session(ctx_->settings(), ctx_->role(), info_, peer_); !c) {
        nw_connection_cancel(conn_);
        closed_ = true;
        return Status(Errc::NetworkError, "TLS handshake with " + peer_ + " failed: " + c.message);
    }
    done_ = true;
    return Status::ok();
}

void AppleTransport::read_session() {
    nw_protocol_definition_t def = nw_protocol_copy_tls_definition();
    nw_protocol_metadata_t md = nw_connection_copy_protocol_metadata(conn_, def);
    if (md) {
        sec_protocol_metadata_t smd = nw_tls_copy_sec_protocol_metadata(md);
        if (smd) {
            const tls_protocol_version_t v = sec_protocol_metadata_get_negotiated_tls_protocol_version(smd);
            info_.protocol = v == tls_protocol_version_TLSv13   ? "TLSv1.3"
                             : v == tls_protocol_version_TLSv12 ? "TLSv1.2"
                                                                : "version " + std::to_string(static_cast<int>(v));
            info_.cipher_suite =
                static_cast<std::uint16_t>(sec_protocol_metadata_get_negotiated_tls_ciphersuite(smd));
            __block bool has_peer = false;
            (void)sec_protocol_metadata_access_peer_certificate_chain(smd, ^(sec_certificate_t) { has_peer = true; });
            info_.peer_certificate = has_peer;
            sec_release(smd);
        }
        nw_release(md);
    }
    nw_release(def);
    if (const CipherSuite* cs = find_allowed_cipher_suite(info_.cipher_suite)) info_.cipher_name = cs->name;
    // A presented certificate got here only through the verify block (or the insecure flag).
    info_.peer_verified = info_.peer_certificate && !ctx_->settings().insecure_skip_validation;
}

void AppleTransport::start_receive_locked() {
    if (st_->recv_pending || st_->eof || !st_->rx_error.empty()) return;
    st_->recv_pending = true;
    std::shared_ptr<ConnState> st = st_;
    nw_connection_receive(conn_, 1, 65536,
                          ^(dispatch_data_t content, nw_content_context_t, bool is_complete, nw_error_t error) {
                              std::lock_guard<std::mutex> l(st->mu);
                              st->recv_pending = false;
                              if (content) {
                                  if (st->rx_pos == st->rx.size()) {
                                      st->rx.clear();
                                      st->rx_pos = 0;
                                  }
                                  (void)dispatch_data_apply(content, ^bool(dispatch_data_t, std::size_t, const void* p,
                                                                           std::size_t size) {
                                      const std::uint8_t* b = static_cast<const std::uint8_t*>(p);
                                      st->rx.insert(st->rx.end(), b, b + size);
                                      return true;
                                  });
                              }
                              if (error) st->rx_error = describe(error);
                              else if (is_complete) st->eof = true;  // the peer closed the stream
                              st->cv.notify_all();
                          });
}

Result<std::size_t> AppleTransport::read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) {
    if (!done_) return Status(Errc::NetworkError, "TLS handshake not completed");
    if (n == 0) return std::size_t{0};
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    std::unique_lock<std::mutex> l(st_->mu);
    while (true) {
        if (st_->rx_pos < st_->rx.size()) {
            const std::size_t k = std::min(n, st_->rx.size() - st_->rx_pos);
            std::memcpy(buf, st_->rx.data() + st_->rx_pos, k);
            st_->rx_pos += k;
            return k;
        }
        if (st_->eof) return std::size_t{0};
        if (!st_->rx_error.empty()) return Status(Errc::NetworkError, st_->rx_error);
        if (st_->cancelled || st_->failed) return Status(Errc::NetworkError, "connection closed");
        start_receive_locked();
        if (st_->cv.wait_until(l, deadline) == std::cv_status::timeout && st_->rx_pos >= st_->rx.size() && !st_->eof &&
            st_->rx_error.empty() && !st_->cancelled && !st_->failed)
            return Status(Errc::Timeout, "read timed out");
    }
}

Result<bool> AppleTransport::wait_readable(int timeout_ms) {
    if (!done_) return Status(Errc::NetworkError, "TLS handshake not completed");
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    std::unique_lock<std::mutex> l(st_->mu);
    while (true) {
        if (st_->rx_pos < st_->rx.size() || st_->eof || !st_->rx_error.empty() || st_->cancelled || st_->failed)
            return true;
        start_receive_locked();
        if (st_->cv.wait_until(l, deadline) == std::cv_status::timeout)
            return st_->rx_pos < st_->rx.size() || st_->eof || !st_->rx_error.empty() || st_->cancelled || st_->failed;
    }
}

Status AppleTransport::write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) {
    if (!done_) return Status(Errc::NetworkError, "TLS handshake not completed");
    std::shared_ptr<ConnState> st = st_;
    constexpr std::size_t kChunk = 256 * 1024;
    std::size_t off = 0;
    while (off < n) {
        const std::size_t k = std::min(kChunk, n - off);
        dispatch_data_t d = dispatch_data_create(data + off, k, queue_, DISPATCH_DATA_DESTRUCTOR_DEFAULT);  // copies
        if (!d) return Status(Errc::OutOfMemory, "out of memory");
        {
            std::lock_guard<std::mutex> l(st_->mu);
            if (!st_->tx_error.empty() || st_->cancelled || st_->failed)
                return Status(Errc::NetworkError, st_->tx_error.empty() ? std::string("connection closed") : st_->tx_error);
            ++st_->sends_pending;
        }
        nw_connection_send(conn_, d, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false, ^(nw_error_t error) {
            std::lock_guard<std::mutex> l2(st->mu);
            --st->sends_pending;
            if (error && st->tx_error.empty()) st->tx_error = describe(error);
            st->cv.notify_all();
        });
        dispatch_release(d);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        std::unique_lock<std::mutex> l(st_->mu);
        if (!st_->cv.wait_until(l, deadline, [&] { return st_->sends_pending == 0 || st_->cancelled; }))
            return Status(Errc::Timeout, "write timed out");
        if (!st_->tx_error.empty()) return Status(Errc::NetworkError, st_->tx_error);
        off += k;
    }
    return Status::ok();
}

void AppleTransport::shutdown() noexcept {
    nw_connection_cancel(conn_);  // thread safe; wakes readers through the cancelled state
    std::lock_guard<std::mutex> l(st_->mu);
    st_->cv.notify_all();
}

void AppleTransport::close() noexcept {
    if (closed_) return;
    closed_ = true;
    if (!started_) {
        nw_connection_cancel(conn_);
        return;
    }
    std::shared_ptr<ConnState> st = st_;
    bool can_send = false;
    {
        std::lock_guard<std::mutex> l(st_->mu);
        can_send = done_ && st_->ready && !st_->cancelled && !st_->failed;
        if (can_send) ++st_->sends_pending;
    }
    if (can_send) {
        // Write side closed: TLS close_notify, then TCP FIN.
        nw_connection_send(conn_, nullptr, NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true, ^(nw_error_t) {
            std::lock_guard<std::mutex> l2(st->mu);
            --st->sends_pending;
            st->cv.notify_all();
        });
        std::unique_lock<std::mutex> l(st_->mu);
        st_->cv.wait_for(l, std::chrono::milliseconds(1000), [&] { return st_->sends_pending == 0 || st_->cancelled; });
    }
    nw_connection_cancel(conn_);
    std::unique_lock<std::mutex> l(st_->mu);
    st_->cv.wait_for(l, std::chrono::milliseconds(1000), [&] { return st_->cancelled; });
}

bool AppleTransport::is_open() const noexcept {
    if (closed_) return false;
    std::lock_guard<std::mutex> l(st_->mu);
    return !st_->cancelled && !st_->failed;
}

// ---------------------------------------------------------------- listener
struct ListenState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<nw_connection_t> pending;
    bool ready = false;
    bool failed = false;
    bool closed = false;
    std::string error;
};

class AppleListener final : public TlsListenerImpl {
public:
    explicit AppleListener(std::shared_ptr<AppleContext> ctx) : ctx_(std::move(ctx)), st_(std::make_shared<ListenState>()) {
        queue_ = dispatch_queue_create("siaara.tls.listener", DISPATCH_QUEUE_SERIAL);
    }
    ~AppleListener() override {
        close();
        if (listener_) nw_release(listener_);
        dispatch_release(queue_);
    }

    Status listen(const std::string& bind_address, std::uint16_t port, int backlog) {
        const std::string port_text = std::to_string(port);
        nw_parameters_t params = ctx_->make_parameters(std::string(), nullptr);
        if (!params) return Status(Errc::NetworkError, "cannot create TLS parameters");
        nw_parameters_set_reuse_local_address(params, true);
        if (!bind_address.empty()) {
            nw_endpoint_t local = nw_endpoint_create_host(bind_address.c_str(), port_text.c_str());
            if (!local) {
                nw_release(params);
                return Status(Errc::InvalidArgument, "invalid bind address");
            }
            nw_parameters_set_local_endpoint(params, local);
            nw_release(local);
            listener_ = nw_listener_create(params);
        } else {
            listener_ = nw_listener_create_with_port(port_text.c_str(), params);
        }
        nw_release(params);
        if (!listener_) return Status(Errc::NetworkError, "cannot listen on port " + port_text);
        std::shared_ptr<ListenState> st = st_;
        const std::size_t max_pending = backlog > 0 ? static_cast<std::size_t>(backlog) : 64;
        nw_listener_set_queue(listener_, queue_);
        nw_listener_set_state_changed_handler(listener_, ^(nw_listener_state_t state, nw_error_t error) {
            std::lock_guard<std::mutex> l(st->mu);
            if (state == nw_listener_state_ready) st->ready = true;
            else if (state == nw_listener_state_failed || state == nw_listener_state_waiting) {
                st->failed = true;
                st->error = describe(error);
            } else if (state == nw_listener_state_cancelled) {
                st->closed = true;
            }
            st->cv.notify_all();
        });
        nw_listener_set_new_connection_handler(listener_, ^(nw_connection_t c) {
            std::lock_guard<std::mutex> l(st->mu);
            if (st->closed || st->pending.size() >= max_pending) {
                nw_connection_cancel(c);  // not retained: Network.framework releases it
                return;
            }
            nw_retain(c);
            st->pending.push_back(c);
            st->cv.notify_all();
        });
        nw_listener_start(listener_);
        std::unique_lock<std::mutex> l(st_->mu);
        st_->cv.wait_for(l, std::chrono::seconds(10), [&] { return st_->ready || st_->failed; });
        if (!st_->ready) return Status(Errc::NetworkError, "cannot listen: " + (st_->failed ? st_->error : "timed out"));
        port_ = nw_listener_get_port(listener_);
        return Status::ok();
    }

    std::uint16_t port() const noexcept override { return port_; }

    Result<std::unique_ptr<TlsTransport>> accept(int timeout_ms) override {
        nw_connection_t c = nullptr;
        {
            std::unique_lock<std::mutex> l(st_->mu);
            st_->cv.wait_for(l, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms),
                             [&] { return !st_->pending.empty() || st_->closed || st_->failed; });
            if (st_->closed || st_->failed) return Status(Errc::NetworkError, "listener closed");
            if (st_->pending.empty()) return Status(Errc::Timeout, "no connection");
            c = st_->pending.front();
            st_->pending.pop_front();
        }
        return std::unique_ptr<TlsTransport>(std::make_unique<AppleTransport>(ctx_, c, std::make_shared<ConnState>()));
    }

    void close() noexcept override {
        std::deque<nw_connection_t> drop;
        {
            std::lock_guard<std::mutex> l(st_->mu);
            st_->closed = true;
            drop.swap(st_->pending);
        }
        for (nw_connection_t c : drop) {
            nw_connection_cancel(c);
            nw_release(c);
        }
        if (listener_ && !cancelled_) {
            cancelled_ = true;
            nw_listener_cancel(listener_);
        }
    }

    bool is_open() const noexcept override {
        std::lock_guard<std::mutex> l(st_->mu);
        return st_->ready && !st_->closed && !st_->failed;
    }

private:
    std::shared_ptr<AppleContext> ctx_;
    std::shared_ptr<ListenState> st_;
    dispatch_queue_t queue_;
    nw_listener_t listener_ = nullptr;
    bool cancelled_ = false;
    std::uint16_t port_ = 0;
};

Result<std::unique_ptr<detail::TlsListenerImpl>> AppleContext::make_listener(const std::string& bind_address,
                                                                             std::uint16_t port, int backlog) {
    auto l = std::make_unique<AppleListener>(shared_from_this());
    if (Status s = l->listen(bind_address, port, backlog); !s) return s;
    return std::unique_ptr<detail::TlsListenerImpl>(std::move(l));
}

}  // namespace

Result<std::shared_ptr<TlsContext>> backend_create_context(const TlsSettings& s, TlsContext::Role role) {
    auto ctx = std::shared_ptr<AppleContext>(new AppleContext(s, role));
    if (Status st = ctx->init(); !st) return st;
    return std::shared_ptr<TlsContext>(std::move(ctx));
}

bool backend_available(std::string&) { return true; }

std::string backend_name() { return "Network.framework"; }

}  // namespace siaara::net::detail

#endif  // __APPLE__
