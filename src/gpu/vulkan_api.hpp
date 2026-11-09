// SIAARA DICOM Toolkit - the subset of the Vulkan 1.0 API used by the Vulkan backend (internal).
//
// Declared here from the Vulkan specification (chapters "Fundamentals", "Initialization",
// "Devices and Queues", "Command Buffers", "Synchronization and Cache Control", "Shaders",
// "Pipelines", "Memory Allocation", "Resource Creation", "Samplers", "Resource Descriptors",
// "Dispatching Commands", "Copy Commands", "Formats", "Limits"), not from the Khronos headers.
// Only the types, enumerants and commands the backend uses are declared. Structures follow the
// specification's member order exactly; with the platform's C layout rules they are
// ABI-identical to the official declarations.
//
// Handles: dispatchable handles are pointers; non-dispatchable handles are 64-bit integers on
// every platform (the specification allows either a pointer or a uint64_t; both have the same
// size and are passed identically on 64-bit ABIs, and uint64_t is what 32-bit ABIs use).
// Calling convention: VKAPI_CALL is __stdcall on 32-bit Windows, the default elsewhere.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64) && (defined(_M_IX86) || defined(__i386__))
#define SIAARA_VKAPI __stdcall
#else
#define SIAARA_VKAPI
#endif

namespace siaara::gpu::vk {

// ---------------------------------------------------------------- basic types
using Bool32 = std::uint32_t;
using DeviceSize = std::uint64_t;
using Flags = std::uint32_t;
using Result = std::int32_t;

struct InstanceT;
struct PhysicalDeviceT;
struct DeviceT;
struct QueueT;
struct CommandBufferT;
using Instance = InstanceT*;
using PhysicalDevice = PhysicalDeviceT*;
using Device = DeviceT*;
using Queue = QueueT*;
using CommandBuffer = CommandBufferT*;

using Buffer = std::uint64_t;
using Image = std::uint64_t;
using ImageView = std::uint64_t;
using DeviceMemory = std::uint64_t;
using Sampler = std::uint64_t;
using DescriptorSetLayout = std::uint64_t;
using DescriptorPool = std::uint64_t;
using DescriptorSet = std::uint64_t;
using PipelineLayout = std::uint64_t;
using Pipeline = std::uint64_t;
using PipelineCache = std::uint64_t;
using ShaderModule = std::uint64_t;
using CommandPool = std::uint64_t;
using Fence = std::uint64_t;
using Semaphore = std::uint64_t;

constexpr std::uint32_t kQueueFamilyIgnored = ~0u;
constexpr DeviceSize kWholeSize = ~0ull;
constexpr std::uint32_t kMaxPhysicalDeviceNameSize = 256;
constexpr std::uint32_t kMaxExtensionNameSize = 256;
constexpr std::uint32_t kUuidSize = 16;
constexpr std::uint32_t kMaxMemoryTypes = 32;
constexpr std::uint32_t kMaxMemoryHeaps = 16;

constexpr std::uint32_t make_api_version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}

// ---------------------------------------------------------------- enumerants
namespace st {  // VkStructureType
constexpr std::int32_t ApplicationInfo = 0, InstanceCreateInfo = 1, DeviceQueueCreateInfo = 2, DeviceCreateInfo = 3,
                       SubmitInfo = 4, MemoryAllocateInfo = 5, FenceCreateInfo = 8, BufferCreateInfo = 12,
                       ImageCreateInfo = 14, ImageViewCreateInfo = 15, ShaderModuleCreateInfo = 16,
                       PipelineShaderStageCreateInfo = 18, ComputePipelineCreateInfo = 29,
                       PipelineLayoutCreateInfo = 30, SamplerCreateInfo = 31, DescriptorSetLayoutCreateInfo = 32,
                       DescriptorPoolCreateInfo = 33, DescriptorSetAllocateInfo = 34, WriteDescriptorSet = 35,
                       CommandPoolCreateInfo = 39, CommandBufferAllocateInfo = 40, CommandBufferBeginInfo = 42,
                       BufferMemoryBarrier = 44, ImageMemoryBarrier = 45;
}  // namespace st

namespace res {  // VkResult
constexpr Result Success = 0, NotReady = 1, Timeout = 2, Incomplete = 5, ErrorOutOfHostMemory = -1,
                 ErrorOutOfDeviceMemory = -2, ErrorInitializationFailed = -3, ErrorDeviceLost = -4,
                 ErrorMemoryMapFailed = -5, ErrorLayerNotPresent = -6, ErrorExtensionNotPresent = -7,
                 ErrorFeatureNotPresent = -8, ErrorIncompatibleDriver = -9, ErrorTooManyObjects = -10,
                 ErrorFormatNotSupported = -11;
}  // namespace res

namespace fmt {  // VkFormat
constexpr std::int32_t R16Unorm = 70, R16Sfloat = 76, R32Uint = 98, R32Sfloat = 100;
}

// VkPhysicalDeviceType
constexpr std::int32_t kDeviceTypeOther = 0, kDeviceTypeIntegrated = 1, kDeviceTypeDiscrete = 2,
                       kDeviceTypeVirtual = 3, kDeviceTypeCpu = 4;
// VkQueueFlagBits
constexpr Flags kQueueCompute = 0x2;
// VkFormatFeatureFlagBits
constexpr Flags kFormatSampledImage = 0x1, kFormatStorageImage = 0x2, kFormatSampledImageFilterLinear = 0x1000;
// VkImageUsageFlagBits
constexpr Flags kImageUsageTransferSrc = 0x1, kImageUsageTransferDst = 0x2, kImageUsageSampled = 0x4,
                kImageUsageStorage = 0x8;
// VkBufferUsageFlagBits
constexpr Flags kBufferUsageTransferSrc = 0x1, kBufferUsageTransferDst = 0x2, kBufferUsageStorage = 0x20;
// VkMemoryPropertyFlagBits / VkMemoryHeapFlagBits
constexpr Flags kMemoryDeviceLocal = 0x1, kMemoryHostVisible = 0x2, kMemoryHostCoherent = 0x4, kMemoryHostCached = 0x8;
constexpr Flags kHeapDeviceLocal = 0x1;
// VkImageType / VkImageViewType
constexpr std::int32_t kImageType2D = 1, kImageType3D = 2;
constexpr std::int32_t kViewType2D = 1, kViewType3D = 2;
// VkImageLayout
constexpr std::int32_t kLayoutUndefined = 0, kLayoutGeneral = 1, kLayoutShaderReadOnly = 5,
                       kLayoutTransferSrc = 6, kLayoutTransferDst = 7;
// VkImageTiling, VkSampleCountFlagBits, VkSharingMode
constexpr std::int32_t kTilingOptimal = 0;
constexpr Flags kSampleCount1 = 0x1;
constexpr std::int32_t kSharingExclusive = 0;
// VkFilter, VkSamplerMipmapMode, VkSamplerAddressMode, VkBorderColor, VkCompareOp
constexpr std::int32_t kFilterNearest = 0, kFilterLinear = 1;
constexpr std::int32_t kMipmapNearest = 0;
constexpr std::int32_t kAddressClampToEdge = 2;
constexpr std::int32_t kBorderFloatTransparentBlack = 0;
constexpr std::int32_t kCompareNever = 0;
// VkDescriptorType
constexpr std::int32_t kDescCombinedImageSampler = 1, kDescStorageImage = 3, kDescStorageBuffer = 7;
// VkShaderStageFlagBits, VkPipelineBindPoint
constexpr Flags kStageCompute = 0x20;
constexpr std::int32_t kBindPointCompute = 1;
// VkCommandBufferLevel, VkCommandBufferUsageFlagBits, VkCommandPoolCreateFlagBits
constexpr std::int32_t kCommandBufferPrimary = 0;
constexpr Flags kCommandBufferOneTimeSubmit = 0x1;
constexpr Flags kCommandPoolResetCommandBuffer = 0x2;
// VkAccessFlagBits
constexpr Flags kAccessShaderRead = 0x20, kAccessShaderWrite = 0x40, kAccessTransferRead = 0x800,
                kAccessTransferWrite = 0x1000, kAccessHostRead = 0x2000, kAccessHostWrite = 0x4000;
// VkPipelineStageFlagBits
constexpr Flags kStageTopOfPipe = 0x1, kStageComputeShader = 0x800, kStageTransfer = 0x1000, kStageHost = 0x4000;
// VkImageAspectFlagBits
constexpr Flags kAspectColor = 0x1;
// VkInstanceCreateFlagBits (VK_KHR_portability_enumeration)
constexpr Flags kInstanceEnumeratePortability = 0x1;

// ---------------------------------------------------------------- structures
struct ApplicationInfo {
    std::int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    std::uint32_t applicationVersion;
    const char* pEngineName;
    std::uint32_t engineVersion;
    std::uint32_t apiVersion;
};

struct InstanceCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    const ApplicationInfo* pApplicationInfo;
    std::uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

struct ExtensionProperties {
    char extensionName[kMaxExtensionNameSize];
    std::uint32_t specVersion;
};

struct PhysicalDeviceLimits {
    std::uint32_t maxImageDimension1D;
    std::uint32_t maxImageDimension2D;
    std::uint32_t maxImageDimension3D;
    std::uint32_t maxImageDimensionCube;
    std::uint32_t maxImageArrayLayers;
    std::uint32_t maxTexelBufferElements;
    std::uint32_t maxUniformBufferRange;
    std::uint32_t maxStorageBufferRange;
    std::uint32_t maxPushConstantsSize;
    std::uint32_t maxMemoryAllocationCount;
    std::uint32_t maxSamplerAllocationCount;
    DeviceSize bufferImageGranularity;
    DeviceSize sparseAddressSpaceSize;
    std::uint32_t maxBoundDescriptorSets;
    std::uint32_t maxPerStageDescriptorSamplers;
    std::uint32_t maxPerStageDescriptorUniformBuffers;
    std::uint32_t maxPerStageDescriptorStorageBuffers;
    std::uint32_t maxPerStageDescriptorSampledImages;
    std::uint32_t maxPerStageDescriptorStorageImages;
    std::uint32_t maxPerStageDescriptorInputAttachments;
    std::uint32_t maxPerStageResources;
    std::uint32_t maxDescriptorSetSamplers;
    std::uint32_t maxDescriptorSetUniformBuffers;
    std::uint32_t maxDescriptorSetUniformBuffersDynamic;
    std::uint32_t maxDescriptorSetStorageBuffers;
    std::uint32_t maxDescriptorSetStorageBuffersDynamic;
    std::uint32_t maxDescriptorSetSampledImages;
    std::uint32_t maxDescriptorSetStorageImages;
    std::uint32_t maxDescriptorSetInputAttachments;
    std::uint32_t maxVertexInputAttributes;
    std::uint32_t maxVertexInputBindings;
    std::uint32_t maxVertexInputAttributeOffset;
    std::uint32_t maxVertexInputBindingStride;
    std::uint32_t maxVertexOutputComponents;
    std::uint32_t maxTessellationGenerationLevel;
    std::uint32_t maxTessellationPatchSize;
    std::uint32_t maxTessellationControlPerVertexInputComponents;
    std::uint32_t maxTessellationControlPerVertexOutputComponents;
    std::uint32_t maxTessellationControlPerPatchOutputComponents;
    std::uint32_t maxTessellationControlTotalOutputComponents;
    std::uint32_t maxTessellationEvaluationInputComponents;
    std::uint32_t maxTessellationEvaluationOutputComponents;
    std::uint32_t maxGeometryShaderInvocations;
    std::uint32_t maxGeometryInputComponents;
    std::uint32_t maxGeometryOutputComponents;
    std::uint32_t maxGeometryOutputVertices;
    std::uint32_t maxGeometryTotalOutputComponents;
    std::uint32_t maxFragmentInputComponents;
    std::uint32_t maxFragmentOutputAttachments;
    std::uint32_t maxFragmentDualSrcAttachments;
    std::uint32_t maxFragmentCombinedOutputResources;
    std::uint32_t maxComputeSharedMemorySize;
    std::uint32_t maxComputeWorkGroupCount[3];
    std::uint32_t maxComputeWorkGroupInvocations;
    std::uint32_t maxComputeWorkGroupSize[3];
    std::uint32_t subPixelPrecisionBits;
    std::uint32_t subTexelPrecisionBits;
    std::uint32_t mipmapPrecisionBits;
    std::uint32_t maxDrawIndexedIndexValue;
    std::uint32_t maxDrawIndirectCount;
    float maxSamplerLodBias;
    float maxSamplerAnisotropy;
    std::uint32_t maxViewports;
    std::uint32_t maxViewportDimensions[2];
    float viewportBoundsRange[2];
    std::uint32_t viewportSubPixelBits;
    std::size_t minMemoryMapAlignment;
    DeviceSize minTexelBufferOffsetAlignment;
    DeviceSize minUniformBufferOffsetAlignment;
    DeviceSize minStorageBufferOffsetAlignment;
    std::int32_t minTexelOffset;
    std::uint32_t maxTexelOffset;
    std::int32_t minTexelGatherOffset;
    std::uint32_t maxTexelGatherOffset;
    float minInterpolationOffset;
    float maxInterpolationOffset;
    std::uint32_t subPixelInterpolationOffsetBits;
    std::uint32_t maxFramebufferWidth;
    std::uint32_t maxFramebufferHeight;
    std::uint32_t maxFramebufferLayers;
    Flags framebufferColorSampleCounts;
    Flags framebufferDepthSampleCounts;
    Flags framebufferStencilSampleCounts;
    Flags framebufferNoAttachmentsSampleCounts;
    std::uint32_t maxColorAttachments;
    Flags sampledImageColorSampleCounts;
    Flags sampledImageIntegerSampleCounts;
    Flags sampledImageDepthSampleCounts;
    Flags sampledImageStencilSampleCounts;
    Flags storageImageSampleCounts;
    std::uint32_t maxSampleMaskWords;
    Bool32 timestampComputeAndGraphics;
    float timestampPeriod;
    std::uint32_t maxClipDistances;
    std::uint32_t maxCullDistances;
    std::uint32_t maxCombinedClipAndCullDistances;
    std::uint32_t discreteQueuePriorities;
    float pointSizeRange[2];
    float lineWidthRange[2];
    float pointSizeGranularity;
    float lineWidthGranularity;
    Bool32 strictLines;
    Bool32 standardSampleLocations;
    DeviceSize optimalBufferCopyOffsetAlignment;
    DeviceSize optimalBufferCopyRowPitchAlignment;
    DeviceSize nonCoherentAtomSize;
};

struct PhysicalDeviceSparseProperties {
    Bool32 residencyStandard2DBlockShape;
    Bool32 residencyStandard2DMultisampleBlockShape;
    Bool32 residencyStandard3DBlockShape;
    Bool32 residencyAlignedMipSize;
    Bool32 residencyNonResidentStrict;
};

struct PhysicalDeviceProperties {
    std::uint32_t apiVersion;
    std::uint32_t driverVersion;
    std::uint32_t vendorID;
    std::uint32_t deviceID;
    std::int32_t deviceType;
    char deviceName[kMaxPhysicalDeviceNameSize];
    std::uint8_t pipelineCacheUUID[kUuidSize];
    PhysicalDeviceLimits limits;
    PhysicalDeviceSparseProperties sparseProperties;
};

struct MemoryType {
    Flags propertyFlags;
    std::uint32_t heapIndex;
};
struct MemoryHeap {
    DeviceSize size;
    Flags flags;
};
struct PhysicalDeviceMemoryProperties {
    std::uint32_t memoryTypeCount;
    MemoryType memoryTypes[kMaxMemoryTypes];
    std::uint32_t memoryHeapCount;
    MemoryHeap memoryHeaps[kMaxMemoryHeaps];
};

struct Extent3D {
    std::uint32_t width, height, depth;
};
struct Offset3D {
    std::int32_t x, y, z;
};

struct QueueFamilyProperties {
    Flags queueFlags;
    std::uint32_t queueCount;
    std::uint32_t timestampValidBits;
    Extent3D minImageTransferGranularity;
};

struct FormatProperties {
    Flags linearTilingFeatures;
    Flags optimalTilingFeatures;
    Flags bufferFeatures;
};

struct DeviceQueueCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::uint32_t queueFamilyIndex;
    std::uint32_t queueCount;
    const float* pQueuePriorities;
};

struct DeviceCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::uint32_t queueCreateInfoCount;
    const DeviceQueueCreateInfo* pQueueCreateInfos;
    std::uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;  // const VkPhysicalDeviceFeatures*; always NULL here
};

struct CommandPoolCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::uint32_t queueFamilyIndex;
};

struct CommandBufferAllocateInfo {
    std::int32_t sType;
    const void* pNext;
    CommandPool commandPool;
    std::int32_t level;
    std::uint32_t commandBufferCount;
};

struct CommandBufferBeginInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    const void* pInheritanceInfo;
};

struct BufferCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    DeviceSize size;
    Flags usage;
    std::int32_t sharingMode;
    std::uint32_t queueFamilyIndexCount;
    const std::uint32_t* pQueueFamilyIndices;
};

struct MemoryRequirements {
    DeviceSize size;
    DeviceSize alignment;
    std::uint32_t memoryTypeBits;
};

struct MemoryAllocateInfo {
    std::int32_t sType;
    const void* pNext;
    DeviceSize allocationSize;
    std::uint32_t memoryTypeIndex;
};

struct ImageCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::int32_t imageType;
    std::int32_t format;
    Extent3D extent;
    std::uint32_t mipLevels;
    std::uint32_t arrayLayers;
    Flags samples;
    std::int32_t tiling;
    Flags usage;
    std::int32_t sharingMode;
    std::uint32_t queueFamilyIndexCount;
    const std::uint32_t* pQueueFamilyIndices;
    std::int32_t initialLayout;
};

struct ComponentMapping {
    std::int32_t r, g, b, a;  // VK_COMPONENT_SWIZZLE_IDENTITY = 0
};
struct ImageSubresourceRange {
    Flags aspectMask;
    std::uint32_t baseMipLevel;
    std::uint32_t levelCount;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};
struct ImageSubresourceLayers {
    Flags aspectMask;
    std::uint32_t mipLevel;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct ImageViewCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    Image image;
    std::int32_t viewType;
    std::int32_t format;
    ComponentMapping components;
    ImageSubresourceRange subresourceRange;
};

struct SamplerCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::int32_t magFilter;
    std::int32_t minFilter;
    std::int32_t mipmapMode;
    std::int32_t addressModeU;
    std::int32_t addressModeV;
    std::int32_t addressModeW;
    float mipLodBias;
    Bool32 anisotropyEnable;
    float maxAnisotropy;
    Bool32 compareEnable;
    std::int32_t compareOp;
    float minLod;
    float maxLod;
    std::int32_t borderColor;
    Bool32 unnormalizedCoordinates;
};

struct DescriptorSetLayoutBinding {
    std::uint32_t binding;
    std::int32_t descriptorType;
    std::uint32_t descriptorCount;
    Flags stageFlags;
    const Sampler* pImmutableSamplers;
};

struct DescriptorSetLayoutCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::uint32_t bindingCount;
    const DescriptorSetLayoutBinding* pBindings;
};

struct PipelineLayoutCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::uint32_t setLayoutCount;
    const DescriptorSetLayout* pSetLayouts;
    std::uint32_t pushConstantRangeCount;
    const void* pPushConstantRanges;  // const VkPushConstantRange*; none here
};

struct ShaderModuleCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::size_t codeSize;
    const std::uint32_t* pCode;
};

struct PipelineShaderStageCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    Flags stage;
    ShaderModule module;
    const char* pName;
    const void* pSpecializationInfo;  // const VkSpecializationInfo*; none here
};

struct ComputePipelineCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    PipelineShaderStageCreateInfo stage;
    PipelineLayout layout;
    Pipeline basePipelineHandle;
    std::int32_t basePipelineIndex;
};

struct DescriptorPoolSize {
    std::int32_t type;
    std::uint32_t descriptorCount;
};

struct DescriptorPoolCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
    std::uint32_t maxSets;
    std::uint32_t poolSizeCount;
    const DescriptorPoolSize* pPoolSizes;
};

struct DescriptorSetAllocateInfo {
    std::int32_t sType;
    const void* pNext;
    DescriptorPool descriptorPool;
    std::uint32_t descriptorSetCount;
    const DescriptorSetLayout* pSetLayouts;
};

struct DescriptorImageInfo {
    Sampler sampler;
    ImageView imageView;
    std::int32_t imageLayout;
};

struct DescriptorBufferInfo {
    Buffer buffer;
    DeviceSize offset;
    DeviceSize range;
};

struct WriteDescriptorSet {
    std::int32_t sType;
    const void* pNext;
    DescriptorSet dstSet;
    std::uint32_t dstBinding;
    std::uint32_t dstArrayElement;
    std::uint32_t descriptorCount;
    std::int32_t descriptorType;
    const DescriptorImageInfo* pImageInfo;
    const DescriptorBufferInfo* pBufferInfo;
    const void* pTexelBufferView;  // const VkBufferView*; none here
};

struct BufferMemoryBarrier {
    std::int32_t sType;
    const void* pNext;
    Flags srcAccessMask;
    Flags dstAccessMask;
    std::uint32_t srcQueueFamilyIndex;
    std::uint32_t dstQueueFamilyIndex;
    Buffer buffer;
    DeviceSize offset;
    DeviceSize size;
};

struct ImageMemoryBarrier {
    std::int32_t sType;
    const void* pNext;
    Flags srcAccessMask;
    Flags dstAccessMask;
    std::int32_t oldLayout;
    std::int32_t newLayout;
    std::uint32_t srcQueueFamilyIndex;
    std::uint32_t dstQueueFamilyIndex;
    Image image;
    ImageSubresourceRange subresourceRange;
};

struct BufferCopy {
    DeviceSize srcOffset;
    DeviceSize dstOffset;
    DeviceSize size;
};

struct BufferImageCopy {
    DeviceSize bufferOffset;
    std::uint32_t bufferRowLength;
    std::uint32_t bufferImageHeight;
    ImageSubresourceLayers imageSubresource;
    Offset3D imageOffset;
    Extent3D imageExtent;
};

struct SubmitInfo {
    std::int32_t sType;
    const void* pNext;
    std::uint32_t waitSemaphoreCount;
    const Semaphore* pWaitSemaphores;
    const Flags* pWaitDstStageMask;
    std::uint32_t commandBufferCount;
    const CommandBuffer* pCommandBuffers;
    std::uint32_t signalSemaphoreCount;
    const Semaphore* pSignalSemaphores;
};

struct FenceCreateInfo {
    std::int32_t sType;
    const void* pNext;
    Flags flags;
};

// ---------------------------------------------------------------- commands
using VoidFunction = void(SIAARA_VKAPI*)();
using PFN_GetInstanceProcAddr = VoidFunction(SIAARA_VKAPI*)(Instance, const char*);
using PFN_GetDeviceProcAddr = VoidFunction(SIAARA_VKAPI*)(Device, const char*);

// Global commands (instance == NULL).
using PFN_CreateInstance = Result(SIAARA_VKAPI*)(const InstanceCreateInfo*, const void*, Instance*);
using PFN_EnumerateInstanceExtensionProperties = Result(SIAARA_VKAPI*)(const char*, std::uint32_t*, ExtensionProperties*);

// Instance commands.
using PFN_DestroyInstance = void(SIAARA_VKAPI*)(Instance, const void*);
using PFN_EnumeratePhysicalDevices = Result(SIAARA_VKAPI*)(Instance, std::uint32_t*, PhysicalDevice*);
using PFN_GetPhysicalDeviceProperties = void(SIAARA_VKAPI*)(PhysicalDevice, PhysicalDeviceProperties*);
using PFN_GetPhysicalDeviceMemoryProperties = void(SIAARA_VKAPI*)(PhysicalDevice, PhysicalDeviceMemoryProperties*);
using PFN_GetPhysicalDeviceQueueFamilyProperties = void(SIAARA_VKAPI*)(PhysicalDevice, std::uint32_t*, QueueFamilyProperties*);
using PFN_GetPhysicalDeviceFormatProperties = void(SIAARA_VKAPI*)(PhysicalDevice, std::int32_t, FormatProperties*);
using PFN_EnumerateDeviceExtensionProperties = Result(SIAARA_VKAPI*)(PhysicalDevice, const char*, std::uint32_t*,
                                                                     ExtensionProperties*);
using PFN_CreateDevice = Result(SIAARA_VKAPI*)(PhysicalDevice, const DeviceCreateInfo*, const void*, Device*);

// Device commands.
using PFN_DestroyDevice = void(SIAARA_VKAPI*)(Device, const void*);
using PFN_GetDeviceQueue = void(SIAARA_VKAPI*)(Device, std::uint32_t, std::uint32_t, Queue*);
using PFN_DeviceWaitIdle = Result(SIAARA_VKAPI*)(Device);
using PFN_CreateCommandPool = Result(SIAARA_VKAPI*)(Device, const CommandPoolCreateInfo*, const void*, CommandPool*);
using PFN_DestroyCommandPool = void(SIAARA_VKAPI*)(Device, CommandPool, const void*);
using PFN_AllocateCommandBuffers = Result(SIAARA_VKAPI*)(Device, const CommandBufferAllocateInfo*, CommandBuffer*);
using PFN_FreeCommandBuffers = void(SIAARA_VKAPI*)(Device, CommandPool, std::uint32_t, const CommandBuffer*);
using PFN_BeginCommandBuffer = Result(SIAARA_VKAPI*)(CommandBuffer, const CommandBufferBeginInfo*);
using PFN_EndCommandBuffer = Result(SIAARA_VKAPI*)(CommandBuffer);
using PFN_ResetCommandBuffer = Result(SIAARA_VKAPI*)(CommandBuffer, Flags);
using PFN_CreateBuffer = Result(SIAARA_VKAPI*)(Device, const BufferCreateInfo*, const void*, Buffer*);
using PFN_DestroyBuffer = void(SIAARA_VKAPI*)(Device, Buffer, const void*);
using PFN_GetBufferMemoryRequirements = void(SIAARA_VKAPI*)(Device, Buffer, MemoryRequirements*);
using PFN_BindBufferMemory = Result(SIAARA_VKAPI*)(Device, Buffer, DeviceMemory, DeviceSize);
using PFN_CreateImage = Result(SIAARA_VKAPI*)(Device, const ImageCreateInfo*, const void*, Image*);
using PFN_DestroyImage = void(SIAARA_VKAPI*)(Device, Image, const void*);
using PFN_GetImageMemoryRequirements = void(SIAARA_VKAPI*)(Device, Image, MemoryRequirements*);
using PFN_BindImageMemory = Result(SIAARA_VKAPI*)(Device, Image, DeviceMemory, DeviceSize);
using PFN_AllocateMemory = Result(SIAARA_VKAPI*)(Device, const MemoryAllocateInfo*, const void*, DeviceMemory*);
using PFN_FreeMemory = void(SIAARA_VKAPI*)(Device, DeviceMemory, const void*);
using PFN_MapMemory = Result(SIAARA_VKAPI*)(Device, DeviceMemory, DeviceSize, DeviceSize, Flags, void**);
using PFN_UnmapMemory = void(SIAARA_VKAPI*)(Device, DeviceMemory);
using PFN_CreateImageView = Result(SIAARA_VKAPI*)(Device, const ImageViewCreateInfo*, const void*, ImageView*);
using PFN_DestroyImageView = void(SIAARA_VKAPI*)(Device, ImageView, const void*);
using PFN_CreateSampler = Result(SIAARA_VKAPI*)(Device, const SamplerCreateInfo*, const void*, Sampler*);
using PFN_DestroySampler = void(SIAARA_VKAPI*)(Device, Sampler, const void*);
using PFN_CreateDescriptorSetLayout = Result(SIAARA_VKAPI*)(Device, const DescriptorSetLayoutCreateInfo*, const void*,
                                                            DescriptorSetLayout*);
using PFN_DestroyDescriptorSetLayout = void(SIAARA_VKAPI*)(Device, DescriptorSetLayout, const void*);
using PFN_CreatePipelineLayout = Result(SIAARA_VKAPI*)(Device, const PipelineLayoutCreateInfo*, const void*, PipelineLayout*);
using PFN_DestroyPipelineLayout = void(SIAARA_VKAPI*)(Device, PipelineLayout, const void*);
using PFN_CreateShaderModule = Result(SIAARA_VKAPI*)(Device, const ShaderModuleCreateInfo*, const void*, ShaderModule*);
using PFN_DestroyShaderModule = void(SIAARA_VKAPI*)(Device, ShaderModule, const void*);
using PFN_CreateComputePipelines = Result(SIAARA_VKAPI*)(Device, PipelineCache, std::uint32_t, const ComputePipelineCreateInfo*,
                                                         const void*, Pipeline*);
using PFN_DestroyPipeline = void(SIAARA_VKAPI*)(Device, Pipeline, const void*);
using PFN_CreateDescriptorPool = Result(SIAARA_VKAPI*)(Device, const DescriptorPoolCreateInfo*, const void*, DescriptorPool*);
using PFN_DestroyDescriptorPool = void(SIAARA_VKAPI*)(Device, DescriptorPool, const void*);
using PFN_AllocateDescriptorSets = Result(SIAARA_VKAPI*)(Device, const DescriptorSetAllocateInfo*, DescriptorSet*);
using PFN_UpdateDescriptorSets = void(SIAARA_VKAPI*)(Device, std::uint32_t, const WriteDescriptorSet*, std::uint32_t,
                                                     const void*);
using PFN_CreateFence = Result(SIAARA_VKAPI*)(Device, const FenceCreateInfo*, const void*, Fence*);
using PFN_DestroyFence = void(SIAARA_VKAPI*)(Device, Fence, const void*);
using PFN_ResetFences = Result(SIAARA_VKAPI*)(Device, std::uint32_t, const Fence*);
using PFN_WaitForFences = Result(SIAARA_VKAPI*)(Device, std::uint32_t, const Fence*, Bool32, std::uint64_t);
using PFN_QueueSubmit = Result(SIAARA_VKAPI*)(Queue, std::uint32_t, const SubmitInfo*, Fence);
using PFN_CmdBindPipeline = void(SIAARA_VKAPI*)(CommandBuffer, std::int32_t, Pipeline);
using PFN_CmdBindDescriptorSets = void(SIAARA_VKAPI*)(CommandBuffer, std::int32_t, PipelineLayout, std::uint32_t,
                                                      std::uint32_t, const DescriptorSet*, std::uint32_t, const std::uint32_t*);
using PFN_CmdDispatch = void(SIAARA_VKAPI*)(CommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t);
using PFN_CmdPipelineBarrier = void(SIAARA_VKAPI*)(CommandBuffer, Flags, Flags, Flags, std::uint32_t, const void*,
                                                   std::uint32_t, const BufferMemoryBarrier*, std::uint32_t,
                                                   const ImageMemoryBarrier*);
using PFN_CmdCopyBuffer = void(SIAARA_VKAPI*)(CommandBuffer, Buffer, Buffer, std::uint32_t, const BufferCopy*);
using PFN_CmdCopyBufferToImage = void(SIAARA_VKAPI*)(CommandBuffer, Buffer, Image, std::int32_t, std::uint32_t,
                                                     const BufferImageCopy*);
using PFN_CmdCopyImageToBuffer = void(SIAARA_VKAPI*)(CommandBuffer, Image, std::int32_t, Buffer, std::uint32_t,
                                                     const BufferImageCopy*);

}  // namespace siaara::gpu::vk
