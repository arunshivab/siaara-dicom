// SIAARA DICOM Toolkit - choosing presentation contexts for C-STORE and transcoding on the fly
// (PS3.4 B.4.1: an SCU may convert an instance to a transfer syntax the SCP accepted; PS3.5 10).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "codec/transcode.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/association.hpp"
#include "net/scu.hpp"
#include "net/spool.hpp"

namespace siaara::net {

// Accepted presentation contexts that can carry an instance of `sop_class` stored in `file_ts`,
// best first: the same transfer syntax, then native re-encodings, then (when `transcode`)
// lossless transcodes, then lossy ones if p.allow_lossy. `requestor_scp` selects contexts on
// which the association requestor plays the Storage SCP role (C-GET sub-operations).
std::vector<const NegotiatedContext*> store_contexts(const Association& a, std::string_view sop_class,
                                                     std::string_view file_ts, bool requestor_scp, bool transcode,
                                                     const codec::TranscodeParams& p);

// Encodes the data set of `file` in `ts`, transcoding pixel data when the encodings differ.
Status encode_for_transfer_syntax(const DicomFile& file, std::string_view ts, const codec::TranscodeParams& p,
                                  std::vector<std::uint8_t>& body);

// Re-encodes a received instance in `ts` (raw bytes, data set and transfer syntax are replaced).
// A spooled instance is written to a new spool file in `spool`'s directory.
Status convert_received(StoreRequest& rq, std::string_view ts, const codec::TranscodeParams& p,
                        const SpoolConfig* spool);

}  // namespace siaara::net
