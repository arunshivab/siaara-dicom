// Test-only helper for tests/crosscheck/capi_net_crosscheck.py: runs the C API service set
// (siaara_scp_start_ex) as an SCP for third-party SCUs, and the C API SCU calls against
// third-party SCPs. Not installed.
//
//   capi_net_tool scp PORTFILE STOREDIR AE [--worklist FILE.json] [--commit same|new|same-then-new]
//                 [--dest AE:HOST:PORT]...        runs until SIGTERM
//   capi_net_tool mpps-scu HOST PORT CALLED       N-CREATE, N-SET COMPLETED, N-SET again
//   capi_net_tool commit-scu HOST PORT CALLED CLASS INSTANCE [RECV_AE RECV_PORT]
//   capi_net_tool worklist-scu HOST PORT CALLED MODALITY
//   capi_net_tool find-scu HOST PORT CALLED LEVEL PATIENT_ID   (Study Root; LEVEL STUDY/SERIES/IMAGE)
//   capi_net_tool get-scu HOST PORT CALLED STUDY_UID FOLDER
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "siaara/siaara.h"

namespace {

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

siaara_dimse_peer peer(const char* host, const char* port, const char* called) {
    siaara_dimse_peer p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.host = host;
    p.port = static_cast<uint32_t>(std::atoi(port));
    p.called_ae = called;
    p.calling_ae = "SIAARA_SCU";
    p.connect_timeout_ms = 5000;
    p.acse_timeout_ms = 5000;
    p.dimse_timeout_ms = 10000;
    return p;
}

siaara_dimse_result result() {
    siaara_dimse_result r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    return r;
}

int32_t SIAARA_CALL count_match(void* user, const siaara_dataset*) {
    ++*static_cast<int*>(user);
    return 0;
}

int fail(const char* what) {
    std::fprintf(stderr, "%s: %s\n", what, siaara_last_error_message());
    return 1;
}

siaara_file* new_dataset() {
    siaara_file* f = nullptr;
    return siaara_file_create(&f) == SIAARA_OK ? f : nullptr;
}

siaara_dataset* ds_of(siaara_file* f) {
    siaara_dataset* d = nullptr;
    (void)siaara_file_dataset(f, &d);
    return d;
}

void set(siaara_file* f, uint32_t tag, const char* vr, const char* v) {
    (void)siaara_dataset_set_string(ds_of(f), tag, vr, v, std::strlen(v));
}

int run_scp(int argc, char** argv) {
    if (argc < 5) return 2;
    const std::string port_file = argv[2], store_dir = argv[3], ae = argv[4];
    std::string worklist;
    int32_t commit_mode = SIAARA_COMMIT_REPORT_SAME_THEN_NEW;
    std::vector<std::string> dests;
    for (int i = 5; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--worklist") {
            std::ifstream in(argv[i + 1]);
            worklist.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        } else if (k == "--commit") {
            const std::string m = argv[i + 1];
            commit_mode = m == "same" ? SIAARA_COMMIT_REPORT_SAME : m == "new" ? SIAARA_COMMIT_REPORT_NEW : commit_mode;
        } else if (k == "--dest") {
            dests.emplace_back(argv[i + 1]);
        }
    }
    siaara_store* store = nullptr;
    if (siaara_store_open(store_dir.c_str(), store_dir.size(), nullptr, &store) != SIAARA_OK) return fail("store");
    std::vector<siaara_ae_entry> table;
    std::vector<std::string> parts;  // keeps the strings alive
    parts.reserve(dests.size() * 2);
    for (const std::string& d : dests) {
        const auto a = d.find(':'), b = d.rfind(':');
        if (a == std::string::npos || a == b) return 2;
        parts.push_back(d.substr(0, a));
        const char* aep = parts.back().c_str();
        parts.push_back(d.substr(a + 1, b - a - 1));
        siaara_ae_entry e;
        std::memset(&e, 0, sizeof e);
        e.struct_size = sizeof e;
        e.ae_title = aep;
        e.host = parts.back().c_str();
        e.port = static_cast<uint32_t>(std::atoi(d.c_str() + b + 1));
        table.push_back(e);
    }
    siaara_scp_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.ae_title = ae.c_str();
    o.bind_address = "127.0.0.1";
    o.allow_any_calling_ae = 1;
    o.accept_compressed = 1;
    siaara_scp_services s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.storage = s.find = s.move = s.get = s.worklist = s.mpps = s.commitment = s.commitment_reports = 1;
    s.ups = s.instance_availability = 1;
    s.relational = s.fuzzy_person_names = s.combined_datetime = 1;
    s.commitment_report_mode = commit_mode;
    s.ae_table = table.data();
    s.ae_count = static_cast<uint32_t>(table.size());
    siaara_scp* scp = nullptr;
    if (siaara_scp_start_ex(&o, &s, store, nullptr, nullptr, &scp) != SIAARA_OK) return fail("scp");
    if (!worklist.empty() && siaara_scp_set_worklist_json(scp, worklist.data(), worklist.size()) != SIAARA_OK)
        return fail("worklist");
    uint32_t port = 0;
    (void)siaara_scp_port(scp, &port);
    {
        std::ofstream pf(port_file + ".tmp");
        pf << port << "\n";
    }
    (void)std::rename((port_file + ".tmp").c_str(), port_file.c_str());
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    siaara_scp_stats st;
    std::memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    (void)siaara_scp_stats_get(scp, &st);
    std::printf("stored=%llu finds=%llu retrieves=%llu mpps=%llu commitments=%llu\n",
                static_cast<unsigned long long>(st.instances_stored), static_cast<unsigned long long>(st.finds),
                static_cast<unsigned long long>(st.retrieves), static_cast<unsigned long long>(st.mpps),
                static_cast<unsigned long long>(st.commitments));
    siaara_scp_free(scp);
    siaara_store_free(store);
    return 0;
}

int run_mpps(char** argv) {
    siaara_dimse_peer p = peer(argv[2], argv[3], argv[4]);
    siaara_file* a = new_dataset();
    set(a, 0x00400252, "CS", "IN PROGRESS");
    set(a, 0x00400253, "SH", "PPS-1");
    char uid[80];
    size_t need = 0;
    siaara_dimse_result r1 = result(), r2 = result(), r3 = result();
    (void)siaara_dimse_mpps_create(&p, nullptr, ds_of(a), nullptr, uid, sizeof uid, &need, &r1);
    siaara_file* m = new_dataset();
    set(m, 0x00400252, "CS", "COMPLETED");
    (void)siaara_dimse_mpps_set(&p, uid, ds_of(m), nullptr, &r2);
    (void)siaara_dimse_mpps_set(&p, uid, ds_of(m), nullptr, &r3);
    std::printf("create=0x%04X set=0x%04X set_again=0x%04X\n", r1.status, r2.status, r3.status);
    siaara_file_free(a);
    siaara_file_free(m);
    return 0;
}

int run_commit(int argc, char** argv) {
    siaara_dimse_peer p = peer(argv[2], argv[3], argv[4]);
    const char* classes[] = {argv[5]};
    const char* insts[] = {argv[6]};
    siaara_commit_options co;
    std::memset(&co, 0, sizeof co);
    co.struct_size = sizeof co;
    co.wait_ms = 15000;
    siaara_store* store = nullptr;
    siaara_scp* recv = nullptr;
    if (argc >= 9) {
        const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/siaara_xc_recv_" + argv[8];
        if (siaara_store_open(dir.c_str(), dir.size(), nullptr, &store) != SIAARA_OK) return fail("store");
        siaara_scp_options o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.ae_title = argv[7];
        o.bind_address = "127.0.0.1";
        o.port = static_cast<uint32_t>(std::atoi(argv[8]));
        o.allow_any_calling_ae = 1;
        siaara_scp_services s;
        std::memset(&s, 0, sizeof s);
        s.struct_size = sizeof s;
        s.commitment_reports = 1;
        if (siaara_scp_start_ex(&o, &s, store, nullptr, nullptr, &recv) != SIAARA_OK) return fail("receiver");
        co.receiver = recv;
        co.same_association_ms = -1;
        p.calling_ae = argv[7];
    }
    uint16_t reason = 0;
    siaara_dimse_result r = result();
    const siaara_status st = siaara_dimse_commit_ex(&p, &co, classes, insts, 1, nullptr, &reason, &r);
    std::printf("status=%d committed=%u failed=%u reason=0x%04X\n", st, r.completed, r.failed, reason);
    siaara_scp_free(recv);
    siaara_store_free(store);
    return st == SIAARA_OK || st == SIAARA_ERR_REJECTED ? 0 : 1;
}

int run_worklist(char** argv) {
    siaara_dimse_peer p = peer(argv[2], argv[3], argv[4]);
    siaara_file* q = nullptr;
    if (siaara_worklist_query(nullptr, argv[5], nullptr, nullptr, nullptr, &q) != SIAARA_OK) return fail("query");
    int n = 0;
    siaara_dimse_result r = result();
    const siaara_status st = siaara_dimse_find(&p, SIAARA_QR_WORKLIST, ds_of(q), nullptr, count_match, &n, &r);
    std::printf("status=%d matches=%d\n", st, n);
    siaara_file_free(q);
    return st == SIAARA_OK ? 0 : 1;
}

int run_find(char** argv) {
    siaara_dimse_peer p = peer(argv[2], argv[3], argv[4]);
    const std::string level = argv[5];
    siaara_qr_keys k;
    std::memset(&k, 0, sizeof k);
    k.struct_size = sizeof k;
    k.return_keys = 1;
    k.patient_id = argv[6];
    const int32_t lv = level == "SERIES" ? SIAARA_LEVEL_SERIES : level == "IMAGE" ? SIAARA_LEVEL_INSTANCE : SIAARA_LEVEL_STUDY;
    siaara_file* id = nullptr;
    if (siaara_qr_identifier(lv, &k, &id) != SIAARA_OK) return fail("identifier");
    siaara_qr_options qo;
    std::memset(&qo, 0, sizeof qo);
    qo.struct_size = sizeof qo;
    qo.flags = SIAARA_QR_RELATIONAL;
    int n = 0;
    siaara_dimse_result r = result();
    const siaara_status st = siaara_dimse_find_ex(&p, SIAARA_QR_STUDY_ROOT, ds_of(id), &qo, nullptr, count_match, &n, &r);
    std::printf("status=%d matches=%d relational=%u\n", st, n, qo.accepted_flags & SIAARA_QR_RELATIONAL);
    siaara_file_free(id);
    return st == SIAARA_OK ? 0 : 1;
}

int run_get(char** argv) {
    siaara_dimse_peer p = peer(argv[2], argv[3], argv[4]);
    siaara_qr_keys k;
    std::memset(&k, 0, sizeof k);
    k.struct_size = sizeof k;
    k.study_instance_uid = argv[5];
    siaara_file* id = nullptr;
    if (siaara_qr_identifier(SIAARA_LEVEL_STUDY, &k, &id) != SIAARA_OK) return fail("identifier");
    siaara_get_options go;
    std::memset(&go, 0, sizeof go);
    go.struct_size = sizeof go;
    go.folder = argv[6];
    siaara_dimse_result r = result();
    const siaara_status st = siaara_dimse_get(&p, SIAARA_QR_STUDY_ROOT, ds_of(id), &go, nullptr, nullptr, nullptr, &r);
    std::printf("status=%d completed=%u failed=%u\n", st, r.completed, r.failed);
    siaara_file_free(id);
    return st == SIAARA_OK ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "scp") return run_scp(argc, argv);
    if (mode == "mpps-scu" && argc >= 5) return run_mpps(argv);
    if (mode == "commit-scu" && argc >= 7) return run_commit(argc, argv);
    if (mode == "worklist-scu" && argc >= 6) return run_worklist(argv);
    if (mode == "find-scu" && argc >= 7) return run_find(argv);
    if (mode == "get-scu" && argc >= 7) return run_get(argv);
    std::fprintf(stderr, "usage: see the comment at the top of capi_net_tool.cpp\n");
    return 2;
}
