// Test-only helper for tests/crosscheck/volume_crosscheck.py (not installed).
//   volume_tool build <out_prefix> [--deshear] file...    -> <out_prefix>.raw (float32) + .txt (geometry)
//   volume_tool resample <in.raw> <in.txt> <target.txt> <interp> <out.raw>
//   volume_tool mpr <in.raw> <in.txt> <view.txt> <interp> <out.raw>
// Geometry text files: "dims nx ny nz", "spacing a b c", "origin x y z", "axes 9 values" (axis i,
// then j, then k), plus "flags", "phases" and "tilt" on output. View files: "size w h",
// "point x y z", "normal x y z", "up x y z", "spacing s", "slab mode thickness" (optional).
// interp: nearest | linear | cubic (Catmull-Rom) | bspline. Outside samples are written as NaN.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "volume/render3d.hpp"
#include "volume/resample.hpp"
#include "volume/volume.hpp"

using namespace siaara;
using namespace siaara::volume;

namespace {

std::map<std::string, std::vector<double>> read_kv(const char* path) {
    std::map<std::string, std::vector<double>> m;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        std::string tok;
        while (ss >> tok) m[key].push_back(std::strtod(tok.c_str(), nullptr));
    }
    return m;
}

bool read_grid(const char* path, Grid& g) {
    auto m = read_kv(path);
    if (m["dims"].size() != 3 || m["spacing"].size() != 3 || m["origin"].size() != 3 || m["axes"].size() != 9) return false;
    for (std::size_t a = 0; a < 3; ++a) {
        g.dims[a] = static_cast<std::uint32_t>(m["dims"][a]);
        g.spacing[a] = m["spacing"][a];
        g.axes[a] = {m["axes"][3 * a], m["axes"][3 * a + 1], m["axes"][3 * a + 2]};
    }
    g.origin = {m["origin"][0], m["origin"][1], m["origin"][2]};
    return true;
}

void write_grid(const char* path, const Volume& v) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    const Grid& g = v.grid();
    std::fprintf(f, "dims %u %u %u\n", g.dims[0], g.dims[1], g.dims[2]);
    std::fprintf(f, "spacing %.17g %.17g %.17g\n", g.spacing[0], g.spacing[1], g.spacing[2]);
    std::fprintf(f, "origin %.17g %.17g %.17g\n", g.origin.x, g.origin.y, g.origin.z);
    std::fprintf(f, "axes");
    for (const auto& a : g.axes) std::fprintf(f, " %.17g %.17g %.17g", a.x, a.y, a.z);
    std::fprintf(f, "\nflags %u\nphases %u\ntilt %.17g\ntype %d\n", v.flags, v.phase_count, v.tilt_degrees,
                 v.type() == VoxelType::Int16 ? 16 : 32);
    std::fclose(f);
}

bool write_floats(const char* path, const float* p, std::size_t n) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(p, sizeof(float), n, f) == n;
    std::fclose(f);
    return ok;
}

std::shared_ptr<Volume> load(const char* raw, const char* txt) {
    Grid g;
    if (!read_grid(txt, g)) return nullptr;
    auto r = Volume::allocate(g, VoxelType::Float32);
    if (!r) return nullptr;
    auto v = std::make_shared<Volume>(std::move(r).value());
    std::FILE* f = std::fopen(raw, "rb");
    if (!f) return nullptr;
    const auto n = static_cast<std::size_t>(v->voxel_count());
    const bool ok = std::fread(v->f32(), sizeof(float), n, f) == n;
    std::fclose(f);
    if (!ok) return nullptr;
    (void)v->update_statistics();
    return v;
}

bool parse_interp(const char* s, Interpolation& m) {
    const std::string t = s;
    if (t == "nearest") m = Interpolation::Nearest;
    else if (t == "linear") m = Interpolation::Linear;
    else if (t == "cubic") m = Interpolation::CubicCatmullRom;
    else if (t == "bspline") m = Interpolation::CubicBSpline;
    else return false;
    return true;
}

int fail(const char* what, const Status& s) {
    std::fprintf(stderr, "%s: %s\n", what, s.message.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: volume_tool build|resample|mpr ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "build") {
        VolumeOptions o;
        o.type = VoxelTypeRequest::Float32;
        std::vector<std::string> paths;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--deshear") == 0) o.deshear_tilt = true;
            else paths.emplace_back(argv[i]);
        }
        auto v = build_volume_from_paths(paths, o);
        if (!v) return fail("build", v.error());
        const std::string prefix = argv[2];
        write_grid((prefix + ".txt").c_str(), *v);
        return write_floats((prefix + ".raw").c_str(), v->f32(), static_cast<std::size_t>(v->voxel_count())) ? 0 : 1;
    }
    if ((cmd == "resample" || cmd == "mpr") && argc == 7) {
        auto src = load(argv[2], argv[3]);
        if (!src) {
            std::fprintf(stderr, "cannot load input\n");
            return 1;
        }
        Interpolation m{};
        if (!parse_interp(argv[5], m)) return 2;
        if (cmd == "resample") {
            Grid t;
            if (!read_grid(argv[4], t)) return 2;
            ResampleOptions ro;
            ro.interpolation = m;
            ro.outside_value = std::numeric_limits<double>::quiet_NaN();
            auto r = resample(*src, t, ro);
            if (!r) return fail("resample", r.error());
            return write_floats(argv[6], r->f32(), static_cast<std::size_t>(r->voxel_count())) ? 0 : 1;
        }
        auto kv = read_kv(argv[4]);
        auto view = View::create(src, static_cast<std::uint32_t>(kv["size"].at(0)), static_cast<std::uint32_t>(kv["size"].at(1)));
        if (!view) return fail("view", view.error());
        Plane p;
        p.point = {kv["point"].at(0), kv["point"].at(1), kv["point"].at(2)};
        p.normal = {kv["normal"].at(0), kv["normal"].at(1), kv["normal"].at(2)};
        p.up = {kv["up"].at(0), kv["up"].at(1), kv["up"].at(2)};
        p.pixel_spacing = kv["spacing"].at(0);
        if (Status s = view->set_plane(p); !s) return fail("plane", s);
        SlabMode slab = SlabMode::None;
        double thick = 0;
        if (kv.count("slab")) {
            slab = static_cast<SlabMode>(static_cast<int>(kv["slab"].at(0)));
            thick = kv["slab"].at(1);
        }
        if (Status s = view->set_mode(ViewMode::Mpr, m, slab, thick); !s) return fail("mode", s);
        std::vector<float> out(static_cast<std::size_t>(view->width()) * view->height());
        if (Status s = view->render_values(out, std::numeric_limits<float>::quiet_NaN()); !s) return fail("render", s);
        return write_floats(argv[6], out.data(), out.size()) ? 0 : 1;
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
