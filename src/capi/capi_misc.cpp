// SIAARA DICOM Toolkit - C API: data set additions (sequence items, multi-valued strings) and
// diagnostics (include/siaara/siaara.h, "viewer / PACS additions").
#include <cstring>
#include <memory>
#include <span>
#include <new>
#include <string>

#include <algorithm>
#include <cmath>
#include <vector>

#include "capi/capi_internal.hpp"
#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "codec/codec.hpp"
#include "gpu/gpu.hpp"
#include "image/png.hpp"
#include "store/dicomdir.hpp"
#include "volume/volume.hpp"

namespace {

using siaara::Errc;
namespace capi = siaara::capi;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
siaara_status fail(const siaara::Status& s) noexcept { return capi::set_error(s); }
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
        e->items.emplace_back().set_inherited_charset(siaara::charset_in_force(d));  // F15
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
        std::copy(all.begin(), all.end(), buf);  // the values carry their own NUL terminators
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_debug_live_handles(int64_t* out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = capi::live_handle_counter().load(std::memory_order_relaxed);
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- cancellable long calls
namespace {

// A C++ progress function that honours the token and the C callback. Thread-unsafe by itself:
// the C++ callers never call it concurrently.
std::function<bool(std::uint64_t, std::uint64_t)> progress_fn(siaara_cancel* cancel, siaara_progress_callback cb, void* user,
                                                              bool& stopped_by_callback) {
    if (!cancel && !cb) return {};
    return [cancel, cb, user, &stopped_by_callback](std::uint64_t done, std::uint64_t total) {
        if (cancel && cancel->is_set()) return false;
        if (cb && cb(user, done, total) != 0) {
            stopped_by_callback = true;
            return false;
        }
        return true;
    };
}

siaara_status cancel_result(const siaara::Status& s, siaara_cancel* cancel, bool by_callback) {
    if (s.code == Errc::Cancelled || (cancel && cancel->is_set()))
        return fail(Errc::Cancelled, by_callback ? "stopped by the progress callback" : "cancelled");
    return fail(s);
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_volume_build_ex(const char* const* utf8_paths, const size_t* path_lens,
                                                            uint32_t count, const siaara_volume_options* options,
                                                            siaara_cancel* cancel, siaara_progress_callback cb,
                                                            void* user, siaara_volume** out) {
    return guarded([&]() -> siaara_status {
        namespace vol = siaara::volume;
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!utf8_paths || count == 0) return fail(Errc::InvalidArgument, "no input paths");
        if (options && options->struct_size < sizeof(siaara_volume_options))
            return fail(Errc::InvalidArgument, "options->struct_size is too small");
        if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
        vol::VolumeOptions o;
        if (options) {
            switch (options->voxel_type) {
                case SIAARA_VOXEL_AUTO: o.type = vol::VoxelTypeRequest::Auto; break;
                case SIAARA_VOXEL_INT16: o.type = vol::VoxelTypeRequest::Int16; break;
                case SIAARA_VOXEL_FLOAT32: o.type = vol::VoxelTypeRequest::Float32; break;
                default: return fail(Errc::InvalidArgument, "unknown voxel_type");
            }
            o.phase = options->phase;
            o.threads = options->threads;
            o.deshear_tilt = options->deshear_tilt != 0;
            o.strict = options->strict != 0;
            if (options->memory_budget) o.memory_budget = options->memory_budget;
            if (options->spacing_tolerance != 0.0) {
                if (!std::isfinite(options->spacing_tolerance) || options->spacing_tolerance < 0.0)
                    return fail(Errc::InvalidArgument, "bad spacing_tolerance");
                o.spacing_tolerance = options->spacing_tolerance;
            }
        }
        bool by_cb = false;
        o.progress = progress_fn(cancel, cb, user, by_cb);
        std::vector<std::string> paths;
        paths.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!utf8_paths[i]) return fail(Errc::InvalidArgument, "path is NULL");
            paths.emplace_back(utf8_paths[i], path_lens ? path_lens[i] : std::strlen(utf8_paths[i]));
        }
        auto v = vol::build_volume_from_paths(paths, o);
        if (!v) return cancel_result(v.error(), cancel, by_cb);
        if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
        auto h = std::make_unique<siaara_volume>();
        h->vol = std::make_shared<const vol::Volume>(std::move(v).value());
        *out = h.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_verify_ex(siaara_store* store, int32_t deep, int32_t repair,
                                                            siaara_cancel* cancel, siaara_progress_callback cb,
                                                            void* user, siaara_store_verify_report* out) {
    return guarded([&]() -> siaara_status {
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (out && out->struct_size < sizeof(siaara_store_verify_report))
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
        bool by_cb = false;
        auto r = store->store->verify(deep != 0, repair != 0, progress_fn(cancel, cb, user, by_cb));
        if (!r) return cancel_result(r.error(), cancel, by_cb);
        if (out) {
            auto c = [](std::size_t v) { return static_cast<uint32_t>(std::min<std::size_t>(v, UINT32_MAX)); };
            out->checked = c(r->checked);
            out->missing = c(r->missing);
            out->size_mismatch = c(r->size_mismatch);
            out->content_mismatch = c(r->content_mismatch);
            out->orphans = c(r->orphans);
            out->repaired = c(r->repaired);
            out->reserved0 = 0;
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_rebuild_ex(siaara_store* store, siaara_cancel* cancel,
                                                             siaara_progress_callback cb, void* user) {
    return guarded([&]() -> siaara_status {
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
        bool by_cb = false;
        if (siaara::Status s = store->store->rebuild(progress_fn(cancel, cb, user, by_cb)); !s)
            return cancel_result(s, cancel, by_cb);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dicomdir_write_ex(const char* root, size_t root_len, const char* const* files,
                                                              const size_t* lens, size_t count,
                                                              const siaara_dicomdir_write_options* options,
                                                              siaara_cancel* cancel, siaara_progress_callback cb,
                                                              void* user, uint32_t* warnings) {
    return guarded([&]() -> siaara_status {
        namespace st = siaara::store;
        if (!root || (count && (!files || !lens))) return fail(Errc::InvalidArgument, "root, files and lens must be set");
        if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
        st::DicomdirWriteOptions o;
        if (options) {
            if (options->struct_size < sizeof(siaara_dicomdir_write_options))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            if (options->profile != SIAARA_PROFILE_GENERAL_CD && options->profile != SIAARA_PROFILE_GENERAL_USB)
                return fail(Errc::InvalidArgument, "unknown profile");
            o.profile = options->profile == SIAARA_PROFILE_GENERAL_USB ? st::MediaProfile::GeneralUsb : st::MediaProfile::GeneralCd;
            o.overwrite = options->overwrite != 0;
            o.allow_any_transfer_syntax = options->allow_any_transfer_syntax != 0;
            o.file_set_id = options->file_set_id ? std::string(options->file_set_id) : std::string();
        }
        bool by_cb = false;
        o.progress = progress_fn(cancel, cb, user, by_cb);
        std::vector<std::string> paths;
        paths.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!files[i]) return fail(Errc::InvalidArgument, "file path is NULL");
            paths.emplace_back(files[i], lens[i]);
        }
        auto r = st::write_dicomdir(std::string(root, root_len), paths, o);
        if (!r) return cancel_result(r.error(), cancel, by_cb);
        if (warnings) *warnings = static_cast<uint32_t>(r->warnings.size());
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- image encoders, GPU choice
struct siaara_buffer {
    std::vector<std::uint8_t> bytes;
    siaara::capi::LiveHandle live_{};
};

namespace {

siaara_status check_pixels(const uint8_t* pixels, size_t size, uint32_t width, uint32_t height, int32_t format,
                           unsigned& channels) {
    if (format != SIAARA_PIXELS_RGBA8 && format != SIAARA_PIXELS_GRAY8) return fail(Errc::InvalidArgument, "unknown format");
    channels = format == SIAARA_PIXELS_RGBA8 ? 4u : 1u;
    if (width == 0 || height == 0 || width > 65535 || height > 65535)
        return fail(Errc::InvalidArgument, "width and height must be 1..65535");
    if (!pixels || size < std::uint64_t{width} * height * channels) return fail(Errc::InvalidArgument, "pixel buffer too short");
    return SIAARA_OK;
}

siaara_status buffer_out(std::vector<std::uint8_t>&& bytes, siaara_buffer** out) {
    auto b = std::make_unique<siaara_buffer>();
    b->bytes = std::move(bytes);
    *out = b.release();
    return ok();
}

}  // namespace

extern "C" {

SIAARA_API void SIAARA_CALL siaara_buffer_free(siaara_buffer* buffer) SIAARA_NOEXCEPT { delete buffer; }

SIAARA_API siaara_status SIAARA_CALL siaara_buffer_data(const siaara_buffer* buffer, const uint8_t** out_data,
                                                        uint64_t* out_size) {
    return guarded([&]() -> siaara_status {
        if (!buffer || !out_data) return fail(Errc::InvalidArgument, "buffer and out_data must not be NULL");
        *out_data = buffer->bytes.data();
        if (out_size) *out_size = buffer->bytes.size();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_encode_jpeg(const uint8_t* pixels, size_t size, uint32_t width, uint32_t height,
                                                        int32_t format, int32_t quality, siaara_buffer** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        unsigned ch = 0;
        if (siaara_status s = check_pixels(pixels, size, width, height, format, ch); s != SIAARA_OK) return s;
        if (quality < 0 || quality > 100) return fail(Errc::InvalidArgument, "quality must be 0..100");
        namespace cd = siaara::codec;
        const std::size_t n = std::size_t{width} * height;
        std::vector<std::uint8_t> in;
        if (ch == 4) {
            in.resize(n * 3);
            for (std::size_t i = 0; i < n; ++i) {
                in[3 * i] = pixels[4 * i];
                in[3 * i + 1] = pixels[4 * i + 1];
                in[3 * i + 2] = pixels[4 * i + 2];
            }
        } else {
            in.assign(pixels, pixels + n);
        }
        cd::FrameInfo fi;
        fi.rows = height;
        fi.columns = width;
        fi.samples_per_pixel = static_cast<std::uint16_t>(ch == 4 ? 3 : 1);
        fi.bits_allocated = 8;
        fi.bits_stored = 8;
        cd::EncodeParams p;
        p.lossless = false;
        p.quality = quality ? quality : 90;
        p.ybr = ch == 4;
        std::vector<std::uint8_t> enc;
        if (siaara::Status s = cd::encode_jpeg(in, fi, p, enc); !s) return fail(s);
        // A JFIF APP0 segment after SOI (JFIF 1.02: 1:1 aspect, no thumbnail) so that viewers
        // outside DICOM read the components as YCbCr.
        if (enc.size() >= 4 && enc[0] == 0xFF && enc[1] == 0xD8 && !(enc[2] == 0xFF && enc[3] == 0xE0)) {
            static const std::uint8_t app0[18] = {0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
                                                  0x01, 0x02, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
            enc.insert(enc.begin() + 2, app0, app0 + 18);
        }
        return buffer_out(std::move(enc), out);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_encode_png(const uint8_t* pixels, size_t size, uint32_t width, uint32_t height,
                                                       int32_t format, int32_t level, siaara_buffer** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        unsigned ch = 0;
        if (siaara_status s = check_pixels(pixels, size, width, height, format, ch); s != SIAARA_OK) return s;
        if (level < -1 || level > 9) return fail(Errc::InvalidArgument, "level must be -1..9");
        std::vector<std::uint8_t> png;
        if (siaara::Status s = siaara::image::encode_png(std::span<const std::uint8_t>(pixels, size), width, height, ch,
                                                         level < 0 ? 6 : level, png);
            !s)
            return fail(s);
        return buffer_out(std::move(png), out);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_gpu_select(uint32_t index) {
    return guarded([&]() -> siaara_status {
        if (siaara::Status s = siaara::gpu::select_device(index); !s) return fail(s);
        return ok();
    });
}

}  // extern "C"
