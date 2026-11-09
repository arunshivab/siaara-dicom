// SIAARA DICOM Toolkit - service class provider server (PS3.4 Annexes A, B, C; PS3.7 9.1, 10.1).
#include "net/scp.hpp"

#include <utility>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"

namespace siaara::net {

namespace {

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
    if (Status s = listener_.listen(cfg_.bind_address, cfg_.port); !s) return s;
    port_ = listener_.port();
    stopping_.store(false);
    running_.store(true);
    workers_.reserve(cfg_.max_associations);
    for (std::size_t i = 0; i < cfg_.max_associations; ++i) workers_.emplace_back([this] { worker_loop(); });
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
    }
    cv_.notify_all();
    if (accept_thread_.joinable()) accept_thread_.join();
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& t : queue_) t->close();
        queue_.clear();
    }
    listener_.close();
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
        Result<std::unique_ptr<Transport>> t = listener_.accept(cfg_.accept_poll_ms);
        if (!t) {
            if (t.error().code == Errc::Timeout) continue;
            log("accept failed: " + t.error().message);
            continue;
        }
        bool full = false;
        {
            std::lock_guard<std::mutex> l(mu_);
            full = busy_ + queue_.size() >= cfg_.max_associations;
            if (!full) queue_.push_back(std::move(t.value()));
        }
        if (full) quick_reject(std::move(t.value()));
        else cv_.notify_one();
    }
}

void ScpServer::quick_reject(std::unique_ptr<Transport> t) {
    rejected_.fetch_add(1);
    Timeouts to = cfg_.timeouts;
    to.artim_ms = std::min(to.artim_ms, 2000);
    Result<std::unique_ptr<Association>> a = Association::receive_request(std::move(t), to, cfg_.limits);
    if (!a) return;
    log("association rejected: local limit exceeded");
    a.value()->reject({rj::Transient, rj::SourcePresentationProvider, rj::PresentationLocalLimitExceeded});
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
    const std::string peer = t->peer_address();
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
    while (!c.dead) {
        Result<DimseReceived> r = receive_dimse(a, cfg_.timeouts.idle_ms);
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
        log("invalid DIMSE request: " + v.message);
        c.a.abort(abort_src::Provider, abort_reason::InvalidPduParameterValue);
        return false;
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
    if (!pc || !pc->requestor_scu || pc->abstract_syntax != m.command.affected_sop_class_uid || !h_.on_store) {
        st = dimse_status::SopClassNotSupported;
    } else {
        StoreRequest rq;
        rq.calling_ae = c.a.calling_ae();
        rq.called_ae = c.a.called_ae();
        rq.sop_class_uid = m.command.affected_sop_class_uid;
        rq.sop_instance_uid = m.command.affected_sop_instance_uid;
        rq.transfer_syntax = m.transfer_syntax;
        rq.move_originator_ae = m.command.move_originator_ae;
        rq.move_originator_message_id = m.command.move_originator_message_id;
        rq.raw = m.data;
        Result<Dataset> ds = decode_message_dataset(m);
        if (!ds) {
            st = dimse_status::CannotUnderstand;
        } else {
            rq.dataset = std::move(ds.value());
            st = h_.on_store(rq);
        }
    }
    return send_dimse(c.a, m.context_id, make_response(m.command, st));
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
    q.sop_class_uid = m.command.affected_sop_class_uid;
    q.message_id = *m.command.message_id;
    q.priority = m.command.priority.value_or(0);
    q.identifier = std::move(ident.value());
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
    q.sop_class_uid = m.command.affected_sop_class_uid;
    q.message_id = *m.command.message_id;
    q.priority = m.command.priority.value_or(0);
    q.move_destination = m.command.move_destination;
    q.identifier = std::move(ident.value());

    std::optional<MoveDestination> dest;
    if (!is_get) {
        if (h_.resolve_move_destination) dest = h_.resolve_move_destination(q.move_destination);
        if (!dest) return send_dimse(c.a, pcid, make_response(m.command, dimse_status::MoveDestinationUnknown));
        if (dest->ae_title.empty()) dest->ae_title = q.move_destination;
    }
    std::vector<RetrieveItem> items;
    const std::uint16_t hs = handler(q, items);
    if (hs != dimse_status::Success) return send_dimse(c.a, pcid, make_response(m.command, hs));
    if (items.size() > 0xFFFF)
        return send_dimse(c.a, pcid, make_response(m.command, dimse_status::OutOfResourcesSubOps));
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
        sub = std::make_unique<Scu>(sc);
        std::vector<std::pair<std::string, std::string>> seen;
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
            Result<DicomFile> full = read_file(l.path);
            if (full) f = std::make_shared<const DicomFile>(std::move(full.value()));
        }
        if (f && is_get) {
            // C-STORE sub-operation on this association, on a context where the requestor is SCP.
            const NegotiatedContext* spc = nullptr;
            std::vector<std::string> pref{f->transfer_syntax};
            if (is_native(f->transfer_syntax))
                for (const std::string& n : native_transfer_syntaxes()) pref.push_back(n);
            for (const std::string& ts : pref) {
                for (const NegotiatedContext& k : c.a.contexts())
                    if (k.accepted() && k.requestor_scp && k.abstract_syntax == l.sop_class && k.transfer_syntax == ts) {
                        spc = &k;
                        break;
                    }
                if (spc) break;
            }
            if (spc) {
                std::vector<std::uint8_t> body;
                if (write_dataset(f->dataset, spc->transfer_syntax, body)) {
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
            Result<std::uint16_t> r = sub->store(*f, q.calling_ae, q.message_id);
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
    if (c.dead || c.release_pending) return Status::ok();
    Command fin = make_response(m.command, cancelled ? dimse_status::Cancel
                                                     : final_retrieve_status(total, completed, failed, warning));
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
    if (!h_.on_n_request || !pc || pc->abstract_syntax != m.command.sop_class())
        return send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::NoSuchSopClass));
    Result<Dataset> ds = decode_message_dataset(m);
    if (!ds) return send_dimse(c.a, m.context_id, make_response(m.command, dimse_status::ProcessingFailure));
    Dataset reply;
    bool has_reply = false;
    const std::uint16_t st = h_.on_n_request(m, *ds, reply, has_reply);
    return send_dimse(c.a, m.context_id, make_response(m.command, st, has_reply), has_reply ? &reply : nullptr);
}

}  // namespace siaara::net
