// SIAARA DICOM Toolkit - service class user (PS3.4 Annexes A, B, C; PS3.7 9.1 and 10.1).
#include "net/scu.hpp"

#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

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

Scu::Scu(ScuConfig cfg) : cfg_(std::move(cfg)) {}

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
        const bool known = std::any_of(proposals_.begin(), proposals_.end(), [&](const ProposedContext& c) {
            return c.abstract_syntax == sop_class && c.transfer_syntaxes == ts;
        });
        if (!known) add_context(sop_class, std::move(ts));
    }
    return Status::ok();
}

Status Scu::connect(AssociateRj* rj) {
    Result<std::unique_ptr<Transport>> t = tcp_connect(cfg_.host, cfg_.port, cfg_.timeouts.connect_ms);
    if (!t) return t.take_error();
    return connect_over(std::move(t.value()), rj);
}

Status Scu::connect_over(std::unique_ptr<Transport> t, AssociateRj* rj) {
    if (assoc_ && assoc_->established()) return Status(Errc::InvalidArgument, "already connected");
    if (proposals_.empty()) add_verification();
    AssociateRq rq;
    rq.called_ae = cfg_.called_ae;
    rq.calling_ae = cfg_.calling_ae;
    if (Status s = build_proposals(proposals_, rq); !s) return s;
    rq.user.identity_rq = cfg_.identity;
    rq.user.async_window = cfg_.async_window;
    rq.user.extended = cfg_.extended_negotiation;
    Result<std::unique_ptr<Association>> a = Association::request_over(std::move(t), rq, cfg_.timeouts, cfg_.limits, rj);
    if (!a) return a.take_error();
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
                                 std::optional<std::uint16_t> move_originator_message_id) {
    if (!connected()) return Status(Errc::InvalidArgument, "not connected");
    const std::string sop_class = sop_class_of(file);
    const std::string sop_instance = sop_instance_of(file);
    if (sop_class.empty() || sop_instance.empty()) return Status(Errc::Malformed, "instance lacks SOP Class/Instance UID");
    // Same transfer syntax, a native re-encoding, or (cfg_.transcode) a transcode on the fly.
    const std::vector<const NegotiatedContext*> cands = store_contexts(
        *assoc_, sop_class, file.transfer_syntax, false, cfg_.transcode, cfg_.transcode_params);
    if (cands.empty())
        return Status(Errc::Unsupported, "no accepted presentation context for this SOP class and transfer syntax");
    const NegotiatedContext* pc = nullptr;
    std::vector<std::uint8_t> body;
    Status enc = Status::ok();
    for (const NegotiatedContext* c : cands) {
        enc = encode_for_transfer_syntax(file, c->transfer_syntax, cfg_.transcode_params, body);
        if (enc) {
            pc = c;
            break;
        }
    }
    if (!pc) return enc;
    const std::uint16_t mid = assoc_->next_message_id();
    const Command rq = make_c_store_rq(mid, sop_class, sop_instance, priority::Medium, move_originator_ae,
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
        Result<Dataset> ds = decode_message_dataset(m);
        if (!ds) {
            st = dimse_status::CannotUnderstand;
        } else {
            rq.dataset = std::move(ds.value());
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
