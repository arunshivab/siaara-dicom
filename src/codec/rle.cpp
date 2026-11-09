// SIAARA DICOM Toolkit - RLE Lossless (DICOM PS3.5 Annex G), written from the standard.
//
// A frame is split into byte segments: for each sample, one segment per byte of the sample,
// most significant byte first. Each segment holds rows x columns bytes and is run-length coded
// (a signed header byte n: 0..127 -> n+1 literal bytes follow, -1..-127 -> the next byte repeats
// 1-n times, -128 -> no operation). A 64-byte header gives the segment count (1..15) and the
// offset of each segment from the start of the header.
#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include "codec/codec.hpp"
#include "core/types.hpp"

namespace siaara::codec {
namespace {

constexpr std::size_t kHeaderSize = 64;
constexpr std::uint32_t kMaxSegments = 15;

struct Layout {
    std::uint64_t pixels = 0;       // rows x columns: bytes per segment
    std::uint32_t bytes = 0;        // bytes per sample
    std::uint32_t samples = 0;
    std::uint32_t segments = 0;
    std::uint64_t frame = 0;
};

Status make_layout(const FrameInfo& info, Layout& l) {
    if (info.rows == 0 || info.columns == 0 || info.samples_per_pixel == 0)
        return Status(Errc::InvalidArgument, "rle: empty frame geometry");
    if (info.bits_allocated == 0 || info.bits_allocated % 8 != 0)
        return Status(Errc::Unsupported, "rle: bits allocated must be a multiple of 8");
    l.bytes = info.bits_allocated / 8u;
    l.samples = info.samples_per_pixel;
    std::uint64_t segs = 0;
    if (!checked_mul(l.bytes, l.samples, segs) || segs > kMaxSegments)
        return Status(Errc::Unsupported, "rle: more than 15 segments needed");
    l.segments = static_cast<std::uint32_t>(segs);
    if (!checked_mul(info.rows, info.columns, l.pixels)) return Status(Errc::LimitExceeded, "rle: frame too large");
    l.frame = info.frame_bytes();
    if (l.frame == 0) return Status(Errc::LimitExceeded, "rle: frame too large");
    return Status::ok();
}

// Where segment `seg` lands in the native frame: its first byte and the distance between bytes.
void segment_placement(const Layout& l, bool planar, std::uint32_t seg, std::uint64_t& base, std::uint64_t& stride) {
    const std::uint32_t sample = seg / l.bytes;
    const std::uint32_t byte = l.bytes - 1 - seg % l.bytes;  // little-endian position of this byte
    if (planar) {
        base = static_cast<std::uint64_t>(sample) * l.pixels * l.bytes + byte;
        stride = l.bytes;
    } else {
        base = static_cast<std::uint64_t>(sample) * l.bytes + byte;
        stride = static_cast<std::uint64_t>(l.samples) * l.bytes;
    }
}

// Decodes one segment into `need` bytes at dst, dst + stride, ... Data after the last needed byte
// (such as the even-length pad) is ignored; a run reaching past the end is cut at the end.
Status decode_segment(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t stride,
                      std::size_t need, std::uint64_t seg_offset) {
    std::size_t i = 0, k = 0;
    while (k < need) {
        if (i >= n)
            return Status(Errc::Truncated, "rle: segment decodes to fewer bytes than the frame needs", seg_offset + i);
        const std::uint8_t h = src[i++];
        if (h < 128) {
            const std::size_t count = std::min<std::size_t>(h + 1u, need - k);
            if (n - i < count) return Status(Errc::Truncated, "rle: literal run past segment end", seg_offset + i);
            if (stride == 1) {
                std::memcpy(dst + k, src + i, count);
            } else {
                std::uint8_t* d = dst + k * stride;
                for (std::size_t j = 0; j < count; ++j, d += stride) *d = src[i + j];
            }
            i += h + 1u;  // a cut-off literal run still consumes its bytes (only matters if k == need)
            k += count;
        } else if (h > 128) {
            if (i >= n) return Status(Errc::Truncated, "rle: replicate run past segment end", seg_offset + i);
            const std::uint8_t v = src[i++];
            const std::size_t count = std::min<std::size_t>(257u - h, need - k);
            if (stride == 1) {
                std::memset(dst + k, v, count);
            } else {
                std::uint8_t* d = dst + k * stride;
                for (std::size_t j = 0; j < count; ++j, d += stride) *d = v;
            }
            k += count;
        }
        // h == 128: no operation.
    }
    return Status::ok();
}

void put_literals(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) {
    while (n > 0) {
        const std::size_t c = std::min<std::size_t>(n, 128);
        out.push_back(static_cast<std::uint8_t>(c - 1));
        out.insert(out.end(), p, p + c);
        p += c;
        n -= c;
    }
}

// Run-length codes one row of one byte plane. Runs never cross the row (G.3.1).
void encode_row(const std::uint8_t* r, std::size_t n, std::vector<std::uint8_t>& out) {
    std::size_t i = 0, lit = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && run < 128 && r[i + run] == r[i]) ++run;
        // A pair breaks a literal run for no gain, so pairs only replicate outside one.
        if (run >= 3 || (run == 2 && lit == i)) {
            put_literals(r + lit, i - lit, out);
            out.push_back(static_cast<std::uint8_t>(257u - run));
            out.push_back(r[i]);
            i += run;
            lit = i;
        } else {
            i += run;
        }
    }
    put_literals(r + lit, n - lit, out);
}

}  // namespace

Status decode_rle(std::span<const std::uint8_t> in, const FrameInfo& info, std::span<std::uint8_t> out) {
    Layout l;
    if (Status s = make_layout(info, l); !s) return s;
    if (out.size() != l.frame)
        return Status(out.size() < l.frame ? Errc::BufferTooSmall : Errc::InvalidArgument,
                      "rle: output buffer does not match the frame size");
    if (in.size() < kHeaderSize) return Status(Errc::Truncated, "rle: header truncated");
    const std::uint32_t count = load_le32(in.data());
    if (count != l.segments)
        return Status(Errc::Malformed, "rle: segment count " + std::to_string(count) + " does not match " +
                                           std::to_string(l.segments) + " expected from the image attributes");
    std::uint32_t offsets[kMaxSegments] = {};
    for (std::uint32_t s = 0; s < count; ++s) {
        offsets[s] = load_le32(in.data() + 4 + 4 * s);
        const std::uint64_t lowest = s == 0 ? kHeaderSize : std::uint64_t{offsets[s - 1]} + 1;
        if (offsets[s] < lowest || offsets[s] > in.size())
            return Status(Errc::Malformed, "rle: segment offset out of order or out of range", 4 + 4 * s);
    }
    for (std::uint32_t s = 0; s < count; ++s) {
        const std::size_t start = offsets[s];
        const std::size_t end = s + 1 < count ? offsets[s + 1] : in.size();
        std::uint64_t base = 0, stride = 0;
        segment_placement(l, info.planar, s, base, stride);
        if (Status st = decode_segment(in.data() + start, end - start, out.data() + base,
                                       static_cast<std::size_t>(stride), static_cast<std::size_t>(l.pixels), start);
            !st)
            return st;
    }
    return Status::ok();
}

Status encode_rle(std::span<const std::uint8_t> in, const FrameInfo& info, const EncodeParams& p,
                  std::vector<std::uint8_t>& out) {
    out.clear();
    Layout l;
    if (Status s = make_layout(info, l); !s) return s;
    if (in.size() != l.frame) return Status(Errc::InvalidArgument, "rle: input does not match the frame size");
    try {
        out.assign(kHeaderSize, 0);
        store_le32(out.data(), l.segments);
        std::vector<std::uint8_t> row(info.columns);
        for (std::uint32_t s = 0; s < l.segments; ++s) {
            if (out.size() > UINT32_MAX) {
                out.clear();
                return Status(Errc::LimitExceeded, "rle: encoded frame exceeds 4 GiB offsets");
            }
            store_le32(out.data() + 4 + 4 * s, static_cast<std::uint32_t>(out.size()));
            std::uint64_t base = 0, stride = 0;
            segment_placement(l, info.planar, s, base, stride);
            const std::uint8_t* src = in.data() + base;
            for (std::uint32_t y = 0; y < info.rows; ++y) {
                const std::uint8_t* q = src + static_cast<std::uint64_t>(y) * info.columns * stride;
                for (std::uint32_t x = 0; x < info.columns; ++x) row[x] = q[static_cast<std::uint64_t>(x) * stride];
                encode_row(row.data(), row.size(), out);
            }
            if (out.size() & 1u) out.push_back(0);  // segments are padded to even length (G.3.1)
        }
    } catch (const std::bad_alloc&) {
        out.clear();
        return Status(Errc::OutOfMemory, "rle: out of memory");
    }
    return Status::ok();
}

}  // namespace siaara::codec
