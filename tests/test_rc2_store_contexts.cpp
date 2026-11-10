// Independent testing verdict on the release candidate, B1: the C API Storage SCU proposes the
// native transfer syntaxes (Implicit VR Little Endian among them) for every SOP class it sends -
// also for files that are Explicit VR Little Endian - and converts on send, so files reach a peer
// that accepts Implicit VR Little Endian only.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "capi/capi_net_internal.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/dimse.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";
constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";

std::filesystem::path fresh_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_rc2_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

std::string write_image(const std::filesystem::path& dir, const char* sop_class, const std::string& sop, std::string_view tsx) {
    DicomFile f;
    f.transfer_syntax = std::string(tsx);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, sop_class);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.9100900");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.9100901");
    (void)ds.set_string(tags::PatientName, VR::PN, "Implicit^Only");
    (void)ds.set_string(tags::Modality, VR::CS, std::string(sop_class) == kCT ? "CT" : "MR");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 4);
    (void)ds.set_uint16(tags::Columns, 4);
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_uint16(tags::BitsStored, 12);
    (void)ds.set_uint16(tags::HighBit, 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(32);
    for (std::size_t k = 0; k < px.size(); k += 2) {
        px[k] = static_cast<std::uint8_t>(k * 5u);
        px[k + 1] = static_cast<std::uint8_t>(k & 0x0Fu);
    }
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    const std::string path = (dir / (sop + ".dcm")).string();
    (void)write_file(f, path);
    return path;
}

bool has_context(const net::Scu& scu, const char* cls, const std::vector<std::string>& exactly) {
    for (const net::ProposedContext& p : scu.proposals())
        if (p.abstract_syntax == cls && p.transfer_syntaxes == exactly) return true;
    return false;
}

struct Received {
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> items;  // SOP Instance UID, transfer syntax received in
    std::vector<std::uint16_t> first_pixel;
};

}  // namespace

TEST_CASE("capi/ rc2 B1 the Storage SCU proposes the native syntaxes for every SOP class and reaches an Implicit-only peer") {
    const auto dir = fresh_dir("b1");
    const std::string ex = write_image(dir, kCT, "2.25.9100001", ts::ExplicitVRLittleEndian);
    const std::string im = write_image(dir, kCT, "2.25.9100002", ts::ImplicitVRLittleEndian);
    const std::string be = write_image(dir, kMR, "2.25.9100003", ts::ExplicitVRBigEndian);
    const std::string ex2 = write_image(dir, kMR, "2.25.9100004", ts::ExplicitVRLittleEndian);

    // What is proposed: per (class, syntax) the file's own; per class the native syntaxes - always.
    {
        net::Scu scu{net::ScuConfig{}};
        REQUIRE(capi::dimse::propose_store_contexts(scu, {{kCT, std::string(ts::ExplicitVRLittleEndian)}}, false) == SIAARA_OK);
        CHECK(has_context(scu, kCT, {std::string(ts::ExplicitVRLittleEndian)}));
        CHECK(has_context(scu, kCT, net::native_transfer_syntaxes()));  // was missing for an Explicit VR LE file
        bool implicit_offered = false;
        for (const net::ProposedContext& p : scu.proposals())
            for (const std::string& t : p.transfer_syntaxes) implicit_offered = implicit_offered || t == ts::ImplicitVRLittleEndian;
        CHECK(implicit_offered);
        net::Scu two{net::ScuConfig{}};
        REQUIRE(capi::dimse::propose_store_contexts(two,
                                                    {{kCT, std::string(ts::ExplicitVRLittleEndian)},
                                                     {kCT, std::string(ts::ImplicitVRLittleEndian)},
                                                     {kMR, std::string(ts::ExplicitVRBigEndian)},
                                                     {kMR, std::string(ts::ExplicitVRLittleEndian)}},
                                                    false) == SIAARA_OK);
        for (const char* cls : {kCT, kMR}) CHECK(has_context(two, cls, net::native_transfer_syntaxes()));
        CHECK(has_context(two, kCT, {std::string(ts::ImplicitVRLittleEndian)}));
        CHECK(has_context(two, kMR, {std::string(ts::ExplicitVRBigEndian)}));
    }

    // An acceptor that takes Implicit VR Little Endian only (as DCMTK "storescp +xi").
    Received got;
    net::ScpConfig cfg;
    cfg.ae_title = "IMPLICIT_ONLY";
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.allow_any_calling_ae = true;
    cfg.accept_any_sop_class = true;
    cfg.transfer_syntaxes = {std::string(ts::ImplicitVRLittleEndian)};
    net::ScpHandlers h;
    h.on_store = [&](const net::StoreRequest& rq) -> std::uint16_t {
        const std::lock_guard<std::mutex> l(got.mu);
        got.items.emplace_back(rq.sop_instance_uid, rq.transfer_syntax);
        const Element* px = rq.dataset.find(tags::PixelData);
        got.first_pixel.push_back(px && px->value.size() >= 4 ? load_le16(px->value.data() + 2) : std::uint16_t{0xFFFF});
        return net::dimse_status::Success;
    };
    net::ScpServer scp(cfg, h);
    REQUIRE(scp.start());

    siaara_dimse_peer peer;
    std::memset(&peer, 0, sizeof peer);
    peer.struct_size = sizeof peer;
    peer.host = "127.0.0.1";
    peer.port = scp.port();
    peer.called_ae = "IMPLICIT_ONLY";
    peer.calling_ae = "RC2_SCU";
    peer.connect_timeout_ms = 3000;
    peer.acse_timeout_ms = 3000;
    peer.dimse_timeout_ms = 5000;
    // Explicit, Implicit and Big Endian files of two SOP classes in one call.
    const char* paths[] = {ex.c_str(), im.c_str(), be.c_str(), ex2.c_str()};
    siaara_store_outcome out[4];
    std::memset(out, 0, sizeof out);
    for (siaara_store_outcome& o : out) o.struct_size = sizeof o;
    siaara_dimse_result r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    CHECK(siaara_dimse_store_files_report(&peer, paths, nullptr, 4, nullptr, nullptr, nullptr, out, &r) == SIAARA_OK);
    CHECK_EQ(r.completed, 4u);
    CHECK_EQ(r.failed, 0u);
    for (const siaara_store_outcome& o : out) {
        CHECK_EQ(o.error, SIAARA_OK);
        CHECK_EQ(o.dimse_status, 0x0000u);  // on the old code the Explicit VR LE files were not sent
        CHECK(std::string(o.transfer_syntax) == ts::ImplicitVRLittleEndian);
        CHECK((o.flags & SIAARA_STORE_SENT) != 0);
        CHECK((o.flags & (SIAARA_STORE_UID_CHANGED | SIAARA_STORE_LOSSY)) == 0);
    }
    // One file through the handle-based call too.
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_open(ex.c_str(), ex.size(), nullptr, &f) == SIAARA_OK);
    siaara_store_outcome one;
    std::memset(&one, 0, sizeof one);
    one.struct_size = sizeof one;
    CHECK(siaara_dimse_store_report(&peer, f, nullptr, &one, &r) == SIAARA_OK);
    CHECK_EQ(one.dimse_status, 0x0000u);
    siaara_file_free(f);
    scp.stop();
    {
        const std::lock_guard<std::mutex> l(got.mu);
        REQUIRE(got.items.size() == 5);
        for (const auto& [uid, tsx] : got.items) CHECK(tsx == ts::ImplicitVRLittleEndian);
        // The values arrived as they are in the files (the Big Endian file was byte-swapped).
        for (const std::uint16_t v : got.first_pixel) CHECK_EQ(v, 0x020Au);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
