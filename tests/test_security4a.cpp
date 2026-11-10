// Regression tests for the security review of 2026-11-11, core / named data / validator findings:
// R-108, R-114 (length recovery heuristic), R-154 (trailing padding), R-111 (one-pass value
// access), R-112 (validator value split), R-123 (named data buffers), R-137 (waveform bits),
// R-138 (non-finite DS), R-140 (line profile), R-157 (path hint). The allocation-guard tests of
// R-123 are in test_security4a_alloc.cpp. Every input is synthetic.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "charset/charset.hpp"
#include "clinical/measure.hpp"
#include "core/dataset.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "modules/data.hpp"
#include "modules/modules.hpp"
#include "repair/validate.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void u16(std::vector<std::uint8_t>& b, std::uint32_t x) {
    b.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}
void u32(std::vector<std::uint8_t>& b, std::uint32_t x) {
    u16(b, x & 0xFFFFu);
    u16(b, x >> 16);
}
// Implicit VR little endian element; `stated` overrides the length written.
void imp(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, std::string_view v,
         std::uint32_t stated = 0xFFFFFFFFu) {
    u16(b, g);
    u16(b, e);
    u32(b, stated == 0xFFFFFFFFu ? static_cast<std::uint32_t>(v.size()) : stated);
    b.insert(b.end(), v.begin(), v.end());
}
// Explicit VR little endian element with a 16-bit length (short VRs).
void exp16(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, const char vr[2], std::string_view v,
           std::uint32_t stated = 0xFFFFFFFFu) {
    u16(b, g);
    u16(b, e);
    b.push_back(static_cast<std::uint8_t>(vr[0]));
    b.push_back(static_cast<std::uint8_t>(vr[1]));
    u16(b, stated == 0xFFFFFFFFu ? static_cast<std::uint32_t>(v.size()) : stated);
    b.insert(b.end(), v.begin(), v.end());
}

std::string le16s(std::uint16_t x) { return std::string{static_cast<char>(x & 0xFF), static_cast<char>(x >> 8)}; }

Result<DicomFile> read_bytes(const std::vector<std::uint8_t>& b, const ReadOptions& opt = {}) {
    return read_part10(Bytes::copy_of(b.data(), b.size()), opt);
}

int count_warnings(const DicomFile& f, std::string_view code) {
    int n = 0;
    for (const std::string& w : f.warnings) n += w.find(code) != std::string::npos ? 1 : 0;
    return n;
}

// A raw implicit VR little endian data set in which (0008,103E) states 13 bytes for 10, as GE
// Genesis files do.
std::vector<std::uint8_t> ge_like() {
    std::vector<std::uint8_t> b;
    imp(b, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(b, 0x0008, 0x0018, "2.25.41080");
    imp(b, 0x0008, 0x0060, "MR");
    imp(b, 0x0008, 0x103E, "AX T1 POST", 13);
    imp(b, 0x0008, 0x1060, "JANETOS ");
    imp(b, 0x0008, 0x1090, "GENESIS_SIGNA ");
    imp(b, 0x0010, 0x0010, "TEN^B ");
    imp(b, 0x0010, 0x0020, "ID4A");
    imp(b, 0x0028, 0x0010, le16s(2));
    imp(b, 0x0028, 0x0011, le16s(2));
    imp(b, 0x7FE0, 0x0010, std::string("\x01\x00\x02\x00\x03\x00\x04\x00", 8));
    return b;
}

// R-108, the reviewer's scenario. A valid Part 10 file (explicit VR file meta, implicit VR little
// endian body) built so that the old recovery, run on the file meta group with the meta's
// explicit VR syntax, cut the last meta element short, read a fake (0002,0016) whose value
// swallowed the start of the body, and so started the body 10 bytes late - inside the value of
// the first body element, where a second, different data set is hidden.
std::vector<std::uint8_t> meta_boundary_file() {
    // What a standard reader sees after the first body element.
    std::vector<std::uint8_t> real;
    imp(real, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(real, 0x0008, 0x0018, "1.2.3.4 ");
    imp(real, 0x0010, 0x0010, "GOOD^PATIENT");
    imp(real, 0x0010, 0x0020, "GOOD-ID ");
    // The hidden data set, as the old lenient reader saw the body.
    std::vector<std::uint8_t> alt;
    // Length 0x4F4C: its low bytes read as the explicit VR "LO", its high bytes as a 16-bit
    // length of 0, so the old probe took this for a second explicit VR header.
    imp(alt, 0x0008, 0x0008, std::string(0x4F4C, ' '));
    imp(alt, 0x0008, 0x0018, "6.6.6.66");
    imp(alt, 0x0010, 0x0010, "EVIL^PATIENT");
    imp(alt, 0x0011, 0x0010, std::string(), static_cast<std::uint32_t>(real.size()));  // swallows the real elements

    std::vector<std::uint8_t> body;
    imp(body, 0x0008, 0x0005, "  " + std::string(alt.begin(), alt.end()));
    body.insert(body.end(), real.begin(), real.end());

    std::vector<std::uint8_t> meta;  // after the group length
    u16(meta, 0x0002);
    u16(meta, 0x0001);
    meta.push_back('O');
    meta.push_back('B');
    u16(meta, 0);
    u32(meta, 2);
    meta.push_back(0);
    meta.push_back(1);
    exp16(meta, 0x0002, 0x0002, "UI", std::string(kMR) + '\0');
    exp16(meta, 0x0002, 0x0003, "UI", "1.2.3.4" + std::string(1, '\0'));
    exp16(meta, 0x0002, 0x0010, "UI", std::string(ts::ImplicitVRLittleEndian) + '\0');
    exp16(meta, 0x0002, 0x0012, "UI", "2.25.999" + std::string());
    // Implementation Version Name whose last 6 bytes read as the start of a (0002,0016) AE header;
    // the first 2 bytes of the body (08 00, the group of its first tag) complete it as length 8.
    exp16(meta, 0x0002, 0x0013, "SH", std::string("AB\x02\x00\x16\x00" "AE", 8));

    std::vector<std::uint8_t> out(128, 0);
    out.insert(out.end(), {'D', 'I', 'C', 'M'});
    exp16(out, 0x0002, 0x0000, "UL", std::string(4, '\0'));
    const std::uint32_t n = static_cast<std::uint32_t>(meta.size());
    for (int k = 0; k < 4; ++k) out[out.size() - 4 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(n >> (8 * k));
    out.insert(out.end(), meta.begin(), meta.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const siaara_dataset* handle(const Dataset& ds) { return reinterpret_cast<const siaara_dataset*>(&ds); }

std::string repeat_values(std::string_view value, std::size_t n) {
    std::string s;
    s.reserve(n * (value.size() + 1));
    for (std::size_t i = 0; i < n; ++i) {
        if (i) s += '\\';
        s += value;
    }
    return s;
}

Dataset& add_item(Dataset& ds, Tag seq) {
    Element* e = ds.find(seq);
    if (!e) {
        Element ne;
        ne.tag = seq;
        ne.vr = VR::SQ;
        e = &ds.put(std::move(ne));
    }
    return e->items.emplace_back();
}

// One multiplex group with one channel of `samples` samples.
Dataset waveform(std::uint16_t bits, const char* interpretation, std::uint32_t samples) {
    Dataset ds;
    Dataset& g = add_item(ds, 0x54000100);
    (void)g.set_uint16(0x003A0005, 1);
    (void)g.set_uint32(0x003A0010, samples);
    (void)g.set_string(0x003A001A, VR::DS, "500");
    (void)g.set_uint16(0x54001004, bits);
    (void)g.set_string(0x54001006, VR::CS, interpretation);
    Dataset& c = add_item(g, 0x003A0200);
    (void)c.set_string(0x003A0210, VR::DS, "2");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(samples) * (bits / 8u), 0x01);
    (void)g.set_bytes(0x54001010, VR::OW, Bytes::copy_of(data.data(), data.size()));
    return ds;
}

}  // namespace

// ---------------------------------------------------------------- R-108

TEST_CASE("sec4a R-108: the length recovery never runs on the file meta group; lenient and strict read the same values") {
    const std::vector<std::uint8_t> in = meta_boundary_file();
    ReadOptions strict;
    strict.lenient = false;
    auto s = read_bytes(in, strict);
    REQUIRE(s.is_ok());
    auto l = read_bytes(in);
    REQUIRE(l.is_ok());
    CHECK(s->dataset.get_string(tags::PatientName).value_or("") == "GOOD^PATIENT");
    CHECK(l->dataset.get_string(tags::PatientName).value_or("") == "GOOD^PATIENT");
    CHECK(l->dataset.get_string(tags::SOPInstanceUID).value_or("") == s->dataset.get_string(tags::SOPInstanceUID).value_or("?"));
    CHECK(l->dataset.get_string(tags::SOPInstanceUID).value_or("") == "1.2.3.4");
    CHECK(l->dataset.get_string(0x00100020).value_or("") == "GOOD-ID");
    CHECK(l->dataset.size() == s->dataset.size());
    CHECK(l->meta.size() == s->meta.size());
    CHECK(!l->meta.contains(0x00020016));
    REQUIRE(l->meta.find(0x00020013) != nullptr);
    CHECK(l->meta.find(0x00020013)->value.size() == 8);
    CHECK(l->complete());
    CHECK(count_warnings(*l, "RECOVERED") == 0);
    // The C API (lenient by default) agrees.
    siaara_file* cf = nullptr;
    REQUIRE(siaara_file_open_memory(in.data(), in.size(), nullptr, &cf) == SIAARA_OK);
    siaara_read_report rr;
    std::memset(&rr, 0, sizeof rr);
    rr.struct_size = sizeof rr;
    REQUIRE(siaara_file_read_report(cf, &rr) == SIAARA_OK);
    CHECK(rr.loss == 0);
    siaara_file_free(cf);
}

TEST_CASE("sec4a R-108: the length recovery is implicit VR little endian only and can be switched off") {
    // Default (kRecoverOverlongLengthsDefault): the GE Genesis case is still recovered.
    const std::vector<std::uint8_t> ge = ge_like();
    CHECK(ReadOptions().recover_overlong_lengths == kRecoverOverlongLengthsDefault);
    {
        ReadOptions on;
        on.recover_overlong_lengths = true;
        auto f = read_bytes(ge, on);
        REQUIRE(f.is_ok());
        CHECK(f->dataset.get_string(0x0008103E).value_or("") == "AX T1 POST");
        CHECK(f->dataset.get_string(0x00081060).value_or("") == "JANETOS");
        CHECK(count_warnings(*f, "ELEMENT_LENGTH_RECOVERED") == 1);
        CHECK((f->read_loss & DicomFile::kReadRecovered) != 0);
    }
    {
        ReadOptions off;
        off.recover_overlong_lengths = false;
        auto f = read_bytes(ge, off);
        if (f.is_ok()) {
            CHECK(count_warnings(*f, "ELEMENT_LENGTH_RECOVERED") == 0);
            CHECK((f->read_loss & DicomFile::kReadRecovered) == 0);
            CHECK(f->dataset.get_string(0x0008103E).value_or("") != "AX T1 POST");
        }
    }
    // C API: SIAARA_RECOVER_OFF, SIAARA_RECOVER_ON, and a caller built before the field existed.
    auto loss_with = [&](int32_t mode, uint32_t struct_size) -> int64_t {
        siaara_read_options o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = struct_size;
        o.allow_raw_dataset = 1;
        o.recover_overlong_lengths = mode;
        siaara_file* cf = nullptr;
        if (siaara_file_open_memory(ge.data(), ge.size(), &o, &cf) != SIAARA_OK) return -1;
        siaara_read_report rr;
        std::memset(&rr, 0, sizeof rr);
        rr.struct_size = sizeof rr;
        const bool got = siaara_file_read_report(cf, &rr) == SIAARA_OK;
        siaara_file_free(cf);
        return got ? static_cast<int64_t>(rr.loss) : -2;
    };
    const int64_t on = loss_with(SIAARA_RECOVER_ON, sizeof(siaara_read_options));
    CHECK(on >= 0);
    CHECK((on & SIAARA_READ_RECOVERED) != 0);
    const int64_t off = loss_with(SIAARA_RECOVER_OFF, sizeof(siaara_read_options));
    CHECK(off < 0 || (off & SIAARA_READ_RECOVERED) == 0);
    const int64_t dflt = loss_with(SIAARA_RECOVER_DEFAULT, sizeof(siaara_read_options));
    CHECK(((dflt >= 0) && (dflt & SIAARA_READ_RECOVERED) != 0) == kRecoverOverlongLengthsDefault);
    // struct_size without the new field: the field's bytes are not read (here they say OFF).
    const int64_t old_caller = loss_with(SIAARA_RECOVER_OFF, static_cast<uint32_t>(offsetof(siaara_read_options, recover_overlong_lengths)));
    CHECK(((old_caller >= 0) && (old_caller & SIAARA_READ_RECOVERED) != 0) == kRecoverOverlongLengthsDefault);

    // The same length error in explicit VR little endian is not touched: the heuristic was built
    // for implicit VR GE files and its probe is not trusted elsewhere.
    std::vector<std::uint8_t> ex;
    exp16(ex, 0x0008, 0x0016, "UI", std::string(kMR) + '\0');
    exp16(ex, 0x0008, 0x0018, "UI", "2.25.41081 ");
    exp16(ex, 0x0008, 0x0060, "CS", "MR");
    exp16(ex, 0x0008, 0x103E, "LO", "AX T1 POST", 13);
    exp16(ex, 0x0008, 0x1060, "PN", "JANETOS ");
    exp16(ex, 0x0008, 0x1090, "LO", "GENESIS_SIGNA ");
    exp16(ex, 0x0010, 0x0010, "PN", "TEN^B ");
    exp16(ex, 0x0010, 0x0020, "LO", "ID4A");
    ex.back() = 'B';
    auto f = read_bytes(ex);
    if (f.is_ok()) {
        CHECK(count_warnings(*f, "ELEMENT_LENGTH_RECOVERED") == 0);
        CHECK((f->read_loss & DicomFile::kReadRecovered) == 0);
    }
}

// ---------------------------------------------------------------- R-114

TEST_CASE("sec4a R-114: binary values are never shortened; a group length of any group is no evidence") {
    // 16-bit image whose last sample is 0xF830, followed by 6 zero pad bytes: the old probe read
    // the sample and the padding as a header (F830,0000) of length 0 and dropped the sample.
    std::vector<std::uint8_t> b;
    imp(b, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(b, 0x0008, 0x0018, "2.25.41082");
    imp(b, 0x0028, 0x0010, le16s(1));
    imp(b, 0x0028, 0x0011, le16s(2));
    imp(b, 0x0028, 0x0100, le16s(16));
    imp(b, 0x7FE0, 0x0010, std::string("\x01\x00\x30\xF8", 4));
    b.insert(b.end(), 6, 0);
    auto f = read_bytes(b);
    REQUIRE(f.is_ok());
    REQUIRE(f->dataset.find(tags::PixelData) != nullptr);
    CHECK(f->dataset.find(tags::PixelData)->value.size() == 4);
    CHECK(!f->dataset.contains(0xF8300000u));
    CHECK(count_warnings(*f, "ELEMENT_LENGTH_RECOVERED") == 0);
    CHECK((f->read_loss & DicomFile::kReadRecovered) == 0);
    // The padding is reported as skipped bytes instead (R-154).
    CHECK((f->read_loss & DicomFile::kReadBytesSkipped) != 0);
    CHECK(f->bytes_not_read == 6);

    // Other binary VRs and UN: an OB value (private, unknown tag) followed by what looks like headers.
    std::vector<std::uint8_t> c;
    imp(c, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(c, 0x0008, 0x0018, "2.25.41083");
    imp(c, 0x0028, 0x0010, le16s(1), 5);  // US stated 5 for 2: binary, not recovered
    imp(c, 0x0028, 0x0011, le16s(2));
    imp(c, 0x0028, 0x0100, le16s(16));
    imp(c, 0x0028, 0x0101, le16s(16));
    imp(c, 0x0028, 0x0102, le16s(15));
    auto g = read_bytes(c);
    if (g.is_ok()) {
        CHECK(count_warnings(*g, "ELEMENT_LENGTH_RECOVERED") == 0);
        CHECK((g->read_loss & DicomFile::kReadRecovered) == 0);
    }

    // A text value whose last bytes with the padding read as (0020,0000) length 0: a generic
    // group length is not a dictionary entry, and one header is not a run of three.
    std::vector<std::uint8_t> d;
    imp(d, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(d, 0x0008, 0x0018, "2.25.41084");
    imp(d, 0x0008, 0x103E, std::string("ABCDEFGHIJ\x20\x00", 12));
    d.insert(d.end(), 6, 0);
    auto h = read_bytes(d);
    REQUIRE(h.is_ok());
    REQUIRE(h->dataset.find(0x0008103E) != nullptr);
    CHECK(h->dataset.find(0x0008103E)->value.size() == 12);
    CHECK(!h->dataset.contains(0x00200000u));
    CHECK(count_warnings(*h, "ELEMENT_LENGTH_RECOVERED") == 0);

    // Two headers after the cut are not enough any more: the full run of three is required.
    std::vector<std::uint8_t> e;
    imp(e, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(e, 0x0008, 0x0018, "2.25.41085");
    imp(e, 0x0008, 0x103E, "AX T1 POST", 13);
    imp(e, 0x0008, 0x1060, "JANETOS ");
    imp(e, 0x0010, 0x0010, "TEN^B ");
    auto k = read_bytes(e);
    if (k.is_ok()) CHECK(count_warnings(*k, "ELEMENT_LENGTH_RECOVERED") == 0);
}

// ---------------------------------------------------------------- R-154

TEST_CASE("sec4a R-154: trailing zero padding is reported as skipped bytes, never read as elements (0000,0000)") {
    DicomFile src;
    src.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)src.dataset.set_string(tags::SOPClassUID, VR::UI, kMR);
    (void)src.dataset.set_string(tags::SOPInstanceUID, VR::UI, "2.25.41086");
    (void)src.dataset.set_string(tags::PatientName, VR::PN, "PAD^TEST");
    std::vector<std::uint8_t> clean;
    REQUIRE(write_part10(src, clean).is_ok());
    for (const std::size_t pad : {std::size_t{8}, std::size_t{3}, std::size_t{512}}) {
        std::vector<std::uint8_t> in = clean;
        in.insert(in.end(), pad, 0);
        auto f = read_bytes(in);
        REQUIRE(f.is_ok());
        CHECK(!f->dataset.contains(0x00000000u));
        CHECK(f->dataset.size() == 3);
        CHECK((f->read_loss & DicomFile::kReadBytesSkipped) != 0);
        CHECK((f->read_loss & DicomFile::kReadInputTruncated) == 0);
        CHECK(f->bytes_not_read == pad);
        CHECK(f->read_loss_offset == clean.size());
        CHECK(count_warnings(*f, "TRAILING_PADDING") == 1);
        REQUIRE(!f->warnings.empty());
        CHECK(f->warnings[0].rfind(std::string(kWarnParseIncomplete), 0) == 0);
        // The incomplete-read rule applies: no silent save; an allowed save writes the elements
        // (byte for byte what the file held before its padding).
        std::vector<std::uint8_t> out;
        CHECK(write_part10(*f, out).code == Errc::Rejected);
        WriteOptions wo;
        wo.allow_incomplete = true;
        out.clear();
        REQUIRE(write_part10(*f, out, wo).is_ok());
        CHECK(out == clean);
        // Strict mode fails, in explicit VR as before.
        ReadOptions strict;
        strict.lenient = false;
        CHECK(!read_bytes(in, strict).is_ok());
    }
    // Implicit VR: strict mode used to accept the padding as elements (0000,0000).
    std::vector<std::uint8_t> raw;
    imp(raw, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(raw, 0x0008, 0x0018, "2.25.41087");
    raw.insert(raw.end(), 16, 0);
    ReadOptions strict;
    strict.lenient = false;
    CHECK(!read_bytes(raw, strict).is_ok());
    auto l = read_bytes(raw);
    REQUIRE(l.is_ok());
    CHECK(l->dataset.size() == 2);
    CHECK(l->bytes_not_read == 16);
    // A data set that really starts with group 0000 (a command set) is not padding.
    std::vector<std::uint8_t> cmd;
    imp(cmd, 0x0000, 0x0000, std::string("\x0a\x00\x00\x00", 4));
    imp(cmd, 0x0000, 0x0100, le16s(0x0030));
    auto c = read_dataset(Bytes::copy_of(cmd.data(), cmd.size()), ts::ImplicitVRLittleEndian);
    REQUIRE(c.is_ok());
    CHECK(c->contains(0x00000000u));
    // Zero bytes inside the last value are part of it.
    std::vector<std::uint8_t> z;
    imp(z, 0x0008, 0x0018, "2.25.41088");
    imp(z, 0x7FE0, 0x0010, std::string(8, '\0'));
    auto zf = read_bytes(z);
    REQUIRE(zf.is_ok());
    CHECK(zf->complete());
    CHECK(zf->dataset.find(tags::PixelData)->value.size() == 8);
}

// ---------------------------------------------------------------- R-111

TEST_CASE("sec4a R-111: one-pass value split and get_doubles agree with the per-value getters") {
    for (std::string_view s : {std::string_view(""), std::string_view("a"), std::string_view("a\\b"), std::string_view("\\"),
                               std::string_view("a\\\\b\\"), std::string_view(" 1 \\ 2")}) {
        const auto all = split_all_values(s);
        std::size_t n = 0;
        const std::size_t visited = for_each_value(s, [&](std::size_t i, std::string_view piece) {
            CHECK(i == n);
            CHECK(split_value(s, i).has_value());
            CHECK(split_value(s, i).value_or("?") == piece);
            ++n;
            return true;
        });
        CHECK(visited == n);
        CHECK(all.size() == n);
        CHECK(!split_value(s, n).has_value());
    }
    CHECK(split_all_values("a\\b\\c", 2).size() == 2);
    std::size_t seen = 0;
    CHECK(for_each_value("a\\b\\c", [&](std::size_t, std::string_view) { return ++seen < 2; }) == 2);

    Dataset ds;
    (void)ds.set_string(0x00200032, VR::DS, "1.5\\ -2 \\3e2");
    (void)ds.set_string(0x00200013, VR::IS, "7\\-8");
    const std::uint16_t us[3] = {1, 2, 65535};
    (void)ds.set_bytes(0x00280010, VR::US, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(us), sizeof us));
    const double fd[2] = {0.25, -1e300};
    (void)ds.set_bytes(0x00189313, VR::FD, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(fd), sizeof fd));
    for (Tag t : {Tag{0x00200032}, Tag{0x00200013}, Tag{0x00280010}, Tag{0x00189313}}) {
        const auto all = ds.get_doubles(t);
        REQUIRE(all.has_value());
        REQUIRE(all->size() == ds.value_multiplicity(t));
        for (std::size_t i = 0; i < all->size(); ++i) CHECK((*all)[i] == ds.get_double(t, i).value_or(-12345.0));
        double one = 0;
        CHECK(ds.get_doubles(t, all->size() - 1, std::span<double>(&one, 1)));
        CHECK(one == all->back());
        CHECK(!ds.get_doubles(t, all->size(), std::span<double>(&one, 1)));  // past the last value
    }
    (void)ds.set_string(0x00200032, VR::DS, "1\\x\\3");
    CHECK(!ds.get_doubles(0x00200032).has_value());
    double two[2] = {0, 0};
    CHECK(!ds.get_doubles(0x00200032, 0, two));
    double first = 0;
    CHECK(ds.get_doubles(0x00200032, 0, std::span<double>(&first, 1)));  // the bad value is not in the range
    CHECK(first == 1.0);
    CHECK(!ds.get_doubles(0x00209999).has_value());

    // Text: decoded once, split once, same values as get_utf8 one by one.
    Dataset t;
    (void)t.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
    (void)t.set_string(0x00081070, VR::PN, "M\xFCller^Jos\xE9\\ Second ^X \\\\Last");
    (void)t.set_string(0x00204000, VR::LT, " a\\b ");
    const auto pn = charset::get_utf8_all(t, 0x00081070);
    REQUIRE(pn.has_value());
    std::string joined;
    for (std::size_t i = 0; i < t.value_multiplicity(0x00081070); ++i) {
        if (i) joined += '\\';
        joined += charset::get_utf8(t, 0x00081070, i).value_or("?");
    }
    CHECK(*pn == joined);
    CHECK(charset::get_utf8_all(t, 0x00204000).value_or("?") == charset::get_utf8(t, 0x00204000).value_or("!"));
    CHECK(!charset::get_utf8_all(t, 0x00209999).has_value());
    auto pv = modules::get_by_path(t, "OperatorsName");
    REQUIRE(pv.is_ok());
    CHECK(pv->text == joined);
    CHECK(pv->vm == 4);
}

TEST_CASE("sec4a R-111: the path API reads 1,000,000 values in one pass (time limit)") {
    constexpr std::size_t kValues = 1'000'000;
    // Quadratic code needed 33 s for 40,000 values under ASan; 1,000,000 would take hours. One
    // pass takes well under a second in a Release build; the limit leaves room for sanitizers.
    constexpr double kLimitSeconds = 30.0;
    Dataset ds;
    Element e;
    e.tag = 0x30060050;  // Contour Data, DS
    e.vr = VR::DS;
    e.value = Bytes::from_string(repeat_values("1.5", kValues));
    ds.put(std::move(e));
    Element lo;
    lo.tag = 0x00081070;  // Operators' Name, PN
    lo.vr = VR::PN;
    lo.value = Bytes::from_string(repeat_values("A^B", kValues));
    ds.put(std::move(lo));
    Element is;
    is.tag = 0x00280009;  // any IS-typed value would do; given the VR explicitly
    is.vr = VR::IS;
    is.value = Bytes::from_string(repeat_values("12", kValues));
    ds.put(std::move(is));

    const auto t0 = std::chrono::steady_clock::now();
    size_t count = 0;
    REQUIRE(siaara_get_doubles_by_path(handle(ds), "ContourData", 11, nullptr, 0, &count) == SIAARA_OK);
    REQUIRE(count == kValues);
    std::vector<double> v(count);
    REQUIRE(siaara_get_doubles_by_path(handle(ds), "ContourData", 11, v.data(), v.size(), &count) == SIAARA_OK);
    CHECK(v.front() == 1.5);
    CHECK(v.back() == 1.5);
    const char* last = "ContourData[999999]";
    double one = 0;
    REQUIRE(siaara_get_doubles_by_path(handle(ds), last, std::strlen(last), &one, 1, &count) == SIAARA_OK);
    CHECK(count == 1);
    CHECK(one == 1.5);

    siaara_path_value pv;
    std::memset(&pv, 0, sizeof pv);
    pv.struct_size = sizeof pv;
    size_t needed = 0;
    REQUIRE(siaara_get_by_path(handle(ds), "ContourData", 11, &pv, nullptr, 0, &needed) == SIAARA_OK);
    CHECK(pv.vm == kValues);
    CHECK(needed == kValues * 4 - 1);
    auto text = modules::get_by_path(ds, "OperatorsName");
    REQUIRE(text.is_ok());
    CHECK(text->text.size() == kValues * 4 - 1);
    auto ints = modules::get_by_path(ds, "(0028,0009)");
    REQUIRE(ints.is_ok());
    CHECK(ints->text.size() == kValues * 3 - 1);
    auto dv = modules::doubles_by_path(ds, "(0028,0009)");
    REQUIRE(dv.is_ok());
    CHECK(dv->size() == kValues);
    CHECK(dv->back() == 12.0);
    const double took = seconds_since(t0);
    std::printf("  sec4a R-111: 1,000,000-value path reads took %.3f s\n", took);
    CHECK(took < kLimitSeconds);
}

TEST_CASE("sec4a R-111: waveform text and RT leaf positions with 200,000 values (time limit)") {
    constexpr std::size_t kValues = 200'000;
    constexpr double kLimitSeconds = 20.0;
    // RT plan beam with one control point.
    Dataset beam;
    Dataset& cp = add_item(beam, 0x300A0111);
    Dataset& dev = add_item(cp, 0x300A011A);
    (void)dev.set_string(0x300A00B8, VR::CS, "MLCX");
    Element pos;
    pos.tag = 0x300A011C;
    pos.vr = VR::DS;
    pos.value = Bytes::from_string(repeat_values("-12.5", kValues));
    dev.put(std::move(pos));
    // Waveform with a channel label of many values.
    Dataset wf = waveform(16, "SS", 4);
    Dataset& ch = wf.find(0x54000100)->items[0].find(0x003A0200)->items[0];
    Element label;
    label.tag = 0x003A0203;
    label.vr = VR::SH;
    label.value = Bytes::from_string(repeat_values("L1", kValues));
    ch.put(std::move(label));

    const auto t0 = std::chrono::steady_clock::now();
    uint32_t from = 99;
    size_t count = 0;
    REQUIRE(siaara_rt_device_positions(handle(beam), 0, "MLCX", 4, 1, nullptr, 0, &count, &from) == SIAARA_OK);
    REQUIRE(count == kValues);
    CHECK(from == 0);
    std::vector<double> v(count);
    REQUIRE(siaara_rt_device_positions(handle(beam), 0, "MLCX", 4, 1, v.data(), v.size(), &count, &from) == SIAARA_OK);
    CHECK(v.front() == -12.5);
    CHECK(v.back() == -12.5);
    auto cpp = modules::device_positions(beam, 0, "MLCX", false, &from);
    REQUIRE(cpp.is_ok());
    CHECK(cpp->size() == kValues);
    auto txt = modules::waveform_text(wf, 0, 0, modules::WaveformText::Label);
    REQUIRE(txt.is_ok());
    CHECK(txt->size() == kValues * 3 - 1);
    const double took = seconds_since(t0);
    std::printf("  sec4a R-111: 200,000-value positions and label took %.3f s\n", took);
    CHECK(took < kLimitSeconds);

    // A value that is not a number: counted by the size query, reported by the read.
    Element* p = dev.find(0x300A011C);
    p->value = Bytes::from_string("1\\two\\3 ");
    REQUIRE(siaara_rt_device_positions(handle(beam), 0, "MLCX", 4, 1, nullptr, 0, &count, nullptr) == SIAARA_OK);
    CHECK(count == 3);
    double three[3];
    CHECK(siaara_rt_device_positions(handle(beam), 0, "MLCX", 4, 1, three, 3, &count, nullptr) == SIAARA_ERR_MALFORMED);
    CHECK(siaara_rt_device_positions(handle(beam), 0, "MLCX", 4, 1, three, 2, &count, nullptr) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(!modules::device_positions(beam, 0, "MLCX", false, nullptr).is_ok());
}

// ---------------------------------------------------------------- R-112

TEST_CASE("sec4a R-112: the validator splits values once; 3 x 20,000 CS values validate fast (time limit)") {
    constexpr std::size_t kValues = 20'000;
    constexpr double kLimitSeconds = 20.0;
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kMR);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.41089");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.41090");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.41091");
    (void)ds.set_string(tags::PatientName, VR::PN, "Val^Fast");
    (void)ds.set_string(tags::Modality, VR::CS, "MR");
    // Image Type, Scanning Sequence, Sequence Variant: condition terms and Enumerated Values of
    // the MR Image Module look at each of them.
    (void)ds.set_string(0x00080008, VR::CS, repeat_values("AB", kValues));
    (void)ds.set_string(0x00180020, VR::CS, repeat_values("SE", kValues));
    (void)ds.set_string(0x00180021, VR::CS, repeat_values("SK", kValues));
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 2);
    (void)ds.set_uint16(tags::Columns, 2);
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>("\1\2\3\4"), 4));
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());

    const auto t0 = std::chrono::steady_clock::now();
    auto r = repair::validate_bytes(Bytes::copy_of(bytes.data(), bytes.size()));
    const double took = seconds_since(t0);
    std::printf("  sec4a R-112: validating 3 x 20,000 CS values took %.3f s\n", took);
    REQUIRE(r.is_ok());
    CHECK(took < kLimitSeconds);
    // More values than the validator compares: said once per attribute, not silently skipped.
    int capped = 0;
    for (const repair::Finding& x : *r) capped += x.code == "values-not-all-checked" ? 1 : 0;
    CHECK(capped >= 1);
    CHECK(capped <= 3);

    // Below the cap nothing changes: an Enumerated Value check still names the bad value.
    (void)ds.set_string(0x00080008, VR::CS, "ORIGINAL\\PRIMARY");
    (void)ds.set_string(0x00180020, VR::CS, "SE\\XX\\IR");
    (void)ds.set_string(0x00180021, VR::CS, "SK");
    bytes.clear();
    REQUIRE(write_part10(f, bytes).is_ok());
    auto small = repair::validate_bytes(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(small.is_ok());
    bool enumerated = false;
    for (const repair::Finding& x : *small) {
        CHECK(x.code != "values-not-all-checked");
        if (x.code == "enumerated-value" && x.message.find("value 2 ") == 0) enumerated = true;
    }
    CHECK(enumerated);
}

// ---------------------------------------------------------------- R-123 / R-137

TEST_CASE("sec4a R-123: array results are counted without decoding and written straight into the buffer") {
    Dataset wf = waveform(16, "SS", 1000);
    size_t count = 0;
    REQUIRE(siaara_waveform_raw_samples(handle(wf), 0, 0, nullptr, 0, &count) == SIAARA_OK);
    CHECK(count == 1000);
    std::vector<int64_t> raw(1000, -1);
    CHECK(siaara_waveform_raw_samples(handle(wf), 0, 0, raw.data(), 999, &count) == SIAARA_ERR_BUFFER_TOO_SMALL);
    CHECK(count == 1000);
    CHECK(raw[0] == -1);  // nothing written
    CHECK(siaara_waveform_raw_samples(handle(wf), 0, 0, nullptr, 1000, &count) == SIAARA_ERR_INVALID_ARGUMENT);
    REQUIRE(siaara_waveform_raw_samples(handle(wf), 0, 0, raw.data(), raw.size(), &count) == SIAARA_OK);
    CHECK(raw[0] == 0x0101);
    CHECK(raw[999] == 0x0101);
    std::vector<double> d(1001, -1.0);
    REQUIRE(siaara_waveform_samples(handle(wf), 0, 0, 1, d.data(), d.size(), &count) == SIAARA_OK);
    CHECK(count == 1000);
    CHECK(d[0] == 0x0101 * 2.0);
    CHECK(d[1000] == -1.0);  // only `count` values written
    REQUIRE(siaara_waveform_samples(handle(wf), 0, 0, 0, d.data(), d.size(), &count) == SIAARA_OK);
    CHECK(d[999] == 0x0101);
    // The C++ vector forms give the same values.
    auto cr = modules::waveform_raw(wf, 0, 0);
    REQUIRE(cr.is_ok());
    CHECK(*cr == raw);
    auto cs = modules::waveform_scaled(wf, 0, 0);
    REQUIRE(cs.is_ok());
    CHECK(cs->size() == 1000);
    CHECK(cs->front() == 0x0101 * 2.0);
    auto n = modules::waveform_sample_count(wf, 0, 0, true);
    REQUIRE(n.is_ok());
    CHECK(*n == 1000);
    CHECK(!modules::waveform_sample_count(wf, 0, 1, false).is_ok());
    std::vector<std::int64_t> wrong(10);
    CHECK(modules::waveform_raw_into(wf, 0, 0, wrong).code == Errc::BufferTooSmall);
    // Budget arithmetic (4 GiB): no overflow, exact boundary.
    static_assert(modules::fits_named_data_budget(modules::kNamedDataMemoryBudget / 8, 8));
    static_assert(!modules::fits_named_data_budget(modules::kNamedDataMemoryBudget / 8 + 1, 8));
    static_assert(!modules::fits_named_data_budget(~std::uint64_t{0}, 8));
    // Path doubles: count, then fill.
    Dataset ds;
    (void)ds.set_string(0x00200032, VR::DS, "1\\2\\3.5");
    auto c = modules::doubles_by_path_count(ds, "ImagePositionPatient");
    REQUIRE(c.is_ok());
    CHECK(*c == 3);
    double three[3] = {0, 0, 0};
    CHECK(modules::doubles_by_path_into(ds, "ImagePositionPatient", three).is_ok());
    CHECK(three[2] == 3.5);
    double two[2];
    CHECK(modules::doubles_by_path_into(ds, "ImagePositionPatient", two).code == Errc::BufferTooSmall);
    auto c1 = modules::doubles_by_path_count(ds, "ImagePositionPatient[2]");
    REQUIRE(c1.is_ok());
    CHECK(*c1 == 1);
    CHECK(!modules::doubles_by_path_count(ds, "ImagePositionPatient[3]").is_ok());
}

TEST_CASE("sec4a R-137: waveform samples only for the Bits Allocated / Sample Interpretation pairs of PS3.3 C.10.9") {
    struct Pair {
        std::uint16_t bits;
        const char* interp;
        bool valid;
    };
    const Pair pairs[] = {{8, "SB", true},   {8, "UB", true},   {8, "MB", true},   {8, "AB", true},   {16, "SS", true},
                          {16, "US", true},  {32, "SL", true},  {32, "UL", true},  {64, "SV", true},  {64, "UV", true},
                          {24, "SS", false}, {40, "SL", false}, {48, "SV", false}, {56, "UV", false}, {16, "SB", false},
                          {8, "SS", false},  {32, "SS", false}, {64, "SL", false}, {16, "", false},   {16, "XX", false},
                          {0, "SS", false},  {12, "SS", false}};
    for (const Pair& p : pairs) {
        if (p.bits % 8 != 0 || p.bits == 0) {
            Dataset odd = waveform(16, p.interp, 4);
            (void)odd.find(0x54000100)->items[0].set_uint16(0x54001004, p.bits);
            CHECK(modules::waveform_raw(odd, 0, 0).error().code == Errc::Malformed);
            continue;
        }
        Dataset wf = waveform(p.bits, p.interp, 4);
        auto g = modules::waveform_group(wf, 0);
        REQUIRE(g.is_ok());
        auto raw = modules::waveform_raw(wf, 0, 0);
        if (p.valid) {
            CHECK(g->data_samples == 4);
            REQUIRE(raw.is_ok());
            CHECK(raw->size() == 4);
        } else {
            CHECK(g->data_samples == 0);
            REQUIRE(!raw.is_ok());
            CHECK(raw.error().code == Errc::Malformed);
            size_t count = 7;
            CHECK(siaara_waveform_raw_samples(handle(wf), 0, 0, nullptr, 0, &count) == SIAARA_ERR_MALFORMED);
            CHECK(count == 0);
        }
    }
}

// ---------------------------------------------------------------- R-138

TEST_CASE("sec4a R-138: DS values that are not finite numbers are not returned as numbers; the bytes stay") {
    for (const char* bad : {"nan", "NaN", "inf", "-inf", "INF", "infinity", "1e999", "-1e999", "0x10", "1e", ""}) {
        Dataset ds;
        const std::string value = std::string("1.5\\") + bad;
        (void)ds.set_string(0x00200032, VR::DS, value);
        CHECK(ds.get_double(0x00200032, 0).value_or(0) == 1.5);
        CHECK(!ds.get_double(0x00200032, 1).has_value());
        CHECK(!ds.get_int(0x00200032, 1).has_value());
        CHECK(!ds.get_doubles(0x00200032).has_value());
        CHECK(!modules::doubles_by_path(ds, "ImagePositionPatient").is_ok());
        double buf[2];
        size_t count = 0;
        CHECK(siaara_get_doubles_by_path(handle(ds), "ImagePositionPatient", 20, buf, 2, &count) == SIAARA_ERR_TYPE_MISMATCH);
        auto pv = modules::get_by_path(ds, "ImagePositionPatient[1]");
        REQUIRE(pv.is_ok());
        CHECK(!pv->has_real);
        // The stored value is untouched and still readable as text.
        CHECK(ds.get_string(0x00200032, 1).value_or("?") == bad);
        std::string_view stored = ds.find(0x00200032)->value.as_chars();
        CHECK(stored.substr(0, value.size()) == value);
    }
}

// ---------------------------------------------------------------- R-140

TEST_CASE("sec4a R-140: line profile refuses extreme end points and sample counts") {
    using clinical::line_profile;
    std::vector<float> img(100, 1.0f);
    const double huge = 1e308;
    CHECK(line_profile(img, 10, 10, {-huge, 0}, {huge, 0}).samples.empty());
    CHECK(line_profile(img, 10, 10, {0, -huge}, {0, huge}, nullptr, 16).samples.empty());
    CHECK(line_profile(img, 10, 10, {2e9, 0}, {5, 5}).samples.empty());
    CHECK(line_profile(img, 10, 10, {5, 5}, {0, -1.5e9}).samples.empty());
    CHECK(line_profile(img, 10, 10, {std::numeric_limits<double>::quiet_NaN(), 0}, {5, 5}).samples.empty());
    CHECK(line_profile(img, 10, 10, {std::numeric_limits<double>::infinity(), 0}, {5, 5}).samples.empty());
    // A sample count nobody needs is refused before anything is reserved.
    CHECK(line_profile(img, 10, 10, {0, 0}, {9, 9}, nullptr, 0xFFFFFFFFu).samples.empty());
    CHECK(line_profile(img, 10, 10, {0, 0}, {9, 9}, nullptr, clinical::kMaxProfileSamples + 1).samples.empty());
    // Still allowed: far outside the image but within the limit (values are NaN outside).
    auto far = line_profile(img, 10, 10, {-1e9, 5}, {1e9, 5}, nullptr, 5);
    REQUIRE(far.samples.size() == 5);
    CHECK(std::isnan(far.samples[0].value));
    CHECK(far.samples[2].value == 1.0);
    CHECK(std::isnan(far.samples[4].value));
    auto usual = line_profile(img, 10, 10, {0.5, 5.5}, {9.5, 5.5});
    CHECK(usual.samples.size() == 10);
}

// ---------------------------------------------------------------- R-157

TEST_CASE("sec4a R-157: an unquoted private creator with a dot gets a hint to quote it") {
    Dataset ds;
    (void)ds.set_string(0x00090010, VR::LO, "GEMS.IDEN.01");
    (void)ds.set_string(0x00091001, VR::LO, "value");
    auto bad = modules::get_by_path(ds, "GEMS.IDEN.01:0009,01");
    REQUIRE(!bad.is_ok());
    CHECK(bad.error().message.find("unknown keyword: GEMS") == 0);
    CHECK(bad.error().message.find("must be quoted") != std::string::npos);
    auto good = modules::get_by_path(ds, "\"GEMS.IDEN.01\":0009,01");
    REQUIRE(good.is_ok());
    CHECK(good->text == "value");
    // No colon anywhere: a plain unknown keyword, no hint.
    auto plain = modules::get_by_path(ds, "NoSuchKeyword");
    REQUIRE(!plain.is_ok());
    CHECK(plain.error().message.find("quoted") == std::string::npos);
}

// ================================================================ testing re-run 2026-11-11, section 6

namespace {

uint32_t module_index(const char* name) {
    uint32_t idx = 0xFFFFFFFFu;
    if (siaara_module_find(name, std::strlen(name), &idx) != SIAARA_OK) return 0xFFFFFFFFu;
    return idx;
}

siaara_module_info module_info(uint32_t idx) {
    siaara_module_info mi;
    std::memset(&mi, 0, sizeof mi);
    mi.struct_size = sizeof mi;
    (void)siaara_module_get(idx, &mi);
    return mi;
}

// True when table `name` lists `tag` at any depth (through its sequence item nodes).
bool module_lists(const char* name, uint32_t tag) {
    const auto idx = modules::find_node(name);
    if (!idx) return false;
    std::vector<std::uint32_t> todo{*idx}, seen;
    while (!todo.empty()) {
        const std::uint32_t n = todo.back();
        todo.pop_back();
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        for (const modules::ModEntry& e : modules::entries(modules::nodes()[n])) {
            if (e.attr >= 0 && modules::attribute(e.attr)->tag == tag) return true;
            if (e.node >= 0) todo.push_back(static_cast<std::uint32_t>(e.node));
        }
    }
    return false;
}

}  // namespace

TEST_CASE("sec4a 6.3: repeating-group keywords resolve in the path API (first group; other groups by tag)") {
    Dataset ds;
    (void)ds.set_uint16(0x60000010, 512);
    (void)ds.set_uint16(0x60020010, 256);
    (void)ds.set_uint16(0x50000005, 2);
    auto a = modules::get_by_path(ds, "OverlayRows");
    REQUIRE(a.is_ok());
    CHECK(a->tag == 0x60000010u);
    CHECK(a->integer == 512);
    auto b = modules::get_by_path(ds, "(6002,0010)");
    REQUIRE(b.is_ok());
    CHECK(b->integer == 256);
    auto c = modules::get_by_path(ds, "CurveDimensions");
    REQUIRE(c.is_ok());
    CHECK(c->integer == 2);
    siaara_dict_entry e;
    std::memset(&e, 0, sizeof e);
    e.struct_size = sizeof e;
    REQUIRE(siaara_dict_lookup_keyword("OverlayRows", 11, &e) == SIAARA_OK);
    CHECK(e.tag == 0x60000010u);
    CHECK(e.mask == 0xFF00FFFFu);
    // A repeating *element* keyword has no default tag: still an error, with a hint.
    auto d = modules::get_by_path(ds, "RowsForNthOrderCoefficients");
    REQUIRE(!d.is_ok());
    CHECK(d.error().message.find("use the tag") != std::string::npos);
}

TEST_CASE("sec4a 6.4: a bare module name prefers the Module over the Macro; Enhanced XA/XRF Image is a module") {
    for (const char* name : {"Cardiac Synchronization", "Respiratory Synchronization", "Real World Value Mapping", "Patient Orientation"}) {
        const uint32_t idx = module_index(name);
        REQUIRE(idx != 0xFFFFFFFFu);
        const siaara_module_info mi = module_info(idx);
        CHECK(mi.kind == static_cast<int32_t>(modules::NodeKind::Module));
        CHECK(std::string(mi.name) == std::string(name) + " Module");
        // The macro is still reachable by its full name.
        const uint32_t macro = module_index((std::string(name) + " Macro").c_str());
        REQUIRE(macro != 0xFFFFFFFFu);
        CHECK(module_info(macro).kind == static_cast<int32_t>(modules::NodeKind::Macro));
    }
    const uint32_t xa = module_index("Enhanced XA/XRF Image");
    REQUIRE(xa != 0xFFFFFFFFu);
    CHECK(module_info(xa).kind == static_cast<int32_t>(modules::NodeKind::Module));
    CHECK(std::string(module_info(xa).name) == "Enhanced XA/XRF Image Module");
    CHECK(module_index("Enhanced XA/XRF Image Module") == xa);
}

TEST_CASE("sec4a 6.5: rows that follow an Include row are in the generated tables") {
    CHECK(module_lists("Image Reference Macro", 0x00081160));   // Referenced Frame Number
    CHECK(module_lists("Image Reference Macro", 0x0062000B));   // Referenced Segment Number
    CHECK(module_lists("Image Reference Macro", 0x0008114B));   // Referenced Real World Value Mapping Instance Sequence
    CHECK(module_lists("Waveform Reference Macro", 0x0040A0B0)); // Referenced Waveform Channels
    // The attributes of the included table are still there.
    CHECK(module_lists("Image Reference Macro", 0x00081199));
    CHECK(module_lists("Image Reference Macro", 0x00081155));
    CHECK(module_lists("Waveform Reference Macro", 0x00081150));
}

TEST_CASE("sec4a 6.6: bytes outside the character set are U+FFFD and counted; the stored bytes stay") {
    Dataset ds;
    (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
    // Patient Position is CS: (0008,0005) does not apply, 0xE9 is not a character of its repertoire.
    (void)ds.set_string(0x00185100, VR::CS, std::string("HF\xE9S"));
    (void)ds.set_string(tags::PatientName, VR::PN, std::string("Andr\xE9"));
    std::size_t bad = 99;
    CHECK(charset::get_utf8(ds, 0x00185100, 0, nullptr, &bad).value_or("") == "HF\xEF\xBF\xBDS");
    CHECK(bad == 1);
    CHECK(charset::get_utf8_all(ds, 0x00185100, nullptr, &bad).value_or("") == "HF\xEF\xBF\xBDS");
    CHECK(bad == 1);
    auto pv = modules::get_by_path(ds, "PatientPosition");
    REQUIRE(pv.is_ok());
    CHECK(pv->text == "HF\xEF\xBF\xBDS");
    CHECK(pv->undecodable == 1);
    CHECK(ds.get_raw_string(0x00185100).value_or("") == std::string("HF\xE9S"));
    // Text the character set covers decodes, with nothing to report.
    auto pn = modules::get_by_path(ds, "PatientName");
    REQUIRE(pn.is_ok());
    CHECK(pn->text == "Andr\xC3\xA9");
    CHECK(pn->undecodable == 0);
    siaara_path_value v;
    std::memset(&v, 0, sizeof v);
    v.struct_size = sizeof v;
    char buf[32];
    size_t needed = 0;
    REQUIRE(siaara_get_by_path(handle(ds), "PatientPosition", 15, &v, buf, sizeof buf, &needed) == SIAARA_OK);
    CHECK(v.undecodable == 1);
}

TEST_CASE("sec4a 6.7: transfer syntax and dictionary lookups") {
    auto ts_info = [](const char* uid, siaara_transfer_syntax_info& out) {
        std::memset(&out, 0, sizeof out);
        out.struct_size = sizeof out;
        return siaara_transfer_syntax_info_get(uid, std::strlen(uid), &out);
    };
    siaara_transfer_syntax_info t;
    // The 11 retired transfer syntaxes the toolkit does not read: found, retired.
    for (const char* uid : {"1.2.840.10008.1.2.4.59", "1.2.840.10008.1.2.4.60", "1.2.840.10008.1.2.4.61", "1.2.840.10008.1.2.4.62",
                            "1.2.840.10008.1.2.4.63", "1.2.840.10008.1.2.4.64", "1.2.840.10008.1.2.4.65", "1.2.840.10008.1.2.4.66",
                            "1.2.840.10008.1.2.6.1", "1.2.840.10008.1.2.6.2", "1.2.840.10008.1.20"}) {
        REQUIRE(ts_info(uid, t) == SIAARA_OK);
        CHECK(t.retired == 1);
        CHECK(find_transfer_syntax(uid) == nullptr);  // readers and writers do not know them
    }
    // JPIP Referenced and SMPTE ST 2110: no native pixel data.
    for (const char* uid : {"1.2.840.10008.1.2.4.94", "1.2.840.10008.1.2.4.95", "1.2.840.10008.1.2.4.204", "1.2.840.10008.1.2.4.205",
                            "1.2.840.10008.1.2.7.1", "1.2.840.10008.1.2.7.2", "1.2.840.10008.1.2.7.3"}) {
        REQUIRE(ts_info(uid, t) == SIAARA_OK);
        CHECK(t.encapsulated == 1);
        CHECK(t.retired == 0);
    }
    REQUIRE(ts_info("1.2.840.10008.1.2.4.95", t) == SIAARA_OK);
    CHECK(t.deflated == 1);
    REQUIRE(ts_info("1.2.840.10008.1.2.1", t) == SIAARA_OK);
    CHECK(t.encapsulated == 0 && t.retired == 0 && t.deflated == 0);
    CHECK(ts_info("1.2.3.4", t) == SIAARA_ERR_NOT_FOUND);

    // Dictionary enumeration: every entry's tag looks up to the entry itself (a repeating element
    // such as (0028,04x0) is not listed under a tag that is another attribute).
    uint32_t count = 0;
    REQUIRE(siaara_dict_count(&count) == SIAARA_OK);
    int masked = 0;
    for (uint32_t i = 0; i < count; ++i) {
        siaara_dict_entry e, l;
        std::memset(&e, 0, sizeof e);
        e.struct_size = sizeof e;
        REQUIRE(siaara_dict_entry_at(i, &e) == SIAARA_OK);
        std::memset(&l, 0, sizeof l);
        l.struct_size = sizeof l;
        REQUIRE(siaara_dict_lookup(e.tag, &l) == SIAARA_OK);
        if (std::strcmp(e.keyword, l.keyword) != 0) {
            std::printf("  entry %u tag %08X: %s looks up as %s\n", i, e.tag, e.keyword, l.keyword);
            CHECK(false);
        }
        masked += e.mask != 0xFFFFFFFFu ? 1 : 0;
    }
    CHECK(masked > 50);
    siaara_dict_entry e;
    std::memset(&e, 0, sizeof e);
    e.struct_size = sizeof e;
    REQUIRE(siaara_dict_lookup_keyword("RowsForNthOrderCoefficients", 27, &e) == SIAARA_OK);
    CHECK(e.tag == 0x00280410u);
    CHECK(e.mask == 0xFFFFFF0Fu);
    REQUIRE(siaara_dict_lookup(0x00280400u, &e) == SIAARA_OK);
    CHECK(std::string(e.keyword) == "TransformLabel");
    // Command elements retired in PS3.7 (Table E.2-1).
    REQUIRE(siaara_dict_lookup(0x00000001u, &e) == SIAARA_OK);
    CHECK(e.retired == 1);
    REQUIRE(siaara_dict_lookup(0x00000200u, &e) == SIAARA_OK);
    CHECK(e.retired == 1);
    REQUIRE(siaara_dict_lookup(0x00000100u, &e) == SIAARA_OK);  // Command Field: current
    CHECK(e.retired == 0);
    // "US or SS or OW": all three VRs.
    const dict::Entry* three = nullptr;
    for (std::size_t i = 0; i < dict::entry_count(); ++i)
        if (dict::entry_at(i)->vr_alt2 != VR::None) three = dict::entry_at(i);
    REQUIRE(three != nullptr);
    CHECK(three->vr == VR::US && three->vr_alt == VR::SS && three->vr_alt2 == VR::OW);
    CHECK(three->allows(VR::OW) && three->allows(VR::US) && !three->allows(VR::LO));
    REQUIRE(siaara_dict_lookup(three->tag, &e) == SIAARA_OK);
    CHECK(std::string(e.vr) == "US" && std::string(e.vr_alt) == "SS" && std::string(e.vr_alt2) == "OW");
    // A caller built before the struct grew is still served (and its bytes past the old end are untouched).
    siaara_dict_entry old;
    std::memset(&old, 0x5A, sizeof old);
    old.struct_size = static_cast<uint32_t>(offsetof(siaara_dict_entry, vr_alt2));
    REQUIRE(siaara_dict_lookup(three->tag, &old) == SIAARA_OK);
    CHECK(std::string(old.vr) == "US");
    CHECK(old.vr_alt2[0] == 0x5A);
    CHECK(old.mask == 0x5A5A5A5Au);
}

TEST_CASE("sec4a 6.9: pixel value tells an assumed identity from a stored rescale") {
    auto make = [](bool with_rescale) {
        DicomFile f;
        f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
        Dataset& ds = f.dataset;
        (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
        (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.41093");
        (void)ds.set_uint16(tags::SamplesPerPixel, 1);
        (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
        (void)ds.set_uint16(tags::Rows, 1);
        (void)ds.set_uint16(tags::Columns, 2);
        (void)ds.set_uint16(tags::BitsAllocated, 8);
        (void)ds.set_uint16(tags::BitsStored, 8);
        (void)ds.set_uint16(tags::HighBit, 7);
        (void)ds.set_uint16(tags::PixelRepresentation, 0);
        if (with_rescale) {
            (void)ds.set_string(tags::RescaleSlope, VR::DS, "1");
            (void)ds.set_string(tags::RescaleIntercept, VR::DS, "0");
        }
        (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>("\x07\x09"), 2));
        std::vector<std::uint8_t> bytes;
        (void)write_part10(f, bytes);
        siaara_file* h = nullptr;
        (void)siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &h);
        return h;
    };
    for (const bool with_rescale : {false, true}) {
        siaara_file* h = make(with_rescale);
        REQUIRE(h);
        siaara_pixel_value v;
        std::memset(&v, 0, sizeof v);
        v.struct_size = sizeof v;
        REQUIRE(siaara_file_pixel_value(h, 0, 1, 0, 0, &v) == SIAARA_OK);
        CHECK(v.stored == 9);
        CHECK(v.has_modality == 1);
        CHECK(v.modality_value == 9.0);
        CHECK(v.rescale_slope == 1.0 && v.rescale_intercept == 0.0);
        CHECK(v.modality_in_data == (with_rescale ? 1 : 0));
        // An older caller (struct without the appended field) is still accepted.
        siaara_pixel_value old;
        std::memset(&old, 0x5A, sizeof old);
        old.struct_size = static_cast<uint32_t>(offsetof(siaara_pixel_value, modality_in_data));
        REQUIRE(siaara_file_pixel_value(h, 0, 0, 0, 0, &old) == SIAARA_OK);
        CHECK(old.stored == 7);
        CHECK(old.modality_in_data == 0x5A5A5A5A);
        siaara_file_free(h);
    }
}
