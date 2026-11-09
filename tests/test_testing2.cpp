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

#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "net/services.hpp"
#include "volume/volume.hpp"
#include "web/dicom_json.hpp"
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
    int32_t code = -1;
    REQUIRE(siaara_file_warning_code(f, 0, &code) == SIAARA_OK);
    CHECK(code == SIAARA_FILE_WARNING_PARSE_INCOMPLETE);
    if (r.warning_count > 1) {
        REQUIRE(siaara_file_warning_code(f, 1, &code) == SIAARA_OK);
        CHECK(code == SIAARA_FILE_WARNING_GENERAL);
    }
    CHECK(siaara_file_warning_code(f, r.warning_count, &code) == SIAARA_ERR_INVALID_ARGUMENT);
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

// ================================================================ F8: "US or SS" by Pixel Representation

namespace {

constexpr std::string_view kImplicitTs = "1.2.840.10008.1.2";

// Implicit VR little endian element.
void iel(Buf& b, Tag t, const Buf& value) {
    u16(b, tag_group(t));
    u16(b, t & 0xFFFFu);
    u32(b, static_cast<std::uint32_t>(value.size()));
    b.insert(b.end(), value.begin(), value.end());
}
Buf ss(int v) { return us(static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)))); }
Buf words(std::initializer_list<int> v) {
    Buf b;
    for (int x : v) u16(b, static_cast<std::uint32_t>(static_cast<std::uint16_t>(x)));
    return b;
}

// Implicit VR CT-like data set: Pixel Representation `pr`, Smallest Image Pixel Value -2000,
// Largest 3000, Pixel Padding -2000 (stored as their 16-bit patterns), a Modality LUT whose
// descriptor is {40000 entries, first mapped -1024, 16 bits}, and an Icon Image Sequence item
// with its own Pixel Representation 0 and Smallest Image Pixel Value 63536 (unsigned).
Buf us_ss_sample(int pr) {
    Buf icon;
    iel(icon, 0x00280103, us(0));
    iel(icon, 0x00280106, us(63536));
    Buf icon_sq;
    item(icon_sq, 0xE000, static_cast<std::uint32_t>(icon.size()));
    icon_sq.insert(icon_sq.end(), icon.begin(), icon.end());
    Buf lut;
    iel(lut, 0x00283002, words({40000, -1024, 16}));
    iel(lut, 0x00283006, words({0, 1, 2, 3}));
    Buf lut_sq;
    item(lut_sq, 0xE000, static_cast<std::uint32_t>(lut.size()));
    lut_sq.insert(lut_sq.end(), lut.begin(), lut.end());
    Buf b;
    iel(b, 0x00080016, str("1.2.840.10008.5.1.4.1.1.2"));
    iel(b, 0x00280103, us(static_cast<unsigned>(pr)));
    iel(b, 0x00280106, ss(-2000));
    iel(b, 0x00280107, us(3000));
    iel(b, 0x00280120, ss(-2000));
    iel(b, 0x00283000, lut_sq);
    iel(b, 0x00880200, icon_sq);
    return b;
}

}  // namespace

TEST_CASE("testing2 F8: implicit VR US or SS values follow Pixel Representation (getters, items, scope)") {
    const Buf b = us_ss_sample(1);
    auto ds = read_dataset(Bytes::copy_of(b.data(), b.size()), kImplicitTs);
    REQUIRE(ds.is_ok());
    CHECK_EQ(ds->get_int(0x00280106).value_or(0), -2000);
    CHECK_EQ(ds->get_int(0x00280107).value_or(0), 3000);
    CHECK_EQ(ds->get_int(0x00280120).value_or(0), -2000);
    CHECK_EQ(ds->get_double(0x00280106).value_or(0), -2000.0);
    CHECK_EQ(ds->get_string(0x00280120).value_or(""), "-2000");
    const Element* e = ds->find(0x00280106);
    REQUIRE(e != nullptr);
    CHECK(e->vr == VR::SS);
    CHECK(e->read.vr_inferred);
    CHECK(e->value.size() == 2 && e->value.data()[0] == 0x30 && e->value.data()[1] == 0xF8);  // bytes unchanged
    // LUT Descriptor in an item inherits Pixel Representation; entries and bits stay unsigned.
    const Element* ml = ds->find(0x00283000);
    REQUIRE(ml != nullptr && ml->items.size() == 1u);
    CHECK_EQ(ml->items[0].get_int(0x00283002, 0).value_or(0), 40000);
    CHECK_EQ(ml->items[0].get_int(0x00283002, 1).value_or(0), -1024);
    CHECK_EQ(ml->items[0].get_int(0x00283002, 2).value_or(0), 16);
    // The icon item has its own Pixel Representation 0.
    const Element* icon = ds->find(0x00880200);
    REQUIRE(icon != nullptr && icon->items.size() == 1u);
    CHECK_EQ(icon->items[0].get_int(0x00280106).value_or(0), 63536);
    // Pixel Representation 0: the same bytes are unsigned.
    const Buf b0 = us_ss_sample(0);
    auto d0 = read_dataset(Bytes::copy_of(b0.data(), b0.size()), kImplicitTs);
    REQUIRE(d0.is_ok());
    CHECK_EQ(d0->get_int(0x00280106).value_or(0), 63536);
    CHECK_EQ(d0->get_int(0x00280120).value_or(0), 63536);
    CHECK(d0->find(0x00280106)->vr == VR::US);
    CHECK_EQ(d0->find(0x00283000)->items[0].get_int(0x00283002, 1).value_or(0), 64512);
}

TEST_CASE("testing2 F8: explicit VR keeps the stored VR; SS LUT descriptors keep unsigned counts") {
    // Explicit VR as stored, as pydicom: US stays US even with Pixel Representation 1.
    Buf b;
    el(b, 0x00280103, "US", us(1));
    el(b, 0x00280106, "US", ss(-2000));
    el(b, 0x00280107, "SS", ss(-5));
    auto ds = read_dataset(Bytes::copy_of(b.data(), b.size()), "1.2.840.10008.1.2.1");
    REQUIRE(ds.is_ok());
    CHECK_EQ(ds->get_int(0x00280106).value_or(0), 63536);
    CHECK(!ds->find(0x00280106)->read.vr_inferred);
    CHECK_EQ(ds->get_int(0x00280107).value_or(0), -5);
    // An SS LUT Descriptor with 40000 entries: entries unsigned, first mapped signed.
    Dataset d;
    (void)d.set_bytes(0x00283002, VR::SS, Bytes::copy_of(words({40000, -1024, 16}).data(), 6));
    CHECK_EQ(d.get_int(0x00283002, 0).value_or(0), 40000);
    CHECK_EQ(d.get_int(0x00283002, 1).value_or(0), -1024);
    CHECK_EQ(d.get_int(0x00283002, 2).value_or(0), 16);
}

TEST_CASE("testing2 F8: DICOM JSON, C API and writing keep the signed reading and the bytes") {
    const Buf body = us_ss_sample(1);
    const Buf file = [&] {
        Buf meta;
        el(meta, 0x00020001, "OB", Buf{0, 1});
        el(meta, 0x00020010, "UI", str("1.2.840.10008.1.2"));
        Buf b(128, 0);
        b.insert(b.end(), {'D', 'I', 'C', 'M'});
        Buf gl;
        u32(gl, static_cast<std::uint32_t>(meta.size()));
        el(b, 0x00020000, "UL", gl);
        b.insert(b.end(), meta.begin(), meta.end());
        b.insert(b.end(), body.begin(), body.end());
        return b;
    }();
    auto f = read_part10(Bytes::copy_of(file.data(), file.size()));
    REQUIRE(f.is_ok());
    std::string json;
    REQUIRE(web::dataset_to_json(f->dataset, json).is_ok());
    CHECK(json.find("\"00280106\":{\"vr\":\"SS\",\"Value\":[-2000]}") != std::string::npos);
    CHECK(json.find("\"00280120\":{\"vr\":\"SS\",\"Value\":[-2000]}") != std::string::npos);
    // Unedited implicit VR save: byte for byte.
    std::vector<std::uint8_t> out;
    REQUIRE(write_part10(*f, out).is_ok());
    CHECK(out == file);
    // Explicit VR save: the resolved VR is written (SS), the value bytes are the same.
    DicomFile ex = *f;
    ex.transfer_syntax = "1.2.840.10008.1.2.1";
    REQUIRE(write_part10(ex, out).is_ok());
    auto back = read_part10(Bytes::copy_of(out.data(), out.size()));
    REQUIRE(back.is_ok());
    CHECK(back->dataset.find(0x00280106)->vr == VR::SS);
    CHECK_EQ(back->dataset.get_int(0x00280106).value_or(0), -2000);
    // C API.
    siaara_file* cf = nullptr;
    REQUIRE(siaara_file_open_memory(file.data(), file.size(), nullptr, &cf) == SIAARA_OK);
    siaara_dataset* cds = nullptr;
    REQUIRE(siaara_file_dataset(cf, &cds) == SIAARA_OK);
    int64_t v = 0;
    REQUIRE(siaara_dataset_get_int64(cds, 0x00280106, 0, &v) == SIAARA_OK);
    CHECK_EQ(v, -2000);
    REQUIRE(siaara_dataset_get_int64(cds, 0x00280120, 0, &v) == SIAARA_OK);
    CHECK_EQ(v, -2000);
    siaara_dataset* lut = nullptr;
    REQUIRE(siaara_dataset_item(cds, 0x00283000, 0, &lut) == SIAARA_OK);
    REQUIRE(siaara_dataset_get_int64(lut, 0x00283002, 1, &v) == SIAARA_OK);
    CHECK_EQ(v, -1024);
    REQUIRE(siaara_dataset_get_int64(lut, 0x00283002, 0, &v) == SIAARA_OK);
    CHECK_EQ(v, 40000);
    char vr[3] = {};
    REQUIRE(siaara_dataset_vr(cds, 0x00280106, vr) == SIAARA_OK);
    CHECK_EQ(std::string(vr), "SS");
    siaara_file_free(cf);
}

TEST_CASE("testing2 F8: an implicit VR LUT descriptor takes its sign from the caller's rule") {
    // VOI LUT in an implicit VR file with Pixel Representation 0: the descriptor's VR was
    // inferred, so its first mapped value is signed when the modality output can be negative
    // (C.11.2.1.1), e.g. CT with Rescale Intercept -1024.
    Buf lut;
    iel(lut, 0x00283002, words({4, -1024, 16}));
    iel(lut, 0x00283006, words({0, 1, 2, 3}));
    Buf sq;
    item(sq, 0xE000, static_cast<std::uint32_t>(lut.size()));
    sq.insert(sq.end(), lut.begin(), lut.end());
    Buf b;
    iel(b, 0x00280103, us(0));
    iel(b, 0x00283010, sq);
    auto ds = read_dataset(Bytes::copy_of(b.data(), b.size()), kImplicitTs);
    REQUIRE(ds.is_ok());
    const Dataset& it = ds->find(0x00283010)->items[0];
    auto s = image::read_lut(it, true);
    auto u = image::read_lut(it, false);
    REQUIRE(s.is_ok() && u.is_ok());
    CHECK_EQ(s->first_mapped, -1024);
    CHECK_EQ(u->first_mapped, 64512);
}

TEST_CASE("testing2 RT Dose P2: text elements stored as UN are read with their dictionary VR") {
    // pydicom rtdose_rle.dcm stores IPP, IOP, Pixel Spacing, SOP Class, Modality, the Grid Frame
    // Offset Vector and Dose Grid Scaling as UN in explicit VR. Before, the volume build failed
    // with "Image Orientation (Patient) is not a pair of orthogonal unit vectors".
    DicomFile f = rt_dose("0\\5\\10\\15", 4);
    for (Tag t : {Tag{0x00080016}, Tag{0x00080060}, Tag{0x00200032}, Tag{0x00200037}, Tag{0x00280030},
                  Tag{0x00280008}, Tag{0x30040002}, Tag{0x3004000C}, Tag{0x3004000E}})
        f.dataset.find(t)->vr = VR::UN;
    auto v = dose_volume(f);
    REQUIRE(v.is_ok());
    CHECK(near(v->grid().spacing[2], 5.0));
    CHECK(near(v->grid().spacing[0], 2.5));
    CHECK(near(v->value_max, 85.13049589, 1e-4));
    CHECK_EQ(f.dataset.value_multiplicity(0x3004000C), 4u);
    CHECK_EQ(f.dataset.get_double(0x3004000E).value_or(0), kDgs);
    CHECK_EQ(f.dataset.get_int(0x00280008).value_or(0), 4);
    CHECK(f.dataset.find(0x3004000E)->vr == VR::UN);  // the element itself is unchanged
    // A private UN element keeps the byte reading.
    (void)f.dataset.set_bytes(0x00091001, VR::UN, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>("12"), 2));
    CHECK_EQ(f.dataset.get_int(0x00091001).value_or(0), '1');
}

TEST_CASE("testing2: the worklist query asks for Specific Character Set") {
    const Dataset q = net::make_worklist_query("CT1", "CT", "");
    const Element* cs = q.find(0x00080005);
    REQUIRE(cs != nullptr);
    CHECK(cs->value.empty());
    // A Latin-1 entry matches and its response says ISO_IR 100.
    Dataset item;
    (void)item.set_string(0x00080005, VR::CS, "ISO_IR 100");
    const std::string name = "M\xFCLLER^J\xD6RG";  // ISO 8859-1 bytes
    (void)item.set_bytes(tags::PatientName, VR::PN, Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    Dataset sps;
    (void)sps.set_string(0x00400001, VR::AE, "CT1");
    (void)sps.set_string(tags::Modality, VR::CS, "CT");
    Element seq;
    seq.tag = 0x00400100;
    seq.vr = VR::SQ;
    seq.items.push_back(std::move(sps));
    item.put(std::move(seq));
    REQUIRE(net::query_matches(q, item));
    const Dataset r = net::query_response(q, item);
    CHECK_EQ(r.get_string(0x00080005).value_or(""), "ISO_IR 100");
}

TEST_CASE("testing2: a private UN sequence is written as SQ in Explicit VR Big Endian") {
    // Implicit VR: (0029,1140) private, undefined length -> read as a UN sequence.
    Buf inner;
    iel(inner, 0x00290010, str("SIEMENS MEDCOM HEADER"));
    iel(inner, 0x00291041, str("SOM 5"));
    Buf body;
    iel(body, 0x00080016, str("1.2.840.10008.5.1.4.1.1.2"));
    iel(body, 0x00290010, str("SIEMENS MEDCOM HEADER"));
    u16(body, 0x0029);
    u16(body, 0x1140);
    u32(body, 0xFFFFFFFFu);
    item(body, 0xE000, 0xFFFFFFFFu);
    body.insert(body.end(), inner.begin(), inner.end());
    item(body, 0xE00D, 0);
    item(body, 0xE0DD, 0);
    auto ds = read_dataset(Bytes::copy_of(body.data(), body.size()), kImplicitTs);
    REQUIRE(ds.is_ok());
    REQUIRE(ds->find(0x00291140) && ds->find(0x00291140)->read.un_sequence);
    std::vector<std::uint8_t> le, be;
    REQUIRE(write_dataset(*ds, "1.2.840.10008.1.2.1", le).is_ok());
    REQUIRE(write_dataset(*ds, "1.2.840.10008.1.2.2", be).is_ok());
    auto find_vr = [](const std::vector<std::uint8_t>& b, std::uint8_t g0, std::uint8_t g1, std::uint8_t e0, std::uint8_t e1) {
        for (std::size_t i = 0; i + 6 <= b.size(); ++i)
            if (b[i] == g0 && b[i + 1] == g1 && b[i + 2] == e0 && b[i + 3] == e1) return std::string{char(b[i + 4]), char(b[i + 5])};
        return std::string();
    };
    CHECK_EQ(find_vr(le, 0x29, 0x00, 0x40, 0x11), "UN");  // little endian: kept as read (PS3.5 6.2.2)
    CHECK_EQ(find_vr(be, 0x00, 0x29, 0x11, 0x40), "SQ");  // big endian: a normal SQ
    auto back = read_dataset(Bytes::copy_of(be.data(), be.size()), "1.2.840.10008.1.2.2");
    REQUIRE(back.is_ok());
    const Element* sq = back->find(0x00291140);
    REQUIRE(sq != nullptr && sq->items.size() == 1u);
    CHECK_EQ(sq->items[0].get_string(0x00291041).value_or(""), "SOM 5");
}

TEST_CASE("testing2 P2: LibIDO ACR-NEMA Rows and Columns are swapped back (lenient, flagged)") {
    Dataset d;
    (void)d.set_string(0x00080010, VR::SH, "ACRNEMA_LIBIDO_1.1");
    (void)d.set_uint16(0x00280002, 1);
    (void)d.set_string(0x00280004, VR::CS, "MONOCHROME2");
    (void)d.set_uint16(0x00280010, 4);  // as LibIDO wrote it: really the number of columns
    (void)d.set_uint16(0x00280011, 2);
    (void)d.set_uint16(0x00280100, 8);
    (void)d.set_uint16(0x00280101, 8);
    (void)d.set_uint16(0x00280102, 7);
    (void)d.set_uint16(0x00280103, 0);
    const Buf px = {1, 2, 3, 4, 5, 6, 7, 8};
    (void)d.set_bytes(0x7FE00010, VR::OW, Bytes::copy_of(px.data(), px.size()));
    auto pi = image::read_pixel_info(d);
    REQUIRE(pi.is_ok());
    CHECK_EQ(pi->rows, 2u);
    CHECK_EQ(pi->columns, 4u);
    CHECK(has_warning(pi->warnings, "LibIDO"));
    CHECK_EQ(d.get_int(0x00280010).value_or(0), 4);  // the data set is unchanged
    image::PixelInfoOptions strict;
    strict.lenient = false;
    auto ps = image::read_pixel_info(d, strict);
    REQUIRE(ps.is_ok());
    CHECK_EQ(ps->rows, 4u);
    (void)d.set_string(0x00080010, VR::SH, "ACR-NEMA 2.0");
    auto other = image::read_pixel_info(d);
    REQUIRE(other.is_ok());
    CHECK_EQ(other->rows, 4u);
}

// ================================================================ lossy transcode: new image (owner decision 2026-11-10)

namespace {

DicomFile gray_image() {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& d = f.dataset;
    (void)d.set_string(0x00080008, VR::CS, "ORIGINAL\\PRIMARY\\AXIAL");
    (void)d.set_string(0x00080016, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)d.set_string(0x00080018, VR::UI, "2.25.111");
    (void)d.set_uint16(0x00280002, 1);
    (void)d.set_string(0x00280004, VR::CS, "MONOCHROME2");
    (void)d.set_uint16(0x00280010, 16);
    (void)d.set_uint16(0x00280011, 16);
    (void)d.set_uint16(0x00280100, 8);
    (void)d.set_uint16(0x00280101, 8);
    (void)d.set_uint16(0x00280102, 7);
    (void)d.set_uint16(0x00280103, 0);
    Buf px(256);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>((i * 37u + i / 16u * 11u) & 0xFFu);
    (void)d.set_bytes(0x7FE00010, VR::OB, Bytes::copy_of(px.data(), px.size()));
    (void)f.meta.set_string(0x00020003, VR::UI, "2.25.111");
    return f;
}

std::string purpose_of(const Dataset& ref) {
    const Element* p = ref.find(0x0040A170);
    if (!p || p->items.empty()) return {};
    return p->items[0].get_string(0x00080100).value_or("");
}

}  // namespace

TEST_CASE("testing2 lossy: a lossy transcode is a new derived image that references its predecessor") {
    DicomFile f = gray_image();
    codec::TranscodeParams p;
    p.allow_lossy = true;
    codec::TranscodeReport rep;
    REQUIRE(codec::transcode_file(f, "1.2.840.10008.1.2.4.50", p, &rep).is_ok());
    const std::string uid1 = f.dataset.get_string(0x00080018).value_or("");
    CHECK(uid1 != "2.25.111");
    CHECK(uid1.rfind("2.25.", 0) == 0);
    CHECK_EQ(rep.new_sop_instance_uid, uid1);
    CHECK_EQ(f.meta.get_string(0x00020003).value_or(""), uid1);
    CHECK_EQ(f.dataset.get_string(0x00080008, 0).value_or(""), "DERIVED");
    CHECK_EQ(f.dataset.get_string(0x00080008, 1).value_or(""), "PRIMARY");
    CHECK_EQ(f.dataset.get_string(0x00080008, 2).value_or(""), "AXIAL");
    CHECK_EQ(f.dataset.get_string(0x00282110).value_or(""), "01");
    CHECK_EQ(f.dataset.get_string(0x00282114).value_or(""), "ISO_10918_1");
    const Element* src = f.dataset.find(0x00082112);
    REQUIRE(src != nullptr && src->items.size() == 1u);
    CHECK_EQ(src->items[0].get_string(0x00081155).value_or(""), "2.25.111");
    CHECK_EQ(src->items[0].get_string(0x00081150).value_or(""), "1.2.840.10008.5.1.4.1.1.7");
    CHECK_EQ(purpose_of(src->items[0]), "121320");  // Uncompressed predecessor (CID 7202)
    const Element* der = f.dataset.find(0x00089215);
    REQUIRE(der != nullptr && der->items.size() == 1u);
    CHECK_EQ(der->items[0].get_string(0x00080100).value_or(""), "113040");  // Lossy Compression (CID 7203)
    CHECK(f.dataset.get_string(0x00082111).value_or("").find("ISO_10918_1") != std::string::npos);

    // Second lossy step: history appended, the intermediate referenced as a lossy predecessor.
    codec::TranscodeParams p2 = p;
    p2.lossy = true;
    REQUIRE(codec::transcode_file(f, "1.2.840.10008.1.2.4.91", p2).is_ok());
    const std::string uid2 = f.dataset.get_string(0x00080018).value_or("");
    CHECK(uid2 != uid1);
    CHECK_EQ(f.meta.get_string(0x00020003).value_or(""), uid2);
    CHECK_EQ(f.dataset.value_multiplicity(0x00282114), 2u);
    CHECK_EQ(f.dataset.get_string(0x00282114, 1).value_or(""), "ISO_15444_1");
    CHECK_EQ(f.dataset.value_multiplicity(0x00282112), 2u);
    src = f.dataset.find(0x00082112);
    REQUIRE(src != nullptr && src->items.size() == 2u);
    CHECK_EQ(src->items[1].get_string(0x00081155).value_or(""), uid1);
    CHECK_EQ(purpose_of(src->items[1]), "121330");  // Lossy compressed predecessor
    CHECK_EQ(f.dataset.find(0x00089215)->items.size(), 2u);
    const std::string desc = f.dataset.get_string(0x00082111).value_or("");
    CHECK(desc.find("ISO_10918_1") != std::string::npos && desc.find("; Lossy compression ISO_15444_1") != std::string::npos);

    // Lossless transcodes keep the UID and add nothing.
    DicomFile g = gray_image();
    REQUIRE(codec::transcode_file(g, "1.2.840.10008.1.2.4.70", p).is_ok());
    CHECK_EQ(g.dataset.get_string(0x00080018).value_or(""), "2.25.111");
    CHECK(!g.dataset.contains(0x00082112));
    CHECK_EQ(g.dataset.get_string(0x00080008, 0).value_or(""), "ORIGINAL");
    // ... and so does a lossless step after the lossy ones (the history stays).
    REQUIRE(codec::transcode_file(f, "1.2.840.10008.1.2.1", p).is_ok());
    CHECK_EQ(f.dataset.get_string(0x00080018).value_or(""), uid2);
    CHECK_EQ(f.dataset.get_string(0x00282110).value_or(""), "01");

    // keep_uid_on_lossy: the UID stays, the lossy attributes are still recorded.
    DicomFile k = gray_image();
    codec::TranscodeParams pk = p;
    pk.keep_uid_on_lossy = true;
    REQUIRE(codec::transcode_file(k, "1.2.840.10008.1.2.4.50", pk).is_ok());
    CHECK_EQ(k.dataset.get_string(0x00080018).value_or(""), "2.25.111");
    CHECK(!k.dataset.contains(0x00082112));
    CHECK_EQ(k.dataset.get_string(0x00282110).value_or(""), "01");
}

TEST_CASE("testing2 lossy: C API keep_uid_on_lossy (struct_size versioned)") {
    const DicomFile base = gray_image();
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(base, bytes).is_ok());
    auto run = [&](uint32_t size, int32_t keep, std::string* uid) -> siaara_status {
        siaara_file* f = nullptr;
        if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &f) != SIAARA_OK) return SIAARA_ERR_INTERNAL;
        siaara_transcode_params tp{};
        tp.struct_size = size;
        tp.allow_lossy = 1;
        tp.keep_uid_on_lossy = keep;
        const siaara_status st = siaara_file_transcode(f, "1.2.840.10008.1.2.4.50", &tp);
        siaara_dataset* ds = nullptr;
        (void)siaara_file_dataset(f, &ds);
        char buf[80] = {};
        size_t need = 0;
        (void)siaara_dataset_get_string(ds, 0x00080018, 0, buf, sizeof buf, &need);
        *uid = buf;
        siaara_file_free(f);
        return st;
    };
    std::string uid;
    CHECK(run(sizeof(siaara_transcode_params), 0, &uid) == SIAARA_OK);
    CHECK(uid != "2.25.111");
    CHECK(run(sizeof(siaara_transcode_params), 1, &uid) == SIAARA_OK);
    CHECK_EQ(uid, "2.25.111");
    CHECK(run(static_cast<uint32_t>(offsetof(siaara_transcode_params, keep_uid_on_lossy)), 1, &uid) == SIAARA_OK);
    CHECK(uid != "2.25.111");  // an older caller: the default
    CHECK(run(sizeof(siaara_transcode_params), 2, &uid) == SIAARA_ERR_INVALID_ARGUMENT);
}
