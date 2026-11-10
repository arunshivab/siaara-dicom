// C API tests for the full networking service set (siaara_scp_start_ex and the SCU / DICOMweb
// additions), loopback ours <-> ours, plus the round-3 security fixes R-093 (lossy transcodes
// and SOP Instance UIDs) and R-094 (SCP dispatcher drop policy).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "web/client.hpp"
#include "web/dicom_json.hpp"
#include "web/multipart.hpp"

using namespace siaara;

namespace {

constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";
constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";
constexpr const char* kJpegBaseline = "1.2.840.10008.1.2.4.50";

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_capinet2_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

DicomFile make_image(const std::string& sop, const std::string& study, const std::string& series, const char* pid,
                     const char* name, bool eight_bit = false) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, eight_bit ? kMR : kCT);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, study);
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, series);
    (void)ds.set_string(tags::PatientName, VR::PN, name);
    (void)ds.set_string(tags::PatientID, VR::LO, pid);
    (void)ds.set_string(tags::Modality, VR::CS, eight_bit ? "MR" : "CT");
    (void)ds.set_string(0x00080020, VR::DA, "20261110");
    (void)ds.set_string(0x00080050, VR::SH, "ACC-" + study.substr(study.size() - 2));
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 16);
    (void)ds.set_uint16(tags::Columns, 16);
    (void)ds.set_uint16(tags::BitsAllocated, eight_bit ? 8 : 16);
    (void)ds.set_uint16(tags::BitsStored, eight_bit ? 8 : 12);
    (void)ds.set_uint16(tags::HighBit, eight_bit ? 7 : 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(eight_bit ? 256 : 512);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>((k * 7) & 0x7F);
    (void)ds.set_bytes(tags::PixelData, eight_bit ? VR::OB : VR::OW, Bytes::copy_of(px.data(), px.size()));
    return f;
}

std::vector<std::uint8_t> part10(const DicomFile& f) {
    std::vector<std::uint8_t> bytes;
    (void)write_part10(f, bytes);
    return bytes;
}

siaara_file* to_capi(const DicomFile& f) {
    const auto bytes = part10(f);
    siaara_file* out = nullptr;
    if (siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &out) != SIAARA_OK) return nullptr;
    return out;
}

siaara_dimse_peer peer(uint32_t port, const char* called, const char* calling = "N2_SCU") {
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

siaara_scp_options scp_options(const char* ae) {
    siaara_scp_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.ae_title = ae;
    o.bind_address = "127.0.0.1";
    o.allow_any_calling_ae = 1;
    o.idle_timeout_ms = 10000;
    return o;
}

siaara_scp_services services() {
    siaara_scp_services s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    return s;
}

struct StoreOpen {
    siaara_store* store = nullptr;
    explicit StoreOpen(const std::filesystem::path& p) {
        const std::string s = p.string();
        (void)siaara_store_open(s.c_str(), s.size(), nullptr, &store);
    }
    ~StoreOpen() { siaara_store_free(store); }
    StoreOpen(const StoreOpen&) = delete;
    StoreOpen& operator=(const StoreOpen&) = delete;
    uint64_t instances() const {
        siaara_store_stats st;
        std::memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        if (siaara_store_stats_get(store, &st) != SIAARA_OK) return 0;
        return st.instances;
    }
    bool add(const DicomFile& f) const {
        const auto b = part10(f);
        return siaara_store_add_memory(store, b.data(), b.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK;
    }
};

uint32_t port_of(const siaara_scp* scp) {
    uint32_t p = 0;
    (void)siaara_scp_port(scp, &p);
    return p;
}

struct Events {
    std::mutex mu;
    std::condition_variable cv;
    struct E {
        int32_t kind;
        uint32_t status;
        std::string calling, cls, inst, original, txn, dest;
        uint32_t completed, failed, command_field;
        bool has_dataset;
    };
    std::vector<E> list;
    int count(int32_t kind) {
        std::lock_guard<std::mutex> l(mu);
        int n = 0;
        for (const E& e : list) n += e.kind == kind;
        return n;
    }
    bool wait_for(int32_t kind, int n, int ms = 5000) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] {
            int k = 0;
            for (const E& e : list) k += e.kind == kind;
            return k >= n;
        });
    }
    // Waits for an event of `kind` whose transaction UID is `txn` (other reports may arrive first).
    bool wait_for_txn(int32_t kind, const std::string& txn, int ms = 8000) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] {
            for (const E& e : list)
                if (e.kind == kind && e.txn == txn) return true;
            return false;
        });
    }
    std::vector<E> of(int32_t kind) {
        std::lock_guard<std::mutex> l(mu);
        std::vector<E> out;
        for (const E& e : list)
            if (e.kind == kind) out.push_back(e);
        return out;
    }
};

void SIAARA_CALL on_event(void* user, const siaara_scp_event* e) {
    auto* ev = static_cast<Events*>(user);
    {
        std::lock_guard<std::mutex> l(ev->mu);
        ev->list.push_back({e->kind, e->dimse_status, e->calling_ae, e->sop_class_uid, e->sop_instance_uid,
                            e->original_sop_instance_uid, e->transaction_uid, e->destination_ae, e->completed, e->failed,
                            e->command_field, e->dataset != nullptr});
    }
    ev->cv.notify_all();
}

struct Matches {
    std::vector<Dataset> list;
};
int32_t SIAARA_CALL collect(void* user, const siaara_dataset* m) {
    static_cast<Matches*>(user)->list.push_back(*reinterpret_cast<const Dataset*>(m));
    return 0;
}

siaara_file* qr(int32_t level, siaara_qr_keys k) {
    k.struct_size = sizeof k;
    siaara_file* f = nullptr;
    if (siaara_qr_identifier(level, &k, &f) != SIAARA_OK) return nullptr;
    return f;
}

const siaara_dataset* ds_of(siaara_file* f) {
    siaara_dataset* d = nullptr;
    (void)siaara_file_dataset(f, &d);
    return d;
}

siaara_status find(const siaara_dimse_peer& p, int32_t model, siaara_file* id, Matches& m, siaara_qr_options* qo = nullptr) {
    siaara_dimse_result r = result();
    return siaara_dimse_find_ex(&p, model, ds_of(id), qo, nullptr, collect, &m, &r);
}

}  // namespace

// ---------------------------------------------------------------- R-094
namespace {
struct Blocker {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    int events = 0;
};
void SIAARA_CALL blocking_cb(void* user, const siaara_scp_event*) {
    auto* b = static_cast<Blocker*>(user);
    std::unique_lock<std::mutex> l(b->mu);
    ++b->events;
    b->cv.wait(l, [&] { return b->open; });  // never returns until the test opens it
}
}  // namespace

TEST_CASE("net2: R-094 a callback that never returns cannot deadlock siaara_scp_stop") {
    const auto dir = temp_dir("r094");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    Blocker b;
    siaara_scp_options o = scp_options("N2_SCP");
    o.event_queue_limit = 2;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start(&o, st.store, blocking_cb, &b, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "N2_SCP");
    // 1 event in the callback, 2 queued; the 4th waits at most 1 s, then is dropped.
    for (int i = 0; i < 4; ++i) {
        siaara_dimse_result r = result();
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_OK);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
    }
    // An echo whose event is waiting for room while stop() runs.
    auto late = std::async(std::launch::async, [&] {
        siaara_dimse_result r = result();
        return siaara_dimse_echo(&p, nullptr, &r);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto stopping = std::async(std::launch::async, [&] { return siaara_scp_stop(scp); });
    const bool stopped = stopping.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(stopped);
    {
        std::lock_guard<std::mutex> l(b.mu);
        b.open = true;  // lets a broken build finish instead of hanging
    }
    b.cv.notify_all();
    CHECK(stopping.get() == SIAARA_OK);
    (void)late.get();
    siaara_scp_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    REQUIRE(siaara_scp_stats_get(scp, &s) == SIAARA_OK);
    CHECK(s.events_dropped >= 1);
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- R-093
namespace {
struct UidChanges {
    std::vector<std::pair<std::string, std::string>> list;
};
void SIAARA_CALL on_uid(void* user, uint32_t, const char* o, const char* n) {
    static_cast<UidChanges*>(user)->list.emplace_back(o, n);
}
}  // namespace

TEST_CASE("net2: R-093 a lossy C-STORE uses the new SOP Instance UID; a strict SCP accepts it") {
    // The SCP accepts the MR class in JPEG Baseline only, and refuses UID mismatches.
    std::mutex mu;
    std::string cmd_uid, ds_uid, meta_ts;
    net::ScpConfig cfg;
    cfg.ae_title = "STRICT";
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.allow_any_calling_ae = true;
    cfg.strict_store_uids = true;
    cfg.accepted = {{kMR, {kJpegBaseline}, true, false}};
    net::ScpHandlers h;
    h.on_store = [&](const net::StoreRequest& rq) -> std::uint16_t {
        std::lock_guard<std::mutex> l(mu);
        cmd_uid = rq.sop_instance_uid;
        ds_uid = rq.dataset.get_string(tags::SOPInstanceUID).value_or("");
        meta_ts = rq.transfer_syntax;
        return 0;
    };
    net::ScpServer server(cfg, h);
    REQUIRE(server.start().is_ok());
    siaara_dimse_peer p = peer(server.port(), "STRICT");
    p.allow_lossy_transcode = 1;
    const DicomFile f = make_image("2.25.9301", "2.25.9310", "2.25.9311", "R093", "Lossy^Test", true);
    siaara_file* cf = to_capi(f);
    REQUIRE(cf);
    UidChanges ch;
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_store_ex(&p, cf, nullptr, on_uid, &ch, &r) == SIAARA_OK);
    CHECK(r.status == 0);
    REQUIRE(ch.list.size() == 1);
    CHECK(ch.list[0].first == "2.25.9301");
    CHECK(ch.list[0].second != "2.25.9301");
    {
        std::lock_guard<std::mutex> l(mu);
        CHECK(cmd_uid == ch.list[0].second);
        CHECK(ds_uid == ch.list[0].second);
        CHECK(meta_ts == kJpegBaseline);
    }
    // Files: the same through siaara_dimse_store_files_ex.
    const auto dir = temp_dir("r093");
    const std::string path = (dir / "mr.dcm").string();
    REQUIRE(siaara_file_save(cf, path.c_str(), path.size(), nullptr) == SIAARA_OK);
    const char* paths[] = {path.c_str()};
    UidChanges ch2;
    r = result();
    CHECK(siaara_dimse_store_files_ex(&p, paths, nullptr, 1, nullptr, nullptr, on_uid, &ch2, &r) == SIAARA_OK);
    CHECK(r.completed == 1);
    REQUIRE(ch2.list.size() == 1);
    {
        std::lock_guard<std::mutex> l(mu);
        CHECK(cmd_uid == ch2.list[0].second);
        CHECK(ds_uid == ch2.list[0].second);
    }
    siaara_file_free(cf);
    server.stop();
}

TEST_CASE("net2: R-093 a lossy C-GET sub-operation sends the new UID and reports the mapping") {
    const auto dir = temp_dir("r093get");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    REQUIRE(st.add(make_image("2.25.9401", "2.25.9410", "2.25.9411", "R093", "Lossy^Get", true)));
    Events ev;
    siaara_scp_options o = scp_options("GET_SCP");
    siaara_scp_services sv = services();
    sv.get = 1;
    sv.allow_lossy = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
    net::ScuConfig c;
    c.calling_ae = "GETTER";
    c.called_ae = "GET_SCP";
    c.port = static_cast<std::uint16_t>(port_of(scp));
    c.strict_store_uids = true;
    net::Scu scu(c);
    scu.add_context(std::string(net::sop::StudyRootGet), net::native_transfer_syntaxes());
    scu.add_get_storage(kMR, {kJpegBaseline});
    REQUIRE(scu.connect().is_ok());
    Dataset id;
    (void)id.set_string(0x00080052, VR::CS, "STUDY");
    (void)id.set_string(tags::StudyInstanceUID, VR::UI, "2.25.9410");
    std::string cmd_uid, ds_uid;
    auto rr = scu.get(net::sop::StudyRootGet, id, [&](const net::StoreRequest& rq) -> std::uint16_t {
        cmd_uid = rq.sop_instance_uid;
        ds_uid = rq.dataset.get_string(tags::SOPInstanceUID).value_or("");
        return 0;
    });
    REQUIRE(rr);
    CHECK(rr->completed == 1);
    CHECK(rr->failed == 0);
    CHECK(!cmd_uid.empty());
    CHECK(cmd_uid == ds_uid);
    CHECK(cmd_uid != "2.25.9401");
    (void)scu.release();
    REQUIRE(ev.wait_for(SIAARA_SCP_UID_CHANGED, 1));
    const auto changed = ev.of(SIAARA_SCP_UID_CHANGED);
    CHECK(changed[0].original == "2.25.9401");
    CHECK(changed[0].inst == cmd_uid);
    REQUIRE(ev.wait_for(SIAARA_SCP_GET, 1));
    CHECK(ev.of(SIAARA_SCP_GET)[0].completed == 1);
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- Q/R from the local store
TEST_CASE("net2: Storage, C-FIND at every level, C-MOVE and C-GET from the store") {
    const auto dir = temp_dir("qr");
    StoreOpen st(dir / "store");
    StoreOpen dest_store(dir / "dest");
    REQUIRE(st.store);
    REQUIRE(dest_store.store);
    // Destination of C-MOVE: a plain Storage SCP.
    siaara_scp_options dopt = scp_options("DEST");
    siaara_scp* dest = nullptr;
    REQUIRE(siaara_scp_start(&dopt, dest_store.store, nullptr, nullptr, &dest) == SIAARA_OK);

    Events ev;
    siaara_scp_options o = scp_options("QR_SCP");
    siaara_scp_services sv = services();
    sv.storage = sv.find = sv.move = sv.get = 1;
    sv.relational = 1;
    siaara_ae_entry ae;
    std::memset(&ae, 0, sizeof ae);
    ae.struct_size = sizeof ae;
    ae.ae_title = "DEST";
    ae.host = "127.0.0.1";
    ae.port = port_of(dest);
    sv.ae_table = &ae;
    sv.ae_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "QR_SCP");

    // Storage through the SCP: study A (two series), study B.
    const DicomFile files[] = {make_image("2.25.9501", "2.25.9550", "2.25.9551", "P1", "Net^One"),
                               make_image("2.25.9502", "2.25.9550", "2.25.9551", "P1", "Net^One"),
                               make_image("2.25.9503", "2.25.9550", "2.25.9552", "P1", "Net^One"),
                               make_image("2.25.9504", "2.25.9560", "2.25.9561", "P2", "Other^Two")};
    for (const DicomFile& f : files) {
        siaara_file* cf = to_capi(f);
        REQUIRE(cf);
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_store(&p, cf, nullptr, &r) == SIAARA_OK);
        siaara_file_free(cf);
    }
    CHECK(st.instances() == 4);

    siaara_qr_keys k;
    std::memset(&k, 0, sizeof k);
    k.return_keys = 1;
    {  // Patient Root, PATIENT level: both patients, with counts.
        siaara_file* id = qr(SIAARA_LEVEL_PATIENT, k);
        REQUIRE(id);
        Matches m;
        CHECK(find(p, SIAARA_QR_PATIENT_ROOT, id, m) == SIAARA_OK);
        CHECK(m.list.size() == 2);
        siaara_file_free(id);
    }
    {  // Study Root, STUDY level by Patient ID; counts and Modalities in Study come back.
        siaara_qr_keys kk = k;
        kk.patient_id = "P1";
        siaara_file* id = qr(SIAARA_LEVEL_STUDY, kk);
        REQUIRE(id);
        Matches m;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, m) == SIAARA_OK);
        REQUIRE(m.list.size() == 1);
        CHECK(m.list[0].get_string(tags::StudyInstanceUID).value_or("") == "2.25.9550");
        CHECK(m.list[0].get_string(0x00201208).value_or("") == "3");
        CHECK(m.list[0].get_string(0x00201206).value_or("") == "2");
        CHECK(m.list[0].get_string(0x00080052).value_or("") == "STUDY");
        siaara_file_free(id);
    }
    {  // Wildcard name, study date range.
        siaara_qr_keys kk = k;
        kk.patient_name = "Net*";
        kk.study_date = "20260101-20261231";
        siaara_file* id = qr(SIAARA_LEVEL_STUDY, kk);
        Matches m;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, m) == SIAARA_OK);
        CHECK(m.list.size() == 1);
        siaara_file_free(id);
    }
    {  // SERIES and IMAGE levels.
        siaara_qr_keys kk = k;
        kk.study_instance_uid = "2.25.9550";
        siaara_file* id = qr(SIAARA_LEVEL_SERIES, kk);
        Matches m;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, m) == SIAARA_OK);
        CHECK(m.list.size() == 2);
        siaara_file_free(id);
        kk.series_instance_uid = "2.25.9551";
        id = qr(SIAARA_LEVEL_INSTANCE, kk);
        Matches mi;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, mi) == SIAARA_OK);
        CHECK(mi.list.size() == 2);
        siaara_file_free(id);
    }
    {  // A UID list.
        siaara_qr_keys kk = k;
        kk.study_instance_uid = "2.25.9550\\2.25.9560";
        siaara_file* id = qr(SIAARA_LEVEL_STUDY, kk);
        Matches m;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, m) == SIAARA_OK);
        CHECK(m.list.size() == 2);
        siaara_file_free(id);
    }
    {  // Hierarchical rules: IMAGE level without the higher unique keys needs relational queries.
        siaara_qr_keys kk;
        std::memset(&kk, 0, sizeof kk);
        kk.patient_id = "P1";
        siaara_file* id = qr(SIAARA_LEVEL_INSTANCE, kk);
        Matches m;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, m) == SIAARA_ERR_REJECTED);
        siaara_qr_options qo;
        std::memset(&qo, 0, sizeof qo);
        qo.struct_size = sizeof qo;
        qo.flags = SIAARA_QR_RELATIONAL;
        Matches mr;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, mr, &qo) == SIAARA_OK);
        CHECK((qo.accepted_flags & SIAARA_QR_RELATIONAL) != 0);
        CHECK(mr.list.size() == 3);
        // max_matches: our own C-CANCEL is not an error.
        qo.max_matches = 1;
        Matches m1;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, id, m1, &qo) == SIAARA_OK);
        CHECK(m1.list.size() == 1);
        siaara_file_free(id);
        // Study Root has no PATIENT level.
        siaara_file* pid = qr(SIAARA_LEVEL_PATIENT, k);
        Matches mp;
        CHECK(find(p, SIAARA_QR_STUDY_ROOT, pid, mp) == SIAARA_ERR_REJECTED);
        siaara_file_free(pid);
    }
    {  // C-MOVE of study A to DEST; unknown destination refused.
        siaara_qr_keys kk;
        std::memset(&kk, 0, sizeof kk);
        kk.study_instance_uid = "2.25.9550";
        siaara_file* id = qr(SIAARA_LEVEL_STUDY, kk);
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_move(&p, SIAARA_QR_STUDY_ROOT, ds_of(id), "DEST", nullptr, nullptr, nullptr, &r) == SIAARA_OK);
        CHECK(r.completed == 3);
        CHECK(dest_store.instances() == 3);
        r = result();
        CHECK(siaara_dimse_move(&p, SIAARA_QR_STUDY_ROOT, ds_of(id), "NOWHERE", nullptr, nullptr, nullptr, &r) ==
              SIAARA_ERR_REJECTED);
        CHECK(r.status == 0xA801);
        REQUIRE(ev.wait_for(SIAARA_SCP_MOVE, 2));
        bool saw = false;
        for (const auto& e : ev.of(SIAARA_SCP_MOVE)) saw = saw || (e.dest == "DEST" && e.completed == 3);
        CHECK(saw);
        siaara_file_free(id);
    }
    {  // C-GET of a series (Patient Root) into a folder.
        siaara_qr_keys kk;
        std::memset(&kk, 0, sizeof kk);
        kk.patient_id = "P1";
        kk.study_instance_uid = "2.25.9550";
        kk.series_instance_uid = "2.25.9552";
        siaara_file* id = qr(SIAARA_LEVEL_SERIES, kk);
        const std::string folder = (dir / "got").string();
        std::filesystem::create_directories(folder);
        siaara_get_options go;
        std::memset(&go, 0, sizeof go);
        go.struct_size = sizeof go;
        go.folder = folder.c_str();
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_get(&p, SIAARA_QR_PATIENT_ROOT, ds_of(id), &go, nullptr, nullptr, nullptr, &r) == SIAARA_OK);
        CHECK(r.completed == 1);
        CHECK(std::filesystem::exists(dir / "got" / "2.25.9503.dcm"));
        siaara_file_free(id);
    }
    siaara_scp_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    REQUIRE(siaara_scp_stats_get(scp, &s) == SIAARA_OK);
    CHECK(s.instances_stored == 4);
    CHECK(s.finds >= 8);
    CHECK(s.retrieves >= 2);
    siaara_scp_free(scp);
    siaara_scp_free(dest);
}

// ---------------------------------------------------------------- worklist, MPPS, commitment, UPS, IAN, ACL
namespace {
const char* kWorklistJson = R"([
 {"00100010":{"vr":"PN","Value":[{"Alphabetic":"Work^One"}]},"00100020":{"vr":"LO","Value":["W1"]},
  "0020000D":{"vr":"UI","Value":["2.25.9701"]},"00080050":{"vr":"SH","Value":["A1"]},
  "00400100":{"vr":"SQ","Value":[{"00400001":{"vr":"AE","Value":["CT01"]},"00080060":{"vr":"CS","Value":["CT"]},
     "00400002":{"vr":"DA","Value":["20261110"]},"00400009":{"vr":"SH","Value":["SPS1"]}}]}},
 {"00100010":{"vr":"PN","Value":[{"Alphabetic":"Work^Two"}]},"00100020":{"vr":"LO","Value":["W2"]},
  "0020000D":{"vr":"UI","Value":["2.25.9702"]},"00080050":{"vr":"SH","Value":["A2"]},
  "00400100":{"vr":"SQ","Value":[{"00400001":{"vr":"AE","Value":["MR01"]},"00080060":{"vr":"CS","Value":["MR"]},
     "00400002":{"vr":"DA","Value":["20261111"]},"00400009":{"vr":"SH","Value":["SPS2"]}}]}}
])";

uint32_t SIAARA_CALL worklist_cb(void* user, const char*, const siaara_dataset*, siaara_worklist_emit emit, void* ctx) {
    auto* items = static_cast<std::vector<Dataset>*>(user);
    for (const Dataset& d : *items)
        if (emit(ctx, reinterpret_cast<const siaara_dataset*>(&d)) != 0) break;
    return 0;
}
}  // namespace

TEST_CASE("net2: Modality Worklist from a list and from a callback") {
    const auto dir = temp_dir("mwl");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("MWL_SCP");
    siaara_scp_services sv = services();
    sv.worklist = 1;
    Events ev;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
    REQUIRE(siaara_scp_set_worklist_json(scp, kWorklistJson, std::strlen(kWorklistJson)) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "MWL_SCP");
    siaara_file* q = nullptr;
    REQUIRE(siaara_worklist_query(nullptr, "CT", nullptr, nullptr, nullptr, &q) == SIAARA_OK);
    Matches m;
    CHECK(find(p, SIAARA_QR_WORKLIST, q, m) == SIAARA_OK);
    REQUIRE(m.list.size() == 1);
    CHECK(m.list[0].get_string(tags::PatientID).value_or("") == "W1");
    CHECK(m.list[0].get_string(0x00080050).value_or("") == "A1");
    siaara_file_free(q);
    REQUIRE(siaara_worklist_query("MR01", nullptr, "20261101-20261130", "Work*", nullptr, &q) == SIAARA_OK);
    Matches m2;
    CHECK(find(p, SIAARA_QR_WORKLIST, q, m2) == SIAARA_OK);
    CHECK(m2.list.size() == 1);
    siaara_file_free(q);
    REQUIRE(ev.wait_for(SIAARA_SCP_WORKLIST, 2));
    siaara_scp_free(scp);

    // Callback-fed: the toolkit filters what the callback emits.
    auto parsed = web::datasets_from_json(kWorklistJson);
    REQUIRE(parsed);
    std::vector<Dataset> items = std::move(parsed.value());
    sv.worklist_cb = worklist_cb;
    sv.worklist_user = &items;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    p = peer(port_of(scp), "MWL_SCP");
    REQUIRE(siaara_worklist_query(nullptr, "MR", nullptr, nullptr, nullptr, &q) == SIAARA_OK);
    Matches m3;
    CHECK(find(p, SIAARA_QR_WORKLIST, q, m3) == SIAARA_OK);
    REQUIRE(m3.list.size() == 1);
    CHECK(m3.list[0].get_string(tags::PatientID).value_or("") == "W2");
    siaara_file_free(q);
    siaara_scp_free(scp);
}

TEST_CASE("net2: MPPS SCU and SCP, allow-lists") {
    const auto dir = temp_dir("mpps");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("MPPS_SCP");
    siaara_scp_services sv = services();
    sv.mpps = 1;
    const char* modalities[] = {"MODALITY"};
    siaara_scp_acl acl;
    std::memset(&acl, 0, sizeof acl);
    acl.struct_size = sizeof acl;
    acl.service = SIAARA_SVC_MPPS;
    acl.calling_aes = modalities;
    acl.count = 1;
    sv.acls = &acl;
    sv.acl_count = 1;
    Events ev;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "MPPS_SCP", "MODALITY");
    Dataset attrs;
    (void)attrs.set_string(0x00400252, VR::CS, "IN PROGRESS");
    (void)attrs.set_string(0x00400253, VR::SH, "PPS1");
    (void)attrs.set_string(tags::PatientID, VR::LO, "W1");
    char uid[80];
    size_t need = 0;
    siaara_dimse_result r = result();
    REQUIRE(siaara_dimse_mpps_create(&p, nullptr, reinterpret_cast<const siaara_dataset*>(&attrs), nullptr, uid, sizeof uid,
                                     &need, &r) == SIAARA_OK);
    CHECK(r.status == 0);
    CHECK(std::strlen(uid) > 5);
    // A duplicate N-CREATE is refused (0x0111).
    r = result();
    CHECK(siaara_dimse_mpps_create(&p, uid, reinterpret_cast<const siaara_dataset*>(&attrs), nullptr, nullptr, 0, nullptr,
                                   &r) == SIAARA_ERR_REJECTED);
    CHECK(r.status == 0x0111);
    Dataset done;
    (void)done.set_string(0x00400252, VR::CS, "COMPLETED");
    r = result();
    CHECK(siaara_dimse_mpps_set(&p, uid, reinterpret_cast<const siaara_dataset*>(&done), nullptr, &r) == SIAARA_OK);
    // Completed: no further changes (F.7.2.2.2).
    r = result();
    CHECK(siaara_dimse_mpps_set(&p, uid, reinterpret_cast<const siaara_dataset*>(&done), nullptr, &r) == SIAARA_ERR_REJECTED);
    siaara_file* got = nullptr;
    REQUIRE(siaara_scp_mpps_get(scp, uid, &got) == SIAARA_OK);
    char status[32];
    REQUIRE(siaara_dataset_get_string(ds_of(got), 0x00400252, 0, status, sizeof status, &need) == SIAARA_OK);
    CHECK(std::string(status) == "COMPLETED");
    siaara_file_free(got);
    CHECK(siaara_scp_mpps_get(scp, "2.25.1", &got) == SIAARA_ERR_NOT_FOUND);
    // Another AE is not on the MPPS allow-list.
    siaara_dimse_peer stranger = peer(port_of(scp), "MPPS_SCP", "OTHER");
    r = result();
    CHECK(siaara_dimse_mpps_create(&stranger, nullptr, reinterpret_cast<const siaara_dataset*>(&attrs), nullptr, nullptr, 0,
                                   nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK(r.status == 0x0124);
    REQUIRE(ev.wait_for(SIAARA_SCP_REFUSED, 1));
    REQUIRE(ev.wait_for(SIAARA_SCP_MPPS_SET, 2));
    const auto created = ev.of(SIAARA_SCP_MPPS_CREATE);
    REQUIRE(!created.empty());
    CHECK(created[0].inst == uid);
    CHECK(created[0].has_dataset);
    siaara_scp_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    REQUIRE(siaara_scp_stats_get(scp, &s) == SIAARA_OK);
    CHECK(s.refused == 1);
    CHECK(s.mpps == 4);
    siaara_scp_free(scp);
}

TEST_CASE("net2: Storage Commitment on the same association and on a new one") {
    const auto dir = temp_dir("commit");
    StoreOpen st(dir / "store");
    REQUIRE(st.add(make_image("2.25.9801", "2.25.9810", "2.25.9811", "C1", "Commit^One")));
    StoreOpen rst(dir / "recv");
    // The requestor's own SCP, receiving reports sent on new associations.
    siaara_scp_options ro = scp_options("RECV");
    siaara_scp_services rsv = services();
    rsv.commitment_reports = 1;
    Events rev;
    siaara_scp* recv = nullptr;
    REQUIRE(siaara_scp_start_ex(&ro, &rsv, rst.store, on_event, &rev, &recv) == SIAARA_OK);

    for (int32_t mode : {SIAARA_COMMIT_REPORT_SAME_THEN_NEW, SIAARA_COMMIT_REPORT_NEW}) {
        siaara_scp_options o = scp_options("COMMIT_SCP");
        siaara_scp_services sv = services();
        sv.commitment = 1;
        sv.commitment_report_mode = mode;
        siaara_ae_entry ae;
        std::memset(&ae, 0, sizeof ae);
        ae.struct_size = sizeof ae;
        ae.ae_title = "RECV";
        ae.host = "127.0.0.1";
        ae.port = port_of(recv);
        sv.ae_table = &ae;
        sv.ae_count = 1;
        Events ev;
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
        siaara_dimse_peer p = peer(port_of(scp), "COMMIT_SCP", "RECV");
        const char* classes[] = {kCT, kCT};
        const char* insts[] = {"2.25.9801", "2.25.9899"};
        uint16_t reasons[2] = {0, 0};
        siaara_dimse_result r = result();
        if (mode == SIAARA_COMMIT_REPORT_SAME_THEN_NEW) {
            CHECK(siaara_dimse_commit(&p, classes, insts, 2, 5000, nullptr, reasons, &r) == SIAARA_ERR_REJECTED);
        } else {
            // The report comes on a new association to RECV; without a receiver it is never seen.
            siaara_commit_options co;
            std::memset(&co, 0, sizeof co);
            co.struct_size = sizeof co;
            co.wait_ms = 1500;
            co.same_association_ms = -1;
            CHECK(siaara_dimse_commit_ex(&p, &co, classes, insts, 1, nullptr, reasons, &r) == SIAARA_ERR_TIMEOUT);
            char txn[65] = {0};
            co.receiver = recv;
            co.wait_ms = 8000;
            co.transaction_uid_out = txn;
            r = result();
            CHECK(siaara_dimse_commit_ex(&p, &co, classes, insts, 2, nullptr, reasons, &r) == SIAARA_ERR_REJECTED);
            CHECK(std::strlen(txn) > 5);
            // The late report of the first (timed-out) request may arrive first; wait for this one.
            CHECK(rev.wait_for_txn(SIAARA_SCP_COMMIT_REPORT, txn));
        }
        CHECK(r.completed == 1);
        CHECK(r.failed == 1);
        CHECK(r.status == 2);
        CHECK(reasons[0] == 0);
        CHECK(reasons[1] == 0x0112);
        REQUIRE(ev.wait_for(SIAARA_SCP_COMMIT_REQUEST, 1));
        siaara_scp_free(scp);
    }
    siaara_scp_free(recv);
}

TEST_CASE("net2: UPS and Instance Availability through generic N-requests; UPS Query") {
    const auto dir = temp_dir("ups");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("UPS_SCP");
    siaara_scp_services sv = services();
    sv.ups = 1;
    sv.instance_availability = 1;
    Events ev;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "UPS_SCP");
    Dataset item;
    (void)item.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)item.set_string(tags::PatientName, VR::PN, "Ups^Patient");
    (void)item.set_string(0x00741200, VR::CS, "MEDIUM");
    siaara_n_request rq;
    std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.command_field = SIAARA_N_CREATE;
    rq.sop_class_uid = "1.2.840.10008.5.1.4.34.6.1";
    rq.sop_instance_uid = "2.25.9901";
    rq.dataset = reinterpret_cast<const siaara_dataset*>(&item);
    char uid[80];
    size_t need = 0;
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_n_request(&p, &rq, nullptr, nullptr, uid, sizeof uid, &need, &r) == SIAARA_OK);
    CHECK(std::string(uid) == "2.25.9901");
    siaara_file* got = nullptr;
    REQUIRE(siaara_scp_ups_get(scp, "2.25.9901", &got) == SIAARA_OK);
    siaara_file_free(got);
    // Locally scheduled workitem, then UPS Query finds both.
    char uid2[80];
    REQUIRE(siaara_scp_ups_create(scp, nullptr, reinterpret_cast<const siaara_dataset*>(&item), uid2, sizeof uid2, &need) ==
            SIAARA_OK);
    Dataset query;
    (void)query.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)query.set_string(tags::SOPInstanceUID, VR::UI, "");
    Matches m;
    siaara_dimse_result fr = result();
    CHECK(siaara_dimse_find(&p, SIAARA_QR_UPS, reinterpret_cast<const siaara_dataset*>(&query), nullptr, collect, &m, &fr) ==
          SIAARA_OK);
    CHECK(m.list.size() == 2);
    // N-GET of the workitem.
    std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.command_field = SIAARA_N_GET;
    rq.sop_class_uid = "1.2.840.10008.5.1.4.34.6.3";  // UPS Pull
    rq.sop_instance_uid = "2.25.9901";
    siaara_file* out = nullptr;
    r = result();
    CHECK(siaara_dimse_n_request(&p, &rq, nullptr, &out, nullptr, 0, nullptr, &r) == SIAARA_OK);
    CHECK(out != nullptr);
    siaara_file_free(out);
    REQUIRE(ev.wait_for(SIAARA_SCP_UPS, 2));
    // Instance Availability Notification.
    Dataset ian;
    (void)ian.set_string(tags::StudyInstanceUID, VR::UI, "2.25.9910");
    std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.command_field = SIAARA_N_CREATE;
    rq.sop_class_uid = "1.2.840.10008.5.1.4.33";
    rq.dataset = reinterpret_cast<const siaara_dataset*>(&ian);
    r = result();
    CHECK(siaara_dimse_n_request(&p, &rq, nullptr, nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
    REQUIRE(ev.wait_for(SIAARA_SCP_IAN, 1));
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- DICOMweb server and client additions
namespace {
int32_t SIAARA_CALL deny_post(void*, const char* method, const char*, const char* auth) {
    if (std::string(method) == "POST") return std::string(auth) == "Bearer ok" ? 1 : 0;
    return 1;
}
struct Prog {
    uint32_t calls = 0, parts = 0;
    uint64_t bytes = 0;
};
int32_t SIAARA_CALL on_web_progress(void* user, uint32_t parts, uint64_t bytes, uint64_t) {
    auto* pr = static_cast<Prog*>(user);
    ++pr->calls;
    pr->parts = parts;
    pr->bytes = bytes;
    return 0;
}
int32_t SIAARA_CALL stop_web(void*, uint32_t, uint64_t, uint64_t) { return 1; }
}  // namespace

TEST_CASE("net2: DICOMweb server over the store; streaming retrieve; client limits") {
    const auto dir = temp_dir("web");
    StoreOpen st(dir / "store");
    StoreOpen copy(dir / "copy");
    for (int i = 0; i < 3; ++i)
        REQUIRE(st.add(make_image("2.25.9601" + std::to_string(i), "2.25.9650", "2.25.9651", "WEB1", "Web^Patient")));
    siaara_web_server_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.authorize = deny_post;
    siaara_web_server* ws = nullptr;
    REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
    uint32_t port = 0;
    REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/dicomweb";
    siaara_web_client* wc = nullptr;
    REQUIRE(siaara_web_client_create(base.data(), base.size(), 5000, &wc) == SIAARA_OK);
    // QIDO-RS at study, series and instance level.
    std::vector<char> buf(1 << 16);
    size_t need = 0;
    const std::string q = "PatientID=WEB1";
    REQUIRE(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, q.data(), q.size(), buf.data(), buf.size(), &need) ==
            SIAARA_OK);
    CHECK(std::string(buf.data()).find("2.25.9650") != std::string::npos);
    REQUIRE(siaara_web_qido(wc, SIAARA_QIDO_INSTANCES, "2.25.9650", "2.25.9651", nullptr, 0, buf.data(), buf.size(),
                            &need) == SIAARA_OK);
    CHECK(std::string(buf.data()).find("2.25.96012") != std::string::npos);
    const std::string none = "PatientID=NOBODY";
    REQUIRE(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, none.data(), none.size(), buf.data(), buf.size(),
                            &need) == SIAARA_OK);
    CHECK(std::string(buf.data()).find("2.25.9650") == std::string::npos);
    // WADO-RS instance.
    siaara_file* f = nullptr;
    REQUIRE(siaara_web_wado_instance(wc, "2.25.9650", "2.25.9651", "2.25.96011", &f) == SIAARA_OK);
    siaara_file_free(f);
    // Streaming retrieve of the series into another store and a folder, with progress.
    const std::string folder = (dir / "files").string();
    std::filesystem::create_directories(folder);
    Prog pr;
    uint32_t stored = 0, failed = 0;
    CHECK(siaara_web_wado_retrieve_to(wc, "2.25.9650", "2.25.9651", nullptr, nullptr, 0, copy.store, SIAARA_DUP_KEEP,
                                      folder.c_str(), on_web_progress, &pr, &stored, &failed) == SIAARA_OK);
    CHECK(stored == 3);
    CHECK(failed == 0);
    CHECK(pr.parts == 3);
    CHECK(copy.instances() == 3);
    CHECK(std::filesystem::exists(dir / "files" / "2.25.96010.dcm"));
    CHECK(siaara_web_wado_retrieve_to(wc, "2.25.9650", nullptr, nullptr, nullptr, 0, copy.store, SIAARA_DUP_KEEP, nullptr,
                                      stop_web, nullptr, &stored, &failed) == SIAARA_ERR_CANCELLED);
    // Client limits: a small body limit makes the buffered retrieve fail; the streaming one keeps
    // only one part at a time, bounded by max_part_bytes.
    siaara_web_limits lim;
    std::memset(&lim, 0, sizeof lim);
    lim.struct_size = sizeof lim;
    lim.max_body = 2000;
    REQUIRE(siaara_web_client_set_limits(wc, &lim) == SIAARA_OK);
    siaara_web_parts* parts = nullptr;
    CHECK(siaara_web_wado_retrieve(wc, "2.25.9650", nullptr, nullptr, nullptr, 0, &parts) != SIAARA_OK);
    lim.max_body = 0;
    lim.max_part_bytes = 1 << 20;
    REQUIRE(siaara_web_client_set_limits(wc, &lim) == SIAARA_OK);
    CHECK(siaara_web_wado_retrieve_to(wc, "2.25.9650", nullptr, nullptr, nullptr, 0, copy.store, SIAARA_DUP_KEEP, nullptr,
                                      nullptr, nullptr, &stored, &failed) == SIAARA_OK);
    CHECK(stored == 3);
    lim.max_part_bytes = 100;
    REQUIRE(siaara_web_client_set_limits(wc, &lim) == SIAARA_OK);
    CHECK(siaara_web_wado_retrieve_to(wc, "2.25.9650", nullptr, nullptr, nullptr, 0, copy.store, SIAARA_DUP_KEEP, nullptr,
                                      nullptr, nullptr, &stored, &failed) == SIAARA_ERR_LIMIT_EXCEEDED);
    // STOW-RS through the authorize callback.
    siaara_file* nf = to_capi(make_image("2.25.9699", "2.25.9650", "2.25.9651", "WEB1", "Web^Patient"));
    REQUIRE(nf);
    CHECK(siaara_web_stow(wc, nf, nullptr) == SIAARA_ERR_REJECTED);
    CHECK(st.instances() == 3);
    const char* bearer = "Bearer ok";
    REQUIRE(siaara_web_client_set_authorization(wc, bearer, std::strlen(bearer)) == SIAARA_OK);
    CHECK(siaara_web_stow(wc, nf, nullptr) == SIAARA_OK);
    CHECK(st.instances() == 4);
    siaara_file_free(nf);
    siaara_web_client_free(wc);
    siaara_web_server_free(ws);
}

// ---------------------------------------------------------------- incomplete input (testing 10b P1, the gaps left)
namespace {

// A Part 10 stream whose last 62 pixel bytes are missing (as MR_truncated.dcm).
std::vector<std::uint8_t> truncated_part10(const DicomFile& f) {
    std::vector<std::uint8_t> b = part10(f);
    b.resize(b.size() - 62);
    return b;
}

// C-GET of study 2.25.9710 from the SCP on `port` (native transfer syntaxes).
net::RetrieveResult get_study(uint32_t port, std::vector<std::size_t>& pixel_bytes) {
    net::ScuConfig c;
    c.calling_ae = "GETTER";
    c.called_ae = "INC_SCP";
    c.port = static_cast<std::uint16_t>(port);
    net::Scu scu(c);
    scu.add_context(std::string(net::sop::StudyRootGet), net::native_transfer_syntaxes());
    scu.add_get_storage(kMR, net::native_transfer_syntaxes());
    net::RetrieveResult none;
    if (!scu.connect().is_ok()) return none;
    Dataset id;
    (void)id.set_string(0x00080052, VR::CS, "STUDY");
    (void)id.set_string(tags::StudyInstanceUID, VR::UI, "2.25.9710");
    auto rr = scu.get(net::sop::StudyRootGet, id, [&](const net::StoreRequest& rq) -> std::uint16_t {
        const Element* px = rq.dataset.find(tags::PixelData);
        pixel_bytes.push_back(px ? px->value.size() : 0);
        return 0;
    });
    (void)scu.release();
    return rr ? *rr : none;
}

}  // namespace

TEST_CASE("net2: siaara_scp_services.allow_incomplete decides C-GET sub-operations for a truncated stored file") {
    const auto dir = temp_dir("scpinc");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    REQUIRE(st.add(make_image("2.25.9701", "2.25.9710", "2.25.9711", "INC", "Inc^Get", true)));
    // The store keeps a truncated file byte for byte (it is an archive of what it was given).
    const auto cut = truncated_part10(make_image("2.25.9702", "2.25.9710", "2.25.9711", "INC", "Inc^Get", true));
    REQUIRE(siaara_store_add_memory(st.store, cut.data(), cut.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    CHECK(st.instances() == 2);
    siaara_scp_options o = scp_options("INC_SCP");
    siaara_scp_services sv = services();
    sv.get = 1;
    // Default: the sub-operation for the truncated file fails; the complete one is delivered.
    {
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
        std::vector<std::size_t> px;
        const net::RetrieveResult r = get_study(port_of(scp), px);
        CHECK(r.completed == 1);
        CHECK(r.failed == 1);
        REQUIRE(px.size() == 1);
        CHECK(px[0] == 256u);
        siaara_scp_free(scp);
    }
    // A services struct of the version before allow_incomplete: the field is not read (default).
    {
        siaara_scp_services old = sv;
        old.struct_size = static_cast<uint32_t>(offsetof(siaara_scp_services, allow_incomplete));
        old.allow_incomplete = 1;
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &old, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
        std::vector<std::size_t> px;
        const net::RetrieveResult r = get_study(port_of(scp), px);
        CHECK(r.completed == 1);
        CHECK(r.failed == 1);
        siaara_scp_free(scp);
    }
    // Not 0 or 1: refused.
    {
        siaara_scp_services bad = sv;
        bad.allow_incomplete = 2;
        siaara_scp* scp = nullptr;
        CHECK(siaara_scp_start_ex(&o, &bad, st.store, nullptr, nullptr, &scp) == SIAARA_ERR_INVALID_ARGUMENT);
        CHECK(scp == nullptr);
    }
    // Opted in: both are sent; the truncated one with the pixel bytes that were read.
    {
        sv.allow_incomplete = 1;
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
        std::vector<std::size_t> px;
        const net::RetrieveResult r = get_study(port_of(scp), px);
        CHECK(r.completed == 2);
        CHECK(r.failed == 0);
        REQUIRE(px.size() == 2);
        std::sort(px.begin(), px.end());
        CHECK(px[0] < 256u);
        CHECK(px[1] == 256u);
        siaara_scp_free(scp);
    }
}

TEST_CASE("net2: STOW-RS does not send an incomplete file unless siaara_web_stow_ex allows it") {
    const auto dir = temp_dir("stowinc");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    siaara_web_server_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    siaara_web_server* ws = nullptr;
    REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
    uint32_t port = 0;
    REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/dicomweb";
    siaara_web_client* wc = nullptr;
    REQUIRE(siaara_web_client_create(base.data(), base.size(), 5000, &wc) == SIAARA_OK);

    const DicomFile whole = make_image("2.25.9801", "2.25.9810", "2.25.9811", "STOWI", "Stow^Inc", true);
    const auto cut = truncated_part10(make_image("2.25.9802", "2.25.9810", "2.25.9811", "STOWI", "Stow^Inc", true));
    siaara_file* tf = nullptr;
    REQUIRE(siaara_file_open_memory(cut.data(), cut.size(), nullptr, &tf) == SIAARA_OK);
    // Refused before any request, with the C-STORE message.
    CHECK(siaara_web_stow(wc, tf, nullptr) == SIAARA_ERR_REJECTED);
    CHECK(std::string(siaara_last_error_message()).rfind("INCOMPLETE_INPUT", 0) == 0);
    siaara_web_stow_options so;
    std::memset(&so, 0, sizeof so);
    so.struct_size = sizeof so;
    CHECK(siaara_web_stow_ex(wc, tf, nullptr, &so) == SIAARA_ERR_REJECTED);
    CHECK(std::string(siaara_last_error_message()).rfind("INCOMPLETE_INPUT", 0) == 0);
    CHECK(siaara_web_stow_ex(wc, tf, nullptr, nullptr) == SIAARA_ERR_REJECTED);
    CHECK(st.instances() == 0);
    so.allow_incomplete = 2;
    CHECK(siaara_web_stow_ex(wc, tf, nullptr, &so) == SIAARA_ERR_INVALID_ARGUMENT);
    so.allow_incomplete = 1;
    so.struct_size = 4;
    CHECK(siaara_web_stow_ex(wc, tf, nullptr, &so) == SIAARA_ERR_INVALID_ARGUMENT);
    so.struct_size = sizeof so;
    CHECK(st.instances() == 0);
    // Opted in: what was read is sent.
    CHECK(siaara_web_stow_ex(wc, tf, nullptr, &so) == SIAARA_OK);
    CHECK(st.instances() == 1);
    siaara_file_free(tf);
    // A complete file goes through both calls.
    siaara_file* gf = to_capi(whole);
    REQUIRE(gf);
    CHECK(siaara_web_stow(wc, gf, nullptr) == SIAARA_OK);
    CHECK(st.instances() == 2);
    siaara_file_free(gf);

    // The C++ client sends bytes as they are, so it reads each part back first.
    web::DicomwebClientConfig cc;
    cc.base_url = base;
    web::DicomwebClient c(cc);
    const auto cut2 = truncated_part10(make_image("2.25.9803", "2.25.9810", "2.25.9811", "STOWI", "Stow^Inc", true));
    const std::string cut_part(reinterpret_cast<const char*>(cut2.data()), cut2.size());
    const auto good3 = part10(make_image("2.25.9804", "2.25.9810", "2.25.9811", "STOWI", "Stow^Inc", true));
    const std::string good_part(reinterpret_cast<const char*>(good3.data()), good3.size());
    auto refused = c.store({good_part, cut_part});
    REQUIRE(!refused.is_ok());
    CHECK(refused.error().code == Errc::Rejected);
    CHECK(refused.error().message.rfind("INCOMPLETE_INPUT", 0) == 0);
    CHECK(refused.error().message.find("part 2 of 2") != std::string::npos);
    auto garbage = c.store({good_part, std::string("not a DICOM file")});
    REQUIRE(!garbage.is_ok());
    CHECK(garbage.error().code == Errc::Rejected);
    CHECK(garbage.error().message.rfind("INCOMPLETE_INPUT", 0) == 0);
    CHECK(st.instances() == 2);  // nothing was sent, not even the good part

    siaara_web_client_free(wc);
    siaara_web_server_free(ws);
}

TEST_CASE("net2: multipart StreamParser gives the same answer however the body is cut (fuzz finding)") {
    // A dash-boundary in the middle of a line is not a delimiter. Before the fix, trimming the
    // preamble buffer could leave it at offset 0 and it was taken for one: "ggg--g--g" with
    // boundary "g" failed when fed whole and passed when fed in small pieces.
    const std::string body = "ggg--g--g";
    for (std::size_t piece = 1; piece <= body.size(); ++piece) {
        web::multipart::StreamParser p("g", {}, 1 << 20, [](const web::multipart::Part&) { return Status::ok(); });
        bool fed = true;
        for (std::size_t i = 0; i < body.size() && fed; i += piece) fed = static_cast<bool>(p.feed(std::string_view(body).substr(i, piece)));
        CHECK(!(fed && static_cast<bool>(p.finish())));
    }
    // A dash-boundary cut exactly at the end of a piece is judged by the byte that follows it.
    const std::string good = "x\r\n--b\r\n\r\nabc--bX\r\n--b--";
    for (std::size_t piece = 1; piece <= good.size(); ++piece) {
        std::vector<std::string> bodies;
        web::multipart::StreamParser p("b", {}, 1 << 20, [&](const web::multipart::Part& part) {
            bodies.emplace_back(part.body);
            return Status::ok();
        });
        bool fed = true;
        for (std::size_t i = 0; i < good.size() && fed; i += piece) fed = static_cast<bool>(p.feed(std::string_view(good).substr(i, piece)));
        CHECK(fed);
        CHECK(static_cast<bool>(p.finish()));
        REQUIRE(bodies.size() == 1u);
        CHECK(bodies[0] == "abc--bX");
    }
    // Transport padding after a boundary has the same limit in the whole-body parser.
    const std::string padded = "--b" + std::string(65, '\t') + "\n--b--";
    CHECK(!web::multipart::parse(padded, "b", {}));
    web::multipart::StreamParser p("b", {}, 1 << 20, [](const web::multipart::Part&) { return Status::ok(); });
    CHECK(!(static_cast<bool>(p.feed(padded)) && static_cast<bool>(p.finish())));
}

TEST_CASE("net2: multipart StreamParser handles any split of the body") {
    web::multipart::Writer w;
    w.add("application/dicom", std::string(300, 'a'));
    w.add("application/dicom", "");
    w.add("application/dicom", "\r\n--not-the-boundary\r\n");
    const std::string body = w.finish();
    for (std::size_t piece : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, body.size()}) {
        std::vector<std::string> got;
        web::multipart::StreamParser p(w.boundary(), {}, 1 << 20, [&](const web::multipart::Part& part) -> Status {
            got.emplace_back(part.body);
            return Status::ok();
        });
        bool fed = true;
        for (std::size_t i = 0; i < body.size(); i += piece) fed = fed && p.feed(std::string_view(body).substr(i, piece));
        CHECK(fed);
        CHECK(p.finish());
        REQUIRE(got.size() == 3);
        CHECK(got[0] == std::string(300, 'a'));
        CHECK(got[1].empty());
        CHECK(got[2] == "\r\n--not-the-boundary\r\n");
    }
    // Truncated, and a part over the limit.
    web::multipart::StreamParser t(w.boundary(), {}, 1 << 20, [](const web::multipart::Part&) { return Status::ok(); });
    CHECK(t.feed(std::string_view(body).substr(0, body.size() / 2)));
    CHECK(!t.finish());
    web::multipart::StreamParser big(w.boundary(), {}, 100, [](const web::multipart::Part&) { return Status::ok(); });
    CHECK(!big.feed(body));
}
