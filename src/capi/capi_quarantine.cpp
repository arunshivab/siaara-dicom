// SIAARA DICOM Toolkit - C API: the quarantine area of a store (siaara_store_quarantine_*;
// R-116, SIAARA_INCOMPLETE_QUARANTINE). include/siaara/siaara.h.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#include "capi/capi_internal.hpp"
#include "capi/capi_net_internal.hpp"
#include "core/io.hpp"
#include "store/index.hpp"
#include "store/quarantine.hpp"

namespace {

using siaara::Errc;
using siaara::Status;
namespace capi = siaara::capi;
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

// The area of a call, or null (error set) for a bad store / area.
st::Quarantine* area_of(const siaara_store* store, int32_t area) {
    if (!store) {
        (void)fail(Errc::InvalidArgument, "store must not be NULL");
        return nullptr;
    }
    if (area == SIAARA_AREA_QUARANTINE && store->quarantine) return store->quarantine.get();
    if (area == SIAARA_AREA_REJECTED && store->rejected) return store->rejected.get();
    (void)fail(Errc::InvalidArgument, "unknown area (SIAARA_AREA_*)");
    return nullptr;
}

template <std::size_t N>
void put(char (&dst)[N], const std::string& v) {
    const std::size_t n = std::min(v.size(), N - 1);
    std::memcpy(dst, v.data(), n);
    dst[n] = '\0';
}

}  // namespace

namespace siaara::capi {

std::uint16_t quarantine_parts(siaara_store* store, const st::QuarantinePart* parts, std::size_t part_count, const char* source,
                               const std::string& calling_ae, const std::string& peer, const std::string& sop_class,
                               const std::string& sop_instance, std::uint32_t read_loss, const std::string& warning,
                               bool rejected) {
    st::Quarantine* area = !store ? nullptr : rejected ? store->rejected.get() : store->quarantine.get();
    if (!area) return 0x0110;
    st::QuarantineRecord rec;
    rec.source = source;
    rec.calling_ae = calling_ae;
    rec.peer = peer;
    rec.sop_class_uid = sop_class;
    rec.sop_instance_uid = sop_instance;
    rec.read_loss = read_loss;
    rec.warning = warning;
    auto r = area->add(parts, part_count, std::move(rec));
    if (r) return 0;
    log(SIAARA_LOG_WARNING, false, "quarantine: an incomplete instance was not kept: " + r.error().message);
    return r.error().code == Errc::LimitExceeded ? 0xA700 : 0x0110;
}

std::uint16_t quarantine_bytes(siaara_store* store, const Bytes& bytes, const char* source, const std::string& calling_ae,
                               const std::string& peer, const std::string& sop_class, const std::string& sop_instance,
                               std::uint32_t read_loss, const std::string& warning, bool rejected) {
    const st::QuarantinePart one{bytes.data(), bytes.size()};
    return quarantine_parts(store, &one, 1, source, calling_ae, peer, sop_class, sop_instance, read_loss, warning, rejected);
}

std::uint16_t quarantine_request(siaara_store* store, const net::StoreRequest& rq, const char* source, const std::string& peer,
                                 bool rejected) {
    // R-159: only the file meta is built in memory; the received bytes (which may be a spooled,
    // memory-mapped data set of many GB) are streamed from where they are.
    std::vector<std::uint8_t> meta;
    if (Status s = net::encode_received_meta(rq, meta); !s) return 0x0110;
    const st::QuarantinePart parts[2] = {{meta.data(), meta.size()}, {rq.raw.data(), rq.raw.size()}};
    return quarantine_parts(store, parts, 2, source, rq.calling_ae, peer, rq.sop_class_uid, rq.sop_instance_uid, rq.read_loss,
                            rq.read_warning, rejected);
}

}  // namespace siaara::capi

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_count(const siaara_store* store, int32_t area,
                                                                   uint32_t* items, uint64_t* bytes) {
    return guarded([&]() -> siaara_status {
        st::Quarantine* q = area_of(store, area);
        if (!q) return SIAARA_ERR_INVALID_ARGUMENT;
        if (items) *items = static_cast<uint32_t>(std::min<std::size_t>(q->items(), 0xFFFFFFFFu));
        if (bytes) *bytes = q->total_bytes();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_get(const siaara_store* store, int32_t area,
                                                                 uint32_t index, siaara_quarantine_record* out) {
    return guarded([&]() -> siaara_status {
        st::Quarantine* q = area_of(store, area);
        if (!q) return SIAARA_ERR_INVALID_ARGUMENT;
        if (!out || out->struct_size < offsetof(siaara_quarantine_record, area) + sizeof(int32_t))
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        auto list = q->list();
        if (!list) return fail(list.error());
        if (index >= list->size()) return fail(Errc::NotFound, "no such quarantine item");
        const st::QuarantineRecord& r = list.value()[index];
        const uint32_t size = out->struct_size;
        std::memset(out, 0, std::min<std::size_t>(size, sizeof *out));
        out->struct_size = size;
        out->read_loss = capi::read_loss_bits(r.read_loss);
        out->time = r.time;
        out->size = r.size;
        put(out->id, r.id);
        put(out->source, r.source);
        put(out->calling_ae, r.calling_ae);
        put(out->peer, r.peer);
        put(out->sop_class_uid, r.sop_class_uid);
        put(out->sop_instance_uid, r.sop_instance_uid);
        put(out->warning, r.warning);
        out->area = area;
        if (size >= offsetof(siaara_quarantine_record, record_missing) + sizeof(int32_t)) out->record_missing = r.record_missing ? 1 : 0;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_bytes(const siaara_store* store, int32_t area,
                                                                   const char* id, uint8_t* buf, size_t cap,
                                                                   size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (needed) *needed = 0;
        st::Quarantine* q = area_of(store, area);
        if (!q) return SIAARA_ERR_INVALID_ARGUMENT;
        if (!id) return fail(Errc::InvalidArgument, "id must not be NULL");
        auto b = q->bytes(id);
        if (!b) return fail(b.error());
        if (needed) *needed = b->size();
        if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
        if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
        if (cap < b->size()) return fail(Errc::BufferTooSmall, "buffer too small");
        if (!b->empty()) std::memcpy(buf, b->data(), b->size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_release(siaara_store* store, int32_t area, const char* id,
                                                                     int32_t policy, int32_t* outcome,
                                                                     char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (needed) *needed = 0;
        if (buf && cap != 0) buf[0] = '\0';
        st::Quarantine* q = area_of(store, area);
        if (!q) return SIAARA_ERR_INVALID_ARGUMENT;
        if (!id) return fail(Errc::InvalidArgument, "id must not be NULL");
        if (policy != SIAARA_DUP_KEEP && policy != SIAARA_DUP_REPLACE) return fail(Errc::InvalidArgument, "unknown policy");
        if (buf ? cap < 65 : cap != 0) return fail(Errc::BufferTooSmall, "buf needs room for 65 characters (or NULL with cap 0)");
        // R-167: one release of an area at a time, across "read, store, remove": a second release
        // of the same item waits and then finds it gone (SIAARA_ERR_NOT_FOUND).
        const auto release = q->release_lock();
        std::string uid;
        {
            auto b = q->bytes(id);  // mapped, not copied; unmapped before the item is removed
            if (!b) return fail(b.error());
            // Only a Part 10 stream is stored unchanged; the store would rewrite anything else.
            if (b->size() < 132 || std::memcmp(b->data() + 128, "DICM", 4) != 0)
                return fail(Errc::Unsupported, "the item has no Part 10 preamble and cannot be stored unchanged");
            st::IndexRecord rec;
            if (st::extract_index(*b, rec, st::network_extract_options())) uid = rec[st::Str::SopInstanceUid];
            auto r = store->store->add_buffer(*b, policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep,
                                              st::kIncompleteRead, st::network_extract_options());
            if (!r) return fail(r.error());
            if (outcome) *outcome = static_cast<int32_t>(*r);
        }
        const std::size_t n = std::min<std::size_t>(uid.size(), 64);
        if (needed) *needed = n;
        if (buf) {
            std::memcpy(buf, uid.data(), n);
            buf[n] = '\0';
        }
        if (Status s = q->remove(id); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_delete(siaara_store* store, int32_t area, const char* id) {
    return guarded([&]() -> siaara_status {
        st::Quarantine* q = area_of(store, area);
        if (!q) return SIAARA_ERR_INVALID_ARGUMENT;
        if (!id) return fail(Errc::InvalidArgument, "id must not be NULL");
        if (Status s = q->remove(id); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_export(const siaara_store* store, int32_t area,
                                                                    const char* id, const char* folder) {
    return guarded([&]() -> siaara_status {
        st::Quarantine* q = area_of(store, area);
        if (!q) return SIAARA_ERR_INVALID_ARGUMENT;
        if (!id || !folder || !folder[0]) return fail(Errc::InvalidArgument, "id and folder must be set");
        auto b = q->bytes(id);
        if (!b) return fail(b.error());
        const std::string path = capi::dimse::join_path(folder, std::string(id) + ".dcm");
        if (Status s = siaara::io::write_file_atomic(path, b->data(), b->size()); !s) return fail(s);
        return ok();
    });
}

}  // extern "C"
