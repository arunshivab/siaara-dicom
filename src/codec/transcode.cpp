// SIAARA DICOM Toolkit - pixel data transcoding (PS3.5 8, 10, A.4; PS3.3 C.7.6.3, C.7.6.1.1.5).
#include "codec/transcode.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/j2k.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpegls.hpp"
#include "core/transfer_syntax.hpp"
#include "image/color.hpp"
#include "image/pixel_info.hpp"

namespace siaara::codec {

namespace {

using image::Photometric;

constexpr Tag kLossyRatio = 0x00282112;
constexpr Tag kLossyMethod = 0x00282114;
constexpr Tag kIconImageSequence = 0x00880200;
constexpr Tag kExtendedOffsetTable = 0x7FE00001;
constexpr Tag kExtendedOffsetTableLengths = 0x7FE00002;

enum class Target {
    Native,        // implicit / explicit LE, explicit BE, deflated
    EncapsulatedRaw,  // 1.2.840.10008.1.2.1.98
    Rle,
    JpegBaseline,
    JpegExtended,
    JpegLossless,     // .57 (any predictor)
    JpegLosslessSv1,  // .70
    JpegLsLossless,   // .80
    JpegLsNear,       // .81
    J2kLossless,      // .90
    J2k,              // .91
    HtLossless,       // .201
    HtRpcl,           // .202
    Ht,               // .203
    None,
};

Target target_of(std::string_view uid) noexcept {
    if (uid == ts::ImplicitVRLittleEndian || uid == ts::ExplicitVRLittleEndian || uid == ts::ExplicitVRBigEndian ||
        uid == ts::DeflatedExplicitVRLittleEndian)
        return Target::Native;
    if (uid == ts::EncapsulatedUncompressedExplicitVRLittleEndian) return Target::EncapsulatedRaw;
    if (uid == "1.2.840.10008.1.2.5") return Target::Rle;
    if (uid == "1.2.840.10008.1.2.4.50") return Target::JpegBaseline;
    if (uid == "1.2.840.10008.1.2.4.51") return Target::JpegExtended;
    if (uid == "1.2.840.10008.1.2.4.57") return Target::JpegLossless;
    if (uid == "1.2.840.10008.1.2.4.70") return Target::JpegLosslessSv1;
    if (uid == "1.2.840.10008.1.2.4.80") return Target::JpegLsLossless;
    if (uid == "1.2.840.10008.1.2.4.81") return Target::JpegLsNear;
    if (uid == "1.2.840.10008.1.2.4.90") return Target::J2kLossless;
    if (uid == "1.2.840.10008.1.2.4.91") return Target::J2k;
    if (uid == "1.2.840.10008.1.2.4.201") return Target::HtLossless;
    if (uid == "1.2.840.10008.1.2.4.202") return Target::HtRpcl;
    if (uid == "1.2.840.10008.1.2.4.203") return Target::Ht;
    return Target::None;
}

bool target_lossy(Target t, const TranscodeParams& p) noexcept {
    switch (t) {
        case Target::JpegBaseline: case Target::JpegExtended: return true;
        case Target::JpegLsNear: return p.lossy && p.near_lossless > 0;
        case Target::J2k: case Target::Ht: return p.lossy;
        default: return false;
    }
}

std::string_view lossy_method(Target t) noexcept {
    switch (t) {
        case Target::JpegBaseline: case Target::JpegExtended: return "ISO_10918_1";
        case Target::JpegLsNear: return "ISO_14495_1";
        case Target::J2k: return "ISO_15444_1";
        case Target::Ht: return "ISO_15444_15";
        default: return "";
    }
}

// Ratio as a DS value with two decimals ("12.34"), without floating-point formatting.
std::string format_ratio(std::uint64_t native, std::uint64_t encoded) {
    if (encoded == 0) return "1";
    // hundredths = round(native * 100 / encoded), computed without overflow for realistic sizes.
    const long double r = static_cast<long double>(native) * 100.0L / static_cast<long double>(encoded);
    std::uint64_t h = static_cast<std::uint64_t>(std::min<long double>(r + 0.5L, 9.0e14L));
    std::string s = std::to_string(h / 100) + ".";
    const std::uint64_t frac = h % 100;
    if (frac < 10) s += '0';
    s += std::to_string(frac);
    return s;
}

// Appends one value to a multi-valued string element (keeps the existing values).
Status append_value(Dataset& ds, Tag tag, VR vr, std::string_view value) {
    std::string v;
    if (auto old = ds.get_raw_string(tag); old && !old->empty()) {
        v.assign(old->begin(), old->end());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.pop_back();
        if (!v.empty()) v += '\\';
    }
    v += value;
    return ds.set_string(tag, vr, v);
}

bool is_lossy_attribute(const Dataset& ds) {
    auto v = ds.get_string(image::tags::LossyImageCompression);
    return v && *v == "01";
}

// Inserts a TLM marker segment (T.800 A.7.1) before the first SOT, with one entry per tile-part
// (Ttlm 16 bits, Ptlm 32 bits). Needed by the HTJ2K RPCL transfer syntax (PS3.5 10.18).
Status insert_tlm(std::vector<std::uint8_t>& cs) {
    const std::size_t n = cs.size();
    if (n < 4 || cs[0] != 0xFF || cs[1] != 0x4F) return Status(Errc::Internal, "TLM: not a J2K codestream");
    std::size_t pos = 2;
    std::size_t first_sot = 0;
    while (pos + 4 <= n) {
        if (cs[pos] != 0xFF) return Status(Errc::Internal, "TLM: marker expected");
        const std::uint8_t m = cs[pos + 1];
        if (m == 0x90) {
            first_sot = pos;
            break;
        }
        const std::size_t len = load_be16(cs.data() + pos + 2);
        if (len < 2 || pos + 2 + len > n) return Status(Errc::Internal, "TLM: bad marker length");
        pos += 2 + len;
    }
    if (first_sot == 0) return Status(Errc::Internal, "TLM: no SOT");
    std::vector<std::pair<std::uint16_t, std::uint32_t>> parts;
    pos = first_sot;
    while (pos + 12 <= n && cs[pos] == 0xFF && cs[pos + 1] == 0x90) {
        const std::uint16_t isot = load_be16(cs.data() + pos + 4);
        std::uint64_t psot = load_be32(cs.data() + pos + 6);
        if (psot == 0) psot = n - 2 - pos;  // last tile-part runs to EOC
        if (psot < 12 || pos + psot > n) return Status(Errc::Internal, "TLM: bad Psot");
        parts.emplace_back(isot, static_cast<std::uint32_t>(psot));
        pos += static_cast<std::size_t>(psot);
    }
    if (parts.empty() || parts.size() > 10000) return Status(Errc::Unsupported, "TLM: tile-part count");
    std::vector<std::uint8_t> seg;
    const std::size_t ltlm = 4 + 6 * parts.size();
    seg.reserve(ltlm + 2);
    seg.push_back(0xFF);
    seg.push_back(0x55);
    seg.push_back(static_cast<std::uint8_t>(ltlm >> 8));
    seg.push_back(static_cast<std::uint8_t>(ltlm));
    seg.push_back(0);     // Ztlm
    seg.push_back(0x60);  // Stlm: ST = 2 (16-bit Ttlm), SP = 1 (32-bit Ptlm)
    for (const auto& [t, len] : parts) {
        seg.push_back(static_cast<std::uint8_t>(t >> 8));
        seg.push_back(static_cast<std::uint8_t>(t));
        for (int k = 3; k >= 0; --k) seg.push_back(static_cast<std::uint8_t>(len >> (8 * k)));
    }
    cs.insert(cs.begin() + static_cast<std::ptrdiff_t>(first_sot), seg.begin(), seg.end());
    return Status::ok();
}

// Probes the first encoded frame for lossy coding that the transfer syntax does not reveal.
bool codestream_lossy(const image::FrameReader& fr) {
    switch (fr.codec()) {
        case Codec::JpegBaseline: case Codec::JpegExtended: return true;
        case Codec::Jpeg2000: case Codec::Htj2k: {
            std::vector<std::uint8_t> f;
            if (fr.frame_count() == 0 || !fr.encoded_frame(0, f)) return false;
            j2k::StreamInfo si;
            return j2k::probe(f, si).is_ok() && !si.reversible;
        }
        case Codec::JpegLs: {
            std::vector<std::uint8_t> f;
            if (fr.frame_count() == 0 || !fr.encoded_frame(0, f)) return false;
            jpegls::StreamInfo si;
            return jpegls::probe(f, si).is_ok() && si.near > 0;
        }
        default: return false;
    }
}

struct Plan {
    Target target = Target::None;
    image::PixelInfo in;            // source description
    Photometric work = Photometric::Unknown;  // photometric of the normalised work frame
    bool expand_422 = false;        // native YBR 4:2:2 source: expand to full samples
    bool deplanarise = false;       // source frames are colour-by-plane
    bool to_rgb = false;            // convert YBR work frames to RGB
    bool codec_mct = false;         // the decoder already undid RCT/ICT
    bool native_layout_ybr = false; // to_rgb input is in native layout (never after expansion)
    Photometric out = Photometric::Unknown;
    bool ybr_encode = false;        // JPEG lossy: encoder converts RGB to YBR
    bool mct = false;               // J2K: apply the multi-component transform
    bool lossy = false;
};

codec::FrameInfo work_info(const image::PixelInfo& pi) {
    codec::FrameInfo fi = pi.frame_info();
    fi.planar = false;
    return fi;
}

Status encode_frame(const Plan& plan, const TranscodeParams& p, std::span<const std::uint8_t> frame,
                    const codec::FrameInfo& fi, std::vector<std::uint8_t>& out) {
    out.clear();
    switch (plan.target) {
        case Target::EncapsulatedRaw:
            out.assign(frame.begin(), frame.end());
            return Status::ok();
        case Target::Rle: return encode_rle(frame, fi, EncodeParams{}, out);
        case Target::JpegLossless: case Target::JpegLosslessSv1: {
            EncodeParams ep;
            ep.lossless = true;
            ep.predictor = plan.target == Target::JpegLosslessSv1 ? 1 : p.predictor;
            return encode_jpeg(frame, fi, ep, out);
        }
        case Target::JpegBaseline: case Target::JpegExtended: {
            EncodeParams ep;
            ep.lossless = false;
            ep.quality = p.quality;
            ep.ybr = plan.ybr_encode;
            ep.chroma_subsampling = fi.samples_per_pixel == 3 ? p.chroma_subsampling : 0;
            return encode_jpeg(frame, fi, ep, out);
        }
        case Target::JpegLsLossless: case Target::JpegLsNear: {
            jpegls::EncodeOptions o;
            o.near = plan.lossy ? p.near_lossless : 0;
            return jpegls::encode(frame, fi, o, out);
        }
        case Target::J2kLossless: case Target::J2k: case Target::HtLossless: case Target::HtRpcl:
        case Target::Ht: {
            j2k::EncodeOptions o;
            o.lossless = !plan.lossy;
            o.quality = p.quality;
            o.mct = plan.mct;
            o.ht = plan.target == Target::HtLossless || plan.target == Target::HtRpcl || plan.target == Target::Ht;
            if (plan.target == Target::HtRpcl) {
                // PS3.5 10.18: RPCL progression, base resolution <= 64, TLM present.
                o.progression = j2k::Progression::RPCL;
                const std::uint32_t dim = std::max(fi.rows, fi.columns);
                int levels = 5;
                while (levels < 32 && ((static_cast<std::uint64_t>(dim) + (1ull << levels) - 1) >> levels) > 64) ++levels;
                o.levels = levels;
            }
            if (Status s = j2k::encode(frame, fi, o, out); !s) return s;
            if (plan.target == Target::HtRpcl) return insert_tlm(out);
            return Status::ok();
        }
        default: return Status(Errc::Internal, "transcode: no encoder for target");
    }
}

}  // namespace

bool can_encode(std::string_view ts_uid) noexcept { return target_of(ts_uid) != Target::None; }

bool can_decode(std::string_view ts_uid) noexcept {
    const TransferSyntaxInfo* t = find_transfer_syntax(ts_uid);
    if (!t) return false;
    if (!t->encapsulated) return t->codec == Codec::None || t->codec == Codec::Deflate;
    switch (t->codec) {
        case Codec::None: case Codec::Rle: case Codec::JpegBaseline: case Codec::JpegExtended:
        case Codec::JpegLossless: case Codec::JpegLs: case Codec::Jpeg2000: case Codec::Htj2k:
            return true;
        default: return false;
    }
}

bool encoding_is_lossy(std::string_view ts_uid, const TranscodeParams& p) noexcept {
    return target_lossy(target_of(ts_uid), p);
}

Result<Dataset> transcode_dataset(const Dataset& ds, std::string_view source_ts, std::string_view target_ts,
                                  const TranscodeParams& p, TranscodeReport* report) {
    TranscodeReport local;
    TranscodeReport& rep = report ? *report : local;
    rep = TranscodeReport{};
    const TransferSyntaxInfo* src = find_transfer_syntax(source_ts);
    if (!src) return Status(Errc::Unsupported, "transcode: unknown source transfer syntax");
    const Target target = target_of(target_ts);
    if (target == Target::None) return Status(Errc::Unsupported, "transcode: cannot encode in the target transfer syntax");
    if (p.chroma_subsampling != 0 && p.chroma_subsampling != 1)
        return Status(Errc::InvalidArgument, "transcode: chroma_subsampling must be 0 or 1");
    if (p.predictor < 1 || p.predictor > 7) return Status(Errc::InvalidArgument, "transcode: predictor must be 1..7");
    if (p.lossy && target == Target::JpegLsNear && (p.near_lossless < 1 || p.near_lossless > 255))
        return Status(Errc::InvalidArgument, "transcode: near_lossless must be 1..255");
    const bool target_encapsulated = target != Target::Native;

    Dataset out = ds;
    rep.source_lossy = is_lossy_attribute(ds);
    const Element* px = ds.find(tags::PixelData);

    // Encapsulated icons are decoded to native: native icon pixel data is valid in every syntax.
    if (const Element* icon = ds.find(kIconImageSequence); icon && icon->is_sequence() && source_ts != target_ts) {
        Element copy = *icon;
        bool changed = false;
        for (Dataset& item : copy.items) {
            const Element* ipx = item.find(tags::PixelData);
            if (!ipx || !ipx->is_encapsulated()) continue;
            TranscodeParams ip = p;
            ip.offset_table = OffsetTableMode::Empty;
            Result<Dataset> t = transcode_dataset(item, source_ts, ts::ExplicitVRLittleEndian, ip);
            if (!t) return t.take_error();
            item = std::move(t.value());
            changed = true;
        }
        if (changed) out.put(std::move(copy));
    }

    if (!px) {
        if (target_encapsulated && (ds.contains(tags::FloatPixelData) || ds.contains(tags::DoubleFloatPixelData)))
            return Status(Errc::Unsupported, "transcode: float pixel data has no encapsulated encoding");
        return out;
    }
    if (source_ts == target_ts) return out;
    const bool source_encapsulated = px->is_encapsulated();
    if (!source_encapsulated && !target_encapsulated) return out;  // the writer re-encodes native data
    if (!can_decode(source_ts)) return Status(Errc::Unsupported, "transcode: no decoder for the source transfer syntax");

    Result<image::FrameReader> frr = image::FrameReader::open(ds, source_ts, p.frames);
    if (!frr) return frr.take_error();
    const image::FrameReader& fr = *frr;
    if (codestream_lossy(fr)) rep.source_lossy = true;

    Plan plan;
    plan.target = target;
    plan.in = fr.info();
    const image::PixelInfo& pi = plan.in;
    if (pi.kind != image::SampleKind::Integer) return Status(Errc::Unsupported, "transcode: float pixel data");
    if (pi.bits_allocated == 1 || pi.bits_allocated > 16)
        return Status(Errc::Unsupported, "transcode: only 8- and 16-bit pixel data can be recoded");
    if (pi.low_bit() != 0) return Status(Errc::Unsupported, "transcode: High Bit must be Bits Stored - 1");
    plan.lossy = target_lossy(target, p);
    if (plan.lossy && !p.allow_lossy) return Status(Errc::Rejected, "transcode: lossy compression not allowed");

    const bool colour = pi.samples_per_pixel == 3;
    plan.deplanarise = colour && pi.planar;
    plan.work = pi.photometric;
    if (!source_encapsulated && pi.native_subsampled()) {
        if (pi.photometric != Photometric::YbrFull422)
            return Status(Errc::Unsupported, "transcode: native YBR_PARTIAL_422 cannot be recoded");
        plan.expand_422 = true;
        plan.deplanarise = false;
        plan.work = Photometric::YbrFull;  // full samples, still YBR (lossy origin: converted below)
    }
    if (fr.codec_applies_color_transform() &&
        (pi.photometric == Photometric::YbrRct || pi.photometric == Photometric::YbrIct))
        plan.work = Photometric::Rgb;
    const bool ybr_work = plan.work == Photometric::YbrFull || plan.work == Photometric::YbrFull422 ||
                          plan.work == Photometric::YbrPartial420 || plan.work == Photometric::YbrPartial422;
    if (colour && ybr_work) {
        // YBR that came from lossy coding (or is labelled as subsampled) is delivered as RGB.
        const bool lossy_origin = rep.source_lossy || plan.expand_422 || plan.work != Photometric::YbrFull;
        if (lossy_origin) {
            plan.to_rgb = true;
            plan.work = Photometric::Rgb;
        }
    }
    if (pi.photometric == Photometric::PaletteColor && plan.lossy)
        return Status(Errc::Unsupported, "transcode: lossy compression of PALETTE COLOR is not allowed");
    if (colour && plan.work != Photometric::Rgb && plan.work != Photometric::YbrFull)
        return Status(Errc::Unsupported, "transcode: colour model " + pi.photometric_text + " cannot be recoded");

    plan.out = plan.work;
    switch (target) {
        case Target::JpegBaseline:
            if (pi.bits_allocated != 8 || pi.bits_stored > 8)
                return Status(Errc::Unsupported, "transcode: JPEG baseline needs 8-bit data");
            [[fallthrough]];
        case Target::JpegExtended:
            if (!(pi.bits_allocated == 8 && pi.bits_stored <= 8) && !(pi.bits_allocated == 16 && pi.bits_stored <= 12))
                return Status(Errc::Unsupported, "transcode: JPEG extended needs 8- or 12-bit data");
            if (pi.is_signed) return Status(Errc::Unsupported, "transcode: lossy JPEG of signed data");
            if (pi.samples_per_pixel != 1 && !colour)
                return Status(Errc::Unsupported, "transcode: JPEG lossy needs 1 or 3 samples");
            if (colour) {
                plan.ybr_encode = plan.work == Photometric::Rgb;
                plan.out = p.chroma_subsampling == 1 ? Photometric::YbrFull422 : Photometric::YbrFull;
            }
            break;
        case Target::JpegLossless: case Target::JpegLosslessSv1:
            if (pi.bits_stored < 2) return Status(Errc::Unsupported, "transcode: JPEG lossless needs 2..16 bits");
            break;
        case Target::JpegLsLossless: case Target::JpegLsNear:
            if (pi.bits_stored < 2) return Status(Errc::Unsupported, "transcode: JPEG-LS needs 2..16 bits");
            if (pi.is_signed) return Status(Errc::Unsupported, "transcode: JPEG-LS of signed data");
            break;
        case Target::J2kLossless: case Target::J2k: case Target::HtLossless: case Target::HtRpcl: case Target::Ht:
            if (plan.lossy && target == Target::Ht)
                return Status(Errc::Unsupported, "transcode: the HTJ2K encoder is lossless only");
            if (colour && plan.work == Photometric::Rgb) {
                plan.mct = true;
                plan.out = plan.lossy ? Photometric::YbrIct : Photometric::YbrRct;
            }
            break;
        default: break;
    }

    // ---- decode, normalise, encode, frame by frame
    const std::uint32_t nframes = fr.frame_count();
    const codec::FrameInfo wfi = work_info(pi);
    const std::uint64_t work_bytes = wfi.frame_bytes();
    if (work_bytes == 0) return Status(Errc::Unsupported, "transcode: frame size");
    if (work_bytes > p.frames.memory_budget || work_bytes > SIZE_MAX)
        return Status(Errc::LimitExceeded, "transcode: frame exceeds the memory budget");
    std::uint64_t native_total = 0;
    if (!checked_mul(work_bytes, nframes, native_total)) return Status(Errc::LimitExceeded, "transcode: size overflow");
    if (!target_encapsulated && (native_total > p.frames.memory_budget || native_total > SIZE_MAX))
        return Status(Errc::LimitExceeded, "transcode: native pixel data exceeds the memory budget");

    if (fr.frame_bytes() > p.frames.memory_budget || fr.frame_bytes() > SIZE_MAX)
        return Status(Errc::LimitExceeded, "transcode: frame exceeds the memory budget");
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(fr.frame_bytes()));
    std::vector<std::uint8_t> work(static_cast<std::size_t>(work_bytes));
    std::vector<std::uint8_t> tmp;
    std::shared_ptr<std::vector<std::uint8_t>> native_out;
    if (!target_encapsulated) {
        native_out = std::make_shared<std::vector<std::uint8_t>>();
        native_out->resize(static_cast<std::size_t>(native_total));
    }
    std::vector<Bytes> fragments;
    std::vector<std::uint64_t> lengths;
    std::vector<std::uint8_t> enc;
    const unsigned bps = pi.bits_allocated / 8u;
    const auto pixels = pi.pixels();
    for (std::uint32_t f = 0; f < nframes; ++f) {
        if (Status s = fr.decode(f, raw); !s) return s;
        std::span<const std::uint8_t> cur(raw);
        if (plan.expand_422) {
            if (Status s = image::expand_422(cur, work, pi.rows, pi.columns, bps, image::ChromaUpsampling::Replicate); !s)
                return s;
            cur = work;
        } else if (plan.deplanarise) {
            if (Status s = image::planar_to_interleaved(cur, work, pixels, pi.samples_per_pixel, bps); !s) return s;
            cur = work;
        }
        if (plan.to_rgb) {
            image::PixelInfo cpi = pi;
            cpi.planar = false;
            if (plan.expand_422) cpi.photometric = Photometric::YbrFull;
            image::ColorOptions co;
            co.native_layout = false;
            co.codec_applied_mct = fr.codec_applies_color_transform();
            tmp.resize(static_cast<std::size_t>(work_bytes));
            if (Status s = image::to_rgb(cpi, cur, tmp, co); !s) return s;
            std::memcpy(work.data(), tmp.data(), tmp.size());
            cur = work;
        }
        if (cur.size() < work_bytes) return Status(Errc::Internal, "transcode: work frame size");
        cur = cur.first(static_cast<std::size_t>(work_bytes));
        if (!target_encapsulated) {
            std::memcpy(native_out->data() + static_cast<std::size_t>(work_bytes) * f, cur.data(), cur.size());
            continue;
        }
        if (Status s = encode_frame(plan, p, cur, wfi, enc); !s) return s;
        lengths.push_back(enc.size());
        rep.encoded_bytes += enc.size();
        if (enc.size() & 1u) enc.push_back(0);  // PS3.5 A.4: fragments have even length
        fragments.push_back(Bytes::copy_of(enc.data(), enc.size()));
    }
    rep.native_bytes = native_total;
    rep.pixel_data_recoded = true;
    rep.lossy_applied = plan.lossy;

    // ---- the new Pixel Data element and the attributes that depend on it
    (void)out.remove(kExtendedOffsetTable);
    (void)out.remove(kExtendedOffsetTableLengths);
    Element pe;
    pe.tag = tags::PixelData;
    if (!target_encapsulated) {
        pe.vr = pi.bits_allocated > 8 ? VR::OW : VR::OB;
        if (native_out->size() & 1u) native_out->push_back(0);  // even length (PS3.5 7.1)
        const std::uint8_t* d = native_out->data();
        pe.value = Bytes(std::shared_ptr<const void>(native_out, native_out.get()), d, native_out->size());
    } else {
        pe.vr = VR::OB;
        pe.undefined_length = true;
        // Offsets of each frame's item from the first byte of the first item after the table.
        std::vector<std::uint64_t> offsets;
        offsets.reserve(fragments.size());
        std::uint64_t off = 0;
        for (const Bytes& b : fragments) {
            offsets.push_back(off);
            off += 8 + b.size();
        }
        const bool fits32 = offsets.empty() || offsets.back() <= 0xFFFFFFFFull;
        OffsetTableMode mode = p.offset_table;
        if (mode == OffsetTableMode::Auto) mode = fits32 ? OffsetTableMode::Basic : OffsetTableMode::Extended;
        if (mode == OffsetTableMode::Basic && !fits32)
            return Status(Errc::LimitExceeded, "transcode: frame offsets do not fit a Basic Offset Table");
        std::vector<std::uint8_t> bot;
        if (mode == OffsetTableMode::Basic) {
            bot.resize(4 * offsets.size());
            for (std::size_t i = 0; i < offsets.size(); ++i)
                store_le32(bot.data() + 4 * i, static_cast<std::uint32_t>(offsets[i]));
        } else if (mode == OffsetTableMode::Extended) {
            std::vector<std::uint8_t> eot(8 * offsets.size()), eotl(8 * offsets.size());
            for (std::size_t i = 0; i < offsets.size(); ++i) {
                store_le32(eot.data() + 8 * i, static_cast<std::uint32_t>(offsets[i]));
                store_le32(eot.data() + 8 * i + 4, static_cast<std::uint32_t>(offsets[i] >> 32));
                store_le32(eotl.data() + 8 * i, static_cast<std::uint32_t>(lengths[i]));
                store_le32(eotl.data() + 8 * i + 4, static_cast<std::uint32_t>(lengths[i] >> 32));
            }
            (void)out.set_bytes(kExtendedOffsetTable, VR::OV, Bytes::copy_of(eot.data(), eot.size()));
            (void)out.set_bytes(kExtendedOffsetTableLengths, VR::OV, Bytes::copy_of(eotl.data(), eotl.size()));
        }
        pe.fragments.reserve(fragments.size() + 1);
        pe.fragments.push_back(Bytes::copy_of(bot.data(), bot.size()));
        for (Bytes& b : fragments) pe.fragments.push_back(std::move(b));
    }
    out.put(std::move(pe));

    if (plan.out != pi.photometric || plan.expand_422) {
        if (Status s = out.set_string(tags::PhotometricInterpretation, VR::CS, image::photometric_name(plan.out)); !s)
            return s;
    }
    if (pi.samples_per_pixel > 1) (void)out.set_uint16(tags::PlanarConfiguration, 0);
    if (plan.lossy) {
        (void)out.set_string(image::tags::LossyImageCompression, VR::CS, "01");
        if (Status s = append_value(out, kLossyRatio, VR::DS, format_ratio(rep.native_bytes, rep.encoded_bytes)); !s)
            return s;
        if (Status s = append_value(out, kLossyMethod, VR::CS, lossy_method(target)); !s) return s;
    } else if (rep.source_lossy) {
        (void)out.set_string(image::tags::LossyImageCompression, VR::CS, "01");  // never reset once lossy
    }
    return out;
}

Status transcode_file(DicomFile& file, std::string_view target_ts, const TranscodeParams& p, TranscodeReport* report) {
    Result<Dataset> r = transcode_dataset(file.dataset, file.transfer_syntax, target_ts, p, report);
    if (!r) return r.take_error();
    file.dataset = std::move(r.value());
    file.transfer_syntax = std::string(target_ts);
    if (Status s = file.meta.set_string(tags::TransferSyntaxUID, VR::UI, target_ts); !s) return s;
    return Status::ok();
}

}  // namespace siaara::codec
