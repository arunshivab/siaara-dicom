// SIAARA DICOM Toolkit - seed corpus generator for the fuzz targets (test tooling only).
//
//     siaara_make_seeds <output-folder>
//
// Writes small, valid inputs for every target into <output-folder>/<target>/, made with the
// toolkit's own writers and encoders, in the input layout each harness in fuzz/targets/ expects.
// fuzz/make_seeds.py runs this and adds the pydicom test files on top (not committed).
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "clinical/common.hpp"
#include "clinical/gsps.hpp"
#include "clinical/sr.hpp"
#include "codec/codec.hpp"
#include "codec/deflate.hpp"
#include "codec/j2k.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpegls.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/dimse.hpp"
#include "net/pdu.hpp"
#include "store/common.hpp"
#include "store/dicomdir.hpp"
#include "store/store.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"
#include "web/multipart.hpp"

using namespace siaara;
namespace fs = std::filesystem;
namespace ct = siaara::clinical::tags;

namespace {

fs::path g_out;
int g_count = 0;

void emit(const char* target, const std::string& name, const void* data, std::size_t size) {
    const fs::path dir = g_out / target;
    fs::create_directories(dir);
    std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ++g_count;
}
void emit(const char* target, const std::string& name, const std::vector<std::uint8_t>& v) {
    emit(target, name, v.data(), v.size());
}
void emit(const char* target, const std::string& name, std::string_view s) { emit(target, name, s.data(), s.size()); }

void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }

Element sequence(Tag t, std::vector<Dataset> items) {
    Element e;
    e.tag = t;
    e.vr = VR::SQ;
    e.items = std::move(items);
    return e;
}

// Patient/study/series identification common to all seeds.
Dataset base(std::string_view sop_class, std::string_view sop_instance, std::string_view modality) {
    Dataset d;
    put(d, tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
    put(d, tags::SOPClassUID, VR::UI, sop_class);
    put(d, tags::SOPInstanceUID, VR::UI, sop_instance);
    put(d, 0x00080020, VR::DA, "20240131");
    put(d, 0x00080030, VR::TM, "101530.25");
    put(d, 0x00080050, VR::SH, "ACC123");
    put(d, tags::Modality, VR::CS, modality);
    put(d, 0x00080090, VR::PN, "Referring^Doctor");
    put(d, 0x00081030, VR::LO, "Study description");
    put(d, tags::PatientName, VR::PN, "Doe^Jane^^Dr");
    put(d, tags::PatientID, VR::LO, "PID001");
    put(d, 0x00100030, VR::DA, "19700101");
    put(d, 0x00100040, VR::CS, "F");
    put(d, tags::StudyInstanceUID, VR::UI, "1.2.826.0.1.3680043.8.498.1");
    put(d, tags::SeriesInstanceUID, VR::UI, "1.2.826.0.1.3680043.8.498.2");
    put(d, 0x00200011, VR::IS, "3");
    put(d, 0x00200013, VR::IS, "7");
    return d;
}

std::vector<std::uint8_t> part10(const Dataset& ds, std::string_view tsuid, const WriteOptions& wo = {}) {
    DicomFile f;
    f.dataset = ds;
    f.transfer_syntax = std::string(tsuid);
    std::vector<std::uint8_t> out;
    if (!write_part10(f, out, wo)) {
        std::fprintf(stderr, "write_part10 failed\n");
        std::exit(1);
    }
    return out;
}

// A gradient test pattern of rows x cols x spp samples of `bits` (8/16/32) bits.
std::vector<std::uint8_t> pattern(std::uint32_t rows, std::uint32_t cols, int spp, int bits, std::uint32_t frames = 1,
                                  int stored = 0) {
    const int bytes = bits / 8;
    if (stored == 0) stored = bits;
    const std::uint64_t maxv = (stored >= 32) ? 0xFFFFFFFFull : ((1ull << stored) - 1);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(rows) * cols * static_cast<std::size_t>(spp) * static_cast<std::size_t>(bytes) * frames);
    std::size_t k = 0;
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t y = 0; y < rows; ++y)
            for (std::uint32_t x = 0; x < cols; ++x)
                for (int s = 0; s < spp; ++s) {
                    const std::uint64_t val = ((x * 37u + y * 11u + static_cast<std::uint32_t>(s) * 71u + f * 5u) * 2654435761u >> 7) % (maxv + 1);
                    for (int b = 0; b < bytes; ++b) v[k++] = static_cast<std::uint8_t>(val >> (8 * b));
                }
    return v;
}

Dataset image(std::uint32_t rows, std::uint32_t cols, int spp, int bits, std::string_view photometric,
              std::uint32_t frames = 1, int stored = 0, bool is_signed = false) {
    Dataset d = base("1.2.840.10008.5.1.4.1.1.7", "1.2.826.0.1.3680043.8.498.10", "OT");
    (void)d.set_uint16(tags::SamplesPerPixel, static_cast<std::uint16_t>(spp));
    put(d, tags::PhotometricInterpretation, VR::CS, photometric);
    if (spp > 1) (void)d.set_uint16(tags::PlanarConfiguration, 0);
    if (frames > 1) put(d, tags::NumberOfFrames, VR::IS, std::to_string(frames));
    (void)d.set_uint16(tags::Rows, static_cast<std::uint16_t>(rows));
    (void)d.set_uint16(tags::Columns, static_cast<std::uint16_t>(cols));
    if (stored == 0) stored = bits;
    (void)d.set_uint16(tags::BitsAllocated, static_cast<std::uint16_t>(bits));
    (void)d.set_uint16(tags::BitsStored, static_cast<std::uint16_t>(stored));
    (void)d.set_uint16(tags::HighBit, static_cast<std::uint16_t>(stored - 1));
    (void)d.set_uint16(tags::PixelRepresentation, is_signed ? 1 : 0);
    return d;
}

void native_pixels(Dataset& d, const std::vector<std::uint8_t>& px, int bits) {
    (void)d.set_bytes(tags::PixelData, bits > 8 ? VR::OW : VR::OB, Bytes::copy_of(px.data(), px.size()));
}

void encapsulated_pixels(Dataset& d, const std::vector<std::vector<std::uint8_t>>& frames) {
    Element e;
    e.tag = tags::PixelData;
    e.vr = VR::OB;
    e.undefined_length = true;
    e.fragments.push_back(Bytes());  // empty Basic Offset Table
    for (const auto& f : frames) {
        std::vector<std::uint8_t> padded = f;
        if (padded.size() & 1u) padded.push_back(0);
        e.fragments.push_back(Bytes::copy_of(padded.data(), padded.size()));
    }
    d.put(std::move(e));
}

codec::FrameInfo info(std::uint32_t rows, std::uint32_t cols, int spp, int bits, int stored = 0) {
    codec::FrameInfo fi;
    fi.rows = rows;
    fi.columns = cols;
    fi.samples_per_pixel = static_cast<std::uint16_t>(spp);
    fi.bits_allocated = static_cast<std::uint16_t>(bits);
    fi.bits_stored = static_cast<std::uint16_t>(stored ? stored : bits);
    return fi;
}

void check(const Status& s, const char* what) {
    if (!s) {
        std::fprintf(stderr, "%s: %s\n", what, s.message.c_str());
        std::exit(1);
    }
}

// ---------------------------------------------------------------- per target

void seeds_part10() {
    Dataset d = image(4, 5, 1, 16, "MONOCHROME2");
    native_pixels(d, pattern(4, 5, 1, 16), 16);
    Dataset item;
    put(item, ct::CodeValue, VR::SH, "121327");
    put(item, ct::CodingSchemeDesignator, VR::SH, "DCM");
    put(item, ct::CodeMeaning, VR::LO, "Full fidelity image");
    d.put(sequence(0x00089215, {item, item}));  // Derivation Code Sequence
    put(d, 0x00090010, VR::LO, "SIAARA PRIVATE");
    put(d, 0x00091001, VR::LO, "private value");
    (void)d.set_bytes(0x00091002, VR::UN, Bytes::from_string("raw\x01\x02\x03"));
    put(d, 0x00180050, VR::DS, "1.25");
    put(d, 0x00280030, VR::DS, "0.5\\0.5");
    (void)d.set_bytes(0x00189219, VR::FD, Bytes::from_string(std::string("\0\0\0\0\0\0\xF0\x3F", 8)));
    const std::string_view syntaxes[] = {ts::ImplicitVRLittleEndian, ts::ExplicitVRLittleEndian,
                                         ts::ExplicitVRBigEndian, ts::DeflatedExplicitVRLittleEndian};
    int i = 0;
    for (std::string_view tsuid : syntaxes) {
        emit("part10", "ts" + std::to_string(i) + ".dcm", part10(d, tsuid));
        WriteOptions wo;
        wo.sequence_lengths = SequenceLengths::Undefined;
        emit("part10", "ts" + std::to_string(i) + "_undef.dcm", part10(d, tsuid, wo));
        ++i;
    }
    // Encapsulated RLE pixel data, two frames.
    Dataset e = image(3, 4, 1, 8, "MONOCHROME2", 2);
    std::vector<std::vector<std::uint8_t>> frames;
    for (int f = 0; f < 2; ++f) {
        std::vector<std::uint8_t> enc;
        check(codec::encode_rle(pattern(3, 4, 1, 8, 1), info(3, 4, 1, 8), {}, enc), "rle");
        frames.push_back(enc);
    }
    encapsulated_pixels(e, frames);
    emit("part10", "rle.dcm", part10(e, "1.2.840.10008.1.2.5"));
    // Raw data set bodies for the second half of the harness (first byte selects the syntax).
    std::vector<std::uint8_t> body;
    check(write_dataset(d, ts::ImplicitVRLittleEndian, body), "write_dataset");
    body.insert(body.begin(), 0);
    emit("part10", "raw_implicit.bin", body);
    body.clear();
    check(write_dataset(d, ts::ExplicitVRBigEndian, body), "write_dataset");
    body.insert(body.begin(), 2);
    emit("part10", "raw_bigendian.bin", body);
}

void seeds_rle() {
    struct C {
        std::uint32_t r, c;
        int spp, bits;
    };
    const C cases[] = {{8, 8, 1, 8}, {5, 7, 3, 8}, {4, 6, 1, 16}, {3, 3, 3, 16}, {2, 5, 1, 32}};
    int i = 0;
    for (const C& k : cases) {
        std::vector<std::uint8_t> enc;
        check(codec::encode_rle(pattern(k.r, k.c, k.spp, k.bits), info(k.r, k.c, k.spp, k.bits), {}, enc), "rle");
        const std::uint8_t bitsel = k.bits == 8 ? 0 : (k.bits == 16 ? 1 : 2);
        std::vector<std::uint8_t> in = {static_cast<std::uint8_t>(k.r - 1), static_cast<std::uint8_t>(k.c - 1),
                                        static_cast<std::uint8_t>((k.spp == 3 ? 1 : 0) | (bitsel << 1)), 0};
        in.insert(in.end(), enc.begin(), enc.end());
        emit("codec_rle", "rle" + std::to_string(i++) + ".bin", in);
    }
}

void seeds_jpeg() {
    int i = 0;
    auto one = [&](std::uint32_t r, std::uint32_t c, int spp, int bits, int stored, bool lossless,
                   codec::JpegEncodeOptions o) {
        codec::EncodeParams p;
        p.lossless = lossless;
        p.quality = 75;
        std::vector<std::uint8_t> enc;
        check(codec::encode_jpeg_ex(pattern(r, c, spp, bits, 1, stored), info(r, c, spp, bits, stored), p, o, enc), "jpeg");
        emit("codec_jpeg", "jpeg" + std::to_string(i++) + ".jpg", enc);
    };
    codec::JpegEncodeOptions o;
    one(16, 16, 1, 8, 8, false, o);
    o.luma_h = 2;
    o.luma_v = 2;
    one(16, 24, 3, 8, 8, false, o);
    o = {};
    o.optimize_huffman = false;
    o.restart_interval = 2;
    one(16, 16, 3, 8, 8, false, o);
    o = {};
    one(8, 8, 1, 16, 12, false, o);  // extended 12-bit
    one(8, 8, 1, 16, 16, true, o);   // lossless
    o.separate_scans = true;
    one(6, 7, 3, 8, 8, true, o);
    o = {};
    o.restart_interval = 7;
    one(4, 7, 1, 8, 8, true, o);
}

void seeds_jpegls() {
    int i = 0;
    auto one = [&](std::uint32_t r, std::uint32_t c, int spp, int bits, int stored, codec::jpegls::EncodeOptions o) {
        std::vector<std::uint8_t> enc;
        check(codec::jpegls::encode(pattern(r, c, spp, bits, 1, stored), info(r, c, spp, bits, stored), o, enc), "jpegls");
        emit("codec_jpegls", "jls" + std::to_string(i++) + ".jls", enc);
    };
    codec::jpegls::EncodeOptions o;
    one(16, 16, 1, 8, 8, o);
    one(9, 11, 1, 16, 12, o);
    o.near = 3;
    one(8, 8, 1, 8, 8, o);
    o = {};
    for (int ilv = 0; ilv <= 2; ++ilv) {
        o.interleave = ilv;
        one(6, 6, 3, 8, 8, o);
    }
    o = {};
    o.restart_interval = 2;
    o.write_preset = true;
    o.preset.maxval = 255;
    one(8, 8, 1, 8, 8, o);
}

void seeds_j2k() {
    int i = 0;
    auto one = [&](std::uint32_t r, std::uint32_t c, int spp, int bits, int stored, codec::j2k::EncodeOptions o) {
        std::vector<std::uint8_t> enc;
        check(codec::j2k::encode(pattern(r, c, spp, bits, 1, stored), info(r, c, spp, bits, stored), o, enc), "j2k");
        emit("codec_j2k", "j2k" + std::to_string(i++) + ".j2k", enc);
    };
    codec::j2k::EncodeOptions o;
    o.levels = 2;
    one(16, 16, 1, 8, 8, o);
    one(12, 9, 3, 8, 8, o);
    o.lossless = false;
    o.quality = 50;
    one(16, 16, 1, 16, 12, o);
    o = {};
    o.levels = 1;
    o.ht = true;
    one(8, 8, 1, 16, 16, o);
    one(8, 10, 3, 8, 8, o);
    o = {};
    o.levels = 1;
    o.layers = 3;
    o.lossless = false;
    o.progression = codec::j2k::Progression::RPCL;
    o.sop = o.eph = true;
    one(16, 16, 1, 8, 8, o);
    o = {};
    o.levels = 1;
    o.tile_width = o.tile_height = 8;
    o.cblk_width_exp = o.cblk_height_exp = 3;
    one(16, 12, 1, 8, 8, o);
}

void seeds_inflate() {
    const std::vector<std::uint8_t> text = [] {
        std::string s;
        for (int i = 0; i < 40; ++i) s += "DICOM deflate seed " + std::to_string(i * i) + "\n";
        return std::vector<std::uint8_t>(s.begin(), s.end());
    }();
    const std::vector<std::uint8_t> noise = pattern(32, 32, 1, 8);
    int i = 0;
    for (int level : {0, 1, 6, 9}) {
        for (const auto* src : {&text, &noise}) {
            std::vector<std::uint8_t> z;
            check(codec::deflate_raw(src->data(), src->size(), level, z), "deflate");
            emit("codec_inflate", "z" + std::to_string(i++) + ".bin", z);
        }
    }
}

void emit_render(const std::string& name, const Dataset& d, std::string_view tsuid, std::uint8_t sel) {
    std::vector<std::uint8_t> f = part10(d, tsuid);
    f.push_back(sel);
    emit("frames_render", name, f);
}

void seeds_render() {
    {
        Dataset d = image(8, 8, 1, 16, "MONOCHROME2", 1, 12);
        native_pixels(d, pattern(8, 8, 1, 16, 1, 12), 16);
        put(d, 0x00281050, VR::DS, "2048\\1000");
        put(d, 0x00281051, VR::DS, "4096\\500");
        put(d, tags::RescaleIntercept, VR::DS, "-1024");
        put(d, tags::RescaleSlope, VR::DS, "1");
        // Overlay plane in group 6000: 8x8 bitmap.
        (void)d.set_uint16(0x60000010, 8);
        (void)d.set_uint16(0x60000011, 8);
        put(d, 0x60000040, VR::CS, "G");
        (void)d.set_bytes(0x60000050, VR::SS, Bytes::from_string(std::string("\1\0\1\0", 4)));
        (void)d.set_uint16(0x60000100, 1);
        (void)d.set_uint16(0x60000102, 0);
        (void)d.set_bytes(0x60003000, VR::OW, Bytes::from_string("\xFF\x81\x81\x81\x81\x81\x81\xFF"));
        // Rectangular shutter.
        put(d, 0x00181600, VR::CS, "RECTANGULAR");
        put(d, 0x00181602, VR::IS, "2");
        put(d, 0x00181604, VR::IS, "7");
        put(d, 0x00181606, VR::IS, "1");
        put(d, 0x00181608, VR::IS, "6");
        emit_render("mono2_16.dcm", d, ts::ExplicitVRLittleEndian, 0);
        emit_render("mono2_16_full.dcm", d, ts::ExplicitVRLittleEndian, 4);
        emit_render("mono2_16_be.dcm", d, ts::ExplicitVRBigEndian, 2 | 32);
    }
    {
        Dataset d = image(6, 6, 1, 8, "MONOCHROME1", 3);
        native_pixels(d, pattern(6, 6, 1, 8, 3), 8);
        emit_render("mono1_3frames.dcm", d, ts::ImplicitVRLittleEndian, 1 | 64);
    }
    {
        Dataset d = image(5, 7, 1, 16, "MONOCHROME2", 1, 16, true);
        native_pixels(d, pattern(5, 7, 1, 16), 16);
        std::vector<std::uint8_t> lut(256 * 2);
        for (int k = 0; k < 256; ++k) store_le16(lut.data() + 2 * k, static_cast<std::uint16_t>(k * 16));
        Dataset voi;
        (void)voi.set_bytes(0x00283002, VR::US, Bytes::from_string(std::string("\0\1\0\0\x10\0", 6)));
        (void)voi.set_bytes(0x00283006, VR::OW, Bytes::copy_of(lut.data(), lut.size()));
        d.put(sequence(0x00283010, {voi}));  // VOI LUT Sequence
        emit_render("signed_voilut.dcm", d, ts::ExplicitVRLittleEndian, 0);
    }
    {
        Dataset d = image(4, 6, 3, 8, "RGB");
        native_pixels(d, pattern(4, 6, 3, 8), 8);
        emit_render("rgb.dcm", d, ts::ExplicitVRLittleEndian, 0);
        (void)d.set_uint16(tags::PlanarConfiguration, 1);
        emit_render("rgb_planar.dcm", d, ts::ExplicitVRLittleEndian, 0);
        Dataset y = image(4, 6, 3, 8, "YBR_FULL_422");
        native_pixels(y, pattern(4, 6, 2, 8), 8);
        emit_render("ybr422.dcm", y, ts::ExplicitVRLittleEndian, 0);
    }
    {
        Dataset d = image(4, 4, 1, 8, "PALETTE COLOR");
        native_pixels(d, pattern(4, 4, 1, 8), 8);
        const std::string desc("\x10\0\0\0\x10\0", 6);  // 16 entries, first 0, 16 bits
        std::vector<std::uint8_t> lut(32);
        for (int k = 0; k < 16; ++k) store_le16(lut.data() + 2 * k, static_cast<std::uint16_t>(k * 4096));
        for (Tag t : {0x00281101u, 0x00281102u, 0x00281103u}) (void)d.set_bytes(t, VR::US, Bytes::from_string(desc));
        for (Tag t : {0x00281201u, 0x00281202u, 0x00281203u}) (void)d.set_bytes(t, VR::OW, Bytes::copy_of(lut.data(), lut.size()));
        emit_render("palette.dcm", d, ts::ExplicitVRLittleEndian, 0);
    }
    // Encapsulated: one per codec.
    auto encap = [](const char* name, std::string_view tsuid, int spp, int bits, std::string_view photometric,
                    const std::vector<std::uint8_t>& frame) {
        Dataset d = image(8, 8, spp, bits, photometric);
        encapsulated_pixels(d, {frame});
        emit_render(name, d, tsuid, 0);
    };
    {
        std::vector<std::uint8_t> enc;
        check(codec::encode_rle(pattern(8, 8, 3, 8), info(8, 8, 3, 8), {}, enc), "rle");
        encap("rle_rgb.dcm", "1.2.840.10008.1.2.5", 3, 8, "RGB", enc);
        codec::EncodeParams p;
        p.lossless = false;
        enc.clear();
        check(codec::encode_jpeg(pattern(8, 8, 1, 8), info(8, 8, 1, 8), p, enc), "jpeg");
        encap("jpeg_baseline.dcm", "1.2.840.10008.1.2.4.50", 1, 8, "MONOCHROME2", enc);
        enc.clear();
        p.lossless = true;
        check(codec::encode_jpeg(pattern(8, 8, 1, 16), info(8, 8, 1, 16), p, enc), "jpeg");
        encap("jpeg_lossless.dcm", "1.2.840.10008.1.2.4.70", 1, 16, "MONOCHROME2", enc);
        enc.clear();
        check(codec::encode_jpegls(pattern(8, 8, 1, 16), info(8, 8, 1, 16), {}, enc), "jpegls");
        encap("jpegls.dcm", "1.2.840.10008.1.2.4.80", 1, 16, "MONOCHROME2", enc);
        enc.clear();
        check(codec::encode_j2k(pattern(8, 8, 3, 8), info(8, 8, 3, 8), {}, enc), "j2k");
        encap("j2k_rgb.dcm", "1.2.840.10008.1.2.4.90", 3, 8, "YBR_RCT", enc);
    }
}

void seeds_charset() {
    auto one = [](const std::string& name, std::string_view spec, std::string_view text) {
        std::string s(spec);
        s.push_back('\0');
        s.append(text);
        emit("charset", name, s);
    };
    one("ascii.bin", "", "Doe^John\\Smith^Ann");
    one("latin1.bin", "ISO_IR 100", "Buc\xE9^J\xE9r\xF4me");
    one("utf8.bin", "ISO_IR 192", "Wang^XiaoDong=\xE7\x8E\x8B^\xE5\xB0\x8F\xE6\x9D\xB1=");
    one("jis.bin", "\\ISO 2022 IR 87",
        "Yamada^Tarou=\x1B$B;3ED\x1B(B^\x1B$BB@O:\x1B(B=\x1B$B$d$^$@\x1B(B^\x1B$B$?$m$&\x1B(B");
    one("jis_kana.bin", "ISO 2022 IR 13\\ISO 2022 IR 87", "\xD4\xCF\xC0\xDE^\xC0\xDB\xB3=\x1B$B;3ED\x1B(J^\x1B$BB@O:\x1B(J=");
    one("korean.bin", "\\ISO 2022 IR 149", "Hong^Gildong=\x1B$)C\xFB\xF3^\x1B$)C\xD1\xCE\xD4\xD7=\x1B$)C\xC8\xAB^\x1B$)C\xB1\xE6\xB5\xBF");
    one("gb18030.bin", "GB18030", "Wang^XiaoDong=\xCD\xF5^\xD0\xA1\xB6\xAB=\x81\x30\x81\x30");
    one("gbk.bin", "GBK", "\xCD\xF5^\xD0\xA1\xB6\xAB");
    one("gb2312.bin", "\\ISO 2022 IR 58", "Zhang^SanFeng=\x1B$A\xD5\xC5^\x1B$A\xC8\xFD\xB7\xE1=");
    one("cyrillic.bin", "ISO_IR 144", "\xBB\xEE\xDA\xE1\xE2\xD5\xD9\xDD^\xBD\xD8\xDA\xDE\xDB\xD0\xD9");
    one("greek.bin", "ISO_IR 126", "\xC4\xE9\xEF\xED\xF5\xF3\xE9\xEF\xF2");
    one("arabic.bin", "ISO_IR 127", "\xE2\xC8\xC7\xE6\xEA^\xE4\xE6\xC7\xD2\xC7\xE6");
    one("hebrew.bin", "ISO_IR 138", "\xF9\xF8\xE5\xEF^\xE3\xE1\xE5\xF8\xE4");
    one("thai.bin", "ISO_IR 166", "\xB9\xD2\xC2\xA1\xE8\xD2\xC7");
    one("multi.bin", "ISO 2022 IR 100\\ISO 2022 IR 144", "Buc\xE9\x1B-L\xBB\xEE\x1B-A\xE9");
    one("jisx0212.bin", "\\ISO 2022 IR 159", "\x1B$(D\x30\x21\x1B(B");
}

void seeds_pdu() {
    using namespace net;
    std::vector<std::vector<std::uint8_t>> out;
    AssociateRq rq;
    rq.calling_ae = "SCU";
    rq.called_ae = "SCP";
    std::vector<ProposedContext> pc;
    pc.push_back({std::string(sop::Verification), native_transfer_syntaxes(), std::nullopt});
    pc.push_back({"1.2.840.10008.5.1.4.1.1.2", {"1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.50"},
                  RoleSelection{"1.2.840.10008.5.1.4.1.1.2", true, true}});
    check(build_proposals(pc, rq), "proposals");
    rq.user.max_length = 16384;
    rq.user.implementation_class_uid = std::string(kImplementationClassUid);
    rq.user.implementation_version_name = std::string(kImplementationVersionName);
    rq.user.async_window = AsyncWindow{1, 1};
    rq.user.extended.push_back({"1.2.840.10008.5.1.4.1.2.2.1", {1, 0, 1}});
    rq.user.common_extended.push_back({"1.2.840.10008.5.1.4.1.1.2", "1.2.840.10008.4.2", {"1.2.840.10008.5.1.4.1.1.2.1"}});
    UserIdentityRq uid;
    uid.type = 2;
    uid.positive_response_requested = true;
    uid.primary = {'u', 's', 'e', 'r'};
    uid.secondary = {'p', 'w'};
    rq.user.identity_rq = uid;
    AssociateAc ac;
    ac.calling_ae = rq.calling_ae;
    ac.called_ae = rq.called_ae;
    ac.contexts.push_back({1, pc_result::Acceptance, "1.2.840.10008.1.2.1"});
    ac.contexts.push_back({3, pc_result::AbstractSyntaxNotSupported, "1.2.840.10008.1.2"});
    ac.user.max_length = 16384;
    ac.user.implementation_class_uid = std::string(kImplementationClassUid);
    ac.user.roles.push_back({"1.2.840.10008.5.1.4.1.1.2", true, false});
    PDataTf pd;
    std::vector<std::uint8_t> cmd;
    check(encode_command(make_c_echo_rq(1), cmd), "cmd");
    pd.pdvs.push_back({1, true, true, Bytes::copy_of(cmd.data(), cmd.size())});
    pd.pdvs.push_back({1, false, false, Bytes::from_string("ABCD")});
    const Pdu pdus[] = {rq, ac, AssociateRj{}, pd, ReleaseRq{}, ReleaseRp{}, Abort{abort_src::Provider, abort_reason::UnexpectedPdu}};
    int i = 0;
    for (const Pdu& p : pdus) {
        std::vector<std::uint8_t> e;
        check(encode_pdu(p, e), "pdu");
        emit("pdu", "pdu" + std::to_string(i++) + ".bin", e);
    }
}

std::vector<std::uint8_t> pdata(std::uint8_t ctx, const std::vector<std::uint8_t>& cmd, const std::vector<std::uint8_t>* data) {
    net::PDataTf pd;
    pd.pdvs.push_back({ctx, true, true, Bytes::copy_of(cmd.data(), cmd.size())});
    std::vector<std::uint8_t> e;
    check(net::encode_pdu(pd, e), "pdata");
    if (data) {
        net::PDataTf dd;
        const std::size_t half = data->size() / 2;
        dd.pdvs.push_back({ctx, false, false, Bytes::copy_of(data->data(), half)});
        dd.pdvs.push_back({ctx, false, true, Bytes::copy_of(data->data() + half, data->size() - half)});
        std::vector<std::uint8_t> e2;
        check(net::encode_pdu(dd, e2), "pdata");
        e.insert(e.end(), e2.begin(), e2.end());
    }
    return e;
}

void seeds_assoc() {
    using namespace net;
    std::vector<std::uint8_t> e;
    auto append = [&e](const Pdu& p) {
        std::vector<std::uint8_t> b;
        check(encode_pdu(p, b), "pdu");
        e.insert(e.end(), b.begin(), b.end());
    };
    // Acceptor: RQ, C-ECHO, C-STORE with a data set, release.
    AssociateRq rq;
    rq.calling_ae = "FUZZSCU";
    rq.called_ae = "FUZZSCP";
    std::vector<ProposedContext> pc;
    pc.push_back({std::string(sop::Verification), {"1.2.840.10008.1.2"}, std::nullopt});
    pc.push_back({"1.2.840.10008.5.1.4.1.1.2", {"1.2.840.10008.1.2.1"}, RoleSelection{"1.2.840.10008.5.1.4.1.1.2", true, true}});
    check(build_proposals(pc, rq), "proposals");
    rq.user.max_length = 16384;
    rq.user.implementation_class_uid = "1.2.3.4";
    e.push_back(0);  // selector: acceptor, small chunks
    append(rq);
    std::vector<std::uint8_t> cmd;
    check(encode_command(make_c_echo_rq(1), cmd), "cmd");
    auto p1 = pdata(1, cmd, nullptr);
    e.insert(e.end(), p1.begin(), p1.end());
    Dataset ds = base("1.2.840.10008.5.1.4.1.1.2", "1.2.3.4.5", "CT");
    std::vector<std::uint8_t> body;
    check(write_dataset(ds, ts::ExplicitVRLittleEndian, body), "body");
    cmd.clear();
    check(encode_command(make_c_store_rq(2, "1.2.840.10008.5.1.4.1.1.2", "1.2.3.4.5"), cmd), "cmd");
    auto p2 = pdata(3, cmd, &body);
    e.insert(e.end(), p2.begin(), p2.end());
    append(ReleaseRq{});
    emit("assoc", "acceptor.bin", e);
    e[0] = 0x7E;  // large chunks
    emit("assoc", "acceptor_big_chunks.bin", e);
    // Requestor: AC accepting both contexts, echo response, release response.
    e.clear();
    e.push_back(1);
    AssociateAc ac;
    ac.calling_ae = "FUZZSCU";
    ac.called_ae = "FUZZSCP";
    ac.contexts.push_back({1, pc_result::Acceptance, "1.2.840.10008.1.2"});
    ac.contexts.push_back({3, pc_result::Acceptance, "1.2.840.10008.1.2.1"});
    ac.user.max_length = 16384;
    ac.user.implementation_class_uid = "1.2.3.4";
    ac.user.roles.push_back({"1.2.840.10008.5.1.4.1.1.2", true, true});
    append(ac);
    cmd.clear();
    check(encode_command(make_response(make_c_echo_rq(1), 0x0000), cmd), "cmd");
    auto p3 = pdata(1, cmd, nullptr);
    e.insert(e.end(), p3.begin(), p3.end());
    append(ReleaseRp{});
    emit("assoc", "requestor.bin", e);
    e.resize(1);
    append(AssociateRj{});
    emit("assoc", "requestor_rejected.bin", e);
}

void seeds_dimse() {
    using namespace net;
    std::vector<Command> cs = {
        make_c_echo_rq(1),
        make_c_store_rq(2, "1.2.840.10008.5.1.4.1.1.2", "1.2.3.4", priority::High, "MOVER", 7),
        make_c_find_rq(3, sop::StudyRootFind),
        make_c_get_rq(4, sop::StudyRootGet),
        make_c_move_rq(5, sop::StudyRootMove, "DEST"),
        make_c_cancel_rq(5),
        make_n_event_report_rq(6, sop::StorageCommitmentPushModel, "1.2.840.10008.1.20.1.1", 1, true),
        make_n_get_rq(7, sop::ModalityPerformedProcedureStep, "1.2.3", {0x00100010, 0x00100020}),
        make_n_set_rq(8, sop::ModalityPerformedProcedureStep, "1.2.3"),
        make_n_action_rq(9, sop::StorageCommitmentPushModel, "1.2.840.10008.1.20.1.1", 1, true),
        make_n_create_rq(10, sop::ModalityPerformedProcedureStep, "1.2.3", true),
        make_n_delete_rq(11, sop::ModalityPerformedProcedureStep, "1.2.3"),
    };
    Command rsp = make_response(cs[4], 0xFF00);
    rsp.remaining = 3;
    rsp.completed = 1;
    rsp.failed = 0;
    rsp.warning = 0;
    cs.push_back(rsp);
    Command err = make_response(cs[1], 0xA700);
    err.error_comment = "out of resources";
    // push_back, not assignment from a one-element initializer list: GCC 13 -O2 with ASan reports a
    // false -Warray-bounds in the inlined copy of the list (testing 10b P3).
    err.offending_elements.push_back(0x00100010);
    cs.push_back(err);
    int i = 0;
    for (const Command& c : cs) {
        std::vector<std::uint8_t> e;
        check(encode_command(c, e), "cmd");
        emit("dimse", "cmd" + std::to_string(i++) + ".bin", e);
    }
}

void seeds_http() {
    auto rq = [](const std::string& name, std::string_view s) { emit("http", name, std::string(1, '\0') + std::string(s)); };
    rq("get.txt", "GET /dicomweb/studies?PatientID=12%2034&limit=10 HTTP/1.1\r\nHost: localhost:8080\r\n"
                  "Accept: application/dicom+json, multipart/related; type=\"application/dicom\"; q=0.5\r\n\r\n");
    rq("post.txt", "POST /dicomweb/studies HTTP/1.1\r\nHost: h\r\nContent-Type: multipart/related; type=\"application/dicom\"; boundary=XyZ\r\n"
                   "Content-Length: 11\r\n\r\nhello world");
    rq("chunked.txt", "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nTrailer: x\r\n\r\n");
    rq("pipelined.txt", "GET /a HTTP/1.1\r\nHost: h\r\n\r\nHEAD /b HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    rq("http10.txt", "GET / HTTP/1.0\r\n\r\n");
    emit("http", "response.txt", std::string(1, '\x01') + "HTTP/1.1 200 OK\r\nContent-Type: application/dicom+json\r\nContent-Length: 2\r\n\r\n[]");
    emit("http", "response_chunked.txt", std::string(1, '\x01') + "HTTP/1.1 206 Partial\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
    emit("http", "response_close.txt", std::string(1, '\x01') + "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nnot found body");
}

void seeds_multipart() {
    web::multipart::Writer w;
    w.add("application/dicom", "DICM-part-one");
    w.add("application/json", "{\"a\":1}", {{"Content-Location", "/x/y"}});
    w.add("application/octet-stream", std::string("\0\r\n--\r\n", 7));
    const std::string body = w.finish();
    emit("multipart", "three_parts.txt", w.boundary() + "\n" + body);
    emit("multipart", "simple.txt", std::string("b\n--b\r\nContent-Type: text/plain\r\n\r\nx\r\n--b--\r\n"));
    emit("multipart", "preamble.txt", std::string("bnd\npreamble\r\n--bnd\r\n\r\n\r\n--bnd\r\nA: b\r\n\r\ny\r\n--bnd--\r\nepilogue"));
    // The incremental parser (R-132): a byte that seeds the split points, then the same layout.
    emit("multipart_stream", "three_parts.txt", "S" + w.boundary() + "\n" + body);
    emit("multipart_stream", "simple.txt", std::string("Sb\n--b\r\nContent-Type: text/plain\r\n\r\nx\r\n--b--\r\n"));
    emit("multipart_stream", "preamble.txt",
         std::string("Sbnd\npreamble\r\n--bnd\r\n\r\n\r\n--bnd\r\nA: b\r\n\r\ny\r\n--bnd--\r\nepilogue"));
    emit("multipart_stream", "near_delimiters.txt",
         std::string("\x07" "b\n--b\r\nContent-Type: application/dicom\r\n\r\n--bx\r\n--b-\r\n\r\n--b\r\n\r\n\r\n--b--\r\ntail"));
    emit("multipart_stream", "truncated.txt", std::string("\x01" "b\n--b\r\nA: 1\r\n\r\nno close"));
}

Dataset rich_dataset() {
    Dataset d = base("1.2.840.10008.5.1.4.1.1.2", "1.2.826.0.1.3680043.8.498.20", "CT");
    put(d, tags::PatientName, VR::PN, "Yamada^Tarou=\xE5\xB1\xB1\xE7\x94\xB0^\xE5\xA4\xAA\xE9\x83\x8E=");
    put(d, tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    put(d, 0x00280030, VR::DS, "0.5\\0.5");
    put(d, 0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
    (void)d.set_uint16(tags::Rows, 2);
    (void)d.set_bytes(0x00189219, VR::FD, Bytes::from_string(std::string("\0\0\0\0\0\0\xF0\x3F", 8)));
    (void)d.set_bytes(0x00091001, VR::UN, Bytes::from_string("\x01\x02\x03\x04"));
    put(d, 0x00090010, VR::LO, "CREATOR");
    (void)d.set_bytes(0x00200020, VR::CS, Bytes::from_string("L\\F "));
    (void)d.set_bytes(0x00081160, VR::IS, Bytes::from_string("1\\2 "));
    (void)d.set_bytes(0x00041510, VR::AT, Bytes::from_string(std::string("\x10\0\x10\0", 4)));
    Dataset item;
    put(item, ct::CodeValue, VR::SH, "T-D1100");
    put(item, ct::CodingSchemeDesignator, VR::SH, "SRT");
    put(item, ct::CodeMeaning, VR::LO, "Head");
    d.put(sequence(0x00082218, {item}));
    d.put(sequence(0x00081115, {}));
    (void)d.set_bytes(tags::PixelData, VR::OW, Bytes::from_string(std::string("\1\0\2\0\3\0\4\0", 8)));
    return d;
}

void seeds_json_xml() {
    const Dataset d = rich_dataset();
    std::string js;
    check(web::dataset_to_json(d, js), "json");
    emit("dicom_json", "one.json", js);
    emit("json", "dataset.json", js);
    std::vector<Dataset> two = {d, base("1.2.3", "1.2.3.4", "MR")};
    js.clear();
    check(web::datasets_to_json(two, js), "json");
    emit("dicom_json", "two.json", js);
    web::ModelWriteOptions wo;
    wo.numbers_as_strings = true;
    wo.bulk_data_uri = [](const Element& e, const web::ElementPath&) -> std::optional<std::string> {
        if (e.tag == tags::PixelData) return std::string("https://h/bulk/7FE00010");
        return std::nullopt;
    };
    js.clear();
    check(web::dataset_to_json(d, js, wo), "json");
    emit("dicom_json", "bulk.json", js);
    emit("json", "values.json", std::string_view("[0,-1.5e+300,true,false,null,\"\\u00e9\\ud83d\\ude00\\n\",{\"\":{}},[[]]]"));
    emit("json", "deep.json", std::string_view("{\"a\":[{\"b\":[1,2,{\"c\":\"d\"}]}],\"e\":12345678901234567890}"));
    std::string xml;
    check(web::dataset_to_xml(d, xml), "xml");
    emit("dicom_xml", "one.xml", xml);
    emit("xml", "dataset.xml", xml);
    emit("xml", "misc.xml", std::string_view("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- c --><a x='1' y=\"&lt;&#x41;&#66;\"><b/><![CDATA[<raw>]]>t&amp;u<?pi x?></a>"));
}

void seeds_dicomdir() {
    const fs::path root = g_out / ".dicomdir-work";
    fs::remove_all(root);
    fs::create_directories(root);
    std::vector<std::string> files;
    for (int i = 0; i < 3; ++i) {
        Dataset d = image(2, 2, 1, 8, "MONOCHROME2");
        put(d, tags::SOPInstanceUID, VR::UI, "1.2.826.0.1.3680043.8.498.30." + std::to_string(i));
        if (i == 2) put(d, tags::SeriesInstanceUID, VR::UI, "1.2.826.0.1.3680043.8.498.31");
        native_pixels(d, pattern(2, 2, 1, 8), 8);
        const fs::path p = root / ("in" + std::to_string(i) + ".dcm");
        DicomFile f;
        f.dataset = d;
        f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
        check(write_file(f, p.string()), "write_file");
        files.push_back(p.string());
    }
    const fs::path media = root / "media";
    store::DicomdirWriteOptions o;
    o.file_set_id = "FUZZSEED";
    auto r = store::write_dicomdir(media.string(), files, o);
    check(r ? Status::ok() : r.error(), "write_dicomdir");
    std::ifstream in(media / "DICOMDIR", std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    emit("dicomdir", "dicomdir3.bin", bytes);
    in.close();
    fs::remove_all(root);
}

// Converts a store index file (16-byte header, then frames of length/CRC/type/payload) into the
// store_journal harness layout.
void frames_to_records(const fs::path& p, std::vector<std::uint8_t>& out, std::size_t* count) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t pos = 16;
    while (pos + 9 <= b.size()) {
        const std::uint32_t len = load_le32(b.data() + pos);
        if (pos + 9 + len > b.size() || len > 0xFFFF) break;
        out.push_back(b[pos + 8]);
        out.push_back(static_cast<std::uint8_t>(len));
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(pos + 9), b.begin() + static_cast<std::ptrdiff_t>(pos + 9 + len));
        pos += 9 + len;
        if (count) ++*count;
    }
}

void seeds_store() {
    const fs::path root = g_out / ".store-work";
    fs::remove_all(root);
    std::vector<std::string> uids;
    {
        auto s = store::Store::open(root.string());
        check(s ? Status::ok() : s.error(), "store open");
        for (int i = 0; i < 3; ++i) {
            Dataset d = image(2, 2, 1, 8, "MONOCHROME2");
            const std::string uid = "1.2.826.0.1.3680043.8.498.40." + std::to_string(i);
            uids.push_back(uid);
            put(d, tags::SOPInstanceUID, VR::UI, uid);
            if (i == 2) put(d, tags::PatientID, VR::LO, "PID002");
            native_pixels(d, pattern(2, 2, 1, 8), 8);
            const std::vector<std::uint8_t> f = part10(d, ts::ExplicitVRLittleEndian);
            auto r = (*s)->add_buffer(Bytes::copy_of(f.data(), f.size()));
            check(r ? Status::ok() : r.error(), "add");
        }
        auto r = (*s)->remove(store::Level::Instance, uids[1]);
        check(r ? Status::ok() : r.error(), "remove");
    }
    // Journal only (no snapshot): flags 0.
    std::vector<std::uint8_t> j = {0, 0};
    frames_to_records(root / "index" / "journal.sxj", j, nullptr);
    emit("store_journal", "journal_only.bin", j);
    // With a torn tail.
    std::vector<std::uint8_t> torn = j;
    torn[0] = 2;
    torn.push_back(0x41);
    torn.push_back(0x10);
    emit("store_journal", "journal_torn.bin", torn);
    // Snapshot plus journal: compact, then add one more instance.
    {
        auto s = store::Store::open(root.string());
        check(s ? Status::ok() : s.error(), "store open");
        check((*s)->compact(), "compact");
        Dataset d = image(2, 2, 1, 8, "MONOCHROME2");
        put(d, tags::SOPInstanceUID, VR::UI, "1.2.826.0.1.3680043.8.498.40.9");
        native_pixels(d, pattern(2, 2, 1, 8), 8);
        const std::vector<std::uint8_t> f = part10(d, ts::ExplicitVRLittleEndian);
        auto r = (*s)->add_buffer(Bytes::copy_of(f.data(), f.size()));
        check(r ? Status::ok() : r.error(), "add");
    }
    std::ifstream sh(root / "index" / "snapshot.sxs", std::ios::binary);
    std::uint8_t hdr[16] = {};
    sh.read(reinterpret_cast<char*>(hdr), 16);
    sh.close();
    std::vector<std::uint8_t> snaprecs;
    std::size_t n = 0;
    frames_to_records(root / "index" / "snapshot.sxs", snaprecs, &n);
    std::vector<std::uint8_t> both = {1, static_cast<std::uint8_t>((load_le32(hdr + 12) - 1) % 4),
                                      static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8)};
    both.insert(both.end(), snaprecs.begin(), snaprecs.end());
    frames_to_records(root / "index" / "journal.sxj", both, nullptr);
    emit("store_journal", "snapshot_journal.bin", both);
    both[0] = 1 | 4;
    emit("store_journal", "snapshot_stale_journal.bin", both);
    fs::remove_all(root);
}

void seeds_deid() {
    Dataset d = base("1.2.840.10008.5.1.4.1.1.2", "1.2.826.0.1.3680043.8.498.50", "CT");
    put(d, 0x00080012, VR::DA, "20240131");
    put(d, 0x0008002A, VR::DT, "20240131101530.123456+0100");
    put(d, 0x00080080, VR::LO, "General Hospital");
    put(d, 0x00081010, VR::SH, "STATION1");
    put(d, 0x00101010, VR::AS, "054Y");
    put(d, 0x00104000, VR::LT, "patient comments with a name");
    put(d, 0x00180015, VR::CS, "HEAD");
    put(d, 0x00200052, VR::UI, "1.2.826.0.1.3680043.8.498.51");
    put(d, 0x00090010, VR::LO, "GEMS_PETD_01");
    put(d, 0x00091001, VR::LO, "private");
    put(d, 0x00280301, VR::CS, "NO");
    Dataset ref;
    put(ref, ct::ReferencedSopClassUid, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    put(ref, ct::ReferencedSopInstanceUid, VR::UI, "1.2.826.0.1.3680043.8.498.52");
    d.put(sequence(0x00081140, {ref}));
    Dataset other;
    put(other, tags::PatientID, VR::LO, "OTHER1");
    d.put(sequence(0x00101002, {other}));
    for (std::uint8_t opt : {std::uint8_t{0}, std::uint8_t{0x02}, std::uint8_t{0x10}, std::uint8_t{0x21}, std::uint8_t{0x48}, std::uint8_t{0xFF}}) {
        std::vector<std::uint8_t> f = part10(d, ts::ExplicitVRLittleEndian);
        // The harness reads its option bits from the first two bytes (the preamble).
        f[0] = opt;
        f[1] = 3;
        emit("deid", "deid_" + std::to_string(opt) + ".dcm", f);
    }
    Dataset img = image(4, 4, 1, 8, "MONOCHROME2");
    native_pixels(img, pattern(4, 4, 1, 8), 8);
    emit("deid", "image.dcm", part10(img, ts::ImplicitVRLittleEndian));
}

void seeds_clinical() {
    // SR: CONTAINER with TEXT, NUM, CODE, IMAGE and a by-reference relationship.
    Dataset sr = base("1.2.840.10008.5.1.4.1.1.88.33", "1.2.826.0.1.3680043.8.498.60", "SR");
    auto code = [](std::string_view v, std::string_view s, std::string_view m) {
        return sequence(ct::ConceptNameCodeSequence, {clinical::code_item({std::string(v), std::string(s), std::string(m), {}})});
    };
    put(sr, ct::ValueType, VR::CS, "CONTAINER");
    sr.put(code("126000", "DCM", "Imaging Measurement Report"));
    put(sr, ct::ContinuityOfContent, VR::CS, "SEPARATE");
    put(sr, ct::CompletionFlag, VR::CS, "COMPLETE");
    put(sr, ct::VerificationFlag, VR::CS, "UNVERIFIED");
    Dataset text;
    put(text, ct::RelationshipType, VR::CS, "CONTAINS");
    put(text, ct::ValueType, VR::CS, "TEXT");
    text.put(code("121071", "DCM", "Finding"));
    put(text, ct::TextValue, VR::UT, "No acute findings");
    Dataset num;
    put(num, ct::RelationshipType, VR::CS, "CONTAINS");
    put(num, ct::ValueType, VR::CS, "NUM");
    num.put(code("410668003", "SCT", "Length"));
    Dataset mv;
    put(mv, ct::NumericValue, VR::DS, "12.5");
    mv.put(sequence(ct::MeasurementUnitsCodeSequence, {clinical::code_item({"mm", "UCUM", "millimeter", {}})}));
    num.put(sequence(ct::MeasuredValueSequence, {mv}));
    Dataset img;
    put(img, ct::RelationshipType, VR::CS, "CONTAINS");
    put(img, ct::ValueType, VR::CS, "IMAGE");
    Dataset sop;
    put(sop, ct::ReferencedSopClassUid, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    put(sop, ct::ReferencedSopInstanceUid, VR::UI, "1.2.3.4");
    img.put(sequence(ct::ReferencedSopSequence, {sop}));
    Dataset byref;
    put(byref, ct::RelationshipType, VR::CS, "INFERRED FROM");
    (void)byref.set_bytes(ct::ReferencedContentItemIdentifier, VR::UL, Bytes::from_string(std::string("\1\0\0\0\2\0\0\0", 8)));
    Dataset cd;
    put(cd, ct::RelationshipType, VR::CS, "HAS PROPERTIES");
    put(cd, ct::ValueType, VR::CS, "CODE");
    cd.put(code("121071", "DCM", "Finding"));
    cd.put(sequence(ct::ConceptCodeSequence, {clinical::code_item({"17621005", "SCT", "Normal", {}})}));
    cd.put(sequence(ct::ContentSequence, {byref}));
    sr.put(sequence(ct::ContentSequence, {text, num, img, cd}));
    emit("clinical", "sr.dcm", part10(sr, ts::ExplicitVRLittleEndian));

    // KOS through the writer.
    Dataset src = base("1.2.840.10008.5.1.4.1.1.2", "1.2.3.4", "CT");
    clinical::KosWriteRequest kr;
    kr.source = &src;
    kr.description = "key images";
    kr.references.push_back({"1.2.826.0.1.3680043.8.498.1", "1.2.826.0.1.3680043.8.498.2", "1.2.840.10008.5.1.4.1.1.2",
                             "1.2.3.4", {1, 2}, false});
    auto kos = clinical::write_kos(kr);
    check(kos ? Status::ok() : kos.error(), "kos");
    std::vector<std::uint8_t> out;
    check(write_part10(*kos, out), "kos write");
    emit("clinical", "kos.dcm", out);

    // GSPS through the writer.
    clinical::GspsWriteRequest gr;
    gr.source = &src;
    clinical::ImageReference ir;
    ir.sop_class_uid = "1.2.840.10008.5.1.4.1.1.2";
    ir.sop_instance_uid = "1.2.3.4";
    ir.series_uid = "1.2.826.0.1.3680043.8.498.2";
    ir.rows = 64;
    ir.columns = 64;
    gr.images.push_back(ir);
    clinical::GraphicAnnotation ga;
    ga.layer = "LAYER1";
    clinical::GraphicObject go;
    go.type = clinical::GraphicType::Polyline;
    go.points = {{1, 1}, {10, 1}, {10, 10}};
    ga.graphics.push_back(go);
    clinical::GraphicObject circle;
    circle.type = clinical::GraphicType::Circle;
    circle.points = {{20, 20}, {25, 20}};
    ga.graphics.push_back(circle);
    clinical::TextObject to;
    to.text = "note";
    to.anchor_units = clinical::AnnotationUnits::Pixel;
    to.anchor = clinical::Point2{5, 5};
    ga.texts.push_back(to);
    gr.annotations.push_back(ga);
    gr.window = image::VoiWindow{40, 400, image::VoiFunction::Linear, {}};
    gr.rotation = 90;
    auto ps = clinical::write_gsps(gr);
    check(ps ? Status::ok() : ps.error(), "gsps");
    out.clear();
    check(write_part10(*ps, out), "gsps write");
    emit("clinical", "gsps.dcm", out);

    // SEG: BINARY, 4x4, 2 frames bit-packed, two segments.
    Dataset seg = image(4, 4, 1, 1, "MONOCHROME2", 2);
    put(seg, tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.66.4");
    put(seg, tags::Modality, VR::CS, "SEG");
    put(seg, ct::SegmentationType, VR::CS, "BINARY");
    put(seg, ct::FrameOfReferenceUid, VR::UI, "1.2.3.4.9");
    std::vector<Dataset> segments;
    for (int k = 1; k <= 2; ++k) {
        Dataset s;
        (void)s.set_uint16(ct::SegmentNumber, static_cast<std::uint16_t>(k));
        put(s, ct::SegmentLabel, VR::LO, k == 1 ? "Liver" : "Lesion");
        put(s, ct::SegmentAlgorithmType, VR::CS, "MANUAL");
        s.put(sequence(ct::SegmentedPropertyCategoryCodeSequence, {clinical::code_item({"123037004", "SCT", "Anatomical Structure", {}})}));
        s.put(sequence(ct::SegmentedPropertyTypeCodeSequence, {clinical::code_item({"10200004", "SCT", "Liver", {}})}));
        segments.push_back(s);
    }
    seg.put(sequence(ct::SegmentSequence, segments));
    std::vector<Dataset> per_frame;
    for (int k = 1; k <= 2; ++k) {
        Dataset it, si, pos;
        (void)si.set_uint16(ct::ReferencedSegmentNumber, static_cast<std::uint16_t>(k));
        it.put(sequence(ct::SegmentIdentificationSequence, {si}));
        put(pos, ct::ImagePositionPatient, VR::DS, "0\\0\\" + std::to_string(k));
        it.put(sequence(ct::PlanePositionSequence, {pos}));
        per_frame.push_back(it);
    }
    seg.put(sequence(0x52009230, per_frame));  // Per-frame Functional Groups Sequence
    Dataset shared, orient, measures;
    put(orient, ct::ImageOrientationPatient, VR::DS, "1\\0\\0\\0\\1\\0");
    shared.put(sequence(ct::PlaneOrientationSequence, {orient}));
    put(measures, ct::PixelSpacing, VR::DS, "1\\1");
    put(measures, ct::SliceThickness, VR::DS, "1");
    shared.put(sequence(ct::PixelMeasuresSequence, {measures}));
    seg.put(sequence(0x52009229, {shared}));  // Shared Functional Groups Sequence
    (void)seg.set_bytes(tags::PixelData, VR::OB, Bytes::from_string(std::string("\x21\x84\xF0\x0F", 4)));
    emit("clinical", "seg_binary.dcm", part10(seg, ts::ExplicitVRLittleEndian));

    // RT Structure Set: one closed planar contour.
    Dataset rt = base("1.2.840.10008.5.1.4.1.1.481.3", "1.2.826.0.1.3680043.8.498.70", "RTSTRUCT");
    put(rt, 0x30060002, VR::SH, "RTSTRUCT1");
    Dataset roi;
    put(roi, ct::RoiNumber, VR::IS, "1");
    put(roi, ct::ReferencedFrameOfReferenceUid, VR::UI, "1.2.3.4.9");
    put(roi, ct::RoiName, VR::LO, "PTV");
    put(roi, ct::RoiGenerationAlgorithm, VR::CS, "MANUAL");
    rt.put(sequence(ct::StructureSetRoiSequence, {roi}));
    Dataset contour;
    put(contour, 0x30060042, VR::CS, "CLOSED_PLANAR");
    put(contour, 0x30060046, VR::IS, "3");
    put(contour, 0x30060050, VR::DS, "0\\0\\5\\10\\0\\5\\10\\10\\5");
    Dataset cimg;
    put(cimg, ct::ReferencedSopClassUid, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    put(cimg, ct::ReferencedSopInstanceUid, VR::UI, "1.2.3.4");
    contour.put(sequence(ct::ContourImageSequence, {cimg}));
    Dataset rc;
    put(rc, 0x30060084, VR::IS, "1");
    put(rc, ct::RoiDisplayColor, VR::IS, "255\\0\\0");
    rc.put(sequence(0x30060040, {contour}));
    rt.put(sequence(0x30060039, {rc}));
    Dataset obs;
    put(obs, 0x30060082, VR::IS, "1");
    put(obs, 0x30060084, VR::IS, "1");
    put(obs, 0x300600A4, VR::CS, "PTV");
    rt.put(sequence(0x30060080, {obs}));
    emit("clinical", "rtstruct.dcm", part10(rt, ts::ImplicitVRLittleEndian));
}

void seeds_values() {
    const char* v[] = {"1.5", " -0.0e+10 ", "+12345678901234", "1e-320", "1.7976931348623157e308", ".5", "5.",
                       "1\\2.5\\-3e2", "20240229", "1900.01.01", "00010101", "99991231", "235959.123456",
                       "12:30:45.5", "0700", "20240101120000.5+0530", "2024-1200", "1999123124", "20240131101530.123456&0000",
                       "19700101-20241231", "-20200101", "20200101-", "  42 ", "-2147483649", "NaN", "inf"};
    int i = 0;
    for (const char* s : v) emit("values", "v" + std::to_string(i++) + ".txt", std::string_view(s));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-folder>\n", argv[0]);
        return 2;
    }
    g_out = argv[1];
    fs::create_directories(g_out);
    seeds_part10();
    seeds_rle();
    seeds_jpeg();
    seeds_jpegls();
    seeds_j2k();
    seeds_inflate();
    seeds_render();
    seeds_charset();
    seeds_pdu();
    seeds_assoc();
    seeds_dimse();
    seeds_http();
    seeds_multipart();
    seeds_json_xml();
    seeds_dicomdir();
    seeds_store();
    seeds_deid();
    seeds_clinical();
    seeds_values();
    std::printf("%d seeds written to %s\n", g_count, g_out.string().c_str());
    return 0;
}
