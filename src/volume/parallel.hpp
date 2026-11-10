// SIAARA DICOM Toolkit - tiny work-sharing helper for the 3D layer (internal).
// Work items are claimed from an atomic counter by up to `threads` workers (std::thread). The
// calling thread always takes part, so when the OS refuses to start threads the work still
// completes on the caller. fn(item, worker) must not throw; `worker` is in [0, workers).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace siaara::volume::detail {

// Most workers any request gets (R-060): per-worker buffers (a 2 MB counter table for 16-bit
// histograms, a frame per worker when building) are sized by this, not by the caller's value.
constexpr unsigned kMaxThreads = 64;

// Threads to use for a request: 0 = hardware concurrency (at most 16); otherwise the request
// clamped to min(kMaxThreads, 2 x hardware concurrency) (R-060).
inline unsigned resolve_threads(unsigned requested) noexcept {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    if (requested == 0) return std::min(hw, 16u);
    return std::min({requested, kMaxThreads, std::max(2u * hw, 2u)});
}

// Runs fn(i, worker) for i in [0, count). Returns the number of workers that ran (>= 1).
template <class F>
unsigned parallel_for(std::uint64_t count, unsigned threads, F&& fn) noexcept {
    threads = resolve_threads(threads);
    if (count < threads) threads = static_cast<unsigned>(count == 0 ? 1 : count);
    if (threads <= 1) {
        for (std::uint64_t i = 0; i < count; ++i) fn(i, 0u);
        return 1;
    }
    std::atomic<std::uint64_t> next{0};
    auto work = [&](unsigned worker) noexcept {
        for (;;) {
            const std::uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) return;
            fn(i, worker);
        }
    };
    std::vector<std::thread> pool;
    try {
        pool.reserve(threads - 1);
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, t);
    } catch (...) {
        // Not all workers could start (std::system_error, std::bad_alloc): the ones that did and
        // the caller finish the work.
        threads = static_cast<unsigned>(pool.size() + 1);
    }
    work(0u);
    for (auto& t : pool) t.join();
    return threads;
}

}  // namespace siaara::volume::detail
