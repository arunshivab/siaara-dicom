// SIAARA DICOM Toolkit - service class provider server: accepts associations on a TCP port and
// serves C-ECHO, C-STORE, C-FIND, C-MOVE, C-GET and generic N-services through user callbacks.
// Each association runs on one thread of a fixed pool sized by max_associations.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/association.hpp"
#include "net/dimse.hpp"
#include "net/scu.hpp"
#include "net/socket.hpp"
#include "net/tls.hpp"

namespace siaara::net {

struct QueryRequest {
    std::string calling_ae;
    std::string sop_class_uid;      // the information model (abstract syntax)
    std::uint16_t message_id = 0;
    std::uint16_t priority = 0;
    std::string move_destination;   // C-MOVE only
    Dataset identifier;
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
};

struct ScpHandlers {
    // Returns the C-ECHO status. Default: success.
    std::function<std::uint16_t(const std::string& calling_ae)> on_echo;
    // Receives each stored instance; returns the C-STORE status.
    std::function<std::uint16_t(const StoreRequest&)> on_store;
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
    // Diagnostics (AE titles, addresses, statuses and errors only; never patient data).
    std::function<void(const std::string&)> log;
};

struct ScpConfig {
    std::string ae_title = "SIAARA_SCP";
    bool require_called_ae = true;          // reject associations addressed to another AE title
    std::string bind_address;               // empty: all interfaces
    std::uint16_t port = 11112;             // 0: ephemeral (see ScpServer::port())
    std::vector<std::string> allowed_calling_aes;  // empty: any
    std::size_t max_associations = 8;       // also the size of the association thread pool
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
    std::mutex stop_mu_;
};

}  // namespace siaara::net
