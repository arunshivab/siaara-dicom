// Regression tests for the independent testing chat's findings on a576a7d (2026-11-10):
//  P0-1  lenient parsing that stopped early and a save that silently wrote the truncated data set
//  P0-2  RT Dose: Dose Grid Scaling and the Grid Frame Offset Vector
//  F8    Implicit VR "US or SS" elements resolved by Pixel Representation
// Every sample is synthetic, built here byte by byte.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/writer.hpp"
#include "volume/volume.hpp"
#include "clinical/roi.hpp"
#include "dict/dictionary.hpp"
#include "image/lut.hpp"
#include "image/render.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

using Buf = std::vector<std::uint8_t>;
constexpr std::string_view kJpegLosslessTs = "1.2.840.10008.1.2.4.70";

void u16(Buf& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
void u32(Buf& b, std::uint32_t v) {
    u16(b, v & 0xFFFFu);
    u16(b, v >> 16);
}
// Explicit VR little endian element; `undefined` writes 0xFFFFFFFF as the length.
void el(Buf& b, Tag t, const char* vr, const Buf& value, bool undefined = false) {
    u16(b, tag_group(t));
    u16(b, t & 0xFFFFu);
    b.push_back(static_cast<std::uint8_t>(vr[0]));
    b.push_back(static_cast<std::uint8_t>(vr[1]));
    const std::string v(vr);
    if (v == "OB" || v == "OW" || v == "SQ" || v == "UN" || v == "UT" || v == "OF" || v == "OD") {
        u16(b, 0);
        u32(b, undefined ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value.size()));
    } else {
        u16(b, static_cast<std::uint32_t>(value.size()));
    }
    b.insert(b.end(), value.begin(), value.end());
}
void item(Buf& b, std::uint16_t e, std::uint32_t len) {
    u16(b, 0xFFFE);
    u16(b, e);
    u32(b, len);
}
Buf us(unsigned v) {
    Buf b;
    u16(b, v);
    return b;
}
Buf str(std::string_view s) {
    Buf b(s.begin(), s.end());
    if (b.size() & 1u) b.push_back(' ');
    return b;
}

// Part 10 wrapper: preamble, DICM, a minimal meta group, then `body`.
Buf part10(const Buf& body, std::string_view ts) {
    Buf meta;
    el(meta, 0x00020001, "OB", Buf{0, 1});
    el(meta, 0x00020010, "UI", [&] {
        Buf u(ts.begin(), ts.end());
        if (u.size() & 1u) u.push_back(0);
        return u;
    }());
    Buf b(128, 0);
    b.insert(b.end(), {'D', 'I', 'C', 'M'});
    el(b, 0x00020000, "UL", [&] {
        Buf v;
        u32(v, static_cast<std::uint32_t>(meta.size()));
        return v;
    }());
    b.insert(b.end(), meta.begin(), meta.end());
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

// Synthetic equivalent of gdcmData Bug_Siemens_PrivateIconNoItem.dcm: a defined-length private
// sequence whose defined-length item holds an icon Pixel Data (7FE0,0010) OB of undefined length
// with the JPEG codestream directly after the header (no items, no sequence delimiter before the
// end of the item). The Patient module and the real Pixel Data follow at top level.
Buf siemens_private_icon_no_item(std::size_t* icon_bytes = nullptr) {
    Buf icon_jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01};
    for (int i = 0; i < 300; ++i) icon_jpeg.push_back(static_cast<std::uint8_t>(i * 13 + 7));
    icon_jpeg.push_back(0xFF);
    icon_jpeg.push_back(0xD9);
    if (icon_bytes) *icon_bytes = icon_jpeg.size();
    Buf it;
    el(it, 0x00280010, "US", us(128));
    el(it, 0x00280011, "US", us(128));
    el(it, 0x7FD10010, "LO", str("GEIIS"));
    el(it, 0x7FE00010, "OB", {}, true);
    it.insert(it.end(), icon_jpeg.begin(), icon_jpeg.end());
    Buf sq;
    item(sq, 0xE000, static_cast<std::uint32_t>(it.size()));
    sq.insert(sq.end(), it.begin(), it.end());

    Buf body;
    el(body, 0x00080016, "UI", str("1.2.840.10008.5.1.4.1.1.2"));
    el(body, 0x00080060, "CS", str("CT"));
    el(body, 0x00090010, "LO", str("SIEMENS CT VA1 DUMMY"));
    el(body, 0x00090011, "LO", str("GEIIS"));
    el(body, 0x00091110, "SQ", sq);
    el(body, 0x00100010, "PN", str("DOE^JANE"));
    el(body, 0x00100020, "LO", str("PID-42"));
    el(body, 0x00200013, "IS", str("7"));
    el(body, 0x00280010, "US", us(4));
    el(body, 0x00280011, "US", us(4));
    el(body, 0x7FE00010, "OB", {}, true);
    item(body, 0xE000, 0);
    item(body, 0xE000, 4);
    body.insert(body.end(), {0xFF, 0xD8, 0xFF, 0xD9});
    item(body, 0xE0DD, 0);
    return part10(body, kJpegLosslessTs);
}

}  // namespace

// ================================================================ P0-1: lenient parse must not stop early

TEST_CASE("testing2 P0-1: private icon Pixel Data without items keeps every element after it") {
    std::size_t icon_bytes = 0;
    const Buf b = siemens_private_icon_no_item(&icon_bytes);
    auto f = read_part10(Bytes::copy_of(b.data(), b.size()));
    REQUIRE(f.is_ok());
    const Dataset& ds = f->dataset;
    CHECK_EQ(ds.size(), 11u);
    CHECK_EQ(ds.get_string(0x00100010).value_or(""), "DOE^JANE");
    CHECK_EQ(ds.get_string(0x00100020).value_or(""), "PID-42");
    CHECK_EQ(ds.get_int(0x00200013).value_or(0), 7);
    const Element* px = ds.find(0x7FE00010);
    REQUIRE(px != nullptr);
    CHECK_EQ(px->fragments.size(), 2u);
    // The icon keeps every byte of its codestream, and nothing beyond the item.
    const Element* sq = ds.find(0x00091110);
    REQUIRE(sq != nullptr && sq->items.size() == 1u);
    const Element* icon = sq->items[0].find(0x7FE00010);
    REQUIRE(icon != nullptr);
    std::size_t total = 0;
    for (const Bytes& fr : icon->fragments) total += fr.size();
    CHECK_EQ(total, icon_bytes);
    CHECK_EQ(sq->items[0].size(), 4u);
    CHECK(!f->warnings.empty());
    CHECK(f->complete());

    // A save keeps every element: write, read back, compare the top-level tags.
    std::vector<std::uint8_t> out;
    REQUIRE(write_part10(*f, out).is_ok());
    auto back = read_part10(Bytes::copy_of(out.data(), out.size()));
    REQUIRE(back.is_ok());
    CHECK_EQ(back->dataset.size(), ds.size());
    CHECK_EQ(back->dataset.get_string(0x00100010).value_or(""), "DOE^JANE");
    CHECK(back->dataset.find(0x7FE00010) != nullptr);
}

namespace {

// A small explicit VR LE file whose Pixel Data fragments are followed by 8 junk bytes instead of
// the sequence delimiter: the junk is in no element.
Buf fragments_then_junk() {
    Buf body;
    el(body, 0x00080016, "UI", str("1.2.840.10008.5.1.4.1.1.7"));
    el(body, 0x00100010, "PN", str("DOE^JANE"));
    el(body, 0x7FE00010, "OB", {}, true);
    item(body, 0xE000, 0);
    item(body, 0xE000, 4);
    body.insert(body.end(), {0xFF, 0xD8, 0xFF, 0xD9});
    body.insert(body.end(), {0x0C, 0xB0, 0xB6, 0x0E, 0x50, 0xD1, 0x02, 0x0E});
    return part10(body, kJpegLosslessTs);
}

// A file cut inside the Patient Name value.
Buf truncated_value() {
    Buf body;
    el(body, 0x00080016, "UI", str("1.2.840.10008.5.1.4.1.1.7"));
    el(body, 0x00100010, "PN", str("DOE^JANE^MIDDLE"));
    Buf b = part10(body, "1.2.840.10008.1.2.1");
    b.resize(b.size() - 5);
    return b;
}

bool starts_with(const std::string& s, std::string_view p) { return s.compare(0, p.size(), p) == 0; }

}  // namespace

TEST_CASE("testing2 P0-1: skipped bytes are a distinct first warning and a save is refused") {
    const Buf b = fragments_then_junk();
    auto f = read_part10(Bytes::copy_of(b.data(), b.size()));
    REQUIRE(f.is_ok());
    CHECK(!f->complete());
    CHECK((f->read_loss & DicomFile::kReadBytesSkipped) != 0);
    CHECK_EQ(f->bytes_not_read, 8u);
    CHECK_EQ(f->read_loss_offset, b.size() - 8);
    REQUIRE(!f->warnings.empty());
    CHECK(starts_with(f->warnings.front(), kWarnParseIncomplete));
    std::vector<std::uint8_t> out;
    const Status s = write_part10(*f, out);
    CHECK(s.code == Errc::Rejected);
    CHECK(out.empty());
    WriteOptions wo;
    wo.allow_incomplete = true;
    CHECK(write_part10(*f, out, wo).is_ok());
    // Strict mode still refuses the file outright.
    ReadOptions strict;
    strict.lenient = false;
    CHECK(!read_part10(Bytes::copy_of(b.data(), b.size()), strict).is_ok());
}

TEST_CASE("testing2 P0-1: truncated input and header-only reads are flagged and refused on save") {
    const Buf t = truncated_value();
    auto f = read_part10(Bytes::copy_of(t.data(), t.size()));
    REQUIRE(f.is_ok());
    CHECK((f->read_loss & DicomFile::kReadInputTruncated) != 0);
    REQUIRE(!f->warnings.empty());
    CHECK(starts_with(f->warnings.front(), kWarnInputTruncated));
    std::vector<std::uint8_t> out;
    CHECK(write_part10(*f, out).code == Errc::Rejected);

    const Buf c = siemens_private_icon_no_item();
    ReadOptions ho;
    ho.stop_before_pixel_data = true;
    auto h = read_part10(Bytes::copy_of(c.data(), c.size()), ho);
    REQUIRE(h.is_ok());
    CHECK(h->read_loss == DicomFile::kReadHeaderOnly);
    CHECK(h->stopped_at_offset > 0);
    CHECK_EQ(h->bytes_not_read, c.size() - h->stopped_at_offset);
    CHECK(h->warnings.size() == 1u);  // only the icon's warning: a header-only read adds none
    CHECK(write_part10(*h, out).code == Errc::Rejected);
    // An edited copy of a complete file still saves.
    auto full = read_part10(Bytes::copy_of(c.data(), c.size()));
    REQUIRE(full.is_ok());
    CHECK(full->complete());
    (void)full->dataset.set_string(0x00100010, VR::PN, "EDITED^NAME");
    CHECK(write_part10(*full, out).is_ok());
}

TEST_CASE("testing2 P0-1: C API read report, warning text and allow_incomplete") {
    const Buf b = fragments_then_junk();
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_open_memory(b.data(), b.size(), nullptr, &f) == SIAARA_OK);
    siaara_read_report r{};
    r.struct_size = sizeof r;
    REQUIRE(siaara_file_read_report(f, &r) == SIAARA_OK);
    CHECK(r.loss == SIAARA_READ_BYTES_SKIPPED);
    CHECK_EQ(r.bytes_not_read, 8u);
    CHECK(r.warning_count >= 1u);
    char text[512];
    size_t need = 0;
    REQUIRE(siaara_file_warning(f, 0, text, sizeof text, &need) == SIAARA_OK);
    CHECK(starts_with(text, "PARSE_INCOMPLETE"));
    CHECK(siaara_file_warning(f, r.warning_count, text, sizeof text, &need) == SIAARA_ERR_INVALID_ARGUMENT);
    r.struct_size = 0;
    CHECK(siaara_file_read_report(f, &r) == SIAARA_ERR_INVALID_ARGUMENT);

    const std::string path = (std::filesystem::temp_directory_path() / "siaara_testing2_incomplete.dcm").string();
    std::remove(path.c_str());
    CHECK(siaara_file_save(f, path.c_str(), path.size(), nullptr) == SIAARA_ERR_REJECTED);
    CHECK(!std::filesystem::exists(path));
    siaara_save_options so{};
    so.struct_size = offsetof(siaara_save_options, allow_incomplete);  // an older caller: default refusal
    CHECK(siaara_file_save_ex(f, path.c_str(), path.size(), nullptr, &so) == SIAARA_ERR_REJECTED);
    so.struct_size = sizeof so;
    so.allow_incomplete = 2;
    CHECK(siaara_file_save_ex(f, path.c_str(), path.size(), nullptr, &so) == SIAARA_ERR_INVALID_ARGUMENT);
    so.allow_incomplete = 1;
    CHECK(siaara_file_save_ex(f, path.c_str(), path.size(), nullptr, &so) == SIAARA_OK);
    std::remove(path.c_str());
    siaara_file_free(f);

    // A complete file reports no loss.
    const Buf c = siemens_private_icon_no_item();
    REQUIRE(siaara_file_open_memory(c.data(), c.size(), nullptr, &f) == SIAARA_OK);
    r.struct_size = sizeof r;
    REQUIRE(siaara_file_read_report(f, &r) == SIAARA_OK);
    CHECK(r.loss == 0u);
    CHECK(r.bytes_not_read == 0u);
    siaara_file_free(f);
}

// ================================================================ P0-2: RT Dose

namespace {

constexpr double kDgs = 4.574449e-05;  // Dose Grid Scaling of the samples
constexpr std::uint32_t kHot = 1861000; // stored maximum: 1861000 x 4.574449e-05 = 85.13049589 Gy

// A Varian-Eclipse-like multi-frame RT Dose: 3 x 4 pixels, unsigned 32-bit, one frame per
// `gfov` value (DS text as stored, e.g. "0\\3\\6\\11"). Stored value of frame k, row r, column c:
// 1000 (k + 1) + 100 r + c, except frame 1 row 1 column 2 = kHot.
DicomFile rt_dose(std::string_view gfov, std::uint32_t frames, bool with_dgs = true) {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& d = f.dataset;
    (void)d.set_string(0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.481.2");
    (void)d.set_string(0x00080018, VR::UI, "2.25.1234");
    (void)d.set_string(0x00080060, VR::CS, "RTDOSE");
    (void)d.set_string(0x00200032, VR::DS, "-10\\-20\\-30");
    (void)d.set_string(0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
    (void)d.set_string(0x00200052, VR::UI, "2.25.99");
    (void)d.set_uint16(0x00280002, 1);
    (void)d.set_string(0x00280004, VR::CS, "MONOCHROME2");
    (void)d.set_string(0x00280008, VR::IS, std::to_string(frames));
    (void)d.set_uint16(0x00280010, 3);
    (void)d.set_uint16(0x00280011, 4);
    (void)d.set_string(0x00280030, VR::DS, "2.5\\2.5");
    (void)d.set_uint16(0x00280100, 32);
    (void)d.set_uint16(0x00280101, 32);
    (void)d.set_uint16(0x00280102, 31);
    (void)d.set_uint16(0x00280103, 0);
    (void)d.set_string(0x30040002, VR::CS, "GY");
    (void)d.set_string(0x30040004, VR::CS, "PHYSICAL");
    (void)d.set_string(0x3004000A, VR::CS, "PLAN");
    if (!gfov.empty()) (void)d.set_string(0x3004000C, VR::DS, gfov);
    if (with_dgs) (void)d.set_string(0x3004000E, VR::DS, "4.574449e-05");
    Buf px;
    for (std::uint32_t k = 0; k < frames; ++k)
        for (std::uint32_t r = 0; r < 3; ++r)
            for (std::uint32_t c = 0; c < 4; ++c) u32(px, k == 1 && r == 1 && c == 2 ? kHot : 1000 * (k + 1) + 100 * r + c);
    (void)d.set_bytes(0x7FE00010, VR::OW, Bytes::copy_of(px.data(), px.size()));
    return f;
}

bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }

Result<volume::Volume> dose_volume(const DicomFile& f, bool strict = false) {
    const DicomFile* files[] = {&f};
    volume::VolumeOptions o;
    o.strict = strict;
    o.threads = 1;
    return volume::build_volume(files, o);
}

bool has_warning(const std::vector<std::string>& w, std::string_view part) {
    for (const auto& s : w)
        if (s.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("testing2 P0-2a: Dose Grid Scaling gives doses in Gy (modality, frame values, ROI stats)") {
    const DicomFile f = rt_dose("0\\5\\10\\15", 4);
    auto img = image::Image::open(f.dataset, f.transfer_syntax);
    REQUIRE(img.is_ok());
    auto m = img->modality(1);
    REQUIRE(m.is_ok());
    CHECK(m->dose_grid_scaling);
    CHECK_EQ(m->slope, kDgs);
    CHECK_EQ(m->intercept, 0.0);
    CHECK_EQ(m->rescale_type, "GY");
    std::vector<float> v(12);
    REQUIRE(image::decode_modality_f32(*img, 1, v).is_ok());
    CHECK(near(v[1 * 4 + 2], 85.13049589, 1e-4));  // hand computed: 1861000 x 4.574449e-05
    CHECK(near(v[0], 2000 * kDgs, 1e-7));            // frame 1, row 0, column 0: stored 2000
    // ROI statistics over row 1 of frame 1 (stored 2100, 2101, kHot, 2103).
    auto st = clinical::roi_stats(v, 3, 4, clinical::Roi::rectangle(0, 1, 4, 2));
    REQUIRE(st.is_ok());
    CHECK(near(st->max, 85.13049589, 1e-4));
    CHECK(near(st->mean, (2100.0 + 2101.0 + kHot + 2103.0) / 4.0 * kDgs, 1e-4));
    // The dose module values.
    const image::RtDoseInfo info = image::read_rt_dose_info(f.dataset);
    CHECK(info.is_rt_dose);
    CHECK(info.dose_grid_scaling && *info.dose_grid_scaling == kDgs);
    CHECK_EQ(info.dose_units, "GY");
    CHECK_EQ(info.dose_type, "PHYSICAL");
    CHECK_EQ(info.dose_summation_type, "PLAN");
    CHECK(info.offset_form == image::RtDoseInfo::OffsetForm::Relative);
    CHECK_EQ(info.offset_count, 4u);
    // Strict mode refuses an RT Dose without Dose Grid Scaling; lenient leaves stored values.
    const DicomFile nodgs = rt_dose("0\\5\\10\\15", 4, false);
    image::PixelInfo pi = img->info();
    CHECK(!image::modality_for_frame(nodgs.dataset, pi, 0, false).is_ok());
    auto lm = image::modality_for_frame(nodgs.dataset, pi, 0, true);
    REQUIRE(lm.is_ok());
    CHECK(lm->is_identity());
}

TEST_CASE("testing2 P0-2a/b: RT Dose volume holds Gy and uses the Grid Frame Offset Vector") {
    // Uniform relative vector: 5 mm frames.
    auto v = dose_volume(rt_dose("0\\5\\10\\15", 4));
    REQUIRE(v.is_ok());
    CHECK(v->type() == volume::VoxelType::Float32);
    CHECK_EQ(v->grid().dims[2], 4u);
    CHECK(near(v->grid().spacing[2], 5.0));
    CHECK(near(v->grid().origin.z, -30.0));
    CHECK(near(v->value_max, 85.13049589, 1e-4));
    CHECK(near(v->at(2, 1, 1), 85.13049589, 1e-4));
    CHECK(near(v->at(0, 0, 3), 4000 * kDgs, 1e-6));
    CHECK(!has_warning(v->warnings, "header slice spacing"));
    CHECK_EQ(v->flags & (volume::kNonUniformSpacing | volume::kMissingSlices), 0u);
    // The same grid in the absolute form (first value = IPP z = -30).
    auto a = dose_volume(rt_dose("-30\\-25\\-20\\-15", 4));
    REQUIRE(a.is_ok());
    CHECK(near(a->grid().spacing[2], 5.0));
    CHECK(near(a->grid().origin.z, -30.0));
    // 3 mm, uniform, both forms.
    auto r3 = dose_volume(rt_dose("0\\3\\6\\9\\12", 5));
    auto a3 = dose_volume(rt_dose("-30\\-27\\-24\\-21\\-18", 5));
    REQUIRE(r3.is_ok() && a3.is_ok());
    CHECK(near(r3->grid().spacing[2], 3.0));
    CHECK(near(a3->grid().spacing[2], 3.0));
    // Decreasing offsets: frames sorted along the normal, so the origin is the last frame.
    auto dn = dose_volume(rt_dose("0\\-3\\-6\\-9", 4));
    REQUIRE(dn.is_ok());
    CHECK(near(dn->grid().spacing[2], 3.0));
    CHECK(near(dn->grid().origin.z, -39.0));
    CHECK(near(dn->at(0, 0, 0), 4000 * kDgs, 1e-6));  // frame 3 is the lowest
}

TEST_CASE("testing2 P0-2b: irregular or invalid Grid Frame Offset Vectors are flagged") {
    // Non-uniform (gaps 3, 3, 5): kept in order, flagged, mean spacing 11/3; strict refuses.
    for (std::string_view g : {"0\\3\\6\\11", "-30\\-27\\-24\\-19"}) {
        auto v = dose_volume(rt_dose(g, 4));
        REQUIRE(v.is_ok());
        CHECK((v->flags & volume::kNonUniformSpacing) != 0);
        CHECK(near(v->grid().spacing[2], 11.0 / 3.0));
        CHECK(near(v->grid().origin.z, -30.0));
        CHECK(has_warning(v->warnings, "gaps from 3 to 5 mm"));
        CHECK(!dose_volume(rt_dose(g, 4), true).is_ok());
    }
    // Wrong count, not monotonic, absolute form not starting at the IPP z: lenient warns and
    // stacks with the header spacing; strict refuses.
    for (std::string_view g : {"0\\5\\10", "0\\5\\5\\10", "0\\5\\3\\10", "-31\\-26\\-21\\-16"}) {
        auto v = dose_volume(rt_dose(g, 4));
        REQUIRE(v.is_ok());
        CHECK(has_warning(v->warnings, "Grid Frame Offset Vector"));
        CHECK(has_warning(v->warnings, "header slice spacing"));
        CHECK(!dose_volume(rt_dose(g, 4), true).is_ok());
    }
    // Absolute form on non-transverse planes is refused (coronal: IOP 1\0\0\0\0\-1).
    DicomFile cor = rt_dose("-30\\-25\\-20\\-15", 4);
    (void)cor.dataset.set_string(0x00200037, VR::DS, "1\\0\\0\\0\\0\\-1");
    auto c = dose_volume(cor);
    REQUIRE(c.is_ok());
    CHECK(has_warning(c->warnings, "not transverse"));
    // ...but the relative form works for any orientation (offsets along the normal).
    DicomFile corr = rt_dose("0\\4\\8\\12", 4);
    (void)corr.dataset.set_string(0x00200037, VR::DS, "1\\0\\0\\0\\0\\-1");
    auto cr = dose_volume(corr);
    REQUIRE(cr.is_ok());
    CHECK(near(cr->grid().spacing[2], 4.0));
}

TEST_CASE("testing2 P0-2: RT Dose through the C API (frame values, info, ROI statistics, volume flag)") {
    const DicomFile d = rt_dose("0\\3\\6\\9", 4);
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(d, bytes).is_ok());
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &f) == SIAARA_OK);
    float px[12] = {};
    size_t need = 0;
    REQUIRE(siaara_frame_decode(f, 1, SIAARA_FRAME_FLOAT32_MODALITY, reinterpret_cast<uint8_t*>(px), sizeof px, &need) == SIAARA_OK);
    CHECK(near(px[6], 85.13049589, 1e-4));
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    siaara_rt_dose_info info{};
    info.struct_size = sizeof info;
    REQUIRE(siaara_rt_dose_info_get(ds, &info) == SIAARA_OK);
    CHECK(info.is_rt_dose == 1);
    CHECK(info.has_dose_grid_scaling == 1);
    CHECK_EQ(info.dose_grid_scaling, kDgs);
    CHECK(info.grid_frame_offset_form == SIAARA_GFOV_RELATIVE);
    CHECK_EQ(info.grid_frame_offset_count, 4u);
    CHECK_EQ(std::string(info.dose_units), "GY");
    CHECK_EQ(std::string(info.dose_type), "PHYSICAL");
    CHECK_EQ(std::string(info.dose_summation_type), "PLAN");
    info.struct_size = 0;
    CHECK(siaara_rt_dose_info_get(ds, &info) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_frame* fr = nullptr;
    REQUIRE(siaara_frame_open(f, 1, &fr) == SIAARA_OK);
    siaara_frame_properties fp{};
    fp.struct_size = sizeof fp;
    REQUIRE(siaara_frame_info(fr, &fp) == SIAARA_OK);
    CHECK((fp.flags & SIAARA_FRAMEF_DOSE_GRID_SCALING) != 0);
    CHECK_EQ(fp.rescale_slope, kDgs);
    CHECK(near(fp.max_value, 85.13049589, 1e-4));
    siaara_frame_free(fr);
    siaara_file_free(f);
}
