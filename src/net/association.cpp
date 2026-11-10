// SIAARA DICOM Toolkit - Upper Layer association: PS3.8 9.2 state machine and P-DATA handling.
#include "net/association.hpp"

#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <variant>

#include "core/writer.hpp"

namespace siaara::net {

using S = UlState;
using E = UlEvent;
using A = UlAction;

// ------------------------------------------------------------------ state table (PS3.8 table 9-10)

UlTransition ul_transition(UlState s, UlEvent e, bool requestor) noexcept {
    const auto in = [s](S lo, S hi) { return s >= lo && s <= hi; };
    // "Sta3..Sta12 except where listed" helper for the many AA-8 cells.
    const bool sta3_to_12 = in(S::Sta3, S::Sta12);
    switch (e) {
        case E::Evt1:
            if (s == S::Sta1) return {A::AE1, S::Sta4};
            break;
        case E::Evt2:
            if (s == S::Sta4) return {A::AE2, S::Sta5};
            break;
        case E::Evt3:
            if (s == S::Sta2) return {A::AA1, S::Sta13};
            if (s == S::Sta5) return {A::AE3, S::Sta6};
            if (s == S::Sta13) return {A::AA6, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
        case E::Evt4:
            if (s == S::Sta2) return {A::AA1, S::Sta13};
            if (s == S::Sta5) return {A::AE4, S::Sta1};
            if (s == S::Sta13) return {A::AA6, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
        case E::Evt5:
            if (s == S::Sta1) return {A::AE5, S::Sta2};
            break;
        case E::Evt6:
            if (s == S::Sta2) return {A::AE6, S::Sta3};
            if (s == S::Sta13) return {A::AA7, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
        case E::Evt7:
            if (s == S::Sta3) return {A::AE7, S::Sta6};
            break;
        case E::Evt8:
            if (s == S::Sta3) return {A::AE8, S::Sta13};
            break;
        case E::Evt9:
            if (s == S::Sta6) return {A::DT1, S::Sta6};
            if (s == S::Sta8) return {A::AR7, S::Sta8};
            break;
        case E::Evt10:
            if (s == S::Sta2) return {A::AA1, S::Sta13};
            if (s == S::Sta6) return {A::DT2, S::Sta6};
            if (s == S::Sta7) return {A::AR6, S::Sta7};
            if (s == S::Sta13) return {A::AA6, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
        case E::Evt11:
            if (s == S::Sta6) return {A::AR1, S::Sta7};
            break;
        case E::Evt12:
            if (s == S::Sta2) return {A::AA1, S::Sta13};
            if (s == S::Sta6) return {A::AR2, S::Sta8};
            if (s == S::Sta7) return {A::AR8, requestor ? S::Sta9 : S::Sta10};
            if (s == S::Sta13) return {A::AA6, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
        case E::Evt13:
            if (s == S::Sta2) return {A::AA1, S::Sta13};
            if (s == S::Sta7) return {A::AR3, S::Sta1};
            if (s == S::Sta10) return {A::AR10, S::Sta12};
            if (s == S::Sta11) return {A::AR3, S::Sta1};
            if (s == S::Sta13) return {A::AA6, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
        case E::Evt14:
            if (s == S::Sta8) return {A::AR4, S::Sta13};
            if (s == S::Sta9) return {A::AR9, S::Sta11};
            if (s == S::Sta12) return {A::AR4, S::Sta13};
            break;
        case E::Evt15:
            if (s == S::Sta4) return {A::AA2, S::Sta1};
            if (sta3_to_12) return {A::AA1, S::Sta13};
            break;
        case E::Evt16:
            if (s == S::Sta2 || s == S::Sta13) return {A::AA2, S::Sta1};
            if (sta3_to_12 && s != S::Sta4) return {A::AA3, S::Sta1};
            break;
        case E::Evt17:
            if (s == S::Sta2) return {A::AA5, S::Sta1};
            if (s == S::Sta13) return {A::AR5, S::Sta1};
            if (sta3_to_12) return {A::AA4, S::Sta1};
            break;
        case E::Evt18:
            if (s == S::Sta2 || s == S::Sta13) return {A::AA2, S::Sta1};
            break;
        case E::Evt19:
            if (s == S::Sta2) return {A::AA1, S::Sta13};
            if (s == S::Sta13) return {A::AA7, S::Sta13};
            if (sta3_to_12 && s != S::Sta4) return {A::AA8, S::Sta13};
            break;
    }
    return {A::Invalid, s};
}

// ------------------------------------------------------------------ negotiation helpers

namespace {

const RoleSelection* find_role(const std::vector<RoleSelection>& roles, std::string_view sop) {
    for (const RoleSelection& r : roles)
        if (r.sop_class_uid == sop) return &r;
    return nullptr;
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

std::vector<NegotiatedContext> negotiate_contexts(const AssociateRq& rq, const std::vector<AcceptedSyntax>& policy,
                                                  const AcceptedSyntax* accept_any, AssociateAc& ac) {
    std::vector<NegotiatedContext> out;
    ac.contexts.clear();
    ac.user.roles.clear();
    for (const PresentationContextRq& pc : rq.contexts) {
        NegotiatedContext n;
        n.id = pc.id;
        n.abstract_syntax = pc.abstract_syntax;
        const AcceptedSyntax* pol = nullptr;
        for (const AcceptedSyntax& a : policy)
            if (a.abstract_syntax == pc.abstract_syntax) {
                pol = &a;
                break;
            }
        if (!pol) pol = accept_any;
        PresentationContextAc pac;
        pac.id = pc.id;
        if (!pol) {
            n.result = pc_result::AbstractSyntaxNotSupported;
        } else {
            const RoleSelection* role = find_role(rq.user.roles, pc.abstract_syntax);
            const bool scp_only = role && role->scp_role && !role->scu_role;
            const std::vector<std::string>& accepted =
                scp_only && !pol->scp_role_transfer_syntaxes.empty() ? pol->scp_role_transfer_syntaxes : pol->transfer_syntaxes;
            for (const std::string& ts : accepted)
                if (contains(pc.transfer_syntaxes, ts)) {
                    n.transfer_syntax = ts;
                    break;
                }
            if (n.transfer_syntax.empty()) {
                n.result = pc_result::TransferSyntaxesNotSupported;
            } else {
                n.result = pc_result::Acceptance;
                if (const RoleSelection* r = find_role(rq.user.roles, pc.abstract_syntax)) {
                    n.requestor_scu = r->scu_role && pol->allow_requestor_scu;
                    n.requestor_scp = r->scp_role && pol->allow_requestor_scp;
                    if (!find_role(ac.user.roles, pc.abstract_syntax))
                        ac.user.roles.push_back({pc.abstract_syntax, n.requestor_scu, n.requestor_scp});
                    if (!n.requestor_scu && !n.requestor_scp) {
                        n.result = pc_result::UserRejection;
                        n.transfer_syntax.clear();
                    }
                } else if (!pol->allow_requestor_scu) {
                    n.result = pc_result::UserRejection;
                    n.transfer_syntax.clear();
                }
            }
        }
        pac.result = n.result;
        pac.transfer_syntax = n.transfer_syntax;
        ac.contexts.push_back(std::move(pac));
        out.push_back(std::move(n));
    }
    return out;
}

std::vector<NegotiatedContext> contexts_from_ac(const AssociateRq& rq, const AssociateAc& ac) {
    std::vector<NegotiatedContext> out;
    for (const PresentationContextRq& pc : rq.contexts) {
        NegotiatedContext n;
        n.id = pc.id;
        n.abstract_syntax = pc.abstract_syntax;
        n.result = pc_result::NoReason;
        for (const PresentationContextAc& a : ac.contexts) {
            if (a.id != pc.id) continue;
            n.result = a.result;
            // The accepted transfer syntax must be one that was proposed.
            if (a.result == pc_result::Acceptance) {
                if (contains(pc.transfer_syntaxes, a.transfer_syntax)) n.transfer_syntax = a.transfer_syntax;
                else n.result = pc_result::NoReason;
            }
            break;
        }
        const RoleSelection* proposed = find_role(rq.user.roles, pc.abstract_syntax);
        const RoleSelection* answer = find_role(ac.user.roles, pc.abstract_syntax);
        if (proposed && answer) {
            n.requestor_scu = proposed->scu_role && answer->scu_role;
            n.requestor_scp = proposed->scp_role && answer->scp_role;
        }
        out.push_back(std::move(n));
    }
    return out;
}

// ------------------------------------------------------------------ association

namespace {
std::mutex& close_mutex_for(const void*) {
    // One mutex for all associations: close/interrupt are rare and short.
    static std::mutex m;
    return m;
}

// Concatenates fragments into one growable buffer as they arrive, so each received PDU buffer is
// released once copied and take() hands the buffer over without a second copy (NET-04, R-015).
// Zero-copy when there is exactly one fragment.
class Accumulator {
public:
    void clear() {
        first_ = Bytes();
        buf_.reset();
        total_ = 0;
    }
    bool add(const Bytes& b, std::uint64_t cap) {
        std::uint64_t t = 0;
        if (!checked_add(total_, b.size(), t) || t > cap || t > SIZE_MAX) return false;
        if (b.empty()) return true;
        if (!buf_ && total_ == 0) {
            first_ = b;
            total_ = t;
            return true;
        }
        try {
            if (!buf_) {
                buf_ = std::make_shared<std::vector<std::uint8_t>>();
                buf_->reserve(static_cast<std::size_t>(t));
                buf_->insert(buf_->end(), first_.data(), first_.data() + first_.size());
                first_ = Bytes();
            }
            buf_->insert(buf_->end(), b.data(), b.data() + b.size());
        } catch (const std::bad_alloc&) {
            return false;
        }
        total_ = t;
        return true;
    }
    std::uint64_t total() const noexcept { return total_; }
    const std::uint8_t* data() const noexcept { return buf_ ? buf_->data() : first_.data(); }
    Bytes take() {
        Bytes r = first_;
        if (buf_) {
            const std::uint8_t* d = buf_->data();
            const std::size_t n = buf_->size();
            r = Bytes(std::shared_ptr<const void>(buf_, buf_.get()), d, n);
        }
        clear();
        return r;
    }

private:
    Bytes first_;
    std::shared_ptr<std::vector<std::uint8_t>> buf_;
    std::uint64_t total_ = 0;
};

}  // namespace

Association::Association(std::unique_ptr<Transport> t, bool requestor, const Timeouts& to,
                         const AssociationLimits& lim)
    : transport_(std::move(t)), requestor_(requestor), to_(to), lim_(lim) {
    if (lim_.max_receive_pdu == 0 || lim_.max_receive_pdu > lim_.pdu.max_pdu_length)
        lim_.max_receive_pdu = static_cast<std::uint32_t>(std::min<std::uint64_t>(lim_.pdu.max_pdu_length, 0xFFFFFFFFu));
    // Strict: P-DATA-TF PDUs are held to the maximum length we advertise (PS3.8 D.1, R-034).
    if (lim_.enforce_max_receive_pdu) lim_.pdu.max_pdata_length = lim_.max_receive_pdu;
    static std::atomic<std::uint64_t> next_id{1};
    id_ = next_id.fetch_add(1, std::memory_order_relaxed);
}

namespace {
thread_local std::uint64_t t_association_id = 0;
}
std::uint64_t current_association_id() noexcept { return t_association_id; }
AssociationIdScope::AssociationIdScope(std::uint64_t id) noexcept : previous_(t_association_id) { t_association_id = id; }
AssociationIdScope::~AssociationIdScope() { t_association_id = previous_; }

Association::~Association() {
    if (state_ != S::Sta1 && transport_ && transport_->is_open()) {
        // Destroyed while associated: abort without waiting.
        if (state_ != S::Sta13 && state_ != S::Sta4 && state_ != S::Sta2) (void)send_pdu(Abort{abort_src::User, 0});
    }
    close_transport();
}

void Association::close_transport() noexcept {
    std::lock_guard<std::mutex> lock(close_mutex_for(this));
    if (transport_) transport_->close();
}

void Association::interrupt() noexcept {
    interrupted_.store(true);
    std::lock_guard<std::mutex> lock(close_mutex_for(this));
    if (transport_ && transport_->is_open()) transport_->shutdown();
}

std::uint16_t Association::next_message_id() noexcept {
    ++message_id_;
    if (message_id_ == 0) message_id_ = 1;
    return message_id_;
}

const NegotiatedContext* Association::context(std::uint8_t id) const noexcept {
    for (const NegotiatedContext& c : contexts_)
        if (c.id == id) return &c;
    return nullptr;
}

const NegotiatedContext* Association::find_context(std::string_view abstract_syntax,
                                                   const std::vector<std::string>& pref) const {
    for (const std::string& ts : pref)
        for (const NegotiatedContext& c : contexts_)
            if (c.accepted() && c.abstract_syntax == abstract_syntax && c.transfer_syntax == ts) return &c;
    for (const NegotiatedContext& c : contexts_)
        if (c.accepted() && c.abstract_syntax == abstract_syntax) return &c;
    return nullptr;
}

Status Association::send_pdu(const Pdu& p) {
    std::vector<std::uint8_t> buf;
    if (Status s = encode_pdu(p, buf); !s) return s;
    if (!transport_ || !transport_->is_open()) return Status(Errc::NetworkError, "transport closed");
    return transport_->write_all(buf.data(), buf.size(), to_.write_ms);
}

Result<Pdu> Association::next_pdu(int timeout_ms, UlEvent& evt) {
    if (interrupted_.load()) {
        evt = E::Evt17;
        return Status(Errc::Cancelled, "association interrupted");
    }
    if (!transport_ || !transport_->is_open()) {
        evt = E::Evt17;
        return Status(Errc::NetworkError, "transport closed");
    }
    Result<Pdu> r = read_pdu(*transport_, timeout_ms, lim_.pdu);
    if (r && std::holds_alternative<PDataTf>(*r)) {
        std::uint64_t len = 0;
        for (const Pdv& v : std::get<PDataTf>(*r).pdvs) len += 6u + v.data.size();
        if (len > lim_.max_receive_pdu) ++oversized_pdus_;  // lenient: accepted, flagged
    }
    if (!r) {
        const Errc c = r.error().code;
        if (c == Errc::Timeout) evt = E::Evt18;  // caller decides what a timeout means
        else if (c == Errc::NetworkError || interrupted_.load()) evt = E::Evt17;
        else {
            evt = E::Evt19;
            pending_abort_reason_ = c == Errc::Unsupported ? abort_reason::UnrecognizedPdu
                                                           : abort_reason::InvalidPduParameterValue;
        }
        return r;
    }
    switch (pdu_type(*r)) {
        case PduType::AssociateRq: evt = E::Evt6; break;
        case PduType::AssociateAc: evt = E::Evt3; break;
        case PduType::AssociateRj: evt = E::Evt4; break;
        case PduType::PData: evt = E::Evt10; break;
        case PduType::ReleaseRq: evt = E::Evt12; break;
        case PduType::ReleaseRp: evt = E::Evt13; break;
        case PduType::Abort: evt = E::Evt16; break;
    }
    pending_abort_reason_ = abort_reason::UnexpectedPdu;
    return r;
}

Status Association::fire(UlEvent e) {
    const UlTransition t = ul_transition(state_, e, requestor_);
    if (t.action == A::Invalid)
        return Status(Errc::InvalidArgument, "operation not valid in association state " +
                                                 std::to_string(static_cast<int>(state_)));
    switch (t.action) {
        case A::AA1:
            (void)send_pdu(Abort{e == E::Evt15 ? abort_src::User : abort_src::Provider,
                                 e == E::Evt15 ? abort_reason::NotSpecified : pending_abort_reason_});
            break;
        case A::AA7:
        case A::AA8:
            (void)send_pdu(Abort{abort_src::Provider, pending_abort_reason_});
            break;
        case A::AA2:
        case A::AA3:
        case A::AA4:
        case A::AA5:
        case A::AR5:
        case A::AR3:
        case A::AE4:
            close_transport();
            break;
        default:
            break;  // AA-6 ignores; other actions are carried out by the caller
    }
    state_ = t.next;
    return Status::ok();
}

void Association::enter_sta13_and_wait() {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(to_.artim_ms);  // ARTIM started
    while (state_ == S::Sta13) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        UlEvent evt = E::Evt18;
        if (left > 0) {
            Result<Pdu> r = next_pdu(static_cast<int>(left), evt);
            (void)r;
        }
        (void)fire(evt);  // Evt17 -> AR-5, Evt18 -> AA-2, Evt16 -> AA-2, Evt6/19 -> AA-7, else AA-6
    }
    close_transport();
}

Status Association::handle_unexpected(UlEvent e, const char* what) {
    (void)fire(e);
    std::string msg = what;
    if (e == E::Evt16) msg = "association aborted by peer";
    else if (e == E::Evt17) msg = "connection closed by peer";
    if (state_ == S::Sta13) enter_sta13_and_wait();
    return Status(e == E::Evt16 || e == E::Evt17 ? Errc::NetworkError : Errc::Malformed, msg);
}

// ---- requestor

Result<std::unique_ptr<Association>> Association::request(const std::string& host, std::uint16_t port,
                                                          const AssociateRq& rq, const Timeouts& to,
                                                          const AssociationLimits& lim, AssociateRj* rj) {
    Result<std::unique_ptr<Transport>> t = tcp_connect(host, port, to.connect_ms);
    if (!t) return t.take_error();
    return request_over(std::move(t.value()), rq, to, lim, rj);
}

Result<std::unique_ptr<Association>> Association::request_over(std::unique_ptr<Transport> t, const AssociateRq& rq,
                                                               const Timeouts& to, const AssociationLimits& lim,
                                                               AssociateRj* rj) {
    if (!t || !t->is_open()) return Status(Errc::InvalidArgument, "transport not connected");
    std::unique_ptr<Association> a(new Association(std::move(t), true, to, lim));
    a->rq_ = rq;
    if (a->rq_.user.max_length == 0) a->rq_.user.max_length = a->lim_.max_receive_pdu;
    if (a->rq_.user.implementation_class_uid.empty())
        a->rq_.user.implementation_class_uid = std::string(kImplementationClassUid);
    if (a->rq_.user.implementation_version_name.empty())
        a->rq_.user.implementation_version_name = std::string(kImplementationVersionName);
    (void)a->fire(E::Evt1);  // AE-1: the transport connection was opened above
    (void)a->fire(E::Evt2);  // AE-2: send A-ASSOCIATE-RQ
    if (Status s = a->send_pdu(a->rq_); !s) {
        a->close_transport();
        a->state_ = S::Sta1;
        return s;
    }
    UlEvent evt = E::Evt18;
    Result<Pdu> r = a->next_pdu(to.acse_ms, evt);
    if (!r && evt == E::Evt18) {
        a->abort();
        return Status(Errc::Timeout, "no A-ASSOCIATE response");
    }
    if (evt == E::Evt3) {
        a->ac_ = std::get<AssociateAc>(std::move(r.value()));
        (void)a->fire(evt);  // AE-3
        a->peer_max_pdu_ = a->ac_.user.max_length;
        if (a->peer_max_pdu_ != 0 && a->peer_max_pdu_ < 8) {
            a->pending_abort_reason_ = abort_reason::InvalidPduParameterValue;
            a->abort(abort_src::Provider, abort_reason::InvalidPduParameterValue);
            return Status(Errc::Malformed, "peer maximum PDU length too small");
        }
        a->contexts_ = contexts_from_ac(a->rq_, a->ac_);
        return a;
    }
    if (evt == E::Evt4) {
        if (rj) *rj = std::get<AssociateRj>(r.value());
        (void)a->fire(evt);  // AE-4
        return Status(Errc::Rejected, "association rejected");
    }
    return a->handle_unexpected(evt, r ? "unexpected PDU while awaiting A-ASSOCIATE response"
                                       : r.error().message.c_str());
}

// ---- acceptor

Result<std::unique_ptr<Association>> Association::receive_request(std::unique_ptr<Transport> t, const Timeouts& to,
                                                                  const AssociationLimits& lim) {
    if (!t || !t->is_open()) return Status(Errc::InvalidArgument, "transport not connected");
    std::unique_ptr<Association> a(new Association(std::move(t), false, to, lim));
    (void)a->fire(E::Evt5);  // AE-5: start ARTIM
    UlEvent evt = E::Evt18;
    Result<Pdu> r = a->next_pdu(to.artim_ms, evt);
    if (evt == E::Evt6) {
        a->rq_ = std::get<AssociateRq>(std::move(r.value()));
        (void)a->fire(evt);  // AE-6: stop ARTIM, indicate
        a->peer_max_pdu_ = a->rq_.user.max_length;
        if (a->peer_max_pdu_ != 0 && a->peer_max_pdu_ < 8) {
            a->reject({rj::Permanent, rj::SourceUser, rj::UserNoReason});
            return Status(Errc::Rejected, "peer maximum PDU length too small");
        }
        return a;
    }
    if (evt == E::Evt18) {
        (void)a->fire(evt);  // AA-2
        return Status(Errc::Timeout, "ARTIM expired waiting for A-ASSOCIATE-RQ");
    }
    return a->handle_unexpected(evt, r ? "unexpected PDU while awaiting A-ASSOCIATE-RQ" : r.error().message.c_str());
}

Status Association::accept(AssociateAc ac, std::vector<NegotiatedContext> contexts) {
    if (state_ != S::Sta3) return Status(Errc::InvalidArgument, "accept() not valid in this state");
    ac.protocol_version = 1;
    ac.called_ae = rq_.called_ae;
    ac.calling_ae = rq_.calling_ae;
    ac.application_context = rq_.application_context;
    if (ac.user.max_length == 0) ac.user.max_length = lim_.max_receive_pdu;
    if (ac.user.implementation_class_uid.empty()) ac.user.implementation_class_uid = std::string(kImplementationClassUid);
    if (ac.user.implementation_version_name.empty())
        ac.user.implementation_version_name = std::string(kImplementationVersionName);
    if (rq_.user.async_window && !ac.user.async_window) ac.user.async_window = AsyncWindow{1, 1};
    (void)fire(E::Evt7);  // AE-7
    ac_ = std::move(ac);
    contexts_ = std::move(contexts);
    if (Status s = send_pdu(ac_); !s) {
        close_transport();
        state_ = S::Sta1;
        return s;
    }
    return Status::ok();
}

void Association::reject(const AssociateRj& r) {
    if (state_ != S::Sta3) return;
    (void)fire(E::Evt8);  // AE-8
    (void)send_pdu(r);
    enter_sta13_and_wait();
}

// ---- data transfer

Status Association::send_message(std::uint8_t context_id, const std::uint8_t* command, std::size_t command_len,
                                 const std::uint8_t* data, std::size_t data_len) {
    if (state_ != S::Sta6 && state_ != S::Sta8) return Status(Errc::InvalidArgument, "association not established");
    const NegotiatedContext* pc = context(context_id);
    if (!pc || !pc->accepted()) return Status(Errc::InvalidArgument, "presentation context not accepted");
    if (Status s = fire(E::Evt9); !s) return s;  // DT-1 / AR-7
    std::uint64_t max_data = peer_max_pdu_ == 0 ? lim_.max_receive_pdu : peer_max_pdu_;
    max_data = std::min<std::uint64_t>(max_data, 0xFFFFFFF0u) - 6;  // PDV item header (4) + context/control (2)
    std::vector<std::uint8_t> buf;
    auto send_part = [&](const std::uint8_t* p, std::size_t n, bool cmd) -> Status {
        std::size_t done = 0;
        do {
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(n - done, max_data));
            const bool last = done + chunk == n;
            buf.resize(12 + chunk);
            encode_single_pdv_header(context_id, cmd, last, static_cast<std::uint32_t>(chunk), buf.data());
            if (chunk) std::memcpy(buf.data() + 12, p + done, chunk);
            if (Status s = transport_->write_all(buf.data(), buf.size(), to_.write_ms); !s) return s;
            done += chunk;
        } while (done < n);
        return Status::ok();
    };
    Status s = send_part(command, command_len, true);
    if (s && data) s = send_part(data, data_len, false);
    if (!s) {
        close_transport();
        state_ = S::Sta1;
    }
    return s;
}

bool Association::data_pending(int timeout_ms) {
    if (!transport_ || !transport_->is_open()) return false;
    Result<bool> r = transport_->wait_readable(timeout_ms);
    return r && *r;
}

namespace {
// Command Field (0000,0100) of a command set (Implicit VR Little Endian, PS3.7 6.3.1), or 0.
std::uint16_t command_field_of(const Bytes& command) noexcept {
    const std::uint8_t* p = command.data();
    std::size_t n = command.size();
    while (n >= 8) {
        const std::uint16_t group = load_le16(p), element = load_le16(p + 2);
        const std::uint32_t len = load_le32(p + 4);
        if (len > n - 8) return 0;
        if (group == 0x0000 && element == 0x0100) return len >= 2 ? load_le16(p + 8) : std::uint16_t{0};
        if (group != 0x0000) return 0;
        p += 8 + static_cast<std::size_t>(len);
        n -= 8 + static_cast<std::size_t>(len);
    }
    return 0;
}
}  // namespace

Result<Received> Association::receive(int timeout_ms, const HasDatasetFn& has_dataset) {
    return receive(timeout_ms, has_dataset, nullptr, {});
}

Result<Received> Association::receive(int timeout_ms, const HasDatasetFn& has_dataset, const SpoolConfig* spool,
                                      const SpoolPrefixFn& prefix) {
    if (state_ != S::Sta6) return Status(Errc::InvalidArgument, "association not established");
    if (interrupted_.load()) return handle_unexpected(E::Evt17, "interrupted");
    // Wait for the first byte without disturbing the stream; a timeout here is not an error
    // of the association and leaves it usable.
    {
        Result<bool> ready = transport_->wait_readable(timeout_ms);
        if (!ready) return handle_unexpected(E::Evt17, "transport error");
        if (!*ready) return Status(Errc::Timeout, "no message received");
    }
    enum class Phase { Command, Data } phase = Phase::Command;
    Accumulator acc;
    RawMessage msg;
    std::unique_ptr<SpoolWriter> sink;  // data set going to disk
    bool discarding = false;            // over the spool quota: drain the data set, keep nothing
    std::uint64_t discarded_bytes = 0;
    bool started = false;
    const SpoolConfig* const store_spool = spool;
    bool is_store = true;  // decided from the command set once it is complete (R-109)
    while (true) {
        UlEvent evt = E::Evt18;
        Result<Pdu> r = next_pdu(started ? to_.dimse_ms : std::max(to_.dimse_ms, 1), evt);
        if (evt == E::Evt18) {
            // Timed out inside a PDU or between fragments: the stream cannot be resynchronised.
            abort(abort_src::Provider, abort_reason::NotSpecified);
            return Status(Errc::Timeout, "timed out receiving message");
        }
        if (evt == E::Evt12) {
            (void)fire(evt);  // AR-2: indicate release
            Received rec;
            rec.kind = ReceiveKind::PeerReleaseRequest;
            return rec;
        }
        if (evt != E::Evt10) return handle_unexpected(evt, r ? "unexpected PDU during data transfer"
                                                             : r.error().message.c_str());
        (void)fire(evt);  // DT-2
        const PDataTf& pd = std::get<PDataTf>(r.value());
        for (std::size_t i = 0; i < pd.pdvs.size(); ++i) {
            const Pdv& v = pd.pdvs[i];
            const NegotiatedContext* pc = context(v.context_id);
            const bool bad_ctx = !pc || !pc->accepted() || (started && v.context_id != msg.context_id);
            const bool bad_order = (phase == Phase::Command && !v.command) || (phase == Phase::Data && v.command);
            if (bad_ctx || bad_order) {
                pending_abort_reason_ = abort_reason::InvalidPduParameterValue;
                return handle_unexpected(E::Evt19, bad_ctx ? "PDV for an unknown or different presentation context"
                                                           : "PDV out of order");
            }
            if (!started) {
                started = true;
                msg.context_id = v.context_id;
            }
            // With a spool, the in-memory part never needs to exceed the spool threshold.
            // R-109: only C-STORE data sets may be large (and spooled); every other data set is
            // bounded by max_service_dataset_size while it arrives.
            const std::uint64_t cap = phase == Phase::Command ? lim_.max_command_size
                                      : !is_store ? lim_.max_service_dataset_size
                                      : spool ? std::max(lim_.max_dataset_size, spool->threshold)
                                              : lim_.max_dataset_size;
            // Over the disk quota or free-space floor (R-074): the rest of the data set is read and
            // dropped so the association stays usable and the peer gets 0xA700.
            auto start_discarding = [&](std::uint64_t so_far) {
                sink.reset();  // removes the partial file and its quota charge
                acc.clear();
                discarding = true;
                discarded_bytes = so_far;
            };
            if (phase == Phase::Data && spool && !sink && !discarding && acc.total() + v.data.size() > spool->threshold) {
                Result<std::vector<std::uint8_t>> pre = prefix ? prefix(msg.command, *pc) : std::vector<std::uint8_t>{};
                Result<std::unique_ptr<SpoolWriter>> w =
                    pre ? SpoolWriter::open(*spool, pre.value()) : Result<std::unique_ptr<SpoolWriter>>(pre.take_error());
                if (!w && w.error().code == Errc::LimitExceeded) {
                    start_discarding(acc.total());
                } else if (!w) {
                    abort(abort_src::Provider, abort_reason::NotSpecified);
                    return Status(w.error().code, "cannot spool data set: " + w.error().message);
                } else {
                    sink = std::move(w.value());
                    if (Status ws = sink->write(acc.data(), static_cast<std::size_t>(acc.total())); !ws) {
                        if (ws.code != Errc::LimitExceeded) {
                            abort(abort_src::Provider, abort_reason::NotSpecified);
                            return ws;
                        }
                        start_discarding(acc.total());
                    } else {
                        acc.clear();
                    }
                }
            }
            if (discarding) {
                discarded_bytes += v.data.size();
                if (discarded_bytes > spool->max_size) {
                    abort(abort_src::Provider, abort_reason::NotSpecified);
                    return Status(Errc::LimitExceeded, "data set too large");
                }
            } else if (sink) {
                if (sink->data_bytes() + v.data.size() > spool->max_size) {
                    abort(abort_src::Provider, abort_reason::NotSpecified);
                    return Status(Errc::LimitExceeded, "data set too large");
                }
                if (Status ws = sink->write(v.data.data(), v.data.size()); !ws) {
                    if (ws.code != Errc::LimitExceeded) {
                        abort(abort_src::Provider, abort_reason::NotSpecified);
                        return ws;
                    }
                    start_discarding(sink->data_bytes() + v.data.size());
                }
            } else if (!acc.add(v.data, cap)) {
                abort(abort_src::Provider, abort_reason::NotSpecified);
                return Status(Errc::LimitExceeded, phase == Phase::Command ? "command set too large"
                                                                          : "data set too large");
            }
            if (!v.last) continue;
            bool complete = false;
            if (phase == Phase::Command) {
                msg.command = acc.take();
                Result<bool> hd = has_dataset(msg.command);
                if (!hd) {
                    abort(abort_src::Provider, abort_reason::InvalidPduParameterValue);
                    return Status(Errc::Malformed, "invalid command set: " + hd.error().message);
                }
                msg.has_data = *hd;
                is_store = command_field_of(msg.command) == 0x0001;  // C-STORE-RQ
                spool = is_store ? store_spool : nullptr;
                if (msg.has_data) phase = Phase::Data;
                else complete = true;
            } else if (discarding) {
                msg.discarded = true;
                complete = true;
            } else if (sink) {
                Result<SpoolWriter::Finished> fin = sink->finish();
                sink.reset();
                if (!fin) {
                    abort(abort_src::Provider, abort_reason::NotSpecified);
                    return fin.take_error();
                }
                msg.data = std::move(fin->data);
                msg.spool = std::move(fin->file);
                complete = true;
            } else {
                msg.data = acc.take();
                complete = true;
            }
            if (complete) {
                if (i + 1 != pd.pdvs.size()) {
                    // Without asynchronous operations a P-DATA-TF never carries the start of the
                    // next message after a completed one; treat as a protocol violation.
                    pending_abort_reason_ = abort_reason::InvalidPduParameterValue;
                    return handle_unexpected(E::Evt19, "PDVs after the end of a message");
                }
                Received rec;
                rec.kind = ReceiveKind::Message;
                rec.message = std::move(msg);
                return rec;
            }
        }
    }
}

// ---- release / abort

Status Association::release() {
    if (state_ != S::Sta6) return Status(Errc::InvalidArgument, "release() requires an established association");
    (void)fire(E::Evt11);  // AR-1
    if (Status s = send_pdu(ReleaseRq{}); !s) {
        close_transport();
        state_ = S::Sta1;
        return s;
    }
    while (true) {
        UlEvent evt = E::Evt18;
        Result<Pdu> r = next_pdu(to_.acse_ms, evt);
        if (evt == E::Evt18) {
            abort();
            return Status(Errc::Timeout, "no A-RELEASE-RP");
        }
        const UlTransition t = ul_transition(state_, evt, requestor_);
        switch (t.action) {
            case A::AR3:  // release confirmed, close
                (void)fire(evt);
                return Status::ok();
            case A::AR6:  // P-DATA during release: indicated to nobody (the user asked to release)
                (void)fire(evt);
                break;
            case A::AR8:  // release collision
                (void)fire(evt);
                if (state_ == S::Sta9) {
                    (void)fire(E::Evt14);  // AR-9: send A-RELEASE-RP, now Sta11
                    if (Status s = send_pdu(ReleaseRp{}); !s) {
                        close_transport();
                        state_ = S::Sta1;
                        return s;
                    }
                }
                break;
            case A::AR10:  // acceptor in collision: release confirmed, now answer
                (void)fire(evt);
                (void)fire(E::Evt14);  // AR-4: send A-RELEASE-RP, Sta13
                (void)send_pdu(ReleaseRp{});
                enter_sta13_and_wait();
                return Status::ok();
            default:
                return handle_unexpected(evt, r ? "unexpected PDU during release" : r.error().message.c_str());
        }
    }
}

Status Association::respond_release() {
    if (state_ != S::Sta8) return Status(Errc::InvalidArgument, "no release request to answer");
    (void)fire(E::Evt14);  // AR-4
    Status s = send_pdu(ReleaseRp{});
    enter_sta13_and_wait();
    return s;
}

void Association::abort(std::uint8_t source, std::uint8_t reason) {
    if (state_ == S::Sta1) {
        close_transport();
        return;
    }
    const UlTransition t = ul_transition(state_, E::Evt15, requestor_);
    if (t.action == A::AA2 || t.action == A::Invalid) {
        close_transport();
        state_ = S::Sta1;
        return;
    }
    state_ = S::Sta13;  // AA-1
    (void)send_pdu(Abort{source, reason});
    enter_sta13_and_wait();
}

}  // namespace siaara::net
