// Regression tests for decoder resource limits found by the 2026-11-09 security review:
// R-010 (JPEG 2000 code-block metadata amplification), R-011 (unbounded progressive JPEG scans),
// R-027 (std::bad_alloc escaping the decoders), R-050 (output-buffer status, budgets, SIZ check
// order). Amplification tests run under an allocation guard (alloc_guard.hpp) and a time bound.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "alloc_guard.hpp"
#include "codec/codec.hpp"
#include "codec/deflate.hpp"
#include "codec/j2k.hpp"
#include "codec/jpeg.hpp"
#include "codec/jpegls.hpp"
#include "codec/transcode.hpp"
#include "core/dataset.hpp"
#include "core/types.hpp"
#include "core/writer.hpp"
#include "image/frames.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::codec;

namespace {

using Buf = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

FrameInfo gray(std::uint32_t rows, std::uint32_t cols, int alloc = 8, int stored = 8) {
    FrameInfo f;
    f.rows = rows;
    f.columns = cols;
    f.samples_per_pixel = 1;
    f.bits_allocated = static_cast<std::uint16_t>(alloc);
    f.bits_stored = static_cast<std::uint16_t>(stored);
    return f;
}

double seconds_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void be16(Buf& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void be32(Buf& b, std::uint32_t v) {
    be16(b, v >> 16);
    be16(b, v & 0xFFFFu);
}

// ------------------------------------------------------------------ JPEG 2000 (R-010)
// SOC, SIZ (w x h, one 8-bit component, one tile), COD with user precincts `pp` at every
// resolution after r = 0 (r = 0 gets `pp0`), QCD (no quantisation), one tile-part whose packet
// data is a single empty-packet byte, EOC.
Buf j2k_stream(std::uint32_t w, std::uint32_t h, int levels, std::uint8_t pp0, std::uint8_t pp) {
    Buf b;
    be16(b, 0xFF4F);
    be16(b, 0xFF51);
    be16(b, 41);
    be16(b, 0);
    be32(b, w);
    be32(b, h);
    be32(b, 0);
    be32(b, 0);
    be32(b, w);
    be32(b, h);
    be32(b, 0);
    be32(b, 0);
    be16(b, 1);
    b.push_back(7);  // 8-bit unsigned
    b.push_back(1);
    b.push_back(1);
    be16(b, 0xFF52);
    be16(b, static_cast<unsigned>(12 + levels + 1));
    b.push_back(0x01);  // Scod: user-defined precincts
    b.push_back(0x00);  // LRCP
    be16(b, 1);         // one layer
    b.push_back(0x00);  // no MCT
    b.push_back(static_cast<std::uint8_t>(levels));
    b.push_back(4);  // 64 x 64 nominal code-blocks
    b.push_back(4);
    b.push_back(0);
    b.push_back(1);  // 5-3
    b.push_back(pp0);
    for (int r = 1; r <= levels; ++r) b.push_back(pp);
    be16(b, 0xFF5C);
    be16(b, static_cast<unsigned>(3 + 3 * levels + 1));
    b.push_back(0x20);  // one guard bit, no quantisation
    for (int i = 0; i < 3 * levels + 1; ++i) b.push_back(static_cast<std::uint8_t>(8 << 3));
    be16(b, 0xFF90);
    be16(b, 10);
    be16(b, 0);
    be32(b, 0);  // Psot 0: to EOC
    b.push_back(0);
    b.push_back(1);
    be16(b, 0xFF93);
    b.push_back(0x00);  // empty packet
    be16(b, 0xFFD9);
    return b;
}

// ------------------------------------------------------------------ progressive JPEG (R-011)
class BitWriter {
public:
    explicit BitWriter(Buf& out) : out_(out) {}
    void put(std::uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            acc_ = static_cast<std::uint32_t>((acc_ << 1) | ((v >> i) & 1u));
            if (++bits_ == 8) emit();
        }
    }
    void flush() {
        while (bits_ != 0) put(1, 1);
    }

private:
    void emit() {
        const std::uint8_t byte = static_cast<std::uint8_t>(acc_);
        out_.push_back(byte);
        if (byte == 0xFF) out_.push_back(0x00);
        acc_ = 0;
        bits_ = 0;
    }
    Buf& out_;
    std::uint32_t acc_ = 0;
    int bits_ = 0;
};

// Gray progressive JPEG with all-zero coefficients. The DC table holds one 1-bit code for
// "difference 0"; the AC table one 1-bit code for EOB14 (an EOB run of 16384 blocks + 14 bits).
// Both tables are 1-bit codes '0'. `w * h` must be a multiple of 64 * 16384.
struct ProgJpeg {
    Buf b;
    std::uint32_t blocks = 0;
    ProgJpeg(std::uint32_t w, std::uint32_t h) {
        blocks = (w / 8) * (h / 8);
        be16(b, 0xFFD8);
        be16(b, 0xFFDB);
        be16(b, 67);
        b.push_back(0x00);
        for (int i = 0; i < 64; ++i) b.push_back(1);
        be16(b, 0xFFC2);
        be16(b, 11);
        b.push_back(8);
        be16(b, h);
        be16(b, w);
        b.push_back(1);
        b.push_back(1);
        b.push_back(0x11);
        b.push_back(0);
        be16(b, 0xFFC4);
        be16(b, 2 + 17 + 1 + 17 + 1);
        b.push_back(0x00);
        b.push_back(1);
        for (int i = 2; i <= 16; ++i) b.push_back(0);
        b.push_back(0x00);
        b.push_back(0x10);
        b.push_back(1);
        for (int i = 2; i <= 16; ++i) b.push_back(0);
        b.push_back(0xE0);
    }
    void sos(int ss, int se, int ah, int al) {
        be16(b, 0xFFDA);
        be16(b, 8);
        b.push_back(1);
        b.push_back(1);
        b.push_back(0x00);
        b.push_back(static_cast<std::uint8_t>(ss));
        b.push_back(static_cast<std::uint8_t>(se));
        b.push_back(static_cast<std::uint8_t>((ah << 4) | al));
    }
    // DC first or DC refinement: one 0 bit per block either way.
    void dc(int ah, int al) {
        sos(0, 0, ah, al);
        BitWriter bw(b);
        for (std::uint32_t i = 0; i < blocks; ++i) bw.put(0, 1);
        bw.flush();
    }
    // AC first or refinement of [ss, se] for all blocks: EOB14 runs only.
    void ac(int ss, int se, int ah, int al) {
        sos(ss, se, ah, al);
        BitWriter bw(b);
        for (std::uint32_t done = 0; done < blocks; done += 16384) {
            bw.put(0, 1);
            bw.put(0, 14);
        }
        bw.flush();
    }
    void eoi() { be16(b, 0xFFD9); }
};

}  // namespace

// R-010: a ~105-byte legal codestream asking for 2 x 2 precincts (hence 1 x 1 code-blocks) on a
// 2048 x 2048 image used ~2.8 GB of code-block and tag-tree metadata. It must now be refused
// before the metadata is allocated, well within a 256 MB allocation bound.
TEST_CASE("codec limits/R-010 J2K tiny precincts cannot amplify code-block metadata") {
    const Buf cs = j2k_stream(2048, 2048, 5, 0x00, 0x11);
    std::printf("  J2K codestream: %zu bytes\n", cs.size());
    const FrameInfo fi = gray(2048, 2048);
    Buf out(static_cast<std::size_t>(fi.frame_bytes()));
    Status st;
    bool escaped = false;
    const auto t0 = Clock::now();
    std::size_t peak = 0;
    {
        siaara_test::AllocGuard guard(256u << 20);
        try {
            st = decode_j2k(cs, fi, out);
        } catch (const std::bad_alloc&) {
            escaped = true;
        }
        peak = guard.peak();
    }
    const double took = seconds_since(t0);
    std::printf("  decode: %s, %s, peak %.1f MB, %.2f s\n", escaped ? "bad_alloc escaped" : "returned",
                st.message.c_str(), static_cast<double>(peak) / 1048576.0, took);
    CHECK(!escaped);
    CHECK_EQ(st.code, Errc::LimitExceeded);
    CHECK(peak < (64u << 20));
    CHECK(took < 5.0);
    // decode_components takes the same path.
    std::vector<j2k::Component> comps;
    escaped = false;
    {
        siaara_test::AllocGuard guard(256u << 20);
        try {
            st = j2k::decode_components(cs, comps);
        } catch (const std::bad_alloc&) {
            escaped = true;
        }
    }
    CHECK(!escaped);
    CHECK_EQ(st.code, Errc::LimitExceeded);
}

TEST_CASE("codec limits/R-010 J2K ordinary precincts on the same image still decode within bounds") {
    // Default precincts (Scod bit 0 clear in effect: 2^15) on the same empty 2048 x 2048 image.
    Buf cs = j2k_stream(2048, 2048, 5, 0xFF, 0xFF);
    const FrameInfo fi = gray(2048, 2048);
    Buf out(static_cast<std::size_t>(fi.frame_bytes()), 0xAA);
    Status st;
    std::size_t peak = 0;
    {
        siaara_test::AllocGuard guard(256u << 20);
        st = decode_j2k(cs, fi, out);
        peak = guard.peak();
    }
    std::printf("  decode: %s, peak %.1f MB\n", st.is_ok() ? "ok" : st.message.c_str(), static_cast<double>(peak) / 1048576.0);
    CHECK(st.is_ok());
    // All coefficients zero: the 5-3 inverse of zero plus the level shift of 128.
    CHECK(out.front() == 128 && out.back() == 128);
    // Small precincts that are legal and not abusive (16 x 16, 8 x 8 code-blocks) still decode.
    cs = j2k_stream(256, 256, 3, 0x44, 0x44);
    const FrameInfo small = gray(256, 256);
    Buf o2(static_cast<std::size_t>(small.frame_bytes()));
    CHECK(decode_j2k(cs, small, o2).is_ok());
}

// R-011: every all-EOB progressive scan walks every block. Repeating the same first-pass band
// thousands of times must be refused (T.81 G.1.1.1), quickly.
TEST_CASE("codec limits/R-011 progressive JPEG repeated first-pass scans are refused quickly") {
    ProgJpeg j(4096, 4096);
    j.dc(0, 0);
    for (int i = 0; i < 3000; ++i) j.ac(1, 63, 0, 0);
    j.eoi();
    std::printf("  stream: %zu bytes, %u blocks, 3001 scans\n", j.b.size(), j.blocks);
    const FrameInfo fi = gray(4096, 4096);
    Buf out(static_cast<std::size_t>(fi.frame_bytes()));
    const auto t0 = Clock::now();
    const Status st = decode_jpeg(j.b, fi, out);
    const double took = seconds_since(t0);
    std::printf("  decode: %s (%.2f s)\n", st.is_ok() ? "ok" : st.message.c_str(), took);
    CHECK(!st.is_ok());
    CHECK_EQ(st.code, Errc::Malformed);
    // The refusal is by a work limit, not by the clock; the time bound only guards against a
    // regression to unbounded work. Sanitizer builds on shared CI runners are several times slower.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    const double limit_s = 15.0;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    const double limit_s = 15.0;
#else
    const double limit_s = 1.5;
#endif
#else
    const double limit_s = 1.5;
#endif
    CHECK(took < limit_s);
}

TEST_CASE("codec limits/R-011 progressive JPEG overlap and successive-approximation rules") {
    const FrameInfo fi = gray(1024, 1024);
    Buf out(static_cast<std::size_t>(fi.frame_bytes()));
    // Valid: DC first, AC split in two bands with three successive-approximation steps each.
    {
        ProgJpeg j(1024, 1024);
        j.dc(0, 1);
        j.ac(1, 5, 0, 2);
        j.ac(6, 63, 0, 2);
        j.ac(1, 5, 2, 1);
        j.ac(6, 63, 2, 1);
        j.dc(1, 0);
        j.ac(1, 5, 1, 0);
        j.ac(6, 63, 1, 0);
        j.eoi();
        const Status st = decode_jpeg(j.b, fi, out);
        CHECK(st.is_ok());
    }
    // Overlapping first passes (6..63 then 1..10).
    {
        ProgJpeg j(1024, 1024);
        j.dc(0, 0);
        j.ac(6, 63, 0, 0);
        j.ac(1, 10, 0, 0);
        j.eoi();
        CHECK_EQ(decode_jpeg(j.b, fi, out).code, Errc::Malformed);
    }
    // Repeated DC first pass.
    {
        ProgJpeg j(1024, 1024);
        j.dc(0, 0);
        j.dc(0, 0);
        j.ac(1, 63, 0, 0);
        j.eoi();
        CHECK_EQ(decode_jpeg(j.b, fi, out).code, Errc::Malformed);
    }
    // Refinement whose Ah is not the previous Al (first pass Al = 2, refinement Ah = 1).
    {
        ProgJpeg j(1024, 1024);
        j.dc(0, 0);
        j.ac(1, 63, 0, 2);
        j.ac(1, 63, 1, 0);
        j.eoi();
        CHECK_EQ(decode_jpeg(j.b, fi, out).code, Errc::Malformed);
    }
    // Refinement of a band that never had a first pass.
    {
        ProgJpeg j(1024, 1024);
        j.dc(0, 0);
        j.ac(1, 63, 1, 0);
        j.eoi();
        CHECK_EQ(decode_jpeg(j.b, fi, out).code, Errc::Malformed);
    }
    // The longest legal progression of one component: DC and each of the 63 AC coefficients
    // first-coded with Al = 13 and refined down to 0 (14 scans each, 896 in all) still decodes.
    {
        ProgJpeg j(1024, 1024);
        j.dc(0, 13);
        for (int k = 1; k <= 63; ++k) j.ac(k, k, 0, 13);
        for (int al = 12; al >= 0; --al) {
            j.dc(al + 1, al);
            for (int k = 1; k <= 63; ++k) j.ac(k, k, al + 1, al);
        }
        j.eoi();
        const Status st = decode_jpeg(j.b, fi, out);
        if (!st) std::printf("  896 scans: %s\n", st.message.c_str());
        CHECK(st.is_ok());
    }
}

// R-027: no decoder lets std::bad_alloc escape; each reports Errc::OutOfMemory instead.
TEST_CASE("codec limits/R-027 decoders report OutOfMemory instead of throwing") {
    const FrameInfo fi = gray(512, 512, 16, 12);
    Buf img(static_cast<std::size_t>(fi.frame_bytes()));
    for (std::size_t i = 0; i < img.size(); i += 2) {
        const unsigned v = static_cast<unsigned>((i * 7) % 4096);
        img[i] = static_cast<std::uint8_t>(v);
        img[i + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    EncodeParams p;
    Buf jpg, jls, j2c;
    REQUIRE(encode_jpeg(img, fi, p, jpg).is_ok());
    REQUIRE(encode_jpegls(img, fi, p, jls).is_ok());
    REQUIRE(encode_j2k(img, fi, p, j2c).is_ok());
    struct Codec {
        const char* name;
        const Buf* in;
        Status (*fn)(std::span<const std::uint8_t>, const FrameInfo&, std::span<std::uint8_t>);
    };
    const Codec codecs[] = {{"jpeg", &jpg, &decode_jpeg}, {"jpegls", &jls, &decode_jpegls}, {"j2k", &j2c, &decode_j2k}};
    Buf out(static_cast<std::size_t>(fi.frame_bytes()));
    for (const Codec& c : codecs) {
        int escaped = 0, oom = 0;
        // (A limit of 0 would also refuse the few bytes of the returned error message.)
        for (std::size_t limit : {std::size_t{1} << 10, std::size_t{4} << 10, std::size_t{16} << 10, std::size_t{100} << 10,
                                  std::size_t{300} << 10, std::size_t{700} << 10}) {
            Status st;
            // Flushed, so a crash in CI names the codec and limit (stdout is buffered there).
            std::printf("  %s: limit %zu\n", c.name, limit);
            std::fflush(stdout);
            try {
                siaara_test::AllocGuard guard(limit);
                st = c.fn(*c.in, fi, out);
            } catch (...) {  // bad_alloc or anything else the standard library might throw
                ++escaped;
                continue;
            }
            if (st.code == Errc::OutOfMemory) ++oom;
            else CHECK(st.is_ok());
        }
        std::printf("  %s: %d of 6 limits let bad_alloc escape, %d reported OutOfMemory\n", c.name, escaped, oom);
        CHECK_EQ(escaped, 0);
        CHECK(oom > 0);
        CHECK(c.fn(*c.in, fi, out).is_ok());
    }
}

// R-050: an output buffer larger than the frame is an argument error (as in RLE), not
// BufferTooSmall; a smaller one stays BufferTooSmall.
TEST_CASE("codec limits/R-050 oversized output buffers are InvalidArgument") {
    const FrameInfo fi = gray(64, 64, 16, 12);
    Buf img(static_cast<std::size_t>(fi.frame_bytes()), 0);
    EncodeParams p;
    Buf jpg, jls, j2c, rle;
    REQUIRE(encode_jpeg(img, fi, p, jpg).is_ok());
    REQUIRE(encode_jpegls(img, fi, p, jls).is_ok());
    REQUIRE(encode_j2k(img, fi, p, j2c).is_ok());
    REQUIRE(encode_rle(img, fi, p, rle).is_ok());
    Buf big(static_cast<std::size_t>(fi.frame_bytes()) + 2), small(static_cast<std::size_t>(fi.frame_bytes()) - 2);
    CHECK_EQ(decode_rle(rle, fi, big).code, Errc::InvalidArgument);
    CHECK_EQ(decode_jpeg(jpg, fi, big).code, Errc::InvalidArgument);
    CHECK_EQ(decode_jpegls(jls, fi, big).code, Errc::InvalidArgument);
    CHECK_EQ(decode_j2k(j2c, fi, big).code, Errc::InvalidArgument);
    CHECK_EQ(decode_jpeg(jpg, fi, small).code, Errc::BufferTooSmall);
    CHECK_EQ(decode_jpegls(jls, fi, small).code, Errc::BufferTooSmall);
    CHECK_EQ(decode_j2k(j2c, fi, small).code, Errc::BufferTooSmall);
}

// R-050: the SIZ-versus-attributes check happens right after SIZ, before the rest of the
// codestream is parsed (here: a main header that would fail to parse later).
TEST_CASE("codec limits/R-050 J2K SIZ disagreement is reported before the rest is parsed") {
    Buf cs = j2k_stream(256, 256, 3, 0x44, 0x44);
    // Corrupt the COD marker that follows SIZ (offset 2 + 2 + 41 = 45).
    REQUIRE(cs[45] == 0xFF && cs[46] == 0x52);
    cs[46] = 0x00;  // not a marker any more
    Buf out(128 * 256);
    const Status st = decode_j2k(cs, gray(128, 256), out);
    std::printf("  %s\n", st.message.c_str());
    CHECK_EQ(st.code, Errc::Malformed);
    CHECK(st.message.find("Rows/Columns") != std::string::npos);
}

// R-011 / R-050: the caller-settable limits of FrameInfo.
TEST_CASE("codec limits/R-011 R-050 FrameInfo max_scans and memory_budget") {
    FrameInfo fi = gray(1024, 1024);
    CHECK_EQ(fi.effective_max_scans(), 1000u);
    CHECK(fi.effective_memory_budget() == (std::uint64_t{4} << 30));
    Buf out(static_cast<std::size_t>(fi.frame_bytes()));
    ProgJpeg j(1024, 1024);
    j.dc(0, 2);
    for (int al = 2; al >= 0; --al) {
        if (al < 2) j.dc(al + 1, al);
        for (int k = 1; k <= 63; k += 9) j.ac(k, k + 8, al == 2 ? 0 : al + 1, al);
    }
    j.eoi();  // 3 DC + 21 AC = 24 scans
    CHECK(decode_jpeg(j.b, fi, out).is_ok());
    fi.max_scans = 24;
    CHECK(decode_jpeg(j.b, fi, out).is_ok());
    fi.max_scans = 23;
    CHECK_EQ(decode_jpeg(j.b, fi, out).code, Errc::LimitExceeded);
    // Progressive planes + coefficients of 1024 x 1024 need 4 MB.
    fi.max_scans = 0;
    fi.memory_budget = 3u << 20;
    CHECK_EQ(decode_jpeg(j.b, fi, out).code, Errc::LimitExceeded);
    fi.memory_budget = 5u << 20;
    CHECK(decode_jpeg(j.b, fi, out).is_ok());
    // JPEG 2000: the 256 x 256 tile buffer alone is 256 KB.
    const Buf cs = j2k_stream(256, 256, 3, 0x44, 0x44);
    FrameInfo f2 = gray(256, 256);
    Buf o2(static_cast<std::size_t>(f2.frame_bytes()));
    f2.memory_budget = 128u << 10;
    CHECK_EQ(decode_j2k(cs, f2, o2).code, Errc::LimitExceeded);
    f2.memory_budget = 0;
    CHECK(decode_j2k(cs, f2, o2).is_ok());
}

// R-027 (defensive): a codec entry point may be noexcept only if it never allocates. A noexcept
// function that allocates turns std::bad_alloc into std::terminate; the allocating entries instead
// catch and report Errc::OutOfMemory. Every allocating entry is checked not to be noexcept here
// (at compile time), and the noexcept ones are run under an allocation guard of 0 bytes.
namespace {
template <class F> struct fn_is_noexcept : std::false_type {};
template <class R, class... A> struct fn_is_noexcept<R(A...) noexcept> : std::true_type {};
template <class F> constexpr bool noexcept_fn(F*) { return fn_is_noexcept<F>::value; }
using DecodeFn = Status(std::span<const std::uint8_t>, const FrameInfo&, std::span<std::uint8_t>);
using EncodeFn = Status(std::span<const std::uint8_t>, const FrameInfo&, const EncodeParams&, std::vector<std::uint8_t>&);
static_assert(!noexcept_fn<DecodeFn>(&decode_rle) && !noexcept_fn<DecodeFn>(&decode_jpeg) &&
              !noexcept_fn<DecodeFn>(&decode_jpegls) && !noexcept_fn<DecodeFn>(&decode_j2k));
static_assert(!noexcept_fn<EncodeFn>(&encode_rle) && !noexcept_fn<EncodeFn>(&encode_jpeg) &&
              !noexcept_fn<EncodeFn>(&encode_jpegls) && !noexcept_fn<EncodeFn>(&encode_j2k));
static_assert(!noexcept_fn(&decode_rle_lenient) && !noexcept_fn(&encode_jpeg_ex) && !noexcept_fn(&jpeg_read_header));
static_assert(!noexcept_fn<Status(std::span<const std::uint8_t>, const FrameInfo&, std::span<std::uint8_t>,
                                   const JpegDecodeOptions&, std::uint64_t&)>(&decode_jpeg_ex));
static_assert(!noexcept_fn<Status(std::span<const std::uint8_t>, const FrameInfo&, const JpegDecodeOptions&,
                                   std::span<std::uint8_t>)>(&decode_jpeg_ex));
static_assert(!noexcept_fn(&jpegls::encode) && !noexcept_fn(&jpegls::probe));
static_assert(!noexcept_fn(&j2k::encode) && !noexcept_fn(&j2k::probe) && !noexcept_fn(&j2k::decode_components));
static_assert(!noexcept_fn(&inflate_raw) && !noexcept_fn(&deflate_raw));
static_assert(!noexcept_fn(&transcode_dataset) && !noexcept_fn(&transcode_file));
// The noexcept entries, exercised below.
static_assert(noexcept_fn(&can_encode) && noexcept_fn(&can_decode) && noexcept_fn(&encoding_is_lossy));
}  // namespace

TEST_CASE("codec limits/R-027 noexcept codec entry points never allocate") {
    const char* uids[] = {"1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.70",
                          "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.90",
                          "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.5", "1.2.3.4.unknown", ""};
    TranscodeParams p;  // constructed before the guard: only the calls themselves are measured
    int encodable = 0, decodable = 0, lossy = 0;
    bool refused = false;
    {
        siaara_test::AllocGuard guard(0);
        for (const char* u : uids) {
            encodable += can_encode(u) ? 1 : 0;
            decodable += can_decode(u) ? 1 : 0;
            lossy += encoding_is_lossy(u, p) ? 1 : 0;
        }
        refused = guard.refused() || guard.peak() != 0;
    }
    CHECK(!refused);
    CHECK(encodable > 0);
    CHECK(decodable > 0);
    CHECK(lossy > 0);
}

// R-062 / R-027 (2026-11-10 review): encoders and transcoding report OutOfMemory instead of
// throwing, and refuse frames whose working memory cannot fit the budget before allocating.
TEST_CASE("codec limits/R-062 encoders and transcode report OutOfMemory instead of throwing") {
    const FrameInfo fi = gray(256, 256, 16, 12);
    Buf img(static_cast<std::size_t>(fi.frame_bytes()));
    for (std::size_t i = 0; i < img.size(); i += 2) {
        const unsigned v = static_cast<unsigned>((i * 7) % 4096);
        img[i] = static_cast<std::uint8_t>(v);
        img[i + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    struct Enc {
        const char* name;
        Status (*fn)(std::span<const std::uint8_t>, const FrameInfo&, const EncodeParams&, std::vector<std::uint8_t>&);
    };
    const Enc encs[] = {{"jpeg", &encode_jpeg}, {"jpegls", &encode_jpegls}, {"j2k", &encode_j2k}};
    EncodeParams p;
    for (const Enc& e : encs) {
        int escaped = 0, oom = 0;
        for (std::size_t limit : {std::size_t{1} << 10, std::size_t{16} << 10, std::size_t{100} << 10, std::size_t{400} << 10}) {
            std::printf("  %s encoder: limit %zu\n", e.name, limit);
            std::fflush(stdout);
            Status st;
            Buf out;
            try {
                siaara_test::AllocGuard guard(limit);
                st = e.fn(img, fi, p, out);
            } catch (...) {
                ++escaped;
                continue;
            }
            if (st.code == Errc::OutOfMemory) ++oom;
            else CHECK(st.is_ok());
        }
        CHECK_EQ(escaped, 0);
        CHECK(oom > 0);
        Buf out;
        CHECK(e.fn(img, fi, p, out).is_ok());
        // The budget is checked before the working memory is allocated.
        FrameInfo tight = fi;
        tight.memory_budget = 1024;
        CHECK_EQ(e.fn(img, tight, p, out).code, Errc::LimitExceeded);
    }
    // Transcoding a whole data set.
    Dataset ds;
    (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(siaara::tags::SOPInstanceUID, VR::UI, "2.25.62");
    (void)ds.set_uint16(siaara::tags::Rows, 256);
    (void)ds.set_uint16(siaara::tags::Columns, 256);
    (void)ds.set_uint16(siaara::tags::SamplesPerPixel, 1);
    (void)ds.set_string(siaara::tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(siaara::tags::BitsAllocated, 16);
    (void)ds.set_uint16(siaara::tags::BitsStored, 12);
    (void)ds.set_uint16(siaara::tags::HighBit, 11);
    (void)ds.set_uint16(siaara::tags::PixelRepresentation, 0);
    (void)ds.set_bytes(siaara::tags::PixelData, VR::OW, Bytes::copy_of(img.data(), img.size()));
    TranscodeParams tp;
    int escaped = 0, oom = 0;
    for (std::size_t limit : {std::size_t{1} << 10, std::size_t{64} << 10, std::size_t{300} << 10}) {
        try {
            siaara_test::AllocGuard guard(limit);
            auto r = transcode_dataset(ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.90", tp);
            if (!r && r.error().code == Errc::OutOfMemory) ++oom;
        } catch (...) {
            ++escaped;
        }
    }
    CHECK_EQ(escaped, 0);
    CHECK(oom > 0);
    CHECK(transcode_dataset(ds, "1.2.840.10008.1.2.1", "1.2.840.10008.1.2.4.90", tp).is_ok());
}

// R-065: NaN / infinite JPEG 2000 quality or rate are argument errors.
TEST_CASE("codec limits/R-065 J2K quality and rate must be finite") {
    const FrameInfo fi = gray(32, 32, 8, 8);
    Buf img(static_cast<std::size_t>(fi.frame_bytes()), 100);
    for (double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        j2k::EncodeOptions q;
        q.lossless = false;
        q.quality = bad;
        Buf out;
        CHECK_EQ(j2k::encode(img, fi, q, out).code, Errc::InvalidArgument);
        j2k::EncodeOptions r;
        r.lossless = false;
        r.rate = bad;
        CHECK_EQ(j2k::encode(img, fi, r, out).code, Errc::InvalidArgument);
    }
    j2k::EncodeOptions neg;
    neg.lossless = false;
    neg.rate = -1;
    Buf out;
    CHECK_EQ(j2k::encode(img, fi, neg, out).code, Errc::InvalidArgument);
    j2k::EncodeOptions ok;
    ok.lossless = false;
    ok.rate = 1.5;
    CHECK(j2k::encode(img, fi, ok, out).is_ok());
}

// Round 6, R-192 / R-201: the scan cap and the crop margin are options of the file handle in the
// C API (siaara_read_options.max_scans / max_crop_factor); 0 keeps the library defaults.
namespace {
Buf part10_with_codestream(std::uint32_t rows, std::uint32_t cols, const char* transfer_syntax, const Buf& codestream) {
    DicomFile f;
    f.transfer_syntax = transfer_syntax;
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.192001");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, static_cast<std::uint16_t>(rows));
    (void)ds.set_uint16(tags::Columns, static_cast<std::uint16_t>(cols));
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    Element e;
    e.tag = tags::PixelData;
    e.vr = VR::OB;
    e.undefined_length = true;
    e.fragments.push_back(Bytes());
    Buf g = codestream;
    if (g.size() & 1u) g.push_back(0);
    e.fragments.push_back(Bytes::copy_of(g.data(), g.size()));
    ds.put(std::move(e));
    Buf out;
    (void)write_part10(f, out);
    return out;
}

siaara_status decode_with(const Buf& file_bytes, std::uint32_t max_scans, std::uint32_t max_crop_factor, std::size_t frame_bytes,
                          bool old_struct = false) {
    siaara_read_options ro;
    std::memset(&ro, 0, sizeof ro);
    ro.struct_size = old_struct ? static_cast<uint32_t>(offsetof(siaara_read_options, max_scans)) : sizeof ro;
    ro.allow_raw_dataset = 1;
    ro.max_scans = max_scans;
    ro.max_crop_factor = max_crop_factor;
    siaara_file* file = nullptr;
    if (siaara_file_open_memory(file_bytes.data(), file_bytes.size(), &ro, &file) != SIAARA_OK) return SIAARA_ERR_INTERNAL;
    Buf out(frame_bytes);
    size_t need = 0;
    const siaara_status st = siaara_frame_decode(file, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need);
    // The limits belong to the handle: a frame handle opened from it is held to them too.
    siaara_frame* frame = nullptr;
    const siaara_status fo = siaara_frame_open(file, 0, &frame);
    siaara_frame_free(frame);
    siaara_file_free(file);
    return st == fo ? st : SIAARA_ERR_INTERNAL;
}
}  // namespace

TEST_CASE("codec limits/R-192 siaara_read_options.max_scans caps the JPEG scans of the handle's frames") {
    static_assert(image::FrameOptions{}.max_scans == 0);
    static_assert(FrameInfo::kDefaultMaxScans == 1000);
    ProgJpeg j(64, 64);
    j.dc(0, 2);
    for (int al = 2; al >= 0; --al) {
        if (al < 2) j.dc(al + 1, al);
        for (int k = 1; k <= 63; k += 9) j.ac(k, k + 8, al == 2 ? 0 : al + 1, al);
    }
    j.eoi();  // 3 DC + 21 AC = 24 scans
    const Buf file = part10_with_codestream(64, 64, "1.2.840.10008.1.2.4.51", j.b);
    REQUIRE(!file.empty());
    CHECK(decode_with(file, 0, 0, 64 * 64) == SIAARA_OK);   // the library default (1000)
    CHECK(decode_with(file, 64, 0, 64 * 64) == SIAARA_OK);  // what the attachment preview uses
    CHECK(decode_with(file, 24, 0, 64 * 64) == SIAARA_OK);
    CHECK(decode_with(file, 23, 0, 64 * 64) == SIAARA_ERR_LIMIT_EXCEEDED);
    CHECK(decode_with(file, 1, 0, 64 * 64) == SIAARA_ERR_LIMIT_EXCEEDED);
    // An older caller's struct (without the fields): the values in it are not read.
    CHECK(decode_with(file, 1, 0, 64 * 64, true) == SIAARA_OK);
}

TEST_CASE("codec limits/R-201 siaara_read_options.max_crop_factor: 1 refuses a codestream larger than the image") {
    static_assert(image::FrameOptions::kDefaultMaxCropFactor == 2);
    static_assert(image::FrameOptions{}.max_crop_factor == image::FrameOptions::kDefaultMaxCropFactor);
    const FrameInfo coded = gray(30, 32);
    Buf img(static_cast<std::size_t>(coded.frame_bytes()), 9);
    EncodeParams p;
    Buf jls;
    REQUIRE(encode_jpegls(img, coded, p, jls).is_ok());
    // The data set states 16 x 16; the codestream is 30 x 32.
    const Buf larger = part10_with_codestream(16, 16, "1.2.840.10008.1.2.4.80", jls);
    CHECK(decode_with(larger, 0, 0, 16 * 16) == SIAARA_OK);  // the default margin of 2: decoded and cropped
    CHECK(decode_with(larger, 0, 2, 16 * 16) == SIAARA_OK);
    CHECK(decode_with(larger, 0, 1, 16 * 16) == SIAARA_ERR_MALFORMED);
    CHECK(decode_with(larger, 0, 1, 16 * 16, true) == SIAARA_OK);
    // A codestream of the stated size is fine with 1.
    const Buf exact = part10_with_codestream(30, 32, "1.2.840.10008.1.2.4.80", jls);
    CHECK(decode_with(exact, 0, 1, 30 * 32) == SIAARA_OK);
}
