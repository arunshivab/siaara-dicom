// GPU vs CPU rendering benchmark for the 3D views (test-only; not run by ctest).
//
//   gpu_bench [--size N] [--frames F] [--out W]
//
// Builds a synthetic int16 CT-like volume of N x N x (N * 3 / 4) voxels (default 256), renders
// W x W views (default 512) in each mode on the CPU renderer and on the GPU backend (backend Gpu,
// so a software device such as Mesa's lavapipe is used when it is the only one), and reports the
// first frame (GPU: includes the volume upload and pipeline warm-up) and the median of F frames.
// The maximum per-channel difference between the CPU and GPU images is reported as a sanity check.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gpu/gpu.hpp"
#include "volume/render3d.hpp"
#include "volume/volume.hpp"

using namespace siaara;
using namespace siaara::volume;

namespace {

double now_ms() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(C::now().time_since_epoch()).count();
}

std::shared_ptr<Volume> make_ct(std::uint32_t n) {
    Grid g;
    g.dims = {n, n, n * 3 / 4};
    g.spacing = {0.7, 0.7, 1.0};
    g.origin = {-90, -90, -100};
    auto r = Volume::allocate(g, VoxelType::Int16);
    if (!r) return nullptr;
    auto v = std::make_shared<Volume>(std::move(r).value());
    const double cx = n / 2.0, cz = g.dims[2] / 2.0;
    std::int16_t* p = v->i16();
    for (std::uint32_t k = 0; k < g.dims[2]; ++k)
        for (std::uint32_t j = 0; j < n; ++j)
            for (std::uint32_t i = 0; i < n; ++i) {
                const double x = (i - cx) / n, y = (j - cx) / n, z = (k - cz) / g.dims[2];
                const double r2 = x * x + y * y;
                double val = -1000.0;
                if (r2 < 0.16) val = 30.0 + 20.0 * std::sin(20.0 * x + 7.0 * z);       // soft tissue
                if (r2 < 0.16 && std::fabs(std::sqrt(r2) - 0.33) < 0.03) val = 900.0;  // "ribs"
                if ((x - 0.1) * (x - 0.1) + y * y + z * z < 0.01) val = 1500.0;        // dense ball
                if (r2 < 0.16 && std::fabs(x) < 0.02 && std::fabs(y + 0.2) < 0.03) val = 1200.0;  // "spine"
                *p++ = static_cast<std::int16_t>(val);
            }
    (void)v->update_statistics();
    return v;
}

struct Timing {
    double first = 0.0, median = 0.0;
    bool ok = false;
};

Timing run(View& v, std::vector<std::uint8_t>& buf, int frames) {
    Timing t;
    double a = now_ms();
    if (!v.render_rgba8(buf)) return t;
    t.first = now_ms() - a;
    std::vector<double> ms;
    for (int i = 0; i < frames; ++i) {
        a = now_ms();
        if (!v.render_rgba8(buf)) return t;
        ms.push_back(now_ms() - a);
    }
    std::sort(ms.begin(), ms.end());
    t.median = ms.empty() ? t.first : ms[ms.size() / 2];
    t.ok = true;
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t n = 256, out = 512;
    int frames = 5;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--size") == 0) n = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--out") == 0) out = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    }
    if (n < 16 || out < 16 || frames < 1) {
        std::fprintf(stderr, "usage: gpu_bench [--size N>=16] [--frames F>=1] [--out W>=16]\n");
        return 2;
    }
    auto vol = make_ct(n);
    if (!vol) return 1;
    const auto devs = gpu::devices();
    std::printf("volume %ux%ux%u int16 (%.1f MiB), output %ux%u, %d frames\n", vol->grid().dims[0], vol->grid().dims[1],
                vol->grid().dims[2], static_cast<double>(vol->byte_size()) / 1048576.0, out, out, frames);
    for (const auto& d : devs)
        std::printf("device: %s (%s)%s\n", d.name.c_str(), d.driver.c_str(), d.selected ? " [selected]" : "");
    if (devs.empty()) std::printf("no GPU device: %s\n", gpu::unavailable_reason().c_str());

    struct Mode {
        const char* name;
        ViewMode mode;
        Interpolation interp;
        SlabMode slab;
        double thick;
        Quality q;
    };
    const Mode modes[] = {
        {"MPR nearest", ViewMode::Mpr, Interpolation::Nearest, SlabMode::None, 0.0, Quality::Final},
        {"MPR trilinear", ViewMode::Mpr, Interpolation::Linear, SlabMode::None, 0.0, Quality::Final},
        {"slab MIP 20 mm", ViewMode::Mpr, Interpolation::Linear, SlabMode::Mip, 20.0, Quality::Final},
        {"VR MIP", ViewMode::VolumeMip, Interpolation::Linear, SlabMode::None, 0.0, Quality::Final},
        {"VR composite shaded (final)", ViewMode::VolumeRender, Interpolation::Linear, SlabMode::None, 0.0, Quality::Final},
        {"VR composite shaded (interactive)", ViewMode::VolumeRender, Interpolation::Linear, SlabMode::None, 0.0, Quality::Interactive},
    };
    std::printf("\n%-36s %12s %12s %12s %12s %8s %9s %10s\n", "mode", "CPU first", "CPU median", "GPU first", "GPU median", "maxdiff",
                "meandiff", ">2 levels");
    for (const Mode& m : modes) {
        auto vr = View::create(vol, out, out);
        if (!vr) return 1;
        View& v = *vr;
        Plane pl = standard_plane(vol->grid(), m.mode == ViewMode::Mpr ? StandardOrientation::Axial : StandardOrientation::Coronal);
        if (m.mode != ViewMode::Mpr) pl.normal = {0.3, 0.95, 0.1};
        (void)v.set_plane(pl);
        (void)v.set_window(0, 300.0, 1600.0);
        if (!v.set_mode(m.mode, m.interp, m.slab, m.thick)) return 1;
        v.set_quality(m.q);
        if (m.mode == ViewMode::VolumeRender) {
            TransferFunction tf;
            tf.opacity = {{-200.0, 0.0}, {100.0, 0.02}, {800.0, 0.4}, {1500.0, 0.95}};
            tf.color = {{-200.0, 0.8, 0.5, 0.4}, {800.0, 1.0, 0.95, 0.9}};
            tf.shading = true;
            (void)v.set_transfer_function(tf);
        }
        std::vector<std::uint8_t> a(std::size_t{out} * out * 4), b(a.size());
        (void)v.set_backend(RenderBackend::Cpu);
        const Timing c = run(v, a, frames);
        Timing g;
        int maxdiff = -1;
        double mean = 0.0;
        std::size_t over2 = 0;
        if (v.set_backend(RenderBackend::Gpu)) {
            g = run(v, b, frames);
            if (v.last_backend() != RenderBackend::Gpu) g.ok = false;
            if (g.ok) {
                maxdiff = 0;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    const int e = std::abs(int(a[i]) - int(b[i]));
                    maxdiff = std::max(maxdiff, e);
                    mean += e;
                    if (e > 2) ++over2;
                }
                mean /= static_cast<double>(a.size());
            }
        }
        if (g.ok)
            std::printf("%-36s %9.1f ms %9.1f ms %9.1f ms %9.1f ms %8d %9.4f %9.4f%%\n", m.name, c.first, c.median, g.first, g.median,
                        maxdiff, mean, 100.0 * static_cast<double>(over2) / static_cast<double>(a.size()));
        else
            std::printf("%-36s %9.1f ms %9.1f ms %12s %12s %8s\n", m.name, c.first, c.median, "-", "-", "-");
    }
    return 0;
}
