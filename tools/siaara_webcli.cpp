// siaara-webcli - DICOMweb from the command line (QIDO-RS, WADO-RS, WADO-URI, STOW-RS).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "web/client.hpp"
#include "web/dicom_json.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

int usage() {
    std::fputs(
        "usage: siaara-webcli [options] <command> <base-url> ...\n"
        "  qido <url> studies|series|instances [--study UID] [--series UID] [KEY=VALUE ...]\n"
        "       [--include FIELD]... [--limit N] [--offset N] [--fuzzy]   prints a JSON array\n"
        "  metadata <url> STUDY [SERIES [INSTANCE]]                         prints a JSON array\n"
        "  wado <url> STUDY [SERIES [INSTANCE]] --out DIR [--ts UID]...    writes DIR/<SOPInstanceUID>.dcm\n"
        "  frames <url> STUDY SERIES INSTANCE LIST --out PREFIX [--ts UID]...  writes PREFIX.<n>\n"
        "  wado-uri <url> STUDY SERIES OBJECT --out FILE [--ts UID]\n"
        "  stow <url> [--study UID] FILE...\n"
        "options: --token TOKEN (bearer) | --basic USER:PASSWORD, --timeout MS\n",
        stderr);
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
    std::vector<std::string> pos;
    std::string token, basic, out, study_opt, series_opt;
    std::vector<std::string> tss, includes;
    QidoParams q;
    int timeout = 30000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
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
        auto r = c.store(files, study_opt);
        if (!r) return fail(r.error());
        std::string js;
        (void)dataset_to_json(r->response, js);
        print(js);
        std::fprintf(stderr, "HTTP %d: %zu stored, %zu failed\n", r->http_status, r->stored.size(), r->failed.size());
        return r->failed.empty() ? 0 : 1;
    }
    return usage();
}
