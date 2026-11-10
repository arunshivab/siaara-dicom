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
    // Refused like Refuse (the sender gets the failure status), but the bytes are kept verbatim in
    // the store's "rejected" area - a second area like the quarantine, with its own caps - for
    // diagnosis. Where no such area is available it is a plain Refuse.
    RefuseKeepCopy = 3,
};

// THE default everywhere (owner decision 2026-11-11: quarantine). Change it here only. Where a
// receiver has no quarantine area (a C-GET into a folder only, a DICOMweb server over a custom
// store) Quarantine falls back to Refuse.
inline constexpr IncompletePolicy kDefaultIncompletePolicy = IncompletePolicy::Quarantine;

// What the sender is told about an instance that was kept although incomplete (AcceptAsReceived
// and Quarantine). Refuse and RefuseKeepCopy always answer the failure status.
enum class QuarantineResponse : std::uint8_t {
    Warning = 0,  // the warning status below (C-STORE), STOW-RS Warning Reason
    Failure = 1,  // the failure status below (the bytes are kept all the same)
    Success = 2,  // 0x0000, no warning (STOW-RS: no Warning Reason)
};
// The default answer. Change it here only.
inline constexpr QuarantineResponse kDefaultQuarantineResponse = QuarantineResponse::Warning;
// Default C-STORE warning status / STOW-RS Warning Reason for a kept incomplete instance
// (0xB007 Data Set does not match SOP Class; 0xB000 and 0xB006 are the other standard choices).
inline constexpr std::uint16_t kQuarantineWarningStatus = 0xB007;
// Default C-STORE failure status / STOW-RS Failure Reason for a refused incomplete instance
// (0xC000 Cannot Understand; 0xA900 is the other usual choice).
inline constexpr std::uint16_t kIncompleteFailureStatus = 0xC000;
// Storage Commitment for instances that arrived incomplete. Quarantined and rejected copies are
// not in the archive and are never committed. One kept in the archive and flagged
// (AcceptAsReceived, or released from quarantine) is committed when this is true.
inline constexpr bool kCommitFlaggedIncomplete = true;

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
