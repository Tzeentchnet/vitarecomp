#ifndef VITARECOMP_GPU_VULKAN_BACKEND_HPP
#define VITARECOMP_GPU_VULKAN_BACKEND_HPP

#include "gpu_internal.hpp"

#include <memory>

namespace vitarecomp::gpu {

std::shared_ptr<BackendFactory> make_vulkan_backend_factory();

} // namespace vitarecomp::gpu

#endif
