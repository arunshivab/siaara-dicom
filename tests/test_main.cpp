#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) siaara_test::data_dir() = argv[++i];
        else filter = argv[i];
    }
    int run = 0;
    for (auto& c : siaara_test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = siaara_test::failures();
        c.fn();
        ++run;
        std::printf("%s %s\n", siaara_test::failures() == before ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("\n%d test cases, %d failed checks\n", run, siaara_test::failures());
    return siaara_test::failures() == 0 ? 0 : 1;
}
