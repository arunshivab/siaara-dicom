// SIAARA DICOM Toolkit - DICOMweb user agent (PS3.18 chapters 8 to 10), written from the standard.
#include "web/client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "web/base64.hpp"
#include "web/dicom_json.hpp"

namespace siaara::web {

std::string bearer_auth(std::string_view token) { return "Bearer " + std::string(token); }

std::string basic_auth(std::string_view user, std::string_view password) {
    std::string cred(user);
    cred.push_back(':');
    cred.append(password);
    return "Basic " + base64_encode(cred);
}

DicomwebClient::DicomwebClient(DicomwebClientConfig cfg) : cfg_(std::move(cfg)) {
    auto u = http::parse_url(cfg_.base_url);
    if (!u) {
        init_ = u.take_error();
        return;
    }
    url_ = std::move(u).value();
    if (!cfg_.wado_uri_url.empty()) {
        auto w = http::parse_url(cfg_.wado_uri_url);
        if (!w) {
            init_ = w.take_error();
            return;
        }
        wado_url_ = std::move(w).value();
        if (wado_url_.host != url_.host || wado_url_.port != url_.port) {
            init_ = Status(Errc::Unsupported, "the WADO-URI endpoint must be on the same host and port as base_url");
            return;
        }
    } else {
        wado_url_ = url_;
        wado_url_.path += "/wado";
    }
    http_.emplace(url_.host, url_.port, cfg_.limits, cfg_.connect_timeout_ms);
    if (url_.scheme == "https") {
        net::TlsSettings ts = cfg_.tls;
        ts.enabled = true;
        auto ctx = net::TlsContext::create(ts, net::TlsContext::Role::Client);
        net::tls_wipe_secret(ts.identity.password);
        net::tls_wipe_secret(cfg_.tls.identity.password);  // the context keeps (and later wipes) its own copy
        if (!ctx) {
            init_ = ctx.take_error();
            return;
        }
        tls_ = std::move(ctx).value();
        http_->default_port = 443;
        std::shared_ptr<net::TlsContext> tls = tls_;
        http_->connector = [tls](const std::string& host, std::uint16_t port,
                                 int timeout_ms) -> Result<std::unique_ptr<net::Transport>> {
            auto t = tls->connect(host, port, timeout_ms);
            if (!t) return t.take_error();
            return std::unique_ptr<net::Transport>(std::move(t).value());
        };
    } else if (cfg_.tls.enabled) {
        init_ = Status(Errc::InvalidArgument, "TLS settings are enabled but the base URL is not https://");
    }
}

Status DicomwebClient::check_status(const http::Response& rs) const {
    if (rs.status >= 200 && rs.status < 300) return Status::ok();
    if (rs.status == 404) return Status(Errc::NotFound, "HTTP 404 Not Found");
    if (rs.status == 401 || rs.status == 403)
        return Status(Errc::Rejected, "HTTP " + std::to_string(rs.status) + " (authorization)");
    return Status(Errc::Rejected, "HTTP " + std::to_string(rs.status) + " " + std::string(http::reason_phrase(rs.status)));
}

Result<http::Response> DicomwebClient::send(http::Request rq) {
    if (!init_) return init_;
    if (cfg_.authorization) {
        std::string a = cfg_.authorization();
        if (!a.empty()) rq.headers.push_back({"Authorization", std::move(a)});
    }
    return http_->send(rq);
}

Result<http::Response> DicomwebClient::request(std::string_view method, std::string_view path_and_query,
                                                http::Headers headers, std::string body) {
    http::Request rq;
    rq.method = std::string(method);
    rq.target = url_.path + std::string(path_and_query);
    rq.headers = std::move(headers);
    rq.body = std::move(body);
    return send(std::move(rq));
}

namespace {

std::string uid_path(std::string_view study, std::string_view series, std::string_view instance) {
    std::string p = "/studies/" + http::percent_encode(study);
    if (!series.empty()) p += "/series/" + http::percent_encode(series);
    if (!instance.empty()) p += "/instances/" + http::percent_encode(instance);
    return p;
}

void add_query(std::string& target, std::string_view k, std::string_view v) {
    target.push_back(target.find('?') == std::string::npos ? '?' : '&');
    target += http::percent_encode_query(k);
    target.push_back('=');
    target += http::percent_encode_query(v);
}

std::string weight_text(std::size_t i) {
    // 1, 0.9, 0.8 ... down to 0.1 for the rest.
    const int w = i >= 9 ? 100 : 1000 - static_cast<int>(i) * 100;
    if (w == 1000) return "1";
    return "0." + std::to_string(w / 100);
}

std::string retrieve_accept(const std::vector<std::string>& tss) {
    std::string accept;
    if (tss.empty()) return "multipart/related; type=\"application/dicom\"";
    for (std::size_t i = 0; i < tss.size(); ++i) {
        if (i) accept += ", ";
        accept += "multipart/related; type=\"application/dicom\"; transfer-syntax=" + http::quote_if_needed(tss[i]);
        accept += "; q=" + weight_text(i);
    }
    return accept;
}

}  // namespace

Result<std::vector<Dataset>> DicomwebClient::qido(std::string path, const QidoParams& q) {
    std::string target = url_.path + path;
    for (const auto& [k, v] : q.match) add_query(target, k, v);
    if (q.include_all) add_query(target, "includefield", "all");
    for (const auto& f : q.include_fields) add_query(target, "includefield", f);
    if (q.limit) add_query(target, "limit", std::to_string(*q.limit));
    if (q.offset) add_query(target, "offset", std::to_string(*q.offset));
    if (q.fuzzy_matching) add_query(target, "fuzzymatching", "true");
    http::Request rq;
    rq.method = "GET";
    rq.target = std::move(target);
    rq.headers.push_back({"Accept", std::string(media::DicomJson)});
    auto rs = send(std::move(rq));
    if (!rs) return rs.take_error();
    if (Status st = check_status(*rs); !st) return st;
    if (rs->status == 204 || rs->body.empty()) return std::vector<Dataset>{};
    return datasets_from_json(rs->body, cfg_.model);
}

Result<std::vector<Dataset>> DicomwebClient::search_studies(const QidoParams& q) { return qido("/studies", q); }

Result<std::vector<Dataset>> DicomwebClient::search_series(const QidoParams& q, std::string_view study) {
    return qido(study.empty() ? std::string("/series") : "/studies/" + http::percent_encode(study) + "/series", q);
}

Result<std::vector<Dataset>> DicomwebClient::search_instances(const QidoParams& q, std::string_view study,
                                                              std::string_view series) {
    std::string p;
    if (!study.empty()) p = "/studies/" + http::percent_encode(study);
    if (!series.empty()) {
        if (study.empty()) return Status(Errc::InvalidArgument, "a series needs its study");
        p += "/series/" + http::percent_encode(series);
    }
    return qido(p + "/instances", q);
}

Result<std::vector<BulkPart>> DicomwebClient::multipart_get(std::string target, std::string accept) {
    http::Request rq;
    rq.method = "GET";
    rq.target = std::move(target);
    rq.headers.push_back({"Accept", std::move(accept)});
    auto rs = send(std::move(rq));
    if (!rs) return rs.take_error();
    if (Status st = check_status(*rs); !st) return st;
    std::vector<BulkPart> out;
    if (rs->status == 204) return out;
    const std::string* ct = http::find_header(rs->headers, "Content-Type");
    if (!ct) return Status(Errc::Malformed, "response without Content-Type");
    auto mt = http::parse_media_type(*ct);
    if (!mt) return Status(Errc::Malformed, "response with an invalid Content-Type");
    if (!mt->is("multipart", "related")) {
        // Single part response (allowed by some servers for one item).
        out.push_back({*ct, std::move(rs->body)});
        return out;
    }
    const std::string* boundary = mt->param("boundary");
    if (!boundary) return Status(Errc::Malformed, "multipart response without boundary");
    auto parts = multipart::parse(rs->body, *boundary, cfg_.multipart);
    if (!parts) return parts.take_error();
    const std::string* root_type = mt->param("type");
    out.reserve(parts->size());
    for (const auto& p : parts.value()) {
        const std::string* pct = p.content_type();
        out.push_back({pct ? *pct : (root_type ? *root_type : std::string()), std::string(p.body)});
    }
    return out;
}

Result<std::vector<std::string>> DicomwebClient::retrieve_raw(std::string_view study, std::string_view series,
                                                              std::string_view instance,
                                                              const std::vector<std::string>& tss) {
    if (study.empty() || (!instance.empty() && series.empty())) return Status(Errc::InvalidArgument, "incomplete UID path");
    auto parts = multipart_get(url_.path + uid_path(study, series, instance), retrieve_accept(tss));
    if (!parts) return parts.take_error();
    std::vector<std::string> out;
    for (auto& p : parts.value()) {
        auto mt = http::parse_media_type(p.content_type);
        if (!mt || !mt->is("application", "dicom")) return Status(Errc::Malformed, "response part is not application/dicom");
        out.push_back(std::move(p.data));
    }
    return out;
}

Status DicomwebClient::retrieve_stream(
    std::string_view study, std::string_view series, std::string_view instance, const std::vector<std::string>& tss,
    std::uint64_t max_part_bytes,
    const std::function<Status(const std::string& content_type, std::string_view data)>& on_part,
    const std::function<bool(std::uint64_t bytes, std::uint64_t total)>& on_progress) {
    if (!init_) return init_;
    if (study.empty() || (!instance.empty() && series.empty())) return Status(Errc::InvalidArgument, "incomplete UID path");
    if (!on_part) return Status(Errc::InvalidArgument, "on_part must be set");
    http::Request rq;
    rq.method = "GET";
    rq.target = url_.path + uid_path(study, series, instance);
    rq.headers.push_back({"Accept", retrieve_accept(tss)});
    if (cfg_.authorization) {
        std::string a = cfg_.authorization();
        if (!a.empty()) rq.headers.push_back({"Authorization", std::move(a)});
    }
    std::optional<multipart::StreamParser> parser;
    std::string single, single_type, root_type;
    std::uint64_t bytes = 0, total = 0;
    bool is_multipart = false;
    http::BodySink sink;
    sink.on_head = [&](const http::Response& h) -> Status {
        if (Status st = check_status(h); !st) return st;
        if (const std::string* cl = http::find_header(h.headers, "Content-Length")) {
            std::uint64_t v = 0;
            for (char c : *cl) {
                if (c < '0' || c > '9' || v > (UINT64_MAX - 9) / 10) {
                    v = 0;
                    break;
                }
                v = v * 10 + static_cast<std::uint64_t>(c - '0');
            }
            total = v;
        }
        const std::string* ct = http::find_header(h.headers, "Content-Type");
        if (!ct) return h.status == 204 ? Status::ok() : Status(Errc::Malformed, "response without Content-Type");
        auto mt = http::parse_media_type(*ct);
        if (!mt) return Status(Errc::Malformed, "response with an invalid Content-Type");
        if (!mt->is("multipart", "related")) {
            single_type = *ct;
            return Status::ok();
        }
        const std::string* boundary = mt->param("boundary");
        if (!boundary) return Status(Errc::Malformed, "multipart response without boundary");
        if (const std::string* t = mt->param("type")) root_type = *t;
        is_multipart = true;
        parser.emplace(*boundary, cfg_.multipart, max_part_bytes, [&](const multipart::Part& p) -> Status {
            const std::string* pct = p.content_type();
            return on_part(pct ? *pct : root_type, p.body);
        });
        return Status::ok();
    };
    sink.on_body = [&](std::string_view piece) -> Status {
        bytes += piece.size();
        if (on_progress && !on_progress(bytes, total)) return Status(Errc::Cancelled, "cancelled");
        if (is_multipart) return parser->feed(piece);
        if (single.size() + piece.size() > max_part_bytes) return Status(Errc::LimitExceeded, "response too large");
        single.append(piece.data(), piece.size());
        return Status::ok();
    };
    if (!http_) return Status(Errc::InvalidArgument, "client not initialised");
    auto rs = http_->send_streaming(rq, sink);
    if (!rs) return rs.take_error();
    if (Status st = check_status(*rs); !st) return st;
    if (rs->status == 204) return Status::ok();
    if (is_multipart) return parser->finish();
    if (!single_type.empty()) return on_part(single_type, single);
    return Status::ok();
}

void DicomwebClient::set_limits(const http::Limits& limits, const multipart::Limits& multipart_limits) {
    cfg_.limits = limits;
    cfg_.multipart = multipart_limits;
    if (http_) http_->set_limits(limits);
}

Result<std::vector<DicomFile>> DicomwebClient::retrieve(std::string_view study, std::string_view series,
                                                        std::string_view instance, const std::vector<std::string>& tss) {
    auto raw = retrieve_raw(study, series, instance, tss);
    if (!raw) return raw.take_error();
    std::vector<DicomFile> out;
    out.reserve(raw->size());
    for (auto& r : raw.value()) {
        auto f = read_part10(detail::to_bytes(std::move(r)));
        if (!f) return f.take_error();
        out.push_back(std::move(f).value());
    }
    return out;
}

Result<std::vector<Dataset>> DicomwebClient::retrieve_metadata(std::string_view study, std::string_view series,
                                                               std::string_view instance) {
    if (study.empty() || (!instance.empty() && series.empty())) return Status(Errc::InvalidArgument, "incomplete UID path");
    http::Request rq;
    rq.method = "GET";
    rq.target = url_.path + uid_path(study, series, instance) + "/metadata";
    rq.headers.push_back({"Accept", std::string(media::DicomJson)});
    auto rs = send(std::move(rq));
    if (!rs) return rs.take_error();
    if (Status st = check_status(*rs); !st) return st;
    return datasets_from_json(rs->body, cfg_.model);
}

Result<std::vector<BulkPart>> DicomwebClient::retrieve_frames(std::string_view study, std::string_view series,
                                                              std::string_view instance,
                                                              const std::vector<std::uint32_t>& frames,
                                                              const std::vector<std::string>& tss) {
    if (study.empty() || series.empty() || instance.empty() || frames.empty())
        return Status(Errc::InvalidArgument, "study, series, instance and frames are required");
    std::string list;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i] == 0) return Status(Errc::InvalidArgument, "frame numbers are 1-based");
        if (i) list.push_back(',');
        list += std::to_string(frames[i]);
    }
    std::string accept;
    if (tss.empty()) {
        accept = "multipart/related; type=\"application/octet-stream\"";
    } else {
        for (std::size_t i = 0; i < tss.size(); ++i) {
            if (i) accept += ", ";
            const std::string_view mt = tss[i] == "*" ? std::string_view("application/octet-stream")
                                                      : media_type_for_transfer_syntax(tss[i]);
            accept += "multipart/related; type=\"" + std::string(mt) + "\"; transfer-syntax=" + http::quote_if_needed(tss[i]);
            accept += "; q=" + weight_text(i);
        }
    }
    return multipart_get(url_.path + uid_path(study, series, instance) + "/frames/" + list, accept);
}

Result<std::vector<BulkPart>> DicomwebClient::retrieve_bulkdata(std::string_view uri) {
    std::string target;
    const std::string origin_prefix = url_.scheme + "://";
    if (uri.substr(0, origin_prefix.size()) == origin_prefix) {
        auto u = http::parse_url(uri);
        if (!u) return u.take_error();
        if (u->host != url_.host || u->port != url_.port)
            return Status(Errc::Unsupported, "bulk data on another server is not followed");
        const auto after = uri.find('/', origin_prefix.size());
        target = after == std::string_view::npos ? std::string("/") : std::string(uri.substr(after));
    } else if (!uri.empty() && uri[0] == '/') {
        target = std::string(uri);
    } else {
        return Status(Errc::InvalidArgument, "unsupported bulk data URI");
    }
    return multipart_get(std::move(target), "multipart/related; type=\"application/octet-stream\"");
}

Result<BulkPart> DicomwebClient::retrieve_rendered(std::string_view study, std::string_view series,
                                                   std::string_view instance, std::string_view media_type,
                                                   std::uint32_t frame) {
    std::string target = url_.path + uid_path(study, series, instance);
    if (frame) target += "/frames/" + std::to_string(frame);
    target += "/rendered";
    http::Request rq;
    rq.method = "GET";
    rq.target = std::move(target);
    rq.headers.push_back({"Accept", std::string(media_type)});
    auto rs = send(std::move(rq));
    if (!rs) return rs.take_error();
    if (Status st = check_status(*rs); !st) return st;
    const std::string* ct = http::find_header(rs->headers, "Content-Type");
    return BulkPart{ct ? *ct : std::string(), std::move(rs->body)};
}

Result<std::string> DicomwebClient::wado_uri(std::string_view study, std::string_view series, std::string_view object,
                                             std::string_view content_type, std::string_view transfer_syntax) {
    std::string target = wado_url_.path.empty() ? std::string("/") : wado_url_.path;
    add_query(target, "requestType", "WADO");
    add_query(target, "studyUID", study);
    add_query(target, "seriesUID", series);
    add_query(target, "objectUID", object);
    if (!content_type.empty()) add_query(target, "contentType", content_type);
    if (!transfer_syntax.empty()) add_query(target, "transferSyntax", transfer_syntax);
    http::Request rq;
    rq.method = "GET";
    rq.target = std::move(target);
    rq.headers.push_back({"Accept", content_type.empty() ? std::string("*/*") : std::string(content_type)});
    auto rs = send(std::move(rq));
    if (!rs) return rs.take_error();
    if (Status st = check_status(*rs); !st) return st;
    return std::move(rs->body);
}

Result<StowResult> DicomwebClient::store(const std::vector<std::string>& part10, std::string_view study) {
    if (part10.empty()) return Status(Errc::InvalidArgument, "nothing to store");
    multipart::Writer w;
    for (const auto& p : part10) w.add(media::Dicom, p);
    http::Request rq;
    rq.method = "POST";
    rq.target = url_.path + (study.empty() ? std::string("/studies") : "/studies/" + http::percent_encode(study));
    rq.body = w.finish();
    rq.headers.push_back({"Content-Type", w.content_type(media::Dicom)});
    rq.headers.push_back({"Accept", std::string(media::DicomJson)});
    auto rs = send(std::move(rq));
    if (!rs) return rs.take_error();
    StowResult r;
    r.http_status = rs->status;
    // 200 / 202 / 409 carry a Store Instances Response; other statuses are plain failures.
    if (rs->status != 200 && rs->status != 202 && rs->status != 409) {
        if (Status st = check_status(*rs); !st) return st;
    }
    const std::string* ct = http::find_header(rs->headers, "Content-Type");
    if (!rs->body.empty() && ct) {
        auto mt = http::parse_media_type(*ct);
        if (mt && (mt->is("application", "dicom+json") || mt->is("application", "json"))) {
            auto d = dataset_from_json(rs->body, cfg_.model);
            if (!d) return d.take_error();
            r.response = std::move(d).value();
        }
    }
    if (const Element* ok = r.response.find(webtags::ReferencedSOPSequence); ok && ok->is_sequence())
        for (const auto& item : ok->items) r.stored.push_back(item.get_string(webtags::ReferencedSOPInstanceUID).value_or(""));
    if (const Element* bad = r.response.find(webtags::FailedSOPSequence); bad && bad->is_sequence())
        for (const auto& item : bad->items)
            r.failed.emplace_back(item.get_string(webtags::ReferencedSOPInstanceUID).value_or(""),
                                  static_cast<std::uint16_t>(item.get_int(webtags::FailureReason).value_or(0)));
    return r;
}

}  // namespace siaara::web
