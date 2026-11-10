// SIAARA DICOM Toolkit - internals shared by the networking C API translation units
// (capi_net.cpp: SCU calls and the Storage SCP; capi_net2.cpp: the full service set, the SCU
// additions and the DICOMweb server). Not installed.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capi/capi_internal.hpp"
#include "core/dataset.hpp"
#include "core/incomplete_policy.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "store/store.hpp"

struct siaara_scp;

namespace siaara::capi::dimse {

std::string hex4(unsigned v);
void result_init(siaara_dimse_result* r);
bool result_ok(const siaara_dimse_result* r);
const std::vector<std::string>& compressed_syntaxes();
// AE title rules of the C API: 1 to 16 printable ASCII characters, not all spaces, no backslash.
bool ae_ok(const std::string& ae);
siaara_status peer_config(const siaara_dimse_peer* p, net::ScuConfig& c);
// Connects; maps an association rejection to a readable message.
siaara_status connect(net::Scu& scu, siaara_cancel* cancel, const net::ScuConfig& c,
                      siaara_dimse_result* result = nullptr);
void finish(net::Scu& scu, bool aborted);
std::function<void()> interrupter(net::Scu& scu);
siaara_status op_error(const Status& s, const CancelScope& scope);
siaara_status dimse_outcome(std::uint16_t status, const char* what);
std::string_view model_find(std::int32_t m);
std::string_view model_move(std::int32_t m);
std::string_view model_get(std::int32_t m);
Bytes bytes_of(std::vector<std::uint8_t>&& v);
bool uid_filename_ok(const std::string& s);
std::string join_path(const std::string& dir, const std::string& name);
// Adds a received instance to a store and / or folder. Returns the DIMSE status to answer.
std::uint16_t keep_instance(const net::StoreRequest& rq, siaara_store* store, store::DuplicatePolicy policy,
                            const std::string& folder, std::int32_t* outcome, std::string& error,
                            bool* kept_existing = nullptr);
void fill_progress(siaara_dimse_result& p, const net::RetrieveProgress& r);
uint32_t accepted_qr_flags(const net::Association& a, std::string_view sop_class, bool is_find);
// The DIMSE SCU calls with the round-2 extras (extended negotiation, UID change reports).
siaara_status find_impl(const siaara_dimse_peer* peer, int32_t model, const siaara_dataset* identifier,
                        siaara_qr_options* qo, siaara_cancel* cancel, siaara_find_callback cb, void* user,
                        siaara_dimse_result* result);
siaara_status move_impl(const siaara_dimse_peer* peer, int32_t model, const siaara_dataset* identifier,
                        const char* destination_ae, siaara_qr_options* qo, siaara_cancel* cancel,
                        siaara_dimse_progress_callback cb, void* user, siaara_dimse_result* result);
siaara_status get_impl(const siaara_dimse_peer* peer, int32_t model, const siaara_dataset* identifier,
                       const siaara_get_options* options, siaara_qr_options* qo, siaara_cancel* cancel,
                       siaara_dimse_progress_callback cb, void* user, siaara_dimse_result* result);
siaara_status store_files_impl(const siaara_dimse_peer* peer, const char* const* paths, const size_t* lens,
                               uint32_t count, siaara_cancel* cancel, siaara_dimse_progress_callback cb,
                               siaara_uid_change_callback uid_cb, void* user, siaara_dimse_result* result,
                               siaara_store_outcome* outcomes = nullptr);
siaara_status store_impl(const siaara_dimse_peer* peer, const siaara_file* file, siaara_cancel* cancel,
                         siaara_uid_change_callback uid_cb, void* user, siaara_dimse_result* result,
                         siaara_store_outcome* outcome = nullptr);
// siaara_scp_options -> ScpConfig (validated).
siaara_status scp_config(const siaara_scp_options* o, net::ScpConfig& cfg);
// The siaara_scp_options fields that belong to the handle (event queue limits).
void scp_handle_options(const siaara_scp_options* o, siaara_scp& h);
// The Verification and Storage handlers of siaara_scp_start (events, counters, the store).
void storage_handlers(siaara_scp* raw, net::ScpHandlers& hd);

}  // namespace siaara::capi::dimse

namespace siaara::capi {
struct ScpServices;  // capi_net2.cpp: the services of siaara_scp_start_ex

// SIAARA_INCOMPLETE_* -> IncompletePolicy (SIAARA_INCOMPLETE_DEFAULT: kDefaultIncompletePolicy).
inline siaara_status incomplete_policy(std::int32_t v, IncompletePolicy& out) {
    switch (v) {
        case SIAARA_INCOMPLETE_DEFAULT: out = kDefaultIncompletePolicy; return SIAARA_OK;
        case SIAARA_INCOMPLETE_REFUSE: out = IncompletePolicy::Refuse; return SIAARA_OK;
        case SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED: out = IncompletePolicy::AcceptAsReceived; return SIAARA_OK;
        case SIAARA_INCOMPLETE_QUARANTINE: out = IncompletePolicy::Quarantine; return SIAARA_OK;
        default: return set_error(Errc::InvalidArgument, "unknown incomplete_policy");
    }
}
inline siaara_status quarantine_response(std::int32_t v, QuarantineResponse& out) {
    switch (v) {
        case SIAARA_QUARANTINE_ANSWER_DEFAULT: out = kDefaultQuarantineResponse; return SIAARA_OK;
        case SIAARA_QUARANTINE_ANSWER_WARNING: out = QuarantineResponse::Warning; return SIAARA_OK;
        case SIAARA_QUARANTINE_ANSWER_FAILURE: out = QuarantineResponse::Failure; return SIAARA_OK;
        default: return set_error(Errc::InvalidArgument, "unknown quarantine_response");
    }
}
// Keeps a received instance (Part 10 bytes exactly as they will be released) in the quarantine
// of `store`. Returns 0, 0xA700 when the quarantine is full, 0x0110 otherwise.
std::uint16_t quarantine_bytes(siaara_store* store, const Bytes& bytes, const char* source, const std::string& calling_ae,
                               const std::string& peer, const std::string& sop_class, const std::string& sop_instance,
                               std::uint32_t read_loss, const std::string& warning);
// DicomFile::ReadLoss bits -> SIAARA_READ_* bits.
inline std::uint32_t read_loss_bits(std::uint32_t loss) noexcept {
    return (loss & DicomFile::kReadBytesSkipped ? static_cast<std::uint32_t>(SIAARA_READ_BYTES_SKIPPED) : 0u) |
           (loss & DicomFile::kReadInputTruncated ? static_cast<std::uint32_t>(SIAARA_READ_INPUT_TRUNCATED) : 0u) |
           (loss & DicomFile::kReadHeaderOnly ? static_cast<std::uint32_t>(SIAARA_READ_HEADER_ONLY) : 0u) |
           (loss & DicomFile::kReadRecovered ? static_cast<std::uint32_t>(SIAARA_READ_RECOVERED) : 0u);
}
}  // namespace siaara::capi

// ---------------------------------------------------------------- the SCP handle
// Events go through a bounded queue to one dispatcher thread, which calls the user's callback
// with no toolkit lock held (R-070): a slow callback delays only the event queue, and the
// associations only once the queue is full (back-pressure). An association waits at most 1 s for
// room; then the event is dropped, counted (siaara_scp_stats.events_dropped) and logged once.
// Once stopping, nothing waits: events that do not fit are dropped at once (R-094).
struct siaara_scp {
    struct Event {
        std::int32_t kind = 0;
        std::uint16_t status = 0;
        std::int32_t outcome = 0;
        std::int32_t uid_mismatch = 0;
        std::string calling, cls, inst, addr, cmd_cls, cmd_inst;
        // round 2 (siaara_scp_event fields after command_sop_instance_uid)
        std::string original_inst, transaction_uid, destination;
        std::optional<siaara::Dataset> dataset;
        std::uint32_t completed = 0, failed = 0, warning = 0;
        std::uint32_t command_field = 0;
        // round 4 (siaara_scp_event fields after command_field)
        std::uint64_t association_id = 0;  // 0: taken from the posting thread's association
        std::uint32_t read_loss = 0;       // SIAARA_READ_* bits
        std::uint64_t bytes = 0;           // of `dataset`, charged to the queue's byte budget
    };
    static constexpr std::size_t kDefaultMaxQueued = 1024;
    static constexpr std::uint64_t kDefaultMaxQueuedBytes = 64ull << 20;
    static constexpr std::chrono::milliseconds kMaxWait{1000};

    std::unique_ptr<siaara::net::ScpServer> server;
    siaara_store* store = nullptr;
    siaara::store::DuplicatePolicy policy = siaara::store::DuplicatePolicy::Keep;
    siaara_scp_callback cb = nullptr;
    void* user = nullptr;
    std::size_t max_queued = kDefaultMaxQueued;
    std::uint64_t max_queued_bytes = kDefaultMaxQueuedBytes;  // R-109
    std::uint64_t queued_bytes = 0;     // under q_mu
    std::mutex q_mu;
    std::condition_variable q_cv;       // events queued / dispatcher stopping
    std::condition_variable space_cv;   // room in the queue / stopping
    std::deque<Event> queue;
    bool closing = false;               // under q_mu: the dispatcher ends once the queue is empty
    bool stopping = false;              // under q_mu: producers no longer wait for room (R-094)
    std::thread dispatcher;
    std::atomic<std::uint64_t> stored{0}, failures{0}, echoes{0}, bytes{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool> drop_logged{false};
    std::atomic<bool> stop_requested{false};  // set by begin_stop(): waits on application callbacks end (R-130)
    // Counters of the round-2 services (siaara_scp_stats fields after events_dropped).
    std::atomic<std::uint64_t> finds{0}, retrieves{0}, sub_operations{0}, mpps{0}, commitments{0}, refused{0};
    std::shared_ptr<siaara::capi::ScpServices> services;
    siaara::capi::LiveHandle live_{};

    void start_dispatcher() {
        if (cb) dispatcher = std::thread([this] { dispatch_loop(); });
    }
    bool on_dispatcher() const noexcept {
        return dispatcher.joinable() && dispatcher.get_id() == std::this_thread::get_id();
    }
    // Producers stop waiting for room (R-094): called before the server is stopped, so an
    // association blocked on a full queue (a callback that does not return) cannot keep
    // ScpServer::stop() from joining its thread.
    void begin_stop() {
        stop_requested.store(true);
        {
            const std::lock_guard<std::mutex> l(q_mu);
            stopping = true;
        }
        space_cv.notify_all();
    }
    void stop_server() {
        begin_stop();
        if (server) server->stop();
    }

    // Stops the server (joins its threads, so no new events), then delivers what is queued and
    // ends the dispatcher, before the members its handlers use go away.
    ~siaara_scp() {
        stop_server();
        {
            const std::lock_guard<std::mutex> l(q_mu);
            closing = true;
        }
        q_cv.notify_all();
        space_cv.notify_all();
        if (dispatcher.joinable()) dispatcher.join();
        siaara::capi::store_detach(store);  // R-150: may perform a deferred siaara_store_free
    }
    // R-150: the store stays alive until this SCP is freed.
    void attach_store(siaara_store* s) {
        store = s;
        siaara::capi::store_attach(s);
    }

    void dispatch_loop();
    void post(Event ev);
    void event(std::int32_t kind, std::uint16_t status, std::int32_t outcome, const std::string& calling,
               const std::string& cls, const std::string& inst, const std::string& addr,
               const siaara::net::StoreRequest* rq = nullptr);
};
