// SIAARA DICOM Toolkit - fuzz harness support (test tooling only; never linked into the library).
//
// Every target is one translation unit under fuzz/targets/ written as
//
//     SIAARA_FUZZ_TARGET(name) { ... use data, size ...; return 0; }
//
// Built three ways:
//   - libFuzzer (clang -fsanitize=fuzzer): the macro defines LLVMFuzzerTestOneInput;
//   - plain replay executable (any compiler): the same, plus fuzz/replay_main.cpp as main();
//   - combined replay (SIAARA_FUZZ_COMBINED): every target registers itself by name in one
//     executable that the normal test suite runs over fuzz/corpus and tests/regress.
// A harness reports a property violation with FUZZ_REQUIRE, which aborts (libFuzzer records the
// input as a crash; the replay tools stop with a non-zero exit status).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace siaara_fuzz {

using TargetFn = int (*)(const std::uint8_t* data, std::size_t size);

struct Target {
    const char* name;
    TargetFn fn;
};

inline std::vector<Target>& targets() {
    static std::vector<Target> t;
    return t;
}

struct Registrar {
    Registrar(const char* name, TargetFn fn) { targets().push_back({name, fn}); }
};

[[noreturn]] inline void fail(const char* file, int line, const char* what) {
    std::fprintf(stderr, "FUZZ_REQUIRE failed at %s:%d: %s\n", file, line, what);
    std::fflush(stderr);
    std::abort();
}

template <class R>
std::string message_of(const R& r) {
    if constexpr (requires { r.error().message; })
        return r.error().message;
    else
        return r.message;
}

// Reads typed values from the front of the input; past the end everything reads as zero.
class Input {
public:
    Input(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}
    std::uint8_t u8() { return pos_ < n_ ? d_[pos_++] : 0; }
    std::uint16_t u16() { return static_cast<std::uint16_t>(u8() | (u8() << 8)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(u16()) | (static_cast<std::uint32_t>(u16()) << 16); }
    bool flag() { return (u8() & 1u) != 0; }
    // The next `k` bytes (fewer at the end).
    std::string_view take(std::size_t k) {
        if (k > n_ - pos_) k = n_ - pos_;
        std::string_view s(reinterpret_cast<const char*>(d_ + pos_), k);
        pos_ += k;
        return s;
    }
    // Bytes up to (not including) the next `sep`, which is consumed; the rest when absent.
    std::string_view until(char sep) {
        std::size_t e = pos_;
        while (e < n_ && d_[e] != static_cast<std::uint8_t>(sep)) ++e;
        std::string_view s(reinterpret_cast<const char*>(d_ + pos_), e - pos_);
        pos_ = e < n_ ? e + 1 : e;
        return s;
    }
    std::string_view rest() { return take(n_ - pos_); }
    const std::uint8_t* ptr() const { return d_ + pos_; }
    std::size_t remaining() const { return n_ - pos_; }

private:
    const std::uint8_t* d_;
    std::size_t n_;
    std::size_t pos_ = 0;
};

}  // namespace siaara_fuzz

#define FUZZ_REQUIRE(cond)                                                    \
    do {                                                                      \
        if (!(cond)) ::siaara_fuzz::fail(__FILE__, __LINE__, #cond);          \
    } while (0)

// For a siaara::Status or Result: also prints the error message.
#define FUZZ_REQUIRE_OK(expr)                                                                    \
    do {                                                                                         \
        const auto& fuzz_r_ = (expr);                                                            \
        if (!fuzz_r_.is_ok()) {                                                                  \
            std::fprintf(stderr, "error: %s\n", ::siaara_fuzz::message_of(fuzz_r_).c_str());     \
            ::siaara_fuzz::fail(__FILE__, __LINE__, #expr);                                      \
        }                                                                                        \
    } while (0)

#if defined(SIAARA_FUZZ_COMBINED)
#define SIAARA_FUZZ_TARGET(name)                                                              \
    static int siaara_fuzz_one(const std::uint8_t* data, std::size_t size);                   \
    static const ::siaara_fuzz::Registrar siaara_fuzz_registrar(#name, &siaara_fuzz_one);     \
    static int siaara_fuzz_one(const std::uint8_t* data, std::size_t size)
#else
#define SIAARA_FUZZ_TARGET(name)                                                              \
    extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
#endif
