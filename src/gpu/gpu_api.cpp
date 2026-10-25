#include "gpu_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <new>
#include <span>
#include <string_view>

namespace {

using vitarecomp::gpu::Status;

template <typename Function>
vita_gpu_result invoke(vita_gpu_error *error, std::string_view function_name,
                       Function &&function) noexcept {
    Status status;
    try {
        status = function();
    } catch (const std::bad_alloc &) {
        status = Status::error(VITA_GPU_ERROR_OUT_OF_MEMORY,
                               "memory allocation failed");
    } catch (const std::exception &exception) {
        status = Status::error(VITA_GPU_ERROR_INTERNAL, exception.what());
    } catch (...) {
        status = Status::error(VITA_GPU_ERROR_INTERNAL,
                               "unknown C++ exception");
    }
    vitarecomp::gpu::write_error(error, status, function_name);
    return status.code;
}

Status null_argument(std::string_view name) {
    return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                         std::string(name) + " is null");
}

bool equals_ascii_case_insensitive(std::string_view left,
                                   std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char lhs, char rhs) {
                          return std::tolower(
                                     static_cast<unsigned char>(lhs)) ==
                                 std::tolower(
                                     static_cast<unsigned char>(rhs));
                      });
}

} // namespace

extern "C" {

uint32_t vita_gpu_get_abi_version(void) {
    return VITA_GPU_ABI_VERSION;
}

const char *vita_gpu_result_string(vita_gpu_result result) {
    switch (result) {
    case VITA_GPU_SUCCESS:
        return "success";
    case VITA_GPU_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case VITA_GPU_ERROR_INVALID_STRUCT:
        return "invalid structure";
    case VITA_GPU_ERROR_UNSUPPORTED_ABI_VERSION:
        return "unsupported ABI version";
    case VITA_GPU_ERROR_BACKEND_UNAVAILABLE:
        return "backend unavailable";
    case VITA_GPU_ERROR_ADAPTER_NOT_FOUND:
        return "adapter not found";
    case VITA_GPU_ERROR_UNSUPPORTED:
        return "unsupported";
    case VITA_GPU_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case VITA_GPU_ERROR_INVALID_HANDLE:
        return "invalid handle";
    case VITA_GPU_ERROR_STALE_HANDLE:
        return "stale handle";
    case VITA_GPU_ERROR_WRONG_OWNER:
        return "wrong owner";
    case VITA_GPU_ERROR_INVALID_STATE:
        return "invalid state";
    case VITA_GPU_ERROR_RESOURCE_IN_USE:
        return "resource in use";
    case VITA_GPU_ERROR_DEVICE_LOST:
        return "device lost";
    case VITA_GPU_ERROR_TIMEOUT:
        return "timeout";
    case VITA_GPU_ERROR_NATIVE:
        return "native API error";
    case VITA_GPU_ERROR_SHADER_COMPILER_UNAVAILABLE:
        return "shader compiler unavailable";
    case VITA_GPU_ERROR_SHADER_COMPILATION:
        return "shader compilation failed";
    case VITA_GPU_ERROR_IO:
        return "I/O error";
    case VITA_GPU_ERROR_INTERNAL:
        return "internal error";
    default:
        return "unknown result";
    }
}

void vita_gpu_error_reset(vita_gpu_error *error) {
    if (error == nullptr ||
        error->struct_version != VITA_GPU_STRUCT_VERSION_1 ||
        error->struct_size < sizeof(*error)) {
        return;
    }
    const uint32_t size = error->struct_size;
    const uint32_t version = error->struct_version;
    std::memset(error, 0, sizeof(*error));
    error->struct_size = size;
    error->struct_version = version;
    error->result = VITA_GPU_SUCCESS;
}

uint32_t vita_gpu_backend_count(void) {
    return 2;
}

vita_gpu_result vita_gpu_get_backend_info(uint32_t index,
                                          vita_gpu_backend_info *info,
                                          vita_gpu_error *error) {
    return invoke(error, "vita_gpu_get_backend_info", [&] {
        return info == nullptr
                   ? null_argument("info")
                   : vitarecomp::gpu::production_runtime().backend_info(index,
                                                                        *info);
    });
}

vita_gpu_result vita_gpu_backend_from_name(const char *name,
                                           vita_gpu_backend *backend,
                                           vita_gpu_error *error) {
    return invoke(error, "vita_gpu_backend_from_name", [&] {
        if (name == nullptr || backend == nullptr) {
            return null_argument(name == nullptr ? "name" : "backend");
        }
        const std::string_view value{name};
        if (equals_ascii_case_insensitive(value, "dx12")) {
            *backend = VITA_GPU_BACKEND_DX12;
            return Status::success();
        }
        if (equals_ascii_case_insensitive(value, "vulkan")) {
            *backend = VITA_GPU_BACKEND_VULKAN;
            return Status::success();
        }
        *backend = VITA_GPU_BACKEND_NONE;
        return Status::error(
            VITA_GPU_ERROR_INVALID_ARGUMENT,
            "renderer name must be exactly 'dx12' or 'vulkan'");
    });
}

const char *vita_gpu_backend_name(vita_gpu_backend backend) {
    switch (backend) {
    case VITA_GPU_BACKEND_DX12:
        return "dx12";
    case VITA_GPU_BACKEND_VULKAN:
        return "vulkan";
    default:
        return nullptr;
    }
}

vita_gpu_result vita_gpu_adapter_count(vita_gpu_backend backend,
                                       uint32_t *count,
                                       vita_gpu_error *error) {
    return invoke(error, "vita_gpu_adapter_count", [&] {
        return count == nullptr
                   ? null_argument("count")
                   : vitarecomp::gpu::production_runtime().adapter_count(
                         backend, *count);
    });
}

vita_gpu_result vita_gpu_get_adapter_info(vita_gpu_backend backend,
                                          uint32_t index,
                                          vita_gpu_adapter_info *info,
                                          vita_gpu_error *error) {
    return invoke(error, "vita_gpu_get_adapter_info", [&] {
        return info == nullptr
                   ? null_argument("info")
                   : vitarecomp::gpu::production_runtime().adapter_info(
                         backend, index, *info);
    });
}

vita_gpu_result vita_gpu_create_device(const vita_gpu_device_desc *desc,
                                       vita_gpu_device *device,
                                       vita_gpu_error *error) {
    return invoke(error, "vita_gpu_create_device", [&] {
        if (desc == nullptr || device == nullptr) {
            return null_argument(desc == nullptr ? "desc" : "device");
        }
        device->value = 0;
        return vitarecomp::gpu::production_runtime().create_device(*desc,
                                                                   *device);
    });
}

vita_gpu_result vita_gpu_destroy_device(vita_gpu_device device,
                                        vita_gpu_error *error) {
    return invoke(error, "vita_gpu_destroy_device", [&] {
        return vitarecomp::gpu::production_runtime().destroy_device(device);
    });
}

vita_gpu_result vita_gpu_device_get_capabilities(
    vita_gpu_device device, vita_gpu_device_capabilities *capabilities,
    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_device_get_capabilities", [&] {
        return capabilities == nullptr
                   ? null_argument("capabilities")
                   : vitarecomp::gpu::production_runtime().capabilities(
                         device, *capabilities);
    });
}

vita_gpu_result vita_gpu_device_wait_idle(vita_gpu_device device,
                                          vita_gpu_error *error) {
    return invoke(error, "vita_gpu_device_wait_idle", [&] {
        return vitarecomp::gpu::production_runtime().wait_idle(device);
    });
}

#define VITA_GPU_API_DESC_OUT(function_name, runtime_method, descriptor_type, \
                              output_type)                                   \
    vita_gpu_result function_name(vita_gpu_device device,                    \
                                  const descriptor_type *desc,                \
                                  output_type *output,                        \
                                  vita_gpu_error *error) {                    \
        return invoke(error, #function_name, [&] {                            \
            if (desc == nullptr || output == nullptr) {                       \
                return null_argument(desc == nullptr ? "desc" : "output");   \
            }                                                                 \
            output->value = 0;                                                 \
            return vitarecomp::gpu::production_runtime().runtime_method(      \
                device, *desc, *output);                                       \
        });                                                                    \
    }

#define VITA_GPU_API_DESTROY(function_name, runtime_method, handle_type) \
    vita_gpu_result function_name(vita_gpu_device device,                \
                                  handle_type handle,                     \
                                  vita_gpu_error *error) {                \
        return invoke(error, #function_name, [&] {                        \
            return vitarecomp::gpu::production_runtime().runtime_method(  \
                device, handle);                                          \
        });                                                                \
    }

VITA_GPU_API_DESC_OUT(vita_gpu_create_swapchain, create_swapchain,
                      vita_gpu_swapchain_desc, vita_gpu_swapchain)
VITA_GPU_API_DESTROY(vita_gpu_destroy_swapchain, destroy_swapchain,
                     vita_gpu_swapchain)
VITA_GPU_API_DESC_OUT(vita_gpu_create_buffer, create_buffer,
                      vita_gpu_buffer_desc, vita_gpu_buffer)
VITA_GPU_API_DESTROY(vita_gpu_destroy_buffer, destroy_buffer, vita_gpu_buffer)
VITA_GPU_API_DESC_OUT(vita_gpu_create_texture, create_texture,
                      vita_gpu_texture_desc, vita_gpu_texture)
VITA_GPU_API_DESTROY(vita_gpu_destroy_texture, destroy_texture,
                     vita_gpu_texture)
VITA_GPU_API_DESC_OUT(vita_gpu_create_sampler, create_sampler,
                      vita_gpu_sampler_desc, vita_gpu_sampler)
VITA_GPU_API_DESTROY(vita_gpu_destroy_sampler, destroy_sampler,
                     vita_gpu_sampler)
VITA_GPU_API_DESC_OUT(vita_gpu_create_render_target, create_render_target,
                      vita_gpu_render_target_desc, vita_gpu_render_target)
VITA_GPU_API_DESTROY(vita_gpu_destroy_render_target, destroy_render_target,
                     vita_gpu_render_target)
VITA_GPU_API_DESC_OUT(vita_gpu_create_depth_target, create_depth_target,
                      vita_gpu_depth_target_desc, vita_gpu_depth_target)
VITA_GPU_API_DESTROY(vita_gpu_destroy_depth_target, destroy_depth_target,
                     vita_gpu_depth_target)
VITA_GPU_API_DESC_OUT(vita_gpu_create_shader_module, create_shader,
                      vita_gpu_shader_module_desc, vita_gpu_shader_module)
VITA_GPU_API_DESTROY(vita_gpu_destroy_shader_module, destroy_shader,
                     vita_gpu_shader_module)
VITA_GPU_API_DESC_OUT(vita_gpu_create_binding_layout, create_binding_layout,
                      vita_gpu_binding_layout_desc, vita_gpu_binding_layout)
VITA_GPU_API_DESTROY(vita_gpu_destroy_binding_layout, destroy_binding_layout,
                     vita_gpu_binding_layout)
VITA_GPU_API_DESC_OUT(vita_gpu_create_binding_set, create_binding_set,
                      vita_gpu_binding_set_desc, vita_gpu_binding_set)
VITA_GPU_API_DESTROY(vita_gpu_destroy_binding_set, destroy_binding_set,
                     vita_gpu_binding_set)
VITA_GPU_API_DESC_OUT(vita_gpu_create_graphics_pipeline,
                      create_graphics_pipeline,
                      vita_gpu_graphics_pipeline_desc, vita_gpu_pipeline)
VITA_GPU_API_DESC_OUT(vita_gpu_create_compute_pipeline,
                      create_compute_pipeline,
                      vita_gpu_compute_pipeline_desc, vita_gpu_pipeline)
VITA_GPU_API_DESTROY(vita_gpu_destroy_pipeline, destroy_pipeline,
                     vita_gpu_pipeline)
VITA_GPU_API_DESC_OUT(vita_gpu_create_readback, create_readback,
                      vita_gpu_readback_desc, vita_gpu_readback)
VITA_GPU_API_DESTROY(vita_gpu_destroy_readback, destroy_readback,
                     vita_gpu_readback)
VITA_GPU_API_DESC_OUT(vita_gpu_create_command_list, create_command_list,
                      vita_gpu_command_list_desc, vita_gpu_command_list)
VITA_GPU_API_DESTROY(vita_gpu_destroy_command_list, destroy_command_list,
                     vita_gpu_command_list)
VITA_GPU_API_DESC_OUT(vita_gpu_create_fence, create_fence,
                      vita_gpu_fence_desc, vita_gpu_fence)
VITA_GPU_API_DESTROY(vita_gpu_destroy_fence, destroy_fence, vita_gpu_fence)

#undef VITA_GPU_API_DESTROY
#undef VITA_GPU_API_DESC_OUT

vita_gpu_result vita_gpu_swapchain_acquire(vita_gpu_device device,
                                           vita_gpu_swapchain swapchain,
                                           vita_gpu_swapchain_image *image,
                                           vita_gpu_error *error) {
    return invoke(error, "vita_gpu_swapchain_acquire", [&] {
        return image == nullptr
                   ? null_argument("image")
                   : vitarecomp::gpu::production_runtime().acquire_swapchain(
                         device, swapchain, *image);
    });
}

vita_gpu_result vita_gpu_swapchain_resize(vita_gpu_device device,
                                          vita_gpu_swapchain swapchain,
                                          uint32_t width, uint32_t height,
                                          vita_gpu_error *error) {
    return invoke(error, "vita_gpu_swapchain_resize", [&] {
        return vitarecomp::gpu::production_runtime().resize_swapchain(
            device, swapchain, width, height);
    });
}

vita_gpu_result vita_gpu_swapchain_present(
    vita_gpu_device device, vita_gpu_swapchain swapchain,
    const vita_gpu_present_desc *desc, vita_gpu_error *error) {
    return invoke(error, "vita_gpu_swapchain_present", [&] {
        return desc == nullptr
                   ? null_argument("desc")
                   : vitarecomp::gpu::production_runtime().present_swapchain(
                         device, swapchain, *desc);
    });
}

vita_gpu_result vita_gpu_shader_request_key(
    const vita_gpu_shader_request_desc *request, vita_gpu_shader_key *key,
    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_shader_request_key", [&] {
        if (request == nullptr || key == nullptr) {
            return null_argument(request == nullptr ? "request" : "key");
        }
        return vitarecomp::gpu::make_shader_key(*request, *key);
    });
}

vita_gpu_result vita_gpu_update_binding_set(
    vita_gpu_device device, vita_gpu_binding_set set,
    const vita_gpu_binding_write *writes, uint32_t write_count,
    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_update_binding_set", [&] {
        if (writes == nullptr && write_count != 0) {
            return null_argument("writes");
        }
        return vitarecomp::gpu::production_runtime().update_binding_set(
            device, set,
            std::span<const vita_gpu_binding_write>(writes, write_count));
    });
}

vita_gpu_result vita_gpu_map_readback(vita_gpu_device device,
                                      vita_gpu_readback readback,
                                      vita_gpu_readback_map *map,
                                      vita_gpu_error *error) {
    return invoke(error, "vita_gpu_map_readback", [&] {
        return map == nullptr
                   ? null_argument("map")
                   : vitarecomp::gpu::production_runtime().map_readback(
                         device, readback, *map);
    });
}

vita_gpu_result vita_gpu_unmap_readback(vita_gpu_device device,
                                        vita_gpu_readback readback,
                                        vita_gpu_error *error) {
    return invoke(error, "vita_gpu_unmap_readback", [&] {
        return vitarecomp::gpu::production_runtime().unmap_readback(device,
                                                                    readback);
    });
}

vita_gpu_result vita_gpu_command_begin(vita_gpu_device device,
                                       vita_gpu_command_list command,
                                       vita_gpu_error *error) {
    return invoke(error, "vita_gpu_command_begin", [&] {
        return vitarecomp::gpu::production_runtime().command_begin(device,
                                                                   command);
    });
}

vita_gpu_result vita_gpu_command_end(vita_gpu_device device,
                                     vita_gpu_command_list command,
                                     vita_gpu_error *error) {
    return invoke(error, "vita_gpu_command_end", [&] {
        return vitarecomp::gpu::production_runtime().command_end(device,
                                                                 command);
    });
}

vita_gpu_result vita_gpu_command_reset(vita_gpu_device device,
                                       vita_gpu_command_list command,
                                       vita_gpu_error *error) {
    return invoke(error, "vita_gpu_command_reset", [&] {
        return vitarecomp::gpu::production_runtime().command_reset(device,
                                                                   command);
    });
}

vita_gpu_result vita_gpu_cmd_barriers(
    vita_gpu_device device, vita_gpu_command_list command,
    const vita_gpu_resource_barrier *barriers, uint32_t barrier_count,
    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_cmd_barriers", [&] {
        if (barriers == nullptr && barrier_count != 0) {
            return null_argument("barriers");
        }
        return vitarecomp::gpu::production_runtime().command_barriers(
            device, command,
            std::span<const vita_gpu_resource_barrier>(barriers,
                                                       barrier_count));
    });
}

#define VITA_GPU_API_COMMAND_DESC(function_name, runtime_method, desc_type) \
    vita_gpu_result function_name(vita_gpu_device device,                 \
                                  vita_gpu_command_list command,           \
                                  const desc_type *desc,                   \
                                  vita_gpu_error *error) {                 \
        return invoke(error, #function_name, [&] {                         \
            return desc == nullptr                                        \
                       ? null_argument("desc")                             \
                       : vitarecomp::gpu::production_runtime()             \
                             .runtime_method(device, command, *desc);      \
        });                                                                \
    }

VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_begin_render_pass,
                          command_begin_render_pass,
                          vita_gpu_render_pass_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_set_viewport, command_set_viewport,
                          vita_gpu_viewport)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_set_scissor, command_set_scissor,
                          vita_gpu_scissor)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_set_index_buffer,
                          command_set_index_buffer,
                          vita_gpu_index_buffer_binding)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_push_constants,
                          command_push_constants,
                          vita_gpu_push_constants_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_draw, command_draw,
                          vita_gpu_draw_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_draw_indexed, command_draw_indexed,
                          vita_gpu_draw_indexed_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_dispatch, command_dispatch,
                          vita_gpu_dispatch_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_copy_buffer, command_copy_buffer,
                          vita_gpu_buffer_copy_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_copy_buffer_to_readback,
                          command_copy_buffer_readback,
                          vita_gpu_buffer_readback_copy_desc)
VITA_GPU_API_COMMAND_DESC(vita_gpu_cmd_copy_texture_to_readback,
                          command_copy_texture_readback,
                          vita_gpu_texture_readback_copy_desc)

#undef VITA_GPU_API_COMMAND_DESC

vita_gpu_result vita_gpu_cmd_end_render_pass(vita_gpu_device device,
                                             vita_gpu_command_list command,
                                             vita_gpu_error *error) {
    return invoke(error, "vita_gpu_cmd_end_render_pass", [&] {
        return vitarecomp::gpu::production_runtime().command_end_render_pass(
            device, command);
    });
}

vita_gpu_result vita_gpu_cmd_bind_pipeline(vita_gpu_device device,
                                           vita_gpu_command_list command,
                                           vita_gpu_pipeline pipeline,
                                           vita_gpu_error *error) {
    return invoke(error, "vita_gpu_cmd_bind_pipeline", [&] {
        return vitarecomp::gpu::production_runtime().command_bind_pipeline(
            device, command, pipeline);
    });
}

vita_gpu_result vita_gpu_cmd_bind_set(vita_gpu_device device,
                                      vita_gpu_command_list command,
                                      uint32_t set_index,
                                      vita_gpu_binding_set set,
                                      vita_gpu_error *error) {
    return invoke(error, "vita_gpu_cmd_bind_set", [&] {
        return vitarecomp::gpu::production_runtime().command_bind_set(
            device, command, set_index, set);
    });
}

vita_gpu_result vita_gpu_cmd_set_vertex_buffers(
    vita_gpu_device device, vita_gpu_command_list command,
    const vita_gpu_vertex_buffer_binding *bindings, uint32_t binding_count,
    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_cmd_set_vertex_buffers", [&] {
        if (bindings == nullptr && binding_count != 0) {
            return null_argument("bindings");
        }
        return vitarecomp::gpu::production_runtime().command_set_vertex_buffers(
            device, command,
            std::span<const vita_gpu_vertex_buffer_binding>(bindings,
                                                            binding_count));
    });
}

vita_gpu_result vita_gpu_cmd_debug_marker(vita_gpu_device device,
                                          vita_gpu_command_list command,
                                          const char *label,
                                          uint32_t rgba_color,
                                          vita_gpu_error *error) {
    return invoke(error, "vita_gpu_cmd_debug_marker", [&] {
        return label == nullptr
                   ? null_argument("label")
                   : vitarecomp::gpu::production_runtime().command_debug_marker(
                         device, command, label, rgba_color);
    });
}

vita_gpu_result vita_gpu_fence_completed_value(vita_gpu_device device,
                                               vita_gpu_fence fence,
                                               uint64_t *value,
                                               vita_gpu_error *error) {
    return invoke(error, "vita_gpu_fence_completed_value", [&] {
        return value == nullptr
                   ? null_argument("value")
                   : vitarecomp::gpu::production_runtime().fence_completed(
                         device, fence, *value);
    });
}

vita_gpu_result vita_gpu_fence_wait(vita_gpu_device device,
                                    vita_gpu_fence fence, uint64_t value,
                                    uint64_t timeout_nanoseconds,
                                    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_fence_wait", [&] {
        return vitarecomp::gpu::production_runtime().fence_wait(
            device, fence, value, timeout_nanoseconds);
    });
}

vita_gpu_result vita_gpu_submit(vita_gpu_device device,
                                vita_gpu_queue_type queue,
                                const vita_gpu_submit_desc *desc,
                                vita_gpu_error *error) {
    return invoke(error, "vita_gpu_submit", [&] {
        return desc == nullptr
                   ? null_argument("desc")
                   : vitarecomp::gpu::production_runtime().submit(device,
                                                                  queue,
                                                                  *desc);
    });
}

vita_gpu_result vita_gpu_debug_capture_begin(
    vita_gpu_device device, const vita_gpu_debug_capture_desc *desc,
    vita_gpu_error *error) {
    return invoke(error, "vita_gpu_debug_capture_begin", [&] {
        return desc == nullptr
                   ? null_argument("desc")
                   : vitarecomp::gpu::production_runtime().debug_capture_begin(
                         device, *desc);
    });
}

vita_gpu_result vita_gpu_debug_capture_end(vita_gpu_device device,
                                           vita_gpu_error *error) {
    return invoke(error, "vita_gpu_debug_capture_end", [&] {
        return vitarecomp::gpu::production_runtime().debug_capture_end(device);
    });
}

} // extern "C"
