// SIAARA DICOM Toolkit - TLS backend for Windows: SChannel through SSPI (secur32) with certificate
// handling through CryptoAPI (crypt32). Both are operating system libraries.
//
// SChannel is used as a record engine over the toolkit's own sockets (EngineTransport in tls.cpp):
// InitializeSecurityContextW / AcceptSecurityContext for the handshake, EncryptMessage /
// DecryptMessage for records, ApplyControlToken(SCHANNEL_SHUTDOWN) for close_notify. Credentials
// use SCH_CREDENTIALS (TLS 1.3 capable, Windows 10 1809 / Server 2019 and later; TLS 1.3 needs
// Windows 11 / Server 2022), falling back to SCHANNEL_CRED with TLS 1.2 on older systems.
//
// Certificates are validated by the toolkit (SCH_CRED_MANUAL_CRED_VALIDATION): chain to the OS
// roots, or to one of the PEM anchors of TlsSettings::ca_file (then only those), validity dates,
// key usage, revocation (TlsSettings::revocation, the OS CRL/OCSP sources, the root excluded),
// the PS3.15 algorithms of every chain certificate, and the host name (CERT_CHAIN_POLICY_SSL) or
// the iPAddress subjectAltName. TLS 1.2 renegotiation is refused (fatal); only TLS 1.3
// post-handshake messages are processed.
//
// Identities: PKCS#12 files or a certificate in the "MY" store selected by thumbprint. A PKCS#12
// key is imported as an ephemeral CNG key (PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP) that
// is never written to the user's key store. Only if SChannel cannot use such a key for the
// credential (client credentials, and server credentials on some versions) is it imported
// persistently (with a logged warning) and deleted again when the context is released. PEM private keys are not supported by this backend.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <subauth.h>
#include <security.h>
#include <schannel.h>

#include <cstring>
#include <cwchar>
#include <vector>

#include "net/tls_internal.hpp"

namespace siaara::net::detail {

namespace {

std::string hex32(unsigned long v) {
    static const char* d = "0123456789ABCDEF";
    std::string s = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) s.push_back(d[(v >> shift) & 0xFu]);
    return s;
}

std::string sspi_text(SECURITY_STATUS st) {
    const unsigned long v = static_cast<unsigned long>(st);
    const char* name = nullptr;
    switch (st) {
        case SEC_E_UNTRUSTED_ROOT: name = "untrusted root"; break;
        case SEC_E_CERT_EXPIRED: name = "certificate expired"; break;
        case SEC_E_WRONG_PRINCIPAL: name = "host name mismatch"; break;
        case SEC_E_ALGORITHM_MISMATCH: name = "no common protocol version or cipher suite"; break;
        case SEC_E_NO_CREDENTIALS: name = "no usable credentials"; break;
        case SEC_E_INVALID_TOKEN: name = "invalid TLS message"; break;
        case SEC_E_ILLEGAL_MESSAGE: name = "the peer sent an alert or an illegal message"; break;
        case SEC_E_DECRYPT_FAILURE: name = "decryption failed"; break;
        case SEC_E_MESSAGE_ALTERED: name = "message integrity check failed"; break;
        case SEC_E_UNSUPPORTED_FUNCTION: name = "unsupported (protocol not enabled?)"; break;
        case SEC_E_CERT_UNKNOWN: name = "certificate unknown"; break;
        case SEC_E_INTERNAL_ERROR: name = "internal error (e.g. no certificate for a server that requires one)"; break;
        default: break;
    }
    return std::string("SChannel error ") + hex32(v) + (name ? std::string(" (") + name + ")" : std::string());
}

std::string chain_text(DWORD err) {
    const char* name = nullptr;
    switch (err) {
        case static_cast<DWORD>(CERT_E_EXPIRED): name = "certificate has expired or is not yet valid"; break;
        case static_cast<DWORD>(CERT_E_UNTRUSTEDROOT): name = "untrusted root (self-signed or unknown CA)"; break;
        case static_cast<DWORD>(CERT_E_UNTRUSTEDCA): name = "untrusted CA"; break;
        case static_cast<DWORD>(CERT_E_CHAINING): name = "no chain to a trusted root"; break;
        case static_cast<DWORD>(CERT_E_CN_NO_MATCH): name = "hostname mismatch"; break;
        case static_cast<DWORD>(CERT_E_WRONG_USAGE): name = "certificate not valid for this usage"; break;
        case static_cast<DWORD>(CERT_E_REVOKED): name = "certificate revoked"; break;
        case static_cast<DWORD>(CRYPT_E_REVOKED): name = "certificate revoked"; break;
        case static_cast<DWORD>(CRYPT_E_REVOCATION_OFFLINE): name = "revocation server offline"; break;
        case static_cast<DWORD>(CRYPT_E_NO_REVOCATION_CHECK): name = "revocation status unknown"; break;
        case static_cast<DWORD>(TRUST_E_CERT_SIGNATURE): name = "bad certificate signature"; break;
        default: break;
    }
    return std::string("certificate verification failed: ") + (name ? name : "error " + hex32(err));
}

// UTF-8 to UTF-16. False when `s` is not valid UTF-8 (or too long); `w` is then empty.
bool widen(const std::string& s, std::wstring& w) {
    w.clear();
    if (s.empty()) return true;
    if (s.size() > 0x7FFFFFFFu) return false;
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    w.assign(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n) != n) {
        w.clear();
        return false;
    }
    return true;
}

std::wstring widen(const std::string& s) {
    std::wstring w;
    (void)widen(s, w);
    return w;
}

void wipe(std::wstring& w) noexcept {
    if (!w.empty()) secure_wipe(w.data(), w.size() * sizeof(wchar_t));
    w.clear();
}

bool parse_thumbprint(const std::string& hex, std::vector<BYTE>& out) {
    out.clear();
    int nib = -1;
    for (char c : hex) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ' || c == ':') continue;
        else return false;
        if (nib < 0) nib = v;
        else {
            out.push_back(static_cast<BYTE>((nib << 4) | v));
            nib = -1;
        }
    }
    return nib < 0 && out.size() == 20;
}

// Deletes the key container a PFX import created (CNG through ncrypt.dll loaded from System32, or
// a legacy CryptoAPI container). Best effort.
void delete_imported_key(PCCERT_CONTEXT cert) noexcept {
    DWORD size = 0;
    if (!::CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) || size == 0) return;
    std::vector<BYTE> buf(size);
    if (!::CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, buf.data(), &size)) return;
    const CRYPT_KEY_PROV_INFO* kpi = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(buf.data());
    if (!kpi->pwszContainerName) return;
    if (kpi->dwProvType == 0) {
        HMODULE nc = ::LoadLibraryExW(L"ncrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!nc) return;
        using OpenProvider = LONG(WINAPI*)(ULONG_PTR*, LPCWSTR, DWORD);
        using OpenKey = LONG(WINAPI*)(ULONG_PTR, ULONG_PTR*, LPCWSTR, DWORD, DWORD);
        using DeleteKey = LONG(WINAPI*)(ULONG_PTR, DWORD);
        using FreeObject = LONG(WINAPI*)(ULONG_PTR);
        OpenProvider open_provider = nullptr;
        OpenKey open_key = nullptr;
        DeleteKey delete_key = nullptr;
        FreeObject free_object = nullptr;
        FARPROC p1 = ::GetProcAddress(nc, "NCryptOpenStorageProvider");
        FARPROC p2 = ::GetProcAddress(nc, "NCryptOpenKey");
        FARPROC p3 = ::GetProcAddress(nc, "NCryptDeleteKey");
        FARPROC p4 = ::GetProcAddress(nc, "NCryptFreeObject");
        if (p1 && p2 && p3 && p4) {
            std::memcpy(&open_provider, &p1, sizeof p1);
            std::memcpy(&open_key, &p2, sizeof p2);
            std::memcpy(&delete_key, &p3, sizeof p3);
            std::memcpy(&free_object, &p4, sizeof p4);
            ULONG_PTR prov = 0, key = 0;
            if (open_provider(&prov, kpi->pwszProvName, 0) == 0) {
                const DWORD machine = kpi->dwFlags & 0x20u;  // NCRYPT_MACHINE_KEY_FLAG
                if (open_key(prov, &key, kpi->pwszContainerName, 0, machine) == 0) (void)delete_key(key, 0);
                (void)free_object(prov);
            }
        }
        (void)::FreeLibrary(nc);
    } else {
        HCRYPTPROV h = 0;
        (void)::CryptAcquireContextW(&h, kpi->pwszContainerName, kpi->pwszProvName, kpi->dwProvType,
                                     CRYPT_DELETEKEYSET | (kpi->dwFlags & CRYPT_MACHINE_KEYSET));
    }
}

// Named curve of an EC public key (DER OID in the algorithm parameters): P-256, P-384 or P-521.
bool strong_curve(const CRYPT_OBJID_BLOB& params) {
    static const BYTE p256[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    static const BYTE p384[] = {0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x22};
    static const BYTE p521[] = {0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x23};
    auto is = [&](const BYTE* oid, std::size_t n) {
        return params.cbData == n && params.pbData && std::memcmp(params.pbData, oid, n) == 0;
    };
    return is(p256, sizeof p256) || is(p384, sizeof p384) || is(p521, sizeof p521);
}

bool oid_is(const char* oid, const char* want) { return oid && std::strcmp(oid, want) == 0; }

CertKey chain_key(PCCERT_CONTEXT c, int& bits) {
    CERT_PUBLIC_KEY_INFO& spki = c->pCertInfo->SubjectPublicKeyInfo;
    const char* alg = spki.Algorithm.pszObjId;
    bits = static_cast<int>(::CertGetPublicKeyLength(X509_ASN_ENCODING, &spki));
    if (oid_is(alg, szOID_RSA_RSA)) return CertKey::Rsa;
    if (oid_is(alg, "1.2.840.113549.1.1.10")) return CertKey::RsaPss;
    if (oid_is(alg, szOID_ECC_PUBLIC_KEY)) {
        if (!strong_curve(spki.Algorithm.Parameters)) bits = 0;  // only P-256, P-384, P-521
        return CertKey::Ec;
    }
    if (oid_is(alg, "1.3.101.112")) return CertKey::Ed25519;
    if (oid_is(alg, "1.3.101.113")) return CertKey::Ed448;
    return CertKey::Other;
}

CertSig chain_sig(PCCERT_CONTEXT c) {
    const char* a = c->pCertInfo->SignatureAlgorithm.pszObjId;
    static const char* const strong[] = {"1.2.840.113549.1.1.11", "1.2.840.113549.1.1.12", "1.2.840.113549.1.1.13",
                                         "1.2.840.113549.1.1.10", "1.2.840.10045.4.3.2",  "1.2.840.10045.4.3.3",
                                         "1.2.840.10045.4.3.4"};
    static const char* const weak[] = {"1.2.840.113549.1.1.4", "1.2.840.113549.1.1.5", "1.2.840.113549.1.1.14",
                                       "1.2.840.10045.4.1",    "1.2.840.10045.4.3.1",  "1.2.840.10040.4.3",
                                       "1.3.14.3.2.29"};
    for (const char* o : strong)
        if (oid_is(a, o)) return CertSig::Sha256Plus;
    for (const char* o : weak)
        if (oid_is(a, o)) return CertSig::Weak;
    if (oid_is(a, "1.3.101.112") || oid_is(a, "1.3.101.113")) return CertSig::EdDsa;
    return CertSig::Other;
}

// Windows build number (RtlGetVersion: not subject to the manifest-based version lie).
DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    FARPROC p = nt ? ::GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (!p) return 0;
    RtlGetVersionFn fn = nullptr;
    std::memcpy(&fn, &p, sizeof p);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return fn(&v) == 0 ? v.dwBuildNumber : 0;
}

bool ip_matches_san(PCCERT_CONTEXT cert, const std::string& ip) {
    BYTE want[16] = {0};
    std::size_t want_len = 0;
    const std::wstring wip = widen(ip);
    if (::InetPtonW(AF_INET, wip.c_str(), want) == 1) want_len = 4;
    else if (::InetPtonW(AF_INET6, wip.c_str(), want) == 1) want_len = 16;
    else return false;
    PCERT_EXTENSION ext = ::CertFindExtension(szOID_SUBJECT_ALT_NAME2, cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
    if (!ext) return false;
    CERT_ALT_NAME_INFO* info = nullptr;
    DWORD size = 0;
    if (!::CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, ext->Value.pbData, ext->Value.cbData,
                               CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &size) || !info)
        return false;
    bool found = false;
    for (DWORD i = 0; i < info->cAltEntry && !found; ++i) {
        const CERT_ALT_NAME_ENTRY& e = info->rgAltEntry[i];
        if (e.dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS && e.IPAddress.cbData == want_len &&
            std::memcmp(e.IPAddress.pbData, want, want_len) == 0)
            found = true;
    }
    ::LocalFree(info);
    return found;
}

// ---------------------------------------------------------------- context
class SchContext final : public EngineContext {
public:
    SchContext(const TlsSettings& s, Role r) : EngineContext(s, r) {}
    ~SchContext() override {
        if (have_cred_) ::FreeCredentialsHandle(&cred_);
        release_identity();
        if (roots_) ::CertCloseStore(roots_, 0);
    }
    Status init();

    CredHandle* cred() noexcept { return &cred_; }
    // Validates a peer certificate (server_auth: the client checks the server).
    Status verify(PCCERT_CONTEXT peer, bool server_auth, const std::string& name) const;

protected:
    Result<std::unique_ptr<TlsEngine>> make_engine(const std::string& peer_name) override;

private:
    CredHandle cred_{};
    bool have_cred_ = false;
    PCCERT_CONTEXT cert_ = nullptr;
    HCERTSTORE id_store_ = nullptr;
    bool imported_key_ = false;
    HCERTSTORE roots_ = nullptr;  // anchors from ca_file (instead of the OS roots)

    Status load_identity(bool persist_key);
    Status load_trust();
    Status acquire();
    void release_identity() noexcept;
};

void SchContext::release_identity() noexcept {
    if (cert_) {
        ::CertFreeCertificateContext(cert_);
        cert_ = nullptr;
    }
    if (imported_key_ && id_store_) {
        // Every key a persistent PKCS#12 import wrote to the key store, not only the leaf's.
        PCCERT_CONTEXT c = nullptr;
        while ((c = ::CertEnumCertificatesInStore(id_store_, c)) != nullptr) delete_imported_key(c);
    }
    imported_key_ = false;
    if (id_store_) {
        ::CertCloseStore(id_store_, 0);
        id_store_ = nullptr;
    }
}

Status SchContext::load_identity(bool persist_key) {
    const TlsIdentity& id = settings_.identity;
    if (!id.certificate_file.empty())
        return Status(Errc::Unsupported, "PEM private keys are not supported by the SChannel backend; use a PKCS#12 "
                                         "file or a certificate store thumbprint");
    if (!id.keychain_identity.empty()) return Status(Errc::Unsupported, "keychain identities exist on macOS only");
    if (!id.pkcs12_file.empty()) {
        Result<std::vector<std::uint8_t>> der = read_secret_file(id.pkcs12_file);
        if (!der) return der.take_error();
        std::vector<std::uint8_t> bytes = std::move(der).value();
        CRYPT_DATA_BLOB blob{};
        blob.cbData = static_cast<DWORD>(bytes.size());
        blob.pbData = bytes.data();
        std::wstring pw;
        if (!widen(id.password, pw)) {
            secure_wipe(bytes);
            return Status(Errc::InvalidArgument, "the PKCS#12 password is not valid UTF-8");
        }
        // Ephemeral CNG key by default: nothing is written to the user's key store.
        const DWORD import = persist_key ? static_cast<DWORD>(CRYPT_USER_KEYSET)
                                         : static_cast<DWORD>(PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP);
        id_store_ = ::PFXImportCertStore(&blob, pw.c_str(), import);
        if (!id_store_ && pw.empty()) id_store_ = ::PFXImportCertStore(&blob, nullptr, import);
        const DWORD err = ::GetLastError();
        wipe(pw);
        secure_wipe(bytes);
        if (!id_store_)
            return Status(Errc::InvalidArgument, "cannot import the PKCS#12 file " + id.pkcs12_file +
                                                     " (wrong password?), error " + hex32(err));
        imported_key_ = persist_key;  // from here on release_identity() deletes what was imported
        // The leaf: a certificate with a private key.
        PCCERT_CONTEXT c = nullptr;
        while ((c = ::CertEnumCertificatesInStore(id_store_, c)) != nullptr) {
            DWORD size = 0;
            // A persisted key has provider info; an ephemeral one only a key handle property.
            if (::CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) ||
                ::CertGetCertificateContextProperty(c, CERT_NCRYPT_KEY_HANDLE_PROP_ID, nullptr, &size) ||
                ::CertGetCertificateContextProperty(c, CERT_KEY_CONTEXT_PROP_ID, nullptr, &size)) {
                cert_ = ::CertDuplicateCertificateContext(c);
                ::CertFreeCertificateContext(c);
                break;
            }
        }
        if (!cert_) return Status(Errc::InvalidArgument, "the PKCS#12 file holds no certificate with a private key");
        return Status::ok();
    }
    if (!id.windows_thumbprint.empty()) {
        std::vector<BYTE> hash;
        if (!parse_thumbprint(id.windows_thumbprint, hash))
            return Status(Errc::InvalidArgument, "a certificate thumbprint is 40 hexadecimal digits");
        const DWORD where = id.windows_local_machine ? CERT_SYSTEM_STORE_LOCAL_MACHINE : CERT_SYSTEM_STORE_CURRENT_USER;
        id_store_ = ::CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                    where | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG, L"MY");
        if (!id_store_) return Status(Errc::IoError, "cannot open the MY certificate store");
        CRYPT_HASH_BLOB hb{};
        hb.cbData = static_cast<DWORD>(hash.size());
        hb.pbData = hash.data();
        cert_ = ::CertFindCertificateInStore(id_store_, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH,
                                             &hb, nullptr);
        if (!cert_) return Status(Errc::NotFound, "no certificate with that thumbprint in the MY store");
        return Status::ok();
    }
    return Status::ok();
}

Status SchContext::load_trust() {
    if (settings_.ca_file.empty()) return Status::ok();  // the OS roots (default chain engine)
    Result<std::vector<std::vector<std::uint8_t>>> certs = read_pem_certificates(settings_.ca_file);
    if (!certs) return certs.take_error();
    roots_ = ::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!roots_) return Status(Errc::OutOfMemory, "cannot create a certificate store");
    for (const std::vector<std::uint8_t>& der : *certs)
        if (!::CertAddEncodedCertificateToStore(roots_, X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()),
                                                CERT_STORE_ADD_ALWAYS, nullptr))
            return Status(Errc::InvalidArgument, "invalid certificate in " + settings_.ca_file);
    return Status::ok();
}

Status SchContext::acquire() {
    const bool server = role_ == Role::Server;
    const bool only13 = settings_.min_version == TlsMinVersion::Tls13;
    PCCERT_CONTEXT creds[1] = {cert_};
    const DWORD ncreds = cert_ ? 1u : 0u;
    DWORD flags = SCH_USE_STRONG_CRYPTO;
    if (!server) flags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp expiry{};

    DWORD enabled = server ? (SP_PROT_TLS1_3_SERVER | (only13 ? 0u : static_cast<DWORD>(SP_PROT_TLS1_2_SERVER)))
                           : (SP_PROT_TLS1_3_CLIENT | (only13 ? 0u : static_cast<DWORD>(SP_PROT_TLS1_2_CLIENT)));
    // The PS3.15 cipher policy at negotiation time: no CBC mode, no static-RSA or finite-field DH
    // key exchange, no 3DES/RC4 (finish() also checks the negotiated suite against the allow-list).
    wchar_t aes[] = L"AES", cbc[] = L"ChainingModeCBC", rsa[] = L"RSA", dh[] = L"DH", des3[] = L"3DES", rc4[] = L"RC4";
    auto ustr = [](wchar_t* w) {
        UNICODE_STRING u{};
        u.Length = static_cast<USHORT>(std::wcslen(w) * sizeof(wchar_t));
        u.MaximumLength = static_cast<USHORT>(u.Length + sizeof(wchar_t));
        u.Buffer = w;
        return u;
    };
    UNICODE_STRING cbc_mode = ustr(cbc);
    CRYPTO_SETTINGS disabled[5]{};
    disabled[0].eAlgorithmUsage = TlsParametersCngAlgUsageCipher;
    disabled[0].strCngAlgId = ustr(aes);
    disabled[0].cChainingModes = 1;
    disabled[0].rgstrChainingModes = &cbc_mode;
    disabled[1].eAlgorithmUsage = TlsParametersCngAlgUsageKeyExchange;
    disabled[1].strCngAlgId = ustr(rsa);
    disabled[2].eAlgorithmUsage = TlsParametersCngAlgUsageKeyExchange;
    disabled[2].strCngAlgId = ustr(dh);
    disabled[3].eAlgorithmUsage = TlsParametersCngAlgUsageCipher;
    disabled[3].strCngAlgId = ustr(des3);
    disabled[4].eAlgorithmUsage = TlsParametersCngAlgUsageCipher;
    disabled[4].strCngAlgId = ustr(rc4);
    TLS_PARAMETERS tp{};
    tp.grbitDisabledProtocols = ~enabled;
    tp.cDisabledCrypto = 5;
    tp.pDisabledCrypto = disabled;
    SCH_CREDENTIALS sc{};
    sc.dwVersion = SCH_CREDENTIALS_VERSION;
    sc.cCreds = ncreds;
    sc.paCred = ncreds ? creds : nullptr;
    sc.dwFlags = flags;
    sc.cTlsParameters = 1;
    sc.pTlsParameters = &tp;
    SECURITY_STATUS st = ::AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
                                                     server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &sc,
                                                     nullptr, nullptr, &cred_, &expiry);
    // Systems before Windows 10 1809 (build 17763) do not know SCH_CREDENTIALS and refuse it as
    // unknown credentials or an invalid parameter. Only then fall back to SCHANNEL_CRED (TLS 1.2
    // only), keeping the cipher policy through the algorithm list; any other failure, or any
    // failure on a newer system, is reported as it is.
    const DWORD build = windows_build();
    if ((st == SEC_E_UNKNOWN_CREDENTIALS || st == SEC_E_INVALID_PARAMETER) && !only13 && build != 0 && build < 17763) {
        tls_log(settings_, "WARNING: TLS: this Windows version (build " + std::to_string(build) +
                               ") does not support SCH_CREDENTIALS; using SCHANNEL_CRED with TLS 1.2 only");
        // ECDHE key exchange, AES (the negotiated suite is still checked against the AEAD
        // allow-list after the handshake), ECDSA or RSA signatures, SHA-2.
        ALG_ID algs[] = {CALG_ECDH_EPHEM, CALG_AES_128, CALG_AES_256, CALG_SHA_256, CALG_SHA_384, CALG_ECDSA, CALG_RSA_SIGN};
        SCHANNEL_CRED old{};
        old.dwVersion = SCHANNEL_CRED_VERSION;
        old.cCreds = ncreds;
        old.paCred = ncreds ? creds : nullptr;
        old.grbitEnabledProtocols = server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;
        old.cSupportedAlgs = static_cast<DWORD>(sizeof algs / sizeof algs[0]);
        old.palgSupportedAlgs = algs;
        old.dwMinimumCipherStrength = 128;
        old.dwFlags = flags;
        st = ::AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
                                         server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &old, nullptr,
                                         nullptr, &cred_, &expiry);
    }
    if (st != SEC_E_OK) return Status(Errc::InvalidArgument, "AcquireCredentialsHandle failed: " + sspi_text(st));
    have_cred_ = true;
    return Status::ok();
}

Status SchContext::init() {
    if (Status s = load_identity(false); !s) return s;
    if (Status s = load_trust(); !s) return s;
    Status a = acquire();
    if (!a && !settings_.identity.pkcs12_file.empty()) {
        // SChannel cannot use an ephemeral key for a client credential (SEC_E_NO_CREDENTIALS), and
        // some versions not for a server credential either: import the key persistently instead
        // (deleted again with the context) and say so.
        release_identity();
        if (Status s = load_identity(true); !s) return s;
        tls_log(settings_, "WARNING: TLS: SChannel refused the ephemeral PKCS#12 key (" + a.message +
                               "); the key was imported into the user's key store until the context is released");
        a = acquire();
    }
    return a;
}

Status SchContext::verify(PCCERT_CONTEXT peer, bool server_auth, const std::string& name) const {
    // Chain building sees the certificates the peer sent and, with ca_file, the anchors from it.
    HCERTSTORE extra = ::CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr);
    if (!extra) return Status(Errc::OutOfMemory, "cannot create a certificate store");
    if (peer->hCertStore) (void)::CertAddStoreToCollection(extra, peer->hCertStore, 0, 0);
    if (roots_) (void)::CertAddStoreToCollection(extra, roots_, 0, 0);
    LPSTR usage = const_cast<LPSTR>(server_auth ? szOID_PKIX_KP_SERVER_AUTH : szOID_PKIX_KP_CLIENT_AUTH);
    CERT_CHAIN_PARA cp{};
    cp.cbSize = sizeof cp;
    cp.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    cp.RequestedUsage.Usage.cUsageIdentifier = 1;
    cp.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    const TlsRevocation rev = settings_.revocation;
    const DWORD chain_flags = rev == TlsRevocation::Off ? 0u : static_cast<DWORD>(CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT);
    const BOOL built = ::CertGetCertificateChain(nullptr, peer, nullptr, extra, &cp, chain_flags, nullptr, &chain);
    ::CertCloseStore(extra, 0);
    if (!built || !chain)
        return Status(Errc::NetworkError, "cannot build the certificate chain, error " + hex32(::GetLastError()));
    if (rev != TlsRevocation::Off) {
        const DWORD e = chain->TrustStatus.dwErrorStatus;
        Status bad;
        if (e & CERT_TRUST_IS_REVOKED)
            bad = Status(Errc::NetworkError, "certificate verification failed: certificate revoked");
        else if (rev == TlsRevocation::HardFail && (e & (CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION)))
            bad = Status(Errc::NetworkError, "certificate verification failed: revocation status unknown (hard-fail)");
        if (!bad) {
            ::CertFreeCertificateChain(chain);
            return bad;
        }
    }
    // PS3.15 algorithms of every certificate of the chain (leaf first, anchor last).
    if (chain->cChain > 0) {
        const CERT_SIMPLE_CHAIN* sc = chain->rgpChain[0];
        for (DWORD i = 0; i < sc->cElement; ++i) {
            PCCERT_CONTEXT c = sc->rgpElement[i]->pCertContext;
            int bits = 0;
            const CertKey key = chain_key(c, bits);
            if (Status bad = check_chain_cert(settings_, i, i + 1 == sc->cElement, key, bits, chain_sig(c)); !bad) {
                ::CertFreeCertificateChain(chain);
                return bad;
            }
        }
    }
    DWORD checks = 0;
    if (roots_) {
        // Trusted only when the chain ends in one of the configured anchors; the OS roots do not
        // count. The policy check then ignores "unknown CA" for that anchor (it is not an OS root).
        bool anchored = false;
        if (chain->cChain > 0) {
            const CERT_SIMPLE_CHAIN* sc = chain->rgpChain[0];
            if (sc->cElement > 0) {
                PCCERT_CONTEXT top = sc->rgpElement[sc->cElement - 1]->pCertContext;
                PCCERT_CONTEXT found =
                    ::CertFindCertificateInStore(roots_, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, top, nullptr);
                if (found) {
                    anchored = true;
                    ::CertFreeCertificateContext(found);
                }
            }
        }
        if (!anchored) {
            ::CertFreeCertificateChain(chain);
            return Status(Errc::NetworkError, chain_text(static_cast<DWORD>(CERT_E_UNTRUSTEDROOT)));
        }
        checks |= 0x00000100u;  // SECURITY_FLAG_IGNORE_UNKNOWN_CA
    }
    const bool ip = server_auth && is_ip_literal(name);
    std::wstring wname = server_auth && !ip ? widen(name) : std::wstring();
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ex{};
    ex.cbSize = sizeof ex;
    ex.dwAuthType = server_auth ? AUTHTYPE_SERVER : AUTHTYPE_CLIENT;
    ex.fdwChecks = checks;
    ex.pwszServerName = wname.empty() ? nullptr : wname.data();
    CERT_CHAIN_POLICY_PARA pp{};
    pp.cbSize = sizeof pp;
    pp.pvExtraPolicyPara = &ex;
    // Soft-fail: an unknown or offline revocation status is not an error (revoked still is, above).
    if (rev != TlsRevocation::HardFail) pp.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
    CERT_CHAIN_POLICY_STATUS ps{};
    ps.cbSize = sizeof ps;
    const BOOL ran = ::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps);
    ::CertFreeCertificateChain(chain);
    if (!ran) return Status(Errc::NetworkError, "certificate policy check failed, error " + hex32(::GetLastError()));
    if (ps.dwError != 0) return Status(Errc::NetworkError, chain_text(ps.dwError));
    if (server_auth && name.empty()) return Status(Errc::NetworkError, "no server name to verify");
    if (ip && !ip_matches_san(peer, name))
        return Status(Errc::NetworkError, "certificate verification failed: IP address mismatch");
    return Status::ok();
}

// ---------------------------------------------------------------- engine
class SchEngine final : public TlsEngine {
public:
    SchEngine(SchContext& c, const TlsSettings& s, bool server, std::string name)
        : c_(c), s_(s), server_(server), name_(std::move(name)) {
        if (!server_ && !name_.empty() && !is_ip_literal(name_)) target_ = widen(name_);
    }
    ~SchEngine() override {
        if (have_ctx_) ::DeleteSecurityContext(&ctx_);
    }

    Status feed(const std::uint8_t* p, std::size_t n) override {
        in_.insert(in_.end(), p, p + n);
        return Status::ok();
    }

    Result<Step> handshake(std::vector<std::uint8_t>& out) override {
        bool retried_credentials = false;
        while (true) {
            if ((server_ || have_ctx_) && in_.empty()) return Step::NeedInput;
            const std::size_t mark = out.size();
            SECURITY_STATUS st = step(in_, out);
            if (st == SEC_I_INCOMPLETE_CREDENTIALS && !server_ && !retried_credentials) {
                retried_credentials = true;  // asked for a certificate we do not have: go on without
                continue;
            }
            if (st == SEC_E_INCOMPLETE_MESSAGE) return Step::NeedInput;
            if (st == SEC_E_OK && server_ && s_.require_client_certificate) {
                // SChannel completes the handshake even when the client sent no (or an untrusted)
                // certificate. Check it before the final flight (TLS 1.2: the server Finished) leaves,
                // so the client's handshake fails too instead of seeing a completed one.
                if (Status c = check_client_certificate(); !c) {
                    out.resize(mark);  // drop the final flight
                    fatal_alert(out);
                    return c;
                }
            }
            if (st == SEC_E_OK) {
                if (::QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK)
                    return Status(Errc::NetworkError, "cannot query the TLS record sizes");
                return Step::Done;
            }
            if (st == SEC_I_CONTINUE_NEEDED) {
                if (in_.empty()) return Step::NeedInput;
                continue;
            }
            return Status(Errc::NetworkError, sspi_text(st));
        }
    }

    Status finish(TlsSessionInfo& info) override {
        SecPkgContext_ConnectionInfo ci{};
        if (::QueryContextAttributesW(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK) {
            tls13_ = (ci.dwProtocol & SP_PROT_TLS1_3) != 0;
            if (ci.dwProtocol & SP_PROT_TLS1_3) info.protocol = "TLSv1.3";
            else if (ci.dwProtocol & SP_PROT_TLS1_2) info.protocol = "TLSv1.2";
            else info.protocol = "protocol " + hex32(ci.dwProtocol);
        }
        SecPkgContext_CipherInfo cinfo{};
        cinfo.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
        if (::QueryContextAttributesW(&ctx_, SECPKG_ATTR_CIPHER_INFO, &cinfo) == SEC_E_OK)
            info.cipher_suite = static_cast<std::uint16_t>(cinfo.dwCipherSuite & 0xFFFFu);
        if (const CipherSuite* cs = find_allowed_cipher_suite(info.cipher_suite)) info.cipher_name = cs->name;
        PCCERT_CONTEXT peer = nullptr;
        if (::QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer) != SEC_E_OK) peer = nullptr;
        info.peer_certificate = peer != nullptr;
        if (!peer) {
            if (!server_) return Status(Errc::NetworkError, "the server sent no certificate");
            if (s_.require_client_certificate) return Status(Errc::NetworkError, "the client sent no certificate");
            return Status::ok();
        }
        // verify() checks the chain, its revocation status and every certificate's algorithms.
        Status st;
        if (!s_.insecure_skip_validation) {
            st = c_.verify(peer, !server_, name_);
            info.peer_verified = st.is_ok();
        }
        ::CertFreeCertificateContext(peer);
        return st;
    }

    Status decrypt(std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& out, bool& closed) override {
        if (renegotiating_) {
            if (Status r = post_handshake(out); !r) return r;
            if (renegotiating_) return Status::ok();
        }
        while (!in_.empty()) {
            SecBuffer b[4];
            b[0].BufferType = SECBUFFER_DATA;
            b[0].cbBuffer = static_cast<unsigned long>(in_.size());
            b[0].pvBuffer = in_.data();
            for (int i = 1; i < 4; ++i) {
                b[i].BufferType = SECBUFFER_EMPTY;
                b[i].cbBuffer = 0;
                b[i].pvBuffer = nullptr;
            }
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS st = ::DecryptMessage(&ctx_, &d, 0, nullptr);
            if (st == SEC_E_INCOMPLETE_MESSAGE) return Status::ok();
            if (st == SEC_I_CONTEXT_EXPIRED) {
                closed = true;
                in_.clear();
                return Status::ok();
            }
            if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE) return Status(Errc::NetworkError, sspi_text(st));
            std::vector<std::uint8_t> rest;
            for (int i = 1; i < 4; ++i) {
                if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer) {
                    const std::uint8_t* p = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                    plain.insert(plain.end(), p, p + b[i].cbBuffer);
                } else if (b[i].BufferType == SECBUFFER_EXTRA && b[i].cbBuffer) {
                    const std::uint8_t* p = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                    rest.assign(p, p + b[i].cbBuffer);
                }
            }
            in_.swap(rest);
            if (st == SEC_I_RENEGOTIATE) {
                // TLS 1.2: the peer asks for a renegotiation. The policy is no renegotiation, and a
                // renegotiated handshake could bring a different, unchecked peer certificate: fatal.
                if (!tls13_) {
                    in_.clear();
                    return Status(Errc::NetworkError, "the peer requested a TLS 1.2 renegotiation, which is refused");
                }
                // TLS 1.3 post-handshake message (NewSessionTicket, KeyUpdate): the handshake call
                // consumes the remaining records.
                renegotiating_ = true;
                if (Status r = post_handshake(out); !r) return r;
                if (renegotiating_) return Status::ok();
            }
        }
        return Status::ok();
    }

    Status encrypt(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) override {
        if (n == 0) return Status::ok();
        if (n > max_plaintext()) return Status(Errc::InvalidArgument, "record too large");
        const std::size_t old = out.size();
        out.resize(old + sizes_.cbHeader + n + sizes_.cbTrailer);
        std::uint8_t* base = out.data() + old;
        std::memcpy(base + sizes_.cbHeader, p, n);
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_STREAM_HEADER;
        b[0].cbBuffer = sizes_.cbHeader;
        b[0].pvBuffer = base;
        b[1].BufferType = SECBUFFER_DATA;
        b[1].cbBuffer = static_cast<unsigned long>(n);
        b[1].pvBuffer = base + sizes_.cbHeader;
        b[2].BufferType = SECBUFFER_STREAM_TRAILER;
        b[2].cbBuffer = sizes_.cbTrailer;
        b[2].pvBuffer = base + sizes_.cbHeader + n;
        b[3].BufferType = SECBUFFER_EMPTY;
        b[3].cbBuffer = 0;
        b[3].pvBuffer = nullptr;
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        const SECURITY_STATUS st = ::EncryptMessage(&ctx_, 0, &d, 0);
        if (st != SEC_E_OK) {
            out.resize(old);
            return Status(Errc::NetworkError, sspi_text(st));
        }
        out.resize(old + b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
        return Status::ok();
    }

    std::size_t max_plaintext() const noexcept override {
        return sizes_.cbMaximumMessage ? static_cast<std::size_t>(sizes_.cbMaximumMessage) : std::size_t{16384};
    }

    void close_notify(std::vector<std::uint8_t>& out) noexcept override {
        if (!have_ctx_) return;
        try {
            DWORD type = SCHANNEL_SHUTDOWN;
            SecBuffer b;
            b.BufferType = SECBUFFER_TOKEN;
            b.cbBuffer = sizeof type;
            b.pvBuffer = &type;
            SecBufferDesc d{SECBUFFER_VERSION, 1, &b};
            if (::ApplyControlToken(&ctx_, &d) != SEC_E_OK) return;
            std::vector<std::uint8_t> none;
            (void)step(none, out);
        } catch (...) {
        }
    }

private:
    SchContext& c_;
    const TlsSettings& s_;
    bool server_;
    std::string name_;
    std::wstring target_;
    CtxtHandle ctx_{};
    bool have_ctx_ = false;
    bool renegotiating_ = false;
    bool tls13_ = false;  // set by finish(); false (TLS 1.2 rules) until then
    SecPkgContext_StreamSizes sizes_{};
    std::vector<std::uint8_t> in_;

    Status check_client_certificate() {
        PCCERT_CONTEXT peer = nullptr;
        const SECURITY_STATUS q = ::QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer);
        if (q != SEC_E_OK || !peer) {
            tls_log(s_, "TLS: client sent no certificate (" + sspi_text(q) + "); mutual TLS required, handshake refused");
            return Status(Errc::NetworkError, "the client sent no certificate (mutual TLS required)");
        }
        Status st;
        if (!s_.insecure_skip_validation) st = c_.verify(peer, false, std::string());
        ::CertFreeCertificateContext(peer);
        if (!st) tls_log(s_, "TLS: client certificate refused before the server Finished: " + st.message);
        return st;
    }

    // Best effort: a fatal bad_certificate alert in place of the dropped final flight.
    void fatal_alert(std::vector<std::uint8_t>& out) noexcept {
        try {
            SCHANNEL_ALERT_TOKEN tok{};
            tok.dwTokenType = SCHANNEL_ALERT;
            tok.dwAlertType = TLS1_ALERT_FATAL;
            tok.dwAlertNumber = TLS1_ALERT_BAD_CERTIFICATE;
            SecBuffer b;
            b.BufferType = SECBUFFER_TOKEN;
            b.cbBuffer = sizeof tok;
            b.pvBuffer = &tok;
            SecBufferDesc d{SECBUFFER_VERSION, 1, &b};
            if (::ApplyControlToken(&ctx_, &d) != SEC_E_OK) return;
            std::vector<std::uint8_t> none;
            (void)step(none, out);
        } catch (...) {
        }
    }

    // One InitializeSecurityContextW / AcceptSecurityContext call with `input` (consumed, the
    // unused tail kept); output tokens and alerts are appended to `out`.
    SECURITY_STATUS step(std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& out) {
        SecBuffer ib[2];
        ib[0].BufferType = SECBUFFER_TOKEN;
        ib[0].cbBuffer = static_cast<unsigned long>(input.size());
        ib[0].pvBuffer = input.empty() ? nullptr : input.data();
        ib[1].BufferType = SECBUFFER_EMPTY;
        ib[1].cbBuffer = 0;
        ib[1].pvBuffer = nullptr;
        SecBufferDesc id{SECBUFFER_VERSION, 2, ib};
        SecBuffer ob[2];
        ob[0].BufferType = SECBUFFER_TOKEN;
        ob[0].cbBuffer = 0;
        ob[0].pvBuffer = nullptr;
        ob[1].BufferType = SECBUFFER_ALERT;
        ob[1].cbBuffer = 0;
        ob[1].pvBuffer = nullptr;
        SecBufferDesc od{SECBUFFER_VERSION, 2, ob};
        ULONG attrs = 0;
        TimeStamp expiry{};
        SECURITY_STATUS st;
        if (server_) {
            const ULONG flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                                ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_EXTENDED_ERROR |
                                (s_.require_client_certificate ? ASC_REQ_MUTUAL_AUTH : 0u);
            st = ::AcceptSecurityContext(c_.cred(), have_ctx_ ? &ctx_ : nullptr, input.empty() ? nullptr : &id, flags, 0,
                                         &ctx_, &od, &attrs, &expiry);
        } else {
            const ULONG flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR |
                                ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS;
            st = ::InitializeSecurityContextW(c_.cred(), have_ctx_ ? &ctx_ : nullptr,
                                              target_.empty() ? nullptr : target_.data(), flags, 0, 0,
                                              have_ctx_ && !input.empty() ? &id : nullptr, 0, &ctx_, &od, &attrs,
                                              &expiry);
        }
        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS ||
            st == SEC_I_CONTEXT_EXPIRED || (FAILED(st) && ob[1].cbBuffer))
            have_ctx_ = true;
        for (SecBuffer& o : ob) {
            if (o.pvBuffer && o.cbBuffer) {
                const std::uint8_t* p = static_cast<const std::uint8_t*>(o.pvBuffer);
                out.insert(out.end(), p, p + o.cbBuffer);
            }
            if (o.pvBuffer) ::FreeContextBuffer(o.pvBuffer);
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE || st == SEC_I_INCOMPLETE_CREDENTIALS) return st;  // input kept
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer > 0 && ib[1].cbBuffer <= input.size())
            input.erase(input.begin(), input.end() - static_cast<std::ptrdiff_t>(ib[1].cbBuffer));
        else
            input.clear();
        return st;
    }

    Status post_handshake(std::vector<std::uint8_t>& out) {
        const SECURITY_STATUS st = step(in_, out);
        if (st == SEC_E_INCOMPLETE_MESSAGE) return Status::ok();  // wait for more input
        renegotiating_ = false;
        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) return Status::ok();
        return Status(Errc::NetworkError, "post-handshake message: " + sspi_text(st));
    }
};

Result<std::unique_ptr<TlsEngine>> SchContext::make_engine(const std::string& peer_name) {
    return std::unique_ptr<TlsEngine>(std::make_unique<SchEngine>(*this, settings_, role_ == Role::Server, peer_name));
}

}  // namespace

Result<std::shared_ptr<TlsContext>> backend_create_context(const TlsSettings& s, TlsContext::Role role) {
    auto ctx = std::shared_ptr<SchContext>(new SchContext(s, role));
    if (Status st = ctx->init(); !st) return st;
    return std::shared_ptr<TlsContext>(std::move(ctx));
}

bool backend_available(std::string&) { return true; }

std::string backend_name() { return "SChannel"; }

}  // namespace siaara::net::detail

#endif  // _WIN32
