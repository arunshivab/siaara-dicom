// SIAARA DICOM Toolkit - frame access. Written from DICOM PS3.5 8.1, 8.2, A.4 and PS3.3 C.7.6.3.1.8.
#include "image/frames.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <string>
#include <system_error>
#include <thread>

#include "codec/codec.hpp"

namespace siaara::image {

namespace {

constexpr std::uint64_t kItemHeader = 8;  // (FFFE,E000) tag + 32-bit length

bool starts_with(const Bytes& b, std::uint8_t m0, std::uint8_t m1) noexcept {
    return b.size() >= 2 && b.data()[0] == m0 && b.data()[1] == m1;
}

// JPEG, JPEG-LS and JPEG 2000 codestreams all end with FFD9 (EOI / EOC). Fragments may carry
// trailing 00 padding (PS3.5 A.4).
bool ends_with_eoi(const Bytes& b) noexcept {
    std::size_t n = b.size();
    while (n > 0 && b.data()[n - 1] == 0x00) --n;
    return n >= 2 && b.data()[n - 2] == 0xFF && b.data()[n - 1] == 0xD9;
}

// Does this fragment begin a codestream of the given codec?
bool starts_codestream(Codec c, const Bytes& b) noexcept {
    switch (c) {
        case Codec::JpegBaseline: case Codec::JpegExtended: case Codec::JpegLossless: case Codec::JpegLs:
            return starts_with(b, 0xFF, 0xD8);  // SOI
        case Codec::Jpeg2000: case Codec::Htj2k: {
            if (starts_with(b, 0xFF, 0x4F)) return true;  // SOC
            // JP2 file format signature box (rare in DICOM, but seen)
            static constexpr std::uint8_t jp2[8] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20};
            return b.size() >= 8 && std::memcmp(b.data(), jp2, 8) == 0;
        }
        default:
            return false;
    }
}

bool is_supported_codec(Codec c) noexcept {
    switch (c) {
        case Codec::None: case Codec::Rle: case Codec::JpegBaseline: case Codec::JpegExtended:
        case Codec::JpegLossless: case Codec::JpegLs: case Codec::Jpeg2000: case Codec::Htj2k:
            return true;
        default:
            return false;
    }
}

}  // namespace

void copy_bits(const std::uint8_t* in, std::uint64_t bit_offset, std::uint64_t bit_count, std::uint8_t* out) noexcept {
    const std::uint64_t nbytes = (bit_count + 7) / 8;
    if (nbytes == 0) return;
    const std::uint8_t* src = in + bit_offset / 8;
    const unsigned s = static_cast<unsigned>(bit_offset % 8);
    // Source bytes touched: ceil((s + bit_count) / 8); never read past them.
    const std::uint64_t src_bytes = (s + bit_count + 7) / 8;
    if (s == 0) {
        std::memcpy(out, src, static_cast<std::size_t>(nbytes));
    } else {
        for (std::uint64_t i = 0; i < nbytes; ++i) {
            unsigned v = static_cast<unsigned>(src[i]) >> s;
            if (i + 1 < src_bytes) v |= static_cast<unsigned>(src[i + 1]) << (8 - s);
            out[i] = static_cast<std::uint8_t>(v);
        }
    }
    const unsigned tail = static_cast<unsigned>(bit_count % 8);
    if (tail) out[nbytes - 1] = static_cast<std::uint8_t>(out[nbytes - 1] & ((1u << tail) - 1u));
}

Result<FrameReader> FrameReader::open(const Dataset& ds, std::string_view ts_uid, const FrameOptions& opt) {
    FrameReader r;
    r.opt_ = opt;
    PixelInfoOptions po;
    po.lenient = opt.lenient;
    auto pi = read_pixel_info(ds, po);
    if (!pi) return pi.take_error();
    r.info_ = std::move(pi).value();
    r.warnings_ = r.info_.warnings;

    r.ts_ = find_transfer_syntax(ts_uid);
    const Element* e = ds.find(r.info_.pixel_tag);
    if (!e) return Status(Errc::NotFound, "frames: pixel data missing", 0, r.info_.pixel_tag);
    if (e->is_sequence()) return Status(Errc::Malformed, "frames: pixel data is a sequence", 0, e->tag);
    r.encapsulated_ = e->is_encapsulated();

    if (r.encapsulated_) {
        if (r.info_.kind != SampleKind::Integer)
            return Status(Errc::Malformed, "frames: float pixel data cannot be encapsulated", 0, e->tag);
        if (!r.ts_) return Status(Errc::Unsupported, "frames: unknown transfer syntax for encapsulated pixel data");
        if (!r.ts_->encapsulated)
            return Status(Errc::Malformed, "frames: encapsulated pixel data in a native transfer syntax", 0, e->tag);
        r.codec_ = r.ts_->codec;
        if (!is_supported_codec(r.codec_))
            return Status(Errc::Unsupported, "frames: no decoder for transfer syntax " + std::string(r.ts_->name));
        r.frame_bytes_ = r.info_.decoded_frame_bytes();
        if (Status s = r.index_fragments(ds); !s) return s;
        return r;
    }

    // ---- native (also: Deflated, already inflated by the reader). Native pixel data in an
    // encapsulated transfer syntax is valid inside a sequence (Icon Image, PS3.5 A.4); the nesting
    // level is not known here, so it is accepted.
    r.codec_ = Codec::None;
    r.frame_bits_ = r.info_.native_frame_bits();
    r.frame_bytes_ = r.info_.native_frame_bytes();
    if (r.info_.native_subsampled() && (r.info_.columns % 2) != 0)
        return Status(Errc::Malformed, "frames: native " + r.info_.photometric_text + " needs an even number of columns",
                      0, siaara::tags::Columns);
    if (r.info_.photometric == Photometric::YbrPartial420 || r.info_.photometric == Photometric::YbrIct ||
        r.info_.photometric == Photometric::YbrRct) {
        if (!opt.lenient)
            return Status(Errc::Malformed, "frames: " + r.info_.photometric_text + " is only defined for encapsulated data",
                          0, siaara::tags::PhotometricInterpretation);
        if (r.info_.photometric == Photometric::YbrPartial420)
            return Status(Errc::Unsupported, "frames: native YBR_PARTIAL_420 has no defined layout", 0,
                          siaara::tags::PhotometricInterpretation);
        r.warnings_.push_back("native pixel data labelled " + r.info_.photometric_text);
    }
    r.native_ = e->value;
    std::uint64_t total_bits = 0;
    if (!checked_mul(r.frame_bits_, r.info_.number_of_frames, total_bits))
        return Status(Errc::LimitExceeded, "frames: total pixel data size overflows", 0, e->tag);
    const std::uint64_t needed = (total_bits + 7) / 8;
    if (r.native_.size() < needed) {
        if (!opt.lenient || r.native_.size() < r.frame_bytes_)
            return Status(Errc::Truncated,
                          "frames: pixel data holds " + std::to_string(r.native_.size()) + " bytes, " +
                              std::to_string(needed) + " expected",
                          0, e->tag);
        r.warnings_.push_back("pixel data shorter than Number of Frames requires; trailing frames unavailable");
    }

    // Big-endian input: the reader swapped the value in units of its VR (OW: 2 bytes, OF: 4, OD: 8,
    // OB: none). Samples wider than that unit still need their bytes put in little-endian order.
    const unsigned w = r.info_.bits_allocated / 8u;
    if (r.ts_ && r.ts_->big_endian && w >= 2) {
        unsigned ru = static_cast<unsigned>(vr_value_size(e->vr));
        if (ru == 0) ru = 1;
        if (ru != w && w % ru == 0) {
            // reader layout: buf[j] = orig[g(j)], g(i) = (i/ru)*ru + ru-1 - i%ru (an involution);
            // wanted: out[k] = orig[w-1-k] = buf[g(w-1-k)].
            for (unsigned k = 0; k < w; ++k) {
                const unsigned i = w - 1 - k;
                r.perm_[k] = static_cast<std::uint8_t>((i / ru) * ru + ru - 1 - i % ru);
            }
            r.permute_ = true;
        }
    }
    return r;
}

Status FrameReader::index_fragments(const Dataset& ds) {
    const Element* e = ds.find(info_.pixel_tag);
    if (e->fragments.empty())
        return Status(Errc::Malformed, "frames: encapsulated pixel data has no Basic Offset Table item", 0, e->tag);
    const Bytes& bot = e->fragments.front();
    fragments_.assign(e->fragments.begin() + 1, e->fragments.end());
    const std::uint32_t nframes = info_.number_of_frames;
    if (fragments_.empty()) return Status(Errc::Malformed, "frames: encapsulated pixel data has no fragments", 0, e->tag);
    if (fragments_.size() > UINT32_MAX) return Status(Errc::LimitExceeded, "frames: too many fragments");
    const auto nfrag = static_cast<std::uint32_t>(fragments_.size());

    // Byte position of each fragment's item tag, relative to the first fragment (PS3.5 A.4).
    std::vector<std::uint64_t> pos(nfrag + 1u, 0);
    for (std::uint32_t i = 0; i < nfrag; ++i) pos[i + 1] = pos[i] + kItemHeader + fragments_[i].size();

    auto from_offsets = [&](const std::vector<std::uint64_t>& offs, const char* what) -> Status {
        frames_.clear();
        frames_.reserve(offs.size());
        std::uint32_t frag = 0;
        for (std::size_t f = 0; f < offs.size(); ++f) {
            if (f > 0 && offs[f] <= offs[f - 1])
                return Status(Errc::Malformed, std::string("frames: ") + what + " offsets are not increasing");
            while (frag < nfrag && pos[frag] < offs[f]) ++frag;
            if (frag >= nfrag || pos[frag] != offs[f])
                return Status(Errc::Malformed,
                              std::string("frames: ") + what + " offset " + std::to_string(offs[f]) +
                                  " does not point to a fragment item tag");
            FrameSpan s;
            s.first = frag;
            frames_.push_back(s);
        }
        for (std::size_t f = 0; f < frames_.size(); ++f) {
            const std::uint32_t end = f + 1 < frames_.size() ? frames_[f + 1].first : nfrag;
            frames_[f].count = end - frames_[f].first;
        }
        return Status::ok();
    };

    // ---- 1. Extended Offset Table (PS3.3 C.7.6.3.1.8)
    const Element* eot = ds.find(tags::ExtendedOffsetTable);
    if (eot && !eot->value.empty()) {
        if (!bot.empty()) {
            if (!opt_.lenient)
                return Status(Errc::Malformed, "frames: Basic Offset Table must be empty with an Extended Offset Table");
            warnings_.push_back("Basic Offset Table present together with an Extended Offset Table; using the latter");
        }
        const Element* eotl = ds.find(tags::ExtendedOffsetTableLengths);
        if (eot->value.size() % 8 != 0)
            return Status(Errc::Malformed, "frames: Extended Offset Table length is not a multiple of 8", 0, eot->tag);
        const std::uint64_t n = eot->value.size() / 8;
        if (n != nframes)
            return Status(Errc::Malformed, "frames: Extended Offset Table has " + std::to_string(n) + " entries for " +
                                               std::to_string(nframes) + " frames",
                          0, eot->tag);
        if (!eotl || eotl->value.size() != eot->value.size())
            return Status(Errc::Malformed, "frames: Extended Offset Table Lengths missing or of the wrong size", 0,
                          tags::ExtendedOffsetTableLengths);
        std::vector<std::uint64_t> offs(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < offs.size(); ++i) offs[i] = load_le64(eot->value.data() + 8 * i);
        if (Status s = from_offsets(offs, "Extended Offset Table"); !s) return s;
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            frames_[i].length = load_le64(eotl->value.data() + 8 * i);
            std::uint64_t avail = 0;
            for (std::uint32_t k = 0; k < frames_[i].count; ++k) avail += fragments_[frames_[i].first + k].size();
            if (frames_[i].length > avail)
                return Status(Errc::Malformed, "frames: Extended Offset Table Length exceeds the frame's fragments", 0,
                              tags::ExtendedOffsetTableLengths);
        }
        return Status::ok();
    }

    // ---- 2. Basic Offset Table
    if (!bot.empty()) {
        if (bot.size() % 4 != 0)
            return Status(Errc::Malformed, "frames: Basic Offset Table length is not a multiple of 4", 0, e->tag);
        const std::size_t n = bot.size() / 4;
        if (n == nframes) {
            std::vector<std::uint64_t> offs(n);
            for (std::size_t i = 0; i < n; ++i) offs[i] = load_le32(bot.data() + 4 * i);
            Status s = from_offsets(offs, "Basic Offset Table");
            if (s) return s;
            if (!opt_.lenient) return s;
            warnings_.push_back(s.message + "; locating frames without it");
        } else {
            if (!opt_.lenient)
                return Status(Errc::Malformed, "frames: Basic Offset Table has " + std::to_string(n) + " entries for " +
                                                   std::to_string(nframes) + " frames");
            warnings_.push_back("Basic Offset Table entry count does not match Number of Frames; ignored");
        }
    }

    // ---- 3. No table
    frames_.clear();
    if (nframes == 1) {
        frames_.push_back(FrameSpan{0, nfrag, UINT64_MAX});
        return Status::ok();
    }
    if (nfrag == nframes) {
        for (std::uint32_t i = 0; i < nfrag; ++i) frames_.push_back(FrameSpan{i, 1, UINT64_MAX});
        return Status::ok();
    }
    if (nfrag < nframes)
        return Status(Errc::Malformed, "frames: " + std::to_string(nfrag) + " fragments for " + std::to_string(nframes) +
                                           " frames");
    if (codec_ == Codec::Rle)  // PS3.5 G.1: each frame is encoded in one and only one fragment
        return Status(Errc::Malformed, "frames: RLE requires exactly one fragment per frame");
    if (codec_ == Codec::None) {
        // Encapsulated Uncompressed: frames have a known size; accumulate fragments to that size.
        std::uint64_t acc = 0;
        FrameSpan cur{0, 0, UINT64_MAX};
        for (std::uint32_t i = 0; i < nfrag; ++i) {
            ++cur.count;
            acc += fragments_[i].size();
            if (acc >= frame_bytes_) {
                frames_.push_back(cur);
                cur = FrameSpan{i + 1, 0, UINT64_MAX};
                acc = 0;
            }
        }
        if (frames_.size() != nframes)
            return Status(Errc::Malformed, "frames: fragments do not divide into frames of the expected size");
        return Status::ok();
    }
    // Codestream boundary scan: a frame starts with a fragment that begins with the codec's start
    // marker (JPEG / JPEG-LS SOI, JPEG 2000 SOC) right after a fragment that ended with EOI/EOC.
    for (int pass = 0; pass < 2; ++pass) {
        frames_.clear();
        for (std::uint32_t i = 0; i < nfrag; ++i) {
            const bool start = i == 0 || (starts_codestream(codec_, fragments_[i]) &&
                                          (pass == 1 || ends_with_eoi(fragments_[i - 1])));
            if (start) frames_.push_back(FrameSpan{i, 0, UINT64_MAX});
            ++frames_.back().count;
        }
        if (frames_.size() == nframes) {
            if (pass == 1) warnings_.push_back("frame boundaries found from start markers only (no EOI before them)");
            return Status::ok();
        }
    }
    const std::size_t found = frames_.size();
    frames_.clear();
    return Status(Errc::Malformed, "frames: found " + std::to_string(found) + " codestreams in " +
                                       std::to_string(nfrag) + " fragments for " + std::to_string(nframes) + " frames");
}

std::uint32_t FrameReader::fragment_count(std::uint32_t index) const noexcept {
    return encapsulated_ && index < frames_.size() ? frames_[index].count : 0;
}

std::optional<std::span<const std::uint8_t>> FrameReader::native_view(std::uint32_t index) const noexcept {
    if (encapsulated_ || permute_ || index >= info_.number_of_frames || info_.bits_allocated == 1) return std::nullopt;
    const std::uint64_t off = std::uint64_t{index} * frame_bytes_;  // fits: checked at open()
    if (off + frame_bytes_ > native_.size()) return std::nullopt;
    return std::span<const std::uint8_t>(native_.data() + off, static_cast<std::size_t>(frame_bytes_));
}

Status FrameReader::decode(std::uint32_t index, std::span<std::uint8_t> out) const {
    if (index >= info_.number_of_frames)
        return Status(Errc::InvalidArgument, "frames: frame index " + std::to_string(index) + " out of range");
    if (out.size() < frame_bytes_) return Status(Errc::BufferTooSmall, "frames: output buffer too small");
    return encapsulated_ ? decode_encapsulated(index, out.first(static_cast<std::size_t>(frame_bytes_)))
                         : decode_native(index, out.first(static_cast<std::size_t>(frame_bytes_)));
}

Result<std::vector<std::uint8_t>> FrameReader::decode(std::uint32_t index) const {
    if (frame_bytes_ > opt_.memory_budget || frame_bytes_ > SIZE_MAX)
        return Status(Errc::LimitExceeded, "frames: frame exceeds the memory budget");
    std::vector<std::uint8_t> out;
    try {
        out.resize(static_cast<std::size_t>(frame_bytes_));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "frames: cannot allocate the frame buffer");
    }
    if (Status s = decode(index, out); !s) return s;
    return out;
}

Status FrameReader::decode_native(std::uint32_t index, std::span<std::uint8_t> out) const {
    const std::uint64_t bit_off = std::uint64_t{index} * frame_bits_;
    const std::uint64_t need_bytes = (bit_off + frame_bits_ + 7) / 8;
    if (need_bytes > native_.size())
        return Status(Errc::Truncated, "frames: frame " + std::to_string(index) + " lies past the end of the pixel data");
    if (info_.bits_allocated == 1) {
        copy_bits(native_.data(), bit_off, frame_bits_, out.data());
        return Status::ok();
    }
    const std::uint8_t* src = native_.data() + bit_off / 8;
    if (!permute_) {
        std::memcpy(out.data(), src, out.size());
        return Status::ok();
    }
    const unsigned w = info_.bits_allocated / 8u;
    for (std::size_t i = 0; i + w <= out.size(); i += w)
        for (unsigned k = 0; k < w; ++k) out[i + k] = src[i + perm_[k]];
    return Status::ok();
}

Status FrameReader::encoded_frame(std::uint32_t index, std::vector<std::uint8_t>& out) const {
    out.clear();
    if (!encapsulated_) return Status(Errc::InvalidArgument, "frames: pixel data is not encapsulated");
    if (index >= frames_.size()) return Status(Errc::InvalidArgument, "frames: frame index out of range");
    const FrameSpan& fs = frames_[index];
    std::uint64_t total = 0;
    for (std::uint32_t k = 0; k < fs.count; ++k) total += fragments_[fs.first + k].size();
    total = std::min(total, fs.length);
    if (total > opt_.memory_budget) return Status(Errc::LimitExceeded, "frames: encoded frame exceeds the memory budget");
    try {
        out.reserve(static_cast<std::size_t>(total));
        for (std::uint32_t k = 0; k < fs.count && out.size() < total; ++k) {
            const Bytes& b = fragments_[fs.first + k];
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), total - out.size()));
            out.insert(out.end(), b.data(), b.data() + take);
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "frames: cannot allocate the encoded frame");
    }
    return Status::ok();
}

Status FrameReader::decode_encapsulated(std::uint32_t index, std::span<std::uint8_t> out) const {
    if (index >= frames_.size()) return Status(Errc::InvalidArgument, "frames: frame index out of range");
    const FrameSpan& fs = frames_[index];
    std::span<const std::uint8_t> data;
    std::vector<std::uint8_t> joined;
    if (fs.count == 1) {
        const Bytes& b = fragments_[fs.first];
        data = std::span<const std::uint8_t>(b.data(), static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), fs.length)));
    } else {
        if (Status s = encoded_frame(index, joined); !s) return s;
        data = joined;
    }
    const codec::FrameInfo fi = info_.frame_info();
    Status st;
    switch (codec_) {
        case Codec::None:  // Encapsulated Uncompressed Explicit VR Little Endian
            if (data.size() < out.size())
                return Status(Errc::Truncated, "frames: encapsulated uncompressed frame is too short");
            std::memcpy(out.data(), data.data(), out.size());
            return Status::ok();
        case Codec::Rle: st = codec::decode_rle(data, fi, out); break;
        case Codec::JpegBaseline: case Codec::JpegExtended: case Codec::JpegLossless:
            st = codec::decode_jpeg(data, fi, out);
            break;
        case Codec::JpegLs: st = codec::decode_jpegls(data, fi, out); break;
        case Codec::Jpeg2000: case Codec::Htj2k: st = codec::decode_j2k(data, fi, out); break;
        default: return Status(Errc::Unsupported, "frames: no decoder for this transfer syntax");
    }
    if (!st) st.message = "frame " + std::to_string(index) + ": " + st.message;
    return st;
}

Status FrameReader::decode_range(std::uint32_t first, std::uint32_t count, std::span<std::uint8_t> out) const {
    if (count == 0) return Status::ok();
    if (first >= info_.number_of_frames || count > info_.number_of_frames - first)
        return Status(Errc::InvalidArgument, "frames: frame range out of range");
    std::uint64_t total = 0;
    if (!checked_mul(frame_bytes_, count, total)) return Status(Errc::LimitExceeded, "frames: range size overflows");
    if (out.size() < total) return Status(Errc::BufferTooSmall, "frames: output buffer too small");
    const auto fb = static_cast<std::size_t>(frame_bytes_);

    unsigned threads = opt_.threads;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 1;
        threads = std::min(threads, 8u);
    }
    threads = static_cast<unsigned>(std::min<std::uint64_t>(threads, count));
    if (threads <= 1) {
        for (std::uint32_t i = 0; i < count; ++i)
            if (Status s = decode(first + i, out.subspan(i * fb, fb)); !s) return s;
        return Status::ok();
    }

    std::vector<Status> results;
    try {
        results.resize(count);
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "frames: out of memory");
    }
    std::atomic<std::uint32_t> next{0};
    std::atomic<bool> stop{false};
    // Frames are claimed in increasing order, so when a frame fails every lower frame has already
    // been claimed and will finish: the lowest failing index is reported deterministically.
    auto work = [&]() noexcept {
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) return;
            const std::uint32_t i = next.fetch_add(1);
            if (i >= count) return;
            Status s = decode(first + i, out.subspan(i * fb, fb));
            if (!s) stop.store(true, std::memory_order_relaxed);
            results[i] = std::move(s);
        }
    };
    std::vector<std::thread> pool;
    try {
        pool.reserve(threads - 1);
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    } catch (...) {
        // Could not start (all) workers: the calling thread does the remaining work.
    }
    work();
    for (auto& t : pool) t.join();
    for (auto& s : results)
        if (!s) return s;
    return Status::ok();
}

Result<std::vector<std::uint8_t>> FrameReader::decode_range(std::uint32_t first, std::uint32_t count) const {
    std::uint64_t total = 0;
    if (!checked_mul(frame_bytes_, count, total) || total > opt_.memory_budget || total > SIZE_MAX)
        return Status(Errc::LimitExceeded, "frames: frame range exceeds the memory budget");
    std::vector<std::uint8_t> out;
    try {
        out.resize(static_cast<std::size_t>(total));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "frames: cannot allocate the frame buffer");
    }
    if (Status s = decode_range(first, count, out); !s) return s;
    return out;
}

}  // namespace siaara::image
