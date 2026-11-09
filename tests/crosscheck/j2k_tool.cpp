// Test-only tool (never shipped): drives the SIAARA JPEG 2000 codec for
// tests/crosscheck/j2k_crosscheck.py, which compares it with independent decoders.
//
//   j2k_tool probe <in>
//       prints "width height components precision signed levels layers progression reversible
//       mct ht tiles subsampled"
//   j2k_tool components <in> <out.raw>
//       decodes every component at its own resolution; writes each as little-endian int32 samples
//       (component after component) and prints one line per component: "width height precision
//       signed dx dy"
//   j2k_tool frame <in> <out.raw> <rows> <cols> <spp> <bits_allocated> <bits_stored> <signed> [planar]
//       decodes through decode_j2k into the DICOM native layout
//   j2k_tool encode <in.raw> <out.j2k> <rows> <cols> <spp> <bits_allocated> <bits_stored> <signed>
//                   <lossless|lossy|ht> [quality] [layers] [levels] [tile] [progression 0..4]
//       input in native layout (colour-by-pixel)
//   j2k_tool bench <in> <iterations>
//       decodes repeatedly through decode_components; prints best and median milliseconds
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "codec/j2k.hpp"
#include "core/types.hpp"

using namespace siaara;
using namespace siaara::codec;

namespace {

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const char* path, const std::vector<std::uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

int fail(const Status& s) {
    std::fprintf(stderr, "error %d: %s (offset %llu)\n", static_cast<int>(s.code), s.message.c_str(),
                 static_cast<unsigned long long>(s.offset));
    return 1;
}

FrameInfo frame_from(char** a) {
    FrameInfo fi;
    fi.rows = static_cast<std::uint32_t>(std::strtoul(a[0], nullptr, 10));
    fi.columns = static_cast<std::uint32_t>(std::strtoul(a[1], nullptr, 10));
    fi.samples_per_pixel = static_cast<std::uint16_t>(std::strtoul(a[2], nullptr, 10));
    fi.bits_allocated = static_cast<std::uint16_t>(std::strtoul(a[3], nullptr, 10));
    fi.bits_stored = static_cast<std::uint16_t>(std::strtoul(a[4], nullptr, 10));
    fi.is_signed = std::atoi(a[5]) != 0;
    return fi;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: see the comment at the top of j2k_tool.cpp\n");
        return 2;
    }
    const std::string cmd = argv[1];
    std::vector<std::uint8_t> in;
    if (!read_file(argv[2], in)) return 2;
    if (cmd == "probe") {
        j2k::StreamInfo si;
        if (Status s = j2k::probe(in, si); !s) return fail(s);
        std::printf("%u %u %d %d %d %d %d %d %d %d %d %u %d\n", si.width, si.height, si.components, si.precision,
                    si.is_signed ? 1 : 0, si.levels, si.layers, si.progression, si.reversible ? 1 : 0, si.mct ? 1 : 0,
                    si.ht ? 1 : 0, si.tiles, si.subsampled ? 1 : 0);
        return 0;
    }
    if (cmd == "components" && argc >= 4) {
        std::vector<j2k::Component> comps;
        if (Status s = j2k::decode_components(in, comps); !s) return fail(s);
        std::vector<std::uint8_t> out;
        for (const auto& c : comps) {
            std::printf("%u %u %d %d %u %u\n", c.width, c.height, c.precision, c.is_signed ? 1 : 0, c.dx, c.dy);
            const std::size_t base = out.size();
            out.resize(base + c.samples.size() * 4);
            for (std::size_t i = 0; i < c.samples.size(); ++i)
                store_le32(&out[base + 4 * i], static_cast<std::uint32_t>(c.samples[i]));
        }
        return write_file(argv[3], out) ? 0 : 2;
    }
    if (cmd == "frame" && argc >= 10) {
        FrameInfo fi = frame_from(argv + 4);
        fi.planar = argc >= 11 && std::atoi(argv[10]) != 0;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(fi.frame_bytes()));
        if (Status s = decode_j2k(in, fi, out); !s) return fail(s);
        return write_file(argv[3], out) ? 0 : 2;
    }
    if (cmd == "encode" && argc >= 11) {
        const FrameInfo fi = frame_from(argv + 4);
        j2k::EncodeOptions o;
        const std::string mode = argv[10];
        o.lossless = mode != "lossy";
        o.ht = mode == "ht";
        if (argc >= 12) o.quality = std::atof(argv[11]);
        if (argc >= 13) o.layers = std::atoi(argv[12]);
        if (argc >= 14) o.levels = std::atoi(argv[13]);
        if (argc >= 15) o.tile_width = o.tile_height = static_cast<std::uint32_t>(std::atoi(argv[14]));
        if (argc >= 16) o.progression = static_cast<j2k::Progression>(std::atoi(argv[15]));
        std::vector<std::uint8_t> out;
        if (Status s = j2k::encode(in, fi, o, out); !s) return fail(s);
        return write_file(argv[3], out) ? 0 : 2;
    }
    if (cmd == "bench" && argc >= 4) {
        const int iters = std::max(1, std::atoi(argv[3]));
        std::vector<double> ms;
        for (int i = 0; i < iters; ++i) {
            std::vector<j2k::Component> comps;
            const auto t0 = std::chrono::steady_clock::now();
            if (Status s = j2k::decode_components(in, comps); !s) return fail(s);
            const auto t1 = std::chrono::steady_clock::now();
            ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        std::printf("best %.3f ms, median %.3f ms\n", ms.front(), ms[ms.size() / 2]);
        return 0;
    }
    std::fprintf(stderr, "usage: see the comment at the top of j2k_tool.cpp\n");
    return 2;
}
