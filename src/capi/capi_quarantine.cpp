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

template <std::size_t N>
void put(char (&dst)[N], const std::string& v) {
    const std::size_t n = std::min(v.size(), N - 1);
    std::memcpy(dst, v.data(), n);
    dst[n] = '\0';
}

}  // namespace

namespace siaara::capi {

std::uint16_t quarantine_bytes(siaara_store* store, const Bytes& bytes, const char* source, const std::string& calling_ae,
                               const std::string& peer, const std::string& sop_class, const std::string& sop_instance,
                               std::uint32_t read_loss, const std::string& warning) {
    if (!store || !store->quarantine) return 0x0110;
    st::QuarantineRecord rec;
    rec.source = source;
    rec.calling_ae = calling_ae;
    rec.peer = peer;
    rec.sop_class_uid = sop_class;
    rec.sop_instance_uid = sop_instance;
    rec.read_loss = read_loss;
    rec.warning = warning;
    auto r = store->quarantine->add(bytes, std::move(rec));
    if (r) return 0;
    log(SIAARA_LOG_WARNING, false, "quarantine: an incomplete instance was not kept: " + r.error().message);
    return r.error().code == Errc::LimitExceeded ? 0xA700 : 0x0110;
}

}  // namespace siaara::capi

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_count(const siaara_store* store, uint32_t* items,
                                                                   uint64_t* bytes) {
    return guarded([&]() -> siaara_status {
        if (!store || !store->quarantine) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (items) *items = static_cast<uint32_t>(std::min<std::size_t>(store->quarantine->items(), 0xFFFFFFFFu));
        if (bytes) *bytes = store->quarantine->total_bytes();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_get(const siaara_store* store, uint32_t index,
                                                                 siaara_quarantine_record* out) {
    return guarded([&]() -> siaara_status {
        if (!store || !store->quarantine) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (!out || out->struct_size < sizeof(siaara_quarantine_record))
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        auto list = store->quarantine->list();
        if (!list) return fail(list.error());
        if (index >= list->size()) return fail(Errc::NotFound, "no such quarantine item");
        const st::QuarantineRecord& r = list.value()[index];
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof *out);
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
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_bytes(const siaara_store* store, const char* id,
                                                                   uint8_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (needed) *needed = 0;
        if (!store || !store->quarantine || !id) return fail(Errc::InvalidArgument, "store and id must not be NULL");
        auto b = store->quarantine->bytes(id);
        if (!b) return fail(b.error());
        if (needed) *needed = b->size();
        if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
        if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
        if (cap < b->size()) return fail(Errc::BufferTooSmall, "buffer too small");
        if (!b->empty()) std::memcpy(buf, b->data(), b->size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_release(siaara_store* store, const char* id, int32_t policy,
                                                                     int32_t* outcome) {
    return guarded([&]() -> siaara_status {
        if (!store || !store->quarantine || !id) return fail(Errc::InvalidArgument, "store and id must not be NULL");
        if (policy != SIAARA_DUP_KEEP && policy != SIAARA_DUP_REPLACE) return fail(Errc::InvalidArgument, "unknown policy");
        auto b = store->quarantine->bytes(id);
        if (!b) return fail(b.error());
        // Only a Part 10 stream is stored unchanged; the store would rewrite anything else.
        if (b->size() < 132 || std::memcmp(b->data() + 128, "DICM", 4) != 0)
            return fail(Errc::Unsupported, "the item has no Part 10 preamble and cannot be stored unchanged");
        auto r = store->store->add_buffer(*b, policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep,
                                          st::kIncompleteRead);
        if (!r) return fail(r.error());
        if (outcome) *outcome = static_cast<int32_t>(*r);
        if (Status s = store->quarantine->remove(id); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_quarantine_delete(siaara_store* store, const char* id) {
    return guarded([&]() -> siaara_status {
        if (!store || !store->quarantine || !id) return fail(Errc::InvalidArgument, "store and id must not be NULL");
        if (Status s = store->quarantine->remove(id); !s) return fail(s);
        return ok();
    });
}

}  // extern "C"
