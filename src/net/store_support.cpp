// SIAARA DICOM Toolkit - C-STORE context choice and on-the-fly transcoding (net/store_support.hpp).
#include "net/store_support.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"

namespace siaara::net {

namespace {

bool is_native(std::string_view ts_uid) {
    const TransferSyntaxInfo* t = find_transfer_syntax(ts_uid);
    return t && !t->encapsulated;
}

}  // namespace

std::vector<const NegotiatedContext*> store_contexts(const Association& a, std::string_view sop_class,
                                                     std::string_view file_ts, bool requestor_scp, bool transcode,
                                                     const codec::TranscodeParams& p) {
    struct Cand {
        int rank;
        std::size_t order;
        const NegotiatedContext* pc;
    };
    std::vector<Cand> c;
    const bool native = is_native(file_ts);
    const std::vector<std::string> native_order = native_transfer_syntaxes();
    std::size_t order = 0;
    for (const NegotiatedContext& k : a.contexts()) {
        ++order;
        if (!k.accepted() || k.abstract_syntax != sop_class) continue;
        if (requestor_scp ? !k.requestor_scp : !k.requestor_scu) continue;
        int rank = -1;
        std::size_t sub = order;
        if (k.transfer_syntax == file_ts) {
            rank = 0;
        } else if (native && is_native(k.transfer_syntax)) {
            rank = 1;
            const auto it = std::find(native_order.begin(), native_order.end(), k.transfer_syntax);
            sub = static_cast<std::size_t>(it - native_order.begin());
        } else if (transcode && codec::can_decode(file_ts) && codec::can_encode(k.transfer_syntax)) {
            if (!codec::encoding_is_lossy(k.transfer_syntax, p)) rank = 2;
            else if (p.allow_lossy) rank = 3;
        }
        if (rank >= 0) c.push_back({rank, sub, &k});
    }
    std::stable_sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) {
        return x.rank != y.rank ? x.rank < y.rank : x.order < y.order;
    });
    std::vector<const NegotiatedContext*> out;
    for (const Cand& k : c) out.push_back(k.pc);
    return out;
}

std::vector<std::string> transcode_targets(const codec::TranscodeParams& p, bool lossy) {
    static constexpr std::string_view kCompressed[] = {
        "1.2.840.10008.1.2.5",    "1.2.840.10008.1.2.4.57", "1.2.840.10008.1.2.4.70",  "1.2.840.10008.1.2.4.80",
        "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.201", "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51",
        "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.4.203"};
    std::vector<std::string> out;
    if (lossy && !p.allow_lossy) return out;
    for (std::string_view t : kCompressed)
        if (codec::can_encode(t) && codec::encoding_is_lossy(t, p) == lossy) out.emplace_back(t);
    return out;
}

Status check_sendable(const DicomFile& file, bool allow_incomplete) {
    if (file.complete() || allow_incomplete) return Status::ok();
    const char* why = (file.read_loss & DicomFile::kReadInputTruncated) ? "the input was truncated"
                      : (file.read_loss & DicomFile::kReadBytesSkipped) ? "input bytes were skipped"
                      : (file.read_loss & DicomFile::kReadRecovered)
                          ? "damaged input was recovered by a heuristic, see the read warnings"
                          : "it was read header-only";
    return Status(Errc::Rejected,
                  std::string(kIncompleteInput) + ": not sending a data set that was not read completely (" + why +
                      "; first at offset " + std::to_string(file.read_loss_offset) +
                      "): the peer would store less than the file holds; allow incomplete input to send it anyway",
                  file.read_loss_offset);
}

Status encode_for_transfer_syntax(const DicomFile& file, std::string_view ts, const codec::TranscodeParams& p,
                                  std::vector<std::uint8_t>& body, codec::TranscodeReport* report) {
    body.clear();
    if (report) *report = codec::TranscodeReport{};
    if (ts == file.transfer_syntax || (is_native(ts) && is_native(file.transfer_syntax)))
        return write_dataset(file.dataset, ts, body);
    Result<Dataset> t = codec::transcode_dataset(file.dataset, file.transfer_syntax, ts, p, report);
    if (!t) return t.take_error();
    return write_dataset(*t, ts, body);
}

Status convert_received(StoreRequest& rq, std::string_view ts, const codec::TranscodeParams& p,
                        const SpoolConfig* spool, std::string* lossy_history_note) {
    if (lossy_history_note) lossy_history_note->clear();
    if (rq.transfer_syntax == ts) return Status::ok();
    DicomFile f;
    f.dataset = rq.dataset;
    f.transfer_syntax = rq.transfer_syntax;
    auto body = std::make_shared<std::vector<std::uint8_t>>();
    codec::TranscodeReport report;
    if (Status s = encode_for_transfer_syntax(f, ts, p, *body, &report); !s) return s;
    if (lossy_history_note) *lossy_history_note = report.lossy_history_note;
    // R-093: a lossy conversion gave the instance a new SOP Instance UID; the file meta, the file
    // name and the index must use it.
    const std::string sop_instance = report.new_sop_instance_uid.empty() ? rq.sop_instance_uid
                                                                         : report.new_sop_instance_uid;
    Bytes raw;
    std::shared_ptr<SpoolFile> file;
    if (rq.spool && spool) {
        std::vector<std::uint8_t> header;
        if (Status s = part10_header(rq.sop_class_uid, sop_instance, ts, rq.calling_ae, header); !s) return s;
        Result<std::unique_ptr<SpoolWriter>> w = SpoolWriter::open(*spool, header);
        if (!w) return w.take_error();
        if (Status s = (*w)->write(body->data(), body->size()); !s) return s;
        body.reset();
        Result<SpoolWriter::Finished> fin = (*w)->finish();
        if (!fin) return fin.take_error();
        raw = fin->data;
        file = fin->file;
    } else {
        const std::uint8_t* d = body->data();
        const std::size_t n = body->size();
        raw = Bytes(std::shared_ptr<const void>(body, body.get()), d, n);
    }
    Result<Dataset> ds = read_dataset(raw, ts);
    if (!ds) return ds.take_error();
    rq.dataset = std::move(ds.value());
    rq.raw = raw;
    rq.spool = file;
    rq.transfer_syntax = std::string(ts);
    if (sop_instance != rq.sop_instance_uid) {
        if (rq.original_sop_instance_uid.empty()) rq.original_sop_instance_uid = rq.sop_instance_uid;
        rq.sop_instance_uid = sop_instance;
    }
    return Status::ok();
}

}  // namespace siaara::net
