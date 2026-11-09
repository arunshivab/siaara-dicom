#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "test.hpp"

namespace {
std::atomic<const char*> g_running{nullptr};  // the test case being run, for the terminate handler
}

int main(int argc, char** argv) {
    // Name the running test if it ends in std::terminate: stdout is buffered when redirected, so its
    // last lines are lost when the process aborts.
    std::set_terminate([] {
        std::fflush(stdout);  // keep the test's own progress lines: stdout is buffered when redirected
        std::fprintf(stderr, "std::terminate called during test case: %s\n", g_running.load() ? g_running.load() : "(none)");
        std::abort();
    });
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) siaara_test::data_dir() = argv[++i];
        else filter = argv[i];
    }
    int run = 0;
    for (auto& c : siaara_test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = siaara_test::failures();
        g_running = c.name;
        c.fn();
        g_running = nullptr;
        ++run;
        std::printf("%s %s\n", siaara_test::failures() == before ? "[ ok ]" : "[FAIL]", c.name);
        std::fflush(stdout);
    }
    std::printf("\n%d test cases, %d failed checks\n", run, siaara_test::failures());
    return siaara_test::failures() == 0 ? 0 : 1;
}
