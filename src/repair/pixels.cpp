#include "repair/pixels.hpp"

#include <algorithm>
#include <set>

#include "codec/j2k.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpegls.hpp"
#include "core/transfer_syntax.hpp"
#include "image/pixel_info.hpp"

namespace siaara::repair {

namespace {

struct Attrs {
    std::int64_t rows = 0, columns = 0, samples = 1, bits_allocated = 0, bits_stored = 0, high_bit = 0;
    std::int64_t pixel_rep = 0, frames = 1, planar = -1;
    bool has_rows = false, has_columns = false, has_ba = false, has_bs = false, has_hb = false, has_pr = false;
    bool has_frames = false, has_samples = false;
    std::string photometric;
};

Attrs read_attrs(const Dataset& ds) {
    Attrs a;
    auto get = [&](Tag t, std::int64_t& v, bool& has) {
        if (auto x = ds.get_int(t)) {
            v = *x;
            has = true;
        }
    };
    bool dummy = false;
    get(tags::Rows, a.rows, a.has_rows);
    get(tags::Columns, a.columns, a.has_columns);
    get(tags::SamplesPerPixel, a.samples, a.has_samples);
    get(tags::BitsAllocated, a.bits_allocated, a.has_ba);
    get(tags::BitsStored, a.bits_stored, a.has_bs);
    get(tags::HighBit, a.high_bit, a.has_hb);
    get(tags::PixelRepresentation, a.pixel_rep, a.has_pr);
    get(tags::NumberOfFrames, a.frames, a.has_frames);
    get(tags::PlanarConfiguration, a.planar, dummy);
    if (auto p = ds.get_string(tags::PhotometricInterpretation)) a.photometric = *p;
    if (!a.has_frames || a.frames < 1) a.frames = 1;
    return a;
}

std::uint64_t frame_bits_for(std::int64_t rows, std::int64_t cols, std::int64_t samples, std::int64_t ba,
                             const std::string& photometric) {
    if (rows <= 0 || cols <= 0 || samples <= 0 || ba <= 0) return 0;
    std::uint64_t px = static_cast<std::uint64_t>(rows) * static_cast<std::uint64_t>(cols);
    std::uint64_t s = static_cast<std::uint64_t>(samples);
    // Native YBR_FULL_422 / YBR_PARTIAL_422 store two samples per pixel (PS3.3 C.7.6.3.1.2).
    if (samples == 3 && (photometric == "YBR_FULL_422" || photometric == "YBR_PARTIAL_422")) s = 2;
    std::uint64_t bits = 0;
    if (!checked_mul(px, s, px) || !checked_mul(px, static_cast<std::uint64_t>(ba), bits)) return 0;
    return bits;
}

Finding make(Severity s, const char* code, std::uint32_t cat, Tag tag, std::string msg) {
    Finding f;
    f.severity = s;
    f.code = code;
    f.category = cat;
    if (tag) f.path.push_back({tag, -1});
    f.message = std::move(msg);
    return f;
}

AttrFix us_fix(Tag t, std::uint32_t v, std::string why) {
    AttrFix f;
    f.tag = t;
    f.vr = VR::US;
    f.number = v;
    f.reason = std::move(why);
    return f;
}
AttrFix text_fix(Tag t, VR vr, std::string v, std::string why) {
    AttrFix f;
    f.tag = t;
    f.vr = vr;
    f.text = true;
    f.str = std::move(v);
    f.reason = std::move(why);
    return f;
}

bool is_mono_or_palette(const std::string& p) {
    return p == "MONOCHROME1" || p == "MONOCHROME2" || p == "PALETTE COLOR";
}
bool is_color(const std::string& p) {
    return p == "RGB" || p.rfind("YBR_", 0) == 0 || p == "HSV" || p == "ARGB" || p == "CMYK";
}

// What a codestream header says.
struct StreamFacts {
    bool ok = false;
    std::uint32_t rows = 0, columns = 0;
    int components = 0, precision = 0;
    bool is_signed = false, signed_known = false;
    bool lossy_jpeg = false;  // DCT JPEG (colour space not signalled)
    bool j2k_mct = false, j2k_reversible = false, j2k = false;
};

StreamFacts probe_stream(Codec codec, std::span<const std::uint8_t> bytes) {
    StreamFacts f;
    switch (codec) {
        case Codec::JpegBaseline:
        case Codec::JpegExtended:
        case Codec::JpegLossless: {
            codec::JpegHeaderInfo h;
            if (!codec::jpeg_read_header(bytes, h)) return f;
            f.ok = true;
            f.rows = h.rows;
            f.columns = h.columns;
            f.components = h.components;
            f.precision = h.precision;
            f.lossy_jpeg = !h.lossless;
            return f;
        }
        case Codec::JpegLs: {
            codec::jpegls::StreamInfo h;
            if (!codec::jpegls::probe(bytes, h)) return f;
            f.ok = true;
            f.rows = h.height;
            f.columns = h.width;
            f.components = h.components;
            f.precision = h.precision;
            return f;
        }
        case Codec::Jpeg2000:
        case Codec::Htj2k: {
            codec::j2k::StreamInfo h;
            if (!codec::j2k::probe(bytes, h)) return f;
            f.ok = true;
            f.j2k = true;
            f.rows = h.height;
            f.columns = h.width;
            f.components = h.components;
            f.precision = h.precision;
            f.is_signed = h.is_signed;
            f.signed_known = true;
            f.j2k_mct = h.mct;
            f.j2k_reversible = h.reversible;
            return f;
        }
        default:
            return f;
    }
}

bool starts_frame(Codec codec, const Bytes& frag) {
    const std::uint8_t* p = frag.data();
    const std::size_t n = frag.size();
    switch (codec) {
        case Codec::JpegBaseline:
        case Codec::JpegExtended:
        case Codec::JpegLossless:
        case Codec::JpegLs:
            return n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
        case Codec::Jpeg2000:
        case Codec::Htj2k:
            return (n >= 4 && p[0] == 0xFF && p[1] == 0x4F && p[2] == 0xFF && p[3] == 0x51) ||
                   (n >= 12 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0x0C && p[4] == 0x6A && p[5] == 0x50);
        case Codec::Rle:
            return true;  // PS3.5 A.4.2: each frame is one fragment
        default:
            return false;
    }
}

std::vector<std::uint64_t> item_offsets(const std::vector<Bytes>& data) {
    std::vector<std::uint64_t> off;
    off.reserve(data.size());
    std::uint64_t at = 0;
    for (const Bytes& b : data) {
        off.push_back(at);
        at += 8 + b.size() + (b.size() & 1u);
    }
    return off;
}

}  // namespace

std::vector<std::uint8_t> frame_bytes_of(const std::vector<Bytes>& data, const FrameSpan& f) {
    std::vector<std::uint8_t> out;
    for (std::uint32_t i = f.first; i < f.first + f.count && i < data.size(); ++i)
        out.insert(out.end(), data[i].data(), data[i].data() + data[i].size());
    return out;
}

std::vector<std::uint8_t> build_basic_offset_table(const std::vector<Bytes>& data, const std::vector<FrameSpan>& frames,
                                                   bool* overflow) {
    if (overflow) *overflow = false;
    const auto off = item_offsets(data);
    std::vector<std::uint8_t> bot;
    for (const FrameSpan& f : frames) {
        if (f.first >= off.size()) break;
        const std::uint64_t o = off[f.first];
        if (o > 0xFFFFFFFFull) {
            if (overflow) *overflow = true;
            return {};
        }
        std::uint8_t b[4];
        store_le32(b, static_cast<std::uint32_t>(o));
        bot.insert(bot.end(), b, b + 4);
    }
    return bot;
}

PixelAnalysis analyse_pixels(const Dataset& ds, std::string_view ts_uid, const PixelScan* scan) {
    PixelAnalysis r;
    const Element* px = ds.find(tags::PixelData);
    if (!px) px = ds.find(tags::FloatPixelData);
    if (!px) px = ds.find(tags::DoubleFloatPixelData);
    if (!px) return r;
    r.present = true;
    const Attrs a = read_attrs(ds);
    r.declared_frames = static_cast<std::uint32_t>(std::min<std::int64_t>(a.frames, 0x7FFFFFFF));
    const TransferSyntaxInfo* ts = find_transfer_syntax(ts_uid);
    const bool ts_encapsulated = ts && ts->encapsulated;
    r.encapsulated = px->is_encapsulated();

    // ---- basic attribute rules (PS3.3 C.7.6.3.1)
    if (!a.has_rows || !a.has_columns || !a.has_ba || !a.has_samples || a.photometric.empty())
        r.findings.push_back(make(Severity::Error, "pixel-attributes-missing", 0, 0,
                                  "pixel data present but Rows, Columns, Samples per Pixel, Photometric "
                                  "Interpretation or Bits Allocated is missing"));
    if (a.has_ba && a.bits_allocated != 1 && a.bits_allocated % 8 != 0)
        r.findings.push_back(make(Severity::Error, "bits-allocated", 0, tags::BitsAllocated,
                                  "Bits Allocated is not 1 or a multiple of 8"));
    if (a.has_ba && a.has_bs && (a.bits_stored < 1 || a.bits_stored > a.bits_allocated))
        r.findings.push_back(make(Severity::Error, "bits-stored", 0, tags::BitsStored,
                                  "Bits Stored is 0 or larger than Bits Allocated"));
    if (a.has_ba && a.has_bs && a.has_hb && a.bits_stored >= 1 && a.bits_stored <= a.bits_allocated &&
        a.high_bit != a.bits_stored - 1) {
        const bool invalid = a.high_bit >= a.bits_allocated || a.high_bit < a.bits_stored - 1;
        r.findings.push_back(make(invalid ? Severity::Error : Severity::Warning, "high-bit", kPixels, tags::HighBit,
                                  "High Bit is not Bits Stored - 1"));
        r.fixes.push_back(us_fix(tags::HighBit, static_cast<std::uint32_t>(a.bits_stored - 1),
                                 "High Bit set to Bits Stored - 1"));
    }
    if (a.has_samples && !a.photometric.empty()) {
        if (is_mono_or_palette(a.photometric) && a.samples != 1)
            r.findings.push_back(make(Severity::Error, "samples-photometric", 0, tags::SamplesPerPixel,
                                      "Samples per Pixel must be 1 for this Photometric Interpretation"));
        if (is_color(a.photometric) && a.photometric != "ARGB" && a.photometric != "CMYK" && a.samples != 3)
            r.findings.push_back(make(Severity::Error, "samples-photometric", 0, tags::SamplesPerPixel,
                                      "Samples per Pixel must be 3 for this Photometric Interpretation"));
        if (a.samples > 1 && a.planar < 0)
            r.findings.push_back(make(Severity::Error, "planar-missing", kPixels, tags::PlanarConfiguration,
                                      "Planar Configuration is required when Samples per Pixel > 1"));
    }
    if (a.has_pr && a.pixel_rep != 0 && a.pixel_rep != 1)
        r.findings.push_back(make(Severity::Error, "pixel-representation", 0, tags::PixelRepresentation,
                                  "Pixel Representation is not 0 or 1"));

    if (px->tag != tags::PixelData) return r;  // float pixel data: no codestreams, sizes only below
    if (r.encapsulated != ts_encapsulated && ts) {
        r.findings.push_back(make(Severity::Error, "pixel-encoding", 0, tags::PixelData,
                                  r.encapsulated ? "pixel data is encapsulated but the transfer syntax is native"
                                                 : "pixel data is native but the transfer syntax is encapsulated"));
        return r;
    }

    if (!r.encapsulated) {
        r.frame_bits = frame_bits_for(a.rows, a.columns, a.samples, a.bits_allocated, a.photometric);
        if (r.frame_bits == 0) return r;
        std::uint64_t expected_bits = 0;
        if (!checked_mul(r.frame_bits, static_cast<std::uint64_t>(a.frames), expected_bits)) return r;
        r.native_expected = (expected_bits + 7) / 8;
        r.native_actual = px->value.size();
        r.native_complete_frames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>((r.native_actual * 8) / r.frame_bits, 0xFFFFFFFFu));
        const std::uint64_t padded = r.native_expected + (r.native_expected & 1u);
        if (r.native_actual == r.native_expected || r.native_actual == padded) return r;
        const bool truncated = scan && scan->value_truncated;
        // Find attribute sets that explain the actual length exactly.
        struct Cand {
            std::string what;
            std::vector<AttrFix> fixes;
        };
        std::vector<Cand> cands;
        const std::uint64_t actual = r.native_actual;
        auto fits = [&](std::uint64_t fb, std::uint64_t frames) {
            std::uint64_t bits = 0;
            if (fb == 0 || !checked_mul(fb, frames, bits)) return false;
            const std::uint64_t bytes = (bits + 7) / 8;
            return actual == bytes || actual == bytes + (bytes & 1u);
        };
        if (!truncated) {
            // Number of Frames
            if ((actual * 8) % r.frame_bits == 0 || ((actual - 1) * 8) % r.frame_bits == 0) {
                std::uint64_t n = (actual * 8) / r.frame_bits;
                if (!fits(r.frame_bits, n)) n = ((actual - 1) * 8) / r.frame_bits;
                if (n >= 1 && n != static_cast<std::uint64_t>(a.frames) && n <= 0x7FFFFFFF && fits(r.frame_bits, n) &&
                    (n > 1 || a.has_frames))
                    cands.push_back({"Number of Frames", {text_fix(tags::NumberOfFrames, VR::IS, std::to_string(n),
                                                                   "Number of Frames set to the frames the pixel "
                                                                   "data holds")}});
            }
            // Bits Allocated
            for (std::int64_t ba : {8, 16, 32}) {
                if (ba == a.bits_allocated) continue;
                const std::uint64_t fb = frame_bits_for(a.rows, a.columns, a.samples, ba, a.photometric);
                if (!fits(fb, static_cast<std::uint64_t>(a.frames))) continue;
                Cand c{"Bits Allocated", {us_fix(tags::BitsAllocated, static_cast<std::uint32_t>(ba),
                                                 "Bits Allocated set to match the pixel data length")}};
                if (a.bits_stored > ba) {
                    c.fixes.push_back(us_fix(tags::BitsStored, static_cast<std::uint32_t>(ba), "Bits Stored limited to Bits Allocated"));
                    c.fixes.push_back(us_fix(tags::HighBit, static_cast<std::uint32_t>(ba - 1), "High Bit set to Bits Stored - 1"));
                }
                cands.push_back(std::move(c));
            }
            // Samples per Pixel
            for (std::int64_t s : {1, 3}) {
                if (s == a.samples) continue;
                const std::string ph = s == 1 ? "MONOCHROME2" : "RGB";
                const std::uint64_t fb = frame_bits_for(a.rows, a.columns, s, a.bits_allocated, ph);
                if (!fits(fb, static_cast<std::uint64_t>(a.frames))) continue;
                Cand c{"Samples per Pixel", {us_fix(tags::SamplesPerPixel, static_cast<std::uint32_t>(s),
                                                    "Samples per Pixel set to match the pixel data length")}};
                if ((s == 1 && !is_mono_or_palette(a.photometric)) || (s == 3 && !is_color(a.photometric)))
                    c.fixes.push_back(text_fix(tags::PhotometricInterpretation, VR::CS, ph,
                                               "Photometric Interpretation set for the new Samples per Pixel"));
                if (s == 3 && a.planar < 0)
                    c.fixes.push_back(us_fix(tags::PlanarConfiguration, 0, "Planar Configuration added (colour-by-pixel)"));
                cands.push_back(std::move(c));
            }
        }
        std::string msg = "native pixel data holds " + std::to_string(actual) + " byte(s) but the image attributes describe " +
                          std::to_string(r.native_expected);
        if (cands.size() == 1) {
            msg += "; only " + cands[0].what + " explains the length";
            r.findings.push_back(make(Severity::Error, "pixel-length", kPixels, tags::PixelData, msg));
            for (auto& f : cands[0].fixes) r.fixes.push_back(std::move(f));
        } else if (actual < r.native_expected) {
            msg += "; " + std::to_string(r.native_complete_frames) + " complete frame(s) can be rescued";
            if (cands.size() > 1) msg += " (several attribute changes would also explain it; none chosen)";
            r.findings.push_back(make(Severity::Error, truncated ? "pixel-truncated" : "pixel-length", kRescue,
                                      tags::PixelData, msg));
        } else {
            msg += cands.empty() ? "; no single attribute change explains it" : "; several attribute changes would explain it";
            r.findings.push_back(make(Severity::Error, "pixel-length", 0, tags::PixelData, msg));
        }
        return r;
    }

    // ---- encapsulated
    if (px->fragments.empty()) {
        r.findings.push_back(make(Severity::Error, "pixel-fragments", 0, tags::PixelData,
                                  "encapsulated pixel data has no Basic Offset Table item"));
        return r;
    }
    const Codec codec = ts ? ts->codec : Codec::Unknown;
    const std::vector<Bytes> data(px->fragments.begin() + 1, px->fragments.end());
    if (data.empty()) {
        r.findings.push_back(make(Severity::Error, "pixel-fragments", kRescue, tags::PixelData,
                                  "encapsulated pixel data has no data fragments"));
        return r;
    }
    const auto off = item_offsets(data);
    // Basic Offset Table
    const Bytes& bot = px->fragments[0];
    std::vector<FrameSpan> from_bot;
    if (!bot.empty()) {
        bool ok = bot.size() % 4 == 0;
        std::vector<std::uint32_t> idx;
        for (std::size_t i = 0; ok && i + 4 <= bot.size(); i += 4) {
            const std::uint32_t o = load_le32(bot.data() + i);
            auto it = std::lower_bound(off.begin(), off.end(), static_cast<std::uint64_t>(o));
            if (it == off.end() || *it != o || (!idx.empty() && static_cast<std::uint32_t>(it - off.begin()) <= idx.back())) {
                ok = false;
                break;
            }
            idx.push_back(static_cast<std::uint32_t>(it - off.begin()));
        }
        if (ok && (idx.empty() || idx[0] != 0)) ok = false;
        if (ok && idx.size() != r.declared_frames && !(r.declared_frames == 1 && idx.size() == 1)) ok = false;
        if (ok) {
            for (std::size_t i = 0; i < idx.size(); ++i) {
                const std::uint32_t end = i + 1 < idx.size() ? idx[i + 1] : static_cast<std::uint32_t>(data.size());
                from_bot.push_back({idx[i], end - idx[i]});
            }
        } else {
            r.bot_invalid = true;
            r.findings.push_back(make(Severity::Error, "bot-invalid", kStructural, tags::PixelData,
                                      "Basic Offset Table does not match the fragments or Number of Frames "
                                      "(PS3.5 A.4); repair rebuilds it"));
        }
    }
    // Extended Offset Table (PS3.3 C.7.6.3.1.8)
    if (const Element* eot = ds.find(image::tags::ExtendedOffsetTable)) {
        bool ok = eot->value.size() % 8 == 0 && eot->value.size() / 8 == r.declared_frames && bot.empty();
        for (std::size_t i = 0; ok && i + 8 <= eot->value.size(); i += 8) {
            const std::uint64_t o = load_le64(eot->value.data() + i);
            if (!std::binary_search(off.begin(), off.end(), o)) ok = false;
        }
        const Element* lens = ds.find(image::tags::ExtendedOffsetTableLengths);
        if (!lens || lens->value.size() != eot->value.size()) ok = false;
        if (!ok) {
            r.eot_invalid = true;
            r.findings.push_back(make(Severity::Error, "eot-invalid", kStructural, image::tags::ExtendedOffsetTable,
                                      "Extended Offset Table does not match the fragments (it must be used with an "
                                      "empty Basic Offset Table); repair recomputes it"));
        }
    }
    // Frame layout
    if (!from_bot.empty()) {
        r.frames = std::move(from_bot);
        r.layout_source = "basic offset table";
    } else if (r.declared_frames == 1 && codec != Codec::Rle) {
        r.frames.push_back({0, static_cast<std::uint32_t>(data.size())});
        r.layout_source = "single frame";
        // Several codestreams in a single-frame object: the header probably understates the frames.
        std::uint32_t starts = 0;
        for (const Bytes& b : data) starts += starts_frame(codec, b) ? 1u : 0u;
        if (starts > 1 && codec != Codec::Rle) {
            r.frames.clear();
            for (std::uint32_t i = 0; i < data.size(); ++i)
                if (starts_frame(codec, data[i])) r.frames.push_back({i, 1});
                else if (!r.frames.empty()) ++r.frames.back().count;
            r.layout_source = "codestream start markers";
        }
    } else if (data.size() == r.declared_frames) {
        for (std::uint32_t i = 0; i < data.size(); ++i) r.frames.push_back({i, 1});
        r.layout_source = "one fragment per frame";
    } else {
        for (std::uint32_t i = 0; i < data.size(); ++i) {
            if (starts_frame(codec, data[i]) || r.frames.empty()) r.frames.push_back({i, 1});
            else ++r.frames.back().count;
        }
        r.layout_source = codec == Codec::Rle ? "one fragment per frame" : "codestream start markers";
    }
    const std::uint32_t found = static_cast<std::uint32_t>(r.frames.size());
    const bool truncated = scan && (scan->last_fragment_truncated || scan->sequence_unterminated);
    if (found != r.declared_frames) {
        if (found > r.declared_frames || !truncated) {
            r.findings.push_back(make(Severity::Error, "frame-count", kPixels, tags::NumberOfFrames,
                                      "Number of Frames is " + std::to_string(r.declared_frames) + " but the pixel data holds " +
                                          std::to_string(found) + " frame(s) (" + r.layout_source + ")"));
            r.fixes.push_back(text_fix(tags::NumberOfFrames, VR::IS, std::to_string(found),
                                       "Number of Frames set to the frames found in the pixel data"));
        } else {
            r.findings.push_back(make(Severity::Error, "frames-missing", kRescue, tags::PixelData,
                                      std::to_string(found) + " of " + std::to_string(r.declared_frames) +
                                          " frame(s) present; the pixel data is truncated"));
        }
    }

    // Codestream headers (first and last frame).
    if (codec == Codec::Rle) {
        const auto f0 = frame_bytes_of(data, r.frames.front());
        if (f0.size() >= 64) {
            const std::uint32_t segs = load_le32(f0.data());
            const std::int64_t want = a.samples * (a.bits_allocated / 8);
            if (a.bits_allocated % 8 == 0 && want > 0 && segs != static_cast<std::uint64_t>(want))
                r.findings.push_back(make(Severity::Error, "rle-segments", 0, tags::PixelData,
                                          "RLE header has " + std::to_string(segs) + " segment(s); Samples per Pixel x "
                                          "Bits Allocated / 8 is " + std::to_string(want)));
        }
        return r;
    }
    const auto f0 = frame_bytes_of(data, r.frames.front());
    StreamFacts s = probe_stream(codec, f0);
    if (!s.ok) {
        if (codec != Codec::Unknown && codec != Codec::None && codec != Codec::Deflate &&
            (codec == Codec::JpegBaseline || codec == Codec::JpegExtended || codec == Codec::JpegLossless ||
             codec == Codec::JpegLs || codec == Codec::Jpeg2000 || codec == Codec::Htj2k))
            r.findings.push_back(make(Severity::Error, "codestream-header", kRescue, tags::PixelData,
                                      "the first frame's codestream header could not be read"));
        return r;
    }
    if (r.frames.size() > 1) {
        const auto fl = frame_bytes_of(data, r.frames.back());
        StreamFacts t = probe_stream(codec, fl);
        if (t.ok && (t.rows != s.rows || t.columns != s.columns || t.components != s.components)) {
            r.findings.push_back(make(Severity::Error, "codestream-frames-differ", 0, tags::PixelData,
                                      "the first and last frames' codestreams have different sizes; no header fix made"));
            return r;
        }
    }
    const std::string where = std::string("codestream header (") + std::string(ts ? ts->name : "") + ")";
    if (s.rows != 0 && a.has_rows && s.rows != static_cast<std::uint64_t>(a.rows)) {
        r.findings.push_back(make(Severity::Error, "rows-codestream", kPixels, tags::Rows,
                                  "Rows is " + std::to_string(a.rows) + "; the " + where + " says " + std::to_string(s.rows)));
        r.fixes.push_back(us_fix(tags::Rows, s.rows, "Rows set from the " + where));
    }
    if (s.columns != 0 && a.has_columns && s.columns != static_cast<std::uint64_t>(a.columns)) {
        r.findings.push_back(make(Severity::Error, "columns-codestream", kPixels, tags::Columns,
                                  "Columns is " + std::to_string(a.columns) + "; the " + where + " says " +
                                      std::to_string(s.columns)));
        r.fixes.push_back(us_fix(tags::Columns, s.columns, "Columns set from the " + where));
    }
    if (s.components != 0 && s.components != a.samples && (s.components == 1 || s.components == 3)) {
        r.findings.push_back(make(Severity::Error, "samples-codestream", kPixels, tags::SamplesPerPixel,
                                  "Samples per Pixel is " + std::to_string(a.samples) + "; the " + where + " has " +
                                      std::to_string(s.components) + " component(s)"));
        r.fixes.push_back(us_fix(tags::SamplesPerPixel, static_cast<std::uint32_t>(s.components),
                                 "Samples per Pixel set from the " + where));
        if (s.components == 1 && !is_mono_or_palette(a.photometric)) {
            r.fixes.push_back(text_fix(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2",
                                       "Photometric Interpretation set for one component"));
        } else if (s.components == 3 && !is_color(a.photometric)) {
            std::string ph;
            if (s.j2k) ph = s.j2k_mct ? (s.j2k_reversible ? "YBR_RCT" : "YBR_ICT") : "RGB";
            else if (!s.lossy_jpeg) ph = "RGB";
            if (!ph.empty())
                r.fixes.push_back(text_fix(tags::PhotometricInterpretation, VR::CS, ph,
                                           "Photometric Interpretation set from the " + where));
            else
                r.findings.push_back(make(Severity::Warning, "photometric-ambiguous", 0, tags::PhotometricInterpretation,
                                          "lossy JPEG colour: YBR_FULL_422 or YBR_FULL cannot be told from the "
                                          "codestream; Photometric Interpretation not changed"));
        }
        if (s.components == 3 && a.planar < 0)
            r.fixes.push_back(us_fix(tags::PlanarConfiguration, 0, "Planar Configuration added"));
    }
    if (s.precision > 0 && a.has_ba && a.has_bs) {
        const auto p = static_cast<std::int64_t>(s.precision);
        if (p > a.bits_allocated && p <= 16 && a.bits_allocated == 8) {
            r.findings.push_back(make(Severity::Error, "precision-codestream", kPixels, tags::BitsAllocated,
                                      "codestream precision " + std::to_string(p) + " exceeds Bits Allocated " +
                                          std::to_string(a.bits_allocated)));
            r.fixes.push_back(us_fix(tags::BitsAllocated, 16, "Bits Allocated set to hold the codestream precision"));
            r.fixes.push_back(us_fix(tags::BitsStored, static_cast<std::uint32_t>(p), "Bits Stored set to the codestream precision"));
            r.fixes.push_back(us_fix(tags::HighBit, static_cast<std::uint32_t>(p - 1), "High Bit set to Bits Stored - 1"));
        } else if (p != a.bits_stored && p <= a.bits_allocated && !s.lossy_jpeg) {
            r.findings.push_back(make(p > a.bits_stored ? Severity::Error : Severity::Warning, "precision-codestream",
                                      kPixels, tags::BitsStored,
                                      "Bits Stored is " + std::to_string(a.bits_stored) + "; the " + where +
                                          " precision is " + std::to_string(p)));
            r.fixes.push_back(us_fix(tags::BitsStored, static_cast<std::uint32_t>(p), "Bits Stored set to the codestream precision"));
            r.fixes.push_back(us_fix(tags::HighBit, static_cast<std::uint32_t>(p - 1), "High Bit set to Bits Stored - 1"));
        } else if (p > a.bits_allocated) {
            r.findings.push_back(make(Severity::Error, "precision-codestream", 0, tags::BitsAllocated,
                                      "codestream precision exceeds Bits Allocated"));
        }
    }
    if (s.signed_known && a.has_pr && (s.is_signed ? 1 : 0) != a.pixel_rep) {
        r.findings.push_back(make(Severity::Error, "signedness-codestream", kPixels, tags::PixelRepresentation,
                                  "Pixel Representation is " + std::to_string(a.pixel_rep) + "; the " + where +
                                      " component signedness says " + (s.is_signed ? "1" : "0")));
        r.fixes.push_back(us_fix(tags::PixelRepresentation, s.is_signed ? 1u : 0u,
                                 "Pixel Representation set from the codestream signedness"));
    }
    // Deduplicate fixes (last one wins per tag, keeping the first reason).
    std::vector<AttrFix> uniq;
    for (auto& f : r.fixes) {
        auto it = std::find_if(uniq.begin(), uniq.end(), [&](const AttrFix& u) { return u.tag == f.tag; });
        if (it == uniq.end()) uniq.push_back(std::move(f));
        else *it = std::move(f);
    }
    r.fixes = std::move(uniq);
    return r;
}

}  // namespace siaara::repair
