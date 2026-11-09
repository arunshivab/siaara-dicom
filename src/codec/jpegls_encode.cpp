// SIAARA DICOM Toolkit - JPEG-LS encoder (ITU-T T.87), written from the Recommendation.
#include <algorithm>

#include "codec/jpegls_internal.hpp"

namespace siaara::codec {

namespace {

using namespace jpegls::detail;

void put_marker(std::vector<std::uint8_t>& out, std::uint8_t m) {
    out.push_back(0xFF);
    out.push_back(m);
}

void put_u16(std::vector<std::uint8_t>& out, unsigned v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// Loads row y of component `comp` from the native buffer into line[1..cols].
void load_row(std::span<const std::uint8_t> in, const FrameInfo& info, int comp, std::uint32_t y,
              std::int32_t* line) {
    const std::size_t cols = info.columns;
    const std::size_t nc = info.samples_per_pixel;
    const std::size_t bps = info.bits_allocated / 8u;
    std::size_t base = 0;
    std::size_t step = 0;
    if (info.planar) {
        base = ((static_cast<std::size_t>(comp) * info.rows + y) * cols) * bps;
        step = bps;
    } else {
        base = (static_cast<std::size_t>(y) * cols * nc + static_cast<std::size_t>(comp)) * bps;
        step = nc * bps;
    }
    const std::uint8_t* s = in.data() + base;
    const std::int32_t mask = (1 << info.bits_stored) - 1;
    if (bps == 1) {
        for (std::size_t x = 0; x < cols; ++x, s += step) line[x + 1] = *s & mask;
    } else {
        for (std::size_t x = 0; x < cols; ++x, s += step) line[x + 1] = load_le16(s) & mask;
    }
}

}  // namespace

Status jpegls::encode(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeOptions& opt,
                      std::vector<std::uint8_t>& out) {
    if (Status s = validate_frame_info(info); !s) return s;
    if (in.size() != info.frame_bytes())
        return Status(Errc::InvalidArgument, "JPEG-LS: input size does not match the frame");
    const int nc = info.samples_per_pixel;
    int ilv = opt.interleave < 0 ? (info.planar ? 0 : 2) : opt.interleave;
    if (ilv > 2) return Status(Errc::InvalidArgument, "JPEG-LS: interleave mode must be 0, 1 or 2");
    if (nc == 1) ilv = 0;

    const PresetParams preset = opt.write_preset ? opt.preset : PresetParams{};
    CodingParams cp;
    if (Status s = derive_params(info.bits_stored, opt.near, preset, cp); !s)
        return Status(Errc::InvalidArgument, s.message);

    const int w = static_cast<int>(info.columns);
    const std::uint32_t h = info.rows;
    const std::size_t stride = static_cast<std::size_t>(w) + 2;

    out.clear();
    out.reserve(in.size() / 2 + 256);
    put_marker(out, kSOI);
    put_marker(out, kSOF55);
    put_u16(out, 8 + 3 * static_cast<unsigned>(nc));
    out.push_back(static_cast<std::uint8_t>(info.bits_stored));
    put_u16(out, h);
    put_u16(out, static_cast<unsigned>(w));
    out.push_back(static_cast<std::uint8_t>(nc));
    for (int c = 0; c < nc; ++c) {
        out.push_back(static_cast<std::uint8_t>(c + 1));
        out.push_back(0x11);
        out.push_back(0);
    }
    if (opt.write_preset) {
        put_marker(out, kLSE);
        put_u16(out, 13);
        out.push_back(1);
        put_u16(out, static_cast<unsigned>(preset.maxval));
        put_u16(out, static_cast<unsigned>(preset.t1));
        put_u16(out, static_cast<unsigned>(preset.t2));
        put_u16(out, static_cast<unsigned>(preset.t3));
        put_u16(out, static_cast<unsigned>(preset.reset));
    }
    const std::uint32_t interval = opt.restart_interval;
    if (interval != 0) {
        put_marker(out, kDRI);
        if (interval <= 0xFFFF) {
            put_u16(out, 4);
            put_u16(out, interval);
        } else {
            put_u16(out, 6);
            put_u16(out, interval >> 16);
            put_u16(out, interval & 0xFFFF);
        }
    }

    auto encode_scan = [&](const int* comps, int ns) -> Status {
        put_marker(out, kSOS);
        put_u16(out, 6 + 2 * static_cast<unsigned>(ns));
        out.push_back(static_cast<std::uint8_t>(ns));
        for (int i = 0; i < ns; ++i) {
            out.push_back(static_cast<std::uint8_t>(comps[i] + 1));
            out.push_back(0);
        }
        out.push_back(static_cast<std::uint8_t>(cp.near));
        out.push_back(static_cast<std::uint8_t>(ns == 1 ? 0 : ilv));
        out.push_back(0);

        std::vector<std::int32_t> buf(static_cast<std::size_t>(ns) * 3 * stride, 0);
        std::int32_t* prev[4] = {nullptr, nullptr, nullptr, nullptr};
        std::int32_t* cur[4] = {nullptr, nullptr, nullptr, nullptr};
        std::int32_t* src[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int c = 0; c < ns; ++c) {
            prev[c] = buf.data() + 3 * static_cast<std::size_t>(c) * stride;
            cur[c] = prev[c] + stride;
            src[c] = cur[c] + stride;
        }
        BitWriter bw(out);
        ScanCoder<true, true> lossless(cp, bw);
        ScanCoder<true, false> lossy(cp, bw);
        const bool is_lossless = cp.near == 0;
        int ri[4] = {0, 0, 0, 0};
        int rst = 0;
        for (std::uint32_t y = 0; y < h; ++y) {
            if (interval != 0 && y != 0 && y % interval == 0) {
                bw.flush();
                put_marker(out, static_cast<std::uint8_t>(kRST0 + rst));
                rst = (rst + 1) & 7;
                lossless.reset_contexts();
                lossy.reset_contexts();
                for (int& r : ri) r = 0;
                for (int c = 0; c < ns; ++c) {
                    std::fill(prev[c], prev[c] + stride, 0);
                    std::fill(cur[c], cur[c] + stride, 0);
                }
            }
            for (int c = 0; c < ns; ++c) {
                load_row(in, info, comps[c], y, src[c]);
                for (int x = 1; x <= w; ++x)
                    if (src[c][x] > cp.maxval)
                        return Status(Errc::InvalidArgument, "JPEG-LS: sample above MAXVAL");
            }
            if (ns > 1 && ilv == 2) {
                if (is_lossless) lossless.line_interleaved(ns, prev, cur, src, w, ri[0]);
                else lossy.line_interleaved(ns, prev, cur, src, w, ri[0]);
            } else {
                for (int c = 0; c < ns; ++c) {
                    if (is_lossless) lossless.line(prev[c], cur[c], src[c], w, ri[c]);
                    else lossy.line(prev[c], cur[c], src[c], w, ri[c]);
                }
            }
            for (int c = 0; c < ns; ++c) std::swap(prev[c], cur[c]);
        }
        bw.flush();
        return Status::ok();
    };

    if (ilv == 0) {
        for (int c = 0; c < nc; ++c)
            if (Status s = encode_scan(&c, 1); !s) return s;
    } else {
        const int comps[4] = {0, 1, 2, 3};
        if (Status s = encode_scan(comps, nc); !s) return s;
    }
    put_marker(out, kEOI);
    return Status::ok();
}

Status encode_jpegls(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                     std::vector<std::uint8_t>& out) {
    if (p.near_lossless < 0) return Status(Errc::InvalidArgument, "JPEG-LS: NEAR must not be negative");
    jpegls::EncodeOptions opt;
    opt.near = p.near_lossless;
    return jpegls::encode(in, info, opt, out);
}

}  // namespace siaara::codec
