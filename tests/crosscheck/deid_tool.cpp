// Test-only helper for tests/crosscheck/deid_crosscheck.py: de-identifies one file with a fixed
// key and writes the result, whatever the burned-in status.
//   deid_tool <in> <out> [option bits, hex] [remove]
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/reader.hpp"
#include "core/writer.hpp"
#include "deid/deid.hpp"

using namespace siaara;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: deid_tool <in> <out> [option bits hex] [remove]\n");
        return 2;
    }
    auto f = read_file(argv[1]);
    if (!f) {
        std::fprintf(stderr, "read failed: %s\n", f.error().message.c_str());
        return 3;
    }
    static const std::string key = "siaara-crosscheck-key-0123456789";
    deid::Options o;
    o.key = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
    if (argc > 3) o.options = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 16));
    if (argc > 4 && std::string_view(argv[4]) == "remove") o.combined = deid::CombinedPolicy::Remove;
    deid::Report rep;
    if (Status s = deid::deidentify(*f, o, &rep); !s) {
        std::fprintf(stderr, "deidentify failed: %s\n", s.message.c_str());
        return 4;
    }
    if (Status s = write_file(*f, argv[2]); !s) {
        std::fprintf(stderr, "write failed: %s\n", s.message.c_str());
        return 5;
    }
    std::printf("flags=%u removed=%zu emptied=%zu dummied=%zu cleaned=%zu uids=%zu\n", rep.flags, rep.removed,
                rep.emptied, rep.dummied, rep.cleaned, rep.uids_replaced);
    return 0;
}
