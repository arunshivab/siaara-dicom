// Transcoding between every supported pair of transfer syntaxes (codec/transcode.hpp) on
// synthetic 8/12/16-bit grayscale and 8-bit RGB images, single and multi-frame: lossless pairs
// must be bit exact, lossy pairs within PSNR limits, and the Image Pixel / lossy attributes and
// the offset tables must follow PS3.5 / PS3.3.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "image/color.hpp"
#include "image/frames.hpp"
#include "image/pixel_info.hpp"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr Tag kLossyRatio = 0x00282112;
constexpr Tag kLossyMethod = 0x00282114;

const std::vector<std::string_view>& all_syntaxes() {
    static const std::vector<std::string_view> v = {
        "1.2.840.10008.1.2",       "1.2.840.10008.1.2.1",     "1.2.840.10008.1.2.2",
        "1.2.840.10008.1.2.1.99",  "1.2.840.10008.1.2.1.98",  "1.2.840.10008.1.2.5",
        "1.2.840.10008.1.2.4.50",  "1.2.840.10008.1.2.4.51",  "1.2.840.10008.1.2.4.57",
        "1.2.840.10008.1.2.4.70",  "1.2.840.10008.1.2.4.80",  "1.2.840.10008.1.2.4.81",
        "1.2.840.10008.1.2.4.90",  "1.2.840.10008.1.2.4.91",  "1.2.840.10008.1.2.4.201",
        "1.2.840.10008.1.2.4.202", "1.2.840.10008.1.2.4.203",
    };
    return v;
}

enum class Kind { Gray8, Gray12, Gray16, Rgb8 };

struct Image {
    Kind kind;
    std::uint32_t frames;
    Dataset ds;                                // explicit VR little endian, native
    std::vector<std::vector<std::uint8_t>> px;  // per frame, interleaved
};

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Gray8: return "gray8";
        case Kind::Gray12: return "gray12";
        case Kind::Gray16: return "gray16";
        default: return "rgb8";
    }
}

Image make_image(Kind k, std::uint32_t frames, bool is_signed = false) {
    Image im{k, frames, {}, {}};
    const std::uint16_t rows = 21, cols = 35;
    const bool rgb = k == Kind::Rgb8;
    const unsigned ba = (k == Kind::Gray8 || rgb) ? 8 : 16;
    const unsigned bs = k == Kind::Gray12 ? 12 : (ba == 8 ? 8 : 16);
    const unsigned spp = rgb ? 3 : 1;
    Dataset& ds = im.ds;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.4242");
    (void)ds.set_uint16(tags::SamplesPerPixel, static_cast<std::uint16_t>(spp));
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, rgb ? "RGB" : "MONOCHROME2");
    if (rgb) (void)ds.set_uint16(tags::PlanarConfiguration, 0);
    if (frames > 1) (void)ds.set_string(tags::NumberOfFrames, VR::IS, std::to_string(frames));
    (void)ds.set_uint16(tags::Rows, rows);
    (void)ds.set_uint16(tags::Columns, cols);
    (void)ds.set_uint16(tags::BitsAllocated, static_cast<std::uint16_t>(ba));
    (void)ds.set_uint16(tags::BitsStored, static_cast<std::uint16_t>(bs));
    (void)ds.set_uint16(tags::HighBit, static_cast<std::uint16_t>(bs - 1));
    (void)ds.set_uint16(tags::PixelRepresentation, is_signed ? 1 : 0);
    const std::int64_t maxv = (std::int64_t{1} << bs) - 1;
    std::uint32_t seed = 12345;
    std::vector<std::uint8_t> all;
    for (std::uint32_t f = 0; f < frames; ++f) {
        std::vector<std::uint8_t> fr;
        for (std::uint32_t y = 0; y < rows; ++y)
            for (std::uint32_t x = 0; x < cols; ++x)
                for (unsigned s = 0; s < spp; ++s) {
                    seed = seed * 1103515245u + 12345u;
                    const std::int64_t noise = static_cast<std::int64_t>((seed >> 16) % 9) - 4;
                    std::int64_t v = static_cast<std::int64_t>(x) * (maxv / 2) / cols +
                                     static_cast<std::int64_t>((y + 3 * s) % rows) * (maxv / 4) / rows +
                                     static_cast<std::int64_t>(f) * (maxv / 16) + maxv / 16 + noise * (maxv / 255 + 1);
                    v = std::clamp<std::int64_t>(v, 0, maxv);
                    if (is_signed) v -= (maxv + 1) / 2;
                    const auto u = static_cast<std::uint64_t>(v) & ((ba == 8) ? 0xFFu : 0xFFFFu);
                    fr.push_back(static_cast<std::uint8_t>(u));
                    if (ba == 16) fr.push_back(static_cast<std::uint8_t>(u >> 8));
                }
        all.insert(all.end(), fr.begin(), fr.end());
        im.px.push_back(std::move(fr));
    }
    if (all.size() & 1u) all.push_back(0);
    (void)ds.set_bytes(tags::PixelData, ba == 8 ? VR::OB : VR::OW, Bytes::copy_of(all.data(), all.size()));
    return im;
}

// Canonical frames: interleaved samples, colour converted to RGB.
std::optional<std::vector<std::vector<std::uint8_t>>> canonical(const Dataset& ds, std::string_view ts) {
    Result<image::FrameReader> fr = image::FrameReader::open(ds, ts);
    if (!fr) {
        std::fprintf(stderr, "    open: %s\n", fr.error().message.c_str());
        return std::nullopt;
    }
    std::vector<std::vector<std::uint8_t>> out;
    const image::PixelInfo& pi = fr->info();
    for (std::uint32_t f = 0; f < fr->frame_count(); ++f) {
        Result<std::vector<std::uint8_t>> d = fr->decode(f);
        if (!d) {
            std::fprintf(stderr, "    decode: %s\n", d.error().message.c_str());
            return std::nullopt;
        }
        if (pi.samples_per_pixel == 3) {
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image::rgb_frame_bytes(pi)));
            image::ColorOptions co;
            co.codec_applied_mct = fr->codec_applies_color_transform();
            co.native_layout = !fr->encapsulated();
            if (!image::to_rgb(pi, *d, rgb, co)) return std::nullopt;
            out.push_back(std::move(rgb));
        } else {
            out.push_back(std::move(d.value()));
        }
    }
    return out;
}

double psnr(const std::vector<std::vector<std::uint8_t>>& a, const std::vector<std::vector<std::uint8_t>>& b,
            unsigned bytes, double peak, std::int64_t* max_err = nullptr) {
    double se = 0;
    double n = 0;
    std::int64_t me = 0;
    for (std::size_t f = 0; f < a.size() && f < b.size(); ++f)
        for (std::size_t i = 0; i + bytes <= a[f].size() && i + bytes <= b[f].size(); i += bytes) {
            const std::int64_t x = bytes == 1 ? a[f][i] : load_le16(&a[f][i]);
            const std::int64_t y = bytes == 1 ? b[f][i] : load_le16(&b[f][i]);
            se += static_cast<double>((x - y) * (x - y));
            me = std::max<std::int64_t>(me, x > y ? x - y : y - x);
            n += 1;
        }
    if (max_err) *max_err = me;
    if (se == 0) return 99.0;
    return 10.0 * std::log10(peak * peak / (se / n));
}

bool supported(std::string_view ts, Kind k) {
    if (ts == "1.2.840.10008.1.2.4.50") return k == Kind::Gray8 || k == Kind::Rgb8;
    if (ts == "1.2.840.10008.1.2.4.51") return k != Kind::Gray16;
    return true;
}
bool is_native_ts(std::string_view ts) {
    const TransferSyntaxInfo* t = find_transfer_syntax(ts);
    return t && !t->encapsulated;
}

std::string str(const Dataset& ds, Tag t) { return ds.get_string(t).value_or(std::string("<absent>")); }

std::size_t vm(const Dataset& ds, Tag t) { return ds.contains(t) ? ds.value_multiplicity(t) : 0; }

// Checks one encoded result against the original image.
void check_result(const Image& im, std::string_view src_ts, std::string_view ts, const Dataset& ds,
                  const std::vector<std::vector<std::uint8_t>>& reference, bool reference_exact, int lossy_steps,
                  double min_psnr, const std::string& label) {
    const bool rgb = im.kind == Kind::Rgb8;
    auto fail = [&](const char* what) { std::fprintf(stderr, "  %s: %s\n", label.c_str(), what); };
    std::optional<std::vector<std::vector<std::uint8_t>>> got = canonical(ds, ts);
    if (!got) {
        fail("cannot decode result");
        CHECK(false);
        return;
    }
    CHECK_EQ(got->size(), static_cast<std::size_t>(im.frames));
    const unsigned bytes = im.kind == Kind::Gray8 || rgb ? 1 : 2;
    const double peak = im.kind == Kind::Gray12 ? 4095.0 : (bytes == 1 ? 255.0 : 65535.0);
    if (reference_exact) {
        if (*got != reference) fail("lossless result differs");
        CHECK(*got == reference);
    } else {
        const double v = psnr(*got, reference, bytes, peak);
        if (v < min_psnr) std::fprintf(stderr, "  %s: PSNR %.2f < %.2f\n", label.c_str(), v, min_psnr);
        CHECK(v >= min_psnr);
    }
    // unchanged geometry
    CHECK_EQ(ds.get_int(tags::Rows).value_or(0), 21);
    CHECK_EQ(ds.get_int(tags::Columns).value_or(0), 35);
    CHECK_EQ(ds.get_int(tags::BitsStored).value_or(0), im.kind == Kind::Gray12 ? 12 : (bytes == 1 ? 8 : 16));
    CHECK_EQ(ds.get_int(tags::BitsAllocated).value_or(0), bytes == 1 ? 8 : 16);
    CHECK_EQ(ds.get_int(tags::NumberOfFrames).value_or(1), static_cast<std::int64_t>(im.frames));
    // photometric interpretation
    const std::string pi = str(ds, tags::PhotometricInterpretation);
    if (!rgb) {
        CHECK_EQ(pi, std::string("MONOCHROME2"));
    } else {
        std::string want = "RGB";
        if (ts == "1.2.840.10008.1.2.4.50" || ts == "1.2.840.10008.1.2.4.51") want = "YBR_FULL_422";
        else if (ts == "1.2.840.10008.1.2.4.90" || ts == "1.2.840.10008.1.2.4.201" ||
                 ts == "1.2.840.10008.1.2.4.202" || ts == "1.2.840.10008.1.2.4.203" || ts == "1.2.840.10008.1.2.4.91")
            want = "YBR_RCT";
        if (src_ts == ts) want = pi;  // unchanged copy
        if (pi != want) std::fprintf(stderr, "  %s: photometric %s, expected %s\n", label.c_str(), pi.c_str(), want.c_str());
        CHECK_EQ(pi, want);
        CHECK_EQ(ds.get_int(tags::PlanarConfiguration).value_or(-1), 0);
    }
    // lossy attributes
    if (lossy_steps > 0) {
        CHECK_EQ(str(ds, image::tags::LossyImageCompression), std::string("01"));
    } else {
        CHECK(!ds.contains(image::tags::LossyImageCompression));
    }
    // encapsulation: one fragment per frame, even lengths, Basic Offset Table
    const Element* e = ds.find(tags::PixelData);
    REQUIRE(e);
    if (!is_native_ts(ts)) {
        REQUIRE(e->fragments.size() == im.frames + 1u);
        CHECK_EQ(e->fragments[0].size(), 4u * im.frames);
        std::uint64_t off = 0;
        for (std::uint32_t f = 0; f < im.frames; ++f) {
            CHECK_EQ(e->fragments[f + 1].size() % 2, 0u);
            if (e->fragments[0].size() >= 4u * (f + 1)) CHECK_EQ(load_le32(e->fragments[0].data() + 4 * f), off);
            off += 8 + e->fragments[f + 1].size();
        }
    } else {
        CHECK(!e->is_encapsulated());
        CHECK_EQ(e->value.size() % 2, 0u);
    }
}

// Writes and reads back a Part 10 stream; the decoded frames must not change.
void check_roundtrip(const Dataset& ds, std::string_view ts, const std::string& label) {
    DicomFile f;
    f.dataset = ds;
    f.transfer_syntax = std::string(ts);
    std::vector<std::uint8_t> buf;
    Status w = write_part10(f, buf);
    if (!w) std::fprintf(stderr, "  %s: write: %s\n", label.c_str(), w.message.c_str());
    REQUIRE(w.is_ok());
    Result<DicomFile> r = read_part10(Bytes::copy_of(buf.data(), buf.size()));
    REQUIRE(r.is_ok());
    CHECK_EQ(r->transfer_syntax, std::string(ts));
    auto a = canonical(ds, ts);
    auto b = canonical(r->dataset, ts);
    CHECK(a && b && *a == *b);
}

}  // namespace

TEST_CASE("transcode: every source x target pair, all image kinds, single and multi-frame") {
    int pairs = 0, refused = 0;
    for (Kind k : {Kind::Gray8, Kind::Gray12, Kind::Gray16, Kind::Rgb8}) {
        for (std::uint32_t frames : {1u, 3u}) {
            const Image im = make_image(k, frames);
            for (std::string_view s : all_syntaxes()) {
                codec::TranscodeReport srep;
                Result<Dataset> src = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", s, {}, &srep);
                if (!supported(s, k)) {
                    CHECK(!src.is_ok() && src.error().code == Errc::Unsupported);
                    ++refused;
                    continue;
                }
                if (!src) std::fprintf(stderr, "  source %s %s: %s\n", kind_name(k), std::string(s).c_str(),
                                       src.error().message.c_str());
                REQUIRE(src.is_ok());
                const bool s_lossy = srep.lossy_applied;
                std::optional<std::vector<std::vector<std::uint8_t>>> src_canon = canonical(*src, s);
                REQUIRE(src_canon.has_value());
                for (std::string_view t : all_syntaxes()) {
                    const std::string label = std::string(kind_name(k)) + " x" + std::to_string(frames) + " " +
                                              std::string(s) + " -> " + std::string(t);
                    codec::TranscodeReport rep;
                    Result<Dataset> dst = codec::transcode_dataset(*src, s, t, {}, &rep);
                    if (!supported(t, k)) {
                        CHECK(!dst.is_ok() && dst.error().code == Errc::Unsupported);
                        ++refused;
                        continue;
                    }
                    if (!dst) std::fprintf(stderr, "  %s: %s\n", label.c_str(), dst.error().message.c_str());
                    REQUIRE(dst.is_ok());
                    ++pairs;
                    const bool t_lossy = rep.lossy_applied && s != t;
                    CHECK_EQ(rep.source_lossy, s_lossy);
                    const int steps = (s_lossy ? 1 : 0) + (t_lossy ? 1 : 0);
                    // Reference: the original when nothing lossy happened, the decoded source when
                    // only the source was lossy, the original (with a PSNR limit) otherwise.
                    const bool exact = !t_lossy;
                    double limit = k == Kind::Gray12 ? 38.0 : 30.0;
                    if (k == Kind::Rgb8) limit = 28.0;
                    if (s_lossy) limit -= 3.0;
                    check_result(im, s, t, *dst, (exact && s_lossy) ? *src_canon : im.px, exact, steps, limit, label);
                    if (t_lossy) {
                        CHECK_EQ(vm(*dst, kLossyRatio), static_cast<std::size_t>(steps));
                        CHECK_EQ(vm(*dst, kLossyMethod), static_cast<std::size_t>(steps));
                        CHECK_EQ(dst->get_string(kLossyMethod, static_cast<std::size_t>(steps - 1)).value_or(""),
                                 std::string("ISO_10918_1"));
                    } else if (s_lossy && s != t) {
                        CHECK_EQ(vm(*dst, kLossyMethod), 1u);
                    }
                    if (frames == 3 || k == Kind::Rgb8) check_roundtrip(*dst, t, label);
                }
            }
        }
    }
    std::printf("  transcode matrix: %d pairs checked, %d refusals checked\n", pairs, refused);
    CHECK(pairs > 1000);
}

TEST_CASE("transcode: lossy modes of JPEG 2000, JPEG-LS and HTJ2K transfer syntaxes") {
    for (Kind k : {Kind::Gray8, Kind::Gray16, Kind::Rgb8}) {
        const Image im = make_image(k, 2);
        const unsigned bytes = k == Kind::Gray16 ? 2 : 1;
        const double peak = bytes == 1 ? 255.0 : 65535.0;
        codec::TranscodeParams p;
        p.lossy = true;
        p.near_lossless = 2;
        codec::TranscodeReport rep;
        // JPEG 2000 irreversible: YBR_ICT for colour, method ISO_15444_1.
        Result<Dataset> j = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.91", p, &rep);
        REQUIRE(j.is_ok());
        CHECK(rep.lossy_applied);
        CHECK_EQ(str(*j, image::tags::LossyImageCompression), std::string("01"));
        CHECK_EQ(str(*j, kLossyMethod), std::string("ISO_15444_1"));
        CHECK_EQ(str(*j, tags::PhotometricInterpretation), std::string(k == Kind::Rgb8 ? "YBR_ICT" : "MONOCHROME2"));
        auto jc = canonical(*j, "1.2.840.10008.1.2.4.91");
        REQUIRE(jc.has_value());
        CHECK(psnr(*jc, im.px, bytes, peak) > 30.0);
        // The irreversible codestream is recognised as lossy when transcoded further.
        Result<Dataset> back = codec::transcode_dataset(*j, "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.1", {}, &rep);
        REQUIRE(back.is_ok());
        CHECK(rep.source_lossy);
        CHECK(!rep.lossy_applied);
        CHECK_EQ(str(*back, image::tags::LossyImageCompression), std::string("01"));
        CHECK_EQ(str(*back, tags::PhotometricInterpretation), std::string(k == Kind::Rgb8 ? "RGB" : "MONOCHROME2"));
        CHECK_EQ(vm(*back, kLossyRatio), 1u);
        // JPEG-LS near-lossless: |error| <= NEAR.
        Result<Dataset> l = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.81", p, &rep);
        REQUIRE(l.is_ok());
        CHECK(rep.lossy_applied);
        CHECK_EQ(str(*l, kLossyMethod), std::string("ISO_14495_1"));
        auto lc = canonical(*l, "1.2.840.10008.1.2.4.81");
        REQUIRE(lc.has_value());
        std::int64_t me = 0;
        (void)psnr(*lc, im.px, bytes, peak, &me);
        CHECK(me <= 2);
        CHECK(me > 0);
        // HTJ2K lossy is not available from this encoder.
        Result<Dataset> h = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.203", p);
        CHECK(!h.is_ok() && h.error().code == Errc::Unsupported);
    }
}

TEST_CASE("transcode: lossy history accumulates and is never reset") {
    const Image im = make_image(Kind::Gray8, 1);
    Result<Dataset> a = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.50");
    REQUIRE(a.is_ok());
    Result<Dataset> b = codec::transcode_dataset(*a, "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51");
    REQUIRE(b.is_ok());
    CHECK_EQ(vm(*b, kLossyRatio), 2u);
    CHECK_EQ(vm(*b, kLossyMethod), 2u);
    Result<Dataset> c = codec::transcode_dataset(*b, "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.90");
    REQUIRE(c.is_ok());
    CHECK_EQ(str(*c, image::tags::LossyImageCompression), std::string("01"));
    CHECK_EQ(vm(*c, kLossyRatio), 2u);
    Result<Dataset> d = codec::transcode_dataset(*c, "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2");
    REQUIRE(d.is_ok());
    CHECK_EQ(str(*d, image::tags::LossyImageCompression), std::string("01"));
    // A ratio value is a valid DS: digits and one '.'.
    const std::string r = d->get_string(kLossyRatio, 0).value_or("");
    CHECK(!r.empty() && r.size() <= 16 && r.find('.') != std::string::npos);
    // A source whose attribute already says "01" keeps it through lossless steps.
    Dataset marked = im.ds;
    (void)marked.set_string(image::tags::LossyImageCompression, VR::CS, "01");
    Result<Dataset> e = codec::transcode_dataset(marked, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.5");
    REQUIRE(e.is_ok());
    CHECK_EQ(str(*e, image::tags::LossyImageCompression), std::string("01"));
}

TEST_CASE("transcode: allow_lossy = false refuses lossy targets") {
    const Image im = make_image(Kind::Rgb8, 1);
    codec::TranscodeParams p;
    p.allow_lossy = false;
    Result<Dataset> a = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.50", p);
    CHECK(!a.is_ok() && a.error().code == Errc::Rejected);
    p.lossy = true;
    Result<Dataset> b = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.91", p);
    CHECK(!b.is_ok() && b.error().code == Errc::Rejected);
    p.lossy = false;
    Result<Dataset> c = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.91", p);
    CHECK(c.is_ok());
    CHECK(codec::encoding_is_lossy("1.2.840.10008.1.2.4.50", p));
    CHECK(!codec::encoding_is_lossy("1.2.840.10008.1.2.4.91", p));
}

TEST_CASE("transcode: Extended Offset Table and empty Basic Offset Table") {
    const Image im = make_image(Kind::Gray16, 4);
    codec::TranscodeParams p;
    p.offset_table = codec::OffsetTableMode::Extended;
    Result<Dataset> a = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.80", p);
    REQUIRE(a.is_ok());
    const Element* e = a->find(tags::PixelData);
    REQUIRE(e && e->fragments.size() == 5);
    CHECK(e->fragments[0].empty());
    const Element* eot = a->find(image::tags::ExtendedOffsetTable);
    const Element* eotl = a->find(image::tags::ExtendedOffsetTableLengths);
    REQUIRE(eot && eotl);
    CHECK(eot->vr == VR::OV && eotl->vr == VR::OV);
    REQUIRE(eot->value.size() == 32 && eotl->value.size() == 32);
    std::uint64_t off = 0;
    for (int f = 0; f < 4; ++f) {
        CHECK_EQ(load_le64(eot->value.data() + 8 * f), off);
        const std::uint64_t len = load_le64(eotl->value.data() + 8 * f);
        CHECK(len == e->fragments[static_cast<std::size_t>(f) + 1].size() ||
              len + 1 == e->fragments[static_cast<std::size_t>(f) + 1].size());
        off += 8 + e->fragments[static_cast<std::size_t>(f) + 1].size();
    }
    auto ac = canonical(*a, "1.2.840.10008.1.2.4.80");
    CHECK(ac && *ac == im.px);
    check_roundtrip(*a, "1.2.840.10008.1.2.4.80", "eot");
    // Transcoding again drops the old table and writes a Basic Offset Table.
    Result<Dataset> b = codec::transcode_dataset(*a, "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.5");
    REQUIRE(b.is_ok());
    CHECK(!b->contains(image::tags::ExtendedOffsetTable));
    CHECK_EQ(b->find(tags::PixelData)->fragments[0].size(), 16u);
    p.offset_table = codec::OffsetTableMode::Empty;
    Result<Dataset> c = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.90", p);
    REQUIRE(c.is_ok());
    CHECK(c->find(tags::PixelData)->fragments[0].empty());
    auto cc = canonical(*c, "1.2.840.10008.1.2.4.90");
    CHECK(cc && *cc == im.px);
}

TEST_CASE("transcode: planar RGB and native YBR_FULL_422 sources") {
    // Colour-by-plane native RGB -> JPEG-LS: Planar Configuration becomes 0, pixels identical.
    const Image im = make_image(Kind::Rgb8, 2);
    Dataset planar = im.ds;
    std::vector<std::uint8_t> pl;
    const std::size_t npx = 21 * 35;
    for (const auto& fr : im.px)
        for (int s = 0; s < 3; ++s)
            for (std::size_t i = 0; i < npx; ++i) pl.push_back(fr[i * 3 + static_cast<std::size_t>(s)]);
    (void)planar.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(pl.data(), pl.size()));
    (void)planar.set_uint16(tags::PlanarConfiguration, 1);
    Result<Dataset> a = codec::transcode_dataset(planar, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.80");
    REQUIRE(a.is_ok());
    CHECK_EQ(a->get_int(tags::PlanarConfiguration).value_or(-1), 0);
    auto ac = canonical(*a, "1.2.840.10008.1.2.4.80");
    CHECK(ac && *ac == im.px);
    // Native YBR_FULL_422 (Y1 Y2 Cb Cr): expanded and delivered as RGB.
    Dataset ybr;
    (void)ybr.set_uint16(tags::SamplesPerPixel, 3);
    (void)ybr.set_string(tags::PhotometricInterpretation, VR::CS, "YBR_FULL_422");
    (void)ybr.set_uint16(tags::PlanarConfiguration, 0);
    (void)ybr.set_uint16(tags::Rows, 4);
    (void)ybr.set_uint16(tags::Columns, 6);
    (void)ybr.set_uint16(tags::BitsAllocated, 8);
    (void)ybr.set_uint16(tags::BitsStored, 8);
    (void)ybr.set_uint16(tags::HighBit, 7);
    (void)ybr.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> v;
    for (int i = 0; i < 4 * 3; ++i) {
        v.push_back(static_cast<std::uint8_t>(40 + i * 5));
        v.push_back(static_cast<std::uint8_t>(42 + i * 5));
        v.push_back(static_cast<std::uint8_t>(120 + i));
        v.push_back(static_cast<std::uint8_t>(130 - i));
    }
    (void)ybr.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(v.data(), v.size()));
    Result<Dataset> b = codec::transcode_dataset(ybr, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.5");
    REQUIRE(b.is_ok());
    CHECK_EQ(str(*b, tags::PhotometricInterpretation), std::string("RGB"));
    auto bc = canonical(*b, "1.2.840.10008.1.2.5");
    auto yc = canonical(ybr, "1.2.840.10008.1.2.1");
    CHECK(bc && yc && *bc == *yc);
    // native -> native leaves the 4:2:2 layout alone
    Result<Dataset> n = codec::transcode_dataset(ybr, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2");
    REQUIRE(n.is_ok());
    CHECK_EQ(str(*n, tags::PhotometricInterpretation), std::string("YBR_FULL_422"));
}

TEST_CASE("transcode: signed 16-bit data through the lossless codecs") {
    const Image im = make_image(Kind::Gray16, 2, true);
    for (std::string_view t : {"1.2.840.10008.1.2.5", "1.2.840.10008.1.2.4.57", "1.2.840.10008.1.2.4.70",
                               "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.201",
                               "1.2.840.10008.1.2.1.98"}) {
        Result<Dataset> a = codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", t);
        if (!a) std::fprintf(stderr, "  signed -> %s: %s\n", std::string(t).c_str(), a.error().message.c_str());
        REQUIRE(a.is_ok());
        auto ac = canonical(*a, t);
        CHECK(ac && *ac == im.px);
        Result<Dataset> back = codec::transcode_dataset(*a, t, "1.2.840.10008.1.2.2");
        REQUIRE(back.is_ok());
        auto bc = canonical(*back, "1.2.840.10008.1.2.2");
        CHECK(bc && *bc == im.px);
    }
    // Near-lossless JPEG-LS and lossy JPEG refuse signed data rather than corrupting it (lossless
    // JPEG-LS codes the two's complement bit patterns exactly, checked above).
    codec::TranscodeParams near;
    near.lossy = true;
    near.allow_lossy = true;
    CHECK(!codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.81", near).is_ok());
    CHECK(!codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.50", near).is_ok());
}

TEST_CASE("transcode: files, unknown syntaxes, data sets without pixel data") {
    const Image im = make_image(Kind::Gray8, 1);
    DicomFile f;
    f.dataset = im.ds;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    (void)f.meta.set_string(tags::TransferSyntaxUID, VR::UI, f.transfer_syntax);
    REQUIRE(codec::transcode_file(f, "1.2.840.10008.1.2.4.70").is_ok());
    CHECK_EQ(f.transfer_syntax, std::string("1.2.840.10008.1.2.4.70"));
    CHECK_EQ(f.meta.get_string(tags::TransferSyntaxUID).value_or(""), std::string("1.2.840.10008.1.2.4.70"));
    // failure leaves the file unchanged
    CHECK(!codec::transcode_file(f, "1.2.840.10008.1.2.4.100").is_ok());
    CHECK_EQ(f.transfer_syntax, std::string("1.2.840.10008.1.2.4.70"));
    CHECK(!codec::can_encode("1.2.840.10008.1.2.4.100"));
    CHECK(codec::can_decode("1.2.840.10008.1.2.4.91"));
    CHECK(!codec::can_decode("1.2.840.10008.1.2.4.110"));
    // no pixel data: any target is a plain re-encoding
    Dataset sr;
    (void)sr.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.88.11");
    Result<Dataset> a = codec::transcode_dataset(sr, "1.2.840.10008.1.2", "1.2.840.10008.1.2.4.50");
    CHECK(a.is_ok());
    // bad parameters
    codec::TranscodeParams p;
    p.predictor = 9;
    CHECK(!codec::transcode_dataset(im.ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.57", p).is_ok());
}

#include "siaara/siaara.h"

TEST_CASE("transcode: C API siaara_file_transcode") {
    const Image im = make_image(Kind::Rgb8, 2);
    DicomFile f;
    f.dataset = im.ds;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    std::vector<std::uint8_t> buf;
    REQUIRE(write_part10(f, buf).is_ok());
    siaara_file* h = nullptr;
    REQUIRE(siaara_file_open_memory(buf.data(), buf.size(), nullptr, &h) == SIAARA_OK);
    CHECK(siaara_file_transcode(h, "1.2.840.10008.1.2.4.90", nullptr) == SIAARA_OK);
    char ts[80];
    size_t need = 0;
    CHECK(siaara_file_transcode(nullptr, "1.2.840.10008.1.2.4.90", nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    REQUIRE(siaara_file_transfer_syntax(h, ts, sizeof ts, &need) == SIAARA_OK);
    CHECK_EQ(std::string(ts), std::string("1.2.840.10008.1.2.4.90"));
    siaara_transcode_params p{};
    p.struct_size = sizeof p;
    p.allow_lossy = 0;
    CHECK(siaara_file_transcode(h, "1.2.840.10008.1.2.4.50", &p) == SIAARA_ERR_REJECTED);
    p.allow_lossy = 1;
    p.quality = 85;
    p.chroma_subsampling = 0;
    CHECK(siaara_file_transcode(h, "1.2.840.10008.1.2.4.50", &p) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(h, &ds) == SIAARA_OK);
    char pi[32];
    REQUIRE(siaara_dataset_get_string(ds, 0x00280004, 0, pi, sizeof pi, &need) == SIAARA_OK);
    CHECK_EQ(std::string(pi), std::string("YBR_FULL"));
    p.offset_table = 7;
    CHECK(siaara_file_transcode(h, "1.2.840.10008.1.2.1", &p) == SIAARA_ERR_INVALID_ARGUMENT);
    p.offset_table = 0;
    CHECK(siaara_file_transcode(h, "1.2.840.10008.1.2.4.100", &p) == SIAARA_ERR_UNSUPPORTED);
    CHECK(siaara_file_transcode(h, "1.2.840.10008.1.2.2", &p) == SIAARA_OK);
    siaara_file_free(h);
}
