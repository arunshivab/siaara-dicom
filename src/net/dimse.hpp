// SIAARA DICOM Toolkit - DIMSE messages (DICOM PS3.7 sections 9 and 10, Annex E)
// and presentation context proposal helpers.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/association.hpp"

namespace siaara::net {

// Command Field values (PS3.7 E.1).
namespace cmd {
constexpr std::uint16_t CStoreRq = 0x0001, CStoreRsp = 0x8001;
constexpr std::uint16_t CGetRq = 0x0010, CGetRsp = 0x8010;
constexpr std::uint16_t CFindRq = 0x0020, CFindRsp = 0x8020;
constexpr std::uint16_t CMoveRq = 0x0021, CMoveRsp = 0x8021;
constexpr std::uint16_t CEchoRq = 0x0030, CEchoRsp = 0x8030;
constexpr std::uint16_t NEventReportRq = 0x0100, NEventReportRsp = 0x8100;
constexpr std::uint16_t NGetRq = 0x0110, NGetRsp = 0x8110;
constexpr std::uint16_t NSetRq = 0x0120, NSetRsp = 0x8120;
constexpr std::uint16_t NActionRq = 0x0130, NActionRsp = 0x8130;
constexpr std::uint16_t NCreateRq = 0x0140, NCreateRsp = 0x8140;
constexpr std::uint16_t NDeleteRq = 0x0150, NDeleteRsp = 0x8150;
constexpr std::uint16_t CCancelRq = 0x0FFF;
}  // namespace cmd

// Common status codes (PS3.7 Annex C and service-specific values from PS3.4).
namespace dimse_status {
constexpr std::uint16_t Success = 0x0000;
constexpr std::uint16_t Pending = 0xFF00;
constexpr std::uint16_t PendingWarning = 0xFF01;
constexpr std::uint16_t Cancel = 0xFE00;
constexpr std::uint16_t SubOperationsWarning = 0xB000;
constexpr std::uint16_t OutOfResources = 0xA700;
constexpr std::uint16_t OutOfResourcesMatches = 0xA701;
constexpr std::uint16_t OutOfResourcesSubOps = 0xA702;
constexpr std::uint16_t MoveDestinationUnknown = 0xA801;
constexpr std::uint16_t IdentifierDoesNotMatch = 0xA900;
constexpr std::uint16_t DataSetDoesNotMatch = 0xA900;  // C-STORE: Data Set does not match SOP Class
constexpr std::uint16_t UnableToProcess = 0xC000;
constexpr std::uint16_t CannotUnderstand = 0xC000;
constexpr std::uint16_t ProcessingFailure = 0x0110;
constexpr std::uint16_t NoSuchSopClass = 0x0118;
constexpr std::uint16_t NoSuchSopInstance = 0x0112;
constexpr std::uint16_t SopClassNotSupported = 0x0122;
constexpr std::uint16_t UnrecognizedOperation = 0x0211;
constexpr std::uint16_t DuplicateInvocation = 0x0210;
}  // namespace dimse_status

enum class StatusClass { Success, Pending, Cancel, Warning, Failure };
StatusClass classify_status(std::uint16_t status) noexcept;

constexpr std::uint16_t kNoDataSet = 0x0101;     // Command Data Set Type: no data set
constexpr std::uint16_t kDataSetPresent = 0x0001;

namespace priority {
constexpr std::uint16_t Medium = 0x0000, High = 0x0001, Low = 0x0002;
}

// Well-known SOP class UIDs used by the services in this layer.
namespace sop {
constexpr std::string_view Verification = "1.2.840.10008.1.1";
constexpr std::string_view PatientRootFind = "1.2.840.10008.5.1.4.1.2.1.1";
constexpr std::string_view PatientRootMove = "1.2.840.10008.5.1.4.1.2.1.2";
constexpr std::string_view PatientRootGet = "1.2.840.10008.5.1.4.1.2.1.3";
constexpr std::string_view StudyRootFind = "1.2.840.10008.5.1.4.1.2.2.1";
constexpr std::string_view StudyRootMove = "1.2.840.10008.5.1.4.1.2.2.2";
constexpr std::string_view StudyRootGet = "1.2.840.10008.5.1.4.1.2.2.3";
constexpr std::string_view ModalityWorklistFind = "1.2.840.10008.5.1.4.31";
constexpr std::string_view StorageCommitmentPushModel = "1.2.840.10008.1.20.1";
constexpr std::string_view ModalityPerformedProcedureStep = "1.2.840.10008.3.1.2.3.3";
}  // namespace sop

// Common storage SOP classes (PS3.4 Annex B), for SCPs that accept "all storage".
const std::vector<std::string>& storage_sop_classes();
// Native transfer syntaxes in a sensible default order: explicit LE, implicit LE, explicit BE.
std::vector<std::string> native_transfer_syntaxes();

// A DIMSE command set. Fields left empty / nullopt are not encoded.
struct Command {
    std::uint16_t command_field = 0;
    std::optional<std::uint16_t> message_id;
    std::optional<std::uint16_t> message_id_being_responded_to;
    std::string affected_sop_class_uid;
    std::string requested_sop_class_uid;
    std::string affected_sop_instance_uid;
    std::string requested_sop_instance_uid;
    bool has_dataset = false;
    std::optional<std::uint16_t> priority;
    std::optional<std::uint16_t> status;
    std::string move_destination;
    std::string move_originator_ae;
    std::optional<std::uint16_t> move_originator_message_id;
    std::optional<std::uint16_t> remaining;
    std::optional<std::uint16_t> completed;
    std::optional<std::uint16_t> failed;
    std::optional<std::uint16_t> warning;
    std::optional<std::uint16_t> event_type_id;
    std::optional<std::uint16_t> action_type_id;
    std::optional<std::vector<Tag>> attribute_identifier_list;
    std::string error_comment;
    std::optional<std::uint16_t> error_id;
    std::vector<Tag> offending_elements;

    bool is_response() const noexcept { return (command_field & 0x8000u) != 0; }
    // Affected SOP class, falling back to the requested one (N-GET/N-SET/N-ACTION/N-DELETE).
    const std::string& sop_class() const noexcept {
        return affected_sop_class_uid.empty() ? requested_sop_class_uid : affected_sop_class_uid;
    }
    const std::string& sop_instance() const noexcept {
        return affected_sop_instance_uid.empty() ? requested_sop_instance_uid : affected_sop_instance_uid;
    }
};

// Encodes as Implicit VR Little Endian with Command Group Length (0000,0000) first (PS3.7 6.3.1).
Status encode_command(const Command& c, std::vector<std::uint8_t>& out);
// Decodes and checks the fields required for the command field (PS3.7 9.3 and 10.3).
Result<Command> decode_command(const Bytes& raw);
// Checks required fields for the command field.
Status validate_command(const Command& c);
// For Association::receive: does a data set follow this command?
Result<bool> command_has_dataset(const Bytes& raw);

// ---- builders for requests (PS3.7 9.3, 10.3)
Command make_c_echo_rq(std::uint16_t message_id, std::string_view sop_class = sop::Verification);
Command make_c_store_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance,
                        std::uint16_t prio = priority::Medium, std::string_view move_originator_ae = {},
                        std::optional<std::uint16_t> move_originator_message_id = std::nullopt);
Command make_c_find_rq(std::uint16_t message_id, std::string_view sop_class, std::uint16_t prio = priority::Medium);
Command make_c_get_rq(std::uint16_t message_id, std::string_view sop_class, std::uint16_t prio = priority::Medium);
Command make_c_move_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view destination,
                       std::uint16_t prio = priority::Medium);
Command make_c_cancel_rq(std::uint16_t message_id_being_cancelled);
Command make_n_event_report_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance,
                               std::uint16_t event_type_id, bool has_dataset);
Command make_n_get_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance,
                      const std::vector<Tag>& attributes = {});
Command make_n_set_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance);
Command make_n_action_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance,
                         std::uint16_t action_type_id, bool has_dataset);
Command make_n_create_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance,
                         bool has_dataset);
Command make_n_delete_rq(std::uint16_t message_id, std::string_view sop_class, std::string_view sop_instance);
// Builds the response for a request: command field | 0x8000, Message ID Being Responded To,
// affected SOP class/instance and event/action type copied as PS3.7 requires.
Command make_response(const Command& rq, std::uint16_t status, bool has_dataset = false);

// ---- messages over an association
struct Message {
    std::uint8_t context_id = 0;
    Command command;
    bool has_data = false;
    Bytes data;                   // raw data set in `transfer_syntax`
    std::string transfer_syntax;  // of the presentation context
    std::string abstract_syntax;
    std::shared_ptr<SpoolFile> spool;  // set when the data set was received to disk (net/spool.hpp)
    bool discarded = false;  // data set drained, not kept: spool quota / free-space floor (R-074)
};

enum class DimseKind { Message, PeerReleaseRequest };
struct DimseReceived {
    DimseKind kind = DimseKind::Message;
    Message message;
};

// Sends a command plus optional data set encoded in the context's transfer syntax.
Status send_dimse(Association& a, std::uint8_t context_id, const Command& c, const Dataset* ds = nullptr);
// Same, with a data set already encoded in the context's transfer syntax.
Status send_dimse_raw(Association& a, std::uint8_t context_id, const Command& c, const std::uint8_t* data,
                      std::size_t len);
Result<DimseReceived> receive_dimse(Association& a, int timeout_ms);
// Same; data sets larger than spool->threshold are received into a Part 10 spool file.
Result<DimseReceived> receive_dimse(Association& a, int timeout_ms, const SpoolConfig* spool);
// Parses the data set of a message with its transfer syntax.
Result<Dataset> decode_message_dataset(const Message& m);

// ---- presentation context proposals
struct ProposedContext {
    std::string abstract_syntax;
    std::vector<std::string> transfer_syntaxes;
    // When set, a role selection item is sent for the abstract syntax.
    std::optional<RoleSelection> role;
};
// Fills rq.contexts (odd IDs from 1) and rq.user.roles. At most 128 contexts.
Status build_proposals(const std::vector<ProposedContext>& proposals, AssociateRq& rq);

}  // namespace siaara::net
