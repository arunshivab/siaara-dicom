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
#include "codec/j2k.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpegls.hpp"

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

// ---------------------------------------------------------------- lenient codestream handling
// The codecs refuse a codestream whose own header disagrees with the Image Pixel module
// (codec.hpp). In lenient mode the frame layer probes the header first; when it disagrees in a way
// that can be resolved, the frame is decoded with the codestream's own parameters into a scratch
// buffer and converted to the layout the data set describes.

enum class Family { None, Jpeg, JpegLs, J2k };

Family codec_family(Codec c) noexcept {
    switch (c) {
        case Codec::JpegBaseline: case Codec::JpegExtended: case Codec::JpegLossless: return Family::Jpeg;
        case Codec::JpegLs: return Family::JpegLs;
        case Codec::Jpeg2000: case Codec::Htj2k: return Family::J2k;
        default: return Family::None;
    }
}

struct StreamHeader {
    std::uint32_t width = 0;
    std::uint32_t height = 0;  // 0: JPEG height defined by DNL
    int components = 0;
    int precision = 0;
    bool has_sign = false;     // the codestream signals signedness (JPEG 2000 only)
    bool is_signed = false;
    bool dct = false;          // JPEG DCT process (samples are returned unsigned)
};

Status probe_header(Family f, std::span<const std::uint8_t> data, StreamHeader& h) {
    h = StreamHeader{};
    if (f == Family::J2k) {
        codec::j2k::StreamInfo si;
        if (Status s = codec::j2k::probe(data, si); !s) return s;
        h.width = si.width;
        h.height = si.height;
        h.components = si.components;
        h.precision = si.precision;
        h.has_sign = true;
        h.is_signed = si.is_signed;
        return Status::ok();
    }
    if (f == Family::JpegLs) {
        codec::jpegls::StreamInfo si;
        if (Status s = codec::jpegls::probe(data, si); !s) return s;
        h.width = si.width;
        h.height = si.height;
        h.components = si.components;
        h.precision = si.precision;
        return Status::ok();
    }
    if (f == Family::Jpeg) {
        codec::JpegHeaderInfo hd;
        if (Status s = codec::jpeg_read_header(data, hd); !s) return s;
        // Only the Huffman processes the decoder implements can be adjusted.
        if (hd.sof_marker < 0xC0 || hd.sof_marker > 0xC3)
            return Status(Errc::Unsupported, "frames: JPEG process not adjustable");
        h.width = hd.columns;
        h.height = hd.rows;
        h.components = hd.components;
        h.precision = hd.precision;
        h.dct = !hd.lossless;
        return Status::ok();
    }
    return Status(Errc::Unsupported, "frames: no header probe for this codec");
}

// How to decode one frame.
struct Plan {
    bool convert = false;      // decode into `temp` and convert; otherwise decode directly
    codec::FrameInfo temp;     // the codestream's own parameters
    bool temp_signed = false;  // scratch samples are two's complement in their container
    bool reinterpret = false;  // reinterpret the low `precision` bits as Pixel Representation says
    bool check_fit = false;    // values may exceed the (effective) Bits Stored: verify them
    int precision = 0;
};

std::string size_text(std::uint32_t w, std::uint32_t h) { return std::to_string(w) + "x" + std::to_string(h); }

bool codec_supports_container(Family f, unsigned ba) noexcept {
    if (f == Family::J2k) return ba == 8 || ba == 16 || ba == 32;
    return ba == 8 || ba == 16;
}

// `info` holds the effective attributes (Bits Stored possibly raised at open()); warnings are
// worded against the declared ones.
Status make_plan(Family f, const StreamHeader& h, const PixelInfo& info, unsigned declared_bs, std::uint32_t frame,
                 Plan& p, std::vector<FrameWarning>* w) {
    p = Plan{};
    const std::uint32_t rows = info.rows, cols = info.columns;
    const std::uint32_t height = h.height != 0 ? h.height : rows;
    const unsigned spp = info.samples_per_pixel, ba = info.bits_allocated, bs = info.bits_stored;
    const int prec = h.precision;
    const std::string where = "frame " + std::to_string(frame) + ": ";
    if (h.width < cols || height < rows)
        return Status(Errc::Malformed, where + "codestream image " + size_text(h.width, height) +
                                           " is smaller than Columns x Rows " + size_text(cols, rows));
    if (h.components < static_cast<int>(spp))
        return Status(Errc::Malformed, where + "codestream has " + std::to_string(h.components) +
                                           " components for Samples per Pixel " + std::to_string(spp));
    if (prec < 2 || prec > 16)
        return Status(Errc::Unsupported, where + "codestream precision " + std::to_string(prec) + " is not supported");
    if (ba != 8 && ba != 16 && ba != 32)
        return Status(Errc::Unsupported, where + "Bits Allocated " + std::to_string(ba) + " with a compressed codestream");

    auto warn = [&](FrameWarningCode code, std::string msg) {
        if (w) w->push_back(FrameWarning{code, frame, std::move(msg)});
    };
    if (h.width > cols || height > rows)
        warn(FrameWarningCode::CodestreamCropped, "codestream image " + size_text(h.width, height) +
                                                      " cropped to Columns x Rows " + size_text(cols, rows));
    if (h.components > static_cast<int>(spp))
        warn(FrameWarningCode::ExtraComponents, "codestream has " + std::to_string(h.components) +
                                                    " components; the first " + std::to_string(spp) + " are used");
    if (f != Family::Jpeg) {  // JPEG precision legitimately differs from Bits Stored (PS3.5 8.2.1)
        if (prec < static_cast<int>(declared_bs))
            warn(FrameWarningCode::PrecisionBelowBitsStored,
                 "codestream precision " + std::to_string(prec) + " below Bits Stored " +
                     std::to_string(declared_bs) + "; values used as decoded");
        else if (prec > static_cast<int>(declared_bs) && prec <= static_cast<int>(ba))
            warn(FrameWarningCode::PrecisionAboveBitsStored,
                 "codestream precision " + std::to_string(prec) + " above Bits Stored " +
                     std::to_string(declared_bs) + "; values kept, precision " + std::to_string(bs) +
                     " used for display");
    }
    if (prec > static_cast<int>(ba))
        warn(FrameWarningCode::PrecisionAboveBitsAllocated,
             "codestream precision " + std::to_string(prec) + " above Bits Allocated " + std::to_string(ba) +
                 "; values must fit " + std::to_string(bs) + " bits");
    if (h.has_sign && h.is_signed != info.is_signed)
        warn(FrameWarningCode::SignednessMismatch,
             std::string("codestream samples are ") + (h.is_signed ? "signed" : "unsigned") +
                 ", Pixel Representation is " + (info.is_signed ? "1" : "0") + "; low " + std::to_string(prec) +
                 " bits reinterpreted per Pixel Representation");

    // Would the codec accept the frame as described (codec.hpp rules)?
    bool consistent = h.width == cols && (h.height == 0 || h.height == rows) &&
                      h.components == static_cast<int>(spp) && codec_supports_container(f, ba);
    if (f == Family::J2k)
        consistent = consistent && prec == static_cast<int>(bs) && h.is_signed == info.is_signed &&
                     prec <= static_cast<int>(ba);
    else if (f == Family::JpegLs)
        consistent = consistent && prec == static_cast<int>(bs) && prec <= static_cast<int>(ba);
    else
        consistent = consistent && prec <= static_cast<int>(ba) && (prec <= 8 || ba == 16);
    if (consistent) return Status::ok();

    const unsigned temp_ba = prec <= 8 ? 8u : 16u;
    if (!codec_supports_container(f, ba))
        warn(FrameWarningCode::ContainerWidened, "decoded as " + std::to_string(temp_ba) + "-bit samples into Bits Allocated " +
                                                     std::to_string(ba));
    p.convert = true;
    p.temp.rows = height;
    p.temp.columns = h.width;
    p.temp.samples_per_pixel = static_cast<std::uint16_t>(h.components);
    p.temp.bits_allocated = static_cast<std::uint16_t>(temp_ba);
    p.temp.bits_stored = static_cast<std::uint16_t>(prec);
    p.temp.is_signed = h.has_sign ? h.is_signed : info.is_signed;
    p.temp.planar = false;
    p.temp_signed = h.has_sign ? h.is_signed : (info.is_signed && !h.dct);
    p.reinterpret = h.has_sign && h.is_signed != info.is_signed;
    p.check_fit = prec > static_cast<int>(bs);
    p.precision = prec;
    return Status::ok();
}

// Scratch frame (codestream parameters, colour-by-pixel) -> the data set's layout.
Status convert_frame(const Plan& p, const PixelInfo& info, std::span<const std::uint8_t> tmp,
                     std::span<std::uint8_t> out, std::uint32_t frame) {
    const std::size_t tb = p.temp.bits_allocated / 8u, ob = info.bits_allocated / 8u;
    const std::size_t tw = p.temp.columns, tc = p.temp.samples_per_pixel;
    const std::size_t rows = info.rows, cols = info.columns, spp = info.samples_per_pixel;
    const unsigned bs = info.bits_stored;
    const std::int64_t lo = info.is_signed ? -(std::int64_t{1} << (bs - 1)) : 0;
    const std::int64_t hi = info.is_signed ? (std::int64_t{1} << (bs - 1)) - 1 : (std::int64_t{1} << bs) - 1;
    const std::uint32_t pmask = static_cast<std::uint32_t>((std::uint64_t{1} << p.precision) - 1u);
    const std::uint32_t psign = std::uint32_t{1} << (p.precision - 1);
    for (std::size_t y = 0; y < rows; ++y) {
        for (std::size_t x = 0; x < cols; ++x) {
            for (std::size_t c = 0; c < spp; ++c) {
                const std::uint8_t* s = tmp.data() + ((y * tw + x) * tc + c) * tb;
                const std::uint32_t raw = tb == 1 ? s[0] : load_le16(s);
                std::int64_t v = raw;
                if (p.temp_signed)
                    v = tb == 1 ? static_cast<std::int64_t>(static_cast<std::int8_t>(raw))
                                : static_cast<std::int64_t>(static_cast<std::int16_t>(raw));
                if (p.reinterpret) {
                    const std::uint32_t bits = static_cast<std::uint32_t>(v) & pmask;
                    v = (info.is_signed && (bits & psign)) ? static_cast<std::int64_t>(bits) - (std::int64_t{1} << p.precision)
                                                           : static_cast<std::int64_t>(bits);
                }
                if (p.check_fit && (v < lo || v > hi))
                    return Status(Errc::Malformed, "frame " + std::to_string(frame) + ": decoded value " +
                                                       std::to_string(v) + " does not fit Bits Stored " +
                                                       std::to_string(bs));
                const std::size_t idx = info.planar ? (c * rows + y) * cols + x : (y * cols + x) * spp + c;
                std::uint8_t* d = out.data() + idx * ob;
                const auto u = static_cast<std::uint32_t>(v);
                if (ob == 1) d[0] = static_cast<std::uint8_t>(u);
                else if (ob == 2) store_le16(d, static_cast<std::uint16_t>(u));
                else store_le32(d, u);
            }
        }
    }
    return Status::ok();
}

Status run_codec(Codec c, std::span<const std::uint8_t> data, const codec::FrameInfo& fi, std::span<std::uint8_t> out) {
    switch (c) {
        case Codec::Rle: return codec::decode_rle(data, fi, out);
        case Codec::JpegBaseline: case Codec::JpegExtended: case Codec::JpegLossless: return codec::decode_jpeg(data, fi, out);
        case Codec::JpegLs: return codec::decode_jpegls(data, fi, out);
        case Codec::Jpeg2000: case Codec::Htj2k: return codec::decode_j2k(data, fi, out);
        default: return Status(Errc::Unsupported, "frames: no decoder for this transfer syntax");
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
    r.declared_bits_stored_ = r.info_.bits_stored;

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
        if (opt.lenient) r.probe_first_frame();
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
        // The core reader already swaps 32/64-bit samples in OW Pixel Data per sample.
        if (e->vr == VR::OW && e->tag == siaara::tags::PixelData && (w == 4 || w == 8)) ru = w;
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

void FrameReader::probe_first_frame() {
    const Family f = codec_family(codec_);
    if (f == Family::None || frames_.empty()) return;
    std::span<const std::uint8_t> data;
    std::vector<std::uint8_t> joined;
    if (!frame_data(0, data, joined)) return;
    StreamHeader h;
    if (!probe_header(f, data, h)) return;  // an unreadable header is reported when decoding
    const int ba = info_.bits_allocated;
    // Display range: the codestream precision when JPEG 2000 / JPEG-LS carries more bits than Bits
    // Stored says, Bits Allocated when it carries more than that.
    int bs = info_.bits_stored;
    if (h.precision > ba && (ba == 8 || ba == 16 || ba == 32)) bs = ba;
    else if (f != Family::Jpeg && h.precision > bs && h.precision <= ba) bs = h.precision;
    if (bs != info_.bits_stored) {
        info_.bits_stored = static_cast<std::uint16_t>(bs);
        info_.high_bit = static_cast<std::uint16_t>(bs - 1);
    }
    Plan p;
    std::vector<FrameWarning> w;
    if (!make_plan(f, h, info_, declared_bits_stored_, 0, p, &w)) return;  // reported when decoding
    for (const auto& x : w) warnings_.push_back("frame 0: " + x.message);
    frame_warnings_ = std::move(w);
}

Status FrameReader::frame_data(std::uint32_t index, std::span<const std::uint8_t>& data,
                               std::vector<std::uint8_t>& joined) const {
    if (index >= frames_.size()) return Status(Errc::InvalidArgument, "frames: frame index out of range");
    const FrameSpan& fs = frames_[index];
    if (fs.count == 1) {
        const Bytes& b = fragments_[fs.first];
        data = std::span<const std::uint8_t>(b.data(), static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), fs.length)));
        return Status::ok();
    }
    if (Status s = encoded_frame(index, joined); !s) return s;
    data = joined;
    return Status::ok();
}

Status FrameReader::check_frame(std::uint32_t index, std::vector<FrameWarning>& out) const {
    out.clear();
    if (index >= info_.number_of_frames)
        return Status(Errc::InvalidArgument, "frames: frame index " + std::to_string(index) + " out of range");
    const Family f = codec_family(codec_);
    if (!encapsulated_ || !opt_.lenient || f == Family::None) return Status::ok();
    std::span<const std::uint8_t> data;
    std::vector<std::uint8_t> joined;
    if (Status s = frame_data(index, data, joined); !s) return s;
    StreamHeader h;
    if (!probe_header(f, data, h)) return Status::ok();
    Plan p;
    return make_plan(f, h, info_, declared_bits_stored_, index, p, &out);
}

Status FrameReader::decode(std::uint32_t index, std::span<std::uint8_t> out) const {
    return decode(index, out, nullptr);
}

Status FrameReader::decode(std::uint32_t index, std::span<std::uint8_t> out, std::vector<FrameWarning>* warnings) const {
    if (warnings) warnings->clear();
    if (index >= info_.number_of_frames)
        return Status(Errc::InvalidArgument, "frames: frame index " + std::to_string(index) + " out of range");
    if (out.size() < frame_bytes_) return Status(Errc::BufferTooSmall, "frames: output buffer too small");
    return encapsulated_ ? decode_encapsulated(index, out.first(static_cast<std::size_t>(frame_bytes_)), warnings)
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

Status FrameReader::decode_encapsulated(std::uint32_t index, std::span<std::uint8_t> out,
                                        std::vector<FrameWarning>* warnings) const {
    std::span<const std::uint8_t> data;
    std::vector<std::uint8_t> joined;
    if (Status s = frame_data(index, data, joined); !s) return s;
    if (codec_ == Codec::None) {  // Encapsulated Uncompressed Explicit VR Little Endian
        if (data.size() < out.size())
            return Status(Errc::Truncated, "frames: encapsulated uncompressed frame is too short");
        std::memcpy(out.data(), data.data(), out.size());
        return Status::ok();
    }
    const Family f = codec_family(codec_);
    Status st;
    StreamHeader h;
    Plan plan;
    if (opt_.lenient && f != Family::None && probe_header(f, data, h)) {
        if (Status s = make_plan(f, h, info_, declared_bits_stored_, index, plan, warnings); !s) return s;
    }
    if (!plan.convert) {
        st = run_codec(codec_, data, info_.frame_info(), out);
    } else {
        const std::uint64_t tb = plan.temp.frame_bytes();
        if (tb == 0 || tb > opt_.memory_budget || tb > SIZE_MAX)
            return Status(Errc::LimitExceeded, "frame " + std::to_string(index) + ": codestream image exceeds the memory budget");
        std::vector<std::uint8_t> tmp;
        try {
            tmp.resize(static_cast<std::size_t>(tb));
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "frames: cannot allocate the scratch frame");
        }
        st = run_codec(codec_, data, plan.temp, tmp);
        if (st) return convert_frame(plan, info_, tmp, out, index);
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
