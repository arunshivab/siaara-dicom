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
