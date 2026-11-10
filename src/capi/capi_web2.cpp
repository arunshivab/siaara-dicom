// SIAARA DICOM Toolkit - C API, DICOMweb: the origin server over a local store
// (siaara_web_server_*), client limits and the streaming WADO-RS retrieve
// (include/siaara/siaara.h, "networking: full service set").
#include <cstring>
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
};

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_web_server_start(const siaara_web_server_options* o, siaara_store* store,
                                                             siaara_web_server** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!o || o->struct_size < sizeof(siaara_web_server_options))
            return fail(Errc::InvalidArgument, "options must be set up with struct_size");
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
        auto h = std::make_unique<siaara_web_server>();
        h->store = store;
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
        if (Status s = h->server.start(cfg, capi::dicomweb_store(*store->store, policy, o->read_only != 0, qo)); !s)
            return fail(s);
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

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve_to(siaara_web_client* client, const char* study_uid,
                                                                 const char* series_uid, const char* instance_uid,
                                                                 const char* const* transfer_syntaxes, uint32_t ts_count,
                                                                 siaara_store* store, int32_t policy, const char* folder,
                                                                 siaara_web_progress_callback cb, void* user,
                                                                 uint32_t* stored, uint32_t* failed) {
    return guarded([&]() -> siaara_status {
        if (stored) *stored = 0;
        if (failed) *failed = 0;
        if (!client || !client->client) return fail(Errc::InvalidArgument, "client must not be NULL");
        if (!store && !folder) return fail(Errc::InvalidArgument, "store or folder is required");
        if (policy != SIAARA_DUP_KEEP && policy != SIAARA_DUP_REPLACE) return fail(Errc::InvalidArgument, "unknown policy");
        std::string study, series, instance;
        if (!study_uid || !uid_arg(study_uid, study) || !uid_arg(series_uid, series) || !uid_arg(instance_uid, instance))
            return fail(Errc::InvalidArgument, "invalid UID");
        if (!instance.empty() && series.empty()) return fail(Errc::InvalidArgument, "instance_uid needs series_uid");
        if (ts_count && !transfer_syntaxes) return fail(Errc::InvalidArgument, "transfer_syntaxes is NULL");
        std::vector<std::string> ts;
        for (uint32_t i = 0; i < ts_count; ++i) {
            if (!transfer_syntaxes[i]) return fail(Errc::InvalidArgument, "transfer_syntaxes entry is NULL");
            ts.emplace_back(transfer_syntaxes[i]);
        }
        siaara_cancel* token = client->cancel.token();
        if (token && token->is_set()) return fail(Errc::Cancelled, "cancelled");
        const auto pol = policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep;
        const std::string dir = folder ? folder : "";
        uint32_t ok_parts = 0, bad_parts = 0;
        std::uint64_t seen_bytes = 0, seen_total = 0;
        bool stopped = false;
        Status first = Status::ok();
        const auto keep = [&](const std::string& content_type, std::string_view data) -> Status {
            auto mt = web::http::parse_media_type(content_type);
            Status s = Status::ok();
            if (!mt || !mt->is("application", "dicom")) s = Status(Errc::Malformed, "response part is not application/dicom");
            siaara::Bytes bytes;
            if (s) bytes = siaara::Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
            if (s && !dir.empty()) {
                siaara::ReadOptions ro;
                ro.stop_before_pixel_data = true;
                auto f = siaara::read_part10(bytes, ro);
                std::string uid;
                if (!f) s = f.error();
                else uid = f->dataset.get_string(siaara::tags::SOPInstanceUID).value_or("");
                if (s && !capi::dimse::uid_filename_ok(uid)) s = Status(Errc::Malformed, "SOP Instance UID unusable as a file name");
                if (s) {
                    const auto mode = pol == st::DuplicatePolicy::Keep ? siaara::io::WriteMode::NoReplace
                                                                       : siaara::io::WriteMode::Replace;
                    Status w = siaara::io::write_file_atomic(capi::dimse::join_path(dir, uid + ".dcm"), bytes.data(),
                                                             bytes.size(), mode);
                    if (!w && w.code != Errc::Rejected) s = w;  // Rejected: kept the existing file (KEEP)
                }
            }
            if (s && store) {
                auto r = store->store->add_buffer(bytes, pol);
                if (!r) s = r.error();
            }
            if (s) {
                ++ok_parts;
            } else {
                if (bad_parts == 0) first = s;
                ++bad_parts;
            }
            if (cb && cb(user, ok_parts + bad_parts, seen_bytes, seen_total) != 0) {
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
        if (stored) *stored = ok_parts;
        if (failed) *failed = bad_parts;
        if (stopped || (token && token->is_set())) return fail(Errc::Cancelled, "cancelled");
        if (!s) {
            if (s.code == Errc::NotFound) return fail(Errc::NotFound, s.message);
            return fail(s);
        }
        if (bad_parts) return fail(first.code, std::to_string(bad_parts) + " parts were not kept; first: " + first.message);
        return ok();
    });
}

}  // extern "C"
