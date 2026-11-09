// siaara-repair: validates DICOM files and repairs them into NEW files.
//
//   siaara-repair validate <in...>                         findings only
//   siaara-repair report   <in...> [--fix C,...]           findings + the changes a fix would make (dry run)
//   siaara-repair fix      <in...> --out DIR --fix C,...   writes repaired copies to DIR
//
// Inputs are never modified (their SHA-256 is checked before and after). Categories: structural,
// values, uids, charset, attributes, pixels, rescue, decompress, study, all (= all but decompress).
// Output never shows input paths or values from the data sets unless --detailed is given (the
// report is then marked as containing patient data).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/io.hpp"
#include "repair/repair.hpp"

using namespace siaara;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: siaara-repair validate|report|fix <input file|folder>... [options]\n"
                 "  --out DIR              output folder (fix); outputs keep the input file names\n"
                 "  --fix LIST             categories: structural,values,uids,charset,attributes,pixels,\n"
                 "                         rescue,decompress,study or all (all but decompress)\n"
                 "                         (report: default all; fix: required)\n"
                 "  --json                 JSON output instead of text\n"
                 "  --report FILE          also write the JSON report to FILE\n"
                 "  --detailed             include paths and old/new values (CONTAINS PATIENT DATA)\n"
                 "  --charset TERM         Specific Character Set to set when detection is ambiguous\n"
                 "  --recompute-group-lengths   structural: recompute instead of dropping them\n"
                 "  --keep-last-duplicate  structural: keep the last copy of a duplicated element\n"
                 "  --no-harmonise --no-new-uids --no-renumber --no-fill-uids   limit the study category\n"
                 "  --uid-seed TEXT        seed for generated UIDs (repeatable runs)\n"
                 "  --overwrite            replace existing output files (never an input)\n"
                 "  --min-severity info|warning|error   text output filter (default info)\n");
}

void expand(const std::string& in, std::vector<std::string>& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(std::u8string_view(reinterpret_cast<const char8_t*>(in.data()), in.size()));
    if (fs::is_directory(p, ec)) {
        std::vector<std::string> found;
        for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::u8string u = it->path().u8string();
            found.emplace_back(reinterpret_cast<const char*>(u.data()), u.size());
        }
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    } else {
        out.push_back(in);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string_view cmd = argv[1];
    if (cmd != "validate" && cmd != "report" && cmd != "fix") {
        usage();
        return 2;
    }
    repair::RepairOptions opt;
    std::vector<std::string> inputs;
    std::string out_dir, report_path, fix_list;
    bool json = false, have_fix = false;
    repair::Severity min_sev = repair::Severity::Info;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--out") {
            const char* v = next();
            if (!v) return usage(), 2;
            out_dir = v;
        } else if (a == "--fix") {
            const char* v = next();
            if (!v) return usage(), 2;
            fix_list = v;
            have_fix = true;
        } else if (a == "--json") {
            json = true;
        } else if (a == "--report") {
            const char* v = next();
            if (!v) return usage(), 2;
            report_path = v;
        } else if (a == "--detailed") {
            opt.detailed_report = true;
        } else if (a == "--charset") {
            const char* v = next();
            if (!v) return usage(), 2;
            opt.charset = v;
        } else if (a == "--recompute-group-lengths") {
            opt.recompute_group_lengths = true;
        } else if (a == "--keep-last-duplicate") {
            opt.duplicates = repair::DuplicatePolicy::KeepLast;
        } else if (a == "--no-harmonise") {
            opt.study.harmonise = false;
        } else if (a == "--no-new-uids") {
            opt.study.new_uids = false;
        } else if (a == "--no-renumber") {
            opt.study.renumber = false;
        } else if (a == "--no-fill-uids") {
            opt.study.fill_uids = false;
        } else if (a == "--uid-seed") {
            const char* v = next();
            if (!v) return usage(), 2;
            opt.uid_seed = v;
        } else if (a == "--overwrite") {
            opt.overwrite = true;
        } else if (a == "--min-severity") {
            const char* v = next();
            if (!v) return usage(), 2;
            const std::string_view s = v;
            min_sev = s == "error" ? repair::Severity::Error : s == "warning" ? repair::Severity::Warning : repair::Severity::Info;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else {
            expand(std::string(a), inputs);
        }
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "no input files\n");
        return 2;
    }
    if (cmd == "validate") {
        opt.fix = 0;
    } else {
        if (!have_fix) {
            if (cmd == "fix") {
                std::fprintf(stderr, "fix needs --fix with the categories to repair (repairs happen only when asked)\n");
                return 2;
            }
            fix_list = "all";
        }
        if (!repair::parse_categories(fix_list, opt.fix)) {
            std::fprintf(stderr, "unknown category in --fix\n");
            return 2;
        }
    }
    if (cmd == "fix") {
        if (out_dir.empty()) {
            std::fprintf(stderr, "fix needs --out DIR (the inputs are never changed)\n");
            return 2;
        }
        opt.write = true;
    }
    auto r = repair::repair_files(inputs, cmd == "fix" ? out_dir : std::string(), opt);
    if (!r) {
        std::fprintf(stderr, "siaara-repair: %s\n", r.error().message.c_str());
        return 2;
    }
    if (cmd == "validate") {
        for (auto& f : r->files) f.changes.clear(), f.after_computed = false;
    }
    const std::string json_text = repair::to_json(*r);
    if (!report_path.empty()) {
        if (Status s = io::write_file_atomic(report_path, reinterpret_cast<const std::uint8_t*>(json_text.data()),
                                             json_text.size());
            !s) {
            std::fprintf(stderr, "siaara-repair: the report could not be written\n");
            return 2;
        }
    }
    if (json) {
        std::fwrite(json_text.data(), 1, json_text.size(), stdout);
        std::fputc('\n', stdout);
    } else {
        const std::string text = repair::to_text(*r, min_sev);
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
    int rc = 0;
    for (const auto& f : r->files) {
        if (!f.error.empty()) rc = 1;
        if (!f.input_unchanged) rc = 3;
        if (cmd == "validate" && repair::count(f.findings, repair::Severity::Error)) rc = rc ? rc : 1;
    }
    return rc;
}
