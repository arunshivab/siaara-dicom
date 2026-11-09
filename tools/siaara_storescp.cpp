// siaara-storescp: a simple Storage SCP (PS3.4 Annex B) that writes received instances as Part 10
// files. For testing it can also serve C-FIND / C-GET / C-MOVE from a folder of files.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "net/scp.hpp"
#include "net/tls.hpp"

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

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage() {
    std::fprintf(stderr,
                 "usage: siaara-storescp [options] port\n"
                 "  -aet AE            AE title (default SIAARA_SCP)\n"
                 "  -od DIR            write received instances to DIR (default: do not write)\n"
                 "  --accept-any       accept any SOP class (default: common storage classes)\n"
                 "  --all-ts           also accept compressed transfer syntaxes known to the toolkit\n"
                 "  --allow AE         allowed calling AE title (repeatable; default any)\n"
                 "  --max-assoc N      maximum concurrent associations (default 8)\n"
                 "  --max-pdu N        maximum PDU length to receive (default 65536)\n"
                 "  --exit-after N     stop after N associations were handled\n"
                 "  --port-file PATH   write the listening port to PATH (useful with port 0)\n"
                 "  --qr-dir DIR       serve C-FIND/C-GET/C-MOVE from the DICOM files in DIR\n"
                 "  --move-dest AE=HOST:PORT  C-MOVE destination (repeatable)\n"
                 "  -v                 log association and TLS events to stderr\n"
                 "  --tls-require-client-cert  mutual TLS: refuse clients without a valid certificate\n");
    std::fputs(kTlsUsage, stderr);
    std::fputs("  (the server needs --tls-cert/--tls-key, --tls-p12 or --tls-os-identity)\n", stderr);
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!s[0] || *end || v > max) return false;
    out = v;
    return true;
}

bool uid_is_filename_safe(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

// ---- tiny query/retrieve back end over a folder (test aid; matching per PS3.4 C.2.2.2, simplified)

struct Indexed {
    std::string path;
    DicomFile header;
};

bool wildcard_match(std::string_view pat, std::string_view s) {
    if (pat.empty()) return s.empty();
    if (pat[0] == '*') {
        for (std::size_t i = 0; i <= s.size(); ++i)
            if (wildcard_match(pat.substr(1), s.substr(i))) return true;
        return false;
    }
    if (s.empty()) return false;
    if (pat[0] == '?' || pat[0] == s[0]) return wildcard_match(pat.substr(1), s.substr(1));
    return false;
}

bool element_matches(const Element& key, const Dataset& file) {
    if (key.is_sequence()) return true;  // sequence matching not supported by this test aid
    const std::string_view kv = trim_value(key.value.as_chars(), key.vr);
    if (kv.empty()) return true;  // universal matching
    const Element* fe = file.find(key.tag);
    const std::string_view fv = fe ? trim_value(fe->value.as_chars(), fe->vr) : std::string_view();
    if (key.vr == VR::UI) {
        // list of UID matching
        std::size_t start = 0;
        while (start <= kv.size()) {
            const std::size_t end = kv.find('\\', start);
            const std::string_view one = kv.substr(start, end == std::string_view::npos ? kv.npos : end - start);
            if (one == fv) return true;
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return false;
    }
    if ((key.vr == VR::DA || key.vr == VR::TM) && kv.find('-') != std::string_view::npos) {
        const std::size_t d = kv.find('-');
        const std::string_view lo = kv.substr(0, d), hi = kv.substr(d + 1);
        return (lo.empty() || fv >= lo) && (hi.empty() || fv <= hi);
    }
    return wildcard_match(kv, fv);
}

Tag level_key(std::string_view level) {
    if (level == "PATIENT") return tags::PatientID;
    if (level == "STUDY") return tags::StudyInstanceUID;
    if (level == "SERIES") return tags::SeriesInstanceUID;
    return tags::SOPInstanceUID;
}

class Archive {
public:
    void load(const std::string& dir) {
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            ReadOptions ro;
            ro.stop_before_pixel_data = true;
            Result<DicomFile> f = read_file(e.path().string(), ro);
            if (!f || !f->dataset.contains(tags::SOPInstanceUID)) continue;
            items_.push_back({e.path().string(), std::move(f.value())});
        }
    }
    std::size_t size() const { return items_.size(); }

    std::vector<const Indexed*> match(const Dataset& id) const {
        std::vector<const Indexed*> out;
        for (const Indexed& it : items_) {
            bool ok = true;
            for (const Element& k : id.elements()) {
                if (k.tag == 0x00080052 || k.tag == tags::SpecificCharacterSet) continue;
                if (!element_matches(k, it.header.dataset)) {
                    ok = false;
                    break;
                }
            }
            if (ok) out.push_back(&it);
        }
        return out;
    }

private:
    std::vector<Indexed> items_;
};

}  // namespace

int main(int argc, char** argv) {
    ScpConfig cfg;
    std::string out_dir, port_file, qr_dir;
    bool all_ts = false, verbose = false;
    unsigned long exit_after = 0, port = 0, tmp = 0;
    std::map<std::string, MoveDestination> dests;
    bool have_port = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        const int tls_opt = std::strcmp(a, "--tls-name") ? parse_tls_option(argc, argv, i, cfg.tls) : -1;
        if (tls_opt < 0) {
            usage();
            return 2;
        }
        if (tls_opt > 0) continue;
        if (!std::strcmp(a, "-aet") && more) cfg.ae_title = argv[++i];
        else if (!std::strcmp(a, "-od") && more) out_dir = argv[++i];
        else if (!std::strcmp(a, "--accept-any")) cfg.accept_any_sop_class = true;
        else if (!std::strcmp(a, "--all-ts")) all_ts = true;
        else if (!std::strcmp(a, "--allow") && more) cfg.allowed_calling_aes.push_back(argv[++i]);
        else if (!std::strcmp(a, "--max-assoc") && more && parse_uint(argv[++i], 256, tmp)) cfg.max_associations = tmp;
        else if (!std::strcmp(a, "--max-pdu") && more && parse_uint(argv[++i], 0xFFFFFFFFul, tmp))
            cfg.limits.max_receive_pdu = static_cast<std::uint32_t>(tmp);
        else if (!std::strcmp(a, "--exit-after") && more && parse_uint(argv[++i], 1000000, exit_after)) {}
        else if (!std::strcmp(a, "--port-file") && more) port_file = argv[++i];
        else if (!std::strcmp(a, "--qr-dir") && more) qr_dir = argv[++i];
        else if (!std::strcmp(a, "--move-dest") && more) {
            const std::string spec = argv[++i];
            const std::size_t eq = spec.find('='), colon = spec.rfind(':');
            unsigned long p = 0;
            if (eq == std::string::npos || colon == std::string::npos || colon < eq ||
                !parse_uint(spec.substr(colon + 1).c_str(), 65535, p)) {
                usage();
                return 2;
            }
            dests[spec.substr(0, eq)] = {spec.substr(eq + 1, colon - eq - 1), static_cast<std::uint16_t>(p), spec.substr(0, eq)};
        } else if (!std::strcmp(a, "-v")) verbose = true;
        else if (a[0] != '-' && !have_port && parse_uint(a, 65535, port)) have_port = true;
        else {
            usage();
            return 2;
        }
    }
    if (!have_port) {
        usage();
        return 2;
    }
    cfg.port = static_cast<std::uint16_t>(port);
    if (all_ts) {
        cfg.transfer_syntaxes = native_transfer_syntaxes();
        const char* extra[] = {"1.2.840.10008.1.2.1.99", "1.2.840.10008.1.2.5",    "1.2.840.10008.1.2.4.50",
                               "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.57", "1.2.840.10008.1.2.4.70",
                               "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.90",
                               "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.4.201", "1.2.840.10008.1.2.4.202",
                               "1.2.840.10008.1.2.4.203"};
        for (const char* t : extra) cfg.transfer_syntaxes.emplace_back(t);
    }

    Archive archive;
    if (!qr_dir.empty()) archive.load(qr_dir);

    std::mutex log_mu;
    std::atomic<unsigned long> stored{0};
    ScpHandlers h;
    if (verbose)
        h.log = [&](const std::string& s) {
            std::lock_guard<std::mutex> l(log_mu);
            std::fprintf(stderr, "siaara-storescp: %s\n", s.c_str());
        };
    else
        cfg.tls.log = [&](const std::string& s) {  // TLS warnings are always shown
            if (s.rfind("WARNING", 0) != 0) return;
            std::lock_guard<std::mutex> l(log_mu);
            std::fprintf(stderr, "siaara-storescp: %s\n", s.c_str());
        };
    h.on_store = [&](const StoreRequest& rq) -> std::uint16_t {
        if (!out_dir.empty()) {
            if (!uid_is_filename_safe(rq.sop_instance_uid)) return dimse_status::CannotUnderstand;
            std::vector<std::uint8_t> buf;
            if (!encode_received_part10(rq, buf)) return dimse_status::OutOfResources;
            const std::string path = (std::filesystem::path(out_dir) / (rq.sop_instance_uid + ".dcm")).string();
            if (!io::write_file_atomic(path, buf.data(), buf.size())) return dimse_status::OutOfResources;
        }
        stored.fetch_add(1);
        return dimse_status::Success;
    };
    if (!qr_dir.empty()) {
        h.on_find = [&](const QueryRequest& q, const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
            const std::string level = q.identifier.get_string(0x00080052).value_or("STUDY");
            const Tag key = level_key(level);
            std::vector<std::string> seen;
            for (const Indexed* it : archive.match(q.identifier)) {
                const std::string k = it->header.dataset.get_string(key).value_or("");
                bool dup = false;
                for (const std::string& s : seen) dup = dup || s == k;
                if (dup) continue;
                seen.push_back(k);
                Dataset r;
                for (const Element& e : q.identifier.elements()) {
                    if (const Element* fe = it->header.dataset.find(e.tag); fe && !fe->is_sequence()) r.put(*fe);
                    else {
                        Element empty;
                        empty.tag = e.tag;
                        empty.vr = e.vr;
                        r.put(empty);
                    }
                }
                (void)r.set_string(0x00080052, VR::CS, level);
                if (!emit(r)) break;
            }
            return dimse_status::Success;
        };
        auto retrieve = [&](const QueryRequest& q, std::vector<RetrieveItem>& items) -> std::uint16_t {
            for (const Indexed* it : archive.match(q.identifier)) items.push_back({it->path, nullptr});
            return dimse_status::Success;
        };
        h.on_get = retrieve;
        h.on_move = retrieve;
        h.resolve_move_destination = [&](const std::string& ae) -> std::optional<MoveDestination> {
            auto it = dests.find(ae);
            if (it == dests.end()) return std::nullopt;
            return it->second;
        };
    }

    ScpServer server(cfg, h);
    if (Status s = server.start(); !s) {
        std::fprintf(stderr, "cannot start: %s\n", s.message.c_str());
        return 1;
    }
    if (!port_file.empty()) {
        const std::string p = std::to_string(server.port()) + "\n";
        if (!io::write_file_atomic(port_file, reinterpret_cast<const std::uint8_t*>(p.data()), p.size())) {
            std::fprintf(stderr, "cannot write port file\n");
            return 1;
        }
    }
    if (verbose) std::fprintf(stderr, "siaara-storescp: listening on port %u as %s\n", server.port(), cfg.ae_title.c_str());
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        if (exit_after && server.associations_handled() + server.associations_rejected() >= exit_after &&
            server.active_associations() == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    server.stop();
    if (verbose) std::fprintf(stderr, "siaara-storescp: %lu instance(s) received\n", stored.load());
    return 0;
}
