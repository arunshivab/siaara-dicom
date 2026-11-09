// SIAARA DICOM Toolkit - GPU kernel parameter layout, shared by the host packer (gpu_frame.cpp),
// the SPIR-V kernels (spirv_kernels.cpp) and the Metal kernels (metal_backend.cpp) (internal).
//
// Every kernel reads two buffers of 32-bit words:
//   params - the per-frame constants below (floats stored as their IEEE-754 bit patterns);
//   data   - tables addressed by word offsets stored in params: colour maps, int16 colour LUTs,
//            the transfer function, gradient-opacity points, slab offsets and the brick grid.
// Textures: the base volume (binding 2), the fusion overlay (binding 3; the base again when there
// is none), and the output (binding 4): one r32ui texel per pixel holding R | G << 8 | B << 16 |
// A << 24, i.e. the RGBA8 bytes in memory order on a little-endian host (the host unpacks
// explicitly, so byte order does not matter).
#pragma once

#include <cstdint>

namespace siaara::gpu::param {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;  // "no table"

enum Kernel : std::uint32_t { KernelMpr = 0, KernelComposite = 1, KernelMip = 2, KernelCount = 3 };

// Global flags (word Flags).
constexpr std::uint32_t kFlagLinear = 1u << 0;    // MPR: trilinear (else nearest)
constexpr std::uint32_t kFlagOverlay = 1u << 1;   // a fusion layer is present
constexpr std::uint32_t kFlagMinIp = 1u << 2;     // MIP kernel: minimum instead of maximum
constexpr std::uint32_t kFlagSkip = 1u << 3;      // VR kernels: empty-space skipping
constexpr std::uint32_t kFlagGradient = 1u << 4;  // composite: gradients needed
constexpr std::uint32_t kFlagShading = 1u << 5;   // composite: Phong shading
constexpr std::uint32_t kFlagGradOpacity = 1u << 6;  // composite: gradient-opacity factor

// Layer flags (layer word LFlags).
constexpr std::uint32_t kLayerManual = 1u << 0;   // trilinear from 8 texel fetches (no hardware filter)
constexpr std::uint32_t kLayerInteger = 1u << 1;  // texels decode to integers (round after scaling)

// Words of a layer block (base at L0, overlay at L1).
constexpr std::uint32_t LS0 = 0;        // 3 floats: continuous voxel index of output pixel (0, 0)
constexpr std::uint32_t LDU = 3;        // 3 floats: index step per output column
constexpr std::uint32_t LDV = 6;        // 3 floats: index step per output row
constexpr std::uint32_t LDN = 9;        // 3 floats: index step per mm along the viewing direction
constexpr std::uint32_t LDimsF = 12;    // 3 floats: dims
constexpr std::uint32_t LEdge = 15;     // 3 floats: dims - 0.5 (upper footprint edge)
constexpr std::uint32_t LDimsU = 18;    // 3 uints: dims
constexpr std::uint32_t LScale = 21;    // float: value = texel * scale + bias
constexpr std::uint32_t LBias = 22;
constexpr std::uint32_t LWinA = 23;     // window: t = value * a + b (0..1 after clamping)
constexpr std::uint32_t LWinB = 24;
constexpr std::uint32_t LLut = 25;      // uint: data offset of the 65536-entry int16 colour LUT, or kNone
constexpr std::uint32_t LCmap = 26;     // uint: data offset of the 256-entry colour map
constexpr std::uint32_t LOpacity = 27;  // float: overlay blending weight
constexpr std::uint32_t LTransparent = 28;  // uint: overlay values at or below the window bottom are transparent
constexpr std::uint32_t LFlags = 29;
constexpr std::uint32_t LayerWords = 32;

// Words of the params buffer.
constexpr std::uint32_t Width = 0;      // uint: pixels in this dispatch
constexpr std::uint32_t Height = 1;
constexpr std::uint32_t Flags = 2;
constexpr std::uint32_t BgR = 3;        // floats 0..255
constexpr std::uint32_t BgG = 4;
constexpr std::uint32_t BgB = 5;
constexpr std::uint32_t SlabCount = 6;  // uint: slab samples (0 = single plane)
constexpr std::uint32_t SlabMode = 7;   // uint: 1 MIP, 2 MinIP, 3 average
constexpr std::uint32_t SlabOffsets = 8;  // uint: data offset of SlabCount floats (mm along the normal)
constexpr std::uint32_t L0 = 16;
constexpr std::uint32_t L1 = L0 + LayerWords;
// volume rendering (base layer only)
constexpr std::uint32_t Step = L1 + LayerWords;  // float: sample distance (mm)
constexpr std::uint32_t Ert = Step + 1;          // float: early-termination opacity
constexpr std::uint32_t TfLo = Step + 2;         // float
constexpr std::uint32_t TfScale = Step + 3;      // float
constexpr std::uint32_t TfEntries = Step + 4;    // uint
constexpr std::uint32_t TfR = Step + 5;          // uint data offsets of TfEntries floats each
constexpr std::uint32_t TfG = Step + 6;
constexpr std::uint32_t TfB = Step + 7;
constexpr std::uint32_t TfA = Step + 8;
constexpr std::uint32_t Ambient = Step + 9;      // floats
constexpr std::uint32_t Diffuse = Step + 10;
constexpr std::uint32_t Specular = Step + 11;
constexpr std::uint32_t Shininess = Step + 12;
constexpr std::uint32_t Gi = Step + 13;          // 9 floats: patient gradient = Gi (row-major) * index gradient
constexpr std::uint32_t Normal = Gi + 9;         // 3 floats: viewing direction (patient)
constexpr std::uint32_t GopOffset = Normal + 3;  // uint: data offset of GopCount (value, factor) float pairs
constexpr std::uint32_t GopCount = GopOffset + 1;
constexpr std::uint32_t BrickSize = GopOffset + 2;  // float
constexpr std::uint32_t BrickInv = GopOffset + 3;   // float: 1 / BrickSize
constexpr std::uint32_t BrickNb = GopOffset + 4;    // 3 uints
constexpr std::uint32_t BrickEmpty = BrickNb + 3;   // uint: data offset of the empty bits (1 bit per brick)
constexpr std::uint32_t BrickMin = BrickNb + 4;     // uint: data offset of the per-brick minimum (floats)
constexpr std::uint32_t BrickMax = BrickNb + 5;     // uint: data offset of the per-brick maximum (floats)
constexpr std::uint32_t Count = BrickNb + 6;

// Local work-group size of every kernel.
constexpr std::uint32_t kGroupX = 8, kGroupY = 8;

}  // namespace siaara::gpu::param
