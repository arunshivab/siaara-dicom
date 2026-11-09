// SIAARA DICOM Toolkit - GPU backends for the 3D views (internal).
//
// The View (src/volume/render3d.hpp) keeps its CPU renderer as the reference. With
// View::set_backend(Auto | Gpu) its render_rgba8() first tries a GPU backend through the
// interface below and falls back to the CPU renderer when that is not possible:
//   - no GPU API is present (no Vulkan loader / driver, no Metal device), or no device qualifies;
//   - the view uses something the GPU kernels do not implement (curved MPR, cubic interpolation),
//     a volume larger than the device's 3D texture limit, or tables beyond the storage-buffer limit;
//   - a GPU call fails (out of device memory, device lost, timeout).
// Auto uses hardware devices only (discrete, integrated, virtual); Gpu also accepts a CPU-type
// device such as Mesa's lavapipe (used by the tests). The CPU renderer is the default.
//
// Backends:
//   - Vulkan (vulkan_backend.cpp): Windows and Linux; on macOS only when enabled at run time
//     (MoltenVK) with SIAARA_VULKAN_ON_APPLE=1 in the environment. The loader is opened at run time
//     (vulkan-1.dll from the system directory, libvulkan.so.1 by soname); nothing is linked.
//     Compute kernels are SPIR-V emitted at run time (spirv_kernels.cpp).
//   - Metal (metal_backend.cpp): macOS; kernels in the Metal Shading Language compiled at run time.
//
// One device is selected per process (discrete first, then integrated, virtual, other, CPU) and
// created on first use; it stays alive while views use it. Volumes are uploaded once per device
// and shared by every view of the same Volume object.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "gpu/gpu_params.hpp"

namespace siaara::volume {
class View;
class Volume;
}  // namespace siaara::volume

namespace siaara::gpu {

enum class Api : std::uint8_t { None = 0, Vulkan = 1, Metal = 2 };
enum class DeviceType : std::uint8_t { Other = 0, Integrated = 1, Discrete = 2, Virtual = 3, Cpu = 4 };

struct DeviceInfo {
    std::string name;
    Api api = Api::None;
    DeviceType type = DeviceType::Other;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t memory_bytes = 0;      // device-local memory (largest heap)
    std::uint32_t max_texture_3d = 0;    // largest 3D texture dimension
    std::uint32_t sub_texel_bits = 0;    // texture filtering weight precision (0: unknown)
    bool float32_filtering = false;      // hardware trilinear filtering of float32 textures
    bool int16_filtering = false;        // hardware trilinear filtering of 16-bit normalized textures
    bool selected = false;               // the device the toolkit uses
    std::string driver;                  // driver / API version text
};

// How the voxel values of a texture decode: value = texel * scale + bias (texel as the sampler
// returns it), rounded first when `integer` (exact int16 from 16-bit normalized texels).
struct TextureEncoding {
    float scale = 1.0f;
    float bias = 0.0f;
    bool integer = false;
    bool hardware_filter = true;  // false: trilinear from 8 texel fetches
};

class Texture {
public:
    virtual ~Texture() = default;
    TextureEncoding encoding;
    std::uint32_t dims[3] = {0, 0, 0};
};

// Per-view GPU resources (output image, staging and parameter buffers, command buffer).
class Session {
public:
    virtual ~Session() = default;
    // Runs kernel k over width x height pixels and writes width * height * 4 bytes of RGBA8.
    virtual Status run(param::Kernel k, std::uint32_t width, std::uint32_t height, std::span<const std::uint32_t> params,
                       std::span<const std::uint32_t> data, const Texture& base, const Texture* overlay,
                       std::span<std::uint8_t> rgba) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual const DeviceInfo& info() const = 0;
    // The device texture of a volume, uploaded on first request and shared while referenced.
    virtual Result<std::shared_ptr<const Texture>> texture(const std::shared_ptr<const volume::Volume>& v) = 0;
    virtual Result<std::unique_ptr<Session>> session() = 0;
    // Largest storage-buffer range in bytes.
    virtual std::uint64_t max_buffer_bytes() const = 0;
    // True after an unrecoverable error (device lost, timeout): the backend refuses further work.
    virtual bool lost() const = 0;
};

// Devices of every available API (probed once; empty when there is none).
std::vector<DeviceInfo> devices();
// The backend of the selected device, created on first use; nullptr when there is none (reason
// in unavailable_reason()).
std::shared_ptr<Backend> acquire();
std::string unavailable_reason();

// Per-view state held by volume::View (through a shared_ptr, so the View stays movable).
struct ViewGpu;
std::shared_ptr<ViewGpu> make_view_state();
// Renders the view on the GPU. Ok when it did; otherwise the caller falls back to the CPU
// renderer (the status says why). allow_cpu_device: accept a CPU-type device (backend Gpu).
Status render_view(const volume::View& v, ViewGpu& state, std::span<std::uint8_t> out, bool allow_cpu_device);
// True when the last render of this view was done on the GPU; note_cpu_render() records a CPU render.
bool last_render_was_gpu(const ViewGpu& state) noexcept;
void note_cpu_render(ViewGpu& state) noexcept;
// Why the last render fell back to the CPU (empty after a GPU render).
std::string last_fallback_reason(const ViewGpu& state);

namespace testing {
// Overrides the Vulkan loader library name (no path separators allowed) for the next probe;
// nullptr restores the default. Takes effect after reset().
void set_vulkan_loader(const char* name);
// Drops the selected backend and the probe results (resources are freed once no view uses them).
void reset();
// Forces trilinear sampling through 8 texel fetches instead of the hardware filter.
void force_manual_filtering(bool on);
bool manual_filtering_forced();
// Vulkan / Metal objects currently alive (resource-leak checks).
std::int64_t live_objects();
// Staging bytes per volume-upload step (0: the default, 64 MiB); small values exercise chunking.
void set_upload_chunk_bytes(std::uint64_t bytes);
}  // namespace testing

namespace detail {
// Registry entry points of the backends (each returns an empty list / nullptr when unavailable).
struct Probe {
    std::vector<DeviceInfo> devices;
    std::string reason;  // when no device was found
};
void count_object(int delta) noexcept;
std::uint64_t upload_chunk_bytes() noexcept;
}  // namespace detail

}  // namespace siaara::gpu
