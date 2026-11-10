// SIAARA DICOM Toolkit - what a receiver does with a data set whose read was not complete
// (DicomFile::read_loss: truncated input, skipped bytes, heuristic recovery): R-116.
// One policy for the Storage SCP, the STOW-RS server and retrieve-to-store.
#pragma once

#include <cstdint>

namespace siaara {

enum class IncompletePolicy : std::uint8_t {
    // Nothing is stored: C-STORE fails with status 0xC000 (Error: Cannot Understand), STOW-RS
    // lists the instance in the Failed SOP Sequence, a retrieve counts the part as failed.
    Refuse = 0,
    // The bytes are stored exactly as received - never transcoded, never rewritten - and the
    // receiver flags it (SCP event read_loss, STOW-RS Warning Reason, retrieve report).
    AcceptAsReceived = 1,
};

// The default everywhere (the rule already used for saving and sending: an incomplete data set
// is not passed on as if it were whole). Change it here only.
inline constexpr IncompletePolicy kDefaultIncompletePolicy = IncompletePolicy::Refuse;

}  // namespace siaara
