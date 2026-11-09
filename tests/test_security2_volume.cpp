// Regression tests for the 2026-11-10 02:15 security review, 3D layer (R-060).
#include <chrono>
#include <cstdint>
#include <thread>

#include "test.hpp"
#include "volume/parallel.hpp"
#include "volume/volume.hpp"

using namespace siaara;
using namespace siaara::volume;

TEST_CASE("R-060: a huge thread count is clamped before per-worker buffers are sized") {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    CHECK(detail::resolve_threads(1000000) <= 64u);
    CHECK(detail::resolve_threads(1000000) <= 2 * hw + 2);
    CHECK_EQ(detail::resolve_threads(1), 1u);
    CHECK(detail::resolve_threads(0) >= 1u);
    Grid g;
    g.dims = {8, 8, 64};
    auto v = Volume::allocate(g, VoxelType::Int16);
    REQUIRE(v);
    // 100000 workers x a 2 MB 16-bit counter table each would be 200 GB.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(v->update_statistics(100000).is_ok());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
}
