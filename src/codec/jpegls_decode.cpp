// SIAARA DICOM Toolkit - JPEG-LS decoder (ITU-T T.87), written from the Recommendation.
#include <algorithm>
#include <cstring>
#include <string>

#include "codec/jpegls_internal.hpp"

namespace siaara::codec {

namespace {

using jpegls::PresetParams;
using namespace jpegls::detail;

constexpr std::size_t kNoMarker = static_cast<std::size_t>(-1);

struct FrameHeader {
    bool seen = false;
    int precision = 0;
    std::uint32_t height = 0;
    std::uint32_t width = 0;
    int components = 0;
    std::uint8_t ids[4] = {0, 0, 0, 0};
};

struct ScanHeader {
    int ns = 0;
    int comp[4] = {0, 0, 0, 0};  // frame component indices
    int near = 0;
    int ilv = 0;
};

struct DecodeState {
    FrameHeader frame;
    PresetParams preset;
    std::uint32_t restart_interval = 0;
    int transform = 0;
    bool done[4] = {false, false, false, false};
};

Status malformed(const std::string& what, std::size_t offset) {
    return Status(Errc::Malformed, "JPEG-LS: " + what, offset);
}

// First marker (0xFF followed by a byte >= 0x80) at or after `from`.
std::size_t find_marker(std::span<const std::uint8_t> in, std::size_t from) {
    for (std::size_t i = from; i + 1 < in.size(); ++i)
        if (in[i] == 0xFF && in[i + 1] >= 0x80) return i;
    return kNoMarker;
}

Status parse_frame(std::span<const std::uint8_t> s, std::size_t off, DecodeState& st) {
    if (st.frame.seen) return malformed("second frame header", off);
    if (s.size() < 6) return malformed("short SOF55", off);
    FrameHeader f;
    f.precision = s[0];
    f.height = load_be16(&s[1]);
    f.width = load_be16(&s[3]);
    f.components = s[5];
    if (f.components < 1 || f.components > 4)
        return Status(Errc::Unsupported, "JPEG-LS: only 1 to 4 components are supported", off);
    if (s.size() != 6 + 3 * static_cast<std::size_t>(f.components)) return malformed("bad SOF55 length", off);
    if (f.precision < 2 || f.precision > 16) return malformed("precision out of range", off);
    for (int i = 0; i < f.components; ++i) {
        const std::size_t b = 6 + 3 * static_cast<std::size_t>(i);
        f.ids[i] = s[b];
        if (s[b + 1] != 0x11)
            return Status(Errc::Unsupported, "JPEG-LS: subsampled components are not supported", off);
        for (int j = 0; j < i; ++j)
            if (f.ids[j] == f.ids[i]) return malformed("duplicate component id", off);
    }
    f.seen = true;
    st.frame = f;
    return Status::ok();
}

Status parse_lse(std::span<const std::uint8_t> s, std::size_t off, DecodeState& st) {
    if (s.empty()) return malformed("empty LSE", off);
    switch (s[0]) {
        case 1:
            if (s.size() != 11) return malformed("bad LSE type 1 length", off);
            st.preset.maxval = load_be16(&s[1]);
            st.preset.t1 = load_be16(&s[3]);
            st.preset.t2 = load_be16(&s[5]);
            st.preset.t3 = load_be16(&s[7]);
            st.preset.reset = load_be16(&s[9]);
            return Status::ok();
        case 2:
        case 3:
            return Status::ok();  // mapping tables; a scan that selects one is rejected
        case 4: {
            if (!st.frame.seen) return malformed("LSE type 4 before the frame header", off);
            if (s.size() < 2) return malformed("short LSE type 4", off);
            const std::size_t w = s[1];
            if (w < 2 || w > 4 || s.size() != 2 + 2 * w) return malformed("bad LSE type 4", off);
            std::uint32_t ye = 0, xe = 0;
            for (std::size_t i = 0; i < w; ++i) {
                ye = (ye << 8) | s[2 + i];
                xe = (xe << 8) | s[2 + w + i];
            }
            st.frame.height = ye;
            st.frame.width = xe;
            return Status::ok();
        }
        default:
            return malformed("unknown LSE type", off);
    }
}

Status parse_scan_header(std::span<const std::uint8_t> s, std::size_t off, const DecodeState& st,
                         ScanHeader& sh) {
    if (!st.frame.seen) return malformed("scan before the frame header", off);
    if (s.empty()) return malformed("empty SOS", off);
    sh.ns = s[0];
    if (sh.ns < 1 || sh.ns > st.frame.components) return malformed("bad component count in SOS", off);
    if (s.size() != 1 + 2 * static_cast<std::size_t>(sh.ns) + 3) return malformed("bad SOS length", off);
    for (int i = 0; i < sh.ns; ++i) {
        const std::uint8_t id = s[1 + 2 * static_cast<std::size_t>(i)];
        const std::uint8_t tm = s[2 + 2 * static_cast<std::size_t>(i)];
        int idx = -1;
        for (int j = 0; j < st.frame.components; ++j)
            if (st.frame.ids[j] == id) idx = j;
        if (idx < 0) return malformed("SOS names an unknown component", off);
        if (st.done[idx]) return malformed("component coded twice", off);
        for (int j = 0; j < i; ++j)
            if (sh.comp[j] == idx) return malformed("component repeated in SOS", off);
        if (tm != 0) return Status(Errc::Unsupported, "JPEG-LS: mapping tables are not supported", off);
        sh.comp[i] = idx;
    }
    const std::size_t b = 1 + 2 * static_cast<std::size_t>(sh.ns);
    sh.near = s[b];
    sh.ilv = s[b + 1];
    if (sh.ilv > 2) return malformed("bad interleave mode", off);
    if (sh.ilv == 0 && sh.ns > 1) return malformed("ILV 0 scan with several components", off);
    if (sh.ns == 1) sh.ilv = 0;
    if ((s[b + 2] & 0x0F) != 0)
        return Status(Errc::Unsupported, "JPEG-LS: point transform is not supported", off);
    if ((s[b + 2] & 0xF0) != 0) return malformed("bad Ah/Al byte", off);
    return Status::ok();
}

// Writes one decoded line of frame component `comp` (raw P-bit values) into the native buffer.
void store_row(std::span<std::uint8_t> out, const FrameInfo& info, int comp, std::uint32_t y,
               const std::int32_t* line) {
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
    std::uint8_t* d = out.data() + base;
    if (bps == 1) {
        for (std::size_t x = 0; x < cols; ++x, d += step) *d = static_cast<std::uint8_t>(line[x + 1]);
    } else {
        for (std::size_t x = 0; x < cols; ++x, d += step) store_le16(d, static_cast<std::uint16_t>(line[x + 1]));
    }
}

template <bool Lossless>
Status decode_lines(std::span<const std::uint8_t> in, const CodingParams& cp, const ScanHeader& sh,
                    const DecodeState& st, const FrameInfo& info, std::span<std::uint8_t> out,
                    std::size_t scan_offset, BitReader& br) {
    const int w = static_cast<int>(info.columns);
    const std::uint32_t h = info.rows;
    const std::size_t stride = static_cast<std::size_t>(w) + 2;
    std::vector<std::int32_t> buf(static_cast<std::size_t>(sh.ns) * 2 * stride, 0);
    std::int32_t* prev[4] = {nullptr, nullptr, nullptr, nullptr};
    std::int32_t* cur[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int c = 0; c < sh.ns; ++c) {
        prev[c] = buf.data() + (2 * static_cast<std::size_t>(c)) * stride;
        cur[c] = prev[c] + stride;
    }

    ScanCoder<false, Lossless> coder(cp, br);
    int ri[4] = {0, 0, 0, 0};
    int rst = 0;
    const std::uint32_t interval = st.restart_interval;

    for (std::uint32_t y = 0; y < h; ++y) {
        if (interval != 0 && y != 0 && y % interval == 0) {
            if (br.overran()) return Status(Errc::Truncated, "JPEG-LS: scan data ends early", scan_offset);
            std::size_t m = find_marker(in, static_cast<std::size_t>(br.resume_point() - in.data()));
            while (m != kNoMarker && m + 1 < in.size() && in[m + 1] == 0xFF) ++m;  // fill bytes
            if (m == kNoMarker || m + 1 >= in.size())
                return Status(Errc::Truncated, "JPEG-LS: missing restart marker", scan_offset);
            if (in[m + 1] != kRST0 + rst) return malformed("expected restart marker", m);
            rst = (rst + 1) & 7;
            br.start(in.data() + m + 2, in.data() + in.size());
            coder.reset_contexts();
            for (int& r : ri) r = 0;
            std::fill(buf.begin(), buf.end(), 0);
        }
        if (sh.ilv == 2) {
            coder.line_interleaved(sh.ns, prev, cur, nullptr, w, ri[0]);
        } else {
            for (int c = 0; c < sh.ns; ++c) coder.line(prev[c], cur[c], nullptr, w, ri[c]);
        }
        if (coder.failed()) return malformed("corrupt entropy-coded data", scan_offset);
        for (int c = 0; c < sh.ns; ++c) {
            store_row(out, info, sh.comp[c], y, cur[c]);
            std::swap(prev[c], cur[c]);
        }
    }
    return Status::ok();
}

Status decode_scan(std::span<const std::uint8_t> in, std::size_t& pos, const ScanHeader& sh,
                   DecodeState& st, const FrameInfo& info, std::span<std::uint8_t> out) {
    const std::size_t scan_offset = pos;
    CodingParams cp;
    if (Status s = derive_params(st.frame.precision, sh.near, st.preset, cp); !s) {
        s.offset = scan_offset;
        return s;
    }
    BitReader br;
    br.start(in.data() + pos, in.data() + in.size());
    Status s = cp.near == 0 ? decode_lines<true>(in, cp, sh, st, info, out, scan_offset, br)
                            : decode_lines<false>(in, cp, sh, st, info, out, scan_offset, br);
    if (!s) return s;
    if (br.overran()) return Status(Errc::Truncated, "JPEG-LS: scan data ends early", scan_offset);
    const std::size_t next = find_marker(in, static_cast<std::size_t>(br.resume_point() - in.data()));
    if (next == kNoMarker) return Status(Errc::Truncated, "JPEG-LS: no marker after scan", scan_offset);
    pos = next;
    for (int c = 0; c < sh.ns; ++c) st.done[sh.comp[c]] = true;
    return Status::ok();
}

// Inverse of the HP colour transforms signalled in APP8 "mrfx" (values 1..3).
void inverse_color_transform(std::span<std::uint8_t> out, const FrameInfo& info, int transform) {
    const std::size_t npx = static_cast<std::size_t>(info.rows) * info.columns;
    const std::size_t bps = info.bits_allocated / 8u;
    const int bits = info.bits_stored;
    const int mask = (1 << bits) - 1;
    const int half = 1 << (bits - 1);
    auto at = [&](std::size_t i, int c) -> std::uint8_t* {
        return info.planar ? out.data() + (static_cast<std::size_t>(c) * npx + i) * bps
                           : out.data() + (i * 3 + static_cast<std::size_t>(c)) * bps;
    };
    auto get = [&](std::uint8_t* p) -> int { return bps == 1 ? *p : load_le16(p); };
    auto put = [&](std::uint8_t* p, int v) {
        if (bps == 1) *p = static_cast<std::uint8_t>(v);
        else store_le16(p, static_cast<std::uint16_t>(v));
    };
    for (std::size_t i = 0; i < npx; ++i) {
        std::uint8_t* p0 = at(i, 0);
        std::uint8_t* p1 = at(i, 1);
        std::uint8_t* p2 = at(i, 2);
        const int v1 = get(p0), v2 = get(p1), v3 = get(p2);
        int r = 0, g = 0, b = 0;
        if (transform == 1) {
            g = v2;
            r = (v1 + v2 - half) & mask;
            b = (v3 + v2 - half) & mask;
        } else if (transform == 2) {
            g = v2;
            r = (v1 + v2 - half) & mask;
            b = (v3 + ((r + g) >> 1) - half) & mask;
        } else {
            g = (v1 - ((v2 + v3) >> 2) + (half >> 1)) & mask;
            r = (v3 + g - half) & mask;
            b = (v2 + g - half) & mask;
        }
        put(p0, r);
        put(p1, g);
        put(p2, b);
    }
}

void sign_extend(std::span<std::uint8_t> out, const FrameInfo& info) {
    const int bits = info.bits_stored;
    if (info.bits_allocated == 8) {
        const unsigned sign = 1u << (bits - 1);
        const unsigned high = 0xFFu & ~((1u << bits) - 1);
        for (auto& v : out)
            if (v & sign) v = static_cast<std::uint8_t>(v | high);
    } else {
        const unsigned sign = 1u << (bits - 1);
        const unsigned high = 0xFFFFu & ~((1u << bits) - 1);
        for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
            const unsigned v = load_le16(&out[i]);
            if (v & sign) store_le16(&out[i], static_cast<std::uint16_t>(v | high));
        }
    }
}

}  // namespace

namespace jpegls::detail {

Status validate_frame_info(const FrameInfo& info) {
    if (info.rows == 0 || info.columns == 0) return Status(Errc::InvalidArgument, "JPEG-LS: empty frame");
    if (info.rows > 65535 || info.columns > 65535)
        return Status(Errc::Unsupported, "JPEG-LS: frame dimensions above 65535");
    if (info.samples_per_pixel < 1 || info.samples_per_pixel > 4)
        return Status(Errc::Unsupported, "JPEG-LS: 1 to 4 samples per pixel are supported");
    if (info.bits_allocated != 8 && info.bits_allocated != 16)
        return Status(Errc::Unsupported, "JPEG-LS: BitsAllocated must be 8 or 16");
    if (info.bits_stored < 2 || info.bits_stored > info.bits_allocated)
        return Status(Errc::Unsupported, "JPEG-LS: BitsStored must be 2..BitsAllocated");
    if (info.frame_bytes() == 0) return Status(Errc::InvalidArgument, "JPEG-LS: bad frame size");
    return Status::ok();
}

}  // namespace jpegls::detail

Status decode_jpegls(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out) {
    if (Status s = jpegls::detail::validate_frame_info(info); !s) return s;
    if (out.size() != info.frame_bytes())
        return Status(Errc::BufferTooSmall, "JPEG-LS: output size does not match the frame");
    if (in.size() < 2 || in[0] != 0xFF || in[1] != kSOI) return malformed("missing SOI", 0);

    DecodeState st;
    std::size_t pos = 2;
    bool eoi = false;
    while (!eoi) {
        if (pos >= in.size()) return Status(Errc::Truncated, "JPEG-LS: missing EOI", pos);
        if (in[pos] != 0xFF) return malformed("expected a marker", pos);
        while (pos < in.size() && in[pos] == 0xFF) ++pos;
        if (pos >= in.size()) return Status(Errc::Truncated, "JPEG-LS: missing EOI", pos);
        const std::uint8_t m = in[pos++];
        if (m == kEOI) {
            eoi = true;
            break;
        }
        if (m == kSOI || m == 0x00 || (m >= kRST0 && m <= kRST0 + 7) || m == 0x01)
            return malformed("unexpected marker", pos - 2);
        if (in.size() - pos < 2) return Status(Errc::Truncated, "JPEG-LS: truncated marker segment", pos);
        const std::size_t len = load_be16(&in[pos]);
        if (len < 2) return malformed("bad marker segment length", pos);
        if (in.size() - pos < len) return Status(Errc::Truncated, "JPEG-LS: truncated marker segment", pos);
        const std::span<const std::uint8_t> seg = in.subspan(pos + 2, len - 2);
        const std::size_t seg_off = pos - 2;
        pos += len;

        if (m == kSOF55) {
            if (Status s = parse_frame(seg, seg_off, st); !s) return s;
        } else if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return Status(Errc::Unsupported, "JPEG-LS: not a JPEG-LS frame (SOF" + std::to_string(m - 0xC0) + ")",
                          seg_off);
        } else if (m == kLSE) {
            if (Status s = parse_lse(seg, seg_off, st); !s) return s;
        } else if (m == kDRI) {
            if (seg.size() < 2 || seg.size() > 4) return malformed("bad DRI length", seg_off);
            std::uint32_t ri = 0;
            for (std::uint8_t b : seg) ri = (ri << 8) | b;
            st.restart_interval = ri;
        } else if (m == kAPP8) {
            if (seg.size() >= 5 && std::memcmp(seg.data(), "mrfx", 4) == 0) st.transform = seg[4];
        } else if (m == kDNL) {
            return Status(Errc::Unsupported, "JPEG-LS: DNL marker is not supported", seg_off);
        } else if (m == kSOS) {
            ScanHeader sh;
            if (Status s = parse_scan_header(seg, seg_off, st, sh); !s) return s;
            const FrameHeader& f = st.frame;
            if (f.width != info.columns || f.height != info.rows ||
                f.components != info.samples_per_pixel || f.precision != info.bits_stored)
                return Status(Errc::Malformed,
                              "JPEG-LS: frame header (" + std::to_string(f.width) + "x" + std::to_string(f.height) +
                                  ", " + std::to_string(f.components) + " components, P=" +
                                  std::to_string(f.precision) + ") does not match the DICOM attributes",
                              seg_off);
            if (Status s = decode_scan(in, pos, sh, st, info, out); !s) return s;
        }
        // Other segments (APPn, COM, ...) are skipped.
    }

    for (int c = 0; c < info.samples_per_pixel; ++c)
        if (!st.done[c]) return Status(Errc::Truncated, "JPEG-LS: not every component was coded", pos);
    if (st.transform != 0) {
        if (st.transform > 3) return Status(Errc::Unsupported, "JPEG-LS: unknown colour transform");
        if (info.samples_per_pixel != 3)
            return Status(Errc::Malformed, "JPEG-LS: colour transform needs 3 components");
        inverse_color_transform(out, info, st.transform);
    }
    if (info.is_signed && info.bits_stored < info.bits_allocated) sign_extend(out, info);
    return Status::ok();
}

Status jpegls::probe(std::span<const std::uint8_t> in, StreamInfo& out) {
    if (in.size() < 2 || in[0] != 0xFF || in[1] != kSOI) return malformed("missing SOI", 0);
    DecodeState st;
    std::size_t pos = 2;
    for (;;) {
        if (pos >= in.size() || in[pos] != 0xFF) return malformed("expected a marker", pos);
        while (pos < in.size() && in[pos] == 0xFF) ++pos;
        if (pos >= in.size()) return Status(Errc::Truncated, "JPEG-LS: truncated", pos);
        const std::uint8_t m = in[pos++];
        if (m == kEOI) return malformed("no scan", pos);
        if (in.size() - pos < 2) return Status(Errc::Truncated, "JPEG-LS: truncated", pos);
        const std::size_t len = load_be16(&in[pos]);
        if (len < 2 || in.size() - pos < len) return Status(Errc::Truncated, "JPEG-LS: truncated", pos);
        const std::span<const std::uint8_t> seg = in.subspan(pos + 2, len - 2);
        const std::size_t seg_off = pos - 2;
        pos += len;
        if (m == kSOF55) {
            if (Status s = parse_frame(seg, seg_off, st); !s) return s;
        } else if (m == kLSE) {
            if (Status s = parse_lse(seg, seg_off, st); !s) return s;
        } else if (m == kAPP8) {
            if (seg.size() >= 5 && std::memcmp(seg.data(), "mrfx", 4) == 0) st.transform = seg[4];
        } else if (m == kSOS) {
            ScanHeader sh;
            if (Status s = parse_scan_header(seg, seg_off, st, sh); !s) return s;
            out.width = st.frame.width;
            out.height = st.frame.height;
            out.precision = st.frame.precision;
            out.components = st.frame.components;
            out.near = sh.near;
            out.interleave = sh.ns == 1 && st.frame.components > 1 ? 0 : sh.ilv;
            out.color_transform = st.transform;
            return Status::ok();
        }
    }
}

}  // namespace siaara::codec
