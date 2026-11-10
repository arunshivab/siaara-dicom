// SIAARA DICOM Toolkit - DIMSE service classes beyond Verification, Storage and Query/Retrieve:
//   - Storage Commitment Push Model (PS3.4 Annex J): N-ACTION request, N-EVENT-REPORT result on the
//     same association or on a new one opened by the SCP (SCP/SCU role selection, PS3.7 D.3.3.4)
//   - Modality Performed Procedure Step (PS3.4 F.7): N-CREATE / N-SET
//   - Modality Worklist (PS3.4 K.6): C-FIND with the matching rules of PS3.4 C.2.2.2
//   - Unified Procedure Step (PS3.4 Annex CC): Push / Pull / Watch / Event / Query
//   - Instance Availability Notification (PS3.4 Annex R): N-CREATE
//   - Basic Grayscale / Color Print Management (PS3.4 Annex H), SCU side, over the Meta SOP Classes
//   - Query/Retrieve extensions (PS3.4 C.5, C.6): SOP Class Extended Negotiation (relational queries,
//     combined date-time matching, fuzzy semantic matching of person names, relational retrieval),
//     Composite Instance Root Retrieve, Composite Instance Retrieve Without Bulk Data
//   - asynchronous C-STORE (PS3.7 D.3.3.3 Asynchronous Operations Window)
// Each service has SCU helpers (functions over a connected Scu) and SCP handlers collected in a
// ServiceSet that is plugged into ScpServer through ScpHandlers::services.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/association.hpp"
#include "net/dimse.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"

namespace siaara::net {

// SOP class and well-known instance UIDs (PS3.6 Annex A).
namespace sop {
constexpr std::string_view StorageCommitmentPushModelInstance = "1.2.840.10008.1.20.1.1";
constexpr std::string_view UpsPush = "1.2.840.10008.5.1.4.34.6.1";
constexpr std::string_view UpsWatch = "1.2.840.10008.5.1.4.34.6.2";
constexpr std::string_view UpsPull = "1.2.840.10008.5.1.4.34.6.3";
constexpr std::string_view UpsEvent = "1.2.840.10008.5.1.4.34.6.4";
constexpr std::string_view UpsQuery = "1.2.840.10008.5.1.4.34.6.5";
constexpr std::string_view UpsGlobalSubscriptionInstance = "1.2.840.10008.5.1.4.34.5";
constexpr std::string_view UpsFilteredGlobalSubscriptionInstance = "1.2.840.10008.5.1.4.34.5.1";
constexpr std::string_view InstanceAvailabilityNotification = "1.2.840.10008.5.1.4.33";
constexpr std::string_view BasicGrayscalePrintManagementMeta = "1.2.840.10008.5.1.1.9";
constexpr std::string_view BasicColorPrintManagementMeta = "1.2.840.10008.5.1.1.18";
constexpr std::string_view BasicFilmSession = "1.2.840.10008.5.1.1.1";
constexpr std::string_view BasicFilmBox = "1.2.840.10008.5.1.1.2";
constexpr std::string_view BasicGrayscaleImageBox = "1.2.840.10008.5.1.1.4";
constexpr std::string_view BasicColorImageBox = "1.2.840.10008.5.1.1.4.1";
constexpr std::string_view Printer = "1.2.840.10008.5.1.1.16";
constexpr std::string_view PrinterInstance = "1.2.840.10008.5.1.1.17";
constexpr std::string_view CompositeInstanceRootMove = "1.2.840.10008.5.1.4.1.2.4.2";
constexpr std::string_view CompositeInstanceRootGet = "1.2.840.10008.5.1.4.1.2.4.3";
constexpr std::string_view CompositeInstanceRetrieveWithoutBulkDataGet = "1.2.840.10008.5.1.4.1.2.5.3";
}  // namespace sop

// Service-specific status codes (PS3.4).
namespace dimse_status {
constexpr std::uint16_t InvalidAttributeValue = 0x0106;
constexpr std::uint16_t DuplicateSopInstance = 0x0111;
constexpr std::uint16_t NoSuchAttribute = 0x0105;
constexpr std::uint16_t MissingAttribute = 0x0120;
constexpr std::uint16_t UpsCreatedWithModifications = 0xB300;
constexpr std::uint16_t UpsDeletionLockNotGranted = 0xB301;
constexpr std::uint16_t UpsAlreadyCanceled = 0xB304;
constexpr std::uint16_t UpsCoerced = 0xB305;
constexpr std::uint16_t UpsAlreadyCompleted = 0xB306;
constexpr std::uint16_t UpsMayNoLongerBeUpdated = 0xC300;
constexpr std::uint16_t UpsWrongTransactionUid = 0xC301;
constexpr std::uint16_t UpsAlreadyInProgress = 0xC302;
constexpr std::uint16_t UpsScheduledOnlyByCreate = 0xC303;
constexpr std::uint16_t UpsFinalStateNotMet = 0xC304;
constexpr std::uint16_t UpsNoSuchInstance = 0xC307;
constexpr std::uint16_t UpsUnknownReceivingAe = 0xC308;
constexpr std::uint16_t UpsCreateStateNotScheduled = 0xC309;
constexpr std::uint16_t UpsNotInProgress = 0xC310;
constexpr std::uint16_t UpsCancelAlreadyCompleted = 0xC311;
constexpr std::uint16_t UpsPerformerCannotBeContacted = 0xC312;
constexpr std::uint16_t UpsPerformerChoosesNotToCancel = 0xC313;
constexpr std::uint16_t UpsActionNotAppropriate = 0xC314;
constexpr std::uint16_t UpsEventsNotSupported = 0xC315;
}  // namespace dimse_status

// A new UID under the 2.25 root (PS3.5 B.2: derived from a 128-bit random number).
std::string make_uid();

// Does a presentation context negotiated for `abstract_syntax` carry messages of `sop_class`?
// True for the same class, for the print SOP classes inside a Print Management Meta SOP Class, and
// for the UPS Push SOP Class on a context negotiated for any UPS SOP class (PS3.4 CC.2).
bool context_covers(std::string_view abstract_syntax, std::string_view sop_class) noexcept;

// ===================================================================== query matching (C.2.2.2)

struct MatchOptions {
    bool fuzzy_person_names = false;  // case-insensitive person names (fuzzy semantic matching)
    bool combined_datetime = false;   // date and time keys matched as one combined value
};

// Does `candidate` match every key of `identifier`? Supports single value, list of UIDs, universal,
// wild card (* ?), range (DA, TM, DT) and sequence matching. Specific Character Set, Query/Retrieve
// Level and group lengths are not matching keys.
bool query_matches(const Dataset& identifier, const Dataset& candidate, const MatchOptions& opt = {});
// The response for a match: every key of the identifier, with the candidate's value (or empty).
Dataset query_response(const Dataset& identifier, const Dataset& candidate);

// SOP Class Extended Negotiation for Query/Retrieve (PS3.4 C.5.1.1.1, C.5.2.1.1, C.5.3.1.1).
struct QueryFlags {
    bool relational = false;           // relational queries / relational retrieval
    bool combined_datetime = false;    // C-FIND only
    bool fuzzy_person_names = false;   // C-FIND only
    bool timezone_adjustment = false;  // C-FIND only
};
ExtendedNegotiation find_extended_negotiation(std::string_view sop_class, const QueryFlags& f);
ExtendedNegotiation retrieve_extended_negotiation(std::string_view sop_class, bool relational);
QueryFlags parse_query_flags(const std::vector<std::uint8_t>& info, bool is_find) noexcept;
// What the acceptor agreed to for `sop_class` on an established association (all false when absent).
QueryFlags negotiated_query_flags(const Association& a, std::string_view sop_class, bool is_find);
// Acceptor side: the reply items for the requestor's proposals given what the SCP supports
// (each reply byte is the AND of the proposal and the supported byte).
std::vector<ExtendedNegotiation> answer_extended_negotiation(const std::vector<ExtendedNegotiation>& proposed,
                                                             const std::vector<ExtendedNegotiation>& supported);
// Hierarchical key checks of PS3.4 C.4.1.2.1 for the Patient/Study Root models: returns Success
// or IdentifierDoesNotMatch. Relational queries/retrievals accept any keys.
std::uint16_t check_query_identifier(std::string_view sop_class, const Dataset& identifier, bool relational,
                                     bool is_retrieve);

// Modality Worklist: an identifier with the usual return keys and the given matching values
// (empty strings are universal matching). `date` may be a range "YYYYMMDD-YYYYMMDD".
Dataset make_worklist_query(std::string_view station_ae, std::string_view modality, std::string_view date,
                            std::string_view patient_name = {}, std::string_view patient_id = {});

// Removes bulk data (Pixel Data, Float/Double Pixel Data, Waveform/Spectroscopy/Overlay data,
// Encapsulated Document...) recursively, for Composite Instance Retrieve Without Bulk Data (PS3.4 Z).
void strip_bulk_data(Dataset& ds);

// ===================================================================== storage commitment (Annex J)

struct SopRef {
    std::string sop_class;
    std::string sop_instance;
    bool operator==(const SopRef&) const = default;
};
struct CommitmentFailure {
    SopRef ref;
    std::uint16_t reason = 0x0110;  // Failure Reason (0008,1197), PS3.3 C.14.1.1
};
struct CommitmentRequest {
    std::string calling_ae;
    std::string transaction_uid;
    std::vector<SopRef> items;
};
struct CommitmentResult {
    std::string transaction_uid;
    std::string retrieve_ae;
    std::vector<SopRef> committed;
    std::vector<CommitmentFailure> failed;
    std::uint16_t event_type() const noexcept { return failed.empty() ? 1 : 2; }
};

Dataset encode_commitment_request(const CommitmentRequest& rq);
Result<CommitmentRequest> parse_commitment_request(const Dataset& ds);
Dataset encode_commitment_result(const CommitmentResult& r);
Result<CommitmentResult> parse_commitment_result(const Dataset& ds);

// SCU: sends the N-ACTION (Request Storage Commitment). Returns the Transaction UID used.
Result<std::string> request_commitment(Scu& scu, const std::vector<SopRef>& items, std::string transaction_uid = {});
// SCU: waits for the N-EVENT-REPORT on the same association and answers it.
Result<CommitmentResult> wait_commitment_result(Scu& scu, int timeout_ms);

// ===================================================================== MPPS (F.7), IAN (R)

Result<Scu::NResponse> mpps_create(Scu& scu, std::string instance_uid, const Dataset& attributes);
Result<Scu::NResponse> mpps_set(Scu& scu, const std::string& instance_uid, const Dataset& modifications);
Result<Scu::NResponse> send_instance_availability(Scu& scu, const Dataset& notification, std::string instance_uid = {});

// Limits of the in-memory N-service models (R-109, R-119). A request that would pass one is
// answered 0xA700 (Refused: Out of Resources); nothing is stored then.
struct ServiceStateLimits {
    std::size_t max_items = 10000;                 // MPPS: IN PROGRESS instances; UPS: workitems not yet final
    std::size_t max_finished = 10000;              // finished instances kept (oldest evicted first)
    std::uint64_t max_bytes = 256ull << 20;        // all data sets held, approximate
    std::size_t max_subscribers_per_item = 16;     // UPS: receiving AEs per workitem (R-117)
    std::size_t max_global_subscribers = 64;       // UPS: global subscribers
    std::size_t max_pending_events = 4096;         // UPS: event reports waiting to be taken
    // Updates (N-SET, state changes) only from the AE that created the instance (R-131); others
    // are answered 0x0124 (Refused: Not Authorized). Off by default: the standard does not
    // require it (PS3.4 F.7.2, CC.2.6) and a second performer may legitimately update a UPS.
    bool bind_to_creator = false;
};

// The approximate memory a data set holds (value bytes plus a fixed cost per element), used for
// the byte budgets above.
std::uint64_t approximate_size(const Dataset& ds) noexcept;

// An MPPS SCP model enforcing PS3.4 F.7.2.2: created IN PROGRESS, modifiable until COMPLETED or
// DISCONTINUED, no duplicates. Only IN PROGRESS instances count against max_items; finished ones
// are kept up to max_finished and the byte budget, the oldest evicted first (R-119).
// With a state folder (set_state_folder) every instance is kept in an owner-only file there and
// reloaded on the next start; without one the model lives in memory only.
class MppsStore {
public:
    MppsStore() = default;
    explicit MppsStore(const ServiceStateLimits& limits) : lim_(limits) {}
    // Persists to `folder` (created owner-only when missing) and loads what it holds. Call before
    // serving. Files that cannot be read are skipped and counted (skipped_files()).
    Status set_state_folder(const std::string& folder);
    std::uint16_t create(const std::string& uid, const Dataset& attributes, const std::string& calling_ae = {});
    std::uint16_t set(const std::string& uid, const Dataset& modifications, const std::string& calling_ae = {});
    std::optional<Dataset> get(const std::string& uid) const;
    std::size_t size() const;
    std::size_t in_progress() const;
    std::uint64_t bytes() const;
    std::size_t skipped_files() const;

private:
    struct Item {
        Dataset ds;
        std::string creator;
        std::uint64_t bytes = 0;
        std::uint64_t finished_seq = 0;  // 0: IN PROGRESS
    };
    bool make_room(std::uint64_t need, const std::string& keep);
    void evict_finished_over_cap();
    Status save(const std::string& uid, const Item& it) const;
    void erase_file(const std::string& uid) const;

    mutable std::mutex mu_;
    ServiceStateLimits lim_;
    std::map<std::string, Item> items_;
    std::map<std::uint64_t, std::string> finished_;  // finished_seq -> uid (oldest first)
    std::uint64_t next_seq_ = 1;
    std::size_t in_progress_ = 0;
    std::uint64_t bytes_ = 0;
    std::string folder_;
    std::size_t skipped_ = 0;
};

// ===================================================================== UPS (Annex CC)

struct UpsEventReport {
    std::string ups_uid;
    std::uint16_t event_type = 1;  // 1 state report, 2 cancel requested, 3 progress, 4 SCP status, 5 assigned
    Dataset info;
};

namespace ups_state {
constexpr std::string_view Scheduled = "SCHEDULED";
constexpr std::string_view InProgress = "IN PROGRESS";
constexpr std::string_view Canceled = "CANCELED";
constexpr std::string_view Completed = "COMPLETED";
}  // namespace ups_state

// A UPS SCP (UPS Push/Pull/Watch/Query provider and UPS Event sender), bounded by
// ServiceStateLimits (R-109): workitems that are not final count against max_items, final ones
// (COMPLETED / CANCELED) are kept up to max_finished and the byte budget, oldest evicted first
// (never while a subscriber holds a deletion lock on them). In memory by default; with a state
// folder every workitem and the global subscriptions are kept in owner-only files and reloaded.
class UpsProvider {
public:
    UpsProvider() = default;
    explicit UpsProvider(const ServiceStateLimits& limits) : lim_(limits) {}
    Status set_state_folder(const std::string& folder);

    std::uint16_t create(const std::string& uid, const Dataset& attributes, const std::string& calling_ae = {});
    std::uint16_t set(const std::string& uid, const Dataset& modifications, const std::string& calling_ae = {});
    std::uint16_t get(const std::string& uid, const std::optional<std::vector<Tag>>& attributes, Dataset& out) const;
    std::uint16_t change_state(const std::string& uid, std::string_view state, const std::string& transaction_uid,
                               const std::string& calling_ae = {});
    std::uint16_t request_cancel(const std::string& uid, const std::string& requesting_ae, const Dataset& info);
    // uid may be one of the global subscription instances.
    std::uint16_t subscribe(const std::string& uid, const std::string& ae, bool deletion_lock);
    std::uint16_t unsubscribe(const std::string& uid, const std::string& ae);
    std::uint16_t suspend_global(const std::string& ae);
    // At most `max_results` matches (0: no cap); *more is set when further ones matched.
    std::vector<Dataset> find(const Dataset& identifier, const MatchOptions& opt = {}, std::size_t max_results = 0,
                              bool* more = nullptr) const;
    std::optional<std::string> state(const std::string& uid) const;
    std::size_t size() const;
    std::uint64_t bytes() const;
    std::size_t skipped_files() const;
    // Event reports produced by the operations so far, per receiving AE (drained).
    std::vector<std::pair<std::string, UpsEventReport>> take_events();
    // Hook to let a performer refuse cancellation requests (default: accept).
    std::function<bool(const std::string& uid)> performer_accepts_cancel;

private:
    struct Item {
        Dataset ds;
        std::string state;
        std::string transaction_uid;
        std::string creator;
        std::set<std::string> subscribers;
        std::set<std::string> locks;  // subscribers holding a deletion lock
        std::uint64_t bytes = 0;
        std::uint64_t finished_seq = 0;
    };
    void queue_state_report(const std::string& uid, const Item& it, const std::set<std::string>& to);
    void queue_event(const std::string& ae, UpsEventReport ev);
    std::set<std::string> watchers(const Item& it) const;
    bool make_room(std::uint64_t need, const std::string& keep);
    void finish(const std::string& uid, Item& it);
    void evict_finished_over_cap();
    Status save(const std::string& uid, const Item& it) const;
    void save_globals() const;
    void erase_file(const std::string& uid) const;

    mutable std::mutex mu_;
    ServiceStateLimits lim_;
    std::map<std::string, Item> items_;
    std::map<std::uint64_t, std::string> finished_;
    std::uint64_t next_seq_ = 1;
    std::size_t open_items_ = 0;
    std::uint64_t bytes_ = 0;
    std::set<std::string> global_;        // global subscribers
    std::set<std::string> suspended_;     // global subscription suspended for new instances
    std::vector<std::pair<std::string, UpsEventReport>> events_;
    std::string folder_;
    std::size_t skipped_ = 0;
};

// SCU helpers. Each uses the presentation context accepted for the given UPS SOP class; the
// Requested / Affected SOP Class UID in the DIMSE-N command is always that of the UPS Push SOP
// Class, as PS3.4 CC.2 requires ("all UPSs are created as instances of the UPS Push SOP Class").
Result<Scu::NResponse> ups_create(Scu& scu, std::string uid, const Dataset& attributes);   // UPS Push
Result<Scu::NResponse> ups_get(Scu& scu, std::string_view sop_class, const std::string& uid,
                               const std::vector<Tag>& attributes = {});
Result<Scu::NResponse> ups_set(Scu& scu, const std::string& uid, const Dataset& modifications);  // UPS Pull
Result<Scu::NResponse> ups_change_state(Scu& scu, const std::string& uid, std::string_view state,
                                        const std::string& transaction_uid);                    // UPS Pull
Result<Scu::NResponse> ups_request_cancel(Scu& scu, std::string_view sop_class, const std::string& uid,
                                          const Dataset& info = {});                            // Push / Watch
Result<Scu::NResponse> ups_subscribe(Scu& scu, const std::string& uid, const std::string& receiving_ae,
                                     bool deletion_lock);                                       // UPS Watch
Result<Scu::NResponse> ups_unsubscribe(Scu& scu, const std::string& uid, const std::string& receiving_ae);
Result<FindResult> ups_find(Scu& scu, std::string_view sop_class, const Dataset& identifier,
                            const std::function<bool(const Dataset&)>& on_result);

// ===================================================================== print management (Annex H)

class PrintScu {
public:
    // `scu` must be connected with the Basic Grayscale (or Color) Print Management Meta SOP Class.
    PrintScu(Scu& scu, bool color);
    Result<Dataset> printer_status();  // N-GET of the Printer SOP Instance
    Result<std::string> create_film_session(const Dataset& attributes);
    struct FilmBox {
        std::string uid;
        std::vector<std::string> image_boxes;
    };
    Result<FilmBox> create_film_box(const Dataset& attributes);  // Referenced Film Session is added
    // `image` holds the Image Pixel attributes (Samples per Pixel ... Pixel Data) of the picture.
    Status set_image_box(const std::string& image_box_uid, std::uint16_t position, const Dataset& image);
    Status print_film_box(const std::string& uid);
    Status print_film_session();
    Status delete_film_box(const std::string& uid);
    Status delete_film_session();
    // Printer N-EVENT-REPORTs seen while waiting for responses (they are answered with Success).
    const std::vector<std::uint16_t>& printer_events() const noexcept { return events_; }
    std::uint16_t last_status() const noexcept { return last_status_; }

private:
    Result<Scu::NResponse> call(Command rq, const Dataset* ds);
    Scu& scu_;
    bool color_;
    std::string session_uid_;
    std::vector<std::uint16_t> events_;
    std::uint16_t last_status_ = 0;
};

// ===================================================================== asynchronous C-STORE

struct StoreOutcome {
    std::string path;
    std::string sop_instance;           // as sent
    std::string original_sop_instance;  // the file's own, when a lossy transcode changed it (R-093)
    std::uint16_t status = 0xFFFF;
    Status error;  // set when the instance could not be sent at all
};
struct StoreManyResult {
    std::vector<StoreOutcome> outcomes;
    std::size_t max_outstanding = 0;  // largest number of requests that were in flight at once
};
// Sends the files with up to `window` outstanding C-STORE requests (bounded by what the peer
// granted in the Asynchronous Operations Window; 1 when nothing was negotiated).
Result<StoreManyResult> store_many(Scu& scu, const std::vector<std::string>& paths, std::uint16_t window = 0);
// The number of operations this requestor may have outstanding on the association.
std::uint16_t async_invoke_window(const Association& a) noexcept;

// ===================================================================== SCP side

enum class CommitmentReportMode {
    SameAssociation,  // N-EVENT-REPORT right after the N-ACTION-RSP on the same association
    NewAssociation,   // a new association to the requestor (resolve_ae) with SCP role selection
    SameThenNew,      // same association; a new one if that fails
};

class EventSender;

struct ServiceSet {
    // Storage Commitment SCP: decides the result of each request.
    std::function<CommitmentResult(const CommitmentRequest&)> on_commitment;
    CommitmentReportMode commitment_report = CommitmentReportMode::SameThenNew;
    // Storage Commitment SCU: results arriving on associations opened by the commitment SCP.
    std::function<std::uint16_t(const std::string& calling_ae, const CommitmentResult&)> on_commitment_report;
    // MPPS SCP (instance UID, attributes / modifications). Default when unset: `mpps` store.
    std::function<std::uint16_t(const std::string& calling_ae, const std::string& uid, const Dataset&)> on_mpps_create;
    std::function<std::uint16_t(const std::string& calling_ae, const std::string& uid, const Dataset&)> on_mpps_set;
    std::shared_ptr<MppsStore> mpps;
    // Instance Availability Notification SCP.
    std::function<std::uint16_t(const std::string& calling_ae, const std::string& uid, const Dataset&)>
        on_instance_availability;
    // UPS SCP and UPS Event receiver.
    std::shared_ptr<UpsProvider> ups;
    std::function<std::uint16_t(const std::string& calling_ae, const UpsEventReport&)> on_ups_event;
    // Where the AE titles live that these services open associations to (commitment reports,
    // UPS event reports).
    std::function<std::optional<MoveDestination>(const std::string& ae)> resolve_ae;
    // Propose SCP/SCU role selection (requestor = SCP) when opening those associations.
    bool propose_role_reversal = true;
    // Admission of every request this set would serve (allow-lists, limits): return Success to
    // serve it, or the status to answer it with (e.g. 0x0124 Refused: Not Authorized).
    std::function<std::uint16_t(const std::string& calling_ae, const Command& rq)> admit;
    // After a UPS request was answered: (calling AE, request, the status sent).
    std::function<void(const std::string& calling_ae, const Command& rq, std::uint16_t status)> on_ups_served;
    // After a Storage Commitment request was decided: the result and whether the report reached
    // the requestor (on its association or a new one).
    std::function<void(const CommitmentRequest& rq, const CommitmentResult& result, bool delivered)> on_commitment_done;
    // The AE an MPPS / UPS update came from is passed to the models when they bind updates to
    // the creator (ServiceStateLimits::bind_to_creator).
    // UPS C-FIND responses per request; more matches end the query with 0xA700 (R-109).
    std::size_t max_ups_find_results = 10000;
    // Event reports that need a new association (Storage Commitment results, UPS events) are sent
    // by one sender thread with a bounded queue and short timeouts (R-117), started and stopped by
    // the ScpServer the set is plugged into; its outbound associations are aborted by stop().
    // Without a server (sender unset) they are sent synchronously, as before.
    std::shared_ptr<EventSender> sender;
    std::size_t max_queued_event_jobs = 256;  // jobs waiting for the sender; more are dropped and logged
    Timeouts event_timeouts = [] {
        Timeouts t;
        t.connect_ms = 5000;
        t.acse_ms = 5000;
        t.artim_ms = 5000;
        t.dimse_ms = 10000;
        t.write_ms = 10000;
        return t;
    }();
};

// One event report to send on a new association (the context is negotiated for
// `abstract_syntax`; the command carries `sop_class`).
struct EventToSend {
    std::string abstract_syntax;
    std::string sop_class;
    std::string instance;
    std::uint16_t event_type = 1;
    Dataset info;
};

// The sender thread of a ServiceSet (R-117).
class EventSender {
public:
    struct Job {
        std::string ae;
        std::vector<EventToSend> events;
        std::function<void(const Status&)> done;  // called on the sender thread (may be empty)
    };
    EventSender(const ServiceSet& set, const ScpConfig& cfg, std::function<void(const std::string&)> log);
    ~EventSender();
    EventSender(const EventSender&) = delete;
    EventSender& operator=(const EventSender&) = delete;
    // False when the queue is full or the sender is stopping (the job is not run; `done` is not called).
    bool post(Job job);
    // Aborts the association in progress, drops what is queued and joins the thread. Idempotent.
    void stop();
    std::size_t queued() const;

private:
    void run();
    const ServiceSet& set_;
    ScpConfig cfg_;
    std::function<void(const std::string&)> log_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    Scu* current_ = nullptr;  // under mu_
    bool stopping_ = false;
    std::thread thread_;
};

// Abstract syntaxes the service set adds to an SCP's presentation context policy.
std::vector<AcceptedSyntax> service_policy(const ServiceSet& s, const std::vector<std::string>& transfer_syntaxes);

struct ServiceContext {
    Association& a;
    const ScpConfig& cfg;
    std::function<void(const std::string&)> log;
    bool release_pending = false;  // set when the peer asked to release while a service waited
    std::uint16_t last_status = 0; // status of the last response sent through the service set
};

enum class ServiceOutcome { NotHandled, Handled, Failed };
// Serves `m` if it belongs to one of the services in `s`.
ServiceOutcome dispatch_service(ServiceSet& s, ServiceContext& c, const Message& m);

}  // namespace siaara::net
