// Test-only tool (never shipped): converts between raw JPEG-LS codestreams and raw native pixel
// bytes so tests/crosscheck/jpegls_crosscheck.py can compare SIAARA with independent decoders.
//
//   jpegls_tool decode <in.jls> <out.raw> [--signed]
//       prints "width height components precision near ilv transform" on success
//   jpegls_tool encode <in.raw> <out.jls> <rows> <cols> <spp> <bits> <near> <ilv> [restart]
//                      [maxval t1 t2 t3 reset]
//       input is colour-by-pixel, 8-bit samples for bits <= 8, else little-endian 16-bit
//   jpegls_tool bench <in.jls> <iterations>
//       decodes repeatedly and prints the best and median time per decode in milliseconds
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "codec/jpegls.hpp"

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

FrameInfo info_from(const jpegls::StreamInfo& si, bool is_signed) {
    FrameInfo fi;
    fi.rows = si.height;
    fi.columns = si.width;
    fi.samples_per_pixel = static_cast<std::uint16_t>(si.components);
    fi.bits_stored = static_cast<std::uint16_t>(si.precision);
    fi.bits_allocated = si.precision > 8 ? 16 : 8;
    fi.is_signed = is_signed;
    return fi;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: see the comment at the top of jpegls_tool.cpp\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "decode" && argc >= 4) {
        std::vector<std::uint8_t> in;
        if (!read_file(argv[2], in)) return 2;
        jpegls::StreamInfo si;
        if (Status s = jpegls::probe(in, si); !s) return fail(s);
        const bool is_signed = argc >= 5 && std::strcmp(argv[4], "--signed") == 0;
        const FrameInfo fi = info_from(si, is_signed);
        std::vector<std::uint8_t> out(fi.frame_bytes());
        if (Status s = decode_jpegls(in, fi, out); !s) return fail(s);
        if (!write_file(argv[3], out)) return 2;
        std::printf("%u %u %d %d %d %d %d\n", si.width, si.height, si.components, si.precision, si.near,
                    si.interleave, si.color_transform);
        return 0;
    }
    if (cmd == "encode" && argc >= 10) {
        std::vector<std::uint8_t> in;
        if (!read_file(argv[2], in)) return 2;
        FrameInfo fi;
        fi.rows = static_cast<std::uint32_t>(std::atoi(argv[4]));
        fi.columns = static_cast<std::uint32_t>(std::atoi(argv[5]));
        fi.samples_per_pixel = static_cast<std::uint16_t>(std::atoi(argv[6]));
        fi.bits_stored = static_cast<std::uint16_t>(std::atoi(argv[7]));
        fi.bits_allocated = fi.bits_stored > 8 ? 16 : 8;
        jpegls::EncodeOptions opt;
        opt.near = std::atoi(argv[8]);
        opt.interleave = std::atoi(argv[9]);
        if (argc >= 11) opt.restart_interval = static_cast<std::uint32_t>(std::atoi(argv[10]));
        if (argc >= 16) {
            opt.write_preset = true;
            opt.preset.maxval = std::atoi(argv[11]);
            opt.preset.t1 = std::atoi(argv[12]);
            opt.preset.t2 = std::atoi(argv[13]);
            opt.preset.t3 = std::atoi(argv[14]);
            opt.preset.reset = std::atoi(argv[15]);
        }
        std::vector<std::uint8_t> out;
        if (Status s = jpegls::encode(in, fi, opt, out); !s) return fail(s);
        return write_file(argv[3], out) ? 0 : 2;
    }
    if (cmd == "bench" && argc >= 4) {
        std::vector<std::uint8_t> in;
        if (!read_file(argv[2], in)) return 2;
        jpegls::StreamInfo si;
        if (Status s = jpegls::probe(in, si); !s) return fail(s);
        const FrameInfo fi = info_from(si, false);
        std::vector<std::uint8_t> out(fi.frame_bytes());
        const int iters = std::max(1, std::atoi(argv[3]));
        std::vector<double> ms;
        for (int i = 0; i < iters; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            if (Status s = decode_jpegls(in, fi, out); !s) return fail(s);
            const auto t1 = std::chrono::steady_clock::now();
            ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        std::printf("best %.3f ms, median %.3f ms over %d decodes (%zu bytes in)\n", ms.front(),
                    ms[ms.size() / 2], iters, in.size());
        return 0;
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
