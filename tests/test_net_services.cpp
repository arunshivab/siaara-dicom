// Loopback tests (127.0.0.1, ephemeral ports) for the services of net/services.hpp, on-the-fly
// transcoding (net/store_support.hpp), spooling to disk (net/spool.hpp), the asynchronous
// operations window and the 0211 reply for unknown commands.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "image/frames.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "net/socket.hpp"
#include "net/spool.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

constexpr const char* kSC = "1.2.840.10008.5.1.4.1.1.7";
constexpr const char* kExplicit = "1.2.840.10008.1.2.1";
constexpr const char* kJls = "1.2.840.10008.1.2.4.80";
constexpr const char* kJ2k = "1.2.840.10008.1.2.4.90";
constexpr const char* kBaseline = "1.2.840.10008.1.2.4.50";

Timeouts fast() {
    Timeouts t;
    t.connect_ms = 5000;
    t.acse_ms = 5000;
    t.dimse_ms = 5000;
    t.artim_ms = 2000;
    t.idle_ms = 10000;
    return t;
}

ScpConfig scp_cfg(const std::string& ae = "SVC_SCP") {
    ScpConfig c;
    c.ae_title = ae;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.max_associations = 4;
    c.timeouts = fast();
    return c;
}

ScuConfig scu_cfg(std::uint16_t port, const std::string& called = "SVC_SCP", const std::string& calling = "SVC_SCU") {
    ScuConfig c;
    c.calling_ae = calling;
    c.called_ae = called;
    c.host = "127.0.0.1";
    c.port = port;
    c.timeouts = fast();
    return c;
}

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_svc_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// An 8-bit grayscale secondary capture image of rows x cols.
DicomFile make_image(int i, std::uint16_t rows = 16, std::uint16_t cols = 16) {
    DicomFile f;
    f.transfer_syntax = kExplicit;
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kSC);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.555" + std::to_string(i));
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.444");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.333");
    (void)ds.set_string(tags::PatientID, VR::LO, "SVC");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, rows);
    (void)ds.set_uint16(tags::Columns, cols);
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(static_cast<std::size_t>(rows) * cols);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>((k * 3 + static_cast<std::size_t>(i)) % 251);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    return f;
}

std::vector<std::uint8_t> decoded(const Dataset& ds, std::string_view ts) {
    Result<image::FrameReader> fr = image::FrameReader::open(ds, ts);
    if (!fr) return {};
    Result<std::vector<std::uint8_t>> d = fr->decode(0);
    return d ? d.value() : std::vector<std::uint8_t>{};
}

template <class Pred>
bool wait_for(Pred p, int ms = 5000) {
    for (int i = 0; i < ms / 10; ++i) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

struct StoreSink {
    std::mutex mu;
    std::vector<StoreRequest> got;
    std::vector<std::string> spool_paths;
    std::string commit_to;  // when set, spooled files are committed here (+ instance UID)
    std::uint16_t operator()(const StoreRequest& rq) {
        std::lock_guard<std::mutex> l(mu);
        if (rq.spool) {
            spool_paths.push_back(rq.spool->path());
            if (!commit_to.empty()) (void)rq.spool->commit(commit_to + "/" + rq.sop_instance_uid + ".dcm");
        }
        got.push_back(rq);
        return dimse_status::Success;
    }
};

}  // namespace

// ===================================================================== transcoding on the network

TEST_CASE("net/services: SCU transcodes to the only transfer syntax the SCP accepts") {
    StoreSink rec;
    ScpConfig cfg = scp_cfg();
    cfg.accepted = {{kSC, {kJls}, true, false}};
    ScpHandlers h;
    h.on_store = std::ref(rec);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    const DicomFile f = make_image(1, 20, 18);
    {
        Scu scu(scu_cfg(scp.port()));
        scu.add_context(kSC, {kExplicit, kJls});
        REQUIRE(scu.connect().is_ok());
        Result<std::uint16_t> st = scu.store(f);
        REQUIRE(st.is_ok());
        CHECK_EQ(*st, dimse_status::Success);
        (void)scu.release();
    }
    {
        std::lock_guard<std::mutex> l(rec.mu);
        REQUIRE(rec.got.size() == 1);
        CHECK_EQ(rec.got[0].transfer_syntax, std::string(kJls));
        CHECK(decoded(rec.got[0].dataset, kJls) == decoded(f.dataset, kExplicit));
    }
    // Disabled transcoding: the instance cannot be sent.
    ScuConfig sc = scu_cfg(scp.port());
    sc.transcode = false;
    Scu scu(sc);
    scu.add_context(kSC, {kExplicit, kJls});
    REQUIRE(scu.connect().is_ok());
    Result<std::uint16_t> st = scu.store(f);
    CHECK(!st.is_ok() && st.error().code == Errc::Unsupported);
    (void)scu.release();
}

TEST_CASE("net/services: lossy transcoding only when allowed") {
    StoreSink rec;
    ScpConfig cfg = scp_cfg();
    cfg.accepted = {{kSC, {kBaseline}, true, false}};
    ScpHandlers h;
    h.on_store = std::ref(rec);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    const DicomFile f = make_image(2);
    {
        Scu scu(scu_cfg(scp.port()));
        scu.add_context(kSC, {kExplicit, kBaseline});
        REQUIRE(scu.connect().is_ok());
        Result<std::uint16_t> st = scu.store(f);
        CHECK(!st.is_ok());
        (void)scu.release();
    }
    ScuConfig sc = scu_cfg(scp.port());
    sc.transcode_params.allow_lossy = true;
    Scu scu(sc);
    scu.add_context(kSC, {kExplicit, kBaseline});
    REQUIRE(scu.connect().is_ok());
    Result<std::uint16_t> st = scu.store(f);
    REQUIRE(st.is_ok());
    CHECK_EQ(*st, dimse_status::Success);
    (void)scu.release();
    std::lock_guard<std::mutex> l(rec.mu);
    REQUIRE(rec.got.size() == 1);
    CHECK_EQ(rec.got[0].transfer_syntax, std::string(kBaseline));
    CHECK_EQ(rec.got[0].dataset.get_string(0x00282110).value_or(""), std::string("01"));
}

TEST_CASE("net/services: SCP stores in its preferred transfer syntax") {
    StoreSink rec;
    ScpConfig cfg = scp_cfg();
    cfg.preferred_store_ts = kJ2k;
    ScpHandlers h;
    h.on_store = std::ref(rec);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    const DicomFile f = make_image(3, 24, 30);
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(kSC, {kExplicit});
    REQUIRE(scu.connect().is_ok());
    Result<std::uint16_t> st = scu.store(f);
    REQUIRE(st.is_ok());
    CHECK_EQ(*st, dimse_status::Success);
    (void)scu.release();
    std::lock_guard<std::mutex> l(rec.mu);
    REQUIRE(rec.got.size() == 1);
    const StoreRequest& r = rec.got[0];
    CHECK_EQ(r.transfer_syntax, std::string(kJ2k));
    CHECK(decoded(r.dataset, kJ2k) == decoded(f.dataset, kExplicit));
    // the raw bytes are the J2K encoding and parse as such
    Result<Dataset> again = read_dataset(r.raw, kJ2k);
    REQUIRE(again.is_ok());
    CHECK(again->find(tags::PixelData)->is_encapsulated());
    std::vector<std::uint8_t> p10;
    REQUIRE(encode_received_part10(r, p10).is_ok());
    Result<DicomFile> rf = read_part10(Bytes::copy_of(p10.data(), p10.size()));
    REQUIRE(rf.is_ok());
    CHECK_EQ(rf->transfer_syntax, std::string(kJ2k));
}

// ===================================================================== spooling

TEST_CASE("net/services: SCP spools large C-STORE data sets to disk and commits them") {
    const std::filesystem::path spool = temp_dir("spool");
    const std::filesystem::path dest = temp_dir("spool_dest");
    StoreSink rec;
    rec.commit_to = dest.string();
    ScpConfig cfg = scp_cfg();
    cfg.spool = SpoolConfig{};
    cfg.spool->threshold = 4096;
    cfg.spool->directory = spool.string();
    cfg.limits.max_receive_pdu = 4096;  // many fragments
    ScpHandlers h;
    h.on_store = std::ref(rec);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    const DicomFile big = make_image(4, 200, 180);
    const DicomFile small = make_image(5, 8, 8);
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(kSC, {kExplicit});
    REQUIRE(scu.connect().is_ok());
    CHECK_EQ(scu.store(big).value(), dimse_status::Success);
    CHECK_EQ(scu.store(small).value(), dimse_status::Success);
    // The uncommitted case: a handler that keeps nothing leaves no file behind.
    (void)scu.release();
    std::lock_guard<std::mutex> l(rec.mu);
    REQUIRE(rec.got.size() == 2);
    REQUIRE(rec.got[0].spool != nullptr);
    CHECK(rec.got[1].spool == nullptr);
    CHECK(rec.got[0].spool->committed());
    const std::string final_path = (dest / ("2.25.5554.dcm")).string();
    CHECK_EQ(rec.got[0].spool->path(), final_path);
    // The committed file is a complete Part 10 file with the received data set.
    Result<DicomFile> rf = read_file(final_path);
    REQUIRE(rf.is_ok());
    CHECK(decoded(rf->dataset, rf->transfer_syntax) == decoded(big.dataset, kExplicit));
    CHECK(decoded(rec.got[0].dataset, kExplicit) == decoded(big.dataset, kExplicit));
    CHECK_EQ(rf->meta.get_string(tags::MediaStorageSOPInstanceUID).value_or(""), std::string("2.25.5554"));
    // nothing left in the spool directory
    CHECK(std::filesystem::is_empty(spool));
}

TEST_CASE("net/services: uncommitted spool files are removed; spooled + preferred syntax") {
    const std::filesystem::path spool = temp_dir("spool2");
    StoreSink rec;
    ScpConfig cfg = scp_cfg();
    cfg.spool = SpoolConfig{};
    cfg.spool->threshold = 1000;
    cfg.spool->directory = spool.string();
    cfg.preferred_store_ts = kJls;
    ScpHandlers h;
    h.on_store = std::ref(rec);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    const DicomFile big = make_image(6, 100, 100);
    {
        Scu scu(scu_cfg(scp.port()));
        scu.add_context(kSC, {kExplicit});
        REQUIRE(scu.connect().is_ok());
        CHECK_EQ(scu.store(big).value(), dimse_status::Success);
        (void)scu.release();
    }
    std::string path;
    {
        std::lock_guard<std::mutex> l(rec.mu);
        REQUIRE(rec.got.size() == 1);
        REQUIRE(rec.got[0].spool != nullptr);
        CHECK_EQ(rec.got[0].transfer_syntax, std::string(kJls));
        path = rec.got[0].spool->path();
        Result<DicomFile> rf = read_file(path);
        REQUIRE(rf.is_ok());
        CHECK_EQ(rf->transfer_syntax, std::string(kJls));
        CHECK(decoded(rf->dataset, kJls) == decoded(big.dataset, kExplicit));
        rec.got.clear();  // drop the last reference
    }
    CHECK(wait_for([&] { return !std::filesystem::exists(path); }));
    CHECK(wait_for([&] { return std::filesystem::is_empty(spool); }));
}

TEST_CASE("net/services: C-GET receiver spools to disk") {
    const std::filesystem::path src = temp_dir("get_src");
    const std::filesystem::path spool = temp_dir("get_spool");
    const std::filesystem::path dest = temp_dir("get_dest");
    const DicomFile big = make_image(7, 128, 128);
    REQUIRE(write_file(big, (src / "a.dcm").string()).is_ok());
    ScpHandlers h;
    h.on_get = [&](const QueryRequest&, std::vector<RetrieveItem>& items) -> std::uint16_t {
        items.push_back({(src / "a.dcm").string(), nullptr});
        return dimse_status::Success;
    };
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    ScuConfig sc = scu_cfg(scp.port());
    sc.spool = SpoolConfig{};
    sc.spool->threshold = 2048;
    sc.spool->directory = spool.string();
    Scu scu(sc);
    scu.add_context(std::string(sop::StudyRootGet), native_transfer_syntaxes());
    scu.add_get_storage(kSC);
    REQUIRE(scu.connect().is_ok());
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, "STUDY");
    (void)q.set_string(tags::StudyInstanceUID, VR::UI, "2.25.444");
    bool spooled = false;
    Result<RetrieveResult> r = scu.get(sop::StudyRootGet, q, [&](const StoreRequest& rq) -> std::uint16_t {
        spooled = rq.spool != nullptr;
        if (rq.spool) (void)rq.spool->commit((dest / "got.dcm").string());
        return dimse_status::Success;
    });
    REQUIRE(r.is_ok());
    CHECK_EQ(r->status, dimse_status::Success);
    CHECK_EQ(r->completed, 1);
    CHECK(spooled);
    (void)scu.release();
    Result<DicomFile> got = read_file((dest / "got.dcm").string());
    REQUIRE(got.is_ok());
    CHECK(decoded(got->dataset, got->transfer_syntax) == decoded(big.dataset, kExplicit));
}

// ===================================================================== unknown command

TEST_CASE("net/services: unknown DIMSE command gets 0211 and the association survives") {
    ScpServer scp(scp_cfg(), {});
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    scu.add_verification();
    REQUIRE(scu.connect().is_ok());
    Association& a = *scu.association();
    const NegotiatedContext* pc = a.find_context(sop::Verification);
    REQUIRE(pc);
    Command c;
    c.command_field = 0x0055;  // not a DIMSE command
    c.message_id = a.next_message_id();
    c.affected_sop_class_uid = std::string(sop::Verification);
    REQUIRE(send_dimse(a, pc->id, c).is_ok());
    Result<DimseReceived> r = receive_dimse(a, 5000);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->message.command.command_field, 0x8055);
    CHECK_EQ(r->message.command.status.value_or(0), dimse_status::UnrecognizedOperation);
    CHECK_EQ(r->message.command.message_id_being_responded_to.value_or(0), *c.message_id);
    Result<std::uint16_t> e = scu.echo();
    REQUIRE(e.is_ok());
    CHECK_EQ(*e, dimse_status::Success);
    CHECK(scu.release().is_ok());
}

// ===================================================================== storage commitment

TEST_CASE("net/services: storage commitment, report on the same association") {
    auto svc = std::make_shared<ServiceSet>();
    svc->commitment_report = CommitmentReportMode::SameAssociation;
    svc->on_commitment = [](const CommitmentRequest& rq) {
        CommitmentResult r;
        r.retrieve_ae = "SVC_SCP";
        for (const SopRef& x : rq.items) {
            if (x.sop_instance == "2.25.2") r.failed.push_back({x, 0x0112});
            else r.committed.push_back(x);
        }
        return r;
    };
    ScpHandlers h;
    h.services = svc;
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(std::string(sop::StorageCommitmentPushModel), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    const std::vector<SopRef> items = {{kSC, "2.25.1"}, {kSC, "2.25.2"}, {kSC, "2.25.3"}};
    Result<std::string> txn = request_commitment(scu, items);
    REQUIRE(txn.is_ok());
    Result<CommitmentResult> res = wait_commitment_result(scu, 5000);
    REQUIRE(res.is_ok());
    CHECK_EQ(res->transaction_uid, *txn);
    CHECK_EQ(res->event_type(), 2);
    CHECK_EQ(res->committed.size(), 2u);
    REQUIRE(res->failed.size() == 1);
    CHECK_EQ(res->failed[0].ref.sop_instance, std::string("2.25.2"));
    CHECK_EQ(res->failed[0].reason, 0x0112);
    CHECK_EQ(res->retrieve_ae, std::string("SVC_SCP"));
    CHECK(scu.release().is_ok());
}

TEST_CASE("net/services: storage commitment, report on a new association with role reversal") {
    // The commitment requestor's own listener.
    std::mutex mu;
    std::condition_variable cv;
    std::optional<CommitmentResult> got;
    std::string from_ae;
    auto lsvc = std::make_shared<ServiceSet>();
    lsvc->on_commitment_report = [&](const std::string& ae, const CommitmentResult& r) -> std::uint16_t {
        std::lock_guard<std::mutex> l(mu);
        got = r;
        from_ae = ae;
        cv.notify_all();
        return dimse_status::Success;
    };
    ScpHandlers lh;
    lh.services = lsvc;
    ScpServer listener(scp_cfg("SVC_SCU"), lh);
    REQUIRE(listener.start().is_ok());

    auto svc = std::make_shared<ServiceSet>();
    svc->commitment_report = CommitmentReportMode::NewAssociation;
    svc->on_commitment = [](const CommitmentRequest& rq) {
        CommitmentResult r;
        r.committed = rq.items;
        return r;
    };
    const std::uint16_t lport = listener.port();
    svc->resolve_ae = [lport](const std::string& ae) -> std::optional<MoveDestination> {
        if (ae != "SVC_SCU") return std::nullopt;
        return MoveDestination{"127.0.0.1", lport, ""};
    };
    ScpHandlers h;
    h.services = svc;
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(std::string(sop::StorageCommitmentPushModel), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    Result<std::string> txn = request_commitment(scu, {{kSC, "2.25.10"}});
    REQUIRE(txn.is_ok());
    CHECK(scu.release().is_ok());
    std::unique_lock<std::mutex> l(mu);
    REQUIRE(cv.wait_for(l, std::chrono::seconds(10), [&] { return got.has_value(); }));
    CHECK_EQ(got->transaction_uid, *txn);
    CHECK_EQ(got->event_type(), 1);
    CHECK_EQ(got->committed.size(), 1u);
    CHECK_EQ(from_ae, std::string("SVC_SCP"));
}

// ===================================================================== MPPS, IAN

TEST_CASE("net/services: MPPS N-CREATE / N-SET with the state rules") {
    auto svc = std::make_shared<ServiceSet>();
    svc->mpps = std::make_shared<MppsStore>();
    ScpHandlers h;
    h.services = svc;
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(std::string(sop::ModalityPerformedProcedureStep), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    Dataset attrs;
    (void)attrs.set_string(0x00400252, VR::CS, "IN PROGRESS");
    (void)attrs.set_string(0x00400253, VR::SH, "PPS1");
    (void)attrs.set_string(tags::Modality, VR::CS, "CT");
    Result<Scu::NResponse> c = mpps_create(scu, "2.25.77", attrs);
    REQUIRE(c.is_ok());
    CHECK_EQ(c->command.status.value_or(1), dimse_status::Success);
    CHECK_EQ(c->command.affected_sop_instance_uid, std::string("2.25.77"));
    CHECK_EQ(mpps_create(scu, "2.25.77", attrs)->command.status.value_or(0), dimse_status::DuplicateSopInstance);
    Dataset bad;
    (void)bad.set_string(0x00400252, VR::CS, "COMPLETED");
    CHECK_EQ(mpps_create(scu, "2.25.78", bad)->command.status.value_or(0), dimse_status::InvalidAttributeValue);
    // SCP-assigned instance UID
    Result<Scu::NResponse> c2 = scu.n_request(make_n_create_rq(0, sop::ModalityPerformedProcedureStep, "", true), &attrs);
    REQUIRE(c2.is_ok());
    CHECK_EQ(c2->command.status.value_or(1), dimse_status::Success);
    CHECK(c2->command.affected_sop_instance_uid.rfind("2.25.", 0) == 0);
    Dataset done;
    (void)done.set_string(0x00400252, VR::CS, "COMPLETED");
    CHECK_EQ(mpps_set(scu, "2.25.77", done)->command.status.value_or(1), dimse_status::Success);
    CHECK_EQ(mpps_set(scu, "2.25.77", done)->command.status.value_or(0), dimse_status::ProcessingFailure);
    CHECK_EQ(mpps_set(scu, "2.25.99", done)->command.status.value_or(0), dimse_status::NoSuchSopInstance);
    CHECK(scu.release().is_ok());
    CHECK_EQ(svc->mpps->size(), 2u);
    CHECK_EQ(svc->mpps->get("2.25.77")->get_string(0x00400252).value_or(""), std::string("COMPLETED"));
}

TEST_CASE("net/services: Instance Availability Notification") {
    std::mutex mu;
    std::vector<std::string> seen;
    auto svc = std::make_shared<ServiceSet>();
    svc->on_instance_availability = [&](const std::string& ae, const std::string& uid, const Dataset& ds) -> std::uint16_t {
        std::lock_guard<std::mutex> l(mu);
        seen.push_back(ae + "|" + uid + "|" + ds.get_string(tags::StudyInstanceUID).value_or(""));
        return dimse_status::Success;
    };
    ScpHandlers h;
    h.services = svc;
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(std::string(sop::InstanceAvailabilityNotification), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    Dataset ian;
    (void)ian.set_string(tags::StudyInstanceUID, VR::UI, "2.25.444");
    Result<Scu::NResponse> r = send_instance_availability(scu, ian, "2.25.31");
    REQUIRE(r.is_ok());
    CHECK_EQ(r->command.status.value_or(1), dimse_status::Success);
    CHECK(scu.release().is_ok());
    std::lock_guard<std::mutex> l(mu);
    REQUIRE(seen.size() == 1);
    CHECK_EQ(seen[0], std::string("SVC_SCU|2.25.31|2.25.444"));
}

// ===================================================================== MWL and matching

namespace {

Dataset worklist_item(const char* name, const char* id, const char* station, const char* modality, const char* date,
                      const char* time) {
    Dataset sps;
    (void)sps.set_string(0x00400001, VR::AE, station);
    (void)sps.set_string(0x00400002, VR::DA, date);
    (void)sps.set_string(0x00400003, VR::TM, time);
    (void)sps.set_string(tags::Modality, VR::CS, modality);
    (void)sps.set_string(0x00400009, VR::SH, "SPS1");
    Dataset d;
    (void)d.set_string(tags::PatientName, VR::PN, name);
    (void)d.set_string(tags::PatientID, VR::LO, id);
    (void)d.set_string(0x00080050, VR::SH, "ACC1");
    Element e;
    e.tag = 0x00400100;
    e.vr = VR::SQ;
    e.items.push_back(sps);
    d.put(std::move(e));
    return d;
}

}  // namespace

TEST_CASE("net/services: query matching rules (PS3.4 C.2.2.2)") {
    const Dataset c = worklist_item("DOE^JOHN", "P1", "CT1", "CT", "20261109", "093000");
    auto q = [](std::string_view station, std::string_view mod, std::string_view date, std::string_view name = {}) {
        return make_worklist_query(station, mod, date, name);
    };
    CHECK(query_matches(q("", "", ""), c));
    CHECK(query_matches(q("CT1", "CT", "20261109"), c));
    CHECK(!query_matches(q("CT2", "", ""), c));
    CHECK(query_matches(q("", "", "20261101-20261130"), c));
    CHECK(query_matches(q("", "", "-20261109"), c));
    CHECK(query_matches(q("", "", "20261109-"), c));
    CHECK(!query_matches(q("", "", "20261110-"), c));
    CHECK(query_matches(q("C?1", "", "", "DOE*"), c));
    CHECK(!query_matches(q("", "", "", "doe*"), c));
    MatchOptions fuzzy;
    fuzzy.fuzzy_person_names = true;
    CHECK(query_matches(q("", "", "", "doe^john"), c, fuzzy));
    Dataset uids;
    (void)uids.set_string(tags::SOPInstanceUID, VR::UI, "1.2\\1.3");
    Dataset ci;
    (void)ci.set_string(tags::SOPInstanceUID, VR::UI, "1.3");
    CHECK(query_matches(uids, ci));
    (void)ci.set_string(tags::SOPInstanceUID, VR::UI, "1.4");
    CHECK(!query_matches(uids, ci));
    Dataset tm;
    (void)tm.set_string(0x00080030, VR::TM, "0900-1000");
    Dataset tc;
    (void)tc.set_string(0x00080030, VR::TM, "09:30:00");
    CHECK(query_matches(tm, tc));
    // the response carries exactly the requested keys
    const Dataset r = query_response(q("CT1", "", ""), c);
    CHECK_EQ(r.get_string(tags::PatientName).value_or(""), std::string("DOE^JOHN"));
    CHECK(r.contains(0x00321060));  // requested but absent: returned empty
    const Element* sps = r.find(0x00400100);
    REQUIRE(sps && sps->items.size() == 1);
    CHECK_EQ(sps->items[0].get_string(0x00400002).value_or(""), std::string("20261109"));
    CHECK(sps->items[0].contains(0x00400007));
    // hierarchical vs relational key checks
    Dataset h;
    (void)h.set_string(0x00080052, VR::CS, "SERIES");
    CHECK_EQ(check_query_identifier(sop::StudyRootFind, h, false, false), dimse_status::IdentifierDoesNotMatch);
    CHECK_EQ(check_query_identifier(sop::StudyRootFind, h, true, false), dimse_status::Success);
    (void)h.set_string(tags::StudyInstanceUID, VR::UI, "1.2.3");
    CHECK_EQ(check_query_identifier(sop::StudyRootFind, h, false, false), dimse_status::Success);
    CHECK_EQ(check_query_identifier(sop::StudyRootMove, h, false, true), dimse_status::IdentifierDoesNotMatch);
    CHECK(make_uid().rfind("2.25.", 0) == 0);
    CHECK(make_uid() != make_uid());
    CHECK(make_uid().size() <= 64);
}

TEST_CASE("net/services: Modality Worklist C-FIND") {
    const std::vector<Dataset> db = {worklist_item("DOE^JOHN", "P1", "CT1", "CT", "20261109", "093000"),
                                     worklist_item("ROE^JANE", "P2", "CT1", "CT", "20261110", "101500"),
                                     worklist_item("POE^ANN", "P3", "MR1", "MR", "20261109", "120000")};
    ScpHandlers h;
    h.on_find = [&](const QueryRequest& q, const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
        if (q.sop_class_uid != sop::ModalityWorklistFind) return dimse_status::SopClassNotSupported;
        for (const Dataset& d : db)
            if (query_matches(q.identifier, d) && !emit(query_response(q.identifier, d))) break;
        return dimse_status::Success;
    };
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    scu.add_context(std::string(sop::ModalityWorklistFind), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    std::vector<std::string> names;
    auto collect = [&](const Dataset& d) {
        names.push_back(d.get_string(tags::PatientName).value_or(""));
        return true;
    };
    Result<FindResult> r = scu.find(sop::ModalityWorklistFind, make_worklist_query("CT1", "CT", ""), collect);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->status, dimse_status::Success);
    CHECK_EQ(names.size(), 2u);
    names.clear();
    r = scu.find(sop::ModalityWorklistFind, make_worklist_query("", "", "20261109"), collect);
    REQUIRE(r.is_ok());
    CHECK_EQ(names.size(), 2u);
    names.clear();
    r = scu.find(sop::ModalityWorklistFind, make_worklist_query("", "", "", "R*"), collect);
    REQUIRE(r.is_ok());
    REQUIRE(names.size() == 1);
    CHECK_EQ(names[0], std::string("ROE^JANE"));
    CHECK(scu.release().is_ok());
}

TEST_CASE("net/services: Q/R extended negotiation (relational, fuzzy) and Composite Instance Root / no bulk data") {
    const std::filesystem::path src = temp_dir("root_src");
    const DicomFile f = make_image(9, 32, 32);
    REQUIRE(write_file(f, (src / "x.dcm").string()).is_ok());
    std::mutex mu;
    QueryFlags seen;
    ScpConfig cfg = scp_cfg();
    QueryFlags sup;
    sup.relational = true;
    sup.fuzzy_person_names = true;
    cfg.extended_negotiation = {find_extended_negotiation(sop::StudyRootFind, sup),
                                retrieve_extended_negotiation(sop::StudyRootGet, true)};
    ScpHandlers h;
    h.on_find = [&](const QueryRequest& q, const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
        const QueryFlags fl = parse_query_flags(q.extended_negotiation, true);
        {
            std::lock_guard<std::mutex> l(mu);
            seen = fl;
        }
        const std::uint16_t chk = check_query_identifier(q.sop_class_uid, q.identifier, fl.relational, false);
        if (chk != dimse_status::Success) return chk;
        MatchOptions mo;
        mo.fuzzy_person_names = fl.fuzzy_person_names;
        Dataset cand;
        (void)cand.set_string(tags::PatientName, VR::PN, "DOE^JOHN");
        (void)cand.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.333");
        (void)cand.set_string(0x00080052, VR::CS, "SERIES");
        if (query_matches(q.identifier, cand, mo)) (void)emit(query_response(q.identifier, cand));
        return dimse_status::Success;
    };
    h.on_get = [&](const QueryRequest&, std::vector<RetrieveItem>& items) -> std::uint16_t {
        items.push_back({(src / "x.dcm").string(), nullptr});
        return dimse_status::Success;
    };
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, "SERIES");
    (void)q.set_string(tags::PatientName, VR::PN, "doe^john");
    (void)q.set_string(tags::SeriesInstanceUID, VR::UI, "");
    for (bool negotiate : {false, true}) {
        ScuConfig sc = scu_cfg(scp.port());
        QueryFlags want;
        want.relational = true;
        want.fuzzy_person_names = true;
        want.combined_datetime = true;  // not supported by the SCP: answered 0
        if (negotiate) sc.extended_negotiation = {find_extended_negotiation(sop::StudyRootFind, want)};
        Scu scu(sc);
        scu.add_context(std::string(sop::StudyRootFind), native_transfer_syntaxes());
        REQUIRE(scu.connect().is_ok());
        const QueryFlags agreed = negotiated_query_flags(*scu.association(), sop::StudyRootFind, true);
        CHECK_EQ(agreed.relational, negotiate);
        CHECK_EQ(agreed.fuzzy_person_names, negotiate);
        CHECK(!agreed.combined_datetime);
        std::size_t n = 0;
        Result<FindResult> r = scu.find(sop::StudyRootFind, q, [&](const Dataset&) {
            ++n;
            return true;
        });
        REQUIRE(r.is_ok());
        // Hierarchical: SERIES level without the Study Instance UID is refused; relational + fuzzy matches.
        CHECK_EQ(r->status, negotiate ? dimse_status::Success : dimse_status::IdentifierDoesNotMatch);
        CHECK_EQ(n, negotiate ? 1u : 0u);
        CHECK(scu.release().is_ok());
    }
    {
        std::lock_guard<std::mutex> l(mu);
        CHECK(seen.relational && seen.fuzzy_person_names && !seen.combined_datetime);
    }
    // Composite Instance Root Retrieve (GET) returns the full instance, Without Bulk Data strips it.
    for (std::string_view cls : {sop::CompositeInstanceRootGet, sop::CompositeInstanceRetrieveWithoutBulkDataGet}) {
        Scu scu(scu_cfg(scp.port()));
        scu.add_context(std::string(cls), native_transfer_syntaxes());
        scu.add_get_storage(kSC);
        REQUIRE(scu.connect().is_ok());
        Dataset rq;
        (void)rq.set_string(0x00080052, VR::CS, "IMAGE");
        (void)rq.set_string(tags::StudyInstanceUID, VR::UI, "2.25.444");
        (void)rq.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.333");
        (void)rq.set_string(tags::SOPInstanceUID, VR::UI, "2.25.5559");
        bool has_pixels = false;
        std::size_t got = 0;
        Result<RetrieveResult> r = scu.get(cls, rq, [&](const StoreRequest& s) -> std::uint16_t {
            ++got;
            has_pixels = s.dataset.contains(tags::PixelData);
            return dimse_status::Success;
        });
        REQUIRE(r.is_ok());
        CHECK_EQ(r->status, dimse_status::Success);
        CHECK_EQ(got, 1u);
        CHECK_EQ(has_pixels, cls == sop::CompositeInstanceRootGet);
        CHECK(scu.release().is_ok());
    }
}

// ===================================================================== UPS

TEST_CASE("net/services: UPS push / pull / watch / event / query") {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<UpsEventReport> events;
    auto wsvc = std::make_shared<ServiceSet>();
    wsvc->on_ups_event = [&](const std::string&, const UpsEventReport& ev) -> std::uint16_t {
        std::lock_guard<std::mutex> l(mu);
        events.push_back(ev);
        cv.notify_all();
        return dimse_status::Success;
    };
    ScpHandlers wh;
    wh.services = wsvc;
    ScpServer watcher(scp_cfg("WATCHER"), wh);
    REQUIRE(watcher.start().is_ok());
    const std::uint16_t wport = watcher.port();

    auto svc = std::make_shared<ServiceSet>();
    svc->ups = std::make_shared<UpsProvider>();
    svc->resolve_ae = [wport](const std::string& ae) -> std::optional<MoveDestination> {
        if (ae != "WATCHER") return std::nullopt;
        return MoveDestination{"127.0.0.1", wport, ""};
    };
    ScpHandlers h;
    h.services = svc;
    ScpServer scp(scp_cfg(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_cfg(scp.port()));
    for (std::string_view c : {sop::UpsPush, sop::UpsPull, sop::UpsWatch, sop::UpsQuery})
        scu.add_context(std::string(c), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    auto status = [](const Result<Scu::NResponse>& r) -> std::uint16_t {
        return r ? r->command.status.value_or(0xFFFF) : 0xFFFF;
    };
    auto wait_events = [&](std::size_t n) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::seconds(10), [&] { return events.size() >= n; });
    };
    CHECK_EQ(status(ups_subscribe(scu, std::string(sop::UpsGlobalSubscriptionInstance), "UNKNOWN_AE", false)),
             dimse_status::UpsUnknownReceivingAe);
    CHECK_EQ(status(ups_subscribe(scu, std::string(sop::UpsGlobalSubscriptionInstance), "WATCHER", false)),
             dimse_status::Success);
    Dataset ups;
    (void)ups.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)ups.set_string(tags::PatientName, VR::PN, "DOE^JOHN");
    (void)ups.set_string(0x00741204, VR::LO, "Plan review");  // Procedure Step Label
    (void)ups.set_string(0x00404041, VR::CS, "READY");
    const std::string uid = "2.25.9100";
    CHECK_EQ(status(ups_create(scu, uid, ups)), dimse_status::Success);
    CHECK_EQ(status(ups_create(scu, uid, ups)), dimse_status::DuplicateSopInstance);
    Dataset wrong = ups;
    (void)wrong.set_string(0x00741000, VR::CS, "IN PROGRESS");
    CHECK_EQ(status(ups_create(scu, "2.25.9101", wrong)), dimse_status::UpsCreateStateNotScheduled);
    REQUIRE(wait_events(1));
    {
        std::lock_guard<std::mutex> l(mu);
        CHECK_EQ(events[0].ups_uid, uid);
        CHECK_EQ(events[0].event_type, 1);
        CHECK_EQ(events[0].info.get_string(0x00741000).value_or(""), std::string("SCHEDULED"));
    }
    // C-FIND on the Pull and Query models
    Dataset q;
    (void)q.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)q.set_string(tags::PatientName, VR::PN, "");
    (void)q.set_string(tags::SOPInstanceUID, VR::UI, "");
    for (std::string_view model : {sop::UpsPull, sop::UpsQuery}) {
        std::vector<std::string> found;
        Result<FindResult> fr = ups_find(scu, model, q, [&](const Dataset& d) {
            found.push_back(d.get_string(tags::SOPInstanceUID).value_or(""));
            return true;
        });
        REQUIRE(fr.is_ok());
        CHECK_EQ(fr->status, dimse_status::Success);
        REQUIRE(found.size() == 1);
        CHECK_EQ(found[0], uid);
    }
    // state machine
    const std::string txn = make_uid();
    CHECK_EQ(status(ups_change_state(scu, uid, "COMPLETED", txn)), dimse_status::UpsNotInProgress);
    CHECK_EQ(status(ups_change_state(scu, uid, "SCHEDULED", txn)), dimse_status::UpsScheduledOnlyByCreate);
    CHECK_EQ(status(ups_change_state(scu, uid, "IN PROGRESS", txn)), dimse_status::Success);
    CHECK_EQ(status(ups_change_state(scu, uid, "IN PROGRESS", txn)), dimse_status::UpsAlreadyInProgress);
    CHECK_EQ(status(ups_change_state(scu, "2.25.1", "IN PROGRESS", txn)), dimse_status::UpsNoSuchInstance);
    Dataset mod;
    (void)mod.set_string(0x00741204, VR::LO, "Plan review 2");
    CHECK_EQ(status(ups_set(scu, uid, mod)), dimse_status::UpsWrongTransactionUid);
    (void)mod.set_string(0x00081195, VR::UI, txn);
    CHECK_EQ(status(ups_set(scu, uid, mod)), dimse_status::Success);
    Result<Scu::NResponse> g = ups_get(scu, sop::UpsPull, uid, {0x00741000, 0x00741204, 0x00081195});
    REQUIRE(g.is_ok());
    CHECK_EQ(g->command.status.value_or(1), dimse_status::Success);
    CHECK_EQ(g->dataset.get_string(0x00741000).value_or(""), std::string("IN PROGRESS"));
    CHECK_EQ(g->dataset.get_string(0x00741204).value_or(""), std::string("Plan review 2"));
    CHECK(!g->dataset.contains(0x00081195));  // the Transaction UID is never returned
    CHECK_EQ(status(ups_get(scu, sop::UpsPull, "2.25.1")), dimse_status::UpsNoSuchInstance);
    Dataset why;
    (void)why.set_string(0x00741238, VR::LT, "Patient left");
    CHECK_EQ(status(ups_request_cancel(scu, sop::UpsPush, uid, why)), dimse_status::Success);
    CHECK_EQ(status(ups_change_state(scu, uid, "COMPLETED", make_uid())), dimse_status::UpsWrongTransactionUid);
    CHECK_EQ(status(ups_change_state(scu, uid, "COMPLETED", txn)), dimse_status::Success);
    CHECK_EQ(status(ups_change_state(scu, uid, "COMPLETED", txn)), dimse_status::UpsAlreadyCompleted);
    CHECK_EQ(status(ups_change_state(scu, uid, "CANCELED", txn)), dimse_status::UpsMayNoLongerBeUpdated);
    CHECK_EQ(status(ups_set(scu, uid, mod)), dimse_status::UpsMayNoLongerBeUpdated);
    CHECK_EQ(status(ups_request_cancel(scu, sop::UpsPush, uid)), dimse_status::UpsCancelAlreadyCompleted);
    // a second UPS, canceled while SCHEDULED
    CHECK_EQ(status(ups_create(scu, "2.25.9102", ups)), dimse_status::Success);
    CHECK_EQ(status(ups_request_cancel(scu, sop::UpsWatch, "2.25.9102")), dimse_status::Success);
    CHECK_EQ(status(ups_request_cancel(scu, sop::UpsWatch, "2.25.9102")), dimse_status::UpsAlreadyCanceled);
    CHECK_EQ(status(ups_unsubscribe(scu, std::string(sop::UpsGlobalSubscriptionInstance), "WATCHER")),
             dimse_status::Success);
    CHECK(scu.release().is_ok());
    // events: SCHEDULED, IN PROGRESS, cancel requested, COMPLETED, SCHEDULED (2nd), CANCELED (2nd)
    REQUIRE(wait_events(6));
    std::lock_guard<std::mutex> l(mu);
    std::vector<std::string> seq;
    for (const UpsEventReport& e : events)
        seq.push_back(std::to_string(e.event_type) + ":" + e.info.get_string(0x00741000).value_or("-"));
    const std::vector<std::string> want = {"1:SCHEDULED", "1:IN PROGRESS", "2:-", "1:COMPLETED", "1:SCHEDULED", "1:CANCELED"};
    CHECK(seq == want);
    CHECK_EQ(events[2].info.get_string(0x00741236).value_or(""), std::string("SVC_SCU"));
    CHECK_EQ(events[2].info.get_string(0x00741238).value_or(""), std::string("Patient left"));
}

// ===================================================================== print management SCU

namespace {

// A minimal Basic Grayscale Print Management SCP written with the association API.
class FakePrinter {
public:
    FakePrinter() {
        (void)listener_.listen("127.0.0.1", 0);
        thread_ = std::thread([this] { run(); });
    }
    ~FakePrinter() {
        if (thread_.joinable()) thread_.join();
        listener_.close();
    }
    std::uint16_t port() const { return listener_.port(); }
    std::atomic<bool> image_had_pixels{false};

private:
    void run() {
        Result<std::unique_ptr<Transport>> t = listener_.accept(10000);
        if (!t) return;
        Result<std::unique_ptr<Association>> ar = Association::receive_request(std::move(t.value()), fast(), {});
        if (!ar) return;
        Association& a = *ar.value();
        AssociateAc ac;
        std::vector<AcceptedSyntax> policy = {{std::string(sop::BasicGrayscalePrintManagementMeta),
                                               native_transfer_syntaxes(), true, false}};
        std::vector<NegotiatedContext> ctx = negotiate_contexts(a.request_pdu(), policy, nullptr, ac);
        if (!a.accept(std::move(ac), std::move(ctx))) return;
        std::string film_box;
        while (true) {
            Result<DimseReceived> r = receive_dimse(a, 10000);
            if (!r) return;
            if (r->kind == DimseKind::PeerReleaseRequest) {
                (void)a.respond_release();
                return;
            }
            const Message& m = r->message;
            const Command& c = m.command;
            Command rsp = make_response(c, dimse_status::Success);
            Dataset out;
            if (c.command_field == cmd::NGetRq && c.sop_class() == sop::Printer) {
                (void)out.set_string(0x21100010, VR::CS, "NORMAL");
                (void)out.set_string(0x21100020, VR::CS, "NORMAL");
                rsp.has_dataset = true;
            } else if (c.command_field == cmd::NCreateRq && c.sop_class() == sop::BasicFilmSession) {
                rsp.affected_sop_instance_uid = "2.25.7001";
            } else if (c.command_field == cmd::NCreateRq && c.sop_class() == sop::BasicFilmBox) {
                film_box = "2.25.7002";
                rsp.affected_sop_instance_uid = film_box;
                Dataset ib;
                (void)ib.set_string(0x00081150, VR::UI, sop::BasicGrayscaleImageBox);
                (void)ib.set_string(0x00081155, VR::UI, "2.25.7003");
                Element e;
                e.tag = 0x20100510;
                e.vr = VR::SQ;
                e.items.push_back(ib);
                out.put(std::move(e));
                rsp.has_dataset = true;
                // A printer status event before the response (the SCU must answer it).
                Command ev = make_n_event_report_rq(a.next_message_id(), sop::Printer, sop::PrinterInstance, 1, false);
                if (!send_dimse(a, m.context_id, ev)) return;
                Result<DimseReceived> er = receive_dimse(a, 10000);
                if (!er || er->message.command.command_field != cmd::NEventReportRsp) return;
            } else if (c.command_field == cmd::NSetRq && c.sop_class() == sop::BasicGrayscaleImageBox) {
                Result<Dataset> ds = decode_message_dataset(m);
                if (ds) {
                    const Element* seq = ds->find(0x20200110);
                    image_had_pixels.store(seq && seq->items.size() == 1 && seq->items[0].contains(tags::PixelData) &&
                                           ds->get_int(0x20200010).value_or(0) == 1);
                }
            } else if (c.command_field == cmd::NActionRq || c.command_field == cmd::NDeleteRq) {
                // print / delete: success
            } else {
                rsp = make_response(c, dimse_status::NoSuchSopClass);
            }
            if (!send_dimse(a, m.context_id, rsp, rsp.has_dataset ? &out : nullptr)) return;
        }
    }
    TcpListener listener_;
    std::thread thread_;
};

}  // namespace

TEST_CASE("net/services: Basic Grayscale Print Management SCU") {
    FakePrinter printer;
    REQUIRE(printer.port() != 0);
    Scu scu(scu_cfg(printer.port(), "PRINTER"));
    scu.add_context(std::string(sop::BasicGrayscalePrintManagementMeta), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    PrintScu p(scu, false);
    Result<Dataset> st = p.printer_status();
    REQUIRE(st.is_ok());
    CHECK_EQ(st->get_string(0x21100010).value_or(""), std::string("NORMAL"));
    Dataset session;
    (void)session.set_string(0x20000010, VR::IS, "1");       // Number of Copies
    (void)session.set_string(0x20000030, VR::CS, "PAPER");   // Medium Type
    Result<std::string> sid = p.create_film_session(session);
    REQUIRE(sid.is_ok());
    CHECK_EQ(*sid, std::string("2.25.7001"));
    Result<PrintScu::FilmBox> fb = p.create_film_box({});
    REQUIRE(fb.is_ok());
    CHECK_EQ(fb->uid, std::string("2.25.7002"));
    REQUIRE(fb->image_boxes.size() == 1);
    CHECK_EQ(p.printer_events().size(), 1u);
    Dataset img;
    (void)img.set_uint16(tags::SamplesPerPixel, 1);
    (void)img.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)img.set_uint16(tags::Rows, 4);
    (void)img.set_uint16(tags::Columns, 4);
    (void)img.set_uint16(tags::BitsAllocated, 8);
    (void)img.set_uint16(tags::BitsStored, 8);
    (void)img.set_uint16(tags::HighBit, 7);
    (void)img.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(16, 0x80);
    (void)img.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    CHECK(p.set_image_box(fb->image_boxes[0], 1, img).is_ok());
    CHECK(p.print_film_box(fb->uid).is_ok());
    CHECK(p.delete_film_box(fb->uid).is_ok());
    CHECK(p.delete_film_session().is_ok());
    CHECK(scu.release().is_ok());
    CHECK(printer.image_had_pixels.load());
    CHECK(context_covers(sop::BasicGrayscalePrintManagementMeta, sop::BasicGrayscaleImageBox));
    CHECK(!context_covers(sop::BasicGrayscalePrintManagementMeta, sop::BasicColorImageBox));
}

// ===================================================================== asynchronous operations

TEST_CASE("net/services: asynchronous C-STORE window") {
    const std::filesystem::path dir = temp_dir("async");
    std::vector<std::string> paths;
    for (int i = 0; i < 20; ++i) {
        const std::string p = (dir / ("f" + std::to_string(i) + ".dcm")).string();
        REQUIRE(write_file(make_image(100 + i), p).is_ok());
        paths.push_back(p);
    }
    paths.push_back((dir / "missing.dcm").string());
    StoreSink rec;
    ScpConfig cfg = scp_cfg();
    cfg.max_async_operations = 8;
    ScpHandlers h;
    h.on_store = std::ref(rec);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    for (std::uint16_t proposed : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{4}, std::uint16_t{32}}) {
        ScuConfig sc = scu_cfg(scp.port());
        if (proposed) sc.async_window = AsyncWindow{proposed, 1};
        Scu scu(sc);
        scu.add_context(kSC, {kExplicit});
        REQUIRE(scu.connect().is_ok());
        const std::uint16_t granted = async_invoke_window(*scu.association());
        CHECK_EQ(granted, proposed == 0 ? 1 : std::min<std::uint16_t>(proposed, 8));
        Result<StoreManyResult> r = store_many(scu, paths);
        REQUIRE(r.is_ok());
        REQUIRE(r->outcomes.size() == paths.size());
        for (std::size_t i = 0; i + 1 < paths.size(); ++i) CHECK_EQ(r->outcomes[i].status, dimse_status::Success);
        CHECK(!r->outcomes.back().error.is_ok());
        CHECK_EQ(r->max_outstanding, static_cast<std::size_t>(granted));
        CHECK(scu.release().is_ok());
    }
    std::lock_guard<std::mutex> l(rec.mu);
    CHECK_EQ(rec.got.size(), 80u);
}
