// siaara-getscu: C-GET SCU (PS3.4 C.4.3): retrieves matching instances on the same association
// and writes them as Part 10 files. Large instances can be received straight to disk.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/io.hpp"
#include "dict/dictionary.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "tool_help.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!s[0] || *end || v > max) return false;
    out = v;
    return true;
}

bool parse_tag(std::string_view s, Tag& t, VR& vr) {
    if (s.size() == 9 && s[4] == ',') {
        const std::string gs(s.substr(0, 4)), es(s.substr(5, 4));
        char* end = nullptr;
        const unsigned long g = std::strtoul(gs.c_str(), &end, 16);
        if (*end) return false;
        const unsigned long e = std::strtoul(es.c_str(), &end, 16);
        if (*end) return false;
        t = make_tag(static_cast<std::uint16_t>(g), static_cast<std::uint16_t>(e));
        const dict::Entry* d = dict::lookup(t);
        vr = d ? d->vr : VR::LO;
        return true;
    }
    const dict::Entry* d = dict::lookup_keyword(s);
    if (!d) return false;
    t = d->tag;
    vr = d->vr;
    return true;
}

bool add_key(Dataset& q, const std::string& k) {
    const std::size_t eq = k.find('=');
    Tag tag = 0;
    VR vr = VR::None;
    if (!parse_tag(std::string_view(k).substr(0, eq), tag, vr) || !vr_is_string(vr)) {
        std::fprintf(stderr, "unknown or unsupported key: %s\n", k.c_str());
        return false;
    }
    return q.set_string(tag, vr, eq == std::string::npos ? std::string() : k.substr(eq + 1)).is_ok();
}

void print_progress(const char* what, const RetrieveProgress& p) {
    std::printf("%s status 0x%04X remaining %u completed %u failed %u warning %u\n", what, p.status, p.remaining,
                p.completed, p.failed, p.warning);
}

}  // namespace

static void usage() {
    std::fprintf(siaara_tools::usage_stream(),
                 "usage: siaara-getscu [options] host port\n"
                 "  -aet AE          calling AE title (default SIAARA_GET)\n"
                 "  -aec AE          called AE title (default ANY-SCP)\n"
                 "  -P | -S | -R | -N   patient root, study root (default), composite instance root,\n"
                 "                   composite instance retrieve without bulk data\n"
                 "  -L LEVEL         Query/Retrieve Level (default STUDY)\n"
                 "  -k KEY=VALUE     identifier key (gggg,eeee or keyword; repeatable)\n"
                 "  -od DIR          write received instances to DIR (default: current directory)\n"
                 "  --spool BYTES    receive instances larger than BYTES to disk (spooled in DIR)\n"
                 "  --compressed     also accept the compressed transfer syntaxes for storage\n"
                 "  --relational     propose relational retrieval (extended negotiation)\n"
                 "  -t MS            timeout in milliseconds (default 60000)\n"
                 "  -q               quiet\n");
}

static bool safe_uid(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

int main(int argc, char** argv) try {
    if (siaara_tools::wants_help(argc, argv)) {
        usage();
        return 0;
    }
    ScuConfig cfg;
    cfg.calling_ae = "SIAARA_GET";
    unsigned long timeout = 60000, spool = 0;
    char model = 'S';
    std::string level = "STUDY", out_dir = ".";
    std::vector<std::string> keys;
    bool relational = false, quiet = false, compressed = false;
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        if (!std::strcmp(a, "-aet") && more) cfg.calling_ae = argv[++i];
        else if (!std::strcmp(a, "-aec") && more) cfg.called_ae = argv[++i];
        else if (!std::strcmp(a, "-P") || !std::strcmp(a, "-S") || !std::strcmp(a, "-R") || !std::strcmp(a, "-N")) model = a[1];
        else if (!std::strcmp(a, "-L") && more) level = argv[++i];
        else if (!std::strcmp(a, "-k") && more) keys.emplace_back(argv[++i]);
        else if (!std::strcmp(a, "-od") && more) out_dir = argv[++i];
        else if (!std::strcmp(a, "--spool") && more && parse_uint(argv[++i], 0xFFFFFFFFul, spool)) {}
        else if (!std::strcmp(a, "--compressed")) compressed = true;
        else if (!std::strcmp(a, "--relational")) relational = true;
        else if (!std::strcmp(a, "-t") && more && parse_uint(argv[++i], 3600000, timeout)) {}
        else if (!std::strcmp(a, "-q")) quiet = true;
        else if (a[0] != '-' && npos < 2) pos[npos++] = a;
        else {
            usage();
            return 2;
        }
    }
    unsigned long port = 0;
    if (npos != 2 || !parse_uint(pos[1], 65535, port)) {
        usage();
        return 2;
    }
    cfg.host = pos[0];
    cfg.port = static_cast<std::uint16_t>(port);
    const int t = static_cast<int>(timeout);
    cfg.timeouts.connect_ms = t;
    cfg.timeouts.acse_ms = t;
    cfg.timeouts.dimse_ms = t;
    if (spool) {
        cfg.spool = SpoolConfig{};
        cfg.spool->threshold = spool;
        cfg.spool->directory = out_dir;
    }
    const std::string_view sop_class = model == 'P' ? sop::PatientRootGet
                                     : model == 'R' ? sop::CompositeInstanceRootGet
                                     : model == 'N' ? sop::CompositeInstanceRetrieveWithoutBulkDataGet
                                                    : sop::StudyRootGet;
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, level);
    for (const std::string& k : keys)
        if (!add_key(q, k)) return 2;
    if (relational) cfg.extended_negotiation.push_back(retrieve_extended_negotiation(sop_class, true));
    Scu scu(cfg);
    scu.add_context(std::string(sop_class), native_transfer_syntaxes());
    std::vector<std::string> ts = native_transfer_syntaxes();
    if (compressed)
        for (const char* c : {"1.2.840.10008.1.2.4.70", "1.2.840.10008.1.2.4.57", "1.2.840.10008.1.2.4.80",
                              "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.91",
                              "1.2.840.10008.1.2.4.201", "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51",
                              "1.2.840.10008.1.2.5"})
            ts.emplace_back(c);
    for (const std::string& s : storage_sop_classes())
        if (scu.proposals().size() < 127) scu.add_get_storage(s, ts);
    if (Status s = scu.connect(); !s) {
        std::fprintf(stderr, "association failed: %s\n", s.message.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(out_dir.data()), out_dir.size())), ec);
    unsigned long written = 0;
    Result<RetrieveResult> r = scu.get(
        sop_class, q,
        [&](const StoreRequest& rq) -> std::uint16_t {
            if (!safe_uid(rq.sop_instance_uid)) return dimse_status::CannotUnderstand;
            const std::string path = out_dir + "/" + rq.sop_instance_uid + ".dcm";
            if (rq.spool) {
                if (!rq.spool->commit(path)) return dimse_status::OutOfResources;
            } else {
                std::vector<std::uint8_t> p10;
                if (!encode_received_part10(rq, p10)) return dimse_status::CannotUnderstand;
                if (!io::write_file_atomic(path, p10.data(), p10.size())) return dimse_status::OutOfResources;
            }
            ++written;
            if (!quiet) std::printf("received %s (%s)\n", rq.sop_instance_uid.c_str(), rq.transfer_syntax.c_str());
            return dimse_status::Success;
        },
        [&](const RetrieveProgress& p) {
            if (!quiet) print_progress("pending", p);
            return true;
        });
    if (!r) {
        std::fprintf(stderr, "C-GET failed: %s\n", r.error().message.c_str());
        return 1;
    }
    print_progress("final", *r);
    std::printf("wrote %lu file(s)\n", written);
    (void)scu.release();
    return r->status == 0 ? 0 : 1;
} catch (const std::exception& e) {
    // An exception (std::bad_alloc, a filesystem error) is reported, not left to std::terminate.
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "error: unexpected exception\n");
    return 1;
}
