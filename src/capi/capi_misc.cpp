// SIAARA DICOM Toolkit - C API: data set additions (sequence items, multi-valued strings) and
// diagnostics (include/siaara/siaara.h, "viewer / PACS additions").
#include <cstring>
#include <new>
#include <string>

#include "capi/capi_internal.hpp"
#include "charset/charset.hpp"
#include "core/dataset.hpp"

namespace {

using siaara::Errc;
namespace capi = siaara::capi;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
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

siaara::Dataset* ds_cast(siaara_dataset* p) { return reinterpret_cast<siaara::Dataset*>(p); }
const siaara::Dataset* ds_cast(const siaara_dataset* p) { return reinterpret_cast<const siaara::Dataset*>(p); }

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_append_item(siaara_dataset* ds, uint32_t tag, siaara_dataset** out,
                                                                uint32_t* index) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        *out = nullptr;
        if (siaara::tag_group(tag) == 0xFFFE || tag == siaara::tags::PixelData)
            return fail(Errc::InvalidArgument, "tag cannot be a sequence");
        siaara::Dataset& d = *ds_cast(ds);
        siaara::Element* e = d.find(tag);
        if (!e) {
            siaara::Element ne;
            ne.tag = tag;
            ne.vr = siaara::VR::SQ;
            e = &d.put(std::move(ne));
        } else if (!e->is_sequence()) {
            return fail(Errc::TypeMismatch, "element is not a sequence");
        }
        if (e->items.size() >= 0x7FFFFFFFu) return fail(Errc::LimitExceeded, "too many items");
        e->items.emplace_back();
        if (index) *index = static_cast<uint32_t>(e->items.size() - 1);
        *out = reinterpret_cast<siaara_dataset*>(&e->items.back());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_remove_item(siaara_dataset* ds, uint32_t tag, uint32_t index) {
    return guarded([&]() -> siaara_status {
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        siaara::Element* e = ds_cast(ds)->find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        if (!e->is_sequence()) return fail(Errc::TypeMismatch, "element is not a sequence");
        if (index >= e->items.size()) return fail(Errc::NotFound, "item index out of range");
        e->items.erase(e->items.begin() + static_cast<std::ptrdiff_t>(index));
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_strings(const siaara_dataset* ds, uint32_t tag, int32_t utf8,
                                                                char* buf, size_t cap, size_t* needed, uint32_t* count) {
    return guarded([&]() -> siaara_status {
        if (count) *count = 0;
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        const siaara::Dataset& d = *ds_cast(ds);
        const siaara::Element* e = d.find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        if (e->is_sequence() || e->is_encapsulated()) return fail(Errc::TypeMismatch, "element has no string values");
        const std::size_t vm = d.value_multiplicity(tag);
        std::string all;
        for (std::size_t k = 0; k < vm; ++k) {
            std::optional<std::string> v = utf8 ? siaara::charset::get_utf8(d, tag, k) : d.get_string(tag, k);
            if (!v) return fail(Errc::TypeMismatch, "value cannot be read as a string");
            all += *v;
            all.push_back('\0');
        }
        if (count) *count = static_cast<uint32_t>(vm);
        if (needed) *needed = all.size();
        if (!buf && cap == 0) return ok();
        if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
        if (cap < all.size()) return fail(Errc::BufferTooSmall, "buffer too small");
        std::memcpy(buf, all.data(), all.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_debug_live_handles(int64_t* out) {
    if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
    *out = capi::live_handle_counter().load(std::memory_order_relaxed);
    return ok();
}

}  // extern "C"
