// siaara-webcli - DICOMweb from the command line (QIDO-RS, WADO-RS, WADO-URI, STOW-RS).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "net/tls.hpp"
#include "web/client.hpp"
#include "web/dicom_json.hpp"
#include "tool_help.hpp"

using namespace siaara;
using namespace siaara::web;

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

int usage() {
    std::fputs(
        "usage: siaara-webcli [options] <command> <base-url> ...\n"
        "  qido <url> studies|series|instances [--study UID] [--series UID] [KEY=VALUE ...]\n"
        "       [--include FIELD]... [--limit N] [--offset N] [--fuzzy]   prints a JSON array\n"
        "  metadata <url> STUDY [SERIES [INSTANCE]]                         prints a JSON array\n"
        "  wado <url> STUDY [SERIES [INSTANCE]] --out DIR [--ts UID]...    writes DIR/<SOPInstanceUID>.dcm\n"
        "  frames <url> STUDY SERIES INSTANCE LIST --out PREFIX [--ts UID]...  writes PREFIX.<n>\n"
        "  wado-uri <url> STUDY SERIES OBJECT --out FILE [--ts UID]\n"
        "  stow <url> [--study UID] [--allow-incomplete] FILE...\n"
        "      a file that cannot be read completely (truncated, damaged) stops the call before anything\n"
        "      is sent, unless --allow-incomplete\n"
        "options: --token TOKEN (bearer) | --basic USER:PASSWORD, --timeout MS\n"
        "https:// URLs use TLS; --tls-name NAME sets the name the server certificate must carry.\n",
        siaara_tools::usage_stream());
    std::fputs(kTlsUsage, siaara_tools::usage_stream());
    return 2;
}

int fail(const Status& s) {
    std::fprintf(stderr, "error: %s\n", s.message.c_str());
    return 1;
}

void print(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

bool safe_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

Status write_out(const std::string& path, const std::string& data) {
    return io::write_file_atomic(path, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (siaara_tools::wants_help(argc, argv)) {
        usage();
        return 0;
    }
    std::vector<std::string> pos;
    std::string token, basic, out, study_opt, series_opt;
    std::vector<std::string> tss, includes;
    QidoParams q;
    int timeout = 30000;
    net::TlsSettings tls;
    bool tls_verbose = false;
    StowOptions stow_opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--tls" || a == "--tls-require-client-cert") return usage();  // the URL scheme decides
        if (a == "--tls-verbose") {
            tls_verbose = true;
            continue;
        }
        const int tls_opt = parse_tls_option(argc, argv, i, tls);
        if (tls_opt < 0) return usage();
        if (tls_opt > 0) continue;
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--token") { if (!next(token)) return usage(); }
        else if (a == "--basic") { if (!next(basic)) return usage(); }
        else if (a == "--out") { if (!next(out)) return usage(); }
        else if (a == "--study") { if (!next(study_opt)) return usage(); }
        else if (a == "--series") { if (!next(series_opt)) return usage(); }
        else if (a == "--ts") { if (!next(v)) return usage(); tss.push_back(v); }
        else if (a == "--include") { if (!next(v)) return usage(); includes.push_back(v); }
        else if (a == "--limit") { if (!next(v)) return usage(); q.limit = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10)); }
        else if (a == "--offset") { if (!next(v)) return usage(); q.offset = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10)); }
        else if (a == "--timeout") { if (!next(v)) return usage(); timeout = std::atoi(v.c_str()); }
        else if (a == "--fuzzy") q.fuzzy_matching = true;
        else if (a == "--allow-incomplete") stow_opt.allow_incomplete = true;
        else if (a.size() > 2 && a.compare(0, 2, "--") == 0) return usage();
        else pos.push_back(a);
    }
    if (pos.size() < 2) return usage();
    const std::string cmd = pos[0];
    DicomwebClientConfig cfg;
    cfg.base_url = pos[1];
    cfg.limits.read_timeout_ms = timeout;
    cfg.limits.write_timeout_ms = timeout;
    cfg.connect_timeout_ms = timeout;
    cfg.tls = tls;
    cfg.tls.handshake_timeout_ms = timeout > 0 ? timeout : 30000;
    cfg.tls.log = [tls_verbose](const std::string& s) {
        if (tls_verbose || s.rfind("WARNING", 0) == 0) std::fprintf(stderr, "siaara-webcli: %s\n", s.c_str());
    };
    net::tls_wipe_secret(tls.identity.password);
    if (!token.empty()) cfg.authorization = [token] { return bearer_auth(token); };
    else if (!basic.empty()) {
        const auto colon = basic.find(':');
        if (colon == std::string::npos) return usage();
        const std::string user = basic.substr(0, colon), pw = basic.substr(colon + 1);
        cfg.authorization = [user, pw] { return basic_auth(user, pw); };
    }
    DicomwebClient c(cfg);
    if (Status st = c.status(); !st) return fail(st);
    auto arg = [&](std::size_t i) { return i < pos.size() ? pos[i] : std::string(); };

    if (cmd == "qido") {
        const std::string level = arg(2);
        for (std::size_t i = 3; i < pos.size(); ++i) {
            const auto eq = pos[i].find('=');
            if (eq == std::string::npos) return usage();
            q.match.emplace_back(pos[i].substr(0, eq), pos[i].substr(eq + 1));
        }
        for (const auto& f : includes) {
            if (f == "all") q.include_all = true;
            else q.include_fields.push_back(f);
        }
        Result<std::vector<Dataset>> r = Status(Errc::InvalidArgument, "level must be studies, series or instances");
        if (level == "studies") r = c.search_studies(q);
        else if (level == "series") r = c.search_series(q, study_opt);
        else if (level == "instances") r = c.search_instances(q, study_opt, series_opt);
        if (!r) return fail(r.error());
        std::string js;
        if (Status st = datasets_to_json(*r, js); !st) return fail(st);
        print(js);
        return 0;
    }
    if (cmd == "metadata") {
        auto r = c.retrieve_metadata(arg(2), arg(3), arg(4));
        if (!r) return fail(r.error());
        std::string js;
        if (Status st = datasets_to_json(*r, js); !st) return fail(st);
        print(js);
        return 0;
    }
    if (cmd == "wado") {
        if (out.empty() || arg(2).empty()) return usage();
        auto r = c.retrieve_raw(arg(2), arg(3), arg(4), tss);
        if (!r) return fail(r.error());
        std::size_t n = 0;
        for (const auto& raw : *r) {
            auto f = read_part10(Bytes::from_string(raw));
            std::string name = f ? f->dataset.get_string(tags::SOPInstanceUID).value_or("") : std::string();
            if (!safe_name(name)) name = "instance" + std::to_string(n);
            if (Status st = write_out(out + "/" + name + ".dcm", raw); !st) return fail(st);
            ++n;
        }
        std::fprintf(stderr, "%zu instance(s) written to %s\n", n, out.c_str());
        return 0;
    }
    if (cmd == "frames") {
        if (out.empty() || pos.size() < 6) return usage();
        std::vector<std::uint32_t> list;
        std::string s = pos[5];
        std::size_t start = 0;
        while (start <= s.size()) {
            auto e = s.find(',', start);
            if (e == std::string::npos) e = s.size();
            list.push_back(static_cast<std::uint32_t>(std::strtoul(s.substr(start, e - start).c_str(), nullptr, 10)));
            start = e + 1;
        }
        auto r = c.retrieve_frames(arg(2), arg(3), arg(4), list, tss);
        if (!r) return fail(r.error());
        for (std::size_t i = 0; i < r->size(); ++i) {
            if (Status st = write_out(out + "." + std::to_string(i < list.size() ? list[i] : i + 1), r->at(i).data); !st)
                return fail(st);
            std::fprintf(stderr, "frame %u: %zu bytes (%s)\n", i < list.size() ? list[i] : static_cast<unsigned>(i + 1),
                         r->at(i).data.size(), r->at(i).content_type.c_str());
        }
        return 0;
    }
    if (cmd == "wado-uri") {
        if (out.empty() || pos.size() < 5) return usage();
        auto r = c.wado_uri(arg(2), arg(3), arg(4), "application/dicom", tss.empty() ? std::string() : tss[0]);
        if (!r) return fail(r.error());
        if (Status st = write_out(out, *r); !st) return fail(st);
        return 0;
    }
    if (cmd == "stow") {
        if (pos.size() < 3) return usage();
        std::vector<std::string> files;
        for (std::size_t i = 2; i < pos.size(); ++i) {
            auto b = io::read_file(pos[i]);
            if (!b) return fail(b.error());
            files.emplace_back(b->as_chars());
        }
        auto r = c.store(files, study_opt, stow_opt);
        if (!r) return fail(r.error());
        std::string js;
        (void)dataset_to_json(r->response, js);
        print(js);
        std::fprintf(stderr, "HTTP %d: %zu stored, %zu failed\n", r->http_status, r->stored.size(), r->failed.size());
        return r->failed.empty() ? 0 : 1;
    }
    return usage();
}
