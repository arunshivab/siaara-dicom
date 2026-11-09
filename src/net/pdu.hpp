// SIAARA DICOM Toolkit - Upper Layer protocol data units (DICOM PS3.8 section 9.3,
// user information sub-items from PS3.7 Annex D.3.3 and PS3.8 Annex D).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::net {

class Transport;

constexpr std::string_view kDicomApplicationContext = "1.2.840.10008.3.1.1.1";

enum class PduType : std::uint8_t {
    AssociateRq = 0x01,
    AssociateAc = 0x02,
    AssociateRj = 0x03,
    PData = 0x04,
    ReleaseRq = 0x05,
    ReleaseRp = 0x06,
    Abort = 0x07,
};

// Hard limits applied while parsing anything that came from the wire.
struct PduLimits {
    std::uint64_t max_pdu_length = 16u << 20;   // PDU-length field cap, checked before allocation
    // A-ASSOCIATE-RQ/AC (and unknown PDU types) are held to this smaller cap; A-ASSOCIATE-RJ,
    // A-RELEASE-RQ/RP and A-ABORT must be exactly 4 bytes long. Checked before allocation.
    std::uint64_t max_associate_length = 64u << 10;
    // P-DATA-TF cap when non-zero (an association sets it from its advertised maximum length).
    std::uint64_t max_pdata_length = 0;
    std::size_t max_presentation_contexts = 128; // PS3.8: context IDs are odd numbers 1..255
    std::size_t max_transfer_syntaxes = 64;      // per presentation context
    std::size_t max_user_items = 256;            // user information sub-items
    std::size_t max_related_sop_classes = 64;    // per common extended negotiation item
    std::size_t max_pdvs = 4096;                 // PDV items per P-DATA-TF
};

// Presentation context results (PS3.8 table 9-18).
namespace pc_result {
constexpr std::uint8_t Acceptance = 0;
constexpr std::uint8_t UserRejection = 1;
constexpr std::uint8_t NoReason = 2;
constexpr std::uint8_t AbstractSyntaxNotSupported = 3;
constexpr std::uint8_t TransferSyntaxesNotSupported = 4;
}  // namespace pc_result

struct PresentationContextRq {
    std::uint8_t id = 1;
    std::string abstract_syntax;
    std::vector<std::string> transfer_syntaxes;
    bool operator==(const PresentationContextRq&) const = default;
};

struct PresentationContextAc {
    std::uint8_t id = 1;
    std::uint8_t result = pc_result::Acceptance;
    std::string transfer_syntax;  // meaningful only when accepted
    bool operator==(const PresentationContextAc&) const = default;
};

// SCP/SCU role selection (PS3.7 D.3.3.4).
struct RoleSelection {
    std::string sop_class_uid;
    bool scu_role = true;
    bool scp_role = false;
    bool operator==(const RoleSelection&) const = default;
};

// SOP class extended negotiation (PS3.7 D.3.3.5): opaque service-class application information.
struct ExtendedNegotiation {
    std::string sop_class_uid;
    std::vector<std::uint8_t> info;
    bool operator==(const ExtendedNegotiation&) const = default;
};

// SOP class common extended negotiation (PS3.7 D.3.3.6), request only.
struct CommonExtendedNegotiation {
    std::string sop_class_uid;
    std::string service_class_uid;
    std::vector<std::string> related_general_sop_classes;
    bool operator==(const CommonExtendedNegotiation&) const = default;
};

// User identity negotiation (PS3.7 D.3.3.7).
namespace identity_type {
constexpr std::uint8_t Username = 1;
constexpr std::uint8_t UsernamePasscode = 2;
constexpr std::uint8_t Kerberos = 3;
constexpr std::uint8_t Saml = 4;
constexpr std::uint8_t Jwt = 5;
}  // namespace identity_type

struct UserIdentityRq {
    std::uint8_t type = identity_type::Username;
    bool positive_response_requested = false;
    std::vector<std::uint8_t> primary;
    std::vector<std::uint8_t> secondary;
    bool operator==(const UserIdentityRq&) const = default;
};

struct UserIdentityAc {
    std::vector<std::uint8_t> server_response;
    bool operator==(const UserIdentityAc&) const = default;
};

struct AsyncWindow {
    std::uint16_t max_invoked = 1;
    std::uint16_t max_performed = 1;
    bool operator==(const AsyncWindow&) const = default;
};

struct UserInformation {
    std::uint32_t max_length = 0;  // maximum P-DATA-TF PDU length the sender can receive; 0 = no limit
    std::string implementation_class_uid;
    std::string implementation_version_name;  // optional, at most 16 characters
    std::optional<AsyncWindow> async_window;
    std::vector<RoleSelection> roles;
    std::vector<ExtendedNegotiation> extended;
    std::vector<CommonExtendedNegotiation> common_extended;  // RQ only
    std::optional<UserIdentityRq> identity_rq;                // RQ only
    std::optional<UserIdentityAc> identity_ac;                // AC only
    bool operator==(const UserInformation&) const = default;
};

struct AssociateRq {
    std::uint16_t protocol_version = 1;
    std::string called_ae;   // up to 16 characters, stored without padding
    std::string calling_ae;
    std::string application_context{kDicomApplicationContext};
    std::vector<PresentationContextRq> contexts;
    UserInformation user;
    bool operator==(const AssociateRq&) const = default;
};

struct AssociateAc {
    std::uint16_t protocol_version = 1;
    std::string called_ae;   // reserved fields in the AC, echoed from the RQ
    std::string calling_ae;
    std::string application_context{kDicomApplicationContext};
    std::vector<PresentationContextAc> contexts;
    UserInformation user;
    bool operator==(const AssociateAc&) const = default;
};

// A-ASSOCIATE-RJ (PS3.8 table 9-21).
namespace rj {
constexpr std::uint8_t Permanent = 1;
constexpr std::uint8_t Transient = 2;
constexpr std::uint8_t SourceUser = 1;          // reasons 1, 2, 3, 7
constexpr std::uint8_t SourceAcseProvider = 2;  // reasons 1, 2
constexpr std::uint8_t SourcePresentationProvider = 3;  // reasons 1, 2
constexpr std::uint8_t UserNoReason = 1;
constexpr std::uint8_t UserAppContextNotSupported = 2;
constexpr std::uint8_t UserCallingAeNotRecognized = 3;
constexpr std::uint8_t UserCalledAeNotRecognized = 7;
constexpr std::uint8_t AcseNoReason = 1;
constexpr std::uint8_t AcseProtocolVersionNotSupported = 2;
constexpr std::uint8_t PresentationTemporaryCongestion = 1;
constexpr std::uint8_t PresentationLocalLimitExceeded = 2;
}  // namespace rj

struct AssociateRj {
    std::uint8_t result = rj::Permanent;
    std::uint8_t source = rj::SourceUser;
    std::uint8_t reason = rj::UserNoReason;
    bool operator==(const AssociateRj&) const = default;
};

// One presentation data value item. `data` shares the received PDU buffer (no copy).
struct Pdv {
    std::uint8_t context_id = 1;
    bool command = false;
    bool last = false;
    Bytes data;
};

struct PDataTf {
    std::vector<Pdv> pdvs;
};

struct ReleaseRq {
    bool operator==(const ReleaseRq&) const = default;
};
struct ReleaseRp {
    bool operator==(const ReleaseRp&) const = default;
};

// A-ABORT (PS3.8 table 9-26).
namespace abort_src {
constexpr std::uint8_t User = 0;
constexpr std::uint8_t Provider = 2;
}  // namespace abort_src
namespace abort_reason {
constexpr std::uint8_t NotSpecified = 0;
constexpr std::uint8_t UnrecognizedPdu = 1;
constexpr std::uint8_t UnexpectedPdu = 2;
constexpr std::uint8_t UnrecognizedPduParameter = 4;
constexpr std::uint8_t UnexpectedPduParameter = 5;
constexpr std::uint8_t InvalidPduParameterValue = 6;
}  // namespace abort_reason

struct Abort {
    std::uint8_t source = abort_src::User;
    std::uint8_t reason = abort_reason::NotSpecified;
    bool operator==(const Abort&) const = default;
};

using Pdu = std::variant<AssociateRq, AssociateAc, AssociateRj, PDataTf, ReleaseRq, ReleaseRp, Abort>;

PduType pdu_type(const Pdu& p) noexcept;

// Encodes a PDU (header included). Fails on values that cannot be represented
// (AE titles over 16 characters, UIDs over 64 characters, lengths over 32 bits).
Status encode_pdu(const Pdu& pdu, std::vector<std::uint8_t>& out);

// Encodes the 6-byte P-DATA-TF header plus one PDV header for `data_len` bytes of payload.
// The payload itself follows in the stream (lets the sender avoid copying large data sets).
void encode_single_pdv_header(std::uint8_t context_id, bool command, bool last, std::uint32_t data_len,
                              std::uint8_t out[12]) noexcept;

// Parses a complete PDU (header included). `buf` owns the bytes, so P-DATA values can reference it.
// Error codes: Malformed (bad structure/values), Truncated, LimitExceeded, Unsupported (unknown type).
Result<Pdu> parse_pdu(const Bytes& buf, const PduLimits& limits = {});

// Reads one PDU from a transport. The PDU length field is checked against the cap for its type
// (pdu_length_cap) before any allocation, and the buffer grows only as bytes arrive.
// `timeout_ms` bounds the whole PDU (absolute deadline), not each read.
// Unknown PDU types still have their body read (bounded by the cap) and return Errc::Unsupported.
Result<Pdu> read_pdu(Transport& t, int timeout_ms, const PduLimits& limits = {});
// Largest PDU length field accepted for a PDU of `type` under `limits`.
std::uint64_t pdu_length_cap(std::uint8_t type, const PduLimits& limits) noexcept;

// Validation helpers (PS3.5 9.1 UID syntax; PS3.5 6.2 AE).
bool is_valid_uid(std::string_view uid) noexcept;
bool is_valid_ae_title(std::string_view ae) noexcept;

}  // namespace siaara::net
