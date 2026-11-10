// SIAARA DICOM Toolkit - service class provider server (PS3.4 Annexes A, B, C; PS3.7 9.1, 10.1).
#include "net/scp.hpp"

#include <utility>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <chrono>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/services.hpp"
#include "net/store_support.hpp"

namespace siaara::net {

namespace {

std::string trimmed_ae(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

bool is_native(std::string_view ts_uid) {
    const TransferSyntaxInfo* t = find_transfer_syntax(ts_uid);
    return t && !t->encapsulated;
}

std::vector<std::string> native_subset(const std::vector<std::string>& ts) {
    std::vector<std::string> out;
    for (const std::string& t : ts)
        if (is_native(t)) out.push_back(t);
    if (out.empty()) out = native_transfer_syntaxes();
    return out;
}

constexpr Tag kFailedSopInstanceUidList = 0x00080058;

std::uint16_t final_retrieve_status(std::uint16_t total, std::uint16_t completed, std::uint16_t failed,
                                    std::uint16_t warning) {
    if (total > 0 && failed == total) return dimse_status::OutOfResourcesSubOps;
    if (failed > 0 || warning > 0 || completed + failed + warning < total) return dimse_status::SubOperationsWarning;
    return dimse_status::Success;
}

}  // namespace

struct ScpServer::Ctx {
    Association& a;
    bool dead = false;             // association failed; stop serving
    bool release_pending = false;  // peer asked to release while an operation was running
};

ScpServer::ScpServer(ScpConfig cfg, ScpHandlers handlers) : cfg_(std::move(cfg)), h_(std::move(handlers)) {
    const std::vector<std::string> native = native_subset(cfg_.transfer_syntaxes);
    if (!cfg_.accepted.empty()) {
        policy_ = cfg_.accepted;
    } else {
        if (h_.on_store || h_.on_get)
            for (const std::string& s : storage_sop_classes())
                policy_.push_back({s, cfg_.transfer_syntaxes, static_cast<bool>(h_.on_store), static_cast<bool>(h_.on_get)});
        if (h_.on_find)
            for (std::string_view s : {sop::PatientRootFind, sop::StudyRootFind, sop::ModalityWorklistFind})
                policy_.push_back({std::string(s), native, true, false});
        if (h_.on_move)
            for (std::string_view s : {sop::PatientRootMove, sop::StudyRootMove})
                policy_.push_back({std::string(s), native, true, false});
        if (h_.on_get)
            for (std::string_view s : {sop::PatientRootGet, sop::StudyRootGet})
                policy_.push_back({std::string(s), native, true, false});
        if (h_.on_move) policy_.push_back({std::string(sop::CompositeInstanceRootMove), native, true, false});
        if (h_.on_get)
            for (std::string_view s : {sop::CompositeInstanceRootGet, sop::CompositeInstanceRetrieveWithoutBulkDataGet})
                policy_.push_back({std::string(s), native, true, false});
    }
    if (h_.services)
        for (AcceptedSyntax& a : service_policy(*h_.services, native)) {
            const bool known = std::any_of(policy_.begin(), policy_.end(), [&](const AcceptedSyntax& x) {
                return x.abstract_syntax == a.abstract_syntax;
            });
            if (!known) policy_.push_back(std::move(a));
        }
    const bool has_verification = std::any_of(policy_.begin(), policy_.end(), [](const AcceptedSyntax& a) {
        return a.abstract_syntax == sop::Verification;
    });
    if (!has_verification) policy_.push_back({std::string(sop::Verification), native, true, false});
    if (cfg_.accept_any_sop_class)
        any_policy_ = AcceptedSyntax{"", cfg_.transfer_syntaxes, true, static_cast<bool>(h_.on_get)};
    if (cfg_.max_associations == 0) cfg_.max_associations = 1;
}

ScpServer::~ScpServer() { stop(); }

void ScpServer::log(const std::string& s) const {
    if (h_.log) h_.log(s);
}

Status ScpServer::start() {
    std::lock_guard<std::mutex> lock(stop_mu_);
    if (running_.load()) return Status(Errc::InvalidArgument, "server already running");
    if (!is_valid_ae_title(cfg_.ae_title)) return Status(Errc::InvalidArgument, "invalid AE title");
    if (cfg_.allowed_calling_aes.empty() && !cfg_.allow_any_calling_ae)
        return Status(Errc::InvalidArgument,
                      "no allowed calling AE titles: set allowed_calling_aes, or allow_any_calling_ae to accept anyone");
    if (cfg_.allowed_calling_aes.empty()) log("warning: accepting associations from any calling AE title");
    if (cfg_.tls.enabled) {
        TlsSettings ts = cfg_.tls;
        if (!ts.log && h_.log) ts.log = h_.log;
        Result<std::shared_ptr<TlsContext>> ctx = TlsContext::create(ts, TlsContext::Role::Server);
        if (!ctx) return ctx.take_error();
        tls_wipe_secret(ts.identity.password);
        tls_wipe_secret(cfg_.tls.identity.password);  // the context keeps (and later wipes) its own copy
        if (Status s = tls_listener_.listen(std::move(ctx).value(), cfg_.bind_address, cfg_.port); !s) return s;
        port_ = tls_listener_.port();
    } else {
        if (Status s = listener_.listen(cfg_.bind_address, cfg_.port); !s) return s;
        port_ = listener_.port();
    }
    stopping_.store(false);
    running_.store(true);
    // R-117: event reports that need a new association go through one sender thread.
    if (h_.services) h_.services->sender = std::make_shared<EventSender>(*h_.services, cfg_, h_.log);
    workers_.reserve(cfg_.max_associations);
    for (std::size_t i = 0; i < cfg_.max_associations; ++i) workers_.emplace_back([this] { worker_loop(); });
    reject_thread_ = std::thread([this] { reject_loop(); });
    accept_thread_ = std::thread([this] { accept_loop(); });
    return Status::ok();
}

void ScpServer::stop() {
    std::lock_guard<std::mutex> lock(stop_mu_);
    if (!running_.load()) return;
    {
        std::lock_guard<std::mutex> l(mu_);
        stopping_.store(true);
        for (Association* a : live_) a->interrupt();
        for (Transport* t : handshaking_) t->shutdown();
    }
    cv_.notify_all();
    reject_cv_.notify_all();
    // The sender's outbound association is aborted and its thread joined first: stop() never
    // waits for a peer that does not answer an event report (R-117).
    if (h_.services && h_.services->sender) h_.services->sender->stop();
    if (accept_thread_.joinable()) accept_thread_.join();
    if (reject_thread_.joinable()) reject_thread_.join();
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    if (h_.services) h_.services->sender.reset();  // no association thread is left to use it
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& t : queue_) t->close();
        queue_.clear();
        for (auto& t : reject_queue_) t->close();
        reject_queue_.clear();
        per_ip_.clear();
    }
    listener_.close();
    tls_listener_.close();
    running_.store(false);
}

std::size_t ScpServer::active_associations() const {
    std::lock_guard<std::mutex> l(mu_);
    return busy_;
}

bool ScpServer::register_assoc(Association* a) {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_.load()) return false;
    live_.insert(a);
    return true;
}

void ScpServer::unregister_assoc(Association* a) {
    std::lock_guard<std::mutex> l(mu_);
    live_.erase(a);
}

void ScpServer::accept_loop() {
    while (!stopping_.load()) {
        Result<std::unique_ptr<Transport>> t = Status(Errc::Timeout, "");
        if (cfg_.tls.enabled) {
            Result<std::unique_ptr<TlsTransport>> s = tls_listener_.accept(cfg_.accept_poll_ms);
            if (s) t = std::unique_ptr<Transport>(std::move(s).value());
            else t = s.take_error();
        } else {
            t = listener_.accept(cfg_.accept_poll_ms);
        }
        if (!t) {
            if (t.error().code == Errc::Timeout) continue;
            log("accept failed: " + t.error().message);
            continue;
        }
        const std::string peer = peer_group(t.value()->peer_address(), cfg_.peer_ipv6_prefix_bits);
        bool full = false;
        {
            std::lock_guard<std::mutex> l(mu_);
            full = busy_ + queue_.size() >= cfg_.max_associations ||
                   (cfg_.max_associations_per_ip != 0 && per_ip_[peer] >= cfg_.max_associations_per_ip);
            if (!full) {
                ++per_ip_[peer];
                queue_.push_back(std::move(t.value()));
            } else if (per_ip_[peer] == 0) {
                per_ip_.erase(peer);
            }
        }
        if (full) quick_reject(std::move(t.value()));
        else cv_.notify_one();
    }
}

void ScpServer::release_peer(const std::string& peer) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = per_ip_.find(peer);
    if (it == per_ip_.end()) return;
    if (it->second <= 1) per_ip_.erase(it);
    else --it->second;
}

// Runs on the accept thread: never reads from or waits on the peer (NET-03).
void ScpServer::quick_reject(std::unique_ptr<Transport> t) {
    rejected_.fetch_add(1);
    if (cfg_.tls.enabled) {
        // An A-ASSOCIATE-RJ would need a TLS handshake; just close.
        log("connection from " + t->peer_address() + " closed: local limit exceeded");
        return;
    }
    constexpr std::size_t kMaxPendingRejects = 16;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!stopping_.load() && reject_queue_.size() < kMaxPendingRejects) {
            reject_queue_.push_back(std::move(t));
            reject_cv_.notify_one();
            return;
        }
    }
    log("connection from " + t->peer_address() + " closed: local limit exceeded");
}

// Answers refused connections with A-ASSOCIATE-RJ (transient, local limit exceeded) without
// parsing anything they send: the RJ goes out at once, then whatever the peer sent is drained
// for a short, absolute time (so closing does not reset the connection before the RJ is read).
void ScpServer::reject_loop() {
    std::vector<std::uint8_t> rj_pdu;
    (void)encode_pdu(AssociateRj{rj::Transient, rj::SourcePresentationProvider, rj::PresentationLocalLimitExceeded},
                     rj_pdu);
    while (true) {
        std::unique_ptr<Transport> t;
        {
            std::unique_lock<std::mutex> l(mu_);
            reject_cv_.wait(l, [this] { return stopping_.load() || !reject_queue_.empty(); });
            if (stopping_.load()) return;
            t = std::move(reject_queue_.front());
            reject_queue_.pop_front();
        }
        log("association from " + t->peer_address() + " rejected: local limit exceeded");
        if (t->write_all(rj_pdu.data(), rj_pdu.size(), 200)) {
            const Deadline until = deadline_after(std::min(cfg_.timeouts.artim_ms, 500));
            std::uint8_t sink[4096];
            while (!stopping_.load()) {
                Result<std::size_t> r = t->read_some(sink, sizeof sink, std::min(remaining_ms(until), 50));
                if (r && *r == 0) break;                                 // peer closed
                if (!r && r.error().code != Errc::Timeout) break;        // reset or error
                if (std::chrono::steady_clock::now() >= until) break;
            }
        }
        t->close();
    }
}

void ScpServer::worker_loop() {
    while (true) {
        std::unique_ptr<Transport> t;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stopping_.load() || !queue_.empty(); });
            if (stopping_.load()) return;
            t = std::move(queue_.front());
            queue_.pop_front();
            ++busy_;
        }
        serve(std::move(t));
        std::lock_guard<std::mutex> l(mu_);
        --busy_;
    }
}

void ScpServer::serve(std::unique_ptr<Transport> t) {
    const std::string peer = peer_group(t->peer_address(), cfg_.peer_ipv6_prefix_bits);
    struct PeerSlot {
        ScpServer* s;
        const std::string& p;
        ~PeerSlot() { s->release_peer(p); }
    } slot{this, peer};
    if (cfg_.tls.enabled) {
        // The handshake runs here, on the pool thread; stop() shuts the socket down to end it early.
        {
            std::lock_guard<std::mutex> l(mu_);
            if (stopping_.load()) return;
            handshaking_.insert(t.get());
        }
        Status hs = static_cast<TlsTransport*>(t.get())->handshake(cfg_.tls.handshake_timeout_ms);
        {
            std::lock_guard<std::mutex> l(mu_);
            handshaking_.erase(t.get());
        }
        if (!hs) {
            log(hs.message);
            return;
        }
    }
    Result<std::unique_ptr<Association>> ar = Association::receive_request(std::move(t), cfg_.timeouts, cfg_.limits);
    if (!ar) {
        log("association setup from " + peer + " failed: " + ar.error().message);
        return;
    }
    std::unique_ptr<Association> owner = std::move(ar.value());
    Association& a = *owner;
    if (!register_assoc(&a)) return;  // stopping: the destructor aborts and closes at once
    struct Unregister {
        ScpServer* s;
        Association* a;
        ~Unregister() { s->unregister_assoc(a); }
    } guard{this, &a};

    const AssociateRq& rq = a.request_pdu();
    auto reject = [&](std::uint8_t result, std::uint8_t source, std::uint8_t reason, const char* why) {
        rejected_.fetch_add(1);
        log(std::string("association from ") + rq.calling_ae + " rejected: " + why);
        a.reject({result, source, reason});
    };
    if ((rq.protocol_version & 1u) == 0)
        return reject(rj::Permanent, rj::SourceAcseProvider, rj::AcseProtocolVersionNotSupported, "protocol version");
    if (rq.application_context != kDicomApplicationContext)
        return reject(rj::Permanent, rj::SourceUser, rj::UserAppContextNotSupported, "application context");
    if (cfg_.require_called_ae && rq.called_ae != cfg_.ae_title)
        return reject(rj::Permanent, rj::SourceUser, rj::UserCalledAeNotRecognized, "called AE title");
    if (!cfg_.allowed_calling_aes.empty() &&
        std::find(cfg_.allowed_calling_aes.begin(), cfg_.allowed_calling_aes.end(), rq.calling_ae) ==
            cfg_.allowed_calling_aes.end())
        return reject(rj::Permanent, rj::SourceUser, rj::UserCallingAeNotRecognized, "calling AE title");

    AssociateAc ac;
    std::vector<NegotiatedContext> contexts =
        negotiate_contexts(rq, policy_, any_policy_ ? &*any_policy_ : nullptr, ac);
    if (!cfg_.extended_negotiation.empty())
        ac.user.extended = answer_extended_negotiation(rq.user.extended, cfg_.extended_negotiation);
    if (rq.user.async_window) {
        // Requests are performed one at a time in arrival order, so any window up to the limit works.
        const std::uint16_t want = rq.user.async_window->max_invoked, lim = cfg_.max_async_operations;
        AsyncWindow w;
        w.max_invoked = want == 0 ? lim : (lim == 0 ? want : std::min(want, lim));
        w.max_performed = 1;
        ac.user.async_window = w;
    }
    if (rq.user.identity_rq && h_.on_identity) {
        std::vector<std::uint8_t> response;
        if (!h_.on_identity(*rq.user.identity_rq, response))
            return reject(rj::Permanent, rj::SourceUser, rj::UserNoReason, "user identity");
        if (rq.user.identity_rq->positive_response_requested) ac.user.identity_ac = UserIdentityAc{std::move(response)};
    }
    if (Status s = a.accept(std::move(ac), std::move(contexts)); !s) {
        log("sending A-ASSOCIATE-AC failed: " + s.message);
        return;
    }
    handled_.fetch_add(1);
    log("association accepted from " + rq.calling_ae + " at " + peer);

    Ctx c{a};
    const AssociationIdScope id_scope(a.id());  // handlers and events of this thread name the association
    const bool limited_life = cfg_.max_association_lifetime_ms > 0;
    const Deadline end_of_life = deadline_after(limited_life ? cfg_.max_association_lifetime_ms : 0);
    std::uint64_t operations = 0;
    while (!c.dead) {
        int wait_ms = cfg_.timeouts.idle_ms;
        if (limited_life) {
            const int left = remaining_ms(end_of_life);
            if (left == 0) {
                log("association lifetime limit reached; aborting association");
                a.abort(abort_src::Provider, abort_reason::NotSpecified);
                break;
            }
            wait_ms = std::min(wait_ms, left);
        }
        Result<DimseReceived> r = receive_dimse(a, wait_ms, cfg_.spool ? &*cfg_.spool : nullptr);
        if (!r && r.error().code == Errc::Timeout && a.established() && limited_life && remaining_ms(end_of_life) == 0)
            continue;  // reported (and aborted) at the top of the loop
        if (!r) {
            if (r.error().code == Errc::Timeout && a.established()) {
                log("idle timeout; aborting association");
                a.abort(abort_src::Provider, abort_reason::NotSpecified);
            } else {
                log("association ended: " + r.error().message);
            }
            break;
        }
        if (r->kind == DimseKind::PeerReleaseRequest) {
            (void)a.respond_release();
            break;
        }
        if (!dispatch(c, r->message)) break;
        if (cfg_.max_operations_per_association != 0 && ++operations >= cfg_.max_operations_per_association &&
            !c.release_pending) {
            log("association operation limit reached; aborting association");
            a.abort(abort_src::Provider, abort_reason::NotSpecified);
            break;
        }
        if (c.release_pending) {
            (void)a.respond_release();
            break;
        }
    }
    if (a.established()) a.abort(abort_src::Provider, abort_reason::NotSpecified);
}

bool ScpServer::dispatch(Ctx& c, Message& m) {
    const Command& rq = m.command;
    if (rq.is_response()) {
        log("unexpected DIMSE response from requestor; aborting");
        c.a.abort(abort_src::Provider, abort_reason::UnexpectedPdu);
        return false;
    }
    if (rq.command_field == cmd::CCancelRq) return true;  // nothing running: ignore
    if (Status v = validate_command(rq); !v) {
        if (v.code == Errc::Unsupported && rq.message_id) {
            // PS3.7 C.4.2: unknown operation -> 0211 (Unrecognized Operation), association kept.
            log("unrecognized DIMSE command; replying 0211");
            Command rsp = make_response(rq, dimse_status::UnrecognizedOperation);
            if (Status s = send_dimse(c.a, m.context_id, rsp); !s) return false;
            return true;
        }
        log("invalid DIMSE request: " + v.message);
        c.a.abort(abort_src::Provider, abort_reason::InvalidPduParameterValue);
        return false;
    }
    if (h_.services) {
        ServiceContext sc{c.a, cfg_, h_.log};
        const ServiceOutcome o = dispatch_service(*h_.services, sc, m);
        if (sc.release_pending) c.release_pending = true;
        if (o == ServiceOutcome::Failed) return false;
        if (o == ServiceOutcome::Handled) return !c.dead;
    }
    Status s = Status::ok();
    switch (rq.command_field) {
        case cmd::CEchoRq: s = do_echo(c, m); break;
        case cmd::CStoreRq: s = do_store(c, m); break;
        case cmd::CFindRq: s = do_find(c, m); break;
        case cmd::CMoveRq: s = do_retrieve(c, m, false); break;
        case cmd::CGetRq: s = do_retrieve(c, m, true); break;
        default: s = do_n(c, m); break;
    }
    if (!s) {
        log("DIMSE service failed: " + s.message);
        return false;
    }
    return !c.dead;
}

Status ScpServer::do_echo(Ctx& c, const Message& m) {
    const std::uint16_t st = h_.on_echo ? h_.on_echo(c.a.calling_ae()) : dimse_status::Success;
    return send_dimse(c.a, m.context_id, make_response(m.command, st));
}

Status ScpServer::do_store(Ctx& c, const Message& m) {
    const NegotiatedContext* pc = c.a.context(m.context_id);
    std::uint16_t st = dimse_status::Success;
    std::string error_comment;
    if (!pc || !pc->requestor_scu || pc->abstract_syntax != m.command.affected_sop_class_uid || !h_.on_store) {
        st = dimse_status::SopClassNotSupported;
    } else {
        StoreRequest rq;
        rq.calling_ae = c.a.calling_ae();
        rq.called_ae = c.a.called_ae();
        rq.peer_address = c.a.peer_address();
        rq.sop_class_uid = m.command.affected_sop_class_uid;
        rq.sop_instance_uid = m.command.affected_sop_instance_uid;
        rq.transfer_syntax = m.transfer_syntax;
        rq.move_originator_ae = m.command.move_originator_ae;
        rq.move_originator_message_id = m.command.move_originator_message_id;
        rq.raw = m.data;
        rq.spool = m.spool;
        // R-116: the read keeps its loss bits and warnings.
        Result<DicomFile> ds = m.discarded ? Result<DicomFile>(Status(Errc::LimitExceeded, "discarded"))
                               : m.has_data ? read_dataset_report(m.data, m.transfer_syntax, network_read_options())
                                            : Result<DicomFile>(DicomFile{});
        if (m.discarded) {
            st = dimse_status::OutOfResources;  // R-074: spool disk quota or free-space floor
            log("C-STORE: data set not kept: spool disk quota or free-space floor reached (0xA700)");
        } else if (!ds) {
            st = dimse_status::CannotUnderstand;
        } else {
            rq.dataset = std::move(ds->dataset);
            rq.read_loss = ds->read_loss;
            rq.read_loss_offset = ds->read_loss_offset;
            if (!ds->warnings.empty()) rq.read_warning = ds->warnings.front();
            std::string note;
            st = check_store_uids(rq, cfg_.strict_store_uids, note);
            // UIDs stay out of this log (they identify patient data); StoreRequest carries both sets.
            if (!note.empty())
                log(st == dimse_status::Success
                        ? "C-STORE: command UIDs differ from the data set's; accepted under the data set's UIDs"
                        : "C-STORE: command UIDs differ from the data set's; refused (0xA900)");
        }
        const auto kept_answer = [this]() -> std::uint16_t {
            switch (cfg_.quarantine_response) {
                case QuarantineResponse::Failure: return cfg_.incomplete_failure_status;
                case QuarantineResponse::Success: return dimse_status::Success;
                default: return cfg_.incomplete_warning_status;
            }
        };
        IncompletePolicy policy = cfg_.incomplete_policy;
        if (const auto by = cfg_.incomplete_policy_by_caller.find(trimmed_ae(rq.calling_ae));
            by != cfg_.incomplete_policy_by_caller.end())
            policy = by->second;
        if (policy == IncompletePolicy::Quarantine && !h_.on_store_quarantine) policy = IncompletePolicy::Refuse;
        if (ds && st == dimse_status::Success && rq.read_loss != 0 && policy == IncompletePolicy::Quarantine) {
            // Kept verbatim outside the archive; the sender learns that it is not a normal store.
            const std::uint16_t answer = kept_answer();
            const std::uint16_t kept = h_.on_store_quarantine(rq, answer);
            st = kept == dimse_status::Success ? answer : kept;
            error_comment = kept == dimse_status::Success ? "data set incomplete: quarantined" : "data set incomplete";
            log(std::string("C-STORE: incomplete data set ") +
                (kept == dimse_status::Success ? "quarantined" : "not quarantined (quarantine full or failed)") + " (" +
                (rq.read_warning.empty() ? std::string("read loss") : rq.read_warning) + ")");
        } else if (ds && st == dimse_status::Success && rq.read_loss != 0 &&
                   (policy == IncompletePolicy::Refuse || policy == IncompletePolicy::RefuseKeepCopy)) {
            // The data set arrived truncated or damaged: storing it would keep less than the
            // sender holds while answering Success.
            st = cfg_.incomplete_failure_status;
            if (policy == IncompletePolicy::RefuseKeepCopy && h_.on_store_keep_rejected) h_.on_store_keep_rejected(rq);
            error_comment = "data set incomplete";
            const std::string why = std::string(kIncompleteInput) + ": the received data set was not read completely (" +
                                    (rq.read_warning.empty() ? std::string("read loss") : rq.read_warning) +
                                    "; first at offset " + std::to_string(rq.read_loss_offset) + "); refused";
            log("C-STORE: " + why);
            if (h_.on_store_refused) h_.on_store_refused(rq, st, why);
        } else if (ds && st == dimse_status::Success) {
            if (rq.read_loss != 0) {
                // Accepted as received: the bytes are kept verbatim, never re-encoded from the parse.
                log("C-STORE: incomplete data set accepted as received (" +
                    (rq.read_warning.empty() ? std::string("read loss") : rq.read_warning) + "); not converted");
            } else if (!cfg_.preferred_store_ts.empty() && rq.transfer_syntax != cfg_.preferred_store_ts) {
                std::string history_note;
                if (Status cs = convert_received(rq, cfg_.preferred_store_ts, cfg_.transcode_params,
                                                 cfg_.spool ? &*cfg_.spool : nullptr, &history_note); !cs)
                    log("kept received transfer syntax: " + cs.message);
                else if (!history_note.empty()) log("C-STORE conversion: " + history_note);
            }
            st = h_.on_store(rq);
            // Accepted although incomplete: stored verbatim and flagged; the sender is told.
            if (rq.read_loss != 0 && st == dimse_status::Success) {
                st = kept_answer();
                error_comment = "data set incomplete: stored as received";
            }
        }
    }
    Command rsp = make_response(m.command, st);
    rsp.error_comment = error_comment;
    return send_dimse(c.a, m.context_id, rsp);
}

bool ScpServer::cancel_requested(Ctx& c, std::uint16_t mid) {
    if (c.dead || c.release_pending) return true;
    if (!c.a.data_pending(0)) return false;
    Result<DimseReceived> r = receive_dimse(c.a, cfg_.timeouts.dimse_ms);
    if (!r) {
        c.dead = true;
        return true;
    }
    if (r->kind == DimseKind::PeerReleaseRequest) {
        c.release_pending = true;
        return true;
    }
    const Command& k = r->message.command;
    if (k.command_field == cmd::CCancelRq && k.message_id_being_responded_to == mid) return true;
    log("unexpected DIMSE message during an operation; aborting");
    c.a.abort(abort_src::Provider, abort_reason::UnexpectedPdu);
    c.dead = true;
    return true;
}

Status ScpServer::do_find(Ctx& c, const Message& m) {
    const NegotiatedContext* pc = c.a.context(m.context_id);
    if (!pc || pc->abstract_syntax != m.command.affected_sop_class_uid || !h_.on_find)
        return send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::SopClassNotSupported));
    Result<Dataset> ident = decode_message_dataset(m);
    if (!ident) return send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::CannotUnderstand));
    QueryRequest q;
    q.calling_ae = c.a.calling_ae();
    q.peer_address = c.a.peer_address();
    q.sop_class_uid = m.command.affected_sop_class_uid;
    q.message_id = *m.command.message_id;
    q.priority = m.command.priority.value_or(0);
    q.identifier = std::move(ident.value());
    for (const ExtendedNegotiation& e : c.a.accept_pdu().user.extended)
        if (e.sop_class_uid == q.sop_class_uid) q.extended_negotiation = e.info;
    bool stopped = false, cancelled = false;
    Status send_error = Status::ok();
    const auto emit = [&](const Dataset& result) -> bool {
        if (stopped) return false;
        if (cancel_requested(c, q.message_id)) {
            stopped = true;
            cancelled = !c.dead && !c.release_pending;
            return false;
        }
        Status s = send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::Pending, true), &result);
        if (!s) {
            send_error = s;
            stopped = true;
            c.dead = true;
            return false;
        }
        return true;
    };
    std::uint16_t st = h_.on_find(q, emit);
    if (c.dead) return send_error;
    if (cancelled) st = dimse_status::Cancel;
    else if (classify_status(st) == StatusClass::Pending) st = dimse_status::Success;
    return send_dimse(c.a, m.context_id, make_response(m.command, st));
}

Status ScpServer::do_retrieve(Ctx& c, const Message& m, bool is_get) {
    const NegotiatedContext* pc = c.a.context(m.context_id);
    const auto& handler = is_get ? h_.on_get : h_.on_move;
    const std::uint8_t pcid = m.context_id;
    if (!pc || pc->abstract_syntax != m.command.affected_sop_class_uid || !handler)
        return send_dimse(c.a, pcid, make_response(m.command, dimse_status::SopClassNotSupported));
    Result<Dataset> ident = decode_message_dataset(m);
    if (!ident) return send_dimse(c.a, pcid, make_response(m.command, dimse_status::CannotUnderstand));
    QueryRequest q;
    q.calling_ae = c.a.calling_ae();
    q.peer_address = c.a.peer_address();
    q.sop_class_uid = m.command.affected_sop_class_uid;
    q.message_id = *m.command.message_id;
    q.priority = m.command.priority.value_or(0);
    q.move_destination = m.command.move_destination;
    q.identifier = std::move(ident.value());
    for (const ExtendedNegotiation& e : c.a.accept_pdu().user.extended)
        if (e.sop_class_uid == q.sop_class_uid) q.extended_negotiation = e.info;
    const bool without_bulk = q.sop_class_uid == sop::CompositeInstanceRetrieveWithoutBulkDataGet;

    std::optional<MoveDestination> dest;
    if (!is_get) {
        // R-129: a calling AE with a destination list may only name those.
        bool allowed_dest = true;
        if (const auto it = cfg_.move_destinations_by_caller.find(trimmed_ae(q.calling_ae));
            it != cfg_.move_destinations_by_caller.end())
            allowed_dest = it->second.count(trimmed_ae(q.move_destination)) != 0;
        if (!allowed_dest) log("C-MOVE from " + q.calling_ae + ": destination " + q.move_destination + " is not in its list");
        if (allowed_dest && h_.resolve_move_destination) dest = h_.resolve_move_destination(q.move_destination);
        if (!dest) {
            if (h_.on_retrieve_done) h_.on_retrieve_done(q, is_get, dimse_status::MoveDestinationUnknown, 0, 0, 0);
            return send_dimse(c.a, pcid, make_response(m.command, dimse_status::MoveDestinationUnknown));
        }
        if (dest->ae_title.empty()) dest->ae_title = q.move_destination;
    }
    std::vector<RetrieveItem> items;
    std::uint16_t hs = handler(q, items);
    if (hs == dimse_status::Success && items.size() > 0xFFFF) hs = dimse_status::OutOfResourcesSubOps;
    if (hs != dimse_status::Success) {
        if (h_.on_retrieve_done) h_.on_retrieve_done(q, is_get, hs, 0, 0, 0);
        return send_dimse(c.a, pcid, make_response(m.command, hs));
    }
    const auto total = static_cast<std::uint16_t>(items.size());

    // Load headers to learn SOP classes and transfer syntaxes (needed to propose sub-association contexts).
    struct Loaded {
        std::shared_ptr<const DicomFile> file;  // full file, loaded lazily for paths
        std::string path;
        std::string sop_class, sop_instance, ts;
        bool ok = false;
    };
    std::vector<Loaded> loaded(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        Loaded& l = loaded[i];
        l.file = items[i].file;
        l.path = items[i].path;
        std::shared_ptr<const DicomFile> hdr = l.file;
        if (!hdr) {
            ReadOptions ro;
            ro.stop_before_pixel_data = true;
            ro.open_mode = io::OpenMode::Map;  // the store's own files
            Result<DicomFile> f = read_file(l.path, ro);
            if (f) hdr = std::make_shared<const DicomFile>(std::move(f.value()));
        }
        if (!hdr) continue;
        l.sop_class = hdr->dataset.get_string(tags::SOPClassUID).value_or(std::string());
        l.sop_instance = hdr->dataset.get_string(tags::SOPInstanceUID).value_or(std::string());
        l.ts = hdr->transfer_syntax;
        l.ok = !l.sop_class.empty() && !l.sop_instance.empty();
    }

    std::unique_ptr<Scu> sub;
    if (!is_get) {
        ScuConfig sc;
        sc.calling_ae = cfg_.ae_title;
        sc.called_ae = dest->ae_title;
        sc.host = dest->host;
        sc.port = dest->port;
        sc.timeouts = cfg_.timeouts;
        sc.limits = cfg_.limits;
        sc.tls = cfg_.move_tls;
        if (dest->tls) sc.tls.enabled = *dest->tls;
        sc.transcode_params = cfg_.transcode_params;
        sc.allow_incomplete = cfg_.allow_incomplete;
        sub = std::make_unique<Scu>(sc);
        std::vector<std::pair<std::string, std::string>> seen;
        std::vector<std::string> native_offered;  // SOP classes with a native-syntax context
        for (const Loaded& l : loaded) {
            if (l.ok && is_native(l.ts) &&
                std::find(native_offered.begin(), native_offered.end(), l.sop_class) == native_offered.end())
                native_offered.push_back(l.sop_class);
        }
        for (const Loaded& l : loaded) {
            if (!l.ok) continue;
            const std::pair<std::string, std::string> key{l.sop_class, l.ts};
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            if (seen.size() >= 128) break;
            seen.push_back(key);
            std::vector<std::string> ts{l.ts};
            if (is_native(l.ts))
                for (const std::string& n : native_transfer_syntaxes())
                    if (n != l.ts) ts.push_back(n);
            sub->add_context(l.sop_class, std::move(ts));
            // T3.1: a compressed instance is also offered in the native transfer syntaxes (Implicit
            // VR Little Endian is the default transfer syntax every AE supports, PS3.5 10.1), once
            // per SOP class, so a destination that accepts only uncompressed data gets it decoded.
            if (!is_native(l.ts) &&
                std::find(native_offered.begin(), native_offered.end(), l.sop_class) == native_offered.end() &&
                sub->proposals().size() < 128) {
                native_offered.push_back(l.sop_class);
                sub->add_context(l.sop_class, native_transfer_syntaxes());
            }
            // The compressed syntaxes the transcoder can write (lossy only when allowed), so a
            // destination that accepts only those still receives the instance (testing 10b P2).
            for (bool lossy : {false, true}) {
                std::vector<std::string> extra = transcode_targets(cfg_.transcode_params, lossy);
                if (!extra.empty() && sub->proposals().size() < 128) sub->add_context(l.sop_class, std::move(extra));
            }
        }
        Status cs = seen.empty() ? Status(Errc::NotFound, "no sendable instances") : sub->connect();
        if (!cs) {
            log("C-MOVE sub-association to " + dest->ae_title + " failed: " + cs.message);
            Command rsp = make_response(m.command, total ? dimse_status::OutOfResourcesSubOps : dimse_status::Success);
            rsp.completed = std::uint16_t{0};
            rsp.failed = total;
            rsp.warning = std::uint16_t{0};
            return send_dimse(c.a, pcid, rsp);
        }
        if (!register_assoc(sub->association())) {
            sub->abort();
            c.dead = true;
            return Status(Errc::Cancelled, "server stopping");
        }
    }
    struct SubGuard {
        ScpServer* s;
        Scu* scu;
        ~SubGuard() {
            if (scu && scu->association()) s->unregister_assoc(scu->association());
        }
    } sub_guard{this, sub.get()};

    std::uint16_t completed = 0, failed = 0, warning = 0;
    std::vector<std::string> failed_uids;
    bool cancelled = false;
    std::size_t done = 0;
    for (; done < loaded.size(); ++done) {
        if (cancel_requested(c, q.message_id)) {
            cancelled = !c.dead && !c.release_pending;
            break;
        }
        Loaded& l = loaded[done];
        std::uint16_t st = dimse_status::ProcessingFailure;
        std::shared_ptr<const DicomFile> f = l.file;
        if (l.ok && !f) {
            ReadOptions full_ro;
            full_ro.open_mode = io::OpenMode::Map;  // the store's own files
            Result<DicomFile> full = read_file(l.path, full_ro);
            if (full) f = std::make_shared<const DicomFile>(std::move(full.value()));
        }
        if (f) {
            if (Status ok = check_sendable(*f, cfg_.allow_incomplete); !ok) {
                log(std::string(is_get ? "C-GET" : "C-MOVE") + " sub-operation " + std::to_string(done + 1) + ": " + ok.message);
                f.reset();
            }
        }
        if (f && without_bulk) {
            auto stripped = std::make_shared<DicomFile>(*f);
            strip_bulk_data(stripped->dataset);
            f = std::move(stripped);
        }
        if (f && is_get) {
            // C-STORE sub-operation on this association, on a context where the requestor is SCP.
            const NegotiatedContext* spc = nullptr;
            std::vector<std::uint8_t> body;
            codec::TranscodeReport report;
            for (const NegotiatedContext* k :
                 store_contexts(c.a, l.sop_class, f->transfer_syntax, true, true, cfg_.transcode_params))
                if (encode_for_transfer_syntax(*f, k->transfer_syntax, cfg_.transcode_params, body, &report)) {
                    spc = k;
                    break;
                }
            if (spc) {
                // A lossy transcode that filled in or could not align the lossy history lists says so.
                if (!report.lossy_history_note.empty())
                    log("C-GET sub-operation " + std::to_string(done + 1) + ": " + report.lossy_history_note);
                // R-093: a lossy transcode gave the image a new SOP Instance UID; send that one.
                if (!report.new_sop_instance_uid.empty() && report.new_sop_instance_uid != l.sop_instance) {
                    if (h_.on_sop_instance_uid_changed)
                        h_.on_sop_instance_uid_changed(q.calling_ae, l.sop_instance, report.new_sop_instance_uid);
                    l.sop_instance = report.new_sop_instance_uid;
                }
                {
                    const std::uint16_t smid = c.a.next_message_id();
                    const Command srq = make_c_store_rq(smid, l.sop_class, l.sop_instance);
                    if (!send_dimse_raw(c.a, spc->id, srq, body.data(), body.size())) {
                        c.dead = true;
                        break;
                    }
                    // Wait for the C-STORE-RSP; a C-CANCEL for the C-GET may arrive first.
                    bool got = false;
                    while (!got) {
                        Result<DimseReceived> r = receive_dimse(c.a, cfg_.timeouts.dimse_ms);
                        if (!r) {
                            if (r.error().code == Errc::Timeout && c.a.established()) c.a.abort();
                            c.dead = true;
                            break;
                        }
                        if (r->kind == DimseKind::PeerReleaseRequest) {
                            c.release_pending = true;
                            break;
                        }
                        const Command& k = r->message.command;
                        if (k.command_field == cmd::CCancelRq && k.message_id_being_responded_to == q.message_id) {
                            cancelled = true;
                            continue;
                        }
                        if (k.command_field == cmd::CStoreRsp && k.message_id_being_responded_to == smid) {
                            st = k.status.value_or(dimse_status::ProcessingFailure);
                            got = true;
                            continue;
                        }
                        c.a.abort(abort_src::Provider, abort_reason::UnexpectedPdu);
                        c.dead = true;
                        break;
                    }
                    if (c.dead || c.release_pending) break;
                }
            }
        } else if (f) {
            StoreSent sent;
            Result<std::uint16_t> r = sub->store(*f, q.calling_ae, q.message_id, &sent);
            if (!sent.lossy_history_note.empty())
                log("C-MOVE sub-operation " + std::to_string(done + 1) + ": " + sent.lossy_history_note);
            if (sent.uid_changed) {
                if (h_.on_sop_instance_uid_changed)
                    h_.on_sop_instance_uid_changed(q.calling_ae, l.sop_instance, sent.sop_instance_uid);
                l.sop_instance = sent.sop_instance_uid;
            }
            if (r) st = *r;
            else if (!sub->connected()) {
                // Sub-association lost: everything left fails.
                ++failed;
                failed_uids.push_back(l.sop_instance);
                for (std::size_t k = done + 1; k < loaded.size(); ++k) {
                    ++failed;
                    if (!loaded[k].sop_instance.empty()) failed_uids.push_back(loaded[k].sop_instance);
                }
                done = loaded.size();
                break;
            }
        }
        switch (classify_status(st)) {
            case StatusClass::Success: ++completed; break;
            case StatusClass::Warning: ++warning; break;
            default:
                ++failed;
                if (!l.sop_instance.empty()) failed_uids.push_back(l.sop_instance);
                break;
        }
        if (cancelled) {
            ++done;
            break;
        }
        if (done + 1 < loaded.size()) {
            Command p = make_response(m.command, dimse_status::Pending);
            p.remaining = static_cast<std::uint16_t>(loaded.size() - done - 1);
            p.completed = completed;
            p.failed = failed;
            p.warning = warning;
            if (Status s = send_dimse(c.a, pcid, p); !s) {
                c.dead = true;
                break;
            }
        }
    }
    if (sub) {
        if (sub->association()) unregister_assoc(sub->association());
        sub_guard.scu = nullptr;
        (void)sub->release();
    }
    const std::uint16_t final_status =
        cancelled ? dimse_status::Cancel : final_retrieve_status(total, completed, failed, warning);
    if (h_.on_retrieve_done) h_.on_retrieve_done(q, is_get, final_status, completed, failed, warning);
    if (c.dead || c.release_pending) return Status::ok();
    Command fin = make_response(m.command, final_status);
    fin.completed = completed;
    fin.failed = failed;
    fin.warning = warning;
    if (cancelled) fin.remaining = static_cast<std::uint16_t>(loaded.size() - done);
    Dataset fail_ds;
    if (!failed_uids.empty()) {
        std::string joined;
        for (const std::string& u : failed_uids) {
            if (!joined.empty()) joined += '\\';
            joined += u;
        }
        if (joined.size() < 0xFFFF && fail_ds.set_string(kFailedSopInstanceUidList, VR::UI, joined)) fin.has_dataset = true;
    }
    return send_dimse(c.a, pcid, fin, fin.has_dataset ? &fail_ds : nullptr);
}

Status ScpServer::do_n(Ctx& c, const Message& m) {
    const NegotiatedContext* pc = c.a.context(m.context_id);
    if (!h_.on_n_request || !pc || !context_covers(pc->abstract_syntax, m.command.sop_class()))
        return send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::NoSuchSopClass));
    Result<Dataset> ds = decode_message_dataset(m);
    if (!ds) return send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::ProcessingFailure));
    Dataset reply;
    bool has_reply = false;
    const std::uint16_t st = h_.on_n_request(m, *ds, reply, has_reply);
    return send_dimse(c.a, m.context_id, make_response(m.command, st, has_reply), has_reply ? &reply : nullptr);
}

}  // namespace siaara::net
