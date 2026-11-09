// SIAARA DICOM Toolkit - shared --help handling of the command-line tools (testing-chat F16).
// `--help` / `-h` anywhere on the command line prints the usage to stdout and exits with 0; a
// usage error still prints it to stderr and exits with 2.
#pragma once

#include <cstdio>
#include <cstring>

namespace siaara_tools {

inline std::FILE*& usage_stream() {
    static std::FILE* s = stderr;
    return s;
}

// True when the command line asks for help; usage text then goes to stdout.
inline bool wants_help(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage_stream() = stdout;
            return true;
        }
    return false;
}

}  // namespace siaara_tools
