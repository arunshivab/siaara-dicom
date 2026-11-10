// SIAARA DICOM Toolkit - C API implementation (include/siaara/siaara.h).
#include "siaara/siaara.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <cstring>
#include <new>
#include <string>

#include "codec/transcode.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "image/frames.hpp"
#include "image/render.hpp"
#include "capi/capi_internal.hpp"  // struct siaara_file, siaara_web_client, siaara_store (wip-capi)

namespace {

using siaara::Errc;
using siaara::Status;

// Error text for siaara_last_error_message(): a fixed per-thread buffer, so recording an error
// never allocates and never throws (CAPI-03, R-013). Longer messages are cut at a UTF-8
// character boundary.
constexpr std::size_t kErrorCap = 512;
thread_local char g_last_error[kErrorCap] = {};

void set_error(std::string_view a, std::string_view b = {}) noexcept {
    std::size_t n = 0;
    for (std::string_view part : {a, b}) {
        std::size_t take = std::min(part.size(), kErrorCap - 1 - n);
        if (take < part.size())  // cut: do not split a multi-byte UTF-8 sequence
            while (take > 0 && (static_cast<unsigned char>(part[take]) & 0xC0u) == 0x80u) --take;
        if (take) std::memcpy(g_last_error + n, part.data(), take);
        n += take;
    }
    g_last_error[n] = '\0';
}

siaara_status fail(Errc code, std::string_view msg) noexcept {
    set_error(msg);
    return static_cast<siaara_status>(code);
}
siaara_status fail(const Status& s) noexcept {
    char suffix[40] = " (offset ";
    std::size_t k = 9;
    if (s.offset) {
        char digits[24];
        std::size_t d = 0;
        for (std::uint64_t v = s.offset; v != 0 && d < sizeof digits; v /= 10) digits[d++] = static_cast<char>('0' + v % 10);
        while (d > 0) suffix[k++] = digits[--d];
        suffix[k++] = ')';
    }
    set_error(s.message, s.offset ? std::string_view(suffix, k) : std::string_view());
    return static_cast<siaara_status>(s.code);
}
siaara_status ok() noexcept {
    g_last_error[0] = '\0';
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

// Same, for exports that return a value instead of a status (`fallback` on any exception).
template <class T, class F>
T guarded_value(T fallback, F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        (void)fail(Errc::OutOfMemory, "out of memory");
    } catch (...) {
        (void)fail(Errc::Internal, "internal error");
    }
    return fallback;
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

// siaara_save_options -> WriteOptions. Fields past struct_size keep their defaults.
Status to_write_options(const siaara_save_options* o, siaara::WriteOptions& w) {
    if (!o) return Status::ok();
    if (o->struct_size < sizeof(uint32_t))
        return Status(Errc::InvalidArgument, "options must be set up with struct_size");
    const auto has = [&](std::size_t end) { return o->struct_size >= end; };
    if (has(offsetof(siaara_save_options, sequence_lengths) + sizeof(int32_t))) {
        switch (o->sequence_lengths) {
            case SIAARA_SEQ_LENGTHS_KEEP: w.sequence_lengths = siaara::SequenceLengths::KeepAsRead; break;
            case SIAARA_SEQ_LENGTHS_EXPLICIT: w.sequence_lengths = siaara::SequenceLengths::Explicit; break;
            case SIAARA_SEQ_LENGTHS_UNDEFINED: w.sequence_lengths = siaara::SequenceLengths::Undefined; break;
            default: return Status(Errc::InvalidArgument, "unknown sequence_lengths");
        }
    }
    if (has(offsetof(siaara_save_options, group_lengths) + sizeof(int32_t))) {
        switch (o->group_lengths) {
            case SIAARA_GROUP_LENGTHS_KEEP: w.group_lengths = siaara::GroupLengths::Keep; break;
            case SIAARA_GROUP_LENGTHS_DROP: w.group_lengths = siaara::GroupLengths::Drop; break;
            case SIAARA_GROUP_LENGTHS_RECOMPUTE: w.group_lengths = siaara::GroupLengths::Recompute; break;
            default: return Status(Errc::InvalidArgument, "unknown group_lengths");
        }
    }
    if (has(offsetof(siaara_save_options, odd_lengths) + sizeof(int32_t))) {
        switch (o->odd_lengths) {
            case SIAARA_ODD_LENGTHS_KEEP: w.odd_lengths = siaara::OddLengths::Keep; break;
            case SIAARA_ODD_LENGTHS_PAD: w.odd_lengths = siaara::OddLengths::Pad; break;
            default: return Status(Errc::InvalidArgument, "unknown odd_lengths");
        }
    }
    if (has(offsetof(siaara_save_options, allow_incomplete) + sizeof(int32_t))) {
        if (o->allow_incomplete != 0 && o->allow_incomplete != 1)
            return Status(Errc::InvalidArgument, "allow_incomplete must be 0 or 1");
        w.allow_incomplete = o->allow_incomplete == 1;
    }
    return Status::ok();
}

bool parse_vr(const char* vr, siaara::VR& out) {
    if (!vr || !vr[0] || !vr[1]) return false;
    out = siaara::vr_from_chars(vr[0], vr[1]);
    return siaara::vr_is_known(out);
}

void put_vr(siaara::VR vr, char out[3]) {
    const auto a = siaara::vr_name(vr);
    out[0] = vr == siaara::VR::None ? 0 : a[0];
    out[1] = vr == siaara::VR::None ? 0 : a[1];
    out[2] = 0;
}

// Pool strings of the private dictionary are NUL-terminated.
void fill_private(const siaara::dict::PrivateEntry& e, siaara_private_dict_entry* out) {
    out->group = e.group;
    out->element = e.element;
    put_vr(e.vr, out->vr);
    out->retired = e.retired ? 1 : 0;
    out->conflict = e.conflict ? 1 : 0;
    out->vm_conflict = e.vm_conflict ? 1 : 0;
    out->confidence = e.confidence == siaara::dict::PrivateConfidence::Assumed ? SIAARA_PRIVATE_ASSUMED : SIAARA_PRIVATE_STATED;
    out->match = e.match == siaara::dict::PrivateMatch::Fuzzy ? SIAARA_PRIVATE_FUZZY : SIAARA_PRIVATE_EXACT;
    out->source_page = e.source.page;
    out->source_count = e.source_count;
    out->candidate_count = e.candidate_count;
    out->creator = e.creator.data();
    out->keyword = e.keyword.data();
    out->name = e.name.data();
    out->vm = e.vm.data();
    out->vendor = e.vendor.data();
    out->source_document = e.source.document.data();
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

SIAARA_API uint32_t SIAARA_CALL siaara_abi_version(void) SIAARA_NOEXCEPT { return SIAARA_ABI_VERSION; }
SIAARA_API const char* SIAARA_CALL siaara_version_string(void) SIAARA_NOEXCEPT { return "0.1.0"; }
SIAARA_API const char* SIAARA_CALL siaara_last_error_message(void) SIAARA_NOEXCEPT { return g_last_error; }

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

SIAARA_API void SIAARA_CALL siaara_file_free(siaara_file* file) SIAARA_NOEXCEPT { delete file; }

SIAARA_API siaara_status SIAARA_CALL siaara_file_create(siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = new siaara_file{};
        (*out)->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_dataset(siaara_file* file, siaara_dataset** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = ds_out(&file->file.dataset);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_meta(siaara_file* file, siaara_dataset** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = ds_out(&file->file.meta);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_transfer_syntax(const siaara_file* file, char* buf,
                                                                 size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
        return copy_out(file->file.transfer_syntax, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_warning_count(const siaara_file* file, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = static_cast<uint32_t>(file->file.warnings.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_warning(const siaara_file* file, uint32_t index, char* buf,
                                                         size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
        if (index >= file->file.warnings.size()) return fail(Errc::InvalidArgument, "warning index out of range");
        return copy_out(file->file.warnings[index], buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_warning_code(const siaara_file* file, uint32_t index, int32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        if (index >= file->file.warnings.size()) return fail(Errc::InvalidArgument, "warning index out of range");
        const std::string& w = file->file.warnings[index];
        const auto starts = [&](std::string_view p) { return w.compare(0, p.size(), p) == 0; };
        *out = starts(siaara::kWarnParseIncomplete)  ? SIAARA_FILE_WARNING_PARSE_INCOMPLETE
               : starts(siaara::kWarnInputTruncated) ? SIAARA_FILE_WARNING_INPUT_TRUNCATED
               : starts(siaara::kWarnReadRecovered) ? SIAARA_FILE_WARNING_READ_RECOVERED
                                                     : SIAARA_FILE_WARNING_GENERAL;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_read_report(const siaara_file* file, siaara_read_report* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        if (out->struct_size < sizeof(uint32_t) * 2)
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        siaara_read_report r{};
        r.struct_size = out->struct_size < sizeof r ? out->struct_size : static_cast<uint32_t>(sizeof r);
        const siaara::DicomFile& f = file->file;
        r.loss = (f.read_loss & siaara::DicomFile::kReadBytesSkipped ? static_cast<uint32_t>(SIAARA_READ_BYTES_SKIPPED) : 0u) |
                 (f.read_loss & siaara::DicomFile::kReadInputTruncated ? static_cast<uint32_t>(SIAARA_READ_INPUT_TRUNCATED) : 0u) |
                 (f.read_loss & siaara::DicomFile::kReadHeaderOnly ? static_cast<uint32_t>(SIAARA_READ_HEADER_ONLY) : 0u) |
                 (f.read_loss & siaara::DicomFile::kReadRecovered ? static_cast<uint32_t>(SIAARA_READ_RECOVERED) : 0u);
        r.first_loss_offset = f.read_loss_offset;
        r.bytes_not_read = f.bytes_not_read;
        r.stopped_at_offset = f.stopped_at_offset;
        r.warning_count = static_cast<uint32_t>(f.warnings.size());
        std::memcpy(out, &r, r.struct_size);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_save(const siaara_file* file, const char* path,
                                                      size_t path_len, const char* ts_uid) {
    return guarded([&]() -> siaara_status { return siaara_file_save_ex(file, path, path_len, ts_uid, nullptr); });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_save_ex(const siaara_file* file, const char* path,
                                                         size_t path_len, const char* ts_uid,
                                                         const siaara_save_options* options) {
    return guarded([&]() -> siaara_status {
        siaara::WriteOptions wo;
        if (Status s = to_write_options(options, wo); !s) return fail(s);
        if (!file || !path) return fail(Errc::InvalidArgument, "file and path must not be NULL");
        if (ts_uid && !siaara::find_transfer_syntax(ts_uid))
            return fail(Errc::Unsupported, "unknown transfer syntax");
        siaara::DicomFile copy = file->file;
        // A different transfer syntax is reached by transcoding. siaara_file_save never applies
        // lossy compression; use siaara_file_transcode for that.
        if (ts_uid && copy.transfer_syntax != ts_uid) {
            const auto* from = siaara::find_transfer_syntax(copy.transfer_syntax);
            const auto* to = siaara::find_transfer_syntax(ts_uid);
            if (!from || !to || from->encapsulated || to->encapsulated) {
                siaara::codec::TranscodeParams p;
                p.allow_lossy = false;
                if (Status s = siaara::codec::transcode_file(copy, ts_uid, p); !s) return fail(s);
            }
            copy.transfer_syntax = ts_uid;
        }
        if (Status s = siaara::write_file(copy, std::string(path, path_len), wo); !s) return fail(s);
        return ok();
    });
}

SIAARA_API int32_t SIAARA_CALL siaara_dataset_contains(const siaara_dataset* ds, uint32_t tag) SIAARA_NOEXCEPT {
    return ds && ds_cast(ds)->contains(tag) ? 1 : 0;
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vr(const siaara_dataset* ds, uint32_t tag, char out_vr[3]) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out_vr) return fail(Errc::InvalidArgument, "ds and out_vr must not be NULL");
        const auto* e = ds_cast(ds)->find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        const auto n = siaara::vr_name(e->vr);
        out_vr[0] = n[0];
        out_vr[1] = n[1];
        out_vr[2] = 0;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vm(const siaara_dataset* ds, uint32_t tag, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
        *out = static_cast<uint32_t>(ds_cast(ds)->value_multiplicity(tag));
        return ok();
    });
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
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
        auto v = ds_cast(ds)->get_int(tag, vm);
        if (!v) return fail(Errc::TypeMismatch, "value cannot be read as an integer at this index");
        *out = *v;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_double(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm, double* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        if (!ds_cast(ds)->contains(tag)) return fail(Errc::NotFound, "element not present");
        auto v = ds_cast(ds)->get_double(tag, vm);
        if (!v) return fail(Errc::TypeMismatch, "value cannot be read as a number at this index");
        *out = *v;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_raw(const siaara_dataset* ds, uint32_t tag,
                                                            const uint8_t** out_data, size_t* out_size) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out_data || !out_size) return fail(Errc::InvalidArgument, "arguments must not be NULL");
        const auto* e = ds_cast(ds)->find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        if (e->is_sequence() || e->is_encapsulated())
            return fail(Errc::TypeMismatch, "element has no single value (sequence or encapsulated)");
        *out_data = e->value.data();
        *out_size = e->value.size();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item_count(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        const auto* e = ds_cast(ds)->find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        if (!e->is_sequence()) return fail(Errc::TypeMismatch, "element is not a sequence");
        *out = static_cast<uint32_t>(e->items.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item(siaara_dataset* ds, uint32_t tag, uint32_t index,
                                                         siaara_dataset** out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        auto* e = ds_cast(ds)->find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        if (!e->is_sequence()) return fail(Errc::TypeMismatch, "element is not a sequence");
        if (index >= e->items.size()) return fail(Errc::NotFound, "item index out of range");
        *out = ds_out(&e->items[index]);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_element_count(const siaara_dataset* ds, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        *out = static_cast<uint32_t>(ds_cast(ds)->size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dataset_element_tag(const siaara_dataset* ds, uint32_t index,
                                                                uint32_t* out_tag) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out_tag) return fail(Errc::InvalidArgument, "ds and out_tag must not be NULL");
        const auto& els = ds_cast(ds)->elements();
        if (index >= els.size()) return fail(Errc::NotFound, "index out of range");
        *out_tag = els[index].tag;
        return ok();
    });
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
    return guarded([&]() -> siaara_status {
        if (!ds) return fail(Errc::InvalidArgument, "ds must not be NULL");
        if (!ds_cast(ds)->remove(tag)) return fail(Errc::NotFound, "element not present");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup(uint32_t tag, siaara_dict_entry* out) {
    return guarded([&]() -> siaara_status {
        if (!out || out->struct_size < sizeof(siaara_dict_entry))
            return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        const auto* e = siaara::dict::lookup(tag);
        if (!e) return fail(Errc::NotFound, "tag not in dictionary");
        fill_entry(*e, out);
        out->tag = tag;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_keyword(const char* keyword, size_t len,
                                                                siaara_dict_entry* out) {
    return guarded([&]() -> siaara_status {
        if (!keyword || !out || out->struct_size < sizeof(siaara_dict_entry))
            return fail(Errc::InvalidArgument, "bad argument");
        const auto* e = siaara::dict::lookup_keyword(std::string_view(keyword, len));
        if (!e) return fail(Errc::NotFound, "keyword not in dictionary");
        fill_entry(*e, out);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_private(const char* creator, size_t len, uint16_t group,
                                                                uint8_t element_low_byte,
                                                                siaara_private_dict_entry* out) {
    return guarded([&]() -> siaara_status {
        if ((!creator && len) || !out || out->struct_size < sizeof(siaara_private_dict_entry))
            return fail(Errc::InvalidArgument, "bad argument");
        const auto e = siaara::dict::lookup_private(std::string_view(creator ? creator : "", len), group, element_low_byte);
        if (!e) return fail(Errc::NotFound, "private creator and element not in the private dictionary");
        fill_private(*e, out);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_private_element(const siaara_dataset* ds, uint32_t tag,
                                                                        siaara_private_dict_entry* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out || out->struct_size < sizeof(siaara_private_dict_entry))
            return fail(Errc::InvalidArgument, "bad argument");
        const auto e = siaara::dict::lookup_private_element(*ds_cast(ds), tag);
        if (!e) return fail(Errc::NotFound, "private element not in the private dictionary");
        fill_private(*e, out);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dict_private_candidate(const char* creator, size_t len, uint16_t group,
                                                                   uint8_t element_low_byte, uint32_t index,
                                                                   siaara_private_dict_candidate* out) {
    return guarded([&]() -> siaara_status {
        if ((!creator && len) || !out || out->struct_size < sizeof(siaara_private_dict_candidate))
            return fail(Errc::InvalidArgument, "bad argument");
        const auto e = siaara::dict::lookup_private(std::string_view(creator ? creator : "", len), group, element_low_byte);
        if (!e) return fail(Errc::NotFound, "private creator and element not in the private dictionary");
        siaara::dict::PrivateCandidate c;
        if (!siaara::dict::private_candidate(*e, index, c)) return fail(Errc::NotFound, "no such candidate");
        put_vr(c.vr, out->vr);
        out->page = c.source.page;
        out->document = c.source.document.data();
        return ok();
    });
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
        if (flags & ~static_cast<uint32_t>(SIAARA_JSON_NUMBERS_AS_STRINGS | SIAARA_JSON_STRICT_CHARSET |
                                           SIAARA_JSON_PRIVATE_NAMES))
            return fail(Errc::InvalidArgument, "unknown flags");
        siaara::web::ModelWriteOptions opt;
        opt.json_private_names = (flags & SIAARA_JSON_PRIVATE_NAMES) != 0;
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

SIAARA_API void SIAARA_CALL siaara_web_client_free(siaara_web_client* client) SIAARA_NOEXCEPT { delete client; }

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

struct siaara_store_result {
    std::vector<siaara::store::IndexRecord> rows;
    std::size_t total = 0;
    bool exact = true;
    siaara::capi::LiveHandle live_{};
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
            if (options->struct_size < offsetof(siaara_store_options, compact_min_records) + sizeof(uint64_t))
                return fail(Errc::InvalidArgument, "options must be set up with struct_size");
            const auto has = [&](std::size_t end) { return options->struct_size >= end; };
            o.create = options->create != 0;
            o.durable = options->durable != 0;
            o.rebuild_if_corrupt = options->rebuild_if_corrupt != 0;
            if (options->compact_min_records) o.compact_min_records = options->compact_min_records;
            if (has(offsetof(siaara_store_options, max_total_bytes) + sizeof(uint64_t)))
                o.max_total_bytes = options->max_total_bytes;
            if (has(offsetof(siaara_store_options, min_free_disk_bytes) + sizeof(uint64_t)) && options->min_free_disk_bytes)
                o.min_free_disk_bytes = options->min_free_disk_bytes;
            if (has(offsetof(siaara_store_options, file_mode) + sizeof(uint32_t))) {
                if (options->file_mode > 07777u) return fail(Errc::InvalidArgument, "file_mode must be a POSIX mode (0 .. 07777)");
                o.file_mode = options->file_mode;
            }
        }
        auto s = st::Store::open(std::string(root, root_len), o);
        if (!s) return fail(s.error());
        *out = new siaara_store{std::move(s).value()};
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_store_free(siaara_store* store) SIAARA_NOEXCEPT { delete store; }

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
    return guarded([&]() -> siaara_status {
        if (!res || !count) return fail(Errc::InvalidArgument, "result and count must not be NULL");
        *count = static_cast<uint32_t>(res->rows.size());
        if (total) *total = static_cast<uint32_t>(std::min<std::size_t>(res->total, UINT32_MAX));
        if (total_exact) *total_exact = res->exact ? 1 : 0;
        return ok();
    });
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

SIAARA_API void SIAARA_CALL siaara_store_result_free(siaara_store_result* res) SIAARA_NOEXCEPT { delete res; }

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

// ---------------------------------------------------------------- TLS (appended)
#include "net/tls.hpp"

namespace {
siaara::net::TlsSettings tls_from_options(const siaara_tls_options* o) {
    siaara::net::TlsSettings t;
    t.enabled = true;
    if (!o) return t;
    auto str = [](const char* p) { return p ? std::string(p) : std::string(); };
    t.ca_file = str(o->ca_file);
    t.identity.certificate_file = str(o->certificate_file);
    t.identity.private_key_file = str(o->private_key_file);
    t.identity.pkcs12_file = str(o->pkcs12_file);
    t.identity.password = str(o->password);
#if defined(_WIN32)
    t.identity.windows_thumbprint = str(o->os_identity);
#elif defined(__APPLE__)
    t.identity.keychain_identity = str(o->os_identity);
#else
    if (o->os_identity) t.identity.keychain_identity = str(o->os_identity);  // refused by the OpenSSL backend
#endif
    t.expected_identity = str(o->expected_identity);
    t.insecure_skip_validation = o->insecure_skip_validation != 0;
    if (o->tls13_only) t.min_version = siaara::net::TlsMinVersion::Tls13;
    return t;
}

}  // namespace

extern "C" {

SIAARA_API int32_t SIAARA_CALL siaara_tls_available(void) {
    return guarded_value<int32_t>(0, [&]() -> int32_t {
        std::string why;
        if (siaara::net::tls_available(&why)) {
            (void)ok();
            return 1;
        }
        (void)fail(Errc::Unsupported, "TLS is not available: " + why);
        return 0;
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_web_client_create_tls(const char* base_url, size_t len, int32_t timeout_ms,
                                                                  const siaara_tls_options* options,
                                                                  siaara_web_client** out) {
    return guarded([&]() -> siaara_status {
        if (!base_url || !out) return fail(Errc::InvalidArgument, "base_url and out must not be NULL");
        *out = nullptr;
        if (timeout_ms < 0) return fail(Errc::InvalidArgument, "timeout_ms must not be negative");
        if (options && options->struct_size < sizeof(siaara_tls_options))
            return fail(Errc::InvalidArgument, "options->struct_size is too small");
        auto c = std::make_unique<siaara_web_client>();
        siaara::web::DicomwebClientConfig cfg;
        cfg.base_url.assign(base_url, len);
        const int t = timeout_ms == 0 ? 30000 : timeout_ms;
        cfg.limits.read_timeout_ms = t;
        cfg.limits.write_timeout_ms = t;
        cfg.limits.header_timeout_ms = t;
        cfg.connect_timeout_ms = t;
        cfg.tls = tls_from_options(options);
        cfg.tls.handshake_timeout_ms = t;
        siaara_web_client* raw = c.get();
        cfg.authorization = [raw]() { return raw->authorization; };
        c->client.emplace(cfg);
        siaara::net::tls_wipe_secret(cfg.tls.identity.password);
        if (Status s = c->client->status(); !s) return fail(s);
        *out = c.release();
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- clinical objects (appended)
#include "clinical/geometry.hpp"
#include "clinical/measure.hpp"
#include "clinical/rtstruct.hpp"
#include "clinical/seg.hpp"
#include "clinical/sr.hpp"

struct siaara_sr {
    siaara::clinical::SrDocument doc;
    siaara::capi::LiveHandle live_{};
};
struct siaara_seg {
    siaara::clinical::Segmentation seg;
    siaara::capi::LiveHandle live_{};
};
struct siaara_rtstruct {
    siaara::clinical::RtStructureSet rt;
    siaara::capi::LiveHandle live_{};
};

namespace {

namespace cl = siaara::clinical;

template <class T>
siaara_status copy_array(const T* data, size_t n, T* buf, size_t cap, size_t* needed) {
    if (needed) *needed = n;
    if (n == 0) return ok();
    if (!buf || cap < n) return fail(Errc::BufferTooSmall, "buffer too small");
    std::memcpy(buf, data, n * sizeof(T));
    return ok();
}

void copy_label(const std::string& s, char (&out)[4]) {
    const size_t n = std::min<size_t>(s.size(), 3);
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_frame_geometry(const siaara_file* file, uint32_t frame,
                                                           siaara_plane_geometry* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out || out->struct_size < sizeof(siaara_plane_geometry))
            return fail(Errc::InvalidArgument, "file and out (with struct_size) must be set");
        auto g = cl::frame_geometry(file->file.dataset, frame);
        if (!g) return fail(g.error());
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_plane_geometry));
        out->struct_size = size;
        out->rows = g->rows;
        out->columns = g->columns;
        out->has_position = g->has_position ? 1 : 0;
        out->has_orientation = g->has_orientation ? 1 : 0;
        out->spacing_source = static_cast<int32_t>(g->spacing.source);
        out->spacing_at_detector = g->spacing.at_detector ? 1 : 0;
        auto put3 = [](const cl::Vec3& v, double* d) { d[0] = v.x, d[1] = v.y, d[2] = v.z; };
        put3(g->position, out->position);
        put3(g->row_cosine, out->row_cosine);
        put3(g->column_cosine, out->column_cosine);
        put3(g->normal, out->normal);
        out->pixel_spacing[0] = g->spacing.row;
        out->pixel_spacing[1] = g->spacing.column;
        out->slice_thickness = g->slice_thickness.value_or(0.0);
        out->spacing_between_slices = g->spacing_between_slices.value_or(0.0);
        out->magnification = g->spacing.magnification;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_orientation_labels(const siaara_file* file, uint32_t frame,
                                                               siaara_edge_labels* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out || out->struct_size < sizeof(siaara_edge_labels))
            return fail(Errc::InvalidArgument, "file and out (with struct_size) must be set");
        auto g = cl::frame_geometry(file->file.dataset, frame);
        if (!g) return fail(g.error());
        auto e = cl::edge_labels(*g);
        if (!e) return fail(Errc::NotFound, "no Image Orientation (Patient)");
        copy_label(e->left, out->left);
        copy_label(e->right, out->right);
        copy_label(e->top, out->top);
        copy_label(e->bottom, out->bottom);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_series_sort(const siaara_file* const* files, size_t count, uint32_t flags,
                                                        uint32_t* out_instance, uint32_t* out_frame,
                                                        uint32_t* out_group, size_t cap, size_t* needed,
                                                        uint32_t* warnings) {
    return guarded([&]() -> siaara_status {
        if (count && !files) return fail(Errc::InvalidArgument, "files must not be NULL");
        std::vector<const siaara::Dataset*> ds(count, nullptr);
        for (size_t i = 0; i < count; ++i)
            if (files[i]) ds[i] = &files[i]->file.dataset;
        cl::SeriesSortOptions o;
        o.expand_frames = (flags & SIAARA_SORT_EXPAND_FRAMES) != 0;
        const cl::SeriesSort r = cl::sort_series(ds, o);
        if (warnings) *warnings = r.warnings;
        if (needed) *needed = r.order.size();
        if (r.order.empty()) return ok();
        if (!out_instance || cap < r.order.size()) return fail(Errc::BufferTooSmall, "buffer too small");
        std::vector<uint32_t> group_of(r.items.size(), 0);
        for (size_t g = 0; g < r.groups.size(); ++g)
            for (auto i : r.groups[g].items) group_of[i] = static_cast<uint32_t>(g);
        for (size_t k = 0; k < r.order.size(); ++k) {
            const auto& it = r.items[r.order[k]];
            out_instance[k] = it.instance;
            if (out_frame) out_frame[k] = it.frame;
            if (out_group) out_group[k] = group_of[r.order[k]];
        }
        return ok();
    });
}

// ---- Structured Reports
SIAARA_API siaara_status SIAARA_CALL siaara_sr_open(const siaara_file* file, siaara_sr** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = nullptr;
        auto r = cl::read_sr(file->file.dataset);
        if (!r) return fail(r.error());
        *out = new siaara_sr{std::move(r).value()};
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_sr_free(siaara_sr* sr) SIAARA_NOEXCEPT { delete sr; }

SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_count(const siaara_sr* sr, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!sr || !out) return fail(Errc::InvalidArgument, "sr and out must not be NULL");
        *out = static_cast<uint32_t>(sr->doc.nodes.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_sr_node(const siaara_sr* sr, uint32_t index, siaara_sr_node_info* out) {
    return guarded([&]() -> siaara_status {
        if (!sr || !out || out->struct_size < sizeof(siaara_sr_node_info))
            return fail(Errc::InvalidArgument, "sr and out (with struct_size) must be set");
        if (index >= sr->doc.nodes.size()) return fail(Errc::InvalidArgument, "node index out of range");
        const cl::SrNode& n = sr->doc.nodes[index];
        out->parent = n.parent;
        out->depth = n.depth;
        out->child_count = static_cast<uint32_t>(n.children.size());
        out->value_type = static_cast<int32_t>(n.value_type);
        out->relationship = static_cast<int32_t>(n.relationship);
        out->referenced_node = n.referenced_node;
        out->has_numeric = n.numeric ? 1 : 0;
        out->numeric = n.numeric.value_or(0.0);
        out->reference_count = static_cast<uint32_t>(n.references.size());
        out->graphic_point_values = static_cast<uint32_t>(n.graphic_data.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_children(const siaara_sr* sr, uint32_t index, uint32_t* buf,
                                                             size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!sr) return fail(Errc::InvalidArgument, "sr must not be NULL");
        if (index >= sr->doc.nodes.size()) return fail(Errc::InvalidArgument, "node index out of range");
        const auto& c = sr->doc.nodes[index].children;
        return copy_array(c.data(), c.size(), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_string(const siaara_sr* sr, uint32_t index, int32_t field,
                                                           char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!sr) return fail(Errc::InvalidArgument, "sr must not be NULL");
        if (index >= sr->doc.nodes.size()) return fail(Errc::InvalidArgument, "node index out of range");
        const cl::SrNode& n = sr->doc.nodes[index];
        static const std::string kEmpty;
        auto code_field = [&](const std::optional<cl::Code>& c, int which) -> const std::string& {
            if (!c) return kEmpty;
            return which == 0 ? c->value : (which == 1 ? c->scheme : c->meaning);
        };
        std::string tmp;
        const std::string* s = &kEmpty;
        switch (field) {
            case SIAARA_SRF_CONCEPT_VALUE: s = &code_field(n.concept_name, 0); break;
            case SIAARA_SRF_CONCEPT_SCHEME: s = &code_field(n.concept_name, 1); break;
            case SIAARA_SRF_CONCEPT_MEANING: s = &code_field(n.concept_name, 2); break;
            case SIAARA_SRF_TEXT: s = &n.text; break;
            case SIAARA_SRF_CODE_VALUE: s = &code_field(n.code, 0); break;
            case SIAARA_SRF_CODE_SCHEME: s = &code_field(n.code, 1); break;
            case SIAARA_SRF_CODE_MEANING: s = &code_field(n.code, 2); break;
            case SIAARA_SRF_NUMERIC_TEXT: s = &n.numeric_text; break;
            case SIAARA_SRF_UNITS_VALUE: s = &code_field(n.units, 0); break;
            case SIAARA_SRF_UNITS_SCHEME: s = &code_field(n.units, 1); break;
            case SIAARA_SRF_UNITS_MEANING: s = &code_field(n.units, 2); break;
            case SIAARA_SRF_REFERENCED_SOP_CLASS:
                if (!n.references.empty()) s = &n.references.front().sop_class_uid;
                break;
            case SIAARA_SRF_REFERENCED_SOP_INSTANCE:
                if (!n.references.empty()) s = &n.references.front().sop_instance_uid;
                break;
            case SIAARA_SRF_GRAPHIC_TYPE: s = &n.graphic_type; break;
            case SIAARA_SRF_VALUE_TYPE: tmp = std::string(cl::value_type_name(n.value_type)); s = &tmp; break;
            case SIAARA_SRF_RELATIONSHIP: s = &n.relationship_text; break;
            case SIAARA_SRF_CONTINUITY: s = &n.continuity; break;
            case SIAARA_SRF_TEMPLATE_ID: s = &n.template_id; break;
            case SIAARA_SRF_POSITION:
                for (size_t i = 0; i < n.position.size(); ++i) {
                    if (i) tmp.push_back('.');
                    tmp += std::to_string(n.position[i]);
                }
                s = &tmp;
                break;
            default: return fail(Errc::InvalidArgument, "unknown field");
        }
        return copy_out(*s, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_graphic_data(const siaara_sr* sr, uint32_t index, double* buf,
                                                                 size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!sr) return fail(Errc::InvalidArgument, "sr must not be NULL");
        if (index >= sr->doc.nodes.size()) return fail(Errc::InvalidArgument, "node index out of range");
        const auto& g = sr->doc.nodes[index].graphic_data;
        return copy_array(g.data(), g.size(), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_sr_to_text(const siaara_sr* sr, char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!sr) return fail(Errc::InvalidArgument, "sr must not be NULL");
        return copy_out(cl::sr_to_text(sr->doc), buf, cap, needed);
    });
}

// ---- Segmentation
SIAARA_API siaara_status SIAARA_CALL siaara_seg_open(const siaara_file* file, siaara_seg** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = nullptr;
        auto r = cl::read_segmentation(file->file.dataset);
        if (!r) return fail(r.error());
        *out = new siaara_seg{std::move(r).value()};
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_seg_free(siaara_seg* seg) SIAARA_NOEXCEPT { delete seg; }

SIAARA_API siaara_status SIAARA_CALL siaara_seg_info(const siaara_seg* seg, int32_t* type, uint32_t* frames,
                                                     uint32_t* max_fractional) {
    return guarded([&]() -> siaara_status {
        if (!seg) return fail(Errc::InvalidArgument, "seg must not be NULL");
        if (type) *type = static_cast<int32_t>(seg->seg.type);
        if (frames) *frames = seg->seg.number_of_frames;
        if (max_fractional) *max_fractional = seg->seg.max_fractional;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_seg_segment_count(const siaara_seg* seg, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!seg || !out) return fail(Errc::InvalidArgument, "seg and out must not be NULL");
        *out = static_cast<uint32_t>(seg->seg.segments.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_seg_segment(const siaara_seg* seg, uint32_t index, siaara_segment_info* out) {
    return guarded([&]() -> siaara_status {
        if (!seg || !out || out->struct_size < sizeof(siaara_segment_info))
            return fail(Errc::InvalidArgument, "seg and out (with struct_size) must be set");
        if (index >= seg->seg.segments.size()) return fail(Errc::InvalidArgument, "segment index out of range");
        const cl::Segment& s = seg->seg.segments[index];
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_segment_info));
        out->struct_size = size;
        out->number = s.number;
        out->frame_count = seg->seg.frame_count(s);
        out->has_color = s.rgb ? 1 : 0;
        if (s.rgb) std::memcpy(out->rgb, s.rgb->data(), 3);
        if (s.cielab)
            for (int k = 0; k < 3; ++k) out->cielab[k] = (*s.cielab)[static_cast<size_t>(k)];
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_seg_segment_label(const siaara_seg* seg, uint32_t index, char* buf,
                                                              size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!seg) return fail(Errc::InvalidArgument, "seg must not be NULL");
        if (index >= seg->seg.segments.size()) return fail(Errc::InvalidArgument, "segment index out of range");
        return copy_out(seg->seg.segments[index].label, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_seg_frames_for_source(const siaara_seg* seg, uint32_t segment_number,
                                                                  const char* sop, size_t len, uint32_t frame,
                                                                  uint32_t* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!seg || !sop) return fail(Errc::InvalidArgument, "seg and sop_instance_uid must not be NULL");
        const auto f = seg->seg.frames_for_source(segment_number, std::string_view(sop, len), frame);
        return copy_array(f.data(), f.size(), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_seg_mask(const siaara_file* file, const siaara_seg* seg, uint32_t frame,
                                                     uint32_t segment_number, uint8_t* buf, size_t cap,
                                                     size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file || !seg) return fail(Errc::InvalidArgument, "file and seg must not be NULL");
        auto m = cl::segment_mask(file->file.dataset, file->file.transfer_syntax, seg->seg, frame, segment_number);
        if (!m) return fail(m.error());
        return copy_array(m->data(), m->size(), buf, cap, needed);
    });
}

// ---- RT Dose
SIAARA_API siaara_status SIAARA_CALL siaara_rt_dose_info_get(const siaara_dataset* ds, siaara_rt_dose_info* out) {
    return guarded([&]() -> siaara_status {
        if (!ds || !out) return fail(Errc::InvalidArgument, "ds and out must not be NULL");
        if (out->struct_size < sizeof(uint32_t) * 2) return fail(Errc::InvalidArgument, "out must be set up with struct_size");
        const siaara::image::RtDoseInfo d = siaara::image::read_rt_dose_info(*ds_cast(ds));
        siaara_rt_dose_info r{};
        r.struct_size = out->struct_size < sizeof r ? out->struct_size : static_cast<uint32_t>(sizeof r);
        r.is_rt_dose = d.is_rt_dose ? 1 : 0;
        r.has_dose_grid_scaling = d.dose_grid_scaling ? 1 : 0;
        r.dose_grid_scaling = d.dose_grid_scaling.value_or(1.0);
        using F = siaara::image::RtDoseInfo::OffsetForm;
        r.grid_frame_offset_form = d.offset_form == F::Relative ? SIAARA_GFOV_RELATIVE
                                   : d.offset_form == F::Absolute ? SIAARA_GFOV_ABSOLUTE : SIAARA_GFOV_NONE;
        r.grid_frame_offset_count = static_cast<uint32_t>(std::min<std::size_t>(d.offset_count, UINT32_MAX));
        const auto put = [](char* dst, std::size_t cap, const std::string& v) {
            const std::size_t n = std::min(v.size(), cap - 1);
            std::memcpy(dst, v.data(), n);
            dst[n] = 0;
        };
        put(r.dose_units, sizeof r.dose_units, d.dose_units);
        put(r.dose_type, sizeof r.dose_type, d.dose_type);
        put(r.dose_summation_type, sizeof r.dose_summation_type, d.dose_summation_type);
        std::memcpy(out, &r, r.struct_size);
        return ok();
    });
}

// ---- RT Structure Set
SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_open(const siaara_file* file, siaara_rtstruct** out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        *out = nullptr;
        auto r = cl::read_rtstruct(file->file.dataset);
        if (!r) return fail(r.error());
        *out = new siaara_rtstruct{std::move(r).value()};
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_rtstruct_free(siaara_rtstruct* rt) SIAARA_NOEXCEPT { delete rt; }

SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_roi_count(const siaara_rtstruct* rt, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!rt || !out) return fail(Errc::InvalidArgument, "rt and out must not be NULL");
        *out = static_cast<uint32_t>(rt->rt.rois.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_roi(const siaara_rtstruct* rt, uint32_t index, siaara_rt_roi_info* out) {
    return guarded([&]() -> siaara_status {
        if (!rt || !out || out->struct_size < sizeof(siaara_rt_roi_info))
            return fail(Errc::InvalidArgument, "rt and out (with struct_size) must be set");
        if (index >= rt->rt.rois.size()) return fail(Errc::InvalidArgument, "ROI index out of range");
        const cl::RtRoi& r = rt->rt.rois[index];
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_rt_roi_info));
        out->struct_size = size;
        out->number = static_cast<int32_t>(std::clamp<std::int64_t>(r.number, INT32_MIN, INT32_MAX));
        out->contour_count = static_cast<uint32_t>(r.contours.size());
        out->has_color = r.color ? 1 : 0;
        if (r.color) std::memcpy(out->rgb, r.color->data(), 3);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_roi_name(const siaara_rtstruct* rt, uint32_t index, char* buf,
                                                              size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!rt) return fail(Errc::InvalidArgument, "rt must not be NULL");
        if (index >= rt->rt.rois.size()) return fail(Errc::InvalidArgument, "ROI index out of range");
        return copy_out(rt->rt.rois[index].name, buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_contour_points(const siaara_rtstruct* rt, uint32_t roi,
                                                                    uint32_t contour, double* buf, size_t cap,
                                                                    size_t* needed, int32_t* closed) {
    return guarded([&]() -> siaara_status {
        if (!rt) return fail(Errc::InvalidArgument, "rt must not be NULL");
        if (roi >= rt->rt.rois.size() || contour >= rt->rt.rois[roi].contours.size())
            return fail(Errc::InvalidArgument, "ROI or contour index out of range");
        const cl::RtContour& c = rt->rt.rois[roi].contours[contour];
        if (closed) *closed = c.geometric_type == "CLOSED_PLANAR" ? 1 : 0;
        std::vector<double> xyz;
        xyz.reserve(c.points.size() * 3);
        for (const auto& p : c.points) {
            xyz.push_back(p.x);
            xyz.push_back(p.y);
            xyz.push_back(p.z);
        }
        return copy_array(xyz.data(), xyz.size(), buf, cap, needed);
    });
}

}  // extern "C"

// ---------------------------------------------------------------- transcoding (codec/transcode.hpp)
#include "codec/transcode.hpp"

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_file_transcode(siaara_file* file, const char* ts_uid,
                                                           const siaara_transcode_params* params) {
    return guarded([&]() -> siaara_status {
        if (!file || !ts_uid) return fail(Errc::InvalidArgument, "file and ts_uid must not be NULL");
        siaara::codec::TranscodeParams p;
        if (params) {
            if (params->struct_size < sizeof(uint32_t) + 7 * sizeof(int32_t))
                return fail(Errc::InvalidArgument, "struct_size too small");
            if (params->quality) p.quality = params->quality;
            p.lossy = params->lossy != 0;
            if (params->near_lossless) p.near_lossless = params->near_lossless;
            p.allow_lossy = params->allow_lossy != 0;
            if (params->predictor) p.predictor = params->predictor;
            p.chroma_subsampling = params->chroma_subsampling;
            switch (params->offset_table) {
                case SIAARA_OFFSET_TABLE_AUTO: p.offset_table = siaara::codec::OffsetTableMode::Auto; break;
                case SIAARA_OFFSET_TABLE_BASIC: p.offset_table = siaara::codec::OffsetTableMode::Basic; break;
                case SIAARA_OFFSET_TABLE_EXTENDED: p.offset_table = siaara::codec::OffsetTableMode::Extended; break;
                case SIAARA_OFFSET_TABLE_EMPTY: p.offset_table = siaara::codec::OffsetTableMode::Empty; break;
                default: return fail(Errc::InvalidArgument, "unknown offset_table");
            }
            if (params->struct_size >= offsetof(siaara_transcode_params, keep_uid_on_lossy) + sizeof(int32_t)) {
                if (params->keep_uid_on_lossy != 0 && params->keep_uid_on_lossy != 1)
                    return fail(Errc::InvalidArgument, "keep_uid_on_lossy must be 0 or 1");
                p.keep_uid_on_lossy = params->keep_uid_on_lossy == 1;
            }
        }
        if (Status s = siaara::codec::transcode_file(file->file, ts_uid, p); !s) return fail(s);
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- volumes and 3D views (appended)
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "volume/render3d.hpp"
#include "volume/volume.hpp"

struct siaara_view {
    siaara::volume::View view;
    siaara::capi::LiveHandle live_{};
};

namespace {

namespace vol = siaara::volume;

bool finite3(const double* p) { return p && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_volume_build(const char* const* utf8_paths, const size_t* path_lens,
                                                         uint32_t count, const siaara_volume_options* options,
                                                         siaara_volume** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!utf8_paths || count == 0) return fail(Errc::InvalidArgument, "no input paths");
        if (options && options->struct_size < sizeof(siaara_volume_options))
            return fail(Errc::InvalidArgument, "options->struct_size is too small");
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
        std::vector<std::string> paths;
        paths.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!utf8_paths[i]) return fail(Errc::InvalidArgument, "path is NULL");
            paths.emplace_back(utf8_paths[i], path_lens ? path_lens[i] : std::strlen(utf8_paths[i]));
        }
        auto v = vol::build_volume_from_paths(paths, o);
        if (!v) return fail(v.error());
        auto h = std::make_unique<siaara_volume>();
        h->vol = std::make_shared<const vol::Volume>(std::move(v).value());
        *out = h.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_volume_info(const siaara_volume* volume, siaara_volume_properties* out) {
    return guarded([&]() -> siaara_status {
        if (!volume || !out) return fail(Errc::InvalidArgument, "volume and out must not be NULL");
        if (out->struct_size < sizeof(siaara_volume_properties)) return fail(Errc::InvalidArgument, "out->struct_size is too small");
        const vol::Volume& v = *volume->vol;
        const vol::Grid& g = v.grid();
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(siaara_volume_properties));
        out->struct_size = size;
        for (int a = 0; a < 3; ++a) {
            const auto ua = static_cast<std::size_t>(a);
            out->dims[a] = g.dims[ua];
            out->spacing[a] = g.spacing[ua];
            out->origin[a] = g.origin[a];
            for (int r = 0; r < 3; ++r) out->direction[r * 3 + a] = g.axes[ua][r];
        }
        out->voxel_type = v.type() == vol::VoxelType::Int16 ? SIAARA_VOXEL_INT16 : SIAARA_VOXEL_FLOAT32;
        out->flags = v.flags;
        out->value_min = v.value_min;
        out->value_max = v.value_max;
        out->phase_count = v.phase_count;
        out->phase_index = v.phase_index;
        out->tilt_degrees = v.tilt_degrees;
        out->warning_count = static_cast<uint32_t>(v.warnings.size());
        out->histogram_bins = static_cast<uint32_t>(std::min<std::size_t>(256, v.histogram.bins.size()));
        out->histogram_min = v.histogram.min;
        out->histogram_max = v.histogram.max;
        for (uint32_t b = 0; b < out->histogram_bins; ++b) out->histogram[b] = v.histogram.bins[b];
        out->data_bytes = v.byte_size();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_volume_warning(const siaara_volume* volume, uint32_t index, char* buf,
                                                           size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!volume) return fail(Errc::InvalidArgument, "volume must not be NULL");
        if (index >= volume->vol->warnings.size()) return fail(Errc::InvalidArgument, "warning index out of range");
        return copy_out(volume->vol->warnings[index], buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_volume_data(const siaara_volume* volume, const void** out_data,
                                                        uint64_t* out_bytes) {
    return guarded([&]() -> siaara_status {
        if (!volume || !out_data) return fail(Errc::InvalidArgument, "volume and out_data must not be NULL");
        *out_data = volume->vol->data();
        if (out_bytes) *out_bytes = volume->vol->byte_size();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_volume_free(siaara_volume* volume) SIAARA_NOEXCEPT { delete volume; }

SIAARA_API siaara_status SIAARA_CALL siaara_view_create(const siaara_volume* volume, uint32_t width, uint32_t height,
                                                        siaara_view** out) {
    return guarded([&]() -> siaara_status {
        if (!volume || !out) return fail(Errc::InvalidArgument, "volume and out must not be NULL");
        *out = nullptr;
        auto v = vol::View::create(volume->vol, width, height);
        if (!v) return fail(v.error());
        auto h = std::make_unique<siaara_view>();
        h->view = std::move(v).value();
        *out = h.release();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_view_free(siaara_view* view) SIAARA_NOEXCEPT { delete view; }

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_plane(siaara_view* view, const double point[3],
                                                           const double normal[3], const double up[3],
                                                           double pixel_spacing) {
    return guarded([&]() -> siaara_status {
        if (!view || !finite3(point) || !finite3(normal) || !finite3(up))
            return fail(Errc::InvalidArgument, "view must not be NULL and the vectors must be finite");
        vol::Plane p;
        p.point = {point[0], point[1], point[2]};
        p.normal = {normal[0], normal[1], normal[2]};
        p.up = {up[0], up[1], up[2]};
        p.pixel_spacing = pixel_spacing;
        if (Status s = view->view.set_plane(p); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_mode(siaara_view* view, int32_t mode, int32_t interpolation,
                                                          int32_t slab, double slab_thickness, int32_t quality) {
    return guarded([&]() -> siaara_status {
        if (!view) return fail(Errc::InvalidArgument, "view must not be NULL");
        if (mode < SIAARA_VIEW_MPR || mode > SIAARA_VIEW_VOLUME_MINIP) return fail(Errc::InvalidArgument, "unknown mode");
        if (interpolation < SIAARA_INTERP_NEAREST || interpolation > SIAARA_INTERP_CUBIC_BSPLINE)
            return fail(Errc::InvalidArgument, "unknown interpolation");
        if (slab < SIAARA_SLAB_NONE || slab > SIAARA_SLAB_AVERAGE) return fail(Errc::InvalidArgument, "unknown slab mode");
        if (quality != SIAARA_QUALITY_INTERACTIVE && quality != SIAARA_QUALITY_FINAL)
            return fail(Errc::InvalidArgument, "unknown quality");
        if (Status s = view->view.set_mode(static_cast<vol::ViewMode>(mode), static_cast<vol::Interpolation>(interpolation),
                                           static_cast<vol::SlabMode>(slab), slab_thickness);
            !s)
            return fail(s);
        view->view.set_quality(quality == SIAARA_QUALITY_FINAL ? vol::Quality::Final : vol::Quality::Interactive);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_window(siaara_view* view, int32_t layer, double center,
                                                            double width) {
    return guarded([&]() -> siaara_status {
        if (!view) return fail(Errc::InvalidArgument, "view must not be NULL");
        if (Status s = view->view.set_window(layer, center, width); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_colormap(siaara_view* view, int32_t layer, int32_t colormap) {
    return guarded([&]() -> siaara_status {
        if (!view) return fail(Errc::InvalidArgument, "view must not be NULL");
        if (colormap < SIAARA_COLORMAP_GRAY || colormap > SIAARA_COLORMAP_PET_20_STEP)
            return fail(Errc::InvalidArgument, "unknown colormap");
        if (Status s = view->view.set_colormap(layer, static_cast<vol::ColorMap>(colormap)); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_curve(siaara_view* view, const double* points, uint32_t count,
                                                           int32_t spline) {
    return guarded([&]() -> siaara_status {
        if (!view || (!points && count)) return fail(Errc::InvalidArgument, "view and points must not be NULL");
        std::vector<vol::Vec3> pts;
        pts.reserve(count);
        for (std::size_t i = 0; i < count; ++i) pts.push_back({points[3 * i], points[3 * i + 1], points[3 * i + 2]});
        if (Status s = view->view.set_curve(pts, spline != 0); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_transfer_function(siaara_view* view,
                                                                       const siaara_transfer_function* tf) {
    return guarded([&]() -> siaara_status {
        if (!view || !tf) return fail(Errc::InvalidArgument, "view and tf must not be NULL");
        if (tf->struct_size < sizeof(siaara_transfer_function)) return fail(Errc::InvalidArgument, "tf->struct_size is too small");
        if ((!tf->opacity_points && tf->opacity_count) || (!tf->color_points && tf->color_count) ||
            (!tf->gradient_points && tf->gradient_count))
            return fail(Errc::InvalidArgument, "point arrays must not be NULL");
        if (tf->opacity_count > 65536 || tf->color_count > 65536 || tf->gradient_count > 65536)
            return fail(Errc::LimitExceeded, "too many transfer function points");
        vol::TransferFunction t;
        for (std::size_t i = 0; i < tf->opacity_count; ++i) t.opacity.push_back({tf->opacity_points[2 * i], tf->opacity_points[2 * i + 1]});
        for (std::size_t i = 0; i < tf->gradient_count; ++i)
            t.gradient_opacity.push_back({tf->gradient_points[2 * i], tf->gradient_points[2 * i + 1]});
        for (std::size_t i = 0; i < tf->color_count; ++i) {
            const double* c = tf->color_points + 4 * i;
            t.color.push_back({c[0], c[1], c[2], c[3]});
        }
        t.shading = tf->shading != 0;
        if (tf->ambient != 0.0 || tf->diffuse != 0.0 || tf->specular != 0.0 || tf->shininess != 0.0) {
            t.ambient = tf->ambient;
            t.diffuse = tf->diffuse;
            t.specular = tf->specular;
            t.shininess = tf->shininess;
        }
        if (tf->unit_distance != 0.0) t.unit_distance = tf->unit_distance;
        if (tf->sample_distance != 0.0) {
            vol::RenderSettings rs;
            rs.sample_distance = tf->sample_distance;
            if (Status s = view->view.set_render_settings(rs); !s) return fail(s);
        }
        if (Status s = view->view.set_transfer_function(t); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_fusion_layer(siaara_view* view, const siaara_volume* overlay,
                                                                  const siaara_fusion_options* options) {
    return guarded([&]() -> siaara_status {
        if (!view) return fail(Errc::InvalidArgument, "view must not be NULL");
        if (options && options->struct_size < sizeof(siaara_fusion_options))
            return fail(Errc::InvalidArgument, "options->struct_size is too small");
        vol::LayerStyle st;
        st.colormap = vol::ColorMap::Pet;
        st.opacity = 0.5;
        bool pre = false;
        if (options) {
            if (options->colormap < SIAARA_COLORMAP_GRAY || options->colormap > SIAARA_COLORMAP_PET_20_STEP)
                return fail(Errc::InvalidArgument, "unknown colormap");
            st.colormap = static_cast<vol::ColorMap>(options->colormap);
            st.window_center = options->window_center;
            st.window_width = options->window_width;
            st.opacity = options->opacity;
            st.transparent_below_window = options->opaque_below_window == 0;
            pre = options->preresample != 0;
        }
        if (Status s = view->view.set_fusion_layer(overlay ? overlay->vol : nullptr, st, pre); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_render_rgba8(siaara_view* view, uint8_t* buf, size_t cap,
                                                              size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!view) return fail(Errc::InvalidArgument, "view must not be NULL");
        const std::uint64_t need = std::uint64_t{view->view.width()} * view->view.height() * 4u;
        bool query = false;
        if (siaara_status st = check_out_buffer(need, buf, cap, needed, query); st != SIAARA_OK || query) return st;
        if (Status s = view->view.render_rgba8(std::span<std::uint8_t>(buf, static_cast<size_t>(need))); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_pick(const siaara_view* view, double x, double y,
                                                      siaara_pick_result* out) {
    return guarded([&]() -> siaara_status {
        if (!view || !out) return fail(Errc::InvalidArgument, "view and out must not be NULL");
        if (out->struct_size < sizeof(siaara_pick_result)) return fail(Errc::InvalidArgument, "out->struct_size is too small");
        auto r = view->view.pick(x, y);
        if (!r) return fail(r.error());
        out->hit = r->hit ? 1 : 0;
        out->inside = r->inside ? 1 : 0;
        for (int a = 0; a < 3; ++a) {
            out->patient[a] = r->patient[a];
            out->index[a] = r->index[a];
        }
        out->value = r->value;
        out->overlay_inside = r->overlay_inside ? 1 : 0;
        out->overlay_value = r->overlay_value;
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- GPU rendering (appended)
#include "gpu/gpu.hpp"

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_gpu_devices(siaara_gpu_device_info* out, uint32_t cap, uint32_t* count) {
    return guarded([&]() -> siaara_status {
        if (!count) return fail(Errc::InvalidArgument, "count must not be NULL");
        if (cap && !out) return fail(Errc::InvalidArgument, "out must not be NULL when cap > 0");
        for (uint32_t i = 0; i < cap; ++i)
            if (out[i].struct_size < sizeof(siaara_gpu_device_info))
                return fail(Errc::InvalidArgument, "out[i].struct_size is too small");
        const std::vector<siaara::gpu::DeviceInfo> devs = siaara::gpu::devices();
        *count = static_cast<uint32_t>(devs.size());
        for (uint32_t i = 0; i < cap && i < devs.size(); ++i) {
            const auto& d = devs[i];
            siaara_gpu_device_info& o = out[i];
            const uint32_t size = o.struct_size;
            std::memset(&o, 0, sizeof o);
            o.struct_size = size;
            o.api = static_cast<int32_t>(d.api);
            o.type = static_cast<int32_t>(d.type);
            o.vendor_id = d.vendor_id;
            o.device_id = d.device_id;
            o.memory_bytes = d.memory_bytes;
            o.max_texture_3d = d.max_texture_3d;
            o.selected = d.selected ? 1 : 0;
            o.float32_filtering = d.float32_filtering ? 1 : 0;
            o.int16_filtering = d.int16_filtering ? 1 : 0;
            std::memcpy(o.name, d.name.data(), std::min(d.name.size(), sizeof o.name - 1));
            std::memcpy(o.driver, d.driver.data(), std::min(d.driver.size(), sizeof o.driver - 1));
        }
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_set_backend(siaara_view* view, int32_t backend) {
    return guarded([&]() -> siaara_status {
        if (!view) return fail(Errc::InvalidArgument, "view must not be NULL");
        vol::RenderBackend b;
        switch (backend) {
            case SIAARA_BACKEND_AUTO: b = vol::RenderBackend::Auto; break;
            case SIAARA_BACKEND_CPU: b = vol::RenderBackend::Cpu; break;
            case SIAARA_BACKEND_GPU: b = vol::RenderBackend::Gpu; break;
            default: return fail(Errc::InvalidArgument, "unknown backend");
        }
        if (Status s = view->view.set_backend(b); !s) return fail(s);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_view_last_backend(const siaara_view* view, int32_t* out_backend) {
    return guarded([&]() -> siaara_status {
        if (!view || !out_backend) return fail(Errc::InvalidArgument, "view and out_backend must not be NULL");
        *out_backend = view->view.last_backend() == vol::RenderBackend::Gpu ? SIAARA_BACKEND_GPU : SIAARA_BACKEND_CPU;
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_file_get_raw_stored(const siaara_file* file, const siaara_dataset* ds,
                                                                uint32_t tag, uint8_t* buf, size_t cap,
                                                                size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file || !ds || !needed || (!buf && cap)) return fail(Errc::InvalidArgument, "file, ds and needed must not be NULL");
        const siaara::Dataset* d = ds_cast(ds);
        const auto* e = d->find(tag);
        if (!e) return fail(Errc::NotFound, "element not present");
        // The file meta group is always explicit VR little endian (PS3.10 7.1).
        const bool meta = d == &file->file.meta;
        const std::uint16_t bits = static_cast<std::uint16_t>(d->get_int(siaara::tags::BitsAllocated).value_or(0));
        std::vector<std::uint8_t> v;
        if (Status s = siaara::stored_value_bytes(*e, meta ? std::string_view(siaara::ts::ExplicitVRLittleEndian)
                                                           : std::string_view(file->file.transfer_syntax),
                                                  bits, v, d == &file->file.dataset);
            !s)
            return fail(s);
        *needed = v.size();
        if (cap < v.size()) return fail(Errc::BufferTooSmall, "buffer too small");
        if (!v.empty()) std::memcpy(buf, v.data(), v.size());
        return ok();
    });
}

}  // extern "C"

// ---------------------------------------------------------------- lenient frame decoding
namespace {

siaara_status frame_warnings_of(const siaara_file* file, uint32_t frame, std::vector<siaara::image::FrameWarning>& out) {
    auto fr = siaara::image::FrameReader::open(file->file.dataset, file->file.transfer_syntax);
    if (!fr) return fail(fr.error());
    if (frame >= fr->frame_count()) return fail(Errc::InvalidArgument, "frame index out of range");
    // Decoding (not only the header probe) also finds the repairs made inside the codecs.
    auto px = fr->decode(frame);
    if (!px) return fail(px.error());
    if (Status s = fr->decode(frame, *px, &out); !s) return fail(s);
    return SIAARA_OK;
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_frame_warning_count(const siaara_file* file, uint32_t frame, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!file || !out) return fail(Errc::InvalidArgument, "file and out must not be NULL");
        std::vector<siaara::image::FrameWarning> w;
        if (siaara_status st = frame_warnings_of(file, frame, w); st != SIAARA_OK) return st;
        *out = static_cast<uint32_t>(w.size());
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_frame_warning(const siaara_file* file, uint32_t frame, uint32_t index,
                                                          int32_t* code, char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
        std::vector<siaara::image::FrameWarning> w;
        if (siaara_status st = frame_warnings_of(file, frame, w); st != SIAARA_OK) return st;
        if (index >= w.size()) return fail(Errc::InvalidArgument, "warning index out of range");
        if (code) *code = static_cast<int32_t>(w[index].code);
        return copy_out(w[index].message, buf, cap, needed);
    });
}

}  // extern "C"

// ================================================================ wip-capi: bridge for src/capi/capi_*.cpp
// The additions for the viewer / PACS application live in their own translation units; they
// share the thread's last error and the TLS option conversion through these functions.
namespace siaara::capi {

std::atomic<std::int64_t>& live_handle_counter() noexcept {
    static std::atomic<std::int64_t> n{0};
    return n;
}

std::atomic<std::uint64_t>& swallowed_exceptions() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

siaara_status set_error(Errc code, std::string_view msg) noexcept { return fail(code, msg); }
siaara_status set_error(const Status& s) noexcept { return fail(s); }
siaara_status clear_error() noexcept { return ok(); }

net::TlsSettings tls_settings(const siaara_tls_options* o) { return tls_from_options(o); }

}  // namespace siaara::capi
// ---------------------------------------------------------------- validation and repair (appended)
#include "repair/repair.hpp"

namespace {

Status to_repair_options(const siaara_repair_options* o, siaara::repair::RepairOptions& r) {
    if (!o) return Status::ok();
    if (o->struct_size < sizeof(uint32_t)) return Status(Errc::InvalidArgument, "options must be set up with struct_size");
    const auto has = [&](std::size_t end) { return o->struct_size >= end; };
    if (has(offsetof(siaara_repair_options, fix) + sizeof(uint32_t))) {
        if (o->fix & ~static_cast<uint32_t>(siaara::repair::kAllCategories))
            return Status(Errc::InvalidArgument, "unknown bits in fix");
        r.fix = o->fix;
    }
    if (has(offsetof(siaara_repair_options, write) + sizeof(int32_t))) r.write = o->write != 0;
    if (has(offsetof(siaara_repair_options, detailed_report) + sizeof(int32_t))) r.detailed_report = o->detailed_report != 0;
    if (has(offsetof(siaara_repair_options, charset_len) + sizeof(size_t)) && o->charset_len) {
        if (!o->charset) return Status(Errc::InvalidArgument, "charset is NULL");
        r.charset.assign(o->charset, o->charset_len);
    }
    if (has(offsetof(siaara_repair_options, recompute_group_lengths) + sizeof(int32_t)))
        r.recompute_group_lengths = o->recompute_group_lengths != 0;
    if (has(offsetof(siaara_repair_options, keep_last_duplicate) + sizeof(int32_t)) && o->keep_last_duplicate)
        r.duplicates = siaara::repair::DuplicatePolicy::KeepLast;
    if (has(offsetof(siaara_repair_options, overwrite) + sizeof(int32_t))) r.overwrite = o->overwrite != 0;
    if (has(offsetof(siaara_repair_options, uid_seed_len) + sizeof(size_t)) && o->uid_seed_len) {
        if (!o->uid_seed) return Status(Errc::InvalidArgument, "uid_seed is NULL");
        r.uid_seed.assign(o->uid_seed, o->uid_seed_len);
    }
    return Status::ok();
}

siaara_status findings_out(const siaara::Result<std::vector<siaara::repair::Finding>>& r, char* buf, size_t cap,
                           size_t* needed) {
    if (!r) return fail(r.error());
    siaara::repair::Report rep;
    siaara::repair::FileReport f;
    f.findings = r.value();
    rep.files.push_back(std::move(f));
    return copy_out(siaara::repair::to_json(rep), buf, cap, needed);
}

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_validate_json(const char* path, size_t path_len, char* buf, size_t cap,
                                                          size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!path) return fail(Errc::InvalidArgument, "path must not be NULL");
        return findings_out(siaara::repair::validate_file(std::string(path, path_len)), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_validate_memory_json(const uint8_t* data, size_t size, char* buf,
                                                                 size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!data && size) return fail(Errc::InvalidArgument, "data must not be NULL");
        return findings_out(siaara::repair::validate_bytes(siaara::Bytes::copy_of(data, size)), buf, cap, needed);
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_repair_file(const char* input_path, size_t input_len,
                                                        const char* output_path, size_t output_len,
                                                        const siaara_repair_options* options, char* buf,
                                                        size_t cap, size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (needed) *needed = 0;
        if (!input_path) return fail(Errc::InvalidArgument, "input_path must not be NULL");
        if (!output_path && output_len) return fail(Errc::InvalidArgument, "output_path is NULL");
        siaara::repair::RepairOptions o;
        if (Status s = to_repair_options(options, o); !s) return fail(s);
        const std::string out = output_path ? std::string(output_path, output_len) : std::string();
        auto r = siaara::repair::repair_file(std::string(input_path, input_len), out, o);
        if (!r) return fail(r.error());
        const siaara_status st = (buf || cap || needed) ? copy_out(siaara::repair::to_json(*r), buf, cap, needed)
                                                               : ok();
        const siaara::repair::FileReport& f = r->files.front();
        if (!f.input_unchanged) return fail(Errc::Internal, "the input file changed during the repair");
        if (!f.error.empty()) return fail(Errc::Rejected, f.error);
        return st;
    });
}

}  // extern "C"
