// Pixel pipeline tests (src/image): pixel description, frame access for every encoding, colour
// models, palettes, LUT maths (PS3.3 C.11), overlays, shutters, rendering and robustness.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "image/color.hpp"
#include "image/frames.hpp"
#include "image/lut.hpp"
#include "image/overlay.hpp"
#include "image/pixel_info.hpp"
#include "image/render.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::image;

namespace {

using Buf = std::vector<std::uint8_t>;
constexpr std::string_view kExplicitLE = "1.2.840.10008.1.2.1";
constexpr std::string_view kRle = "1.2.840.10008.1.2.5";
constexpr std::string_view kJpegLossless = "1.2.840.10008.1.2.4.70";
constexpr std::string_view kJpegBaseline = "1.2.840.10008.1.2.4.50";
constexpr std::string_view kJpegExtended = "1.2.840.10008.1.2.4.51";
constexpr std::string_view kJpegLs = "1.2.840.10008.1.2.4.80";
constexpr std::string_view kJpegLsNear = "1.2.840.10008.1.2.4.81";
constexpr std::string_view kJ2k = "1.2.840.10008.1.2.4.90";
constexpr std::string_view kEncUncompressed = "1.2.840.10008.1.2.1.98";

void put_us(Dataset& ds, Tag t, std::uint16_t v) { (void)ds.set_uint16(t, v); }
void put_str(Dataset& ds, Tag t, VR vr, std::string_view s) { (void)ds.set_string(t, vr, s); }
void put_raw(Dataset& ds, Tag t, VR vr, const Buf& b) { (void)ds.set_bytes(t, vr, Bytes::copy_of(b.data(), b.size())); }
void put_us_multi(Dataset& ds, Tag t, VR vr, std::initializer_list<int> vals) {
    Buf b;
    for (int v : vals) {
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    put_raw(ds, t, vr, b);
}

Dataset image_ds(std::uint16_t rows, std::uint16_t cols, std::uint16_t spp, std::uint16_t ba, std::uint16_t bs,
                 bool is_signed, std::string_view photometric, int frames = 1, bool planar = false) {
    Dataset ds;
    put_us(ds, siaara::tags::Rows, rows);
    put_us(ds, siaara::tags::Columns, cols);
    put_us(ds, siaara::tags::SamplesPerPixel, spp);
    put_us(ds, siaara::tags::BitsAllocated, ba);
    put_us(ds, siaara::tags::BitsStored, bs);
    put_us(ds, siaara::tags::HighBit, static_cast<std::uint16_t>(bs - 1));
    put_us(ds, siaara::tags::PixelRepresentation, is_signed ? 1 : 0);
    put_str(ds, siaara::tags::PhotometricInterpretation, VR::CS, photometric);
    if (spp > 1) put_us(ds, siaara::tags::PlanarConfiguration, planar ? 1 : 0);
    if (frames != 1) put_str(ds, siaara::tags::NumberOfFrames, VR::IS, std::to_string(frames));
    return ds;
}

void set_native(Dataset& ds, const Buf& px) {
    Buf b = px;
    if (b.size() & 1u) b.push_back(0);
    put_raw(ds, siaara::tags::PixelData, VR::OW, b);
}

void set_encapsulated(Dataset& ds, const Buf& bot, const std::vector<Buf>& frags) {
    Element e;
    e.tag = siaara::tags::PixelData;
    e.vr = VR::OB;
    e.undefined_length = true;
    e.fragments.push_back(Bytes::copy_of(bot.data(), bot.size()));
    for (const Buf& f : frags) {
        Buf g = f;
        if (g.size() & 1u) g.push_back(0);
        e.fragments.push_back(Bytes::copy_of(g.data(), g.size()));
    }
    ds.put(std::move(e));
}

Buf le32s(const std::vector<std::uint32_t>& v) {
    Buf b(v.size() * 4);
    for (std::size_t i = 0; i < v.size(); ++i) store_le32(b.data() + 4 * i, v[i]);
    return b;
}
Buf le64s(const std::vector<std::uint64_t>& v) {
    Buf b(v.size() * 8);
    for (std::size_t i = 0; i < v.size(); ++i) {
        store_le32(b.data() + 8 * i, static_cast<std::uint32_t>(v[i]));
        store_le32(b.data() + 8 * i + 4, static_cast<std::uint32_t>(v[i] >> 32));
    }
    return b;
}

Buf random_frame(std::size_t n, unsigned bits_stored, unsigned bytes, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    Buf b(n * bytes);
    const std::uint64_t mask = bits_stored >= 64 ? ~0ull : (1ull << bits_stored) - 1;
    for (std::size_t i = 0; i < n; ++i) {
        // smooth-ish data compresses and exercises predictors
        const std::uint64_t v = (static_cast<std::uint64_t>(i * 37 + (rng() % 7))) & mask;
        for (unsigned k = 0; k < bytes; ++k) b[i * bytes + k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
    return b;
}

codec::FrameInfo finfo(const Dataset& ds) {
    auto pi = read_pixel_info(ds);
    return pi ? pi->frame_info() : codec::FrameInfo{};
}

template <class T>
std::size_t sz(T v) { return static_cast<std::size_t>(v); }

const std::uint8_t* px_at(const Buf& rgba, std::uint32_t cols, std::uint32_t r, std::uint32_t c) {
    return rgba.data() + (std::size_t{r} * cols + c) * 4;
}

}  // namespace

// ================================================================ pixel description

TEST_CASE("image: pixel info basics and frame sizes") {
    Dataset ds = image_ds(4, 5, 1, 16, 12, true, "MONOCHROME2", 3);
    set_native(ds, Buf(4 * 5 * 2 * 3));
    auto pi = read_pixel_info(ds);
    REQUIRE(pi.is_ok());
    CHECK_EQ(pi->rows, 4u);
    CHECK_EQ(pi->columns, 5u);
    CHECK_EQ(pi->number_of_frames, 3u);
    CHECK(pi->is_signed);
    CHECK(pi->photometric == Photometric::Monochrome2);
    CHECK_EQ(pi->native_frame_bytes(), 40u);
    CHECK_EQ(pi->decoded_frame_bytes(), 40u);
    CHECK_EQ(frame_count(ds).value(), 3u);

    Dataset y = image_ds(2, 4, 3, 8, 8, false, "YBR_FULL_422");
    set_native(y, Buf(2 * 4 * 2));
    auto py = read_pixel_info(y);
    REQUIRE(py.is_ok());
    CHECK_EQ(py->native_frame_bytes(), 16u);   // Y Y Cb Cr: 2 samples per pixel (PS3.3 C.7.6.3.1.2)
    CHECK_EQ(py->decoded_frame_bytes(), 24u);  // codec output has 3

    Dataset b = image_ds(3, 3, 1, 1, 1, false, "MONOCHROME2", 5);
    set_native(b, Buf(6));
    auto pb = read_pixel_info(b);
    REQUIRE(pb.is_ok());
    CHECK_EQ(pb->native_frame_bits(), 9u);
    CHECK_EQ(pb->native_frame_bytes(), 2u);

    Dataset f;
    put_us(f, siaara::tags::Rows, 2);
    put_us(f, siaara::tags::Columns, 2);
    put_us(f, siaara::tags::SamplesPerPixel, 1);
    put_us(f, siaara::tags::BitsAllocated, 32);
    put_str(f, siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    put_raw(f, siaara::tags::FloatPixelData, VR::OF, Buf(16));
    auto pf = read_pixel_info(f);
    REQUIRE(pf.is_ok());
    CHECK(pf->kind == SampleKind::Float32);
    CHECK(pf->pixel_tag == siaara::tags::FloatPixelData);
}

TEST_CASE("image: malformed pixel modules are rejected") {
    auto expect = [](Dataset ds, Errc code, bool lenient = true) {
        PixelInfoOptions o;
        o.lenient = lenient;
        auto r = read_pixel_info(ds, o);
        CHECK(!r.is_ok());
        if (!r.is_ok()) CHECK(r.error().code == code);
    };
    Dataset base = image_ds(4, 4, 1, 16, 16, false, "MONOCHROME2");
    set_native(base, Buf(32));
    for (int ba : {0, 2, 12, 24, 48, 128}) {
        Dataset d = base;
        put_us(d, siaara::tags::BitsAllocated, static_cast<std::uint16_t>(ba));
        expect(d, Errc::Malformed);
    }
    { Dataset d = base; put_us(d, siaara::tags::BitsStored, 17); expect(d, Errc::Malformed); }
    { Dataset d = base; put_us(d, siaara::tags::BitsStored, 0); expect(d, Errc::Malformed); }
    { Dataset d = base; put_us(d, siaara::tags::HighBit, 16); expect(d, Errc::Malformed); }
    { Dataset d = base; put_us(d, siaara::tags::Rows, 0); expect(d, Errc::Malformed); }
    { Dataset d = base; d.remove(siaara::tags::Columns); expect(d, Errc::Malformed); }
    { Dataset d = base; put_us(d, siaara::tags::SamplesPerPixel, 3); expect(d, Errc::Malformed); }
    { Dataset d = base; put_us(d, siaara::tags::SamplesPerPixel, 2); expect(d, Errc::Malformed); }
    { Dataset d = base; put_us(d, siaara::tags::PixelRepresentation, 2); expect(d, Errc::Malformed); }
    { Dataset d = base; put_str(d, siaara::tags::NumberOfFrames, VR::IS, "1A"); expect(d, Errc::Malformed); }
    { Dataset d = base; put_str(d, siaara::tags::NumberOfFrames, VR::IS, "-3"); expect(d, Errc::Malformed); }
    { Dataset d = base; put_str(d, siaara::tags::NumberOfFrames, VR::IS, "0"); expect(d, Errc::Malformed, false); }
    { Dataset d = base; d.remove(siaara::tags::PixelData); expect(d, Errc::NotFound); }
    { Dataset d = base; put_str(d, siaara::tags::PhotometricInterpretation, VR::CS, "RGB"); expect(d, Errc::Malformed); }
    {
        Dataset d = image_ds(4, 4, 3, 1, 1, false, "RGB");
        set_native(d, Buf(6));
        expect(d, Errc::Malformed);
    }
    // High Bit != Bits Stored - 1: strict refuses, lenient shifts.
    {
        Dataset d = base;
        put_us(d, siaara::tags::BitsStored, 12);
        put_us(d, siaara::tags::HighBit, 13);
        expect(d, Errc::Malformed, false);
        auto r = read_pixel_info(d);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->low_bit(), 2u);
        CHECK(!r->warnings.empty());
    }
    // Missing Photometric Interpretation: lenient assumes MONOCHROME2.
    {
        Dataset d = base;
        d.remove(siaara::tags::PhotometricInterpretation);
        expect(d, Errc::Malformed, false);
        auto r = read_pixel_info(d);
        REQUIRE(r.is_ok());
        CHECK(r->photometric == Photometric::Monochrome2);
    }
    // FrameReader: pixel data too short; strict and lenient.
    {
        Dataset d = image_ds(4, 4, 1, 16, 16, false, "MONOCHROME2", 3);
        set_native(d, Buf(32 * 2));
        FrameOptions o;
        o.lenient = false;
        CHECK(!FrameReader::open(d, kExplicitLE, o).is_ok());
        auto r = FrameReader::open(d, kExplicitLE);
        REQUIRE(r.is_ok());
        Buf out(32);
        CHECK(r->decode(1, out).is_ok());
        CHECK(r->decode(2, out).code == Errc::Truncated);
        CHECK(r->decode(3, out).code == Errc::InvalidArgument);
    }
}

// ================================================================ native frames

TEST_CASE("image: native frames 8/12/16/32 bits, signed, planar, multi-frame") {
    struct C { std::uint16_t ba, bs; bool sgn; std::uint16_t spp; bool planar; };
    for (C c : {C{8, 8, false, 1, false}, C{16, 12, false, 1, false}, C{16, 16, true, 1, false},
                C{32, 32, false, 1, false}, C{32, 24, true, 1, false}, C{8, 8, false, 3, true}, C{16, 16, false, 3, false}}) {
        const char* photo = c.spp == 3 ? "RGB" : "MONOCHROME2";
        Dataset ds = image_ds(3, 5, c.spp, c.ba, c.bs, c.sgn, photo, 4, c.planar);
        const std::size_t fb = std::size_t{3} * 5 * c.spp * (c.ba / 8u);
        Buf all = random_frame(fb / (c.ba / 8u) * 4, c.bs, c.ba / 8u, c.ba + c.bs);
        set_native(ds, all);
        auto r = FrameReader::open(ds, kExplicitLE);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->frame_bytes(), fb);
        for (std::uint32_t f = 0; f < 4; ++f) {
            auto out = r->decode(f);
            REQUIRE(out.is_ok());
            CHECK(std::equal(out->begin(), out->end(), all.begin() + static_cast<std::ptrdiff_t>(f * fb)));
            auto v = r->native_view(f);
            CHECK(v.has_value());
        }
    }
}

TEST_CASE("image: 1-bit frames that are not byte aligned") {
    // 3x3 = 9 bits per frame, 4 frames: frames 1..3 start inside a byte (PS3.5 8.1.1).
    std::vector<int> bits(36);
    for (std::size_t i = 0; i < bits.size(); ++i) bits[i] = (i * 7 + i / 3) % 3 == 0;
    Buf packed(5, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) packed[i / 8] = static_cast<std::uint8_t>(packed[i / 8] | (1u << (i % 8)));
    Dataset ds = image_ds(3, 3, 1, 1, 1, false, "MONOCHROME2", 4);
    set_native(ds, packed);
    auto r = FrameReader::open(ds, kExplicitLE);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->frame_bytes(), 2u);
    for (std::uint32_t f = 0; f < 4; ++f) {
        auto out = r->decode(f);
        REQUIRE(out.is_ok());
        for (std::size_t i = 0; i < 9; ++i) CHECK_EQ(((*out)[i / 8] >> (i % 8)) & 1, bits[f * 9 + i]);
        CHECK_EQ((*out)[1] & 0xFE, 0);  // bits past the frame are cleared
    }
    // Render: 1 -> white, 0 -> black (full range window)
    auto img = Image::open(ds, kExplicitLE);
    REQUIRE(img.is_ok());
    RenderParams p;
    p.frame = 2;
    auto rgba = render_2d(*img, p);
    REQUIRE(rgba.is_ok());
    for (std::size_t i = 0; i < 9; ++i) CHECK_EQ((*rgba)[4 * i], bits[18 + i] ? 255 : 0);
}

TEST_CASE("image: big-endian 16-bit pixel data round trip") {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRBigEndian);
    f.dataset = image_ds(2, 3, 1, 16, 16, false, "MONOCHROME2", 2);
    (void)f.dataset.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)f.dataset.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25.7");
    Buf px = random_frame(12, 16, 2, 99);
    set_native(f.dataset, px);
    Buf bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    auto g = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(g.is_ok());
    auto r = FrameReader::open(g->dataset, g->transfer_syntax);
    REQUIRE(r.is_ok());
    CHECK(r->decode_range(0, 2).value() == px);
}

TEST_CASE("image: big-endian 32-bit samples in OW are put in little-endian order") {
    // Hand-built Explicit VR Big Endian data set (PS3.5 7.3): 32-bit samples stored big-endian.
    Buf s;
    auto el = [&](std::uint16_t g, std::uint16_t e, const char* vr, const Buf& v) {
        s.push_back(static_cast<std::uint8_t>(g >> 8)); s.push_back(static_cast<std::uint8_t>(g));
        s.push_back(static_cast<std::uint8_t>(e >> 8)); s.push_back(static_cast<std::uint8_t>(e));
        s.push_back(static_cast<std::uint8_t>(vr[0])); s.push_back(static_cast<std::uint8_t>(vr[1]));
        if (vr[0] == 'O') {
            s.push_back(0); s.push_back(0);
            const auto n = static_cast<std::uint32_t>(v.size());
            for (int k = 3; k >= 0; --k) s.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
        } else {
            s.push_back(static_cast<std::uint8_t>(v.size() >> 8)); s.push_back(static_cast<std::uint8_t>(v.size()));
        }
        s.insert(s.end(), v.begin(), v.end());
    };
    auto us = [](std::uint16_t v) { return Buf{static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)}; };
    el(0x0028, 0x0002, "US", us(1));
    el(0x0028, 0x0004, "CS", Buf{'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
    el(0x0028, 0x0008, "IS", Buf{'2', ' '});
    el(0x0028, 0x0010, "US", us(1));
    el(0x0028, 0x0011, "US", us(3));
    el(0x0028, 0x0100, "US", us(32));
    el(0x0028, 0x0101, "US", us(32));
    el(0x0028, 0x0102, "US", us(31));
    el(0x0028, 0x0103, "US", us(0));
    const std::uint32_t vals[6] = {0x01020304u, 0xA0B0C0D0u, 7u, 0xFFFFFFFEu, 0x00010000u, 0x80000000u};
    Buf be, le;
    for (std::uint32_t v : vals)
        for (int k = 0; k < 4; ++k) {
            be.push_back(static_cast<std::uint8_t>(v >> (24 - 8 * k)));
            le.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    el(0x7FE0, 0x0010, "OW", be);
    auto ds = read_dataset(Bytes::copy_of(s.data(), s.size()), ts::ExplicitVRBigEndian);
    REQUIRE(ds.is_ok());
    auto r = FrameReader::open(*ds, ts::ExplicitVRBigEndian);
    REQUIRE(r.is_ok());
    CHECK(!r->native_view(0).has_value());
    CHECK(r->decode_range(0, 2).value() == le);
}

TEST_CASE("image: big-endian 32-bit pixel data round trip through the writer") {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRBigEndian);
    f.dataset = image_ds(2, 3, 1, 32, 32, false, "MONOCHROME2", 2);
    (void)f.dataset.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)f.dataset.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25.8");
    Buf px = random_frame(12, 32, 4, 123);
    set_native(f.dataset, px);
    Buf bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    auto g = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(g.is_ok());
    auto r = FrameReader::open(g->dataset, g->transfer_syntax);
    REQUIRE(r.is_ok());
    CHECK(r->decode_range(0, 2).value() == px);
}

// ================================================================ encapsulated frames

TEST_CASE("image: RLE frames with and without offset tables") {
    Dataset ds = image_ds(6, 7, 3, 16, 16, false, "RGB", 3);
    const codec::FrameInfo fi = finfo([&] { Dataset d = ds; set_native(d, Buf(sz(6 * 7 * 6 * 3))); return d; }());
    std::vector<Buf> raw, enc;
    for (int f = 0; f < 3; ++f) {
        raw.push_back(random_frame(6 * 7 * 3, 16, 2, static_cast<std::uint64_t>(f)));
        Buf z;
        REQUIRE(codec::encode_rle(raw.back(), fi, {}, z).is_ok());
        enc.push_back(z);
    }
    // a) empty BOT, one fragment per frame
    set_encapsulated(ds, {}, enc);
    auto r = FrameReader::open(ds, kRle);
    REQUIRE(r.is_ok());
    for (std::uint32_t f = 0; f < 3; ++f) CHECK(r->decode(f).value() == raw[f]);
    // b) BOT
    std::vector<std::uint32_t> offs{0};
    for (int f = 0; f < 2; ++f) offs.push_back(offs.back() + 8 + static_cast<std::uint32_t>((enc[sz(f)].size() + 1) & ~1u));
    set_encapsulated(ds, le32s(offs), enc);
    r = FrameReader::open(ds, kRle);
    REQUIRE(r.is_ok());
    CHECK(r->decode(2).value() == raw[2]);
    // c) BOT pointing into a fragment: strict fails, lenient falls back (one fragment per frame)
    offs[1] += 2;
    set_encapsulated(ds, le32s(offs), enc);
    FrameOptions strict;
    strict.lenient = false;
    CHECK(!FrameReader::open(ds, kRle, strict).is_ok());
    r = FrameReader::open(ds, kRle);
    REQUIRE(r.is_ok());
    CHECK(r->decode(1).value() == raw[1]);
    CHECK(!r->warnings().empty());
    // d) fewer fragments than frames
    set_encapsulated(ds, {}, {enc[0], enc[1]});
    CHECK(FrameReader::open(ds, kRle).error().code == Errc::Malformed);
    // e) more fragments than frames: RLE needs exactly one per frame
    set_encapsulated(ds, {}, {enc[0], enc[1], enc[2], enc[2]});
    CHECK(FrameReader::open(ds, kRle).error().code == Errc::Malformed);
}

TEST_CASE("image: Extended Offset Table") {
    Dataset ds = image_ds(5, 5, 1, 8, 8, false, "MONOCHROME2", 3);
    const codec::FrameInfo fi = finfo([&] { Dataset d = ds; set_native(d, Buf(75)); return d; }());
    std::vector<Buf> raw, enc;
    for (int f = 0; f < 3; ++f) {
        raw.push_back(random_frame(25, 8, 1, static_cast<std::uint64_t>(10 + f)));
        Buf z;
        codec::EncodeParams ep;
        ep.lossless = true;
        REQUIRE(codec::encode_jpegls(raw.back(), fi, ep, z).is_ok());
        enc.push_back(z);
    }
    set_encapsulated(ds, {}, enc);
    std::vector<std::uint64_t> offs{0}, lens;
    for (int f = 0; f < 3; ++f) {
        lens.push_back(enc[sz(f)].size());
        if (f < 2) offs.push_back(offs.back() + 8 + ((enc[sz(f)].size() + 1) & ~std::size_t{1}));
    }
    put_raw(ds, image::tags::ExtendedOffsetTable, VR::OV, le64s(offs));
    put_raw(ds, image::tags::ExtendedOffsetTableLengths, VR::OV, le64s(lens));
    auto r = FrameReader::open(ds, kJpegLs);
    REQUIRE(r.is_ok());
    for (std::uint32_t f = 0; f < 3; ++f) CHECK(r->decode(f).value() == raw[f]);
    // Lengths exceeding the fragment, wrong count, missing lengths
    lens[1] += 100;
    put_raw(ds, image::tags::ExtendedOffsetTableLengths, VR::OV, le64s(lens));
    CHECK(!FrameReader::open(ds, kJpegLs).is_ok());
    put_raw(ds, image::tags::ExtendedOffsetTableLengths, VR::OV, le64s({1, 2}));
    CHECK(!FrameReader::open(ds, kJpegLs).is_ok());
    ds.remove(image::tags::ExtendedOffsetTableLengths);
    CHECK(!FrameReader::open(ds, kJpegLs).is_ok());
}

TEST_CASE("image: multi-fragment JPEG frames located by scanning (no table)") {
    Dataset ds = image_ds(16, 16, 1, 16, 12, false, "MONOCHROME2", 3);
    const codec::FrameInfo fi = finfo([&] { Dataset d = ds; set_native(d, Buf(16 * 16 * 2 * 3)); return d; }());
    std::vector<Buf> raw, frags;
    std::vector<std::uint32_t> bot;
    std::uint32_t pos = 0;
    for (int f = 0; f < 3; ++f) {
        raw.push_back(random_frame(256, 12, 2, static_cast<std::uint64_t>(20 + f)));
        Buf z;
        codec::EncodeParams ep;
        ep.lossless = true;
        REQUIRE(codec::encode_jpeg(raw.back(), fi, ep, z).is_ok());
        bot.push_back(pos);
        // split each codestream into 1 + f + 1 fragments of even size
        const std::size_t pieces = static_cast<std::size_t>(f) + 2;
        std::size_t step = (z.size() / pieces) & ~std::size_t{1};
        std::size_t at = 0;
        for (std::size_t k = 0; k < pieces; ++k) {
            const std::size_t end = k + 1 == pieces ? z.size() : at + step;
            Buf piece(z.begin() + static_cast<std::ptrdiff_t>(at), z.begin() + static_cast<std::ptrdiff_t>(end));
            pos += 8 + static_cast<std::uint32_t>((piece.size() + 1) & ~std::size_t{1});
            frags.push_back(piece);
            at = end;
        }
    }
    set_encapsulated(ds, {}, frags);
    auto r = FrameReader::open(ds, kJpegLossless);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->fragment_count(0), 2u);
    CHECK_EQ(r->fragment_count(2), 4u);
    for (std::uint32_t f = 0; f < 3; ++f) CHECK(r->decode(f).value() == raw[f]);
    set_encapsulated(ds, le32s(bot), frags);  // with BOT: same result
    r = FrameReader::open(ds, kJpegLossless);
    REQUIRE(r.is_ok());
    for (std::uint32_t f = 0; f < 3; ++f) CHECK(r->decode(f).value() == raw[f]);
    // parallel range equals serial
    FrameOptions par;
    par.threads = 3;
    auto rp = FrameReader::open(ds, kJpegLossless, par);
    REQUIRE(rp.is_ok());
    auto all = rp->decode_range(0, 3);
    REQUIRE(all.is_ok());
    Buf expect;
    for (auto& b : raw) expect.insert(expect.end(), b.begin(), b.end());
    CHECK(*all == expect);
}

TEST_CASE("image: every codec through the frame API") {
    struct C { std::string_view ts; std::uint16_t spp, ba, bs; const char* photo; bool lossy; };
    for (C c : {C{kRle, 1, 8, 8, "MONOCHROME2", false}, C{kRle, 3, 8, 8, "RGB", false}, C{kRle, 1, 32, 32, "MONOCHROME2", false},
                C{kJpegLossless, 1, 16, 16, "MONOCHROME2", false}, C{kJpegLossless, 3, 8, 8, "RGB", false},
                C{kJpegBaseline, 1, 8, 8, "MONOCHROME2", true}, C{kJpegBaseline, 3, 8, 8, "YBR_FULL", true},
                C{kJpegExtended, 1, 16, 12, "MONOCHROME2", true}, C{kJpegLs, 1, 16, 16, "MONOCHROME2", false},
                C{kJpegLs, 3, 8, 8, "RGB", false}, C{kJpegLsNear, 1, 8, 8, "MONOCHROME2", true},
                C{kEncUncompressed, 1, 16, 16, "MONOCHROME2", false}}) {
        Dataset ds = image_ds(16, 24, c.spp, c.ba, c.bs, false, c.photo, 2);
        const codec::FrameInfo fi = finfo([&] { Dataset d = ds; set_native(d, Buf(16 * 24 * 2 * c.spp * (c.ba / 8u))); return d; }());
        std::vector<Buf> raw, enc;
        for (int f = 0; f < 2; ++f) {
            raw.push_back(random_frame(std::size_t{16} * 24 * c.spp, c.bs, c.ba / 8u, static_cast<std::uint64_t>(f + 5)));
            Buf z;
            codec::EncodeParams ep;
            ep.lossless = !c.lossy;
            ep.near_lossless = c.ts == kJpegLsNear ? 2 : 0;
            ep.quality = 95;
            Status s;
            if (c.ts == kRle) s = codec::encode_rle(raw.back(), fi, ep, z);
            else if (c.ts == kJpegLs || c.ts == kJpegLsNear) s = codec::encode_jpegls(raw.back(), fi, ep, z);
            else if (c.ts == kEncUncompressed) z = raw.back();
            else s = codec::encode_jpeg(raw.back(), fi, ep, z);
            REQUIRE(s.is_ok());
            enc.push_back(z);
        }
        set_encapsulated(ds, {}, enc);
        auto r = FrameReader::open(ds, c.ts);
        REQUIRE(r.is_ok());
        for (std::uint32_t f = 0; f < 2; ++f) {
            auto out = r->decode(f);
            REQUIRE(out.is_ok());
            REQUIRE(out->size() == raw[f].size());
            if (!c.lossy) {
                CHECK(*out == raw[f]);
            } else {
                // lossy: bounded error
                const unsigned bps = c.ba / 8u;
                long worst = 0;
                for (std::size_t i = 0; i < out->size(); i += bps) {
                    long a = 0, b = 0;
                    for (unsigned k = 0; k < bps; ++k) {
                        a |= static_cast<long>((*out)[i + k]) << (8 * k);
                        b |= static_cast<long>(raw[f][i + k]) << (8 * k);
                    }
                    worst = std::max(worst, std::labs(a - b));
                }
                if (worst > (c.ts == kJpegLsNear ? 2 : (c.bs > 8 ? 256 : 24)))
                    std::printf("  lossy %s %s: max error %ld\n", std::string(c.ts).c_str(), c.photo, worst);
                CHECK(worst <= (c.ts == kJpegLsNear ? 2 : (c.bs > 8 ? 256 : 24)));
            }
        }
    }
    // JPEG 2000 dispatch reaches the codec (placeholder or real)
    Dataset ds = image_ds(4, 4, 1, 8, 8, false, "MONOCHROME2");
    set_encapsulated(ds, {}, {Buf{0xFF, 0x4F, 0xFF, 0x51, 0, 0, 0xFF, 0xD9}});
    auto r = FrameReader::open(ds, kJ2k);
    REQUIRE(r.is_ok());
    CHECK(!r->decode(0).is_ok());
    CHECK(r->codec_applies_color_transform());
    // Unknown / unsupported transfer syntaxes
    CHECK(FrameReader::open(ds, "1.2.3.4").error().code == Errc::Unsupported);
    CHECK(FrameReader::open(ds, "1.2.840.10008.1.2.4.100").error().code == Errc::Unsupported);  // MPEG2
    CHECK(FrameReader::open(ds, kExplicitLE).error().code == Errc::Malformed);
}

TEST_CASE("image: parallel decode is deterministic and reports the lowest failing frame") {
    Dataset ds = image_ds(8, 8, 1, 8, 8, false, "MONOCHROME2", 12);
    const codec::FrameInfo fi = finfo([&] { Dataset d = ds; set_native(d, Buf(64 * 12)); return d; }());
    std::vector<Buf> enc;
    Buf expect;
    for (int f = 0; f < 12; ++f) {
        Buf raw = random_frame(64, 8, 1, static_cast<std::uint64_t>(f));
        expect.insert(expect.end(), raw.begin(), raw.end());
        Buf z;
        REQUIRE(codec::encode_rle(raw, fi, {}, z).is_ok());
        enc.push_back(z);
    }
    set_encapsulated(ds, {}, enc);
    for (unsigned t : {1u, 2u, 4u, 0u}) {
        FrameOptions o;
        o.threads = t;
        auto r = FrameReader::open(ds, kRle, o);
        REQUIRE(r.is_ok());
        CHECK(r->decode_range(0, 12).value() == expect);
        auto part = r->decode_range(3, 5);
        REQUIRE(part.is_ok());
        CHECK(std::equal(part->begin(), part->end(), expect.begin() + 3 * 64));
    }
    enc[4] = Buf(10, 0);
    enc[9] = Buf(10, 0);
    set_encapsulated(ds, {}, enc);
    FrameOptions o;
    o.threads = 4;
    auto r = FrameReader::open(ds, kRle, o);
    REQUIRE(r.is_ok());
    for (int rep = 0; rep < 5; ++rep) {
        auto res = r->decode_range(0, 12);
        CHECK(!res.is_ok());
        if (!res.is_ok()) CHECK(res.error().message.find("frame 4") != std::string::npos);
    }
    CHECK(r->decode_range(10, 3).error().code == Errc::LimitExceeded ||
          r->decode_range(10, 3).error().code == Errc::InvalidArgument);
}

TEST_CASE("image: memory budget") {
    Dataset ds = image_ds(64, 64, 1, 16, 16, false, "MONOCHROME2", 4);
    set_native(ds, Buf(64 * 64 * 2 * 4));
    FrameOptions o;
    o.memory_budget = 64 * 64 * 2 * 2;
    auto r = FrameReader::open(ds, kExplicitLE, o);
    REQUIRE(r.is_ok());
    CHECK(r->decode(0).is_ok());
    CHECK(r->decode_range(0, 2).is_ok());
    CHECK(r->decode_range(0, 3).error().code == Errc::LimitExceeded);
    o.memory_budget = 100;
    r = FrameReader::open(ds, kExplicitLE, o);
    REQUIRE(r.is_ok());
    CHECK(r->decode(0).error().code == Errc::LimitExceeded);
    Buf caller(sz(r->frame_bytes()));
    CHECK(r->decode(0, caller).is_ok());  // caller buffers are not limited
}

// ================================================================ colour

TEST_CASE("image: planar and interleaved conversion") {
    Buf planar{1, 2, 3, 4, 10, 20, 30, 40, 100, 101, 102, 103};
    Buf inter(12), back(12);
    REQUIRE(planar_to_interleaved(planar, inter, 4, 3, 1).is_ok());
    CHECK(inter == (Buf{1, 10, 100, 2, 20, 101, 3, 30, 102, 4, 40, 103}));
    REQUIRE(interleaved_to_planar(inter, back, 4, 3, 1).is_ok());
    CHECK(back == planar);
    Buf p16{1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0}, i16(12);
    REQUIRE(planar_to_interleaved(p16, i16, 2, 3, 2).is_ok());
    CHECK(i16 == (Buf{1, 0, 3, 0, 5, 0, 2, 0, 4, 0, 6, 0}));
    CHECK(!planar_to_interleaved(Buf(5), inter, 4, 3, 1).is_ok());
}

TEST_CASE("image: YBR_FULL, YBR_PARTIAL, YBR_ICT, YBR_RCT to RGB") {
    // Forward-convert known RGB with the PS3.3 equations, then check the inverse within 1.
    const int rgbs[][3] = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 64, 200}, {17, 230, 99}};
    Dataset ds = image_ds(1, 7, 3, 8, 8, false, "YBR_FULL");
    set_native(ds, Buf(21));
    PixelInfo pi = read_pixel_info(ds).value();
    Buf full(21), partial(21);
    for (std::size_t i = 0; i < 7; ++i) {
        const double R = rgbs[i][0], G = rgbs[i][1], B = rgbs[i][2];
        auto cl = [](double v) { return static_cast<std::uint8_t>(std::clamp(std::floor(v + 0.5), 0.0, 255.0)); };
        full[3 * i] = cl(.2990 * R + .5870 * G + .1140 * B);
        full[3 * i + 1] = cl(-.1687 * R - .3313 * G + .5000 * B + 128);
        full[3 * i + 2] = cl(.5000 * R - .4187 * G - .0813 * B + 128);
        partial[3 * i] = cl(.2568 * R + .5041 * G + .0979 * B + 16);
        partial[3 * i + 1] = cl(-.1482 * R - .2910 * G + .4392 * B + 128);
        partial[3 * i + 2] = cl(.4392 * R - .3678 * G - .0714 * B + 128);
    }
    auto check = [&](const Buf& out) {
        for (std::size_t i = 0; i < 7; ++i)
            for (int k = 0; k < 3; ++k) CHECK(std::abs(int(out[3 * i + sz(k)]) - rgbs[i][k]) <= 2);
    };
    Buf out(21);
    REQUIRE(to_rgb(pi, full, out).is_ok());
    check(out);
    pi.photometric = Photometric::YbrPartial422;
    pi.photometric_text = "YBR_PARTIAL_422";
    ColorOptions codec_out;
    codec_out.native_layout = false;  // full samples, as a codec would produce
    REQUIRE(to_rgb(pi, partial, out, codec_out).is_ok());
    check(out);
    // ICT: chroma centred on zero, here stored unsigned with offset 128
    pi.photometric = Photometric::YbrIct;
    pi.photometric_text = "YBR_ICT";
    REQUIRE(to_rgb(pi, full, out, codec_out).is_ok());
    check(out);
    codec_out.codec_applied_mct = true;  // codec already gave RGB: copied
    REQUIRE(to_rgb(pi, full, out, codec_out).is_ok());
    CHECK(out == full);
    // RCT (ISO/IEC 15444-1 G.2) on level-shifted values
    pi.photometric = Photometric::YbrRct;
    pi.photometric_text = "YBR_RCT";
    codec_out.codec_applied_mct = false;
    Buf rct(21);
    for (std::size_t i = 0; i < 7; ++i) {
        const int R = rgbs[i][0], G = rgbs[i][1], B = rgbs[i][2];
        const int y0 = static_cast<int>(std::floor((R + 2 * G + B) / 4.0));
        rct[3 * i] = static_cast<std::uint8_t>(y0);
        rct[3 * i + 1] = static_cast<std::uint8_t>(std::clamp(B - G + 128, 0, 255));
        rct[3 * i + 2] = static_cast<std::uint8_t>(std::clamp(R - G + 128, 0, 255));
    }
    REQUIRE(to_rgb(pi, rct, out, codec_out).is_ok());
    // exact where the differences fit in 8 bits (black and white here)
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    CHECK(out[3] == 255 && out[4] == 255 && out[5] == 255);
    // Unsupported models
    pi.photometric = Photometric::Hsv;
    pi.photometric_text = "HSV";
    CHECK(to_rgb(pi, full, out).code == Errc::Unsupported);
    // 16-bit YBR_FULL: half scale is 2^(BitsStored-1)
    Dataset d16 = image_ds(1, 1, 3, 16, 12, false, "YBR_FULL");
    set_native(d16, Buf(6));
    PixelInfo p16 = read_pixel_info(d16).value();
    Buf in16{0x00, 0x08, 0x00, 0x08, 0x00, 0x08}, out16(6);  // Y = Cb = Cr = 2048
    REQUIRE(to_rgb(p16, in16, out16).is_ok());
    CHECK(out16 == in16);
}

TEST_CASE("image: native YBR_FULL_422 chroma upsampling") {
    Dataset ds = image_ds(1, 4, 3, 8, 8, false, "YBR_FULL_422");
    Buf stored{10, 20, 100, 200, 30, 40, 110, 210};  // Y1 Y2 Cb Cr per pair
    set_native(ds, stored);
    auto r = FrameReader::open(ds, kExplicitLE);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->frame_bytes(), 8u);
    Buf frame = r->decode(0).value();
    CHECK(frame == stored);
    const PixelInfo& pi = r->info();
    Buf full(12);
    REQUIRE(to_interleaved_full(pi, frame, full).is_ok());
    CHECK(full == (Buf{10, 100, 200, 20, 100, 200, 30, 110, 210, 40, 110, 210}));
    ColorOptions lin;
    lin.upsampling = ChromaUpsampling::Linear;
    REQUIRE(to_interleaved_full(pi, frame, full, lin).is_ok());
    CHECK(full == (Buf{10, 100, 200, 20, 105, 205, 30, 110, 210, 40, 110, 210}));
    Dataset odd = image_ds(1, 3, 3, 8, 8, false, "YBR_FULL_422");
    set_native(odd, Buf(6));
    CHECK(!FrameReader::open(odd, kExplicitLE).is_ok());
    // native YBR_PARTIAL_420 has no defined layout
    Dataset p420 = image_ds(2, 2, 3, 8, 8, false, "YBR_PARTIAL_420");
    set_native(p420, Buf(12));
    CHECK(!FrameReader::open(p420, kExplicitLE).is_ok());
}

// ================================================================ palettes

TEST_CASE("image: palette descriptors (65536 entries, first mapped, 8/16 bits)") {
    // 16-bit entries, descriptor 0 = 65536 entries
    {
        Dataset ds = image_ds(1, 4, 1, 16, 16, false, "PALETTE COLOR");
        set_native(ds, Buf{0, 0, 1, 0, 0xFF, 0xFF, 0x34, 0x12});
        Buf data(65536 * 2);
        for (std::size_t i = 0; i < 65536; ++i) store_le16(data.data() + 2 * i, static_cast<std::uint16_t>(i));
        for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u}) put_us_multi(ds, t, VR::US, {0, 0, 16});
        for (Tag t : {0x00281201u, 0x00281202u, 0x00281203u}) put_raw(ds, t, VR::OW, data);
        auto lut = read_palette(ds, false);
        REQUIRE(lut.is_ok());
        CHECK_EQ(lut->entries, 65536u);
        CHECK_EQ(lut->bits, 16);
        PixelInfo pi = read_pixel_info(ds).value();
        std::vector<std::uint16_t> out(12);
        REQUIRE(apply_palette(pi, ds.find(siaara::tags::PixelData)->value.span(), *lut, out).is_ok());
        CHECK(out[3] == 1 && out[6] == 0xFFFF && out[9] == 0x1234);
        CHECK_EQ(lut->to8(0xFFFF), 255);
        CHECK_EQ(lut->to8(0x8080), 128);
    }
    // signed first-mapped value, clamping below and above, 8-bit packed vs padded entries
    for (bool padded : {false, true}) {
        Dataset ds = image_ds(1, 5, 1, 16, 16, true, "PALETTE COLOR");
        Buf px;
        for (int v : {-200, -100, -99, -97, 500}) {
            px.push_back(static_cast<std::uint8_t>(v & 0xFF));
            px.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        }
        set_native(ds, px);
        for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u}) put_us_multi(ds, t, VR::SS, {4, -100, 8});
        Buf d = padded ? Buf{10, 0, 20, 0, 30, 0, 40, 0} : Buf{10, 20, 30, 40};
        for (Tag t : {0x00281201u, 0x00281202u, 0x00281203u}) put_raw(ds, t, VR::OW, d);
        auto lut = read_palette(ds, true);
        REQUIRE(lut.is_ok());
        CHECK_EQ(lut->first_mapped, -100);
        auto img = Image::open(ds, kExplicitLE);
        REQUIRE(img.is_ok());
        auto rgba = render_2d(*img, RenderParams{});
        REQUIRE(rgba.is_ok());
        CHECK_EQ((*rgba)[0], 10);   // -200: below first mapped
        CHECK_EQ((*rgba)[4], 10);   // -100: first entry
        CHECK_EQ((*rgba)[8], 20);   // -99
        CHECK_EQ((*rgba)[12], 40);  // -97: last entry
        CHECK_EQ((*rgba)[16], 40);  // 500: above the table
    }
    // descriptors that disagree, short data, bad bits
    {
        Dataset ds = image_ds(1, 1, 1, 8, 8, false, "PALETTE COLOR");
        set_native(ds, Buf{0, 0});
        put_us_multi(ds, 0x00281101, VR::US, {4, 0, 8});
        put_us_multi(ds, 0x00281102, VR::US, {5, 0, 8});
        put_us_multi(ds, 0x00281103, VR::US, {4, 0, 8});
        for (Tag t : {0x00281201u, 0x00281202u, 0x00281203u}) put_raw(ds, t, VR::OW, Buf(10));
        CHECK(!read_palette(ds, false).is_ok());
        put_us_multi(ds, 0x00281102, VR::US, {4, 0, 8});
        CHECK(read_palette(ds, false).is_ok());
        put_raw(ds, 0x00281202, VR::OW, Buf(2));
        CHECK(!read_palette(ds, false).is_ok());
        put_raw(ds, 0x00281202, VR::OW, Buf(8));
        for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u}) put_us_multi(ds, t, VR::US, {4, 0, 12});
        CHECK(!read_palette(ds, false).is_ok());
        ds.remove(0x00281103);
        CHECK(!read_palette(ds, false).is_ok());
        CHECK(!Image::open(ds, kExplicitLE).is_ok());  // PALETTE COLOR requires the tables
    }
}

TEST_CASE("image: segmented palettes (PS3.3 C.7.9.2)") {
    std::vector<std::uint16_t> out;
    // discrete 3 entries, linear to 100 over 4, indirect copy of the first segment
    std::vector<std::uint16_t> w{0, 3, 10, 20, 30, 1, 4, 70, 2, 1, 0, 0};
    REQUIRE(expand_segmented(w, 10, out).is_ok());
    CHECK(out == (std::vector<std::uint16_t>{10, 20, 30, 40, 50, 60, 70, 10, 20, 30}));
    CHECK(!expand_segmented(w, 11, out).is_ok());                          // too few entries
    CHECK(!expand_segmented(w, 9, out).is_ok());                           // too many
    CHECK(!expand_segmented(std::vector<std::uint16_t>{1, 3, 5}, 3, out).is_ok());   // linear first
    CHECK(!expand_segmented(std::vector<std::uint16_t>{0, 1, 5, 2, 1, 6, 0, 0, 2, 1, 0, 0}, 4, out).is_ok());  // indirect -> indirect
    CHECK(!expand_segmented(std::vector<std::uint16_t>{7, 1, 0}, 1, out).is_ok());   // reserved opcode
    CHECK(!expand_segmented(std::vector<std::uint16_t>{0, 9, 1}, 9, out).is_ok());   // truncated
    CHECK(!expand_segmented(std::vector<std::uint16_t>{0, 1, 1, 2, 1, 0x1000, 0x7FFF}, 2, out).is_ok());  // offset out of range
    // many zero-length indirect copies are bounded
    std::vector<std::uint16_t> evil{0, 0};
    for (int i = 0; i < 2000; ++i) {
        evil.push_back(2);
        evil.push_back(0xFFFF);
        evil.push_back(0);
        evil.push_back(0);
    }
    CHECK(!expand_segmented(evil, 4, out).is_ok());
    // Through read_palette: segmented 8-bit tables
    Dataset ds = image_ds(1, 2, 1, 8, 8, false, "PALETTE COLOR");
    set_native(ds, Buf{0, 3});
    for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u}) put_us_multi(ds, t, VR::US, {4, 0, 8});
    for (Tag t : {0x00281221u, 0x00281222u, 0x00281223u}) put_us_multi(ds, t, VR::OW, {0, 1, 0, 1, 3, 255});
    auto lut = read_palette(ds, false);
    REQUIRE(lut.is_ok());
    CHECK(lut->segmented);
    CHECK(lut->red == (std::vector<std::uint16_t>{0, 85, 170, 255}));
}

TEST_CASE("image: standard palettes from PS3.6 Annex B") {
    for (auto p : {StandardPalette::HotIron, StandardPalette::Pet, StandardPalette::HotMetalBlue, StandardPalette::Pet20Step}) {
        const PaletteLut& l = standard_palette(p);
        CHECK_EQ(l.entries, 256u);
        CHECK_EQ(l.bits, 8);
        CHECK_EQ(l.red.size(), 256u);
        CHECK(!standard_palette_uid(p).empty());
    }
    const PaletteLut& hi = standard_palette(StandardPalette::HotIron);
    CHECK(hi.red[0] == 0 && hi.red[1] == 2 && hi.red[255] == 255);
    CHECK(hi.blue[255] == 255);
    CHECK_EQ(standard_palette_label(StandardPalette::Pet20Step), "PET_20_STEP");
    CHECK_EQ(standard_palette_uid(StandardPalette::HotMetalBlue), "1.2.840.10008.1.5.3");
}

// ================================================================ LUT maths

TEST_CASE("image: VOI LINEAR at the PS3.3 C.11.2.1.2 boundary values") {
    auto f = [](double x, double c, double w) { return apply_window(x, c, w, VoiFunction::Linear, 0, 255); };
    // c=2048, w=4096
    CHECK_EQ(f(0, 2048, 4096), 0.0);
    CHECK(f(1, 2048, 4096) > 0.0);
    CHECK_EQ(f(4095, 2048, 4096), 255.0);
    CHECK(std::fabs(f(2047.5, 2048, 4096) - 127.5) < 1e-9);
    CHECK(std::fabs(f(4094, 2048, 4096) - ((4094 - 2047.5) / 4095 + 0.5) * 255) < 1e-9);
    // c=2048, w=1: threshold at 2047.5
    CHECK_EQ(f(2047, 2048, 1), 0.0);
    CHECK_EQ(f(2047.5, 2048, 1), 0.0);
    CHECK_EQ(f(2047.5000001, 2048, 1), 255.0);
    CHECK_EQ(f(2048, 2048, 1), 255.0);
    // c=0, w=100
    CHECK_EQ(f(-50, 0, 100), 0.0);
    CHECK(f(-49, 0, 100) > 0.0);
    CHECK(std::fabs(f(49, 0, 100) - 255.0) < 1e-9);
    CHECK_EQ(f(49.0001, 0, 100), 255.0);
    // c=0, w=1
    CHECK_EQ(f(-0.5, 0, 1), 0.0);
    CHECK_EQ(f(0, 0, 1), 255.0);
    CHECK_EQ(f(-1, 0, 1), 0.0);
    // width 2 gives the same as width 1 for integer input
    for (int x = -3; x <= 3; ++x) CHECK_EQ(f(x, 0, 1), f(x, 0, 2));
    // identity: c = 2^(n-1), w = 2^n maps 0..2^n-1 onto the output range
    for (int x = 0; x < 256; ++x) CHECK(std::fabs(apply_window(x, 128, 256, VoiFunction::Linear, 0, 255) - x) < 1e-9);
    CHECK(!window_valid(0, 0.5, VoiFunction::Linear));
    CHECK(window_valid(0, 1, VoiFunction::Linear));
    CHECK(!window_valid(0, 0, VoiFunction::LinearExact));
    CHECK(window_valid(0, 0.25, VoiFunction::LinearExact));
    CHECK(!window_valid(0, -1, VoiFunction::Sigmoid));
    CHECK(!window_valid(std::nan(""), 10, VoiFunction::Linear));
}

TEST_CASE("image: VOI LINEAR_EXACT and SIGMOID") {
    auto e = [](double x, double c, double w) { return apply_window(x, c, w, VoiFunction::LinearExact, 0, 1); };
    CHECK_EQ(e(-50, 0, 100), 0.0);   // x <= c - w/2
    CHECK(e(-49.999, 0, 100) > 0.0);
    CHECK_EQ(e(50, 0, 100), 1.0);    // x == c + w/2 is inside: ((50)/100 + .5) = 1
    CHECK_EQ(e(50.001, 0, 100), 1.0);
    CHECK(std::fabs(e(0, 0, 100) - 0.5) < 1e-12);
    CHECK(std::fabs(e(25, 0, 100) - 0.75) < 1e-12);
    CHECK_EQ(e(1, 1.5, 1), 0.0);
    CHECK(std::fabs(e(1.25, 1.5, 1) - 0.25) < 1e-12);
    auto s = [](double x, double c, double w) { return apply_window(x, c, w, VoiFunction::Sigmoid, 0, 1); };
    CHECK(std::fabs(s(40, 40, 80) - 0.5) < 1e-12);
    CHECK(std::fabs(s(80, 40, 80) - 1 / (1 + std::exp(-2.0))) < 1e-12);
    CHECK(s(-1e9, 40, 80) < 1e-12);
    CHECK(s(1e9, 40, 80) > 1 - 1e-12);
    CHECK(std::fabs(apply_window(40, 40, 80, VoiFunction::Sigmoid, -100, 100)) < 1e-12);
}

TEST_CASE("image: modality LUT, rescale, VOI LUT sequence and functional groups") {
    Dataset ds = image_ds(1, 4, 1, 16, 12, false, "MONOCHROME2", 2);
    set_native(ds, Buf(16));
    PixelInfo pi = read_pixel_info(ds).value();
    put_str(ds, siaara::tags::RescaleSlope, VR::DS, "2");
    put_str(ds, siaara::tags::RescaleIntercept, VR::DS, "-1024");
    auto m = modality_for_frame(ds, pi, 0).value();
    CHECK(!m.has_lut);
    CHECK_EQ(m.apply(1000), 976.0);
    // Modality LUT Sequence wins over rescale
    {
        Element seq;
        seq.tag = image::tags::ModalityLutSequence;
        seq.vr = VR::SQ;
        Dataset item;
        put_us_multi(item, image::tags::LutDescriptor, VR::US, {3, 10, 16});
        put_us_multi(item, image::tags::LutData, VR::OW, {100, 200, 300});
        seq.items.push_back(item);
        Dataset d = ds;
        d.put(seq);
        auto ml = modality_for_frame(d, pi, 0).value();
        CHECK(ml.has_lut);
        CHECK_EQ(ml.apply(0), 100.0);
        CHECK_EQ(ml.apply(11), 200.0);
        CHECK_EQ(ml.apply(4000), 300.0);
        CHECK_EQ(ml.lut.max_output(), 65535.0);
    }
    // Windows: pairs, explanation, function, invalid ones dropped
    put_str(ds, image::tags::WindowCenter, VR::DS, "40\\300\\7");
    put_str(ds, image::tags::WindowWidth, VR::DS, "400\\1500\\0");
    put_str(ds, image::tags::WindowCenterWidthExplanation, VR::LO, "SOFT\\BONE\\BAD");
    auto v = voi_for_frame(ds, pi, m, 0).value();
    REQUIRE(v.windows.size() == 2);
    CHECK_EQ(v.windows[1].center, 300.0);
    CHECK_EQ(v.windows[1].explanation, "BONE");
    CHECK(!voi_for_frame(ds, pi, m, 0, false).is_ok());
    put_str(ds, image::tags::VoiLutFunction, VR::CS, "SIGMOID");
    CHECK(voi_for_frame(ds, pi, m, 0).value().windows[0].function == VoiFunction::Sigmoid);
    // VOI LUT Sequence: first value mapped is SS when the rescale output may be signed (C.11.2.1.1)
    {
        Element seq;
        seq.tag = image::tags::VoiLutSequence;
        seq.vr = VR::SQ;
        Dataset item;
        put_us_multi(item, image::tags::LutDescriptor, VR::None, {2, 0xFC00, 8});  // -1024 when read signed
        put_raw(item, image::tags::LutData, VR::OW, Buf{5, 250});                 // 8-bit entries, one byte each
        seq.items.push_back(item);
        ds.put(seq);
        auto vv = voi_for_frame(ds, pi, m, 0).value();
        REQUIRE(vv.luts.size() == 1);
        CHECK_EQ(vv.luts[0].first_mapped, -1024);
        CHECK_EQ(apply_voi_lut(vv.luts[0], -1023), 250.0);
        CHECK_EQ(apply_voi_lut(vv.luts[0], -5000), 5.0);
        ModalityTransform id;
        CHECK_EQ(voi_for_frame(ds, pi, id, 0).value().luts[0].first_mapped, 0xFC00);  // unsigned PR, no rescale
    }
    // Enhanced: shared and per-frame functional groups override the top level
    {
        Dataset d = ds;
        auto group = [](Tag macro, Dataset content) {
            Element e;
            e.tag = macro;
            e.vr = VR::SQ;
            e.items.push_back(std::move(content));
            Dataset g;
            g.put(std::move(e));
            return g;
        };
        Dataset pvt0, pvt1, voi1;
        put_str(pvt0, siaara::tags::RescaleSlope, VR::DS, "1");
        put_str(pvt0, siaara::tags::RescaleIntercept, VR::DS, "5");
        put_str(pvt1, siaara::tags::RescaleSlope, VR::DS, "3");
        put_str(pvt1, siaara::tags::RescaleIntercept, VR::DS, "0");
        put_str(voi1, image::tags::WindowCenter, VR::DS, "11");
        put_str(voi1, image::tags::WindowWidth, VR::DS, "22");
        Element shared;
        shared.tag = image::tags::SharedFunctionalGroupsSequence;
        shared.vr = VR::SQ;
        shared.items.push_back(group(image::tags::PixelValueTransformationSequence, pvt0));
        d.put(shared);
        Element per;
        per.tag = image::tags::PerFrameFunctionalGroupsSequence;
        per.vr = VR::SQ;
        per.items.push_back(Dataset{});
        Dataset f1 = group(image::tags::PixelValueTransformationSequence, pvt1);
        Element fv;
        fv.tag = image::tags::FrameVoiLutSequence;
        fv.vr = VR::SQ;
        fv.items.push_back(voi1);
        f1.put(fv);
        per.items.push_back(f1);
        d.put(per);
        auto m0 = modality_for_frame(d, pi, 0).value();
        auto m1 = modality_for_frame(d, pi, 1).value();
        CHECK(m0.from_functional_groups && m0.apply(10) == 15.0);
        CHECK(m1.apply(10) == 30.0);
        auto v0 = voi_for_frame(d, pi, m0, 0).value();
        auto v1 = voi_for_frame(d, pi, m1, 1).value();
        CHECK(!v0.from_functional_groups && v0.windows[0].center == 40.0);
        CHECK(v1.from_functional_groups && v1.windows.size() == 1 && v1.windows[0].width == 22.0);
    }
}

TEST_CASE("image: presentation shape and MONOCHROME1 inversion") {
    Dataset ds = image_ds(1, 3, 1, 8, 8, false, "MONOCHROME1");
    set_native(ds, Buf{0, 128, 255});
    PixelInfo pi = read_pixel_info(ds).value();
    CHECK(output_inverted(pi, ds));
    put_str(ds, image::tags::PresentationLutShape, VR::CS, "INVERSE");
    CHECK(output_inverted(pi, ds));  // not inverted twice
    auto img = Image::open(ds, kExplicitLE);
    REQUIRE(img.is_ok());
    RenderParams p;
    p.voi = VoiMode::Identity;
    auto a = render_2d(*img, p).value();
    CHECK(a[0] == 255 && a[4] == 127 && a[8] == 0);
    p.invert = true;  // the user's invert undoes it
    a = render_2d(*img, p).value();
    CHECK(a[0] == 0 && a[8] == 255);
    Dataset m2 = image_ds(1, 3, 1, 8, 8, false, "MONOCHROME2");
    set_native(m2, Buf{0, 128, 255});
    PixelInfo p2 = read_pixel_info(m2).value();
    CHECK(!output_inverted(p2, m2));
    put_str(m2, image::tags::PresentationLutShape, VR::CS, "IDENTITY");
    CHECK(!output_inverted(p2, m2));
    put_str(m2, image::tags::PresentationLutShape, VR::CS, "INVERSE");
    CHECK(output_inverted(p2, m2));
}

// ================================================================ rendering

TEST_CASE("image: CT render with rescale and window") {
    Dataset ds = image_ds(2, 3, 1, 16, 16, true, "MONOCHROME2");
    Buf px;
    for (int v : {-2000, -1024, -1000, -960, -625, 3000}) {
        px.push_back(static_cast<std::uint8_t>(v & 0xFF));
        px.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    set_native(ds, px);
    put_str(ds, siaara::tags::RescaleSlope, VR::DS, "1");
    put_str(ds, siaara::tags::RescaleIntercept, VR::DS, "1024");  // HU = stored + 1024
    put_str(ds, image::tags::WindowCenter, VR::DS, "40");
    put_str(ds, image::tags::WindowWidth, VR::DS, "400");
    auto img = Image::open(ds, kExplicitLE);
    REQUIRE(img.is_ok());
    auto a = render_2d(*img, RenderParams{}).value();
    auto expect = [](double hu) {
        return static_cast<int>(std::floor(apply_window(hu, 40, 400, VoiFunction::Linear, 0, 1) * 255 + 0.5));
    };
    const double hus[] = {-976, 0, 24, 64, 399, 4024};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK_EQ(a[4 * i], expect(hus[i]));
        CHECK_EQ(a[4 * i], a[4 * i + 1]);
        CHECK_EQ(a[4 * i + 3], 255);
    }
    RenderParams p;
    p.voi = VoiMode::Explicit;
    p.window_center = 0;
    p.window_width = 1;
    a = render_2d(*img, p).value();
    CHECK(a[0] == 0 && a[4] == 255);  // threshold at -0.5
    p.window_width = 0.5;
    CHECK(render_2d(*img, p).error().code == Errc::InvalidArgument);
    p.voi = VoiMode::File;
    p.voi_index = 3;
    CHECK(render_2d(*img, p).error().code == Errc::NotFound);
    p.voi = VoiMode::FullRange;
    a = render_2d(*img, p).value();
    CHECK(a[0] == 0 && a[20] == 255);
    p.pseudo_color = StandardPalette::HotIron;
    a = render_2d(*img, p).value();
    CHECK(a[20] == 255 && a[21] == 255 && a[22] == 255);
    CHECK(a[0] == 0 && a[1] == 0 && a[2] == 0);
    RenderParams bad_frame;
    bad_frame.frame = 1;
    CHECK(render_2d(*img, bad_frame).error().code == Errc::InvalidArgument);
}

TEST_CASE("image: render colour, 32-bit, float, presentation LUT, supplemental palette") {
    {   // planar RGB 16-bit
        Dataset ds = image_ds(1, 2, 3, 16, 16, false, "RGB", 1, true);
        set_native(ds, Buf{0xFF, 0xFF, 0, 0, 0, 0, 0x80, 0x80, 0, 0, 0xFF, 0xFF});
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(a == (Buf{255, 0, 0, 255, 0, 128, 255, 255}));
    }
    {   // 32-bit unsigned, full range
        Dataset ds = image_ds(1, 3, 1, 32, 32, false, "MONOCHROME2");
        set_native(ds, Buf{0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0xFF, 0xFF, 0xFF});
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(a[0] == 0 && a[4] == 128 && a[8] == 255);
    }
    {   // float pixel data
        Dataset ds;
        put_us(ds, siaara::tags::Rows, 1);
        put_us(ds, siaara::tags::Columns, 3);
        put_us(ds, siaara::tags::SamplesPerPixel, 1);
        put_us(ds, siaara::tags::BitsAllocated, 32);
        put_str(ds, siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
        float f[3] = {-1.5f, 0.25f, 2.0f};
        Buf b(12);
        std::memcpy(b.data(), f, 12);
        put_raw(ds, siaara::tags::FloatPixelData, VR::OF, b);
        auto img = Image::open(ds, kExplicitLE).value();
        auto a = render_2d(img, RenderParams{}).value();
        CHECK(a[0] == 0 && a[8] == 255 && a[4] == 128);
        std::vector<float> out(3);
        REQUIRE(decode_modality_f32(img, 0, out).is_ok());
        CHECK(out[0] == -1.5f && out[2] == 2.0f);
    }
    {   // Presentation LUT Sequence: inverting ramp
        Dataset ds = image_ds(1, 2, 1, 8, 8, false, "MONOCHROME2");
        set_native(ds, Buf{0, 255});
        Element seq;
        seq.tag = image::tags::PresentationLutSequence;
        seq.vr = VR::SQ;
        Dataset item;
        put_us_multi(item, image::tags::LutDescriptor, VR::US, {2, 0, 12});
        put_us_multi(item, image::tags::LutData, VR::OW, {4095, 0});
        seq.items.push_back(item);
        ds.put(seq);
        RenderParams p;
        p.voi = VoiMode::Identity;
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), p).value();
        CHECK(a[0] == 255 && a[4] == 0);
        p.apply_presentation_lut = false;
        a = render_2d(Image::open(ds, kExplicitLE).value(), p).value();
        CHECK(a[0] == 0 && a[4] == 255);
    }
    {   // Supplemental palette: values >= first mapped are coloured
        Dataset ds = image_ds(1, 3, 1, 16, 12, false, "MONOCHROME2");
        set_native(ds, Buf{0, 0, 0xFF, 0x07, 0x00, 0x08});
        put_str(ds, image::tags::PixelPresentation, VR::CS, "MIXED");
        for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u}) put_us_multi(ds, t, VR::US, {2, 2048, 16});
        put_us_multi(ds, 0x00281201, VR::OW, {0xFFFF, 0xFFFF});
        put_us_multi(ds, 0x00281202, VR::OW, {0, 0});
        put_us_multi(ds, 0x00281203, VR::OW, {0, 0});
        put_str(ds, image::tags::WindowCenter, VR::DS, "1024");
        put_str(ds, image::tags::WindowWidth, VR::DS, "2048");
        auto img = Image::open(ds, kExplicitLE).value();
        CHECK(img.supplemental_palette());
        auto a = render_2d(img, RenderParams{}).value();
        CHECK(a[0] == 0 && a[1] == 0);
        CHECK(a[4] == 255 && a[5] == 255);           // 2047: gray
        CHECK(a[8] == 255 && a[9] == 0 && a[10] == 0);  // 2048: red
    }
    {   // YBR_FULL_422 native renders through colour conversion
        Dataset ds = image_ds(1, 2, 3, 8, 8, false, "YBR_FULL_422");
        set_native(ds, Buf{76, 76, 85, 255});  // ~ pure red
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(a[0] >= 253 && a[1] <= 2 && a[2] <= 2 && a[4] >= 253);
    }
}

TEST_CASE("image: overlays (separate, origin offset, multi-frame, embedded)") {
    Dataset ds = image_ds(4, 4, 1, 8, 8, false, "MONOCHROME2", 3);
    set_native(ds, Buf(48, 0));
    // group 6000: 2x2, origin 2\3, single frame, bits 1 0 / 0 1
    put_us(ds, 0x60000010, 2);
    put_us(ds, 0x60000011, 2);
    put_us_multi(ds, 0x60000050, VR::SS, {2, 3});
    put_str(ds, 0x60000040, VR::CS, "G");
    put_us(ds, 0x60000100, 1);
    put_us(ds, 0x60000102, 0);
    put_raw(ds, 0x60003000, VR::OW, Buf{0x09, 0});
    // group 6002: 1x2 multi-frame overlay, 2 frames starting at image frame 2, origin 0\0 (partly outside)
    put_us(ds, 0x60020010, 2);
    put_us(ds, 0x60020011, 2);
    put_us_multi(ds, 0x60020050, VR::SS, {0, 0});
    put_us(ds, 0x60020100, 1);
    put_str(ds, 0x60020015, VR::IS, "2");
    put_us(ds, 0x60020051, 2);
    put_raw(ds, 0x60023000, VR::OW, Buf{0x80, 0x00});  // frame 0 bits 0..3 = 0; frame 1: bit 7 -> (1,1)
    auto img = Image::open(ds, kExplicitLE);
    REQUIRE(img.is_ok());
    REQUIRE(img->overlays().size() == 2);
    CHECK(!img->overlays()[1].overlay_frame(0).has_value());
    CHECK_EQ(img->overlays()[1].overlay_frame(2).value(), 1u);
    RenderParams p;
    p.voi = VoiMode::Identity;
    p.overlay_rgba = {255, 0, 0, 255};
    auto a = render_2d(*img, p).value();
    auto red = [&](std::uint32_t r, std::uint32_t c) { return px_at(a, 4, r, c)[0] == 255 && px_at(a, 4, r, c)[1] == 0; };
    CHECK(red(1, 2));
    CHECK(!red(1, 3));
    CHECK(!red(2, 2));
    CHECK(red(2, 3));
    CHECK_EQ(std::count(a.begin(), a.end(), std::uint8_t{255}), 2 + 16);  // 2 red pixels + 16 alpha bytes
    p.frame = 2;
    a = render_2d(*img, p).value();
    CHECK(red(0, 0));  // overlay frame 1, pixel (1,1) of the overlay lands at image (0,0)
    p.overlay_groups = 0x1;
    a = render_2d(*img, p).value();
    CHECK(!red(0, 0));
    p.overlays = false;
    a = render_2d(*img, p).value();
    CHECK(!red(1, 2));
    // Embedded overlay in bit 15 of 12-bit data
    Dataset e = image_ds(1, 2, 1, 16, 12, false, "MONOCHROME2");
    set_native(e, Buf{0x00, 0x80, 0x00, 0x00});
    put_us(e, 0x60000010, 1);
    put_us(e, 0x60000011, 2);
    put_us_multi(e, 0x60000050, VR::SS, {1, 1});
    put_us(e, 0x60000100, 16);
    put_us(e, 0x60000102, 15);
    auto ei = Image::open(e, kExplicitLE).value();
    REQUIRE(ei.overlays().size() == 1);
    CHECK(ei.overlays()[0].embedded);
    RenderParams q;
    q.voi = VoiMode::Identity;
    q.overlay_rgba = {0, 255, 0, 255};
    auto b = render_2d(ei, q).value();
    CHECK(b[0] == 0 && b[1] == 255 && b[5] == 0);
    // Embedded bit inside the stored bits: refused (lenient skips it)
    put_us(e, 0x60000102, 3);
    CHECK(Image::open(e, kExplicitLE).value().overlays().empty());
    FrameOptions strict;
    strict.lenient = false;
    CHECK(!Image::open(e, kExplicitLE, strict).is_ok());
    // Overlay data too short
    put_raw(ds, 0x60003000, VR::OW, Buf{});
    CHECK(Image::open(ds, kExplicitLE).value().overlays().size() == 1);
}

TEST_CASE("image: display shutters (rectangular, circular, polygonal, bitmap)") {
    Dataset base = image_ds(9, 9, 1, 8, 8, false, "MONOCHROME2");
    set_native(base, Buf(81, 200));
    put_str(base, image::tags::WindowCenter, VR::DS, "128");
    put_str(base, image::tags::WindowWidth, VR::DS, "256");
    auto visible = [](const Buf& a, std::uint32_t r, std::uint32_t c) { return px_at(a, 9, r, c)[0] == 200; };
    {
        Dataset ds = base;
        put_str(ds, 0x00181600, VR::CS, "RECTANGULAR");
        put_str(ds, 0x00181602, VR::IS, "2");
        put_str(ds, 0x00181604, VR::IS, "4");
        put_str(ds, 0x00181606, VR::IS, "3");
        put_str(ds, 0x00181608, VR::IS, "5");
        put_us(ds, 0x00181622, 0xFFFF);
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(visible(a, 2, 1) && visible(a, 4, 3) && visible(a, 2, 3));
        CHECK(!visible(a, 1, 1) && !visible(a, 2, 0) && !visible(a, 5, 3) && !visible(a, 2, 4));
        CHECK_EQ(px_at(a, 9, 0, 0)[0], 255);  // presentation value white
        RenderParams p;
        p.shutters = false;
        a = render_2d(Image::open(ds, kExplicitLE).value(), p).value();
        CHECK(visible(a, 0, 0));
    }
    {
        Dataset ds = base;
        put_str(ds, 0x00181600, VR::CS, "CIRCULAR\\RECTANGULAR");
        put_str(ds, 0x00181610, VR::IS, "5\\5");
        put_str(ds, 0x00181612, VR::IS, "3");
        put_str(ds, 0x00181602, VR::IS, "1");
        put_str(ds, 0x00181604, VR::IS, "5");
        put_str(ds, 0x00181606, VR::IS, "1");
        put_str(ds, 0x00181608, VR::IS, "9");
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(visible(a, 4, 4) && visible(a, 1, 4) && visible(a, 4, 1));
        CHECK(!visible(a, 0, 4) && !visible(a, 1, 1) && !visible(a, 4, 5));  // outside circle / rectangle
        CHECK_EQ(px_at(a, 9, 0, 0)[0], 0);
    }
    {
        Dataset ds = base;
        put_str(ds, 0x00181600, VR::CS, "POLYGONAL");
        put_str(ds, 0x00181620, VR::IS, "1\\1\\1\\9\\9\\1");  // triangle: upper-left half
        put_us_multi(ds, 0x00181624, VR::US, {0xFFFF, 0x8080, 0x8080});  // CIELab white
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(visible(a, 0, 0) && visible(a, 0, 8) && visible(a, 8, 0) && visible(a, 4, 4) && visible(a, 2, 3));
        CHECK(!visible(a, 8, 8) && !visible(a, 5, 5));
        CHECK(px_at(a, 9, 8, 8)[0] >= 250 && px_at(a, 9, 8, 8)[2] >= 250);
    }
    {
        Dataset ds = base;
        put_str(ds, 0x00181600, VR::CS, "BITMAP");
        put_us(ds, 0x00181623, 0x6004);
        put_us(ds, 0x60040010, 9);
        put_us(ds, 0x60040011, 9);
        put_us_multi(ds, 0x60040050, VR::SS, {1, 1});
        put_us(ds, 0x60040100, 1);
        Buf bits(11, 0);
        bits[0] = 0x01;  // pixel (0,0)
        put_raw(ds, 0x60043000, VR::OW, bits);
        RenderParams p;
        p.overlay_rgba = {255, 0, 0, 255};
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), p).value();
        CHECK(!visible(a, 0, 0) && visible(a, 0, 1));
        CHECK(px_at(a, 9, 0, 0)[0] == 0);  // shuttered, and the shutter overlay is not drawn
    }
    {
        Dataset ds = base;  // malformed shutters: strict fails, lenient ignores
        put_str(ds, 0x00181600, VR::CS, "POLYGONAL\\HEXAGON");
        put_str(ds, 0x00181620, VR::IS, "1\\1\\2\\2");
        FrameOptions strict;
        strict.lenient = false;
        CHECK(!Image::open(ds, kExplicitLE, strict).is_ok());
        auto a = render_2d(Image::open(ds, kExplicitLE).value(), RenderParams{}).value();
        CHECK(visible(a, 8, 8));
    }
}

TEST_CASE("image: icon image sequence and part 10 round trip") {
    DicomFile f;
    f.transfer_syntax = std::string(kRle);
    f.dataset = image_ds(4, 4, 1, 8, 8, false, "MONOCHROME2");
    (void)f.dataset.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)f.dataset.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25.8");
    Buf raw = random_frame(16, 8, 1, 3), z;
    REQUIRE(codec::encode_rle(raw, finfo([&] { Dataset d = f.dataset; set_native(d, raw); return d; }()), {}, z).is_ok());
    set_encapsulated(f.dataset, {}, {z});
    Dataset icon = image_ds(2, 2, 1, 8, 8, false, "MONOCHROME2");
    set_native(icon, Buf{1, 2, 3, 4});
    Element seq;
    seq.tag = image::tags::IconImageSequence;
    seq.vr = VR::SQ;
    seq.items.push_back(icon);
    f.dataset.put(seq);
    Buf bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    auto g = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(g.is_ok());
    auto r = FrameReader::open(g->dataset, g->transfer_syntax);
    REQUIRE(r.is_ok());
    CHECK(r->decode(0).value() == raw);
    const Dataset* ic = icon_image(g->dataset);
    REQUIRE(ic != nullptr);
    auto ri = FrameReader::open(*ic, g->transfer_syntax);  // native inside an encapsulated syntax
    REQUIRE(ri.is_ok());
    CHECK(!ri->encapsulated());
    CHECK(ri->decode(0).value() == (Buf{1, 2, 3, 4}));
}

TEST_CASE("image: malformed inputs never crash") {
    std::mt19937_64 rng(1234);
    const char* photos[] = {"MONOCHROME1", "MONOCHROME2", "RGB", "YBR_FULL", "YBR_FULL_422", "PALETTE COLOR",
                            "YBR_PARTIAL_420", "YBR_RCT", "HSV", "FOO", ""};
    const std::uint16_t bas[] = {1, 8, 16, 32, 64, 0, 12, 7};
    const std::string_view tss[] = {kExplicitLE, kRle, kJpegLossless, kJpegLs, kJ2k, kEncUncompressed, "1.2.840.10008.1.2.2"};
    int opened = 0, rendered = 0;
    for (int iter = 0; iter < 600; ++iter) {
        // Mostly-valid attributes with occasional bad ones, so the deeper paths are reached.
        auto bad = [&] { return rng() % 8 == 0; };
        Dataset ds;
        const auto rows = static_cast<std::uint16_t>(bad() ? rng() % 3 : 1 + rng() % 6);
        const auto cols = static_cast<std::uint16_t>(bad() ? rng() % 3 : 1 + rng() % 6);
        const char* photo = photos[rng() % 11];
        const bool color = std::strcmp(photo, "RGB") == 0 || std::strncmp(photo, "YBR", 3) == 0 || std::strcmp(photo, "HSV") == 0;
        const auto spp = static_cast<std::uint16_t>(bad() ? rng() % 5 : (color ? 3 : 1));
        const std::uint16_t ba = bad() ? bas[rng() % 8] : bas[rng() % 4];
        const auto bs = static_cast<std::uint16_t>(bad() ? rng() % 70 : (ba <= 1 ? 1 : 1 + rng() % ba));
        const int nframes = bad() ? static_cast<int>(rng() % 5) - 1 : 1 + static_cast<int>(rng() % 3);
        put_us(ds, siaara::tags::Rows, rows);
        put_us(ds, siaara::tags::Columns, cols);
        put_us(ds, siaara::tags::SamplesPerPixel, spp);
        put_us(ds, siaara::tags::BitsAllocated, ba);
        put_us(ds, siaara::tags::BitsStored, bs);
        put_us(ds, siaara::tags::HighBit, static_cast<std::uint16_t>(bad() ? rng() % 70 : bs - 1u));
        put_us(ds, siaara::tags::PixelRepresentation, static_cast<std::uint16_t>(bad() ? 2 : rng() % 2));
        put_us(ds, siaara::tags::PlanarConfiguration, static_cast<std::uint16_t>(bad() ? 2 : rng() % 2));
        put_str(ds, siaara::tags::PhotometricInterpretation, VR::CS, photo);
        put_str(ds, siaara::tags::NumberOfFrames, VR::IS, std::to_string(nframes));
        const std::string_view ts = tss[rng() % 7];
        std::size_t expect = std::size_t{rows} * cols * spp * std::max<std::size_t>(ba / 8u, 1) * static_cast<std::size_t>(std::max(nframes, 1));
        if (bad()) expect = static_cast<std::size_t>(rng() % (expect + 40));
        Buf data(expect);
        for (auto& v : data) v = static_cast<std::uint8_t>(rng());
        if (ts != kExplicitLE && ts != "1.2.840.10008.1.2.2") {
            std::vector<Buf> frags;
            const int nf = static_cast<int>(rng() % 5);
            for (int k = 0; k < nf; ++k) {
                Buf fr(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(data.size() / 2));
                if (fr.size() >= 2 && rng() % 2) { fr[0] = 0xFF; fr[1] = 0xD8; }
                frags.push_back(fr);
            }
            set_encapsulated(ds, rng() % 2 ? Buf{} : le32s({0, static_cast<std::uint32_t>(rng() % 300)}), frags);
        } else {
            set_native(ds, data);
        }
        if (rng() % 3 == 0) {
            for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u})
                put_us_multi(ds, t, VR::US, {static_cast<int>(rng() % 5), static_cast<int>(rng() % 70000), static_cast<int>(rng() % 20)});
            for (Tag t : {0x00281201u, 0x00281202u, 0x00281203u}) put_raw(ds, t, VR::OW, Buf(static_cast<std::size_t>(rng() % 12)));
        }
        if (rng() % 3 == 0) {
            put_us(ds, 0x60000010, static_cast<std::uint16_t>(rng() % 9));
            put_us(ds, 0x60000011, static_cast<std::uint16_t>(rng() % 9));
            put_us_multi(ds, 0x60000050, VR::SS, {static_cast<int>(rng() % 20) - 10, static_cast<int>(rng() % 20) - 10});
            put_us(ds, 0x60000100, 1);
            put_raw(ds, 0x60003000, VR::OW, Buf(static_cast<std::size_t>(rng() % 10), 0xFF));
            put_str(ds, 0x60000015, VR::IS, std::to_string(rng() % 4));
        }
        if (rng() % 3 == 0) {
            put_str(ds, 0x00181600, VR::CS, "RECTANGULAR\\CIRCULAR\\POLYGONAL");
            put_str(ds, 0x00181620, VR::IS, "1\\1\\-50\\900\\3\\-7\\1e9\\2");
            put_str(ds, 0x00181610, VR::IS, "2\\2");
            put_str(ds, 0x00181612, VR::IS, "1");
        }
        if (rng() % 3 == 0) {
            put_str(ds, image::tags::WindowCenter, VR::DS, "40\\x");
            put_str(ds, image::tags::WindowWidth, VR::DS, "-3\\0\\1e308");
            put_str(ds, siaara::tags::RescaleSlope, VR::DS, rng() % 2 ? "1e308" : "nan");
        }
        auto img = Image::open(ds, ts);
        if (!img) continue;
        ++opened;
        for (std::uint32_t f = 0; f < 2; ++f) {
            RenderParams p;
            p.frame = f;
            p.voi = static_cast<VoiMode>(rng() % 4);
            p.window_width = 1;
            auto a = render_2d(*img, p);
            if (a) ++rendered;
            (void)img->frames().decode_range(0, img->frames().frame_count());
            std::vector<float> fl(sz(img->info().pixels() * img->info().samples_per_pixel));
            (void)decode_modality_f32(*img, f, fl);
        }
    }
    CHECK(opened > 0);
    CHECK(rendered > 0);
    std::printf("  fuzz: %d opened, %d rendered of 600\n", opened, rendered);
}
