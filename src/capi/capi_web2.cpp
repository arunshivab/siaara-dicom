// SIAARA DICOM Toolkit - C API, DICOMweb: the origin server over a local store
// (siaara_web_server_*), client limits and the streaming WADO-RS retrieve
// (include/siaara/siaara.h, "networking: full service set").
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "capi/capi_internal.hpp"
#include "capi/capi_net_internal.hpp"
#include "capi/store_services.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "net/store_support.hpp"
#include "store/common.hpp"
#include "web/client.hpp"
#include "web/http.hpp"
#include "web/server.hpp"

namespace {

using siaara::Errc;
using siaara::Status;
namespace capi = siaara::capi;
namespace web = siaara::web;
namespace st = siaara::store;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
siaara_status fail(const Status& s) noexcept { return capi::set_error(s); }
siaara_status ok() noexcept { return capi::clear_error(); }

template <class F>
siaara_status guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return fail(Errc::OutOfMemory, "out of memory");
    } catch (...) {
        return fail(Errc::Internal, "internal error");
    }
}

bool uid_arg(const char* s, std::string& out) {
    out.clear();
    if (!s) return true;
    out = s;
    if (out.empty() || out.size() > 64) return false;
    for (char c : out)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

}  // namespace

struct siaara_web_server {
    web::DicomwebServer server;
    siaara_store* store = nullptr;
    siaara_web_authorize_callback authorize = nullptr;
    void* authorize_user = nullptr;
    capi::LiveHandle live_{};
    // R-150: the store stays alive until the server is freed (a siaara_store_free in between is deferred).
    void attach_store(siaara_store* s) {
        store = s;
        capi::store_attach(s);
    }
    ~siaara_web_server() {
        server.stop();
        capi::store_detach(store);
    }
};

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_web_server_start(const siaara_web_server_options* o, siaara_store* store,
                                                             siaara_web_server** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        // The first version of the options ended with max_body_bytes; later fields are read when
        // struct_size covers them.
        if (!o || o->struct_size < offsetof(siaara_web_server_options, max_body_bytes) + sizeof(uint64_t))
            return fail(Errc::InvalidArgument, "options must be set up with struct_size");
        const auto has = [&](std::size_t end) { return o->struct_size >= end; };
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (o->port > 65535) return fail(Errc::InvalidArgument, "port must be 0..65535");
        for (int32_t v : {o->require_client_certificate, o->allow_insecure_remote, o->read_only})
            if (v != 0 && v != 1) return fail(Errc::InvalidArgument, "flags must be 0 or 1");
        if (o->duplicate_policy != SIAARA_DUP_KEEP && o->duplicate_policy != SIAARA_DUP_REPLACE)
            return fail(Errc::InvalidArgument, "unknown duplicate_policy");
        web::DicomwebServerConfig cfg;
        if (o->bind_address) cfg.http.bind_address = o->bind_address;
        cfg.http.port = static_cast<std::uint16_t>(o->port);
        cfg.http.allow_insecure_remote = o->allow_insecure_remote != 0;
        if (o->max_connections) cfg.http.max_connections = o->max_connections;
        if (o->max_body_bytes) cfg.http.limits.max_body = o->max_body_bytes;
        cfg.http.log = capi::log_sink(SIAARA_LOG_INFO);
        if (o->tls) {
            if (o->tls->struct_size < sizeof(siaara_tls_options)) return fail(Errc::InvalidArgument, "tls->struct_size is too small");
            cfg.http.tls = capi::tls_settings(o->tls);
            cfg.http.tls.require_client_certificate = o->require_client_certificate != 0;
            cfg.http.tls.log = capi::log_sink(SIAARA_LOG_INFO);
        } else if (o->require_client_certificate) {
            return fail(Errc::InvalidArgument, "require_client_certificate needs tls");
        }
        if (o->base_path) cfg.base_path = o->base_path;
        if (o->public_base_url) cfg.public_base_url = o->public_base_url;
        if (o->cors_allow_origin) cfg.cors_allow_origin = o->cors_allow_origin;
        if (o->max_qido_results) cfg.max_qido_results = o->max_qido_results;
        if (has(offsetof(siaara_web_server_options, max_response_bytes) + sizeof(uint64_t)) && o->max_response_bytes)
            cfg.max_response_bytes = o->max_response_bytes;
        if (has(offsetof(siaara_web_server_options, response_timeout_ms) + sizeof(int32_t))) {
            if (o->response_timeout_ms < -1) return fail(Errc::InvalidArgument, "response_timeout_ms: -1, 0 or a time");
            if (o->response_timeout_ms == -1) cfg.http.limits.send_timeout_ms = 0;
            else if (o->response_timeout_ms) cfg.http.limits.send_timeout_ms = o->response_timeout_ms;
        }
        if (has(offsetof(siaara_web_server_options, min_response_rate) + sizeof(uint32_t)) && o->min_response_rate)
            cfg.http.limits.min_send_rate = o->min_response_rate;
        if (has(offsetof(siaara_web_server_options, incomplete_policy) + sizeof(int32_t)))
            if (siaara_status ps = capi::incomplete_policy(o->incomplete_policy, cfg.incomplete_policy); ps != SIAARA_OK)
                return ps;
        if (has(offsetof(siaara_web_server_options, quarantine_response) + sizeof(int32_t)))
            if (siaara_status ps = capi::quarantine_response(o->quarantine_response, cfg.quarantine_response); ps != SIAARA_OK)
                return ps;
        auto h = std::make_unique<siaara_web_server>();
        h->attach_store(store);
        h->authorize = o->authorize;
        h->authorize_user = o->authorize_user;
        if (o->authorize) {
            siaara_web_server* raw = h.get();
            cfg.authorize = [raw](const web::http::Request& rq) -> bool {
                const std::string* a = web::http::find_header(rq.headers, "Authorization");
                try {
                    return raw->authorize(raw->authorize_user, rq.method.c_str(), rq.target.c_str(),
                                          a ? a->c_str() : "") == 1;
                } catch (...) {
                    capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            };
        }
        capi::StoreQrOptions qo;
        qo.max_find_results = static_cast<std::uint32_t>(cfg.max_qido_results) + 1u;
        const auto policy = o->duplicate_policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep;
        web::Store backend = capi::dicomweb_store(*store->store, policy, o->read_only != 0, qo);
        if (o->read_only == 0)
            backend.quarantine = [store](const siaara::DicomFile& f, const siaara::Bytes& part, const std::string& peer) -> std::uint16_t {
                return capi::quarantine_bytes(store, part, "STOW-RS", std::string(), peer,
                                              f.dataset.get_string(siaara::tags::SOPClassUID).value_or(""),
                                              f.dataset.get_string(siaara::tags::SOPInstanceUID).value_or(""), f.read_loss,
                                              f.warnings.empty() ? std::string() : f.warnings.front());
            };
        if (o->read_only == 0)
            backend.keep_rejected = [store](const siaara::DicomFile& f, const siaara::Bytes& part, const std::string& peer) {
                (void)capi::quarantine_bytes(store, part, "STOW-RS", std::string(), peer,
                                             f.dataset.get_string(siaara::tags::SOPClassUID).value_or(""),
                                             f.dataset.get_string(siaara::tags::SOPInstanceUID).value_or(""), f.read_loss,
                                             f.warnings.empty() ? std::string() : f.warnings.front(), true);
            };
        if (Status s = h->server.start(cfg, std::move(backend)); !s) return fail(s);
        capi::log(SIAARA_LOG_INFO, false, "DICOMweb server listening on port " + std::to_string(h->server.port()));
        *out = h.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_server_port(const siaara_web_server* server, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!server || !out) return fail(Errc::InvalidArgument, "server and out must not be NULL");
        *out = server->server.port();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_web_server_free(siaara_web_server* server) SIAARA_NOEXCEPT { delete server; }

SIAARA_API siaara_status SIAARA_CALL siaara_web_client_set_limits(siaara_web_client* client,
                                                                  const siaara_web_limits* limits) {
    return guarded([&]() -> siaara_status {
        if (!client || !client->client) return fail(Errc::InvalidArgument, "client must not be NULL");
        if (!limits || limits->struct_size < sizeof(siaara_web_limits))
            return fail(Errc::InvalidArgument, "limits must be set up with struct_size");
        if (limits->read_timeout_ms < 0 || limits->body_timeout_ms < -1)
            return fail(Errc::InvalidArgument, "timeouts must not be negative (body_timeout_ms: -1 = none)");
        web::http::Limits l = client->client->limits();
        web::multipart::Limits m = client->client->multipart_limits();
        if (limits->read_timeout_ms) l.read_timeout_ms = limits->read_timeout_ms;
        if (limits->max_body) l.max_body = limits->max_body;
        if (limits->body_timeout_ms == -1) l.body_timeout_ms = 0;
        else if (limits->body_timeout_ms) l.body_timeout_ms = limits->body_timeout_ms;
        if (limits->max_parts) m.max_parts = limits->max_parts;
        if (limits->max_part_bytes) {
            client->max_part_bytes = limits->max_part_bytes;
        }
        client->client->set_limits(l, m);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve_to_ex(siaara_web_client* client, const char* study_uid,
                                                                    const char* series_uid, const char* instance_uid,
                                                                    const siaara_web_retrieve_options* options,
                                                                    siaara_web_progress_callback cb, void* user,
                                                                    siaara_web_retrieve_report* report) {
    return guarded([&]() -> siaara_status {
        if (report) {
            if (report->struct_size < sizeof(siaara_web_retrieve_report))
                return fail(Errc::InvalidArgument, "report must be set up with struct_size");
            const uint32_t size = report->struct_size;
            std::memset(report, 0, sizeof *report);
            report->struct_size = size;
        }
        if (!client || !client->client) return fail(Errc::InvalidArgument, "client must not be NULL");
        if (!options || options->struct_size < sizeof(siaara_web_retrieve_options))
            return fail(Errc::InvalidArgument, "options must be set up with struct_size");
        siaara_store* const store = options->store;
        if (!store && !options->folder) return fail(Errc::InvalidArgument, "store or folder is required");
        if (options->duplicate_policy != SIAARA_DUP_KEEP && options->duplicate_policy != SIAARA_DUP_REPLACE)
            return fail(Errc::InvalidArgument, "unknown policy");
        if (options->allow_other_uids != 0 && options->allow_other_uids != 1)
            return fail(Errc::InvalidArgument, "allow_other_uids must be 0 or 1");
        siaara::IncompletePolicy incomplete = siaara::kDefaultIncompletePolicy;
        if (siaara_status ps = capi::incomplete_policy(options->incomplete_policy, incomplete); ps != SIAARA_OK) return ps;
        std::string study, series, instance;
        if (!study_uid || !uid_arg(study_uid, study) || !uid_arg(series_uid, series) || !uid_arg(instance_uid, instance))
            return fail(Errc::InvalidArgument, "invalid UID");
        if (!instance.empty() && series.empty()) return fail(Errc::InvalidArgument, "instance_uid needs series_uid");
        if (options->transfer_syntax_count && !options->transfer_syntaxes)
            return fail(Errc::InvalidArgument, "transfer_syntaxes is NULL");
        std::vector<std::string> ts;
        for (uint32_t i = 0; i < options->transfer_syntax_count; ++i) {
            if (!options->transfer_syntaxes[i]) return fail(Errc::InvalidArgument, "transfer_syntaxes entry is NULL");
            ts.emplace_back(options->transfer_syntaxes[i]);
        }
        siaara_cancel* token = client->cancel.token();
        if (token && token->is_set()) return fail(Errc::Cancelled, "cancelled");
        const auto pol = options->duplicate_policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace
                                                                         : st::DuplicatePolicy::Keep;
        const std::string dir = options->folder ? options->folder : "";
        const bool strict_uids = options->allow_other_uids == 0;
        siaara_web_retrieve_report rep{};
        std::uint64_t seen_bytes = 0, seen_total = 0;
        bool stopped = false;
        Status first = Status::ok();
        const auto keep = [&](const std::string& content_type, std::string_view data) -> Status {
            auto mt = web::http::parse_media_type(content_type);
            Status s = Status::ok();
            if (!mt || !mt->is("application", "dicom")) s = Status(Errc::Malformed, "response part is not application/dicom");
            siaara::Bytes bytes;
            bool loss = false, raw = false;
            std::string uid;
            if (s) {
                bytes = siaara::Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
                // The whole part is read: only that shows whether it is complete (R-116).
                auto f = siaara::read_part10(bytes, siaara::network_read_options());
                if (!f) {
                    s = f.error();
                } else {
                    loss = !f->complete();
                    raw = !f->had_preamble;
                    uid = f->dataset.get_string(siaara::tags::SOPInstanceUID).value_or("");
                    // R-152: the part must be what was asked for.
                    const auto is = [&](siaara::Tag t, const std::string& want) {
                        return want.empty() || f->dataset.get_string(t).value_or("") == want;
                    };
                    if (strict_uids && !(is(siaara::tags::StudyInstanceUID, study) && is(siaara::tags::SeriesInstanceUID, series) &&
                                         is(siaara::tags::SOPInstanceUID, instance))) {
                        ++rep.uid_mismatch;
                        s = Status(Errc::Rejected, "response part is not from the requested study / series / instance");
                    } else if (loss && incomplete == siaara::IncompletePolicy::Quarantine && store) {
                        // Kept outside the archive: not stored, so it counts as failed as well.
                        const std::uint16_t kept = capi::quarantine_bytes(
                            store, bytes, "WADO-RS", std::string(), std::string(),
                            f->dataset.get_string(siaara::tags::SOPClassUID).value_or(""), uid, f->read_loss,
                            f->warnings.empty() ? std::string() : f->warnings.front());
                        if (kept == 0) ++rep.quarantined;
                        else ++rep.incomplete_refused;
                        s = Status(kept == 0xA700 ? Errc::LimitExceeded : Errc::Rejected,
                                   std::string(siaara::net::kIncompleteInput) +
                                       (kept == 0 ? ": the response part was not read completely; quarantined"
                                                  : ": the response part was not read completely; the quarantine did not take it"));
                    } else if (loss && (incomplete != siaara::IncompletePolicy::AcceptAsReceived || raw)) {
                        // Never stored rewritten; refused unless it can be kept exactly as received.
                        ++rep.incomplete_refused;
                        s = Status(Errc::Rejected,
                                   std::string(siaara::net::kIncompleteInput) + ": the response part was not read completely" +
                                       (f->warnings.empty() ? std::string() : " (" + f->warnings.front() + ")"));
                    } else if (raw) {
                        // A complete data set without a preamble: kept as a new Part 10 file.
                        std::vector<std::uint8_t> out;
                        if (Status w = siaara::write_part10(*f, out); !w) s = w;
                        else bytes = capi::dimse::bytes_of(std::move(out));
                    }
                }
            }
            std::string path;
            bool created = false;
            if (s && !dir.empty()) {
                if (!capi::dimse::uid_filename_ok(uid)) s = Status(Errc::Malformed, "SOP Instance UID unusable as a file name");
                if (s) {
                    path = capi::dimse::join_path(dir, uid + ".dcm");
                    std::error_code ec;
                    const bool existed = std::filesystem::exists(st::u8path(path), ec);
                    const auto mode = pol == st::DuplicatePolicy::Keep ? siaara::io::WriteMode::NoReplace
                                                                       : siaara::io::WriteMode::Replace;
                    Status w = siaara::io::write_file_atomic(path, bytes.data(), bytes.size(), mode);
                    if (!w && w.code != Errc::Rejected) s = w;  // Rejected: kept the existing file (KEEP)
                    created = static_cast<bool>(w) && !existed;
                }
            }
            if (s && store) {
                auto r = store->store->add_buffer(bytes, pol);
                if (!r) {
                    s = r.error();
                    // R-152: a part the store refused leaves no file behind.
                    if (created) {
                        std::error_code ec;
                        std::filesystem::remove(st::u8path(path), ec);
                    }
                }
            }
            if (s) {
                ++rep.stored;
                if (loss) ++rep.incomplete_accepted;
                else if (raw) ++rep.rewritten;
            } else {
                if (rep.failed == 0) first = s;
                ++rep.failed;
            }
            if (cb && cb(user, rep.stored + rep.failed, seen_bytes, seen_total) != 0) {
                stopped = true;
                return Status(Errc::Cancelled, "stopped by the callback");
            }
            return Status::ok();
        };
        const auto progress = [&](std::uint64_t bytes, std::uint64_t total) -> bool {
            seen_bytes = bytes;
            seen_total = total;
            return !(token && token->is_set());
        };
        const std::uint64_t max_part = client->max_part_bytes ? client->max_part_bytes : client->client->limits().max_body;
        Status s = client->client->retrieve_stream(study, series, instance, ts, max_part, keep, progress);
        if (report) {
            const uint32_t size = report->struct_size;
            *report = rep;
            report->struct_size = size;
        }
        if (rep.incomplete_accepted)
            capi::log(SIAARA_LOG_WARNING, false,
                      "WADO-RS retrieve: " + std::to_string(rep.incomplete_accepted) +
                          " incomplete parts were stored exactly as received");
        if (rep.rewritten)
            capi::log(SIAARA_LOG_WARNING, false,
                      "WADO-RS retrieve: " + std::to_string(rep.rewritten) +
                          " parts had no Part 10 preamble and were stored as new Part 10 files");
        if (stopped || (token && token->is_set())) return fail(Errc::Cancelled, "cancelled");
        if (!s) {
            if (s.code == Errc::NotFound) return fail(Errc::NotFound, s.message);
            return fail(s);
        }
        if (rep.failed)
            return fail(first.code, std::to_string(rep.failed) + " parts were not kept; first: " + first.message);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve_to(siaara_web_client* client, const char* study_uid,
                                                                 const char* series_uid, const char* instance_uid,
                                                                 const char* const* transfer_syntaxes, uint32_t ts_count,
                                                                 siaara_store* store, int32_t policy, const char* folder,
                                                                 siaara_web_progress_callback cb, void* user,
                                                                 uint32_t* stored, uint32_t* failed) {
    return guarded([&]() -> siaara_status {
        if (stored) *stored = 0;
        if (failed) *failed = 0;
        siaara_web_retrieve_options o{};
        o.struct_size = sizeof o;
        o.duplicate_policy = policy;
        o.store = store;
        o.folder = folder;
        o.transfer_syntaxes = transfer_syntaxes;
        o.transfer_syntax_count = ts_count;
        siaara_web_retrieve_report rep{};
        rep.struct_size = sizeof rep;
        const siaara_status s =
            siaara_web_wado_retrieve_to_ex(client, study_uid, series_uid, instance_uid, &o, cb, user, &rep);
        if (stored) *stored = rep.stored;
        if (failed) *failed = rep.failed;
        return s;
    });
}

}  // extern "C"
