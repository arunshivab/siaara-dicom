// siaara-movescu: C-MOVE SCU (PS3.4 C.4.2): asks a Q/R SCP to send matching instances to a
// destination AE.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "dict/dictionary.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"

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
    std::fprintf(stderr,
                 "usage: siaara-movescu [options] host port\n"
                 "  -aet AE          calling AE title (default SIAARA_MOVE)\n"
                 "  -aec AE          called AE title (default ANY-SCP)\n"
                 "  -aem AE          move destination AE title (required)\n"
                 "  -P | -S | -R     patient root, study root (default), composite instance root\n"
                 "  -L LEVEL         Query/Retrieve Level (default STUDY)\n"
                 "  -k KEY=VALUE     identifier key (gggg,eeee or keyword; repeatable)\n"
                 "  --relational     propose relational retrieval (extended negotiation)\n"
                 "  -t MS            timeout in milliseconds (default 60000)\n"
                 "  -q               quiet\n");
}

int main(int argc, char** argv) {
    ScuConfig cfg;
    cfg.calling_ae = "SIAARA_MOVE";
    unsigned long timeout = 60000;
    char model = 'S';
    std::string level = "STUDY", dest;
    std::vector<std::string> keys;
    bool relational = false, quiet = false;
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        if (!std::strcmp(a, "-aet") && more) cfg.calling_ae = argv[++i];
        else if (!std::strcmp(a, "-aec") && more) cfg.called_ae = argv[++i];
        else if (!std::strcmp(a, "-aem") && more) dest = argv[++i];
        else if (!std::strcmp(a, "-P") || !std::strcmp(a, "-S") || !std::strcmp(a, "-R")) model = a[1];
        else if (!std::strcmp(a, "-L") && more) level = argv[++i];
        else if (!std::strcmp(a, "-k") && more) keys.emplace_back(argv[++i]);
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
    if (npos != 2 || !parse_uint(pos[1], 65535, port) || dest.empty()) {
        usage();
        return 2;
    }
    cfg.host = pos[0];
    cfg.port = static_cast<std::uint16_t>(port);
    const int t = static_cast<int>(timeout);
    cfg.timeouts.connect_ms = t;
    cfg.timeouts.acse_ms = t;
    cfg.timeouts.dimse_ms = t;
    const std::string_view sop_class = model == 'P' ? sop::PatientRootMove
                                     : model == 'R' ? sop::CompositeInstanceRootMove
                                                    : sop::StudyRootMove;
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, level);
    for (const std::string& k : keys)
        if (!add_key(q, k)) return 2;
    if (relational) cfg.extended_negotiation.push_back(retrieve_extended_negotiation(sop_class, true));
    Scu scu(cfg);
    scu.add_context(std::string(sop_class), native_transfer_syntaxes());
    if (Status s = scu.connect(); !s) {
        std::fprintf(stderr, "association failed: %s\n", s.message.c_str());
        return 1;
    }
    Result<RetrieveResult> r = scu.move(sop_class, dest, q, [&](const RetrieveProgress& p) {
        if (!quiet) print_progress("pending", p);
        return true;
    });
    if (!r) {
        std::fprintf(stderr, "C-MOVE failed: %s\n", r.error().message.c_str());
        return 1;
    }
    print_progress("final", *r);
    (void)scu.release();
    return r->status == 0 ? 0 : 1;
}
