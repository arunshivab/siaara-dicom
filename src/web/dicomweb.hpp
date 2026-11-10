// SIAARA DICOM Toolkit - definitions shared by the DICOMweb client and server (PS3.18).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::web {

namespace media {
constexpr std::string_view Dicom = "application/dicom";
constexpr std::string_view DicomJson = "application/dicom+json";
constexpr std::string_view DicomXml = "application/dicom+xml";
constexpr std::string_view OctetStream = "application/octet-stream";
constexpr std::string_view Json = "application/json";
constexpr std::string_view MultipartRelated = "multipart/related";
}  // namespace media

enum class QueryLevel : std::uint8_t { Study, Series, Instance };

// Attribute tags used in responses (PS3.6).
namespace webtags {
constexpr Tag RetrieveURL = 0x00081190;
constexpr Tag FailedSOPSequence = 0x00081198;
constexpr Tag ReferencedSOPSequence = 0x00081199;
constexpr Tag ReferencedSOPClassUID = 0x00081150;
constexpr Tag ReferencedSOPInstanceUID = 0x00081155;
constexpr Tag FailureReason = 0x00081197;
constexpr Tag WarningReason = 0x00081196;
}  // namespace webtags

// STOW-RS failure reasons (PS3.18 / PS3.4 C-STORE status classes).
namespace failure {
constexpr std::uint16_t ProcessingFailure = 0x0110;
constexpr std::uint16_t SopClassNotSupported = 0x0122;
constexpr std::uint16_t OutOfResources = 0xA700;
constexpr std::uint16_t DataSetDoesNotMatchSopClass = 0xA900;
constexpr std::uint16_t CannotUnderstand = 0xC000;
constexpr std::uint16_t TransferSyntaxNotSupported = 0xC122;
}  // namespace failure

// Warning Reason (0008,1196) values of a STOW-RS response (PS3.18 table I.2-1).
namespace warning {
constexpr std::uint16_t CoercionOfDataElements = 0xB000;
constexpr std::uint16_t ElementsDiscarded = 0xB006;
constexpr std::uint16_t DataSetDoesNotMatchSopClass = 0xB007;
}  // namespace warning

// Media type of the bulk data of a transfer syntax (PS3.18 table 8.7.3-5), "application/octet-stream"
// for native and unknown syntaxes.
std::string_view media_type_for_transfer_syntax(std::string_view ts_uid) noexcept;

// A DICOM UID as it may appear in a URL path: 1..64 characters of digits and dots.
bool is_uid(std::string_view s) noexcept;

}  // namespace siaara::web
