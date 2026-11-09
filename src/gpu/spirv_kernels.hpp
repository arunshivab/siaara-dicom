// SIAARA DICOM Toolkit - the compute kernels of the Vulkan backend, emitted as SPIR-V at run time
// by spirv_builder (internal). No shader compiler is involved: each kernel is written below as
// calls to the builder, mirroring the CPU renderer (src/volume/render3d.cpp) step by step.
#pragma once

#include <cstdint>
#include <vector>

#include "gpu/gpu_params.hpp"

namespace siaara::gpu::spirv {

// SPIR-V 1.0 binary of a kernel (GLCompute, local size kGroupX x kGroupY). Bindings (set 0):
// 0 params, 1 data (storage buffers of uint), 2 base volume, 3 overlay volume (combined image +
// sampler, 3D float), 4 output (2D r32ui storage image).
std::vector<std::uint32_t> kernel(param::Kernel k);

}  // namespace siaara::gpu::spirv
