// SIAARA DICOM Toolkit - service class provider server: accepts associations on a TCP port and
// serves C-ECHO, C-STORE, C-FIND, C-MOVE, C-GET and generic N-services through user callbacks.
// Each association runs on one thread of a fixed pool sized by max_associations.
#pragma once

#include <atomic>
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
#include <thread>
#include <vector>

#include "codec/transcode.hpp"
#include "core/dataset.hpp"
#include "core/incomplete_policy.hpp"
#include "core/status.hpp"
#include "net/association.hpp"
#include "net/dimse.hpp"
#include "net/scu.hpp"
#include "net/socket.hpp"
#include "net/tls.hpp"
#include "net/spool.hpp"

namespace siaara::net {

struct ServiceSet;  // net/services.hpp

struct QueryRequest {
    std::string calling_ae;
    std::string peer_address;       // the requestor's network address
    std::string sop_class_uid;      // the information model (abstract syntax)
    std::uint16_t message_id = 0;
    std::uint16_t priority = 0;
    std::string move_destination;   // C-MOVE only
    Dataset identifier;
    // SOP Class Extended Negotiation agreed for this SOP class (empty when none); see
    // parse_query_flags() in net/services.hpp.
    std::vector<std::uint8_t> extended_negotiation;
};

// One instance to send for C-MOVE / C-GET: either a file path (read when sent) or a loaded file.
struct RetrieveItem {
    std::string path;
    std::shared_ptr<const DicomFile> file;
};

struct MoveDestination {
    std::string host;
    std::uint16_t port = 0;
    std::string ae_title;  // defaults to the requested destination AE when empty
    // TLS for associations to this AE: nullopt follows ScpConfig::move_tls.enabled; true uses
    // ScpConfig::move_tls (with enabled forced on), false plain TCP.
    std::optional<bool> tls = std::nullopt;
};

struct ScpHandlers {
    // Returns the C-ECHO status. Default: success.
    std::function<std::uint16_t(const std::string& calling_ae)> on_echo;
    // Receives each stored instance; returns the C-STORE status.
    std::function<std::uint16_t(const StoreRequest&)> on_store;
    // A C-STORE the server refused before on_store (R-116: incomplete data set under
    // IncompletePolicy::Refuse): the request (data set as far as it was read), the status answered
    // and why (a fixed code and offsets; no patient data).
    std::function<void(const StoreRequest&, std::uint16_t status, const std::string& why)> on_store_refused;
    // IncompletePolicy::Quarantine: keeps the incomplete instance (rq.raw exactly as received)
    // outside the archive. `answer` is the status the sender will get when this returns Success;
    // return a failure status (0xA700: quarantine full) when it could not be kept.
    std::function<std::uint16_t(const StoreRequest&, std::uint16_t answer)> on_store_quarantine;
    // Yields matches through `emit` (which returns false once the SCU cancelled or the
    // association failed); returns the final status (Success, or a failure code).
    std::function<std::uint16_t(const QueryRequest&, const std::function<bool(const Dataset&)>& emit)> on_find;
    // Supplies the instances for a C-MOVE / C-GET; returns Success to proceed or a failure status.
    std::function<std::uint16_t(const QueryRequest&, std::vector<RetrieveItem>& items)> on_move;
    std::function<std::uint16_t(const QueryRequest&, std::vector<RetrieveItem>& items)> on_get;
    // Resolves a C-MOVE destination AE title. Unknown destination: return nullopt.
    std::function<std::optional<MoveDestination>(const std::string& ae)> resolve_move_destination;
    // Generic N-service handler: fills `reply` (and sets has_reply) for responses with a data set.
    std::function<std::uint16_t(const Message& rq, const Dataset& rq_dataset, Dataset& reply, bool& has_reply)>
        on_n_request;
    // User identity negotiation: return false to reject the association. `response` is sent back
    // when the requestor asked for a positive response.
    std::function<bool(const UserIdentityRq&, std::vector<std::uint8_t>& response)> on_identity;
    // A C-GET / C-MOVE sub-operation sent an instance under a new SOP Instance UID because a
    // lossy transcode was needed (R-093): (calling AE, the instance's own UID, the UID sent).
    std::function<void(const std::string& calling_ae, const std::string& original_uid, const std::string& sent_uid)>
        on_sop_instance_uid_changed;
    // After a C-MOVE / C-GET: the request, the final status and the sub-operation counts.
    std::function<void(const QueryRequest&, bool is_get, std::uint16_t status, std::uint16_t completed,
                       std::uint16_t failed, std::uint16_t warning)>
        on_retrieve_done;
    // Diagnostics (AE titles, addresses, statuses and errors only; never patient data).
    std::function<void(const std::string&)> log;
    // Storage Commitment, MPPS, UPS, Instance Availability and other services (net/services.hpp).
    std::shared_ptr<ServiceSet> services;
};

struct ScpConfig {
    std::string ae_title = "SIAARA_SCP";
    bool require_called_ae = true;          // reject associations addressed to another AE title
    std::string bind_address;               // empty: all interfaces
    std::uint16_t port = 11112;             // 0: ephemeral (see ScpServer::port())
    // Calling AE titles allowed to associate. An empty list is refused by start() unless
    // allow_any_calling_ae is set explicitly (NET-05).
    std::vector<std::string> allowed_calling_aes;
    bool allow_any_calling_ae = false;      // accept any calling AE title when the list is empty
    std::size_t max_associations = 8;       // also the size of the association thread pool
    // Resource limits (NET-04); 0 = unlimited.
    std::size_t max_associations_per_ip = 4;            // concurrent (queued or served) per peer address
    // Per-IP counting groups IPv6 peers by this prefix and IPv4-mapped addresses as IPv4 (R-016).
    unsigned peer_ipv6_prefix_bits = 64;
    int max_association_lifetime_ms = 2 * 60 * 60 * 1000;  // from acceptance; then A-ABORT
    std::uint64_t max_operations_per_association = 100000;  // DIMSE requests; then A-ABORT
    // Presentation context policy. When empty it is derived from the handlers that are set:
    // Verification always; storage classes for on_store / on_get; Q/R models for find/move/get.
    std::vector<AcceptedSyntax> accepted;
    // Transfer syntaxes (in preference order) for the derived policy and for accept_any_sop_class.
    std::vector<std::string> transfer_syntaxes = native_transfer_syntaxes();
    bool accept_any_sop_class = false;      // accept any abstract syntax with transfer_syntaxes
    Timeouts timeouts;
    AssociationLimits limits;
    // Pool threads: max_associations. The accept loop polls for stop() at this interval.
    int accept_poll_ms = 100;
    // TLS (PS3.15 B.12/B.13) when tls.enabled: every connection on this port must use TLS
    // (serve plain DIMSE from a second server on another port). tls.log defaults to handlers.log.
    TlsSettings tls;
    // TLS client settings for C-MOVE sub-associations (used when move_tls.enabled).
    TlsSettings move_tls;
    // ---- transcoding, streaming and negotiation extras
    // Received C-STORE data sets larger than spool->threshold are written to disk (net/spool.hpp).
    std::optional<SpoolConfig> spool;
    // C-STORE requests whose command UIDs differ from the data set's: refuse (0xA900) instead of
    // accepting them under the data set's UIDs with StoreRequest::uid_mismatch (R-073).
    bool strict_store_uids = false;
    // When set, received instances are converted to this transfer syntax before on_store sees them
    // (left as received when the conversion is not possible).
    std::string preferred_store_ts;
    // Parameters for that conversion and for C-GET / C-MOVE sub-operations that must transcode.
    codec::TranscodeParams transcode_params = [] {
        codec::TranscodeParams p;
        p.allow_lossy = false;
        return p;
    }();
    // A received C-STORE data set whose read was not complete - truncated, bytes skipped or
    // recovered by a heuristic (R-116). Refuse: status 0xC000, nothing reaches on_store
    // (on_store_refused is told). AcceptAsReceived: on_store gets it with StoreRequest::read_loss
    // set and the bytes exactly as received; it is never converted to preferred_store_ts.
    IncompletePolicy incomplete_policy = kDefaultIncompletePolicy;
    // The policy for particular calling AEs (trimmed AE title -> policy); others get the one above.
    std::map<std::string, IncompletePolicy> incomplete_policy_by_caller;
    // Quarantine: on_store_quarantine keeps the bytes outside the archive; the sender gets
    // 0xB007 (Warning) or 0xC000 (Failure) per quarantine_response, or the handler's failure
    // status (0xA700 when the quarantine is full). Without the handler the instance is refused.
    QuarantineResponse quarantine_response = kDefaultQuarantineResponse;
    // C-MOVE: the destinations a calling AE may name (R-129). A calling AE without an entry may
    // name any destination resolve_move_destination knows; others get 0xA801 (Move Destination
    // unknown) for a destination outside their list.
    std::map<std::string, std::set<std::string>> move_destinations_by_caller;
    // C-GET / C-MOVE sub-operations for files whose read was not complete (truncated input,
    // skipped bytes): refused (the sub-operation fails, logged) unless set (check_sendable).
    bool allow_incomplete = false;
    // Operations a requestor may have outstanding when it proposes an Asynchronous Operations
    // Window (PS3.7 D.3.3.3); 0 = unlimited. Requests are performed in order of arrival.
    std::uint16_t max_async_operations = 16;
    // SOP Class Extended Negotiation the SCP supports (each byte: 1 = supported).
    std::vector<ExtendedNegotiation> extended_negotiation;
};

class ScpServer {
public:
    ScpServer(ScpConfig cfg, ScpHandlers handlers);
    ~ScpServer();
    ScpServer(const ScpServer&) = delete;
    ScpServer& operator=(const ScpServer&) = delete;

    Status start();
    // Stops accepting, aborts running associations, joins all threads. Idempotent.
    void stop();
    std::uint16_t port() const noexcept { return port_; }
    bool running() const noexcept { return running_.load(); }
    std::size_t active_associations() const;
    std::uint64_t associations_handled() const noexcept { return handled_.load(); }
    std::uint64_t associations_rejected() const noexcept { return rejected_.load(); }

private:
    void accept_loop();
    void worker_loop();
    void serve(std::unique_ptr<Transport> t);
    void quick_reject(std::unique_ptr<Transport> t);
    void reject_loop();
    void release_peer(const std::string& peer);
    bool register_assoc(Association* a);
    void unregister_assoc(Association* a);
    void log(const std::string& s) const;

    // per-association services
    struct Ctx;
    bool dispatch(Ctx& c, Message& m);
    Status do_echo(Ctx& c, const Message& m);
    Status do_store(Ctx& c, const Message& m);
    Status do_find(Ctx& c, const Message& m);
    Status do_retrieve(Ctx& c, const Message& m, bool is_get);
    Status do_n(Ctx& c, const Message& m);
    bool cancel_requested(Ctx& c, std::uint16_t mid);

    ScpConfig cfg_;
    ScpHandlers h_;
    std::vector<AcceptedSyntax> policy_;
    std::optional<AcceptedSyntax> any_policy_;
    TcpListener listener_;
    TlsListener tls_listener_;           // used instead of listener_ when cfg_.tls.enabled
    std::set<Transport*> handshaking_;   // TLS handshakes in progress (stop() wakes them); under mu_
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> handled_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::thread accept_thread_;
    std::vector<std::thread> workers_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Transport>> queue_;
    std::size_t busy_ = 0;
    std::set<Association*> live_;
    std::map<std::string, std::size_t> per_ip_;  // queued or served associations per peer; under mu_
    // Connections refused because the server is full: answered by reject_thread_, never on the
    // accept thread (NET-03). Bounded; under mu_.
    std::deque<std::unique_ptr<Transport>> reject_queue_;
    std::condition_variable reject_cv_;
    std::thread reject_thread_;
    std::mutex stop_mu_;
};

}  // namespace siaara::net
