// Test-only: writes the broken sample files of repair_samples.hpp to a folder for
// tests/crosscheck/repair_crosscheck.py. Usage: repair_samples <output folder>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/io.hpp"
#include "repair_samples.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: repair_samples <output folder>\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(argv[1], ec);
    const std::string dir = argv[1];
    int n = 0;
    for (const auto& s : repair_samples::all_samples()) {
        if (!siaara::io::write_file_atomic(dir + "/" + s.name + ".dcm", s.bytes.data(), s.bytes.size())) return 1;
        std::printf("%s\t%s\n", s.name.c_str(), s.fix.c_str());
        ++n;
    }
    const auto study = repair_samples::study_set();
    std::filesystem::create_directories(dir + "/study", ec);
    for (std::size_t i = 0; i < study.size(); ++i)
        if (!siaara::io::write_file_atomic(dir + "/study/instance" + std::to_string(i) + ".dcm", study[i].data(),
                                           study[i].size()))
            return 1;
    std::fprintf(stderr, "%d samples and %zu study files written\n", n, study.size());
    return 0;
}
