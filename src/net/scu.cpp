// SIAARA DICOM Toolkit - service class user (PS3.4 Annexes A, B, C; PS3.7 9.1 and 10.1).
#include "net/scu.hpp"

#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <mutex>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/store_support.hpp"

namespace siaara::net {

namespace {

bool is_native(std::string_view ts_uid) {
    const TransferSyntaxInfo* t = find_transfer_syntax(ts_uid);
    return t && !t->encapsulated;
}

std::string sop_class_of(const DicomFile& f) {
    if (auto v = f.dataset.get_string(tags::SOPClassUID)) return *v;
    return f.meta.get_string(tags::MediaStorageSOPClassUID).value_or(std::string());
}
std::string sop_instance_of(const DicomFile& f) {
    if (auto v = f.dataset.get_string(tags::SOPInstanceUID)) return *v;
    return f.meta.get_string(tags::MediaStorageSOPInstanceUID).value_or(std::string());
}

RetrieveProgress progress_of(const Command& c) {
    RetrieveProgress p;
    p.status = c.status.value_or(0);
    p.remaining = c.remaining.value_or(0);
    p.completed = c.completed.value_or(0);
    p.failed = c.failed.value_or(0);
    p.warning = c.warning.value_or(0);
    return p;
}

}  // namespace

std::uint16_t check_store_uids(StoreRequest& rq, bool strict, std::string& note) {
    note.clear();
    const auto cls = rq.dataset.get_string(tags::SOPClassUID);
    const auto inst = rq.dataset.get_string(tags::SOPInstanceUID);
    const bool cls_ok = cls && *cls == rq.sop_class_uid;
    const bool inst_ok = inst && *inst == rq.sop_instance_uid;
    if (cls_ok && inst_ok) return dimse_status::Success;
    if (!inst_ok)
        note = "the command's Affected SOP Instance UID " + rq.sop_instance_uid + " differs from the data set's " +
               (inst ? *inst : std::string("(absent)"));
    if (!cls_ok)
        note += std::string(note.empty() ? "" : "; ") + "the command's Affected SOP Class UID " + rq.sop_class_uid +
                " differs from the data set's " + (cls ? *cls : std::string("(absent)"));
    if (strict) return dimse_status::DataSetDoesNotMatch;
    rq.uid_mismatch = true;
    rq.command_sop_class_uid = rq.sop_class_uid;
    rq.command_sop_instance_uid = rq.sop_instance_uid;
    if (cls && !cls->empty()) rq.sop_class_uid = *cls;
    if (inst && !inst->empty()) rq.sop_instance_uid = *inst;
    rq.spool.reset();  // its file meta carries the command's UIDs; `raw` keeps the bytes alive
    return dimse_status::Success;
}

DicomFile to_part10(const StoreRequest& rq) {
    DicomFile f;
    f.transfer_syntax = rq.transfer_syntax;
    f.dataset = rq.dataset;
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, rq.sop_class_uid);
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, rq.sop_instance_uid);
    if (!rq.calling_ae.empty()) (void)f.meta.set_string(0x00020016, VR::AE, rq.calling_ae);  // Source AE Title
    return f;
}

Status encode_received_part10(const StoreRequest& rq, std::vector<std::uint8_t>& out) {
    DicomFile f = to_part10(rq);
    f.dataset = Dataset{};
    if (Status s = write_part10(f, out); !s) return s;
    // Keep preamble, "DICM" and the group 0002 block; drop whatever the empty body encoded to.
    if (out.size() < 144) return Status(Errc::Internal, "file meta too short");
    const std::uint64_t meta_end = 144ull + load_le32(out.data() + 140);
    if (meta_end > out.size()) return Status(Errc::Internal, "file meta length mismatch");
    out.resize(static_cast<std::size_t>(meta_end));
    out.insert(out.end(), rq.raw.data(), rq.raw.data() + rq.raw.size());
    return Status::ok();
}

// Transports interrupt() may shut down: registered while they exist, under the gate's mutex, so a
// shutdown never touches a transport that is being closed or destroyed.
struct Scu::Gate {
    std::mutex mu;
    std::vector<Transport*> open;
    std::atomic<bool> cancelled{false};
};

Scu::Scu(ScuConfig cfg) : cfg_(std::move(cfg)), gate_(std::make_shared<Gate>()) {}

namespace {

// Forwards to `inner` and keeps it registered with the gate for as long as it is open.
class GatedTransport final : public Transport {
public:
    GatedTransport(std::unique_ptr<Transport> inner, std::shared_ptr<Scu::Gate> gate);
    ~GatedTransport() override { detach(); }
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override {
        return inner_->read_some(buf, n, timeout_ms);
    }
    Status write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) override {
        return inner_->write_all(data, n, timeout_ms);
    }
    Result<bool> wait_readable(int timeout_ms) override { return inner_->wait_readable(timeout_ms); }
    void shutdown() noexcept override { inner_->shutdown(); }
    void shutdown_send() noexcept override { inner_->shutdown_send(); }
    void close() noexcept override {
        detach();
        inner_->close();
    }
    bool is_open() const noexcept override { return inner_->is_open(); }
    std::string peer_address() const override { return inner_->peer_address(); }

private:
    void detach() noexcept;
    std::unique_ptr<Transport> inner_;
    std::shared_ptr<Scu::Gate> gate_;
};

}  // namespace

Scu::~Scu() {
    if (assoc_ && assoc_->established()) assoc_->abort();
}

void Scu::add_context(std::string abstract_syntax, std::vector<std::string> transfer_syntaxes,
                      std::optional<RoleSelection> role) {
    proposals_.push_back({std::move(abstract_syntax), std::move(transfer_syntaxes), std::move(role)});
}

void Scu::add_verification() { add_context(std::string(sop::Verification), native_transfer_syntaxes()); }

void Scu::add_get_storage(std::string sop_class, std::vector<std::string> transfer_syntaxes) {
    RoleSelection r;
    r.scu_role = false;
    r.scp_role = true;
    add_context(std::move(sop_class), std::move(transfer_syntaxes), r);
}

Status Scu::add_contexts_for_files(const std::vector<std::string>& paths) {
    ReadOptions opt;
    opt.stop_before_pixel_data = true;
    for (const std::string& p : paths) {
        Result<DicomFile> f = read_file(p, opt);
        if (!f) return f.take_error();
        const std::string sop_class = sop_class_of(*f);
        if (sop_class.empty()) return Status(Errc::Malformed, "file has no SOP Class UID");
        std::vector<std::string> ts;
        ts.push_back(f->transfer_syntax);
        if (is_native(f->transfer_syntax))
            for (const std::string& n : native_transfer_syntaxes())
                if (n != f->transfer_syntax) ts.push_back(n);
        const auto known = [&](const std::vector<std::string>& list) {
            return std::any_of(proposals_.begin(), proposals_.end(), [&](const ProposedContext& c) {
                return c.abstract_syntax == sop_class && c.transfer_syntaxes == list;
            });
        };
        if (!known(ts)) add_context(sop_class, std::move(ts));
        // With transcoding, the compressed syntaxes the transcoder writes (lossy ones only when
        // allowed), each group in its own context, so a peer that accepts only those gets the file.
        if (cfg_.transcode)
            for (bool lossy : {false, true}) {
                std::vector<std::string> extra = transcode_targets(cfg_.transcode_params, lossy);
                if (!extra.empty() && !known(extra) && proposals_.size() < 128) add_context(sop_class, std::move(extra));
            }
    }
    if (proposals_.size() > 128) return Status(Errc::LimitExceeded, "more than 128 presentation contexts needed");
    return Status::ok();
}

namespace {

GatedTransport::GatedTransport(std::unique_ptr<Transport> inner, std::shared_ptr<Scu::Gate> gate)
    : inner_(std::move(inner)), gate_(std::move(gate)) {
    const std::lock_guard<std::mutex> l(gate_->mu);
    gate_->open.push_back(inner_.get());
    if (gate_->cancelled.load()) inner_->shutdown();
}

void GatedTransport::detach() noexcept {
    const std::lock_guard<std::mutex> l(gate_->mu);
    const auto it = std::find(gate_->open.begin(), gate_->open.end(), inner_.get());
    if (it != gate_->open.end()) gate_->open.erase(it);
}

ConnectFailure failure_of(ConnectError e) noexcept {
    switch (e) {
        case ConnectError::None: return ConnectFailure::None;
        case ConnectError::Resolve: return ConnectFailure::Resolve;
        case ConnectError::Refused: return ConnectFailure::Refused;
        case ConnectError::Timeout: return ConnectFailure::ConnectTimeout;
        case ConnectError::Cancelled: return ConnectFailure::Cancelled;
        default: return ConnectFailure::Other;
    }
}

}  // namespace

void Scu::interrupt() noexcept {
    gate_->cancelled.store(true);
    const std::lock_guard<std::mutex> l(gate_->mu);
    for (Transport* t : gate_->open) t->shutdown();
}

bool Scu::interrupted() const noexcept { return gate_->cancelled.load(); }

Status Scu::connect(AssociateRj* rj) {
    failure_ = ConnectFailure::None;
    const auto cancelled = [this]() -> Status {
        failure_ = ConnectFailure::Cancelled;
        return Status(Errc::Cancelled, "cancelled");
    };
    if (gate_->cancelled.load()) return cancelled();
    ConnectError why = ConnectError::None;
    if (cfg_.tls.enabled) {
#if defined(__APPLE__)
        // Network.framework owns the TCP connection: the TLS connect itself cannot be interrupted
        // (it is bounded by the connect and handshake timeouts); the negotiation after it can.
        Result<std::unique_ptr<TlsTransport>> s = tls_connect(cfg_.tls, cfg_.host, cfg_.port, cfg_.timeouts.connect_ms);
#else
        Result<std::shared_ptr<TlsContext>> ctx = TlsContext::create(cfg_.tls, TlsContext::Role::Client);
        if (!ctx) {
            failure_ = ConnectFailure::Tls;
            return ctx.take_error();
        }
        Result<std::unique_ptr<Transport>> tcp =
            tcp_connect(cfg_.host, cfg_.port, cfg_.timeouts.connect_ms, &gate_->cancelled, &why);
        if (!tcp) {
            failure_ = failure_of(why);
            return tcp.take_error();
        }
        Result<std::unique_ptr<TlsTransport>> s = ctx.value()->wrap_client(
            std::unique_ptr<Transport>(new GatedTransport(std::move(tcp.value()), gate_)), cfg_.host);
#endif
        if (gate_->cancelled.load()) return cancelled();
        if (!s) {
            failure_ = ConnectFailure::Tls;
            return s.take_error();
        }
        return connect_over(std::unique_ptr<Transport>(std::move(s).value()), rj);
    }
    Result<std::unique_ptr<Transport>> t =
        tcp_connect(cfg_.host, cfg_.port, cfg_.timeouts.connect_ms, &gate_->cancelled, &why);
    if (!t) {
        failure_ = failure_of(why);
        return t.take_error();
    }
    return connect_over(std::move(t.value()), rj);
}

Status Scu::connect_over(std::unique_ptr<Transport> t, AssociateRj* rj) {
    failure_ = ConnectFailure::None;
    if (assoc_ && assoc_->established()) return Status(Errc::InvalidArgument, "already connected");
    if (proposals_.empty()) add_verification();
    AssociateRq rq;
    rq.called_ae = cfg_.called_ae;
    rq.calling_ae = cfg_.calling_ae;
    if (Status s = build_proposals(proposals_, rq); !s) {
        failure_ = ConnectFailure::Other;
        return s;
    }
    rq.user.identity_rq = cfg_.identity;
    rq.user.async_window = cfg_.async_window;
    rq.user.extended = cfg_.extended_negotiation;
    // The transport stays registered with the gate for the life of the association, so
    // interrupt() also wakes the wait for the A-ASSOCIATE response and later service calls.
    if (t) t = std::unique_ptr<Transport>(new GatedTransport(std::move(t), gate_));
    Result<std::unique_ptr<Association>> a = Association::request_over(std::move(t), rq, cfg_.timeouts, cfg_.limits, rj);
    if (!a) {
        if (gate_->cancelled.load()) {
            failure_ = ConnectFailure::Cancelled;
            return Status(Errc::Cancelled, "cancelled");
        }
        failure_ = a.error().code == Errc::Rejected  ? ConnectFailure::Rejected
                   : a.error().code == Errc::Timeout ? ConnectFailure::AcseTimeout
                   : a.error().code == Errc::NetworkError ? ConnectFailure::Closed
                                                          : ConnectFailure::Other;
        return a.take_error();
    }
    assoc_ = std::move(a.value());
    return Status::ok();
}

Status Scu::release() {
    if (!assoc_) return Status::ok();
    Status s = assoc_->established() ? assoc_->release() : Status::ok();
    assoc_.reset();
    return s;
}

void Scu::abort() {
    if (assoc_) assoc_->abort();
    assoc_.reset();
}

Result<Message> Scu::wait_response(std::uint16_t message_id, std::uint16_t response_field) {
    while (true) {
        Result<DimseReceived> r = receive_dimse(*assoc_, cfg_.timeouts.dimse_ms);
        if (!r) {
            if (r.error().code == Errc::Timeout && assoc_->established()) assoc_->abort();
            return r.take_error();
        }
        if (r->kind == DimseKind::PeerReleaseRequest) {
            (void)assoc_->respond_release();
            return Status(Errc::NetworkError, "peer released the association while a response was due");
        }
        Message& m = r->message;
        if (m.command.command_field == response_field &&
            m.command.message_id_being_responded_to == message_id) {
            if (Status s = validate_command(m.command); !s) return s;
            return std::move(m);
        }
        // Without asynchronous operations nothing else may arrive here.
        assoc_->abort(abort_src::Provider, abort_reason::UnexpectedPdu);
        return Status(Errc::Malformed, "unexpected DIMSE message while waiting for a response");
    }
}

Result<std::uint16_t> Scu::echo() {
    if (!connected()) return Status(Errc::InvalidArgument, "not connected");
    const NegotiatedContext* pc = assoc_->find_context(sop::Verification);
    if (!pc) return Status(Errc::Unsupported, "Verification SOP class not accepted");
    const std::uint16_t mid = assoc_->next_message_id();
    if (Status s = send_dimse(*assoc_, pc->id, make_c_echo_rq(mid)); !s) return s;
    Result<Message> rsp = wait_response(mid, cmd::CEchoRsp);
    if (!rsp) return rsp.take_error();
    return rsp->command.status.value_or(0xFFFF);
}

Result<std::uint16_t> Scu::store(const DicomFile& file, std::string_view move_originator_ae,
                                 std::optional<std::uint16_t> move_originator_message_id, StoreSent* sent) {
    if (!connected()) return Status(Errc::InvalidArgument, "not connected");
    const std::string sop_class = sop_class_of(file);
    const std::string sop_instance = sop_instance_of(file);
    if (sop_class.empty() || sop_instance.empty()) return Status(Errc::Malformed, "instance lacks SOP Class/Instance UID");
    if (sent) {
        *sent = StoreSent{};
        sent->sop_instance_uid = sop_instance;
        sent->original_sop_instance_uid = sop_instance;
    }
    if (Status s = check_sendable(file, cfg_.allow_incomplete); !s) {
        if (sent) sent->incomplete = true;
        return s;
    }
    // Same transfer syntax, a native re-encoding, or (cfg_.transcode) a transcode on the fly.
    const std::vector<const NegotiatedContext*> cands = store_contexts(
        *assoc_, sop_class, file.transfer_syntax, false, cfg_.transcode, cfg_.transcode_params);
    if (cands.empty())
        return Status(Errc::Unsupported, "no accepted presentation context for this SOP class and transfer syntax");
    const NegotiatedContext* pc = nullptr;
    std::vector<std::uint8_t> body;
    Status enc = Status::ok();
    codec::TranscodeReport report;
    for (const NegotiatedContext* c : cands) {
        enc = encode_for_transfer_syntax(file, c->transfer_syntax, cfg_.transcode_params, body, &report);
        if (enc) {
            pc = c;
            break;
        }
    }
    if (!pc) return enc;
    // R-093: a lossy transcode gave the image a new SOP Instance UID; the command must carry it.
    const std::string& sent_uid = report.new_sop_instance_uid.empty() ? sop_instance : report.new_sop_instance_uid;
    if (sent) {
        sent->sop_instance_uid = sent_uid;
        sent->original_sop_instance_uid = sop_instance;
        sent->transfer_syntax = pc->transfer_syntax;
        sent->uid_changed = sent_uid != sop_instance;
        sent->lossy = report.lossy_applied;
        sent->lossy_history = report.lossy_history;
        sent->lossy_history_note = report.lossy_history_note;
    }
    const std::uint16_t mid = assoc_->next_message_id();
    const Command rq = make_c_store_rq(mid, sop_class, sent_uid, priority::Medium, move_originator_ae,
                                       move_originator_message_id);
    if (Status s = send_dimse_raw(*assoc_, pc->id, rq, body.data(), body.size()); !s) return s;
    Result<Message> rsp = wait_response(mid, cmd::CStoreRsp);
    if (!rsp) return rsp.take_error();
    return rsp->command.status.value_or(0xFFFF);
}

Result<std::uint16_t> Scu::store_file(const std::string& path) {
    Result<DicomFile> f = read_file(path);
    if (!f) return f.take_error();
    return store(*f);
}

Result<FindResult> Scu::find(std::string_view sop_class, const Dataset& identifier,
                             const std::function<bool(const Dataset&)>& on_result) {
    if (!connected()) return Status(Errc::InvalidArgument, "not connected");
    const NegotiatedContext* pc = assoc_->find_context(sop_class);
    if (!pc) return Status(Errc::Unsupported, "query SOP class not accepted");
    const std::uint8_t pcid = pc->id;
    const std::uint16_t mid = assoc_->next_message_id();
    if (Status s = send_dimse(*assoc_, pcid, make_c_find_rq(mid, sop_class), &identifier); !s) return s;
    FindResult out;
    while (true) {
        Result<Message> rsp = wait_response(mid, cmd::CFindRsp);
        if (!rsp) return rsp.take_error();
        const std::uint16_t st = rsp->command.status.value_or(0xFFFF);
        if (classify_status(st) != StatusClass::Pending) {
            out.status = st;
            return out;
        }
        if (out.cancel_sent) continue;  // results after C-CANCEL are discarded
        ++out.matches;
        Result<Dataset> ds = decode_message_dataset(*rsp);
        if (!ds) {
            assoc_->abort(abort_src::Provider, abort_reason::NotSpecified);
            return ds.take_error();
        }
        if (on_result && !on_result(*ds)) {
            if (Status s = send_dimse(*assoc_, pcid, make_c_cancel_rq(mid)); !s) return s;
            out.cancel_sent = true;
        }
    }
}

Status Scu::handle_get_store(const Message& m, const std::function<std::uint16_t(const StoreRequest&)>& on_instance) {
    const NegotiatedContext* pc = assoc_->context(m.context_id);
    std::uint16_t st = dimse_status::Success;
    if (Status v = validate_command(m.command); !v) {
        st = dimse_status::CannotUnderstand;
    } else if (!pc || !pc->requestor_scp || pc->abstract_syntax != m.command.affected_sop_class_uid) {
        st = dimse_status::SopClassNotSupported;
    } else {
        StoreRequest rq;
        rq.calling_ae = assoc_->called_ae();
        rq.called_ae = assoc_->calling_ae();
        rq.sop_class_uid = m.command.affected_sop_class_uid;
        rq.sop_instance_uid = m.command.affected_sop_instance_uid;
        rq.transfer_syntax = m.transfer_syntax;
        rq.move_originator_ae = m.command.move_originator_ae;
        rq.move_originator_message_id = m.command.move_originator_message_id;
        rq.raw = m.data;
        rq.spool = m.spool;
        Result<DicomFile> ds = m.discarded ? Result<DicomFile>(Status(Errc::LimitExceeded, "discarded"))
                               : m.has_data ? read_dataset_report(m.data, m.transfer_syntax)
                                            : Result<DicomFile>(DicomFile{});
        if (m.discarded) {
            st = dimse_status::OutOfResources;  // R-074: spool disk quota or free-space floor
        } else if (!ds) {
            st = dimse_status::CannotUnderstand;
        } else {
            rq.dataset = std::move(ds->dataset);
            rq.read_loss = ds->read_loss;
            rq.read_loss_offset = ds->read_loss_offset;
            if (!ds->warnings.empty()) rq.read_warning = ds->warnings.front();
            std::string note;
            st = check_store_uids(rq, cfg_.strict_store_uids, note);
            // R-116: an incomplete data set is refused, or handed on flagged and as received.
            if (st == dimse_status::Success && rq.read_loss != 0 && cfg_.incomplete_policy == IncompletePolicy::Refuse)
                st = dimse_status::CannotUnderstand;
            else if (st == dimse_status::Success)
                st = on_instance ? on_instance(rq) : dimse_status::Success;
        }
    }
    return send_dimse(*assoc_, m.context_id, make_response(m.command, st));
}

Result<RetrieveResult> Scu::retrieve(bool is_get, std::string_view sop_class, std::string_view destination,
                                     const Dataset& identifier,
                                     const std::function<std::uint16_t(const StoreRequest&)>& on_instance,
                                     const std::function<bool(const RetrieveProgress&)>& on_progress) {
    if (!connected()) return Status(Errc::InvalidArgument, "not connected");
    const NegotiatedContext* pc = assoc_->find_context(sop_class);
    if (!pc) return Status(Errc::Unsupported, "retrieve SOP class not accepted");
    const std::uint8_t pcid = pc->id;
    const std::uint16_t mid = assoc_->next_message_id();
    const Command rq = is_get ? make_c_get_rq(mid, sop_class) : make_c_move_rq(mid, sop_class, destination);
    if (Status s = send_dimse(*assoc_, pcid, rq, &identifier); !s) return s;
    const std::uint16_t rsp_field = is_get ? cmd::CGetRsp : cmd::CMoveRsp;
    bool cancel_sent = false;
    while (true) {
        Result<DimseReceived> r =
            receive_dimse(*assoc_, cfg_.timeouts.dimse_ms, is_get && cfg_.spool ? &*cfg_.spool : nullptr);
        if (!r) {
            if (r.error().code == Errc::Timeout && assoc_->established()) assoc_->abort();
            return r.take_error();
        }
        if (r->kind == DimseKind::PeerReleaseRequest) {
            (void)assoc_->respond_release();
            return Status(Errc::NetworkError, "peer released the association during a retrieve");
        }
        const Message& m = r->message;
        if (is_get && m.command.command_field == cmd::CStoreRq) {
            if (Status s = handle_get_store(m, on_instance); !s) return s;
            continue;
        }
        if (m.command.command_field != rsp_field || m.command.message_id_being_responded_to != mid) {
            assoc_->abort(abort_src::Provider, abort_reason::UnexpectedPdu);
            return Status(Errc::Malformed, "unexpected DIMSE message during retrieve");
        }
        if (Status s = validate_command(m.command); !s) return s;
        const RetrieveProgress p = progress_of(m.command);
        if (classify_status(p.status) != StatusClass::Pending) return p;
        if (on_progress && !cancel_sent && !on_progress(p)) {
            if (Status s = send_dimse(*assoc_, pcid, make_c_cancel_rq(mid)); !s) return s;
            cancel_sent = true;
        }
    }
}

Result<RetrieveResult> Scu::move(std::string_view sop_class, std::string_view destination_ae, const Dataset& identifier,
                                 const std::function<bool(const RetrieveProgress&)>& on_progress) {
    if (!is_valid_ae_title(destination_ae)) return Status(Errc::InvalidArgument, "invalid move destination AE title");
    return retrieve(false, sop_class, destination_ae, identifier, {}, on_progress);
}

Result<RetrieveResult> Scu::get(std::string_view sop_class, const Dataset& identifier,
                                const std::function<std::uint16_t(const StoreRequest&)>& on_instance,
                                const std::function<bool(const RetrieveProgress&)>& on_progress) {
    return retrieve(true, sop_class, {}, identifier, on_instance, on_progress);
}

Result<Scu::NResponse> Scu::n_request(Command rq, const Dataset* ds) {
    if (!connected()) return Status(Errc::InvalidArgument, "not connected");
    const NegotiatedContext* pc = assoc_->find_context(rq.sop_class());
    if (!pc) return Status(Errc::Unsupported, "SOP class not accepted");
    rq.message_id = assoc_->next_message_id();
    if (Status s = validate_command(rq); !s) return s;
    if (Status s = send_dimse(*assoc_, pc->id, rq, ds); !s) return s;
    Result<Message> rsp = wait_response(*rq.message_id, static_cast<std::uint16_t>(rq.command_field | 0x8000u));
    if (!rsp) return rsp.take_error();
    NResponse out;
    out.command = rsp->command;
    if (rsp->has_data) {
        Result<Dataset> d = decode_message_dataset(*rsp);
        if (!d) return d.take_error();
        out.dataset = std::move(d.value());
    }
    return out;
}

}  // namespace siaara::net
