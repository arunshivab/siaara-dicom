// SIAARA DICOM Toolkit - C API, networking: the full DIMSE service set on one SCP
// (siaara_scp_start_ex: Storage, Q/R C-FIND / C-MOVE / C-GET from the local store, Modality
// Worklist, MPPS, Storage Commitment, UPS, Instance Availability), the SCU additions (Q/R
// identifier builder, extended negotiation, MPPS, Storage Commitment with the report on a new
// association, generic N-services, UID change reports) and the DICOMweb origin server over the
// local store (include/siaara/siaara.h, "networking: full service set").
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <vector>

#include "capi/capi_internal.hpp"
#include "capi/capi_net_internal.hpp"
#include "capi/store_services.hpp"
#include "codec/transcode.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "net/dimse.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "web/dicom_json.hpp"
#include "web/server.hpp"

namespace {

using siaara::Errc;
using siaara::Status;
namespace net = siaara::net;
namespace st = siaara::store;
namespace capi = siaara::capi;
namespace web = siaara::web;
using siaara::Dataset;
using siaara::Tag;
using siaara::VR;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
siaara_status fail(const Status& s) noexcept { return capi::set_error(s); }
siaara_status ok() noexcept { return capi::clear_error(); }

template <class F>
siaara_status guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return fail(Errc::OutOfMemory, "out of memory");
    } catch (...) {
        return fail(Errc::Internal, "internal error");
    }
}

const Dataset* ds_cast(const siaara_dataset* p) { return reinterpret_cast<const Dataset*>(p); }
const siaara_dataset* ds_out(const Dataset* p) { return reinterpret_cast<const siaara_dataset*>(p); }

siaara_status copy_out(std::string_view s, char* buf, size_t cap, size_t* needed) {
    if (needed) *needed = s.size();
    if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (s.size() + 1 > cap) {
        std::memcpy(buf, s.data(), cap - 1);
        buf[cap - 1] = '\0';
        return fail(Errc::BufferTooSmall, "buffer too small");
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return ok();
}

bool uid_ok(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return s.front() != '.' && s.back() != '.';
}

// Status codes used by the services (PS3.4, PS3.7 C).
constexpr std::uint16_t kSuccess = 0x0000;
constexpr std::uint16_t kNotAuthorized = 0x0124;
constexpr std::uint16_t kOutOfResources = 0xA700;
constexpr std::uint16_t kResourceLimitation = 0x0213;  // Failure Reason (PS3.3 C.14.1.1)
constexpr std::uint16_t kNoSuchObjectInstance = 0x0112;
constexpr std::uint16_t kClassInstanceConflict = 0x0119;

std::string trim_ae(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

}  // namespace

// ---------------------------------------------------------------- services of siaara_scp_start_ex
namespace siaara::capi {

struct ScpServices {
    siaara_scp* scp = nullptr;
    std::string ae_title;
    bool storage = false, find = false, move = false, get = false, worklist = false, mpps = false;
    bool commitment = false, commitment_reports = false, ups = false, ian = false;
    std::map<std::int32_t, std::set<std::string>> acl;  // SIAARA_SVC_* -> allowed calling AEs
    std::map<std::string, net::MoveDestination> ae_table;
    StoreQrOptions qr;
    std::uint32_t max_worklist = 10000, max_commit = 10000;
    net::ServiceStateLimits mpps_limits, ups_limits;
    std::string state_folder;  // empty: MPPS / UPS state in memory only
    // R-130: the worklist callback runs on a helper thread with an inactivity deadline.
    int worklist_timeout_ms = 30000;
    std::shared_ptr<std::atomic<int>> worklist_hung = std::make_shared<std::atomic<int>>(0);
    siaara_worklist_callback worklist_cb = nullptr;
    void* worklist_user = nullptr;
    mutable std::mutex wl_mu;
    std::shared_ptr<const std::vector<Dataset>> wl_items = std::make_shared<std::vector<Dataset>>();
    std::shared_ptr<net::MppsStore> mpps_store;
    std::shared_ptr<net::UpsProvider> ups_provider;
    // Storage Commitment SCU role: reports awaited by siaara_dimse_commit_ex, by Transaction UID.
    std::mutex cr_mu;
    std::condition_variable cr_cv;
    // R-118: a report counts only when it comes from the AE the request went to and lists
    // nothing that was not requested.
    struct PendingCommit {
        std::string expected_ae;
        std::set<std::pair<std::string, std::string>> items;  // (SOP class, SOP instance)
        std::optional<net::CommitmentResult> result;
    };
    std::map<std::string, PendingCommit> pending;

    bool allowed(std::int32_t svc, const std::string& calling) const {
        const auto it = acl.find(svc);
        return it == acl.end() || it->second.count(trim_ae(calling)) != 0;
    }
    std::shared_ptr<const std::vector<Dataset>> worklist_items() const {
        const std::lock_guard<std::mutex> l(wl_mu);
        return wl_items;
    }
};

}  // namespace siaara::capi

namespace {

using capi::ScpServices;

siaara_scp::Event base_event(std::int32_t kind, std::uint16_t status, const std::string& calling,
                             const std::string& cls, const std::string& addr) {
    siaara_scp::Event ev;
    ev.kind = kind;
    ev.status = status;
    ev.calling = calling;
    ev.cls = cls;
    ev.addr = addr;
    return ev;
}

void refused(ScpServices& sv, const std::string& calling, const std::string& cls, std::uint32_t command_field,
             const std::string& addr) {
    sv.scp->refused.fetch_add(1, std::memory_order_relaxed);
    capi::log(SIAARA_LOG_WARNING, false, "SCP: request from " + calling + " refused by the service allow-list");
    siaara_scp::Event ev = base_event(SIAARA_SCP_REFUSED, kNotAuthorized, calling, cls, addr);
    ev.command_field = command_field;
    sv.scp->post(std::move(ev));
}

std::int32_t service_of_n(const net::Command& rq) {
    const std::string& cls = rq.sop_class();
    if (cls == net::sop::StorageCommitmentPushModel) return SIAARA_SVC_COMMITMENT;
    if (cls == net::sop::ModalityPerformedProcedureStep) return SIAARA_SVC_MPPS;
    if (cls == net::sop::InstanceAvailabilityNotification) return SIAARA_SVC_IAN;
    return SIAARA_SVC_UPS;
}

// Matching options from the extended negotiation agreed for the request's SOP class.
net::MatchOptions match_options(const net::QueryRequest& q, bool& relational) {
    const net::QueryFlags f = net::parse_query_flags(q.extended_negotiation, true);
    relational = f.relational;
    net::MatchOptions mo;
    mo.fuzzy_person_names = f.fuzzy_person_names;
    mo.combined_datetime = f.combined_datetime;
    return mo;
}

// Handlers run on the server's threads: no exception may leave them (out of memory while
// building a response or an event). `on_error` is the status answered instead.
template <class F>
auto shielded(std::uint16_t on_error, F&& f) noexcept -> std::uint16_t {
    try {
        return f();
    } catch (...) {
        capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
        return on_error;
    }
}
template <class F>
void shielded_void(F&& f) noexcept {
    try {
        f();
    } catch (...) {
        capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
    }
}

// R-130: the application's worklist callback runs on a helper thread, so a callback that hangs
// cannot hold the association thread (and with it siaara_scp_stop) for ever. The association
// thread waits for it; when the callback neither returns nor calls emit() for
// worklist_timeout_ms, or the SCP is stopping, the call is abandoned: the C-FIND ends with
// 0x0110, emit() tells the callback to stop, and the helper thread ends whenever the callback
// returns. At most kMaxHungWorklistCalls abandoned calls may still be running; further queries
// are then answered 0x0110 without calling the callback.
constexpr int kMaxHungWorklistCalls = 4;

struct WorklistCall {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false, abandoned = false;
    std::uint32_t result = 0;
    const std::function<bool(const Dataset&)>* offer = nullptr;  // valid until done or abandoned
    Dataset identifier;
    std::string calling;
    std::chrono::steady_clock::time_point last_activity;
};

int32_t SIAARA_CALL worklist_emit_fn(void* c, const siaara_dataset* item) {
    if (!c || !item) return 1;
    auto* call = static_cast<WorklistCall*>(c);
    try {
        // Held while the item is matched and sent: the waiting thread abandons the call only
        // between items, so `offer` (which lives on its stack) is never used after it left.
        const std::lock_guard<std::mutex> l(call->mu);
        if (call->abandoned || !call->offer) return 1;
        const bool go_on = (*call->offer)(*reinterpret_cast<const Dataset*>(item));
        call->last_activity = std::chrono::steady_clock::now();
        return go_on ? 0 : 1;
    } catch (...) {
        capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
}

std::uint16_t run_worklist_callback(siaara_scp& scp, ScpServices& sv, const net::QueryRequest& q,
                                    const std::function<bool(const Dataset&)>& offer) {
    if (sv.worklist_hung->load() >= kMaxHungWorklistCalls) {
        capi::log(SIAARA_LOG_ERROR, false, "SCP: worklist callbacks that never returned are still running; query refused");
        return net::dimse_status::ProcessingFailure;
    }
    auto call = std::make_shared<WorklistCall>();
    call->offer = &offer;
    call->identifier = q.identifier;
    call->calling = q.calling_ae;
    call->last_activity = std::chrono::steady_clock::now();
    const siaara_worklist_callback cb = sv.worklist_cb;
    void* const user = sv.worklist_user;
    const std::shared_ptr<std::atomic<int>> hung = sv.worklist_hung;
    std::thread helper([call, cb, user, hung] {
        std::uint32_t r = net::dimse_status::ProcessingFailure;
        try {
            r = cb(user, call->calling.c_str(), ds_out(&call->identifier), worklist_emit_fn, call.get());
        } catch (...) {
            capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
        }
        bool was_abandoned = false;
        {
            const std::lock_guard<std::mutex> l(call->mu);
            call->done = true;
            call->result = r;
            was_abandoned = call->abandoned;
        }
        if (was_abandoned) hung->fetch_sub(1);
        call->cv.notify_all();
    });
    const auto timeout = std::chrono::milliseconds(sv.worklist_timeout_ms);
    bool abandoned = false;
    std::uint32_t result = 0;
    {
        std::unique_lock<std::mutex> l(call->mu);
        while (!call->done) {
            (void)call->cv.wait_for(l, std::chrono::milliseconds(100));
            if (call->done) break;
            if (scp.stop_requested.load() || std::chrono::steady_clock::now() - call->last_activity >= timeout) {
                call->abandoned = abandoned = true;
                hung->fetch_add(1);
                break;
            }
        }
        call->offer = nullptr;
        result = call->result;
    }
    if (abandoned) {
        helper.detach();
        capi::log(SIAARA_LOG_ERROR, false,
                  "SCP: the worklist callback did not return in time; the query was ended with 0x0110");
        return net::dimse_status::ProcessingFailure;
    }
    helper.join();
    return static_cast<std::uint16_t>(result);
}

// ---- the handlers
void install_handlers(siaara_scp* raw, ScpServices& sv, net::ScpHandlers& hd, net::ServiceSet& ss) {
    ScpServices* s = &sv;
    // Verification and Storage as siaara_scp_start, behind the allow-lists.
    net::ScpHandlers base;
    capi::dimse::storage_handlers(raw, base);
    hd.log = base.log;
    hd.on_store_refused = base.on_store_refused;        // R-116: incomplete data refused
    hd.on_store_quarantine = base.on_store_quarantine;  // R-116: incomplete data kept outside the archive
    hd.on_echo = [s, echo = base.on_echo](const std::string& calling) -> std::uint16_t {
        return shielded(net::dimse_status::ProcessingFailure, [&]() -> std::uint16_t {
            if (!s->allowed(SIAARA_SVC_VERIFICATION, calling)) {
                refused(*s, calling, std::string(net::sop::Verification), 0x0030, "");
                return kNotAuthorized;
            }
            return echo(calling);
        });
    };
    if (sv.storage)
        hd.on_store = [s, store = base.on_store](const net::StoreRequest& rq) -> std::uint16_t {
            return shielded(net::dimse_status::OutOfResources, [&]() -> std::uint16_t {
                if (!s->allowed(SIAARA_SVC_STORAGE, rq.calling_ae)) {
                    refused(*s, rq.calling_ae, rq.sop_class_uid, 0x0001, rq.peer_address);
                    return kNotAuthorized;
                }
                return store(rq);
            });
        };
    hd.on_sop_instance_uid_changed = [raw](const std::string& calling, const std::string& old_uid,
                                           const std::string& new_uid) {
      shielded_void([&] {
        capi::log(SIAARA_LOG_WARNING, true,
                  "SCP: a retrieve sub-operation was coded lossily; " + old_uid + " was sent as " + new_uid);
        siaara_scp::Event ev = base_event(SIAARA_SCP_UID_CHANGED, kSuccess, calling, "", "");
        ev.inst = new_uid;
        ev.original_inst = old_uid;
        raw->post(std::move(ev));
      });
    };
    hd.on_retrieve_done = [raw](const net::QueryRequest& q, bool is_get, std::uint16_t status, std::uint16_t completed,
                                std::uint16_t failed, std::uint16_t warning) {
      shielded_void([&] {
        raw->sub_operations.fetch_add(static_cast<std::uint64_t>(completed) + failed + warning, std::memory_order_relaxed);
        siaara_scp::Event ev = base_event(is_get ? SIAARA_SCP_GET : SIAARA_SCP_MOVE, status, q.calling_ae,
                                          q.sop_class_uid, q.peer_address);
        ev.destination = q.move_destination;
        ev.completed = completed;
        ev.failed = failed;
        ev.warning = warning;
        ev.command_field = is_get ? 0x0010u : 0x0021u;
        ev.dataset = q.identifier;
        raw->post(std::move(ev));
      });
    };

    // C-FIND: Q/R from the store, and Modality Worklist.
    if (sv.find || sv.worklist)
        hd.on_find = [raw, s](const net::QueryRequest& q,
                              const std::function<bool(const Dataset&)>& emit) -> std::uint16_t {
          return shielded(net::dimse_status::OutOfResources, [&]() -> std::uint16_t {
            const bool is_worklist = q.sop_class_uid == net::sop::ModalityWorklistFind;
            const std::int32_t svc = is_worklist ? SIAARA_SVC_WORKLIST : SIAARA_SVC_FIND;
            if (!s->allowed(svc, q.calling_ae)) {
                refused(*s, q.calling_ae, q.sop_class_uid, 0x0020, q.peer_address);
                return kNotAuthorized;
            }
            raw->finds.fetch_add(1, std::memory_order_relaxed);
            bool relational = false;
            const net::MatchOptions mo = match_options(q, relational);
            std::uint32_t sent = 0;
            std::uint16_t status = kSuccess;
            if (is_worklist) {
                if (!s->worklist) return net::dimse_status::SopClassNotSupported;
                const Dataset id = capi::identifier_utf8(q.identifier);
                bool stop = false;
                const auto offer = [&](const Dataset& item) -> bool {
                    if (stop) return false;
                    if (!net::query_matches(id, item, mo)) return true;
                    if (sent >= s->max_worklist) {
                        status = kOutOfResources;
                        stop = true;
                        return false;
                    }
                    Dataset rsp = net::query_response(id, item);
                    (void)rsp.set_string(siaara::tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
                    if (!emit(rsp)) {
                        stop = true;
                        return false;
                    }
                    ++sent;
                    return true;
                };
                if (s->worklist_cb) {
                    const std::uint16_t r = run_worklist_callback(*raw, *s, q, offer);
                    if (status == kSuccess && r != 0) status = r;
                } else {
                    const auto items = s->worklist_items();
                    for (const Dataset& item : *items)
                        if (!offer(item)) break;
                }
            } else {
                if (!s->find) return net::dimse_status::SopClassNotSupported;
                status = capi::store_find(*raw->store->store, q.sop_class_uid, q.identifier, relational, mo, s->qr,
                                          emit, sent);
            }
            siaara_scp::Event ev = base_event(is_worklist ? SIAARA_SCP_WORKLIST : SIAARA_SCP_FIND, status, q.calling_ae,
                                              q.sop_class_uid, q.peer_address);
            ev.completed = sent;
            ev.command_field = 0x0020;
            ev.dataset = q.identifier;
            raw->post(std::move(ev));
            return status;
          });
        };

    // C-MOVE / C-GET from the store.
    const auto retrieve = [raw, s](bool is_get) {
        return [raw, s, is_get](const net::QueryRequest& q, std::vector<net::RetrieveItem>& items) -> std::uint16_t {
            if (!s->allowed(is_get ? SIAARA_SVC_GET : SIAARA_SVC_MOVE, q.calling_ae)) {
                refused(*s, q.calling_ae, q.sop_class_uid, is_get ? 0x0010u : 0x0021u, q.peer_address);
                return kNotAuthorized;
            }
            raw->retrieves.fetch_add(1, std::memory_order_relaxed);
            const net::QueryFlags f = net::parse_query_flags(q.extended_negotiation, false);
            // Refusals are reported through on_retrieve_done like completed retrieves.
            return shielded(net::dimse_status::OutOfResources, [&]() {
                return capi::store_retrieve(*raw->store->store, q.sop_class_uid, q.identifier, f.relational, s->qr, items);
            });
        };
    };
    if (sv.move) hd.on_move = retrieve(false);
    if (sv.get) hd.on_get = retrieve(true);
    hd.resolve_move_destination = [s](const std::string& ae) noexcept -> std::optional<net::MoveDestination> {
        try {
            const auto it = s->ae_table.find(trim_ae(ae));
            if (it == s->ae_table.end()) return std::nullopt;
            return it->second;
        } catch (...) {
            capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
    };

    // N-services.
    ss.resolve_ae = hd.resolve_move_destination;
    ss.admit = [s](const std::string& calling, const net::Command& rq) -> std::uint16_t {
        return shielded(kNotAuthorized, [&]() -> std::uint16_t {
            if (s->allowed(service_of_n(rq), calling)) return kSuccess;
            refused(*s, calling, rq.sop_class(), rq.command_field, "");
            return kNotAuthorized;
        });
    };
    if (sv.mpps) {
        ss.mpps = sv.mpps_store;  // created (and loaded from the state folder) by siaara_scp_start_ex
        ss.on_mpps_create = [raw, s](const std::string& calling, const std::string& uid, const Dataset& ds) -> std::uint16_t {
          return shielded(net::dimse_status::ProcessingFailure, [&]() -> std::uint16_t {
            raw->mpps.fetch_add(1, std::memory_order_relaxed);
            // The store bounds itself: IN PROGRESS instances, finished ones kept, bytes (R-109, R-119).
            const std::uint16_t status = s->mpps_store->create(uid, ds, trim_ae(calling));
            siaara_scp::Event ev = base_event(SIAARA_SCP_MPPS_CREATE, status, calling,
                                              std::string(net::sop::ModalityPerformedProcedureStep), "");
            ev.inst = uid;
            ev.command_field = 0x0140;
            ev.dataset = ds;
            raw->post(std::move(ev));
            return status;
          });
        };
        ss.on_mpps_set = [raw, s](const std::string& calling, const std::string& uid, const Dataset& ds) -> std::uint16_t {
          return shielded(net::dimse_status::ProcessingFailure, [&]() -> std::uint16_t {
            raw->mpps.fetch_add(1, std::memory_order_relaxed);
            const std::uint16_t status = s->mpps_store->set(uid, ds, trim_ae(calling));
            siaara_scp::Event ev = base_event(SIAARA_SCP_MPPS_SET, status, calling,
                                              std::string(net::sop::ModalityPerformedProcedureStep), "");
            ev.inst = uid;
            ev.command_field = 0x0120;
            ev.dataset = ds;
            raw->post(std::move(ev));
            return status;
          });
        };
    }
    if (sv.commitment) {
        ss.on_commitment = [raw, s](const net::CommitmentRequest& rq) -> net::CommitmentResult {
            raw->commitments.fetch_add(1, std::memory_order_relaxed);
            net::CommitmentResult r;
            r.transaction_uid = rq.transaction_uid;
            try {
                r.retrieve_ae = s->ae_title;
                const bool too_many = rq.items.size() > s->max_commit;
                for (const net::SopRef& ref : rq.items) {
                    if (too_many) {
                        r.failed.push_back({ref, kResourceLimitation});
                        continue;
                    }
                    auto rec = raw->store->store->instance(ref.sop_instance);
                    // A quarantined copy is not in the archive: never "committed" (R-116).
                    if (!rec) r.failed.push_back({ref, kNoSuchObjectInstance});
                    else if ((*rec)[st::Str::SopClassUid] != ref.sop_class) r.failed.push_back({ref, kClassInstanceConflict});
                    else r.committed.push_back(ref);
                }
            } catch (...) {
                // Out of memory part way: nothing is reported as committed.
                capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
                r.committed.clear();
                r.failed.clear();
            }
            return r;
        };
        ss.on_commitment_done = [raw](const net::CommitmentRequest& rq, const net::CommitmentResult& r, bool delivered) {
          shielded_void([&] {
            siaara_scp::Event ev = base_event(SIAARA_SCP_COMMIT_REQUEST, r.event_type(), rq.calling_ae,
                                              std::string(net::sop::StorageCommitmentPushModel), "");
            ev.transaction_uid = rq.transaction_uid;
            ev.outcome = delivered ? 1 : 0;
            ev.completed = static_cast<std::uint32_t>(r.committed.size());
            ev.failed = static_cast<std::uint32_t>(r.failed.size());
            ev.command_field = 0x0130;
            ev.dataset = net::encode_commitment_result(r);
            raw->post(std::move(ev));
          });
        };
        switch (sv.qr.commitment_mode) {
            case SIAARA_COMMIT_REPORT_SAME: ss.commitment_report = net::CommitmentReportMode::SameAssociation; break;
            case SIAARA_COMMIT_REPORT_NEW: ss.commitment_report = net::CommitmentReportMode::NewAssociation; break;
            default: ss.commitment_report = net::CommitmentReportMode::SameThenNew; break;
        }
    }
    if (sv.commitment_reports)
        ss.on_commitment_report = [raw, s](const std::string& calling, const net::CommitmentResult& r) -> std::uint16_t {
          return shielded(net::dimse_status::ProcessingFailure, [&]() -> std::uint16_t {
            {
                const std::lock_guard<std::mutex> l(s->cr_mu);
                const auto it = s->pending.find(r.transaction_uid);
                if (it != s->pending.end()) {
                    // R-118: only the AE the request went to may report on it, and only on what
                    // was requested.
                    if (trim_ae(calling) != it->second.expected_ae) {
                        capi::log(SIAARA_LOG_WARNING, false,
                                  "SCP: storage commitment report from " + calling + " ignored: the request went to " +
                                      it->second.expected_ae);
                        refused(*s, calling, std::string(net::sop::StorageCommitmentPushModel), 0x0100, "");
                        return kNotAuthorized;
                    }
                    bool foreign = false;
                    for (const net::SopRef& x : r.committed) foreign = foreign || !it->second.items.count({x.sop_class, x.sop_instance});
                    for (const net::CommitmentFailure& x : r.failed)
                        foreign = foreign || !it->second.items.count({x.ref.sop_class, x.ref.sop_instance});
                    if (foreign) {
                        capi::log(SIAARA_LOG_WARNING, false,
                                  "SCP: storage commitment report from " + calling +
                                      " ignored: it lists instances that were not in the request");
                        refused(*s, calling, std::string(net::sop::StorageCommitmentPushModel), 0x0100, "");
                        return net::dimse_status::InvalidAttributeValue;
                    }
                    it->second.result = r;
                }
            }
            s->cr_cv.notify_all();
            siaara_scp::Event ev = base_event(SIAARA_SCP_COMMIT_REPORT, r.event_type(), calling,
                                              std::string(net::sop::StorageCommitmentPushModel), "");
            ev.transaction_uid = r.transaction_uid;
            ev.completed = static_cast<std::uint32_t>(r.committed.size());
            ev.failed = static_cast<std::uint32_t>(r.failed.size());
            ev.command_field = 0x0100;
            ev.dataset = net::encode_commitment_result(r);
            raw->post(std::move(ev));
            return kSuccess;
          });
        };
    if (sv.ups) {
        ss.ups = sv.ups_provider;  // created by siaara_scp_start_ex
        ss.on_ups_served = [raw](const std::string& calling, const net::Command& rq, std::uint16_t status) {
            shielded_void([&] {
                siaara_scp::Event ev = base_event(SIAARA_SCP_UPS, status, calling, rq.sop_class(), "");
                ev.inst = rq.sop_instance();
                ev.command_field = rq.command_field;
                raw->post(std::move(ev));
            });
        };
    }
    if (sv.ian)
        ss.on_instance_availability = [raw](const std::string& calling, const std::string& uid,
                                            const Dataset& ds) -> std::uint16_t {
            shielded_void([&] {
                siaara_scp::Event ev = base_event(SIAARA_SCP_IAN, kSuccess, calling,
                                                  std::string(net::sop::InstanceAvailabilityNotification), "");
                ev.inst = uid;
                ev.command_field = 0x0140;
                ev.dataset = ds;
                raw->post(std::move(ev));
            });
            return kSuccess;
        };
}

// Presentation contexts the enabled services accept (the service set adds its own N-classes).
void build_policy(const ScpServices& sv, const std::vector<std::string>& storage_classes,
                  const std::vector<std::string>& storage_ts, net::ScpConfig& cfg) {
    const std::vector<std::string> native = net::native_transfer_syntaxes();
    cfg.accepted.clear();
    cfg.accepted.push_back({std::string(net::sop::Verification), native, true, false});
    // C-GET sub-operations: whatever this side can send, native first, lossy last (and only when
    // allowed); the transcoder converts each instance as needed.
    std::vector<std::string> get_ts = native;
    for (int pass = 0; pass < 2; ++pass)
        for (const std::string& t : capi::dimse::compressed_syntaxes()) {
            if (!siaara::codec::can_encode(t)) continue;
            const bool lossy = siaara::codec::encoding_is_lossy(t, cfg.transcode_params);
            if (lossy != (pass == 1) || (lossy && !cfg.transcode_params.allow_lossy)) continue;
            get_ts.push_back(t);
        }
    for (const std::string& t : storage_ts)
        if (std::find(get_ts.begin(), get_ts.end(), t) == get_ts.end()) get_ts.push_back(t);
    if (sv.storage || sv.get)
        for (const std::string& c : storage_classes) {
            net::AcceptedSyntax a{c, storage_ts, sv.storage, sv.get};
            if (sv.get) a.scp_role_transfer_syntaxes = get_ts;
            cfg.accepted.push_back(std::move(a));
        }
    const auto add = [&](std::string_view c) { cfg.accepted.push_back({std::string(c), native, true, false}); };
    if (sv.find) {
        add(net::sop::PatientRootFind);
        add(net::sop::StudyRootFind);
    }
    if (sv.worklist) add(net::sop::ModalityWorklistFind);
    if (sv.move) {
        add(net::sop::PatientRootMove);
        add(net::sop::StudyRootMove);
        add(net::sop::CompositeInstanceRootMove);
    }
    if (sv.get) {
        add(net::sop::PatientRootGet);
        add(net::sop::StudyRootGet);
        add(net::sop::CompositeInstanceRootGet);
        add(net::sop::CompositeInstanceRetrieveWithoutBulkDataGet);
    }
    // SOP Class Extended Negotiation the SCP supports.
    cfg.extended_negotiation.clear();
    net::QueryFlags qf;
    qf.relational = sv.qr.relational;
    qf.combined_datetime = sv.qr.combined_datetime;
    qf.fuzzy_person_names = sv.qr.fuzzy_person_names;
    if (sv.find && (qf.relational || qf.combined_datetime || qf.fuzzy_person_names))
        for (std::string_view c : {net::sop::PatientRootFind, net::sop::StudyRootFind})
            cfg.extended_negotiation.push_back(net::find_extended_negotiation(c, qf));
    if (qf.relational) {
        if (sv.move)
            for (std::string_view c : {net::sop::PatientRootMove, net::sop::StudyRootMove})
                cfg.extended_negotiation.push_back(net::retrieve_extended_negotiation(c, true));
        if (sv.get)
            for (std::string_view c : {net::sop::PatientRootGet, net::sop::StudyRootGet})
                cfg.extended_negotiation.push_back(net::retrieve_extended_negotiation(c, true));
    }
}

siaara_status string_list(const char* const* v, uint32_t n, const char* what, std::vector<std::string>& out) {
    if (n && !v) return fail(Errc::InvalidArgument, std::string(what) + " is NULL");
    for (uint32_t i = 0; i < n; ++i) {
        if (!v[i] || !v[i][0]) return fail(Errc::InvalidArgument, std::string(what) + " entry is NULL or empty");
        out.emplace_back(v[i]);
    }
    return SIAARA_OK;
}

siaara_status read_services(const siaara_scp_services* o, const net::ScpConfig& base, ScpServices& sv,
                            net::ScpConfig& cfg) {
    // The siaara_scp_services of this version: every field up to acls is required.
    if (o->struct_size < offsetof(siaara_scp_services, acls) + sizeof(void*))
        return fail(Errc::InvalidArgument, "services must be set up with struct_size");
    const auto flag = [](int32_t v, const char* what, bool& out) -> siaara_status {
        if (v != 0 && v != 1) return fail(Errc::InvalidArgument, std::string(what) + " must be 0 or 1");
        out = v != 0;
        return SIAARA_OK;
    };
    bool lossy = false, rel = false, fuzzy = false, dt = false;
    for (auto [v, what, out] : std::initializer_list<std::tuple<int32_t, const char*, bool*>>{
             {o->storage, "storage", &sv.storage},
             {o->find, "find", &sv.find},
             {o->move, "move", &sv.move},
             {o->get, "get", &sv.get},
             {o->worklist, "worklist", &sv.worklist},
             {o->mpps, "mpps", &sv.mpps},
             {o->commitment, "commitment", &sv.commitment},
             {o->commitment_reports, "commitment_reports", &sv.commitment_reports},
             {o->ups, "ups", &sv.ups},
             {o->instance_availability, "instance_availability", &sv.ian},
             {o->allow_lossy, "allow_lossy", &lossy},
             {o->relational, "relational", &rel},
             {o->fuzzy_person_names, "fuzzy_person_names", &fuzzy},
             {o->combined_datetime, "combined_datetime", &dt}})
        if (siaara_status s = flag(v, what, *out); s != SIAARA_OK) return s;
    if (o->commitment_report_mode < SIAARA_COMMIT_REPORT_SAME_THEN_NEW || o->commitment_report_mode > SIAARA_COMMIT_REPORT_NEW)
        return fail(Errc::InvalidArgument, "unknown commitment_report_mode");
    sv.qr.relational = rel;
    sv.qr.fuzzy_person_names = fuzzy;
    sv.qr.combined_datetime = dt;
    sv.qr.commitment_mode = o->commitment_report_mode;
    sv.qr.retrieve_ae = base.ae_title;
    if (o->max_find_results) sv.qr.max_find_results = o->max_find_results;
    if (o->max_retrieve_instances) sv.qr.max_retrieve_instances = std::min<std::uint32_t>(o->max_retrieve_instances, 65535u);
    if (o->max_worklist_items) sv.max_worklist = o->max_worklist_items;
    if (o->max_mpps_instances) sv.mpps_limits.max_items = o->max_mpps_instances;
    if (o->max_commitment_items) sv.max_commit = o->max_commitment_items;
    sv.worklist_cb = o->worklist_cb;
    sv.worklist_user = o->worklist_user;
    sv.ae_title = base.ae_title;

    cfg.transcode_params.allow_lossy = lossy;
    if (o->struct_size >= offsetof(siaara_scp_services, allow_incomplete) + sizeof(int32_t)) {
        if (siaara_status s = flag(o->allow_incomplete, "allow_incomplete", cfg.allow_incomplete); s != SIAARA_OK) return s;
    }
    if (o->store_transfer_syntax) {
        if (!uid_ok(o->store_transfer_syntax)) return fail(Errc::InvalidArgument, "store_transfer_syntax is not a UID");
        cfg.preferred_store_ts = o->store_transfer_syntax;
    }
    std::vector<std::string> classes, ts;
    if (siaara_status s = string_list(o->storage_sop_classes, o->storage_sop_class_count, "storage_sop_classes", classes);
        s != SIAARA_OK)
        return s;
    if (siaara_status s = string_list(o->transfer_syntaxes, o->transfer_syntax_count, "transfer_syntaxes", ts); s != SIAARA_OK)
        return s;
    if (classes.size() > 512) return fail(Errc::LimitExceeded, "at most 512 storage SOP classes");
    if (ts.size() > 64) return fail(Errc::LimitExceeded, "at most 64 transfer syntaxes");
    for (const std::string& u : classes)
        if (!uid_ok(u)) return fail(Errc::InvalidArgument, "storage_sop_classes entry is not a UID");
    for (const std::string& u : ts)
        if (!uid_ok(u)) return fail(Errc::InvalidArgument, "transfer_syntaxes entry is not a UID");
    if (classes.empty()) classes = net::storage_sop_classes();
    if (ts.empty()) ts = cfg.transfer_syntaxes;
    else cfg.transfer_syntaxes = ts;

    // AE table.
    if (o->ae_count && !o->ae_table) return fail(Errc::InvalidArgument, "ae_table is NULL");
    if (o->ae_count > 4096) return fail(Errc::LimitExceeded, "at most 4096 ae_table entries");
    bool any_tls = false;
    for (uint32_t i = 0; i < o->ae_count; ++i) {
        const siaara_ae_entry& e = o->ae_table[i];
        if (e.struct_size < sizeof(siaara_ae_entry)) return fail(Errc::InvalidArgument, "ae_table entry: struct_size");
        if (!e.ae_title || !e.host || !e.host[0]) return fail(Errc::InvalidArgument, "ae_table entry: ae_title and host");
        const std::string ae = trim_ae(e.ae_title);
        if (!capi::dimse::ae_ok(ae)) return fail(Errc::InvalidArgument, "ae_table entry: invalid AE title");
        if (e.port == 0 || e.port > 65535) return fail(Errc::InvalidArgument, "ae_table entry: port must be 1..65535");
        if (e.tls != 0 && e.tls != 1) return fail(Errc::InvalidArgument, "ae_table entry: tls must be 0 or 1");
        net::MoveDestination d;
        d.host = e.host;
        d.port = static_cast<std::uint16_t>(e.port);
        d.ae_title = ae;
        d.tls = e.tls != 0;
        any_tls = any_tls || e.tls != 0;
        sv.ae_table[ae] = d;
    }
    if (o->outbound_tls) {
        if (o->outbound_tls->struct_size < sizeof(siaara_tls_options))
            return fail(Errc::InvalidArgument, "outbound_tls->struct_size is too small");
        cfg.move_tls = capi::tls_settings(o->outbound_tls);
    }
    cfg.move_tls.enabled = false;  // per destination (siaara_ae_entry.tls)
    if (any_tls && capi::log_enabled(SIAARA_LOG_INFO)) cfg.move_tls.log = capi::log_sink(SIAARA_LOG_INFO);

    // Allow-lists.
    if (o->acl_count && !o->acls) return fail(Errc::InvalidArgument, "acls is NULL");
    for (uint32_t i = 0; i < o->acl_count; ++i) {
        const siaara_scp_acl& a = o->acls[i];
        if (a.struct_size < sizeof(siaara_scp_acl)) return fail(Errc::InvalidArgument, "acls entry: struct_size");
        if (a.service < SIAARA_SVC_VERIFICATION || a.service > SIAARA_SVC_IAN)
            return fail(Errc::InvalidArgument, "acls entry: unknown service");
        if (a.count && !a.calling_aes) return fail(Errc::InvalidArgument, "acls entry: calling_aes is NULL");
        std::set<std::string>& set = sv.acl[a.service];
        for (uint32_t k = 0; k < a.count; ++k) {
            if (!a.calling_aes[k]) return fail(Errc::InvalidArgument, "acls entry: calling AE is NULL");
            set.insert(trim_ae(a.calling_aes[k]));
        }
    }
    // ---- security review 4 additions
    const auto has = [&](std::size_t end) { return o->struct_size >= end; };
    if (has(offsetof(siaara_scp_services, max_service_dataset_bytes) + sizeof(uint32_t)) && o->max_service_dataset_bytes)
        cfg.limits.max_service_dataset_size = o->max_service_dataset_bytes;
    if (has(offsetof(siaara_scp_services, max_ups_items) + sizeof(uint32_t)) && o->max_ups_items)
        sv.ups_limits.max_items = o->max_ups_items;
    if (has(offsetof(siaara_scp_services, max_state_bytes) + sizeof(uint64_t)) && o->max_state_bytes)
        sv.mpps_limits.max_bytes = sv.ups_limits.max_bytes = o->max_state_bytes;
    if (has(offsetof(siaara_scp_services, max_finished_instances) + sizeof(uint32_t)) && o->max_finished_instances)
        sv.mpps_limits.max_finished = sv.ups_limits.max_finished = o->max_finished_instances;
    if (has(offsetof(siaara_scp_services, max_ups_subscribers) + sizeof(uint32_t)) && o->max_ups_subscribers)
        sv.ups_limits.max_subscribers_per_item = o->max_ups_subscribers;
    if (has(offsetof(siaara_scp_services, state_folder) + sizeof(void*)) && o->state_folder) {
        if (!o->state_folder[0]) return fail(Errc::InvalidArgument, "state_folder is empty");
        sv.state_folder = o->state_folder;
    }
    if (has(offsetof(siaara_scp_services, bind_updates_to_creator) + sizeof(int32_t))) {
        bool bind = false;
        if (siaara_status s = flag(o->bind_updates_to_creator, "bind_updates_to_creator", bind); s != SIAARA_OK) return s;
        sv.mpps_limits.bind_to_creator = sv.ups_limits.bind_to_creator = bind;
    }
    if (has(offsetof(siaara_scp_services, worklist_timeout_ms) + sizeof(int32_t))) {
        if (o->worklist_timeout_ms < 0) return fail(Errc::InvalidArgument, "worklist_timeout_ms must not be negative");
        if (o->worklist_timeout_ms) sv.worklist_timeout_ms = o->worklist_timeout_ms;
    }
    if (has(offsetof(siaara_scp_services, move_acl_count) + sizeof(uint32_t))) {
        if (o->move_acl_count && !o->move_acls) return fail(Errc::InvalidArgument, "move_acls is NULL");
        if (o->move_acl_count > 4096) return fail(Errc::LimitExceeded, "at most 4096 move_acls entries");
        for (uint32_t i = 0; i < o->move_acl_count; ++i) {
            const siaara_move_acl& a = o->move_acls[i];
            if (a.struct_size < sizeof(siaara_move_acl)) return fail(Errc::InvalidArgument, "move_acls entry: struct_size");
            if (!a.calling_ae || (a.count && !a.destination_aes))
                return fail(Errc::InvalidArgument, "move_acls entry: calling_ae and destination_aes");
            std::set<std::string>& set = cfg.move_destinations_by_caller[trim_ae(a.calling_ae)];
            for (uint32_t k = 0; k < a.count; ++k) {
                if (!a.destination_aes[k]) return fail(Errc::InvalidArgument, "move_acls entry: destination AE is NULL");
                set.insert(trim_ae(a.destination_aes[k]));
            }
        }
    }
    build_policy(sv, classes, ts, cfg);
    return SIAARA_OK;
}

// R-129: with no allow-list a service that touches patient data is open to every accepted peer.
void warn_open_services(const ScpServices& sv, const net::ScpConfig& cfg) {
    const std::pair<std::int32_t, bool> phi[] = {
        {SIAARA_SVC_STORAGE, sv.storage}, {SIAARA_SVC_FIND, sv.find},       {SIAARA_SVC_MOVE, sv.move},
        {SIAARA_SVC_GET, sv.get},         {SIAARA_SVC_WORKLIST, sv.worklist}, {SIAARA_SVC_MPPS, sv.mpps},
        {SIAARA_SVC_COMMITMENT, sv.commitment}, {SIAARA_SVC_UPS, sv.ups}};
    static const char* const names[] = {"Storage", "C-FIND", "C-MOVE", "C-GET", "Worklist", "MPPS", "Storage Commitment",
                                        "UPS"};
    std::string open;
    for (std::size_t i = 0; i < std::size(phi); ++i)
        if (phi[i].second && sv.acl.find(phi[i].first) == sv.acl.end()) open += std::string(open.empty() ? "" : ", ") + names[i];
    if (!open.empty())
        capi::log(SIAARA_LOG_WARNING, false,
                  std::string("SCP: no allow-list (siaara_scp_services.acls) for: ") + open + "; " +
                      (cfg.allowed_calling_aes.empty() ? "open to ANY calling AE" : "open to every allowed calling AE"));
    if (sv.move && cfg.move_destinations_by_caller.empty() && !sv.ae_table.empty())
        capi::log(SIAARA_LOG_WARNING, false,
                  "SCP: no move_acls: a peer allowed C-MOVE may send any study to any ae_table entry");
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_scp_start_ex(const siaara_scp_options* options,
                                                         const siaara_scp_services* services, siaara_store* store,
                                                         siaara_scp_callback cb, void* user, siaara_scp** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!services) return siaara_scp_start(options, store, cb, user, out);
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        net::ScpConfig cfg;
        if (siaara_status s = capi::dimse::scp_config(options, cfg); s != SIAARA_OK) return s;
        auto sv = std::make_shared<ScpServices>();
        if (siaara_status s = read_services(services, cfg, *sv, cfg); s != SIAARA_OK) return s;
        // The MPPS and UPS models, bounded (R-109, R-119), persistent when a state folder is given.
        if (sv->mpps) {
            sv->mpps_store = std::make_shared<net::MppsStore>(sv->mpps_limits);
            if (!sv->state_folder.empty())
                if (Status s = sv->mpps_store->set_state_folder(capi::dimse::join_path(sv->state_folder, "mpps")); !s)
                    return fail(s.code, "state_folder (MPPS): " + s.message);
            if (sv->mpps_store->skipped_files())
                capi::log(SIAARA_LOG_WARNING, false,
                          "SCP: " + std::to_string(sv->mpps_store->skipped_files()) + " MPPS state files could not be loaded");
        }
        if (sv->ups) {
            sv->ups_provider = std::make_shared<net::UpsProvider>(sv->ups_limits);
            if (!sv->state_folder.empty())
                if (Status s = sv->ups_provider->set_state_folder(capi::dimse::join_path(sv->state_folder, "ups")); !s)
                    return fail(s.code, "state_folder (UPS): " + s.message);
            if (sv->ups_provider->skipped_files())
                capi::log(SIAARA_LOG_WARNING, false,
                          "SCP: " + std::to_string(sv->ups_provider->skipped_files()) + " UPS state files could not be loaded");
        }
        warn_open_services(*sv, cfg);
        auto h = std::make_unique<siaara_scp>();
        h->attach_store(store);
        h->policy = options->duplicate_policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep;
        h->cb = cb;
        h->user = user;
        capi::dimse::scp_handle_options(options, *h);
        sv->scp = h.get();
        h->services = sv;
        h->start_dispatcher();
        net::ScpHandlers hd;
        auto ss = std::make_shared<net::ServiceSet>();
        install_handlers(h.get(), *sv, hd, *ss);
        hd.services = ss;
        h->server = std::make_unique<net::ScpServer>(std::move(cfg), std::move(hd));
        if (Status s = h->server->start(); !s) return fail(s);
        capi::log(SIAARA_LOG_INFO, false, "SCP listening on port " + std::to_string(h->server->port()));
        *out = h.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_set_worklist(siaara_scp* scp, const siaara_dataset* const* items,
                                                             uint32_t count) {
    return guarded([&]() -> siaara_status {
        if (!scp || !scp->services) return fail(Errc::InvalidArgument, "scp must be started with siaara_scp_start_ex");
        if (count && !items) return fail(Errc::InvalidArgument, "items is NULL");
        auto v = std::make_shared<std::vector<Dataset>>();
        v->reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!items[i]) return fail(Errc::InvalidArgument, "items entry is NULL");
            v->push_back(capi::identifier_utf8(*ds_cast(items[i])));
        }
        const std::lock_guard<std::mutex> l(scp->services->wl_mu);
        scp->services->wl_items = std::move(v);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_set_worklist_json(siaara_scp* scp, const char* json, size_t len) {
    return guarded([&]() -> siaara_status {
        if (!scp || !scp->services) return fail(Errc::InvalidArgument, "scp must be started with siaara_scp_start_ex");
        if (!json && len) return fail(Errc::InvalidArgument, "json is NULL");
        auto parsed = web::datasets_from_json(std::string_view(json ? json : "", len));
        if (!parsed) return fail(parsed.error());
        auto v = std::make_shared<std::vector<Dataset>>();
        v->reserve(parsed->size());
        for (const Dataset& d : *parsed) v->push_back(capi::identifier_utf8(d));
        const std::lock_guard<std::mutex> l(scp->services->wl_mu);
        scp->services->wl_items = std::move(v);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_mpps_get(const siaara_scp* scp, const char* sop_instance_uid,
                                                         siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!scp || !scp->services || !scp->services->mpps_store)
            return fail(Errc::InvalidArgument, "scp must run the MPPS service (siaara_scp_start_ex)");
        if (!sop_instance_uid) return fail(Errc::InvalidArgument, "sop_instance_uid must not be NULL");
        auto d = scp->services->mpps_store->get(sop_instance_uid);
        if (!d) return fail(Errc::NotFound, "no such MPPS instance");
        auto f = std::make_unique<siaara_file>();
        f->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        f->file.dataset = std::move(*d);
        *out = f.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_ups_create(siaara_scp* scp, const char* uid,
                                                           const siaara_dataset* attributes, char* buf, size_t cap,
                                                           size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!scp || !scp->services || !scp->services->ups_provider)
            return fail(Errc::InvalidArgument, "scp must run the UPS service (siaara_scp_start_ex)");
        if (!attributes) return fail(Errc::InvalidArgument, "attributes must not be NULL");
        std::string u = uid ? std::string(uid) : net::make_uid();
        if (!uid_ok(u)) return fail(Errc::InvalidArgument, "uid is not a valid UID");
        const std::uint16_t s = scp->services->ups_provider->create(u, *ds_cast(attributes));
        if (net::classify_status(s) == net::StatusClass::Failure)
            return fail(Errc::Rejected, "UPS create refused with status " + capi::dimse::hex4(s));
        return copy_out(u, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_ups_get(const siaara_scp* scp, const char* uid, siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!scp || !scp->services || !scp->services->ups_provider)
            return fail(Errc::InvalidArgument, "scp must run the UPS service (siaara_scp_start_ex)");
        if (!uid) return fail(Errc::InvalidArgument, "uid must not be NULL");
        Dataset d;
        const std::uint16_t s = scp->services->ups_provider->get(uid, std::nullopt, d);
        if (s != kSuccess) return fail(Errc::NotFound, "no such UPS workitem");
        auto f = std::make_unique<siaara_file>();
        f->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        f->file.dataset = std::move(d);
        *out = f.release();
        return ok();
    });
}

// ---------------------------------------------------------------- SCU additions
SIAARA_API siaara_status SIAARA_CALL siaara_qr_identifier(int32_t level, const siaara_qr_keys* keys,
                                                          siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (level < SIAARA_LEVEL_PATIENT || level > SIAARA_LEVEL_INSTANCE) return fail(Errc::InvalidArgument, "unknown level");
        if (keys && keys->struct_size < sizeof(siaara_qr_keys))
            return fail(Errc::InvalidArgument, "keys must be set up with struct_size");
        if (keys && keys->return_keys != 0 && keys->return_keys != 1)
            return fail(Errc::InvalidArgument, "return_keys must be 0 or 1");
        capi::QrKeyValues v;
        if (keys) {
            v.return_keys = keys->return_keys != 0;
            const auto put = [&](Tag t, const char* s) {
                if (s && s[0]) v.values.emplace_back(t, std::string(s));
            };
            put(0x00100020, keys->patient_id);
            put(0x00100010, keys->patient_name);
            put(0x00100030, keys->patient_birth_date);
            put(0x00100040, keys->patient_sex);
            put(0x0020000D, keys->study_instance_uid);
            put(0x00080020, keys->study_date);
            put(0x00080030, keys->study_time);
            put(0x00080050, keys->accession_number);
            put(0x00200010, keys->study_id);
            put(0x00081030, keys->study_description);
            put(0x00080090, keys->referring_physician);
            put(0x00080061, keys->modalities_in_study);
            put(0x0020000E, keys->series_instance_uid);
            put(0x00080060, keys->modality);
            put(0x00200011, keys->series_number);
            put(0x0008103E, keys->series_description);
            put(0x00080018, keys->sop_instance_uid);
            put(0x00080016, keys->sop_class_uid);
            put(0x00200013, keys->instance_number);
        }
        auto f = std::make_unique<siaara_file>();
        if (Status st = capi::make_qr_identifier(level, v, f->file.dataset); !st) return fail(st);
        f->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        *out = f.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_clone(const siaara_dataset* ds, siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        auto f = std::make_unique<siaara_file>();
        f->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        f->file.dataset = *ds_cast(ds);
        *out = f.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_find_ex(const siaara_dimse_peer* peer, int32_t model,
                                                          const siaara_dataset* identifier, siaara_qr_options* options,
                                                          siaara_cancel* cancel, siaara_find_callback cb, void* user,
                                                          siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        return capi::dimse::find_impl(peer, model, identifier, options, cancel, cb, user, result);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_move_ex(const siaara_dimse_peer* peer, int32_t model,
                                                          const siaara_dataset* identifier, const char* destination_ae,
                                                          siaara_qr_options* options, siaara_cancel* cancel,
                                                          siaara_dimse_progress_callback cb, void* user,
                                                          siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        return capi::dimse::move_impl(peer, model, identifier, destination_ae, options, cancel, cb, user, result);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_get_ex(const siaara_dimse_peer* peer, int32_t model,
                                                         const siaara_dataset* identifier,
                                                         const siaara_get_options* get_options,
                                                         siaara_qr_options* options, siaara_cancel* cancel,
                                                         siaara_dimse_progress_callback cb, void* user,
                                                         siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        return capi::dimse::get_impl(peer, model, identifier, get_options, options, cancel, cb, user, result);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_files_ex(const siaara_dimse_peer* peer, const char* const* paths,
                                                                 const size_t* lens, uint32_t count,
                                                                 siaara_cancel* cancel, siaara_dimse_progress_callback cb,
                                                                 siaara_uid_change_callback uid_cb, void* user,
                                                                 siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        return capi::dimse::store_files_impl(peer, paths, lens, count, cancel, cb, uid_cb, user, result);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_ex(const siaara_dimse_peer* peer, const siaara_file* file,
                                                           siaara_cancel* cancel, siaara_uid_change_callback uid_cb,
                                                           void* user, siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status { return capi::dimse::store_impl(peer, file, cancel, uid_cb, user, result); });
}

}  // extern "C"

namespace {

constexpr std::uint16_t cmd_n_event_report_rq() { return 0x0100; }

// One N-request on its own association (MPPS, UPS, IAN, generic).
struct NCall {
    std::string abstract_syntax;
    net::Command command;
    const Dataset* dataset = nullptr;
    bool propose_scp_role = false;
};

// Sends the request on the context negotiated for call.abstract_syntax (which may be a Meta SOP
// Class covering the command's SOP class) and waits for its response.
siaara::Result<net::Scu::NResponse> n_request_on(net::Scu& scu, const NCall& call) {
    net::Association* a = scu.association();
    if (!a || !a->established()) return Status(Errc::InvalidArgument, "not connected");
    if (call.abstract_syntax == call.command.sop_class()) return scu.n_request(call.command, call.dataset);
    const net::NegotiatedContext* pc = a->find_context(call.abstract_syntax);
    if (!pc) return Status(Errc::Unsupported, "SOP class not accepted");
    net::Command rq = call.command;
    rq.message_id = a->next_message_id();
    if (Status s = net::validate_command(rq); !s) return s;
    if (Status s = net::send_dimse(*a, pc->id, rq, call.dataset); !s) return s;
    const auto want = static_cast<std::uint16_t>(rq.command_field | 0x8000u);
    for (;;) {
        auto r = net::receive_dimse(*a, a->timeouts().dimse_ms);
        if (!r) {
            if (r.error().code == Errc::Timeout && a->established()) a->abort();
            return r.take_error();
        }
        if (r->kind == net::DimseKind::PeerReleaseRequest) {
            (void)a->respond_release();
            return Status(Errc::NetworkError, "peer released the association while a response was due");
        }
        const net::Message& m = r->message;
        if (m.command.command_field == cmd_n_event_report_rq() && m.command.message_id) {
            // An event report (e.g. a printer status) before our response: acknowledge it.
            if (Status s = net::send_dimse(*a, m.context_id, net::make_response(m.command, kSuccess)); !s) return s;
            continue;
        }
        if (m.command.command_field != want || m.command.message_id_being_responded_to != rq.message_id) {
            a->abort();
            return Status(Errc::Malformed, "unexpected DIMSE message while waiting for a response");
        }
        net::Scu::NResponse out;
        out.command = m.command;
        if (m.has_data) {
            auto d = net::decode_message_dataset(m);
            if (!d) return d.take_error();
            out.dataset = std::move(d.value());
        }
        return out;
    }
}

siaara_status n_call(const siaara_dimse_peer* peer, siaara_cancel* cancel, const NCall& call, siaara_dimse_result* result,
                     net::Scu::NResponse* response) {
    namespace d = capi::dimse;
    if (!d::result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
    d::result_init(result);
    net::ScuConfig c;
    if (siaara_status s = d::peer_config(peer, c); s != SIAARA_OK) return s;
    net::Scu scu(c);
    std::optional<net::RoleSelection> role;
    if (call.propose_scp_role) {
        net::RoleSelection r;
        r.sop_class_uid = call.abstract_syntax;
        r.scu_role = false;
        r.scp_role = true;
        role = r;
    }
    scu.add_context(call.abstract_syntax, net::native_transfer_syntaxes(), role);
    if (siaara_status s = d::connect(scu, cancel, c, result); s != SIAARA_OK) return s;
    siaara_status out = SIAARA_OK;
    bool aborted = false;
    {
        capi::CancelScope scope(cancel, d::interrupter(scu));
        auto r = n_request_on(scu, call);
        if (!r) {
            out = d::op_error(r.error(), scope);
            aborted = true;
        } else {
            const std::uint16_t status = r->command.status.value_or(0xFFFF);
            if (result) result->status = status;
            out = d::dimse_outcome(status, "DIMSE-N request");
            if (response) *response = std::move(r.value());
        }
    }
    d::finish(scu, aborted);
    return out;
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_mpps_create(const siaara_dimse_peer* peer, const char* uid,
                                                              const siaara_dataset* attributes, siaara_cancel* cancel,
                                                              char* buf, size_t cap, size_t* needed,
                                                              siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!attributes) return fail(Errc::InvalidArgument, "attributes must not be NULL");
        const std::string u = uid ? std::string(uid) : net::make_uid();
        if (!uid_ok(u)) return fail(Errc::InvalidArgument, "uid is not a valid UID");
        NCall call;
        call.abstract_syntax = std::string(net::sop::ModalityPerformedProcedureStep);
        call.command = net::make_n_create_rq(0, net::sop::ModalityPerformedProcedureStep, u, true);
        call.dataset = ds_cast(attributes);
        net::Scu::NResponse rsp;
        const siaara_status s = n_call(peer, cancel, call, result, &rsp);
        if (s != SIAARA_OK) return s;
        const std::string& used = rsp.command.affected_sop_instance_uid.empty() ? u : rsp.command.affected_sop_instance_uid;
        if (!buf && cap == 0 && !needed) return ok();
        return copy_out(used, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_mpps_set(const siaara_dimse_peer* peer, const char* uid,
                                                           const siaara_dataset* modifications, siaara_cancel* cancel,
                                                           siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!modifications || !uid || !uid_ok(uid)) return fail(Errc::InvalidArgument, "uid and modifications must be set");
        NCall call;
        call.abstract_syntax = std::string(net::sop::ModalityPerformedProcedureStep);
        call.command = net::make_n_set_rq(0, net::sop::ModalityPerformedProcedureStep, uid);
        call.dataset = ds_cast(modifications);
        return n_call(peer, cancel, call, result, nullptr);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_n_request(const siaara_dimse_peer* peer, const siaara_n_request* rq,
                                                            siaara_cancel* cancel, siaara_file** out, char* buf,
                                                            size_t cap, size_t* needed, siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (out) *out = nullptr;
        if (!rq || rq->struct_size < sizeof(siaara_n_request))
            return fail(Errc::InvalidArgument, "request must be set up with struct_size");
        if (!rq->sop_class_uid || !uid_ok(rq->sop_class_uid)) return fail(Errc::InvalidArgument, "sop_class_uid must be a UID");
        if (rq->abstract_syntax && !uid_ok(rq->abstract_syntax))
            return fail(Errc::InvalidArgument, "abstract_syntax must be a UID");
        if (rq->sop_instance_uid && !uid_ok(rq->sop_instance_uid))
            return fail(Errc::InvalidArgument, "sop_instance_uid must be a UID");
        if (rq->propose_scp_role != 0 && rq->propose_scp_role != 1)
            return fail(Errc::InvalidArgument, "propose_scp_role must be 0 or 1");
        if (rq->type_id > 0xFFFF) return fail(Errc::InvalidArgument, "type_id must be 0..65535");
        if (rq->attribute_count && !rq->attribute_tags) return fail(Errc::InvalidArgument, "attribute_tags is NULL");
        const std::string cls = rq->sop_class_uid;
        const std::string inst = rq->sop_instance_uid ? rq->sop_instance_uid : "";
        const auto type = static_cast<std::uint16_t>(rq->type_id);
        const bool has_ds = rq->dataset != nullptr;
        NCall call;
        call.abstract_syntax = rq->abstract_syntax ? rq->abstract_syntax : cls;
        call.dataset = ds_cast(rq->dataset);
        call.propose_scp_role = rq->propose_scp_role != 0;
        switch (rq->command_field) {
            case SIAARA_N_EVENT_REPORT: call.command = net::make_n_event_report_rq(0, cls, inst, type, has_ds); break;
            case SIAARA_N_GET: {
                std::vector<Tag> tags;
                for (uint32_t i = 0; i < rq->attribute_count; ++i) tags.push_back(rq->attribute_tags[i]);
                call.command = net::make_n_get_rq(0, cls, inst, tags);
                call.dataset = nullptr;
                break;
            }
            case SIAARA_N_SET: call.command = net::make_n_set_rq(0, cls, inst); break;
            case SIAARA_N_ACTION: call.command = net::make_n_action_rq(0, cls, inst, type, has_ds); break;
            case SIAARA_N_CREATE: call.command = net::make_n_create_rq(0, cls, inst, has_ds); break;
            case SIAARA_N_DELETE:
                call.command = net::make_n_delete_rq(0, cls, inst);
                call.dataset = nullptr;
                break;
            default: return fail(Errc::InvalidArgument, "unknown command_field");
        }
        if (rq->command_field != SIAARA_N_CREATE && inst.empty())
            return fail(Errc::InvalidArgument, "sop_instance_uid is required except for N-CREATE");
        if ((rq->command_field == SIAARA_N_SET) && !has_ds) return fail(Errc::InvalidArgument, "N-SET needs a data set");
        call.command.has_dataset = call.dataset != nullptr;
        net::Scu::NResponse rsp;
        const siaara_status s = n_call(peer, cancel, call, result, &rsp);
        if (s != SIAARA_OK && s != SIAARA_ERR_REJECTED) return s;
        if (out && rsp.command.has_dataset) {
            auto f = std::make_unique<siaara_file>();
            f->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
            f->file.dataset = std::move(rsp.dataset);
            *out = f.release();
        }
        const std::string& uid = rsp.command.sop_instance().empty() ? inst : rsp.command.sop_instance();
        if (buf || cap || needed) {
            const siaara_status c = copy_out(uid, buf, cap, needed);
            if (c != SIAARA_OK) return c;
        }
        if (s != SIAARA_OK) {
            const std::uint16_t code = result ? static_cast<std::uint16_t>(result->status) : 0xFFFF;
            return fail(Errc::Rejected, "DIMSE-N request failed with status " + capi::dimse::hex4(code));
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_commit_ex(const siaara_dimse_peer* peer,
                                                            const siaara_commit_options* options,
                                                            const char* const* sop_classes,
                                                            const char* const* sop_instances, uint32_t count,
                                                            siaara_cancel* cancel, uint16_t* failure_reasons,
                                                            siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        namespace d = capi::dimse;
        if (!d::result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        d::result_init(result);
        if (options && options->struct_size < sizeof(siaara_commit_options))
            return fail(Errc::InvalidArgument, "options must be set up with struct_size");
        if (count == 0 || !sop_classes || !sop_instances) return fail(Errc::InvalidArgument, "no instances");
        const int32_t wait_ms = options && options->wait_ms ? options->wait_ms : 60000;
        if (wait_ms < 0) return fail(Errc::InvalidArgument, "wait_ms must not be negative");
        siaara_scp* receiver = options ? options->receiver : nullptr;
        ScpServices* rs = receiver ? receiver->services.get() : nullptr;
        if (receiver && (!rs || !rs->commitment_reports))
            return fail(Errc::InvalidArgument, "receiver must be an SCP started with commitment_reports = 1");
        const int32_t same_ms = !receiver ? wait_ms
                                : options->same_association_ms < 0 ? 0
                                : options->same_association_ms == 0 ? 2000
                                                                     : options->same_association_ms;
        std::vector<net::SopRef> items;
        items.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!sop_classes[i] || !sop_instances[i]) return fail(Errc::InvalidArgument, "UID is NULL");
            items.push_back({sop_classes[i], sop_instances[i]});
        }
        if (failure_reasons)
            for (uint32_t i = 0; i < count; ++i) failure_reasons[i] = 0xFFFF;
        std::string txn = options && options->transaction_uid ? std::string(options->transaction_uid) : net::make_uid();
        if (!uid_ok(txn)) return fail(Errc::InvalidArgument, "transaction_uid is not a valid UID");
        if (options && options->transaction_uid_out) std::memcpy(options->transaction_uid_out, txn.c_str(), txn.size() + 1);
        net::ScuConfig c;
        if (siaara_status s = d::peer_config(peer, c); s != SIAARA_OK) return s;
        // Register before the request, so a report racing the N-ACTION-RSP is not missed.
        struct Pending {
            ScpServices* rs;
            std::string txn;
            ~Pending() {
                if (!rs) return;
                const std::lock_guard<std::mutex> l(rs->cr_mu);
                rs->pending.erase(txn);
            }
        } pending{rs, txn};
        if (rs) {
            const std::lock_guard<std::mutex> l(rs->cr_mu);
            if (rs->pending.count(txn)) return fail(Errc::InvalidArgument, "transaction_uid is already awaited");
            ScpServices::PendingCommit& pc = rs->pending[txn];
            pc.expected_ae = trim_ae(c.called_ae);  // R-118: the commitment SCP the request goes to
            for (const net::SopRef& x : items) pc.items.insert({x.sop_class, x.sop_instance});
        }
        const auto taken = [&]() -> std::optional<net::CommitmentResult> {
            if (!rs) return std::nullopt;
            const std::lock_guard<std::mutex> l(rs->cr_mu);
            const auto it = rs->pending.find(txn);
            return it == rs->pending.end() ? std::nullopt : it->second.result;
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        net::Scu scu(c);
        scu.add_context(std::string(net::sop::StorageCommitmentPushModel), net::native_transfer_syntaxes());
        if (siaara_status s = d::connect(scu, cancel, c, result); s != SIAARA_OK) return s;
        std::optional<net::CommitmentResult> report;
        siaara_status out = SIAARA_OK;
        bool aborted = false, gone = false;
        {
            capi::CancelScope scope(cancel, d::interrupter(scu));
            auto sent = net::request_commitment(scu, items, txn);
            if (!sent) {
                out = d::op_error(sent.error(), scope);
                aborted = true;
            } else {
                // The same association first (up to same_ms), watching the receiver meanwhile.
                const auto same_end = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(same_ms));
                while (!report && !scope.cancelled() && std::chrono::steady_clock::now() < same_end) {
                    report = taken();
                    if (report) break;
                    net::Association* a = scu.association();
                    if (!a || !a->established()) {
                        gone = true;
                        break;
                    }
                    if (!a->data_pending(100)) continue;
                    auto r = net::wait_commitment_result(scu, c.timeouts.dimse_ms);
                    if (r) {
                        report = std::move(r.value());
                    } else {
                        gone = true;  // released, aborted or something unexpected
                        break;
                    }
                }
                if (scope.cancelled()) {
                    out = fail(Errc::Cancelled, "cancelled");
                    aborted = true;
                }
            }
        }
        if (!aborted) d::finish(scu, gone && !scu.connected());
        else d::finish(scu, true);
        if (out != SIAARA_OK) return out;
        // Then the new association, at the receiver.
        if (!report && rs) {
            std::unique_lock<std::mutex> l(rs->cr_mu);
            while (!report) {
                if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
                const auto it = rs->pending.find(txn);
                if (it != rs->pending.end() && it->second.result) {
                    report = it->second.result;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) break;
                rs->cr_cv.wait_for(l, std::chrono::milliseconds(100));
            }
        }
        if (!report) return fail(Errc::Timeout, "no storage commitment report arrived in time");
        std::uint32_t committed = 0, failed = 0;
        for (uint32_t i = 0; i < count; ++i) {
            std::uint16_t reason = 0xFFFF;
            for (const auto& x : report->committed)
                if (x == items[i]) reason = 0;
            for (const auto& f : report->failed)
                if (f.ref == items[i]) reason = f.reason;
            if (reason == 0) ++committed;
            else ++failed;
            if (failure_reasons) failure_reasons[i] = reason;
        }
        if (result) {
            result->completed = committed;
            result->failed = failed;
            result->status = report->event_type();
        }
        return failed ? fail(Errc::Rejected, std::to_string(failed) + " instances were not committed") : ok();
    });
}

}  // extern "C"
