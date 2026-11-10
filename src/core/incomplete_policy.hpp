// SIAARA DICOM Toolkit - what a receiver does with a data set whose read was not complete
// (DicomFile::read_loss: truncated input, skipped bytes, heuristic recovery): R-116.
// One policy for the Storage SCP, the STOW-RS server and retrieve-to-store.
#pragma once

#include <cstdint>

#include "core/reader.hpp"

namespace siaara {

enum class IncompletePolicy : std::uint8_t {
    // Nothing is stored: C-STORE fails with status 0xC000 (Error: Cannot Understand), STOW-RS
    // lists the instance in the Failed SOP Sequence, a retrieve counts the part as failed.
    Refuse = 0,
    // The bytes are stored exactly as received - never transcoded, never rewritten - and the
    // receiver flags it (SCP event read_loss, STOW-RS Warning Reason, retrieve report).
    AcceptAsReceived = 1,
    // The bytes are kept exactly as received in the store's quarantine area (store/quarantine.hpp):
    // owner-only, outside the archive index, so no query or retrieve returns them, until an
    // operator releases them to the archive or deletes them. The sender is answered per
    // QuarantineResponse. Where no quarantine area is available the instance is refused.
    Quarantine = 2,
};

// What the sender is told about an instance that went to quarantine.
enum class QuarantineResponse : std::uint8_t {
    Warning = 0,  // C-STORE 0xB007 (Data Set does not match SOP Class), STOW-RS Warning Reason 0xB007
    Failure = 1,  // C-STORE 0xC000, STOW-RS Failure Reason 0xC000 (the bytes are quarantined all the same)
};
// The default answer. Change it here only.
inline constexpr QuarantineResponse kDefaultQuarantineResponse = QuarantineResponse::Warning;
// C-STORE status / STOW-RS Warning Reason for a quarantined instance answered with a warning.
inline constexpr std::uint16_t kQuarantineWarningStatus = 0xB007;

// The default everywhere (the rule already used for saving and sending: an incomplete data set
// is not passed on as if it were whole). Change it here only.
inline constexpr IncompletePolicy kDefaultIncompletePolicy = IncompletePolicy::Refuse;

// ReadOptions::recover_overlong_lengths for data that arrives over the network (C-STORE data
// sets at the SCP, C-GET / C-MOVE sub-operations received, every other DIMSE data set, STOW-RS
// parts at the server, WADO-RS parts kept by retrieve-to-store). THE ONE PLACE TO FLIP IT; it
// does not follow kRecoverOverlongLengthsDefault (local files). false: a length that overruns
// the data is not "recovered" by a heuristic - the data set then reads as truncated, and the
// IncompletePolicy decides.
inline constexpr bool kRecoverOverlongLengthsNetwork = false;

// The read options of every network receive path.
inline ReadOptions network_read_options() {
    ReadOptions o;
    o.recover_overlong_lengths = kRecoverOverlongLengthsNetwork;
    return o;
}

}  // namespace siaara
