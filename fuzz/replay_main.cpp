// SIAARA DICOM Toolkit - corpus replay driver for the fuzz targets (no libFuzzer needed).
//
// Single target (built with one fuzz/targets/*.cpp):
//     fuzz_<name>_replay [--max-ms N] <file-or-folder>...
// Combined (SIAARA_FUZZ_COMBINED, every target in one executable):
//     siaara_fuzz_replay [--max-ms N] --target <name> <file-or-folder>...
//     siaara_fuzz_replay [--max-ms N] --all <root>...   (replays <root>/<target>/ for every target)
//     siaara_fuzz_replay --list
// Folders are walked recursively in sorted order. Files whose name starts with '.' and README*
// files are skipped. An input slower than --max-ms fails the run (hang regression check).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fuzz.hpp"

#if !defined(SIAARA_FUZZ_COMBINED)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
#endif

namespace fs = std::filesystem;

namespace {

bool skipped(const fs::path& p) {
    const std::string n = p.filename().string();
    return n.empty() || n[0] == '.' || n.rfind("README", 0) == 0;
}

void collect(const fs::path& p, std::vector<fs::path>& out) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && !skipped(it->path())) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        out.insert(out.end(), files.begin(), files.end());
    } else if (fs::is_regular_file(p, ec)) {
        out.push_back(p);
    } else {
        std::fprintf(stderr, "warning: %s not found\n", p.string().c_str());
    }
}

bool read_all(const fs::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Runs every file; returns the number of failures (slow inputs).
int replay(siaara_fuzz::TargetFn fn, const char* name, const std::vector<fs::path>& files, long max_ms) {
    int bad = 0;
    std::vector<std::uint8_t> buf;
    double worst = 0;
    for (const fs::path& p : files) {
        if (!read_all(p, buf)) {
            std::fprintf(stderr, "%s: cannot read %s\n", name, p.string().c_str());
            ++bad;
            continue;
        }
        // A copy of exactly the input size, so heap overreads past the end are detected.
        auto* copy = static_cast<std::uint8_t*>(std::malloc(buf.empty() ? 1 : buf.size()));
        if (!copy) return bad + 1;
        if (!buf.empty()) std::memcpy(copy, buf.data(), buf.size());
        const auto t0 = std::chrono::steady_clock::now();
        fn(copy, buf.size());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::free(copy);
        worst = std::max(worst, ms);
        if (max_ms > 0 && ms > static_cast<double>(max_ms)) {
            std::fprintf(stderr, "%s: SLOW %.0f ms (limit %ld) %s\n", name, ms, max_ms, p.string().c_str());
            ++bad;
        }
    }
    std::printf("%s: %zu inputs, slowest %.1f ms\n", name, files.size(), worst);
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    long max_ms = 0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-ms" && i + 1 < argc) {
            max_ms = std::strtol(argv[++i], nullptr, 10);
        } else {
            args.push_back(a);
        }
    }
#if defined(SIAARA_FUZZ_COMBINED)
    auto& reg = siaara_fuzz::targets();
    std::sort(reg.begin(), reg.end(), [](const auto& a, const auto& b) { return std::strcmp(a.name, b.name) < 0; });
    if (args.size() == 1 && args[0] == "--list") {
        for (const auto& t : reg) std::printf("%s\n", t.name);
        return 0;
    }
    if (args.size() >= 3 && args[0] == "--target") {
        for (const auto& t : reg)
            if (args[1] == t.name) {
                std::vector<fs::path> files;
                for (std::size_t i = 2; i < args.size(); ++i) collect(args[i], files);
                return replay(t.fn, t.name, files, max_ms) ? 1 : 0;
            }
        std::fprintf(stderr, "unknown target %s\n", args[1].c_str());
        return 2;
    }
    if (args.size() >= 2 && args[0] == "--all") {
        int bad = 0;
        std::size_t total = 0;
        for (const auto& t : reg) {
            std::vector<fs::path> files;
            for (std::size_t i = 1; i < args.size(); ++i) {
                const fs::path d = fs::path(args[i]) / t.name;
                std::error_code ec;
                if (fs::is_directory(d, ec)) collect(d, files);
            }
            total += files.size();
            bad += replay(t.fn, t.name, files, max_ms);
        }
        std::printf("%zu targets, %zu inputs, %d failures\n", reg.size(), total, bad);
        return bad ? 1 : 0;
    }
    std::fprintf(stderr, "usage: %s [--max-ms N] (--list | --target NAME PATH... | --all ROOT...)\n", argv[0]);
    return 2;
#else
    if (args.empty()) {
        std::fprintf(stderr, "usage: %s [--max-ms N] FILE-OR-FOLDER...\n", argv[0]);
        return 2;
    }
    std::vector<fs::path> files;
    for (const auto& a : args) collect(a, files);
    return replay(&LLVMFuzzerTestOneInput, "replay", files, max_ms) ? 1 : 0;
#endif
}
