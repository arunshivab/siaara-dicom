// SIAARA DICOM Toolkit - C API implementation (include/siaara/siaara.h).
#include "siaara/siaara.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include <cstring>
#include <new>
#include <string>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "image/frames.hpp"
#include "image/render.hpp"

struct siaara_file {
    siaara::DicomFile file;
};

namespace {

using siaara::Errc;
using siaara::Status;

thread_local std::string g_last_error;

siaara_status fail(Errc code, std::string msg) {
    g_last_error = std::move(msg);
    return static_cast<siaara_status>(code);
}
siaara_status fail(const Status& s) {
    g_last_error = s.message;
    if (s.offset) g_last_error += " (offset " + std::to_string(s.offset) + ")";
    return static_cast<siaara_status>(s.code);
}
siaara_status ok() {
    g_last_error.clear();
    return SIAARA_OK;
}

siaara::Dataset* ds_cast(siaara_dataset* p) { return reinterpret_cast<siaara::Dataset*>(p); }
const siaara::Dataset* ds_cast(const siaara_dataset* p) {
    return reinterpret_cast<const siaara::Dataset*>(p);
}
siaara_dataset* ds_out(siaara::Dataset* p) { return reinterpret_cast<siaara_dataset*>(p); }

// Every exported body runs inside this guard: no exception may cross the C boundary (CAPI-03).
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

siaara::ReadOptions to_options(const siaara_read_options* o) {
    siaara::ReadOptions r;
    if (!o || o->struct_size < sizeof(uint32_t) * 5) return r;
    r.stop_before_pixel_data = o->stop_before_pixel_data != 0;
    r.allow_raw_dataset = o->allow_raw_dataset != 0;
    r.lenient = o->strict == 0;
    if (o->max_depth) r.max_depth = o->max_depth;
    return r;
}

bool parse_vr(const char* vr, siaara::VR& out) {
    if (!vr || !vr[0] || !vr[1]) return false;
    out = siaara::vr_from_chars(vr[0], vr[1]);
    return siaara::vr_is_known(out);
}

void fill_entry(const siaara::dict::Entry& e, siaara_dict_entry* out) {
    out->tag = e.tag;
    const auto a = siaara::vr_name(e.vr);
    const auto b = siaara::vr_name(e.vr_alt);
    out->vr[0] = e.vr == siaara::VR::None ? 0 : a[0];
    out->vr[1] = e.vr == siaara::VR::None ? 0 : a[1];
    out->vr[2] = 0;
    out->vr_alt[0] = e.vr_alt == siaara::VR::None ? 0 : b[0];
    out->vr_alt[1] = e.vr_alt == siaara::VR::None ? 0 : b[1];
    out->vr_alt[2] = 0;
    out->retired = e.retired ? 1 : 0;
    out->keyword = e.keyword.data();  // generated literals are NUL-terminated
    out->name = e.name.data();
    out->vm = e.vm.data();
}

}  // namespace

extern "C" {

SIAARA_API uint32_t SIAARA_CALL siaara_abi_version(void) { return SIAARA_ABI_VERSION; }
SIAARA_API const char* SIAARA_CALL siaara_version_string(void) { return "0.1.0"; }
SIAARA_API const char* SIAARA_CALL siaara_last_error_message(void) { return g_last_error.c_str(); }

SIAARA_API siaara_status SIAARA_CALL siaara_file_open(const char* path, size_t path_len,
                                                      const siaara_read_options* options,
                                                      siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!path || !out) return fail(Errc::InvalidArgument, "path and out must not be NULL");
        *out = nullptr;
        auto r = siaara::read_file(std::string(path, path_len), to_options(options));
        if (!r) return fail(r.error());
        *out = new siaara_file{std::move(r).value()};
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_open_memory(const uint8_t* data, size_t size,
                                                             const siaara_read_options* options,
                                                             siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if ((!data && size) || !out) return fail(Errc::InvalidArgument, "data and out must not be NULL");
        *out = nullptr;
        auto r = siaara::read_part10(siaara::Bytes::copy_of(data, size), to_options(options));
        if (!r) return fail(r.error());
        *out = new siaara_file{std::move(r).value()};
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_file_free(siaara_file* file) { delete file; }

SIAARA_API siaara_status SIAARA_CALL siaara_file_create(siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = new siaara_file{};
        (*out)->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_dataset(siaara_file* file, siaara_dataset** out) {
    if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
    *out = ds_out(&file->file.dataset);
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_meta(siaara_file* file, siaara_dataset** out) {
    if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
    *out = ds_out(&file->file.meta);
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_transfer_syntax(const siaara_file* file, char* buf,
                                                                 size_t cap, size_t* needed) {
    if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
    return copy_out(file->file.transfer_syntax, buf, cap, needed);
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_warning_count(const siaara_file* file, uint32_t* out) {
    if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
    *out = static_cast<uint32_t>(file->file.warnings.size());
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_save(const siaara_file* file, const char* path,
                                                      size_t path_len, const char* ts_uid) {
    return guarded([&]() -> siaara_status {
        if (!file || !path) return fail(Errc::InvalidArgument, "file and path must not be NULL");
        if (ts_uid && !siaara::find_transfer_syntax(ts_uid))
            return fail(Errc::Unsupported, "unknown transfer syntax");
        siaara::DicomFile copy = file->file;
        if (ts_uid) copy.transfer_syntax = ts_uid;
        // Native-only for now: refuse to relabel encapsulated data as native or vice versa.
        const auto* from = siaara::find_transfer_syntax(file->file.transfer_syntax);
        const auto* to = siaara::find_transfer_syntax(copy.transfer_syntax);
        if (from && to && from->encapsulated != to->encapsulated)
            return fail(Errc::Unsupported, "transcoding pixel data is not available yet");
        if (Status s = siaara::write_file(copy, std::string(path, path_len)); !s) return fail(s);
        return ok();
    });
}

SIAARA_API int32_t SIAARA_CALL siaara_dataset_contains(const siaara_dataset* ds, uint32_t tag) {
    return ds && ds_cast(ds)->contains(tag) ? 1 : 0;
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vr(const siaara_dataset* ds, uint32_t tag, char out_vr[3]) {
    if (!ds || !out_vr) return fail(Errc::InvalidArgument, "ds and out_vr must not be NULL");
    const auto* e = ds_cast(ds)->find(tag);
    if (!e) return fail(Errc::NotFound, "element not present");
    const auto n = siaara::vr_name(e->vr);
    out_vr[0] = n[0];
    out_vr[1] = n[1];
    out_vr[2] = 0;
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vm(const siaara_dataset* ds, uint32_t tag, uint32_t* out) {
    if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
    if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
    *out = static_cast<uint32_t>(ds_cast(ds)->value_multiplicity(tag));
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_string(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm, char* buf, size_t cap,
                                                               size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
        auto v = ds_cast(ds)->get_string(tag, vm);
        if (!v) return fail(Errc::TypeMismatch, "value cannot be read as a string at this index");
        return copy_out(*v, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_int64(const siaara_dataset* ds, uint32_t tag,
                                                              uint32_t vm, int64_t* out) {
    if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
    if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
    auto v = ds_cast(ds)->get_int(tag, vm);
    if (!v) return fail(Errc::TypeMismatch, "value cannot be read as an integer at this index");
    *out = *v;
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_double(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm, double* out) {
    if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
    if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
    auto v = ds_cast(ds)->get_double(tag, vm);
    if (!v) return fail(Errc::TypeMismatch, "value cannot be read as a number at this index");
    *out = *v;
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_raw(const siaara_dataset* ds, uint32_t tag,
                                                            const uint8_t** out_data, size_t* out_size) {
    if (!ds || !out_data || !out_size) return fail(Errc::InvalidArgument, "arguments must not be NULL");
    const auto* e = ds_cast(ds)->find(tag);
    if (!e) return fail(Errc::NotFound, "element not present");
    if (e->is_sequence() || e->is_encapsulated())
        return fail(Errc::TypeMismatch, "element has no single value (sequence or encapsulated)");
    *out_data = e->value.data();
    *out_size = e->value.size();
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item_count(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t* out) {
    if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
    const auto* e = ds_cast(ds)->find(tag);
    if (!e) return fail(Errc::NotFound, "element not present");
    if (!e->is_sequence()) return fail(Errc::TypeMismatch, "element is not a sequence");
    *out = static_cast<uint32_t>(e->items.size());
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item(siaara_dataset* ds, uint32_t tag, uint32_t index,
                                                         siaara_dataset** out) {
    if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
    auto* e = ds_cast(ds)->find(tag);
    if (!e) return fail(Errc::NotFound, "element not present");
    if (!e->is_sequence()) return fail(Errc::TypeMismatch, "element is not a sequence");
    if (index >= e->items.size()) return fail(Errc::NotFound, "item index out of range");
    *out = ds_out(&e->items[index]);
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_element_count(const siaara_dataset* ds, uint32_t* out) {
    if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
    *out = static_cast<uint32_t>(ds_cast(ds)->size());
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_element_tag(const siaara_dataset* ds, uint32_t index,
                                                                uint32_t* out_tag) {
    if (!ds || !out_tag) return fail(Errc::InvalidArgument, "ds and out_tag must not be NULL");
    const auto& els = ds_cast(ds)->elements();
    if (index >= els.size()) return fail(Errc::NotFound, "index out of range");
    *out_tag = els[index].tag;
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_string(siaara_dataset* ds, uint32_t tag, const char* vr,
                                                               const char* utf8, size_t len) {
    return guarded([&]() -> siaara_status {
        siaara::VR v{};
        if (!ds || (!utf8 && len) || !parse_vr(vr, v)) return fail(Errc::InvalidArgument, "bad argument");
        if (Status s = ds_cast(ds)->set_string(tag, v, std::string_view(utf8 ? utf8 : "", len)); !s)
            return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_uint16(siaara_dataset* ds, uint32_t tag, uint16_t v) {
    return guarded([&]() -> siaara_status {
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        (void)ds_cast(ds)->set_uint16(tag, v);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_uint32(siaara_dataset* ds, uint32_t tag, uint32_t v) {
    return guarded([&]() -> siaara_status {
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        (void)ds_cast(ds)->set_uint32(tag, v);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_bytes(siaara_dataset* ds, uint32_t tag, const char* vr,
                                                              const uint8_t* data, size_t size) {
    return guarded([&]() -> siaara_status {
        siaara::VR v{};
        if (!ds || (!data && size) || !parse_vr(vr, v)) return fail(Errc::InvalidArgument, "bad argument");
        (void)ds_cast(ds)->set_bytes(tag, v, siaara::Bytes::copy_of(data, size));
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_remove(siaara_dataset* ds, uint32_t tag) {
    if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
    if (!ds_cast(ds)->remove(tag)) return fail(Errc::NotFound, "element not present");
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup(uint32_t tag, siaara_dict_entry* out) {
    if (!out || out->struct_size < sizeof(siaara_dict_entry))
        return fail(Errc::InvalidArgument, "out must be set up with struct_size");
    const auto* e = siaara::dict::lookup(tag);
    if (!e) return fail(Errc::NotFound, "tag not in dictionary");
    fill_entry(*e, out);
    out->tag = tag;
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_keyword(const char* keyword, size_t len,
                                                                siaara_dict_entry* out) {
    if (!keyword || !out || out->struct_size < sizeof(siaara_dict_entry))
        return fail(Errc::InvalidArgument, "bad argument");
    const auto* e = siaara::dict::lookup_keyword(std::string_view(keyword, len));
    if (!e) return fail(Errc::NotFound, "keyword not in dictionary");
    fill_entry(*e, out);
    return ok();
}

}  // extern "C"

// ---------------------------------------------------------------- pixel data
namespace {

// Size query / buffer check shared by the frame and render calls. Returns SIAARA_OK with
// query == false when the caller should go on and fill the buffer; otherwise the status to return.
siaara_status check_out_buffer(std::uint64_t need, const uint8_t* buf, size_t cap, size_t* needed, bool& query) {
    query = false;
    if (need > SIZE_MAX) return fail(Errc::LimitExceeded, "output does not fit in memory on this platform");
    if (needed) *needed = static_cast<size_t>(need);
    if (!buf && cap == 0) {
        query = true;
        return ok();
    }
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (cap < need) return fail(Errc::BufferTooSmall, "buffer too small");
    return SIAARA_OK;
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_file_pixel_info(const siaara_file* file, siaara_pixel_info* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out || out->struct_size < sizeof(siaara_pixel_info))
            return fail(Errc::InvalidArgument, "file and out (with struct_size) must be set");
        auto fr = siaara::image::FrameReader::open(file->file.dataset, file->file.transfer_syntax);
        if (!fr) return fail(fr.error());
        const auto& pi = fr->info();
        const auto* ts = siaara::find_transfer_syntax(file->file.transfer_syntax);
        out->rows = pi.rows;
        out->columns = pi.columns;
        out->samples_per_pixel = pi.samples_per_pixel;
        out->bits_allocated = pi.bits_allocated;
        out->bits_stored = pi.bits_stored;
        out->high_bit = pi.high_bit;
        out->pixel_representation = pi.is_signed ? 1 : 0;
        out->planar_configuration = pi.planar ? 1 : 0;
        out->number_of_frames = pi.number_of_frames;
        out->lossy = (pi.lossy_attribute || (ts && ts->lossy && fr->encapsulated())) ? 1 : 0;
        out->sample_kind = pi.kind == siaara::image::SampleKind::Integer   ? SIAARA_SAMPLES_INTEGER
                           : pi.kind == siaara::image::SampleKind::Float32 ? SIAARA_SAMPLES_FLOAT32
                                                                           : SIAARA_SAMPLES_FLOAT64;
        out->encapsulated = fr->encapsulated() ? 1 : 0;
        out->frame_bytes = fr->frame_bytes();
        const size_t n = std::min(pi.photometric_text.size(), sizeof(out->photometric) - 1);
        std::memcpy(out->photometric, pi.photometric_text.data(), n);
        out->photometric[n] = '\0';
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_count(const siaara_file* file, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        auto pi = siaara::image::read_pixel_info(file->file.dataset);
        if (!pi) return fail(pi.error());
        *out = pi->number_of_frames;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_decode(const siaara_file* file, uint32_t frame, int32_t format,
                                                         uint8_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
        if (format != SIAARA_FRAME_NATIVE && format != SIAARA_FRAME_FLOAT32_MODALITY)
            return fail(Errc::InvalidArgument, "unknown frame format");
        auto img = siaara::image::Image::open(file->file.dataset, file->file.transfer_syntax);
        if (!img) return fail(img.error());
        const auto& pi = img->info();
        if (frame >= pi.number_of_frames) return fail(Errc::InvalidArgument, "frame index out of range");
        std::uint64_t need = img->frames().frame_bytes();
        if (format == SIAARA_FRAME_FLOAT32_MODALITY) need = pi.pixels() * pi.samples_per_pixel * 4u;
        bool query = false;
        if (siaara_status st = check_out_buffer(need, buf, cap, needed, query); st != SIAARA_OK || query) return st;
        const auto n = static_cast<size_t>(need);
        if (format == SIAARA_FRAME_NATIVE) {
            if (Status s = img->frames().decode(frame, std::span<std::uint8_t>(buf, n)); !s) return fail(s);
            return ok();
        }
        std::vector<float> tmp(n / 4);
        if (Status s = siaara::image::decode_modality_f32(*img, frame, tmp); !s) return fail(s);
        std::memcpy(buf, tmp.data(), n);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_render_rgba8(const siaara_file* file, const siaara_render_options* options,
                                                         uint8_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
        siaara::image::RenderParams p;
        if (options) {
            if (options->struct_size < sizeof(siaara_render_options))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            p.frame = options->frame;
            switch (options->voi_mode) {
                case SIAARA_VOI_FILE: p.voi = siaara::image::VoiMode::File; break;
                case SIAARA_VOI_EXPLICIT: p.voi = siaara::image::VoiMode::Explicit; break;
                case SIAARA_VOI_FULL_RANGE: p.voi = siaara::image::VoiMode::FullRange; break;
                default: return fail(Errc::InvalidArgument, "unknown voi_mode");
            }
            p.window_center = options->window_center;
            p.window_width = options->window_width;
            p.invert = options->invert != 0;
            p.overlays = options->no_overlays == 0;
            p.shutters = options->no_shutters == 0;
        }
        auto img = siaara::image::Image::open(file->file.dataset, file->file.transfer_syntax);
        if (!img) return fail(img.error());
        if (p.frame >= img->info().number_of_frames) return fail(Errc::InvalidArgument, "frame index out of range");
        bool query = false;
        if (siaara_status st = check_out_buffer(img->info().pixels() * 4u, buf, cap, needed, query); st != SIAARA_OK || query)
            return st;
        const auto n = static_cast<size_t>(img->info().pixels() * 4u);
        if (Status s = siaara::image::render_2d(*img, p, std::span<std::uint8_t>(buf, n)); !s) return fail(s);
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- character sets (appended)
#include <optional>
#include <string_view>

#include "charset/charset.hpp"

namespace {

siaara_status get_utf8_impl(const siaara_dataset* ds, uint32_t tag, uint32_t vm, const char* inherited,
                            size_t inherited_len, char* buf, size_t cap, size_t* needed) {
    if (!ds || (!inherited && inherited_len)) return fail(Errc::InvalidArgument, "bad argument");
    if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
    std::optional<siaara::charset::CharacterSet> parent;
    if (inherited) parent = siaara::charset::CharacterSet::parse(std::string_view(inherited, inherited_len));
    auto v = siaara::charset::get_utf8(*ds_cast(ds), tag, vm, parent ? &*parent : nullptr);
    if (!v) return fail(Errc::TypeMismatch, "value cannot be read as text at this index");
    return copy_out(*v, buf, cap, needed);
}

siaara_status set_utf8_impl(siaara_dataset* ds, uint32_t tag, const char* vr, const char* utf8, size_t len,
                            const char* inherited, size_t inherited_len) {
    siaara::VR v{};
    if (!ds || (!utf8 && len) || (!inherited && inherited_len) || !parse_vr(vr, v))
        return fail(Errc::InvalidArgument, "bad argument");
    std::optional<siaara::charset::CharacterSet> parent;
    if (inherited) parent = siaara::charset::CharacterSet::parse(std::string_view(inherited, inherited_len));
    if (Status s = siaara::charset::set_utf8(*ds_cast(ds), tag, v, std::string_view(utf8 ? utf8 : "", len),
                                             parent ? &*parent : nullptr);
        !s)
        return fail(s);
    return ok();
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_utf8(const siaara_dataset* ds, uint32_t tag,
                                                             uint32_t vm, char* buf, size_t cap,
                                                             size_t* needed) {
    return guarded([&]() -> siaara_status { return get_utf8_impl(ds, tag, vm, nullptr, 0, buf, cap, needed); });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_utf8(siaara_dataset* ds, uint32_t tag, const char* vr,
                                                             const char* utf8, size_t len) {
    return guarded([&]() -> siaara_status { return set_utf8_impl(ds, tag, vr, utf8, len, nullptr, 0); });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_utf8_cs(const siaara_dataset* ds, uint32_t tag,
                                                                uint32_t vm, const char* inherited,
                                                                size_t inherited_len, char* buf,
                                                                size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        return get_utf8_impl(ds, tag, vm, inherited, inherited_len, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_utf8_cs(siaara_dataset* ds, uint32_t tag,
                                                                const char* vr, const char* utf8,
                                                                size_t len, const char* inherited,
                                                                size_t inherited_len) {
    return guarded([&]() -> siaara_status {
        return set_utf8_impl(ds, tag, vr, utf8, len, inherited, inherited_len);
    });
}

}  // extern "C"

// ---------------------------------------------------------------- DICOM JSON and DICOMweb client
#include <optional>

#include "web/client.hpp"
#include "web/dicom_json.hpp"

struct siaara_web_client {
    std::optional<siaara::web::DicomwebClient> client;
    std::string authorization;
    std::string last_response;
};

namespace {

bool optional_uid(const char* s, std::string& out) {
    out.clear();
    if (!s) return true;
    out = s;
    return out.empty() || siaara::web::is_uid(out);
}

siaara_status http_fail(const siaara::Status& s) {
    return fail(s);
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_to_json(const siaara_dataset* ds, uint32_t flags, char* buf,
                                                            size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        if (flags & ~static_cast<uint32_t>(SIAARA_JSON_NUMBERS_AS_STRINGS | SIAARA_JSON_STRICT_CHARSET))
            return fail(Errc::InvalidArgument, "unknown flags");
        siaara::web::ModelWriteOptions opt;
        opt.numbers_as_strings = (flags & SIAARA_JSON_NUMBERS_AS_STRINGS) != 0;
        opt.strict_charset = (flags & SIAARA_JSON_STRICT_CHARSET) != 0;
        std::string out;
        if (Status s = siaara::web::dataset_to_json(*ds_cast(ds), out, opt); !s) return fail(s);
        return copy_out(out, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_from_json(const char* utf8, size_t len, siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if ((!utf8 && len) || !out) return fail(Errc::InvalidArgument, "utf8 and out must not be NULL");
        *out = nullptr;
        auto r = siaara::web::dataset_from_json(std::string_view(utf8 ? utf8 : "", len));
        if (!r) return fail(r.error());
        auto f = std::make_unique<siaara_file>();
        for (auto& e : r->elements()) {
            if (siaara::tag_group(e.tag) == 0x0002) f->file.meta.put(std::move(e));
            else f->file.dataset.append_sorted(std::move(e));
        }
        f->file.transfer_syntax = f->file.meta.get_string(siaara::tags::TransferSyntaxUID)
                                      .value_or(std::string(siaara::ts::ExplicitVRLittleEndian));
        if (!siaara::find_transfer_syntax(f->file.transfer_syntax))
            return fail(Errc::Unsupported, "unknown transfer syntax in the JSON file meta");
        *out = f.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_client_create(const char* base_url, size_t len, int32_t timeout_ms,
                                                              siaara_web_client** out) {
    return guarded([&]() -> siaara_status {
        if (!base_url || !out) return fail(Errc::InvalidArgument, "base_url and out must not be NULL");
        *out = nullptr;
        if (timeout_ms < 0) return fail(Errc::InvalidArgument, "timeout_ms must not be negative");
        auto c = std::make_unique<siaara_web_client>();
        siaara::web::DicomwebClientConfig cfg;
        cfg.base_url.assign(base_url, len);
        const int t = timeout_ms == 0 ? 30000 : timeout_ms;
        cfg.limits.read_timeout_ms = t;
        cfg.limits.write_timeout_ms = t;
        cfg.limits.header_timeout_ms = t;
        cfg.connect_timeout_ms = t;
        siaara_web_client* raw = c.get();
        cfg.authorization = [raw]() { return raw->authorization; };
        c->client.emplace(std::move(cfg));
        if (Status s = c->client->status(); !s) return fail(s);
        *out = c.release();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_web_client_free(siaara_web_client* client) { delete client; }

SIAARA_API siaara_status SIAARA_CALL siaara_web_client_set_authorization(siaara_web_client* client, const char* value,
                                                                         size_t len) {
    return guarded([&]() -> siaara_status {
        if (!client) return fail(Errc::InvalidArgument, "client must not be NULL");
        if (!value || len == 0) {
            client->authorization.clear();
            return ok();
        }
        std::string v(value, len);
        if (v.find_first_of("\r\n", 0) != std::string::npos || v.find('\0') != std::string::npos)
            return fail(Errc::InvalidArgument, "authorization value contains a line break or NUL");
        client->authorization = std::move(v);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_qido(siaara_web_client* client, int32_t level, const char* study_uid,
                                                     const char* series_uid, const char* query, size_t query_len,
                                                     char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!client || (!query && query_len)) return fail(Errc::InvalidArgument, "bad argument");
        std::string study, series;
        if (!optional_uid(study_uid, study) || !optional_uid(series_uid, series))
            return fail(Errc::InvalidArgument, "invalid UID");
        std::string path;
        if (!study.empty()) path = "/studies/" + study;
        switch (level) {
            case SIAARA_QIDO_STUDIES:
                if (!study.empty() || !series.empty()) return fail(Errc::InvalidArgument, "study search takes no UIDs");
                path = "/studies";
                break;
            case SIAARA_QIDO_SERIES:
                if (!series.empty()) return fail(Errc::InvalidArgument, "series search takes no series UID");
                path += "/series";
                break;
            case SIAARA_QIDO_INSTANCES:
                if (!series.empty()) {
                    if (study.empty()) return fail(Errc::InvalidArgument, "a series needs its study");
                    path += "/series/" + series;
                }
                path += "/instances";
                break;
            default: return fail(Errc::InvalidArgument, "unknown level");
        }
        const std::string q(query ? query : "", query_len);
        for (char c : q)
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F || c == '#')
                return fail(Errc::InvalidArgument, "query must be percent-encoded");
        if (!q.empty()) path += "?" + q;
        auto rs = client->client->request("GET", path, {{"Accept", "application/dicom+json"}});
        if (!rs) return http_fail(rs.error());
        if (rs->status == 404) return fail(Errc::NotFound, "HTTP 404 Not Found");
        if (rs->status < 200 || rs->status >= 300) return fail(Errc::Rejected, "HTTP " + std::to_string(rs->status));
        client->last_response = rs->status == 204 || rs->body.empty() ? std::string("[]") : std::move(rs->body);
        return copy_out(client->last_response, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_last_response(const siaara_web_client* client, char* buf, size_t cap,
                                                              size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!client) return fail(Errc::InvalidArgument, "client must not be NULL");
        return copy_out(client->last_response, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_instance(siaara_web_client* client, const char* study_uid,
                                                              const char* series_uid, const char* instance_uid,
                                                              siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!client || !out) return fail(Errc::InvalidArgument, "client and out must not be NULL");
        *out = nullptr;
        std::string st, se, in;
        if (!optional_uid(study_uid, st) || !optional_uid(series_uid, se) || !optional_uid(instance_uid, in) ||
            st.empty() || se.empty() || in.empty())
            return fail(Errc::InvalidArgument, "study, series and instance UIDs are required");
        auto r = client->client->retrieve(st, se, in, {"*"});
        if (!r) return fail(r.error());
        if (r->size() != 1) return fail(Errc::Malformed, "server returned " + std::to_string(r->size()) + " instances");
        *out = new siaara_file{std::move(r->front())};
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_stow(siaara_web_client* client, const siaara_file* file,
                                                     const char* study_uid) {
    return guarded([&]() -> siaara_status {
        if (!client || !file) return fail(Errc::InvalidArgument, "client and file must not be NULL");
        std::string study;
        if (!optional_uid(study_uid, study)) return fail(Errc::InvalidArgument, "invalid study UID");
        std::vector<std::uint8_t> bytes;
        if (Status s = siaara::write_part10(file->file, bytes); !s) return fail(s);
        std::vector<std::string> parts(1, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto r = client->client->store(parts, study);
        if (!r) return fail(r.error());
        if (r->http_status == 200 && r->failed.empty()) return ok();
        std::string why = "HTTP " + std::to_string(r->http_status);
        if (!r->failed.empty()) why += ", failure reason " + std::to_string(r->failed.front().second);
        return fail(Errc::Rejected, why);
    });
}

}  // extern "C"

// ---------------------------------------------------------------- index records, store, scan, DICOMDIR (appended)
#include <cstddef>
#include <memory>

#include "store/dicomdir.hpp"
#include "store/index.hpp"
#include "store/scan.hpp"
#include "store/store.hpp"

static_assert(sizeof(siaara_index_record) == 432, "siaara_index_record layout");
static_assert(offsetof(siaara_index_record, file_size) == 56, "siaara_index_record layout");
static_assert(offsetof(siaara_index_record, str_offset) == 176, "siaara_index_record layout");
static_assert(static_cast<std::size_t>(SIAARA_IDX_COUNT) == siaara::store::kStrCount, "string ids");
static_assert(SIAARA_INDEX_MAX_STRINGS >= SIAARA_IDX_COUNT, "string ids");
static_assert(sizeof(siaara_store_stats) == 96, "siaara_store_stats layout");
static_assert(sizeof(siaara_dicomdir_entry) == 32, "siaara_dicomdir_entry layout");

struct siaara_store {
    std::unique_ptr<siaara::store::Store> store;
};

struct siaara_store_result {
    std::vector<siaara::store::IndexRecord> rows;
    std::size_t total = 0;
    bool exact = true;
};

namespace {

namespace st = siaara::store;

// Numeric fields and string offsets; returns the string table (starts with one NUL).
std::string fill_index_record(const st::IndexRecord& r, siaara_index_record* out) {
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(siaara_index_record));
    out->struct_size = size;
    out->level = static_cast<int32_t>(r.level);
    out->flags = r.flags;
    out->series_number = r.series_number;
    out->instance_number = r.instance_number;
    out->number_of_frames = r.number_of_frames;
    out->rows = r.rows;
    out->columns = r.columns;
    out->bits_allocated = r.bits_allocated;
    out->bits_stored = r.bits_stored;
    out->high_bit = r.high_bit;
    out->samples_per_pixel = r.samples_per_pixel;
    out->pixel_representation = r.pixel_representation;
    out->num_studies = r.num_studies;
    out->num_series = r.num_series;
    out->num_instances = r.num_instances;
    out->file_size = r.file_size;
    out->pixel_data_offset = r.pixel_data_offset;
    out->pixel_data_length = r.pixel_data_length;
    std::memcpy(out->image_position, r.position, sizeof out->image_position);
    std::memcpy(out->image_orientation, r.orientation, sizeof out->image_orientation);
    std::memcpy(out->pixel_spacing, r.pixel_spacing, sizeof out->pixel_spacing);
    out->slice_thickness = r.slice_thickness;
    std::string tab(1, '\0');
    for (std::size_t i = 0; i < st::kStrCount; ++i) {
        const std::string& v = r.s[i];
        if (v.empty()) continue;
        out->str_offset[i] = static_cast<uint32_t>(tab.size());
        out->str_length[i] = static_cast<uint32_t>(v.size());
        tab += v;
        tab.push_back('\0');
    }
    return tab;
}

siaara_status record_out(const st::IndexRecord& r, siaara_index_record* out, char* buf, size_t cap, size_t* needed) {
    if (!out || out->struct_size < sizeof(siaara_index_record))
        return fail(Errc::InvalidArgument, "out must be set up with struct_size");
    const std::string tab = fill_index_record(r, out);
    if (needed) *needed = tab.size();
    if (!buf && cap == 0) return ok();
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (cap < tab.size()) return fail(Errc::BufferTooSmall, "buffer too small");
    std::memcpy(buf, tab.data(), tab.size());
    return ok();
}

std::string str_arg(const char* p) { return p ? std::string(p) : std::string(); }

bool level_ok(int32_t level) { return level >= SIAARA_LEVEL_PATIENT && level <= SIAARA_LEVEL_INSTANCE; }

siaara_status result_out(siaara::Result<std::vector<st::IndexRecord>> r, siaara_store_result** out) {
    if (!r) return fail(r.error());
    auto* res = new siaara_store_result;
    res->rows = std::move(r).value();
    res->total = res->rows.size();
    *out = res;
    return ok();
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_extract_index(const char* path, size_t path_len, siaara_index_record* out,
                                                          char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!path || !out || out->struct_size < sizeof(siaara_index_record))
            return fail(Errc::InvalidArgument, "path and out (with struct_size) must be set");
        st::IndexRecord r;
        if (Status s = st::extract_index_file(std::string(path, path_len), r); !s) return fail(s);
        return record_out(r, out, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_extract_index_memory(const uint8_t* data, size_t size,
                                                                 siaara_index_record* out, char* buf, size_t cap,
                                                                 size_t* needed) {
    return guarded([&]() -> siaara_status {
        if ((!data && size) || !out || out->struct_size < sizeof(siaara_index_record))
            return fail(Errc::InvalidArgument, "data and out (with struct_size) must be set");
        st::IndexRecord r;
        // Non-owning view: the record copies everything it keeps.
        const siaara::Bytes view(std::shared_ptr<const void>(), data, size);
        if (Status s = st::extract_index(view, r); !s) return fail(s);
        return record_out(r, out, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_open(const char* root, size_t root_len,
                                                       const siaara_store_options* options, siaara_store** out) {
    return guarded([&]() -> siaara_status {
        if (!root || !out) return fail(Errc::InvalidArgument, "root and out must not be NULL");
        *out = nullptr;
        st::StoreOptions o;
        if (options) {
            if (options->struct_size < sizeof(siaara_store_options))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            o.create = options->create != 0;
            o.durable = options->durable != 0;
            o.rebuild_if_corrupt = options->rebuild_if_corrupt != 0;
            if (options->compact_min_records) o.compact_min_records = options->compact_min_records;
        }
        auto s = st::Store::open(std::string(root, root_len), o);
        if (!s) return fail(s.error());
        *out = new siaara_store{std::move(s).value()};
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_store_free(siaara_store* store) { delete store; }

SIAARA_API siaara_status SIAARA_CALL siaara_store_add_file(siaara_store* store, const char* path, size_t path_len,
                                                           int32_t policy, int32_t mode, int32_t* outcome) {
    return guarded([&]() -> siaara_status {
        if (!store || !path) return fail(Errc::InvalidArgument, "store and path must not be NULL");
        if ((policy != SIAARA_DUP_KEEP && policy != SIAARA_DUP_REPLACE) ||
            (mode != SIAARA_ADD_COPY && mode != SIAARA_ADD_REFERENCE))
            return fail(Errc::InvalidArgument, "unknown policy or mode");
        auto r = store->store->add_file(std::string(path, path_len),
                                        policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep,
                                        mode == SIAARA_ADD_REFERENCE ? st::AddMode::Reference : st::AddMode::Copy);
        if (!r) return fail(r.error());
        if (outcome) *outcome = static_cast<int32_t>(*r);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_add_memory(siaara_store* store, const uint8_t* data, size_t size,
                                                             int32_t policy, int32_t* outcome) {
    return guarded([&]() -> siaara_status {
        if (!store || (!data && size)) return fail(Errc::InvalidArgument, "store and data must not be NULL");
        if (policy != SIAARA_DUP_KEEP && policy != SIAARA_DUP_REPLACE) return fail(Errc::InvalidArgument, "unknown policy");
        auto r = store->store->add_buffer(siaara::Bytes::copy_of(data, size),
                                          policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep);
        if (!r) return fail(r.error());
        if (outcome) *outcome = static_cast<int32_t>(*r);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_query(const siaara_store* store, const siaara_store_filter* q,
                                                        siaara_store_result** out) {
    return guarded([&]() -> siaara_status {
        if (!store || !q || !out || q->struct_size < sizeof(siaara_store_filter))
            return fail(Errc::InvalidArgument, "store, query (with struct_size) and out must be set");
        *out = nullptr;
        if (!level_ok(q->level)) return fail(Errc::InvalidArgument, "unknown level");
        st::QueryFilter f;
        f.level = static_cast<st::Level>(q->level);
        f.patient_name = str_arg(q->patient_name);
        f.patient_id = str_arg(q->patient_id);
        f.patient_birth_date = str_arg(q->patient_birth_date);
        f.patient_sex = str_arg(q->patient_sex);
        f.study_date = str_arg(q->study_date);
        f.accession_number = str_arg(q->accession_number);
        f.study_description = str_arg(q->study_description);
        f.referring_physician = str_arg(q->referring_physician);
        f.modality = str_arg(q->modality);
        f.series_description = str_arg(q->series_description);
        f.body_part = str_arg(q->body_part);
        f.study_uid = str_arg(q->study_uid);
        f.series_uid = str_arg(q->series_uid);
        f.sop_instance_uid = str_arg(q->sop_instance_uid);
        f.offset = q->offset;
        f.limit = q->limit;
        f.want_total = (q->flags & SIAARA_QUERY_NO_TOTAL) == 0;
        auto r = store->store->query(f);
        if (!r) return fail(r.error());
        auto* res = new siaara_store_result;
        res->rows = std::move(r->rows);
        res->total = r->total;
        res->exact = r->total_is_exact;
        *out = res;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_get_series(const siaara_store* store, const char* uid, size_t len,
                                                             siaara_store_result** out) {
    return guarded([&]() -> siaara_status {
        if (!store || !uid || !out) return fail(Errc::InvalidArgument, "store, uid and out must not be NULL");
        *out = nullptr;
        return result_out(store->store->series_of(std::string(uid, len)), out);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_get_instances(const siaara_store* store, const char* uid, size_t len,
                                                                siaara_store_result** out) {
    return guarded([&]() -> siaara_status {
        if (!store || !uid || !out) return fail(Errc::InvalidArgument, "store, uid and out must not be NULL");
        *out = nullptr;
        return result_out(store->store->instances_of(std::string(uid, len)), out);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_result_count(const siaara_store_result* res, uint32_t* count,
                                                               uint32_t* total, int32_t* total_exact) {
    if (!res || !count) return fail(Errc::InvalidArgument, "result and count must not be NULL");
    *count = static_cast<uint32_t>(res->rows.size());
    if (total) *total = static_cast<uint32_t>(std::min<std::size_t>(res->total, UINT32_MAX));
    if (total_exact) *total_exact = res->exact ? 1 : 0;
    return ok();
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_result_get(const siaara_store_result* res, uint32_t index,
                                                             siaara_index_record* out, char* buf, size_t cap,
                                                             size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!res) return fail(Errc::InvalidArgument, "result must not be NULL");
        if (index >= res->rows.size()) return fail(Errc::NotFound, "row index out of range");
        return record_out(res->rows[index], out, buf, cap, needed);
    });
}

SIAARA_API void SIAARA_CALL siaara_store_result_free(siaara_store_result* res) { delete res; }

SIAARA_API siaara_status SIAARA_CALL siaara_store_patient_key(const siaara_store_result* res, uint32_t index, char* buf,
                                                              size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!res) return fail(Errc::InvalidArgument, "result must not be NULL");
        if (index >= res->rows.size()) return fail(Errc::NotFound, "row index out of range");
        return copy_out(st::Store::patient_key(res->rows[index]), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_delete(siaara_store* store, int32_t level, const char* key, size_t len,
                                                         uint32_t* removed) {
    return guarded([&]() -> siaara_status {
        if (!store || !key) return fail(Errc::InvalidArgument, "store and key must not be NULL");
        if (!level_ok(level)) return fail(Errc::InvalidArgument, "unknown level");
        auto r = store->store->remove(static_cast<st::Level>(level), std::string(key, len));
        if (!r) return fail(r.error());
        if (removed) *removed = static_cast<uint32_t>(*r);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_move(siaara_store* store, siaara_store* dest, int32_t level,
                                                       const char* key, size_t len, uint32_t* moved) {
    return guarded([&]() -> siaara_status {
        if (!store || !dest || !key) return fail(Errc::InvalidArgument, "store, dest and key must not be NULL");
        if (!level_ok(level)) return fail(Errc::InvalidArgument, "unknown level");
        auto r = store->store->move_to(*dest->store, static_cast<st::Level>(level), std::string(key, len));
        if (!r) return fail(r.error());
        if (moved) *moved = static_cast<uint32_t>(*r);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_verify(siaara_store* store, int32_t deep, int32_t repair,
                                                         siaara_store_verify_report* out) {
    return guarded([&]() -> siaara_status {
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (out && out->struct_size < sizeof(siaara_store_verify_report))
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        auto r = store->store->verify(deep != 0, repair != 0);
        if (!r) return fail(r.error());
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

SIAARA_API siaara_status SIAARA_CALL siaara_store_rebuild(siaara_store* store) {
    return guarded([&]() -> siaara_status {
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (Status s = store->store->rebuild(); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_compact(siaara_store* store) {
    return guarded([&]() -> siaara_status {
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (Status s = store->store->compact(); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_set_callback(siaara_store* store, siaara_store_callback cb, void* user) {
    return guarded([&]() -> siaara_status {
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (!cb) {
            store->store->set_callback(nullptr);
            return ok();
        }
        store->store->set_callback([cb, user](const st::Event& e) {
            try {
                cb(user, static_cast<int32_t>(e.kind), e.sop_instance_uid.c_str(), e.series_uid.c_str(),
                   e.study_uid.c_str());
            } catch (...) {
                // a callback written in C cannot throw; keep foreign unwinding out of the store
            }
        });
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_store_stats_get(const siaara_store* store, siaara_store_stats* out) {
    return guarded([&]() -> siaara_status {
        if (!store || !out || out->struct_size < sizeof(siaara_store_stats))
            return fail(Errc::InvalidArgument, "store and out (with struct_size) must be set");
        const st::StoreStats s = store->store->stats();
        out->reserved0 = 0;
        out->patients = s.patients;
        out->studies = s.studies;
        out->series = s.series;
        out->instances = s.instances;
        out->data_bytes = s.data_bytes;
        out->journal_bytes = s.journal_bytes;
        out->journal_records = s.journal_records;
        out->snapshot_bytes = s.snapshot_bytes;
        out->generation = s.generation;
        out->replayed_records = s.replayed_records;
        out->recovered_bytes = s.recovered_bytes;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_folder_scan(const char* root, size_t root_len,
                                                        const siaara_scan_options* options, siaara_scan_callback cb,
                                                        void* user, siaara_scan_stats* stats) {
    return guarded([&]() -> siaara_status {
        if (!root) return fail(Errc::InvalidArgument, "root must not be NULL");
        if (stats && stats->struct_size < sizeof(siaara_scan_stats))
            return fail(Errc::InvalidArgument, "stats must be set up with struct_size");
        st::ScanOptions o;
        if (options) {
            if (options->struct_size < sizeof(siaara_scan_options))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            o.threads = options->threads;
            o.recursive = options->recursive != 0;
            o.follow_symlinks = options->follow_symlinks != 0;
            o.allow_raw = options->allow_raw != 0;
            o.report_skipped = options->report_skipped != 0;
            o.use_dicomdir = options->use_dicomdir != 0;
            o.max_files = options->max_files;
        }
        auto r = st::scan_folder(std::string(root, root_len), [&](const st::ScanItem& item) {
            if (!cb) return true;
            const bool has_rec = item.kind == st::ScanKind::Instance || item.kind == st::ScanKind::Dicomdir;
            siaara_index_record rec{};
            rec.struct_size = sizeof rec;
            std::string tab;
            if (has_rec) tab = fill_index_record(item.record, &rec);
            return cb(user, item.path.c_str(), static_cast<int32_t>(item.kind), static_cast<siaara_status>(item.status.code),
                      has_rec ? &rec : nullptr, has_rec ? tab.c_str() : nullptr) == 0;
        }, o);
        if (!r) return fail(r.error());
        if (stats) {
            stats->used_dicomdir = r->used_dicomdir ? 1 : 0;
            stats->files = r->files;
            stats->instances = r->instances;
            stats->dicomdirs = r->dicomdirs;
            stats->skipped = r->skipped;
            stats->errors = r->errors;
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dicomdir_read(const char* path, size_t path_len,
                                                          const siaara_dicomdir_read_options* options,
                                                          siaara_dicomdir_callback cb, void* user,
                                                          siaara_dicomdir_summary* summary) {
    return guarded([&]() -> siaara_status {
        if (!path) return fail(Errc::InvalidArgument, "path must not be NULL");
        if (summary && summary->struct_size < sizeof(siaara_dicomdir_summary))
            return fail(Errc::InvalidArgument, "summary must be set up with struct_size");
        st::DicomdirReadOptions o;
        if (options) {
            if (options->struct_size < sizeof(siaara_dicomdir_read_options))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            o.include_inactive = options->include_inactive != 0;
            o.resolve_files = options->resolve_files != 0;
            o.case_insensitive = options->case_insensitive != 0;
            if (options->max_depth) o.max_depth = options->max_depth;
        }
        auto r = st::read_dicomdir(std::string(path, path_len), [&](const st::DicomdirRecord& d) {
            if (!cb) return true;
            siaara_dicomdir_entry e{};
            e.struct_size = sizeof e;
            e.index = static_cast<uint32_t>(d.index);
            e.parent = static_cast<int32_t>(d.parent);
            e.depth = d.depth;
            e.in_use = d.in_use ? 1 : 0;
            e.file_status = static_cast<siaara_status>(d.file_status.code);
            e.offset = d.offset;
            siaara_index_record rec{};
            rec.struct_size = sizeof rec;
            const std::string tab = fill_index_record(d.keys, &rec);
            return cb(user, &e, &rec, tab.c_str()) == 0;
        }, o);
        if (!r) return fail(r.error());
        if (summary) {
            auto c = [](std::size_t v) { return static_cast<uint32_t>(std::min<std::size_t>(v, UINT32_MAX)); };
            summary->records = c(r->records);
            summary->reported = c(r->reported);
            summary->files = c(r->files);
            summary->missing_files = c(r->missing_files);
            summary->rejected_paths = c(r->rejected_paths);
            summary->unreachable = c(r->unreachable);
            summary->sequential_fallback = r->sequential_fallback ? 1 : 0;
            summary->warnings = c(r->warnings.size());
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dicomdir_write(const char* root, size_t root_len, const char* const* files,
                                                           const size_t* lens, size_t count,
                                                           const siaara_dicomdir_write_options* options,
                                                           uint32_t* warnings) {
    return guarded([&]() -> siaara_status {
        if (!root || (count && (!files || !lens))) return fail(Errc::InvalidArgument, "root, files and lens must be set");
        st::DicomdirWriteOptions o;
        if (options) {
            if (options->struct_size < sizeof(siaara_dicomdir_write_options))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            if (options->profile != SIAARA_PROFILE_GENERAL_CD && options->profile != SIAARA_PROFILE_GENERAL_USB)
                return fail(Errc::InvalidArgument, "unknown profile");
            o.profile = options->profile == SIAARA_PROFILE_GENERAL_USB ? st::MediaProfile::GeneralUsb
                                                                       : st::MediaProfile::GeneralCd;
            o.overwrite = options->overwrite != 0;
            o.allow_any_transfer_syntax = options->allow_any_transfer_syntax != 0;
            o.file_set_id = str_arg(options->file_set_id);
        }
        std::vector<std::string> paths;
        paths.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!files[i]) return fail(Errc::InvalidArgument, "file path is NULL");
            paths.emplace_back(files[i], lens[i]);
        }
        auto r = st::write_dicomdir(std::string(root, root_len), paths, o);
        if (!r) return fail(r.error());
        if (warnings) *warnings = static_cast<uint32_t>(r->warnings.size());
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- de-identification (appended)
#include <cstddef>

#include "deid/deid.hpp"
#include "deid/sha256.hpp"

namespace {

// Key bytes copied for the duration of a call and wiped afterwards.
struct WipedKey {
    std::vector<std::uint8_t> bytes;
    ~WipedKey() {
        if (!bytes.empty()) siaara::deid::secure_wipe(bytes.data(), bytes.size());
    }
};

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_deidentify(siaara_file* file, const siaara_deid_options* options,
                                                       uint32_t* out_flags) {
    return guarded([&]() -> siaara_status {
        if (out_flags) *out_flags = 0;
        if (!file || !options) return fail(Errc::InvalidArgument, "file and options must not be NULL");
        constexpr std::size_t kMin = offsetof(siaara_deid_options, key_len) + sizeof(size_t);
        if (options->struct_size < kMin) return fail(Errc::InvalidArgument, "struct_size too small");
        auto has = [&](std::size_t end) { return options->struct_size >= end; };
        if (!options->key && options->key_len) return fail(Errc::InvalidArgument, "key is NULL");
        WipedKey key;
        key.bytes.assign(options->key, options->key + options->key_len);
        siaara::deid::Options o;
        o.options = options->flags;
        o.key = key.bytes;
        if (has(offsetof(siaara_deid_options, combined_policy) + sizeof(int32_t))) {
            if (options->combined_policy == SIAARA_DEID_COMBINED_REMOVE) o.combined = siaara::deid::CombinedPolicy::Remove;
            else if (options->combined_policy != SIAARA_DEID_COMBINED_CONFORMANT)
                return fail(Errc::InvalidArgument, "unknown combined_policy");
        }
        if (has(offsetof(siaara_deid_options, pixel_data_clean) + sizeof(int32_t)))
            o.pixel_data_clean = options->pixel_data_clean != 0;
        if (has(offsetof(siaara_deid_options, region_count) + sizeof(size_t)) && options->region_count) {
            if (!options->regions) return fail(Errc::InvalidArgument, "regions is NULL");
            for (size_t i = 0; i < options->region_count; ++i) {
                const siaara_deid_region& r = options->regions[i];
                o.pixel_regions.push_back({r.frame, r.x, r.y, r.width, r.height});
            }
        }
        if (has(offsetof(siaara_deid_options, safe_words_len) + sizeof(size_t)) && options->safe_words_len) {
            if (!options->safe_words) return fail(Errc::InvalidArgument, "safe_words is NULL");
            std::string word;
            for (size_t i = 0; i <= options->safe_words_len; ++i) {
                const char c = i < options->safe_words_len ? options->safe_words[i] : ' ';
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                    if (!word.empty()) o.safe_words.push_back(std::move(word));
                    word.clear();
                } else {
                    word.push_back(c);
                }
            }
        }
        if (has(offsetof(siaara_deid_options, max_shift_days) + sizeof(uint32_t)) && options->max_shift_days)
            o.max_shift_days = options->max_shift_days;
        siaara::deid::Report rep;
        if (Status s = siaara::deid::deidentify(file->file, o, &rep); !s) return fail(s);
        if (out_flags) *out_flags = rep.flags;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_deid_replacement_uid(const uint8_t* key, size_t key_len, const char* uid,
                                                                 size_t uid_len, char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!key || key_len < 16) return fail(Errc::InvalidArgument, "key must be at least 16 bytes");
        if (!uid && uid_len) return fail(Errc::InvalidArgument, "uid is NULL");
        WipedKey k;
        k.bytes.assign(key, key + key_len);
        const std::string r = siaara::deid::replacement_uid(k.bytes, std::string_view(uid ? uid : "", uid_len));
        return copy_out(r, buf, cap, needed);
    });
}

}  // extern "C"
