// SIAARA DICOM Toolkit - DICOM Upper Layer association (PS3.8 section 9.2 state machine,
// presentation context negotiation, P-DATA fragmentation and reassembly).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/pdu.hpp"
#include "net/socket.hpp"
#include "net/spool.hpp"

namespace siaara::net {

// ------------------------------------------------------------------ state machine (PS3.8 9.2)

enum class UlState : std::uint8_t {
    Sta1 = 1,   // idle
    Sta2,       // transport open, awaiting A-ASSOCIATE-RQ PDU
    Sta3,       // awaiting local A-ASSOCIATE response primitive
    Sta4,       // awaiting transport connection opening to complete
    Sta5,       // awaiting A-ASSOCIATE-AC or -RJ PDU
    Sta6,       // association established, ready for data transfer
    Sta7,       // awaiting A-RELEASE-RP PDU
    Sta8,       // awaiting local A-RELEASE response primitive
    Sta9,       // release collision requestor side: awaiting A-RELEASE response primitive
    Sta10,      // release collision acceptor side: awaiting A-RELEASE-RP PDU
    Sta11,      // release collision requestor side: awaiting A-RELEASE-RP PDU
    Sta12,      // release collision acceptor side: awaiting A-RELEASE response primitive
    Sta13,      // awaiting transport connection close indication
};

enum class UlEvent : std::uint8_t {
    Evt1 = 1,   // A-ASSOCIATE request (local user)
    Evt2,       // transport connect confirmation
    Evt3,       // A-ASSOCIATE-AC PDU received
    Evt4,       // A-ASSOCIATE-RJ PDU received
    Evt5,       // transport connection indication
    Evt6,       // A-ASSOCIATE-RQ PDU received
    Evt7,       // A-ASSOCIATE response primitive (accept)
    Evt8,       // A-ASSOCIATE response primitive (reject)
    Evt9,       // P-DATA request primitive
    Evt10,      // P-DATA-TF PDU received
    Evt11,      // A-RELEASE request primitive
    Evt12,      // A-RELEASE-RQ PDU received
    Evt13,      // A-RELEASE-RP PDU received
    Evt14,      // A-RELEASE response primitive
    Evt15,      // A-ABORT request primitive
    Evt16,      // A-ABORT PDU received
    Evt17,      // transport connection closed indication
    Evt18,      // ARTIM timer expired
    Evt19,      // unrecognized or invalid PDU received
};

enum class UlAction : std::uint8_t {
    Invalid,  // event not allowed in this state (local primitive misuse)
    AE1, AE2, AE3, AE4, AE5, AE6, AE7, AE8,
    DT1, DT2,
    AR1, AR2, AR3, AR4, AR5, AR6, AR7, AR8, AR9, AR10,
    AA1, AA2, AA3, AA4, AA5, AA6, AA7, AA8,
};

struct UlTransition {
    UlAction action = UlAction::Invalid;
    UlState next = UlState::Sta1;  // for AE-6 the caller picks Sta3 or Sta13
};

// The state table of PS3.8 table 9-10. `requestor` matters only for AR-8 (release collision).
UlTransition ul_transition(UlState s, UlEvent e, bool requestor) noexcept;

// ------------------------------------------------------------------ configuration

struct Timeouts {
    int connect_ms = 15000;   // TCP connect
    int acse_ms = 30000;      // waiting for A-ASSOCIATE-AC/RJ or A-RELEASE-RP
    int dimse_ms = 60000;     // waiting for a DIMSE response / next fragment
    int artim_ms = 30000;     // ARTIM (PS3.8 9.1.5)
    int idle_ms = 300000;     // acceptor waiting for the next request
    int write_ms = 60000;     // waiting for send-buffer space
};

// Default of AssociationLimits::max_service_dataset_size (4 MiB).
inline constexpr std::uint64_t kDefaultMaxServiceDatasetSize = 4ull << 20;

struct AssociationLimits {
    PduLimits pdu;
    std::uint32_t max_receive_pdu = 65536;            // advertised maximum length (clamped to pdu.max_pdu_length)
    std::uint64_t max_command_size = 1u << 20;        // reassembled command set
    // Reassembled data set held in memory (NET-04). With a spool configured, data sets above the
    // spool threshold go to disk instead and are bounded by SpoolConfig::max_size.
    std::uint64_t max_dataset_size = 256ull << 20;
    // Data sets of every message other than C-STORE-RQ: N-service data sets (MPPS, UPS, Storage
    // Commitment, print ...), C-FIND / C-MOVE / C-GET identifiers and their responses (R-109).
    // They are small by nature; the limit is enforced while the fragments arrive (before
    // reassembly) and such data sets are never spooled. Over it: A-ABORT, Errc::LimitExceeded.
    std::uint64_t max_service_dataset_size = kDefaultMaxServiceDatasetSize;
    // P-DATA-TF PDUs longer than the maximum length we advertised (max_receive_pdu) violate
    // PS3.8 D.1. true: abort the association. false (lenient): accept them up to
    // pdu.max_pdu_length and count them (Association::oversized_pdus()).
    bool enforce_max_receive_pdu = false;
};

// An accepted (or rejected) presentation context after negotiation.
struct NegotiatedContext {
    std::uint8_t id = 0;
    std::string abstract_syntax;
    std::string transfer_syntax;
    std::uint8_t result = pc_result::NoReason;
    bool requestor_scu = true;   // roles after SCP/SCU role selection (PS3.7 D.3.3.4)
    bool requestor_scp = false;
    bool accepted() const noexcept { return result == pc_result::Acceptance; }
};

// Acceptor-side policy for one abstract syntax: transfer syntaxes in preference order.
struct AcceptedSyntax {
    std::string abstract_syntax;
    std::vector<std::string> transfer_syntaxes;
    // Roles the acceptor is willing to grant when the requestor proposes role selection.
    bool allow_requestor_scu = true;
    bool allow_requestor_scp = false;
    // Transfer syntaxes (in preference order) for contexts on which the requestor takes only the
    // SCP role (C-GET sub-operations, which this side sends and may transcode). Empty: as above.
    std::vector<std::string> scp_role_transfer_syntaxes = {};
};

// Negotiates the requestor's proposals against the acceptor's policy (acceptor preference wins).
// `accept_any` (optional) is used for abstract syntaxes not in `policy`.
// Fills AC presentation contexts and role-selection replies; returns negotiated contexts.
std::vector<NegotiatedContext> negotiate_contexts(const AssociateRq& rq, const std::vector<AcceptedSyntax>& policy,
                                                  const AcceptedSyntax* accept_any, AssociateAc& ac);

// Interprets an A-ASSOCIATE-AC against what was proposed (requestor side).
std::vector<NegotiatedContext> contexts_from_ac(const AssociateRq& rq, const AssociateAc& ac);

// ------------------------------------------------------------------ messages

// A reassembled DIMSE message: the raw command set (implicit VR little endian) and, if present,
// the raw data set encoded in the presentation context's transfer syntax.
struct RawMessage {
    std::uint8_t context_id = 0;
    Bytes command;
    bool has_data = false;
    Bytes data;
    // Set when the data set was spooled to disk (see net/spool.hpp); `data` then views the mapping.
    std::shared_ptr<SpoolFile> spool;
    // The data set was received but not kept: the spool disk quota or free-space floor would have
    // been passed (R-074). `data` is empty; the receiver answers Out of Resources (0xA700).
    bool discarded = false;
};

// What receive() returned.
enum class ReceiveKind { Message, PeerReleaseRequest };

struct Received {
    ReceiveKind kind = ReceiveKind::Message;
    RawMessage message;
};

// Callback that tells receive() whether a data set follows a given command set.
// (The upper layer does not parse commands itself; the DIMSE layer supplies this.)
using HasDatasetFn = std::function<Result<bool>(const Bytes& command)>;
// Supplies the bytes a spool file starts with (the Part 10 header) for a command whose data set
// is about to be written to disk.
using SpoolPrefixFn = std::function<Result<std::vector<std::uint8_t>>(const Bytes& command, const NegotiatedContext& pc)>;

class Association {
public:
    ~Association();
    Association(const Association&) = delete;
    Association& operator=(const Association&) = delete;

    // ---- requestor: AE-1 .. AE-4. On rejection returns Errc::Rejected and fills *rj if given.
    static Result<std::unique_ptr<Association>> request(const std::string& host, std::uint16_t port,
                                                        const AssociateRq& rq, const Timeouts& to,
                                                        const AssociationLimits& lim, AssociateRj* rj = nullptr);
    // Same, over an already connected transport (e.g. a TLS transport).
    static Result<std::unique_ptr<Association>> request_over(std::unique_ptr<Transport> t, const AssociateRq& rq,
                                                             const Timeouts& to, const AssociationLimits& lim,
                                                             AssociateRj* rj = nullptr);

    // ---- acceptor: AE-5/AE-6. Waits (ARTIM) for the A-ASSOCIATE-RQ PDU; returns in Sta3.
    static Result<std::unique_ptr<Association>> receive_request(std::unique_ptr<Transport> t, const Timeouts& to,
                                                                const AssociationLimits& lim);
    const AssociateRq& request_pdu() const noexcept { return rq_; }
    // AE-7: sends the A-ASSOCIATE-AC. Max length and implementation UID are filled in if empty.
    Status accept(AssociateAc ac, std::vector<NegotiatedContext> contexts);
    // AE-8: sends A-ASSOCIATE-RJ and waits (ARTIM) for the peer to close.
    void reject(const AssociateRj& rj);

    // ---- data transfer (Sta6, and Sta8 for the acceptor before answering a release)
    // Sends a command set and optional data set as P-DATA-TF PDUs sized for the peer's maximum.
    Status send_message(std::uint8_t context_id, const std::uint8_t* command, std::size_t command_len,
                        const std::uint8_t* data, std::size_t data_len);
    // Receives the next complete message (or the peer's A-RELEASE-RQ). `timeout_ms` bounds the
    // wait for the first fragment; later fragments use the DIMSE timeout.
    Result<Received> receive(int timeout_ms, const HasDatasetFn& has_dataset);
    // Same; data sets larger than spool->threshold are written to a spool file (when spool is set).
    Result<Received> receive(int timeout_ms, const HasDatasetFn& has_dataset, const SpoolConfig* spool,
                             const SpoolPrefixFn& prefix);
    // True when bytes are waiting (used to look for C-CANCEL between responses).
    bool data_pending(int timeout_ms);

    // ---- release and abort
    // AR-1 then AR-3, handling a release collision (AR-8 .. AR-10) for either role.
    Status release();
    // AR-4: answers the peer's A-RELEASE-RQ, then waits (ARTIM) for the peer to close.
    Status respond_release();
    // AA-1: sends A-ABORT and closes.
    void abort(std::uint8_t source = abort_src::User, std::uint8_t reason = abort_reason::NotSpecified);
    // Wakes a blocked receive() from another thread (the association then fails with an error).
    void interrupt() noexcept;

    // ---- information
    UlState state() const noexcept { return state_; }
    bool established() const noexcept { return state_ == UlState::Sta6; }
    bool is_requestor() const noexcept { return requestor_; }
    const std::vector<NegotiatedContext>& contexts() const noexcept { return contexts_; }
    const NegotiatedContext* context(std::uint8_t id) const noexcept;
    // First accepted context for an abstract syntax, preferring the given transfer syntax order.
    const NegotiatedContext* find_context(std::string_view abstract_syntax,
                                          const std::vector<std::string>& ts_preference = {}) const;
    std::uint32_t peer_max_pdu() const noexcept { return peer_max_pdu_; }
    const AssociateAc& accept_pdu() const noexcept { return ac_; }
    const std::string& calling_ae() const noexcept { return rq_.calling_ae; }
    const std::string& called_ae() const noexcept { return rq_.called_ae; }
    std::string peer_address() const { return transport_ ? transport_->peer_address() : std::string(); }
    const Timeouts& timeouts() const noexcept { return to_; }
    const AssociationLimits& limits() const noexcept { return lim_; }
    std::uint16_t next_message_id() noexcept;
    // Last A-ABORT received from the peer, if any.
    std::optional<Abort> peer_abort() const noexcept { return peer_abort_; }
    // P-DATA-TF PDUs received that were longer than our advertised maximum length (lenient mode).
    std::uint64_t oversized_pdus() const noexcept { return oversized_pdus_; }
    // A number unique among the associations of this process run (1, 2, ...), for telling the
    // events of concurrent associations apart.
    std::uint64_t id() const noexcept { return id_; }

private:
    Association(std::unique_ptr<Transport> t, bool requestor, const Timeouts& to, const AssociationLimits& lim);

    Status fire(UlEvent e);  // applies a transition; performs the generic abort actions
    Status send_pdu(const Pdu& p);
    Result<Pdu> next_pdu(int timeout_ms, UlEvent& evt);
    void enter_sta13_and_wait();
    void close_transport() noexcept;
    Status handle_unexpected(UlEvent e, const char* what);

    std::unique_ptr<Transport> transport_;
    bool requestor_;
    Timeouts to_;
    AssociationLimits lim_;
    UlState state_ = UlState::Sta1;
    AssociateRq rq_;
    AssociateAc ac_;
    std::vector<NegotiatedContext> contexts_;
    std::uint32_t peer_max_pdu_ = 0;
    std::uint16_t message_id_ = 0;
    std::optional<Abort> peer_abort_;
    std::atomic<bool> interrupted_{false};
    std::uint8_t pending_abort_reason_ = abort_reason::UnexpectedPdu;
    std::uint64_t oversized_pdus_ = 0;
    std::uint64_t id_ = 0;
};

// The id of the association whose request the calling thread is serving (ScpServer sets it for
// its handlers), or 0.
std::uint64_t current_association_id() noexcept;
// Sets it for a scope (ScpServer per association; an event sender for a deferred report).
class AssociationIdScope {
public:
    explicit AssociationIdScope(std::uint64_t id) noexcept;
    ~AssociationIdScope();
    AssociationIdScope(const AssociationIdScope&) = delete;
    AssociationIdScope& operator=(const AssociationIdScope&) = delete;

private:
    std::uint64_t previous_;
};

}  // namespace siaara::net
