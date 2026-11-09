// SIAARA DICOM Toolkit - internals shared by the C API translation units (src/capi/*.cpp).
// Not installed. capi.cpp holds the original API; capi_view2d.cpp, capi_net.cpp, capi_web.cpp
// and capi_misc.cpp hold the additions for the viewer / PACS application (branch wip-capi).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "siaara/siaara.h"
#include "core/reader.hpp"
#include "core/status.hpp"
#include "net/tls.hpp"
#include "store/store.hpp"
#include "volume/volume.hpp"
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

// Process-wide log callback (siaara_set_log_callback). Never throws.
void log(int level, bool phi, std::string_view message) noexcept;
bool log_enabled(int level) noexcept;
// A std::function suitable for ScpHandlers::log / TlsSettings::log (no patient data).
std::function<void(const std::string&)> log_sink(int level);

}  // namespace siaara::capi

// Cancellation token: a flag plus interrupt hooks registered by the calls that use it.
struct siaara_cancel {
    std::atomic<bool> flag{false};
    std::mutex mu;
    std::uint64_t next_id = 1;
    std::map<std::uint64_t, std::function<void()>> hooks;
    siaara::capi::LiveHandle live_{};

    bool is_set() const noexcept { return flag.load(std::memory_order_acquire); }
    void set() {
        flag.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> l(mu);
        for (auto& [id, fn] : hooks) {
            try {
                fn();
            } catch (...) {
            }
        }
    }
};

namespace siaara::capi {

// Registers `interrupt` with a token for the lifetime of the scope (runs it at once when the
// token is already set). A NULL token makes this a no-op.
class CancelScope {
public:
    CancelScope(siaara_cancel* c, std::function<void()> interrupt) : c_(c) {
        if (!c_) return;
        std::lock_guard<std::mutex> l(c_->mu);
        id_ = c_->next_id++;
        c_->hooks.emplace(id_, std::move(interrupt));
    }
    ~CancelScope() {
        if (!c_) return;
        std::lock_guard<std::mutex> l(c_->mu);
        c_->hooks.erase(id_);
    }
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;
    bool cancelled() const noexcept { return c_ && c_->is_set(); }
    // For C++ options that take a flag pointer.
    const std::atomic<bool>* flag() const noexcept { return c_ ? &c_->flag : nullptr; }

private:
    siaara_cancel* c_ = nullptr;
    std::uint64_t id_ = 0;
};

}  // namespace siaara::capi

struct siaara_file {
    siaara::DicomFile file;
    siaara::capi::LiveHandle live_{};
};

namespace siaara::capi {

// A token attached to a long-lived object (siaara_web_client_set_cancel): its hook stays
// registered until detach() or destruction.
class CancelAttachment {
public:
    CancelAttachment() = default;
    CancelAttachment(const CancelAttachment&) = delete;
    CancelAttachment& operator=(const CancelAttachment&) = delete;
    ~CancelAttachment() { detach(); }
    void attach(siaara_cancel* c, std::function<void()> fn) {
        detach();
        if (!c) return;
        std::lock_guard<std::mutex> l(c->mu);
        id_ = c->next_id++;
        c->hooks.emplace(id_, std::move(fn));
        c_ = c;
    }
    void detach() noexcept {
        if (!c_) return;
        std::lock_guard<std::mutex> l(c_->mu);
        c_->hooks.erase(id_);
        c_ = nullptr;
    }
    siaara_cancel* token() const noexcept { return c_; }

private:
    siaara_cancel* c_ = nullptr;
    std::uint64_t id_ = 0;
};

}  // namespace siaara::capi

struct siaara_web_client {
    std::optional<siaara::web::DicomwebClient> client;
    std::string authorization;
    std::string last_response;
    siaara::capi::CancelAttachment cancel;  // declared after `client`: detached before it is destroyed
    siaara::capi::LiveHandle live_{};
};

struct siaara_store {
    std::unique_ptr<siaara::store::Store> store;
    siaara::capi::LiveHandle live_{};
};

struct siaara_volume {
    std::shared_ptr<const siaara::volume::Volume> vol;
    siaara::capi::LiveHandle live_{};
};
