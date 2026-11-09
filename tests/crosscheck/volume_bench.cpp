// Test-only benchmark for the volume layer (not run by ctest).
//   volume_bench <folder> [slices=1000] [threads=0]
// Writes a synthetic 512 x 512 x N CT series (native explicit VR little endian, int16) into
// <folder> once, then times: volume building from the files (page cache dropped for those files
// first with posix_fadvise(DONTNEED) on Linux - "cold" as far as an unprivileged process can make
// it; also a warm run), MPR 512 x 512 (axial, coronal, oblique; linear; slab MIP), and ray casting
// 512 x 512 (interactive and final, composite with shading, and MIP).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/writer.hpp"
#include "volume/render3d.hpp"
#include "volume/volume.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace siaara;
using namespace siaara::volume;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

void drop_cache(const std::vector<std::string>& paths) {
#if defined(__linux__)
    for (const auto& p : paths) {
        const int fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) continue;
        (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
#else
    (void)paths;
#endif
}

std::vector<std::string> make_series(const std::filesystem::path& dir, int n) {
    std::filesystem::create_directories(dir);
    std::vector<std::string> paths;
    std::vector<std::uint8_t> px(512 * 512 * 2);
    for (int k = 0; k < n; ++k) {
        const auto path = (dir / ("ct_" + std::to_string(k) + ".dcm")).string();
        paths.push_back(path);
        if (std::filesystem::exists(path)) continue;
        // Body ellipse (soft tissue ~ 40 HU + texture), a spine-like bone disc, air outside.
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x) {
                const double ex = (x - 256) / 200.0, ey = (y - 256) / 150.0;
                int hu = -1000;
                if (ex * ex + ey * ey < 1.0) hu = 40 + static_cast<int>(30 * std::sin(x * 0.05 + k * 0.03) * std::cos(y * 0.07));
                const double bx = x - 256, by = y - 330;
                if (bx * bx + by * by < 900 + 400 * std::sin(k * 0.1)) hu = 700 + (x + y + k) % 300;
                const auto stored = static_cast<std::uint16_t>(static_cast<std::int16_t>(hu + 1024));
                const auto at = static_cast<std::size_t>(2 * (y * 512 + x));
                px[at] = static_cast<std::uint8_t>(stored & 0xFF);
                px[at + 1] = static_cast<std::uint8_t>(stored >> 8);
            }
        DicomFile f;
        f.transfer_syntax = "1.2.840.10008.1.2.1";
        Dataset& ds = f.dataset;
        (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
        (void)ds.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25.4242" + std::to_string(k));
        (void)ds.set_string(siaara::tags::Modality, VR::CS, "CT");
        (void)ds.set_uint16(siaara::tags::Rows, 512);
        (void)ds.set_uint16(siaara::tags::Columns, 512);
        (void)ds.set_uint16(siaara::tags::SamplesPerPixel, 1);
        (void)ds.set_uint16(siaara::tags::BitsAllocated, 16);
        (void)ds.set_uint16(siaara::tags::BitsStored, 16);
        (void)ds.set_uint16(siaara::tags::HighBit, 15);
        (void)ds.set_uint16(siaara::tags::PixelRepresentation, 1);
        (void)ds.set_string(siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
        char ipp[96];
        std::snprintf(ipp, sizeof ipp, "-200\\-200\\%.3f", -500.0 + 0.625 * k);
        (void)ds.set_string(siaara::volume::tags::ImagePositionPatient, VR::DS, ipp);
        (void)ds.set_string(siaara::volume::tags::ImageOrientationPatient, VR::DS, "1\\0\\0\\0\\1\\0");
        (void)ds.set_string(siaara::volume::tags::PixelSpacing, VR::DS, "0.78125\\0.78125");
        (void)ds.set_string(siaara::volume::tags::InstanceNumber, VR::IS, std::to_string(k + 1));
        (void)ds.set_string(siaara::tags::RescaleSlope, VR::DS, "1");
        (void)ds.set_string(siaara::tags::RescaleIntercept, VR::DS, "-1024");
        (void)ds.set_bytes(siaara::tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
        if (Status s = write_file(f, path); !s) {
            std::fprintf(stderr, "write failed: %s\n", s.message.c_str());
            std::exit(1);
        }
    }
    return paths;
}

template <class F>
double median_ms(int runs, F&& f) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = Clock::now();
        f();
        t.push_back(ms_since(t0));
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: volume_bench <folder> [slices] [threads]\n");
        return 2;
    }
    const int n = argc > 2 ? std::atoi(argv[2]) : 1000;
    const unsigned threads = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 0u;
    std::printf("hardware threads: %u, using %u\n", std::thread::hardware_concurrency(), threads);
    auto t0 = Clock::now();
    const auto paths = make_series(argv[1], n);
    std::printf("series ready (%d files) in %.0f ms\n", n, ms_since(t0));

    VolumeOptions vo;
    vo.threads = threads;
    drop_cache(paths);
    t0 = Clock::now();
    auto v = build_volume_from_paths(paths, vo);
    const double cold = ms_since(t0);
    if (!v) {
        std::fprintf(stderr, "build failed: %s\n", v.error().message.c_str());
        return 1;
    }
    t0 = Clock::now();
    auto v2 = build_volume_from_paths(paths, vo);
    const double warm = ms_since(t0);
    std::printf("build 512x512x%d: cold(fadvise) %.0f ms, warm %.0f ms, type %s, range %.0f..%.0f\n", n, cold, warm,
                v->type() == VoxelType::Int16 ? "int16" : "float32", v->value_min, v->value_max);
    v2 = Status(Errc::Ok, "");
    auto vol = std::make_shared<const Volume>(std::move(v).value());

    auto view = View::create(vol, 512, 512);
    if (!view) return 1;
    RenderSettings rs;
    rs.threads = threads;
    (void)view->set_render_settings(rs);
    std::vector<std::uint8_t> img(512 * 512 * 4);
    (void)view->set_window(0, 40, 400);
    struct P {
        const char* name;
        Plane p;
    };
    Plane ax = standard_plane(vol->grid(), StandardOrientation::Axial);
    Plane co = standard_plane(vol->grid(), StandardOrientation::Coronal);
    Plane ob = ax;
    ob.normal = {0.3, -0.4, 0.87};
    ob.up = {0.1, -1, 0.3};
    for (const P& p : {P{"axial", ax}, P{"coronal", co}, P{"oblique", ob}}) {
        (void)view->set_plane(p.p);
        for (Interpolation m : {Interpolation::Nearest, Interpolation::Linear, Interpolation::CubicCatmullRom}) {
            (void)view->set_mode(ViewMode::Mpr, m);
            const double t = median_ms(15, [&] { (void)view->render_rgba8(img); });
            std::printf("MPR 512x512 %-8s %-8s %7.2f ms\n", p.name,
                        m == Interpolation::Nearest ? "nearest" : (m == Interpolation::Linear ? "linear" : "cubic"), t);
        }
        (void)view->set_mode(ViewMode::Mpr, Interpolation::Linear, SlabMode::Mip, 10.0);
        std::printf("MPR 512x512 %-8s slab MIP 10 mm (linear) %7.2f ms\n", p.name, median_ms(7, [&] { (void)view->render_rgba8(img); }));
    }
    TransferFunction tf;
    tf.opacity = {{150, 0.0}, {300, 0.15}, {700, 0.6}, {1500, 0.9}};
    tf.color = {{150, 0.8, 0.4, 0.3}, {700, 1.0, 0.95, 0.85}};
    tf.shading = true;
    t0 = Clock::now();
    (void)view->set_transfer_function(tf);
    (void)view->set_plane(co);
    (void)view->set_mode(ViewMode::VolumeRender);
    std::printf("VR prepare (brick grid + TF) %.0f ms\n", ms_since(t0));
    for (Quality q : {Quality::Interactive, Quality::Final}) {
        view->set_quality(q);
        const double t = median_ms(5, [&] { (void)view->render_rgba8(img); });
        std::printf("VR 512x512 composite+shading %-11s %8.1f ms\n", q == Quality::Interactive ? "interactive" : "final", t);
    }
    Plane obv = co;
    obv.normal = {0.4, 0.85, -0.3};
    (void)view->set_plane(obv);
    view->set_quality(Quality::Interactive);
    std::printf("VR 512x512 composite oblique interactive %8.1f ms\n", median_ms(5, [&] { (void)view->render_rgba8(img); }));
    (void)view->set_plane(co);
    (void)view->set_mode(ViewMode::VolumeMip);
    for (Quality q : {Quality::Interactive, Quality::Final}) {
        view->set_quality(q);
        std::printf("VR 512x512 MIP %-11s %8.1f ms\n", q == Quality::Interactive ? "interactive" : "final",
                    median_ms(5, [&] { (void)view->render_rgba8(img); }));
    }
    return 0;
}
