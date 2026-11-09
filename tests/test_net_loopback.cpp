// In-process SCP + SCU over 127.0.0.1 (ephemeral ports): every DIMSE-C service, rejection,
// fragmentation with tiny PDUs, timeouts, release collision and server shutdown.
// Optional oracle tests run only when SIAARA_NET_ORACLE_* environment variables are set
// (see tests/crosscheck/net_crosscheck.py).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/pdu.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/socket.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";

std::optional<std::string> env(const char* name) {
#if defined(_MSC_VER)
    char* v = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) return std::nullopt;
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

DicomFile make_instance(int i, std::string_view ts_uid, std::size_t pixel_bytes = 64) {
    DicomFile f;
    f.transfer_syntax = std::string(ts_uid);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kCT);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, "2.25.9000" + std::to_string(i));
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.8000");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.7000");
    (void)ds.set_string(tags::PatientName, VR::PN, "Loop^Back");
    (void)ds.set_string(tags::PatientID, VR::LO, "PID" + std::to_string(i % 2));
    (void)ds.set_string(tags::Modality, VR::CS, "CT");
    (void)ds.set_uint16(tags::Rows, 1);
    (void)ds.set_uint16(tags::Columns, static_cast<std::uint16_t>(pixel_bytes / 2));
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    std::vector<std::uint8_t> px(pixel_bytes);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>(k * 7 + static_cast<std::size_t>(i));
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    return f;
}

bool same_pixels(const Dataset& a, const Dataset& b) {
    const Element* x = a.find(tags::PixelData);
    const Element* y = b.find(tags::PixelData);
    return x && y && x->value.size() == y->value.size() &&
           std::equal(x->value.data(), x->value.data() + x->value.size(), y->value.data());
}

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_net_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

Timeouts fast_timeouts() {
    Timeouts t;
    t.connect_ms = 5000;
    t.acse_ms = 5000;
    t.dimse_ms = 5000;
    t.artim_ms = 2000;
    t.idle_ms = 10000;
    return t;
}

ScpConfig scp_config(const std::string& ae = "TEST_SCP") {
    ScpConfig c;
    c.ae_title = ae;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.max_associations = 4;
    c.timeouts = fast_timeouts();
    return c;
}

ScuConfig scu_config(std::uint16_t port, const std::string& called = "TEST_SCP", const std::string& calling = "TEST_SCU") {
    ScuConfig c;
    c.calling_ae = calling;
    c.called_ae = called;
    c.host = "127.0.0.1";
    c.port = port;
    c.timeouts = fast_timeouts();
    return c;
}

struct Store {
    std::mutex mu;
    std::vector<StoreRequest> got;
    std::uint16_t operator()(const StoreRequest& rq) {
        std::lock_guard<std::mutex> l(mu);
        got.push_back(rq);
        return dimse_status::Success;
    }
    std::size_t size() {
        std::lock_guard<std::mutex> l(mu);
        return got.size();
    }
};

}  // namespace

TEST_CASE("net/loopback: C-ECHO") {
    ScpServer scp(scp_config(), {});
    REQUIRE(scp.start().is_ok());
    CHECK(scp.port() != 0);
    Scu scu(scu_config(scp.port()));
    scu.add_verification();
    REQUIRE(scu.connect().is_ok());
    for (int i = 0; i < 3; ++i) {
        Result<std::uint16_t> r = scu.echo();
        REQUIRE(r.is_ok());
        CHECK_EQ(*r, 0);
    }
    CHECK(scu.association()->peer_max_pdu() == 65536);
    CHECK(scu.release().is_ok());
    scp.stop();
    CHECK(scp.associations_handled() == 1);
}

TEST_CASE("net/loopback: C-STORE several files and transfer syntaxes") {
    auto store = std::make_shared<Store>();
    ScpHandlers h;
    h.on_store = [store](const StoreRequest& rq) { return (*store)(rq); };
    ScpConfig cfg = scp_config();
    cfg.transfer_syntaxes = native_transfer_syntaxes();
    cfg.transfer_syntaxes.emplace_back(ts::DeflatedExplicitVRLittleEndian);
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());

    const std::filesystem::path dir = temp_dir("store");
    const std::string_view tss[] = {ts::ExplicitVRLittleEndian, ts::ImplicitVRLittleEndian, ts::ExplicitVRBigEndian,
                                    ts::DeflatedExplicitVRLittleEndian};
    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        const std::string p = (dir / ("f" + std::to_string(i) + ".dcm")).string();
        REQUIRE(write_file(make_instance(i, tss[static_cast<std::size_t>(i)], 200), p).is_ok());
        paths.push_back(p);
    }
    // One context per transfer syntax, so each file travels in its own syntax.
    Scu scu(scu_config(scp.port()));
    for (std::string_view t : tss) scu.add_context(kCT, {std::string(t)});
    REQUIRE(scu.connect().is_ok());
    for (const std::string& p : paths) {
        Result<std::uint16_t> r = scu.store_file(p);
        REQUIRE(r.is_ok());
        CHECK_EQ(*r, 0);
    }
    REQUIRE(scu.release().is_ok());

    // add_contexts_for_files proposes the file's syntax plus native fallbacks; the SCP's
    // preference (explicit VR LE first) then wins for every file.
    Scu scu2(scu_config(scp.port()));
    REQUIRE(scu2.add_contexts_for_files(paths).is_ok());
    REQUIRE(scu2.connect().is_ok());
    for (const std::string& p : paths) CHECK_EQ(scu2.store_file(p).value(), 0);
    REQUIRE(scu2.release().is_ok());
    scp.stop();
    REQUIRE(store->size() == 8u);
    for (std::size_t i = 4; i < 8; ++i) CHECK_EQ(store->got[i].transfer_syntax, std::string(ts::ExplicitVRLittleEndian));
    for (std::size_t i = 0; i < 4; ++i) {
        const StoreRequest& rq = store->got[i];
        CHECK_EQ(rq.calling_ae, "TEST_SCU");
        CHECK_EQ(rq.sop_class_uid, kCT);
        CHECK_EQ(rq.sop_instance_uid, "2.25.9000" + std::to_string(i));
        CHECK_EQ(rq.transfer_syntax, std::string(tss[i]));
        CHECK_EQ(rq.dataset.get_string(tags::PatientName).value_or(""), "Loop^Back");
        CHECK(same_pixels(rq.dataset, make_instance(static_cast<int>(i), tss[i], 200).dataset));
        // byte-exact Part 10 output re-reads to the same content
        std::vector<std::uint8_t> p10;
        REQUIRE(encode_received_part10(rq, p10).is_ok());
        Result<DicomFile> back = read_part10(Bytes::copy_of(p10.data(), p10.size()));
        REQUIRE(back.is_ok());
        CHECK_EQ(back->transfer_syntax, rq.transfer_syntax);
        CHECK(same_pixels(back->dataset, rq.dataset));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("net/loopback: C-FIND with pending results and C-CANCEL") {
    ScpHandlers h;
    std::atomic<int> emitted{0};
    h.on_find = [&](const QueryRequest& q, const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
        CHECK_EQ(q.identifier.get_string(0x00080052).value_or(""), "STUDY");
        for (int i = 0; i < 40; ++i) {
            Dataset r;
            (void)r.set_string(0x00080052, VR::CS, "STUDY");
            (void)r.set_string(tags::StudyInstanceUID, VR::UI, "2.25.1" + std::to_string(i));
            (void)r.set_string(tags::PatientName, VR::PN, "P^" + std::to_string(i));
            if (!emit(r)) break;
            emitted.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return dimse_status::Success;
    };
    ScpServer scp(scp_config(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_config(scp.port()));
    scu.add_context(std::string(sop::StudyRootFind), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, "STUDY");
    (void)q.set_string(tags::StudyInstanceUID, VR::UI, "");
    (void)q.set_string(tags::PatientName, VR::PN, "");
    std::vector<std::string> names;
    Result<FindResult> all = scu.find(sop::StudyRootFind, q, [&](const Dataset& d) {
        names.push_back(d.get_string(tags::PatientName).value_or(""));
        return true;
    });
    REQUIRE(all.is_ok());
    CHECK_EQ(all->status, 0);
    CHECK_EQ(all->matches, 40u);
    CHECK_EQ(names.size(), 40u);
    CHECK_EQ(names[39], "P^39");

    emitted = 0;
    int seen = 0;
    Result<FindResult> cut = scu.find(sop::StudyRootFind, q, [&](const Dataset&) { return ++seen < 3; });
    REQUIRE(cut.is_ok());
    CHECK(cut->cancel_sent);
    CHECK_EQ(cut->status, dimse_status::Cancel);
    CHECK(emitted.load() < 40);
    CHECK_EQ(seen, 3);
    // association still usable after the cancel
    REQUIRE(scu.find(sop::StudyRootFind, q, {}).is_ok());
    CHECK(scu.release().is_ok());
}

TEST_CASE("net/loopback: C-GET with C-STORE sub-operations and role selection") {
    std::vector<std::shared_ptr<const DicomFile>> files;
    for (int i = 0; i < 5; ++i)
        files.push_back(std::make_shared<const DicomFile>(make_instance(i, i % 2 ? ts::ImplicitVRLittleEndian
                                                                                  : ts::ExplicitVRLittleEndian)));
    ScpHandlers h;
    h.on_get = [&](const QueryRequest& q, std::vector<RetrieveItem>& items) -> std::uint16_t {
        CHECK_EQ(q.identifier.get_string(tags::StudyInstanceUID).value_or(""), "2.25.8000");
        for (const auto& f : files) items.push_back({"", f});
        return dimse_status::Success;
    };
    ScpServer scp(scp_config(), h);
    REQUIRE(scp.start().is_ok());
    Scu scu(scu_config(scp.port()));
    scu.add_context(std::string(sop::StudyRootGet), native_transfer_syntaxes());
    scu.add_get_storage(kCT);
    REQUIRE(scu.connect().is_ok());
    const NegotiatedContext* spc = scu.association()->find_context(kCT);
    REQUIRE(spc != nullptr);
    CHECK(spc->requestor_scp);
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, "STUDY");
    (void)q.set_string(tags::StudyInstanceUID, VR::UI, "2.25.8000");
    std::vector<std::string> got;
    std::vector<RetrieveProgress> prog;
    Result<RetrieveResult> r = scu.get(
        sop::StudyRootGet, q,
        [&](const StoreRequest& rq) {
            got.push_back(rq.sop_instance_uid);
            CHECK(same_pixels(rq.dataset, files[got.size() - 1]->dataset));
            return got.size() == 3 ? dimse_status::OutOfResources : dimse_status::Success;  // one failure
        },
        [&](const RetrieveProgress& p) {
            prog.push_back(p);
            return true;
        });
    REQUIRE(r.is_ok());
    CHECK_EQ(got.size(), 5u);
    CHECK_EQ(r->completed, 4);
    CHECK_EQ(r->failed, 1);
    CHECK_EQ(r->status, dimse_status::SubOperationsWarning);
    CHECK_EQ(prog.size(), 4u);
    if (!prog.empty()) CHECK_EQ(prog.back().remaining, 1);

    // cancel after the first sub-operation
    got.clear();
    Result<RetrieveResult> c = scu.get(
        sop::StudyRootGet, q, [&](const StoreRequest& rq) { got.push_back(rq.sop_instance_uid); return dimse_status::Success; },
        [&](const RetrieveProgress&) { return false; });
    REQUIRE(c.is_ok());
    CHECK_EQ(c->status, dimse_status::Cancel);
    CHECK(got.size() < 5u);
    CHECK(c->remaining > 0);
    CHECK(scu.release().is_ok());
}

TEST_CASE("net/loopback: C-MOVE to a second in-process SCP") {
    auto store = std::make_shared<Store>();
    ScpHandlers dh;
    dh.on_store = [store](const StoreRequest& rq) { return (*store)(rq); };
    ScpServer dest(scp_config("DEST_SCP"), dh);
    REQUIRE(dest.start().is_ok());

    const std::filesystem::path dir = temp_dir("move");
    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        const std::string p = (dir / ("m" + std::to_string(i) + ".dcm")).string();
        REQUIRE(write_file(make_instance(i, ts::ExplicitVRLittleEndian, 128), p).is_ok());
        paths.push_back(p);
    }
    ScpHandlers h;
    h.on_move = [&](const QueryRequest&, std::vector<RetrieveItem>& items) -> std::uint16_t {
        for (const std::string& p : paths) items.push_back({p, nullptr});
        return dimse_status::Success;
    };
    const std::uint16_t dport = dest.port();
    h.resolve_move_destination = [dport](const std::string& ae) -> std::optional<MoveDestination> {
        if (ae != "DEST_SCP") return std::nullopt;
        return MoveDestination{"127.0.0.1", dport, ""};
    };
    ScpServer src(scp_config("SRC_SCP"), h);
    REQUIRE(src.start().is_ok());

    Scu scu(scu_config(src.port(), "SRC_SCP", "MOVER"));
    scu.add_context(std::string(sop::PatientRootMove), native_transfer_syntaxes());
    REQUIRE(scu.connect().is_ok());
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, "PATIENT");
    (void)q.set_string(tags::PatientID, VR::LO, "PID0");
    int pending = 0;
    Result<RetrieveResult> r = scu.move(sop::PatientRootMove, "DEST_SCP", q, [&](const RetrieveProgress&) {
        ++pending;
        return true;
    });
    REQUIRE(r.is_ok());
    CHECK_EQ(r->status, 0);
    CHECK_EQ(r->completed, 4);
    CHECK_EQ(r->failed, 0);
    CHECK_EQ(pending, 3);
    REQUIRE(store->size() == 4u);
    CHECK_EQ(store->got[0].move_originator_ae, "MOVER");
    CHECK_EQ(store->got[0].calling_ae, "SRC_SCP");
    CHECK(store->got[0].move_originator_message_id.has_value());

    Result<RetrieveResult> unknown = scu.move(sop::PatientRootMove, "NOWHERE", q);
    REQUIRE(unknown.is_ok());
    CHECK_EQ(unknown->status, dimse_status::MoveDestinationUnknown);
    CHECK(scu.release().is_ok());
    src.stop();
    dest.stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("net/loopback: association rejection") {
    ScpConfig cfg = scp_config();
    cfg.allowed_calling_aes = {"GOOD_SCU"};
    ScpServer scp(cfg, {});
    REQUIRE(scp.start().is_ok());
    {
        Scu bad(scu_config(scp.port(), "TEST_SCP", "BAD_SCU"));
        AssociateRj rj;
        Status s = bad.connect(&rj);
        CHECK(s.code == Errc::Rejected);
        CHECK_EQ(rj.result, rj::Permanent);
        CHECK_EQ(rj.source, rj::SourceUser);
        CHECK_EQ(rj.reason, rj::UserCallingAeNotRecognized);
    }
    {
        Scu bad(scu_config(scp.port(), "OTHER_SCP", "GOOD_SCU"));
        AssociateRj rj;
        CHECK(bad.connect(&rj).code == Errc::Rejected);
        CHECK_EQ(rj.reason, rj::UserCalledAeNotRecognized);
    }
    {
        Scu good(scu_config(scp.port(), "TEST_SCP", "GOOD_SCU"));
        REQUIRE(good.connect().is_ok());
        CHECK(good.echo().is_ok());
        // a SOP class the SCP does not support is rejected per context, association stays up
        CHECK(!good.store(make_instance(1, ts::ExplicitVRLittleEndian)).is_ok());
        CHECK(good.release().is_ok());
    }
    CHECK(scp.associations_rejected() == 2);
}

TEST_CASE("net/loopback: max concurrent associations") {
    ScpConfig cfg = scp_config();
    cfg.max_associations = 1;
    ScpServer scp(cfg, {});
    REQUIRE(scp.start().is_ok());
    Scu first(scu_config(scp.port()));
    REQUIRE(first.connect().is_ok());
    Scu second(scu_config(scp.port()));
    AssociateRj rj;
    CHECK(second.connect(&rj).code == Errc::Rejected);
    CHECK_EQ(rj.result, rj::Transient);
    CHECK_EQ(rj.source, rj::SourcePresentationProvider);
    CHECK_EQ(rj.reason, rj::PresentationLocalLimitExceeded);
    CHECK(first.release().is_ok());
    // once the first association is gone a new one is accepted
    bool ok = false;
    for (int i = 0; i < 50 && !ok; ++i) {
        Scu again(scu_config(scp.port()));
        ok = again.connect().is_ok() && again.echo().is_ok() && again.release().is_ok();
        if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(ok);
}

TEST_CASE("net/loopback: fragmentation with tiny PDUs") {
    auto store = std::make_shared<Store>();
    ScpHandlers h;
    h.on_store = [store](const StoreRequest& rq) { return (*store)(rq); };
    ScpConfig cfg = scp_config();
    cfg.limits.max_receive_pdu = 32;  // SCU must send many small PDVs
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    ScuConfig sc = scu_config(scp.port());
    sc.limits.max_receive_pdu = 20;  // and the SCP must fragment its responses
    Scu scu(sc);
    scu.add_context(kCT, {std::string(ts::ExplicitVRLittleEndian)});
    REQUIRE(scu.connect().is_ok());
    CHECK_EQ(scu.association()->peer_max_pdu(), 32u);
    const DicomFile f = make_instance(3, ts::ExplicitVRLittleEndian, 3000);
    Result<std::uint16_t> r = scu.store(f);
    REQUIRE(r.is_ok());
    CHECK_EQ(*r, 0);
    CHECK(scu.echo().is_ok() == false);  // Verification not proposed
    CHECK(scu.release().is_ok());
    REQUIRE(store->size() == 1u);
    CHECK(same_pixels(store->got[0].dataset, f.dataset));
}

TEST_CASE("net/loopback: sender respects the peer's maximum PDU length") {
    // A hand-written acceptor advertising max length 40 checks every P-DATA-TF it receives.
    TcpListener l;
    REQUIRE(l.listen("127.0.0.1", 0).is_ok());
    std::atomic<bool> ok{false};
    std::atomic<std::size_t> pdus{0};
    std::thread acceptor([&] {
        Result<std::unique_ptr<Transport>> t = l.accept(5000);
        if (!t) return;
        Transport& tr = **t;
        Result<Pdu> rq = read_pdu(tr, 5000);
        if (!rq || !std::holds_alternative<AssociateRq>(*rq)) return;
        const AssociateRq& r = std::get<AssociateRq>(*rq);
        AssociateAc ac;
        ac.called_ae = r.called_ae;
        ac.calling_ae = r.calling_ae;
        ac.contexts.push_back({r.contexts[0].id, pc_result::Acceptance, r.contexts[0].transfer_syntaxes[0]});
        ac.user.max_length = 40;
        ac.user.implementation_class_uid = "1.2.3";
        std::vector<std::uint8_t> b;
        if (!encode_pdu(ac, b) || !tr.write_all(b.data(), b.size(), 5000)) return;
        bool good = true;
        while (true) {
            std::uint8_t hdr[6];
            if (!read_exact(tr, hdr, 6, 5000)) break;
            const std::uint32_t len = load_be32(hdr + 2);
            std::vector<std::uint8_t> body(len);
            if (len && !read_exact(tr, body.data(), len, 5000)) break;
            if (hdr[0] == 0x04) {
                ++pdus;
                good = good && len <= 40;
            }
            if (hdr[0] == 0x05) {
                std::vector<std::uint8_t> rp;
                (void)encode_pdu(ReleaseRp{}, rp);
                (void)tr.write_all(rp.data(), rp.size(), 5000);
                break;
            }
        }
        ok = good;
    });
    ScuConfig sc = scu_config(l.port(), "X", "Y");
    sc.timeouts.dimse_ms = 300;
    Scu scu(sc);
    scu.add_context(kCT, {std::string(ts::ExplicitVRLittleEndian)});
    REQUIRE(scu.connect().is_ok());
    Result<std::uint16_t> r = scu.store(make_instance(1, ts::ExplicitVRLittleEndian, 500));
    CHECK(!r.is_ok());  // no response from the fake peer
    CHECK(r.error().code == Errc::Timeout);
    scu.abort();
    acceptor.join();
    CHECK(ok.load());
    CHECK(pdus.load() > 15);
}

TEST_CASE("net/loopback: timeouts") {
    // DIMSE timeout at the SCU: the echo handler stalls.
    ScpHandlers h;
    h.on_echo = [](const std::string&) -> std::uint16_t {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        return 0;
    };
    ScpConfig cfg = scp_config();
    cfg.timeouts.idle_ms = 300;
    cfg.timeouts.artim_ms = 300;
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    {
        ScuConfig sc = scu_config(scp.port());
        sc.timeouts.dimse_ms = 150;
        sc.timeouts.artim_ms = 300;
        Scu scu(sc);
        REQUIRE(scu.connect().is_ok());
        const auto t0 = std::chrono::steady_clock::now();
        Result<std::uint16_t> r = scu.echo();
        REQUIRE(!r.is_ok());
        CHECK(r.error().code == Errc::Timeout);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
        CHECK(!scu.connected());
    }
    {
        // SCP idle timeout: the association is aborted when the SCU stays silent.
        Scu scu(scu_config(scp.port()));
        REQUIRE(scu.connect().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        Result<std::uint16_t> r = scu.echo();
        CHECK(!r.is_ok());
    }
    {
        // ARTIM: a TCP connection that never sends A-ASSOCIATE-RQ is closed by the SCP.
        Result<std::unique_ptr<Transport>> t = tcp_connect("127.0.0.1", scp.port(), 2000);
        REQUIRE(t.is_ok());
        std::uint8_t b[8];
        const auto t0 = std::chrono::steady_clock::now();
        Result<std::size_t> n = (*t)->read_some(b, sizeof b, 3000);
        CHECK((n.is_ok() && *n == 0) || (!n.is_ok() && n.error().code == Errc::NetworkError));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2500));
    }
    {
        // connection refused is reported, not hung
        TcpListener tmp;
        REQUIRE(tmp.listen("127.0.0.1", 0).is_ok());
        const std::uint16_t dead = tmp.port();
        tmp.close();
        Scu scu(scu_config(dead));
        CHECK(!scu.connect().is_ok());
    }
}

TEST_CASE("net/loopback: release collision") {
    TcpListener l;
    REQUIRE(l.listen("127.0.0.1", 0).is_ok());
    std::atomic<bool> acceptor_ok{false};
    std::atomic<int> ready{0};
    Timeouts to = fast_timeouts();
    std::thread acceptor([&] {
        Result<std::unique_ptr<Transport>> t = l.accept(5000);
        if (!t) return;
        Result<std::unique_ptr<Association>> a = Association::receive_request(std::move(t.value()), to, {});
        if (!a) return;
        AssociateAc ac;
        std::vector<NegotiatedContext> ctx =
            negotiate_contexts((*a)->request_pdu(), {{std::string(sop::Verification), native_transfer_syntaxes()}}, nullptr, ac);
        if (!(*a)->accept(ac, ctx)) return;
        ready.fetch_add(1);
        while (ready.load() < 2) std::this_thread::yield();
        acceptor_ok = (*a)->release().is_ok() && (*a)->state() == UlState::Sta1;
    });
    AssociateRq rq;
    rq.called_ae = "A";
    rq.calling_ae = "B";
    REQUIRE(build_proposals({{std::string(sop::Verification), native_transfer_syntaxes(), std::nullopt}}, rq).is_ok());
    Result<std::unique_ptr<Association>> a = Association::request("127.0.0.1", l.port(), rq, to, {});
    REQUIRE(a.is_ok());
    while (ready.load() < 1) std::this_thread::yield();
    ready.fetch_add(1);
    // Both sides send A-RELEASE-RQ before reading anything: a guaranteed collision (Sta7 -> Sta9/Sta10).
    Status s = (*a)->release();
    acceptor.join();
    CHECK(s.is_ok());
    CHECK((*a)->state() == UlState::Sta1);
    CHECK(acceptor_ok.load());
}

TEST_CASE("net/loopback: generic N-service, abort and stop") {
    ScpConfig cfg = scp_config();
    cfg.accepted = {{std::string(sop::StorageCommitmentPushModel), native_transfer_syntaxes(), true, false}};
    ScpHandlers h;
    h.on_n_request = [](const Message& m, const Dataset& in, Dataset& reply, bool& has_reply) -> std::uint16_t {
        if (m.command.command_field != cmd::NActionRq || m.command.action_type_id != 1) return dimse_status::ProcessingFailure;
        (void)reply.set_string(0x00081195, VR::UI, in.get_string(0x00081195).value_or(""));  // Transaction UID
        has_reply = true;
        return dimse_status::Success;
    };
    ScpServer scp(cfg, h);
    REQUIRE(scp.start().is_ok());
    {
        Scu scu(scu_config(scp.port()));
        scu.add_context(std::string(sop::StorageCommitmentPushModel), native_transfer_syntaxes());
        REQUIRE(scu.connect().is_ok());
        Dataset ds;
        (void)ds.set_string(0x00081195, VR::UI, "2.25.42");
        Result<Scu::NResponse> r =
            scu.n_request(make_n_action_rq(0, sop::StorageCommitmentPushModel, "1.2.840.10008.1.20.1.1", 1, true), &ds);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->command.status.value_or(0xFFFF), 0);
        CHECK_EQ(r->command.action_type_id.value_or(0), 1);
        CHECK_EQ(r->command.affected_sop_instance_uid, "1.2.840.10008.1.20.1.1");
        CHECK_EQ(r->dataset.get_string(0x00081195).value_or(""), "2.25.42");
        scu.abort();  // the server survives a peer abort
    }
    Scu idle(scu_config(scp.port()));
    REQUIRE(idle.connect().is_ok());
    REQUIRE(idle.echo().is_ok());
    const auto t0 = std::chrono::steady_clock::now();
    scp.stop();  // aborts the idle association and joins the pool
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    CHECK(!scp.running());
    CHECK(!idle.echo().is_ok());
}

// ---------------------------------------------------------------- oracle (external peer) tests

TEST_CASE("net/oracle: C-STORE and C-ECHO to an external SCP") {
    const auto port = env("SIAARA_NET_ORACLE_STORE_PORT");
    if (!port) return;
    const std::string ae = env("SIAARA_NET_ORACLE_STORE_AE").value_or("STORESCP");
    Scu scu(scu_config(static_cast<std::uint16_t>(std::atoi(port->c_str())), ae, "SIAARA_XCHK"));
    scu.add_verification();
    for (std::string_view t : {ts::ExplicitVRLittleEndian, ts::ImplicitVRLittleEndian, ts::ExplicitVRBigEndian})
        scu.add_context(kCT, {std::string(t)});
    REQUIRE(scu.connect().is_ok());
    CHECK_EQ(scu.echo().value(), 0);
    int i = 100;
    for (std::string_view t : {ts::ExplicitVRLittleEndian, ts::ImplicitVRLittleEndian, ts::ExplicitVRBigEndian}) {
        Result<std::uint16_t> r = scu.store(make_instance(i++, t, 4096));
        REQUIRE(r.is_ok());
        CHECK_EQ(*r, 0);
    }
    CHECK(scu.release().is_ok());
}

TEST_CASE("net/oracle: C-FIND / C-GET / C-MOVE against an external Q/R SCP") {
    const auto port = env("SIAARA_NET_ORACLE_QR_PORT");
    if (!port) return;
    const std::string ae = env("SIAARA_NET_ORACLE_QR_AE").value_or("QRSCP");
    const std::string move_dest = env("SIAARA_NET_ORACLE_MOVE_DEST").value_or("");
    const std::size_t expect = static_cast<std::size_t>(std::atoi(env("SIAARA_NET_ORACLE_QR_COUNT").value_or("0").c_str()));
    const auto p = static_cast<std::uint16_t>(std::atoi(port->c_str()));
    Dataset q;
    (void)q.set_string(0x00080052, VR::CS, "IMAGE");
    (void)q.set_string(tags::PatientID, VR::LO, "");
    (void)q.set_string(tags::StudyInstanceUID, VR::UI, "2.25.8000");
    (void)q.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.7000");
    (void)q.set_string(tags::SOPInstanceUID, VR::UI, "");
    Dataset sq;  // study-level identifier for retrieves
    (void)sq.set_string(0x00080052, VR::CS, "STUDY");
    (void)sq.set_string(tags::StudyInstanceUID, VR::UI, "2.25.8000");
    {
        Scu scu(scu_config(p, ae, "SIAARA_XCHK"));
        scu.add_context(std::string(sop::StudyRootFind), native_transfer_syntaxes());
        REQUIRE(scu.connect().is_ok());
        std::size_t n = 0;
        Result<FindResult> r = scu.find(sop::StudyRootFind, q, [&](const Dataset& d) {
            if (d.contains(tags::SOPInstanceUID)) ++n;
            return true;
        });
        REQUIRE(r.is_ok());
        CHECK_EQ(r->status, 0);
        CHECK_EQ(n, expect);
        std::printf("  oracle C-FIND: %zu matches, status 0x%04X\n", n, r->status);
        CHECK(scu.release().is_ok());
    }
    {
        Scu scu(scu_config(p, ae, "SIAARA_XCHK"));
        scu.add_context(std::string(sop::StudyRootGet), native_transfer_syntaxes());
        scu.add_get_storage(kCT);
        REQUIRE(scu.connect().is_ok());
        std::size_t n = 0;
        Result<RetrieveResult> r = scu.get(sop::StudyRootGet, sq, [&](const StoreRequest&) {
            ++n;
            return dimse_status::Success;
        });
        REQUIRE(r.is_ok());
        CHECK_EQ(r->status, 0);
        CHECK_EQ(n, expect);
        std::printf("  oracle C-GET: %zu instances, completed %u, status 0x%04X\n", n, r->completed, r->status);
        CHECK(scu.release().is_ok());
    }
    if (!move_dest.empty()) {
        Scu scu(scu_config(p, ae, "SIAARA_XCHK"));
        scu.add_context(std::string(sop::StudyRootMove), native_transfer_syntaxes());
        REQUIRE(scu.connect().is_ok());
        Result<RetrieveResult> r = scu.move(sop::StudyRootMove, move_dest, sq);
        REQUIRE(r.is_ok());
        CHECK_EQ(r->status, 0);
        CHECK_EQ(static_cast<std::size_t>(r->completed), expect);
        std::printf("  oracle C-MOVE: completed %u, failed %u, status 0x%04X\n", r->completed, r->failed, r->status);
        CHECK(scu.release().is_ok());
    }
}
