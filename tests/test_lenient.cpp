// Lenient frame decoding (src/image/frames.*): encapsulated codestreams whose own header disagrees
// with the Image Pixel module. Every case is a synthetic file made with our own encoders and a
// deliberately inconsistent data set; strict mode must keep refusing it.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/j2k.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpegls.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "image/frames.hpp"
#include "image/render.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::image;

namespace {

using Buf = std::vector<std::uint8_t>;
using W = FrameWarningCode;
constexpr std::string_view kJpegLossless = "1.2.840.10008.1.2.4.70";
constexpr std::string_view kJpegExtended = "1.2.840.10008.1.2.4.51";
constexpr std::string_view kJpegLs = "1.2.840.10008.1.2.4.80";
constexpr std::string_view kJ2k = "1.2.840.10008.1.2.4.90";

enum class Enc { J2k, JpegLs, JpegLossless, JpegLossy };

std::string_view ts_of(Enc e) {
    switch (e) {
        case Enc::J2k: return kJ2k;
        case Enc::JpegLs: return kJpegLs;
        case Enc::JpegLossless: return kJpegLossless;
        default: return kJpegExtended;
    }
}

codec::FrameInfo fi(std::uint32_t rows, std::uint32_t cols, int spp, int ba, int bs, bool sgn) {
    codec::FrameInfo f;
    f.rows = rows;
    f.columns = cols;
    f.samples_per_pixel = static_cast<std::uint16_t>(spp);
    f.bits_allocated = static_cast<std::uint16_t>(ba);
    f.bits_stored = static_cast<std::uint16_t>(bs);
    f.is_signed = sgn;
    return f;
}

// Interleaved samples, little-endian in `f.bits_allocated` containers; value(i) gives sample i.
template <class F>
Buf samples(const codec::FrameInfo& f, F value) {
    const std::size_t n = std::size_t{f.rows} * f.columns * f.samples_per_pixel;
    const std::size_t b = f.bits_allocated / 8u;
    Buf out(n * b);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<std::uint32_t>(value(i));
        if (b == 1) out[i] = static_cast<std::uint8_t>(v);
        else store_le16(&out[i * 2], static_cast<std::uint16_t>(v));
    }
    return out;
}

Buf encode(Enc e, const Buf& px, const codec::FrameInfo& f) {
    Buf out;
    codec::EncodeParams p;
    Status s;
    switch (e) {
        case Enc::J2k: s = codec::encode_j2k(px, f, p, out); break;
        case Enc::JpegLs: s = codec::encode_jpegls(px, f, p, out); break;
        case Enc::JpegLossless: s = codec::encode_jpeg(px, f, p, out); break;
        case Enc::JpegLossy:
            p.lossless = false;
            s = codec::encode_jpeg(px, f, p, out);
            break;
    }
    if (!s) std::fprintf(stderr, "  encode failed: %s\n", s.message.c_str());
    return out;
}

// A data set describing the image with the given (possibly wrong) attributes.
Dataset dataset(std::uint32_t rows, std::uint32_t cols, int spp, int ba, int bs, bool sgn, const std::vector<Buf>& frames) {
    Dataset ds;
    (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25.4242");
    (void)ds.set_uint16(siaara::tags::Rows, static_cast<std::uint16_t>(rows));
    (void)ds.set_uint16(siaara::tags::Columns, static_cast<std::uint16_t>(cols));
    (void)ds.set_uint16(siaara::tags::SamplesPerPixel, static_cast<std::uint16_t>(spp));
    (void)ds.set_uint16(siaara::tags::BitsAllocated, static_cast<std::uint16_t>(ba));
    (void)ds.set_uint16(siaara::tags::BitsStored, static_cast<std::uint16_t>(bs));
    (void)ds.set_uint16(siaara::tags::HighBit, static_cast<std::uint16_t>(bs - 1));
    (void)ds.set_uint16(siaara::tags::PixelRepresentation, sgn ? 1 : 0);
    (void)ds.set_string(siaara::tags::PhotometricInterpretation, VR::CS, spp == 3 ? "RGB" : "MONOCHROME2");
    if (spp > 1) (void)ds.set_uint16(siaara::tags::PlanarConfiguration, 0);
    if (frames.size() != 1) (void)ds.set_string(siaara::tags::NumberOfFrames, VR::IS, std::to_string(frames.size()));
    Element e;
    e.tag = siaara::tags::PixelData;
    e.vr = VR::OB;
    e.undefined_length = true;
    e.fragments.push_back(Bytes());
    for (const Buf& f : frames) {
        Buf g = f;
        if (g.size() & 1u) g.push_back(0);
        e.fragments.push_back(Bytes::copy_of(g.data(), g.size()));
    }
    ds.put(std::move(e));
    return ds;
}

FrameOptions strict_options() {
    FrameOptions o;
    o.lenient = false;
    o.threads = 1;
    return o;
}

bool has(const std::vector<FrameWarning>& w, W code) {
    for (const auto& x : w)
        if (x.code == code) return true;
    return false;
}

// Sample i of a decoded frame (data set layout) as an integer per Pixel Representation.
std::int64_t value_at(const PixelInfo& pi, const Buf& frame, std::size_t i) {
    const unsigned b = pi.bits_allocated / 8u;
    std::uint64_t raw = b == 1 ? frame[i] : b == 2 ? load_le16(&frame[i * 2]) : load_le32(&frame[i * 4]);
    const unsigned bits = pi.bits_stored;
    raw &= (std::uint64_t{1} << bits) - 1;
    if (pi.is_signed && (raw >> (bits - 1)) & 1u) return static_cast<std::int64_t>(raw) - (std::int64_t{1} << bits);
    return static_cast<std::int64_t>(raw);
}

// Opens `ds` leniently and strictly; returns the lenient frame 0 (empty when it fails).
struct Outcome {
    bool strict_refused = false;
    bool lenient_ok = false;
    Status lenient_status;
    PixelInfo info;
    std::uint16_t declared_bs = 0;
    std::vector<FrameWarning> open_warnings, decode_warnings, check_warnings;
    Buf frame;
};

Outcome run(const Dataset& ds, std::string_view ts, std::uint32_t index = 0) {
    Outcome o;
    auto s = FrameReader::open(ds, ts, strict_options());
    o.strict_refused = !s || !s->decode(index).is_ok();
    FrameOptions lo;
    lo.threads = 1;
    auto r = FrameReader::open(ds, ts, lo);
    if (!r) {
        o.lenient_status = r.error();
        return o;
    }
    o.info = r->info();
    o.declared_bs = r->declared_bits_stored();
    o.open_warnings = r->frame_warnings();
    (void)r->check_frame(index, o.check_warnings);
    o.frame.resize(static_cast<std::size_t>(r->frame_bytes()));
    o.lenient_status = r->decode(index, o.frame, &o.decode_warnings);
    o.lenient_ok = o.lenient_status.is_ok();
    if (!o.lenient_ok) o.frame.clear();
    return o;
}

template <class F>
bool values_equal(const Outcome& o, std::size_t n, F expect) {
    if (!o.lenient_ok) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (value_at(o.info, o.frame, i) != static_cast<std::int64_t>(expect(i))) {
            std::fprintf(stderr, "  sample %zu: %lld, expected %lld\n", i, static_cast<long long>(value_at(o.info, o.frame, i)),
                         static_cast<long long>(expect(i)));
            return false;
        }
    return true;
}

std::uint32_t ramp(std::size_t i, std::uint32_t mod) { return static_cast<std::uint32_t>((i * 37u + 11u) % mod); }

}  // namespace

// ================================================================ precision vs Bits Stored (all codecs)

TEST_CASE("lenient: codestream precision below Bits Stored (J2K, JPEG-LS, JPEG)") {
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const auto f = fi(12, 10, 1, 16, 12, false);
        const Buf px = samples(f, [](std::size_t i) { return ramp(i, 4096); });
        const Dataset ds = dataset(12, 10, 1, 16, 16, false, {encode(e, px, f)});
        const Outcome o = run(ds, ts_of(e));
        CHECK(o.strict_refused);
        CHECK(values_equal(o, 120, [](std::size_t i) { return ramp(i, 4096); }));
        CHECK(has(o.open_warnings, W::PrecisionBelowBitsStored));
        CHECK(has(o.decode_warnings, W::PrecisionBelowBitsStored));
        CHECK(has(o.check_warnings, W::PrecisionBelowBitsStored));
        CHECK_EQ(o.info.bits_stored, 16);
    }
}

TEST_CASE("lenient: codestream precision above Bits Stored keeps the values (J2K, JPEG-LS, JPEG)") {
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const auto f = fi(9, 11, 1, 16, 16, false);
        const Buf px = samples(f, [](std::size_t i) { return ramp(i, 65536) * 7u % 65536u; });
        const Dataset ds = dataset(9, 11, 1, 16, 12, false, {encode(e, px, f)});
        const Outcome o = run(ds, ts_of(e));
        CHECK(o.strict_refused);
        CHECK_EQ(o.declared_bs, 12);
        CHECK_EQ(o.info.bits_stored, 16);  // display uses the codestream precision
        CHECK_EQ(o.info.high_bit, 15);
        CHECK(values_equal(o, 99, [](std::size_t i) { return ramp(i, 65536) * 7u % 65536u; }));
        CHECK(has(o.open_warnings, W::PrecisionAboveBitsStored));
        CHECK(has(o.decode_warnings, W::PrecisionAboveBitsStored));
    }
}

TEST_CASE("lenient: R-026 JPEG 12-bit stream with Bits Stored 8 (strict error, lenient warning)") {
    for (Enc e : {Enc::JpegLossless, Enc::JpegLossy}) {
        const auto f = fi(16, 16, 1, 16, 12, false);
        const Buf px = samples(f, [](std::size_t i) { return 1000u + static_cast<std::uint32_t>(i % 16) * 150u; });
        const Dataset ds = dataset(16, 16, 1, 16, 8, false, {encode(e, px, f)});
        auto s = FrameReader::open(ds, ts_of(e), strict_options());
        REQUIRE(s.is_ok());
        auto sd = s->decode(0);
        CHECK(!sd.is_ok());
        if (!sd) CHECK(sd.error().code == Errc::TypeMismatch);
        const Outcome o = run(ds, ts_of(e));
        CHECK(o.lenient_ok);
        CHECK_EQ(o.info.bits_stored, 12);
        CHECK(has(o.open_warnings, W::PrecisionAboveBitsStored));
        if (e == Enc::JpegLossless) CHECK(values_equal(o, 256, [](std::size_t i) { return 1000u + (i % 16) * 150u; }));
    }
}

TEST_CASE("lenient: R-026 J2K and JPEG-LS precision vs Bits Stored, strict refuses both ways") {
    for (Enc e : {Enc::J2k, Enc::JpegLs}) {
        for (int declared : {8, 16}) {
            const auto f = fi(8, 8, 1, 16, 12, false);
            const Buf px = samples(f, [](std::size_t i) { return ramp(i, 256); });
            const Dataset ds = dataset(8, 8, 1, 16, declared, false, {encode(e, px, f)});
            const Outcome o = run(ds, ts_of(e));
            CHECK(o.strict_refused);
            CHECK(o.lenient_ok);
            CHECK(has(o.decode_warnings, declared < 12 ? W::PrecisionAboveBitsStored : W::PrecisionBelowBitsStored));
            CHECK(values_equal(o, 64, [](std::size_t i) { return ramp(i, 256); }));
        }
    }
}

TEST_CASE("lenient: a later frame with more precision than frame 0 must fit the display precision") {
    const auto f12 = fi(8, 8, 1, 16, 12, false);
    const auto f16 = fi(8, 8, 1, 16, 16, false);
    const Buf a = samples(f12, [](std::size_t i) { return ramp(i, 4096); });
    const Buf small = samples(f16, [](std::size_t i) { return ramp(i, 4096); });   // fits 12 bits
    const Buf big = samples(f16, [](std::size_t i) { return 40000u + ramp(i, 100); });
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const Dataset ds = dataset(8, 8, 1, 16, 10, false, {encode(e, a, f12), encode(e, small, f16), encode(e, big, f16)});
        FrameOptions lo;
        lo.threads = 1;
        auto r = FrameReader::open(ds, ts_of(e), lo);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->info().bits_stored, 12);
        std::vector<FrameWarning> w;
        auto ok1 = r->decode(1);
        CHECK(ok1.is_ok());
        Buf out(static_cast<std::size_t>(r->frame_bytes()));
        CHECK(r->decode(1, out, &w).is_ok());
        CHECK(has(w, W::PrecisionAboveBitsStored));
        CHECK(!r->decode(2).is_ok());  // values above 12 bits: refused clearly
    }
}

// ================================================================ signedness (JPEG 2000)

TEST_CASE("lenient: J2K unsigned codestream, Pixel Representation 1 (pydicom rule: reinterpret low P bits)") {
    // 13-bit signed values stored as an unsigned 13-bit codestream (like J2K_pixelrep_mismatch.dcm).
    auto sval = [](std::size_t i) { return static_cast<std::int32_t>(ramp(i, 8192)) - 4096; };
    const auto f = fi(10, 10, 1, 16, 13, false);
    const Buf px = samples(f, [&](std::size_t i) { return static_cast<std::uint32_t>(sval(i)) & 0x1FFFu; });
    const Dataset ds = dataset(10, 10, 1, 16, 13, true, {encode(Enc::J2k, px, f)});
    const Outcome o = run(ds, kJ2k);
    CHECK(o.strict_refused);
    CHECK(has(o.decode_warnings, W::SignednessMismatch));
    CHECK(values_equal(o, 100, sval));
    // The 16-bit container holds the sign-extended value.
    if (o.lenient_ok) CHECK_EQ(static_cast<std::int16_t>(load_le16(&o.frame[0])), sval(0));
}

TEST_CASE("lenient: J2K signedness mismatch at full precision is a bit-pattern reinterpretation") {
    // Signed 16-bit codestream, Pixel Representation 0 (like TOSHIBA_J2K_SIZ1_PixRep0.dcm), and the
    // reverse (TOSHIBA_J2K_SIZ0_PixRep1.dcm).
    for (bool cs_signed : {true, false}) {
        auto v = [](std::size_t i) { return static_cast<std::int32_t>(ramp(i, 65536)) - 32768; };
        const auto f = fi(6, 7, 1, 16, 16, cs_signed);
        const Buf px = samples(f, [&](std::size_t i) { return static_cast<std::uint32_t>(v(i)) & 0xFFFFu; });
        const Dataset ds = dataset(6, 7, 1, 16, 16, !cs_signed, {encode(Enc::J2k, px, f)});
        const Outcome o = run(ds, kJ2k);
        CHECK(o.strict_refused);
        CHECK(has(o.decode_warnings, W::SignednessMismatch));
        CHECK(o.lenient_ok && o.frame == px);  // same 16-bit patterns
    }
}

TEST_CASE("lenient: JPEG-LS signed data with precision below Bits Stored is sign-extended from P") {
    auto sval = [](std::size_t i) { return static_cast<std::int32_t>(ramp(i, 4096)) - 2048; };
    const auto f = fi(9, 9, 1, 16, 12, false);
    const Buf px = samples(f, [&](std::size_t i) { return static_cast<std::uint32_t>(sval(i)) & 0xFFFu; });
    const Dataset ds = dataset(9, 9, 1, 16, 16, true, {encode(Enc::JpegLs, px, f)});
    const Outcome o = run(ds, kJpegLs);
    CHECK(o.strict_refused);
    CHECK(values_equal(o, 81, sval));
}

// ================================================================ size and components

TEST_CASE("lenient: codestream larger than Rows x Columns is cropped, smaller is refused") {
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const auto f = fi(12, 14, 1, 16, 12, false);
        const Buf px = samples(f, [](std::size_t i) { return ramp(i, 4096); });
        const Buf cs = encode(e, px, f);
        const Outcome o = run(dataset(10, 9, 1, 16, 12, false, {cs}), ts_of(e));
        CHECK(o.strict_refused);
        CHECK(has(o.decode_warnings, W::CodestreamCropped));
        CHECK(values_equal(o, 90, [](std::size_t i) { return ramp((i / 9) * 14 + i % 9, 4096); }));
        const Outcome bigger = run(dataset(13, 14, 1, 16, 12, false, {cs}), ts_of(e));
        CHECK(bigger.strict_refused);
        CHECK(!bigger.lenient_ok);
        CHECK(bigger.lenient_status.code == Errc::Malformed);
    }
}

TEST_CASE("lenient: more components than Samples per Pixel uses the first, fewer is refused") {
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const auto f3 = fi(8, 6, 3, 8, 8, false);
        const Buf rgb = samples(f3, [](std::size_t i) { return ramp(i, 256); });
        const Outcome o = run(dataset(8, 6, 1, 8, 8, false, {encode(e, rgb, f3)}), ts_of(e));
        CHECK(o.strict_refused);
        CHECK(has(o.decode_warnings, W::ExtraComponents));
        CHECK(values_equal(o, 48, [](std::size_t i) { return ramp(i * 3, 256); }));

        const auto f1 = fi(8, 6, 1, 8, 8, false);
        const Buf gray = samples(f1, [](std::size_t i) { return ramp(i, 256); });
        const Outcome fewer = run(dataset(8, 6, 3, 8, 8, false, {encode(e, gray, f1)}), ts_of(e));
        CHECK(fewer.strict_refused);
        CHECK(!fewer.lenient_ok);
        CHECK(fewer.lenient_status.code == Errc::Malformed);
    }
}

TEST_CASE("lenient: colour-by-plane output after a lenient conversion") {
    const auto f = fi(5, 4, 3, 16, 12, false);
    const Buf rgb = samples(f, [](std::size_t i) { return ramp(i, 4096); });
    Dataset ds = dataset(5, 4, 3, 16, 16, false, {encode(Enc::J2k, rgb, f)});
    (void)ds.set_uint16(siaara::tags::PlanarConfiguration, 1);
    const Outcome o = run(ds, kJ2k);
    REQUIRE(o.lenient_ok);
    bool ok = true;
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t p = 0; p < 20; ++p)
            ok = ok && load_le16(&o.frame[(c * 20 + p) * 2]) == ramp(p * 3 + c, 4096);
    CHECK(ok);
}

// ================================================================ Bits Allocated

TEST_CASE("lenient: precision above Bits Allocated decodes only when the values fit") {
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const auto f = fi(7, 7, 1, 16, 12, false);
        const Buf fits = samples(f, [](std::size_t i) { return ramp(i, 256); });
        const Outcome o = run(dataset(7, 7, 1, 8, 8, false, {encode(e, fits, f)}), ts_of(e));
        CHECK(o.strict_refused);
        CHECK(has(o.decode_warnings, W::PrecisionAboveBitsAllocated));
        CHECK(values_equal(o, 49, [](std::size_t i) { return ramp(i, 256); }));
        const Buf wide = samples(f, [](std::size_t i) { return 300u + ramp(i, 256); });
        const Outcome bad = run(dataset(7, 7, 1, 8, 8, false, {encode(e, wide, f)}), ts_of(e));
        CHECK(!bad.lenient_ok);
        CHECK(bad.lenient_status.code == Errc::Malformed);
    }
}

TEST_CASE("lenient: 16-bit codestreams in a 32-bit container are widened (JPEG-LS, JPEG)") {
    for (Enc e : {Enc::JpegLs, Enc::JpegLossless}) {
        const auto f = fi(6, 5, 1, 16, 16, true);
        auto v = [](std::size_t i) { return static_cast<std::int32_t>(ramp(i, 65536)) - 32768; };
        const Buf px = samples(f, [&](std::size_t i) { return static_cast<std::uint32_t>(v(i)) & 0xFFFFu; });
        const Outcome o = run(dataset(6, 5, 1, 32, 16, true, {encode(e, px, f)}), ts_of(e));
        CHECK(o.strict_refused);
        CHECK(has(o.decode_warnings, W::ContainerWidened));
        CHECK(values_equal(o, 30, v));
        if (o.lenient_ok) CHECK_EQ(static_cast<std::int32_t>(load_le32(&o.frame[4])), v(1));  // sign-extended
    }
}

// ================================================================ consistent files, rendering, C API

TEST_CASE("lenient: a consistent codestream has no frame warnings") {
    for (Enc e : {Enc::J2k, Enc::JpegLs, Enc::JpegLossless}) {
        const auto f = fi(8, 8, 1, 16, 12, false);
        const Buf px = samples(f, [](std::size_t i) { return ramp(i, 4096); });
        const Outcome o = run(dataset(8, 8, 1, 16, 12, false, {encode(e, px, f)}), ts_of(e));
        CHECK(!o.strict_refused);
        CHECK(o.lenient_ok && o.frame == px);
        CHECK(o.open_warnings.empty() && o.decode_warnings.empty() && o.check_warnings.empty());
    }
}

TEST_CASE("lenient: mismatched files render through Image") {
    const auto f = fi(10, 10, 1, 16, 16, false);
    const Buf px = samples(f, [](std::size_t i) { return ramp(i, 65536); });
    const Dataset ds = dataset(10, 10, 1, 16, 12, false, {encode(Enc::J2k, px, f)});
    auto img = Image::open(ds, kJ2k);
    REQUIRE(img.is_ok());
    CHECK_EQ(img->info().bits_stored, 16);
    RenderParams p;
    p.voi = VoiMode::FullRange;
    auto out = render_2d(*img, p);
    CHECK(out.is_ok());
    FrameOptions so;
    so.lenient = false;
    auto simg = Image::open(ds, kJ2k, so);
    CHECK(!simg || !render_2d(*simg, p).is_ok());
}

TEST_CASE("lenient: frame warnings through the C API") {
    const auto f = fi(8, 8, 1, 16, 13, false);
    const Buf px = samples(f, [](std::size_t i) { return ramp(i, 8192); });
    DicomFile file;
    file.transfer_syntax = std::string(kJ2k);
    file.dataset = dataset(8, 8, 1, 16, 13, true, {encode(Enc::J2k, px, f)});
    Buf bytes;
    REQUIRE(write_part10(file, bytes).is_ok());
    siaara_file* h = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &h) == SIAARA_OK);
    std::uint32_t n = 0;
    CHECK(siaara_frame_warning_count(h, 0, &n) == SIAARA_OK);
    CHECK_EQ(n, 1u);
    std::int32_t code = 0;
    std::size_t need = 0;
    CHECK(siaara_frame_warning(h, 0, 0, &code, nullptr, 0, &need) == SIAARA_OK);
    CHECK_EQ(code, SIAARA_FRAME_WARN_SIGNEDNESS_MISMATCH);
    std::string msg(need + 1, '\0');
    CHECK(siaara_frame_warning(h, 0, 0, nullptr, msg.data(), msg.size(), &need) == SIAARA_OK);
    CHECK(msg.find("Pixel Representation") != std::string::npos);
    CHECK(siaara_frame_warning(h, 0, 1, &code, nullptr, 0, &need) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_frame_warning_count(h, 1, &n) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_frame_warning_count(nullptr, 0, &n) == SIAARA_ERR_INVALID_ARGUMENT);
    std::size_t fb = 0;
    CHECK(siaara_frame_decode(h, 0, SIAARA_FRAME_NATIVE, nullptr, 0, &fb) == SIAARA_OK);
    Buf out(fb);
    CHECK(siaara_frame_decode(h, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &fb) == SIAARA_OK);
    // A value above 4095 in 13 bits is negative per Pixel Representation 1.
    CHECK_EQ(static_cast<std::int16_t>(load_le16(&out[2 * 9])), static_cast<std::int16_t>(ramp(9, 8192) - (ramp(9, 8192) >= 4096 ? 8192 : 0)));
    siaara_file_free(h);
}
