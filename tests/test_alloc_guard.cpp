// Global operator new/delete replacement for the test binary (see alloc_guard.hpp). Every
// non-aligned form is replaced so that allocation and release always pair up; the aligned forms
// keep the implementation's own pair.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <thread>

#include "alloc_guard.hpp"
#include "test.hpp"

namespace {

// The header keeps the returned pointer at the alignment the compiler assumes for operator new
// (__STDCPP_DEFAULT_NEW_ALIGNMENT__: 16 on Apple arm64, where alignof(std::max_align_t) is only 8).
constexpr std::size_t kMax(std::size_t a, std::size_t b) { return a > b ? a : b; }
constexpr std::size_t kHeader =
    kMax(kMax(alignof(std::max_align_t), sizeof(std::size_t)), static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__));
std::atomic<std::size_t> g_current{0};
// Only the thread that created the guard is limited: operating-system and framework threads
// (libdispatch, Metal, ...) allocate through the same operator new and must never see a refusal.
thread_local bool t_guarded = false;
std::atomic<bool> g_active{false};
std::atomic<std::size_t> g_base{0};
std::atomic<std::size_t> g_limit{0};
std::atomic<std::size_t> g_peak{0};
std::atomic<bool> g_refused{false};

void* allocate(std::size_t n, bool nothrow) {
    if (t_guarded && g_active.load(std::memory_order_relaxed)) {
        const std::size_t now = g_current.load(std::memory_order_relaxed);
        const std::size_t base = g_base.load(std::memory_order_relaxed);
        const std::size_t used = now > base ? now - base : 0;
        if (n > g_limit.load(std::memory_order_relaxed) || used > g_limit.load(std::memory_order_relaxed) - n) {
            g_refused.store(true, std::memory_order_relaxed);
            if (nothrow) return nullptr;
            throw std::bad_alloc();
        }
    }
    if (n > SIZE_MAX - kHeader) {
        if (nothrow) return nullptr;
        throw std::bad_alloc();
    }
    void* p = std::malloc(n + kHeader);
    if (!p) {
        if (nothrow) return nullptr;
        throw std::bad_alloc();
    }
    *static_cast<std::size_t*>(p) = n;
    const std::size_t now = g_current.fetch_add(n, std::memory_order_relaxed) + n;
    if (g_active.load(std::memory_order_relaxed)) {
        const std::size_t base = g_base.load(std::memory_order_relaxed);
        const std::size_t used = now > base ? now - base : 0;
        std::size_t peak = g_peak.load(std::memory_order_relaxed);
        while (used > peak && !g_peak.compare_exchange_weak(peak, used, std::memory_order_relaxed)) {
        }
    }
    return static_cast<unsigned char*>(p) + kHeader;
}

void release(void* p) noexcept {
    if (!p) return;
    unsigned char* b = static_cast<unsigned char*>(p) - kHeader;
    g_current.fetch_sub(*reinterpret_cast<std::size_t*>(b), std::memory_order_relaxed);
    std::free(b);
}

}  // namespace

void* operator new(std::size_t n) { return allocate(n, false); }
void* operator new[](std::size_t n) { return allocate(n, false); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return allocate(n, true);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return allocate(n, true);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

namespace siaara_test {

AllocGuard::AllocGuard(std::size_t limit_bytes) {
    g_base.store(g_current.load());
    g_limit.store(limit_bytes);
    g_peak.store(0);
    g_refused.store(false);
    t_guarded = true;
    g_active.store(true);
}

AllocGuard::~AllocGuard() {
    g_active.store(false);
    t_guarded = false;
}

std::size_t AllocGuard::peak() const noexcept { return g_peak.load(); }

bool AllocGuard::refused() const noexcept { return g_refused.load(); }

}  // namespace siaara_test

TEST_CASE("test harness: allocation guard counts and refuses") {
    siaara_test::AllocGuard g(1u << 20);
    void* small = ::operator new(8000);
    CHECK(g.peak() >= 8000);
    bool threw = false;
    try {
        void* big = ::operator new(2u << 20);
        ::operator delete(big);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(g.refused());
    ::operator delete(small);
    // Other threads are counted but never refused.
    bool other_threw = false;
    std::thread th([&] {
        try {
            void* big = ::operator new(2u << 20);
            ::operator delete(big);
        } catch (const std::bad_alloc&) {
            other_threw = true;
        }
    });
    th.join();
    CHECK(!other_threw);
}
