// siaara-findscu: C-FIND SCU (PS3.4 C.4.1 Query/Retrieve, K.6 Modality Worklist, CC UPS query).
// Prints each matching response.
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
#include "tool_help.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

void usage() {
    std::fprintf(siaara_tools::usage_stream(),
                 "usage: siaara-findscu [options] host port\n"
                 "  -aet AE          calling AE title (default SIAARA_FIND)\n"
                 "  -aec AE          called AE title (default ANY-SCP)\n"
                 "  -P | -S | -W | -U   patient root, study root (default), modality worklist, UPS (pull)\n"
                 "  -L LEVEL         Query/Retrieve Level (default STUDY; not for -W/-U)\n"
                 "  -k KEY[=VALUE]   matching / return key; KEY is gggg,eeee or a keyword (repeatable)\n"
                 "  --station AE --modality CS --date D   worklist keys (-W)\n"
                 "  --relational     propose relational queries (extended negotiation)\n"
                 "  --fuzzy          propose fuzzy semantic matching of person names\n"
                 "  -t MS            timeout in milliseconds (default 15000)\n"
                 "  --cancel-after N send C-CANCEL after N responses\n"
                 "  -q               print only the number of matches\n");
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!s[0] || *end || v > max) return false;
    out = v;
    return true;
}

bool parse_tag(std::string_view s, Tag& t, VR& vr) {
    if (s.size() == 9 && s[4] == ',') {
        unsigned long g = 0, e = 0;
        const std::string gs(s.substr(0, 4)), es(s.substr(5, 4));
        char* end = nullptr;
        g = std::strtoul(gs.c_str(), &end, 16);
        if (*end) return false;
        e = std::strtoul(es.c_str(), &end, 16);
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

void print(const Dataset& ds, int indent) {
    for (const Element& e : ds.elements()) {
        std::printf("%*s(%04X,%04X) %.2s ", indent, "", tag_group(e.tag), tag_element(e.tag), vr_name(e.vr).data());
        if (e.is_sequence()) {
            std::printf("%zu item(s)\n", e.items.size());
            for (const Dataset& item : e.items) {
                std::printf("%*s  item\n", indent, "");
                print(item, indent + 4);
            }
        } else if (vr_is_string(e.vr)) {
            const std::string_view v = trim_value(e.value.as_chars(), e.vr);
            std::printf("[%.*s]\n", static_cast<int>(v.size()), v.data());
        } else {
            std::printf("<%zu bytes>\n", e.value.size());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (siaara_tools::wants_help(argc, argv)) {
        usage();
        return 0;
    }
    ScuConfig cfg;
    cfg.calling_ae = "SIAARA_FIND";
    unsigned long timeout = 15000, cancel_after = 0;
    char model = 'S';
    std::string level = "STUDY", station, modality, date;
    std::vector<std::string> keys;
    bool relational = false, fuzzy = false, quiet = false;
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        if (!std::strcmp(a, "-aet") && more) cfg.calling_ae = argv[++i];
        else if (!std::strcmp(a, "-aec") && more) cfg.called_ae = argv[++i];
        else if (!std::strcmp(a, "-P") || !std::strcmp(a, "-S") || !std::strcmp(a, "-W") || !std::strcmp(a, "-U")) model = a[1];
        else if (!std::strcmp(a, "-L") && more) level = argv[++i];
        else if (!std::strcmp(a, "-k") && more) keys.emplace_back(argv[++i]);
        else if (!std::strcmp(a, "--station") && more) station = argv[++i];
        else if (!std::strcmp(a, "--modality") && more) modality = argv[++i];
        else if (!std::strcmp(a, "--date") && more) date = argv[++i];
        else if (!std::strcmp(a, "--relational")) relational = true;
        else if (!std::strcmp(a, "--fuzzy")) fuzzy = true;
        else if (!std::strcmp(a, "-t") && more && parse_uint(argv[++i], 3600000, timeout)) {}
        else if (!std::strcmp(a, "--cancel-after") && more && parse_uint(argv[++i], 1000000, cancel_after)) {}
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

    std::string_view sop_class = sop::StudyRootFind;
    if (model == 'P') sop_class = sop::PatientRootFind;
    if (model == 'W') sop_class = sop::ModalityWorklistFind;
    if (model == 'U') sop_class = sop::UpsPull;
    Dataset q;
    if (model == 'W') q = make_worklist_query(station, modality, date);
    else if (model != 'U') (void)q.set_string(0x00080052, VR::CS, level);
    for (const std::string& k : keys) {
        const std::size_t eq = k.find('=');
        Tag tag = 0;
        VR vr = VR::None;
        if (!parse_tag(std::string_view(k).substr(0, eq), tag, vr) || vr == VR::SQ) {
            std::fprintf(stderr, "unknown or unsupported key: %s\n", k.c_str());
            return 2;
        }
        const std::string value = eq == std::string::npos ? std::string() : k.substr(eq + 1);
        if (vr_is_string(vr)) {
            if (Status s = q.set_string(tag, vr, value); !s) return 2;
        } else if (vr == VR::US && !value.empty()) {
            (void)q.set_uint16(tag, static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10)));
        } else {
            (void)q.set_bytes(tag, vr, Bytes{});
        }
    }
    if (relational || fuzzy) {
        QueryFlags f;
        f.relational = relational;
        f.fuzzy_person_names = fuzzy;
        cfg.extended_negotiation.push_back(find_extended_negotiation(sop_class, f));
    }
    Scu scu(cfg);
    scu.add_context(std::string(sop_class), native_transfer_syntaxes());
    AssociateRj rj;
    if (Status s = scu.connect(&rj); !s) {
        if (s.code == Errc::Rejected)
            std::fprintf(stderr, "association rejected: result %u source %u reason %u\n", rj.result, rj.source, rj.reason);
        else
            std::fprintf(stderr, "association failed: %s\n", s.message.c_str());
        return 1;
    }
    if (relational || fuzzy) {
        const QueryFlags got = negotiated_query_flags(*scu.association(), sop_class, true);
        if (!quiet)
            std::printf("# extended negotiation: relational=%d fuzzy=%d\n", got.relational ? 1 : 0,
                        got.fuzzy_person_names ? 1 : 0);
    }
    unsigned long n = 0;
    Result<FindResult> r = scu.find(sop_class, q, [&](const Dataset& d) {
        ++n;
        if (!quiet) {
            std::printf("# response %lu\n", n);
            print(d, 0);
        }
        return cancel_after == 0 || n < cancel_after;
    });
    if (!r) {
        std::fprintf(stderr, "C-FIND failed: %s\n", r.error().message.c_str());
        return 1;
    }
    std::printf("# status 0x%04X, %zu match(es)%s\n", r->status, r->matches, r->cancel_sent ? ", cancelled" : "");
    (void)scu.release();
    const StatusClass sc = classify_status(r->status);
    return sc == StatusClass::Success || sc == StatusClass::Cancel ? 0 : 1;
}
