// Regression tests of security review round 4 (R-109 .. R-153), the independent testing findings
// (T3.1, T3.2, T-P3-2) and the viewer's C API gaps (C1, C3, C5, C6): DIMSE side. Loopback, ours
// against ours. The DICOMweb side is in test_sec4b_web.cpp.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec/transcode.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "image/frames.hpp"
#include "net/dimse.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "net/socket.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";
constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";
[[maybe_unused]] constexpr const char* kMppsClass = "1.2.840.10008.3.1.2.3.3";
constexpr const char* kUpsPush = "1.2.840.10008.5.1.4.34.6.1";
constexpr const char* kUpsPull = "1.2.840.10008.5.1.4.34.6.3";
constexpr const char* kUpsWatch = "1.2.840.10008.5.1.4.34.6.2";

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_sec4b_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

DicomFile make_image(const std::string& sop, const std::string& study, const std::string& series, bool eight_bit = false) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, eight_bit ? kMR : kCT);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, study);
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, series);
    (void)ds.set_string(tags::PatientName, VR::PN, "Sec^Four");
    (void)ds.set_string(tags::PatientID, VR::LO, "S4");
    (void)ds.set_string(tags::Modality, VR::CS, eight_bit ? "MR" : "CT");
    (void)ds.set_string(0x00080020, VR::DA, "20261111");
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

siaara_dimse_peer peer(uint32_t port, const char* called, const char* calling = "S4_SCU") {
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
        std::string calling, inst;
        uint64_t association;
        uint32_t read_loss;
        uint32_t command_field;
    };
    std::vector<E> list;
    bool wait_for(int32_t kind, int n, int ms = 5000) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] {
            int k = 0;
            for (const E& e : list) k += e.kind == kind;
            return k >= n;
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
        ev->list.push_back({e->kind, e->dimse_status, e->calling_ae, e->sop_instance_uid, e->association_id, e->read_loss,
                            e->command_field});
    }
    ev->cv.notify_all();
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

net::ScpConfig scp_cfg(const std::string& ae) {
    net::ScpConfig c;
    c.ae_title = ae;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.max_associations = 4;
    c.allow_any_calling_ae = true;
    c.timeouts = fast();
    return c;
}

net::ScuConfig scu_cfg(std::uint16_t port, const std::string& called, const std::string& calling = "S4_SCU") {
    net::ScuConfig c;
    c.calling_ae = calling;
    c.called_ae = called;
    c.host = "127.0.0.1";
    c.port = port;
    c.timeouts = fast();
    return c;
}

// A TCP listener that accepts connections and never answers them.
struct SilentPeer {
    net::TcpListener listener;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::vector<std::unique_ptr<net::Transport>> held;
    SilentPeer() {
        (void)listener.listen("127.0.0.1", 0);
        thread = std::thread([this] {
            while (!stop.load()) {
                auto t = listener.accept(50);
                if (t) {
                    std::lock_guard<std::mutex> l(mu);
                    held.push_back(std::move(t.value()));
                }
            }
        });
    }
    ~SilentPeer() {
        stop.store(true);
        thread.join();
        for (auto& t : held) t->close();
    }
    std::uint16_t port() const { return listener.port(); }
};

std::int64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

Dataset mpps_attributes(std::size_t padding = 0) {
    Dataset ds;
    (void)ds.set_string(0x00400252, VR::CS, "IN PROGRESS");
    (void)ds.set_string(0x00400253, VR::SH, "PPS1");
    if (padding) (void)ds.set_string(0x00400254, VR::LO, std::string(padding > 64 ? 64 : padding, 'x'));
    if (padding > 64) {
        const std::vector<std::uint8_t> blob(padding, 0x41);
        (void)ds.set_bytes(0x00420011, VR::OB, Bytes::copy_of(blob.data(), blob.size()));
    }
    return ds;
}

Dataset ups_attributes() {
    Dataset ds;
    (void)ds.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)ds.set_string(0x00741204, VR::LO, "Sec4 step");
    (void)ds.set_string(0x00404041, VR::CS, "READY");
    return ds;
}

const siaara_dataset* capi_ds(const Dataset& ds) { return reinterpret_cast<const siaara_dataset*>(&ds); }

}  // namespace

// ---------------------------------------------------------------- T3.1
TEST_CASE("net2: sec4 T3.1 C-MOVE decompresses for a destination that accepts only uncompressed syntaxes") {
    const auto dir = temp_dir("t31");
    StoreOpen st(dir / "store");
    REQUIRE(st.store);
    // The destination accepts only Implicit and Explicit VR Little Endian.
    std::mutex mu;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> received;  // (transfer syntax, pixel bytes)
    net::ScpConfig dcfg = scp_cfg("NATIVE_ONLY");
    dcfg.transfer_syntaxes = {std::string(ts::ImplicitVRLittleEndian), std::string(ts::ExplicitVRLittleEndian)};
    net::ScpHandlers dh;
    dh.on_store = [&](const net::StoreRequest& rq) -> std::uint16_t {
        std::vector<std::uint8_t> px;
        if (const Element* e = rq.dataset.find(tags::PixelData)) px.assign(e->value.data(), e->value.data() + e->value.size());
        std::lock_guard<std::mutex> l(mu);
        received.emplace_back(rq.transfer_syntax, std::move(px));
        return net::dimse_status::Success;
    };
    net::ScpServer dest(dcfg, dh);
    REQUIRE(dest.start().is_ok());

    // One instance per compressed transfer syntax (lossless ones keep the pixels exactly).
    const char* const syntaxes[] = {"1.2.840.10008.1.2.4.70", "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.90",
                                    "1.2.840.10008.1.2.5", "1.2.840.10008.1.2.4.50"};
    const DicomFile plain = make_image("2.25.41000", "2.25.41900", "2.25.41901", true);
    const Element* src_px = plain.dataset.find(tags::PixelData);
    REQUIRE(src_px);
    int n = 0;
    for (const char* t : syntaxes) {
        DicomFile f = make_image("2.25.4100" + std::to_string(++n), "2.25.41900", "2.25.41901", true);
        codec::TranscodeParams p;
        p.keep_uid_on_lossy = true;
        REQUIRE(codec::transcode_file(f, t, p).is_ok());
        REQUIRE(st.add(f));
    }
    CHECK(st.instances() == 5);

    siaara_scp_options o = scp_options("MOVE_SCP");
    siaara_scp_services sv = services();
    sv.move = 1;
    siaara_ae_entry ae;
    std::memset(&ae, 0, sizeof ae);
    ae.struct_size = sizeof ae;
    ae.ae_title = "NATIVE_ONLY";
    ae.host = "127.0.0.1";
    ae.port = dest.port();
    sv.ae_table = &ae;
    sv.ae_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "MOVE_SCP");
    siaara_qr_keys k;
    std::memset(&k, 0, sizeof k);
    k.struct_size = sizeof k;
    k.study_instance_uid = "2.25.41900";
    siaara_file* id = nullptr;
    REQUIRE(siaara_qr_identifier(SIAARA_LEVEL_STUDY, &k, &id) == SIAARA_OK);
    siaara_dataset* ids = nullptr;
    REQUIRE(siaara_file_dataset(id, &ids) == SIAARA_OK);
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_move_ex(&p, SIAARA_QR_STUDY_ROOT, ids, "NATIVE_ONLY", nullptr, nullptr, nullptr, nullptr, &r) ==
          SIAARA_OK);
    CHECK_EQ(r.completed, 5u);
    CHECK_EQ(r.failed, 0u);
    siaara_file_free(id);
    siaara_scp_free(scp);
    dest.stop();
    std::lock_guard<std::mutex> l(mu);
    REQUIRE(received.size() == 5);
    std::size_t exact = 0;
    for (const auto& [tsyn, px] : received) {
        CHECK(tsyn == ts::ImplicitVRLittleEndian || tsyn == ts::ExplicitVRLittleEndian);
        CHECK_EQ(px.size(), src_px->value.size());
        if (px.size() == src_px->value.size() && std::memcmp(px.data(), src_px->value.data(), px.size()) == 0) ++exact;
    }
    CHECK(exact >= 4);  // the four lossless ones arrive bit-exact; JPEG Baseline is lossy
}

// ---------------------------------------------------------------- T3.2
TEST_CASE("net2: sec4 T3.2 UPS DIMSE-N commands name the UPS Push SOP Class on any UPS context") {
    // A receiver of UPS events that records the Affected SOP Class UID of each N-EVENT-REPORT.
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> event_classes;
    net::ScpConfig wcfg = scp_cfg("WATCHER");
    wcfg.accepted = {{std::string(net::sop::UpsEvent), net::native_transfer_syntaxes(), true, true}};
    net::ScpHandlers wh;
    wh.on_n_request = [&](const net::Message& m, const Dataset&, Dataset&, bool&) -> std::uint16_t {
        std::lock_guard<std::mutex> l(mu);
        event_classes.push_back(m.command.affected_sop_class_uid);
        cv.notify_all();
        return net::dimse_status::Success;
    };
    net::ScpServer watcher(wcfg, wh);
    REQUIRE(watcher.start().is_ok());

    const auto dir = temp_dir("t32");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("UPS_SCP");
    siaara_scp_services sv = services();
    sv.ups = 1;
    siaara_ae_entry ae;
    std::memset(&ae, 0, sizeof ae);
    ae.struct_size = sizeof ae;
    ae.ae_title = "WATCHER";
    ae.host = "127.0.0.1";
    ae.port = watcher.port();
    sv.ae_table = &ae;
    sv.ae_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "UPS_SCP");
    const Dataset attrs = ups_attributes();
    const auto call = [&](uint32_t field, const char* abstract, const char* cls, const char* uid, uint32_t type,
                          const Dataset* ds) -> uint32_t {
        siaara_n_request rq;
        std::memset(&rq, 0, sizeof rq);
        rq.struct_size = sizeof rq;
        rq.command_field = field;
        rq.abstract_syntax = abstract;
        rq.sop_class_uid = cls;
        rq.sop_instance_uid = uid;
        rq.type_id = type;
        rq.dataset = ds ? capi_ds(*ds) : nullptr;
        siaara_dimse_result r = result();
        siaara_file* out = nullptr;
        (void)siaara_dimse_n_request(&p, &rq, nullptr, &out, nullptr, 0, nullptr, &r);
        siaara_file_free(out);
        return r.status;
    };
    // Subscribe (UPS Watch context, UPS Push UID in the command - the standard form).
    Dataset sub;
    (void)sub.set_string(0x00741234, VR::AE, "WATCHER");
    (void)sub.set_string(0x00741230, VR::CS, "FALSE");
    CHECK_EQ(call(SIAARA_N_ACTION, kUpsWatch, kUpsPush, "1.2.840.10008.5.1.4.34.5", 3, &sub), 0u);
    CHECK_EQ(call(SIAARA_N_CREATE, kUpsPush, kUpsPush, "2.25.42001", 0, &attrs), 0u);
    // N-GET on a UPS Pull association: the standard form (Push UID) and the negotiated UID both work.
    CHECK_EQ(call(SIAARA_N_GET, kUpsPull, kUpsPush, "2.25.42001", 0, nullptr), 0u);
    CHECK_EQ(call(SIAARA_N_GET, kUpsPull, kUpsPull, "2.25.42001", 0, nullptr), 0u);
    CHECK_EQ(call(SIAARA_N_GET, kUpsWatch, kUpsPush, "2.25.42001", 0, nullptr), 0u);
    // N-ACTION Change State on a Pull association with the Push UID.
    Dataset state;
    (void)state.set_string(0x00741000, VR::CS, "IN PROGRESS");
    (void)state.set_string(0x00081195, VR::UI, "2.25.42099");
    CHECK_EQ(call(SIAARA_N_ACTION, kUpsPull, kUpsPush, "2.25.42001", 1, &state), 0u);
    // A workitem cannot be created through a Pull association.
    CHECK(call(SIAARA_N_CREATE, kUpsPull, kUpsPush, "2.25.42002", 0, &attrs) != 0u);
    // The C++ helpers send the Push UID themselves.
    {
        net::Scu scu(scu_cfg(static_cast<std::uint16_t>(port_of(scp)), "UPS_SCP"));
        scu.add_context(std::string(net::sop::UpsPull), net::native_transfer_syntaxes());
        REQUIRE(scu.connect().is_ok());
        auto g = net::ups_get(scu, net::sop::UpsPull, "2.25.42001");
        REQUIRE(g.is_ok());
        CHECK_EQ(g->command.status.value_or(0xFFFF), 0);
        CHECK(g->command.affected_sop_class_uid == net::sop::UpsPush);
        CHECK(scu.release().is_ok());
    }
    // The event reports name UPS Push (PS3.4 CC.2.4.3) on a context negotiated for UPS Event.
    {
        std::unique_lock<std::mutex> l(mu);
        CHECK(cv.wait_for(l, std::chrono::seconds(8), [&] { return event_classes.size() >= 2; }));
        for (const std::string& c : event_classes) CHECK(c == net::sop::UpsPush);
    }
    siaara_scp_free(scp);
    watcher.stop();
}

// ---------------------------------------------------------------- R-109
TEST_CASE("net2: sec4 R-109 N-service data sets are capped before reassembly; C-STORE is not") {
    const auto dir = temp_dir("r109");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("CAP_SCP");
    siaara_scp_services sv = services();
    sv.mpps = sv.storage = 1;
    sv.max_service_dataset_bytes = 64 * 1024;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "CAP_SCP");
    // A small MPPS N-CREATE is served.
    const Dataset small = mpps_attributes();
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_mpps_create(&p, "2.25.43001", capi_ds(small), nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
    // One with a 200 KB data set ends the association: it never reaches the service.
    const Dataset big = mpps_attributes(200 * 1024);
    r = result();
    const siaara_status s = siaara_dimse_mpps_create(&p, "2.25.43002", capi_ds(big), nullptr, nullptr, 0, nullptr, &r);
    CHECK(s == SIAARA_ERR_NETWORK || s == SIAARA_ERR_TIMEOUT);
    siaara_file* got = nullptr;
    CHECK(siaara_scp_mpps_get(scp, "2.25.43002", &got) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_scp_mpps_get(scp, "2.25.43001", &got) == SIAARA_OK);
    siaara_file_free(got);
    // A C-STORE data set larger than that cap is still accepted.
    DicomFile image = make_image("2.25.43010", "2.25.43900", "2.25.43901");
    const std::vector<std::uint8_t> blob(300 * 1024, 0x42);
    (void)image.dataset.set_bytes(0x00420011, VR::OB, Bytes::copy_of(blob.data(), blob.size()));
    const auto bytes = part10(image);
    siaara_file* cf = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &cf) == SIAARA_OK);
    r = result();
    CHECK(siaara_dimse_store(&p, cf, nullptr, &r) == SIAARA_OK);
    siaara_file_free(cf);
    CHECK(st.instances() == 1);
    siaara_scp_free(scp);
}

TEST_CASE("net2: sec4 R-109 R-119 the MPPS and UPS models are bounded and evict finished instances") {
    net::ServiceStateLimits lim;
    lim.max_items = 3;
    lim.max_finished = 2;
    net::MppsStore mpps(lim);
    Dataset done;
    (void)done.set_string(0x00400252, VR::CS, "COMPLETED");
    // Only IN PROGRESS instances count against the cap: finishing one makes room for the next.
    for (int i = 0; i < 20; ++i) {
        const std::string uid = "2.25.4400" + std::to_string(i);
        CHECK_EQ(mpps.create(uid, mpps_attributes()), net::dimse_status::Success);
        CHECK_EQ(mpps.set(uid, done), net::dimse_status::Success);
    }
    CHECK_EQ(mpps.in_progress(), 0u);
    CHECK_EQ(mpps.size(), 2u);                       // the two newest finished ones are kept
    CHECK(!mpps.get("2.25.44000").has_value());      // the oldest was evicted
    CHECK(mpps.get("2.25.440019").has_value());
    for (int i = 0; i < 3; ++i) CHECK_EQ(mpps.create("2.25.4410" + std::to_string(i), mpps_attributes()), 0);
    CHECK_EQ(mpps.create("2.25.44199", mpps_attributes()), net::dimse_status::OutOfResources);
    // The byte budget: an N-SET that would pass it is refused and nothing changes.
    net::ServiceStateLimits tight;
    tight.max_bytes = 4096;
    net::MppsStore small(tight);
    CHECK_EQ(small.create("2.25.44201", mpps_attributes()), 0);
    Dataset grow;
    const std::vector<std::uint8_t> blob(8192, 0x41);
    (void)grow.set_bytes(0x00420011, VR::OB, Bytes::copy_of(blob.data(), blob.size()));
    CHECK_EQ(small.set("2.25.44201", grow), net::dimse_status::OutOfResources);
    CHECK(!small.get("2.25.44201")->contains(0x00420011));
    CHECK(small.bytes() <= 4096);

    // UPS: item cap, eviction of final workitems, subscriber cap (R-117).
    net::ServiceStateLimits ul;
    ul.max_items = 2;
    ul.max_finished = 1;
    ul.max_subscribers_per_item = 2;
    net::UpsProvider ups(ul);
    CHECK_EQ(ups.create("2.25.44301", ups_attributes()), 0);
    CHECK_EQ(ups.create("2.25.44302", ups_attributes()), 0);
    CHECK_EQ(ups.create("2.25.44303", ups_attributes()), net::dimse_status::OutOfResources);
    CHECK_EQ(ups.subscribe("2.25.44301", "A", false), 0);
    CHECK_EQ(ups.subscribe("2.25.44301", "B", false), 0);
    CHECK_EQ(ups.subscribe("2.25.44301", "C", false), net::dimse_status::OutOfResources);
    CHECK_EQ(ups.subscribe("2.25.44301", "A", false), 0);  // already subscribed: not a new one
    CHECK_EQ(ups.request_cancel("2.25.44301", "X", Dataset{}), 0);  // SCHEDULED -> CANCELED
    CHECK_EQ(ups.request_cancel("2.25.44302", "X", Dataset{}), 0);
    CHECK_EQ(ups.size(), 1u);  // one final workitem kept, the older one evicted
    CHECK_EQ(ups.create("2.25.44304", ups_attributes()), 0);
    // Pending events are bounded when nobody takes them.
    CHECK(ups.take_events().size() <= ul.max_pending_events);
}

TEST_CASE("net2: sec4 R-119 MPPS and UPS state survives a restart when a state folder is given") {
    const auto dir = temp_dir("r119");
    const std::string state = (dir / "state").string();
    const Dataset attrs = mpps_attributes();
    const Dataset ups = ups_attributes();
    {
        StoreOpen st(dir / "store");
        siaara_scp_options o = scp_options("STATE_SCP");
        siaara_scp_services sv = services();
        sv.mpps = sv.ups = 1;
        sv.state_folder = state.c_str();
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
        siaara_dimse_peer p = peer(port_of(scp), "STATE_SCP");
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_mpps_create(&p, "2.25.45001", capi_ds(attrs), nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
        Dataset set;
        (void)set.set_string(0x00400252, VR::CS, "COMPLETED");
        (void)set.set_string(0x00400254, VR::LO, "done here");
        r = result();
        CHECK(siaara_dimse_mpps_set(&p, "2.25.45001", capi_ds(set), nullptr, &r) == SIAARA_OK);
        r = result();
        CHECK(siaara_dimse_mpps_create(&p, "2.25.45002", capi_ds(attrs), nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
        CHECK(siaara_scp_ups_create(scp, "2.25.45010", capi_ds(ups), nullptr, 0, nullptr) == SIAARA_OK);
        siaara_scp_free(scp);
    }
#if !defined(_WIN32)
    // The state files are owner-only.
    for (const auto& e : std::filesystem::recursive_directory_iterator(state)) {
        const auto perms = std::filesystem::status(e.path()).permissions();
        CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
              std::filesystem::perms::none);
    }
#endif
    {
        StoreOpen st(dir / "store");
        siaara_scp_options o = scp_options("STATE_SCP");
        siaara_scp_services sv = services();
        sv.mpps = sv.ups = 1;
        sv.state_folder = state.c_str();
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
        siaara_file* f = nullptr;
        REQUIRE(siaara_scp_mpps_get(scp, "2.25.45001", &f) == SIAARA_OK);
        siaara_dataset* ds = nullptr;
        REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
        const auto* d = reinterpret_cast<const Dataset*>(ds);
        CHECK(d->get_string(0x00400252).value_or("") == "COMPLETED");
        CHECK(d->get_string(0x00400254).value_or("") == "done here");
        siaara_file_free(f);
        // The reloaded instance keeps its state: a finished one may not be updated, an open one may.
        siaara_dimse_peer p = peer(port_of(scp), "STATE_SCP");
        Dataset set;
        (void)set.set_string(0x00400254, VR::LO, "again");
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_mpps_set(&p, "2.25.45001", capi_ds(set), nullptr, &r) == SIAARA_ERR_REJECTED);
        r = result();
        CHECK(siaara_dimse_mpps_set(&p, "2.25.45002", capi_ds(set), nullptr, &r) == SIAARA_OK);
        r = result();
        CHECK(siaara_dimse_mpps_create(&p, "2.25.45001", capi_ds(attrs), nullptr, nullptr, 0, nullptr, &r) ==
              SIAARA_ERR_REJECTED);  // duplicate of a reloaded instance
        REQUIRE(siaara_scp_ups_get(scp, "2.25.45010", &f) == SIAARA_OK);
        siaara_file_free(f);
        siaara_scp_free(scp);
    }
}

TEST_CASE("net2: sec4 R-109 the SCP event queue is bounded in bytes") {
    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        bool open = false;
        int calls = 0;
    } gate;
    const auto dir = temp_dir("r109q");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("QUEUE_SCP");
    o.event_queue_max_bytes = 64 * 1024;
    siaara_scp_services sv = services();
    sv.mpps = 1;
    siaara_scp* scp = nullptr;
    const siaara_scp_callback cb = [](void* user, const siaara_scp_event*) {
        auto* g = static_cast<Gate*>(user);
        std::unique_lock<std::mutex> l(g->mu);
        ++g->calls;
        g->cv.wait(l, [&] { return g->open; });
    };
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, cb, &gate, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "QUEUE_SCP");
    const Dataset attrs = mpps_attributes(40 * 1024);
    // The callback blocks on the first event; the queue then holds at most 64 KiB of data sets.
    for (int i = 0; i < 6; ++i) {
        siaara_dimse_result r = result();
        const std::string uid = "2.25.4600" + std::to_string(i);
        CHECK(siaara_dimse_mpps_create(&p, uid.c_str(), capi_ds(attrs), nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
    }
    siaara_scp_stats stats;
    std::memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    REQUIRE(siaara_scp_stats_get(scp, &stats) == SIAARA_OK);
    CHECK(stats.events_dropped >= 3);  // the operations themselves all succeeded
    {
        std::lock_guard<std::mutex> l(gate.mu);
        gate.open = true;
    }
    gate.cv.notify_all();
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- R-116
namespace {

// Sends a C-STORE whose data set is `body` (already encoded in Explicit VR LE) and returns the status.
std::uint16_t raw_store(std::uint16_t port, const char* called, const char* sop_class, const std::string& uid,
                        const std::vector<std::uint8_t>& body) {
    net::Scu scu(scu_cfg(port, called));
    scu.add_context(sop_class, {std::string(ts::ExplicitVRLittleEndian)});
    if (!scu.connect()) return 0xFFFF;
    net::Association* a = scu.association();
    const net::NegotiatedContext* pc = a->find_context(sop_class);
    if (!pc) return 0xFFFE;
    const std::uint16_t mid = a->next_message_id();
    if (!net::send_dimse_raw(*a, pc->id, net::make_c_store_rq(mid, sop_class, uid), body.data(), body.size())) return 0xFFFD;
    auto r = net::receive_dimse(*a, 5000);
    const std::uint16_t st = r ? r->message.command.status.value_or(0xFFFC) : 0xFFFB;
    (void)scu.release();
    return st;
}

std::vector<std::uint8_t> dataset_bytes(const DicomFile& f) {
    std::vector<std::uint8_t> body;
    (void)write_dataset(f.dataset, ts::ExplicitVRLittleEndian, body);
    return body;
}

}  // namespace

TEST_CASE("net2: sec4 R-116 a truncated C-STORE data set is refused by default, or kept verbatim and flagged") {
    const DicomFile whole = make_image("2.25.47001", "2.25.47900", "2.25.47901", true);
    std::vector<std::uint8_t> cut = dataset_bytes(make_image("2.25.47002", "2.25.47900", "2.25.47901", true));
    cut.resize(cut.size() - 60);  // the data set ends inside Pixel Data
    {  // REFUSE: 0xC000, nothing stored, the event says why.
        const auto dir = temp_dir("r116a");
        StoreOpen st(dir / "store");
        Events ev;
        siaara_scp_options o = scp_options("INC_SCP");
        o.incomplete_policy = SIAARA_INCOMPLETE_REFUSE;
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start(&o, st.store, on_event, &ev, &scp) == SIAARA_OK);
        const auto port = static_cast<std::uint16_t>(port_of(scp));
        CHECK_EQ(raw_store(port, "INC_SCP", kMR, "2.25.47002", cut), 0xC000);
        CHECK(st.instances() == 0);
        CHECK_EQ(raw_store(port, "INC_SCP", kMR, "2.25.47001", dataset_bytes(whole)), 0x0000);
        CHECK(st.instances() == 1);
        REQUIRE(ev.wait_for(SIAARA_SCP_STORE_FAILED, 1));
        REQUIRE(ev.wait_for(SIAARA_SCP_STORED, 1));
        const auto failed = ev.of(SIAARA_SCP_STORE_FAILED);
        CHECK_EQ(failed[0].status, 0xC000u);
        CHECK((failed[0].read_loss & SIAARA_READ_INPUT_TRUNCATED) != 0);
        CHECK_EQ(ev.of(SIAARA_SCP_STORED)[0].read_loss, 0u);
        siaara_scp_free(scp);
    }
    {  // ACCEPT_AS_RECEIVED: the bytes are stored exactly as they arrived, never converted, and flagged.
        const auto dir = temp_dir("r116b");
        StoreOpen st(dir / "store");
        Events ev;
        siaara_scp_options o = scp_options("INC_SCP");
        o.incomplete_policy = SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED;
        siaara_scp_services sv = services();
        sv.storage = 1;
        sv.store_transfer_syntax = "1.2.840.10008.1.2.5";  // RLE: a complete instance is converted
        siaara_scp* scp = nullptr;
        REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
        const auto port = static_cast<std::uint16_t>(port_of(scp));
        CHECK_EQ(raw_store(port, "INC_SCP", kMR, "2.25.47002", cut), 0xB007);  // stored, with the warning answer
        CHECK_EQ(raw_store(port, "INC_SCP", kMR, "2.25.47001", dataset_bytes(whole)), 0x0000);
        REQUIRE(ev.wait_for(SIAARA_SCP_STORED, 2));
        for (const auto& e : ev.of(SIAARA_SCP_STORED)) {
            if (e.inst == "2.25.47002") {
                CHECK((e.read_loss & SIAARA_READ_INPUT_TRUNCATED) != 0);
                CHECK_EQ(e.status, 0xB007u);  // P3-2: the status the sender was answered with, not 0x0000
            } else {
                CHECK_EQ(e.read_loss, 0u);
                CHECK_EQ(e.status, 0x0000u);
            }
        }
        siaara_scp_free(scp);
        // The incomplete one kept its transfer syntax and its bytes; the complete one was converted.
        std::string path_cut, path_whole;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir / "store")) {
            if (!e.is_regular_file()) continue;
            auto f = read_file(e.path().string());
            if (!f) continue;
            const std::string uid = f->dataset.get_string(tags::SOPInstanceUID).value_or("");
            if (uid == "2.25.47002") {
                path_cut = e.path().string();
                CHECK(f->transfer_syntax == ts::ExplicitVRLittleEndian);
                CHECK(!f->complete());
            } else if (uid == "2.25.47001") {
                path_whole = e.path().string();
                CHECK(f->transfer_syntax == "1.2.840.10008.1.2.5");
            }
        }
        REQUIRE(!path_cut.empty());
        CHECK(!path_whole.empty());
        auto stored = io::read_file(path_cut);
        REQUIRE(stored.is_ok());
        REQUIRE(stored->size() > cut.size());
        CHECK(std::memcmp(stored->data() + stored->size() - cut.size(), cut.data(), cut.size()) == 0);
    }
}

// ---------------------------------------------------------------- R-117
TEST_CASE("net2: sec4 R-117 event reports do not pin the requestor's association or siaara_scp_stop") {
    SilentPeer silent;  // a subscriber that accepts the connection and never answers
    const auto dir = temp_dir("r117");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("EV_SCP");
    siaara_scp_services sv = services();
    sv.ups = 1;
    siaara_ae_entry ae;
    std::memset(&ae, 0, sizeof ae);
    ae.struct_size = sizeof ae;
    ae.ae_title = "SILENT";
    ae.host = "127.0.0.1";
    ae.port = silent.port();
    sv.ae_table = &ae;
    sv.ae_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "EV_SCP");
    const Dataset attrs = ups_attributes();
    CHECK(siaara_scp_ups_create(scp, "2.25.48001", capi_ds(attrs), nullptr, 0, nullptr) == SIAARA_OK);
    Dataset sub;
    (void)sub.set_string(0x00741234, VR::AE, "SILENT");
    (void)sub.set_string(0x00741230, VR::CS, "FALSE");
    siaara_n_request rq;
    std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.command_field = SIAARA_N_ACTION;
    rq.abstract_syntax = kUpsWatch;
    rq.sop_class_uid = kUpsPush;
    rq.sop_instance_uid = "2.25.48001";
    rq.type_id = 3;
    rq.dataset = capi_ds(sub);
    // The subscription produces an event report for SILENT: the N-ACTION itself returns at once,
    // and so do the next ones (the old code sent the report on this thread with full timeouts).
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_n_request(&p, &rq, nullptr, nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
    }
    CHECK(ms_since(t0) < 2500);
    // Stopping aborts the outbound association instead of waiting for the silent peer.
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(siaara_scp_stop(scp) == SIAARA_OK);
    CHECK(ms_since(t1) < 2500);
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- R-118
TEST_CASE("net2: sec4 R-118 a storage commitment report from another AE is ignored") {
    const auto dir = temp_dir("r118");
    StoreOpen st(dir / "store");
    StoreOpen rst(dir / "receiver");
    REQUIRE(st.add(make_image("2.25.49001", "2.25.49900", "2.25.49901")));
    // The commitment SCP reports on a new association only, and has no address for us: no report.
    siaara_scp_options o = scp_options("COMMIT_SCP");
    siaara_scp_services sv = services();
    sv.commitment = 1;
    sv.commitment_report_mode = SIAARA_COMMIT_REPORT_NEW;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    // Our receiver of reports.
    Events ev;
    siaara_scp_options ro = scp_options("S4_SCU");
    siaara_scp_services rsv = services();
    rsv.commitment_reports = 1;
    siaara_scp* receiver = nullptr;
    REQUIRE(siaara_scp_start_ex(&ro, &rsv, rst.store, on_event, &ev, &receiver) == SIAARA_OK);

    const char* classes[] = {kCT};
    const char* instances[] = {"2.25.49001"};
    siaara_status commit_status = SIAARA_OK;
    uint16_t reason = 0xFFFF;
    std::thread waiter([&] {
        siaara_dimse_peer p = peer(port_of(scp), "COMMIT_SCP");
        siaara_commit_options co;
        std::memset(&co, 0, sizeof co);
        co.struct_size = sizeof co;
        co.wait_ms = 4000;
        co.receiver = receiver;
        co.same_association_ms = -1;
        co.transaction_uid = "2.25.49500";
        siaara_dimse_result r = result();
        commit_status = siaara_dimse_commit_ex(&p, &co, classes, instances, 1, nullptr, &reason, &r);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    // Another allowed AE that knows the Transaction UID claims "all committed".
    Dataset forged;
    (void)forged.set_string(0x00081195, VR::UI, "2.25.49500");
    {
        Dataset ref;
        (void)ref.set_string(0x00081150, VR::UI, kCT);
        (void)ref.set_string(0x00081155, VR::UI, "2.25.49001");
        Element seq;
        seq.tag = 0x00081199;
        seq.vr = VR::SQ;
        seq.items.push_back(std::move(ref));
        forged.put(std::move(seq));
    }
    siaara_dimse_peer evil = peer(port_of(receiver), "S4_SCU", "EVIL_AE");
    siaara_n_request rq;
    std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.command_field = SIAARA_N_EVENT_REPORT;
    rq.sop_class_uid = "1.2.840.10008.1.20.1";
    rq.sop_instance_uid = "1.2.840.10008.1.20.1.1";
    rq.type_id = 1;
    rq.dataset = capi_ds(forged);
    rq.propose_scp_role = 1;
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_n_request(&evil, &rq, nullptr, nullptr, nullptr, 0, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK_EQ(r.status, 0x0124u);
    // From the right AE, a report listing an instance that was never requested is ignored as well.
    Dataset wrong = forged;
    {
        Dataset ref;
        (void)ref.set_string(0x00081150, VR::UI, kCT);
        (void)ref.set_string(0x00081155, VR::UI, "2.25.49777");
        Element seq;
        seq.tag = 0x00081199;
        seq.vr = VR::SQ;
        seq.items.push_back(std::move(ref));
        wrong.put(std::move(seq));
    }
    siaara_dimse_peer right = peer(port_of(receiver), "S4_SCU", "COMMIT_SCP");
    rq.dataset = capi_ds(wrong);
    r = result();
    CHECK(siaara_dimse_n_request(&right, &rq, nullptr, nullptr, nullptr, 0, nullptr, &r) == SIAARA_ERR_REJECTED);
    waiter.join();
    CHECK(commit_status == SIAARA_ERR_TIMEOUT);  // the forged reports did not complete the request
    CHECK_EQ(reason, 0xFFFF);
    siaara_scp_free(receiver);
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- R-128, T-P3-2
TEST_CASE("net2: sec4 R-128 T-P3-2 UID lists are capped; C-FIND responses carry Retrieve AE Title") {
    const auto dir = temp_dir("r128");
    StoreOpen st(dir / "store");
    for (int i = 0; i < 4; ++i)
        REQUIRE(st.add(make_image("2.25.5000" + std::to_string(i), "2.25.50900", "2.25.50901")));
    siaara_scp_options o = scp_options("FIND_SCP");
    siaara_scp_services sv = services();
    sv.find = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    net::Scu scu(scu_cfg(static_cast<std::uint16_t>(port_of(scp)), "FIND_SCP"));
    scu.add_context(std::string(net::sop::StudyRootFind), net::native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    const auto find = [&](const std::string& uids, std::vector<Dataset>& out) -> std::uint16_t {
        Dataset id;
        (void)id.set_string(0x00080052, VR::CS, "IMAGE");
        (void)id.set_string(tags::StudyInstanceUID, VR::UI, "2.25.50900");
        (void)id.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.50901");
        Element e;
        e.tag = tags::SOPInstanceUID;
        e.vr = VR::UI;
        std::string v = uids;
        if (v.size() % 2) v.push_back('\0');
        e.value = Bytes::copy_of(v.data(), v.size());
        id.put(std::move(e));
        auto r = scu.find(net::sop::StudyRootFind, id, [&](const Dataset& d) {
            out.push_back(d);
            return true;
        });
        return r ? r->status : std::uint16_t{0xFFFF};
    };
    std::vector<Dataset> got;
    // A list of UIDs (one repeated, one unknown): each instance once.
    CHECK_EQ(find("2.25.50001\\2.25.50003\\2.25.50001\\2.25.59999", got), 0x0000);
    CHECK_EQ(got.size(), 2u);
    // T-P3-2: Retrieve AE Title is in every response although the identifier did not ask for it.
    for (const Dataset& d : got) CHECK(d.get_string(0x00080054).value_or("") == "FIND_SCP");
    // More than 10,000 values: refused, not run.
    std::string many;
    for (int i = 0; i < 10001; ++i) many += (i ? "\\2.25.6" : "2.25.6") + std::to_string(i);
    got.clear();
    CHECK_EQ(find(many, got), net::dimse_status::OutOfResources);
    CHECK(got.empty());
    CHECK(scu.release().is_ok());
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- R-129
TEST_CASE("net2: sec4 R-129 C-MOVE destinations can be limited per calling AE") {
    const auto dir = temp_dir("r129");
    StoreOpen st(dir / "store");
    StoreOpen d1(dir / "d1");
    StoreOpen d2(dir / "d2");
    REQUIRE(st.add(make_image("2.25.51001", "2.25.51900", "2.25.51901")));
    siaara_scp_options o1 = scp_options("DEST_ONE"), o2 = scp_options("DEST_TWO");
    siaara_scp *dest1 = nullptr, *dest2 = nullptr;
    REQUIRE(siaara_scp_start(&o1, d1.store, nullptr, nullptr, &dest1) == SIAARA_OK);
    REQUIRE(siaara_scp_start(&o2, d2.store, nullptr, nullptr, &dest2) == SIAARA_OK);
    siaara_ae_entry aes[2];
    std::memset(aes, 0, sizeof aes);
    aes[0].struct_size = aes[1].struct_size = sizeof(siaara_ae_entry);
    aes[0].ae_title = "DEST_ONE";
    aes[1].ae_title = "DEST_TWO";
    aes[0].host = aes[1].host = "127.0.0.1";
    aes[0].port = port_of(dest1);
    aes[1].port = port_of(dest2);
    const char* allowed[] = {"DEST_ONE"};
    siaara_move_acl acl;
    std::memset(&acl, 0, sizeof acl);
    acl.struct_size = sizeof acl;
    acl.calling_ae = "LIMITED";
    acl.destination_aes = allowed;
    acl.count = 1;
    siaara_scp_options o = scp_options("MOVE_SCP");
    siaara_scp_services sv = services();
    sv.move = 1;
    sv.ae_table = aes;
    sv.ae_count = 2;
    sv.move_acls = &acl;
    sv.move_acl_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_qr_keys k;
    std::memset(&k, 0, sizeof k);
    k.struct_size = sizeof k;
    k.study_instance_uid = "2.25.51900";
    siaara_file* id = nullptr;
    REQUIRE(siaara_qr_identifier(SIAARA_LEVEL_STUDY, &k, &id) == SIAARA_OK);
    siaara_dataset* ids = nullptr;
    REQUIRE(siaara_file_dataset(id, &ids) == SIAARA_OK);
    const auto move = [&](const char* calling, const char* dest, siaara_dimse_result& r) {
        siaara_dimse_peer p = peer(port_of(scp), "MOVE_SCP", calling);
        r = result();
        return siaara_dimse_move_ex(&p, SIAARA_QR_STUDY_ROOT, ids, dest, nullptr, nullptr, nullptr, nullptr, &r);
    };
    siaara_dimse_result r;
    CHECK(move("LIMITED", "DEST_TWO", r) == SIAARA_ERR_REJECTED);
    CHECK_EQ(r.status, 0xA801u);
    CHECK(d2.instances() == 0);
    CHECK(move("LIMITED", "DEST_ONE", r) == SIAARA_OK);
    CHECK(d1.instances() == 1);
    CHECK(move("OTHER_AE", "DEST_TWO", r) == SIAARA_OK);  // no entry for this caller: any ae_table entry
    CHECK(d2.instances() == 1);
    siaara_file_free(id);
    siaara_scp_free(scp);
    siaara_scp_free(dest1);
    siaara_scp_free(dest2);
}

// ---------------------------------------------------------------- R-130
namespace {
struct HangingWorklist {
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> calls{0};
};
uint32_t SIAARA_CALL hanging_worklist(void* user, const char*, const siaara_dataset*, siaara_worklist_emit, void*) {
    auto* h = static_cast<HangingWorklist*>(user);
    h->calls.fetch_add(1);
    std::unique_lock<std::mutex> l(h->mu);
    h->cv.wait(l, [&] { return h->release; });
    return 0;
}
}  // namespace

TEST_CASE("net2: sec4 R-130 a worklist callback that hangs does not hold the association or siaara_scp_stop") {
    HangingWorklist hang;
    const auto dir = temp_dir("r130");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("WL_SCP");
    siaara_scp_services sv = services();
    sv.worklist = 1;
    sv.worklist_cb = hanging_worklist;
    sv.worklist_user = &hang;
    sv.worklist_timeout_ms = 400;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "WL_SCP");
    siaara_file* q = nullptr;
    REQUIRE(siaara_worklist_query(nullptr, nullptr, nullptr, nullptr, nullptr, &q) == SIAARA_OK);
    siaara_dataset* qd = nullptr;
    REQUIRE(siaara_file_dataset(q, &qd) == SIAARA_OK);
    const auto t0 = std::chrono::steady_clock::now();
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_find_ex(&p, SIAARA_QR_WORKLIST, qd, nullptr, nullptr, nullptr, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK_EQ(r.status, 0x0110u);
    CHECK(ms_since(t0) < 3000);
    CHECK_EQ(hang.calls.load(), 1);
    // A second query starts while the first callback still hangs; stopping does not wait for it.
    std::thread second([&] {
        siaara_dimse_result r2 = result();
        (void)siaara_dimse_find_ex(&p, SIAARA_QR_WORKLIST, qd, nullptr, nullptr, nullptr, nullptr, &r2);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(siaara_scp_stop(scp) == SIAARA_OK);
    CHECK(ms_since(t1) < 3000);
    second.join();
    {
        std::lock_guard<std::mutex> l(hang.mu);
        hang.release = true;
    }
    hang.cv.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // the helper threads end
    siaara_file_free(q);
    siaara_scp_free(scp);
}

// ---------------------------------------------------------------- R-131
TEST_CASE("net2: sec4 R-131 MPPS updates can be bound to the creating AE") {
    const auto dir = temp_dir("r131");
    StoreOpen st(dir / "store");
    siaara_scp_options o = scp_options("BIND_SCP");
    siaara_scp_services sv = services();
    sv.mpps = 1;
    sv.bind_updates_to_creator = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, nullptr, nullptr, &scp) == SIAARA_OK);
    const Dataset attrs = mpps_attributes();
    Dataset set;
    (void)set.set_string(0x00400254, VR::LO, "update");
    siaara_dimse_peer creator = peer(port_of(scp), "BIND_SCP", "MODALITY_A");
    siaara_dimse_peer other = peer(port_of(scp), "BIND_SCP", "MODALITY_B");
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_mpps_create(&creator, "2.25.52001", capi_ds(attrs), nullptr, nullptr, 0, nullptr, &r) == SIAARA_OK);
    r = result();
    CHECK(siaara_dimse_mpps_set(&other, "2.25.52001", capi_ds(set), nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK_EQ(r.status, 0x0124u);
    r = result();
    CHECK(siaara_dimse_mpps_set(&creator, "2.25.52001", capi_ds(set), nullptr, &r) == SIAARA_OK);
    siaara_scp_free(scp);
    // Default (off): any allowed AE may update.
    net::MppsStore open_store;
    CHECK_EQ(open_store.create("2.25.52002", attrs, "A"), 0);
    CHECK_EQ(open_store.set("2.25.52002", set, "B"), 0);
}

// ---------------------------------------------------------------- R-150
TEST_CASE("capi/sec4 R-150 freeing the store before its SCP is deferred, not a use-after-free") {
    const auto dir = temp_dir("r150");
    const std::string root = (dir / "store").string();
    siaara_store* store = nullptr;
    REQUIRE(siaara_store_open(root.c_str(), root.size(), nullptr, &store) == SIAARA_OK);
    siaara_scp_options o = scp_options("FREE_SCP");
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start(&o, store, nullptr, nullptr, &scp) == SIAARA_OK);
    siaara_store_free(store);  // the SCP still uses it
    const DicomFile f = make_image("2.25.53001", "2.25.53900", "2.25.53901");
    const auto bytes = part10(f);
    siaara_file* cf = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &cf) == SIAARA_OK);
    siaara_dimse_peer p = peer(port_of(scp), "FREE_SCP");
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_store(&p, cf, nullptr, &r) == SIAARA_OK);  // served from the still-open store
    siaara_file_free(cf);
    siaara_scp_free(scp);  // performs the deferred free: the store can be opened again
    siaara_store* again = nullptr;
    REQUIRE(siaara_store_open(root.c_str(), root.size(), nullptr, &again) == SIAARA_OK);
    siaara_store_stats stats;
    std::memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    REQUIRE(siaara_store_stats_get(again, &stats) == SIAARA_OK);
    CHECK(stats.instances == 1);
    siaara_store_free(again);
}

// ---------------------------------------------------------------- C1, C3, C5, C6
TEST_CASE("capi/sec4 C1 the cancel token ends an SCU call during the association handshake") {
    SilentPeer silent;
    siaara_dimse_peer p = peer(silent.port(), "ANYONE");
    p.acse_timeout_ms = 10000;
    siaara_cancel* token = nullptr;
    REQUIRE(siaara_cancel_create(&token) == SIAARA_OK);
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        (void)siaara_cancel_set(token);
    });
    const auto t0 = std::chrono::steady_clock::now();
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_echo(&p, token, &r) == SIAARA_ERR_CANCELLED);
    CHECK(ms_since(t0) < 2500);  // not the 10 s association timeout (plus ARTIM)
    CHECK_EQ(r.connect_failure, SIAARA_CONNECT_CANCELLED);
    canceller.join();
    // Without a token the wait ends at the association timeout, reported as such.
    p.acse_timeout_ms = 500;
    r = result();
    CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_ERR_TIMEOUT);
    CHECK_EQ(r.connect_failure, SIAARA_CONNECT_ACSE_TIMEOUT);
    siaara_cancel_free(token);
}

TEST_CASE("capi/sec4 C3 C5 C6 association ids in events, rejection details, port in use") {
    const auto dir = temp_dir("c356");
    StoreOpen st(dir / "store");
    Events ev;
    siaara_scp_options o = scp_options("ID_SCP");
    const char* allowed[] = {"S4_SCU"};
    o.allow_any_calling_ae = 0;
    o.allowed_calling_aes = allowed;
    o.allowed_count = 1;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start(&o, st.store, on_event, &ev, &scp) == SIAARA_OK);
    const uint32_t port = port_of(scp);
    // C3: two echoes on two associations, then two stores on one.
    siaara_dimse_peer p = peer(port, "ID_SCP");
    siaara_dimse_result r = result();
    CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_OK);
    CHECK_EQ(r.connect_failure, SIAARA_CONNECT_OK);
    r = result();
    CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_OK);
    const auto f1 = (dir / "a.dcm").string(), f2 = (dir / "b.dcm").string();
    REQUIRE(write_file(make_image("2.25.54001", "2.25.54900", "2.25.54901"), f1).is_ok());
    REQUIRE(write_file(make_image("2.25.54002", "2.25.54900", "2.25.54901"), f2).is_ok());
    const char* paths[] = {f1.c_str(), f2.c_str()};
    r = result();
    CHECK(siaara_dimse_store_files(&p, paths, nullptr, 2, nullptr, nullptr, nullptr, &r) == SIAARA_OK);
    REQUIRE(ev.wait_for(SIAARA_SCP_ECHO, 2));
    REQUIRE(ev.wait_for(SIAARA_SCP_STORED, 2));
    const auto echoes = ev.of(SIAARA_SCP_ECHO);
    const auto stores = ev.of(SIAARA_SCP_STORED);
    CHECK(echoes[0].association != 0);
    CHECK(echoes[0].association != echoes[1].association);
    CHECK(stores[0].association == stores[1].association);
    CHECK(stores[0].association != echoes[0].association && stores[0].association != echoes[1].association);

    // C5: a rejected association says who rejected it and why.
    siaara_dimse_peer wrong_called = peer(port, "NOT_THIS_AE");
    r = result();
    CHECK(siaara_dimse_echo(&wrong_called, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK_EQ(r.connect_failure, SIAARA_CONNECT_REJECTED);
    CHECK_EQ(r.reject_result, 1u);
    CHECK_EQ(r.reject_source, 1u);
    CHECK_EQ(r.reject_reason, 7u);  // called AE title not recognized
    CHECK(std::string(siaara_last_error_message()).find("called AE title not recognized") != std::string::npos);
    siaara_dimse_peer wrong_calling = peer(port, "ID_SCP", "STRANGER");
    r = result();
    CHECK(siaara_dimse_echo(&wrong_calling, nullptr, &r) == SIAARA_ERR_REJECTED);
    CHECK_EQ(r.reject_reason, 3u);  // calling AE title not recognized
    // An older caller with the first version of the result struct still works.
    {
        siaara_dimse_result old_r = result();
        old_r.struct_size = static_cast<uint32_t>(offsetof(siaara_dimse_result, current) + sizeof(uint32_t));
        old_r.reject_reason = 0xDEAD;
        CHECK(siaara_dimse_echo(&wrong_called, nullptr, &old_r) == SIAARA_ERR_REJECTED);
        CHECK_EQ(old_r.reject_reason, 0xDEADu);  // not written beyond its struct_size
    }

    // C6: the port is taken.
    siaara_scp_options o2 = scp_options("SECOND");
    o2.port = port;
    siaara_scp* second = nullptr;
    CHECK(siaara_scp_start(&o2, st.store, nullptr, nullptr, &second) == SIAARA_ERR_ADDRESS_IN_USE);
    CHECK(second == nullptr);
    CHECK(std::string(siaara_last_error_message()).find(std::to_string(port)) != std::string::npos);
    siaara_web_server_options wo;
    std::memset(&wo, 0, sizeof wo);
    wo.struct_size = sizeof wo;
    wo.port = port;
    siaara_web_server* ws = nullptr;
    CHECK(siaara_web_server_start(&wo, st.store, &ws) == SIAARA_ERR_ADDRESS_IN_USE);
    siaara_scp_free(scp);

    // C5: nothing listens on the port any more.
    r = result();
    CHECK(siaara_dimse_echo(&p, nullptr, &r) == SIAARA_ERR_NETWORK);
    CHECK_EQ(r.connect_failure, SIAARA_CONNECT_REFUSED);
}

// ---------------------------------------------------------------- T-P3-11
namespace {
// Image Type (0008,0008) with all its values, padding removed.
std::string image_type(const Dataset& ds) {
    std::string v(ds.get_raw_string(0x00080008).value_or(""));
    while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.pop_back();
    return v;
}
}  // namespace

TEST_CASE("net2: sec4 T-P3-11 a lossy transcode of an image without Image Type marks the result DERIVED") {
    DicomFile f = make_image("2.25.55001", "2.25.55900", "2.25.55901", true);
    REQUIRE(!f.dataset.contains(0x00080008));
    codec::TranscodeReport rep;
    REQUIRE(codec::transcode_file(f, "1.2.840.10008.1.2.4.50", codec::TranscodeParams{}, &rep).is_ok());
    CHECK(rep.lossy_applied);
    CHECK(rep.image_type_added);
    CHECK(rep.derivation_note.rfind("IMAGE_TYPE_ADDED", 0) == 0);
    CHECK(image_type(f.dataset) == "DERIVED\\SECONDARY");
    CHECK(f.dataset.get_string(tags::SOPInstanceUID).value_or("") != "2.25.55001");  // a new image
    // With an Image Type only value 1 changes; a lossless transcode adds nothing.
    DicomFile g = make_image("2.25.55002", "2.25.55900", "2.25.55901", true);
    (void)g.dataset.set_string(0x00080008, VR::CS, "ORIGINAL\\PRIMARY\\AXIAL");
    codec::TranscodeReport rep2;
    REQUIRE(codec::transcode_file(g, "1.2.840.10008.1.2.4.50", codec::TranscodeParams{}, &rep2).is_ok());
    CHECK(!rep2.image_type_added);
    CHECK(image_type(g.dataset) == "DERIVED\\PRIMARY\\AXIAL");
    DicomFile h = make_image("2.25.55003", "2.25.55900", "2.25.55901", true);
    REQUIRE(codec::transcode_file(h, "1.2.840.10008.1.2.5").is_ok());
    CHECK(!h.dataset.contains(0x00080008));
}

// ---------------------------------------------------------------- R-135
#if !defined(_WIN32)
TEST_CASE("capi/sec4 R-135 a store whose folders are open to others is tightened on open, or reported") {
    namespace fs = std::filesystem;
    const auto dir = temp_dir("r135");
    const std::string root = (dir / "store").string();
    {
        StoreOpen st(dir / "store");
        REQUIRE(st.store);
        REQUIRE(st.add(make_image("2.25.56001", "2.25.56900", "2.25.56901")));
    }
    // A store as an older version made it: folders 0755.
    fs::path index, data;
    for (const auto& e : fs::directory_iterator(dir / "store"))
        if (e.is_directory()) (index.empty() ? index : data) = e.path();
    REQUIRE(!index.empty());
    REQUIRE(!data.empty());
    const auto open_others = fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec;
    const auto wide = [&] {
        fs::permissions(index, fs::perms::owner_all | open_others);
        fs::permissions(data, fs::perms::owner_all | open_others);
    };
    const auto is_private = [](const fs::path& p) {
        return (fs::status(p).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none;
    };
    wide();
    {  // keep_permissions = 1: left as they are (and logged).
        siaara_store_options o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.create = 1;
        o.keep_permissions = 1;
        siaara_store* s = nullptr;
        REQUIRE(siaara_store_open(root.c_str(), root.size(), &o, &s) == SIAARA_OK);
        siaara_store_free(s);
        CHECK(!is_private(index));
        CHECK(!is_private(data));
    }
    {  // Default: tightened to owner-only; the instance is still there.
        StoreOpen st(dir / "store");
        REQUIRE(st.store);
        CHECK(is_private(index));
        CHECK(is_private(data));
        CHECK(st.instances() == 1);
    }
}
#endif

// ---------------------------------------------------------------- R-133 (Windows only)
#if defined(_WIN32)
namespace {
std::size_t temp_files_in(const std::filesystem::path& dir) {
    std::size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().filename().string().rfind(".siaara-tmp-", 0) == 0) ++n;
    return n;
}
std::string slurp(const std::filesystem::path& p) {
    auto b = io::read_file(p.string());
    return b ? std::string(b->as_chars()) : std::string("<unreadable>");
}
}  // namespace

TEST_CASE("io/sec4 R-133 Windows: atomic replace through the creating handle, read-only targets, no temp file left") {
    const auto dir = temp_dir("r133");
    const std::string target = (dir / "target.dcm").string();
    const std::string one = "first contents", two = "second, longer contents";
    const auto put = [&](const std::string& text, io::WriteMode mode) {
        return io::write_file_atomic(target, reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), mode);
    };
    // Create, then replace.
    REQUIRE(put(one, io::WriteMode::Replace).is_ok());
    CHECK(slurp(target) == one);
    REQUIRE(put(two, io::WriteMode::Replace).is_ok());
    CHECK(slurp(target) == two);
    CHECK_EQ(temp_files_in(dir), 0u);
    // Replace while a reader holds the target mapped (the toolkit's readers share delete).
    {
        auto held = io::map_file(target);
        REQUIRE(held.is_ok());
        REQUIRE(put(one, io::WriteMode::Replace).is_ok());
        CHECK(std::string(held->as_chars()) == two);  // the reader keeps the file it opened
    }
    CHECK(slurp(target) == one);
    // NoReplace keeps an existing target and removes its temporary file.
    const Status kept = put(two, io::WriteMode::NoReplace);
    CHECK(kept.code == Errc::Rejected);
    CHECK(slurp(target) == one);
    CHECK_EQ(temp_files_in(dir), 0u);
    // A read-only target: the write fails at once (no 5 s of retries), the target is unchanged
    // and no temporary file with the new contents stays behind.
    std::filesystem::permissions(target, std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                             std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove);
    const auto t0 = std::chrono::steady_clock::now();
    const Status ro = put(two, io::WriteMode::Replace);
    CHECK(!ro.is_ok());
    CHECK(ms_since(t0) < 900);
    CHECK(slurp(target) == one);
    CHECK_EQ(temp_files_in(dir), 0u);
    std::filesystem::permissions(target, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    // The fallback rename (MoveFileExW) is used where the file system has no POSIX rename; on a
    // volume that has it the same call must still succeed after the read-only attribute is gone.
    REQUIRE(put(two, io::WriteMode::Replace).is_ok());
    CHECK(slurp(target) == two);
    // Many writers of one target: each write is whole, nothing is left behind.
    std::vector<std::thread> writers;
    std::atomic<int> failures{0};
    for (int w = 0; w < 4; ++w)
        writers.emplace_back([&, w] {
            const std::string text(1000 + static_cast<std::size_t>(w), static_cast<char>('a' + w));
            for (int i = 0; i < 20; ++i)
                if (!io::write_file_atomic(target, reinterpret_cast<const std::uint8_t*>(text.data()), text.size())) ++failures;
        });
    for (auto& t : writers) t.join();
    CHECK_EQ(failures.load(), 0);
    const std::string last = slurp(target);
    CHECK(last.size() >= 1000 && last.find_first_not_of(last[0]) == std::string::npos);
    CHECK_EQ(temp_files_in(dir), 0u);
}
#endif

// ---------------------------------------------------------------- R-116: quarantine
namespace {
std::vector<siaara_quarantine_record> quarantine_list(const siaara_store* store, int32_t area = SIAARA_AREA_QUARANTINE) {
    std::vector<siaara_quarantine_record> out;
    uint32_t n = 0;
    if (siaara_store_quarantine_count(store, area, &n, nullptr) != SIAARA_OK) return out;
    for (uint32_t i = 0; i < n; ++i) {
        siaara_quarantine_record r;
        std::memset(&r, 0, sizeof r);
        r.struct_size = sizeof r;
        if (siaara_store_quarantine_get(store, area, i, &r) == SIAARA_OK) out.push_back(r);
    }
    return out;
}
}  // namespace

TEST_CASE("net2: sec4 R-116 quarantine keeps incomplete instances outside the archive; release, delete, commitment") {
    const DicomFile whole = make_image("2.25.57001", "2.25.57900", "2.25.57901", true);
    std::vector<std::uint8_t> cut = dataset_bytes(make_image("2.25.57002", "2.25.57900", "2.25.57901", true));
    cut.resize(cut.size() - 60);
    std::vector<std::uint8_t> cut2 = dataset_bytes(make_image("2.25.57003", "2.25.57900", "2.25.57901", true));
    cut2.resize(cut2.size() - 30);
    const auto dir = temp_dir("quarantine");
    StoreOpen st(dir / "store");
    Events ev;
    siaara_scp_options o = scp_options("Q_SCP");
    // QUARANTINE is the default policy (owner decision): nothing is set here.
    // One calling AE is refused instead, another keeps a copy of what is refused (per-AE override).
    siaara_incomplete_override ov[2];
    std::memset(ov, 0, sizeof ov);
    ov[0].struct_size = ov[1].struct_size = sizeof(siaara_incomplete_override);
    ov[0].calling_ae = "STRICT_AE";
    ov[0].policy = SIAARA_INCOMPLETE_REFUSE;
    ov[1].calling_ae = "COPY_AE";
    ov[1].policy = SIAARA_INCOMPLETE_REFUSE_KEEP_COPY;
    o.incomplete_overrides = ov;
    o.incomplete_override_count = 2;
    siaara_scp_services sv = services();
    sv.storage = sv.find = sv.commitment = sv.get = 1;
    sv.commitment_report_mode = SIAARA_COMMIT_REPORT_SAME;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start_ex(&o, &sv, st.store, on_event, &ev, &scp) == SIAARA_OK);
    const auto port = static_cast<std::uint16_t>(port_of(scp));
    // Default caller: quarantined, answered with the warning status; not in the archive.
    CHECK_EQ(raw_store(port, "Q_SCP", kMR, "2.25.57002", cut), 0xB007);
    CHECK_EQ(raw_store(port, "Q_SCP", kMR, "2.25.57001", dataset_bytes(whole)), 0x0000);
    CHECK(st.instances() == 1);
    REQUIRE(ev.wait_for(SIAARA_SCP_QUARANTINED, 1));
    const auto qe = ev.of(SIAARA_SCP_QUARANTINED);
    CHECK_EQ(qe[0].status, 0xB007u);
    CHECK((qe[0].read_loss & SIAARA_READ_INPUT_TRUNCATED) != 0);
    auto items = quarantine_list(st.store);
    REQUIRE(items.size() == 1);
    CHECK(std::string(items[0].sop_instance_uid) == "2.25.57002");
    CHECK(std::string(items[0].calling_ae) == "S4_SCU");
    CHECK(std::string(items[0].source) == "C-STORE");
    CHECK(std::strlen(items[0].id) == 64);
    CHECK((items[0].read_loss & SIAARA_READ_INPUT_TRUNCATED) != 0);
    CHECK(items[0].time > 1700000000);
    // The same bytes again: one item (dedupe by SHA-256).
    CHECK_EQ(raw_store(port, "Q_SCP", kMR, "2.25.57002", cut), 0xB007);
    CHECK(quarantine_list(st.store).size() == 1);
    // The bytes are exactly what arrived (after the file meta the release would store).
    size_t need = 0;
    REQUIRE(siaara_store_quarantine_bytes(st.store, SIAARA_AREA_QUARANTINE, items[0].id, nullptr, 0, &need) == SIAARA_OK);
    CHECK(need == items[0].size);
    std::vector<uint8_t> got(need);
    REQUIRE(siaara_store_quarantine_bytes(st.store, SIAARA_AREA_QUARANTINE, items[0].id, got.data(), got.size(), &need) == SIAARA_OK);
    REQUIRE(got.size() > cut.size());
    CHECK(std::memcmp(got.data() + got.size() - cut.size(), cut.data(), cut.size()) == 0);
    // The quarantine area is owner-only.
#if !defined(_WIN32)
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir / "store" / "quarantine"))
        CHECK((std::filesystem::status(e.path()).permissions() &
               (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    // Per-AE overrides.
    {
        net::Scu scu(scu_cfg(port, "Q_SCP", "STRICT_AE"));
        scu.add_context(kMR, {std::string(ts::ExplicitVRLittleEndian)});
        REQUIRE(scu.connect().is_ok());
        net::Association* a = scu.association();
        const std::uint16_t mid = a->next_message_id();
        REQUIRE(net::send_dimse_raw(*a, a->find_context(kMR)->id, net::make_c_store_rq(mid, kMR, "2.25.57003"), cut2.data(),
                                    cut2.size())
                    .is_ok());
        auto r = net::receive_dimse(*a, 5000);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->message.command.status.value_or(0), 0xC000);  // refused for this AE
        (void)scu.release();
    }
    CHECK(quarantine_list(st.store).size() == 1);
    CHECK(quarantine_list(st.store, SIAARA_AREA_REJECTED).empty());
    // REFUSE_KEEP_COPY for another AE: the failure answer, and a copy in the "rejected" area.
    {
        net::Scu scu(scu_cfg(port, "Q_SCP", "COPY_AE"));
        scu.add_context(kMR, {std::string(ts::ExplicitVRLittleEndian)});
        REQUIRE(scu.connect().is_ok());
        net::Association* a = scu.association();
        const std::uint16_t mid = a->next_message_id();
        REQUIRE(net::send_dimse_raw(*a, a->find_context(kMR)->id, net::make_c_store_rq(mid, kMR, "2.25.57003"), cut2.data(),
                                    cut2.size())
                    .is_ok());
        auto r = net::receive_dimse(*a, 5000);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->message.command.status.value_or(0), 0xC000);
        (void)scu.release();
    }
    {
        const auto rejected = quarantine_list(st.store, SIAARA_AREA_REJECTED);
        REQUIRE(rejected.size() == 1);
        CHECK(std::string(rejected[0].calling_ae) == "COPY_AE");
        CHECK(rejected[0].area == SIAARA_AREA_REJECTED);
        // Export writes the bytes unchanged to a folder; the item stays.
        const std::string out_dir = (dir / "export").string();
        std::filesystem::create_directories(out_dir);
        REQUIRE(siaara_store_quarantine_export(st.store, SIAARA_AREA_REJECTED, rejected[0].id, out_dir.c_str()) == SIAARA_OK);
        const auto exported = std::filesystem::path(out_dir) / (std::string(rejected[0].id) + ".dcm");
        CHECK(std::filesystem::file_size(exported) == rejected[0].size);
        CHECK(quarantine_list(st.store, SIAARA_AREA_REJECTED).size() == 1);
        CHECK(siaara_store_quarantine_delete(st.store, SIAARA_AREA_REJECTED, rejected[0].id) == SIAARA_OK);
        CHECK(siaara_store_quarantine_count(st.store, 7, nullptr, nullptr) == SIAARA_ERR_INVALID_ARGUMENT);
    }
    CHECK(quarantine_list(st.store).size() == 1);
    CHECK(st.instances() == 1);
    // C-FIND does not return the quarantined instance; Storage Commitment does not commit it.
    siaara_dimse_peer p = peer(port, "Q_SCP");
    {
        const char* classes[] = {kMR, kMR};
        const char* instances[] = {"2.25.57001", "2.25.57002"};
        uint16_t reasons[2] = {0xFFFF, 0xFFFF};
        siaara_dimse_result r = result();
        CHECK(siaara_dimse_commit(&p, classes, instances, 2, 5000, nullptr, reasons, &r) == SIAARA_ERR_REJECTED);
        CHECK_EQ(reasons[0], 0);
        CHECK_EQ(reasons[1], 0x0112);  // no such object instance: quarantined is not committed
    }
    // Release: stored unchanged, flagged incomplete in the index, gone from the quarantine.
    int32_t outcome = -1;
    REQUIRE(siaara_store_quarantine_release(st.store, SIAARA_AREA_QUARANTINE, items[0].id, SIAARA_DUP_KEEP, &outcome, nullptr, 0, nullptr) == SIAARA_OK);
    CHECK(outcome == SIAARA_ADD_ADDED);
    CHECK(quarantine_list(st.store).empty());
    CHECK(st.instances() == 2);
    {
        bool flagged = false, same = false;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir / "store")) {
            if (!e.is_regular_file() || e.file_size() != got.size()) continue;
            auto b = io::read_file(e.path().string());
            same = same || (b && std::memcmp(b->data(), got.data(), got.size()) == 0);
        }
        CHECK(same);
        siaara_store_filter f;
        std::memset(&f, 0, sizeof f);
        f.struct_size = sizeof f;
        f.level = SIAARA_LEVEL_INSTANCE;
        f.sop_instance_uid = "2.25.57002";
        siaara_store_result* res = nullptr;
        if (siaara_store_query(st.store, &f, &res) == SIAARA_OK && res) {
            siaara_index_record rec;
            std::memset(&rec, 0, sizeof rec);
            rec.struct_size = sizeof rec;
            std::vector<char> text(8192);
            size_t text_need = 0;
            if (siaara_store_result_get(res, 0, &rec, text.data(), text.size(), &text_need) == SIAARA_OK)
                flagged = (rec.flags & SIAARA_IDXF_INCOMPLETE) != 0;
            siaara_store_result_free(res);
        }
        CHECK(flagged);
    }
    CHECK(siaara_store_quarantine_release(st.store, SIAARA_AREA_QUARANTINE, items[0].id, SIAARA_DUP_KEEP, nullptr, nullptr, 0, nullptr) == SIAARA_ERR_NOT_FOUND);
    // Delete.
    CHECK_EQ(raw_store(port, "Q_SCP", kMR, "2.25.57003", cut2), 0xB007);
    items = quarantine_list(st.store);
    REQUIRE(items.size() == 1);
    CHECK(siaara_store_quarantine_delete(st.store, SIAARA_AREA_QUARANTINE, items[0].id) == SIAARA_OK);
    CHECK(quarantine_list(st.store).empty());
    CHECK(siaara_store_quarantine_delete(st.store, SIAARA_AREA_QUARANTINE, items[0].id) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_store_quarantine_delete(st.store, SIAARA_AREA_QUARANTINE, "../../etc/passwd") == SIAARA_ERR_NOT_FOUND);
    siaara_scp_free(scp);
}

TEST_CASE("net2: sec4 R-116 quarantine caps, the FAILURE answer, and survival across a reopen") {
    std::vector<std::uint8_t> cut_a = dataset_bytes(make_image("2.25.58001", "2.25.58900", "2.25.58901", true));
    cut_a.resize(cut_a.size() - 60);
    std::vector<std::uint8_t> cut_b = dataset_bytes(make_image("2.25.58002", "2.25.58900", "2.25.58901", true));
    cut_b.resize(cut_b.size() - 60);
    const auto dir = temp_dir("quarantine_caps");
    const std::string root = (dir / "store").string();
    siaara_store_options so;
    std::memset(&so, 0, sizeof so);
    so.struct_size = sizeof so;
    so.create = 1;
    so.quarantine_max_items = 1;
    siaara_store* store = nullptr;
    REQUIRE(siaara_store_open(root.c_str(), root.size(), &so, &store) == SIAARA_OK);
    Events ev;
    siaara_scp_options o = scp_options("Q_SCP");
    o.incomplete_policy = SIAARA_INCOMPLETE_QUARANTINE;
    o.quarantine_response = SIAARA_QUARANTINE_ANSWER_FAILURE;
    siaara_scp* scp = nullptr;
    REQUIRE(siaara_scp_start(&o, store, on_event, &ev, &scp) == SIAARA_OK);
    const auto port = static_cast<std::uint16_t>(port_of(scp));
    CHECK_EQ(raw_store(port, "Q_SCP", kMR, "2.25.58001", cut_a), 0xC000);  // quarantined, answered as a failure
    CHECK(quarantine_list(store).size() == 1);
    CHECK_EQ(raw_store(port, "Q_SCP", kMR, "2.25.58002", cut_b), 0xA700);  // the item cap: refused, nothing kept
    CHECK(quarantine_list(store).size() == 1);
    REQUIRE(ev.wait_for(SIAARA_SCP_QUARANTINED, 1));
    REQUIRE(ev.wait_for(SIAARA_SCP_STORE_FAILED, 1));
    siaara_scp_free(scp);
    siaara_store_free(store);
    // The quarantine is on disk: a new handle lists the item.
    so.quarantine_max_items = 0;
    so.quarantine_max_bytes = 64;  // a byte cap smaller than any instance
    REQUIRE(siaara_store_open(root.c_str(), root.size(), &so, &store) == SIAARA_OK);
    const auto items = quarantine_list(store);
    REQUIRE(items.size() == 1);
    CHECK(std::string(items[0].sop_instance_uid) == "2.25.58001");
    siaara_scp_options o2 = scp_options("Q_SCP");
    o2.incomplete_policy = SIAARA_INCOMPLETE_QUARANTINE;
    REQUIRE(siaara_scp_start(&o2, store, nullptr, nullptr, &scp) == SIAARA_OK);
    CHECK_EQ(raw_store(static_cast<std::uint16_t>(port_of(scp)), "Q_SCP", kMR, "2.25.58002", cut_b), 0xA700);  // byte cap
    siaara_scp_free(scp);
    siaara_store_free(store);
    // Option "evict oldest": with room for one item the new one replaces the old one; SUCCESS answer.
    so.quarantine_max_bytes = 0;
    so.quarantine_max_items = 1;
    so.quarantine_when_full = SIAARA_AREA_FULL_EVICT_OLDEST;
    REQUIRE(siaara_store_open(root.c_str(), root.size(), &so, &store) == SIAARA_OK);
    siaara_scp_options o3 = scp_options("Q_SCP");
    o3.quarantine_response = SIAARA_QUARANTINE_ANSWER_SUCCESS;
    o3.incomplete_warning_status = 0xB006;
    REQUIRE(siaara_scp_start(&o3, store, nullptr, nullptr, &scp) == SIAARA_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // item times have one-second resolution
    CHECK_EQ(raw_store(static_cast<std::uint16_t>(port_of(scp)), "Q_SCP", kMR, "2.25.58002", cut_b), 0x0000);
    const auto after = quarantine_list(store);
    REQUIRE(after.size() == 1);
    CHECK(std::string(after[0].sop_instance_uid) == "2.25.58002");
    siaara_scp_free(scp);
    // The selectable warning status.
    siaara_scp_options o4 = scp_options("Q_SCP");
    o4.incomplete_warning_status = 0xB006;
    REQUIRE(siaara_scp_start(&o4, store, nullptr, nullptr, &scp) == SIAARA_OK);
    CHECK_EQ(raw_store(static_cast<std::uint16_t>(port_of(scp)), "Q_SCP", kMR, "2.25.58002", cut_b), 0xB006);
    o4.incomplete_warning_status = 0xC000;  // not a warning status
    siaara_scp* bad = nullptr;
    CHECK(siaara_scp_start(&o4, store, nullptr, nullptr, &bad) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_scp_free(scp);
    siaara_store_free(store);
}

// ---------------------------------------------------------------- R-111 in window lists
TEST_CASE("net2: sec4 window lists with very many values are read in one pass") {
    // 60,000 windows: the per-index getters made this quadratic (minutes); one pass is milliseconds.
    constexpr int kWindows = 60000;
    std::string centers, widths;
    for (int i = 0; i < kWindows; ++i) {
        centers += (i ? "\\" : "") + std::to_string(40 + i % 7);
        widths += (i ? "\\" : "") + std::to_string(300 + i % 11);
    }
    DicomFile f = make_image("2.25.59001", "2.25.59900", "2.25.59901", true);
    f.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);  // 32-bit lengths: the lists pass 64 KiB
    (void)f.dataset.set_string(0x00281050, VR::DS, centers);
    (void)f.dataset.set_string(0x00281051, VR::DS, widths);
    const auto bytes = part10(f);
    siaara_file* cf = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &cf) == SIAARA_OK);
    siaara_frame* frame = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(siaara_frame_open(cf, 0, &frame) == SIAARA_OK);
    uint32_t count = 0;
    CHECK(siaara_voi_count(frame, &count) == SIAARA_OK);
    CHECK_EQ(count, static_cast<uint32_t>(kWindows));
    CHECK(ms_since(t0) < 5000);
    siaara_frame_free(frame);
    siaara_file_free(cf);
}
