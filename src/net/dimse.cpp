// SIAARA DICOM Toolkit - DIMSE command sets and message transport (PS3.7 6.3, 9, 10, Annex E).
#include "net/dimse.hpp"

#include <vector>
#include <string>
#include <cstdint>

#include "core/incomplete_policy.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"

namespace siaara::net {

namespace {

// Command element tags (PS3.7 E.1).
constexpr Tag kGroupLength = 0x00000000;
constexpr Tag kAffectedSopClass = 0x00000002;
constexpr Tag kRequestedSopClass = 0x00000003;
constexpr Tag kCommandField = 0x00000100;
constexpr Tag kMessageId = 0x00000110;
constexpr Tag kMessageIdResponded = 0x00000120;
constexpr Tag kMoveDestination = 0x00000600;
constexpr Tag kPriority = 0x00000700;
constexpr Tag kDataSetType = 0x00000800;
constexpr Tag kStatus = 0x00000900;
constexpr Tag kOffendingElement = 0x00000901;
constexpr Tag kErrorComment = 0x00000902;
constexpr Tag kErrorId = 0x00000903;
constexpr Tag kAffectedSopInstance = 0x00001000;
constexpr Tag kRequestedSopInstance = 0x00001001;
constexpr Tag kEventTypeId = 0x00001002;
constexpr Tag kAttributeIdList = 0x00001005;
constexpr Tag kActionTypeId = 0x00001008;
constexpr Tag kRemaining = 0x00001020;
constexpr Tag kCompleted = 0x00001021;
constexpr Tag kFailed = 0x00001022;
constexpr Tag kWarning = 0x00001023;
constexpr Tag kMoveOriginatorAe = 0x00001030;
constexpr Tag kMoveOriginatorMid = 0x00001031;

Status put_tags(Dataset& ds, Tag t, const std::vector<Tag>& list) {
    std::vector<std::uint8_t> b(list.size() * 4);
    for (std::size_t i = 0; i < list.size(); ++i) {
        store_le16(b.data() + 4 * i, tag_group(list[i]));
        store_le16(b.data() + 4 * i + 2, tag_element(list[i]));
    }
    return ds.set_bytes(t, VR::AT, Bytes::copy_of(b.data(), b.size()));
}

std::vector<Tag> get_tags(const Dataset& ds, Tag t) {
    std::vector<Tag> out;
    const Element* e = ds.find(t);
    if (!e) return out;
    const std::size_t n = e->value.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = e->value.data() + 4 * i;
        out.push_back(make_tag(load_le16(p), load_le16(p + 2)));
    }
    return out;
}

Status put_u16(Dataset& ds, Tag t, const std::optional<std::uint16_t>& v) {
    return v ? ds.set_uint16(t, *v) : Status::ok();
}
Status put_str(Dataset& ds, Tag t, VR vr, const std::string& v) {
    return v.empty() ? Status::ok() : ds.set_string(t, vr, v);
}

Result<std::optional<std::uint16_t>> get_u16(const Dataset& ds, Tag t) {
    const Element* e = ds.find(t);
    if (!e) return std::optional<std::uint16_t>{};
    if (e->value.size() != 2) return Status(Errc::Malformed, "command element has wrong length", 0, t);
    return std::optional<std::uint16_t>(load_le16(e->value.data()));
}

std::string get_str(const Dataset& ds, Tag t) { return ds.get_string(t).value_or(std::string()); }

Command base(std::uint16_t field, std::optional<std::uint16_t> mid, bool has_ds) {
    Command c;
    c.command_field = field;
    c.message_id = mid;
    c.has_dataset = has_ds;
    return c;
}

Status require(bool ok, const char* what, Tag t) {
    return ok ? Status::ok() : Status(Errc::Malformed, std::string("command lacks ") + what, 0, t);
}

}  // namespace

StatusClass classify_status(std::uint16_t s) noexcept {
    if (s == 0x0000) return StatusClass::Success;
    if (s == 0xFF00 || s == 0xFF01) return StatusClass::Pending;
    if (s == 0xFE00) return StatusClass::Cancel;
    if (s == 0x0001 || s == 0x0107 || s == 0x0116 || (s & 0xF000u) == 0xB000u) return StatusClass::Warning;
    return StatusClass::Failure;
}

const std::vector<std::string>& storage_sop_classes() {
    static const std::vector<std::string> v = {
        "1.2.840.10008.5.1.4.1.1.1",       // CR
        "1.2.840.10008.5.1.4.1.1.1.1",     // DX for presentation
        "1.2.840.10008.5.1.4.1.1.1.1.1",   // DX for processing
        "1.2.840.10008.5.1.4.1.1.1.2",     // mammography for presentation
        "1.2.840.10008.5.1.4.1.1.1.2.1",   // mammography for processing
        "1.2.840.10008.5.1.4.1.1.2",       // CT
        "1.2.840.10008.5.1.4.1.1.2.1",     // enhanced CT
        "1.2.840.10008.5.1.4.1.1.3.1",     // US multi-frame
        "1.2.840.10008.5.1.4.1.1.4",       // MR
        "1.2.840.10008.5.1.4.1.1.4.1",     // enhanced MR
        "1.2.840.10008.5.1.4.1.1.6.1",     // US
        "1.2.840.10008.5.1.4.1.1.7",       // secondary capture
        "1.2.840.10008.5.1.4.1.1.7.1",     // multi-frame single bit SC
        "1.2.840.10008.5.1.4.1.1.7.2",     // multi-frame grayscale byte SC
        "1.2.840.10008.5.1.4.1.1.7.3",     // multi-frame grayscale word SC
        "1.2.840.10008.5.1.4.1.1.7.4",     // multi-frame true color SC
        "1.2.840.10008.5.1.4.1.1.11.1",    // grayscale softcopy presentation state
        "1.2.840.10008.5.1.4.1.1.12.1",    // XA
        "1.2.840.10008.5.1.4.1.1.12.2",    // XRF
        "1.2.840.10008.5.1.4.1.1.20",      // NM
        "1.2.840.10008.5.1.4.1.1.66",      // raw data
        "1.2.840.10008.5.1.4.1.1.66.4",    // segmentation
        "1.2.840.10008.5.1.4.1.1.77.1.6",  // VL whole slide microscopy
        "1.2.840.10008.5.1.4.1.1.88.11",   // basic text SR
        "1.2.840.10008.5.1.4.1.1.88.22",   // enhanced SR
        "1.2.840.10008.5.1.4.1.1.88.33",   // comprehensive SR
        "1.2.840.10008.5.1.4.1.1.104.1",   // encapsulated PDF
        "1.2.840.10008.5.1.4.1.1.128",     // PET
        "1.2.840.10008.5.1.4.1.1.130",     // enhanced PET
        "1.2.840.10008.5.1.4.1.1.481.1",   // RT image
        "1.2.840.10008.5.1.4.1.1.481.2",   // RT dose
        "1.2.840.10008.5.1.4.1.1.481.3",   // RT structure set
        "1.2.840.10008.5.1.4.1.1.481.4",   // RT beams treatment record
        "1.2.840.10008.5.1.4.1.1.481.5",   // RT plan
        "1.2.840.10008.5.1.4.1.1.481.8",   // RT ion plan
    };
    return v;
}

std::vector<std::string> native_transfer_syntaxes() {
    return {std::string(ts::ExplicitVRLittleEndian), std::string(ts::ImplicitVRLittleEndian),
            std::string(ts::ExplicitVRBigEndian)};
}

// ------------------------------------------------------------------ encode / decode

Status encode_command(const Command& c, std::vector<std::uint8_t>& out) {
    Dataset ds;
    Status s = Status::ok();
    auto chain = [&](Status x) {
        if (s && !x) s = std::move(x);
    };
    chain(put_str(ds, kAffectedSopClass, VR::UI, c.affected_sop_class_uid));
    chain(put_str(ds, kRequestedSopClass, VR::UI, c.requested_sop_class_uid));
    chain(ds.set_uint16(kCommandField, c.command_field));
    chain(put_u16(ds, kMessageId, c.message_id));
    chain(put_u16(ds, kMessageIdResponded, c.message_id_being_responded_to));
    chain(put_str(ds, kMoveDestination, VR::AE, c.move_destination));
    chain(put_u16(ds, kPriority, c.priority));
    chain(ds.set_uint16(kDataSetType, c.has_dataset ? kDataSetPresent : kNoDataSet));
    chain(put_u16(ds, kStatus, c.status));
    if (!c.offending_elements.empty()) chain(put_tags(ds, kOffendingElement, c.offending_elements));
    chain(put_str(ds, kErrorComment, VR::LO, c.error_comment));
    chain(put_u16(ds, kErrorId, c.error_id));
    chain(put_str(ds, kAffectedSopInstance, VR::UI, c.affected_sop_instance_uid));
    chain(put_str(ds, kRequestedSopInstance, VR::UI, c.requested_sop_instance_uid));
    chain(put_u16(ds, kEventTypeId, c.event_type_id));
    if (c.attribute_identifier_list) chain(put_tags(ds, kAttributeIdList, *c.attribute_identifier_list));
    chain(put_u16(ds, kActionTypeId, c.action_type_id));
    chain(put_u16(ds, kRemaining, c.remaining));
    chain(put_u16(ds, kCompleted, c.completed));
    chain(put_u16(ds, kFailed, c.failed));
    chain(put_u16(ds, kWarning, c.warning));
    chain(put_str(ds, kMoveOriginatorAe, VR::AE, c.move_originator_ae));
    chain(put_u16(ds, kMoveOriginatorMid, c.move_originator_message_id));
    if (!s) return s;
    std::vector<std::uint8_t> body;
    if (Status w = write_dataset(ds, ts::ImplicitVRLittleEndian, body); !w) return w;
    if (body.size() > 0xFFFFFFF0u) return Status(Errc::LimitExceeded, "command set too large");
    out.resize(12);
    store_le16(out.data(), 0x0000);
    store_le16(out.data() + 2, 0x0000);
    store_le32(out.data() + 4, 4);
    store_le32(out.data() + 8, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return Status::ok();
}

Result<Command> decode_command(const Bytes& raw) {
    ReadOptions opt;
    opt.lenient = false;
    opt.allow_raw_dataset = true;
    opt.max_depth = 1;
    opt.max_elements = 256;
    Result<Dataset> rd = read_dataset(raw, ts::ImplicitVRLittleEndian, opt);
    if (!rd) return Status(Errc::Malformed, "cannot parse command set: " + rd.error().message);
    const Dataset& ds = *rd;
    for (const Element& e : ds.elements())
        if (tag_group(e.tag) != 0x0000) return Status(Errc::Malformed, "non-command element in command set", 0, e.tag);
    if (const Element* gl = ds.find(kGroupLength)) {
        if (gl->value.size() != 4) return Status(Errc::Malformed, "bad command group length");
        const std::uint64_t claimed = load_le32(gl->value.data());
        if (claimed + 12 != raw.size()) return Status(Errc::Malformed, "command group length mismatch");
    }
    Command c;
    auto cf = get_u16(ds, kCommandField);
    if (!cf) return cf.take_error();
    if (!cf.value()) return Status(Errc::Malformed, "command lacks Command Field", 0, kCommandField);
    c.command_field = *cf.value();
    auto dst = get_u16(ds, kDataSetType);
    if (!dst) return dst.take_error();
    if (!dst.value()) return Status(Errc::Malformed, "command lacks Command Data Set Type", 0, kDataSetType);
    c.has_dataset = *dst.value() != kNoDataSet;

    struct U16Field {
        Tag tag;
        std::optional<std::uint16_t> Command::*field;
    };
    const U16Field u16s[] = {
        {kMessageId, &Command::message_id},
        {kMessageIdResponded, &Command::message_id_being_responded_to},
        {kPriority, &Command::priority},
        {kStatus, &Command::status},
        {kErrorId, &Command::error_id},
        {kEventTypeId, &Command::event_type_id},
        {kActionTypeId, &Command::action_type_id},
        {kRemaining, &Command::remaining},
        {kCompleted, &Command::completed},
        {kFailed, &Command::failed},
        {kWarning, &Command::warning},
        {kMoveOriginatorMid, &Command::move_originator_message_id},
    };
    for (const U16Field& f : u16s) {
        auto v = get_u16(ds, f.tag);
        if (!v) return v.take_error();
        c.*(f.field) = v.value();
    }
    c.affected_sop_class_uid = get_str(ds, kAffectedSopClass);
    c.requested_sop_class_uid = get_str(ds, kRequestedSopClass);
    c.affected_sop_instance_uid = get_str(ds, kAffectedSopInstance);
    c.requested_sop_instance_uid = get_str(ds, kRequestedSopInstance);
    c.move_destination = get_str(ds, kMoveDestination);
    c.move_originator_ae = get_str(ds, kMoveOriginatorAe);
    c.error_comment = get_str(ds, kErrorComment);
    if (ds.contains(kAttributeIdList)) c.attribute_identifier_list = get_tags(ds, kAttributeIdList);
    c.offending_elements = get_tags(ds, kOffendingElement);
    return c;
}

Result<bool> command_has_dataset(const Bytes& raw) {
    Result<Command> c = decode_command(raw);
    if (!c) return c.take_error();
    return c->has_dataset;
}

Status validate_command(const Command& c) {
    const bool mid = c.message_id.has_value();
    const bool rsp_ok = c.message_id_being_responded_to.has_value() && c.status.has_value();
    if (c.is_response()) {
        switch (c.command_field) {
            case cmd::CStoreRsp: case cmd::CGetRsp: case cmd::CFindRsp: case cmd::CMoveRsp: case cmd::CEchoRsp:
            case cmd::NEventReportRsp: case cmd::NGetRsp: case cmd::NSetRsp: case cmd::NActionRsp:
            case cmd::NCreateRsp: case cmd::NDeleteRsp:
                return require(rsp_ok, "Message ID Being Responded To or Status", kStatus);
            default:
                return Status(Errc::Unsupported, "unknown response command field");
        }
    }
    switch (c.command_field) {
        case cmd::CEchoRq:
            return require(mid && !c.affected_sop_class_uid.empty(), "Message ID or Affected SOP Class", kMessageId);
        case cmd::CStoreRq:
            if (Status s = require(mid && !c.affected_sop_class_uid.empty() && !c.affected_sop_instance_uid.empty(),
                                   "Message ID or Affected SOP Class/Instance", kMessageId); !s)
                return s;
            if (Status s = require(c.priority.has_value(), "Priority", kPriority); !s) return s;
            return require(c.has_dataset, "data set", kDataSetType);
        case cmd::CFindRq:
        case cmd::CGetRq:
        case cmd::CMoveRq:
            if (Status s = require(mid && !c.affected_sop_class_uid.empty(), "Message ID or Affected SOP Class",
                                   kMessageId); !s)
                return s;
            if (Status s = require(c.priority.has_value(), "Priority", kPriority); !s) return s;
            if (c.command_field == cmd::CMoveRq)
                if (Status s = require(!c.move_destination.empty(), "Move Destination", kMoveDestination); !s)
                    return s;
            return require(c.has_dataset, "identifier", kDataSetType);
        case cmd::CCancelRq:
            return require(c.message_id_being_responded_to.has_value(), "Message ID Being Responded To",
                           kMessageIdResponded);
        case cmd::NEventReportRq:
            return require(mid && !c.affected_sop_class_uid.empty() && !c.affected_sop_instance_uid.empty() &&
                               c.event_type_id.has_value(),
                           "N-EVENT-REPORT required fields", kEventTypeId);
        case cmd::NGetRq:
        case cmd::NDeleteRq:
            return require(mid && !c.requested_sop_class_uid.empty() && !c.requested_sop_instance_uid.empty(),
                           "Requested SOP Class/Instance", kRequestedSopClass);
        case cmd::NSetRq:
            if (Status s = require(mid && !c.requested_sop_class_uid.empty() && !c.requested_sop_instance_uid.empty(),
                                   "Requested SOP Class/Instance", kRequestedSopClass); !s)
                return s;
            return require(c.has_dataset, "modification list", kDataSetType);
        case cmd::NActionRq:
            return require(mid && !c.requested_sop_class_uid.empty() && !c.requested_sop_instance_uid.empty() &&
                               c.action_type_id.has_value(),
                           "N-ACTION required fields", kActionTypeId);
        case cmd::NCreateRq:
            return require(mid && !c.affected_sop_class_uid.empty(), "Affected SOP Class", kAffectedSopClass);
        default:
            return Status(Errc::Unsupported, "unknown command field");
    }
}

// ------------------------------------------------------------------ builders

Command make_c_echo_rq(std::uint16_t mid, std::string_view sop_class) {
    Command c = base(cmd::CEchoRq, mid, false);
    c.affected_sop_class_uid = sop_class;
    return c;
}

Command make_c_store_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance,
                        std::uint16_t prio, std::string_view move_originator_ae,
                        std::optional<std::uint16_t> move_originator_message_id) {
    Command c = base(cmd::CStoreRq, mid, true);
    c.affected_sop_class_uid = sop_class;
    c.affected_sop_instance_uid = sop_instance;
    c.priority = prio;
    c.move_originator_ae = move_originator_ae;
    c.move_originator_message_id = move_originator_message_id;
    return c;
}

Command make_c_find_rq(std::uint16_t mid, std::string_view sop_class, std::uint16_t prio) {
    Command c = base(cmd::CFindRq, mid, true);
    c.affected_sop_class_uid = sop_class;
    c.priority = prio;
    return c;
}

Command make_c_get_rq(std::uint16_t mid, std::string_view sop_class, std::uint16_t prio) {
    Command c = make_c_find_rq(mid, sop_class, prio);
    c.command_field = cmd::CGetRq;
    return c;
}

Command make_c_move_rq(std::uint16_t mid, std::string_view sop_class, std::string_view destination,
                       std::uint16_t prio) {
    Command c = make_c_find_rq(mid, sop_class, prio);
    c.command_field = cmd::CMoveRq;
    c.move_destination = destination;
    return c;
}

Command make_c_cancel_rq(std::uint16_t mid_cancelled) {
    Command c = base(cmd::CCancelRq, std::nullopt, false);
    c.message_id_being_responded_to = mid_cancelled;
    return c;
}

Command make_n_event_report_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance,
                               std::uint16_t event_type_id, bool has_dataset) {
    Command c = base(cmd::NEventReportRq, mid, has_dataset);
    c.affected_sop_class_uid = sop_class;
    c.affected_sop_instance_uid = sop_instance;
    c.event_type_id = event_type_id;
    return c;
}

Command make_n_get_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance,
                      const std::vector<Tag>& attributes) {
    Command c = base(cmd::NGetRq, mid, false);
    c.requested_sop_class_uid = sop_class;
    c.requested_sop_instance_uid = sop_instance;
    if (!attributes.empty()) c.attribute_identifier_list = attributes;
    return c;
}

Command make_n_set_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance) {
    Command c = base(cmd::NSetRq, mid, true);
    c.requested_sop_class_uid = sop_class;
    c.requested_sop_instance_uid = sop_instance;
    return c;
}

Command make_n_action_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance,
                         std::uint16_t action_type_id, bool has_dataset) {
    Command c = base(cmd::NActionRq, mid, has_dataset);
    c.requested_sop_class_uid = sop_class;
    c.requested_sop_instance_uid = sop_instance;
    c.action_type_id = action_type_id;
    return c;
}

Command make_n_create_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance,
                         bool has_dataset) {
    Command c = base(cmd::NCreateRq, mid, has_dataset);
    c.affected_sop_class_uid = sop_class;
    c.affected_sop_instance_uid = sop_instance;
    return c;
}

Command make_n_delete_rq(std::uint16_t mid, std::string_view sop_class, std::string_view sop_instance) {
    Command c = base(cmd::NDeleteRq, mid, false);
    c.requested_sop_class_uid = sop_class;
    c.requested_sop_instance_uid = sop_instance;
    return c;
}

Command make_response(const Command& rq, std::uint16_t status, bool has_dataset) {
    Command c;
    c.command_field = static_cast<std::uint16_t>(rq.command_field | 0x8000u);
    c.message_id_being_responded_to = rq.message_id;
    c.status = status;
    c.has_dataset = has_dataset;
    // Responses carry the Affected SOP Class/Instance even for requests that used "Requested".
    c.affected_sop_class_uid = rq.sop_class();
    switch (rq.command_field) {
        case cmd::CStoreRq:
        case cmd::NEventReportRq:
        case cmd::NGetRq:
        case cmd::NSetRq:
        case cmd::NActionRq:
        case cmd::NCreateRq:
        case cmd::NDeleteRq:
            c.affected_sop_instance_uid = rq.sop_instance();
            break;
        default:
            break;
    }
    if (rq.command_field == cmd::NEventReportRq) c.event_type_id = rq.event_type_id;
    if (rq.command_field == cmd::NActionRq) c.action_type_id = rq.action_type_id;
    return c;
}

// ------------------------------------------------------------------ messages

Status send_dimse_raw(Association& a, std::uint8_t context_id, const Command& c, const std::uint8_t* data,
                      std::size_t len) {
    std::vector<std::uint8_t> cmdbuf;
    if (Status s = encode_command(c, cmdbuf); !s) return s;
    const bool with_data = c.has_dataset;
    return a.send_message(context_id, cmdbuf.data(), cmdbuf.size(), with_data ? data : nullptr,
                          with_data ? len : 0);
}

Status send_dimse(Association& a, std::uint8_t context_id, const Command& c, const Dataset* ds) {
    std::vector<std::uint8_t> body;
    if (c.has_dataset) {
        const NegotiatedContext* pc = a.context(context_id);
        if (!pc || !pc->accepted()) return Status(Errc::InvalidArgument, "presentation context not accepted");
        Dataset empty;
        if (Status s = write_dataset(ds ? *ds : empty, pc->transfer_syntax, body); !s) return s;
        if (body.empty()) {
            // A zero-length data set PDV is legal but some peers mishandle it; nothing to do.
        }
    }
    return send_dimse_raw(a, context_id, c, body.data(), body.size());
}

Result<DimseReceived> receive_dimse(Association& a, int timeout_ms) { return receive_dimse(a, timeout_ms, nullptr); }

Result<DimseReceived> receive_dimse(Association& a, int timeout_ms, const SpoolConfig* spool) {
    const std::string peer_ae = a.is_requestor() ? a.called_ae() : a.calling_ae();
    const SpoolPrefixFn prefix = [&peer_ae](const Bytes& command, const NegotiatedContext& pc)
        -> Result<std::vector<std::uint8_t>> {
        Result<Command> c = decode_command(command);
        if (!c) return c.take_error();
        std::vector<std::uint8_t> out;
        if (Status s = part10_header(c->affected_sop_class_uid, c->affected_sop_instance_uid, pc.transfer_syntax,
                                     peer_ae, out); !s)
            return s;
        return out;
    };
    Result<Received> r = spool ? a.receive(timeout_ms, command_has_dataset, spool, prefix)
                               : a.receive(timeout_ms, command_has_dataset);
    if (!r) return r.take_error();
    DimseReceived out;
    if (r->kind == ReceiveKind::PeerReleaseRequest) {
        out.kind = DimseKind::PeerReleaseRequest;
        return out;
    }
    Result<Command> c = decode_command(r->message.command);
    if (!c) {
        a.abort(abort_src::Provider, abort_reason::InvalidPduParameterValue);
        return c.take_error();
    }
    out.message.context_id = r->message.context_id;
    out.message.command = std::move(c.value());
    out.message.has_data = r->message.has_data;
    out.message.data = std::move(r->message.data);
    out.message.spool = std::move(r->message.spool);
    out.message.discarded = r->message.discarded;
    if (const NegotiatedContext* pc = a.context(out.message.context_id)) {
        out.message.transfer_syntax = pc->transfer_syntax;
        out.message.abstract_syntax = pc->abstract_syntax;
    }
    return out;
}

Result<Dataset> decode_message_dataset(const Message& m) {
    if (!m.has_data) return Dataset{};
    return read_dataset(m.data, m.transfer_syntax, network_read_options());
}

Status build_proposals(const std::vector<ProposedContext>& proposals, AssociateRq& rq) {
    if (proposals.size() > 128) return Status(Errc::LimitExceeded, "more than 128 presentation contexts");
    rq.contexts.clear();
    rq.user.roles.clear();
    unsigned id = 1;
    for (const ProposedContext& p : proposals) {
        if (p.transfer_syntaxes.empty()) return Status(Errc::InvalidArgument, "proposal without transfer syntaxes");
        rq.contexts.push_back({static_cast<std::uint8_t>(id), p.abstract_syntax, p.transfer_syntaxes});
        id += 2;
        if (p.role) {
            bool dup = false;
            for (const RoleSelection& r : rq.user.roles) dup = dup || r.sop_class_uid == p.abstract_syntax;
            if (!dup) {
                RoleSelection r = *p.role;
                r.sop_class_uid = p.abstract_syntax;
                rq.user.roles.push_back(std::move(r));
            }
        }
    }
    return Status::ok();
}

}  // namespace siaara::net
