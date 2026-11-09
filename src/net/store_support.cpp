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

Status encode_for_transfer_syntax(const DicomFile& file, std::string_view ts, const codec::TranscodeParams& p,
                                  std::vector<std::uint8_t>& body) {
    body.clear();
    if (ts == file.transfer_syntax || (is_native(ts) && is_native(file.transfer_syntax)))
        return write_dataset(file.dataset, ts, body);
    Result<Dataset> t = codec::transcode_dataset(file.dataset, file.transfer_syntax, ts, p);
    if (!t) return t.take_error();
    return write_dataset(*t, ts, body);
}

Status convert_received(StoreRequest& rq, std::string_view ts, const codec::TranscodeParams& p,
                        const SpoolConfig* spool) {
    if (rq.transfer_syntax == ts) return Status::ok();
    DicomFile f;
    f.dataset = rq.dataset;
    f.transfer_syntax = rq.transfer_syntax;
    auto body = std::make_shared<std::vector<std::uint8_t>>();
    if (Status s = encode_for_transfer_syntax(f, ts, p, *body); !s) return s;
    Bytes raw;
    std::shared_ptr<SpoolFile> file;
    if (rq.spool && spool) {
        std::vector<std::uint8_t> header;
        if (Status s = part10_header(rq.sop_class_uid, rq.sop_instance_uid, ts, rq.calling_ae, header); !s) return s;
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
    return Status::ok();
}

}  // namespace siaara::net
