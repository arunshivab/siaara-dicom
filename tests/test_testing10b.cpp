// Fixes for the independent tester's report of 2026-11-10 (claude/testing/results-2026-11-10b.md):
//   P1  every C-STORE path refuses a file whose read was not complete unless the caller opts in
//   P2  allow_lossy proposes the lossy transfer syntaxes the transcoder can write
//   4.3 the caller learns the SOP Instance UID each file was sent with and can commit it
// Loopback SCPs in-process (ours <-> ours).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "net/store_support.hpp"
#include "repair/repair.hpp"
#include "repair/report.hpp"
#include "repair/validate.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";
constexpr const char* kJpegBaseline = "1.2.840.10008.1.2.4.50";

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_t10b_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// An 8-bit MR image (JPEG Baseline can code it), 16 x 16, native.
DicomFile mr8(const std::string& sop) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kMR);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.10100");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.10101");
    (void)ds.set_string(tags::PatientName, VR::PN, "Ten^B");
    (void)ds.set_string(tags::PatientID, VR::LO, "T10B");
    (void)ds.set_string(tags::Modality, VR::CS, "MR");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 16);
    (void)ds.set_uint16(tags::Columns, 16);
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(256);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>((k * 7) & 0x7F);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    return f;
}

std::vector<std::uint8_t> bytes_of(const DicomFile& f) {
    std::vector<std::uint8_t> b;
    (void)write_part10(f, b);
    return b;
}

void write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& b, std::size_t drop = 0) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size() - drop));
}

// A good file and a truncated one (the last 62 pixel bytes missing, as MR_truncated.dcm).
struct Files {
    std::filesystem::path dir, good, truncated;
    explicit Files(const std::string& name) : dir(temp_dir(name)) {
        good = dir / "good.dcm";
        truncated = dir / "truncated.dcm";
        write_bytes(good, bytes_of(mr8("2.25.10111")));
        write_bytes(truncated, bytes_of(mr8("2.25.10112")), 62);
    }
};

Timeouts fast() {
    Timeouts t;
    t.connect_ms = 3000;
    t.acse_ms = 3000;
    t.artim_ms = 3000;
    t.dimse_ms = 5000;
    t.write_ms = 5000;
    t.idle_ms = 10000;
    return t;
}

ScpConfig scp_cfg(const std::string& ae) {
    ScpConfig c;
    c.ae_title = ae;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.allow_any_calling_ae = true;
    c.timeouts = fast();
    return c;
}

ScuConfig scu_cfg(std::uint16_t port, const std::string& called) {
    ScuConfig c;
    c.calling_ae = "T10B_SCU";
    c.called_ae = called;
    c.host = "127.0.0.1";
    c.port = port;
    c.timeouts = fast();
    return c;
}

struct Got10b {
    std::mutex mu;
    std::vector<std::string> uids, ts;
    std::vector<std::size_t> pixel_bytes;
    std::uint16_t operator()(const StoreRequest& rq) {
        std::lock_guard<std::mutex> l(mu);
        uids.push_back(rq.sop_instance_uid);
        ts.push_back(rq.transfer_syntax);
        const Element* px = rq.dataset.find(tags::PixelData);
        pixel_bytes.push_back(px ? px->value.size() : 0);
        return 0;
    }
    std::size_t count() {
        std::lock_guard<std::mutex> l(mu);
        return uids.size();
    }
};

siaara_dimse_peer peer(uint32_t port, const char* called) {
    siaara_dimse_peer p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.host = "127.0.0.1";
    p.port = port;
    p.called_ae = called;
    p.calling_ae = "T10B_SCU";
    p.connect_timeout_ms = 3000;
    p.acse_timeout_ms = 3000;
    p.dimse_timeout_ms = 5000;
    return p;
}

siaara_dimse_result result() {
    siaara_dimse_result r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    return r;
}

std::vector<siaara_store_outcome> outcomes(std::size_t n) {
    std::vector<siaara_store_outcome> v(n);
    for (auto& o : v) {
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------- P1: incomplete input is not sent

TEST_CASE("10b P1: the truncated test file reads as INPUT_TRUNCATED and is refused by save") {
    Files fs("p1read");
    auto f = read_file(fs.truncated.string());
    REQUIRE(f.is_ok());
    CHECK((f->read_loss & DicomFile::kReadInputTruncated) != 0);
    std::vector<std::uint8_t> out;
    CHECK(write_part10(*f, out).code == Errc::Rejected);
    CHECK(check_sendable(*f, false).code == Errc::Rejected);
    CHECK(check_sendable(*f, false).message.rfind(std::string(kIncompleteInput), 0) == 0);
    CHECK(check_sendable(*f, true).is_ok());
}

TEST_CASE("10b P1: C API store_files refuses a truncated file per file; allow_incomplete sends it") {
    Files fs("p1capi");
    Got10b got;
    ScpHandlers h;
    h.on_store = [&](const StoreRequest& rq) { return got(rq); };
    ScpServer scp(scp_cfg("T10B_SCP"), h);
    REQUIRE(scp.start().is_ok());
    const std::string a = fs.good.string(), b = fs.truncated.string();
    const char* paths[] = {a.c_str(), b.c_str()};
    siaara_dimse_peer p = peer(scp.port(), "T10B_SCP");
    auto outs = outcomes(2);
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_store_files_report(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, outs.data(), &r) ==
          SIAARA_ERR_REJECTED);
    CHECK(r.completed == 1);
    CHECK(r.failed == 1);
    CHECK(got.count() == 1);  // the peer never saw the truncated file
    CHECK(outs[0].error == SIAARA_OK);
    CHECK((outs[0].flags & SIAARA_STORE_SENT) != 0);
    CHECK(outs[0].dimse_status == 0);
    CHECK(outs[1].error == SIAARA_ERR_REJECTED);
    CHECK((outs[1].flags & SIAARA_STORE_INCOMPLETE_INPUT) != 0);
    CHECK((outs[1].flags & SIAARA_STORE_SENT) == 0);
    CHECK(outs[1].dimse_status == 0xFFFF);
    CHECK(std::string(outs[1].original_sop_instance_uid) == "2.25.10112");

    // The older entry point (no outcomes) refuses it the same way.
    r = result();
    CHECK(siaara_dimse_store_files(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.failed == 1);
    CHECK(got.count() == 2);

    // Opt in: both are sent; the peer gets the data set as read (8130 of 8192... here 194 of 256).
    p.allow_incomplete = 1;
    outs = outcomes(2);
    r = result();
    CHECK(siaara_dimse_store_files_report(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, outs.data(), &r) == SIAARA_OK);
    CHECK(r.completed == 2);
    CHECK(outs[1].error == SIAARA_OK);
    CHECK(got.count() == 4);

    // A peer struct of the first version (no allow_incomplete) is still accepted, with the default.
    siaara_dimse_peer old = peer(scp.port(), "T10B_SCP");
    old.struct_size = static_cast<uint32_t>(offsetof(siaara_dimse_peer, reserved0) + sizeof(uint32_t));
    old.allow_incomplete = 1;  // beyond struct_size: not read
    r = result();
    CHECK(siaara_dimse_store_files(&old, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.failed == 1);
    old.struct_size = 8;
    CHECK(siaara_dimse_store_files(&old, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    p.allow_incomplete = 2;
    CHECK(siaara_dimse_store_files(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);

    // In memory: siaara_dimse_store(_report) refuses before connecting.
    siaara_file* cf = nullptr;
    REQUIRE(siaara_file_open(b.c_str(), b.size(), nullptr, &cf) == SIAARA_OK);
    p.allow_incomplete = 0;
    auto one = outcomes(1);
    r = result();
    const std::size_t before = got.count();
    CHECK(siaara_dimse_store_report(&p, cf, nullptr, one.data(), &r) == SIAARA_ERR_REJECTED);
    CHECK((one[0].flags & SIAARA_STORE_INCOMPLETE_INPUT) != 0);
    CHECK(r.failed == 1);
    CHECK(siaara_dimse_store(&p, cf, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK(got.count() == before);
    p.allow_incomplete = 1;
    CHECK(siaara_dimse_store(&p, cf, nullptr, &r) == SIAARA_OK);
    CHECK(got.count() == before + 1);
    siaara_file_free(cf);
    scp.stop();
}

TEST_CASE("10b P1: C++ Scu::store and store_many refuse incomplete input unless allow_incomplete") {
    Files fs("p1cpp");
    Got10b got;
    ScpHandlers h;
    h.on_store = [&](const StoreRequest& rq) { return got(rq); };
    ScpServer scp(scp_cfg("T10B_SCP"), h);
    REQUIRE(scp.start().is_ok());
    auto trunc = read_file(fs.truncated.string());
    REQUIRE(trunc.is_ok());
    {
        Scu scu(scu_cfg(scp.port(), "T10B_SCP"));
        REQUIRE(scu.add_contexts_for_files({fs.good.string(), fs.truncated.string()}).is_ok());
        REQUIRE(scu.connect().is_ok());
        StoreSent sent;
        auto r = scu.store(*trunc, {}, std::nullopt, &sent);
        CHECK(!r.is_ok());
        CHECK(r.error().code == Errc::Rejected);
        CHECK(sent.incomplete);
        CHECK(scu.connected());  // a refused file does not end the association
        CHECK(scu.store_file(fs.truncated.string()).error().code == Errc::Rejected);
        auto ok = scu.store_file(fs.good.string());
        REQUIRE(ok.is_ok());
        CHECK(*ok == 0);
        auto many = store_many(scu, {fs.good.string(), fs.truncated.string()});
        REQUIRE(many.is_ok());
        CHECK(many->outcomes[0].error.is_ok());
        CHECK(many->outcomes[1].error.code == Errc::Rejected);
        CHECK(scu.release().is_ok());
    }
    CHECK(got.count() == 2);
    {
        ScuConfig c = scu_cfg(scp.port(), "T10B_SCP");
        c.allow_incomplete = true;
        Scu scu(c);
        REQUIRE(scu.add_contexts_for_files({fs.truncated.string()}).is_ok());
        REQUIRE(scu.connect().is_ok());
        auto r = scu.store(*trunc);
        REQUIRE(r.is_ok());
        CHECK(*r == 0);
        CHECK(scu.release().is_ok());
    }
    CHECK(got.count() == 3);
    scp.stop();
}

TEST_CASE("10b P1: C-GET and C-MOVE sub-operations from our SCP refuse incomplete files") {
    Files fs("p1retr");
    // C-GET: the sub-operation for the truncated file fails, the good one is delivered.
    ScpHandlers h;
    h.on_get = [&](const QueryRequest&, std::vector<RetrieveItem>& items) -> std::uint16_t {
        items.push_back({fs.good.string(), nullptr});
        items.push_back({fs.truncated.string(), nullptr});
        return 0;
    };
    // C-MOVE to a second SCP.
    Got10b dest_got;
    ScpHandlers dh;
    dh.on_store = [&](const StoreRequest& rq) { return dest_got(rq); };
    ScpServer dest(scp_cfg("T10B_DEST"), dh);
    REQUIRE(dest.start().is_ok());
    h.on_move = h.on_get;
    h.resolve_move_destination = [&](const std::string& ae) -> std::optional<MoveDestination> {
        if (ae != "T10B_DEST") return std::nullopt;
        MoveDestination d;
        d.host = "127.0.0.1";
        d.port = dest.port();
        d.ae_title = ae;
        return d;
    };
    ScpServer scp(scp_cfg("T10B_QR"), h);
    REQUIRE(scp.start().is_ok());
    Dataset id;
    (void)id.set_string(0x00080052, VR::CS, "STUDY");
    (void)id.set_string(tags::StudyInstanceUID, VR::UI, "2.25.10100");
    {
        Scu scu(scu_cfg(scp.port(), "T10B_QR"));
        scu.add_context(std::string(sop::StudyRootGet), native_transfer_syntaxes());
        scu.add_context(std::string(sop::StudyRootMove), native_transfer_syntaxes());
        scu.add_get_storage(kMR);
        REQUIRE(scu.connect().is_ok());
        std::vector<std::string> got;
        auto g = scu.get(sop::StudyRootGet, id, [&](const StoreRequest& rq) -> std::uint16_t {
            got.push_back(rq.sop_instance_uid);
            return 0;
        });
        REQUIRE(g.is_ok());
        CHECK(g->completed == 1);
        CHECK(g->failed == 1);
        REQUIRE(got.size() == 1);
        CHECK(got[0] == "2.25.10111");
        auto m = scu.move(sop::StudyRootMove, "T10B_DEST", id);
        REQUIRE(m.is_ok());
        CHECK(m->completed == 1);
        CHECK(m->failed == 1);
        CHECK(scu.release().is_ok());
    }
    CHECK(dest_got.count() == 1);
    scp.stop();
    dest.stop();
}

// ---------------------------------------------------------------- P2: lossy targets are proposed

TEST_CASE("10b P2: transcode_targets lists lossless and (when allowed) lossy syntaxes") {
    codec::TranscodeParams p;
    p.allow_lossy = false;
    const auto lossless = transcode_targets(p, false);
    CHECK(std::find(lossless.begin(), lossless.end(), "1.2.840.10008.1.2.4.70") != lossless.end());
    CHECK(std::find(lossless.begin(), lossless.end(), kJpegBaseline) == lossless.end());
    CHECK(transcode_targets(p, true).empty());
    p.allow_lossy = true;
    const auto lossy = transcode_targets(p, true);
    CHECK(std::find(lossy.begin(), lossy.end(), kJpegBaseline) != lossy.end());
    CHECK(std::find(lossy.begin(), lossy.end(), "1.2.840.10008.1.2.4.70") == lossy.end());
}

TEST_CASE("10b P2: a peer that accepts only JPEG Baseline gets native files (C API batch, C++, C-MOVE)") {
    Files fs("p2");
    const auto second = fs.dir / "good2.dcm";
    write_bytes(second, bytes_of(mr8("2.25.10113")));
    Got10b got;
    ScpConfig cfg = scp_cfg("ONLY50");
    cfg.accepted = {{kMR, {kJpegBaseline}, true, false}};
    ScpHandlers h;
    h.on_store = [&](const StoreRequest& rq) { return got(rq); };
    ScpServer only50(cfg, h);
    REQUIRE(only50.start().is_ok());

    // C API, a batch of native files only.
    const std::string a = fs.good.string(), b = second.string();
    const char* paths[] = {a.c_str(), b.c_str()};
    siaara_dimse_peer p = peer(only50.port(), "ONLY50");
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_store_files(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.failed == 2);  // lossy not allowed: nothing can be sent
    p.allow_lossy_transcode = 1;
    r = result();
    CHECK(siaara_dimse_store_files(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_OK);
    CHECK(r.completed == 2);
    REQUIRE(got.count() == 2);
    CHECK(got.ts[0] == kJpegBaseline);

    // C++ Scu with add_contexts_for_files.
    {
        ScuConfig c = scu_cfg(only50.port(), "ONLY50");
        c.transcode_params.allow_lossy = true;
        Scu scu(c);
        REQUIRE(scu.add_contexts_for_files({fs.good.string()}).is_ok());
        REQUIRE(scu.connect().is_ok());
        auto s = scu.store_file(fs.good.string());
        REQUIRE(s.is_ok());
        CHECK(*s == 0);
        CHECK(scu.release().is_ok());
    }
    CHECK(got.count() == 3);
    {
        Scu scu(scu_cfg(only50.port(), "ONLY50"));  // lossy not allowed: no context for MR
        REQUIRE(scu.add_contexts_for_files({fs.good.string()}).is_ok());
        REQUIRE(scu.connect().is_ok());
        CHECK(!scu.store_file(fs.good.string()).is_ok());
        CHECK(scu.release().is_ok());
    }

    // C-MOVE from our SCP to the JPEG-Baseline-only destination.
    ScpConfig qcfg = scp_cfg("T10B_QR2");
    qcfg.transcode_params.allow_lossy = true;
    ScpHandlers qh;
    qh.on_move = [&](const QueryRequest&, std::vector<RetrieveItem>& items) -> std::uint16_t {
        items.push_back({fs.good.string(), nullptr});
        return 0;
    };
    qh.resolve_move_destination = [&](const std::string&) -> std::optional<MoveDestination> {
        MoveDestination d;
        d.host = "127.0.0.1";
        d.port = only50.port();
        d.ae_title = "ONLY50";
        return d;
    };
    ScpServer qr(qcfg, qh);
    REQUIRE(qr.start().is_ok());
    {
        Scu scu(scu_cfg(qr.port(), "T10B_QR2"));
        scu.add_context(std::string(sop::StudyRootMove), native_transfer_syntaxes());
        REQUIRE(scu.connect().is_ok());
        Dataset id;
        (void)id.set_string(0x00080052, VR::CS, "STUDY");
        (void)id.set_string(tags::StudyInstanceUID, VR::UI, "2.25.10100");
        auto m = scu.move(sop::StudyRootMove, "ONLY50", id);
        REQUIRE(m.is_ok());
        CHECK(m->completed == 1);
        CHECK(m->failed == 0);
        CHECK(scu.release().is_ok());
    }
    CHECK(got.count() == 4);
    qr.stop();
    only50.stop();
}

// ---------------------------------------------------------------- 4.3: the UID sent, fed to commitment

TEST_CASE("10b 4.3: outcomes carry the UID sent after a lossy C-STORE; commitment of those succeeds") {
    Files fs("q43");
    const auto dir = temp_dir("q43store");
    siaara_store* store = nullptr;
    const std::string sp = (dir / "store").string();
    REQUIRE(siaara_store_open(sp.c_str(), sp.size(), nullptr, &store) == SIAARA_OK);
    siaara_scp_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.ae_title = "T10B_PACS";
    o.bind_address = "127.0.0.1";
    o.allow_any_calling_ae = 1;
    o.idle_timeout_ms = 10000;
    const char* only[] = {kJpegBaseline};
    siaara_scp_services sv;
    std::memset(&sv, 0, sizeof sv);
    sv.struct_size = sizeof sv;
    sv.storage = 1;
    sv.commitment = 1;
    sv.commitment_report_mode = SIAARA_COMMIT_REPORT_SAME;
    sv.transfer_syntaxes = only;
    sv.transfer_syntax_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, store, nullptr, nullptr, &scp) == SIAARA_OK);
    uint32_t port = 0;
    REQUIRE(siaara_scp_port(scp, &port) == SIAARA_OK);

    siaara_dimse_peer p = peer(port, "T10B_PACS");
    p.allow_lossy_transcode = 1;
    const std::string a = fs.good.string();
    const char* paths[] = {a.c_str()};
    auto outs = outcomes(1);
    siaara_dimse_result r = result();
    REQUIRE(siaara_dimse_store_files_report(&p, paths, nullptr, 1, nullptr, nullptr, nullptr, outs.data(), &r) == SIAARA_OK);
    CHECK((outs[0].flags & SIAARA_STORE_SENT) != 0);
    CHECK((outs[0].flags & SIAARA_STORE_LOSSY) != 0);
    CHECK((outs[0].flags & SIAARA_STORE_UID_CHANGED) != 0);
    CHECK(std::string(outs[0].original_sop_instance_uid) == "2.25.10111");
    CHECK(std::string(outs[0].sop_instance_uid) != "2.25.10111");
    CHECK(std::string(outs[0].transfer_syntax) == kJpegBaseline);
    CHECK(std::string(outs[0].sop_class_uid) == kMR);

    // Storage Commitment of what was sent: committed. Of the file's own UID: not (the peer never
    // received that instance).
    const char* classes[] = {outs[0].sop_class_uid, outs[0].sop_class_uid};
    const char* insts[] = {outs[0].sop_instance_uid, outs[0].original_sop_instance_uid};
    uint16_t reasons[2] = {0xFFFF, 0xFFFF};
    r = result();
    CHECK(siaara_dimse_commit(&p, classes, insts, 1, 5000, nullptr, reasons, &r) == SIAARA_OK);
    CHECK(reasons[0] == 0);
    r = result();
    CHECK(siaara_dimse_commit(&p, classes, insts, 2, 5000, nullptr, reasons, &r) == SIAARA_ERR_REJECTED);
    CHECK(reasons[0] == 0);
    CHECK(reasons[1] != 0);
    siaara_commit_options co;
    std::memset(&co, 0, sizeof co);
    co.struct_size = sizeof co;
    co.wait_ms = 5000;
    r = result();
    CHECK(siaara_dimse_commit_ex(&p, &co, classes, insts, 1, nullptr, reasons, &r) == SIAARA_OK);
    CHECK(r.completed == 1);

    // keep_uid_on_lossy: the file's UID is sent (and can be committed as is).
    const auto second = fs.dir / "keep.dcm";
    write_bytes(second, bytes_of(mr8("2.25.10114")));
    const std::string k = second.string();
    const char* kpaths[] = {k.c_str()};
    p.keep_uid_on_lossy = 1;
    outs = outcomes(1);
    r = result();
    REQUIRE(siaara_dimse_store_files_report(&p, kpaths, nullptr, 1, nullptr, nullptr, nullptr, outs.data(), &r) == SIAARA_OK);
    CHECK((outs[0].flags & SIAARA_STORE_LOSSY) != 0);
    CHECK((outs[0].flags & SIAARA_STORE_UID_CHANGED) == 0);
    CHECK(std::string(outs[0].sop_instance_uid) == "2.25.10114");
    const char* kclass[] = {kMR};
    const char* kinst[] = {"2.25.10114"};
    r = result();
    CHECK(siaara_dimse_commit(&p, kclass, kinst, 1, 5000, nullptr, reasons, &r) == SIAARA_OK);

    // In memory, siaara_dimse_store_report.
    siaara_file* cf = nullptr;
    const auto mem = bytes_of(mr8("2.25.10115"));
    REQUIRE(siaara_file_open_memory(mem.data(), mem.size(), nullptr, &cf) == SIAARA_OK);
    p.keep_uid_on_lossy = 0;
    auto one = outcomes(1);
    r = result();
    CHECK(siaara_dimse_store_report(&p, cf, nullptr, one.data(), &r) == SIAARA_OK);
    CHECK((one[0].flags & SIAARA_STORE_UID_CHANGED) != 0);
    CHECK(std::string(one[0].original_sop_instance_uid) == "2.25.10115");
    siaara_file_free(cf);

    // Outcome elements must be set up.
    outs = outcomes(1);
    outs[0].struct_size = 4;
    CHECK(siaara_dimse_store_files_report(&p, paths, nullptr, 1, nullptr, nullptr, nullptr, outs.data(), &r) ==
          SIAARA_ERR_INVALID_ARGUMENT);
    siaara_scp_free(scp);
    siaara_store_free(store);
}

// ---------------------------------------------------------------- item 5: defined-length fragments

namespace {

// A JPEG Lossless file whose Pixel Data is written with a defined length (items, no delimiter;
// with `keep_delimiter` the Sequence Delimitation Item stays inside the length).
bool defined_length_jpeg(bool keep_delimiter, DicomFile& native, std::vector<std::uint8_t>& b) {
    native = mr8("2.25.10120");
    DicomFile f = native;
    if (!codec::transcode_file(f, "1.2.840.10008.1.2.4.70").is_ok()) return false;
    b = bytes_of(f);
    // Find (7FE0,0010) OB 00 00 FFFFFFFF at the top level (the last occurrence).
    static constexpr std::uint8_t hdr[] = {0xE0, 0x7F, 0x10, 0x00, 'O', 'B', 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    std::size_t at = std::string::npos;
    for (std::size_t i = 0; i + sizeof hdr <= b.size(); ++i)
        if (std::memcmp(b.data() + i, hdr, sizeof hdr) == 0) at = i;
    static constexpr std::uint8_t delim[] = {0xFE, 0xFF, 0xDD, 0xE0, 0, 0, 0, 0};
    if (at == std::string::npos || b.size() < at + sizeof hdr + 8) return false;
    if (std::memcmp(b.data() + b.size() - 8, delim, 8) != 0) return false;  // Pixel Data is the last element
    if (!keep_delimiter) b.resize(b.size() - 8);
    const auto len = static_cast<std::uint32_t>(b.size() - (at + sizeof hdr));
    for (int k = 0; k < 4; ++k) b[at + 8 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(len >> (8 * k));
    return true;
}

}  // namespace

TEST_CASE("10b item 5: encapsulated Pixel Data with a defined length is read leniently and kept as read") {
    for (bool keep_delimiter : {false, true}) {
        DicomFile native;
        std::vector<std::uint8_t> in;
        REQUIRE(defined_length_jpeg(keep_delimiter, native, in));
        auto f = read_part10(Bytes::copy_of(in.data(), in.size()));
        REQUIRE(f.is_ok());
        CHECK(f->complete());
        const Element* px = f->dataset.find(tags::PixelData);
        REQUIRE(px != nullptr);
        CHECK(px->is_encapsulated());
        CHECK(px->fragments.size() == 2);  // empty Basic Offset Table + one fragment
        CHECK(px->read.fragments_defined_length == (keep_delimiter ? 2 : 1));
        bool warned = false;
        for (const std::string& w : f->warnings) warned = warned || w.rfind("PIXEL_DATA_DEFINED_LENGTH", 0) == 0;
        CHECK(warned);
        // Decodes to the original pixels.
        auto dec = codec::transcode_dataset(f->dataset, f->transfer_syntax, ts::ExplicitVRLittleEndian);
        REQUIRE(dec.is_ok());
        const Element* a = dec->find(tags::PixelData);
        const Element* b = native.dataset.find(tags::PixelData);
        REQUIRE(a && b);
        CHECK(a->value.size() == b->value.size());
        CHECK(std::memcmp(a->value.data(), b->value.data(), b->value.size()) == 0);
        // Saved byte for byte as read.
        std::vector<std::uint8_t> out;
        REQUIRE(write_part10(*f, out).is_ok());
        CHECK(out == in);
        // The validator reports it as a warning, not as native pixel data.
        auto v = repair::validate_bytes(Bytes::copy_of(in.data(), in.size()));
        REQUIRE(v.is_ok());
        bool flagged = false, native_error = false;
        for (const repair::Finding& x : *v) {
            flagged = flagged || (x.code == "pixel-defined-length" && x.severity == repair::Severity::Warning);
            native_error = native_error || x.code == "pixel-encoding";
        }
        CHECK(flagged);
        CHECK(!native_error);
        // Strict mode still refuses to interpret it as fragments (the value stays bytes).
        ReadOptions strict;
        strict.lenient = false;
        auto s = read_part10(Bytes::copy_of(in.data(), in.size()), strict);
        if (s.is_ok()) CHECK(s->dataset.find(tags::PixelData)->fragments.empty());
    }
    // Items that do not fill the length exactly: left as bytes (nothing is guessed).
    DicomFile native;
    std::vector<std::uint8_t> bad;
    REQUIRE(defined_length_jpeg(false, native, bad));
    bad.push_back(0);
    bad.push_back(0);
    for (std::size_t i = 0; i + 12 <= bad.size(); ++i)
        if (bad[i] == 0xE0 && bad[i + 1] == 0x7F && bad[i + 2] == 0x10 && bad[i + 3] == 0x00 && bad[i + 4] == 'O') {
            std::uint32_t l = 0;
            for (int k = 0; k < 4; ++k) l |= std::uint32_t{bad[i + 8 + static_cast<std::size_t>(k)]} << (8 * k);
            l += 2;
            for (int k = 0; k < 4; ++k) bad[i + 8 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(l >> (8 * k));
        }
    auto g = read_part10(Bytes::copy_of(bad.data(), bad.size()));
    REQUIRE(g.is_ok());
    CHECK(g->dataset.find(tags::PixelData)->fragments.empty());
}

// ---------------------------------------------------------------- P3 (b): repair frame counts

TEST_CASE("10b P3: repair of a truncated single frame reports counts that agree") {
    DicomFile f = mr8("2.25.10130");
    std::vector<std::uint8_t> b = bytes_of(f);
    b.resize(b.size() - 62);
    repair::RepairOptions opt;
    opt.fix = repair::kRescue;
    opt.write = true;
    std::vector<std::uint8_t> out;
    const repair::Report rep = repair::repair_buffer(Bytes::copy_of(b.data(), b.size()), &out, opt);
    REQUIRE(rep.files.size() == 1);
    const repair::FileReport& fr = rep.files[0];
    CHECK(fr.rescue_attempted);
    CHECK(fr.frames_declared == 1);
    CHECK(fr.frames_recovered.empty());
    REQUIRE(fr.frames_lost.size() == 1);
    CHECK(fr.frames_lost[0] == 0);  // 0-based index
    const std::string text = repair::to_text(rep);
    CHECK(text.find("frames: 0 of 1 recovered, 1 lost (frame 1)") != std::string::npos);
    CHECK(text.find("lost: 0") == std::string::npos);
    const std::string json = repair::to_json(rep);
    CHECK(json.find("\"recovered_count\":0,\"lost_count\":1") != std::string::npos);
}
