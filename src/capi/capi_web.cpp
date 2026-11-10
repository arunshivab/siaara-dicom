// SIAARA DICOM Toolkit - C API: DICOMweb additions (metadata, frames, bulk retrieve, WADO-URI,
// cancellation) (include/siaara/siaara.h, "viewer / PACS additions").
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "capi/capi_internal.hpp"
#include "core/incomplete_policy.hpp"
#include "core/reader.hpp"
#include "web/dicom_model.hpp"

struct siaara_web_parts {
    std::vector<siaara::web::BulkPart> parts;
    siaara::capi::LiveHandle live_{};
};

namespace {

using siaara::Errc;
using siaara::Status;
namespace capi = siaara::capi;
namespace web = siaara::web;

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

siaara_status copy_out(std::string_view s, char* buf, size_t cap, size_t* needed) {
    if (needed) *needed = s.size();
    if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (s.size() + 1 > cap) {
        std::memcpy(buf, s.data(), cap - 1);
        buf[cap - 1] = '\0';
        return fail(Errc::BufferTooSmall, "buffer too small");
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return ok();
}

// UID argument: NULL = absent; otherwise a valid UID.
bool uid_arg(const char* s, std::string& out) {
    out.clear();
    if (!s) return true;
    out = s;
    return !out.empty() && web::is_uid(out);
}

bool ts_list(const char* const* ts, uint32_t n, std::vector<std::string>& out) {
    if (n && !ts) return false;
    for (uint32_t i = 0; i < n; ++i) {
        if (!ts[i] || !ts[i][0]) return false;
        const std::string t(ts[i]);
        if (t != "*" && !web::is_uid(t)) return false;
        out.push_back(t);
    }
    return true;
}

// Runs one client request with the attached token: refuses to start when it is set and maps a
// failure after cancellation to SIAARA_ERR_CANCELLED.
template <class F>
siaara_status with_cancel(siaara_web_client* c, F&& f) {
    siaara_cancel* t = c->cancel.token();
    if (t && t->is_set()) return fail(Errc::Cancelled, "cancelled");
    const siaara_status s = f();
    if (s != SIAARA_OK && t && t->is_set()) return fail(Errc::Cancelled, "cancelled");
    return s;
}

siaara_status parts_out(std::vector<web::BulkPart> parts, siaara_web_parts** out) {
    auto h = std::make_unique<siaara_web_parts>();
    h->parts = std::move(parts);
    *out = h.release();
    return ok();
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_web_client_set_cancel(siaara_web_client* client, siaara_cancel* token) {
    return guarded([&]() -> siaara_status {
        if (!client) return fail(Errc::InvalidArgument, "client must not be NULL");
        siaara_web_client* raw = client;
        client->cancel.attach(token, [raw]() {
            if (raw->client) raw->client->interrupt();
        });
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_metadata(siaara_web_client* client, const char* study_uid,
                                                              const char* series_uid, const char* instance_uid, char* buf,
                                                              size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!client) return fail(Errc::InvalidArgument, "client must not be NULL");
        std::string st, se, in;
        if (!uid_arg(study_uid, st) || !uid_arg(series_uid, se) || !uid_arg(instance_uid, in) || st.empty() ||
            (!in.empty() && se.empty()))
            return fail(Errc::InvalidArgument, "a valid study UID is required (and a series UID with an instance UID)");
        std::string path = "/studies/" + st;
        if (!se.empty()) path += "/series/" + se;
        if (!in.empty()) path += "/instances/" + in;
        path += "/metadata";
        return with_cancel(client, [&]() -> siaara_status {
            auto rs = client->client->request("GET", path, {{"Accept", "application/dicom+json"}});
            if (!rs) return fail(rs.error());
            if (rs->status == 404) return fail(Errc::NotFound, "HTTP 404 Not Found");
            if (rs->status < 200 || rs->status >= 300) return fail(Errc::Rejected, "HTTP " + std::to_string(rs->status));
            client->last_response = rs->status == 204 || rs->body.empty() ? std::string("[]") : std::move(rs->body);
            return copy_out(client->last_response, buf, cap, needed);
        });
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_frames(siaara_web_client* client, const char* study_uid,
                                                            const char* series_uid, const char* instance_uid,
                                                            const uint32_t* frames, uint32_t count,
                                                            const char* const* transfer_syntaxes, uint32_t ts_count,
                                                            siaara_web_parts** out) {
    return guarded([&]() -> siaara_status {
        if (!client || !out) return fail(Errc::InvalidArgument, "client and out must not be NULL");
        *out = nullptr;
        std::string st, se, in;
        if (!uid_arg(study_uid, st) || !uid_arg(series_uid, se) || !uid_arg(instance_uid, in) || st.empty() ||
            se.empty() || in.empty())
            return fail(Errc::InvalidArgument, "study, series and instance UIDs are required");
        if (!frames || count == 0) return fail(Errc::InvalidArgument, "no frames");
        std::vector<std::uint32_t> f(frames, frames + count);
        for (std::uint32_t n : f)
            if (n == 0) return fail(Errc::InvalidArgument, "frame numbers are 1-based");
        std::vector<std::string> ts;
        if (!ts_list(transfer_syntaxes, ts_count, ts)) return fail(Errc::InvalidArgument, "bad transfer syntax list");
        return with_cancel(client, [&]() -> siaara_status {
            auto r = client->client->retrieve_frames(st, se, in, f, ts);
            if (!r) return fail(r.error());
            return parts_out(std::move(r).value(), out);
        });
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve(siaara_web_client* client, const char* study_uid,
                                                              const char* series_uid, const char* instance_uid,
                                                              const char* const* transfer_syntaxes, uint32_t ts_count,
                                                              siaara_web_parts** out) {
    return guarded([&]() -> siaara_status {
        if (!client || !out) return fail(Errc::InvalidArgument, "client and out must not be NULL");
        *out = nullptr;
        std::string st, se, in;
        if (!uid_arg(study_uid, st) || !uid_arg(series_uid, se) || !uid_arg(instance_uid, in) || st.empty() ||
            (!in.empty() && se.empty()))
            return fail(Errc::InvalidArgument, "a valid study UID is required (and a series UID with an instance UID)");
        std::vector<std::string> ts;
        if (!ts_list(transfer_syntaxes, ts_count, ts)) return fail(Errc::InvalidArgument, "bad transfer syntax list");
        return with_cancel(client, [&]() -> siaara_status {
            auto r = client->client->retrieve_raw(st, se, in, ts);
            if (!r) return fail(r.error());
            std::vector<web::BulkPart> parts;
            parts.reserve(r->size());
            for (std::string& p : *r) parts.push_back({"application/dicom", std::move(p)});
            return parts_out(std::move(parts), out);
        });
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_uri(siaara_web_client* client, const char* study_uid,
                                                         const char* series_uid, const char* object_uid,
                                                         const char* content_type, const char* transfer_syntax,
                                                         siaara_web_parts** out) {
    return guarded([&]() -> siaara_status {
        if (!client || !out) return fail(Errc::InvalidArgument, "client and out must not be NULL");
        *out = nullptr;
        std::string st, se, ob, ts;
        if (!uid_arg(study_uid, st) || !uid_arg(series_uid, se) || !uid_arg(object_uid, ob) || st.empty() ||
            se.empty() || ob.empty())
            return fail(Errc::InvalidArgument, "study, series and object UIDs are required");
        if (!uid_arg(transfer_syntax, ts)) return fail(Errc::InvalidArgument, "invalid transfer syntax UID");
        const std::string ct = content_type ? content_type : "application/dicom";
        for (char c : ct)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return fail(Errc::InvalidArgument, "bad content type");
        return with_cancel(client, [&]() -> siaara_status {
            auto r = client->client->wado_uri(st, se, ob, ct, ts);
            if (!r) return fail(r.error());
            std::vector<web::BulkPart> parts;
            parts.push_back({ct, std::move(r).value()});
            return parts_out(std::move(parts), out);
        });
    });
}

SIAARA_API void SIAARA_CALL siaara_web_parts_free(siaara_web_parts* parts) SIAARA_NOEXCEPT { delete parts; }

SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_count(const siaara_web_parts* parts, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!parts || !out) return fail(Errc::InvalidArgument, "parts and out must not be NULL");
        *out = static_cast<uint32_t>(parts->parts.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_get(const siaara_web_parts* parts, uint32_t index,
                                                          const uint8_t** out_data, uint64_t* out_size, char* buf,
                                                          size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!parts || !out_data) return fail(Errc::InvalidArgument, "parts and out_data must not be NULL");
        if (index >= parts->parts.size()) return fail(Errc::NotFound, "part index out of range");
        const web::BulkPart& p = parts->parts[index];
        *out_data = reinterpret_cast<const uint8_t*>(p.data.data());
        if (out_size) *out_size = p.data.size();
        if (!buf && cap == 0) {
            if (needed) *needed = p.content_type.size();
            return ok();
        }
        return copy_out(p.content_type, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_open_file(const siaara_web_parts* parts, uint32_t index,
                                                                siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!parts || !out) return fail(Errc::InvalidArgument, "parts and out must not be NULL");
        *out = nullptr;
        if (index >= parts->parts.size()) return fail(Errc::NotFound, "part index out of range");
        const std::string& d = parts->parts[index].data;
        // Bytes from the network: no overlong-length recovery (R-163).
        auto r = siaara::read_part10(siaara::Bytes::copy_of(d.data(), d.size()), siaara::network_read_options());
        if (!r) return fail(r.error());
        auto f = std::make_unique<siaara_file>();
        f->file = std::move(r).value();
        *out = f.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_add_to_store(const siaara_web_parts* parts, siaara_store* store,
                                                                   int32_t policy, uint32_t* added, uint32_t* failed) {
    return guarded([&]() -> siaara_status {
        if (added) *added = 0;
        if (failed) *failed = 0;
        if (!parts || !store) return fail(Errc::InvalidArgument, "parts and store must not be NULL");
        if (policy != SIAARA_DUP_KEEP && policy != SIAARA_DUP_REPLACE) return fail(Errc::InvalidArgument, "unknown policy");
        const auto pol = policy == SIAARA_DUP_REPLACE ? siaara::store::DuplicatePolicy::Replace
                                                      : siaara::store::DuplicatePolicy::Keep;
        uint32_t a = 0, f = 0;
        Status first = Status::ok();
        for (const web::BulkPart& p : parts->parts) {
            auto r = store->store->add_buffer(siaara::Bytes::copy_of(p.data.data(), p.data.size()), pol, 0,
                                              siaara::store::network_extract_options());
            if (r) {
                ++a;
            } else {
                if (f == 0) first = r.error();
                ++f;
            }
        }
        if (added) *added = a;
        if (failed) *failed = f;
        if (f) return fail(first.code, std::to_string(f) + " parts were not added; first: " + first.message);
        return ok();
    });
}

}  // extern "C"
