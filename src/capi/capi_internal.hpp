// SIAARA DICOM Toolkit - internals shared by the C API translation units (src/capi/*.cpp).
// Not installed. capi.cpp holds the original API; capi_view2d.cpp, capi_net.cpp, capi_web.cpp
// and capi_misc.cpp hold the additions for the viewer / PACS application (branch wip-capi).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "siaara/siaara.h"
#include "core/reader.hpp"
#include "core/status.hpp"
#include "net/tls.hpp"
#include "store/store.hpp"
#include "web/client.hpp"

namespace siaara::capi {

// Number of live C API handles (siaara_debug_live_handles). Each handle struct holds one
// LiveHandle member, so every allocation and free is counted without touching the functions.
std::atomic<std::int64_t>& live_handle_counter() noexcept;

struct LiveHandle {
    LiveHandle() noexcept { live_handle_counter().fetch_add(1, std::memory_order_relaxed); }
    LiveHandle(const LiveHandle&) noexcept { live_handle_counter().fetch_add(1, std::memory_order_relaxed); }
    LiveHandle& operator=(const LiveHandle&) noexcept { return *this; }
    ~LiveHandle() { live_handle_counter().fetch_sub(1, std::memory_order_relaxed); }
};

// The calling thread's last error (siaara_last_error_message), shared with capi.cpp.
siaara_status set_error(Errc code, std::string_view msg) noexcept;
siaara_status set_error(const Status& s) noexcept;
siaara_status clear_error() noexcept;

// siaara_tls_options -> TlsSettings (enabled), as siaara_web_client_create_tls reads them.
net::TlsSettings tls_settings(const siaara_tls_options* o);

}  // namespace siaara::capi

struct siaara_file {
    siaara::DicomFile file;
    siaara::capi::LiveHandle live_{};
};

struct siaara_web_client {
    std::optional<siaara::web::DicomwebClient> client;
    std::string authorization;
    std::string last_response;
    siaara::capi::LiveHandle live_{};
};

struct siaara_store {
    std::unique_ptr<siaara::store::Store> store;
    siaara::capi::LiveHandle live_{};
};
