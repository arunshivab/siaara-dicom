// SIAARA DICOM Toolkit - DIMSE service classes (net/services.hpp).
// Written from DICOM PS3.4 Annexes C (matching C.2.2.2, extended negotiation C.5), F.7, H, J, K, R,
// Z and CC; message rules from PS3.7 sections 9-10 and Annex D.3.3.
#include "net/services.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "net/store_support.hpp"

namespace siaara::net {

namespace {

// Attribute tags used by the services (PS3.6).
constexpr Tag kQueryRetrieveLevel = 0x00080052;
constexpr Tag kRetrieveAeTitle = 0x00080054;
constexpr Tag kAccessionNumber = 0x00080050;
constexpr Tag kReferencedSopClass = 0x00081150;
constexpr Tag kReferencedSopInstance = 0x00081155;
constexpr Tag kTransactionUid = 0x00081195;
constexpr Tag kFailureReason = 0x00081197;
constexpr Tag kFailedSopSequence = 0x00081198;
constexpr Tag kReferencedSopSequence = 0x00081199;
constexpr Tag kPatientBirthDate = 0x00100030;
constexpr Tag kPatientSex = 0x00100040;
constexpr Tag kRequestedProcedureDescription = 0x00321060;
constexpr Tag kScheduledStationAe = 0x00400001;
constexpr Tag kSpsStartDate = 0x00400002;
constexpr Tag kSpsStartTime = 0x00400003;
constexpr Tag kScheduledPhysician = 0x00400006;
constexpr Tag kSpsDescription = 0x00400007;
constexpr Tag kSpsId = 0x00400009;
constexpr Tag kSpsSequence = 0x00400100;
constexpr Tag kRequestedProcedureId = 0x00401001;
constexpr Tag kProcedureStepState = 0x00741000;
constexpr Tag kDeletionLock = 0x00741230;
constexpr Tag kReceivingAe = 0x00741234;
constexpr Tag kImageDisplayFormat = 0x20100010;
constexpr Tag kReferencedFilmSessionSequence = 0x20100500;
constexpr Tag kReferencedImageBoxSequence = 0x20100510;
constexpr Tag kImageBoxPosition = 0x20200010;
constexpr Tag kBasicGrayscaleImageSequence = 0x20200110;
constexpr Tag kBasicColorImageSequence = 0x20200111;
constexpr Tag kPrinterStatus = 0x21100010;
constexpr Tag kPrinterStatusInfo = 0x21100020;

constexpr std::uint16_t kNoSuchActionType = 0x0123;

Element make_sequence(Tag tag, std::vector<Dataset> items) {
    Element e;
    e.tag = tag;
    e.vr = VR::SQ;
    e.items = std::move(items);
    return e;
}

std::string str_of(const Dataset& ds, Tag t) { return ds.get_string(t).value_or(std::string()); }

Status status_error(const char* what, std::uint16_t st) {
    static const char* hex = "0123456789ABCDEF";
    std::string h;
    for (int i = 12; i >= 0; i -= 4) h += hex[(st >> i) & 0xF];
    return Status(Errc::Rejected, std::string(what) + " failed with status 0x" + h);
}

bool is_failure(std::uint16_t st) { return classify_status(st) == StatusClass::Failure; }

// ------------------------------------------------------------------ matching helpers

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& ch : r) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return r;
}

// '*' any sequence, '?' one character (PS3.4 C.2.2.2.4).
bool wildcard(std::string_view p, std::string_view s) {
    std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::vector<std::string_view> values_of(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t k = s.find('\\', start);
        out.push_back(s.substr(start, k == std::string_view::npos ? std::string_view::npos : k - start));
        if (k == std::string_view::npos) break;
        start = k + 1;
    }
    return out;
}

std::string_view trimmed(const Element& e) {
    std::string_view v = trim_value(e.value.as_chars(), e.vr);
    while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
    return v;
}

bool is_universal(const Element& key) {
    if (key.is_sequence()) return key.items.empty() || (key.items.size() == 1 && key.items[0].empty());
    if (vr_is_string(key.vr)) return trimmed(key).empty();
    return key.value.empty();
}

bool not_a_key(Tag t) {
    return t == tags::SpecificCharacterSet || t == kQueryRetrieveLevel || tag_element(t) == 0x0000;
}

std::string pn_norm(std::string_view v, bool fuzzy) {
    std::string s(v);
    while (!s.empty() && (s.back() == '^' || s.back() == ' ')) s.pop_back();
    return fuzzy ? upper(s) : s;
}

bool value_matches(const Element& key, std::string_view k, std::string_view c, const MatchOptions& opt) {
    while (!c.empty() && c.back() == ' ') c.remove_suffix(1);
    while (!c.empty() && c.front() == ' ') c.remove_prefix(1);
    const VR vr = key.vr;
    if (vr == VR::DA || vr == VR::TM || vr == VR::DT) {
        const std::size_t dash = k.find('-');
        if (dash != std::string_view::npos) {
            // Range matching (C.2.2.2.5): compare as many characters as each bound has.
            const std::string_view lo = k.substr(0, dash), hi = k.substr(dash + 1);
            auto norm = [vr](std::string_view x) {
                std::string r;
                for (char ch : x)
                    if (!(vr == VR::TM && ch == ':')) r += ch;
                return r;
            };
            const std::string cv = norm(c), l = norm(lo), h = norm(hi);
            if (cv.empty()) return false;
            if (!l.empty() && cv.substr(0, l.size()) < l) return false;
            if (!h.empty() && cv.substr(0, h.size()) > h) return false;
            return true;
        }
        return k == c;
    }
    if (vr == VR::PN) {
        const std::string kk = pn_norm(k, opt.fuzzy_person_names), cc = pn_norm(c, opt.fuzzy_person_names);
        if (kk.find_first_of("*?") != std::string::npos) return wildcard(kk, cc);
        return kk == cc;
    }
    if (vr != VR::UI && k.find_first_of("*?") != std::string_view::npos) return wildcard(k, c);
    return k == c;
}

bool element_matches(const Element& key, const Element* cand, const MatchOptions& opt) {
    if (is_universal(key)) return true;
    if (!cand) return false;
    if (key.is_sequence()) {
        if (!cand->is_sequence()) return false;
        for (const Dataset& item : cand->items)
            if (query_matches(key.items[0], item, opt)) return true;
        return false;
    }
    if (!vr_is_string(key.vr)) {
        return key.value.size() == cand->value.size() &&
               std::equal(key.value.data(), key.value.data() + key.value.size(), cand->value.data());
    }
    const std::string_view kv = trimmed(key);
    const std::string_view cv = trim_value(cand->value.as_chars(), cand->vr);
    // A list of UIDs matches any (C.2.2.2.2); other keys hold one value, the candidate may hold several.
    const std::vector<std::string_view> keys = key.vr == VR::UI ? values_of(kv) : std::vector<std::string_view>{kv};
    for (std::string_view c : values_of(cv))
        for (std::string_view k : keys)
            if (value_matches(key, k, c, opt)) return true;
    return false;
}

// ------------------------------------------------------------------ service plumbing

// Sends a request on `a` and waits for its response, answering N-EVENT-REPORTs that arrive first.
Result<Scu::NResponse> call_on(Association& a, std::uint8_t pcid, Command rq, const Dataset* ds,
                               std::vector<std::uint16_t>* events) {
    rq.message_id = a.next_message_id();
    if (Status s = validate_command(rq); !s) return s;
    if (Status s = send_dimse(a, pcid, rq, ds); !s) return s;
    const std::uint16_t want = static_cast<std::uint16_t>(rq.command_field | 0x8000u);
    while (true) {
        Result<DimseReceived> r = receive_dimse(a, a.timeouts().dimse_ms);
        if (!r) {
            if (r.error().code == Errc::Timeout && a.established()) a.abort();
            return r.take_error();
        }
        if (r->kind == DimseKind::PeerReleaseRequest) {
            (void)a.respond_release();
            return Status(Errc::NetworkError, "peer released the association while a response was due");
        }
        const Message& m = r->message;
        if (m.command.command_field == want && m.command.message_id_being_responded_to == rq.message_id) {
            if (Status s = validate_command(m.command); !s) return s;
            Scu::NResponse out;
            out.command = m.command;
            if (m.has_data) {
                Result<Dataset> d = decode_message_dataset(m);
                if (!d) return d.take_error();
                out.dataset = std::move(d.value());
            }
            return out;
        }
        if (m.command.command_field == cmd::NEventReportRq && events) {
            events->push_back(m.command.event_type_id.value_or(0));
            if (Status s = send_dimse(a, m.context_id, make_response(m.command, dimse_status::Success)); !s) return s;
            continue;
        }
        a.abort(abort_src::Provider, abort_reason::UnexpectedPdu);
        return Status(Errc::Malformed, "unexpected DIMSE message while waiting for a response");
    }
}

// Opens an association to `ae` (as resolved by the service set) and sends the events. `current`
// (may be null) is told the Scu while it is in use, so a stopping sender can interrupt it.
Status send_events_on_new_association(const ServiceSet& s, const ScpConfig& cfg, const Timeouts& timeouts,
                                      const std::string& ae, const std::vector<EventToSend>& events,
                                      const std::function<bool(Scu*)>& current = {}) {
    if (events.empty()) return Status::ok();
    std::optional<MoveDestination> dest = s.resolve_ae ? s.resolve_ae(ae) : std::nullopt;
    if (!dest) return Status(Errc::NotFound, "no address for AE " + ae);
    ScuConfig sc;
    sc.calling_ae = cfg.ae_title;
    sc.called_ae = dest->ae_title.empty() ? ae : dest->ae_title;
    sc.host = dest->host;
    sc.port = dest->port;
    sc.timeouts = timeouts;
    sc.limits = cfg.limits;
    sc.tls = cfg.move_tls;  // as for C-MOVE sub-associations
    if (dest->tls) sc.tls.enabled = *dest->tls;
    Scu scu(sc);
    std::set<std::string> classes;
    for (const EventToSend& e : events) classes.insert(e.abstract_syntax);
    for (const std::string& cls : classes) {
        RoleSelection role;
        role.scu_role = false;
        role.scp_role = true;
        scu.add_context(cls, native_transfer_syntaxes(),
                        s.propose_role_reversal ? std::optional<RoleSelection>(role) : std::nullopt);
    }
    struct Current {
        const std::function<bool(Scu*)>& fn;
        ~Current() {
            if (fn) (void)fn(nullptr);
        }
    } guard{current};
    if (current && !current(&scu)) return Status(Errc::Cancelled, "stopping");
    if (Status st = scu.connect(); !st) return st;
    Status result = Status::ok();
    for (const EventToSend& e : events) {
        Association* a = scu.association();
        const NegotiatedContext* pc = a ? a->find_context(e.abstract_syntax) : nullptr;
        if (!pc) {
            result = Status(Errc::Unsupported, "event SOP class not accepted");
            continue;
        }
        Result<Scu::NResponse> r =
            call_on(*a, pc->id, make_n_event_report_rq(0, e.sop_class, e.instance, e.event_type, true), &e.info, nullptr);
        if (!r) {
            result = r.take_error();
            break;
        }
        // A response without a status counts as a failure (0xFFFF); never dereference the empty value.
        const std::uint16_t code = r->command.status.value_or(0xFFFF);
        if (is_failure(code)) result = status_error("N-EVENT-REPORT", code);
    }
    if (scu.connected()) (void)scu.release();
    return result;
}

// Hands event reports to the set's sender (R-117); without one they are sent at once on this thread.
// `done` (may be empty) receives the outcome: on the sender thread, or here when there is no sender
// or the queue is full.
void deliver_events(const ServiceSet& s, const ServiceContext& c, const std::string& ae,
                    std::vector<EventToSend> events, std::function<void(const Status&)> done) {
    if (!s.sender) {
        const Status st = send_events_on_new_association(s, c.cfg, s.event_timeouts, ae, events);
        if (done) done(st);
        return;
    }
    EventSender::Job job;
    job.ae = ae;
    job.events = std::move(events);
    job.done = done;
    if (!s.sender->post(std::move(job))) {
        if (c.log) c.log("event report to " + ae + " dropped: the event sender queue is full or stopping");
        if (done) done(Status(Errc::LimitExceeded, "event sender queue full"));
    }
}

bool is_ups_abstract(std::string_view cls) {
    return cls == sop::UpsPush || cls == sop::UpsPull || cls == sop::UpsWatch || cls == sop::UpsQuery;
}

ServiceOutcome reply(ServiceContext& c, const Message& m, Command rsp, const Dataset* ds = nullptr) {
    c.last_status = rsp.status.value_or(0);
    if (Status s = send_dimse(c.a, m.context_id, rsp, rsp.has_dataset ? ds : nullptr); !s) {
        if (c.log) c.log("sending DIMSE response failed: " + s.message);
        return ServiceOutcome::Failed;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome do_commitment_action(ServiceSet& s, ServiceContext& c, const Message& m) {
    const Command& rq = m.command;
    if (rq.action_type_id.value_or(0) != 1) return reply(c, m, make_response(rq, kNoSuchActionType));
    Result<Dataset> ds = decode_message_dataset(m);
    if (!ds) return reply(c, m, make_response(rq, dimse_status::ProcessingFailure));
    Result<CommitmentRequest> req = parse_commitment_request(*ds);
    if (!req) return reply(c, m, make_response(rq, dimse_status::MissingAttribute));
    req->calling_ae = c.a.calling_ae();
    if (reply(c, m, make_response(rq, dimse_status::Success)) != ServiceOutcome::Handled) return ServiceOutcome::Failed;

    CommitmentResult result = s.on_commitment(*req);
    result.transaction_uid = req->transaction_uid;
    const Dataset info = encode_commitment_result(result);
    bool delivered = false;
    if (s.commitment_report != CommitmentReportMode::NewAssociation) {
        Command ev = make_n_event_report_rq(c.a.next_message_id(), sop::StorageCommitmentPushModel,
                                            sop::StorageCommitmentPushModelInstance, result.event_type(), true);
        if (Status st = send_dimse(c.a, m.context_id, ev, &info); st) {
            Result<DimseReceived> r = receive_dimse(c.a, c.cfg.timeouts.dimse_ms);
            if (!r) {
                if (!c.a.established()) return ServiceOutcome::Failed;
            } else if (r->kind == DimseKind::PeerReleaseRequest) {
                c.release_pending = true;  // the requestor left before answering the report
            } else if (r->message.command.command_field == cmd::NEventReportRsp &&
                       r->message.command.message_id_being_responded_to == ev.message_id) {
                delivered = true;
            } else {
                if (c.log) c.log("unexpected message while waiting for N-EVENT-REPORT-RSP; aborting");
                c.a.abort(abort_src::Provider, abort_reason::UnexpectedPdu);
                return ServiceOutcome::Failed;
            }
        } else if (!c.a.established()) {
            return ServiceOutcome::Failed;
        }
    }
    if (!delivered && s.commitment_report != CommitmentReportMode::SameAssociation) {
        // On a new association: through the sender, so this association's thread is not held (R-117).
        EventToSend e{std::string(sop::StorageCommitmentPushModel), std::string(sop::StorageCommitmentPushModel),
                      std::string(sop::StorageCommitmentPushModelInstance), result.event_type(), info};
        const auto log = c.log;
        const auto on_done = s.on_commitment_done;
        const CommitmentRequest request = *req;
        const std::uint64_t association = c.a.id();
        deliver_events(s, c, req->calling_ae, {std::move(e)}, [log, on_done, request, result, association](const Status& st) {
            // Runs on the sender thread: what it reports still belongs to the requestor's association.
            const AssociationIdScope id_scope(association);
            if (!st && log) log("storage commitment report to " + request.calling_ae + " failed: " + st.message);
            if (on_done) on_done(request, result, static_cast<bool>(st));
        });
        return ServiceOutcome::Handled;
    }
    if (!delivered && c.log) c.log("storage commitment report not delivered");
    if (s.on_commitment_done) s.on_commitment_done(*req, result, delivered);
    return ServiceOutcome::Handled;
}

ServiceOutcome do_ups(ServiceSet& s, ServiceContext& c, const Message& m, std::string_view negotiated) {
    UpsProvider& ups = *s.ups;
    const Command& rq = m.command;
    const std::string& calling = c.a.calling_ae();
    Result<Dataset> ds = decode_message_dataset(m);
    if (!ds) return reply(c, m, make_response(rq, dimse_status::ProcessingFailure));
    ServiceOutcome out = ServiceOutcome::Handled;
    switch (rq.command_field) {
        case cmd::NCreateRq: {
            // Workitems are created through the UPS Push SOP Class only (PS3.4 CC.2.5).
            if (rq.affected_sop_class_uid != sop::UpsPush || negotiated != sop::UpsPush)
                return reply(c, m, make_response(rq, dimse_status::UnrecognizedOperation));
            std::string uid = rq.affected_sop_instance_uid.empty() ? make_uid() : rq.affected_sop_instance_uid;
            Command rsp = make_response(rq, ups.create(uid, *ds, calling));
            rsp.affected_sop_instance_uid = uid;
            out = reply(c, m, rsp);
            break;
        }
        case cmd::NGetRq: {
            Dataset got;
            const std::uint16_t st = ups.get(rq.requested_sop_instance_uid, rq.attribute_identifier_list, got);
            out = reply(c, m, make_response(rq, st, st == dimse_status::Success), &got);
            break;
        }
        case cmd::NSetRq:
            out = reply(c, m, make_response(rq, ups.set(rq.requested_sop_instance_uid, *ds, calling)));
            break;
        case cmd::NActionRq: {
            const std::string& uid = rq.requested_sop_instance_uid;
            std::uint16_t st = kNoSuchActionType;
            switch (rq.action_type_id.value_or(0)) {
                case 1:
                    st = ups.change_state(uid, str_of(*ds, kProcedureStepState), str_of(*ds, kTransactionUid), calling);
                    break;
                case 2: st = ups.request_cancel(uid, c.a.calling_ae(), *ds); break;
                case 3: {
                    const std::string ae = str_of(*ds, kReceivingAe);
                    if (ae.empty() || (s.resolve_ae && !s.resolve_ae(ae))) st = dimse_status::UpsUnknownReceivingAe;
                    else st = ups.subscribe(uid, ae, str_of(*ds, kDeletionLock) == "TRUE");
                    break;
                }
                case 4: st = ups.unsubscribe(uid, str_of(*ds, kReceivingAe)); break;
                case 5: st = ups.suspend_global(str_of(*ds, kReceivingAe)); break;
                default: break;
            }
            out = reply(c, m, make_response(rq, st));
            break;
        }
        case cmd::CFindRq: {
            QueryFlags f;
            for (const ExtendedNegotiation& e : c.a.accept_pdu().user.extended)
                if (e.sop_class_uid == rq.affected_sop_class_uid) f = parse_query_flags(e.info, true);
            MatchOptions mo;
            mo.fuzzy_person_names = f.fuzzy_person_names;
            mo.combined_datetime = f.combined_datetime;
            bool more = false;
            for (const Dataset& match : ups.find(*ds, mo, s.max_ups_find_results, &more)) {
                if (Status st = send_dimse(c.a, m.context_id, make_response(rq, dimse_status::Pending, true), &match); !st)
                    return ServiceOutcome::Failed;
            }
            out = reply(c, m, make_response(rq, more ? dimse_status::OutOfResources : dimse_status::Success));
            break;
        }
        default: return ServiceOutcome::NotHandled;
    }
    if (out != ServiceOutcome::Handled) return out;
    // Deliver the event reports the operation produced, one association per receiving AE, through
    // the sender. The context is negotiated for UPS Event; the Affected SOP Class UID of the
    // N-EVENT-REPORT is that of UPS Push (PS3.4 CC.2.4.3).
    std::map<std::string, std::vector<EventToSend>> by_ae;
    for (auto& [ae, ev] : ups.take_events())
        by_ae[ae].push_back({std::string(sop::UpsEvent), std::string(sop::UpsPush), ev.ups_uid, ev.event_type,
                             std::move(ev.info)});
    for (auto& [ae, list] : by_ae) {
        const auto log = c.log;
        const std::string to = ae;
        deliver_events(s, c, ae, std::move(list), [log, to](const Status& st) {
            if (!st && log) log("UPS event report to " + to + " failed: " + st.message);
        });
    }
    return out;
}

}  // namespace

// ===================================================================== general

std::string make_uid() {
    std::random_device rd;
    std::uint32_t w[4];
    for (std::uint32_t& x : w) x = static_cast<std::uint32_t>(rd());
    std::string digits;
    while (w[0] || w[1] || w[2] || w[3]) {
        std::uint64_t rem = 0;
        for (std::uint32_t& x : w) {
            const std::uint64_t cur = (rem << 32) | x;
            x = static_cast<std::uint32_t>(cur / 10);
            rem = cur % 10;
        }
        digits.push_back(static_cast<char>('0' + rem));
    }
    if (digits.empty()) digits = "0";
    std::reverse(digits.begin(), digits.end());
    return "2.25." + digits;
}

bool context_covers(std::string_view abstract_syntax, std::string_view sop_class) noexcept {
    if (abstract_syntax == sop_class) return true;
    // T3.2 (PS3.4 CC.2): on a context negotiated for any UPS SOP class the DIMSE-N commands name
    // the UPS Push SOP Class.
    if (sop_class == sop::UpsPush && (is_ups_abstract(abstract_syntax) || abstract_syntax == sop::UpsEvent)) return true;
    const bool gray = abstract_syntax == sop::BasicGrayscalePrintManagementMeta;
    const bool color = abstract_syntax == sop::BasicColorPrintManagementMeta;
    if (!gray && !color) return false;
    if (sop_class == sop::BasicFilmSession || sop_class == sop::BasicFilmBox || sop_class == sop::Printer) return true;
    return gray ? sop_class == sop::BasicGrayscaleImageBox : sop_class == sop::BasicColorImageBox;
}

// ===================================================================== matching

bool query_matches(const Dataset& identifier, const Dataset& candidate, const MatchOptions& opt) {
    for (const Element& key : identifier.elements()) {
        if (not_a_key(key.tag)) continue;
        if (!element_matches(key, candidate.find(key.tag), opt)) return false;
    }
    return true;
}

Dataset query_response(const Dataset& identifier, const Dataset& candidate) {
    Dataset out;
    if (const Element* cs = candidate.find(tags::SpecificCharacterSet)) out.put(*cs);
    for (const Element& key : identifier.elements()) {
        if (tag_element(key.tag) == 0x0000 || key.tag == tags::SpecificCharacterSet) continue;
        if (key.tag == kQueryRetrieveLevel) {
            out.put(key);
            continue;
        }
        const Element* c = candidate.find(key.tag);
        if (key.is_sequence()) {
            Element e = make_sequence(key.tag, {});
            if (c && c->is_sequence())
                for (const Dataset& item : c->items)
                    e.items.push_back(key.items.empty() || key.items[0].empty() ? item : query_response(key.items[0], item));
            out.put(std::move(e));
            continue;
        }
        if (c) {
            out.put(*c);
        } else {
            Element e;
            e.tag = key.tag;
            e.vr = key.vr;
            out.put(std::move(e));
        }
    }
    return out;
}

ExtendedNegotiation find_extended_negotiation(std::string_view sop_class, const QueryFlags& f) {
    ExtendedNegotiation e;
    e.sop_class_uid = std::string(sop_class);
    e.info = {static_cast<std::uint8_t>(f.relational), static_cast<std::uint8_t>(f.combined_datetime),
              static_cast<std::uint8_t>(f.fuzzy_person_names), static_cast<std::uint8_t>(f.timezone_adjustment)};
    return e;
}

ExtendedNegotiation retrieve_extended_negotiation(std::string_view sop_class, bool relational) {
    ExtendedNegotiation e;
    e.sop_class_uid = std::string(sop_class);
    e.info = {static_cast<std::uint8_t>(relational)};
    return e;
}

QueryFlags parse_query_flags(const std::vector<std::uint8_t>& info, bool is_find) noexcept {
    QueryFlags f;
    f.relational = !info.empty() && info[0] == 1;
    if (is_find) {
        f.combined_datetime = info.size() > 1 && info[1] == 1;
        f.fuzzy_person_names = info.size() > 2 && info[2] == 1;
        f.timezone_adjustment = info.size() > 3 && info[3] == 1;
    }
    return f;
}

QueryFlags negotiated_query_flags(const Association& a, std::string_view sop_class, bool is_find) {
    for (const ExtendedNegotiation& e : a.accept_pdu().user.extended)
        if (e.sop_class_uid == sop_class) return parse_query_flags(e.info, is_find);
    return {};
}

std::vector<ExtendedNegotiation> answer_extended_negotiation(const std::vector<ExtendedNegotiation>& proposed,
                                                             const std::vector<ExtendedNegotiation>& supported) {
    std::vector<ExtendedNegotiation> out;
    for (const ExtendedNegotiation& p : proposed)
        for (const ExtendedNegotiation& s : supported) {
            if (s.sop_class_uid != p.sop_class_uid) continue;
            ExtendedNegotiation r;
            r.sop_class_uid = p.sop_class_uid;
            for (std::size_t i = 0; i < p.info.size(); ++i)
                r.info.push_back(static_cast<std::uint8_t>(i < s.info.size() && s.info[i] == 1 && p.info[i] == 1));
            out.push_back(std::move(r));
            break;
        }
    return out;
}

std::uint16_t check_query_identifier(std::string_view sop_class, const Dataset& identifier, bool relational,
                                     bool is_retrieve) {
    const bool patient_root = sop_class == sop::PatientRootFind || sop_class == sop::PatientRootMove ||
                              sop_class == sop::PatientRootGet;
    const bool study_root = sop_class == sop::StudyRootFind || sop_class == sop::StudyRootMove ||
                            sop_class == sop::StudyRootGet;
    if (!patient_root && !study_root) return dimse_status::Success;
    const std::string level = identifier.get_string(kQueryRetrieveLevel).value_or(std::string());
    std::vector<std::pair<std::string, Tag>> levels;
    if (patient_root) levels.push_back({"PATIENT", tags::PatientID});
    levels.push_back({"STUDY", tags::StudyInstanceUID});
    levels.push_back({"SERIES", tags::SeriesInstanceUID});
    levels.push_back({"IMAGE", tags::SOPInstanceUID});
    std::size_t idx = levels.size();
    for (std::size_t i = 0; i < levels.size(); ++i)
        if (levels[i].first == level) idx = i;
    if (idx == levels.size()) return dimse_status::IdentifierDoesNotMatch;
    if (relational) return dimse_status::Success;
    auto single = [&](Tag t) {
        const std::optional<std::string> v = identifier.get_string(t);
        return v && !v->empty() && v->find_first_of("*?\\") == std::string::npos;
    };
    for (std::size_t i = 0; i < idx; ++i)
        if (!single(levels[i].second)) return dimse_status::IdentifierDoesNotMatch;
    if (is_retrieve) {
        const std::optional<std::string> v = identifier.get_string(levels[idx].second);
        if (!v || v->empty()) return dimse_status::IdentifierDoesNotMatch;
    }
    return dimse_status::Success;
}

Dataset make_worklist_query(std::string_view station_ae, std::string_view modality, std::string_view date,
                            std::string_view patient_name, std::string_view patient_id) {
    Dataset sps;
    (void)sps.set_string(kScheduledStationAe, VR::AE, station_ae);
    (void)sps.set_string(kSpsStartDate, VR::DA, date);
    (void)sps.set_string(kSpsStartTime, VR::TM, "");
    (void)sps.set_string(tags::Modality, VR::CS, modality);
    (void)sps.set_string(kScheduledPhysician, VR::PN, "");
    (void)sps.set_string(kSpsDescription, VR::LO, "");
    (void)sps.set_string(kSpsId, VR::SH, "");
    Dataset q;
    // Asked for, so an SCP that answers in e.g. ISO_IR 100 says so and Latin-1 names decode
    // correctly; when it returns none the responses are read with the default repertoire
    // (flagged as before). Testing-chat worklist finding.
    (void)q.set_string(0x00080005, VR::CS, "");
    (void)q.set_string(kAccessionNumber, VR::SH, "");
    (void)q.set_string(tags::PatientName, VR::PN, patient_name);
    (void)q.set_string(tags::PatientID, VR::LO, patient_id);
    (void)q.set_string(kPatientBirthDate, VR::DA, "");
    (void)q.set_string(kPatientSex, VR::CS, "");
    (void)q.set_string(tags::StudyInstanceUID, VR::UI, "");
    (void)q.set_string(kRequestedProcedureDescription, VR::LO, "");
    q.put(make_sequence(kSpsSequence, {std::move(sps)}));
    (void)q.set_string(kRequestedProcedureId, VR::SH, "");
    return q;
}

void strip_bulk_data(Dataset& ds) {
    std::vector<Tag> remove;
    for (Element& e : ds.elements()) {
        const Tag t = e.tag;
        const std::uint16_t g = tag_group(t), el = tag_element(t);
        const bool bulk = t == tags::PixelData || t == tags::FloatPixelData || t == tags::DoubleFloatPixelData ||
                          t == 0x7FE00001 || t == 0x7FE00002 ||                // Extended Offset Table (+ Lengths)
                          t == 0x54001010 || t == 0x56000020 || t == 0x00420011 ||  // Waveform, Spectroscopy, Document
                          ((g & 0xFF01u) == 0x6000u && el == 0x3000) ||     // Overlay Data
                          ((g & 0xFF01u) == 0x5000u && (el == 0x3000 || el == 0x200C));  // Curve / Audio data
        if (bulk) remove.push_back(t);
        else if (e.is_sequence())
            for (Dataset& item : e.items) strip_bulk_data(item);
    }
    for (Tag t : remove) (void)ds.remove(t);
}

// ===================================================================== storage commitment

Dataset encode_commitment_request(const CommitmentRequest& rq) {
    Dataset ds;
    (void)ds.set_string(kTransactionUid, VR::UI, rq.transaction_uid);
    std::vector<Dataset> items;
    for (const SopRef& r : rq.items) {
        Dataset i;
        (void)i.set_string(kReferencedSopClass, VR::UI, r.sop_class);
        (void)i.set_string(kReferencedSopInstance, VR::UI, r.sop_instance);
        items.push_back(std::move(i));
    }
    ds.put(make_sequence(kReferencedSopSequence, std::move(items)));
    return ds;
}

Result<CommitmentRequest> parse_commitment_request(const Dataset& ds) {
    CommitmentRequest rq;
    rq.transaction_uid = str_of(ds, kTransactionUid);
    if (rq.transaction_uid.empty()) return Status(Errc::Malformed, "storage commitment request lacks Transaction UID");
    const Element* seq = ds.find(kReferencedSopSequence);
    if (!seq || !seq->is_sequence() || seq->items.empty())
        return Status(Errc::Malformed, "storage commitment request lacks Referenced SOP Sequence");
    for (const Dataset& i : seq->items) rq.items.push_back({str_of(i, kReferencedSopClass), str_of(i, kReferencedSopInstance)});
    return rq;
}

Dataset encode_commitment_result(const CommitmentResult& r) {
    Dataset ds;
    (void)ds.set_string(kTransactionUid, VR::UI, r.transaction_uid);
    if (!r.retrieve_ae.empty()) (void)ds.set_string(kRetrieveAeTitle, VR::AE, r.retrieve_ae);
    if (!r.committed.empty()) {
        std::vector<Dataset> items;
        for (const SopRef& x : r.committed) {
            Dataset i;
            (void)i.set_string(kReferencedSopClass, VR::UI, x.sop_class);
            (void)i.set_string(kReferencedSopInstance, VR::UI, x.sop_instance);
            items.push_back(std::move(i));
        }
        ds.put(make_sequence(kReferencedSopSequence, std::move(items)));
    }
    if (!r.failed.empty()) {
        std::vector<Dataset> items;
        for (const CommitmentFailure& x : r.failed) {
            Dataset i;
            (void)i.set_string(kReferencedSopClass, VR::UI, x.ref.sop_class);
            (void)i.set_string(kReferencedSopInstance, VR::UI, x.ref.sop_instance);
            (void)i.set_uint16(kFailureReason, x.reason);
            items.push_back(std::move(i));
        }
        ds.put(make_sequence(kFailedSopSequence, std::move(items)));
    }
    return ds;
}

Result<CommitmentResult> parse_commitment_result(const Dataset& ds) {
    CommitmentResult r;
    r.transaction_uid = str_of(ds, kTransactionUid);
    if (r.transaction_uid.empty()) return Status(Errc::Malformed, "storage commitment result lacks Transaction UID");
    r.retrieve_ae = str_of(ds, kRetrieveAeTitle);
    if (const Element* s = ds.find(kReferencedSopSequence); s && s->is_sequence())
        for (const Dataset& i : s->items) r.committed.push_back({str_of(i, kReferencedSopClass), str_of(i, kReferencedSopInstance)});
    if (const Element* s = ds.find(kFailedSopSequence); s && s->is_sequence())
        for (const Dataset& i : s->items) {
            CommitmentFailure f;
            f.ref = {str_of(i, kReferencedSopClass), str_of(i, kReferencedSopInstance)};
            f.reason = static_cast<std::uint16_t>(i.get_int(kFailureReason).value_or(0x0110));
            r.failed.push_back(std::move(f));
        }
    return r;
}

Result<std::string> request_commitment(Scu& scu, const std::vector<SopRef>& items, std::string transaction_uid) {
    if (items.empty()) return Status(Errc::InvalidArgument, "nothing to commit");
    CommitmentRequest rq;
    rq.transaction_uid = transaction_uid.empty() ? make_uid() : std::move(transaction_uid);
    rq.items = items;
    const Dataset ds = encode_commitment_request(rq);
    Result<Scu::NResponse> r = scu.n_request(
        make_n_action_rq(0, sop::StorageCommitmentPushModel, sop::StorageCommitmentPushModelInstance, 1, true), &ds);
    if (!r) return r.take_error();
    const std::uint16_t st = r->command.status.value_or(0xFFFF);
    if (is_failure(st)) return status_error("storage commitment N-ACTION", st);
    return rq.transaction_uid;
}

Result<CommitmentResult> wait_commitment_result(Scu& scu, int timeout_ms) {
    Association* a = scu.association();
    if (!a || !a->established()) return Status(Errc::InvalidArgument, "not connected");
    Result<DimseReceived> r = receive_dimse(*a, timeout_ms);
    if (!r) return r.take_error();
    if (r->kind == DimseKind::PeerReleaseRequest) {
        (void)a->respond_release();
        return Status(Errc::NetworkError, "peer released the association before reporting");
    }
    const Message& m = r->message;
    if (m.command.command_field != cmd::NEventReportRq || m.command.sop_class() != sop::StorageCommitmentPushModel) {
        a->abort(abort_src::Provider, abort_reason::UnexpectedPdu);
        return Status(Errc::Malformed, "expected a storage commitment N-EVENT-REPORT");
    }
    Result<Dataset> ds = decode_message_dataset(m);
    Result<CommitmentResult> res = ds ? parse_commitment_result(*ds) : Result<CommitmentResult>(ds.take_error());
    const std::uint16_t st = res ? dimse_status::Success : dimse_status::ProcessingFailure;
    if (Status s = send_dimse(*a, m.context_id, make_response(m.command, st)); !s) return s;
    return res;
}

// ===================================================================== MPPS, IAN

Result<Scu::NResponse> mpps_create(Scu& scu, std::string instance_uid, const Dataset& attributes) {
    if (instance_uid.empty()) instance_uid = make_uid();
    return scu.n_request(make_n_create_rq(0, sop::ModalityPerformedProcedureStep, instance_uid, true), &attributes);
}

Result<Scu::NResponse> mpps_set(Scu& scu, const std::string& instance_uid, const Dataset& modifications) {
    return scu.n_request(make_n_set_rq(0, sop::ModalityPerformedProcedureStep, instance_uid), &modifications);
}

Result<Scu::NResponse> send_instance_availability(Scu& scu, const Dataset& notification, std::string instance_uid) {
    if (instance_uid.empty()) instance_uid = make_uid();
    return scu.n_request(make_n_create_rq(0, sop::InstanceAvailabilityNotification, instance_uid, true), &notification);
}

// ---- UPS SCU helpers

namespace {
// A DIMSE-N request on the context negotiated for `negotiated`; the command names UPS Push.
Result<Scu::NResponse> ups_call(Scu& scu, std::string_view negotiated, Command rq, const Dataset* ds) {
    Association* a = scu.association();
    if (!a || !a->established()) return Status(Errc::InvalidArgument, "not connected");
    const NegotiatedContext* pc = a->find_context(negotiated);
    if (!pc) return Status(Errc::Unsupported, "SOP class not accepted");
    return call_on(*a, pc->id, std::move(rq), ds, nullptr);
}
}  // namespace

Result<Scu::NResponse> ups_create(Scu& scu, std::string uid, const Dataset& attributes) {
    if (uid.empty()) uid = make_uid();
    return ups_call(scu, sop::UpsPush, make_n_create_rq(0, sop::UpsPush, uid, true), &attributes);
}

Result<Scu::NResponse> ups_get(Scu& scu, std::string_view sop_class, const std::string& uid,
                               const std::vector<Tag>& attributes) {
    return ups_call(scu, sop_class, make_n_get_rq(0, sop::UpsPush, uid, attributes), nullptr);
}

Result<Scu::NResponse> ups_set(Scu& scu, const std::string& uid, const Dataset& modifications) {
    return ups_call(scu, sop::UpsPull, make_n_set_rq(0, sop::UpsPush, uid), &modifications);
}

Result<Scu::NResponse> ups_change_state(Scu& scu, const std::string& uid, std::string_view state,
                                        const std::string& transaction_uid) {
    Dataset ds;
    (void)ds.set_string(kProcedureStepState, VR::CS, state);
    (void)ds.set_string(kTransactionUid, VR::UI, transaction_uid);
    return ups_call(scu, sop::UpsPull, make_n_action_rq(0, sop::UpsPush, uid, 1, true), &ds);
}

Result<Scu::NResponse> ups_request_cancel(Scu& scu, std::string_view sop_class, const std::string& uid,
                                          const Dataset& info) {
    return ups_call(scu, sop_class, make_n_action_rq(0, sop::UpsPush, uid, 2, !info.empty()),
                    info.empty() ? nullptr : &info);
}

Result<Scu::NResponse> ups_subscribe(Scu& scu, const std::string& uid, const std::string& receiving_ae,
                                     bool deletion_lock) {
    Dataset ds;
    (void)ds.set_string(kReceivingAe, VR::AE, receiving_ae);
    (void)ds.set_string(kDeletionLock, VR::CS, deletion_lock ? "TRUE" : "FALSE");
    return ups_call(scu, sop::UpsWatch, make_n_action_rq(0, sop::UpsPush, uid, 3, true), &ds);
}

Result<Scu::NResponse> ups_unsubscribe(Scu& scu, const std::string& uid, const std::string& receiving_ae) {
    Dataset ds;
    (void)ds.set_string(kReceivingAe, VR::AE, receiving_ae);
    return ups_call(scu, sop::UpsWatch, make_n_action_rq(0, sop::UpsPush, uid, 4, true), &ds);
}

Result<FindResult> ups_find(Scu& scu, std::string_view sop_class, const Dataset& identifier,
                            const std::function<bool(const Dataset&)>& on_result) {
    return scu.find(sop_class, identifier, on_result);
}

// ===================================================================== print management

PrintScu::PrintScu(Scu& scu, bool color) : scu_(scu), color_(color) {}

Result<Scu::NResponse> PrintScu::call(Command rq, const Dataset* ds) {
    Association* a = scu_.association();
    if (!a || !a->established()) return Status(Errc::InvalidArgument, "not connected");
    const NegotiatedContext* pc =
        a->find_context(color_ ? sop::BasicColorPrintManagementMeta : sop::BasicGrayscalePrintManagementMeta);
    if (!pc) return Status(Errc::Unsupported, "print management meta SOP class not accepted");
    Result<Scu::NResponse> r = call_on(*a, pc->id, std::move(rq), ds, &events_);
    if (r) last_status_ = r->command.status.value_or(0xFFFF);
    return r;
}

Result<Dataset> PrintScu::printer_status() {
    Result<Scu::NResponse> r =
        call(make_n_get_rq(0, sop::Printer, sop::PrinterInstance, {kPrinterStatus, kPrinterStatusInfo}), nullptr);
    if (!r) return r.take_error();
    if (is_failure(last_status_)) return status_error("Printer N-GET", last_status_);
    return std::move(r->dataset);
}

Result<std::string> PrintScu::create_film_session(const Dataset& attributes) {
    Result<Scu::NResponse> r = call(make_n_create_rq(0, sop::BasicFilmSession, "", true), &attributes);
    if (!r) return r.take_error();
    if (is_failure(last_status_)) return status_error("Film Session N-CREATE", last_status_);
    if (r->command.affected_sop_instance_uid.empty()) return Status(Errc::Malformed, "N-CREATE-RSP lacks the instance UID");
    session_uid_ = r->command.affected_sop_instance_uid;
    return session_uid_;
}

Result<PrintScu::FilmBox> PrintScu::create_film_box(const Dataset& attributes) {
    if (session_uid_.empty()) return Status(Errc::InvalidArgument, "no film session");
    Dataset ds = attributes;
    if (!ds.contains(kImageDisplayFormat)) (void)ds.set_string(kImageDisplayFormat, VR::ST, "STANDARD\\1,1");
    Dataset ref;
    (void)ref.set_string(kReferencedSopClass, VR::UI, sop::BasicFilmSession);
    (void)ref.set_string(kReferencedSopInstance, VR::UI, session_uid_);
    ds.put(make_sequence(kReferencedFilmSessionSequence, {std::move(ref)}));
    Result<Scu::NResponse> r = call(make_n_create_rq(0, sop::BasicFilmBox, "", true), &ds);
    if (!r) return r.take_error();
    if (is_failure(last_status_)) return status_error("Film Box N-CREATE", last_status_);
    FilmBox fb;
    fb.uid = r->command.affected_sop_instance_uid;
    if (const Element* s = r->dataset.find(kReferencedImageBoxSequence); s && s->is_sequence())
        for (const Dataset& i : s->items) fb.image_boxes.push_back(str_of(i, kReferencedSopInstance));
    return fb;
}

Status PrintScu::set_image_box(const std::string& image_box_uid, std::uint16_t position, const Dataset& image) {
    Dataset ds;
    (void)ds.set_uint16(kImageBoxPosition, position);
    ds.put(make_sequence(color_ ? kBasicColorImageSequence : kBasicGrayscaleImageSequence, {image}));
    Result<Scu::NResponse> r =
        call(make_n_set_rq(0, color_ ? sop::BasicColorImageBox : sop::BasicGrayscaleImageBox, image_box_uid), &ds);
    if (!r) return r.take_error();
    return is_failure(last_status_) ? status_error("Image Box N-SET", last_status_) : Status::ok();
}

Status PrintScu::print_film_box(const std::string& uid) {
    Result<Scu::NResponse> r = call(make_n_action_rq(0, sop::BasicFilmBox, uid, 1, false), nullptr);
    if (!r) return r.take_error();
    return is_failure(last_status_) ? status_error("Film Box N-ACTION", last_status_) : Status::ok();
}

Status PrintScu::print_film_session() {
    Result<Scu::NResponse> r = call(make_n_action_rq(0, sop::BasicFilmSession, session_uid_, 1, false), nullptr);
    if (!r) return r.take_error();
    return is_failure(last_status_) ? status_error("Film Session N-ACTION", last_status_) : Status::ok();
}

Status PrintScu::delete_film_box(const std::string& uid) {
    Result<Scu::NResponse> r = call(make_n_delete_rq(0, sop::BasicFilmBox, uid), nullptr);
    if (!r) return r.take_error();
    return is_failure(last_status_) ? status_error("Film Box N-DELETE", last_status_) : Status::ok();
}

Status PrintScu::delete_film_session() {
    Result<Scu::NResponse> r = call(make_n_delete_rq(0, sop::BasicFilmSession, session_uid_), nullptr);
    if (!r) return r.take_error();
    if (!is_failure(last_status_)) session_uid_.clear();
    return is_failure(last_status_) ? status_error("Film Session N-DELETE", last_status_) : Status::ok();
}

// ===================================================================== asynchronous C-STORE

std::uint16_t async_invoke_window(const Association& a) noexcept {
    const std::optional<AsyncWindow>& w = a.accept_pdu().user.async_window;
    if (!w || !a.request_pdu().user.async_window) return 1;
    return w->max_invoked;  // 0 = unlimited
}

Result<StoreManyResult> store_many(Scu& scu, const std::vector<std::string>& paths, std::uint16_t window) {
    Association* a = scu.association();
    if (!a || !a->established()) return Status(Errc::InvalidArgument, "not connected");
    const std::uint16_t granted = async_invoke_window(*a);
    std::size_t w = window ? window : (granted ? granted : 16);
    if (granted) w = std::min<std::size_t>(w, granted);
    if (w == 0) w = 1;
    StoreManyResult out;
    out.outcomes.resize(paths.size());
    std::map<std::uint16_t, std::size_t> outstanding;  // message ID -> index
    std::size_t next = 0;
    auto receive_one = [&]() -> Status {
        Result<DimseReceived> r = receive_dimse(*a, scu.config().timeouts.dimse_ms);
        if (!r) {
            if (r.error().code == Errc::Timeout && a->established()) a->abort();
            return r.take_error();
        }
        if (r->kind == DimseKind::PeerReleaseRequest) {
            (void)a->respond_release();
            return Status(Errc::NetworkError, "peer released the association with requests outstanding");
        }
        const Command& c = r->message.command;
        auto it = c.message_id_being_responded_to ? outstanding.find(*c.message_id_being_responded_to) : outstanding.end();
        if (c.command_field != cmd::CStoreRsp || it == outstanding.end()) {
            a->abort(abort_src::Provider, abort_reason::UnexpectedPdu);
            return Status(Errc::Malformed, "unexpected DIMSE message during asynchronous C-STORE");
        }
        out.outcomes[it->second].status = c.status.value_or(0xFFFF);
        outstanding.erase(it);
        return Status::ok();
    };
    while (next < paths.size() || !outstanding.empty()) {
        while (next < paths.size() && outstanding.size() < w) {
            StoreOutcome& o = out.outcomes[next];
            o.path = paths[next];
            ++next;
            Result<DicomFile> f = read_file(o.path);
            if (!f) {
                o.error = f.take_error();
                continue;
            }
            const std::string cls = f->dataset.get_string(tags::SOPClassUID).value_or("");
            o.sop_instance = f->dataset.get_string(tags::SOPInstanceUID).value_or("");
            if (cls.empty() || o.sop_instance.empty()) {
                o.error = Status(Errc::Malformed, "instance lacks SOP Class/Instance UID");
                continue;
            }
            if (Status ok = check_sendable(*f, scu.config().allow_incomplete); !ok) {
                o.error = ok;
                continue;
            }
            const NegotiatedContext* pc = nullptr;
            std::vector<std::uint8_t> body;
            Status enc = Status(Errc::Unsupported, "no accepted presentation context for this instance");
            codec::TranscodeReport report;
            for (const NegotiatedContext* k : store_contexts(*a, cls, f->transfer_syntax, false, scu.config().transcode,
                                                             scu.config().transcode_params)) {
                enc = encode_for_transfer_syntax(*f, k->transfer_syntax, scu.config().transcode_params, body, &report);
                if (enc) {
                    pc = k;
                    break;
                }
            }
            if (!pc) {
                o.error = enc;
                continue;
            }
            // R-093: a lossy transcode gave the image a new SOP Instance UID; send that one.
            if (!report.new_sop_instance_uid.empty() && report.new_sop_instance_uid != o.sop_instance) {
                o.original_sop_instance = o.sop_instance;
                o.sop_instance = report.new_sop_instance_uid;
            }
            const std::uint16_t mid = a->next_message_id();
            if (Status s = send_dimse_raw(*a, pc->id, make_c_store_rq(mid, cls, o.sop_instance), body.data(), body.size());
                !s)
                return s;
            outstanding[mid] = next - 1;
            out.max_outstanding = std::max(out.max_outstanding, outstanding.size());
        }
        if (!outstanding.empty())
            if (Status s = receive_one(); !s) return s;
    }
    return out;
}

// ===================================================================== SCP side

std::vector<AcceptedSyntax> service_policy(const ServiceSet& s, const std::vector<std::string>& ts) {
    std::vector<AcceptedSyntax> p;
    if (s.on_commitment || s.on_commitment_report)
        p.push_back({std::string(sop::StorageCommitmentPushModel), ts, true, static_cast<bool>(s.on_commitment_report)});
    if (s.on_mpps_create || s.on_mpps_set || s.mpps)
        p.push_back({std::string(sop::ModalityPerformedProcedureStep), ts, true, false});
    if (s.on_instance_availability)
        p.push_back({std::string(sop::InstanceAvailabilityNotification), ts, true, false});
    if (s.ups)
        for (std::string_view c : {sop::UpsPush, sop::UpsPull, sop::UpsWatch, sop::UpsQuery})
            p.push_back({std::string(c), ts, true, false});
    if (s.on_ups_event) p.push_back({std::string(sop::UpsEvent), ts, true, true});
    return p;
}

ServiceOutcome dispatch_service(ServiceSet& s, ServiceContext& c, const Message& m) {
    const Command& rq = m.command;
    const std::string& cls = rq.sop_class();
    const NegotiatedContext* pc = c.a.context(m.context_id);
    if (!pc) return ServiceOutcome::NotHandled;
    // T3.2 (PS3.4 CC.2): on an association negotiated for any UPS SOP class the DIMSE-N commands
    // name the UPS Push SOP Class; the negotiated UID is accepted as well.
    const std::string& negotiated = pc->abstract_syntax;
    const bool ups_request = s.ups && is_ups_abstract(negotiated) && (cls == negotiated || cls == sop::UpsPush);
    const bool ups_event = negotiated == sop::UpsEvent && (cls == sop::UpsEvent || cls == sop::UpsPush) &&
                           s.on_ups_event && rq.command_field == cmd::NEventReportRq;
    if (!ups_request && !ups_event && !context_covers(negotiated, cls)) return ServiceOutcome::NotHandled;

    // Requests this set serves pass the admission check first (allow-lists, limits).
    if (s.admit) {
        const bool mine =
            (cls == sop::StorageCommitmentPushModel &&
             ((rq.command_field == cmd::NActionRq && s.on_commitment) ||
              (rq.command_field == cmd::NEventReportRq && s.on_commitment_report))) ||
            (cls == sop::ModalityPerformedProcedureStep && (s.on_mpps_create || s.on_mpps_set || s.mpps) &&
             (rq.command_field == cmd::NCreateRq || rq.command_field == cmd::NSetRq)) ||
            (cls == sop::InstanceAvailabilityNotification && s.on_instance_availability &&
             rq.command_field == cmd::NCreateRq) ||
            ups_request || ups_event;
        if (mine)
            if (const std::uint16_t st = s.admit(c.a.calling_ae(), rq); st != dimse_status::Success)
                return reply(c, m, make_response(rq, st));
    }

    if (ups_request) {
        const ServiceOutcome o = do_ups(s, c, m, negotiated);
        if (o == ServiceOutcome::Handled && s.on_ups_served) s.on_ups_served(c.a.calling_ae(), rq, c.last_status);
        return o;
    }
    if (ups_event) {
        Result<Dataset> ds = decode_message_dataset(m);
        if (!ds) return reply(c, m, make_response(rq, dimse_status::ProcessingFailure));
        UpsEventReport ev;
        ev.ups_uid = rq.affected_sop_instance_uid;
        ev.event_type = rq.event_type_id.value_or(0);
        ev.info = std::move(ds.value());
        return reply(c, m, make_response(rq, s.on_ups_event(c.a.calling_ae(), ev)));
    }
    if (cls == sop::StorageCommitmentPushModel) {
        if (rq.command_field == cmd::NActionRq && s.on_commitment) return do_commitment_action(s, c, m);
        if (rq.command_field == cmd::NEventReportRq && s.on_commitment_report) {
            Result<Dataset> ds = decode_message_dataset(m);
            Result<CommitmentResult> r = ds ? parse_commitment_result(*ds) : Result<CommitmentResult>(ds.take_error());
            const std::uint16_t st = r ? s.on_commitment_report(c.a.calling_ae(), *r) : dimse_status::ProcessingFailure;
            return reply(c, m, make_response(rq, st));
        }
        return ServiceOutcome::NotHandled;
    }
    if (cls == sop::ModalityPerformedProcedureStep && (s.on_mpps_create || s.on_mpps_set || s.mpps) &&
        (rq.command_field == cmd::NCreateRq || rq.command_field == cmd::NSetRq)) {
        Result<Dataset> ds = decode_message_dataset(m);
        if (!ds) return reply(c, m, make_response(rq, dimse_status::ProcessingFailure));
        const bool create = rq.command_field == cmd::NCreateRq;
        std::string uid = rq.sop_instance();
        if (create && uid.empty()) uid = make_uid();
        std::uint16_t st = dimse_status::ProcessingFailure;
        const std::string& calling = c.a.calling_ae();
        if (create) st = s.on_mpps_create ? s.on_mpps_create(calling, uid, *ds)
                                          : (s.mpps ? s.mpps->create(uid, *ds, calling) : st);
        else st = s.on_mpps_set ? s.on_mpps_set(calling, uid, *ds) : (s.mpps ? s.mpps->set(uid, *ds, calling) : st);
        Command rsp = make_response(rq, st);
        rsp.affected_sop_instance_uid = uid;
        return reply(c, m, rsp);
    }
    if (cls == sop::InstanceAvailabilityNotification && s.on_instance_availability && rq.command_field == cmd::NCreateRq) {
        Result<Dataset> ds = decode_message_dataset(m);
        if (!ds) return reply(c, m, make_response(rq, dimse_status::ProcessingFailure));
        std::string uid = rq.affected_sop_instance_uid.empty() ? make_uid() : rq.affected_sop_instance_uid;
        Command rsp = make_response(rq, s.on_instance_availability(c.a.calling_ae(), uid, *ds));
        rsp.affected_sop_instance_uid = uid;
        return reply(c, m, rsp);
    }
    return ServiceOutcome::NotHandled;
}

// ===================================================================== event sender (R-117)

EventSender::EventSender(const ServiceSet& set, const ScpConfig& cfg, std::function<void(const std::string&)> log)
    : set_(set), cfg_(cfg), log_(std::move(log)) {
    thread_ = std::thread([this] { run(); });
}

EventSender::~EventSender() { stop(); }

bool EventSender::post(Job job) {
    {
        const std::lock_guard<std::mutex> l(mu_);
        if (stopping_ || queue_.size() >= set_.max_queued_event_jobs) return false;
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

std::size_t EventSender::queued() const {
    const std::lock_guard<std::mutex> l(mu_);
    return queue_.size();
}

void EventSender::stop() {
    {
        const std::lock_guard<std::mutex> l(mu_);
        stopping_ = true;
        if (current_) current_->interrupt();  // the outbound association ends at once
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void EventSender::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;  // what is still queued is dropped: the server is going away
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        Status st = Status::ok();
        try {
            st = send_events_on_new_association(set_, cfg_, set_.event_timeouts, job.ae, job.events, [this](Scu* scu) {
                const std::lock_guard<std::mutex> l(mu_);
                current_ = scu;
                return !(scu && stopping_);
            });
        } catch (...) {
            st = Status(Errc::Internal, "event report failed");
        }
        try {
            if (job.done) job.done(st);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a callback must not end the sender
        }
    }
}

}  // namespace siaara::net
