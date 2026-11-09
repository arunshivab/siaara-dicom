// siaara-storescu: C-STORE SCU (PS3.4 Annex B). Sends Part 10 files; with --transcode the files
// are converted on the fly to the transfer syntax the SCP accepts (PS3.5 10, codec/transcode.hpp).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: siaara-storescu [options] host port file...\n"
                 "  -aet AE          calling AE title (default SIAARA_STORE)\n"
                 "  -aec AE          called AE title (default ANY-SCP)\n"
                 "  --transcode TS   propose only transfer syntax TS and convert every file to it\n"
                 "  --propose TS     also propose TS for every SOP class (repeatable)\n"
                 "  --no-transcode   never convert pixel data (send only in an accepted encoding)\n"
                 "  --allow-lossy    permit lossy conversion (JPEG baseline/extended, lossy JPEG 2000 / JPEG-LS)\n"
                 "  --lossy          code lossily in syntaxes that allow both (.81, .91)\n"
                 "  --quality N      lossy quality 1..100 (default 90)\n"
                 "  --async N        propose an asynchronous operations window of N outstanding requests\n"
                 "  --max-pdu N      maximum PDU length to receive (default 65536)\n"
                 "  -t MS            timeout in milliseconds (default 30000)\n"
                 "  -q               quiet\n");
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!s[0] || *end || v > max) return false;
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) try {
    ScuConfig cfg;
    cfg.calling_ae = "SIAARA_STORE";
    unsigned long timeout = 30000, quality = 90, async = 0, max_pdu = 65536;
    std::string transcode_to;
    std::vector<std::string> extra;
    std::vector<std::string> files;
    bool quiet = false;
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        if (!std::strcmp(a, "-aet") && more) cfg.calling_ae = argv[++i];
        else if (!std::strcmp(a, "-aec") && more) cfg.called_ae = argv[++i];
        else if (!std::strcmp(a, "--transcode") && more) transcode_to = argv[++i];
        else if (!std::strcmp(a, "--propose") && more) extra.emplace_back(argv[++i]);
        else if (!std::strcmp(a, "--no-transcode")) cfg.transcode = false;
        else if (!std::strcmp(a, "--allow-lossy")) cfg.transcode_params.allow_lossy = true;
        else if (!std::strcmp(a, "--lossy")) cfg.transcode_params.lossy = true;
        else if (!std::strcmp(a, "--quality") && more && parse_uint(argv[++i], 100, quality) && quality > 0) {}
        else if (!std::strcmp(a, "--async") && more && parse_uint(argv[++i], 65535, async)) {}
        else if (!std::strcmp(a, "--max-pdu") && more && parse_uint(argv[++i], 0xFFFFFFFFul, max_pdu)) {}
        else if (!std::strcmp(a, "-t") && more && parse_uint(argv[++i], 3600000, timeout)) {}
        else if (!std::strcmp(a, "-q")) quiet = true;
        else if (a[0] != '-' && npos < 2) pos[npos++] = a;
        else if (a[0] != '-') files.emplace_back(a);
        else {
            usage();
            return 2;
        }
    }
    unsigned long port = 0;
    if (npos != 2 || files.empty() || !parse_uint(pos[1], 65535, port)) {
        usage();
        return 2;
    }
    for (const std::string& ts : extra)
        if (!find_transfer_syntax(ts)) {
            std::fprintf(stderr, "unknown transfer syntax %s\n", ts.c_str());
            return 2;
        }
    if (!transcode_to.empty() && !codec::can_encode(transcode_to)) {
        std::fprintf(stderr, "cannot encode transfer syntax %s\n", transcode_to.c_str());
        return 2;
    }
    cfg.host = pos[0];
    cfg.port = static_cast<std::uint16_t>(port);
    const int t = static_cast<int>(timeout);
    cfg.timeouts.connect_ms = t;
    cfg.timeouts.acse_ms = t;
    cfg.timeouts.dimse_ms = t;
    cfg.limits.max_receive_pdu = static_cast<std::uint32_t>(max_pdu);
    cfg.transcode_params.quality = static_cast<int>(quality);
    if (async) cfg.async_window = AsyncWindow{static_cast<std::uint16_t>(async), 1};

    // One presentation context per SOP class with the transfer syntaxes to propose.
    std::map<std::string, std::vector<std::string>> proposals;
    ReadOptions ro;
    ro.stop_before_pixel_data = true;
    for (const std::string& f : files) {
        Result<DicomFile> h = read_file(f, ro);
        if (!h) {
            std::fprintf(stderr, "%s: %s\n", f.c_str(), h.error().message.c_str());
            return 1;
        }
        std::string cls = h->dataset.get_string(tags::SOPClassUID).value_or("");
        if (cls.empty()) cls = h->meta.get_string(tags::MediaStorageSOPClassUID).value_or("");
        if (cls.empty()) {
            std::fprintf(stderr, "%s: no SOP Class UID\n", f.c_str());
            return 1;
        }
        std::vector<std::string>& ts = proposals[cls];
        auto add = [&ts](const std::string& x) {
            for (const std::string& y : ts)
                if (y == x) return;
            ts.push_back(x);
        };
        if (!transcode_to.empty()) {
            add(transcode_to);
        } else {
            add(h->transfer_syntax);
            for (const std::string& n : native_transfer_syntaxes()) add(n);
        }
        for (const std::string& x : extra) add(x);
    }
    if (proposals.size() > 128) {
        std::fprintf(stderr, "more than 128 SOP classes\n");
        return 1;
    }
    Scu scu(cfg);
    for (auto& [cls, ts] : proposals) scu.add_context(cls, ts);
    AssociateRj rj;
    if (Status s = scu.connect(&rj); !s) {
        if (s.code == Errc::Rejected)
            std::fprintf(stderr, "association rejected: result %u source %u reason %u\n", rj.result, rj.source, rj.reason);
        else
            std::fprintf(stderr, "association failed: %s\n", s.message.c_str());
        return 1;
    }
    int failures = 0;
    if (async) {
        Result<StoreManyResult> r = store_many(scu, files);
        if (!r) {
            std::fprintf(stderr, "C-STORE failed: %s\n", r.error().message.c_str());
            return 1;
        }
        for (const StoreOutcome& o : r->outcomes) {
            if (!o.error.is_ok()) {
                std::fprintf(stderr, "%s: %s\n", o.path.c_str(), o.error.message.c_str());
                ++failures;
                continue;
            }
            if (classify_status(o.status) == StatusClass::Failure) ++failures;
            if (!quiet) std::printf("%s: status 0x%04X\n", o.path.c_str(), o.status);
        }
        if (!quiet) std::printf("# window %u, at most %zu outstanding\n", async_invoke_window(*scu.association()),
                                r->max_outstanding);
    } else {
        for (const std::string& f : files) {
            Result<std::uint16_t> r = scu.store_file(f);
            if (!r) {
                std::fprintf(stderr, "%s: %s\n", f.c_str(), r.error().message.c_str());
                ++failures;
                if (!scu.connected()) return 1;
                continue;
            }
            if (classify_status(*r) == StatusClass::Failure) ++failures;
            if (!quiet) std::printf("%s: status 0x%04X\n", f.c_str(), *r);
        }
    }
    if (Status s = scu.release(); !s) {
        std::fprintf(stderr, "release failed: %s\n", s.message.c_str());
        return 1;
    }
    return failures == 0 ? 0 : 1;
} catch (const std::exception& e) {
    // An exception (std::bad_alloc, a filesystem error) is reported, not left to std::terminate.
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "error: unexpected exception\n");
    return 1;
}
