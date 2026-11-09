// Test-only allocation guard: the test binary replaces the global operator new/delete
// (test_alloc_guard.cpp) with versions that count the bytes currently allocated. While an
// AllocGuard is alive, allocations that would raise the bytes allocated since the guard started
// above its limit throw std::bad_alloc instead, and the peak is recorded. Amplification
// regression tests use it to bound a decoder's memory without letting it take the machine.
// Only the thread that runs the test should allocate while a guard is active.
#pragma once

#include <cstddef>

namespace siaara_test {

class AllocGuard {
public:
    explicit AllocGuard(std::size_t limit_bytes);
    ~AllocGuard();
    AllocGuard(const AllocGuard&) = delete;
    AllocGuard& operator=(const AllocGuard&) = delete;
    // Highest number of bytes allocated (above the level at construction) while the guard was alive.
    std::size_t peak() const noexcept;
    // True when an allocation was refused because of the limit.
    bool refused() const noexcept;
};

}  // namespace siaara_test
