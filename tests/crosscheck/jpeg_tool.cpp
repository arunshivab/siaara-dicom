// Test-only helper for tests/crosscheck/jpeg_crosscheck.py (never shipped).
//   jpeg_tool decode <in.jpg> <out.raw> rows cols spp bits_alloc bits_stored signed planar
//   jpeg_tool encode <in.raw> <out.jpg> rows cols spp bits_alloc bits_stored signed planar
//                    lossless predictor quality ybr luma_h luma_v restart point_transform
//   jpeg_tool bench  <in.jpg> rows cols spp bits_alloc bits_stored signed planar iterations
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/jpeg.hpp"

using namespace siaara::codec;

namespace {

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    out.clear();
    std::uint8_t buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

bool write_file(const char* path, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

long num(const char* s) { return std::strtol(s, nullptr, 10); }

FrameInfo parse_info(char** a) {
    FrameInfo fi;
    fi.rows = static_cast<std::uint32_t>(num(a[0]));
    fi.columns = static_cast<std::uint32_t>(num(a[1]));
    fi.samples_per_pixel = static_cast<std::uint16_t>(num(a[2]));
    fi.bits_allocated = static_cast<std::uint16_t>(num(a[3]));
    fi.bits_stored = static_cast<std::uint16_t>(num(a[4]));
    fi.is_signed = num(a[5]) != 0;
    fi.planar = num(a[6]) != 0;
    return fi;
}

int usage() {
    std::fprintf(stderr, "usage: jpeg_tool decode|encode|bench ... (see source)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string_view mode = argv[1];
    if (mode == "decode" && argc == 11) {
        std::vector<std::uint8_t> in;
        if (!read_file(argv[2], in)) return 3;
        const FrameInfo fi = parse_info(argv + 4);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(static_cast<std::size_t>(fi.frame_bytes())));
        const siaara::Status st = decode_jpeg(in, fi, out);
        if (!st) {
            std::fprintf(stderr, "decode failed: %s (offset %llu)\n", st.message.c_str(),
                         static_cast<unsigned long long>(st.offset));
            return 1;
        }
        return write_file(argv[3], out) ? 0 : 3;
    }
    if (mode == "encode" && argc == 19) {
        std::vector<std::uint8_t> in;
        if (!read_file(argv[2], in)) return 3;
        const FrameInfo fi = parse_info(argv + 4);
        EncodeParams p;
        p.lossless = num(argv[11]) != 0;
        p.predictor = static_cast<int>(num(argv[12]));
        p.quality = static_cast<int>(num(argv[13]));
        p.ybr = num(argv[14]) != 0;
        JpegEncodeOptions o;
        o.luma_h = static_cast<int>(num(argv[15]));
        o.luma_v = static_cast<int>(num(argv[16]));
        o.restart_interval = static_cast<std::uint16_t>(num(argv[17]));
        o.point_transform = static_cast<int>(num(argv[18]));
        std::vector<std::uint8_t> out;
        const siaara::Status st = encode_jpeg_ex(in, fi, p, o, out);
        if (!st) {
            std::fprintf(stderr, "encode failed: %s\n", st.message.c_str());
            return 1;
        }
        return write_file(argv[3], out) ? 0 : 3;
    }
    if (mode == "bench" && argc == 11) {
        std::vector<std::uint8_t> in;
        if (!read_file(argv[2], in)) return 3;
        const FrameInfo fi = parse_info(argv + 3);
        const long iters = num(argv[10]);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(static_cast<std::size_t>(fi.frame_bytes())));
        double best = 1e9;
        for (long i = 0; i < iters; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const siaara::Status st = decode_jpeg(in, fi, out);
            const auto t1 = std::chrono::steady_clock::now();
            if (!st) {
                std::fprintf(stderr, "decode failed: %s\n", st.message.c_str());
                return 1;
            }
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::printf("%.3f\n", best);
        return 0;
    }
    return usage();
}
