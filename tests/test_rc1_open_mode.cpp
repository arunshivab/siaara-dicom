// Release-candidate review, R-160: how a file opened by path is brought into memory
// (io::OpenMode Auto / Map / Read; siaara_read_options.open_mode). A file shortened after it was
// opened never takes the process down in the default mode, and a mapped file whose size changed
// is reported instead of being touched.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "image/frames.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

std::filesystem::path fresh_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_rc1open_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// A CT-like image whose pixel data spans many pages (256 x 256, 16 bit).
std::vector<std::uint8_t> image_bytes() {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.2");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.160001");
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.160900");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.160901");
    (void)ds.set_string(tags::PatientName, VR::PN, "Open^Mode");
    (void)ds.set_string(tags::Modality, VR::CS, "CT");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 256);
    (void)ds.set_uint16(tags::Columns, 256);
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_uint16(tags::BitsStored, 12);
    (void)ds.set_uint16(tags::HighBit, 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(256u * 256u * 2u);
    for (std::size_t k = 0; k < px.size(); k += 2) {
        px[k] = static_cast<std::uint8_t>((k * 13u) & 0xFFu);
        px[k + 1] = static_cast<std::uint8_t>((k >> 9) & 0x0Fu);
    }
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return out;
}

void write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

ReadOptions mode(io::OpenMode m) {
    ReadOptions o;
    o.open_mode = m;
    return o;
}

constexpr io::OpenMode kModes[] = {io::OpenMode::Auto, io::OpenMode::Map, io::OpenMode::Read};

}  // namespace

TEST_CASE("rc1 R-160: Auto, Map and Read give the same values, the same read report and a byte-identical save") {
    const auto dir = fresh_dir("same");
    const std::vector<std::uint8_t> whole = image_bytes();
    std::vector<std::uint8_t> cut = whole;
    cut.resize(cut.size() - 1000);  // ends inside Pixel Data: an incomplete read with a report
    write_file(dir / "whole.dcm", whole);
    write_file(dir / "cut.dcm", cut);
    CHECK(io::kDefaultOpenMode == io::OpenMode::Auto);
    CHECK_EQ(io::kDefaultOpenReadThreshold, std::uint64_t{256} << 20);
    CHECK(ReadOptions{}.open_mode == io::kDefaultOpenMode);

    auto ref = read_part10(Bytes::copy_of(whole.data(), whole.size()));
    auto ref_cut = read_part10(Bytes::copy_of(cut.data(), cut.size()));
    REQUIRE(ref);
    REQUIRE(ref_cut);
    CHECK(!ref_cut->complete());
    for (const io::OpenMode m : kModes) {
        auto f = read_file((dir / "whole.dcm").string(), mode(m));
        REQUIRE(f);
        CHECK(f->complete());
        CHECK(f->warnings == ref->warnings);
        CHECK(f->dataset.get_string(tags::PatientName).value_or("") == "Open^Mode");
        CHECK(f->dataset.size() == ref->dataset.size());
        // Zero-copy accessor: the Pixel Data value is the file's bytes.
        const Element* px = f->dataset.find(tags::PixelData);
        const Element* rpx = ref->dataset.find(tags::PixelData);
        REQUIRE(px != nullptr && rpx != nullptr);
        REQUIRE(px->value.size() == rpx->value.size());
        CHECK(std::memcmp(px->value.data(), rpx->value.data(), px->value.size()) == 0);
        // Keep-as-read save: the bytes of the file.
        std::vector<std::uint8_t> saved;
        REQUIRE(write_part10(*f, saved));
        CHECK(saved == whole);
        // Decoded pixels.
        auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
        REQUIRE(fr);
        auto d = fr->decode(0);
        REQUIRE(d);
        CHECK(d->size() == px->value.size() && std::memcmp(d->data(), px->value.data(), d->size()) == 0);

        // The incomplete file: the same loss bits, offset and warnings in every mode.
        auto c = read_file((dir / "cut.dcm").string(), mode(m));
        REQUIRE(c);
        CHECK_EQ(c->read_loss, ref_cut->read_loss);
        CHECK_EQ(c->read_loss_offset, ref_cut->read_loss_offset);
        CHECK(c->warnings == ref_cut->warnings);
        std::vector<std::uint8_t> refused;
        CHECK(!write_part10(*c, refused));  // never saved as if whole, in any mode
    }
    // Auto above the threshold maps; Read above its limit refuses, reading nothing.
    ReadOptions tiny = mode(io::OpenMode::Auto);
    tiny.open_read_threshold = 1024;
    auto mapped = read_file((dir / "whole.dcm").string(), tiny);
    REQUIRE(mapped);
    const Element* mpx = mapped->dataset.find(tags::PixelData);
    REQUIRE(mpx != nullptr);
    ReadOptions capped = mode(io::OpenMode::Read);
    capped.open_read_max_bytes = 1024;
    auto too_big = read_file((dir / "whole.dcm").string(), capped);
    CHECK(!too_big);
    CHECK(!too_big && too_big.error().code == Errc::LimitExceeded);
    mapped = Status(Errc::Internal, "closed");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc1 R-160: a file shortened after opening - the default mode is unaffected, a mapping reports Truncated") {
    const auto dir = fresh_dir("shrink");
    const std::vector<std::uint8_t> whole = image_bytes();
    const auto shrink = [&](const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::resize_file(p, 4096, ec);  // in place: the same file, most of its pages gone
        return !ec;
    };
    // Default (Auto, below the threshold) and Read: the data is owned; everything still works.
    for (const io::OpenMode m : {io::OpenMode::Auto, io::OpenMode::Read}) {
        const auto p = dir / "owned.dcm";
        write_file(p, whole);
        auto f = m == io::OpenMode::Auto ? read_file(p.string()) : read_file(p.string(), mode(m));
        REQUIRE(f);
        REQUIRE(shrink(p));
        auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
        REQUIRE(fr);
        auto d = fr->decode(0);
        REQUIRE(d);
        CHECK(d->size() == 256u * 256u * 2u);
        CHECK(std::memcmp(d->data(), whole.data() + whole.size() - d->size(), d->size()) == 0);
        std::vector<std::uint8_t> saved;
        REQUIRE(write_part10(*f, saved));
        CHECK(saved == whole);
    }
    // Map (asked for, or Auto above its threshold): the shortened file is noticed before pixel
    // data is touched. (Not a guarantee: see io::check_mapping.)
    for (int pass = 0; pass < 2; ++pass) {
        const auto p = dir / ("mapped" + std::to_string(pass) + ".dcm");
        write_file(p, whole);
        ReadOptions o = mode(pass == 0 ? io::OpenMode::Map : io::OpenMode::Auto);
        o.open_read_threshold = 1024;
        auto f = read_file(p.string(), o);
        REQUIRE(f);
        const Element* px = f->dataset.find(tags::PixelData);
        REQUIRE(px != nullptr);
        CHECK(io::check_mapping(px->value.data()));
        REQUIRE(shrink(p));
        const Status s = io::check_mapping(px->value.data());
        CHECK(!s);
        CHECK(s.code == Errc::Truncated);
        auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
        REQUIRE(!fr);  // on the old code this opened and the decode below ended the process
        CHECK(fr.error().code == Errc::Truncated);
    }
    // A file replaced by rename is another file: the mapping of the old one is whole.
    {
        const auto p = dir / "replaced.dcm";
        write_file(p, whole);
        auto f = read_file(p.string(), mode(io::OpenMode::Map));
        REQUIRE(f);
        const std::vector<std::uint8_t> small(100, 0);
        REQUIRE(io::write_file_atomic(p.string(), small.data(), small.size()));
        auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
        REQUIRE(fr);
        CHECK(fr->decode(0));
    }
    // A pointer that is in no mapping is fine.
    CHECK(io::check_mapping(whole.data()));
    CHECK(io::check_mapping(nullptr));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc1 R-160: C API open_mode - default AUTO, READ with a size limit, older struct sizes") {
    const auto dir = fresh_dir("capi");
    const std::vector<std::uint8_t> whole = image_bytes();
    const auto p = dir / "a.dcm";
    write_file(p, whole);
    const std::string path = p.string();
    // NULL options: AUTO. The file can be shortened afterwards and the frame still decodes.
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_open(path.c_str(), path.size(), nullptr, &f) == SIAARA_OK);
    std::error_code ec;
    std::filesystem::resize_file(p, 4096, ec);
    REQUIRE(!ec);
    std::vector<std::uint8_t> out(256u * 256u * 2u);
    size_t need = 0;
    CHECK(siaara_frame_decode(f, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) == SIAARA_OK);
    CHECK(std::memcmp(out.data(), whole.data() + whole.size() - out.size(), out.size()) == 0);
    siaara_file_free(f);
    write_file(p, whole);
    siaara_read_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.allow_raw_dataset = 1;
    o.open_mode = SIAARA_OPEN_READ;
    o.open_read_max_bytes = 1000;
    f = nullptr;
    CHECK(siaara_file_open(path.c_str(), path.size(), &o, &f) == SIAARA_ERR_LIMIT_EXCEEDED);
    CHECK(f == nullptr);
    o.open_read_max_bytes = 0;  // the default limit
    REQUIRE(siaara_file_open(path.c_str(), path.size(), &o, &f) == SIAARA_OK);
    siaara_file_free(f);
    // MAP: the shortened file is reported when a frame is asked for.
    o.open_mode = SIAARA_OPEN_MAP;
    REQUIRE(siaara_file_open(path.c_str(), path.size(), &o, &f) == SIAARA_OK);
    std::filesystem::resize_file(p, 4096, ec);
    REQUIRE(!ec);
    CHECK(siaara_frame_decode(f, 0, SIAARA_FRAME_NATIVE, out.data(), out.size(), &need) == SIAARA_ERR_TRUNCATED);
    siaara_file_free(f);
    // A caller built before the field existed: struct_size stops before open_mode.
    write_file(p, whole);
    o.open_mode = 77;
    o.struct_size = static_cast<uint32_t>(offsetof(siaara_read_options, open_mode));
    REQUIRE(siaara_file_open(path.c_str(), path.size(), &o, &f) == SIAARA_OK);
    siaara_file_free(f);
    std::filesystem::remove_all(dir, ec);
}
