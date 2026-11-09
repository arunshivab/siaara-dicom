// Test-only helper for the codec cross-checks and benchmarks (never shipped).
//   codec_tool inflate <in> <out>
//   codec_tool deflate <level> <in> <out>
//   codec_tool rle-decode <rows> <cols> <samples> <bits> <planar> <in> <out>
//   codec_tool rle-encode <rows> <cols> <samples> <bits> <planar> <in> <out>
//   codec_tool bench-inflate <in> <seconds>          prints MB/s of inflated output
//   codec_tool bench-rle-decode <rows> <cols> <samples> <bits> <planar> <in> <seconds>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/deflate.hpp"

using namespace siaara;

namespace {

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    out.clear();
    std::uint8_t buf[1 << 16];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

bool write_file(const char* path, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

int fail(const Status& s) {
    std::fprintf(stderr, "error %d: %s (offset %llu)\n", static_cast<int>(s.code), s.message.c_str(),
                 static_cast<unsigned long long>(s.offset));
    return 2;
}

codec::FrameInfo frame(char** a) {
    codec::FrameInfo fi;
    fi.rows = static_cast<std::uint32_t>(std::strtoul(a[0], nullptr, 10));
    fi.columns = static_cast<std::uint32_t>(std::strtoul(a[1], nullptr, 10));
    fi.samples_per_pixel = static_cast<std::uint16_t>(std::strtoul(a[2], nullptr, 10));
    fi.bits_allocated = static_cast<std::uint16_t>(std::strtoul(a[3], nullptr, 10));
    fi.bits_stored = fi.bits_allocated;
    fi.planar = std::strtoul(a[4], nullptr, 10) != 0;
    return fi;
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const std::string mode = argv[1];
    std::vector<std::uint8_t> in, out;
    if (mode == "inflate" && argc == 4) {
        if (!read_file(argv[2], in)) return 1;
        if (Status s = codec::inflate_raw(in.data(), in.size(), SIZE_MAX / 2, out); !s) return fail(s);
        return write_file(argv[3], out) ? 0 : 1;
    }
    if (mode == "deflate" && argc == 5) {
        if (!read_file(argv[3], in)) return 1;
        if (Status s = codec::deflate_raw(in.data(), in.size(), std::atoi(argv[2]), out); !s) return fail(s);
        return write_file(argv[4], out) ? 0 : 1;
    }
    if ((mode == "rle-decode" || mode == "rle-encode") && argc == 9) {
        const codec::FrameInfo fi = frame(argv + 2);
        if (!read_file(argv[7], in)) return 1;
        if (mode == "rle-decode") {
            out.resize(static_cast<std::size_t>(fi.frame_bytes()));
            if (Status s = codec::decode_rle(in, fi, out); !s) return fail(s);
        } else {
            if (Status s = codec::encode_rle(in, fi, codec::EncodeParams{}, out); !s) return fail(s);
        }
        return write_file(argv[8], out) ? 0 : 1;
    }
    if (mode == "bench-inflate" && argc == 4) {
        if (!read_file(argv[2], in)) return 1;
        const double budget = std::atof(argv[3]);
        double best = 1e30;
        const double start = now();
        int reps = 0;
        while (reps < 3 || now() - start < budget) {
            const double t0 = now();
            if (Status s = codec::inflate_raw(in.data(), in.size(), SIZE_MAX / 2, out); !s) return fail(s);
            const double t = now() - t0;
            if (t < best) best = t;
            ++reps;
        }
        std::printf("inflate: %zu -> %zu bytes, best of %d: %.4f s, %.1f MB/s (output)\n", in.size(), out.size(), reps,
                    best, static_cast<double>(out.size()) / best / 1e6);
        return 0;
    }
    if (mode == "bench-rle-decode" && argc == 9) {
        const codec::FrameInfo fi = frame(argv + 2);
        if (!read_file(argv[7], in)) return 1;
        const double budget = std::atof(argv[8]);
        out.resize(static_cast<std::size_t>(fi.frame_bytes()));
        double best = 1e30;
        const double start = now();
        int reps = 0;
        while (reps < 3 || now() - start < budget) {
            const double t0 = now();
            if (Status s = codec::decode_rle(in, fi, out); !s) return fail(s);
            const double t = now() - t0;
            if (t < best) best = t;
            ++reps;
        }
        std::printf("rle decode: %zu -> %zu bytes, best of %d: %.4f s, %.1f MB/s (output)\n", in.size(), out.size(),
                    reps, best, static_cast<double>(out.size()) / best / 1e6);
        return 0;
    }
    std::fprintf(stderr, "bad arguments\n");
    return 1;
}
