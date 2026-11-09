// SIAARA DICOM Toolkit - Vulkan backend (see gpu.hpp, vulkan_api.hpp).
//
// Loading. The Vulkan loader is opened at run time and never linked:
//   - Windows: vulkan-1.dll with LoadLibraryExW(LOAD_LIBRARY_SEARCH_SYSTEM32) (the system
//     directory only; never the application or current directory);
//   - Linux and other POSIX systems: libvulkan.so.1 by soname (the dynamic loader's system search
//     path; a name without '/' is never looked up in the current directory), and the library must
//     have been loaded from an absolute path;
//   - macOS: only when SIAARA_VULKAN_ON_APPLE=1 (MoltenVK), from the absolute paths of the
//     Vulkan SDK / Homebrew installs (/usr/local/lib, /opt/homebrew/lib), because dlopen of a
//     bare name searches the current directory there. The portability enumeration instance
//     extension and the portability subset device extension are enabled when offered.
// Only vkGetInstanceProcAddr is taken from the library; every other command is resolved through
// it (instance commands) or vkGetDeviceProcAddr (device commands). Libraries stay loaded for the
// life of the process.
//
// Work. Compute pipelines only, no presentation: each frame copies the parameter and table words
// from a host-visible staging buffer into device-local storage buffers, dispatches one kernel
// writing an r32ui storage image, copies the image into a host-visible read-back buffer and waits
// on a fence. Volumes are uploaded once into 3D images through a staging buffer in chunks of
// slices: int16 as R16_UNORM (value + 32768, decoded exactly in the kernel) when that format can
// be filtered linearly, otherwise as R32_SFLOAT; float32 as R32_SFLOAT. Formats without linear
// filtering are sampled through 8 texel fetches per trilinear sample (kLayerManual).
// Every Vulkan call result is checked; every object created is destroyed (and counted for the
// leak tests). A device-lost error or a fence timeout marks the backend lost: it refuses further
// work and the views fall back to the CPU renderer.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "core/system_library.hpp"
#include "gpu/gpu_backends.hpp"
#include "gpu/spirv_kernels.hpp"
#include "gpu/vulkan_api.hpp"
#include "volume/volume.hpp"

// OS headers last, so that their macros cannot reach the toolkit's headers.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace siaara::gpu::vulkan {

namespace {

namespace V = siaara::gpu::vk;

constexpr std::uint64_t kFenceTimeoutNs = 30ull * 1000 * 1000 * 1000;  // 30 s

std::string result_text(V::Result r) {
    switch (r) {
        case V::res::ErrorOutOfHostMemory: return "out of host memory";
        case V::res::ErrorOutOfDeviceMemory: return "out of device memory";
        case V::res::ErrorInitializationFailed: return "initialization failed";
        case V::res::ErrorDeviceLost: return "device lost";
        case V::res::ErrorMemoryMapFailed: return "memory map failed";
        case V::res::ErrorLayerNotPresent: return "layer not present";
        case V::res::ErrorExtensionNotPresent: return "extension not present";
        case V::res::ErrorFeatureNotPresent: return "feature not present";
        case V::res::ErrorIncompatibleDriver: return "incompatible driver";
        case V::res::ErrorTooManyObjects: return "too many objects";
        case V::res::ErrorFormatNotSupported: return "format not supported";
        case V::res::Timeout: return "timeout";
        default: return "VkResult " + std::to_string(r);
    }
}

Status vk_error(const char* what, V::Result r) {
    const Errc code = (r == V::res::ErrorOutOfDeviceMemory || r == V::res::ErrorOutOfHostMemory) ? Errc::OutOfMemory
                      : r == V::res::Timeout                                                     ? Errc::Timeout
                                                                                                 : Errc::Internal;
    return Status(code, std::string("vulkan: ") + what + " failed (" + result_text(r) + ")");
}

// ---------------------------------------------------------------- the loader library
struct Library {
    bool ok = false;
    std::string reason;
    V::PFN_GetInstanceProcAddr gipa = nullptr;
};

std::mutex& library_mutex() {
    static std::mutex m;
    return m;
}

#if defined(_WIN32)
Library open_library(const std::string& name) {
    Library L;
    std::wstring wname;
    for (char c : name) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            L.reason = "the Vulkan loader name must be ASCII";
            return L;
        }
        wname.push_back(static_cast<wchar_t>(c));
    }
    HMODULE h = ::LoadLibraryExW(wname.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) {
        L.reason = "the Vulkan loader (" + name + ") was not found in the system directory";
        return L;
    }
    FARPROC p = ::GetProcAddress(h, "vkGetInstanceProcAddr");
    if (!p) {
        L.reason = name + " lacks vkGetInstanceProcAddr";
        return L;
    }
    L.gipa = reinterpret_cast<V::PFN_GetInstanceProcAddr>(reinterpret_cast<void (*)()>(p));
    L.ok = true;
    return L;
}
#else
// Same hardening as the OpenSSL loader (R-063, R-021, core/system_library.hpp): the loader is
// opened only by absolute path from the fixed system folders, after checking that the file and
// its folder are owned by root or the user and not group/world writable; LD_LIBRARY_PATH,
// RUNPATH and the current directory play no part, and a refused library is closed again.
// The Vulkan loader itself then reads its ICD and layer lists from the environment
// (VK_ICD_FILENAMES / VK_DRIVER_FILES, VK_LAYER_PATH, VK_ADD_LAYER_PATH, VK_INSTANCE_LAYERS):
// whoever controls the process environment chooses the GPU driver code that runs. See SECURITY.md.
Library open_library(const std::string& name) {
    Library L;
    std::string path;
    void* h = siaara::sys::open_system_library(name.c_str(), path);
    if (!h) {
        L.reason = "the Vulkan loader (" + name + ") was not found in a trusted system library folder";
        return L;
    }
    void* sym = ::dlsym(h, "vkGetInstanceProcAddr");
    if (!sym) {
        ::dlclose(h);
        L.reason = name + " lacks vkGetInstanceProcAddr";
        return L;
    }
    Dl_info info{};
    if (::dladdr(sym, &info) == 0 || !info.dli_fname || path != info.dli_fname) {
        ::dlclose(h);
        L.reason = "refusing a Vulkan loader whose entry point is not in the library opened from the system path";
        return L;
    }
    static_assert(sizeof(V::PFN_GetInstanceProcAddr) == sizeof(void*), "function and object pointers differ in size");
    std::memcpy(reinterpret_cast<unsigned char*>(&L.gipa), reinterpret_cast<const unsigned char*>(&sym), sizeof sym);
    L.ok = true;
    return L;
}
#endif

// Opened libraries are kept for the life of the process (one entry per name).
Library load_library(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        Library L;
        L.reason = "the Vulkan loader name must be a plain file name";
        return L;
    }
    std::lock_guard<std::mutex> lock(library_mutex());
    static std::map<std::string, Library>* libs = new std::map<std::string, Library>();  // never destroyed
    auto it = libs->find(name);
    if (it != libs->end()) return it->second;
    Library L = open_library(name);
    (*libs)[name] = L;
    return L;
}

template <class F>
bool resolve_instance(V::PFN_GetInstanceProcAddr gipa, V::Instance inst, const char* name, F& fn) {
    V::VoidFunction p = gipa(inst, name);
    if (!p) return false;
    fn = reinterpret_cast<F>(p);
    return true;
}
template <class F>
bool resolve_device(V::PFN_GetDeviceProcAddr gdpa, V::Device dev, const char* name, F& fn) {
    V::VoidFunction p = gdpa(dev, name);
    if (!p) return false;
    fn = reinterpret_cast<F>(p);
    return true;
}

// ---------------------------------------------------------------- instance and physical devices
struct InstanceFns {
    V::PFN_DestroyInstance DestroyInstance = nullptr;
    V::PFN_EnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    V::PFN_GetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    V::PFN_GetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    V::PFN_GetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    V::PFN_GetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    V::PFN_EnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    V::PFN_CreateDevice CreateDevice = nullptr;
    V::PFN_GetDeviceProcAddr GetDeviceProcAddr = nullptr;
};

struct Phys {
    V::PhysicalDevice pd = nullptr;
    DeviceInfo info;
    std::uint32_t queue_family = 0;
    bool portability_subset = false;
    bool r16_linear = false;
    bool r32f_linear = false;
    std::uint64_t max_buffer = 0;
};

struct InstanceHolder {
    V::PFN_GetInstanceProcAddr gipa = nullptr;
    V::Instance inst = nullptr;
    InstanceFns f;
    std::vector<Phys> phys;
    InstanceHolder() = default;
    InstanceHolder(const InstanceHolder&) = delete;
    InstanceHolder& operator=(const InstanceHolder&) = delete;
    ~InstanceHolder() {
        if (inst && f.DestroyInstance) {
            f.DestroyInstance(inst, nullptr);
            detail::count_object(-1);
        }
    }
};

DeviceType device_type(std::int32_t t) {
    switch (t) {
        case V::kDeviceTypeIntegrated: return DeviceType::Integrated;
        case V::kDeviceTypeDiscrete: return DeviceType::Discrete;
        case V::kDeviceTypeVirtual: return DeviceType::Virtual;
        case V::kDeviceTypeCpu: return DeviceType::Cpu;
        default: return DeviceType::Other;
    }
}

bool has_extension(const std::vector<V::ExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        char buf[V::kMaxExtensionNameSize + 1];
        std::memcpy(buf, e.extensionName, V::kMaxExtensionNameSize);
        buf[V::kMaxExtensionNameSize] = '\0';
        if (std::strcmp(buf, name) == 0) return true;
    }
    return false;
}

std::string version_text(std::uint32_t v) {
    return std::to_string(v >> 22) + "." + std::to_string((v >> 12) & 0x3FFu) + "." + std::to_string(v & 0xFFFu);
}

Result<std::shared_ptr<InstanceHolder>> make_instance(const std::string& loader_name) {
    const Library L = load_library(loader_name);
    if (!L.ok) return Status(Errc::Unsupported, L.reason);
    V::PFN_CreateInstance CreateInstance = nullptr;
    V::PFN_EnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    if (!resolve_instance(L.gipa, nullptr, "vkCreateInstance", CreateInstance) ||
        !resolve_instance(L.gipa, nullptr, "vkEnumerateInstanceExtensionProperties", EnumerateInstanceExtensionProperties))
        return Status(Errc::Unsupported, "vulkan: the loader lacks the global commands");
    std::vector<V::ExtensionProperties> exts;
    {
        std::uint32_t n = 0;
        V::Result r = EnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        if (r != V::res::Success) return vk_error("vkEnumerateInstanceExtensionProperties", r);
        exts.resize(n);
        r = EnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
        if (r != V::res::Success && r != V::res::Incomplete) return vk_error("vkEnumerateInstanceExtensionProperties", r);
        exts.resize(n);
    }
    std::vector<const char*> enabled;
    V::Flags flags = 0;
    if (has_extension(exts, "VK_KHR_portability_enumeration")) {
        enabled.push_back("VK_KHR_portability_enumeration");
        flags |= V::kInstanceEnumeratePortability;
    }
    V::ApplicationInfo app{V::st::ApplicationInfo, nullptr, "SIAARA", 1, "SIAARA DICOM Toolkit", 1, V::make_api_version(1, 0, 0)};
    V::InstanceCreateInfo ci{V::st::InstanceCreateInfo, nullptr, flags, &app, 0, nullptr,
                             static_cast<std::uint32_t>(enabled.size()), enabled.empty() ? nullptr : enabled.data()};
    auto h = std::make_shared<InstanceHolder>();
    h->gipa = L.gipa;
    V::Result r = CreateInstance(&ci, nullptr, &h->inst);
    if (r != V::res::Success) {
        h->inst = nullptr;
        if (r == V::res::ErrorIncompatibleDriver) return Status(Errc::Unsupported, "vulkan: no compatible driver is installed");
        return vk_error("vkCreateInstance", r);
    }
    detail::count_object(1);
    InstanceFns& f = h->f;
    bool ok = resolve_instance(L.gipa, h->inst, "vkDestroyInstance", f.DestroyInstance);
    ok = resolve_instance(L.gipa, h->inst, "vkEnumeratePhysicalDevices", f.EnumeratePhysicalDevices) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkGetPhysicalDeviceProperties", f.GetPhysicalDeviceProperties) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkGetPhysicalDeviceMemoryProperties", f.GetPhysicalDeviceMemoryProperties) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkGetPhysicalDeviceQueueFamilyProperties", f.GetPhysicalDeviceQueueFamilyProperties) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkGetPhysicalDeviceFormatProperties", f.GetPhysicalDeviceFormatProperties) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkEnumerateDeviceExtensionProperties", f.EnumerateDeviceExtensionProperties) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkCreateDevice", f.CreateDevice) && ok;
    ok = resolve_instance(L.gipa, h->inst, "vkGetDeviceProcAddr", f.GetDeviceProcAddr) && ok;
    if (!f.DestroyInstance) {
        // Cannot destroy what we cannot reach: keep the instance (it is one object, once).
        h->inst = nullptr;
        return Status(Errc::Unsupported, "vulkan: the loader lacks vkDestroyInstance");
    }
    if (!ok) return Status(Errc::Unsupported, "vulkan: the loader lacks instance commands");

    std::uint32_t n = 0;
    r = f.EnumeratePhysicalDevices(h->inst, &n, nullptr);
    if (r != V::res::Success) return vk_error("vkEnumeratePhysicalDevices", r);
    std::vector<V::PhysicalDevice> pds(n);
    if (n) {
        r = f.EnumeratePhysicalDevices(h->inst, &n, pds.data());
        if (r != V::res::Success && r != V::res::Incomplete) return vk_error("vkEnumeratePhysicalDevices", r);
        pds.resize(n);
    }
    for (V::PhysicalDevice pd : pds) {
        V::PhysicalDeviceProperties props{};
        f.GetPhysicalDeviceProperties(pd, &props);
        Phys p;
        p.pd = pd;
        char name[V::kMaxPhysicalDeviceNameSize + 1];
        std::memcpy(name, props.deviceName, V::kMaxPhysicalDeviceNameSize);
        name[V::kMaxPhysicalDeviceNameSize] = '\0';
        p.info.name = name;
        p.info.api = Api::Vulkan;
        p.info.type = device_type(props.deviceType);
        p.info.vendor_id = props.vendorID;
        p.info.device_id = props.deviceID;
        p.info.max_texture_3d = props.limits.maxImageDimension3D;
        p.info.sub_texel_bits = props.limits.subTexelPrecisionBits;
        p.info.driver = "Vulkan " + version_text(props.apiVersion) + ", driver " + std::to_string(props.driverVersion);
        p.max_buffer = props.limits.maxStorageBufferRange;
        V::PhysicalDeviceMemoryProperties mem{};
        f.GetPhysicalDeviceMemoryProperties(pd, &mem);
        for (std::uint32_t i = 0; i < mem.memoryHeapCount && i < V::kMaxMemoryHeaps; ++i)
            if (mem.memoryHeaps[i].flags & V::kHeapDeviceLocal) p.info.memory_bytes = std::max(p.info.memory_bytes, mem.memoryHeaps[i].size);
        // A queue family with compute.
        std::uint32_t qn = 0;
        f.GetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<V::QueueFamilyProperties> qf(qn);
        if (qn) f.GetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        bool have_queue = false;
        for (std::uint32_t i = 0; i < qn && i < qf.size(); ++i)
            if ((qf[i].queueFlags & V::kQueueCompute) && qf[i].queueCount > 0) {
                p.queue_family = i;
                have_queue = true;
                break;
            }
        if (!have_queue) continue;
        // Formats: r32ui storage image and r32f sampled are required by the backend.
        V::FormatProperties fp{};
        f.GetPhysicalDeviceFormatProperties(pd, V::fmt::R32Uint, &fp);
        if (!(fp.optimalTilingFeatures & V::kFormatStorageImage)) continue;
        f.GetPhysicalDeviceFormatProperties(pd, V::fmt::R32Sfloat, &fp);
        if (!(fp.optimalTilingFeatures & V::kFormatSampledImage)) continue;
        p.r32f_linear = (fp.optimalTilingFeatures & V::kFormatSampledImageFilterLinear) != 0;
        f.GetPhysicalDeviceFormatProperties(pd, V::fmt::R16Unorm, &fp);
        p.r16_linear = (fp.optimalTilingFeatures & V::kFormatSampledImage) && (fp.optimalTilingFeatures & V::kFormatSampledImageFilterLinear);
        p.info.float32_filtering = p.r32f_linear;
        p.info.int16_filtering = p.r16_linear;
        // Portability subset (MoltenVK) must be enabled when offered.
        std::uint32_t en = 0;
        if (f.EnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr) == V::res::Success && en) {
            std::vector<V::ExtensionProperties> de(en);
            const V::Result er = f.EnumerateDeviceExtensionProperties(pd, nullptr, &en, de.data());
            if (er == V::res::Success || er == V::res::Incomplete) {
                de.resize(en);
                p.portability_subset = has_extension(de, "VK_KHR_portability_subset");
            }
        }
        h->phys.push_back(std::move(p));
    }
    return h;
}

// ---------------------------------------------------------------- device context
struct DeviceFns {
    V::PFN_DestroyDevice DestroyDevice = nullptr;
    V::PFN_GetDeviceQueue GetDeviceQueue = nullptr;
    V::PFN_DeviceWaitIdle DeviceWaitIdle = nullptr;
    V::PFN_CreateCommandPool CreateCommandPool = nullptr;
    V::PFN_DestroyCommandPool DestroyCommandPool = nullptr;
    V::PFN_AllocateCommandBuffers AllocateCommandBuffers = nullptr;
    V::PFN_FreeCommandBuffers FreeCommandBuffers = nullptr;
    V::PFN_BeginCommandBuffer BeginCommandBuffer = nullptr;
    V::PFN_EndCommandBuffer EndCommandBuffer = nullptr;
    V::PFN_ResetCommandBuffer ResetCommandBuffer = nullptr;
    V::PFN_CreateBuffer CreateBuffer = nullptr;
    V::PFN_DestroyBuffer DestroyBuffer = nullptr;
    V::PFN_GetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    V::PFN_BindBufferMemory BindBufferMemory = nullptr;
    V::PFN_CreateImage CreateImage = nullptr;
    V::PFN_DestroyImage DestroyImage = nullptr;
    V::PFN_GetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    V::PFN_BindImageMemory BindImageMemory = nullptr;
    V::PFN_AllocateMemory AllocateMemory = nullptr;
    V::PFN_FreeMemory FreeMemory = nullptr;
    V::PFN_MapMemory MapMemory = nullptr;
    V::PFN_UnmapMemory UnmapMemory = nullptr;
    V::PFN_CreateImageView CreateImageView = nullptr;
    V::PFN_DestroyImageView DestroyImageView = nullptr;
    V::PFN_CreateSampler CreateSampler = nullptr;
    V::PFN_DestroySampler DestroySampler = nullptr;
    V::PFN_CreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    V::PFN_DestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    V::PFN_CreatePipelineLayout CreatePipelineLayout = nullptr;
    V::PFN_DestroyPipelineLayout DestroyPipelineLayout = nullptr;
    V::PFN_CreateShaderModule CreateShaderModule = nullptr;
    V::PFN_DestroyShaderModule DestroyShaderModule = nullptr;
    V::PFN_CreateComputePipelines CreateComputePipelines = nullptr;
    V::PFN_DestroyPipeline DestroyPipeline = nullptr;
    V::PFN_CreateDescriptorPool CreateDescriptorPool = nullptr;
    V::PFN_DestroyDescriptorPool DestroyDescriptorPool = nullptr;
    V::PFN_AllocateDescriptorSets AllocateDescriptorSets = nullptr;
    V::PFN_UpdateDescriptorSets UpdateDescriptorSets = nullptr;
    V::PFN_CreateFence CreateFence = nullptr;
    V::PFN_DestroyFence DestroyFence = nullptr;
    V::PFN_ResetFences ResetFences = nullptr;
    V::PFN_WaitForFences WaitForFences = nullptr;
    V::PFN_QueueSubmit QueueSubmit = nullptr;
    V::PFN_CmdBindPipeline CmdBindPipeline = nullptr;
    V::PFN_CmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    V::PFN_CmdDispatch CmdDispatch = nullptr;
    V::PFN_CmdPipelineBarrier CmdPipelineBarrier = nullptr;
    V::PFN_CmdCopyBuffer CmdCopyBuffer = nullptr;
    V::PFN_CmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    V::PFN_CmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
};

struct Buf {
    V::Buffer buffer = 0;
    V::DeviceMemory memory = 0;
    V::DeviceSize size = 0;
    void* mapped = nullptr;
};

struct Img {
    V::Image image = 0;
    V::DeviceMemory memory = 0;
    V::ImageView view = 0;
};

class Context;

class VkTexture final : public Texture {
public:
    explicit VkTexture(std::shared_ptr<Context> c) : ctx(std::move(c)) {}
    ~VkTexture() override;
    VkTexture(const VkTexture&) = delete;
    VkTexture& operator=(const VkTexture&) = delete;
    std::shared_ptr<Context> ctx;
    Img img;
    bool linear_sampler = true;
};

class Context final : public Backend, public std::enable_shared_from_this<Context> {
public:
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() override;

    Status init(std::shared_ptr<InstanceHolder> ih, std::size_t index);

    const DeviceInfo& info() const override { return phys_.info; }
    Result<std::shared_ptr<const Texture>> texture(const std::shared_ptr<const volume::Volume>& v) override;
    Result<std::unique_ptr<Session>> session() override;
    std::uint64_t max_buffer_bytes() const override { return phys_.max_buffer; }
    bool lost() const override { return lost_.load(); }

    // ---- helpers for textures and sessions
    Status make_buffer(V::DeviceSize size, V::Flags usage, bool host, Buf& out);
    void destroy_buffer(Buf& b) noexcept;
    Status make_image(V::Flags usage, std::int32_t format, std::int32_t type, std::uint32_t w, std::uint32_t h,
                      std::uint32_t d, Img& out);
    void destroy_image(Img& i) noexcept;
    // Submits one command buffer and waits for its fence. Marks the context lost on device loss
    // or timeout.
    Status submit_wait(V::CommandBuffer cb, V::Fence fence);
    void mark_lost() noexcept { lost_.store(true); }
    std::uint32_t queue_family() const noexcept { return phys_.queue_family; }

    DeviceFns f;
    V::Device dev = nullptr;
    V::Sampler sampler_linear = 0, sampler_nearest = 0;
    V::DescriptorSetLayout dsl = 0;
    V::PipelineLayout layout = 0;
    V::Pipeline pipelines[param::KernelCount] = {0, 0, 0};

private:
    Status load_device_fns();
    Status create_pipelines();
    Result<std::shared_ptr<VkTexture>> upload(const volume::Volume& v);
    int find_memory(std::uint32_t bits, V::Flags required, V::Flags preferred) const;

    std::shared_ptr<InstanceHolder> ih_;
    Phys phys_;
    V::PhysicalDeviceMemoryProperties mem_{};
    V::Queue queue_ = nullptr;
    V::CommandPool upload_pool_ = 0;
    std::mutex queue_mu_;   // vkQueueSubmit (external synchronisation of the queue)
    std::mutex upload_mu_;  // the upload command pool
    std::mutex cache_mu_;
    std::vector<std::pair<std::weak_ptr<const volume::Volume>, std::weak_ptr<const Texture>>> cache_;
    std::atomic<bool> lost_{false};
};

#define SIAARA_VK_DEV(name) ok = resolve_device(gdpa, dev, "vk" #name, f.name) && ok

Status Context::load_device_fns() {
    const V::PFN_GetDeviceProcAddr gdpa = ih_->f.GetDeviceProcAddr;
    bool ok = true;
    SIAARA_VK_DEV(DestroyDevice);
    SIAARA_VK_DEV(GetDeviceQueue);
    SIAARA_VK_DEV(DeviceWaitIdle);
    SIAARA_VK_DEV(CreateCommandPool);
    SIAARA_VK_DEV(DestroyCommandPool);
    SIAARA_VK_DEV(AllocateCommandBuffers);
    SIAARA_VK_DEV(FreeCommandBuffers);
    SIAARA_VK_DEV(BeginCommandBuffer);
    SIAARA_VK_DEV(EndCommandBuffer);
    SIAARA_VK_DEV(ResetCommandBuffer);
    SIAARA_VK_DEV(CreateBuffer);
    SIAARA_VK_DEV(DestroyBuffer);
    SIAARA_VK_DEV(GetBufferMemoryRequirements);
    SIAARA_VK_DEV(BindBufferMemory);
    SIAARA_VK_DEV(CreateImage);
    SIAARA_VK_DEV(DestroyImage);
    SIAARA_VK_DEV(GetImageMemoryRequirements);
    SIAARA_VK_DEV(BindImageMemory);
    SIAARA_VK_DEV(AllocateMemory);
    SIAARA_VK_DEV(FreeMemory);
    SIAARA_VK_DEV(MapMemory);
    SIAARA_VK_DEV(UnmapMemory);
    SIAARA_VK_DEV(CreateImageView);
    SIAARA_VK_DEV(DestroyImageView);
    SIAARA_VK_DEV(CreateSampler);
    SIAARA_VK_DEV(DestroySampler);
    SIAARA_VK_DEV(CreateDescriptorSetLayout);
    SIAARA_VK_DEV(DestroyDescriptorSetLayout);
    SIAARA_VK_DEV(CreatePipelineLayout);
    SIAARA_VK_DEV(DestroyPipelineLayout);
    SIAARA_VK_DEV(CreateShaderModule);
    SIAARA_VK_DEV(DestroyShaderModule);
    SIAARA_VK_DEV(CreateComputePipelines);
    SIAARA_VK_DEV(DestroyPipeline);
    SIAARA_VK_DEV(CreateDescriptorPool);
    SIAARA_VK_DEV(DestroyDescriptorPool);
    SIAARA_VK_DEV(AllocateDescriptorSets);
    SIAARA_VK_DEV(UpdateDescriptorSets);
    SIAARA_VK_DEV(CreateFence);
    SIAARA_VK_DEV(DestroyFence);
    SIAARA_VK_DEV(ResetFences);
    SIAARA_VK_DEV(WaitForFences);
    SIAARA_VK_DEV(QueueSubmit);
    SIAARA_VK_DEV(CmdBindPipeline);
    SIAARA_VK_DEV(CmdBindDescriptorSets);
    SIAARA_VK_DEV(CmdDispatch);
    SIAARA_VK_DEV(CmdPipelineBarrier);
    SIAARA_VK_DEV(CmdCopyBuffer);
    SIAARA_VK_DEV(CmdCopyBufferToImage);
    SIAARA_VK_DEV(CmdCopyImageToBuffer);
    if (!ok) return Status(Errc::Unsupported, "vulkan: the driver lacks device commands");
    return Status::ok();
}

#undef SIAARA_VK_DEV

Status Context::init(std::shared_ptr<InstanceHolder> ih, std::size_t index) {
    ih_ = std::move(ih);
    if (index >= ih_->phys.size()) return Status(Errc::InvalidArgument, "vulkan: no such device");
    phys_ = ih_->phys[index];
    ih_->f.GetPhysicalDeviceMemoryProperties(phys_.pd, &mem_);
    const float priority = 1.0f;
    V::DeviceQueueCreateInfo qci{V::st::DeviceQueueCreateInfo, nullptr, 0, phys_.queue_family, 1, &priority};
    const char* exts[1] = {"VK_KHR_portability_subset"};
    V::DeviceCreateInfo dci{V::st::DeviceCreateInfo, nullptr, 0, 1, &qci, 0, nullptr,
                            phys_.portability_subset ? 1u : 0u, phys_.portability_subset ? exts : nullptr, nullptr};
    V::Result r = ih_->f.CreateDevice(phys_.pd, &dci, nullptr, &dev);
    if (r != V::res::Success) {
        dev = nullptr;
        return vk_error("vkCreateDevice", r);
    }
    detail::count_object(1);
    if (Status s = load_device_fns(); !s) {
        // DestroyDevice may itself be missing: then the device is left to the driver.
        if (!f.DestroyDevice) dev = nullptr;
        return s;
    }
    f.GetDeviceQueue(dev, phys_.queue_family, 0, &queue_);
    if (!queue_) return Status(Errc::Internal, "vulkan: vkGetDeviceQueue returned no queue");
    V::CommandPoolCreateInfo pci{V::st::CommandPoolCreateInfo, nullptr, V::kCommandPoolResetCommandBuffer, phys_.queue_family};
    r = f.CreateCommandPool(dev, &pci, nullptr, &upload_pool_);
    if (r != V::res::Success) {
        upload_pool_ = 0;
        return vk_error("vkCreateCommandPool", r);
    }
    detail::count_object(1);
    V::SamplerCreateInfo sci{V::st::SamplerCreateInfo, nullptr, 0, V::kFilterLinear, V::kFilterLinear, V::kMipmapNearest,
                             V::kAddressClampToEdge, V::kAddressClampToEdge, V::kAddressClampToEdge, 0.0f, 0, 1.0f, 0,
                             V::kCompareNever, 0.0f, 0.0f, V::kBorderFloatTransparentBlack, 0};
    r = f.CreateSampler(dev, &sci, nullptr, &sampler_linear);
    if (r != V::res::Success) {
        sampler_linear = 0;
        return vk_error("vkCreateSampler", r);
    }
    detail::count_object(1);
    sci.magFilter = V::kFilterNearest;
    sci.minFilter = V::kFilterNearest;
    r = f.CreateSampler(dev, &sci, nullptr, &sampler_nearest);
    if (r != V::res::Success) {
        sampler_nearest = 0;
        return vk_error("vkCreateSampler", r);
    }
    detail::count_object(1);
    return create_pipelines();
}

Status Context::create_pipelines() {
    const V::DescriptorSetLayoutBinding b[5] = {
        {0, V::kDescStorageBuffer, 1, V::kStageCompute, nullptr},
        {1, V::kDescStorageBuffer, 1, V::kStageCompute, nullptr},
        {2, V::kDescCombinedImageSampler, 1, V::kStageCompute, nullptr},
        {3, V::kDescCombinedImageSampler, 1, V::kStageCompute, nullptr},
        {4, V::kDescStorageImage, 1, V::kStageCompute, nullptr},
    };
    V::DescriptorSetLayoutCreateInfo dci{V::st::DescriptorSetLayoutCreateInfo, nullptr, 0, 5, b};
    V::Result r = f.CreateDescriptorSetLayout(dev, &dci, nullptr, &dsl);
    if (r != V::res::Success) {
        dsl = 0;
        return vk_error("vkCreateDescriptorSetLayout", r);
    }
    detail::count_object(1);
    V::PipelineLayoutCreateInfo lci{V::st::PipelineLayoutCreateInfo, nullptr, 0, 1, &dsl, 0, nullptr};
    r = f.CreatePipelineLayout(dev, &lci, nullptr, &layout);
    if (r != V::res::Success) {
        layout = 0;
        return vk_error("vkCreatePipelineLayout", r);
    }
    detail::count_object(1);
    for (std::uint32_t k = 0; k < param::KernelCount; ++k) {
        std::vector<std::uint32_t> code;
        try {
            code = spirv::kernel(static_cast<param::Kernel>(k));
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "vulkan: out of memory");
        }
        V::ShaderModuleCreateInfo sm{V::st::ShaderModuleCreateInfo, nullptr, 0, code.size() * 4u, code.data()};
        V::ShaderModule mod = 0;
        r = f.CreateShaderModule(dev, &sm, nullptr, &mod);
        if (r != V::res::Success) return vk_error("vkCreateShaderModule", r);
        detail::count_object(1);
        V::ComputePipelineCreateInfo pci{V::st::ComputePipelineCreateInfo, nullptr, 0,
                                         {V::st::PipelineShaderStageCreateInfo, nullptr, 0, V::kStageCompute, mod, "main", nullptr},
                                         layout, 0, -1};
        r = f.CreateComputePipelines(dev, 0, 1, &pci, nullptr, &pipelines[k]);
        f.DestroyShaderModule(dev, mod, nullptr);
        detail::count_object(-1);
        if (r != V::res::Success) {
            pipelines[k] = 0;
            return vk_error("vkCreateComputePipelines", r);
        }
        detail::count_object(1);
    }
    return Status::ok();
}

Context::~Context() {
    // After a device loss or timeout objects may still be in use: everything is left to the driver.
    if (!dev || lost_.load()) return;
    // Nothing should be in flight (every submission is waited for); if the device cannot even
    // report idle, it is treated as lost and its objects are left to the driver.
    if (!f.DeviceWaitIdle || f.DeviceWaitIdle(dev) != V::res::Success) return;
    for (auto& p : pipelines)
        if (p) {
            f.DestroyPipeline(dev, p, nullptr);
            detail::count_object(-1);
        }
    if (layout) {
        f.DestroyPipelineLayout(dev, layout, nullptr);
        detail::count_object(-1);
    }
    if (dsl) {
        f.DestroyDescriptorSetLayout(dev, dsl, nullptr);
        detail::count_object(-1);
    }
    if (sampler_linear) {
        f.DestroySampler(dev, sampler_linear, nullptr);
        detail::count_object(-1);
    }
    if (sampler_nearest) {
        f.DestroySampler(dev, sampler_nearest, nullptr);
        detail::count_object(-1);
    }
    if (upload_pool_) {
        f.DestroyCommandPool(dev, upload_pool_, nullptr);
        detail::count_object(-1);
    }
    if (f.DestroyDevice) {
        f.DestroyDevice(dev, nullptr);
        detail::count_object(-1);
    }
}

int Context::find_memory(std::uint32_t bits, V::Flags required, V::Flags preferred) const {
    int fallback = -1;
    for (std::uint32_t i = 0; i < mem_.memoryTypeCount && i < V::kMaxMemoryTypes; ++i) {
        if (!(bits & (1u << i))) continue;
        const V::Flags p = mem_.memoryTypes[i].propertyFlags;
        if ((p & required) != required) continue;
        if ((p & preferred) == preferred) return static_cast<int>(i);
        if (fallback < 0) fallback = static_cast<int>(i);
    }
    return fallback;
}

Status Context::make_buffer(V::DeviceSize size, V::Flags usage, bool host, Buf& out) {
    out = Buf{};
    V::BufferCreateInfo bci{V::st::BufferCreateInfo, nullptr, 0, size, usage, V::kSharingExclusive, 0, nullptr};
    V::Result r = f.CreateBuffer(dev, &bci, nullptr, &out.buffer);
    if (r != V::res::Success) {
        out.buffer = 0;
        return vk_error("vkCreateBuffer", r);
    }
    detail::count_object(1);
    V::MemoryRequirements req{};
    f.GetBufferMemoryRequirements(dev, out.buffer, &req);
    // Host buffers: visible + coherent (no flushes needed), cached preferred (fast read-back).
    // Device buffers: device-local preferred (any type works, e.g. on unified-memory devices).
    const int type = host ? find_memory(req.memoryTypeBits, V::kMemoryHostVisible | V::kMemoryHostCoherent,
                                        V::kMemoryHostVisible | V::kMemoryHostCoherent | V::kMemoryHostCached)
                          : find_memory(req.memoryTypeBits, 0, V::kMemoryDeviceLocal);
    if (type < 0) {
        destroy_buffer(out);
        return Status(Errc::Unsupported, "vulkan: no suitable memory type for a buffer");
    }
    V::MemoryAllocateInfo mai{V::st::MemoryAllocateInfo, nullptr, req.size, static_cast<std::uint32_t>(type)};
    r = f.AllocateMemory(dev, &mai, nullptr, &out.memory);
    if (r != V::res::Success) {
        out.memory = 0;
        destroy_buffer(out);
        return vk_error("vkAllocateMemory", r);
    }
    detail::count_object(1);
    r = f.BindBufferMemory(dev, out.buffer, out.memory, 0);
    if (r != V::res::Success) {
        destroy_buffer(out);
        return vk_error("vkBindBufferMemory", r);
    }
    if (host) {
        r = f.MapMemory(dev, out.memory, 0, V::kWholeSize, 0, &out.mapped);
        if (r != V::res::Success || !out.mapped) {
            out.mapped = nullptr;
            destroy_buffer(out);
            return vk_error("vkMapMemory", r == V::res::Success ? V::res::ErrorMemoryMapFailed : r);
        }
    }
    out.size = size;
    return Status::ok();
}

void Context::destroy_buffer(Buf& b) noexcept {
    if (b.mapped) f.UnmapMemory(dev, b.memory);
    if (b.buffer) {
        f.DestroyBuffer(dev, b.buffer, nullptr);
        detail::count_object(-1);
    }
    if (b.memory) {
        f.FreeMemory(dev, b.memory, nullptr);
        detail::count_object(-1);
    }
    b = Buf{};
}

Status Context::make_image(V::Flags usage, std::int32_t format, std::int32_t type, std::uint32_t w, std::uint32_t h,
                           std::uint32_t d, Img& out) {
    out = Img{};
    V::ImageCreateInfo ici{V::st::ImageCreateInfo, nullptr, 0, type, format, {w, h, d}, 1, 1, V::kSampleCount1,
                           V::kTilingOptimal, usage, V::kSharingExclusive, 0, nullptr, V::kLayoutUndefined};
    V::Result r = f.CreateImage(dev, &ici, nullptr, &out.image);
    if (r != V::res::Success) {
        out.image = 0;
        return vk_error("vkCreateImage", r);
    }
    detail::count_object(1);
    V::MemoryRequirements req{};
    f.GetImageMemoryRequirements(dev, out.image, &req);
    const int mt = find_memory(req.memoryTypeBits, 0, V::kMemoryDeviceLocal);
    if (mt < 0) {
        destroy_image(out);
        return Status(Errc::Unsupported, "vulkan: no suitable memory type for an image");
    }
    V::MemoryAllocateInfo mai{V::st::MemoryAllocateInfo, nullptr, req.size, static_cast<std::uint32_t>(mt)};
    r = f.AllocateMemory(dev, &mai, nullptr, &out.memory);
    if (r != V::res::Success) {
        out.memory = 0;
        destroy_image(out);
        return vk_error("vkAllocateMemory", r);
    }
    detail::count_object(1);
    r = f.BindImageMemory(dev, out.image, out.memory, 0);
    if (r != V::res::Success) {
        destroy_image(out);
        return vk_error("vkBindImageMemory", r);
    }
    V::ImageViewCreateInfo vci{V::st::ImageViewCreateInfo, nullptr, 0, out.image,
                               type == V::kImageType3D ? V::kViewType3D : V::kViewType2D, format, {0, 0, 0, 0},
                               {V::kAspectColor, 0, 1, 0, 1}};
    r = f.CreateImageView(dev, &vci, nullptr, &out.view);
    if (r != V::res::Success) {
        out.view = 0;
        destroy_image(out);
        return vk_error("vkCreateImageView", r);
    }
    detail::count_object(1);
    return Status::ok();
}

void Context::destroy_image(Img& i) noexcept {
    if (i.view) {
        f.DestroyImageView(dev, i.view, nullptr);
        detail::count_object(-1);
    }
    if (i.image) {
        f.DestroyImage(dev, i.image, nullptr);
        detail::count_object(-1);
    }
    if (i.memory) {
        f.FreeMemory(dev, i.memory, nullptr);
        detail::count_object(-1);
    }
    i = Img{};
}

Status Context::submit_wait(V::CommandBuffer cb, V::Fence fence) {
    if (lost_.load()) return Status(Errc::Unsupported, "vulkan: the device was lost");
    V::Result r = f.ResetFences(dev, 1, &fence);
    if (r != V::res::Success) return vk_error("vkResetFences", r);
    V::SubmitInfo si{V::st::SubmitInfo, nullptr, 0, nullptr, nullptr, 1, &cb, 0, nullptr};
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        r = f.QueueSubmit(queue_, 1, &si, fence);
    }
    if (r != V::res::Success) {
        if (r == V::res::ErrorDeviceLost) mark_lost();
        return vk_error("vkQueueSubmit", r);
    }
    r = f.WaitForFences(dev, 1, &fence, 1, kFenceTimeoutNs);
    if (r != V::res::Success) {
        // The work may still be running: nothing it uses may be freed, so the backend stops here.
        mark_lost();
        return vk_error("vkWaitForFences", r);
    }
    return Status::ok();
}

// One-shot command buffer from the context's upload pool (caller holds upload_mu_).
struct OneShot {
    Context& c;
    V::CommandPool pool;
    V::CommandBuffer cb = nullptr;
    V::Fence fence = 0;
    OneShot(Context& ctx, V::CommandPool p) : c(ctx), pool(p) {}
    OneShot(const OneShot&) = delete;
    OneShot& operator=(const OneShot&) = delete;
    Status init() {
        V::CommandBufferAllocateInfo ai{V::st::CommandBufferAllocateInfo, nullptr, pool, V::kCommandBufferPrimary, 1};
        V::Result r = c.f.AllocateCommandBuffers(c.dev, &ai, &cb);
        if (r != V::res::Success) {
            cb = nullptr;
            return vk_error("vkAllocateCommandBuffers", r);
        }
        detail::count_object(1);
        V::FenceCreateInfo fci{V::st::FenceCreateInfo, nullptr, 0};
        r = c.f.CreateFence(c.dev, &fci, nullptr, &fence);
        if (r != V::res::Success) {
            fence = 0;
            return vk_error("vkCreateFence", r);
        }
        detail::count_object(1);
        return Status::ok();
    }
    Status begin() {
        V::Result r = c.f.ResetCommandBuffer(cb, 0);
        if (r != V::res::Success) return vk_error("vkResetCommandBuffer", r);
        V::CommandBufferBeginInfo bi{V::st::CommandBufferBeginInfo, nullptr, V::kCommandBufferOneTimeSubmit, nullptr};
        r = c.f.BeginCommandBuffer(cb, &bi);
        if (r != V::res::Success) return vk_error("vkBeginCommandBuffer", r);
        return Status::ok();
    }
    Status end_submit() {
        V::Result r = c.f.EndCommandBuffer(cb);
        if (r != V::res::Success) return vk_error("vkEndCommandBuffer", r);
        return c.submit_wait(cb, fence);
    }
    ~OneShot() {
        if (c.lost()) return;  // possibly still in use: leave to the driver
        if (fence) {
            c.f.DestroyFence(c.dev, fence, nullptr);
            detail::count_object(-1);
        }
        if (cb) {
            c.f.FreeCommandBuffers(c.dev, pool, 1, &cb);
            detail::count_object(-1);
        }
    }
};

V::ImageMemoryBarrier image_barrier(V::Image img, V::Flags src, V::Flags dst, std::int32_t from, std::int32_t to) {
    return V::ImageMemoryBarrier{V::st::ImageMemoryBarrier, nullptr, src, dst, from, to, V::kQueueFamilyIgnored,
                                 V::kQueueFamilyIgnored, img, {V::kAspectColor, 0, 1, 0, 1}};
}

Result<std::shared_ptr<VkTexture>> Context::upload(const volume::Volume& vol) {
    const auto& g = vol.grid();
    const std::uint32_t nx = g.dims[0], ny = g.dims[1], nz = g.dims[2];
    if (nx == 0 || ny == 0 || nz == 0) return Status(Errc::InvalidArgument, "vulkan: empty volume");
    const std::uint32_t lim = phys_.info.max_texture_3d;
    if (nx > lim || ny > lim || nz > lim) return Status(Errc::Unsupported, "vulkan: the volume exceeds the 3D texture limit");
    const bool i16 = vol.type() == volume::VoxelType::Int16;
    // Format choice (see the file comment).
    const bool use_r16 = i16 && phys_.r16_linear;
    const std::int32_t format = use_r16 ? V::fmt::R16Unorm : V::fmt::R32Sfloat;
    const std::uint64_t texel = use_r16 ? 2u : 4u;
    std::uint64_t slice = 0, total = 0;
    if (!checked_mul(std::uint64_t{nx} * ny, texel, slice) || !checked_mul(slice, nz, total))
        return Status(Errc::LimitExceeded, "vulkan: volume too large");
    if (phys_.info.memory_bytes && total > phys_.info.memory_bytes)
        return Status(Errc::Unsupported, "vulkan: the volume exceeds the device memory");
    auto tex = std::make_shared<VkTexture>(shared_from_this());
    tex->dims[0] = nx;
    tex->dims[1] = ny;
    tex->dims[2] = nz;
    if (use_r16) {
        tex->encoding = {65535.0f, -32768.0f, true, true};
    } else {
        tex->encoding = {1.0f, 0.0f, i16, phys_.r32f_linear};
    }
    tex->linear_sampler = use_r16 ? true : phys_.r32f_linear;
    if (Status s = make_image(V::kImageUsageTransferDst | V::kImageUsageSampled, format, V::kImageType3D, nx, ny, nz, tex->img); !s)
        return s;
    // Slices per chunk.
    const std::uint64_t per = std::max<std::uint64_t>(1, detail::upload_chunk_bytes() / slice);
    const std::uint64_t chunk_slices = std::min<std::uint64_t>(per, nz);
    std::uint64_t staging_bytes = 0;
    if (!checked_mul(chunk_slices, slice, staging_bytes) || static_cast<std::size_t>(staging_bytes) != staging_bytes)
        return Status(Errc::LimitExceeded, "vulkan: volume slice too large");
    std::lock_guard<std::mutex> lock(upload_mu_);
    Buf staging;
    if (Status s = make_buffer(staging_bytes, V::kBufferUsageTransferSrc, true, staging); !s) return s;
    struct StagingGuard {
        Context& c;
        Buf& b;
        ~StagingGuard() {
            if (!c.lost()) c.destroy_buffer(b);
        }
    } guard{*this, staging};
    OneShot os(*this, upload_pool_);
    if (Status s = os.init(); !s) return s;
    const std::size_t row_voxels = static_cast<std::size_t>(nx) * ny;
    for (std::uint32_t z0 = 0; z0 < nz;) {
        const std::uint32_t z1 = static_cast<std::uint32_t>(std::min<std::uint64_t>(nz, z0 + chunk_slices));
        // Convert into the staging buffer.
        const std::size_t count = row_voxels * (z1 - z0);
        const std::size_t first = row_voxels * z0;
        if (use_r16) {
            auto* dst = static_cast<std::uint16_t*>(staging.mapped);
            const std::int16_t* src = vol.i16() + first;
            for (std::size_t i = 0; i < count; ++i) dst[i] = static_cast<std::uint16_t>(static_cast<std::int32_t>(src[i]) + 32768);
        } else if (i16) {
            auto* dst = static_cast<float*>(staging.mapped);
            const std::int16_t* src = vol.i16() + first;
            for (std::size_t i = 0; i < count; ++i) dst[i] = static_cast<float>(src[i]);
        } else {
            std::memcpy(staging.mapped, vol.f32() + first, count * sizeof(float));
        }
        if (Status s = os.begin(); !s) return s;
        if (z0 == 0) {
            const V::ImageMemoryBarrier b = image_barrier(tex->img.image, 0, V::kAccessTransferWrite, V::kLayoutUndefined, V::kLayoutTransferDst);
            f.CmdPipelineBarrier(os.cb, V::kStageTopOfPipe, V::kStageTransfer, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        V::BufferImageCopy region{0, 0, 0, {V::kAspectColor, 0, 0, 1}, {0, 0, static_cast<std::int32_t>(z0)}, {nx, ny, z1 - z0}};
        f.CmdCopyBufferToImage(os.cb, staging.buffer, tex->img.image, V::kLayoutTransferDst, 1, &region);
        if (z1 == nz) {
            const V::ImageMemoryBarrier b = image_barrier(tex->img.image, V::kAccessTransferWrite, V::kAccessShaderRead,
                                                          V::kLayoutTransferDst, V::kLayoutShaderReadOnly);
            f.CmdPipelineBarrier(os.cb, V::kStageTransfer, V::kStageComputeShader, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        if (Status s = os.end_submit(); !s) return s;
        z0 = z1;
    }
    return tex;
}

Result<std::shared_ptr<const Texture>> Context::texture(const std::shared_ptr<const volume::Volume>& v) {
    if (!v) return Status(Errc::InvalidArgument, "vulkan: no volume");
    if (lost_.load()) return Status(Errc::Unsupported, "vulkan: the device was lost");
    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            auto vol = it->first.lock();
            auto tex = it->second.lock();
            if (!vol || !tex) {
                it = cache_.erase(it);
                continue;
            }
            if (vol == v) return std::shared_ptr<const Texture>(tex);
            ++it;
        }
    }
    auto t = upload(*v);
    if (!t) return t.take_error();
    std::shared_ptr<const Texture> tex = std::move(t).value();
    std::lock_guard<std::mutex> lock(cache_mu_);
    cache_.emplace_back(v, tex);
    return tex;
}

VkTexture::~VkTexture() {
    if (ctx && !ctx->lost()) ctx->destroy_image(img);
}

// ---------------------------------------------------------------- sessions
class VkSession final : public Session {
public:
    explicit VkSession(std::shared_ptr<Context> c) : ctx_(std::move(c)) {}
    VkSession(const VkSession&) = delete;
    VkSession& operator=(const VkSession&) = delete;
    ~VkSession() override;
    Status init();
    Status run(param::Kernel k, std::uint32_t width, std::uint32_t height, std::span<const std::uint32_t> params,
               std::span<const std::uint32_t> data, const Texture& base, const Texture* overlay,
               std::span<std::uint8_t> rgba) override;

private:
    Status ensure(std::uint64_t staging_bytes, std::uint64_t params_bytes, std::uint64_t data_bytes, std::uint32_t w,
                  std::uint32_t h);
    std::shared_ptr<Context> ctx_;
    V::CommandPool pool_ = 0;
    V::CommandBuffer cb_ = nullptr;
    V::Fence fence_ = 0;
    V::DescriptorPool dpool_ = 0;
    V::DescriptorSet set_ = 0;
    Buf staging_, params_, data_, readback_;
    Img out_;
    std::uint32_t out_w_ = 0, out_h_ = 0;
};

Status VkSession::init() {
    Context& c = *ctx_;
    V::CommandPoolCreateInfo pci{V::st::CommandPoolCreateInfo, nullptr, V::kCommandPoolResetCommandBuffer, c.queue_family()};
    V::Result r = c.f.CreateCommandPool(c.dev, &pci, nullptr, &pool_);
    if (r != V::res::Success) {
        pool_ = 0;
        return vk_error("vkCreateCommandPool", r);
    }
    detail::count_object(1);
    V::CommandBufferAllocateInfo ai{V::st::CommandBufferAllocateInfo, nullptr, pool_, V::kCommandBufferPrimary, 1};
    r = c.f.AllocateCommandBuffers(c.dev, &ai, &cb_);
    if (r != V::res::Success) {
        cb_ = nullptr;
        return vk_error("vkAllocateCommandBuffers", r);
    }
    detail::count_object(1);
    V::FenceCreateInfo fci{V::st::FenceCreateInfo, nullptr, 0};
    r = c.f.CreateFence(c.dev, &fci, nullptr, &fence_);
    if (r != V::res::Success) {
        fence_ = 0;
        return vk_error("vkCreateFence", r);
    }
    detail::count_object(1);
    const V::DescriptorPoolSize sizes[3] = {{V::kDescStorageBuffer, 2}, {V::kDescCombinedImageSampler, 2}, {V::kDescStorageImage, 1}};
    V::DescriptorPoolCreateInfo dpci{V::st::DescriptorPoolCreateInfo, nullptr, 0, 1, 3, sizes};
    r = c.f.CreateDescriptorPool(c.dev, &dpci, nullptr, &dpool_);
    if (r != V::res::Success) {
        dpool_ = 0;
        return vk_error("vkCreateDescriptorPool", r);
    }
    detail::count_object(1);
    V::DescriptorSetAllocateInfo dai{V::st::DescriptorSetAllocateInfo, nullptr, dpool_, 1, &c.dsl};
    r = c.f.AllocateDescriptorSets(c.dev, &dai, &set_);
    if (r != V::res::Success) {
        set_ = 0;
        return vk_error("vkAllocateDescriptorSets", r);
    }
    return Status::ok();
}

Status VkSession::ensure(std::uint64_t staging_bytes, std::uint64_t params_bytes, std::uint64_t data_bytes, std::uint32_t w,
                         std::uint32_t h) {
    Context& c = *ctx_;
    auto grow = [&](Buf& b, std::uint64_t need, V::Flags usage, bool host) -> Status {
        if (b.buffer && b.size >= need) return Status::ok();
        c.destroy_buffer(b);
        // Some headroom so that small changes (e.g. slab sample counts) do not reallocate.
        std::uint64_t size = need + need / 4 + 256;
        return c.make_buffer(size, usage, host, b);
    };
    if (Status s = grow(staging_, staging_bytes, V::kBufferUsageTransferSrc, true); !s) return s;
    if (Status s = grow(params_, params_bytes, V::kBufferUsageStorage | V::kBufferUsageTransferDst, false); !s) return s;
    if (Status s = grow(data_, data_bytes, V::kBufferUsageStorage | V::kBufferUsageTransferDst, false); !s) return s;
    if (!out_.image || out_w_ < w || out_h_ < h) {
        c.destroy_image(out_);
        out_w_ = out_h_ = 0;
        const std::uint32_t nw = std::max(w, out_w_), nh = std::max(h, out_h_);
        if (Status s = c.make_image(V::kImageUsageStorage | V::kImageUsageTransferSrc, V::fmt::R32Uint, V::kImageType2D, nw, nh, 1, out_); !s)
            return s;
        out_w_ = nw;
        out_h_ = nh;
    }
    return grow(readback_, std::uint64_t{w} * h * 4u, V::kBufferUsageTransferDst, true);
}

Status VkSession::run(param::Kernel k, std::uint32_t width, std::uint32_t height, std::span<const std::uint32_t> params,
                      std::span<const std::uint32_t> data, const Texture& base, const Texture* overlay,
                      std::span<std::uint8_t> rgba) {
    Context& c = *ctx_;
    if (c.lost()) return Status(Errc::Unsupported, "vulkan: the device was lost");
    if (k >= param::KernelCount || width == 0 || height == 0) return Status(Errc::InvalidArgument, "vulkan: bad dispatch");
    if (rgba.size() < std::uint64_t{width} * height * 4u) return Status(Errc::BufferTooSmall, "vulkan: output too small");
    const auto& tb = static_cast<const VkTexture&>(base);
    const auto& to = overlay ? static_cast<const VkTexture&>(*overlay) : tb;
    if (tb.ctx.get() != &c || to.ctx.get() != &c) return Status(Errc::InvalidArgument, "vulkan: texture of another device");
    const std::uint64_t pbytes = params.size() * 4u;
    const std::uint64_t dwords = std::max<std::size_t>(data.size(), 4);
    const std::uint64_t dbytes = dwords * 4u;
    if (pbytes == 0 || pbytes > c.max_buffer_bytes() || dbytes > c.max_buffer_bytes())
        return Status(Errc::Unsupported, "vulkan: tables exceed the storage-buffer limit");
    const std::uint64_t data_off = (pbytes + 255u) & ~std::uint64_t{255};
    const std::uint64_t staging_need = data_off + dbytes;
    if (static_cast<std::size_t>(staging_need) != staging_need) return Status(Errc::LimitExceeded, "vulkan: tables too large");
    if (Status s = ensure(staging_need, pbytes, dbytes, width, height); !s) return s;
    auto* stage = static_cast<std::uint8_t*>(staging_.mapped);
    std::memcpy(stage, params.data(), params.size() * sizeof(std::uint32_t));
    if (!data.empty()) std::memcpy(stage + static_cast<std::size_t>(data_off), data.data(), data.size() * sizeof(std::uint32_t));

    V::DescriptorBufferInfo pinfo{params_.buffer, 0, pbytes};
    V::DescriptorBufferInfo dinfo{data_.buffer, 0, dbytes};
    V::DescriptorImageInfo i0{tb.linear_sampler ? c.sampler_linear : c.sampler_nearest, tb.img.view, V::kLayoutShaderReadOnly};
    V::DescriptorImageInfo i1{to.linear_sampler ? c.sampler_linear : c.sampler_nearest, to.img.view, V::kLayoutShaderReadOnly};
    V::DescriptorImageInfo io{0, out_.view, V::kLayoutGeneral};
    const V::WriteDescriptorSet w[5] = {
        {V::st::WriteDescriptorSet, nullptr, set_, 0, 0, 1, V::kDescStorageBuffer, nullptr, &pinfo, nullptr},
        {V::st::WriteDescriptorSet, nullptr, set_, 1, 0, 1, V::kDescStorageBuffer, nullptr, &dinfo, nullptr},
        {V::st::WriteDescriptorSet, nullptr, set_, 2, 0, 1, V::kDescCombinedImageSampler, &i0, nullptr, nullptr},
        {V::st::WriteDescriptorSet, nullptr, set_, 3, 0, 1, V::kDescCombinedImageSampler, &i1, nullptr, nullptr},
        {V::st::WriteDescriptorSet, nullptr, set_, 4, 0, 1, V::kDescStorageImage, &io, nullptr, nullptr},
    };
    c.f.UpdateDescriptorSets(c.dev, 5, w, 0, nullptr);

    V::Result r = c.f.ResetCommandBuffer(cb_, 0);
    if (r != V::res::Success) return vk_error("vkResetCommandBuffer", r);
    V::CommandBufferBeginInfo bi{V::st::CommandBufferBeginInfo, nullptr, V::kCommandBufferOneTimeSubmit, nullptr};
    r = c.f.BeginCommandBuffer(cb_, &bi);
    if (r != V::res::Success) return vk_error("vkBeginCommandBuffer", r);
    // Order this frame's transfer writes after the previous frame's GPU work on the same buffers
    // (write-after-read on params / data, write-after-write on the read-back buffer).
    const V::BufferMemoryBarrier pre[3] = {
        {V::st::BufferMemoryBarrier, nullptr, 0, V::kAccessTransferWrite, V::kQueueFamilyIgnored, V::kQueueFamilyIgnored,
         params_.buffer, 0, V::kWholeSize},
        {V::st::BufferMemoryBarrier, nullptr, 0, V::kAccessTransferWrite, V::kQueueFamilyIgnored, V::kQueueFamilyIgnored,
         data_.buffer, 0, V::kWholeSize},
        {V::st::BufferMemoryBarrier, nullptr, V::kAccessTransferWrite, V::kAccessTransferWrite, V::kQueueFamilyIgnored,
         V::kQueueFamilyIgnored, readback_.buffer, 0, V::kWholeSize},
    };
    c.f.CmdPipelineBarrier(cb_, V::kStageTransfer | V::kStageComputeShader, V::kStageTransfer, 0, 0, nullptr, 3, pre, 0, nullptr);
    const V::BufferCopy pc{0, 0, pbytes};
    c.f.CmdCopyBuffer(cb_, staging_.buffer, params_.buffer, 1, &pc);
    const V::BufferCopy dc{data_off, 0, dbytes};
    c.f.CmdCopyBuffer(cb_, staging_.buffer, data_.buffer, 1, &dc);
    const V::BufferMemoryBarrier bb[2] = {
        {V::st::BufferMemoryBarrier, nullptr, V::kAccessTransferWrite, V::kAccessShaderRead, V::kQueueFamilyIgnored,
         V::kQueueFamilyIgnored, params_.buffer, 0, V::kWholeSize},
        {V::st::BufferMemoryBarrier, nullptr, V::kAccessTransferWrite, V::kAccessShaderRead, V::kQueueFamilyIgnored,
         V::kQueueFamilyIgnored, data_.buffer, 0, V::kWholeSize},
    };
    // The output image's previous contents are not needed: UNDEFINED -> GENERAL.
    const V::ImageMemoryBarrier ib = image_barrier(out_.image, 0, V::kAccessShaderWrite, V::kLayoutUndefined, V::kLayoutGeneral);
    c.f.CmdPipelineBarrier(cb_, V::kStageTransfer | V::kStageTopOfPipe, V::kStageComputeShader, 0, 0, nullptr, 2, bb, 1, &ib);
    c.f.CmdBindPipeline(cb_, V::kBindPointCompute, c.pipelines[k]);
    c.f.CmdBindDescriptorSets(cb_, V::kBindPointCompute, c.layout, 0, 1, &set_, 0, nullptr);
    c.f.CmdDispatch(cb_, (width + param::kGroupX - 1) / param::kGroupX, (height + param::kGroupY - 1) / param::kGroupY, 1);
    const V::ImageMemoryBarrier ib2 = image_barrier(out_.image, V::kAccessShaderWrite, V::kAccessTransferRead, V::kLayoutGeneral,
                                                    V::kLayoutTransferSrc);
    c.f.CmdPipelineBarrier(cb_, V::kStageComputeShader, V::kStageTransfer, 0, 0, nullptr, 0, nullptr, 1, &ib2);
    V::BufferImageCopy region{0, 0, 0, {V::kAspectColor, 0, 0, 1}, {0, 0, 0}, {width, height, 1}};
    c.f.CmdCopyImageToBuffer(cb_, out_.image, V::kLayoutTransferSrc, readback_.buffer, 1, &region);
    const V::BufferMemoryBarrier hb{V::st::BufferMemoryBarrier, nullptr, V::kAccessTransferWrite, V::kAccessHostRead,
                                    V::kQueueFamilyIgnored, V::kQueueFamilyIgnored, readback_.buffer, 0, V::kWholeSize};
    c.f.CmdPipelineBarrier(cb_, V::kStageTransfer, V::kStageHost, 0, 0, nullptr, 1, &hb, 0, nullptr);
    r = c.f.EndCommandBuffer(cb_);
    if (r != V::res::Success) return vk_error("vkEndCommandBuffer", r);
    if (Status s = c.submit_wait(cb_, fence_); !s) return s;
    // Unpack R | G << 8 | B << 16 | A << 24 into bytes (independent of the host byte order).
    const auto* px = static_cast<const std::uint32_t*>(readback_.mapped);
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::uint8_t* o = rgba.data();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t v = px[i];
        o[4 * i] = static_cast<std::uint8_t>(v);
        o[4 * i + 1] = static_cast<std::uint8_t>(v >> 8);
        o[4 * i + 2] = static_cast<std::uint8_t>(v >> 16);
        o[4 * i + 3] = static_cast<std::uint8_t>(v >> 24);
    }
    return Status::ok();
}

VkSession::~VkSession() {
    Context& c = *ctx_;
    if (c.lost()) return;  // the device may still use these: leave them to the driver
    c.destroy_buffer(staging_);
    c.destroy_buffer(params_);
    c.destroy_buffer(data_);
    c.destroy_buffer(readback_);
    c.destroy_image(out_);
    if (dpool_) {
        c.f.DestroyDescriptorPool(c.dev, dpool_, nullptr);  // frees set_
        detail::count_object(-1);
    }
    if (fence_) {
        c.f.DestroyFence(c.dev, fence_, nullptr);
        detail::count_object(-1);
    }
    if (cb_) {
        c.f.FreeCommandBuffers(c.dev, pool_, 1, &cb_);
        detail::count_object(-1);
    }
    if (pool_) {
        c.f.DestroyCommandPool(c.dev, pool_, nullptr);
        detail::count_object(-1);
    }
}

Result<std::unique_ptr<Session>> Context::session() {
    if (lost_.load()) return Status(Errc::Unsupported, "vulkan: the device was lost");
    std::unique_ptr<VkSession> s;
    try {
        s = std::make_unique<VkSession>(shared_from_this());
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "vulkan: out of memory");
    }
    if (Status st = s->init(); !st) return st;
    return std::unique_ptr<Session>(std::move(s));
}

std::mutex& probe_mutex() {
    static std::mutex m;
    return m;
}
std::shared_ptr<InstanceHolder>& probed() {
    static std::shared_ptr<InstanceHolder>* p = new std::shared_ptr<InstanceHolder>();  // never destroyed
    return *p;
}

}  // namespace

std::string default_loader_name() {
#if defined(_WIN32)
    return "vulkan-1.dll";
#elif defined(__APPLE__)
    const char* on = std::getenv("SIAARA_VULKAN_ON_APPLE");
    return on && std::strcmp(on, "1") == 0 ? "libvulkan.1.dylib" : "";
#else
    return "libvulkan.so.1";
#endif
}

detail::Probe probe(const std::string& loader_name) {
    detail::Probe pr;
    std::lock_guard<std::mutex> lock(probe_mutex());
    probed().reset();
    if (loader_name.empty()) {
        pr.reason = "Vulkan is disabled on this platform";
        return pr;
    }
    try {
        auto ih = make_instance(loader_name);
        if (!ih) {
            pr.reason = ih.error().message;
            return pr;
        }
        for (const auto& p : (*ih)->phys) pr.devices.push_back(p.info);
        if (pr.devices.empty()) pr.reason = "vulkan: no device with compute and the required formats";
        probed() = std::move(ih).value();
    } catch (const std::bad_alloc&) {
        pr.devices.clear();
        pr.reason = "vulkan: out of memory";
    }
    return pr;
}

Result<std::shared_ptr<Backend>> create(std::size_t index) {
    std::shared_ptr<InstanceHolder> ih;
    {
        std::lock_guard<std::mutex> lock(probe_mutex());
        ih = probed();
    }
    if (!ih) return Status(Errc::Unsupported, "vulkan: not probed");
    try {
        auto c = std::make_shared<Context>();
        if (Status s = c->init(std::move(ih), index); !s) return s;
        return std::shared_ptr<Backend>(std::move(c));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "vulkan: out of memory");
    }
}

void release() {
    std::lock_guard<std::mutex> lock(probe_mutex());
    probed().reset();
}

}  // namespace siaara::gpu::vulkan
