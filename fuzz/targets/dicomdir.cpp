// Fuzz target: DICOMDIR reader (PS3.10 8, PS3.3 F): record links (offsets, loops, depth), keys
// and Referenced File ID checks. The input is written to a scratch file named DICOMDIR.
#include <fstream>

#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "store/dicomdir.hpp"

using namespace siaara;
using namespace siaara::store;

SIAARA_FUZZ_TARGET(dicomdir) {
    const std::filesystem::path p = siaara_fuzz::scratch_dir("dicomdir") / "DICOMDIR";
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    for (int pass = 0; pass < 2; ++pass) {
        DicomdirReadOptions o;
        o.include_inactive = pass == 1;
        o.resolve_files = pass == 0;
        std::size_t n = 0;
        (void)read_dicomdir(p.string(), [&n](const DicomdirRecord& r) {
            FUZZ_REQUIRE(r.depth <= 16);
            return ++n < 100'000;
        }, o);
    }
    return 0;
}
