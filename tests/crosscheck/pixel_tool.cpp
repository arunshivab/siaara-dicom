// Test-only helper for tests/crosscheck/pixel_crosscheck.py and the render benchmark (not installed).
//   pixel_tool info <file>                      key=value lines
//   pixel_tool frames <file> <out>              all frames, native layout, concatenated
//   pixel_tool full <file> <out>                colour frames as interleaved full samples (4:2:2 expanded)
//   pixel_tool rgb <file> <out>                 colour frames converted to RGB
//   pixel_tool palette <file> <out>             PALETTE COLOR frames as uint16 RGB
//   pixel_tool modality <file> <out>            float64 modality output, all frames
//   pixel_tool voi <file> <out> <ymin> <ymax>   float64 modality + first VOI window/LUT, all frames
//   pixel_tool render <file> <out> [frame]      RGBA8 of one frame
//   pixel_tool bench <file> [iterations]        render timing of frame 0
// Exit codes: 0 ok, 1 usage/IO, 2 library error, 3 unsupported.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "image/color.hpp"
#include "image/frames.hpp"
#include "image/lut.hpp"
#include "image/render.hpp"
#include "image/samples.hpp"

using namespace siaara;
using namespace siaara::image;

namespace {

int fail(const Status& s) {
    std::fprintf(stderr, "error %d: %s\n", static_cast<int>(s.code), s.message.c_str());
    return s.code == Errc::Unsupported ? 3 : 2;
}

bool write_out(const std::string& path, const void* data, std::size_t n) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

double parse_d(const char* s) { return std::strtod(s, nullptr); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pixel_tool <command> <file> ...\n");
        return 1;
    }
    const std::string cmd = argv[1];
    auto file = read_file(argv[2]);
    if (!file) return fail(file.error());
    const Dataset& ds = file->dataset;
    auto img = Image::open(ds, file->transfer_syntax);
    if (!img) return fail(img.error());
    const PixelInfo& pi = img->info();
    const FrameReader& fr = img->frames();
    const std::uint32_t nf = fr.frame_count();

    if (cmd == "info") {
        std::printf("rows=%u\ncolumns=%u\nspp=%u\nbits_allocated=%u\nbits_stored=%u\nsigned=%d\nplanar=%d\nframes=%u\n"
                    "photometric=%s\nencapsulated=%d\nframe_bytes=%llu\nkind=%d\n",
                    pi.rows, pi.columns, pi.samples_per_pixel, pi.bits_allocated, pi.bits_stored, pi.is_signed ? 1 : 0,
                    pi.planar ? 1 : 0, nf, pi.photometric_text.c_str(), fr.encapsulated() ? 1 : 0,
                    static_cast<unsigned long long>(fr.frame_bytes()), static_cast<int>(pi.kind));
        for (const auto& w : fr.warnings()) std::printf("warning=%s\n", w.c_str());
        return 0;
    }
    if (argc < 4 && cmd != "bench") return 1;
    if (cmd == "frames") {
        auto all = fr.decode_range(0, nf);
        if (!all) return fail(all.error());
        return write_out(argv[3], all->data(), all->size()) ? 0 : 1;
    }
    if (cmd == "full" || cmd == "rgb") {
        ColorOptions co;
        co.native_layout = !fr.encapsulated();
        co.codec_applied_mct = fr.codec_applies_color_transform();
        std::vector<std::uint8_t> out;
        std::vector<std::uint8_t> one(static_cast<std::size_t>(rgb_frame_bytes(pi)));
        for (std::uint32_t f = 0; f < nf; ++f) {
            auto frame = fr.decode(f);
            if (!frame) return fail(frame.error());
            Status s = cmd == "full" ? to_interleaved_full(pi, *frame, one, co) : to_rgb(pi, *frame, one, co);
            if (!s) return fail(s);
            out.insert(out.end(), one.begin(), one.end());
        }
        return write_out(argv[3], out.data(), out.size()) ? 0 : 1;
    }
    if (cmd == "palette") {
        if (!img->palette()) return fail(Status(Errc::Unsupported, "no palette"));
        std::vector<std::uint16_t> out, one(static_cast<std::size_t>(pi.pixels() * 3));
        for (std::uint32_t f = 0; f < nf; ++f) {
            auto frame = fr.decode(f);
            if (!frame) return fail(frame.error());
            if (Status s = apply_palette(pi, *frame, *img->palette(), one); !s) return fail(s);
            out.insert(out.end(), one.begin(), one.end());
        }
        return write_out(argv[3], out.data(), out.size() * 2) ? 0 : 1;
    }
    if (cmd == "modality" || cmd == "voi") {
        if (pi.samples_per_pixel != 1 || pi.kind != SampleKind::Integer) return fail(Status(Errc::Unsupported, "not integer grayscale"));
        const double ymin = cmd == "voi" && argc > 5 ? parse_d(argv[4]) : 0;
        const double ymax = cmd == "voi" && argc > 5 ? parse_d(argv[5]) : 255;
        std::vector<double> out;
        const detail::SampleReader rd(pi);
        for (std::uint32_t f = 0; f < nf; ++f) {
            auto frame = fr.decode(f);
            if (!frame) return fail(frame.error());
            auto m = img->modality(f);
            if (!m) return fail(m.error());
            VoiSet vs;
            if (cmd == "voi") {
                auto v = img->voi(f, *m);
                if (!v) return fail(v.error());
                vs = std::move(v).value();
                if (vs.empty()) return fail(Status(Errc::Unsupported, "no VOI in file"));
            }
            for (std::uint64_t i = 0; i < pi.pixels(); ++i) {
                double x = m->apply(rd.value(frame->data(), i));
                if (cmd == "voi") {
                    if (!vs.windows.empty())
                        x = apply_window(x, vs.windows[0].center, vs.windows[0].width, vs.windows[0].function, ymin, ymax);
                    else
                        x = apply_voi_lut(vs.luts[0], x);
                }
                out.push_back(x);
            }
        }
        return write_out(argv[3], out.data(), out.size() * sizeof(double)) ? 0 : 1;
    }
    if (cmd == "render") {
        RenderParams p;
        if (argc > 4) p.frame = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
        auto out = render_2d(*img, p);
        if (!out) return fail(out.error());
        return write_out(argv[3], out->data(), out->size()) ? 0 : 1;
    }
    if (cmd == "bench") {
        const int iters = argc > 3 ? std::atoi(argv[3]) : 200;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(pi.pixels() * 4));
        RenderParams p;
        if (Status s = render_2d(*img, p, out); !s) return fail(s);  // warm up
        double best = 1e9, total = 0;
        for (int i = 0; i < iters; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            (void)render_2d(*img, p, out);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            best = ms < best ? ms : best;
            total += ms;
        }
        std::printf("render %ux%u %u-bit %s: best %.3f ms, mean %.3f ms over %d\n", pi.columns, pi.rows, pi.bits_stored,
                    pi.photometric_text.c_str(), best, total / iters, iters);
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 1;
}
