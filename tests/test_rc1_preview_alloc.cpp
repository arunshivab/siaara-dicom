// Release-candidate review, the mail-preview use case: untrusted attachment bytes -> a caption and
// a bounded picture (examples/c/attachment_preview.h, the path the examples document). Hostile
// input - mutated and truncated files, decompression bombs, a deflate bomb, a million claimed
// frames, element and nesting bombs - must end in an error or a bounded preview, within a small
// memory budget and a time limit; never a crash or a hang.
// Round 6: R-191 (a render is charged to the frame memory budget; a pixel-count cap before any
// decode), R-192 (JPEG scans per frame, a pixel budget for the whole preview), R-193 (a huge
// caption value is not decoded), R-199 (control and format characters, HTML escaping), R-201.
// Uses the allocation guard (alloc_guard.hpp), so this file is left out of TSan builds.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "alloc_guard.hpp"
#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

#include "../examples/c/attachment_preview.h"

using namespace siaara;

namespace {

constexpr const char* kRle = "1.2.840.10008.1.2.5";
constexpr const char* kJpeg = "1.2.840.10008.1.2.4.50";
constexpr const char* kJls = "1.2.840.10008.1.2.4.80";
constexpr const char* kJ2k = "1.2.840.10008.1.2.4.90";
constexpr const char* kDeflated = "1.2.840.10008.1.2.1.99";

// The preview may use this much heap in all, whatever the attachment claims ...
constexpr std::size_t kHeapBudget = std::size_t{96} << 20;
// ... and this long (a sanitizer build on a slow machine included).
constexpr double kSecondsPerPreview = 20.0;

// Limits tighter than the recommended ones, so that the bombs below are small enough for a test.
preview_limits test_limits() {
    preview_limits l;
    preview_default_limits(&l);
    l.max_attachment_bytes = std::size_t{16} << 20;
    l.max_elements = 20000;
    l.max_inflated_size = std::uint64_t{8} << 20;
    l.max_frame_bytes = std::uint64_t{8} << 20;
    l.frame_memory_budget = std::uint64_t{32} << 20;
    l.max_strip_frames = 4;
    return l;
}

struct Run {
    int outcome = -1;
    std::size_t peak = 0;
    bool refused = false;
    double seconds = 0;
    std::uint32_t width = 0, height = 0, strip_frames = 0;
    bool incomplete = false;
    std::string reason;
};

// One preview under the allocation guard and a clock; a picture is also encoded, as the mail
// server would.
Run preview(const std::vector<std::uint8_t>& bytes, const preview_limits& lim, std::size_t heap_budget = kHeapBudget) {
    Run run;
    preview_result r;
    const auto t0 = std::chrono::steady_clock::now();
    {
        siaara_test::AllocGuard guard(heap_budget);
        run.outcome = preview_attachment(bytes.data(), bytes.size(), &lim, &r);
        if (run.outcome == PREVIEW_OK) {
            siaara_buffer* png = nullptr;
            siaara_buffer* jpeg = nullptr;
            if (preview_encode(&r.image, 0, &png) != SIAARA_OK || preview_encode(&r.image, 1, &jpeg) != SIAARA_OK) run.outcome = -2;
            siaara_buffer_free(png);
            siaara_buffer_free(jpeg);
        }
        run.peak = guard.peak();
        run.refused = guard.refused();
    }
    run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    run.width = r.image.width;
    run.height = r.image.height;
    run.strip_frames = r.strip_frames;
    run.incomplete = r.incomplete != 0;
    run.reason = r.reason;
    preview_free(&r);
    return run;
}

// What every preview of hostile bytes must satisfy.
bool bounded(const Run& run, const preview_limits& lim, const char* what) {
    bool ok = true;
    if (run.outcome < PREVIEW_OK || run.outcome > PREVIEW_DAMAGED) ok = false;
    if (run.peak >= kHeapBudget) ok = false;
    if (run.seconds >= kSecondsPerPreview) ok = false;
    if (run.outcome == PREVIEW_OK && (run.width == 0 || run.height == 0 || run.width > lim.max_box || run.height > lim.max_box)) ok = false;
    if (!ok)
        std::fprintf(stderr, "  %s: outcome %d, peak %zu bytes, %.2f s, picture %u x %u (%s)\n", what, run.outcome, run.peak, run.seconds,
                     static_cast<unsigned>(run.width), static_cast<unsigned>(run.height), run.reason.c_str());
    return ok;
}

struct ImageSpec {
    std::uint16_t rows = 64, columns = 48, bits = 16, samples = 1;
    std::uint32_t frames = 1;
    const char* photometric = "MONOCHROME2";
    bool is_signed = false;
    std::uint32_t pixel_bytes = 0xFFFFFFFFu;  // bytes of Pixel Data actually written (default: all)
    const char* claimed_frames = nullptr;     // Number of Frames as written, when it is a lie
};

DicomFile image_file(const ImageSpec& s) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.777001");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.777900");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.777901");
    (void)ds.set_string(tags::Modality, VR::CS, "OT");
    (void)ds.set_string(0x00080020, VR::DA, "20260115");
    (void)ds.set_string(0x0008103E, VR::LO, "Preview test");
    (void)ds.set_string(tags::PatientName, VR::PN, "Hostile^Input");
    (void)ds.set_string(tags::PatientID, VR::LO, "H-1");
    (void)ds.set_uint16(tags::SamplesPerPixel, s.samples);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, s.photometric);
    if (s.samples > 1) (void)ds.set_uint16(tags::PlanarConfiguration, 0);
    if (s.frames > 1 || s.claimed_frames)
        (void)ds.set_string(tags::NumberOfFrames, VR::IS, s.claimed_frames ? s.claimed_frames : std::to_string(s.frames));
    (void)ds.set_uint16(tags::Rows, s.rows);
    (void)ds.set_uint16(tags::Columns, s.columns);
    (void)ds.set_uint16(tags::BitsAllocated, s.bits);
    (void)ds.set_uint16(tags::BitsStored, static_cast<std::uint16_t>(s.bits == 16 ? 12 : s.bits));
    (void)ds.set_uint16(tags::HighBit, static_cast<std::uint16_t>(s.bits == 16 ? 11 : s.bits - 1));
    (void)ds.set_uint16(tags::PixelRepresentation, s.is_signed ? 1 : 0);
    if (std::string(s.photometric) == "PALETTE COLOR") {
        const std::uint16_t desc[3] = {256, 0, 16};
        std::vector<std::uint8_t> lut(512);
        for (std::size_t i = 0; i < 256; ++i) lut[2 * i + 1] = static_cast<std::uint8_t>(i);
        for (const Tag t : {Tag{0x00281101u}, Tag{0x00281102u}, Tag{0x00281103u}})
            (void)ds.set_bytes(t, VR::US, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(desc), sizeof desc));
        for (const Tag t : {Tag{0x00281201u}, Tag{0x00281202u}, Tag{0x00281203u}})
            (void)ds.set_bytes(t, VR::OW, Bytes::copy_of(lut.data(), lut.size()));
    }
    std::uint64_t n = std::uint64_t{s.rows} * s.columns * s.samples * (s.bits / 8u) * s.frames;
    if (s.pixel_bytes != 0xFFFFFFFFu) n = s.pixel_bytes;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(n));
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>(s.bits == 16 && (k & 1u) ? (k >> 7) & 0x0Fu : (k * 3u) & 0xFFu);
    (void)ds.set_bytes(tags::PixelData, s.bits > 8 ? VR::OW : VR::OB, Bytes::copy_of(px.data(), px.size()));
    return f;
}

std::vector<std::uint8_t> bytes_of(const DicomFile& f) {
    std::vector<std::uint8_t> out;
    WriteOptions wo;
    wo.allow_incomplete = true;
    (void)write_part10(f, out, wo);
    return out;
}

std::vector<std::uint8_t> encoded(const ImageSpec& s, const char* transfer_syntax, bool lossy = false) {
    DicomFile f = image_file(s);
    codec::TranscodeParams p;
    p.lossy = lossy;
    p.allow_lossy = true;
    p.keep_uid_on_lossy = true;
    if (!codec::transcode_file(f, transfer_syntax, p)) return {};
    return bytes_of(f);
}

// Position of the first occurrence of a two-byte marker at or after `from`, or npos.
std::size_t find_marker(const std::vector<std::uint8_t>& b, std::uint8_t m0, std::uint8_t m1, std::size_t from = 0) {
    for (std::size_t i = from; i + 1 < b.size(); ++i)
        if (b[i] == m0 && b[i + 1] == m1) return i;
    return std::string::npos;
}

std::uint32_t next_random(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

void put16(std::vector<std::uint8_t>& b, std::uint32_t x) {
    b.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t x) {
    put16(b, x & 0xFFFFu);
    put16(b, x >> 16);
}

// The file meta (preamble, "DICM", group 0002) of an Explicit VR Little Endian file.
std::vector<std::uint8_t> meta_only() {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, "2.25.777002");
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    out.resize(144u + load_le32(out.data() + 140));
    return out;
}

}  // namespace

TEST_CASE("rc1 preview: good attachments of every kind give a bounded picture; pixel-less and undecodable ones a reason") {
    const preview_limits lim = test_limits();
    struct Kind {
        const char* name;
        ImageSpec spec;
        const char* transfer_syntax;  // nullptr: as built (Explicit VR Little Endian)
        bool lossy;
    };
    ImageSpec gray16, gray8, rgb, palette, is_signed, big, cine;
    gray8.bits = 8;
    rgb.bits = 8;
    rgb.samples = 3;
    rgb.photometric = "RGB";
    palette.bits = 8;
    palette.photometric = "PALETTE COLOR";
    is_signed.is_signed = true;
    big.rows = 1500;   // larger than the box in both directions: scaled down
    big.columns = 2000;
    cine.frames = 40;  // a big multi-frame: only a few frames are ever decoded
    const Kind kinds[] = {
        {"16-bit grayscale", gray16, nullptr, false}, {"8-bit grayscale", gray8, nullptr, false},
        {"RGB", rgb, nullptr, false},                 {"palette colour", palette, nullptr, false},
        {"signed 16-bit", is_signed, nullptr, false}, {"2000 x 1500", big, nullptr, false},
        {"40 frames", cine, nullptr, false},          {"RLE", gray16, kRle, false},
        {"JPEG baseline colour", rgb, kJpeg, true},   {"JPEG-LS", gray16, kJls, false},
        {"JPEG 2000", gray16, kJ2k, false},           {"JPEG 2000, 40 frames", cine, kJ2k, false},
        {"deflated", gray16, kDeflated, false},
    };
    for (const Kind& k : kinds) {
        std::vector<std::uint8_t> bytes;
        if (!k.transfer_syntax) {
            bytes = bytes_of(image_file(k.spec));
        } else if (std::string(k.transfer_syntax) == kDeflated) {
            DicomFile f = image_file(k.spec);
            f.transfer_syntax = kDeflated;
            (void)f.meta.set_string(tags::TransferSyntaxUID, VR::UI, kDeflated);
            bytes = bytes_of(f);
        } else {
            bytes = encoded(k.spec, k.transfer_syntax, k.lossy);
        }
        REQUIRE(!bytes.empty());
        const Run run = preview(bytes, lim);
        if (run.outcome != PREVIEW_OK) std::fprintf(stderr, "  %s: outcome %d (%s)\n", k.name, run.outcome, run.reason.c_str());
        CHECK_EQ(run.outcome, PREVIEW_OK);
        CHECK(bounded(run, lim, k.name));
        CHECK(!run.incomplete);
        CHECK(run.width <= lim.max_box && run.height <= lim.max_box);
        if (k.spec.rows == 1500) {
            CHECK_EQ(run.width, 512u);
            CHECK_EQ(run.height, 384u);
        }
        CHECK_EQ(run.strip_frames, k.spec.frames > 1 ? lim.max_strip_frames : 0u);
        // Bytes after the data set are lost: still a picture, flagged as incomplete.
        if (!k.transfer_syntax && k.spec.frames == 1 && k.spec.rows == 64 && k.spec.bits == 16 && !k.spec.is_signed) {
            std::vector<std::uint8_t> tail = bytes;
            const std::uint8_t cut_element[] = {0xFC, 0xFF, 0xFC, 0xFF, 'O', 'B', 0, 0, 100, 0, 0, 0, 1, 2, 3};  // ends inside its value
            tail.insert(tail.end(), cut_element, cut_element + sizeof cut_element);
            const Run cut = preview(tail, lim);
            CHECK_EQ(cut.outcome, PREVIEW_OK);
            CHECK(cut.incomplete);
            CHECK(bounded(cut, lim, "trailing cut element"));
        }
    }
    // Patient identifiers only on request.
    {
        const std::vector<std::uint8_t> bytes = bytes_of(image_file(gray16));
        char name[64], id[64];
        CHECK_EQ(preview_patient_fields(bytes.data(), bytes.size(), &lim, name, sizeof name, id, sizeof id), 0);
        CHECK(std::string(name) == "Hostile^Input");
        CHECK(std::string(id) == "H-1");
    }
    // No picture, with the reason: not DICOM; no pixel data (SR, RT Structure Set, PDF, DICOMDIR are
    // all "a DICOM object without Pixel Data" here); a transfer syntax without a decoder.
    {
        const std::vector<std::uint8_t> text(4096, 'a');
        CHECK_EQ(preview(text, lim).outcome, PREVIEW_NOT_DICOM);
        CHECK_EQ(preview({}, lim).outcome, PREVIEW_NOT_DICOM);
        for (const char* sop : {"1.2.840.10008.5.1.4.1.1.88.11", "1.2.840.10008.5.1.4.1.1.481.3", "1.2.840.10008.5.1.4.1.1.104.1",
                                "1.2.840.10008.1.3.10"}) {
            DicomFile f;
            f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
            (void)f.dataset.set_string(tags::SOPClassUID, VR::UI, sop);
            (void)f.dataset.set_string(tags::SOPInstanceUID, VR::UI, "2.25.777010");
            (void)f.dataset.set_string(tags::Modality, VR::CS, "SR");
            const Run run = preview(bytes_of(f), lim);
            CHECK_EQ(run.outcome, PREVIEW_NO_PIXEL_DATA);
            CHECK(bounded(run, lim, sop));
        }
        // MPEG-4 video, HEVC, JPEG XL: stored and sent untouched by the toolkit, never decoded.
        for (const char* video : {"1.2.840.10008.1.2.4.102", "1.2.840.10008.1.2.4.107", "1.2.840.10008.1.2.4.110"}) {
            DicomFile f = image_file(gray8);
            Element px;
            px.tag = tags::PixelData;
            px.vr = VR::OB;
            px.fragments.push_back(Bytes{});
            const std::vector<std::uint8_t> stream(2000, 0x47);
            px.fragments.push_back(Bytes::copy_of(stream.data(), stream.size()));
            f.dataset.put(std::move(px));
            f.transfer_syntax = video;
            (void)f.meta.set_string(tags::TransferSyntaxUID, VR::UI, video);
            const Run run = preview(bytes_of(f), lim);
            if (run.outcome != PREVIEW_UNSUPPORTED) std::fprintf(stderr, "  %s: outcome %d (%s)\n", video, run.outcome, run.reason.c_str());
            CHECK_EQ(run.outcome, PREVIEW_UNSUPPORTED);
            CHECK(!run.reason.empty());
            CHECK(bounded(run, lim, video));
        }
    }
}

TEST_CASE("rc1 preview: mutated and truncated attachments never crash, hang or leave the memory budget") {
    const preview_limits lim = test_limits();
    ImageSpec gray16, rgb, cine;
    rgb.bits = 8;
    rgb.samples = 3;
    rgb.photometric = "RGB";
    cine.frames = 6;
    std::vector<std::vector<std::uint8_t>> seeds;
    seeds.push_back(bytes_of(image_file(gray16)));
    seeds.push_back(bytes_of(image_file(cine)));
    seeds.push_back(encoded(gray16, kRle));
    seeds.push_back(encoded(rgb, kJpeg, true));
    seeds.push_back(encoded(gray16, kJls));
    seeds.push_back(encoded(gray16, kJ2k));
    seeds.push_back(encoded(cine, kJ2k));
    {
        DicomFile f = image_file(gray16);
        f.transfer_syntax = kDeflated;
        (void)f.meta.set_string(tags::TransferSyntaxUID, VR::UI, kDeflated);
        seeds.push_back(bytes_of(f));
    }
    std::uint32_t rng = 0x5EED1234u;
    int previews = 0, pictures = 0, failures = 0;
    double slowest = 0;
    std::size_t highest = 0;
    for (const std::vector<std::uint8_t>& seed : seeds) {
        REQUIRE(seed.size() > 400);
        // Truncations: a spread over the whole file, and every length around the header's end.
        std::vector<std::size_t> cuts;
        for (std::size_t i = 1; i <= 24; ++i) cuts.push_back(seed.size() * i / 25);
        for (std::size_t n = 128; n < 400; n += 9) cuts.push_back(n);
        for (const std::size_t n : cuts) {
            const std::vector<std::uint8_t> cut(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(n));
            const Run run = preview(cut, lim);
            failures += bounded(run, lim, "truncated") ? 0 : 1;
            ++previews;
            pictures += run.outcome == PREVIEW_OK ? 1 : 0;
            slowest = std::max(slowest, run.seconds);
            highest = std::max(highest, run.peak);
        }
        // Mutations: 1 to 8 bytes changed anywhere after the preamble (lengths, tags, codestreams).
        for (int i = 0; i < 60; ++i) {
            std::vector<std::uint8_t> m = seed;
            const std::uint32_t changes = 1 + next_random(rng) % 8;
            for (std::uint32_t c = 0; c < changes; ++c) {
                const std::size_t at = 132 + next_random(rng) % (m.size() - 132);
                const std::uint32_t how = next_random(rng) % 4;
                std::uint32_t v = next_random(rng) & 0xFFu;
                if (how == 0) v = 0xFFu;
                else if (how == 1) v = 0u;
                else if (how == 2) v = (m[at] ^ (1u << (next_random(rng) % 8u))) & 0xFFu;
                m[at] = static_cast<std::uint8_t>(v);
            }
            const Run run = preview(m, lim);
            failures += bounded(run, lim, "mutated") ? 0 : 1;
            ++previews;
            pictures += run.outcome == PREVIEW_OK ? 1 : 0;
            slowest = std::max(slowest, run.seconds);
            highest = std::max(highest, run.peak);
        }
    }
    std::fprintf(stderr, "  %d hostile previews: %d pictures, slowest %.2f s, highest heap %zu kB\n", previews, pictures, slowest, highest / 1024);
    CHECK_EQ(failures, 0);
    CHECK(previews > 800);
    CHECK(pictures > 0);  // many mutations leave the image decodable: those are shown
}

TEST_CASE("rc1 preview: decompression bombs, a deflate bomb, a million frames, element and nesting bombs") {
    const preview_limits lim = test_limits();
    // Huge Rows x Columns with almost no data: 65535 x 65535, 16 bit, 3 samples = 25 GiB "per frame".
    {
        ImageSpec s;
        s.rows = 65535;
        s.columns = 65535;
        s.samples = 3;
        s.photometric = "RGB";
        s.pixel_bytes = 64;
        const Run run = preview(bytes_of(image_file(s)), lim);
        CHECK(run.outcome == PREVIEW_TOO_LARGE || run.outcome == PREVIEW_DAMAGED);
        CHECK(bounded(run, lim, "huge rows x columns, native"));
        CHECK(run.peak < (std::size_t{4} << 20));
        // The same claim in front of compressed streams.
        for (const char* tsx : {kRle, kJls, kJ2k}) {
            ImageSpec small;
            std::vector<std::uint8_t> b = encoded(small, tsx);
            REQUIRE(!b.empty());
            auto f = read_part10(Bytes::copy_of(b.data(), b.size()));
            REQUIRE(f);
            (void)f->dataset.set_uint16(tags::Rows, 65535);
            (void)f->dataset.set_uint16(tags::Columns, 65535);
            const Run r2 = preview(bytes_of(*f), lim);
            // Refused - or, where the codestream states its own (small) size and lenient reading
            // takes that over the attributes (JPEG-LS, JPEG 2000), the small picture it really holds.
            CHECK(r2.outcome == PREVIEW_TOO_LARGE || r2.outcome == PREVIEW_DAMAGED ||
                  (r2.outcome == PREVIEW_OK && r2.width == 48 && r2.height == 64));
            if (std::string(tsx) == kRle) CHECK_EQ(r2.outcome, PREVIEW_TOO_LARGE);
            CHECK(bounded(r2, lim, tsx));
            CHECK(r2.peak < (std::size_t{8} << 20));
        }
    }
    // Codestreams that claim huge dimensions themselves, behind modest DICOM attributes.
    {
        ImageSpec small;
        // JPEG 2000: SIZ marker FF51, then Lsiz(2) Rsiz(2) Xsiz(4) Ysiz(4), big endian.
        std::vector<std::uint8_t> j2k = encoded(small, kJ2k);
        std::size_t siz = find_marker(j2k, 0xFF, 0x51, 300);
        REQUIRE(siz != std::string::npos);
        for (std::size_t k = 0; k < 8; ++k) j2k[siz + 6 + k] = (k % 4) == 0 ? 0x7F : 0xFF;  // 0x7FFFFFFF x 0x7FFFFFFF
        const Run a = preview(j2k, lim);
        CHECK(a.outcome != PREVIEW_OK || (a.width <= lim.max_box && a.height <= lim.max_box));
        CHECK(bounded(a, lim, "JPEG 2000 claiming 2^31 x 2^31"));
        std::vector<std::uint8_t> j2k_60k = encoded(small, kJ2k);
        siz = find_marker(j2k_60k, 0xFF, 0x51, 300);
        const std::uint8_t dim[4] = {0x00, 0x00, 0xEA, 0x60};  // 60000
        std::memcpy(&j2k_60k[siz + 6], dim, 4);
        std::memcpy(&j2k_60k[siz + 10], dim, 4);
        CHECK(bounded(preview(j2k_60k, lim), lim, "JPEG 2000 claiming 60000 x 60000"));
        // JPEG-LS: SOF55 marker FFF7, then Lf(2) P(1) Y(2) X(2).
        std::vector<std::uint8_t> jls = encoded(small, kJls);
        const std::size_t sof = find_marker(jls, 0xFF, 0xF7, 300);
        REQUIRE(sof != std::string::npos);
        jls[sof + 5] = jls[sof + 6] = jls[sof + 7] = jls[sof + 8] = 0xFF;  // 65535 x 65535
        CHECK(bounded(preview(jls, lim), lim, "JPEG-LS claiming 65535 x 65535"));
        // JPEG: SOF0 marker FFC0, the same layout.
        ImageSpec rgb;
        rgb.bits = 8;
        rgb.samples = 3;
        rgb.photometric = "RGB";
        std::vector<std::uint8_t> jpg = encoded(rgb, kJpeg, true);
        const std::size_t sof0 = find_marker(jpg, 0xFF, 0xC0, 300);
        REQUIRE(sof0 != std::string::npos);
        jpg[sof0 + 5] = jpg[sof0 + 6] = jpg[sof0 + 7] = jpg[sof0 + 8] = 0xFF;
        CHECK(bounded(preview(jpg, lim), lim, "JPEG claiming 65535 x 65535"));
        // RLE: the largest frame the limits allow (2048 x 2048, 16 bit = 8 MiB) behind a stream of
        // a few bytes: decoded leniently (missing bytes are zero) or refused - bounded either way.
        ImageSpec tiny;
        std::vector<std::uint8_t> rle = encoded(tiny, kRle);
        auto rf = read_part10(Bytes::copy_of(rle.data(), rle.size()));
        REQUIRE(rf);
        (void)rf->dataset.set_uint16(tags::Rows, 2048);
        (void)rf->dataset.set_uint16(tags::Columns, 2048);
        CHECK(bounded(preview(bytes_of(*rf), lim), lim, "RLE, 8 MiB frame from a tiny stream"));
    }
    // A deflate bomb: 64 MiB of zeros in a few kB of a deflated transfer syntax; the limit is 8 MiB.
    {
        std::vector<std::uint8_t> bomb;
        {
            DicomFile f;
            f.transfer_syntax = kDeflated;
            (void)f.dataset.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
            (void)f.dataset.set_string(tags::SOPInstanceUID, VR::UI, "2.25.777020");
            const std::vector<std::uint8_t> zeros(std::size_t{64} << 20, 0);
            (void)f.dataset.set_bytes(0x00420011, VR::OB, Bytes::copy_of(zeros.data(), zeros.size()));
            bomb = bytes_of(f);
        }
        REQUIRE(!bomb.empty());
        CHECK(bomb.size() < (std::size_t{1} << 20));
        const Run run = preview(bomb, lim);
        CHECK_EQ(run.outcome, PREVIEW_TOO_LARGE);
        CHECK(bounded(run, lim, "deflate bomb"));
        // Bounded by the limit (the output buffer grows by doubling up to it), not by the 64 MiB
        // the stream would inflate to.
        CHECK(run.peak < 4 * lim.max_inflated_size);
        // The C API option on its own, and the unchanged default.
        siaara_read_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.allow_raw_dataset = 1;
        ro.max_inflated_size = std::uint64_t{8} << 20;
        siaara_file* file = nullptr;
        CHECK(siaara_file_open_memory(bomb.data(), bomb.size(), &ro, &file) == SIAARA_ERR_LIMIT_EXCEEDED);
        ro.max_inflated_size = 0;  // the default (4 GiB): the same bytes open
        REQUIRE(siaara_file_open_memory(bomb.data(), bomb.size(), &ro, &file) == SIAARA_OK);
        siaara_file_free(file);
    }
    // 1,000,000 claimed frames with the pixel data of one.
    {
        ImageSpec s;
        s.claimed_frames = "1000000";
        const Run native = preview(bytes_of(image_file(s)), lim);
        CHECK(bounded(native, lim, "1,000,000 frames claimed, native"));
        CHECK(native.peak < (std::size_t{16} << 20));
        for (const char* tsx : {kRle, kJ2k}) {
            ImageSpec one;
            std::vector<std::uint8_t> b = encoded(one, tsx);
            auto f = read_part10(Bytes::copy_of(b.data(), b.size()));
            REQUIRE(f);
            (void)f->dataset.set_string(tags::NumberOfFrames, VR::IS, "1000000");
            const Run run = preview(bytes_of(*f), lim);
            CHECK(bounded(run, lim, "1,000,000 frames claimed, encapsulated"));
            CHECK(run.peak < (std::size_t{16} << 20));
        }
    }
    // Element bomb: 400,000 empty items (8 bytes each), and 400,000 tiny elements.
    {
        std::vector<std::uint8_t> items = meta_only();
        put16(items, 0x0008);
        put16(items, 0x1140);
        items.push_back('S');
        items.push_back('Q');
        put16(items, 0);
        put32(items, 0xFFFFFFFFu);
        for (int i = 0; i < 400000; ++i) {
            put16(items, 0xFFFE);
            put16(items, 0xE000);
            put32(items, 0);
        }
        const Run a = preview(items, lim);
        CHECK_EQ(a.outcome, PREVIEW_TOO_LARGE);
        CHECK(bounded(a, lim, "400,000 empty items"));
        CHECK(a.peak < (std::size_t{32} << 20));
        std::vector<std::uint8_t> elements = meta_only();
        for (std::uint32_t i = 0; i < 400000; ++i) {
            put16(elements, 0x0009 + 2 * (i / 60000));
            put16(elements, 0x1000 + (i % 60000));
            elements.push_back('U');
            elements.push_back('S');
            put16(elements, 0);
        }
        const Run b = preview(elements, lim);
        CHECK_EQ(b.outcome, PREVIEW_TOO_LARGE);
        CHECK(bounded(b, lim, "400,000 elements"));
        CHECK(b.peak < (std::size_t{32} << 20));
        // The C API options on their own; the defaults read both files.
        siaara_read_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        siaara_file* file = nullptr;
        REQUIRE(siaara_file_open_memory(items.data(), items.size(), &ro, &file) == SIAARA_OK);
        siaara_file_free(file);
        ro.max_elements = 1000;
        CHECK(siaara_file_open_memory(items.data(), items.size(), &ro, &file) == SIAARA_ERR_LIMIT_EXCEEDED);
        CHECK(siaara_file_open_memory(elements.data(), elements.size(), &ro, &file) == SIAARA_ERR_LIMIT_EXCEEDED);
    }
    // Nesting bomb: 5,000 sequences inside one another.
    {
        std::vector<std::uint8_t> nest = meta_only();
        for (int i = 0; i < 5000; ++i) {
            put16(nest, 0x0008);
            put16(nest, 0x1140);
            nest.push_back('S');
            nest.push_back('Q');
            put16(nest, 0);
            put32(nest, 0xFFFFFFFFu);
            put16(nest, 0xFFFE);
            put16(nest, 0xE000);
            put32(nest, 0xFFFFFFFFu);
        }
        const Run run = preview(nest, lim);
        CHECK(run.outcome != PREVIEW_OK);
        CHECK(bounded(run, lim, "5,000 nested sequences"));
    }
}

TEST_CASE("rc1 preview: the frame limits of siaara_read_options hold for every frame function of the file handle") {
    ImageSpec s;
    s.rows = 512;
    s.columns = 512;  // 512 KiB per frame
    const std::vector<std::uint8_t> native = bytes_of(image_file(s));
    const std::vector<std::uint8_t> j2k = encoded(s, kJ2k);
    REQUIRE(!j2k.empty());
    for (const std::vector<std::uint8_t>* bytes : {&native, &j2k}) {
        std::vector<std::uint8_t> out(512u * 512u * 4u);
        size_t need = 0;
        // Defaults (options NULL, or the new fields 0): everything works as before.
        for (int pass = 0; pass < 2; ++pass) {
            siaara_read_options ro;
            std::memset(&ro, 0, sizeof ro);
            ro.struct_size = sizeof ro;
            ro.allow_raw_dataset = 1;
            siaara_file* file = nullptr;
            REQUIRE(siaara_file_open_memory(bytes->data(), bytes->size(), pass == 0 ? nullptr : &ro, &file) == SIAARA_OK);
            CHECK(siaara_frame_decode(file, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) == SIAARA_OK);
            CHECK(siaara_render_rgba8(file, nullptr, out.data(), out.size(), &need) == SIAARA_OK);
            siaara_frame* frame = nullptr;
            CHECK(siaara_frame_open(file, 0, &frame) == SIAARA_OK);
            siaara_frame_free(frame);
            siaara_file_free(file);
        }
        // max_frame_bytes below the frame's size: refused before anything is decoded, with a
        // small heap budget to prove it; the header is still readable.
        siaara_read_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.allow_raw_dataset = 1;
        ro.max_frame_bytes = 100000;
        siaara_file* file = nullptr;
        REQUIRE(siaara_file_open_memory(bytes->data(), bytes->size(), &ro, &file) == SIAARA_OK);
        siaara_pixel_info pi;
        std::memset(&pi, 0, sizeof pi);
        pi.struct_size = sizeof pi;
        CHECK(siaara_file_pixel_info(file, &pi) == SIAARA_OK);
        CHECK_EQ(pi.frame_bytes, 512u * 512u * 2u);
        {
            siaara_test::AllocGuard guard(256 * 1024);
            siaara_frame* frame = nullptr;
            siaara_pixel_value pv;
            std::memset(&pv, 0, sizeof pv);
            pv.struct_size = sizeof pv;
            uint32_t warnings = 0;
            CHECK(siaara_frame_decode(file, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) == SIAARA_ERR_LIMIT_EXCEEDED);
            CHECK(siaara_render_rgba8(file, nullptr, out.data(), out.size(), &need) == SIAARA_ERR_LIMIT_EXCEEDED);
            CHECK(siaara_frame_open(file, 0, &frame) == SIAARA_ERR_LIMIT_EXCEEDED);
            CHECK(frame == nullptr);
            CHECK(siaara_file_pixel_value(file, 0, 1, 1, 0, &pv) == SIAARA_ERR_LIMIT_EXCEEDED);
            CHECK(siaara_frame_warning_count(file, 0, &warnings) == SIAARA_ERR_LIMIT_EXCEEDED);
            CHECK(!guard.refused());
        }
        siaara_file_free(file);
        // frame_memory_budget below the frame's size: the same.
        ro.max_frame_bytes = 0;
        ro.frame_memory_budget = 100000;
        REQUIRE(siaara_file_open_memory(bytes->data(), bytes->size(), &ro, &file) == SIAARA_OK);
        CHECK(siaara_frame_decode(file, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) != SIAARA_OK);
        siaara_frame* frame = nullptr;
        CHECK(siaara_frame_open(file, 0, &frame) != SIAARA_OK);
        siaara_file_free(file);
        // An older caller's struct (without the new fields) is read as before.
        ro.frame_memory_budget = 1;
        ro.max_frame_bytes = 1;
        ro.struct_size = static_cast<uint32_t>(offsetof(siaara_read_options, max_inflated_size));
        REQUIRE(siaara_file_open_memory(bytes->data(), bytes->size(), &ro, &file) == SIAARA_OK);
        CHECK(siaara_frame_decode(file, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) == SIAARA_OK);
        siaara_file_free(file);
    }
}

namespace {

// A single-frame grayscale image of rows x columns with `bits` allocated (1 or 8), all zeros, in
// the Deflated Explicit VR Little Endian transfer syntax: tens of megabytes of pixels in a file
// of a few tens of kilobytes.
std::vector<std::uint8_t> deflated_gray(std::uint16_t rows, std::uint16_t columns, std::uint16_t bits) {
    ImageSpec s;
    s.rows = 1;
    s.columns = 1;
    s.bits = 8;
    DicomFile f = image_file(s);
    f.transfer_syntax = kDeflated;
    Dataset& ds = f.dataset;
    (void)ds.set_uint16(tags::Rows, rows);
    (void)ds.set_uint16(tags::Columns, columns);
    (void)ds.set_uint16(tags::BitsAllocated, bits);
    (void)ds.set_uint16(tags::BitsStored, bits);
    (void)ds.set_uint16(tags::HighBit, static_cast<std::uint16_t>(bits - 1));
    const std::uint64_t pixels = std::uint64_t{rows} * columns;
    const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(bits == 1 ? (pixels + 7) / 8 : pixels), 0);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(zeros.data(), zeros.size()));
    return bytes_of(f);
}

constexpr std::size_t kMiB = std::size_t{1} << 20;

}  // namespace

TEST_CASE("rc2 R-191: a 62 kB attachment that states 23000 x 23000 1-bit pixels is refused before anything is decoded") {
    // The recommended limits, as a mail server would run them. The frame is 63 MiB packed (within
    // max_frame_bytes) and 529 million pixels: 2.1 GiB as RGBA.
    preview_limits lim;
    preview_default_limits(&lim);
    CHECK_EQ(lim.max_pixels, std::uint64_t{PREVIEW_DEFAULT_MAX_PIXELS});
    const std::vector<std::uint8_t> bytes = deflated_gray(23000, 23000, 1);
    REQUIRE(!bytes.empty());
    CHECK(bytes.size() < 200 * 1024);
    // What the limits allow for the read: up to twice max_inflated_size while the inflated data
    // set grows (R-195, a known limitation). Nothing of the picture: no decoded frame, no RGBA.
    const Run run = preview(bytes, lim, std::size_t{1} << 30);
    std::printf("  1-bit 23000 x 23000, %zu bytes: outcome %d, peak %zu MiB, %.2f s (%s)\n", bytes.size(), run.outcome,
                run.peak / kMiB, run.seconds, run.reason.c_str());
    CHECK_EQ(run.outcome, PREVIEW_TOO_LARGE);
    CHECK(!run.refused);
    CHECK(run.reason.find("pixels") != std::string::npos);
    CHECK(run.peak < 2 * lim.max_inflated_size);
}

TEST_CASE("rc2 R-191: a 60 kB attachment with an 8000 x 8000 8-bit frame never renders past the frame memory budget") {
    preview_limits lim;
    preview_default_limits(&lim);
    const std::vector<std::uint8_t> bytes = deflated_gray(8000, 8000, 8);
    REQUIRE(!bytes.empty());
    CHECK(bytes.size() < 200 * 1024);
    // 64,000,000 pixels: exactly at max_pixels, so the frame is decoded (61 MiB, within the
    // budget of 256 MiB); the render would need 244 MiB more as RGBA and is refused by the budget
    // of the file handle before it allocates anything.
    const Run run = preview(bytes, lim, std::size_t{1} << 30);
    std::printf("  8-bit 8000 x 8000, %zu bytes: outcome %d, peak %zu MiB, %.2f s (%s)\n", bytes.size(), run.outcome,
                run.peak / kMiB, run.seconds, run.reason.c_str());
    CHECK_EQ(run.outcome, PREVIEW_TOO_LARGE);
    CHECK(!run.refused);
    CHECK(run.reason.find("frame_memory_budget") != std::string::npos);
    // The read (2 x 61 MiB while inflating) or the data set plus the decoded frame, never the
    // RGBA on top: well inside frame_memory_budget.
    CHECK(run.peak < lim.frame_memory_budget);
    // One pixel fewer per row and the cap of max_pixels would not matter either; one more and it
    // refuses before the decode.
    lim.max_pixels = 63999999u;
    const Run early = preview(bytes, lim, std::size_t{1} << 30);
    CHECK_EQ(early.outcome, PREVIEW_TOO_LARGE);
    CHECK(early.reason.find("pixels") != std::string::npos);
}

TEST_CASE("rc2 R-191: siaara_frame_render is held to the frame memory budget of the file handle") {
    // 1-bit 8000 x 8000: 7.6 MiB decoded, 64 million pixels. With the pixel cap of the preview out
    // of the way the library itself must refuse the render: 244 MiB of RGBA against 32 MiB.
    preview_limits lim = test_limits();
    lim.max_pixels = UINT64_MAX;
    lim.max_total_pixels = UINT64_MAX;
    const std::vector<std::uint8_t> bytes = deflated_gray(8000, 8000, 1);
    REQUIRE(!bytes.empty());
    const Run run = preview(bytes, lim);
    std::printf("  1-bit 8000 x 8000, budget 32 MiB: outcome %d, peak %zu MiB, %.2f s (%s)\n", run.outcome, run.peak / kMiB,
                run.seconds, run.reason.c_str());
    CHECK_EQ(run.outcome, PREVIEW_TOO_LARGE);
    CHECK(!run.refused);
    CHECK(run.peak < lim.frame_memory_budget + 2 * lim.max_inflated_size);
    // The same through the C API, every output format and with a size query first; an image that
    // fits renders as before, and the default budget (4 GiB) leaves ordinary images alone.
    ImageSpec s;
    s.rows = 512;
    s.columns = 512;  // 512 KiB decoded, 1 MiB of RGBA
    const std::vector<std::uint8_t> small = bytes_of(image_file(s));
    for (const std::uint64_t budget : {std::uint64_t{0}, std::uint64_t{2} << 20, std::uint64_t{1} << 20}) {
        siaara_read_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.frame_memory_budget = budget;
        siaara_file* file = nullptr;
        REQUIRE(siaara_file_open_memory(small.data(), small.size(), &ro, &file) == SIAARA_OK);
        siaara_frame* frame = nullptr;
        REQUIRE(siaara_frame_open(file, 0, &frame) == SIAARA_OK);
        for (const int32_t format : {SIAARA_PIXELS_RGBA8, SIAARA_PIXELS_GRAY8}) {
            siaara_view2d_options vo;
            std::memset(&vo, 0, sizeof vo);
            vo.struct_size = sizeof vo;
            vo.viewport_width = 64;
            vo.viewport_height = 64;
            vo.output_format = format;
            vo.background_rgba[3] = 255;
            std::vector<std::uint8_t> out(64u * 64u * 4u);
            size_t need = 0;
            CHECK(siaara_frame_render(frame, &vo, nullptr, 0, &need) == SIAARA_OK);  // the size query allocates nothing
            siaara_status st = SIAARA_OK;
            bool refused = false;
            {
                siaara_test::AllocGuard guard(4 * kMiB);
                st = siaara_frame_render(frame, &vo, out.data(), out.size(), &need);
                refused = guard.refused();
            }
            CHECK(!refused);
            // 512 KiB held + 1 MiB RGBA + the 16 KiB value table (+ 256 KiB gray copy).
            if (budget == (std::uint64_t{1} << 20)) CHECK(st == SIAARA_ERR_LIMIT_EXCEEDED);
            else CHECK(st == SIAARA_OK);
        }
        siaara_frame_free(frame);
        siaara_file_free(file);
    }
}

TEST_CASE("rc2 R-192: the preview's pixel budget ends the strip; the limits reach the file handle") {
    ImageSpec s;
    s.rows = 256;
    s.columns = 256;
    s.frames = 6;
    const std::vector<std::uint8_t> bytes = bytes_of(image_file(s));
    preview_limits lim = test_limits();
    lim.max_strip_frames = 6;
    CHECK_EQ(preview(bytes, lim).strip_frames, 6u);
    // Room for the picture and three more frames.
    lim.max_total_pixels = 4u * 256u * 256u;
    const Run cut = preview(bytes, lim);
    CHECK_EQ(cut.outcome, PREVIEW_OK);
    CHECK_EQ(cut.strip_frames, 3u);
    // Not even the picture.
    lim.max_total_pixels = 256u * 256u - 1u;
    CHECK_EQ(preview(bytes, lim).outcome, PREVIEW_TOO_LARGE);
    // The defaults of the example are the documented ones.
    preview_default_limits(&lim);
    CHECK_EQ(lim.max_scans, 64u);
    CHECK_EQ(lim.max_crop_factor, 1u);
    CHECK_EQ(lim.max_total_pixels, std::uint64_t{128000000});
    CHECK_EQ(lim.max_value_bytes, std::size_t{65536});
    CHECK_EQ(lim.strip_control_and_format, 1);
}

TEST_CASE("rc2 R-193: a caption value of many megabytes is not decoded") {
    // Series Description as 24 MiB of text (UT, so the length fits).
    ImageSpec s;
    DicomFile f = image_file(s);
    const std::vector<std::uint8_t> text(24 * kMiB, 'A');
    (void)f.dataset.set_bytes(0x0008103E, VR::UT, Bytes::copy_of(text.data(), text.size()));
    const std::vector<std::uint8_t> bytes = bytes_of(f);
    REQUIRE(bytes.size() > 24 * kMiB);
    preview_limits lim;
    preview_default_limits(&lim);
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &file) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(file, &ds) == SIAARA_OK);
    char description[260] = "x", modality[20] = "";
    std::size_t peak = 0;
    bool refused = true;
    {
        siaara_test::AllocGuard guard(kMiB);
        preview_text(ds, 0x0008103Eu, &lim, description, sizeof description);
        preview_text(ds, 0x00080060u, &lim, modality, sizeof modality);
        peak = guard.peak();
        refused = guard.refused();
    }
    CHECK(!refused);
    CHECK(peak < 64 * 1024);
    CHECK(description[0] == '\0');
    CHECK(std::string(modality) == "OT");
    // With the cap out of the way the value is decoded (and does not fit the caption): the cap is
    // what saved the 24 MiB.
    lim.max_value_bytes = SIZE_MAX;
    {
        siaara_test::AllocGuard guard(std::size_t{1} << 30);
        preview_text(ds, 0x0008103Eu, &lim, description, sizeof description);
        peak = guard.peak();
    }
    CHECK(peak >= 24 * kMiB);
    CHECK(description[0] == '\0');
    siaara_file_free(file);
    // The whole preview of the same attachment, deflated to a few kB, stays bounded.
    f.transfer_syntax = kDeflated;
    const std::vector<std::uint8_t> small = bytes_of(f);
    REQUIRE(!small.empty());
    CHECK(small.size() < kMiB);
    preview_default_limits(&lim);
    preview_result r;
    CHECK_EQ(preview_attachment(small.data(), small.size(), &lim, &r), PREVIEW_OK);
    CHECK(r.series_description[0] == '\0');
    CHECK(std::string(r.modality) == "OT");
    preview_free(&r);
}

TEST_CASE("rc2 R-199: captions lose control and format characters; text is escaped for HTML") {
    // C0, DEL, C1 (U+0085), the line separator: a space each. RIGHT-TO-LEFT OVERRIDE (U+202E),
    // ZERO WIDTH SPACE (U+200B), the byte order mark (U+FEFF), a tag character (U+E0041): removed.
    // Bytes that are not UTF-8: '?'. Everything else stays, four-byte characters included.
    char text[] = "A\x01" "B\x7F" "C\xC2\x85" "D\xE2\x80\xA8" "E\xE2\x80\xAE" "gpj.exe\xE2\x80\x8B" "F\xEF\xBB\xBF"
                  "G\xF3\xA0\x81\x81" "H\xFF" "I\xC3" " \xC3\xA9\xE0\xAE\x85\xF0\x9F\x98\x80" "\xE0\x80\x80" "Z";
    preview_clean_text(text);
    CHECK(std::string(text) == "A B C D Egpj.exeFGH?I? \xC3\xA9\xE0\xAE\x85\xF0\x9F\x98\x80???Z");
    char empty[] = "";
    preview_clean_text(empty);
    CHECK(empty[0] == '\0');
    // Through the preview: the caption of a file whose Series Description carries an override.
    ImageSpec s;
    DicomFile f = image_file(s);
    (void)f.dataset.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    (void)f.dataset.set_string(0x0008103E, VR::LO, "scan\xE2\x80\xAE" "gpj.exe<b>");
    const std::vector<std::uint8_t> bytes = bytes_of(f);
    preview_limits lim = test_limits();
    preview_result r;
    REQUIRE(preview_attachment(bytes.data(), bytes.size(), &lim, &r) == PREVIEW_OK);
    CHECK(std::string(r.series_description) == "scangpj.exe<b>");
    char html[64] = {0};
    const size_t escaped = preview_html_escape(r.series_description, html, sizeof html);  // first: it fills html
    CHECK_EQ(escaped, std::strlen(html));
    CHECK(std::string(html) == "scangpj.exe&lt;b&gt;");
    preview_free(&r);
    // The option off: the text as the file has it (C0 controls still become spaces).
    lim.strip_control_and_format = 0;
    REQUIRE(preview_attachment(bytes.data(), bytes.size(), &lim, &r) == PREVIEW_OK);
    CHECK(std::string(r.series_description) == "scan\xE2\x80\xAE" "gpj.exe<b>");
    preview_free(&r);
    // Escaping: the five characters, and a cut that never splits a reference.
    CHECK_EQ(preview_html_escape("a&b<c>d\"e'f", html, sizeof html), std::size_t{30});
    CHECK(std::string(html) == "a&amp;b&lt;c&gt;d&quot;e&#39;f");
    char tiny[8];
    CHECK_EQ(preview_html_escape("ab&cd", tiny, sizeof tiny), std::size_t{7});
    CHECK(std::string(tiny) == "ab&amp;");
    CHECK_EQ(preview_html_escape("abc&d", tiny, sizeof tiny), std::size_t{3});
    CHECK(std::string(tiny) == "abc");
}
