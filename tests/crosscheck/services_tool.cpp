// Test-only helper for tests/crosscheck/services_crosscheck.py (not installed). Runs SIAARA's side
// of the interoperability checks against DCMTK and pynetdicom: SCPs that write their port to a
// file and run until killed, one-shot SCUs, synthetic images and frame dumps.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "codec/transcode.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "image/color.hpp"
#include "image/frames.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int fail(const std::string& m) {
    std::fprintf(stderr, "services_tool: %s\n", m.c_str());
    return 1;
}

void write_port(const std::string& file, std::uint16_t port) {
    std::ofstream(file) << port << "\n";
}

void run_until_stopped(ScpServer& s) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    s.stop();
}

ScpConfig scp_config(const std::string& ae) {
    ScpConfig c;
    c.ae_title = ae;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.require_called_ae = false;
    c.allow_any_calling_ae = true;
    return c;
}

ScpHandlers with_services(std::shared_ptr<ServiceSet> svc, bool verbose = false) {
    ScpHandlers h;
    h.services = std::move(svc);
    if (verbose) h.log = [](const std::string& m) { std::fprintf(stderr, "scp: %s\n", m.c_str()); };
    return h;
}

ScuConfig scu_config(const std::string& host, const char* port, const std::string& called, const std::string& calling) {
    ScuConfig c;
    c.host = host;
    c.port = static_cast<std::uint16_t>(std::strtoul(port, nullptr, 10));
    c.called_ae = called;
    c.calling_ae = calling;
    c.timeouts.dimse_ms = 20000;
    return c;
}

// "AE=host:port"
std::optional<std::pair<std::string, MoveDestination>> parse_peer(const std::string& s) {
    const std::size_t eq = s.find('='), colon = s.rfind(':');
    if (eq == std::string::npos || colon == std::string::npos || colon < eq) return std::nullopt;
    MoveDestination d;
    d.host = s.substr(eq + 1, colon - eq - 1);
    d.port = static_cast<std::uint16_t>(std::strtoul(s.c_str() + colon + 1, nullptr, 10));
    return std::make_pair(s.substr(0, eq), d);
}

std::function<std::optional<MoveDestination>(const std::string&)> resolver(std::map<std::string, MoveDestination> m) {
    return [m](const std::string& ae) -> std::optional<MoveDestination> {
        auto it = m.find(ae);
        if (it == m.end()) return std::nullopt;
        return it->second;
    };
}

// ---------------------------------------------------------------- images

int make_image(int argc, char** argv) {
    if (argc < 6) return fail("make-image OUT gray8|gray12|gray16|rgb8 ROWS COLS [FRAMES] [UID]");
    const std::string kind = argv[2];
    const auto rows = static_cast<std::uint16_t>(std::atoi(argv[3]));
    const auto cols = static_cast<std::uint16_t>(std::atoi(argv[4]));
    const auto frames = argc > 5 ? static_cast<std::uint32_t>(std::atoi(argv[5])) : 1u;
    const std::string uid = argc > 6 ? argv[6] : "2.25.123456789";
    const bool rgb = kind == "rgb8";
    const unsigned ba = (kind == "gray8" || rgb) ? 8 : 16;
    const unsigned bs = kind == "gray12" ? 12 : ba;
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.7");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, uid);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, "2.25.1001");
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.1002");
    (void)ds.set_string(tags::PatientID, VR::LO, "XCHECK");
    (void)ds.set_string(tags::PatientName, VR::PN, "Cross^Check");
    (void)ds.set_string(tags::Modality, VR::CS, "OT");
    (void)ds.set_uint16(tags::SamplesPerPixel, rgb ? 3 : 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, rgb ? "RGB" : "MONOCHROME2");
    if (rgb) (void)ds.set_uint16(tags::PlanarConfiguration, 0);
    if (frames > 1) (void)ds.set_string(tags::NumberOfFrames, VR::IS, std::to_string(frames));
    (void)ds.set_uint16(tags::Rows, rows);
    (void)ds.set_uint16(tags::Columns, cols);
    (void)ds.set_uint16(tags::BitsAllocated, static_cast<std::uint16_t>(ba));
    (void)ds.set_uint16(tags::BitsStored, static_cast<std::uint16_t>(bs));
    (void)ds.set_uint16(tags::HighBit, static_cast<std::uint16_t>(bs - 1));
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    const std::int64_t maxv = (std::int64_t{1} << bs) - 1;
    std::uint32_t seed = 99;
    std::vector<std::uint8_t> px;
    const unsigned spp = rgb ? 3 : 1;
    for (std::uint32_t fr = 0; fr < frames; ++fr)
        for (std::uint32_t y = 0; y < rows; ++y)
            for (std::uint32_t x = 0; x < cols; ++x)
                for (unsigned s = 0; s < spp; ++s) {
                    seed = seed * 1103515245u + 12345u;
                    std::int64_t v = static_cast<std::int64_t>(x) * (maxv / 2) / cols +
                                     static_cast<std::int64_t>((y + 5 * s) % rows) * (maxv / 4) / rows +
                                     static_cast<std::int64_t>(fr) * (maxv / 16) +
                                     (static_cast<std::int64_t>((seed >> 16) % 7) - 3) * (maxv / 255 + 1) + maxv / 8;
                    v = std::max<std::int64_t>(0, std::min(v, maxv));
                    px.push_back(static_cast<std::uint8_t>(v));
                    if (ba == 16) px.push_back(static_cast<std::uint8_t>(v >> 8));
                }
    if (px.size() & 1u) px.push_back(0);
    (void)ds.set_bytes(tags::PixelData, ba == 8 ? VR::OB : VR::OW, Bytes::copy_of(px.data(), px.size()));
    Status s = write_file(f, argv[1]);
    return s ? 0 : fail(s.message);
}

// Decoded frames, interleaved, colour as RGB: the comparison format of the crosscheck script.
int frames(int argc, char** argv) {
    if (argc < 3) return fail("frames FILE OUT");
    Result<DicomFile> f = read_file(argv[1]);
    if (!f) return fail(f.error().message);
    Result<image::FrameReader> fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
    if (!fr) return fail(fr.error().message);
    std::ofstream out(argv[2], std::ios::binary);
    const image::PixelInfo& pi = fr->info();
    for (std::uint32_t i = 0; i < fr->frame_count(); ++i) {
        Result<std::vector<std::uint8_t>> d = fr->decode(i);
        if (!d) return fail(d.error().message);
        std::vector<std::uint8_t> v = std::move(d.value());
        if (pi.samples_per_pixel == 3) {
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image::rgb_frame_bytes(pi)));
            image::ColorOptions co;
            co.codec_applied_mct = fr->codec_applies_color_transform();
            co.native_layout = !fr->encapsulated();
            if (Status s = image::to_rgb(pi, v, rgb, co); !s) return fail(s.message);
            v = std::move(rgb);
        }
        out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
    }
    std::printf("%s %s %s\n", f->transfer_syntax.c_str(), pi.photometric_text.c_str(),
                f->dataset.get_string(0x00282110).value_or("-").c_str());
    return 0;
}

int transcode(int argc, char** argv) {
    if (argc < 4) return fail("transcode IN OUT TS [--lossy] [--allow-lossy]");
    Result<DicomFile> f = read_file(argv[1]);
    if (!f) return fail(f.error().message);
    codec::TranscodeParams p;
    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--lossy")) p.lossy = true;
    }
    if (Status s = codec::transcode_file(*f, argv[3], p); !s) return fail(s.message);
    Status s = write_file(*f, argv[2]);
    return s ? 0 : fail(s.message);
}

// ---------------------------------------------------------------- SCPs

int storescp(int argc, char** argv) {
    if (argc < 3) return fail("storescp PORTFILE OUTDIR [--preferred TS] [--spool BYTES] [--accept TS]...");
    ScpConfig cfg = scp_config("SIAARA_SCP");
    cfg.accept_any_sop_class = true;
    std::vector<std::string> accept;
    for (int i = 3; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--preferred")) cfg.preferred_store_ts = argv[++i];
        else if (!std::strcmp(argv[i], "--accept")) accept.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--spool")) {
            cfg.spool = SpoolConfig{};
            cfg.spool->threshold = std::strtoull(argv[++i], nullptr, 10);
            cfg.spool->directory = argv[2];
        }
    }
    if (!accept.empty()) cfg.transfer_syntaxes = accept;
    else {
        cfg.transfer_syntaxes = native_transfer_syntaxes();
        for (const char* t : {"1.2.840.10008.1.2.4.70", "1.2.840.10008.1.2.4.57", "1.2.840.10008.1.2.4.80",
                              "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.91",
                              "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.5",
                              "1.2.840.10008.1.2.1.99"})
            cfg.transfer_syntaxes.emplace_back(t);
    }
    const std::string out = argv[2];
    ScpHandlers h;
    h.on_store = [out](const StoreRequest& rq) -> std::uint16_t {
        const std::string path = out + "/" + rq.sop_instance_uid + ".dcm";
        if (rq.spool) return rq.spool->commit(path) ? dimse_status::Success : dimse_status::OutOfResources;
        std::vector<std::uint8_t> p10;
        if (!encode_received_part10(rq, p10)) return dimse_status::CannotUnderstand;
        return io::write_file_atomic(path, p10.data(), p10.size()) ? dimse_status::Success : dimse_status::OutOfResources;
    };
    ScpServer s(cfg, h);
    if (Status st = s.start(); !st) return fail(st.message);
    write_port(argv[1], s.port());
    run_until_stopped(s);
    return 0;
}

Dataset worklist_item(const char* name, const char* id, const char* station, const char* modality, const char* date) {
    Dataset sps;
    (void)sps.set_string(0x00400001, VR::AE, station);
    (void)sps.set_string(0x00400002, VR::DA, date);
    (void)sps.set_string(0x00400003, VR::TM, "093000");
    (void)sps.set_string(tags::Modality, VR::CS, modality);
    (void)sps.set_string(0x00400009, VR::SH, std::string("SPS-") + id);
    (void)sps.set_string(0x00400007, VR::LO, "Scheduled step");
    Dataset d;
    (void)d.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
    (void)d.set_string(tags::PatientName, VR::PN, name);
    (void)d.set_string(tags::PatientID, VR::LO, id);
    (void)d.set_string(0x00080050, VR::SH, std::string("ACC-") + id);
    (void)d.set_string(tags::StudyInstanceUID, VR::UI, std::string("2.25.77") + (id + 1));
    Element e;
    e.tag = 0x00400100;
    e.vr = VR::SQ;
    e.items.push_back(sps);
    d.put(std::move(e));
    return d;
}

int mwlscp(int argc, char** argv) {
    if (argc < 2) return fail("mwlscp PORTFILE");
    static const std::vector<Dataset> db = {worklist_item("DOE^JOHN", "P1", "CT1", "CT", "20261109"),
                                            worklist_item("ROE^JANE", "P2", "CT1", "CT", "20261110"),
                                            worklist_item("POE^ANN", "P3", "MR1", "MR", "20261109")};
    ScpHandlers h;
    h.on_find = [](const QueryRequest& q, const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
        if (q.sop_class_uid != sop::ModalityWorklistFind) return dimse_status::SopClassNotSupported;
        for (const Dataset& d : db)
            if (query_matches(q.identifier, d) && !emit(query_response(q.identifier, d))) break;
        return dimse_status::Success;
    };
    ScpServer s(scp_config("SIAARA_MWL"), h);
    if (Status st = s.start(); !st) return fail(st.message);
    write_port(argv[1], s.port());
    run_until_stopped(s);
    return 0;
}

// commit-scp PORTFILE MODE [AE=host:port] - MODE: same | new | same-then-new
int commit_scp(int argc, char** argv) {
    if (argc < 3) return fail("commit-scp PORTFILE same|new|same-then-new [AE=host:port]...");
    auto svc = std::make_shared<ServiceSet>();
    const std::string mode = argv[2];
    svc->commitment_report = mode == "same" ? CommitmentReportMode::SameAssociation
                             : mode == "new" ? CommitmentReportMode::NewAssociation
                                             : CommitmentReportMode::SameThenNew;
    std::map<std::string, MoveDestination> peers;
    for (int i = 3; i < argc; ++i)
        if (auto p = parse_peer(argv[i])) peers[p->first] = p->second;
    svc->resolve_ae = resolver(peers);
    svc->on_commitment = [](const CommitmentRequest& rq) {
        CommitmentResult r;
        r.retrieve_ae = "SIAARA_COMMIT";
        for (const SopRef& x : rq.items) {
            if (x.sop_instance.size() >= 2 && x.sop_instance.compare(x.sop_instance.size() - 2, 2, ".9") == 0)
                r.failed.push_back({x, 0x0112});
            else
                r.committed.push_back(x);
        }
        std::printf("commitment request %s: %zu item(s)\n", rq.transaction_uid.c_str(), rq.items.size());
        std::fflush(stdout);
        return r;
    };
    ScpServer s(scp_config("SIAARA_COMMIT"), with_services(svc));
    if (Status st = s.start(); !st) return fail(st.message);
    write_port(argv[1], s.port());
    run_until_stopped(s);
    return 0;
}

// commit-scu HOST PORT CALLED [--listen PORTFILE] : prints the result
int commit_scu(int argc, char** argv) {
    if (argc < 4) return fail("commit-scu HOST PORT CALLED [--listen]");
    const bool listen = argc > 4 && !std::strcmp(argv[4], "--listen");
    std::mutex mu;
    std::optional<CommitmentResult> got;
    std::unique_ptr<ScpServer> listener;
    std::uint16_t listen_port = 0;
    if (listen) {
        auto svc = std::make_shared<ServiceSet>();
        svc->on_commitment_report = [&](const std::string&, const CommitmentResult& r) -> std::uint16_t {
            std::lock_guard<std::mutex> l(mu);
            got = r;
            return dimse_status::Success;
        };
        ScpConfig lc = scp_config("SIAARA_SCU");
        lc.port = static_cast<std::uint16_t>(argc > 5 ? std::atoi(argv[5]) : 0);
        listener = std::make_unique<ScpServer>(lc, with_services(svc));
        if (Status st = listener->start(); !st) return fail(st.message);
        listen_port = listener->port();
        std::printf("listening on %u\n", listen_port);
        std::fflush(stdout);
    }
    Scu scu(scu_config(argv[1], argv[2], argv[3], "SIAARA_SCU"));
    scu.add_context(std::string(sop::StorageCommitmentPushModel), native_transfer_syntaxes());
    if (Status st = scu.connect(); !st) return fail(st.message);
    const std::vector<SopRef> items = {{"1.2.840.10008.5.1.4.1.1.7", "2.25.1001.1"},
                                       {"1.2.840.10008.5.1.4.1.1.7", "2.25.1001.9"}};
    Result<std::string> txn = request_commitment(scu, items);
    if (!txn) return fail(txn.error().message);
    if (!listen) {
        Result<CommitmentResult> r = wait_commitment_result(scu, 20000);
        if (!r) return fail(r.error().message);
        got = *r;
        (void)scu.release();
    } else {
        (void)scu.release();
        for (int i = 0; i < 400; ++i) {
            {
                std::lock_guard<std::mutex> l(mu);
                if (got) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        listener->stop();
    }
    if (!got) return fail("no storage commitment result");
    std::printf("result txn_match=%d event=%u committed=%zu failed=%zu\n", got->transaction_uid == *txn ? 1 : 0,
                got->event_type(), got->committed.size(), got->failed.size());
    return 0;
}

int mpps_scp(int argc, char** argv) {
    if (argc < 2) return fail("mpps-scp PORTFILE");
    auto svc = std::make_shared<ServiceSet>();
    auto store = std::make_shared<MppsStore>();
    svc->on_mpps_create = [store](const std::string&, const std::string& uid, const Dataset& d) {
        const std::uint16_t st = store->create(uid, d);
        std::printf("N-CREATE %s -> 0x%04X\n", uid.c_str(), st);
        std::fflush(stdout);
        return st;
    };
    svc->on_mpps_set = [store](const std::string&, const std::string& uid, const Dataset& d) {
        const std::uint16_t st = store->set(uid, d);
        std::printf("N-SET %s -> 0x%04X\n", uid.c_str(), st);
        std::fflush(stdout);
        return st;
    };
    ScpServer s(scp_config("SIAARA_MPPS"), with_services(svc));
    if (Status st = s.start(); !st) return fail(st.message);
    write_port(argv[1], s.port());
    run_until_stopped(s);
    return 0;
}

int mpps_scu(int argc, char** argv) {
    if (argc < 4) return fail("mpps-scu HOST PORT CALLED");
    Scu scu(scu_config(argv[1], argv[2], argv[3], "SIAARA_SCU"));
    scu.add_context(std::string(sop::ModalityPerformedProcedureStep), native_transfer_syntaxes());
    if (Status st = scu.connect(); !st) return fail(st.message);
    Dataset a;
    (void)a.set_string(0x00400252, VR::CS, "IN PROGRESS");
    (void)a.set_string(0x00400253, VR::SH, "PPS-1");
    (void)a.set_string(tags::Modality, VR::CS, "CT");
    (void)a.set_string(tags::PatientName, VR::PN, "Cross^Check");
    const std::string uid = make_uid();
    Result<Scu::NResponse> c = mpps_create(scu, uid, a);
    if (!c) return fail(c.error().message);
    Dataset d;
    (void)d.set_string(0x00400252, VR::CS, "COMPLETED");
    Result<Scu::NResponse> s1 = mpps_set(scu, uid, d);
    if (!s1) return fail(s1.error().message);
    Result<Scu::NResponse> s2 = mpps_set(scu, uid, d);
    if (!s2) return fail(s2.error().message);
    std::printf("create=0x%04X set=0x%04X set_again=0x%04X\n", c->command.status.value_or(0xFFFF),
                s1->command.status.value_or(0xFFFF), s2->command.status.value_or(0xFFFF));
    (void)scu.release();
    return 0;
}

int ups_scp(int argc, char** argv) {
    if (argc < 2) return fail("ups-scp PORTFILE [AE=host:port]...");
    auto svc = std::make_shared<ServiceSet>();
    svc->ups = std::make_shared<UpsProvider>();
    std::map<std::string, MoveDestination> peers;
    for (int i = 2; i < argc; ++i)
        if (auto p = parse_peer(argv[i])) peers[p->first] = p->second;
    svc->resolve_ae = resolver(peers);
    ScpServer s(scp_config("SIAARA_UPS"), with_services(svc, true));
    if (Status st = s.start(); !st) return fail(st.message);
    write_port(argv[1], s.port());
    run_until_stopped(s);
    return 0;
}

// ups-scu HOST PORT CALLED: create, find, claim, set, get, complete; prints statuses.
int ups_scu(int argc, char** argv) {
    if (argc < 4) return fail("ups-scu HOST PORT CALLED");
    Scu scu(scu_config(argv[1], argv[2], argv[3], "SIAARA_SCU"));
    for (std::string_view c : {sop::UpsPush, sop::UpsPull, sop::UpsWatch})
        scu.add_context(std::string(c), native_transfer_syntaxes());
    if (Status st = scu.connect(); !st) return fail(st.message);
    auto st = [](const Result<Scu::NResponse>& r) -> unsigned { return r ? r->command.status.value_or(0xFFFF) : 0xFFFFFu; };
    Dataset ups;
    (void)ups.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)ups.set_string(tags::PatientName, VR::PN, "Cross^Check");
    (void)ups.set_string(0x00741204, VR::LO, "Crosscheck step");
    (void)ups.set_string(0x00404041, VR::CS, "READY");
    (void)ups.set_string(0x00404005, VR::DT, "20261109093000");  // Scheduled Procedure Step Start DateTime
    (void)ups.set_string(0x00741200, VR::CS, "MEDIUM");          // Scheduled Procedure Step Priority
    const std::string uid = make_uid();
    const unsigned create = st(ups_create(scu, uid, ups));
    Dataset q;
    (void)q.set_string(0x00741000, VR::CS, "SCHEDULED");
    (void)q.set_string(tags::SOPInstanceUID, VR::UI, "");
    std::size_t found = 0;
    Result<FindResult> fr = ups_find(scu, sop::UpsPull, q, [&](const Dataset&) {
        ++found;
        return true;
    });
    const std::string txn = make_uid();
    const unsigned claim = st(ups_change_state(scu, uid, "IN PROGRESS", txn));
    Dataset mod;
    (void)mod.set_string(0x00741204, VR::LO, "Crosscheck step 2");
    (void)mod.set_string(0x00081195, VR::UI, txn);
    const unsigned set = st(ups_set(scu, uid, mod));
    Result<Scu::NResponse> g = ups_get(scu, sop::UpsPull, uid, {0x00741000, 0x00741204});
    const unsigned done = st(ups_change_state(scu, uid, "COMPLETED", txn));
    std::printf("create=0x%04X find=%s/%zu claim=0x%04X set=0x%04X get=0x%04X state=%s label=%s complete=0x%04X\n",
                create, fr ? "ok" : "error", found, claim, set, st(g),
                g ? g->dataset.get_string(0x00741000).value_or("-").c_str() : "-",
                g ? g->dataset.get_string(0x00741204).value_or("-").c_str() : "-", done);
    (void)scu.release();
    return 0;
}

// ups-watch PORTFILE OUTFILE : UPS Event receiver; appends "event uid state" lines to OUTFILE
int ups_watch(int argc, char** argv) {
    if (argc < 3) return fail("ups-watch PORTFILE OUTFILE");
    auto svc = std::make_shared<ServiceSet>();
    const std::string out = argv[2];
    auto mu = std::make_shared<std::mutex>();
    svc->on_ups_event = [out, mu](const std::string&, const UpsEventReport& ev) -> std::uint16_t {
        std::lock_guard<std::mutex> l(*mu);
        std::ofstream(out, std::ios::app) << ev.event_type << " " << ev.ups_uid << " "
                                          << ev.info.get_string(0x00741000).value_or("-") << "\n";
        return dimse_status::Success;
    };
    ScpServer s(scp_config("SIAARA_WATCH"), with_services(svc));
    if (Status st = s.start(); !st) return fail(st.message);
    write_port(argv[1], s.port());
    run_until_stopped(s);
    return 0;
}

int mwlscu(int argc, char** argv) {
    if (argc < 5) return fail("mwlscu HOST PORT CALLED STATION [DATE]");
    Scu scu(scu_config(argv[1], argv[2], argv[3], "SIAARA_SCU"));
    scu.add_context(std::string(sop::ModalityWorklistFind), native_transfer_syntaxes());
    if (Status st = scu.connect(); !st) return fail(st.message);
    std::vector<std::string> names;
    Result<FindResult> r = scu.find(sop::ModalityWorklistFind, make_worklist_query(argv[4], "", argc > 5 ? argv[5] : ""),
                                    [&](const Dataset& d) {
                                        names.push_back(d.get_string(tags::PatientName).value_or(""));
                                        return true;
                                    });
    if (!r) return fail(r.error().message);
    std::printf("status=0x%04X matches=%zu", r->status, names.size());
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) std::printf(" %s", n.c_str());
    std::printf("\n");
    (void)scu.release();
    return 0;
}

int print_scu(int argc, char** argv) {
    if (argc < 4) return fail("print-scu HOST PORT CALLED");
    Scu scu(scu_config(argv[1], argv[2], argv[3], "SIAARA_SCU"));
    scu.add_context(std::string(sop::BasicGrayscalePrintManagementMeta), native_transfer_syntaxes());
    if (Status st = scu.connect(); !st) return fail(st.message);
    PrintScu p(scu, false);
    Result<Dataset> ps = p.printer_status();
    std::printf("printer=%s", ps ? ps->get_string(0x21100010).value_or("-").c_str() : ps.error().message.c_str());
    Dataset session;
    (void)session.set_string(0x20000010, VR::IS, "1");
    (void)session.set_string(0x20000030, VR::CS, "PAPER");
    Result<std::string> sid = p.create_film_session(session);
    if (!sid) return fail(sid.error().message);
    Dataset fbattrs;
    (void)fbattrs.set_string(0x20100010, VR::ST, "STANDARD\\1,1");
    (void)fbattrs.set_string(0x20100040, VR::CS, "PORTRAIT");
    (void)fbattrs.set_string(0x20100050, VR::CS, "8INX10IN");
    Result<PrintScu::FilmBox> fb = p.create_film_box(fbattrs);
    if (!fb) return fail(fb.error().message);
    if (fb->image_boxes.empty()) return fail("no image box");
    Dataset img;
    (void)img.set_uint16(tags::SamplesPerPixel, 1);
    (void)img.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)img.set_uint16(tags::Rows, 64);
    (void)img.set_uint16(tags::Columns, 64);
    (void)img.set_string(0x00280034, VR::IS, "1\\1");  // Pixel Aspect Ratio
    (void)img.set_uint16(tags::BitsAllocated, 8);
    (void)img.set_uint16(tags::BitsStored, 8);
    (void)img.set_uint16(tags::HighBit, 7);
    (void)img.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(64 * 64);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(i % 64 * 4);
    (void)img.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    Status s1 = p.set_image_box(fb->image_boxes[0], 1, img);
    Status s2 = p.print_film_box(fb->uid);
    Status s3 = p.delete_film_box(fb->uid);
    Status s4 = p.delete_film_session();
    std::printf(" session=%s filmbox=%s imagebox=%s print=%s delete_box=%s delete_session=%s events=%zu\n",
                sid->empty() ? "-" : "ok", fb->uid.empty() ? "-" : "ok", s1 ? "ok" : s1.message.c_str(),
                s2 ? "ok" : s2.message.c_str(), s3 ? "ok" : s3.message.c_str(), s4 ? "ok" : s4.message.c_str(),
                p.printer_events().size());
    (void)scu.release();
    return s1 && s2 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return fail("command expected");
    const std::string c = argv[1];
    if (c == "make-image") return make_image(argc - 1, argv + 1);
    if (c == "frames") return frames(argc - 1, argv + 1);
    if (c == "transcode") return transcode(argc - 1, argv + 1);
    if (c == "storescp") return storescp(argc - 1, argv + 1);
    if (c == "mwlscp") return mwlscp(argc - 1, argv + 1);
    if (c == "mwlscu") return mwlscu(argc - 1, argv + 1);
    if (c == "commit-scp") return commit_scp(argc - 1, argv + 1);
    if (c == "commit-scu") return commit_scu(argc - 1, argv + 1);
    if (c == "mpps-scp") return mpps_scp(argc - 1, argv + 1);
    if (c == "mpps-scu") return mpps_scu(argc - 1, argv + 1);
    if (c == "ups-scp") return ups_scp(argc - 1, argv + 1);
    if (c == "ups-scu") return ups_scu(argc - 1, argv + 1);
    if (c == "ups-watch") return ups_watch(argc - 1, argv + 1);
    if (c == "print-scu") return print_scu(argc - 1, argv + 1);
    return fail("unknown command " + c);
}
