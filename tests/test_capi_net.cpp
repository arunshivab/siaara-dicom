// C API tests for the networking additions: cancellation tokens, logging, DIMSE SCU calls
// (echo, find, move, get, store with transcoding, commitment, worklist), the Storage SCP and
// the DICOMweb additions (metadata, frames, retrieve, WADO-URI, cancel).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "crosscheck/web_memory_store.hpp"
#include "net/scp.hpp"
#include "net/services.hpp"
#include "net/socket.hpp"
#include "siaara/siaara.h"
#include "web/server.hpp"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_capinet_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

DicomFile make_ct(int i, const std::string& study = "2.25.500", const std::string& series = "2.25.501") {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kCT);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.600" + std::to_string(i));
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, study);
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, series);
    (void)ds.set_string(tags::PatientName, VR::PN, "Net^Test");
    (void)ds.set_string(tags::PatientID, VR::LO, "NT1");
    (void)ds.set_string(tags::Modality, VR::CS, "CT");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 8);
    (void)ds.set_uint16(tags::Columns, 8);
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_uint16(tags::BitsStored, 12);
    (void)ds.set_uint16(tags::HighBit, 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(128);
    for (std::size_t k = 0; k < px.size(); k += 2) px[k] = static_cast<std::uint8_t>(k + static_cast<std::size_t>(i));
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    return f;
}

siaara_file* to_capi(const DicomFile& f) {
    std::vector<std::uint8_t> bytes;
    if (!write_part10(f, bytes)) return nullptr;
    siaara_file* out = nullptr;
    if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &out) != SIAARA_OK) return nullptr;
    return out;
}

siaara_dimse_peer peer(uint32_t port, const char* called = "CAPI_SCP", const char* calling = "CAPI_SCU") {
    siaara_dimse_peer p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.host = "127.0.0.1";
    p.port = port;
    p.called_ae = called;
    p.calling_ae = calling;
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

net::Timeouts fast() {
    net::Timeouts t;
    t.connect_ms = 3000;
    t.acse_ms = 3000;
    t.dimse_ms = 5000;
    t.artim_ms = 2000;
    t.idle_ms = 10000;
    return t;
}

net::ScpConfig qr_config() {
    net::ScpConfig c;
    c.ae_title = "QR_SCP";
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.max_associations = 4;
    c.allow_any_calling_ae = true;
    c.timeouts = fast();
    return c;
}

struct StoreOpen {
    siaara_store* store = nullptr;
    explicit StoreOpen(const std::filesystem::path& p) {
        const std::string s = p.string();
        (void)siaara_store_open(s.c_str(), s.size(), nullptr, &store);
    }
    ~StoreOpen() { siaara_store_free(store); }
    uint64_t instances() const {
        siaara_store_stats st;
        std::memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        if (siaara_store_stats_get(store, &st) != SIAARA_OK) return 0;
        return st.instances;
    }
};

struct ScpEvents {
    std::mutex mu;
    std::vector<std::string> stored;
    int echoes = 0;
};

void SIAARA_CALL on_scp(void* user, const siaara_scp_event* e) {
    auto* ev = static_cast<ScpEvents*>(user);
    std::lock_guard<std::mutex> l(ev->mu);
    if (e->kind == SIAARA_SCP_STORED) ev->stored.emplace_back(e->sop_instance_uid);
    if (e->kind == SIAARA_SCP_ECHO) ++ev->echoes;
}

siaara_scp* start_scp(siaara_store* store, ScpEvents* ev, const char* const* allowed = nullptr, uint32_t n = 0) {
    siaara_scp_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.ae_title = "CAPI_SCP";
    o.bind_address = "127.0.0.1";
    o.allowed_calling_aes = allowed;
    o.allowed_count = n;
    o.allow_any_calling_ae = n == 0 ? 1 : 0;
    o.idle_timeout_ms = 10000;
    siaara_scp* scp = nullptr;
    if (siaara_scp_start(&o, store, on_scp, ev, &scp) != SIAARA_OK) return nullptr;
    return scp;
}

int32_t SIAARA_CALL count_progress(void* user, const siaara_dimse_result* p) {
    static_cast<std::vector<uint32_t>*>(user)->push_back(p->completed);
    return 0;
}

struct LogSeen {
    std::mutex mu;
    int messages = 0;
    int phi = 0;
};
void SIAARA_CALL on_log(void* user, int32_t, int32_t phi, const char*) {
    auto* s = static_cast<LogSeen*>(user);
    std::lock_guard<std::mutex> l(s->mu);
    ++s->messages;
    if (phi) ++s->phi;
}

}  // namespace

TEST_CASE("capi/net: cancellation token basics") {
    siaara_cancel* c = nullptr;
    REQUIRE(siaara_cancel_create(&c) == SIAARA_OK);
    int32_t set = 9;
    REQUIRE(siaara_cancel_is_set(c, &set) == SIAARA_OK);
    CHECK(set == 0);
    CHECK(siaara_cancel_set(c) == SIAARA_OK);
    CHECK(siaara_cancel_is_set(c, &set) == SIAARA_OK && set == 1);
    // A set token makes a call fail before it connects.
    siaara_dimse_peer p = peer(1);
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_echo(&p, c, &r) == SIAARA_ERR_CANCELLED);
    CHECK(siaara_cancel_reset(c) == SIAARA_OK);
    CHECK(siaara_cancel_is_set(c, &set) == SIAARA_OK && set == 0);
    siaara_cancel_free(c);
    siaara_cancel_free(nullptr);
    CHECK(siaara_cancel_set(nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_set_log_callback(nullptr, nullptr, 7) == SIAARA_ERR_INVALID_ARGUMENT);
}

TEST_CASE("capi/net: Storage SCP, C-ECHO, allowed peers, C-STORE with transcoding, stats, logging") {
    LogSeen logs;
    REQUIRE(siaara_set_log_callback(on_log, &logs, SIAARA_LOG_DEBUG) == SIAARA_OK);
    const auto dir = temp_dir("scp");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    ScpEvents ev;
    const char* allowed[] = {"CAPI_SCU"};
    siaara_scp* scp = start_scp(st.store, &ev, allowed, 1);
    REQUIRE(scp);
    uint32_t port = 0;
    REQUIRE(siaara_scp_port(scp, &port) == SIAARA_OK);
    CHECK(port != 0);

    siaara_dimse_peer p = peer(port);
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_OK);
    CHECK(r.status == 0);
    siaara_dimse_peer stranger = peer(port, "CAPI_SCP", "STRANGER");
    CHECK(siaara_dimse_echo(&stranger, nullptr, &r) == SIAARA_ERR_REJECTED);
    siaara_dimse_peer wrong_called = peer(port, "OTHER");
    CHECK(siaara_dimse_echo(&wrong_called, nullptr, &r) == SIAARA_ERR_REJECTED);

    // Three files: native, RLE (transcoded to native, the SCP accepts only native), and a broken one.
    std::vector<std::string> paths;
    for (int i = 0; i < 2; ++i) {
        DicomFile f = make_ct(i);
        const std::string path = (dir / ("f" + std::to_string(i) + ".dcm")).string();
        siaara_file* cf = to_capi(f);
        REQUIRE(cf);
        if (i == 1) REQUIRE(siaara_file_transcode(cf, "1.2.840.10008.1.2.5", nullptr) == SIAARA_OK);
        REQUIRE(siaara_file_save(cf, path.c_str(), path.size(), nullptr) == SIAARA_OK);
        siaara_file_free(cf);
        paths.push_back(path);
    }
    paths.push_back((dir / "missing.dcm").string());
    std::vector<const char*> ptrs;
    for (const auto& s : paths) ptrs.push_back(s.c_str());
    std::vector<uint32_t> progress;
    r = result();
    CHECK(siaara_dimse_store_files(&p, ptrs.data(), nullptr, 3, nullptr, count_progress, &progress, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.completed == 2 && r.failed == 1);
    CHECK(progress.size() == 3 && progress[1] == 2);
    CHECK(st.instances() == 2);
    // The RLE instance arrived uncompressed and decodes to the same pixels.
    {
        siaara_store_filter q;
        std::memset(&q, 0, sizeof q);
        q.struct_size = sizeof q;
        q.level = SIAARA_LEVEL_INSTANCE;
        q.sop_instance_uid = "2.25.6001";
        siaara_store_result* res = nullptr;
        REQUIRE(siaara_store_query(st.store, &q, &res) == SIAARA_OK);
        siaara_index_record rec;
        std::memset(&rec, 0, sizeof rec);
        rec.struct_size = sizeof rec;
        char tab[2048];
        size_t need = 0;
        REQUIRE(siaara_store_result_get(res, 0, &rec, tab, sizeof tab, &need) == SIAARA_OK);
        CHECK(std::string(tab + rec.str_offset[SIAARA_IDX_TRANSFER_SYNTAX_UID]) != "1.2.840.10008.1.2.5");
        siaara_store_result_free(res);
    }
    // One file in memory.
    siaara_file* mem = to_capi(make_ct(7));
    REQUIRE(mem);
    r = result();
    CHECK(siaara_dimse_store(&p, mem, nullptr, &r) == SIAARA_OK);
    CHECK(r.completed == 1);
    siaara_file_free(mem);
    CHECK(st.instances() == 3);
    {
        std::lock_guard<std::mutex> l(ev.mu);
        CHECK(ev.stored.size() == 3 && ev.echoes == 1);
    }
    siaara_scp_stats ss;
    std::memset(&ss, 0, sizeof ss);
    ss.struct_size = sizeof ss;
    REQUIRE(siaara_scp_stats_get(scp, &ss) == SIAARA_OK);
    CHECK(ss.instances_stored == 3 && ss.echoes == 1 && ss.associations_rejected >= 1 && ss.bytes_received > 0);
    CHECK(siaara_scp_stop(scp) == SIAARA_OK);
    CHECK(siaara_scp_stop(scp) == SIAARA_OK);
    CHECK(siaara_dimse_echo(&p, nullptr, &r) != SIAARA_OK);
    siaara_scp_free(scp);
    REQUIRE(siaara_set_log_callback(nullptr, nullptr, SIAARA_LOG_DEBUG) == SIAARA_OK);
    {
        std::lock_guard<std::mutex> l(logs.mu);
        CHECK(logs.messages > 0);
    }
    // Argument checks.
    siaara_dimse_peer badp = peer(0);
    CHECK(siaara_dimse_echo(&badp, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    badp = peer(104, "THIS_AE_TITLE_IS_TOO_LONG");
    CHECK(siaara_dimse_echo(&badp, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_dimse_echo(nullptr, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    r.struct_size = 4;
    CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_scp_start(nullptr, st.store, nullptr, nullptr, &scp) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_scp_options open_to_all;
    std::memset(&open_to_all, 0, sizeof open_to_all);
    open_to_all.struct_size = sizeof open_to_all;
    CHECK(siaara_scp_start(&open_to_all, st.store, nullptr, nullptr, &scp) == SIAARA_ERR_INVALID_ARGUMENT);  // NET-05
}

TEST_CASE("capi/net: C-FIND with matches, callback stop and cancellation; worklist query") {
    std::atomic<bool> slow{false};
    net::ScpHandlers h;
    h.on_find = [&](const net::QueryRequest& q, const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
        for (int i = 0; i < 3; ++i) {
            if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            Dataset m;
            (void)m.set_string(tags::PatientName, VR::PN, "Match^" + std::to_string(i));
            (void)m.set_string(0x00080052, VR::CS, q.sop_class_uid == std::string(net::sop::ModalityWorklistFind) ? "" : "STUDY");
            if (!emit(m)) return 0xFE00;
        }
        return net::dimse_status::Success;
    };
    net::ScpServer srv(qr_config(), h);
    REQUIRE(srv.start().is_ok());
    siaara_dimse_peer p = peer(srv.port(), "QR_SCP");
    siaara_file* idf = nullptr;
    REQUIRE(siaara_file_create(&idf) == SIAARA_OK);
    siaara_dataset* id = nullptr;
    REQUIRE(siaara_file_dataset(idf, &id) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(id, 0x00080052, "CS", "STUDY", 5) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(id, 0x00100010, "PN", "", 0) == SIAARA_OK);
    struct Seen {
        std::vector<std::string> names;
        int stop_after = 100;
    } seen;
    auto cb = [](void* user, const siaara_dataset* m) -> int32_t {
        auto* s = static_cast<Seen*>(user);
        char buf[64];
        size_t need = 0;
        if (siaara_dataset_get_string(m, 0x00100010, 0, buf, sizeof buf, &need) == SIAARA_OK) s->names.emplace_back(buf);
        return static_cast<int>(s->names.size()) >= s->stop_after ? 1 : 0;
    };
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_find(&p, SIAARA_QR_STUDY_ROOT, id, nullptr, cb, &seen, &r) == SIAARA_OK);
    CHECK(r.matches == 3 && seen.names.size() == 3 && seen.names[2] == "Match^2");
    seen.names.clear();
    seen.stop_after = 1;
    CHECK(siaara_dimse_find(&p, SIAARA_QR_PATIENT_ROOT, id, nullptr, cb, &seen, &r) == SIAARA_ERR_CANCELLED);
    CHECK(seen.names.size() == 1);
    CHECK(siaara_dimse_find(&p, 9, id, nullptr, cb, &seen, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    // Worklist identifier and C-FIND on the worklist model.
    siaara_file* wl = nullptr;
    REQUIRE(siaara_worklist_query("CT01", "CT", "20260101-20261231", nullptr, nullptr, &wl) == SIAARA_OK);
    siaara_dataset* wds = nullptr;
    REQUIRE(siaara_file_dataset(wl, &wds) == SIAARA_OK);
    uint32_t items = 0;
    CHECK(siaara_dataset_item_count(wds, 0x00400100, &items) == SIAARA_OK && items == 1);
    seen.names.clear();
    seen.stop_after = 100;
    CHECK(siaara_dimse_find(&p, SIAARA_QR_WORKLIST, wds, nullptr, cb, &seen, &r) == SIAARA_OK);
    CHECK(r.matches == 3);
    siaara_file_free(wl);
    // Cancellation from another thread interrupts a slow find at once.
    slow = true;
    siaara_cancel* c = nullptr;
    REQUIRE(siaara_cancel_create(&c) == SIAARA_OK);
    std::thread t([c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        (void)siaara_cancel_set(c);
    });
    const auto t0 = std::chrono::steady_clock::now();
    seen.names.clear();
    CHECK(siaara_dimse_find(&p, SIAARA_QR_STUDY_ROOT, id, c, cb, &seen, &r) == SIAARA_ERR_CANCELLED);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1400);
    t.join();
    siaara_cancel_free(c);
    srv.stop();
    siaara_file_free(idf);
}

TEST_CASE("capi/net: C-GET into a store and a folder, C-MOVE to the Storage SCP") {
    const auto dir = temp_dir("qr");
    std::vector<std::shared_ptr<const DicomFile>> files;
    for (int i = 0; i < 3; ++i) files.push_back(std::make_shared<const DicomFile>(make_ct(20 + i, "2.25.700", "2.25.701")));
    // The C API Storage SCP is the C-MOVE destination.
    StoreOpen dest(dir / "dest");
    REQUIRE(dest.store);
    ScpEvents ev;
    siaara_scp* scp = start_scp(dest.store, &ev);
    REQUIRE(scp);
    uint32_t scp_port = 0;
    REQUIRE(siaara_scp_port(scp, &scp_port) == SIAARA_OK);

    net::ScpHandlers h;
    auto supply = [&](const net::QueryRequest&, std::vector<net::RetrieveItem>& items) -> std::uint16_t {
        for (const auto& f : files) items.push_back({std::string(), f});
        return net::dimse_status::Success;
    };
    h.on_get = supply;
    h.on_move = supply;
    h.resolve_move_destination = [&](const std::string& ae) -> std::optional<net::MoveDestination> {
        if (ae != "CAPI_SCP") return std::nullopt;
        return net::MoveDestination{"127.0.0.1", static_cast<std::uint16_t>(scp_port), "CAPI_SCP"};
    };
    net::ScpServer srv(qr_config(), h);
    REQUIRE(srv.start().is_ok());
    siaara_dimse_peer p = peer(srv.port(), "QR_SCP");

    siaara_file* idf = nullptr;
    REQUIRE(siaara_file_create(&idf) == SIAARA_OK);
    siaara_dataset* id = nullptr;
    REQUIRE(siaara_file_dataset(idf, &id) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(id, 0x00080052, "CS", "STUDY", 5) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(id, 0x0020000D, "UI", "2.25.700", 8) == SIAARA_OK);

    StoreOpen got(dir / "got");
    REQUIRE(got.store);
    const auto folder = dir / "folder";
    std::filesystem::create_directories(folder);
    const std::string folder_s = folder.string();
    siaara_get_options go;
    std::memset(&go, 0, sizeof go);
    go.struct_size = sizeof go;
    go.store = got.store;
    go.folder = folder_s.c_str();
    std::vector<uint32_t> progress;
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_get(&p, SIAARA_QR_STUDY_ROOT, id, &go, nullptr, count_progress, &progress, &r) == SIAARA_OK);
    CHECK(r.completed == 3 && r.failed == 0);
    CHECK(!progress.empty());
    CHECK(got.instances() == 3);
    CHECK(std::filesystem::exists(folder / "2.25.60020.dcm"));
    go.store = nullptr;
    go.folder = nullptr;
    CHECK(siaara_dimse_get(&p, SIAARA_QR_STUDY_ROOT, id, &go, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_dimse_get(&p, SIAARA_QR_WORKLIST, id, &go, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_INVALID_ARGUMENT);

    r = result();
    CHECK(siaara_dimse_move(&p, SIAARA_QR_STUDY_ROOT, id, "CAPI_SCP", nullptr, nullptr, nullptr, &r) == SIAARA_OK);
    CHECK(r.completed == 3);
    CHECK(dest.instances() == 3);
    CHECK(siaara_dimse_move(&p, SIAARA_QR_STUDY_ROOT, id, "NOWHERE", nullptr, nullptr, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.status == 0xA801);  // move destination unknown
    srv.stop();
    siaara_scp_free(scp);
    siaara_file_free(idf);
}

TEST_CASE("capi/net: storage commitment on the same association") {
    auto svc = std::make_shared<net::ServiceSet>();
    svc->commitment_report = net::CommitmentReportMode::SameAssociation;
    svc->on_commitment = [](const net::CommitmentRequest& rq) {
        net::CommitmentResult res;
        for (const net::SopRef& x : rq.items) {
            if (x.sop_instance == "2.25.2") res.failed.push_back({x, 0x0112});
            else res.committed.push_back(x);
        }
        return res;
    };
    net::ScpHandlers h;
    h.services = svc;
    net::ScpServer srv(qr_config(), h);
    REQUIRE(srv.start().is_ok());
    siaara_dimse_peer p = peer(srv.port(), "QR_SCP");
    const char* classes[] = {kCT, kCT, kCT};
    const char* inst[] = {"2.25.1", "2.25.2", "2.25.3"};
    uint16_t reasons[3] = {};
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_commit(&p, classes, inst, 3, 5000, nullptr, reasons, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.completed == 2 && r.failed == 1);
    CHECK(reasons[0] == 0 && reasons[1] == 0x0112 && reasons[2] == 0);
    CHECK(siaara_dimse_commit(&p, classes, inst, 1, 5000, nullptr, reasons, &r) == SIAARA_OK);
    srv.stop();
}

// ---------------------------------------------------------------- DICOMweb additions
TEST_CASE("capi/web: metadata, frames, retrieve, WADO-URI, parts into a store, cancel") {
    siaara_test_web::MemoryStore mem;
    web::DicomwebServer server;
    web::DicomwebServerConfig cfg;
    cfg.http.port = 0;
    cfg.http.bind_address = "127.0.0.1";
    REQUIRE(server.start(cfg, mem.make_store()).is_ok());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";
    siaara_web_client* wc = nullptr;
    REQUIRE(siaara_web_client_create(base.data(), base.size(), 5000, &wc) == SIAARA_OK);
    for (int i = 0; i < 2; ++i) {
        siaara_file* f = to_capi(make_ct(40 + i, "2.25.800", "2.25.801"));
        REQUIRE(f);
        REQUIRE(siaara_web_stow(wc, f, nullptr) == SIAARA_OK);
        siaara_file_free(f);
    }
    // Metadata of the series.
    char small[4];
    size_t need = 0;
    CHECK(siaara_web_wado_metadata(wc, "2.25.800", "2.25.801", nullptr, small, sizeof small, &need) ==
          SIAARA_ERR_BUFFER_TOO_SMALL);
    std::vector<char> js(need + 1);
    REQUIRE(siaara_web_last_response(wc, js.data(), js.size(), &need) == SIAARA_OK);
    CHECK(std::string(js.data()).find("2.25.60040") != std::string::npos);
    CHECK(siaara_web_wado_metadata(wc, "2.25.800", nullptr, "2.25.60040", js.data(), js.size(), &need) ==
          SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_web_wado_metadata(wc, "2.25.999", nullptr, nullptr, js.data(), js.size(), &need) == SIAARA_ERR_NOT_FOUND);
    // Frames.
    siaara_web_parts* parts = nullptr;
    const uint32_t frames[] = {1};
    const char* ts[] = {"1.2.840.10008.1.2.1"};
    REQUIRE(siaara_web_wado_frames(wc, "2.25.800", "2.25.801", "2.25.60041", frames, 1, ts, 1, &parts) == SIAARA_OK);
    uint32_t n = 0;
    REQUIRE(siaara_web_parts_count(parts, &n) == SIAARA_OK);
    CHECK(n == 1);
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    char ct[128];
    REQUIRE(siaara_web_parts_get(parts, 0, &data, &size, ct, sizeof ct, &need) == SIAARA_OK);
    CHECK(size == 128 && data[2] == static_cast<uint8_t>(2 + 41));
    CHECK(siaara_web_parts_get(parts, 1, &data, &size, nullptr, 0, &need) == SIAARA_ERR_NOT_FOUND);
    siaara_web_parts_free(parts);
    const uint32_t zero[] = {0};
    CHECK(siaara_web_wado_frames(wc, "2.25.800", "2.25.801", "2.25.60041", zero, 1, nullptr, 0, &parts) ==
          SIAARA_ERR_INVALID_ARGUMENT);
    // Bulk retrieve of the study into a store.
    REQUIRE(siaara_web_wado_retrieve(wc, "2.25.800", nullptr, nullptr, nullptr, 0, &parts) == SIAARA_OK);
    REQUIRE(siaara_web_parts_count(parts, &n) == SIAARA_OK);
    CHECK(n == 2);
    siaara_file* one = nullptr;
    REQUIRE(siaara_web_parts_open_file(parts, 0, &one) == SIAARA_OK);
    siaara_file_free(one);
    const auto dir = temp_dir("web");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    uint32_t added = 0, failed = 9;
    CHECK(siaara_web_parts_add_to_store(parts, st.store, SIAARA_DUP_KEEP, &added, &failed) == SIAARA_OK);
    CHECK(added == 2 && failed == 0 && st.instances() == 2);
    siaara_web_parts_free(parts);
    // WADO-URI.
    REQUIRE(siaara_web_wado_uri(wc, "2.25.800", "2.25.801", "2.25.60040", nullptr, nullptr, &parts) == SIAARA_OK);
    REQUIRE(siaara_web_parts_open_file(parts, 0, &one) == SIAARA_OK);
    siaara_file_free(one);
    siaara_web_parts_free(parts);
    // A set token refuses to start a request.
    siaara_cancel* c = nullptr;
    REQUIRE(siaara_cancel_create(&c) == SIAARA_OK);
    REQUIRE(siaara_web_client_set_cancel(wc, c) == SIAARA_OK);
    REQUIRE(siaara_cancel_set(c) == SIAARA_OK);
    CHECK(siaara_web_wado_metadata(wc, "2.25.800", nullptr, nullptr, js.data(), js.size(), &need) == SIAARA_ERR_CANCELLED);
    REQUIRE(siaara_cancel_reset(c) == SIAARA_OK);
    CHECK(siaara_web_wado_metadata(wc, "2.25.800", nullptr, nullptr, nullptr, 0, &need) == SIAARA_OK);
    REQUIRE(siaara_web_client_set_cancel(wc, nullptr) == SIAARA_OK);
    siaara_web_client_free(wc);
    server.stop();

    // A server that never answers: siaara_cancel_set aborts the request at once.
    net::TcpListener mute;
    REQUIRE(mute.listen("127.0.0.1", 0).is_ok());
    std::atomic<bool> done{false};
    std::thread acceptor([&] {
        std::vector<std::unique_ptr<net::Transport>> held;
        while (!done) {
            auto t = mute.accept(100);
            if (t) held.push_back(std::move(t).value());
        }
    });
    const std::string mute_url = "http://127.0.0.1:" + std::to_string(mute.port()) + "/dicomweb";
    REQUIRE(siaara_web_client_create(mute_url.data(), mute_url.size(), 8000, &wc) == SIAARA_OK);
    REQUIRE(siaara_web_client_set_cancel(wc, c) == SIAARA_OK);
    std::thread t([c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        (void)siaara_cancel_set(c);
    });
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(siaara_web_wado_metadata(wc, "2.25.800", nullptr, nullptr, nullptr, 0, &need) == SIAARA_ERR_CANCELLED);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 3000);
    t.join();
    siaara_web_client_free(wc);  // detaches from the token
    siaara_cancel_free(c);
    done = true;
    acceptor.join();
}

// ---------------------------------------------------------------- cancellable long calls
namespace {

int32_t SIAARA_CALL stop_after_two(void* user, uint64_t done, uint64_t) {
    auto* calls = static_cast<int*>(user);
    ++*calls;
    return done >= 2 ? 1 : 0;
}
int32_t SIAARA_CALL count_only(void* user, uint64_t, uint64_t) {
    ++*static_cast<int*>(user);
    return 0;
}

}  // namespace

TEST_CASE("capi/long calls: volume build, store verify / rebuild, DICOMDIR write with progress and cancel") {
    const auto dir = temp_dir("long");
    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        DicomFile f = make_ct(80 + i, "2.25.900", "2.25.901");
        (void)f.dataset.set_string(0x00200032u, VR::DS, "0\\0\\" + std::to_string(i * 2));
        (void)f.dataset.set_string(0x00200037u, VR::DS, "1\\0\\0\\0\\1\\0");
        (void)f.dataset.set_string(0x00280030u, VR::DS, "1\\1");
        (void)f.dataset.set_string(0x00200052u, VR::UI, "2.25.902");
        const std::string p = (dir / ("s" + std::to_string(i) + ".dcm")).string();
        REQUIRE(write_file(f, p).is_ok());
        paths.push_back(p);
    }
    std::vector<const char*> ptrs;
    for (const auto& s : paths) ptrs.push_back(s.c_str());
    siaara_volume_options vo;
    std::memset(&vo, 0, sizeof vo);
    vo.struct_size = sizeof vo;
    vo.threads = 1;
    int calls = 0;
    siaara_volume* v = nullptr;
    REQUIRE(siaara_volume_build_ex(ptrs.data(), nullptr, 4, &vo, nullptr, count_only, &calls, &v) == SIAARA_OK);
    CHECK(calls == 4);
    siaara_volume_free(v);
    calls = 0;
    v = nullptr;
    CHECK(siaara_volume_build_ex(ptrs.data(), nullptr, 4, &vo, nullptr, stop_after_two, &calls, &v) == SIAARA_ERR_CANCELLED);
    CHECK(v == nullptr && calls == 2);
    siaara_cancel* c = nullptr;
    REQUIRE(siaara_cancel_create(&c) == SIAARA_OK);
    REQUIRE(siaara_cancel_set(c) == SIAARA_OK);
    CHECK(siaara_volume_build_ex(ptrs.data(), nullptr, 4, nullptr, c, nullptr, nullptr, &v) == SIAARA_ERR_CANCELLED);
    REQUIRE(siaara_cancel_reset(c) == SIAARA_OK);

    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    for (const auto& p : paths) REQUIRE(siaara_store_add_file(st.store, p.c_str(), p.size(), SIAARA_DUP_KEEP, SIAARA_ADD_COPY, nullptr) == SIAARA_OK);
    siaara_store_verify_report rep;
    std::memset(&rep, 0, sizeof rep);
    rep.struct_size = sizeof rep;
    calls = 0;
    REQUIRE(siaara_store_verify_ex(st.store, 1, 0, c, count_only, &calls, &rep) == SIAARA_OK);
    CHECK(rep.checked == 4 && calls == 4);
    calls = 0;
    CHECK(siaara_store_verify_ex(st.store, 1, 1, nullptr, stop_after_two, &calls, &rep) == SIAARA_ERR_CANCELLED);
    calls = 0;
    REQUIRE(siaara_store_rebuild_ex(st.store, nullptr, count_only, &calls) == SIAARA_OK);
    CHECK(calls == 4 && st.instances() == 4);
    calls = 0;
    CHECK(siaara_store_rebuild_ex(st.store, nullptr, stop_after_two, &calls) == SIAARA_ERR_CANCELLED);
    CHECK(st.instances() == 4);

    std::vector<size_t> lens;
    for (const auto& s : paths) lens.push_back(s.size());
    const std::string media = (dir / "media").string();
    calls = 0;
    CHECK(siaara_dicomdir_write_ex(media.c_str(), media.size(), ptrs.data(), lens.data(), 4, nullptr, nullptr, stop_after_two,
                                   &calls, nullptr) == SIAARA_ERR_CANCELLED);
    CHECK(!std::filesystem::exists(dir / "media" / "DICOMDIR"));
    const std::string media2 = (dir / "media2").string();
    calls = 0;
    uint32_t warnings = 0;
    REQUIRE(siaara_dicomdir_write_ex(media2.c_str(), media2.size(), ptrs.data(), lens.data(), 4, nullptr, c, count_only, &calls,
                                     &warnings) == SIAARA_OK);
    CHECK(calls == 8 && std::filesystem::exists(dir / "media2" / "DICOMDIR"));
    siaara_cancel_free(c);
}

namespace {
struct LogText {
    std::mutex mu;
    std::vector<std::string> lines;
};
void SIAARA_CALL on_log_text(void* user, int32_t, int32_t, const char* m) {
    auto* s = static_cast<LogText*>(user);
    std::lock_guard<std::mutex> l(s->mu);
    s->lines.emplace_back(m);
}
std::string file_text(const std::filesystem::path& p) {
    std::string s;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}
}  // namespace

TEST_CASE("capi/net: R-072 C-GET into a folder honours SIAARA_DUP_KEEP") {
    const auto dir = temp_dir("r072");
    std::vector<std::shared_ptr<const DicomFile>> files;
    for (int i = 0; i < 2; ++i) files.push_back(std::make_shared<const DicomFile>(make_ct(30 + i, "2.25.710", "2.25.711")));
    net::ScpHandlers h;
    h.on_get = [&](const net::QueryRequest&, std::vector<net::RetrieveItem>& items) -> std::uint16_t {
        for (const auto& f : files) items.push_back({std::string(), f});
        return net::dimse_status::Success;
    };
    net::ScpServer srv(qr_config(), h);
    REQUIRE(srv.start().is_ok());
    siaara_dimse_peer p = peer(srv.port(), "QR_SCP");
    siaara_file* idf = nullptr;
    REQUIRE(siaara_file_create(&idf) == SIAARA_OK);
    siaara_dataset* id = nullptr;
    REQUIRE(siaara_file_dataset(idf, &id) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(id, 0x00080052, "CS", "STUDY", 5) == SIAARA_OK);
    REQUIRE(siaara_dataset_set_string(id, 0x0020000D, "UI", "2.25.710", 8) == SIAARA_OK);

    const auto folder = dir / "folder";
    std::filesystem::create_directories(folder);
    const auto existing = folder / "2.25.60030.dcm";
    {
        FILE* f = std::fopen(existing.string().c_str(), "wb");
        REQUIRE(f);
        std::fputs("EXISTING", f);
        std::fclose(f);
    }
    const std::string folder_s = folder.string();
    siaara_get_options go;
    std::memset(&go, 0, sizeof go);
    go.struct_size = sizeof go;
    go.folder = folder_s.c_str();
    go.duplicate_policy = SIAARA_DUP_KEEP;
    LogText logs;
    REQUIRE(siaara_set_log_callback(on_log_text, &logs, SIAARA_LOG_DEBUG) == SIAARA_OK);
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_get(&p, SIAARA_QR_STUDY_ROOT, id, &go, nullptr, nullptr, nullptr, &r) == SIAARA_OK);
    CHECK(r.completed == 2 && r.failed == 0);
    CHECK_EQ(file_text(existing), std::string("EXISTING"));            // kept
    CHECK(file_text(folder / "2.25.60031.dcm").size() > 132);          // the new one written
    REQUIRE(siaara_set_log_callback(nullptr, nullptr, SIAARA_LOG_DEBUG) == SIAARA_OK);
    bool logged = false;
    for (const auto& l : logs.lines)
        if (l.find("already exists") != std::string::npos) logged = true;
    CHECK(logged);
    // REPLACE overwrites it.
    go.duplicate_policy = SIAARA_DUP_REPLACE;
    r = result();
    CHECK(siaara_dimse_get(&p, SIAARA_QR_STUDY_ROOT, id, &go, nullptr, nullptr, nullptr, &r) == SIAARA_OK);
    const std::string replaced = file_text(existing);
    CHECK(replaced.size() > 132 && replaced.compare(128, 4, "DICM") == 0);
    srv.stop();
    siaara_file_free(idf);
}

TEST_CASE("capi/net: R-074 store quota through siaara_store_options; SCP association ceiling and spool options") {
    const auto dir = temp_dir("r074");
    siaara_store_options so;
    std::memset(&so, 0, sizeof so);
    so.struct_size = sizeof so;
    so.create = 1;
    so.max_total_bytes = 1;
    siaara_store* store = nullptr;
    const std::string root = (dir / "store").string();
    REQUIRE(siaara_store_open(root.c_str(), root.size(), &so, &store) == SIAARA_OK);
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(make_ct(40), bytes).is_ok());
    CHECK(siaara_store_add_memory(store, bytes.data(), bytes.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_ERR_LIMIT_EXCEEDED);
    // An older, smaller struct_size is still accepted (the new fields keep their defaults).
    so.struct_size = static_cast<uint32_t>(offsetof(siaara_store_options, max_total_bytes));
    siaara_store* old = nullptr;
    const std::string root2 = (dir / "store2").string();
    REQUIRE(siaara_store_open(root2.c_str(), root2.size(), &so, &old) == SIAARA_OK);
    CHECK(siaara_store_add_memory(old, bytes.data(), bytes.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    siaara_store_free(old);

    siaara_scp_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.ae_title = "CAPI_SCP";
    o.bind_address = "127.0.0.1";
    o.allow_any_calling_ae = 1;
    o.max_associations = 100;
    siaara_scp* scp = nullptr;
    CHECK(siaara_scp_start(&o, store, nullptr, nullptr, &scp) == SIAARA_ERR_INVALID_ARGUMENT);
    o.max_associations = 4;
    const std::string spool = (dir / "spool").string();
    std::filesystem::create_directories(spool);
    o.spool_directory = spool.c_str();
    o.spool_threshold = 4096;
    REQUIRE(siaara_scp_start(&o, store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_scp_free(scp);
    siaara_store_free(store);
}
