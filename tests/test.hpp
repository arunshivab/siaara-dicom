// Minimal zero-dependency test framework for SIAARA.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace siaara_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline std::string& data_dir() {
    static std::string d;
    return d;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace siaara_test

#define SIAARA_CAT2(a, b) a##b
#define SIAARA_CAT(a, b) SIAARA_CAT2(a, b)
#define TEST_CASE(name)                                                                     \
    static void SIAARA_CAT(test_fn_, __LINE__)();                                           \
    static ::siaara_test::Registrar SIAARA_CAT(test_reg_, __LINE__)(name, &SIAARA_CAT(test_fn_, __LINE__)); \
    static void SIAARA_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
            ++::siaara_test::failures();                                                   \
        }                                                                                  \
    } while (0)

#define REQUIRE(cond)                                                                      \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "  FAILED (fatal): %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++::siaara_test::failures();                                                   \
            return;                                                                        \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
