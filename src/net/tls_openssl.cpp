// SIAARA DICOM Toolkit - TLS backend for Linux (and other non-Apple POSIX systems): the system's
// OpenSSL 3.x, loaded at run time.
//
// Nothing is linked at build time and no OpenSSL header is needed. On first use libcrypto.so.3 and
// libssl.so.3 are opened by soname (the dynamic loader's system search path; never a path relative
// to the current directory), the version is checked (>= 3.0.0) and every entry point below is
// resolved before any is called. The libraries stay loaded for the life of the process.
//
// The prototypes are written from the OpenSSL 3.0 manual pages; every OpenSSL object is opaque
// here (void*). Constants are the documented numeric values of the macros named in the comments.
//
// TLS runs over memory BIOs: OpenSSL never touches the socket. EngineTransport (tls.cpp) moves
// the bytes with the toolkit's own timeouts.
#if !defined(_WIN32) && !defined(__APPLE__)

#include <dlfcn.h>

#include <climits>
#include <cstring>
#include <mutex>

#include "net/tls_internal.hpp"

namespace siaara::net::detail {

namespace {

// ---------------------------------------------------------------- documented constants
constexpr int kCtrlMode = 33;                // SSL_CTRL_MODE
constexpr int kCtrlSetTlsextHostname = 55;   // SSL_CTRL_SET_TLSEXT_HOSTNAME
constexpr int kCtrlChainCert = 89;           // SSL_CTRL_CHAIN_CERT
constexpr int kCtrlSetGroupsList = 92;       // SSL_CTRL_SET_GROUPS_LIST
constexpr int kCtrlSetMinProto = 123;        // SSL_CTRL_SET_MIN_PROTO_VERSION
constexpr int kCtrlSetMaxProto = 124;        // SSL_CTRL_SET_MAX_PROTO_VERSION
constexpr long kTls12 = 0x0303;              // TLS1_2_VERSION
constexpr long kTls13 = 0x0304;              // TLS1_3_VERSION
constexpr long kNameTypeHost = 0;            // TLSEXT_NAMETYPE_host_name
constexpr std::uint64_t kOpNoCompression = std::uint64_t{1} << 17;        // SSL_OP_NO_COMPRESSION
constexpr std::uint64_t kOpServerPreference = std::uint64_t{1} << 22;     // SSL_OP_CIPHER_SERVER_PREFERENCE
constexpr std::uint64_t kOpNoRenegotiation = std::uint64_t{1} << 30;      // SSL_OP_NO_RENEGOTIATION
constexpr long kModeAcceptMovingWriteBuffer = 0x2;                        // SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
constexpr int kVerifyNone = 0;               // SSL_VERIFY_NONE
constexpr int kVerifyPeer = 1;               // SSL_VERIFY_PEER
constexpr int kVerifyFailIfNoPeerCert = 2;   // SSL_VERIFY_FAIL_IF_NO_PEER_CERT
constexpr int kFiletypePem = 1;              // SSL_FILETYPE_PEM
constexpr int kErrorWantRead = 2;            // SSL_ERROR_WANT_READ
constexpr int kErrorWantWrite = 3;           // SSL_ERROR_WANT_WRITE
constexpr int kErrorZeroReturn = 6;          // SSL_ERROR_ZERO_RETURN
constexpr int kBioSetMemEofReturn = 130;     // BIO_C_SET_BUF_MEM_EOF_RETURN
constexpr unsigned kNoPartialWildcards = 0x4;  // X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS
constexpr std::uint64_t kInitLoadSslStrings = 0x00200000;     // OPENSSL_INIT_LOAD_SSL_STRINGS
constexpr std::uint64_t kInitLoadCryptoStrings = 0x00000002;  // OPENSSL_INIT_LOAD_CRYPTO_STRINGS
constexpr int kVersionText = 0;              // OPENSSL_VERSION
constexpr int kPkeyRsa = 6;                  // EVP_PKEY_RSA (NID_rsaEncryption)
constexpr int kPkeyRsaPss = 912;             // EVP_PKEY_RSA_PSS
constexpr int kPkeyEc = 408;                 // EVP_PKEY_EC (NID_X9_62_id_ecPublicKey)
constexpr unsigned long kMinVersion = 0x30000000UL;  // OpenSSL 3.0.0

using PasswordCb = int (*)(char*, int, int, void*);
using VerifyCb = int (*)(int, void*);

// ---------------------------------------------------------------- entry points
struct Api {
    // libcrypto
    unsigned long (*OpenSSL_version_num)();
    const char* (*OpenSSL_version)(int);
    unsigned long (*ERR_get_error)();
    void (*ERR_error_string_n)(unsigned long, char*, std::size_t);
    void (*ERR_clear_error)();
    const void* (*BIO_s_mem)();
    void* (*BIO_new)(const void*);
    int (*BIO_free)(void*);
    int (*BIO_read)(void*, void*, int);
    int (*BIO_write)(void*, const void*, int);
    long (*BIO_ctrl)(void*, int, long, void*);
    std::size_t (*BIO_ctrl_pending)(void*);
    int (*X509_VERIFY_PARAM_set1_ip_asc)(void*, const char*);
    void (*X509_VERIFY_PARAM_set_hostflags)(void*, unsigned int);
    const char* (*X509_verify_cert_error_string)(long);
    void* (*X509_get0_pubkey)(const void*);
    void (*X509_free)(void*);
    int (*EVP_PKEY_get_bits)(const void*);
    int (*EVP_PKEY_get_base_id)(const void*);
    void (*EVP_PKEY_free)(void*);
    void* (*d2i_PKCS12)(void**, const unsigned char**, long);
    int (*PKCS12_parse)(void*, const char*, void**, void**, void**);
    void (*PKCS12_free)(void*);
    int (*OPENSSL_sk_num)(const void*);
    void* (*OPENSSL_sk_value)(const void*, int);
    void (*OPENSSL_sk_free)(void*);
    // libssl
    int (*OPENSSL_init_ssl)(std::uint64_t, const void*);
    const void* (*TLS_client_method)();
    const void* (*TLS_server_method)();
    void* (*SSL_CTX_new)(const void*);
    void (*SSL_CTX_free)(void*);
    long (*SSL_CTX_ctrl)(void*, int, long, void*);
    std::uint64_t (*SSL_CTX_set_options)(void*, std::uint64_t);
    int (*SSL_CTX_set_cipher_list)(void*, const char*);
    int (*SSL_CTX_set_ciphersuites)(void*, const char*);
    void (*SSL_CTX_set_security_level)(void*, int);
    void (*SSL_CTX_set_verify)(void*, int, VerifyCb);
    int (*SSL_CTX_load_verify_file)(void*, const char*);
    int (*SSL_CTX_set_default_verify_paths)(void*);
    int (*SSL_CTX_use_certificate_chain_file)(void*, const char*);
    int (*SSL_CTX_use_PrivateKey_file)(void*, const char*, int);
    int (*SSL_CTX_use_certificate)(void*, void*);
    int (*SSL_CTX_use_PrivateKey)(void*, void*);
    int (*SSL_CTX_check_private_key)(const void*);
    void (*SSL_CTX_set_default_passwd_cb)(void*, PasswordCb);
    void (*SSL_CTX_set_default_passwd_cb_userdata)(void*, void*);
    int (*SSL_CTX_set_session_id_context)(void*, const unsigned char*, unsigned int);
    void* (*SSL_load_client_CA_file)(const char*);
    void (*SSL_CTX_set_client_CA_list)(void*, void*);
    void* (*SSL_new)(void*);
    void (*SSL_free)(void*);
    void (*SSL_set_bio)(void*, void*, void*);
    void (*SSL_set_connect_state)(void*);
    void (*SSL_set_accept_state)(void*);
    int (*SSL_do_handshake)(void*);
    int (*SSL_read)(void*, void*, int);
    int (*SSL_write)(void*, const void*, int);
    int (*SSL_get_error)(const void*, int);
    int (*SSL_shutdown)(void*);
    long (*SSL_ctrl)(void*, int, long, void*);
    int (*SSL_set1_host)(void*, const char*);
    void* (*SSL_get0_param)(void*);
    long (*SSL_get_verify_result)(const void*);
    int (*SSL_version)(const void*);
    const void* (*SSL_get_current_cipher)(const void*);
    std::uint16_t (*SSL_CIPHER_get_protocol_id)(const void*);
    void* (*SSL_get0_peer_certificate)(const void*);
};

struct Library {
    bool ok = false;
    std::string reason;
    std::string version;
    Api api{};
};

template <class F>
bool resolve(void* lib, const char* name, F& fn, std::string& missing) {
    static_assert(sizeof(F) == sizeof(void*), "function and object pointers must have the same size");
    void* p = ::dlsym(lib, name);
    if (!p) {
        if (missing.empty()) missing = name;
        return false;
    }
    std::memcpy(&fn, &p, sizeof fn);
    return true;
}

#define SIAARA_SSL_SYM(lib, name) ok = resolve(lib, #name, a.name, missing) && ok

void load(Library& L) {
    // By soname only: the loader searches its configured system paths (ld.so.cache, /lib, /usr/lib,
    // LD_LIBRARY_PATH as set by the administrator); a name without '/' is never looked up in the
    // current directory.
    void* crypto = ::dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!crypto) {
        L.reason = "the system OpenSSL 3 library (libcrypto.so.3) was not found";
        return;
    }
    void* ssl = ::dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!ssl) {
        L.reason = "the system OpenSSL 3 library (libssl.so.3) was not found";
        return;
    }
    Api a{};
    std::string missing;
    bool ok = true;
    SIAARA_SSL_SYM(crypto, OpenSSL_version_num);
    SIAARA_SSL_SYM(crypto, OpenSSL_version);
    if (!ok) {
        L.reason = "libcrypto.so.3 lacks " + missing;
        return;
    }
    // Refuse a library that the loader found through a relative search path entry.
    for (void* lib : {crypto, ssl}) {
        void* sym = ::dlsym(lib, lib == crypto ? "OpenSSL_version_num" : "SSL_new");
        Dl_info info{};
        if (!sym || ::dladdr(sym, &info) == 0 || !info.dli_fname || info.dli_fname[0] != '/') {
            L.reason = "refusing an OpenSSL library that was not loaded from an absolute system path";
            return;
        }
    }
    const unsigned long v = a.OpenSSL_version_num();
    if (v < kMinVersion) {
        L.reason = "OpenSSL 3.0 or newer is required";
        return;
    }
    SIAARA_SSL_SYM(crypto, ERR_get_error);
    SIAARA_SSL_SYM(crypto, ERR_error_string_n);
    SIAARA_SSL_SYM(crypto, ERR_clear_error);
    SIAARA_SSL_SYM(crypto, BIO_s_mem);
    SIAARA_SSL_SYM(crypto, BIO_new);
    SIAARA_SSL_SYM(crypto, BIO_free);
    SIAARA_SSL_SYM(crypto, BIO_read);
    SIAARA_SSL_SYM(crypto, BIO_write);
    SIAARA_SSL_SYM(crypto, BIO_ctrl);
    SIAARA_SSL_SYM(crypto, BIO_ctrl_pending);
    SIAARA_SSL_SYM(crypto, X509_VERIFY_PARAM_set1_ip_asc);
    SIAARA_SSL_SYM(crypto, X509_VERIFY_PARAM_set_hostflags);
    SIAARA_SSL_SYM(crypto, X509_verify_cert_error_string);
    SIAARA_SSL_SYM(crypto, X509_get0_pubkey);
    SIAARA_SSL_SYM(crypto, X509_free);
    SIAARA_SSL_SYM(crypto, EVP_PKEY_get_bits);
    SIAARA_SSL_SYM(crypto, EVP_PKEY_get_base_id);
    SIAARA_SSL_SYM(crypto, EVP_PKEY_free);
    SIAARA_SSL_SYM(crypto, d2i_PKCS12);
    SIAARA_SSL_SYM(crypto, PKCS12_parse);
    SIAARA_SSL_SYM(crypto, PKCS12_free);
    SIAARA_SSL_SYM(crypto, OPENSSL_sk_num);
    SIAARA_SSL_SYM(crypto, OPENSSL_sk_value);
    SIAARA_SSL_SYM(crypto, OPENSSL_sk_free);
    SIAARA_SSL_SYM(ssl, OPENSSL_init_ssl);
    SIAARA_SSL_SYM(ssl, TLS_client_method);
    SIAARA_SSL_SYM(ssl, TLS_server_method);
    SIAARA_SSL_SYM(ssl, SSL_CTX_new);
    SIAARA_SSL_SYM(ssl, SSL_CTX_free);
    SIAARA_SSL_SYM(ssl, SSL_CTX_ctrl);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_options);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_cipher_list);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_ciphersuites);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_security_level);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_verify);
    SIAARA_SSL_SYM(ssl, SSL_CTX_load_verify_file);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_default_verify_paths);
    SIAARA_SSL_SYM(ssl, SSL_CTX_use_certificate_chain_file);
    SIAARA_SSL_SYM(ssl, SSL_CTX_use_PrivateKey_file);
    SIAARA_SSL_SYM(ssl, SSL_CTX_use_certificate);
    SIAARA_SSL_SYM(ssl, SSL_CTX_use_PrivateKey);
    SIAARA_SSL_SYM(ssl, SSL_CTX_check_private_key);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_default_passwd_cb);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_default_passwd_cb_userdata);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_session_id_context);
    SIAARA_SSL_SYM(ssl, SSL_load_client_CA_file);
    SIAARA_SSL_SYM(ssl, SSL_CTX_set_client_CA_list);
    SIAARA_SSL_SYM(ssl, SSL_new);
    SIAARA_SSL_SYM(ssl, SSL_free);
    SIAARA_SSL_SYM(ssl, SSL_set_bio);
    SIAARA_SSL_SYM(ssl, SSL_set_connect_state);
    SIAARA_SSL_SYM(ssl, SSL_set_accept_state);
    SIAARA_SSL_SYM(ssl, SSL_do_handshake);
    SIAARA_SSL_SYM(ssl, SSL_read);
    SIAARA_SSL_SYM(ssl, SSL_write);
    SIAARA_SSL_SYM(ssl, SSL_get_error);
    SIAARA_SSL_SYM(ssl, SSL_shutdown);
    SIAARA_SSL_SYM(ssl, SSL_ctrl);
    SIAARA_SSL_SYM(ssl, SSL_set1_host);
    SIAARA_SSL_SYM(ssl, SSL_get0_param);
    SIAARA_SSL_SYM(ssl, SSL_get_verify_result);
    SIAARA_SSL_SYM(ssl, SSL_version);
    SIAARA_SSL_SYM(ssl, SSL_get_current_cipher);
    SIAARA_SSL_SYM(ssl, SSL_CIPHER_get_protocol_id);
    SIAARA_SSL_SYM(ssl, SSL_get0_peer_certificate);
    if (!ok) {
        L.reason = "the system OpenSSL lacks " + missing;
        return;
    }
    if (a.OPENSSL_init_ssl(kInitLoadSslStrings | kInitLoadCryptoStrings, nullptr) != 1) {
        L.reason = "OpenSSL initialisation failed";
        return;
    }
    // "OpenSSL 3.0.13 30 Jan 2024" -> "OpenSSL 3.0.13"
    std::string text = a.OpenSSL_version(kVersionText) ? a.OpenSSL_version(kVersionText) : "OpenSSL 3";
    const std::size_t sp = text.find(' ');
    const std::size_t sp2 = sp == std::string::npos ? sp : text.find(' ', sp + 1);
    L.version = sp2 == std::string::npos ? text : text.substr(0, sp2);
    L.api = a;
    L.ok = true;
}

#undef SIAARA_SSL_SYM

const Library& library() {
    static Library lib;
    static std::once_flag once;
    std::call_once(once, [] { load(lib); });
    return lib;
}

std::string errors(const Api& a) {
    std::string s;
    int n = 0;
    while (const unsigned long e = a.ERR_get_error()) {
        if (n++ >= 4) continue;
        char buf[256] = {0};
        a.ERR_error_string_n(e, buf, sizeof buf);
        if (!s.empty()) s += "; ";
        s += buf;
    }
    return s.empty() ? std::string("unspecified OpenSSL error") : s;
}

int password_cb(char* buf, int size, int, void* user) {
    const std::string* pw = static_cast<const std::string*>(user);
    if (!pw || size <= 0) return 0;
    const std::size_t n = pw->size() < static_cast<std::size_t>(size) ? pw->size() : static_cast<std::size_t>(size);
    std::memcpy(buf, pw->data(), n);
    return static_cast<int>(n);
}

int accept_any_certificate(int, void*) { return 1; }

std::string protocol_name(int v) {
    if (v == kTls13) return "TLSv1.3";
    if (v == kTls12) return "TLSv1.2";
    return "version " + std::to_string(v);
}

// ---------------------------------------------------------------- context
class OsslContext final : public EngineContext {
public:
    OsslContext(const TlsSettings& s, Role r) : EngineContext(s, r), api_(library().api) {}
    ~OsslContext() override {
        if (ctx_) api_.SSL_CTX_free(ctx_);
        secure_wipe(password_);
    }
    Status init();
    const Api& api() const noexcept { return api_; }

protected:
    Result<std::unique_ptr<TlsEngine>> make_engine(const std::string& peer_name) override;

private:
    const Api& api_;
    void* ctx_ = nullptr;
    std::string password_;  // read by password_cb while loading the identity

    Status fail(const std::string& what) { return Status(Errc::InvalidArgument, what + ": " + errors(api_)); }
    Status load_identity();
};

Status OsslContext::load_identity() {
    const TlsIdentity& id = settings_.identity;
    if (!id.windows_thumbprint.empty() || !id.keychain_identity.empty())
        return Status(Errc::Unsupported, "OS certificate stores are not available with OpenSSL; use PEM or PKCS#12 files");
    if (!id.certificate_file.empty()) {
        password_ = id.password;
        api_.SSL_CTX_set_default_passwd_cb(ctx_, &password_cb);
        api_.SSL_CTX_set_default_passwd_cb_userdata(ctx_, &password_);
        if (api_.SSL_CTX_use_certificate_chain_file(ctx_, id.certificate_file.c_str()) != 1)
            return fail("cannot load the certificate chain " + id.certificate_file);
        const int k = api_.SSL_CTX_use_PrivateKey_file(ctx_, id.private_key_file.c_str(), kFiletypePem);
        api_.SSL_CTX_set_default_passwd_cb_userdata(ctx_, nullptr);
        secure_wipe(password_);
        if (k != 1) return fail("cannot load the private key " + id.private_key_file);
    } else if (!id.pkcs12_file.empty()) {
        Result<std::vector<std::uint8_t>> der = read_secret_file(id.pkcs12_file);
        if (!der) return der.take_error();
        std::vector<std::uint8_t> bytes = std::move(der).value();
        const unsigned char* p = bytes.data();
        void* p12 = api_.d2i_PKCS12(nullptr, &p, static_cast<long>(bytes.size()));
        secure_wipe(bytes);
        if (!p12) return fail("cannot parse the PKCS#12 file " + id.pkcs12_file);
        void* pkey = nullptr;
        void* cert = nullptr;
        void* chain = nullptr;
        const int parsed = api_.PKCS12_parse(p12, id.password.c_str(), &pkey, &cert, &chain);
        api_.PKCS12_free(p12);
        Status st;
        if (parsed != 1 || !pkey || !cert) {
            st = fail("cannot decrypt the PKCS#12 file " + id.pkcs12_file + " (wrong password?)");
        } else if (api_.SSL_CTX_use_certificate(ctx_, cert) != 1 || api_.SSL_CTX_use_PrivateKey(ctx_, pkey) != 1) {
            st = fail("cannot use the PKCS#12 identity");
        } else if (chain) {
            const int n = api_.OPENSSL_sk_num(chain);
            for (int i = 0; i < n && st; ++i)
                if (api_.SSL_CTX_ctrl(ctx_, kCtrlChainCert, 1, api_.OPENSSL_sk_value(chain, i)) != 1)
                    st = fail("cannot add a chain certificate");
        }
        if (chain) {
            const int n = api_.OPENSSL_sk_num(chain);
            for (int i = 0; i < n; ++i) api_.X509_free(api_.OPENSSL_sk_value(chain, i));
            api_.OPENSSL_sk_free(chain);
        }
        if (cert) api_.X509_free(cert);
        if (pkey) api_.EVP_PKEY_free(pkey);
        if (!st) return st;
    } else {
        return Status::ok();
    }
    if (api_.SSL_CTX_check_private_key(ctx_) != 1) return fail("the private key does not match the certificate");
    return Status::ok();
}

Status OsslContext::init() {
    const bool server = role_ == Role::Server;
    const TlsSettings& s = settings_;
    api_.ERR_clear_error();
    ctx_ = api_.SSL_CTX_new(server ? api_.TLS_server_method() : api_.TLS_client_method());
    if (!ctx_) return fail("SSL_CTX_new failed");
    const long min = s.min_version == TlsMinVersion::Tls13 ? kTls13 : kTls12;
    if (api_.SSL_CTX_ctrl(ctx_, kCtrlSetMinProto, min, nullptr) != 1 ||
        api_.SSL_CTX_ctrl(ctx_, kCtrlSetMaxProto, kTls13, nullptr) != 1)
        return fail("cannot set the TLS protocol versions");
    (void)api_.SSL_CTX_set_options(ctx_, kOpNoCompression | kOpNoRenegotiation | (server ? kOpServerPreference : 0));
    (void)api_.SSL_CTX_ctrl(ctx_, kCtrlMode, kModeAcceptMovingWriteBuffer, nullptr);
    // TLS 1.2: ECDHE + AEAD only, in the order of detail::allowed_cipher_suites().
    if (api_.SSL_CTX_set_cipher_list(ctx_,
                                     "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
                                     "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
                                     "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305") != 1 ||
        api_.SSL_CTX_set_ciphersuites(ctx_, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256") != 1)
        return fail("cannot set the cipher suites");
    char groups[] = "X25519:P-256:P-384:P-521";
    if (api_.SSL_CTX_ctrl(ctx_, kCtrlSetGroupsList, 0, groups) != 1) return fail("cannot set the key exchange groups");
    // Level 2: RSA/DH keys >= 2048 bits, ECC >= 224 bits, no SHA-1 signatures (finish() also
    // checks the peer key against the PS3.15 minimums).
    api_.SSL_CTX_set_security_level(ctx_, 2);
    if (!s.ca_file.empty()) {
        if (api_.SSL_CTX_load_verify_file(ctx_, s.ca_file.c_str()) != 1) return fail("cannot load the CA file " + s.ca_file);
    } else if (api_.SSL_CTX_set_default_verify_paths(ctx_) != 1) {
        return fail("cannot load the system trust store");
    }
    if (server) {
        static const unsigned char sid[] = {'s', 'i', 'a', 'a', 'r', 'a'};
        (void)api_.SSL_CTX_set_session_id_context(ctx_, sid, static_cast<unsigned>(sizeof sid));
        if (s.require_client_certificate) {
            api_.SSL_CTX_set_verify(ctx_, kVerifyPeer | kVerifyFailIfNoPeerCert,
                                    s.insecure_skip_validation ? &accept_any_certificate : nullptr);
            if (!s.ca_file.empty())
                if (void* names = api_.SSL_load_client_CA_file(s.ca_file.c_str())) api_.SSL_CTX_set_client_CA_list(ctx_, names);
        } else {
            api_.SSL_CTX_set_verify(ctx_, kVerifyNone, nullptr);
        }
    } else {
        api_.SSL_CTX_set_verify(ctx_, s.insecure_skip_validation ? kVerifyNone : kVerifyPeer, nullptr);
    }
    if (Status st = load_identity(); !st) return st;
    api_.ERR_clear_error();
    return Status::ok();
}

// ---------------------------------------------------------------- engine
class OsslEngine final : public TlsEngine {
public:
    OsslEngine(const Api& a, const TlsSettings& s, bool server) : a_(a), s_(s), server_(server) {}
    ~OsslEngine() override {
        if (ssl_) a_.SSL_free(ssl_);  // also frees both BIOs
    }

    Status init(void* ctx, const std::string& peer_name) {
        a_.ERR_clear_error();
        ssl_ = a_.SSL_new(ctx);
        if (!ssl_) return Status(Errc::OutOfMemory, "SSL_new failed: " + errors(a_));
        void* rbio = a_.BIO_new(a_.BIO_s_mem());
        void* wbio = a_.BIO_new(a_.BIO_s_mem());
        if (!rbio || !wbio) {
            if (rbio) a_.BIO_free(rbio);
            if (wbio) a_.BIO_free(wbio);
            return Status(Errc::OutOfMemory, "BIO_new failed");
        }
        (void)a_.BIO_ctrl(rbio, kBioSetMemEofReturn, -1, nullptr);  // empty: "retry", not EOF
        (void)a_.BIO_ctrl(wbio, kBioSetMemEofReturn, -1, nullptr);
        a_.SSL_set_bio(ssl_, rbio, wbio);
        rbio_ = rbio;
        wbio_ = wbio;
        if (server_) {
            a_.SSL_set_accept_state(ssl_);
            return Status::ok();
        }
        a_.SSL_set_connect_state(ssl_);
        if (peer_name.empty()) return Status::ok();
        if (is_ip_literal(peer_name)) {
            // No SNI for addresses (RFC 6066 section 3); match an iPAddress subjectAltName.
            if (!s_.insecure_skip_validation &&
                a_.X509_VERIFY_PARAM_set1_ip_asc(a_.SSL_get0_param(ssl_), peer_name.c_str()) != 1)
                return Status(Errc::InvalidArgument, "invalid IP address " + peer_name);
        } else {
            std::string name = peer_name;
            if (a_.SSL_ctrl(ssl_, kCtrlSetTlsextHostname, kNameTypeHost, name.data()) != 1)
                return Status(Errc::InvalidArgument, "invalid server name " + peer_name);
            if (!s_.insecure_skip_validation) {
                a_.X509_VERIFY_PARAM_set_hostflags(a_.SSL_get0_param(ssl_), kNoPartialWildcards);
                if (a_.SSL_set1_host(ssl_, peer_name.c_str()) != 1)
                    return Status(Errc::InvalidArgument, "invalid server name " + peer_name);
            }
        }
        return Status::ok();
    }

    Status feed(const std::uint8_t* p, std::size_t n) override {
        while (n) {
            const int k = n > (1u << 20) ? (1 << 20) : static_cast<int>(n);
            if (a_.BIO_write(rbio_, p, k) != k) return Status(Errc::OutOfMemory, "BIO_write failed");
            p += k;
            n -= static_cast<std::size_t>(k);
        }
        return Status::ok();
    }

    Result<Step> handshake(std::vector<std::uint8_t>& out) override {
        a_.ERR_clear_error();
        const int r = a_.SSL_do_handshake(ssl_);
        drain(out);
        if (r == 1) return Step::Done;
        const int e = a_.SSL_get_error(ssl_, r);
        if (e == kErrorWantRead || e == kErrorWantWrite) return Step::NeedInput;
        const long v = a_.SSL_get_verify_result(ssl_);
        if (v != 0) {
            const char* why = a_.X509_verify_cert_error_string(v);
            a_.ERR_clear_error();
            return Status(Errc::NetworkError, std::string("certificate verification failed: ") + (why ? why : "?"));
        }
        return Status(Errc::NetworkError, errors(a_));
    }

    Status finish(TlsSessionInfo& info) override {
        info.protocol = protocol_name(a_.SSL_version(ssl_));
        const void* c = a_.SSL_get_current_cipher(ssl_);
        info.cipher_suite = c ? a_.SSL_CIPHER_get_protocol_id(c) : std::uint16_t{0};
        if (const CipherSuite* cs = find_allowed_cipher_suite(info.cipher_suite)) info.cipher_name = cs->name;
        void* peer = a_.SSL_get0_peer_certificate(ssl_);
        info.peer_certificate = peer != nullptr;
        info.peer_verified = peer && !s_.insecure_skip_validation && a_.SSL_get_verify_result(ssl_) == 0;
        if (!peer) {
            if (!server_) return Status(Errc::NetworkError, "the server sent no certificate");
            if (s_.require_client_certificate) return Status(Errc::NetworkError, "the client sent no certificate");
            return Status::ok();
        }
        if (const void* key = a_.X509_get0_pubkey(peer)) {
            const int id = a_.EVP_PKEY_get_base_id(key);
            const int bits = a_.EVP_PKEY_get_bits(key);
            if ((id == kPkeyRsa || id == kPkeyRsaPss) && bits < 2048)
                return Status(Errc::NetworkError, "peer RSA key shorter than 2048 bits");
            if (id == kPkeyEc && bits < 256) return Status(Errc::NetworkError, "peer EC key shorter than 256 bits");
        }
        return Status::ok();
    }

    Status decrypt(std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& out, bool& closed) override {
        constexpr int kChunk = 16384;
        while (true) {
            a_.ERR_clear_error();
            const std::size_t old = plain.size();
            plain.resize(old + kChunk);
            const int r = a_.SSL_read(ssl_, plain.data() + old, kChunk);
            if (r > 0) {
                plain.resize(old + static_cast<std::size_t>(r));
                continue;
            }
            plain.resize(old);
            const int e = a_.SSL_get_error(ssl_, r);
            drain(out);
            if (e == kErrorWantRead || e == kErrorWantWrite) return Status::ok();
            if (e == kErrorZeroReturn) {
                closed = true;
                return Status::ok();
            }
            return Status(Errc::NetworkError, errors(a_));
        }
    }

    Status encrypt(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) override {
        if (n == 0) return Status::ok();
        a_.ERR_clear_error();
        const int r = a_.SSL_write(ssl_, p, static_cast<int>(n));
        drain(out);
        if (r != static_cast<int>(n)) return Status(Errc::NetworkError, errors(a_));
        return Status::ok();
    }

    void close_notify(std::vector<std::uint8_t>& out) noexcept override {
        a_.ERR_clear_error();
        (void)a_.SSL_shutdown(ssl_);
        a_.ERR_clear_error();
        drain(out);
    }

private:
    const Api& a_;
    const TlsSettings& s_;
    bool server_;
    void* ssl_ = nullptr;
    void* rbio_ = nullptr;
    void* wbio_ = nullptr;

    void drain(std::vector<std::uint8_t>& out) {
        while (true) {
            const std::size_t pending = a_.BIO_ctrl_pending(wbio_);
            if (pending == 0) return;
            const std::size_t old = out.size();
            const int want = pending > (1u << 20) ? (1 << 20) : static_cast<int>(pending);
            out.resize(old + static_cast<std::size_t>(want));
            const int r = a_.BIO_read(wbio_, out.data() + old, want);
            if (r <= 0) {
                out.resize(old);
                return;
            }
            out.resize(old + static_cast<std::size_t>(r));
        }
    }
};

Result<std::unique_ptr<TlsEngine>> OsslContext::make_engine(const std::string& peer_name) {
    auto e = std::make_unique<OsslEngine>(api_, settings_, role_ == Role::Server);
    if (Status s = e->init(ctx_, peer_name); !s) return s;
    return std::unique_ptr<TlsEngine>(std::move(e));
}

}  // namespace

Result<std::shared_ptr<TlsContext>> backend_create_context(const TlsSettings& s, TlsContext::Role role) {
    auto ctx = std::shared_ptr<OsslContext>(new OsslContext(s, role));
    if (Status st = ctx->init(); !st) return st;
    return std::shared_ptr<TlsContext>(std::move(ctx));
}

bool backend_available(std::string& reason) {
    const Library& L = library();
    if (!L.ok) reason = L.reason;
    return L.ok;
}

std::string backend_name() {
    const Library& L = library();
    return L.ok ? L.version : std::string("OpenSSL (not loaded)");
}

}  // namespace siaara::net::detail

#endif  // !_WIN32 && !__APPLE__
