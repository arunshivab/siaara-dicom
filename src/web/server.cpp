// SIAARA DICOM Toolkit - DICOMweb origin server (PS3.18 chapters 8 to 10), written from the standard.
#include "web/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "image/frames.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"

namespace siaara::web {

namespace {

using http::iequals;

void fail(http::Response& rs, int status, std::string_view message) {
    rs.status = status;
    rs.headers.clear();
    rs.set("Content-Type", "text/plain; charset=utf-8");
    rs.body = std::string(message) + "\n";
}

bool vr_is_bulk(VR vr) noexcept {
    return vr == VR::OB || vr == VR::OD || vr == VR::OF || vr == VR::OL || vr == VR::OV || vr == VR::OW || vr == VR::UN;
}

std::string trimmed(const Dataset& ds, Tag t) {
    auto v = ds.get_string(t);
    return v ? *v : std::string();
}

// Absolute origin ("http[s]://host[:port]") from the Host header, when it is a plain host[:port].
std::string origin_of(const http::Request& rq, bool tls) {
    const std::string* host = http::find_header(rq.headers, "Host");
    if (!host || host->empty() || host->size() > 255) return std::string();
    for (char c : *host)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
              c == ':' || c == '[' || c == ']'))
            return std::string();
    return (tls ? "https://" : "http://") + *host;
}

// ---------------------------------------------------------------- transfer syntax handling
// Converts a stored instance to the wanted transfer syntax ("" = Explicit VR Little Endian, the
// PS3.18 default; "*" = as stored). Native targets only: compressed data is decompressed with
// the codec layer, compressing is not offered.
Result<Bytes> convert_instance(const Bytes& raw, const std::string& want, std::string& out_ts,
                               std::uint64_t decode_memory = 0) {
    auto f = read_part10(raw);
    if (!f) return f.take_error();
    const std::string stored = f->transfer_syntax;
    if (want == "*" || want == stored || (want.empty() && stored == ts::ExplicitVRLittleEndian)) {
        out_ts = stored;
        return raw;
    }
    const std::string target = want.empty() ? std::string(ts::ExplicitVRLittleEndian) : want;
    const auto* to = find_transfer_syntax(target);
    const auto* from = find_transfer_syntax(stored);
    if (!to || !from || to->encapsulated || to->codec == Codec::Unknown)
        return Status(Errc::Unsupported, "transfer syntax not available");
    DicomFile copy = std::move(f).value();
    if (from->encapsulated) {
        Element* px = copy.dataset.find(tags::PixelData);
        if (px && px->is_encapsulated()) {
            image::FrameOptions fo;
            if (decode_memory != 0) fo.memory_budget = std::min(fo.memory_budget, decode_memory);
            auto fr = image::FrameReader::open(copy.dataset, stored, fo);
            if (!fr) return fr.take_error();
            auto frames = fr->decode_range(0, fr->frame_count());
            if (!frames) return frames.take_error();
            const auto& pi = fr->info();
            const bool rct = fr->codec_applies_color_transform();
            Element e;
            e.tag = tags::PixelData;
            e.vr = pi.bits_allocated > 8 ? VR::OW : VR::OB;
            e.value = detail::to_bytes(std::move(frames).value());
            copy.dataset.put(std::move(e));
            const std::string pm = trimmed(copy.dataset, tags::PhotometricInterpretation);
            if (rct && (pm == "YBR_RCT" || pm == "YBR_ICT")) (void)copy.dataset.set_string(tags::PhotometricInterpretation, VR::CS, "RGB");
            if (pm == "YBR_FULL_422") (void)copy.dataset.set_string(tags::PhotometricInterpretation, VR::CS, "YBR_FULL");
            if (from->lossy) (void)copy.dataset.set_string(0x00282110, VR::CS, "01");  // Lossy Image Compression
        }
    }
    copy.transfer_syntax = target;
    copy.meta.remove(tags::TransferSyntaxUID);
    std::vector<std::uint8_t> out;
    if (Status st = write_part10(copy, out); !st) return st;
    out_ts = target;
    return detail::to_bytes(std::move(out));
}

// Candidate transfer syntaxes for application/dicom from an Accept header, in preference order.
// Returns false when nothing acceptable for DICOM instances was offered.
bool instance_candidates(const http::Request& rq, std::vector<std::string>& out) {
    const std::string* accept = http::find_header(rq.headers, "Accept");
    if (!accept || accept->empty()) {
        out.push_back("");
        return true;
    }
    for (const auto& m : http::parse_accept(*accept)) {
        if (m.weight == 0) continue;
        const std::string* ts_param = m.param("transfer-syntax");
        if (m.is("*", "*") || m.is("multipart", "*")) { out.push_back(""); continue; }
        if (m.is("multipart", "related") || m.is("application", "dicom")) {
            if (m.is("multipart", "related")) {
                const std::string* type = m.param("type");
                if (type && !iequals(*type, media::Dicom)) continue;
            }
            out.push_back(ts_param ? *ts_param : std::string());
        }
    }
    return !out.empty();
}

enum class MetaFormat { Json, Xml, None };

MetaFormat metadata_format(const http::Request& rq) {
    const std::string* accept = http::find_header(rq.headers, "Accept");
    if (!accept || accept->empty()) return MetaFormat::Json;
    for (const auto& m : http::parse_accept(*accept)) {
        if (m.weight == 0) continue;
        if (m.is("application", "dicom+json") || m.is("application", "json") || m.is("*", "*") || m.is("application", "*"))
            return MetaFormat::Json;
        if (m.is("multipart", "related")) {
            const std::string* type = m.param("type");
            if (type && iequals(*type, media::DicomXml)) return MetaFormat::Xml;
        }
        if (m.is("application", "dicom+xml")) return MetaFormat::Xml;
    }
    return MetaFormat::None;
}

void add_cors(const DicomwebServerConfig& cfg, http::Response& rs) {
    if (cfg.cors_allow_origin.empty()) return;
    rs.set("Access-Control-Allow-Origin", cfg.cors_allow_origin);
    rs.set("Access-Control-Expose-Headers", "Content-Type, Content-Length, Warning");
}

// ---------------------------------------------------------------- request handler
std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) noexcept {
    return a != 0 && b > UINT64_MAX / a ? UINT64_MAX : a * b;
}
std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

// R-161: the size of the instance's Part 10 stream once its pixel data is decoded, from its
// header alone (the pixel data is not read). 0 when it has no encapsulated pixel data.
std::uint64_t decoded_size_from_header(const Bytes& raw) {
    ReadOptions ro;
    ro.stop_before_pixel_data = true;
    auto f = read_part10(raw, ro);
    if (!f) return 0;
    const auto* ts_info = find_transfer_syntax(f->transfer_syntax);
    if (!ts_info || !ts_info->encapsulated) return 0;
    const Dataset& ds = f->dataset;
    const auto num = [&](Tag t, std::uint64_t fallback) -> std::uint64_t {
        const auto v = ds.get_int(t);
        return v && *v >= 0 ? static_cast<std::uint64_t>(*v) : fallback;
    };
    const std::uint64_t rows = num(tags::Rows, 0), cols = num(tags::Columns, 0);
    const std::uint64_t samples = num(tags::SamplesPerPixel, 1);
    const std::uint64_t bits = num(tags::BitsAllocated, 8);
    const std::uint64_t frames = std::max<std::uint64_t>(num(tags::NumberOfFrames, 1), 1);
    const std::uint64_t pixels = sat_mul(sat_mul(sat_mul(rows, cols), frames), sat_mul(std::max<std::uint64_t>(samples, 1), (bits + 7) / 8));
    // The header's own bytes stand for the other elements (the stream up to Pixel Data).
    return sat_add(pixels, std::min<std::uint64_t>(raw.size(), std::uint64_t{64} << 20));
}

class Handler {
public:
    Handler(const DicomwebServerConfig& cfg, const Store& store, const http::Request& rq, http::Response& rs,
            std::shared_ptr<DecodeBudget> decode_budget)
        : cfg_(cfg), store_(store), rq_(rq), rs_(rs), decode_budget_(std::move(decode_budget)) {}

    void run() {
        auto t = http::parse_target(rq_.target);
        if (!t) return fail(rs_, 400, "invalid request target");
        target_ = std::move(t).value();
        const bool get = iequals(rq_.method, "GET") || iequals(rq_.method, "HEAD");
        const bool post = iequals(rq_.method, "POST");
        if (iequals(rq_.method, "OPTIONS") && !cfg_.cors_allow_origin.empty()) {
            rs_.status = 204;
            rs_.set("Access-Control-Allow-Origin", cfg_.cors_allow_origin);
            rs_.set("Access-Control-Allow-Methods", "GET, HEAD, POST, OPTIONS");
            rs_.set("Access-Control-Allow-Headers", "Accept, Content-Type, Authorization");
            rs_.set("Access-Control-Max-Age", "600");
            return;
        }
        if (cfg_.authorize && !cfg_.authorize(rq_)) {
            fail(rs_, 401, "unauthorized");
            rs_.set("WWW-Authenticate", "Bearer");
            return;
        }
        // WADO-URI
        if (!cfg_.wado_uri_path.empty() && target_.path == cfg_.base_path + cfg_.wado_uri_path) {
            if (!get) return method_not_allowed("GET, HEAD");
            return wado_uri();
        }
        // Strip the base path.
        std::vector<std::string> base;
        if (auto bt = http::parse_target(cfg_.base_path.empty() ? std::string("/") : cfg_.base_path)) base = bt->segments;
        if (target_.segments.size() < base.size() ||
            !std::equal(base.begin(), base.end(), target_.segments.begin()))
            return fail(rs_, 404, "not found");
        seg_.assign(target_.segments.begin() + static_cast<std::ptrdiff_t>(base.size()), target_.segments.end());
        base_url_ = cfg_.public_base_url.empty() ? origin_of(rq_, cfg_.http.tls.enabled) + cfg_.base_path : cfg_.public_base_url;
        route(get, post);
        add_cors(cfg_, rs_);
    }

private:
    const DicomwebServerConfig& cfg_;
    const Store& store_;
    const http::Request& rq_;
    http::Response& rs_;
    std::shared_ptr<DecodeBudget> decode_budget_;
    http::Target target_;
    std::vector<std::string> seg_;
    std::string base_url_;

    void method_not_allowed(const char* allow) {
        fail(rs_, 405, "method not allowed");
        rs_.set("Allow", allow);
    }

    bool uid_ok(std::size_t i) const { return i < seg_.size() && is_uid(seg_[i]); }

    void route(bool get, bool post) {
        const auto n = seg_.size();
        auto is = [&](std::size_t i, const char* s) { return i < n && seg_[i] == s; };
        if (n == 0) return fail(rs_, 404, "not found");
        // QIDO-RS
        if (n == 1 && (is(0, "studies") || is(0, "series") || is(0, "instances"))) {
            if (is(0, "studies") && post) return stow("");
            if (!get) return method_not_allowed(is(0, "studies") ? "GET, HEAD, POST" : "GET, HEAD");
            return qido(is(0, "studies") ? QueryLevel::Study : is(0, "series") ? QueryLevel::Series : QueryLevel::Instance, "", "");
        }
        if (!is(0, "studies")) return fail(rs_, 404, "not found");
        if (!uid_ok(1)) return fail(rs_, n >= 2 ? 400 : 404, "invalid study UID");
        const std::string& study = seg_[1];
        if (n == 2) {
            if (post) return stow(study);
            if (!get) return method_not_allowed("GET, HEAD, POST");
            return retrieve({study, "", ""});
        }
        if (!get) return method_not_allowed("GET, HEAD");
        if (n == 3 && is(2, "series")) return qido(QueryLevel::Series, study, "");
        if (n == 3 && is(2, "instances")) return qido(QueryLevel::Instance, study, "");
        if (n == 3 && is(2, "metadata")) return metadata({study, "", ""});
        if (n == 3 && is(2, "rendered")) return fail(rs_, 406, "rendered media types are not supported");
        if (!is(2, "series")) return fail(rs_, 404, "not found");
        if (!uid_ok(3)) return fail(rs_, 400, "invalid series UID");
        const std::string& series = seg_[3];
        if (n == 4) return retrieve({study, series, ""});
        if (n == 5 && is(4, "instances")) return qido(QueryLevel::Instance, study, series);
        if (n == 5 && is(4, "metadata")) return metadata({study, series, ""});
        if (n == 5 && is(4, "rendered")) return fail(rs_, 406, "rendered media types are not supported");
        if (!is(4, "instances")) return fail(rs_, 404, "not found");
        if (!uid_ok(5)) return fail(rs_, 400, "invalid instance UID");
        const InstanceKey key{study, series, seg_[5]};
        if (n == 6) return retrieve(key);
        if (n == 7 && is(6, "metadata")) return metadata(key);
        if (n == 7 && is(6, "rendered")) return fail(rs_, 406, "rendered media types are not supported");
        if (n == 8 && is(6, "frames")) return frames(key, seg_[7]);
        if (n == 9 && is(6, "frames") && is(8, "rendered")) return fail(rs_, 406, "rendered media types are not supported");
        if (n >= 8 && is(6, "bulkdata")) return bulkdata(key, 7);
        return fail(rs_, 404, "not found");
    }

    // ---- QIDO-RS (PS3.18 10.6)
    void qido(QueryLevel level, const std::string& study, const std::string& series) {
        if (!store_.search) return fail(rs_, 501, "search is not supported");
        SearchQuery q;
        q.level = level;
        q.study_uid = study;
        q.series_uid = series;
        for (const auto& [k, v] : target_.query) {
            if (k == "limit" || k == "offset") {
                auto n = detail::parse_uint(v, 0xFFFFFFFFu);
                if (!n || v.empty() || v[0] == '-' || v.find_first_not_of("0123456789") != std::string::npos)
                    return fail(rs_, 400, "invalid limit or offset");
                if (k == "limit") q.limit = static_cast<std::uint32_t>(*n);
                else q.offset = static_cast<std::uint32_t>(*n);
                continue;
            }
            if (k == "fuzzymatching") {
                if (v == "true") q.fuzzy_matching = true;
                else if (v == "false") q.fuzzy_matching = false;
                else return fail(rs_, 400, "invalid fuzzymatching value");
                continue;
            }
            if (k == "includefield") {
                std::size_t s = 0;
                while (s <= v.size()) {
                    auto e = v.find(',', s);
                    if (e == std::string::npos) e = v.size();
                    const std::string f = v.substr(s, e - s);
                    s = e + 1;
                    if (f.empty()) continue;
                    if (f == "all") { q.include_all = true; continue; }
                    auto tag = attribute_id(f);
                    if (!tag) return fail(rs_, 400, "unknown attribute in includefield");
                    if (q.include_fields.size() >= 1024) return fail(rs_, 400, "too many include fields");
                    q.include_fields.push_back(*tag);
                }
                continue;
            }
            const auto id = attribute_id(k);
            if (!id) return fail(rs_, 400, "unknown query parameter or attribute");
            const Tag tag = *id;
            for (const auto& m : q.match)
                if (m.first == tag) return fail(rs_, 400, "attribute matched more than once");
            q.match.emplace_back(tag, v);
        }
        // Accept: JSON (default) or multipart XML.
        const MetaFormat fmt = metadata_format(rq_);
        if (fmt == MetaFormat::None) return fail(rs_, 406, "acceptable media types: application/dicom+json, multipart/related; type=\"application/dicom+xml\"");
        // Let the store stop early (WEB-05): at most max_qido_results + 1 after the offset.
        {
            const std::uint64_t cap = std::min<std::uint64_t>(cfg_.max_qido_results, 0xFFFFFFFEu) + 1u;
            q.limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(q.limit.value_or(0xFFFFFFFFu), cap));
        }
        std::vector<Dataset> results;
        Status st = store_.search(q, results);
        if (!st) return fail(rs_, st.code == Errc::Unsupported ? 400 : 500, st.code == Errc::Unsupported ? "query not supported" : "search failed");
        // Offset / limit / cap.
        const std::size_t off = std::min<std::size_t>(q.offset, results.size());
        std::size_t count = results.size() - off;
        if (q.limit && *q.limit < count) count = *q.limit;
        bool cut = false;
        if (count > cfg_.max_qido_results) { count = cfg_.max_qido_results; cut = true; }
        // T-P3-1: Retrieve URL (0008,1190) of each result at its level (PS3.18 10.6.3.3), built
        // from the public base URL, or from the request's Host header when none is configured.
        if (!base_url_.empty() && base_url_.find("://") != std::string::npos)
            for (std::size_t i = 0; i < count; ++i) {
                Dataset& d = results[off + i];
                const std::string st_uid = !study.empty() ? study : trimmed(d, tags::StudyInstanceUID);
                const std::string se_uid = !series.empty() ? series : trimmed(d, tags::SeriesInstanceUID);
                const std::string in_uid = trimmed(d, tags::SOPInstanceUID);
                if (!is_uid(st_uid)) continue;
                std::string url = base_url_ + "/studies/" + st_uid;
                if (level != QueryLevel::Study) {
                    if (!is_uid(se_uid)) continue;
                    url += "/series/" + se_uid;
                }
                if (level == QueryLevel::Instance) {
                    if (!is_uid(in_uid)) continue;
                    url += "/instances/" + in_uid;
                }
                (void)d.set_string(webtags::RetrieveURL, VR::UR, url);
            }
        std::vector<const Dataset*> page;
        page.reserve(count);
        for (std::size_t i = 0; i < count; ++i) page.push_back(&results[off + i]);
        if (cut) rs_.set("Warning", "299 SIAARA \"The number of results exceeded the maximum supported by the server\"");
        if (fmt == MetaFormat::Json) {
            std::string body;
            if (Status es = datasets_to_json(page, body); !es) return fail(rs_, 500, "cannot encode results");
            rs_.status = 200;
            rs_.set("Content-Type", std::string(media::DicomJson));
            rs_.body = std::move(body);
            return;
        }
        multipart::Writer w;
        for (const Dataset* d : page) {
            std::string x;
            if (Status es = dataset_to_xml(*d, x); !es) return fail(rs_, 500, "cannot encode results");
            w.add(media::DicomXml, x);
        }
        rs_.body = w.finish();
        rs_.status = 200;
        rs_.set("Content-Type", w.content_type(media::DicomXml));
    }

    static std::optional<Tag> attribute_id(const std::string& s) {
        if (auto t = detail::parse_tag_hex(s)) return t;
        if (s.size() > 128) return std::nullopt;
        if (const auto* e = dict::lookup_keyword(s)) return e->tag;
        return std::nullopt;
    }

    // ---- WADO-RS (PS3.18 10.4)
    // The instances under the key from the store's index: nothing is read (R-110).
    bool locate(const InstanceKey& key, std::vector<StoredInstance>& out) {
        Status st = store_.locate(key, out);
        if (st.code == Errc::NotFound || (st && out.empty())) { fail(rs_, 404, "not found"); return false; }
        // R-182: more instances under the key than a reply may hold is the request's size, not a server fault.
        if (st.code == Errc::LimitExceeded) { fail(rs_, 413, "response too large"); return false; }
        if (!st) { fail(rs_, 500, "retrieve failed"); return false; }
        return true;
    }

    // Is the instance sent as it is stored for the Accept candidate `want`?
    static bool sent_as_stored(const std::string& stored, const std::string& want) {
        return want == "*" || (!stored.empty() && want == stored) || (want.empty() && stored == ts::ExplicitVRLittleEndian);
    }

    static Result<Bytes> open_instance(const Store& store, const StoredInstance& inst) {
        if (store.open) return store.open(inst);
        return io::map_file(inst.path, false);  // mapped: pixel data is paged in only when used
    }

    // The Part 10 streams under the key, for the routes that work on one instance (frames, bulk
    // data, WADO-URI). With Store::locate they are memory-mapped, not read.
    bool load(const InstanceKey& key, std::vector<Bytes>& out) {
        if (store_.locate) {
            std::vector<StoredInstance> list;
            if (!locate(key, list)) return false;
            if (list.size() > 64) { fail(rs_, 413, "response too large"); return false; }
            for (const StoredInstance& inst : list) {
                auto b = open_instance(store_, inst);
                if (!b) { fail(rs_, 500, "retrieve failed"); return false; }
                out.push_back(std::move(b.value()));
            }
            return true;
        }
        if (!store_.retrieve) { fail(rs_, 501, "retrieve is not supported"); return false; }
        Status st = store_.retrieve(key, out);
        if (st.code == Errc::NotFound || (st && out.empty())) { fail(rs_, 404, "not found"); return false; }
        if (!st) { fail(rs_, 500, "retrieve failed"); return false; }
        return true;
    }

    // Can an instance stored in `stored` be sent for the Accept candidate `want`? (As
    // convert_instance decides, from the indexed transfer syntax alone.)
    static bool servable(const std::string& stored, const std::string& want) {
        if (stored.empty()) return true;  // not indexed: found out when the instance is opened
        if (want == "*" || want == stored || (want.empty() && stored == ts::ExplicitVRLittleEndian)) return true;
        const std::string target = want.empty() ? std::string(ts::ExplicitVRLittleEndian) : want;
        const auto* to = find_transfer_syntax(target);
        const auto* from = find_transfer_syntax(stored);
        return to && from && !to->encapsulated && to->codec != Codec::Unknown;
    }

    // R-110: the sizes come from the index, 413 is answered before any file is opened, and the
    // multipart reply is written part by part - one instance is open at a time.
    void retrieve_streamed(const InstanceKey& key, const std::vector<std::string>& candidates) {
        std::vector<StoredInstance> list;
        if (!locate(key, list)) return;
        std::uint64_t stored_total = 0;
        for (const StoredInstance& inst : list) {
            if (inst.size > cfg_.max_response_bytes - std::min(stored_total, cfg_.max_response_bytes))
                return fail(rs_, 413, "response too large");
            stored_total += inst.size;
        }
        const std::string* chosen = nullptr;
        for (const auto& want : candidates) {
            bool all = true;
            for (const StoredInstance& inst : list) all = all && servable(inst.transfer_syntax, want);
            if (all) { chosen = &want; break; }
        }
        if (!chosen) return fail(rs_, 406, "requested transfer syntax not available");
        // R-161: instances that are decoded for the reply count with their decoded size, known
        // before anything is sent: from the index, else from the instance's header. 413 up front
        // when the reply would pass the cap or one instance cannot be decoded within the
        // server's decode memory - never a 200 that is cut in the middle.
        std::vector<std::uint64_t> charge(list.size(), 0);  // decode memory per instance; 0: sent as stored
        std::uint64_t reply_total = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const StoredInstance& inst = list[i];
            std::uint64_t part = inst.size;
            const auto* stored_ts = find_transfer_syntax(inst.transfer_syntax);
            if (!sent_as_stored(inst.transfer_syntax, *chosen) && (inst.transfer_syntax.empty() || (stored_ts && stored_ts->encapsulated))) {
                std::uint64_t decoded = inst.decoded_size;
                if (decoded == 0) {
                    Result<Bytes> raw = open_instance(store_, inst);
                    if (!raw) return fail(rs_, 500, "retrieve failed");
                    decoded = decoded_size_from_header(*raw);
                }
                if (decoded != 0) {
                    part = decoded;
                    charge[i] = sat_mul(decoded, 2);  // the decoded frames and the stream built from them
                    if (charge[i] > cfg_.max_decode_memory) return fail(rs_, 413, "response too large");
                }
            }
            if (part > cfg_.max_response_bytes - std::min(reply_total, cfg_.max_response_bytes))
                return fail(rs_, 413, "response too large");
            reply_total += part;
        }
        const std::string boundary = multipart::make_boundary();
        rs_.status = 200;
        rs_.set("Content-Type", "multipart/related; type=" + http::quote_if_needed(media::Dicom) +
                                    "; boundary=" + http::quote_if_needed(boundary));
        const Store* store = &store_;  // the server's own, alive while it serves
        const std::uint64_t max_bytes = cfg_.max_response_bytes;
        const std::uint64_t decode_memory = cfg_.max_decode_memory;
        const int decode_wait_ms = cfg_.decode_wait_ms;
        rs_.stream = [store, list = std::move(list), charge = std::move(charge), want = *chosen, boundary, max_bytes, decode_memory,
                      decode_wait_ms, budget = decode_budget_](const http::BodyWriter& write) -> Status {
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < list.size(); ++i) {
                const StoredInstance& inst = list[i];
                // The decode memory of this instance is held until its part has been sent.
                struct Held {
                    DecodeBudget* b;
                    std::uint64_t n;
                    ~Held() {
                        if (b && n) b->give(n);
                    }
                } held{nullptr, 0};
                if (charge[i] != 0 && budget) {
                    if (!budget->take(charge[i], decode_wait_ms)) return Status(Errc::LimitExceeded, "decode memory is in use");
                    held.b = budget.get();
                    held.n = charge[i];
                }
                Result<Bytes> raw = open_instance(*store, inst);
                if (!raw) return raw.take_error();
                std::string ts_out;
                Result<Bytes> conv = convert_instance(*raw, want, ts_out, decode_memory);
                if (!conv) return conv.take_error();
                total += conv->size();
                if (total > max_bytes) return Status(Errc::LimitExceeded, "response too large");
                if (Status s = write("--" + boundary + "\r\nContent-Type: " + std::string(media::Dicom) +
                                     "; transfer-syntax=" + ts_out + "\r\n\r\n"); !s)
                    return s;
                const std::string_view body = conv->as_chars();
                constexpr std::size_t kPiece = 1u << 20;
                for (std::size_t off = 0; off < body.size(); off += kPiece)
                    if (Status s = write(body.substr(off, kPiece)); !s) return s;
                if (Status s = write("\r\n"); !s) return s;
            }
            return write("--" + boundary + "--\r\n");
        };
    }

    void retrieve(const InstanceKey& key) {
        std::vector<std::string> candidates;
        if (!instance_candidates(rq_, candidates))
            return fail(rs_, 406, "acceptable media type: multipart/related; type=\"application/dicom\"");
        // R-161, option: no transfer syntax named in Accept means "as stored" instead of PS3.18's
        // Explicit VR Little Endian (nothing is decoded then).
        if (cfg_.default_transfer_syntax == DefaultTransferSyntax::AsStored)
            for (std::string& c : candidates)
                if (c.empty()) c = "*";
        if (store_.locate) return retrieve_streamed(key, candidates);
        std::vector<Bytes> raw;
        if (!load(key, raw)) return;
        // R-052: the reply is built in memory, so it is capped like the frames response.
        std::uint64_t stored_total = 0;
        for (const auto& r : raw) stored_total += r.size();
        if (stored_total > cfg_.max_response_bytes) return fail(rs_, 413, "response too large");
        for (const auto& want : candidates) {
            multipart::Writer w;
            bool all = true;
            std::uint64_t total = 0;
            for (const auto& r : raw) {
                std::string ts_out;
                auto conv = convert_instance(r, want, ts_out);
                if (!conv) { all = false; break; }
                total += conv->size();
                if (total > cfg_.max_response_bytes) return fail(rs_, 413, "response too large");
                w.add(std::string(media::Dicom) + "; transfer-syntax=" + ts_out, conv->as_chars());
            }
            if (!all) continue;
            rs_.body = w.finish();
            rs_.status = 200;
            rs_.set("Content-Type", w.content_type(media::Dicom));
            return;
        }
        fail(rs_, 406, "requested transfer syntax not available");
    }

    std::string instance_url(const Dataset& ds) const {
        const std::string st = trimmed(ds, tags::StudyInstanceUID), se = trimmed(ds, tags::SeriesInstanceUID),
                          in = trimmed(ds, tags::SOPInstanceUID);
        if (!is_uid(st) || !is_uid(se) || !is_uid(in)) return std::string();
        return base_url_ + "/studies/" + st + "/series/" + se + "/instances/" + in;
    }

    void metadata(const InstanceKey& key) {
        const MetaFormat fmt = metadata_format(rq_);
        if (fmt == MetaFormat::None) return fail(rs_, 406, "acceptable media types: application/dicom+json, multipart/related; type=\"application/dicom+xml\"");
        // R-110: metadata never needs the pixel data in memory. With Store::locate the instances
        // are handled one at a time: each file is memory-mapped and parsed in place (values are
        // views into the mapping, so pixel data is not read), encoded, and released.
        std::vector<StoredInstance> list;
        std::vector<Bytes> raw;
        if (store_.locate) {
            if (!locate(key, list)) return;
        } else {
            if (!load(key, raw)) return;
        }
        const std::size_t count = store_.locate ? list.size() : raw.size();
        std::string json;
        multipart::Writer w;
        std::uint64_t xml_total = 0;
        if (fmt == MetaFormat::Json) json.push_back('[');
        for (std::size_t i = 0; i < count; ++i) {
            Bytes bytes;
            if (store_.locate) {
                auto b = open_instance(store_, list[i]);
                if (!b) return fail(rs_, 500, "retrieve failed");
                bytes = std::move(b.value());
            } else {
                bytes = raw[i];
            }
            auto parsed = read_part10(bytes);
            if (!parsed) return fail(rs_, 500, "stored instance cannot be read");
            const DicomFile& file = parsed.value();
            const std::string url = instance_url(file.dataset);
            ModelWriteOptions opt;
            const std::uint64_t threshold = cfg_.bulk_data_threshold;
            opt.bulk_data_uri = [&url, threshold](const Element& e, const ElementPath& path) -> std::optional<std::string> {
                if (url.empty()) return std::nullopt;
                if (!e.is_encapsulated() && (!vr_is_bulk(e.vr) || e.value.size() <= threshold)) return std::nullopt;
                std::string u = url + "/bulkdata";
                for (std::size_t k = 0; k < path.size(); ++k)
                    u += "/" + ((k % 2 == 0) ? detail::tag_hex(path[k]) : std::to_string(path[k]));
                return u;
            };
            if (fmt == MetaFormat::Json) {
                if (i) json.push_back(',');
                if (Status st = dataset_to_json(file.dataset, json, opt); !st) return fail(rs_, 500, "cannot encode metadata");
                if (json.size() > cfg_.max_response_bytes) return fail(rs_, 413, "response too large");
            } else {
                std::string x;
                if (Status st = dataset_to_xml(file.dataset, x, opt); !st) return fail(rs_, 500, "cannot encode metadata");
                xml_total += x.size();
                if (xml_total > cfg_.max_response_bytes) return fail(rs_, 413, "response too large");
                w.add(media::DicomXml, x);
            }
        }
        rs_.status = 200;
        if (fmt == MetaFormat::Json) {
            json.push_back(']');
            rs_.set("Content-Type", std::string(media::DicomJson));
            rs_.body = std::move(json);
        } else {
            rs_.body = w.finish();
            rs_.set("Content-Type", w.content_type(media::DicomXml));
        }
    }

    // Accept for frames / bulk data: (media type, transfer syntax) candidates.
    struct BulkWant {
        std::string media;  // "application/octet-stream", "image/jls", ... or "*" (any)
        std::string ts;     // "" (default: uncompressed), "*" or a UID
    };
    bool bulk_candidates(std::vector<BulkWant>& out) const {
        const std::string* accept = http::find_header(rq_.headers, "Accept");
        if (!accept || accept->empty()) {
            out.push_back({std::string(media::OctetStream), ""});
            return true;
        }
        for (const auto& m : http::parse_accept(*accept)) {
            if (m.weight == 0) continue;
            const std::string* ts = m.param("transfer-syntax");
            if (m.is("*", "*") || m.is("multipart", "*")) { out.push_back({"*", ts ? *ts : "*"}); continue; }
            if (!m.is("multipart", "related")) {
                // A single-part media type is accepted as the type of the parts.
                out.push_back({m.essence(), ts ? *ts : ""});
                continue;
            }
            const std::string* type = m.param("type");
            std::string media_of_parts(media::OctetStream);
            if (type) {
                const auto slash = type->find('/');
                if (slash != std::string::npos && type->substr(slash + 1) == "*") media_of_parts = "*";  // */*, image/*
                else if (auto pm = http::parse_media_type(*type)) media_of_parts = pm->essence();
                else continue;
            }
            out.push_back({media_of_parts, ts ? *ts : (media_of_parts == "*" ? "*" : "")});
        }
        return !out.empty();
    }

    void frames(const InstanceKey& key, const std::string& list) {
        std::vector<std::uint32_t> numbers;
        std::size_t s = 0;
        while (s <= list.size()) {
            auto e = list.find(',', s);
            if (e == std::string::npos) e = list.size();
            const std::string item = list.substr(s, e - s);
            s = e + 1;
            if (item.empty() || item.find_first_not_of("0123456789") != std::string::npos) return fail(rs_, 400, "invalid frame list");
            auto v = detail::parse_uint(item, 0xFFFFFFFFu);
            if (!v || *v == 0) return fail(rs_, 400, "invalid frame list");
            if (numbers.size() >= 100000) return fail(rs_, 400, "too many frames requested");
            numbers.push_back(static_cast<std::uint32_t>(*v));
        }
        {
            // Each frame at most once (PARSE-31: /frames/1,1,1,... multiplied one frame's size).
            std::vector<std::uint32_t> sorted = numbers;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                return fail(rs_, 400, "duplicate frame number");
        }
        std::vector<BulkWant> wants;
        if (!bulk_candidates(wants)) return fail(rs_, 406, "not acceptable");
        std::vector<Bytes> raw;
        if (!load(key, raw)) return;
        if (raw.size() != 1) return fail(rs_, 500, "instance is not unique");
        auto f = read_part10(raw[0]);
        if (!f) return fail(rs_, 500, "stored instance cannot be read");
        auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax);
        if (!fr) return fail(rs_, 404, "instance has no frames");
        if (numbers.size() > fr->frame_count()) return fail(rs_, 400, "more frames requested than the instance has");
        for (std::uint32_t n : numbers)
            if (n > fr->frame_count()) return fail(rs_, 404, "frame number out of range");
        const std::string stored_media(media_type_for_transfer_syntax(f->transfer_syntax));
        for (const auto& want : wants) {
            // Option A: as stored (encapsulated) frames.
            const bool stored_ok = fr->encapsulated() &&
                                   (want.media == "*" || want.media == stored_media || want.ts == "*") &&
                                   (want.ts == "*" || want.ts == f->transfer_syntax || (want.ts.empty() && want.media != std::string(media::OctetStream)));
            // Option B: uncompressed frames.
            const bool native_ok = (want.media == "*" || want.media == media::OctetStream) &&
                                   (want.ts.empty() || want.ts == "*" || want.ts == ts::ExplicitVRLittleEndian ||
                                    (!fr->encapsulated() && want.ts == f->transfer_syntax));
            if (!stored_ok && !native_ok) continue;
            const bool as_stored = stored_ok && (!native_ok || want.ts == "*" || want.media != media::OctetStream);
            multipart::Writer w;
            std::uint64_t total = 0;
            auto over_budget = [&](std::size_t part) {
                total += part;
                return total > cfg_.max_frames_response_bytes;
            };
            for (std::uint32_t n : numbers) {
                if (as_stored) {
                    std::vector<std::uint8_t> enc;
                    if (Status st = fr->encoded_frame(n - 1, enc); !st) return fail(rs_, 500, "frame cannot be read");
                    if (over_budget(enc.size())) return fail(rs_, 413, "response too large");
                    w.add(stored_media + "; transfer-syntax=" + f->transfer_syntax,
                          std::string_view(reinterpret_cast<const char*>(enc.data()), enc.size()));
                } else {
                    auto d = fr->decode(n - 1);
                    if (!d) return fail(rs_, 406, "frame cannot be decompressed");
                    if (over_budget(d->size())) return fail(rs_, 413, "response too large");
                    w.add(std::string(media::OctetStream) + "; transfer-syntax=" + std::string(ts::ExplicitVRLittleEndian),
                          std::string_view(reinterpret_cast<const char*>(d->data()), d->size()));
                }
            }
            const std::string part_type = as_stored ? stored_media : std::string(media::OctetStream);
            rs_.body = w.finish();
            rs_.status = 200;
            rs_.set("Content-Type", w.content_type(part_type));
            return;
        }
        fail(rs_, 406, "requested media type or transfer syntax not available");
    }

    void bulkdata(const InstanceKey& key, std::size_t first) {
        std::vector<Bytes> raw;
        if (!load(key, raw)) return;
        if (raw.size() != 1) return fail(rs_, 500, "instance is not unique");
        auto f = read_part10(raw[0]);
        if (!f) return fail(rs_, 500, "stored instance cannot be read");
        const Dataset* ds = &f->dataset;
        const Element* el = nullptr;
        for (std::size_t i = first; i < seg_.size(); ++i) {
            const bool is_tag = (i - first) % 2 == 0;
            if (is_tag) {
                auto t = detail::parse_tag_hex(seg_[i]);
                if (!t) return fail(rs_, 400, "invalid bulk data path");
                el = ds->find(*t);
                if (!el) return fail(rs_, 404, "not found");
            } else {
                auto idx = detail::parse_uint(seg_[i], 0xFFFFFFFFu);
                if (!idx || !el || !el->is_sequence() || *idx >= el->items.size()) return fail(rs_, 404, "not found");
                ds = &el->items[static_cast<std::size_t>(*idx)];
                el = nullptr;
            }
        }
        if (!el || el->is_sequence()) return fail(rs_, 404, "not found");
        const std::string* accept = http::find_header(rq_.headers, "Accept");
        if (accept && !accept->empty()) {
            bool ok = false;
            for (const auto& m : http::parse_accept(*accept))
                if (m.weight > 0 && (m.is("*", "*") || m.is("multipart", "*") || m.is("multipart", "related") ||
                                     m.is("application", "octet-stream")))
                    ok = true;
            if (!ok) return fail(rs_, 406, "acceptable media type: multipart/related; type=\"application/octet-stream\"");
        }
        multipart::Writer w;
        std::string part_type(media::OctetStream);
        if (el->is_encapsulated()) {
            // Encapsulated pixel data: one part per frame, in its compressed form.
            auto fr = image::FrameReader::open(*ds, f->transfer_syntax);
            if (!fr) return fail(rs_, 500, "pixel data cannot be read");
            part_type = std::string(media_type_for_transfer_syntax(f->transfer_syntax));
            std::uint64_t total = 0;
            for (std::uint32_t n = 0; n < fr->frame_count(); ++n) {
                std::vector<std::uint8_t> enc;
                if (Status st = fr->encoded_frame(n, enc); !st) return fail(rs_, 500, "frame cannot be read");
                total += enc.size();
                if (total > cfg_.max_response_bytes) return fail(rs_, 413, "response too large");  // R-052
                w.add(part_type + "; transfer-syntax=" + f->transfer_syntax,
                      std::string_view(reinterpret_cast<const char*>(enc.data()), enc.size()));
            }
        } else {
            if (el->value.size() > cfg_.max_response_bytes) return fail(rs_, 413, "response too large");  // R-052
            w.add(media::OctetStream, el->value.as_chars());
        }
        rs_.body = w.finish();
        rs_.status = 200;
        rs_.set("Content-Type", w.content_type(part_type));
    }

    // ---- STOW-RS (PS3.18 10.5)
    void stow(const std::string& study) {
        if (!store_.store) return fail(rs_, 501, "store is not supported");
        const std::string* ct = http::find_header(rq_.headers, "Content-Type");
        if (!ct) return fail(rs_, 415, "Content-Type must be multipart/related; type=\"application/dicom\"");
        auto mt = http::parse_media_type(*ct);
        if (!mt || !mt->is("multipart", "related")) return fail(rs_, 415, "Content-Type must be multipart/related; type=\"application/dicom\"");
        const std::string* type = mt->param("type");
        if (type && !iequals(*type, media::Dicom)) return fail(rs_, 415, "only application/dicom parts are supported");
        const std::string* boundary = mt->param("boundary");
        if (!boundary) return fail(rs_, 400, "multipart boundary missing");
        auto parts = multipart::parse(rq_.body, *boundary, cfg_.multipart);
        if (!parts) return fail(rs_, parts.error().code == Errc::LimitExceeded ? 413 : 400, "malformed multipart body");
        if (parts->empty()) return fail(rs_, 400, "no instances in the request");
        Dataset response;
        std::string common_study;
        bool one_study = true;
        bool any_warning = false;
        Element failed, stored;
        failed.tag = webtags::FailedSOPSequence;
        failed.vr = VR::SQ;
        stored.tag = webtags::ReferencedSOPSequence;
        stored.vr = VR::SQ;
        for (const auto& p : parts.value()) {
            std::uint16_t reason = 0, warn = 0;
            bool quarantined = false;  // kept outside the archive: not retrievable
            std::string cls, inst, st_uid, se_uid;
            const std::string* pct = p.content_type();
            if (pct) {
                auto pm = http::parse_media_type(*pct);
                if (!pm || !pm->is("application", "dicom")) reason = failure::CannotUnderstand;
            }
            if (!reason) {
                const Bytes bytes = Bytes::copy_of(p.body.data(), p.body.size());
                auto f = read_part10(bytes, network_read_options());
                if (!f) {
                    reason = failure::CannotUnderstand;
                } else {
                    cls = trimmed(f->dataset, tags::SOPClassUID);
                    inst = trimmed(f->dataset, tags::SOPInstanceUID);
                    st_uid = trimmed(f->dataset, tags::StudyInstanceUID);
                    se_uid = trimmed(f->dataset, tags::SeriesInstanceUID);
                    if (!is_uid(cls) || !is_uid(inst) || !is_uid(st_uid) || !is_uid(se_uid)) reason = failure::CannotUnderstand;
                    else if (!study.empty() && st_uid != study) reason = failure::ProcessingFailure;
                    // R-116: a part that was not read completely is refused, or stored exactly as
                    // received and flagged - never rewritten (a part without a preamble could only
                    // be stored rewritten, so that one is refused under both policies).
                    else if (!f->complete() && cfg_.incomplete_policy == IncompletePolicy::Quarantine && store_.quarantine) {
                        reason = store_.quarantine(f.value(), bytes, std::string());
                        if (!reason) {
                            quarantined = true;
                            if (cfg_.quarantine_response == QuarantineResponse::Warning) warn = kQuarantineWarningStatus;
                            else if (cfg_.quarantine_response == QuarantineResponse::Failure) reason = kIncompleteFailureStatus;
                        }
                    } else if (!f->complete() &&
                               (cfg_.incomplete_policy != IncompletePolicy::AcceptAsReceived || !f->had_preamble)) {
                        reason = kIncompleteFailureStatus;
                        // RefuseKeepCopy: a copy for diagnosis, outside the archive.
                        if (cfg_.incomplete_policy == IncompletePolicy::RefuseKeepCopy && store_.keep_rejected)
                            store_.keep_rejected(f.value(), bytes, std::string());
                    }
                    else {
                        reason = store_.store(f.value(), bytes);
                        // Accepted although incomplete: the answer for kept instances is the
                        // quarantine_response option (WARNING: Warning Reason 0xB006; SUCCESS: no
                        // warning; FAILURE: listed as failed with 0xC000 although it is stored).
                        if (!reason && !f->complete()) {
                            if (cfg_.quarantine_response == QuarantineResponse::Warning) warn = warning::ElementsDiscarded;
                            else if (cfg_.quarantine_response == QuarantineResponse::Failure) reason = kIncompleteFailureStatus;
                        }
                        // A complete data set without a Part 10 preamble is stored as a new
                        // Part 10 file (file meta added): say so.
                        else if (!reason && !f->had_preamble) warn = warning::CoercionOfDataElements;
                    }
                }
            }
            Dataset item;
            if (is_uid(cls)) (void)item.set_string(webtags::ReferencedSOPClassUID, VR::UI, cls);
            if (is_uid(inst)) (void)item.set_string(webtags::ReferencedSOPInstanceUID, VR::UI, inst);
            if (reason) {
                (void)item.set_uint16(webtags::FailureReason, reason);
                failed.items.push_back(std::move(item));
            } else {
                if (!quarantined) {
                    if (common_study.empty()) common_study = st_uid;
                    else if (common_study != st_uid) one_study = false;
                    (void)item.set_string(webtags::RetrieveURL, VR::UR,
                                          base_url_ + "/studies/" + st_uid + "/series/" + se_uid + "/instances/" + inst);
                }
                if (warn) {
                    (void)item.set_uint16(webtags::WarningReason, warn);
                    any_warning = true;
                }
                stored.items.push_back(std::move(item));
            }
        }
        if (!study.empty()) (void)response.set_string(webtags::RetrieveURL, VR::UR, base_url_ + "/studies/" + study);
        else if (one_study && !common_study.empty())
            (void)response.set_string(webtags::RetrieveURL, VR::UR, base_url_ + "/studies/" + common_study);
        const bool any_failed = !failed.items.empty();
        const bool any_stored = !stored.items.empty();
        if (any_failed) response.put(std::move(failed));
        if (any_stored) response.put(std::move(stored));
        // PS3.18 10.5.3: 202 when some instances failed or were stored with warnings.
        rs_.status = any_failed ? (any_stored ? 202 : 409) : any_warning ? 202 : 200;
        const MetaFormat fmt = metadata_format(rq_);
        if (fmt == MetaFormat::Xml) {
            std::string x;
            (void)dataset_to_xml(response, x);
            rs_.set("Content-Type", std::string(media::DicomXml));
            rs_.body = std::move(x);
        } else {
            std::string j;
            (void)dataset_to_json(response, j);
            rs_.set("Content-Type", std::string(media::DicomJson));
            rs_.body = std::move(j);
        }
    }

    // ---- WADO-URI (PS3.18 chapter 9)
    void wado_uri() {
        auto param = [&](const char* name) -> const std::string* {
            for (const auto& [k, v] : target_.query)
                if (k == name) return &v;
            return nullptr;
        };
        const std::string* request_type = param("requestType");
        if (!request_type || *request_type != "WADO") return fail(rs_, 400, "requestType must be WADO");
        const std::string* st = param("studyUID");
        const std::string* se = param("seriesUID");
        const std::string* ob = param("objectUID");
        if (!st || !se || !ob || !is_uid(*st) || !is_uid(*se) || !is_uid(*ob))
            return fail(rs_, 400, "studyUID, seriesUID and objectUID are required");
        const std::string* content_type = param("contentType");
        bool dicom = false;
        if (content_type) {
            std::size_t s = 0;
            while (s <= content_type->size()) {
                auto e = content_type->find(',', s);
                if (e == std::string::npos) e = content_type->size();
                auto mt = http::parse_media_type(content_type->substr(s, e - s));
                if (mt && mt->is("application", "dicom")) dicom = true;
                s = e + 1;
            }
        }
        if (!dicom) return fail(rs_, 406, "only contentType=application/dicom is supported");
        std::string want;
        if (const std::string* ts = param("transferSyntax")) {
            if (!is_uid(*ts)) return fail(rs_, 400, "invalid transferSyntax");
            want = *ts;
        }
        std::vector<Bytes> raw;
        if (!load({*st, *se, *ob}, raw)) return;
        if (raw.size() != 1) return fail(rs_, 500, "instance is not unique");
        std::string ts_out;
        auto conv = convert_instance(raw[0], want, ts_out);
        if (!conv) return fail(rs_, 406, "requested transfer syntax not available");
        rs_.status = 200;
        rs_.set("Content-Type", std::string(media::Dicom));
        rs_.body.assign(conv->as_chars());
        add_cors(cfg_, rs_);
    }
};

}  // namespace

Status DicomwebServer::start(const DicomwebServerConfig& cfg, Store store) {
    cfg_ = cfg;
    store_ = std::move(store);
    decode_budget_ = std::make_shared<DecodeBudget>(cfg_.max_decode_memory);
    while (!cfg_.base_path.empty() && cfg_.base_path.back() == '/') cfg_.base_path.pop_back();
    while (!cfg_.public_base_url.empty() && cfg_.public_base_url.back() == '/') cfg_.public_base_url.pop_back();
    if (!cfg_.base_path.empty() && cfg_.base_path[0] != '/') return Status(Errc::InvalidArgument, "base_path must start with '/'");
    if (cfg_.cors_allow_origin == "*" && cfg_.authorize)
        return Status(Errc::InvalidArgument, "cors_allow_origin \"*\" cannot be combined with authorize; name the origin");
    const bool remote = !http::is_loopback_address(cfg_.http.bind_address);
    const bool protected_remote = cfg_.http.tls.enabled && (cfg_.authorize || cfg_.http.tls.require_client_certificate);
    if (remote && !protected_remote && !cfg_.http.allow_insecure_remote)
        return Status(Errc::InvalidArgument,
                      "refusing to serve DICOMweb off loopback without TLS and authorize (or client certificates); "
                      "set http.allow_insecure_remote to override");
    if (remote && !protected_remote && cfg_.http.log)
        cfg_.http.log("dicomweb: warning: serving off loopback without TLS plus authentication (allow_insecure_remote)");
    return http_.start(cfg_.http, [this](const http::Request& rq, http::Response& rs) { handle(rq, rs); });
}

bool DecodeBudget::take(std::uint64_t n, int wait_ms) {
    if (n > limit_) return false;
    std::unique_lock<std::mutex> l(mu_);
    const auto fits = [&] { return used_ <= limit_ - n; };
    if (!cv_.wait_for(l, std::chrono::milliseconds(std::max(wait_ms, 0)), fits)) return false;
    used_ += n;
    return true;
}

void DecodeBudget::give(std::uint64_t n) noexcept {
    {
        const std::lock_guard<std::mutex> l(mu_);
        used_ -= std::min(used_, n);
    }
    cv_.notify_all();
}

void DicomwebServer::handle(const http::Request& rq, http::Response& rs) const {
    Handler h(cfg_, store_, rq, rs, decode_budget_);
    h.run();
}

}  // namespace siaara::web
