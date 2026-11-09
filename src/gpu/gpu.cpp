// SIAARA DICOM Toolkit - GPU device registry: probing, selection, test hooks (see gpu.hpp).
#include <algorithm>
#include <atomic>
#include <new>

#include "gpu/gpu_backends.hpp"

namespace siaara::gpu {

namespace {

std::atomic<std::int64_t> g_objects{0};
std::atomic<bool> g_force_manual{false};
std::atomic<bool> g_metal_enabled{true};
std::atomic<std::uint64_t> g_upload_chunk{0};

struct Source {
    Api api = Api::None;
    std::size_t index = 0;  // within the backend's own probe
};

struct Registry {
    std::mutex m;
    bool probed = false;
    std::vector<DeviceInfo> devices;
    std::vector<Source> sources;
    int selected = -1;
    std::string reason;
    std::shared_ptr<Backend> active;
    bool create_failed = false;
    bool has_loader_override = false;
    std::string loader_override;
};

// Never destroyed: the selected device lives until the process ends (or testing::reset()), and
// no Vulkan / Metal object is released from a static destructor after the drivers may be gone.
Registry& reg() {
    static Registry* r = new Registry();
    return *r;
}

int rank(DeviceType t) {
    switch (t) {
        case DeviceType::Discrete: return 0;
        case DeviceType::Integrated: return 1;
        case DeviceType::Virtual: return 2;
        case DeviceType::Other: return 3;
        case DeviceType::Cpu: return 4;
    }
    return 5;
}

void ensure_probed(Registry& r) {
    if (r.probed) return;
    r.probed = true;
    r.devices.clear();
    r.sources.clear();
    r.selected = -1;
    std::string reasons;
    try {
#if defined(__APPLE__)
        {
            detail::Probe p;
            if (g_metal_enabled.load()) p = metal::probe();
            else p.reason = "metal: disabled";
            for (std::size_t i = 0; i < p.devices.size(); ++i) {
                r.devices.push_back(p.devices[i]);
                r.sources.push_back({Api::Metal, i});
            }
            if (p.devices.empty()) reasons = p.reason;
        }
#endif
        {
            const std::string name = r.has_loader_override ? r.loader_override : vulkan::default_loader_name();
            detail::Probe p = vulkan::probe(name);
            for (std::size_t i = 0; i < p.devices.size(); ++i) {
                r.devices.push_back(p.devices[i]);
                r.sources.push_back({Api::Vulkan, i});
            }
            if (p.devices.empty()) reasons += (reasons.empty() ? "" : "; ") + p.reason;
        }
    } catch (const std::bad_alloc&) {
        r.devices.clear();
        r.sources.clear();
        reasons = "out of memory";
    }
    // Best rank wins; earlier entries (Metal before Vulkan on Apple) win ties.
    for (std::size_t i = 0; i < r.devices.size(); ++i)
        if (r.selected < 0 || rank(r.devices[i].type) < rank(r.devices[static_cast<std::size_t>(r.selected)].type))
            r.selected = static_cast<int>(i);
    if (r.selected >= 0) r.devices[static_cast<std::size_t>(r.selected)].selected = true;
    r.reason = r.devices.empty() ? (reasons.empty() ? std::string("no GPU device") : reasons) : std::string();
}

}  // namespace

namespace detail {
void count_object(int delta) noexcept { g_objects.fetch_add(delta, std::memory_order_relaxed); }
std::uint64_t upload_chunk_bytes() noexcept {
    const std::uint64_t v = g_upload_chunk.load();
    return v ? v : (64ull << 20);
}
}  // namespace detail

std::vector<DeviceInfo> devices() {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.m);
    ensure_probed(r);
    return r.devices;
}

std::shared_ptr<Backend> acquire() {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.m);
    ensure_probed(r);
    if (r.active) return r.active->lost() ? nullptr : r.active;
    if (r.create_failed || r.selected < 0) return nullptr;
    const Source src = r.sources[static_cast<std::size_t>(r.selected)];
    Result<std::shared_ptr<Backend>> b = Status(Errc::Unsupported, "no backend");
#if defined(__APPLE__)
    if (src.api == Api::Metal) b = metal::create(src.index);
#endif
    if (src.api == Api::Vulkan) b = vulkan::create(src.index);
    if (!b) {
        r.create_failed = true;
        r.reason = b.error().message;
        return nullptr;
    }
    r.active = std::move(b).value();
    return r.active;
}

Status select_device(std::size_t index) {
    Registry& r = reg();
    std::shared_ptr<Backend> old;
    {
        std::lock_guard<std::mutex> lock(r.m);
        ensure_probed(r);
        if (index >= r.devices.size()) return Status(Errc::NotFound, "gpu: no device with that index");
        if (static_cast<std::size_t>(r.selected) == index && !r.create_failed) return Status::ok();
        for (DeviceInfo& d : r.devices) d.selected = false;
        r.selected = static_cast<int>(index);
        r.devices[index].selected = true;
        old = std::move(r.active);
        r.active.reset();
        r.create_failed = false;
        r.reason.clear();
    }
    old.reset();  // outside the lock: the backend may wait for the device
    return Status::ok();
}

std::string unavailable_reason() {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.m);
    ensure_probed(r);
    if (r.active && r.active->lost()) return "the GPU device was lost";
    return r.reason;
}

namespace testing {

void set_vulkan_loader(const char* name) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.m);
    r.has_loader_override = name != nullptr;
    r.loader_override = name ? name : "";
}

void reset() {
    Registry& r = reg();
    std::shared_ptr<Backend> old;
    {
        std::lock_guard<std::mutex> lock(r.m);
        old = std::move(r.active);
        r.active.reset();
        r.probed = false;
        r.create_failed = false;
        r.devices.clear();
        r.sources.clear();
        r.selected = -1;
        r.reason.clear();
    }
    old.reset();
    vulkan::release();
#if defined(__APPLE__)
    metal::release();
#endif
}

void force_manual_filtering(bool on) { g_force_manual.store(on); }
void set_metal_enabled(bool on) { g_metal_enabled.store(on); }
bool manual_filtering_forced() { return g_force_manual.load(); }
std::int64_t live_objects() { return g_objects.load(); }
void set_upload_chunk_bytes(std::uint64_t bytes) { g_upload_chunk.store(bytes); }

}  // namespace testing

}  // namespace siaara::gpu
