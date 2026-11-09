// Test-only helpers for the store, scan and DICOMDIR tests and the store benchmark:
// synthetic DICOM instances and self-cleaning temporary folders.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"

namespace store_synth {

struct Spec {
    std::string charset;  // (0008,0005); empty = default repertoire
    std::string patient_id = "PID1", patient_name = "Doe^Jane", birth = "19700101", sex = "F";
    std::string study_uid = "1.2.3.1", study_date = "20200115", study_time = "101500", accession = "ACC1";
    std::string study_desc = "CT Chest", referring = "Ref^Doc", study_id = "S1";
    std::string series_uid = "1.2.3.1.1", modality = "CT", series_desc = "Axial", body_part = "CHEST";
    std::string sop_uid = "1.2.3.1.1.1";
    std::string sop_class = "1.2.840.10008.5.1.4.1.1.2";  // CT Image Storage
    int series_number = 1, instance_number = 1;
    bool has_instance_number = true;
    double z = 0.0;
    std::uint16_t rows = 4, columns = 4;
    bool pixels = true;
    std::string transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
    bool preamble = true;
    std::vector<std::tuple<siaara::Tag, siaara::VR, std::string>> extra;  // more string elements
    bool concept_name = false;  // add a Concept Name Code Sequence (for SR)
};

inline void put_text(siaara::Dataset& ds, siaara::Tag tag, siaara::VR vr, const std::string& utf8) {
    if (siaara::charset::vr_uses_character_set(vr)) {
        (void)siaara::charset::set_utf8(ds, tag, vr, utf8);
    } else {
        (void)ds.set_string(tag, vr, utf8);
    }
}

inline siaara::DicomFile make_file(const Spec& s) {
    using siaara::VR;
    siaara::DicomFile f;
    f.transfer_syntax = s.transfer_syntax;
    siaara::Dataset& ds = f.dataset;
    if (!s.charset.empty()) (void)ds.set_string(0x00080005, VR::CS, s.charset);
    (void)ds.set_string(0x00080008, VR::CS, "ORIGINAL\\PRIMARY\\AXIAL");
    (void)ds.set_string(0x00080016, VR::UI, s.sop_class);
    (void)ds.set_string(0x00080018, VR::UI, s.sop_uid);
    (void)ds.set_string(0x00080020, VR::DA, s.study_date);
    (void)ds.set_string(0x00080030, VR::TM, s.study_time);
    put_text(ds, 0x00080050, VR::SH, s.accession);
    (void)ds.set_string(0x00080060, VR::CS, s.modality);
    put_text(ds, 0x00080090, VR::PN, s.referring);
    put_text(ds, 0x00081030, VR::LO, s.study_desc);
    put_text(ds, 0x0008103E, VR::LO, s.series_desc);
    put_text(ds, 0x00100010, VR::PN, s.patient_name);
    put_text(ds, 0x00100020, VR::LO, s.patient_id);
    (void)ds.set_string(0x00100030, VR::DA, s.birth);
    (void)ds.set_string(0x00100040, VR::CS, s.sex);
    (void)ds.set_string(0x00180015, VR::CS, s.body_part);
    (void)ds.set_string(0x00180050, VR::DS, "1.5");
    (void)ds.set_string(0x0020000D, VR::UI, s.study_uid);
    (void)ds.set_string(0x0020000E, VR::UI, s.series_uid);
    put_text(ds, 0x00200010, VR::SH, s.study_id);
    (void)ds.set_string(0x00200011, VR::IS, std::to_string(s.series_number));
    if (s.has_instance_number) (void)ds.set_string(0x00200013, VR::IS, std::to_string(s.instance_number));
    char ipp[64];
    std::snprintf(ipp, sizeof ipp, "-10\\-20\\%g", s.z);
    (void)ds.set_string(0x00200032, VR::DS, ipp);
    (void)ds.set_string(0x00200037, VR::DS, "1\\0\\0\\0\\1\\0");
    (void)ds.set_string(0x00200052, VR::UI, s.study_uid + ".9");
    for (const auto& [tag, vr, value] : s.extra) put_text(ds, tag, vr, value);
    if (s.concept_name) {
        siaara::Dataset code;
        (void)code.set_string(0x00080100, VR::SH, "18748-4");
        (void)code.set_string(0x00080102, VR::SH, "LN");
        (void)code.set_string(0x00080104, VR::LO, "Diagnostic Imaging Report");
        siaara::Element sq;
        sq.tag = 0x0040A043;
        sq.vr = VR::SQ;
        sq.items.push_back(std::move(code));
        ds.put(std::move(sq));
    }
    if (s.pixels) {
        (void)ds.set_uint16(0x00280002, 1);
        (void)ds.set_string(0x00280004, VR::CS, "MONOCHROME2");
        (void)ds.set_uint16(0x00280010, s.rows);
        (void)ds.set_uint16(0x00280011, s.columns);
        (void)ds.set_string(0x00280030, VR::DS, "0.5\\0.6");
        (void)ds.set_uint16(0x00280100, 16);
        (void)ds.set_uint16(0x00280101, 12);
        (void)ds.set_uint16(0x00280102, 11);
        (void)ds.set_uint16(0x00280103, 0);
        std::vector<std::uint8_t> px(static_cast<std::size_t>(s.rows) * s.columns * 2u);
        for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(0xA0u + (i & 15u));
        (void)ds.set_bytes(0x7FE00010, VR::OW, siaara::Bytes::copy_of(px.data(), px.size()));
    }
    return f;
}

inline std::vector<std::uint8_t> make_bytes(const Spec& s) {
    const siaara::DicomFile f = make_file(s);
    std::vector<std::uint8_t> out;
    if (s.preamble) (void)siaara::write_part10(f, out);
    else (void)siaara::write_dataset(f.dataset, s.transfer_syntax, out);
    return out;
}

inline bool write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& b) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    return ok;
}

inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path& p) {
    std::vector<std::uint8_t> b;
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return b;
    std::uint8_t buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

inline bool write_spec(const std::filesystem::path& p, const Spec& s) { return write_bytes(p, make_bytes(s)); }

// A unique folder under the system temp folder, removed on destruction.
class TempDir {
public:
    explicit TempDir(const char* tag = "siaara") {
        std::random_device rd;
        std::error_code ec;
        path_ = std::filesystem::temp_directory_path(ec) /
                (std::string(tag) + "-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }
    std::filesystem::path operator/(const std::string& s) const { return path_ / s; }

private:
    std::filesystem::path path_;
};

}  // namespace store_synth
