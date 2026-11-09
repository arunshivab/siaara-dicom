// GPU backend tests (src/gpu): every view mode rendered on the GPU is compared with the CPU
// reference renderer (src/volume/render3d.cpp), plus device enumeration, the fallback when the
// Vulkan loader is missing, resource cleanup, SPIR-V structure checks, spirv-val validation of
// every generated module, and the C API.
//
// On a machine without a Vulkan device (CI runners without Mesa's lavapipe) the comparison tests
// print SKIP and pass. Tolerances (see the checks below):
//   - MPR nearest: exact for int16 volumes (values are fetched exactly and coloured by a LUT the
//     host computes in double like the CPU); float32 volumes within 1 level (window in float).
//   - MPR trilinear and slabs: hardware texture filtering uses fixed-point weights (8 bits on most
//     GPUs, see DeviceInfo::sub_texel_bits), so values differ by up to ~1/256 of the neighbour
//     difference; checked as a per-channel limit plus a mean limit. The 8-fetch path is checked
//     against the same CPU image with tighter limits.
//   - volume rendering: float instead of double along the ray; within a few levels per channel,
//     with a small fraction of pixels allowed beyond that at silhouettes / brick boundaries.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "gpu/gpu.hpp"
#include "gpu/spirv_kernels.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "volume/render3d.hpp"
#include "volume/resample.hpp"
#include "volume/volume.hpp"

using namespace siaara;
using namespace siaara::volume;

namespace {

using Fn = std::function<float(std::uint32_t, std::uint32_t, std::uint32_t)>;

std::shared_ptr<Volume> make_volume(std::array<std::uint32_t, 3> dims, std::array<double, 3> spacing, Vec3 origin,
                                    std::array<Vec3, 3> axes, VoxelType type, const Fn& f) {
    Grid g;
    g.dims = dims;
    g.spacing = spacing;
    g.origin = origin;
    g.axes = axes;
    auto r = Volume::allocate(g, type);
    if (!r) return nullptr;
    auto v = std::make_shared<Volume>(std::move(r).value());
    for (std::uint32_t k = 0; k < dims[2]; ++k)
        for (std::uint32_t j = 0; j < dims[1]; ++j)
            for (std::uint32_t i = 0; i < dims[0]; ++i) v->set(i, j, k, f(i, j, k));
    (void)v->update_statistics(2);
    return v;
}

std::array<Vec3, 3> rotated_axes(double a, double b) {
    const Vec3 x{std::cos(a), std::sin(a), 0}, y{-std::sin(a), std::cos(a), 0}, z{0, 0, 1};
    auto rx = [&](Vec3 v) { return Vec3{v.x, v.y * std::cos(b) - v.z * std::sin(b), v.y * std::sin(b) + v.z * std::cos(b)}; };
    return {rx(x), rx(y), rx(z)};
}
std::array<Vec3, 3> identity_axes() { return {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}; }

// A CT-like phantom: smooth background, a dense sphere, a hollow shell and a ramp.
float phantom(std::uint32_t i, std::uint32_t j, std::uint32_t k, std::array<std::uint32_t, 3> d) {
    const double x = (i - d[0] / 2.0) / d[0], y = (j - d[1] / 2.0) / d[1], z = (k - d[2] / 2.0) / d[2];
    const double r = std::sqrt(x * x + y * y + z * z);
    double v = -900.0 + 400.0 * (0.5 + 0.5 * std::sin(6.0 * x) * std::cos(5.0 * y));
    if (r < 0.38) v = 40.0 + 30.0 * std::sin(13.0 * z);
    if (r < 0.22) v = 700.0 + 300.0 * x;
    if (std::fabs(r - 0.30) < 0.02) v = 1200.0;
    v += 3.0 * static_cast<double>(k);
    return static_cast<float>(std::round(v));
}

std::shared_ptr<Volume> ct_volume(VoxelType type, bool oblique, std::array<double, 3> spacing = {0.8, 0.9, 1.5}) {
    const std::array<std::uint32_t, 3> d{48, 44, 36};
    return make_volume(d, spacing, {-20, -15, 30}, oblique ? rotated_axes(0.3, 0.2) : identity_axes(), type,
                       [d](std::uint32_t i, std::uint32_t j, std::uint32_t k) { return phantom(i, j, k, d); });
}

// Distinct RGBA values in an image (guards against comparing two blank images).
std::size_t distinct(const std::vector<std::uint8_t>& a) {
    std::vector<std::uint32_t> px(a.size() / 4);
    for (std::size_t i = 0; i < px.size(); ++i) std::memcpy(&px[i], &a[4 * i], 4);
    std::sort(px.begin(), px.end());
    return static_cast<std::size_t>(std::unique(px.begin(), px.end()) - px.begin());
}

// SKIP support: the GPU backend with any device (lavapipe included).
bool gpu_present(const char* test) {
    if (gpu::acquire()) return true;
    std::printf("  SKIP %s: no GPU device (%s)\n", test, gpu::unavailable_reason().c_str());
    return false;
}

struct Diff {
    std::size_t channels = 0, over = 0;  // channel values with |diff| > tol
    int max = 0;
    double mean = 0.0;
    std::size_t exact_pixels = 0;
};
Diff compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int tol) {
    Diff d;
    double sum = 0.0;
    for (std::size_t p = 0; p + 3 < a.size(); p += 4) {
        bool same = true;
        for (std::size_t c = 0; c < 4; ++c) {
            const int e = std::abs(static_cast<int>(a[p + c]) - static_cast<int>(b[p + c]));
            d.max = std::max(d.max, e);
            sum += e;
            if (e > tol) ++d.over;
            if (e) same = false;
            ++d.channels;
        }
        if (same) ++d.exact_pixels;
    }
    d.mean = d.channels ? sum / static_cast<double>(d.channels) : 0.0;
    return d;
}

// Renders on the CPU and then on the GPU (backend Gpu) and returns the difference; nullopt-like
// empty diff with channels == 0 when the GPU did not render.
struct Pair {
    std::vector<std::uint8_t> cpu, gpu;
    bool gpu_used = false;
    std::string reason;
};
Pair render_both(View& v) {
    Pair p;
    p.cpu.assign(std::size_t{v.width()} * v.height() * 4u, 0);
    p.gpu.assign(p.cpu.size(), 0);
    CHECK(v.set_backend(RenderBackend::Cpu));
    CHECK(v.render_rgba8(p.cpu));
    CHECK(v.last_backend() == RenderBackend::Cpu);
    CHECK(v.set_backend(RenderBackend::Gpu));
    CHECK(v.render_rgba8(p.gpu));
    p.gpu_used = v.last_backend() == RenderBackend::Gpu;
    CHECK(v.set_backend(RenderBackend::Cpu));
    return p;
}

void report(const char* what, const Diff& d) {
    std::printf("  %-44s max %3d  mean %.4f  over-tol %zu/%zu  exact px %zu\n", what, d.max, d.mean, d.over, d.channels,
                d.exact_pixels);
}

// Checks: all channels within tol, except a fraction `outliers` that may reach tol_out.
void expect_close(const char* what, const Pair& p, int tol, double max_mean, double outliers = 0.0, int tol_out = 255) {
    CHECK(p.gpu_used);
    if (!p.gpu_used) return;
    const Diff d = compare(p.cpu, p.gpu, tol);
    report(what, d);
    CHECK(distinct(p.cpu) >= 16);
    CHECK(d.mean <= max_mean);
    CHECK(static_cast<double>(d.over) <= outliers * static_cast<double>(d.channels));
    CHECK(d.max <= tol_out);
}

}  // namespace

// ---------------------------------------------------------------- SPIR-V
namespace {
// Structural walk of a SPIR-V binary: header, every instruction's word count within bounds,
// ids below the bound for the result ids of type / constant / variable instructions.
bool spirv_well_formed(const std::vector<std::uint32_t>& w, std::string& why) {
    if (w.size() < 5) return why = "short", false;
    if (w[0] != 0x07230203u) return why = "magic", false;
    if (w[1] != 0x00010000u) return why = "version", false;
    const std::uint32_t bound = w[3];
    if (bound == 0) return why = "bound", false;
    std::size_t i = 5;
    bool saw_entry = false, saw_function_end = false;
    while (i < w.size()) {
        const std::uint32_t count = w[i] >> 16, opcode = w[i] & 0xFFFFu;
        if (count == 0 || i + count > w.size()) return why = "word count at " + std::to_string(i), false;
        if (opcode == 15) saw_entry = true;
        if (opcode == 56) saw_function_end = true;
        // OpTypeInt / OpTypeFloat / OpVariable / OpConstant: result id range.
        if ((opcode == 21 || opcode == 22) && w[i + 1] >= bound) return why = "type id", false;
        if ((opcode == 59 || opcode == 43) && w[i + 2] >= bound) return why = "result id", false;
        i += count;
    }
    if (!saw_entry || !saw_function_end) return why = "entry point / function", false;
    return true;
}
}  // namespace

TEST_CASE("gpu/spirv: generated kernels are structurally well formed") {
    for (std::uint32_t k = 0; k < gpu::param::KernelCount; ++k) {
        const auto code = gpu::spirv::kernel(static_cast<gpu::param::Kernel>(k));
        std::string why;
        CHECK(spirv_well_formed(code, why));
        if (!why.empty()) std::printf("  kernel %u: %s\n", k, why.c_str());
        // Deterministic.
        CHECK(code == gpu::spirv::kernel(static_cast<gpu::param::Kernel>(k)));
    }
}

TEST_CASE("gpu/spirv: every generated module passes spirv-val (Vulkan 1.0 environment)") {
#if defined(_WIN32)
    std::printf("  SKIP: spirv-val check runs on POSIX hosts only\n");
#else
    if (std::system("spirv-val --version > /dev/null 2>&1") != 0) {
        std::printf("  SKIP: spirv-val not installed (apt-get install spirv-tools)\n");
        return;
    }
    for (std::uint32_t k = 0; k < gpu::param::KernelCount; ++k) {
        const auto code = gpu::spirv::kernel(static_cast<gpu::param::Kernel>(k));
        char path[128];
        std::snprintf(path, sizeof path, "/tmp/siaara_gpu_kernel_%u_%d.spv", k, static_cast<int>(::getpid()));
        FILE* f = std::fopen(path, "wb");
        REQUIRE(f != nullptr);
        CHECK(std::fwrite(code.data(), 4, code.size(), f) == code.size());
        std::fclose(f);
        const std::string cmd = std::string("spirv-val --target-env vulkan1.0 ") + path;
        const int rc = std::system(cmd.c_str());
        CHECK(rc == 0);
        std::printf("  kernel %u: %zu words, spirv-val %s\n", k, code.size(), rc == 0 ? "ok" : "FAILED");
        std::remove(path);
    }
#endif
}

// ---------------------------------------------------------------- devices and fallback
TEST_CASE("gpu/devices: enumeration and selection") {
    const auto devs = gpu::devices();
    std::printf("  %zu device(s)%s\n", devs.size(), devs.empty() ? (" - " + gpu::unavailable_reason()).c_str() : "");
    int selected = 0;
    for (const auto& d : devs) {
        std::printf("  %s [%s] type %d, %llu MiB, 3D %u, sub-texel bits %u, filtering f32 %d int16 %d%s; %s\n", d.name.c_str(),
                    d.api == gpu::Api::Vulkan ? "Vulkan" : "Metal", static_cast<int>(d.type),
                    static_cast<unsigned long long>(d.memory_bytes >> 20), d.max_texture_3d, d.sub_texel_bits,
                    d.float32_filtering ? 1 : 0, d.int16_filtering ? 1 : 0, d.selected ? ", selected" : "", d.driver.c_str());
        CHECK(!d.name.empty());
        CHECK(d.api != gpu::Api::None);
        CHECK(d.max_texture_3d >= 256);  // Vulkan's required minimum
        if (d.selected) ++selected;
    }
    CHECK(selected == (devs.empty() ? 0 : 1));
    // Discrete devices are preferred over everything else.
    for (const auto& d : devs)
        if (d.type == gpu::DeviceType::Discrete) {
            for (const auto& e : devs)
                if (e.selected) CHECK(e.type == gpu::DeviceType::Discrete);
        }
    if (!devs.empty()) CHECK(gpu::acquire() != nullptr);
}

TEST_CASE("gpu/fallback: missing Vulkan loader falls back to the CPU renderer") {
    auto vol = ct_volume(VoxelType::Int16, false);
    REQUIRE(vol);
    std::vector<std::uint8_t> cpu, other;
    {
        auto v = View::create(vol, 64, 48);
        REQUIRE(v);
        cpu.assign(64 * 48 * 4, 0);
        CHECK(v->render_rgba8(cpu));
    }
    gpu::testing::set_vulkan_loader("libvulkan-siaara-missing.so.1");
    gpu::testing::reset();
#if !defined(__APPLE__)
    CHECK(gpu::devices().empty());
    CHECK(gpu::acquire() == nullptr);
    CHECK(!gpu::unavailable_reason().empty());
    std::printf("  reason: %s\n", gpu::unavailable_reason().c_str());
#endif
    {
        auto v = View::create(vol, 64, 48);
        REQUIRE(v);
#if !defined(__APPLE__)
        const Status s = v->set_backend(RenderBackend::Gpu);
        CHECK(!s);
        CHECK(s.code == Errc::Unsupported);
#endif
        CHECK(v->set_backend(RenderBackend::Auto));
        other.assign(cpu.size(), 1);
        CHECK(v->render_rgba8(other));
#if !defined(__APPLE__)
        CHECK(v->last_backend() == RenderBackend::Cpu);
#endif
        CHECK(other == cpu);
    }
    // Names with a path are refused (never loaded from a directory).
    gpu::testing::set_vulkan_loader("./libvulkan.so.1");
    gpu::testing::reset();
#if !defined(__APPLE__)
    CHECK(gpu::acquire() == nullptr);
#endif
    gpu::testing::set_vulkan_loader(nullptr);
    gpu::testing::reset();
}

TEST_CASE("gpu/fallback: Auto skips software devices; unsupported modes render on the CPU") {
    if (!gpu_present("fallback modes")) return;
    auto vol = ct_volume(VoxelType::Int16, true);
    REQUIRE(vol);
    auto v = View::create(vol, 40, 30);
    REQUIRE(v);
    std::vector<std::uint8_t> a(40 * 30 * 4), b(a.size());
    CHECK(v->set_backend(RenderBackend::Auto));
    CHECK(v->render_rgba8(a));
    const bool software = gpu::acquire()->info().type == gpu::DeviceType::Cpu;
    CHECK(v->last_backend() == (software ? RenderBackend::Cpu : RenderBackend::Gpu));
    // Cubic interpolation and curved MPR are CPU-only: Gpu falls back and matches the CPU exactly.
    CHECK(v->set_mode(ViewMode::Mpr, Interpolation::CubicCatmullRom));
    auto p = render_both(*v);
    CHECK(!p.gpu_used);
    CHECK(p.cpu == p.gpu);
    const Vec3 pts[3] = {vol->grid().index_to_patient({5, 5, 10}), vol->grid().index_to_patient({20, 25, 18}),
                         vol->grid().index_to_patient({40, 30, 25})};
    CHECK(v->set_curve(pts, true));
    CHECK(v->set_mode(ViewMode::CurvedStraightened, Interpolation::Linear));
    p = render_both(*v);
    CHECK(!p.gpu_used);
    CHECK(p.cpu == p.gpu);
}

// ---------------------------------------------------------------- MPR
// Grid planes of a volume with equal spacings put every output pixel on a voxel centre, so no
// sample is at a rounding tie of floor(c + 0.5) (with unequal in-plane spacings some rows fall
// exactly halfway between voxels, where double and float rounding pick different neighbours).
TEST_CASE("gpu/mpr: nearest is exact for int16 (grid planes and oblique), float32 within 1 level") {
    if (!gpu_present("mpr nearest")) return;
    for (int t = 0; t < 2; ++t) {
        const bool i16 = t == 0;
        for (int obl = 0; obl < 2; ++obl) {
            auto vol = ct_volume(i16 ? VoxelType::Int16 : VoxelType::Float32, obl == 1, {0.9, 0.9, 0.9});
            REQUIRE(vol);
            const Grid& g = vol->grid();
            for (int axis = 0; axis < 3; ++axis) {
                const std::uint32_t w = axis == 0 ? g.dims[1] : g.dims[0], h = axis == 2 ? g.dims[1] : g.dims[2];
                auto v = View::create(vol, w, h);
                REQUIRE(v);
                CHECK(v->set_plane(grid_plane(g, axis, axis == 2 ? 17.0 : 11.0)));
                CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Nearest));
                CHECK(v->set_window(0, 100.0, 1500.0));
                auto p = render_both(*v);
                CHECK(p.gpu_used);
                const Diff d = compare(p.cpu, p.gpu, i16 ? 0 : 1);
                char what[96];
                std::snprintf(what, sizeof what, "nearest %s %s grid axis %d", i16 ? "int16" : "float32", obl ? "oblique" : "axial", axis);
                report(what, d);
                CHECK(distinct(p.cpu) >= 16);
                CHECK(d.over == 0);
            }
            // An oblique plane (not through voxel centres) with a hot-iron map: same rule.
            auto v = View::create(vol, 96, 80);
            REQUIRE(v);
            Plane pl = standard_plane(g, StandardOrientation::Axial);
            pl.normal = {0.2, 0.3, 0.93};
            pl.up = {0.1, -1.0, 0.0};
            pl.pixel_spacing = 0.45;
            CHECK(v->set_plane(pl));
            CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Nearest));
            CHECK(v->set_colormap(0, ColorMap::HotIron));
            auto p = render_both(*v);
            const Diff d = compare(p.cpu, p.gpu, i16 ? 0 : 1);
            report(i16 ? "nearest int16 oblique plane, hot iron" : "nearest float32 oblique plane, hot iron", d);
            CHECK(p.gpu_used);
            // Oblique sample points are computed in float on the GPU: a point within ~1e-5 voxel of
            // a rounding tie may pick the neighbouring voxel. Allow a handful of pixels.
            CHECK(d.over <= 8);
        }
    }
}

TEST_CASE("gpu/mpr: trilinear (hardware filter and 8-fetch path) within tolerance") {
    if (!gpu_present("mpr trilinear")) return;
    for (int t = 0; t < 2; ++t) {
        auto vol = ct_volume(t == 0 ? VoxelType::Int16 : VoxelType::Float32, true);
        REQUIRE(vol);
        auto v = View::create(vol, 128, 112);
        REQUIRE(v);
        Plane pl = standard_plane(vol->grid(), StandardOrientation::Coronal);
        pl.normal = {0.1, 0.95, 0.3};
        pl.pixel_spacing = 0.33;
        CHECK(v->set_plane(pl));
        CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Linear));
        CHECK(v->set_window(0, 200.0, 1200.0));
        for (int manual = 0; manual < 2; ++manual) {
            gpu::testing::force_manual_filtering(manual == 1);
            auto p = render_both(*v);
            char what[96];
            std::snprintf(what, sizeof what, "trilinear %s %s", t == 0 ? "int16" : "float32", manual ? "8-fetch" : "hardware");
            if (manual) expect_close(what, p, 1, 0.01, 0.002, 2);
            else expect_close(what, p, 4, 0.5, 0.01, 12);
        }
        gpu::testing::force_manual_filtering(false);
    }
}

TEST_CASE("gpu/mpr: thick slab MIP, MinIP and average") {
    if (!gpu_present("slab")) return;
    auto vol = ct_volume(VoxelType::Int16, true);
    REQUIRE(vol);
    auto v = View::create(vol, 96, 96);
    REQUIRE(v);
    Plane pl = standard_plane(vol->grid(), StandardOrientation::Sagittal);
    pl.pixel_spacing = 0.5;
    CHECK(v->set_plane(pl));
    const SlabMode modes[3] = {SlabMode::Mip, SlabMode::MinIp, SlabMode::Average};
    const char* names[3] = {"slab MIP 12 mm", "slab MinIP 12 mm", "slab average 12 mm"};
    for (int m = 0; m < 3; ++m)
        for (int interp = 0; interp < 2; ++interp) {
            CHECK(v->set_mode(ViewMode::Mpr, interp ? Interpolation::Linear : Interpolation::Nearest, modes[m], 12.0));
            auto p = render_both(*v);
            char what[96];
            std::snprintf(what, sizeof what, "%s %s", names[m], interp ? "trilinear" : "nearest");
            if (interp) expect_close(what, p, 4, 0.5, 0.01, 12);
            else if (m == 2) expect_close(what, p, 1, 0.05, 0.002, 2);  // float sum order
            else expect_close(what, p, 0, 0.01, 0.001, 255);
        }
}

TEST_CASE("gpu/mpr: two-layer fusion (on the fly and pre-resampled)") {
    if (!gpu_present("fusion")) return;
    auto ct = ct_volume(VoxelType::Int16, false);
    const std::array<std::uint32_t, 3> pd{24, 22, 18};
    auto pet = make_volume(pd, {1.9, 1.9, 3.1}, {-24, -18, 28}, rotated_axes(-0.15, 0.1), VoxelType::Float32,
                           [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
                               const double x = i - 12.0, y = j - 11.0, z = k - 9.0;
                               return static_cast<float>(12.0 * std::exp(-(x * x + y * y + z * z) / 30.0));
                           });
    REQUIRE(ct);
    REQUIRE(pet);
    for (int pre = 0; pre < 2; ++pre) {
        auto v = View::create(ct, 100, 90);
        REQUIRE(v);
        Plane pl = standard_plane(ct->grid(), StandardOrientation::Axial);
        pl.pixel_spacing = 0.5;
        CHECK(v->set_plane(pl));
        CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Linear));
        LayerStyle st;
        st.colormap = ColorMap::Pet;
        st.window_center = 5.0;
        st.window_width = 8.0;
        st.opacity = 0.45;
        CHECK(v->set_fusion_layer(pet, st, pre == 1));
        auto p = render_both(*v);
        expect_close(pre ? "fusion PET over CT, pre-resampled" : "fusion PET over CT, on the fly", p, 6, 0.6, 0.02, 40);
        CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Nearest));
        p = render_both(*v);
        // Nearest: the int16 base is exact; the float overlay's window is evaluated in float.
        expect_close(pre ? "fusion nearest, pre-resampled" : "fusion nearest, on the fly", p, 1, 0.05, 0.002, 3);
    }
}

// ---------------------------------------------------------------- volume rendering
TEST_CASE("gpu/vr: ray-cast MIP and MinIP (skipping on and off)") {
    if (!gpu_present("vr mip")) return;
    for (int t = 0; t < 2; ++t) {
        auto vol = ct_volume(t == 0 ? VoxelType::Int16 : VoxelType::Float32, true);
        REQUIRE(vol);
        auto v = View::create(vol, 80, 72);
        REQUIRE(v);
        Plane pl = standard_plane(vol->grid(), StandardOrientation::Coronal);
        pl.normal = {0.3, 0.9, 0.2};
        CHECK(v->set_plane(pl));
        CHECK(v->set_window(0, 200.0, 2400.0));
        for (int mode = 0; mode < 2; ++mode)
            for (int skip = 0; skip < 2; ++skip) {
                RenderSettings rs;
                rs.empty_space_skipping = skip == 1;
                CHECK(v->set_render_settings(rs));
                CHECK(v->set_mode(mode == 0 ? ViewMode::VolumeMip : ViewMode::VolumeMinIp));
                auto p = render_both(*v);
                char what[96];
                std::snprintf(what, sizeof what, "%s %s skip %s", mode == 0 ? "VR MIP" : "VR MinIP", t == 0 ? "int16" : "float32",
                              skip ? "on" : "off");
                expect_close(what, p, 3, 0.4, 0.01, 30);
            }
    }
}

TEST_CASE("gpu/vr: composite with transfer function, shading, gradient opacity, quality") {
    if (!gpu_present("vr composite")) return;
    auto vol = ct_volume(VoxelType::Int16, true);
    REQUIRE(vol);
    auto v = View::create(vol, 96, 80);
    REQUIRE(v);
    Plane pl = standard_plane(vol->grid(), StandardOrientation::Coronal);
    pl.normal = {0.25, 0.9, -0.3};
    CHECK(v->set_plane(pl));
    TransferFunction tf;
    tf.opacity = {{-100.0, 0.0}, {100.0, 0.05}, {600.0, 0.3}, {1300.0, 0.9}};
    tf.color = {{-100.0, 0.8, 0.4, 0.3}, {500.0, 1.0, 0.9, 0.8}, {1300.0, 1.0, 1.0, 1.0}};
    for (int shading = 0; shading < 2; ++shading)
        for (int gop = 0; gop < 2; ++gop) {
            tf.shading = shading == 1;
            tf.gradient_opacity.clear();
            if (gop) tf.gradient_opacity = {{0.0, 0.2}, {50.0, 1.0}};
            CHECK(v->set_transfer_function(tf));
            for (int skip = 0; skip < 2; ++skip) {
                RenderSettings rs;
                rs.empty_space_skipping = skip == 1;
                rs.background = {20, 30, 40};
                CHECK(v->set_render_settings(rs));
                CHECK(v->set_mode(ViewMode::VolumeRender));
                v->set_quality(Quality::Final);
                auto p = render_both(*v);
                char what[96];
                std::snprintf(what, sizeof what, "VR composite shading %d gradient-opacity %d skip %d", shading, gop, skip);
                expect_close(what, p, 4, 0.6, 0.02, 40);
            }
        }
    // Interactive quality (half resolution + upsampling, double step) and early termination.
    RenderSettings rs;
    rs.early_termination = 0.9;
    CHECK(v->set_render_settings(rs));
    v->set_quality(Quality::Interactive);
    auto p = render_both(*v);
    expect_close("VR composite interactive, early termination 0.9", p, 4, 0.6, 0.02, 40);
    // Skipping must not change the GPU image beyond float noise (bricks only skip empty space).
    v->set_quality(Quality::Final);
    std::vector<std::uint8_t> on(std::size_t{96} * 80 * 4), off(on.size());
    CHECK(v->set_backend(RenderBackend::Gpu));
    rs.empty_space_skipping = true;
    CHECK(v->set_render_settings(rs));
    CHECK(v->render_rgba8(on));
    rs.empty_space_skipping = false;
    CHECK(v->set_render_settings(rs));
    CHECK(v->render_rgba8(off));
    const Diff d = compare(on, off, 1);
    report("GPU composite skipping on vs off", d);
    CHECK(d.over <= on.size() / 500);
}


TEST_CASE("gpu/state: uploads in chunks, window / colour-map / plane / layer changes, volume swaps") {
    if (!gpu_present("state changes")) return;
    gpu::testing::reset();
    gpu::testing::set_upload_chunk_bytes(3 * 48 * 44 * 2 + 10);  // 3 int16 slices per step (12 steps)
    auto vol = ct_volume(VoxelType::Int16, true);
    auto vol2 = ct_volume(VoxelType::Float32, false);
    REQUIRE(vol);
    REQUIRE(vol2);
    auto v = View::create(vol, 72, 64);
    REQUIRE(v);
    CHECK(v->set_mode(ViewMode::Mpr, Interpolation::Nearest));
    auto p = render_both(*v);
    expect_close("chunked upload, nearest", p, 0, 0.0, 0.0, 0);
    gpu::testing::set_upload_chunk_bytes(0);
    // Every change must reach the GPU image (no stale tables or textures).
    CHECK(v->set_window(0, -300.0, 900.0));
    p = render_both(*v);
    expect_close("after a window change", p, 0, 0.0, 0.0, 0);
    CHECK(v->set_colormap(0, ColorMap::InverseGray));
    p = render_both(*v);
    expect_close("after a colour-map change", p, 0, 0.0, 0.0, 0);
    Plane pl = standard_plane(vol->grid(), StandardOrientation::Sagittal);
    pl.pixel_spacing = 0.6;
    CHECK(v->set_plane(pl));
    p = render_both(*v);
    expect_close("after a plane change", p, 0, 0.0, 0.0, 0);
    RenderSettings rs;
    rs.background = {200, 10, 90};
    CHECK(v->set_render_settings(rs));
    p = render_both(*v);
    expect_close("background colour", p, 0, 0.0, 0.0, 0);
    LayerStyle st;
    st.colormap = ColorMap::HotMetalBlue;
    st.opacity = 0.7;
    st.transparent_below_window = false;
    CHECK(v->set_fusion_layer(vol2, st));
    p = render_both(*v);
    expect_close("fusion, opaque below window", p, 1, 0.05, 0.002, 3);
    CHECK(v->set_fusion_layer(nullptr, st));
    p = render_both(*v);
    expect_close("fusion removed", p, 0, 0.0, 0.0, 0);
    // Another volume in a new view, then MIP at interactive quality.
    auto v2 = View::create(vol2, 50, 70);
    REQUIRE(v2);
    CHECK(v2->set_mode(ViewMode::VolumeMip));
    v2->set_quality(Quality::Interactive);
    p = render_both(*v2);
    expect_close("float32 volume, VR MIP interactive", p, 3, 0.4, 0.01, 30);
    CHECK(v2->set_size(33, 17));
    p = render_both(*v2);
    expect_close("after a resize (odd sizes)", p, 3, 0.4, 0.01, 30);
}

// ---------------------------------------------------------------- lifetime, sharing, threads
TEST_CASE("gpu/resources: views share uploads; everything is released") {
    if (!gpu_present("resources")) return;
    gpu::testing::reset();
    REQUIRE(gpu::acquire() != nullptr);
    const std::int64_t base = gpu::testing::live_objects();
    {
        auto vol = ct_volume(VoxelType::Int16, false);
        REQUIRE(vol);
        std::vector<View> views;
        for (int i = 0; i < 3; ++i) {
            auto v = View::create(vol, 32 + 8 * static_cast<std::uint32_t>(i), 32);
            REQUIRE(v);
            views.push_back(std::move(v).value());
        }
        std::vector<std::uint8_t> buf(64 * 32 * 4);
        for (auto& v : views) {
            CHECK(v.set_backend(RenderBackend::Gpu));
            CHECK(v.render_rgba8(std::span<std::uint8_t>(buf.data(), std::size_t{v.width()} * v.height() * 4u)));
            CHECK(v.last_backend() == RenderBackend::Gpu);
        }
        const std::int64_t with_one_texture = gpu::testing::live_objects();
        // One texture (image, memory, view) shared by the three views, plus per-view sessions.
        CHECK(with_one_texture > base);
        // Two views rendering at the same time from two threads.
        std::thread t1([&] {
            for (int i = 0; i < 5; ++i) CHECK(views[0].render_rgba8(std::span<std::uint8_t>(buf.data(), 32u * 32u * 4u)));
        });
        std::vector<std::uint8_t> buf2(buf.size());
        for (int i = 0; i < 5; ++i) CHECK(views[1].render_rgba8(std::span<std::uint8_t>(buf2.data(), 40u * 32u * 4u)));
        t1.join();
        // Resizing re-creates the output image.
        CHECK(views[2].set_size(200, 150));
        std::vector<std::uint8_t> big(200 * 150 * 4);
        CHECK(views[2].render_rgba8(big));
        CHECK(views[2].last_backend() == RenderBackend::Gpu);
    }
    CHECK(gpu::testing::live_objects() == base);
    gpu::testing::reset();
    CHECK(gpu::testing::live_objects() == 0);
}

// ---------------------------------------------------------------- C API
TEST_CASE("gpu/capi: device list and view backend") {
    uint32_t count = 99;
    CHECK(siaara_gpu_devices(nullptr, 0, &count) == SIAARA_OK);
    CHECK(count == gpu::devices().size());
    std::vector<siaara_gpu_device_info> infos(count + 1);
    for (auto& i : infos) i.struct_size = sizeof(siaara_gpu_device_info);
    uint32_t n2 = 0;
    CHECK(siaara_gpu_devices(infos.data(), static_cast<uint32_t>(infos.size()), &n2) == SIAARA_OK);
    CHECK(n2 == count);
    int selected = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(infos[i].name[0] != '\0');
        selected += infos[i].selected;
    }
    CHECK(selected == (count ? 1 : 0));
    infos[0].struct_size = 4;
    CHECK(siaara_gpu_devices(infos.data(), 1, &n2) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_gpu_devices(nullptr, 1, &n2) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_view_set_backend(nullptr, SIAARA_BACKEND_AUTO) == SIAARA_ERR_INVALID_ARGUMENT);
    int32_t last = -1;
    CHECK(siaara_view_last_backend(nullptr, &last) == SIAARA_ERR_INVALID_ARGUMENT);
}
