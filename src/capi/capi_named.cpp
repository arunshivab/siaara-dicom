// SIAARA DICOM Toolkit - C API: named data layer (include/siaara/siaara.h, "named data layer").
// Modules and macros generated from PS3.3, the path API, values at a point, waveforms, RT
// control points, NM index vectors, SOP class / transfer syntax information and dictionary search.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "capi/capi_internal.hpp"
#include "core/dataset.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"
#include "image/render.hpp"
#include "modules/data.hpp"
#include "modules/modules.hpp"
#include "repair/iod.hpp"
#include "volume/volume.hpp"

namespace {

using siaara::Errc;
using siaara::Status;
namespace capi = siaara::capi;
namespace mod = siaara::modules;
namespace vol = siaara::volume;

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

const siaara::Dataset* ds_cast(const siaara_dataset* p) { return reinterpret_cast<const siaara::Dataset*>(p); }

template <class T>
bool sized(const T* out) noexcept {
    return out && out->struct_size >= sizeof(T);
}
template <class T>
void reset(T* out) noexcept {
    const uint32_t s = out->struct_size;
    std::memset(out, 0, sizeof(T));
    out->struct_size = s;
}

siaara_status copy_text(std::string_view s, char* buf, size_t cap, size_t* needed) {
    if (needed) *needed = s.size();
    if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    if (s.size() + 1 > cap) {
        std::memcpy(buf, s.data(), cap - 1);
        buf[cap - 1] = '\0';
        return fail(Errc::BufferTooSmall, "buffer too small");
    }
    if (!s.empty()) std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return ok();
}

template <class T>
siaara_status copy_array(const std::vector<T>& v, T* buf, size_t cap, size_t* count) {
    if (count) *count = v.size();
    if (v.empty() || (!buf && cap == 0)) return ok();  // nothing to copy, or a size query
    if (cap < v.size()) return fail(Errc::BufferTooSmall, "buffer too small");
    if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
    std::memcpy(buf, v.data(), v.size() * sizeof(T));
    return ok();
}

std::string_view sv(const char* p, size_t n) { return p ? std::string_view(p, n) : std::string_view(); }

void fill_units(char (&dst)[32], const std::string& u) {
    const std::size_t n = std::min(u.size(), sizeof dst - 1);
    std::memcpy(dst, u.data(), n);
    dst[n] = '\0';
}

}  // namespace

namespace siaara::capi {
// Shared with capi_view2d.cpp (siaara_frame_pixel_value).
void fill_pixel_value(const mod::PixelValue& v, uint32_t frame, uint32_t column, uint32_t row, uint32_t sample,
                      siaara_pixel_value* out) {
    reset(out);
    out->frame = frame;
    out->column = column;
    out->row = row;
    out->sample = sample;
    out->sample_kind = v.kind == siaara::image::SampleKind::Integer   ? SIAARA_SAMPLES_INTEGER
                       : v.kind == siaara::image::SampleKind::Float32 ? SIAARA_SAMPLES_FLOAT32
                                                                      : SIAARA_SAMPLES_FLOAT64;
    out->stored = v.stored;
    out->stored_real = v.stored_real;
    out->has_modality = v.has_modality ? 1 : 0;
    out->modality_lut = v.modality_lut ? 1 : 0;
    out->dose_grid_scaling = v.dose_grid_scaling ? 1 : 0;
    out->units_source = static_cast<int32_t>(v.units_source);
    out->modality_value = v.modality_value;
    out->rescale_slope = v.slope;
    out->rescale_intercept = v.intercept;
    fill_units(out->units, v.units);
}
}  // namespace siaara::capi

extern "C" {

// ---------------------------------------------------------------- modules

SIAARA_API siaara_status SIAARA_CALL siaara_module_count(uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = static_cast<uint32_t>(mod::nodes().size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_module_get(uint32_t index, siaara_module_info* out) {
    return guarded([&]() -> siaara_status {
        if (!sized(out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        const auto ns = mod::nodes();
        if (index >= ns.size()) return fail(Errc::NotFound, "module index out of range");
        reset(out);
        const mod::ModNode& n = ns[index];
        out->index = index;
        out->kind = n.kind;
        out->attribute_count = static_cast<uint32_t>(mod::flattened(index).size());
        out->name = n.name.data();
        out->table_id = n.table_id.data();
        out->class_name = n.class_name.data();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_module_find(const char* name, size_t len, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!name || !out) return fail(Errc::InvalidArgument, "name and out must not be NULL");
        auto i = mod::find_node(std::string_view(name, len));
        if (!i) return fail(Errc::NotFound, "no module or macro of that name");
        *out = *i;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_module_attribute_get(uint32_t module, uint32_t index,
                                                             siaara_module_attribute* out) {
    return guarded([&]() -> siaara_status {
        if (!sized(out)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        if (module >= mod::nodes().size()) return fail(Errc::NotFound, "module index out of range");
        const auto& f = mod::flattened(module);
        if (index >= f.size()) return fail(Errc::NotFound, "attribute index out of range");
        reset(out);
        const mod::FlatAttr& a = f[index];
        out->tag = a.attr->tag;
        out->mask = a.attr->mask;
        out->type = a.type;
        out->item_module = a.item_node;
        out->retired = a.attr->retired ? 1 : 0;
        out->keyword = a.attr->keyword.data();
        out->vr = a.attr->vr.data();
        out->vm = a.attr->vm.data();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_module_present(const siaara_file* file, const char* name, size_t len,
                                                           int32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !name || !out) return fail(Errc::InvalidArgument, "file, name and out must not be NULL");
        auto i = mod::find_node(std::string_view(name, len));
        if (!i) return fail(Errc::NotFound, "no module or macro of that name");
        *out = mod::node_present(file->file.dataset, *i) ? 1 : 0;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_module_present(const siaara_dataset* ds, const char* name,
                                                                   size_t len, int32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !name || !out) return fail(Errc::InvalidArgument, "ds, name and out must not be NULL");
        auto i = mod::find_node(std::string_view(name, len));
        if (!i) return fail(Errc::NotFound, "no module or macro of that name");
        *out = mod::node_present(*ds_cast(ds), *i) ? 1 : 0;
        return ok();
    });
}

// ---------------------------------------------------------------- path API

SIAARA_API siaara_status SIAARA_CALL siaara_get_by_path(const siaara_dataset* ds, const char* path, size_t len,
                                                        siaara_path_value* out, char* buf, size_t cap,
                                                        size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (needed) *needed = 0;
        if (!ds || !path) return fail(Errc::InvalidArgument, "ds and path must not be NULL");
        if (out && out->struct_size < sizeof(siaara_path_value)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        auto r = mod::get_by_path(*ds_cast(ds), std::string_view(path, len));
        if (!r) return fail(r.error());
        const mod::PathValue& v = r.value();
        if (out) {
            reset(out);
            out->tag = v.tag;
            const auto n = siaara::vr_name(v.vr);
            if (v.vr != siaara::VR::None && n.size() >= 2) {
                out->vr[0] = n[0];
                out->vr[1] = n[1];
            }
            out->vm = v.vm;
            out->kind = static_cast<int32_t>(v.kind);
            out->value_index = static_cast<int32_t>(v.value_index);
            out->item_count = v.item_count;
            out->has_integer = v.has_integer ? 1 : 0;
            out->has_real = v.has_real ? 1 : 0;
            out->integer = v.integer;
            out->real = v.real;
            out->byte_length = v.byte_length;
        }
        return copy_text(v.text, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_get_doubles_by_path(const siaara_dataset* ds, const char* path,
                                                                size_t len, double* buf, size_t cap,
                                                                size_t* count) {
    return guarded([&]() -> siaara_status {
        if (count) *count = 0;
        if (!ds || !path || !count) return fail(Errc::InvalidArgument, "ds, path and count must not be NULL");
        auto r = mod::doubles_by_path(*ds_cast(ds), std::string_view(path, len));
        if (!r) return fail(r.error());
        return copy_array(r.value(), buf, cap, count);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_by_path(siaara_dataset* ds, const char* path, size_t len,
                                                            siaara_dataset** out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !path || !out) return fail(Errc::InvalidArgument, "ds, path and out must not be NULL");
        *out = nullptr;
        auto r = mod::resolve_path(*ds_cast(ds), std::string_view(path, len));
        if (!r) return fail(r.error());
        if (!r.value().item) return fail(Errc::TypeMismatch, "the path does not select a sequence item");
        // The item belongs to ds's tree, which the caller may change: hand out a mutable pointer.
        *out = reinterpret_cast<siaara_dataset*>(const_cast<siaara::Dataset*>(r.value().item));
        return ok();
    });
}

// ---------------------------------------------------------------- values at a point

SIAARA_API siaara_status SIAARA_CALL siaara_file_pixel_value(const siaara_file* file, uint32_t frame, uint32_t column,
                                                             uint32_t row, uint32_t sample, siaara_pixel_value* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !sized(out)) return fail(Errc::InvalidArgument, "file and out (with struct_size) must be set");
        auto im = siaara::image::Image::open(file->file.dataset, file->file.transfer_syntax);
        if (!im) return fail(im.error());
        const siaara::image::Image& img = im.value();
        const auto& pi = img.info();
        if (frame >= pi.number_of_frames) return fail(Errc::InvalidArgument, "frame index out of range");
        if (column >= pi.columns || row >= pi.rows || sample >= pi.samples_per_pixel)
            return fail(Errc::InvalidArgument, "column, row or sample out of range");
        auto px = img.frames().decode(frame);
        if (!px) return fail(px.error());
        std::optional<siaara::image::ModalityTransform> m;
        if (pi.samples_per_pixel == 1 && pi.kind == siaara::image::SampleKind::Integer) {
            auto mt = img.modality(frame);
            if (!mt) return fail(mt.error());
            m = std::move(mt).value();
        }
        auto v = mod::pixel_value(file->file.dataset, pi, px.value(), m ? &*m : nullptr, column, row, sample);
        if (!v) return fail(v.error());
        capi::fill_pixel_value(v.value(), frame, column, row, sample, out);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_volume_voxel(const siaara_volume* volume, uint32_t i, uint32_t j,
                                                         uint32_t k, double* out) {
    return guarded([&]() -> siaara_status {
        if (!volume || !out) return fail(Errc::InvalidArgument, "volume and out must not be NULL");
        const vol::Volume& v = *volume->vol;
        const auto& d = v.grid().dims;
        if (i >= d[0] || j >= d[1] || k >= d[2]) return fail(Errc::InvalidArgument, "voxel index outside the volume");
        *out = static_cast<double>(v.at(i, j, k));
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_volume_value_at(const siaara_volume* volume, const double patient[3],
                                                            int32_t interpolation, siaara_volume_sample* out) {
    return guarded([&]() -> siaara_status {
        if (!volume || !patient || !sized(out)) return fail(Errc::InvalidArgument, "volume, patient and out must be set");
        if (interpolation != SIAARA_INTERP_NEAREST && interpolation != SIAARA_INTERP_LINEAR)
            return fail(Errc::InvalidArgument, "interpolation must be SIAARA_INTERP_NEAREST or SIAARA_INTERP_LINEAR");
        auto r = mod::volume_value_at(*volume->vol, patient, interpolation == SIAARA_INTERP_LINEAR);
        if (!r) return fail(r.error());
        reset(out);
        out->interpolation = interpolation;
        for (int a = 0; a < 3; ++a) {
            out->patient[a] = patient[a];
            out->index[a] = r.value().index[a];
            out->voxel[a] = r.value().voxel[a];
        }
        out->inside = r.value().inside ? 1 : 0;
        out->value = r.value().inside ? r.value().value : 0;
        return ok();
    });
}

// ---------------------------------------------------------------- waveforms

SIAARA_API siaara_status SIAARA_CALL siaara_waveform_group_count(const siaara_dataset* ds, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        *out = 0;
        auto r = mod::waveform_group_count(*ds_cast(ds));
        if (!r) return fail(r.error());
        *out = r.value();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_waveform_group_info(const siaara_dataset* ds, uint32_t group,
                                                                siaara_waveform_group* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !sized(out)) return fail(Errc::InvalidArgument, "ds and out (with struct_size) must be set");
        auto r = mod::waveform_group(*ds_cast(ds), group);
        if (!r) return fail(r.error());
        const mod::WaveformGroup& g = r.value();
        reset(out);
        out->channels = g.channels;
        out->samples = g.samples;
        out->bits_allocated = g.bits_allocated;
        out->sampling_frequency = g.sampling_frequency;
        const std::size_t n = std::min<std::size_t>(g.interpretation.size(), 3);
        std::memcpy(out->sample_interpretation, g.interpretation.data(), n);
        out->has_padding = g.padding ? 1 : 0;
        out->padding_value = g.padding.value_or(0);
        out->has_time_offset = g.time_offset ? 1 : 0;
        out->time_offset = g.time_offset.value_or(0);
        out->has_trigger_time_offset = g.trigger_time_offset ? 1 : 0;
        out->trigger_time_offset = g.trigger_time_offset.value_or(0);
        out->data_samples = g.data_samples;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_waveform_channel_info(const siaara_dataset* ds, uint32_t group,
                                                                  uint32_t channel, siaara_waveform_channel* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !sized(out)) return fail(Errc::InvalidArgument, "ds and out (with struct_size) must be set");
        auto r = mod::waveform_channel(*ds_cast(ds), group, channel);
        if (!r) return fail(r.error());
        const mod::WaveformChannel& c = r.value();
        reset(out);
        out->bits_stored = c.bits_stored;
        out->has_channel_number = c.number ? 1 : 0;
        out->channel_number = static_cast<int32_t>(c.number.value_or(0));
        out->has_sensitivity = c.sensitivity ? 1 : 0;
        out->sensitivity = c.sensitivity.value_or(0);
        out->has_correction = c.correction ? 1 : 0;
        out->correction = c.correction.value_or(1);
        out->has_baseline = c.baseline ? 1 : 0;
        out->baseline = c.baseline.value_or(0);
        out->has_time_skew = c.time_skew ? 1 : 0;
        out->time_skew = c.time_skew.value_or(0);
        out->has_sample_skew = c.sample_skew ? 1 : 0;
        out->sample_skew = c.sample_skew.value_or(0);
        out->has_offset = c.offset ? 1 : 0;
        out->offset = c.offset.value_or(0);
        out->has_filter_low = c.filter_low ? 1 : 0;
        out->filter_low = c.filter_low.value_or(0);
        out->has_filter_high = c.filter_high ? 1 : 0;
        out->filter_high = c.filter_high.value_or(0);
        out->has_notch = c.notch ? 1 : 0;
        out->notch = c.notch.value_or(0);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_waveform_channel_string(const siaara_dataset* ds, uint32_t group,
                                                                    uint32_t channel, int32_t which, char* buf,
                                                                    size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (needed) *needed = 0;
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        if (which < SIAARA_WAVEFORM_LABEL || which > SIAARA_WAVEFORM_GROUP_LABEL)
            return fail(Errc::InvalidArgument, "unknown SIAARA_WAVEFORM_* value");
        auto r = mod::waveform_text(*ds_cast(ds), group, channel, static_cast<mod::WaveformText>(which));
        if (!r) return fail(r.error());
        return copy_text(r.value(), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_waveform_samples(const siaara_dataset* ds, uint32_t group, uint32_t channel,
                                                             int32_t scaled, double* buf, size_t cap, size_t* count) {
    return guarded([&]() -> siaara_status {
        if (count) *count = 0;
        if (!ds || !count) return fail(Errc::InvalidArgument, "ds and count must not be NULL");
        if (scaled != 0 && scaled != 1) return fail(Errc::InvalidArgument, "scaled must be 0 or 1");
        if (scaled) {
            auto r = mod::waveform_scaled(*ds_cast(ds), group, channel);
            if (!r) return fail(r.error());
            return copy_array(r.value(), buf, cap, count);
        }
        auto r = mod::waveform_raw(*ds_cast(ds), group, channel);
        if (!r) return fail(r.error());
        std::vector<double> d(r.value().begin(), r.value().end());
        return copy_array(d, buf, cap, count);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_waveform_raw_samples(const siaara_dataset* ds, uint32_t group,
                                                                 uint32_t channel, int64_t* buf, size_t cap,
                                                                 size_t* count) {
    return guarded([&]() -> siaara_status {
        if (count) *count = 0;
        if (!ds || !count) return fail(Errc::InvalidArgument, "ds and count must not be NULL");
        auto r = mod::waveform_raw(*ds_cast(ds), group, channel);
        if (!r) return fail(r.error());
        return copy_array(r.value(), buf, cap, count);
    });
}

// ---------------------------------------------------------------- RT control points

SIAARA_API siaara_status SIAARA_CALL siaara_rt_control_point_source(const siaara_dataset* beam, uint32_t cp,
                                                                    uint32_t tag, uint32_t* out_cp) {
    return guarded([&]() -> siaara_status {
        if (!beam || !out_cp) return fail(Errc::InvalidArgument, "beam and out_cp must not be NULL");
        auto r = mod::control_point_source(*ds_cast(beam), cp, tag);
        if (!r) return fail(r.error());
        *out_cp = r.value();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_rt_device_positions(const siaara_dataset* beam, uint32_t cp,
                                                                const char* device_type, size_t len,
                                                                int32_t effective, double* buf, size_t cap,
                                                                size_t* count, uint32_t* out_cp) {
    return guarded([&]() -> siaara_status {
        if (count) *count = 0;
        if (!beam || !device_type || !count) return fail(Errc::InvalidArgument, "beam, device_type and count must not be NULL");
        if (effective != 0 && effective != 1) return fail(Errc::InvalidArgument, "effective must be 0 or 1");
        uint32_t from = 0;
        auto r = mod::device_positions(*ds_cast(beam), cp, std::string_view(device_type, len), effective == 1, &from);
        if (!r) return fail(r.error());
        if (out_cp) *out_cp = from;
        return copy_array(r.value(), buf, cap, count);
    });
}

// ---------------------------------------------------------------- NM

SIAARA_API siaara_status SIAARA_CALL siaara_nm_frame_index_get(const siaara_dataset* ds, uint32_t frame,
                                                               siaara_nm_frame_index* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !sized(out)) return fail(Errc::InvalidArgument, "ds and out (with struct_size) must be set");
        auto r = mod::nm_frame_index(*ds_cast(ds), frame);
        if (!r) return fail(r.error());
        const mod::NmFrameIndex& x = r.value();
        reset(out);
        out->frame = frame;
        out->present = x.present;
        out->in_pointer = x.in_pointer;
        out->energy_window = x.values[0];
        out->detector = x.values[1];
        out->phase = x.values[2];
        out->rotation = x.values[3];
        out->rr_interval = x.values[4];
        out->time_slot = x.values[5];
        out->slice = x.values[6];
        out->angular_view = x.values[7];
        out->time_slice = x.values[8];
        return ok();
    });
}

// ---------------------------------------------------------------- SOP classes, transfer syntaxes, dictionary

SIAARA_API siaara_status SIAARA_CALL siaara_sop_class_info_get(const char* uid, size_t len, siaara_sop_class_info* out) {
    return guarded([&]() -> siaara_status {
        if (!uid || !sized(out)) return fail(Errc::InvalidArgument, "uid and out (with struct_size) must be set");
        std::string_view u(uid, len);
        while (!u.empty() && (u.back() == '\0' || u.back() == ' ')) u.remove_suffix(1);
        const auto* e = siaara::dict::lookup_uid(u);
        if (!e || e->type.find("SOP Class") == std::string_view::npos) return fail(Errc::NotFound, "not a SOP Class UID of PS3.6");
        reset(out);
        out->name = e->name.data();
        out->keyword = e->keyword.data();
        out->retired = e->retired ? 1 : 0;
        out->iod_name = "";
        out->iod_table_id = "";
        const auto* iod = siaara::repair::iod_for_sop_class(u);
        if (!iod) return ok();
        out->has_iod = 1;
        out->iod_name = iod->name.data();
        out->iod_table_id = iod->table_id.data();
        // Object kind from the IOD's own modules (PS3.3 Annex A), most specific first.
        struct Rule {
            std::string_view module;
            int32_t kind;
        };
        static constexpr Rule kRules[] = {
            {"RT General Plan", SIAARA_OBJECT_RT_PLAN},
            {"RT Dose", SIAARA_OBJECT_RT_DOSE},
            {"Structure Set", SIAARA_OBJECT_RT_STRUCTURE_SET},
            {"RT Image", SIAARA_OBJECT_RT_IMAGE},
            {"RT General Treatment Record", SIAARA_OBJECT_RT_TREATMENT_RECORD},
            {"SR Document Content", SIAARA_OBJECT_STRUCTURED_REPORT},
            {"Segmentation Image", SIAARA_OBJECT_SEGMENTATION},
            {"Waveform", SIAARA_OBJECT_WAVEFORM},
            {"Presentation State Identification", SIAARA_OBJECT_PRESENTATION_STATE},
            {"Encapsulated Document", SIAARA_OBJECT_ENCAPSULATED_DOCUMENT},
            {"Spatial Registration", SIAARA_OBJECT_REGISTRATION},
            {"Deformable Spatial Registration", SIAARA_OBJECT_REGISTRATION},
            {"Raw Data", SIAARA_OBJECT_RAW_DATA},
            {"MR Spectroscopy", SIAARA_OBJECT_SPECTROSCOPY},
            {"Surface Mesh", SIAARA_OBJECT_SURFACE},
            {"Image Pixel", SIAARA_OBJECT_IMAGE},
        };
        for (const Rule& r : kRules) {
            for (const auto& m : iod->modules) {
                if (m.name == r.module) {
                    out->object_kind = r.kind;
                    return ok();
                }
            }
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_transfer_syntax_info_get(const char* uid, size_t len,
                                                                     siaara_transfer_syntax_info* out) {
    return guarded([&]() -> siaara_status {
        if (!uid || !sized(out)) return fail(Errc::InvalidArgument, "uid and out (with struct_size) must be set");
        std::string_view u(uid, len);
        while (!u.empty() && (u.back() == '\0' || u.back() == ' ')) u.remove_suffix(1);
        const auto* t = siaara::find_transfer_syntax(u);
        if (!t) return fail(Errc::NotFound, "unknown transfer syntax");
        reset(out);
        out->explicit_vr = t->explicit_vr ? 1 : 0;
        out->big_endian = t->big_endian ? 1 : 0;
        out->encapsulated = t->encapsulated ? 1 : 0;
        out->lossy = t->lossy ? 1 : 0;
        out->deflated = t->codec == siaara::Codec::Deflate ? 1 : 0;
        out->retired = t->retired ? 1 : 0;
        out->name = t->name.data();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_count(uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = static_cast<uint32_t>(siaara::dict::entry_count());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_entry_at(uint32_t index, siaara_dict_entry* out) {
    return guarded([&]() -> siaara_status {
        if (!out || out->struct_size < sizeof(siaara_dict_entry)) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        const auto* e = siaara::dict::entry_at(index);
        if (!e) return fail(Errc::NotFound, "index out of range");
        // The same fields siaara_dict_lookup fills.
        const auto a = siaara::vr_name(e->vr);
        const auto b = siaara::vr_name(e->vr_alt);
        out->tag = e->tag;
        out->vr[0] = e->vr == siaara::VR::None ? 0 : a[0];
        out->vr[1] = e->vr == siaara::VR::None ? 0 : a[1];
        out->vr[2] = 0;
        out->vr_alt[0] = e->vr_alt == siaara::VR::None ? 0 : b[0];
        out->vr_alt[1] = e->vr_alt == siaara::VR::None ? 0 : b[1];
        out->vr_alt[2] = 0;
        out->retired = e->retired ? 1 : 0;
        out->keyword = e->keyword.data();
        out->name = e->name.data();
        out->vm = e->vm.data();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_search(const char* text, size_t len, const char* vr, int32_t flags,
                                                        uint32_t* buf, size_t cap, size_t* count) {
    return guarded([&]() -> siaara_status {
        if (count) *count = 0;
        if (!count || (!text && len)) return fail(Errc::InvalidArgument, "count must not be NULL");
        if (flags & ~(SIAARA_DICT_MATCH_NAME | SIAARA_DICT_MATCH_KEYWORD | SIAARA_DICT_INCLUDE_RETIRED))
            return fail(Errc::InvalidArgument, "unknown flags");
        std::string needle(sv(text, len));
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        siaara::VR want = siaara::VR::None;
        if (vr) {
            if (!vr[0] || !vr[1]) return fail(Errc::InvalidArgument, "vr must be two characters");
            want = siaara::vr_from_chars(vr[0], vr[1]);
            if (!siaara::vr_is_known(want)) return fail(Errc::InvalidArgument, "unknown VR");
        }
        const bool by_name = (flags & SIAARA_DICT_MATCH_NAME) != 0 || (flags & (SIAARA_DICT_MATCH_NAME | SIAARA_DICT_MATCH_KEYWORD)) == 0;
        const bool by_kw = (flags & SIAARA_DICT_MATCH_KEYWORD) != 0 || (flags & (SIAARA_DICT_MATCH_NAME | SIAARA_DICT_MATCH_KEYWORD)) == 0;
        auto contains = [&](std::string_view hay) {
            if (needle.empty()) return true;
            if (hay.size() < needle.size()) return false;
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                std::size_t k = 0;
                while (k < needle.size() && std::tolower(static_cast<unsigned char>(hay[i + k])) == needle[k]) ++k;
                if (k == needle.size()) return true;
            }
            return false;
        };
        std::vector<uint32_t> hits;
        const std::size_t n = siaara::dict::entry_count();
        for (std::size_t i = 0; i < n; ++i) {
            const auto* e = siaara::dict::entry_at(i);
            if (!e) continue;
            if (e->retired && !(flags & SIAARA_DICT_INCLUDE_RETIRED)) continue;
            if (want != siaara::VR::None && e->vr != want && e->vr_alt != want) continue;
            if (!((by_name && contains(e->name)) || (by_kw && contains(e->keyword)))) continue;
            hits.push_back(static_cast<uint32_t>(i));
        }
        *count = hits.size();
        if (buf && cap) std::memcpy(buf, hits.data(), std::min(cap, hits.size()) * sizeof(uint32_t));
        return ok();
    });
}

}  // extern "C"
