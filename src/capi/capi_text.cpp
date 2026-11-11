// SIAARA DICOM Toolkit - C API: the display-safe copy of text from outside (core/display_safe.hpp).
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#include "capi/capi_internal.hpp"
#include "core/display_safe.hpp"

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

}  // namespace

extern "C" {

SIAARA_API siaara_status SIAARA_CALL siaara_text_display_safe(const char* text, size_t len, char* buf, size_t cap,
                                                              size_t* needed) {
    return guarded([&]() -> siaara_status {
        if (!text && len) return fail(Errc::InvalidArgument, "text must not be NULL");
        const std::string s = siaara::text::display_safe(std::string_view(text ? text : "", len));
        if (needed) *needed = s.size();
        if (cap == 0) return buf ? fail(Errc::BufferTooSmall, "buffer too small") : ok();
        if (!buf) return fail(Errc::InvalidArgument, "buf is NULL");
        if (s.size() + 1 > cap) {
            // Never cut inside a character: a partial copy ends at a character boundary.
            std::size_t n = cap - 1;
            while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
            std::memcpy(buf, s.data(), n);
            buf[n] = '\0';
            return fail(Errc::BufferTooSmall, "buffer too small");
        }
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        return ok();
    });
}

}  // extern "C"
