// SIAARA DICOM Toolkit - entry points of the individual GPU backends, used by the registry in
// gpu.cpp (internal).
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "gpu/gpu.hpp"

namespace siaara::gpu::vulkan {
// Opens the loader (by the given library name), creates an instance and lists the usable devices.
// The instance is kept for create() until release().
detail::Probe probe(const std::string& loader_name);
// Creates the backend for device `index` of the last probe.
Result<std::shared_ptr<Backend>> create(std::size_t index);
void release();
// The default loader name of this platform ("" when Vulkan is disabled here by default).
std::string default_loader_name();
}  // namespace siaara::gpu::vulkan

namespace siaara::gpu::metal {
detail::Probe probe();
Result<std::shared_ptr<Backend>> create(std::size_t index);
void release();
}  // namespace siaara::gpu::metal
