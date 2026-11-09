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

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
// True for the same class and for the print SOP classes inside a Print Management Meta SOP Class.
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

// An MPPS SCP model enforcing PS3.4 F.7.2.2: created IN PROGRESS, modifiable until COMPLETED or
// DISCONTINUED, no duplicates.
class MppsStore {
public:
    std::uint16_t create(const std::string& uid, const Dataset& attributes);
    std::uint16_t set(const std::string& uid, const Dataset& modifications);
    std::optional<Dataset> get(const std::string& uid) const;
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, Dataset> items_;
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

// An in-memory UPS SCP (UPS Push/Pull/Watch/Query provider and UPS Event sender).
class UpsProvider {
public:
    UpsProvider() = default;

    std::uint16_t create(const std::string& uid, const Dataset& attributes);
    std::uint16_t set(const std::string& uid, const Dataset& modifications);
    std::uint16_t get(const std::string& uid, const std::optional<std::vector<Tag>>& attributes, Dataset& out) const;
    std::uint16_t change_state(const std::string& uid, std::string_view state, const std::string& transaction_uid);
    std::uint16_t request_cancel(const std::string& uid, const std::string& requesting_ae, const Dataset& info);
    // uid may be one of the global subscription instances.
    std::uint16_t subscribe(const std::string& uid, const std::string& ae, bool deletion_lock);
    std::uint16_t unsubscribe(const std::string& uid, const std::string& ae);
    std::uint16_t suspend_global(const std::string& ae);
    std::vector<Dataset> find(const Dataset& identifier, const MatchOptions& opt = {}) const;
    std::optional<std::string> state(const std::string& uid) const;
    // Event reports produced by the operations so far, per receiving AE (drained).
    std::vector<std::pair<std::string, UpsEventReport>> take_events();
    // Hook to let a performer refuse cancellation requests (default: accept).
    std::function<bool(const std::string& uid)> performer_accepts_cancel;

private:
    struct Item {
        Dataset ds;
        std::string state;
        std::string transaction_uid;
        std::set<std::string> subscribers;
    };
    void queue_state_report(const std::string& uid, const Item& it, const std::set<std::string>& to);
    std::set<std::string> watchers(const Item& it) const;

    mutable std::mutex mu_;
    std::map<std::string, Item> items_;
    std::set<std::string> global_;        // global subscribers
    std::set<std::string> suspended_;     // global subscription suspended for new instances
    std::vector<std::pair<std::string, UpsEventReport>> events_;
};

// SCU helpers (each uses the context accepted for the given UPS SOP class).
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
    std::string sop_instance;
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
};

// Abstract syntaxes the service set adds to an SCP's presentation context policy.
std::vector<AcceptedSyntax> service_policy(const ServiceSet& s, const std::vector<std::string>& transfer_syntaxes);

struct ServiceContext {
    Association& a;
    const ScpConfig& cfg;
    std::function<void(const std::string&)> log;
    bool release_pending = false;  // set when the peer asked to release while a service waited
};

enum class ServiceOutcome { NotHandled, Handled, Failed };
// Serves `m` if it belongs to one of the services in `s`.
ServiceOutcome dispatch_service(ServiceSet& s, ServiceContext& c, const Message& m);

}  // namespace siaara::net
