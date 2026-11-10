// SIAARA DICOM Toolkit - choosing presentation contexts for C-STORE and transcoding on the fly
// (PS3.4 B.4.1: an SCU may convert an instance to a transfer syntax the SCP accepted; PS3.5 10).
#pragma once

#include <cstdint>
#include <string>
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

// Compressed transfer syntaxes to propose for C-STORE besides the file's own and the native ones,
// so that an instance can be transcoded for a peer that accepts only those: the ones the
// transcoder writes without loss (`lossy` false), or - when p.allow_lossy - the lossy ones it can
// write (`lossy` true; JPEG Baseline / Extended, and JPEG-LS near-lossless, lossy JPEG 2000 and
// HTJ2K when p.lossy). Proposed as separate presentation contexts (testing 10b P2).
std::vector<std::string> transcode_targets(const codec::TranscodeParams& p, bool lossy);

// Whether `file` may be sent (C-STORE, C-GET / C-MOVE sub-operations): a file whose read was not
// complete (DicomFile::read_loss: truncated input, skipped bytes, header-only read) would reach
// the peer as an instance that looks whole but holds less than the source, so it is refused
// (Errc::Rejected, message starting "INCOMPLETE_INPUT") unless `allow_incomplete` - the same
// rule as WriteOptions::allow_incomplete for saving (testing 10b P1).
Status check_sendable(const DicomFile& file, bool allow_incomplete);
inline constexpr std::string_view kIncompleteInput = "INCOMPLETE_INPUT";

// Encodes the data set of `file` in `ts`, transcoding pixel data when the encodings differ.
// `report` (may be null) receives what the transcoder did: when a lossy transcode gave the image a
// new SOP Instance UID (report->new_sop_instance_uid), the caller must use that UID in the
// C-STORE command, the file meta and any index (R-093).
Status encode_for_transfer_syntax(const DicomFile& file, std::string_view ts, const codec::TranscodeParams& p,
                                  std::vector<std::uint8_t>& body, codec::TranscodeReport* report = nullptr);

// Re-encodes a received instance in `ts` (raw bytes, data set and transfer syntax are replaced).
// A spooled instance is written to a new spool file in `spool`'s directory. When a lossy
// conversion gives the instance a new SOP Instance UID, rq.sop_instance_uid becomes it and the
// old one is kept in rq.original_sop_instance_uid (R-093).
Status convert_received(StoreRequest& rq, std::string_view ts, const codec::TranscodeParams& p,
                        const SpoolConfig* spool);

}  // namespace siaara::net
