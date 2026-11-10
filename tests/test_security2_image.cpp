// Regression tests for the 2026-11-10 02:15 security review, 2D rasterisation (R-083, R-084).
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "clinical/roi.hpp"
#include "core/dataset.hpp"
#include "image/overlay.hpp"
#include "image/pixel_info.hpp"
#include "test.hpp"

using namespace siaara;

namespace {
image::PixelInfo info(std::uint32_t rows, std::uint32_t cols) {
    image::PixelInfo pi;
    pi.rows = rows;
    pi.columns = cols;
    pi.bits_allocated = 8;
    pi.bits_stored = 8;
    pi.number_of_frames = 1;
    return pi;
}
std::size_t visible_count(const image::Shutters& sh, std::uint32_t rows, std::uint32_t cols, Status* st = nullptr) {
    std::vector<std::uint8_t> vis(std::size_t{rows} * cols);
    Status s = image::shutter_mask(sh, info(rows, cols), 0, {}, vis);
    if (st) *st = s;
    std::size_t n = 0;
    for (auto v : vis) n += v;
    return n;
}
}  // namespace

TEST_CASE("R-083: far-away vertices do not overflow the double to integer conversions") {
    // Under UBSan the old casts of 1e300 were undefined behaviour (float-cast-overflow).
    const std::vector<clinical::Point2> poly = {{-1e300, 2.0}, {1e300, 2.0}, {1e300, 1e300}, {-1e300, 1e300}};
    auto m = clinical::roi_mask(16, 16, clinical::Roi::polygon(poly));
    REQUIRE(m);
    std::size_t in = 0;
    for (float v : *m) in += v > 0.0f ? 1u : 0u;
    CHECK_EQ(in, 16u * 14u);  // rows 2..15 entirely inside
    image::Shutters sh;
    sh.polygonal = true;
    sh.vertices = {{-1e300, -1e300}, {-1e300, 1e300}, {1e300, 1e300}, {1e300, -1e300}};
    CHECK_EQ(visible_count(sh, 16, 16), 256u);
}

TEST_CASE("R-084: polygonal shutters are capped at 4096 vertices and keep their pixel rule") {
    Dataset ds;
    (void)ds.set_string(0x00181600, VR::CS, "POLYGONAL");
    std::string v;
    for (int i = 0; i < 4097; ++i) {
        if (i) v += '\\';
        v += std::to_string(1 + i % 50) + "\\" + std::to_string(1 + (i * 7) % 50);
    }
    (void)ds.set_string(0x00181620, VR::IS, v);
    auto lenient = image::read_shutters(ds, true);
    REQUIRE(lenient);
    CHECK(!lenient->polygonal);
    CHECK(!image::read_shutters(ds, false));
    // A square with corners on pixel centres: boundary included (rows/cols 3..8 of 10).
    image::Shutters sq;
    sq.polygonal = true;
    sq.vertices = {{3, 3}, {3, 8}, {8, 8}, {8, 3}};
    CHECK_EQ(visible_count(sq, 10, 10), 36u);
    // 4096-vertex zigzag on a tall image: an edge-table scan, not rows x vertices.
    image::Shutters zz;
    zz.polygonal = true;
    for (int i = 0; i < 4094; ++i) zz.vertices.push_back({1.0 + i * (7998.0 / 4094.0), i % 2 ? 30.0 : 10.0});
    zz.vertices.push_back({7999.0, 1.0});
    zz.vertices.push_back({1.0, 1.0});
    const auto t0 = std::chrono::steady_clock::now();
    Status st;
    CHECK(visible_count(zz, 8000, 40, &st) > 0u);
    CHECK(st.is_ok());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}
