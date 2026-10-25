#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>

#include "vulkan_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitarecomp::gpu {
namespace {

constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

Status vulkan_error(VkResult result, std::string message) {
    vita_gpu_result code = VITA_GPU_ERROR_NATIVE;
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY ||
        result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
        result == VK_ERROR_OUT_OF_POOL_MEMORY ||
        result == VK_ERROR_FRAGMENTED_POOL) {
        code = VITA_GPU_ERROR_OUT_OF_MEMORY;
    } else if (result == VK_ERROR_DEVICE_LOST) {
        code = VITA_GPU_ERROR_DEVICE_LOST;
    } else if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        code = VITA_GPU_ERROR_TIMEOUT;
    }
    message += " (VkResult " + std::to_string(static_cast<int>(result)) + ")";
    return Status::error(code, std::move(message), VITA_GPU_BACKEND_VULKAN,
                         static_cast<std::int64_t>(result));
}

Status unsupported(std::string message) {
    return Status::error(VITA_GPU_ERROR_UNSUPPORTED, std::move(message),
                         VITA_GPU_BACKEND_VULKAN);
}

Status invalid_argument(std::string message) {
    return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                         std::move(message), VITA_GPU_BACKEND_VULKAN);
}

Status unavailable(std::string message) {
    return Status::error(VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                         std::move(message), VITA_GPU_BACKEND_VULKAN);
}

VkFormat to_vk_format(vita_gpu_format format) {
    switch (format) {
    case VITA_GPU_FORMAT_R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case VITA_GPU_FORMAT_RG8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case VITA_GPU_FORMAT_RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VITA_GPU_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VITA_GPU_FORMAT_BGRA8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VITA_GPU_FORMAT_BGRA8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case VITA_GPU_FORMAT_R16_FLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case VITA_GPU_FORMAT_RG16_FLOAT:
        return VK_FORMAT_R16G16_SFLOAT;
    case VITA_GPU_FORMAT_RGBA16_FLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VITA_GPU_FORMAT_R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case VITA_GPU_FORMAT_RG32_FLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case VITA_GPU_FORMAT_RGB32_FLOAT:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VITA_GPU_FORMAT_RGBA32_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VITA_GPU_FORMAT_R16_UINT:
        return VK_FORMAT_R16_UINT;
    case VITA_GPU_FORMAT_R32_UINT:
        return VK_FORMAT_R32_UINT;
    case VITA_GPU_FORMAT_D16_UNORM:
        return VK_FORMAT_D16_UNORM;
    case VITA_GPU_FORMAT_D24_UNORM_S8_UINT:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case VITA_GPU_FORMAT_D32_FLOAT:
        return VK_FORMAT_D32_SFLOAT;
    case VITA_GPU_FORMAT_D32_FLOAT_S8_UINT:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case VITA_GPU_FORMAT_BC1_RGBA_UNORM:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VITA_GPU_FORMAT_BC1_RGBA_SRGB:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VITA_GPU_FORMAT_BC2_UNORM:
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case VITA_GPU_FORMAT_BC2_SRGB:
        return VK_FORMAT_BC2_SRGB_BLOCK;
    case VITA_GPU_FORMAT_BC3_UNORM:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case VITA_GPU_FORMAT_BC3_SRGB:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case VITA_GPU_FORMAT_BC4_UNORM:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case VITA_GPU_FORMAT_BC5_UNORM:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case VITA_GPU_FORMAT_BC7_UNORM:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case VITA_GPU_FORMAT_BC7_SRGB:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool is_depth_format(vita_gpu_format format) {
    return format >= VITA_GPU_FORMAT_D16_UNORM &&
           format <= VITA_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

bool has_stencil(vita_gpu_format format) {
    return format == VITA_GPU_FORMAT_D24_UNORM_S8_UINT ||
           format == VITA_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

VkImageAspectFlags aspect_for(vita_gpu_format format) {
    if (!is_depth_format(format)) {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    VkImageAspectFlags result = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (has_stencil(format)) {
        result |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return result;
}

struct FormatLayout {
    std::uint32_t block_width = 1;
    std::uint32_t block_height = 1;
    std::uint32_t bytes_per_block = 0;
};

FormatLayout format_layout(vita_gpu_format format) {
    switch (format) {
    case VITA_GPU_FORMAT_R8_UNORM:
        return {1, 1, 1};
    case VITA_GPU_FORMAT_RG8_UNORM:
    case VITA_GPU_FORMAT_R16_FLOAT:
    case VITA_GPU_FORMAT_R16_UINT:
    case VITA_GPU_FORMAT_D16_UNORM:
        return {1, 1, 2};
    case VITA_GPU_FORMAT_RGBA8_UNORM:
    case VITA_GPU_FORMAT_RGBA8_SRGB:
    case VITA_GPU_FORMAT_BGRA8_UNORM:
    case VITA_GPU_FORMAT_BGRA8_SRGB:
    case VITA_GPU_FORMAT_RG16_FLOAT:
    case VITA_GPU_FORMAT_R32_FLOAT:
    case VITA_GPU_FORMAT_R32_UINT:
    case VITA_GPU_FORMAT_D24_UNORM_S8_UINT:
    case VITA_GPU_FORMAT_D32_FLOAT:
        return {1, 1, 4};
    case VITA_GPU_FORMAT_RGBA16_FLOAT:
    case VITA_GPU_FORMAT_RG32_FLOAT:
    case VITA_GPU_FORMAT_D32_FLOAT_S8_UINT:
        return {1, 1, 8};
    case VITA_GPU_FORMAT_RGB32_FLOAT:
        return {1, 1, 12};
    case VITA_GPU_FORMAT_RGBA32_FLOAT:
        return {1, 1, 16};
    case VITA_GPU_FORMAT_BC1_RGBA_UNORM:
    case VITA_GPU_FORMAT_BC1_RGBA_SRGB:
    case VITA_GPU_FORMAT_BC4_UNORM:
        return {4, 4, 8};
    case VITA_GPU_FORMAT_BC2_UNORM:
    case VITA_GPU_FORMAT_BC2_SRGB:
    case VITA_GPU_FORMAT_BC3_UNORM:
    case VITA_GPU_FORMAT_BC3_SRGB:
    case VITA_GPU_FORMAT_BC5_UNORM:
    case VITA_GPU_FORMAT_BC7_UNORM:
    case VITA_GPU_FORMAT_BC7_SRGB:
        return {4, 4, 16};
    default:
        return {};
    }
}

VkSampleCountFlagBits to_vk_samples(std::uint32_t count) {
    switch (count) {
    case 1:
        return VK_SAMPLE_COUNT_1_BIT;
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    default:
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

VkShaderStageFlags to_vk_stages(vita_gpu_shader_stage_flags stages) {
    VkShaderStageFlags result = 0;
    if ((stages & VITA_GPU_SHADER_STAGE_VERTEX_BIT) != 0) {
        result |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((stages & VITA_GPU_SHADER_STAGE_PIXEL_BIT) != 0) {
        result |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if ((stages & VITA_GPU_SHADER_STAGE_COMPUTE_BIT) != 0) {
        result |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return result;
}

VkCompareOp to_vk_compare(vita_gpu_compare_op value) {
    return static_cast<VkCompareOp>(VK_COMPARE_OP_NEVER +
                                    static_cast<int>(value));
}

VkImageLayout layout_for(vita_gpu_resource_state state,
                         bool depth_resource) {
    if (state == VITA_GPU_RESOURCE_STATE_UNDEFINED) {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    const vita_gpu_resource_state effective =
        state & ~VITA_GPU_RESOURCE_STATE_COMMON;
    if (effective == VITA_GPU_RESOURCE_STATE_PRESENT) {
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_COPY_SOURCE) {
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_COPY_DEST) {
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_RENDER_TARGET) {
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_DEPTH_WRITE) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_DEPTH_READ ||
        (depth_resource &&
         effective == (VITA_GPU_RESOURCE_STATE_DEPTH_READ |
                       VITA_GPU_RESOURCE_STATE_SHADER_READ))) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_SHADER_WRITE) {
        return VK_IMAGE_LAYOUT_GENERAL;
    }
    if (effective == VITA_GPU_RESOURCE_STATE_SHADER_READ) {
        return depth_resource
                   ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

struct AccessState {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 access = VK_ACCESS_2_MEMORY_READ_BIT |
                            VK_ACCESS_2_MEMORY_WRITE_BIT;
};

AccessState access_for(vita_gpu_resource_state state) {
    if (state == VITA_GPU_RESOURCE_STATE_UNDEFINED) {
        return {VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
    }
    AccessState result{0, 0};
    if ((state & VITA_GPU_RESOURCE_STATE_COMMON) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        result.access |= VK_ACCESS_2_MEMORY_READ_BIT |
                         VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_COPY_SOURCE) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        result.access |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_COPY_DEST) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        result.access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        result.access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_INDEX_BUFFER) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        result.access |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        result.access |= VK_ACCESS_2_UNIFORM_READ_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_SHADER_READ) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        result.access |= VK_ACCESS_2_SHADER_READ_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_SHADER_WRITE) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        result.access |= VK_ACCESS_2_SHADER_READ_BIT |
                         VK_ACCESS_2_SHADER_WRITE_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_RENDER_TARGET) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        result.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_DEPTH_READ) != 0) {
        result.stages |=
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        result.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_DEPTH_WRITE) != 0) {
        result.stages |=
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        result.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_PRESENT) != 0) {
        result.stages |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    }
    if (result.stages == 0) {
        result.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return result;
}

struct ValidationState {
    std::atomic<std::uint32_t> errors{0};
    std::mutex mutex;
    std::string last_error;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user_data) {
    auto *state = static_cast<ValidationState *>(user_data);
    if (state != nullptr &&
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        state->errors.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(state->mutex);
        state->last_error =
            data != nullptr && data->pMessage != nullptr
                ? data->pMessage
                : "Vulkan validation reported an unspecified error";
    }
    if (data != nullptr && data->pMessage != nullptr) {
        OutputDebugStringA(data->pMessage);
        OutputDebugStringA("\n");
    }
    return VK_FALSE;
}

class InstanceContext {
public:
    ~InstanceContext() {
        if (instance_ != VK_NULL_HANDLE) {
            if (debug_messenger_ != VK_NULL_HANDLE &&
                dispatch_.vkDestroyDebugUtilsMessengerEXT != nullptr) {
                dispatch_.vkDestroyDebugUtilsMessengerEXT(
                    instance_, debug_messenger_, nullptr);
            }
            dispatch_.vkDestroyInstance(instance_, nullptr);
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    InstanceContext(const InstanceContext &) = delete;
    InstanceContext &operator=(const InstanceContext &) = delete;

    static Status create(bool validation, std::string_view application_name,
                         std::unique_ptr<InstanceContext> &output) {
        auto context = std::unique_ptr<InstanceContext>(new InstanceContext());
        context->validation_state_ = std::make_unique<ValidationState>();
        context->module_ = LoadLibraryExW(
            L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (context->module_ == nullptr) {
            return unavailable(
                "Vulkan loader vulkan-1.dll is not installed");
        }
        const auto get_instance_proc_addr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(context->module_, "vkGetInstanceProcAddr"));
        if (get_instance_proc_addr == nullptr) {
            return unavailable(
                "Vulkan loader does not export vkGetInstanceProcAddr");
        }
        context->get_instance_proc_addr_ = get_instance_proc_addr;
        context->dispatch_.init(get_instance_proc_addr);
        if (context->dispatch_.vkCreateInstance == nullptr) {
            return unavailable(
                "Vulkan loader does not expose vkCreateInstance");
        }

        std::uint32_t loader_version = VK_API_VERSION_1_0;
        if (context->dispatch_.vkEnumerateInstanceVersion != nullptr) {
            const VkResult version_result =
                context->dispatch_.vkEnumerateInstanceVersion(&loader_version);
            if (version_result != VK_SUCCESS) {
                return vulkan_error(version_result,
                                    "vkEnumerateInstanceVersion failed");
            }
        }
        if (loader_version < kRequiredApiVersion) {
            return unavailable(
                "Vulkan 1.3 or newer is required by the production backend");
        }

        std::uint32_t extension_count = 0;
        VkResult result =
            context->dispatch_.vkEnumerateInstanceExtensionProperties(
                nullptr, &extension_count, nullptr);
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result, "vkEnumerateInstanceExtensionProperties failed");
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        result = context->dispatch_.vkEnumerateInstanceExtensionProperties(
            nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result, "vkEnumerateInstanceExtensionProperties failed");
        }
        const auto has_extension = [&extensions](std::string_view name) {
            return std::any_of(
                extensions.begin(), extensions.end(),
                [name](const VkExtensionProperties &extension) {
                    return name == extension.extensionName;
                });
        };
        if (!has_extension(VK_KHR_SURFACE_EXTENSION_NAME) ||
            !has_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
            return unavailable(
                "Vulkan loader is missing Win32 surface extensions");
        }

        std::uint32_t layer_count = 0;
        result = context->dispatch_.vkEnumerateInstanceLayerProperties(
            &layer_count, nullptr);
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result, "vkEnumerateInstanceLayerProperties failed");
        }
        std::vector<VkLayerProperties> layers(layer_count);
        result = context->dispatch_.vkEnumerateInstanceLayerProperties(
            &layer_count, layers.data());
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result, "vkEnumerateInstanceLayerProperties failed");
        }
        context->validation_available_ = std::any_of(
            layers.begin(), layers.end(), [](const VkLayerProperties &layer) {
                return std::string_view(layer.layerName) ==
                       "VK_LAYER_KHRONOS_validation";
            });
        if (validation && !context->validation_available_) {
            return unsupported(
                "VK_LAYER_KHRONOS_validation is not installed");
        }
        context->debug_utils_available_ =
            has_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        std::vector<const char *> enabled_extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        if (context->debug_utils_available_) {
            enabled_extensions.push_back(
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        std::vector<const char *> enabled_layers;
        if (validation) {
            enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        }

        const std::string app_name =
            application_name.empty() ? "vitarecomp" :
                                       std::string(application_name);
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = app_name.c_str();
        application.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        application.pEngineName = "vitarecomp";
        application.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        application.apiVersion = kRequiredApiVersion;

        VkDebugUtilsMessengerCreateInfoEXT debug_info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_info.pfnUserCallback = debug_callback;
        debug_info.pUserData = context->validation_state_.get();

        VkInstanceCreateInfo create_info{
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create_info.pApplicationInfo = &application;
        create_info.enabledExtensionCount =
            static_cast<std::uint32_t>(enabled_extensions.size());
        create_info.ppEnabledExtensionNames = enabled_extensions.data();
        create_info.enabledLayerCount =
            static_cast<std::uint32_t>(enabled_layers.size());
        create_info.ppEnabledLayerNames = enabled_layers.data();
        if (validation && context->debug_utils_available_) {
            create_info.pNext = &debug_info;
        }
        result = context->dispatch_.vkCreateInstance(
            &create_info, nullptr, &context->instance_);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
                return unavailable(
                    "Vulkan loader found no compatible installed ICD");
            }
            return vulkan_error(result, "vkCreateInstance failed");
        }
        context->dispatch_.init(context->instance_,
                                context->get_instance_proc_addr_);

        if (validation && context->debug_utils_available_ &&
            context->dispatch_.vkCreateDebugUtilsMessengerEXT != nullptr) {
            result = context->dispatch_.vkCreateDebugUtilsMessengerEXT(
                context->instance_, &debug_info, nullptr,
                &context->debug_messenger_);
            if (result != VK_SUCCESS) {
                return vulkan_error(
                    result, "vkCreateDebugUtilsMessengerEXT failed");
            }
        }
        context->validation_enabled_ = validation;
        output = std::move(context);
        return Status::success();
    }

    Status enumerate_devices(std::vector<VkPhysicalDevice> &devices) {
        std::uint32_t count = 0;
        VkResult result = dispatch_.vkEnumeratePhysicalDevices(
            instance_, &count, nullptr);
        if (result != VK_SUCCESS) {
            return vulkan_error(result,
                                "vkEnumeratePhysicalDevices failed");
        }
        devices.resize(count);
        if (count != 0) {
            result = dispatch_.vkEnumeratePhysicalDevices(
                instance_, &count, devices.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return vulkan_error(result,
                                    "vkEnumeratePhysicalDevices failed");
            }
            devices.resize(count);
        }
        return Status::success();
    }

    Status validation_status() const {
        if (validation_state_->errors.load(std::memory_order_relaxed) == 0) {
            return Status::success();
        }
        std::scoped_lock lock(validation_state_->mutex);
        return Status::error(
            VITA_GPU_ERROR_NATIVE,
            "Vulkan validation error: " + validation_state_->last_error,
            VITA_GPU_BACKEND_VULKAN);
    }

    vk::detail::DispatchLoaderDynamic &dispatch() { return dispatch_; }
    const vk::detail::DispatchLoaderDynamic &dispatch() const {
        return dispatch_;
    }
    VkInstance instance() const { return instance_; }
    PFN_vkGetInstanceProcAddr get_instance_proc_addr() const {
        return get_instance_proc_addr_;
    }
    bool validation_available() const { return validation_available_; }
    bool validation_enabled() const { return validation_enabled_; }
    bool debug_utils_available() const { return debug_utils_available_; }

private:
    InstanceContext() = default;

    HMODULE module_ = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
    vk::detail::DispatchLoaderDynamic dispatch_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    std::unique_ptr<ValidationState> validation_state_;
    bool validation_available_ = false;
    bool validation_enabled_ = false;
    bool debug_utils_available_ = false;
};

vita_gpu_adapter_type adapter_type(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return VITA_GPU_ADAPTER_INTEGRATED;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return VITA_GPU_ADAPTER_DISCRETE;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return VITA_GPU_ADAPTER_SOFTWARE;
    default:
        return VITA_GPU_ADAPTER_OTHER;
    }
}

std::optional<std::uint32_t> graphics_queue_family(
    const vk::detail::DispatchLoaderDynamic &dispatch,
    VkPhysicalDevice physical) {
    std::uint32_t count = 0;
    dispatch.vkGetPhysicalDeviceQueueFamilyProperties(
        physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    dispatch.vkGetPhysicalDeviceQueueFamilyProperties(
        physical, &count, properties.data());
    for (std::uint32_t index = 0; index < count; ++index) {
        const VkQueueFlags required =
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
            VK_QUEUE_TRANSFER_BIT;
        if ((properties[index].queueFlags & required) == required &&
            properties[index].queueCount != 0 &&
            dispatch.vkGetPhysicalDeviceWin32PresentationSupportKHR(
                physical, index) == VK_TRUE) {
            return index;
        }
    }
    return std::nullopt;
}

vita_gpu_capability_flags adapter_capabilities(
    const vk::detail::DispatchLoaderDynamic &dispatch,
    VkPhysicalDevice physical) {
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features.pNext = &features12;
    dispatch.vkGetPhysicalDeviceFeatures2(physical, &features);
    vita_gpu_capability_flags flags = VITA_GPU_CAPABILITY_GRAPHICS;
    if (graphics_queue_family(dispatch, physical).has_value()) {
        flags |= VITA_GPU_CAPABILITY_COMPUTE;
    }
    if (features12.timelineSemaphore == VK_TRUE) {
        flags |= VITA_GPU_CAPABILITY_TIMELINE_FENCES;
    }
    if (features.features.textureCompressionBC == VK_TRUE) {
        flags |= VITA_GPU_CAPABILITY_BC_TEXTURES;
    }
    if (features.features.samplerAnisotropy == VK_TRUE) {
        flags |= VITA_GPU_CAPABILITY_ANISOTROPY;
    }
    if (features12.shaderFloat16 == VK_TRUE) {
        flags |= VITA_GPU_CAPABILITY_SHADER_FLOAT16;
    }
    if (features.features.shaderInt64 == VK_TRUE) {
        flags |= VITA_GPU_CAPABILITY_SHADER_INT64;
    }
    return flags;
}

Status fill_adapter_info(InstanceContext &context, VkPhysicalDevice physical,
                         std::uint32_t index,
                         vita_gpu_adapter_info &output) {
    VkPhysicalDeviceIDProperties id{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &id;
    context.dispatch().vkGetPhysicalDeviceProperties2(physical, &properties);
    VkPhysicalDeviceMemoryProperties memory{};
    context.dispatch().vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    output.adapter_index = index;
    output.type = adapter_type(properties.properties.deviceType);
    output.vendor_id = properties.properties.vendorID;
    output.device_id = properties.properties.deviceID;
    output.dedicated_video_memory = 0;
    output.dedicated_system_memory = 0;
    output.shared_system_memory = 0;
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if ((memory.memoryHeaps[heap].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            output.dedicated_video_memory += memory.memoryHeaps[heap].size;
        } else {
            output.shared_system_memory += memory.memoryHeaps[heap].size;
        }
    }
    output.platform_id = 0;
    if (id.deviceLUIDValid == VK_TRUE) {
        static_assert(VK_LUID_SIZE == sizeof(std::uint64_t));
        std::memcpy(&output.platform_id, id.deviceLUID, sizeof(std::uint64_t));
    }
    std::memcpy(output.device_uuid, id.deviceUUID, VK_UUID_SIZE);
    output.capabilities =
        adapter_capabilities(context.dispatch(), physical);
    std::memset(output.name, 0, sizeof(output.name));
    const std::size_t name_size =
        std::min(sizeof(output.name) - 1U,
                 std::strlen(properties.properties.deviceName));
    std::memcpy(output.name, properties.properties.deviceName, name_size);
    return Status::success();
}

struct BufferObject {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkMemoryPropertyFlags memory_properties = 0;
};

struct TextureObject {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView default_view = VK_NULL_HANDLE;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t layers = 1;
    std::uint32_t mip_levels = 1;
    std::uint32_t sample_count = 1;
    vita_gpu_texture_dimension dimension = VITA_GPU_TEXTURE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool owned = true;
    bool layout_initialized = true;
    bool copy_source_supported = false;
};

struct SamplerObject {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct ViewObject {
    VkImageView view = VK_NULL_HANDLE;
    std::uint64_t texture = 0;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sample_count = 1;
    std::uint32_t layers = 1;
    bool read_only = false;
};

struct ShaderObject {
    VkShaderModule module = VK_NULL_HANDLE;
    vita_gpu_shader_stage stage = VITA_GPU_SHADER_VERTEX;
};

struct BindingLayoutObject {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::uint32_t set_index = 0;
    std::array<std::uint64_t, 5> set_counts{};
    std::array<std::uint64_t, 5> vertex_counts{};
    std::array<std::uint64_t, 5> pixel_counts{};
    std::array<std::uint64_t, 5> compute_counts{};
};

bool descriptor_pipeline_supported(
    const VkPhysicalDeviceLimits &limits,
    std::span<const BindingLayoutObject *const> layouts,
    VkShaderStageFlags active_stages) {
    std::array<std::uint64_t, 5> set_counts{};
    std::array<std::uint64_t, 5> vertex_counts{};
    std::array<std::uint64_t, 5> pixel_counts{};
    std::array<std::uint64_t, 5> compute_counts{};
    const auto accumulate =
        [](std::array<std::uint64_t, 5> &destination,
           const std::array<std::uint64_t, 5> &source) {
            for (std::size_t index = 0;
                 index < destination.size(); ++index) {
                destination[index] += source[index];
            }
        };
    for (const BindingLayoutObject *layout : layouts) {
        accumulate(set_counts, layout->set_counts);
        accumulate(vertex_counts, layout->vertex_counts);
        accumulate(pixel_counts, layout->pixel_counts);
        accumulate(compute_counts, layout->compute_counts);
    }
    const auto stage_supported =
        [&limits](const std::array<std::uint64_t, 5> &counts) {
            const std::uint64_t resources =
                counts[0] + counts[1] + counts[2] +
                counts[3] + counts[4];
            return counts[0] <=
                       limits.maxPerStageDescriptorUniformBuffers &&
                   counts[1] <=
                       limits.maxPerStageDescriptorStorageBuffers &&
                   counts[2] <=
                       limits.maxPerStageDescriptorSampledImages &&
                   counts[3] <=
                       limits.maxPerStageDescriptorStorageImages &&
                   counts[4] <=
                       limits.maxPerStageDescriptorSamplers &&
                   resources <= limits.maxPerStageResources;
        };
    const bool sets_supported =
        set_counts[0] <= limits.maxDescriptorSetUniformBuffers &&
        set_counts[1] <= limits.maxDescriptorSetStorageBuffers &&
        set_counts[2] <= limits.maxDescriptorSetSampledImages &&
        set_counts[3] <= limits.maxDescriptorSetStorageImages &&
        set_counts[4] <= limits.maxDescriptorSetSamplers;
    return sets_supported &&
           ((active_stages & VK_SHADER_STAGE_VERTEX_BIT) == 0 ||
            stage_supported(vertex_counts)) &&
           ((active_stages & VK_SHADER_STAGE_FRAGMENT_BIT) == 0 ||
            stage_supported(pixel_counts)) &&
           ((active_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
            stage_supported(compute_counts));
}

struct BindingSetObject {
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint64_t layout = 0;
};

struct PipelineObject {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::vector<vita_gpu_format> color_formats;
    vita_gpu_format depth_format = VITA_GPU_FORMAT_UNKNOWN;
    std::uint32_t sample_count = 1;
    bool depth_write = false;
    std::set<std::uint32_t> required_vertex_bindings;
};

struct FenceObject {
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct ReadbackObject {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkMemoryPropertyFlags memory_properties = 0;
    void *mapped = nullptr;
    std::uint32_t row_pitch = 0;
    std::uint32_t slice_pitch = 0;
};

struct SwapchainFrame {
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkCommandBuffer present_command = VK_NULL_HANDLE;
    bool submitted = false;
};

struct SwapchainObject {
    HWND window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    vita_gpu_format contract_format = VITA_GPU_FORMAT_UNKNOWN;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t requested_image_count = 2;
    std::vector<std::uint64_t> texture_ids;
    std::vector<std::uint64_t> target_ids;
    std::vector<SwapchainFrame> frames;
    std::uint32_t frame_cursor = 0;
    std::uint32_t acquired_frame = 0;
    std::uint32_t acquired_image = 0;
    bool acquired = false;
    bool submitted = false;
    bool suboptimal = false;
};

struct PendingSubmission {
    VkSemaphore completion = VK_NULL_HANDLE;
    std::uint64_t value = 0;
    std::vector<VkCommandBuffer> command_buffers;
};

struct ReadbackLayoutUpdate {
    NativeId readback;
    std::uint32_t row_pitch = 0;
    std::uint32_t slice_pitch = 0;
};

class VulkanDevice final : public BackendDevice {
public:
    VulkanDevice(std::unique_ptr<InstanceContext> context,
                 VkPhysicalDevice physical, VkDevice device,
                 std::uint32_t queue_family, VkQueue queue,
                 VkPhysicalDeviceProperties properties,
                 VkPhysicalDeviceFeatures features)
        : context_(std::move(context)),
          physical_(physical),
          device_(device),
          queue_family_(queue_family),
          queue_(queue),
          properties_(properties),
          features_(features) {}

    ~VulkanDevice() override;

    Status initialize();

    [[nodiscard]] vita_gpu_backend backend() const noexcept override {
        return VITA_GPU_BACKEND_VULKAN;
    }
    Status capabilities(vita_gpu_device_capabilities &output) override;
    Status wait_idle() override;
    Status create_swapchain(const vita_gpu_swapchain_desc &desc,
                            NativeId &output) override;
    Status resize_swapchain(NativeId swapchain, std::uint32_t width,
                            std::uint32_t height) override;
    Status acquire_swapchain(NativeId swapchain,
                             BackendSwapchainImage &output) override;
    Status present_swapchain(NativeId swapchain,
                             const vita_gpu_present_desc &desc) override;
    Status create_buffer(const vita_gpu_buffer_desc &desc,
                         NativeId &output) override;
    Status create_texture(const vita_gpu_texture_desc &desc,
                          NativeId &output) override;
    Status create_sampler(const vita_gpu_sampler_desc &desc,
                          NativeId &output) override;
    Status create_render_target(const vita_gpu_render_target_desc &desc,
                                NativeId texture,
                                NativeId &output) override;
    Status create_depth_target(const vita_gpu_depth_target_desc &desc,
                               NativeId texture,
                               NativeId &output) override;
    Status create_shader_module(const vita_gpu_shader_module_desc &desc,
                                NativeId &output) override;
    Status create_binding_layout(const vita_gpu_binding_layout_desc &desc,
                                 NativeId &output) override;
    Status create_binding_set(const vita_gpu_binding_set_desc &desc,
                              NativeId layout,
                              NativeId &output) override;
    Status update_binding_set(
        NativeId set, std::span<const BackendBindingWrite> writes) override;
    Status create_graphics_pipeline(
        const BackendGraphicsPipelineDesc &desc,
        NativeId &output) override;
    Status create_compute_pipeline(
        const BackendComputePipelineDesc &desc,
        NativeId &output) override;
    Status create_readback(const vita_gpu_readback_desc &desc,
                           NativeId &output) override;
    Status create_fence(const vita_gpu_fence_desc &desc,
                        NativeId &output) override;
    Status destroy_object(ObjectKind kind, NativeId object) override;
    Status submit(
        vita_gpu_queue_type queue,
        std::span<const RecordedCommandList *const> command_lists,
        std::span<const BackendFencePoint> waits,
        BackendFencePoint signal) override;
    Status fence_completed_value(NativeId fence,
                                 std::uint64_t &value) override;
    Status fence_wait(NativeId fence, std::uint64_t value,
                      std::uint64_t timeout_nanoseconds) override;
    Status map_readback(NativeId readback,
                        BackendReadbackMap &output) override;
    Status unmap_readback(NativeId readback) override;
    Status debug_capture_begin(
        const vita_gpu_debug_capture_desc &) override {
        return unsupported(
            "Vulkan debug capture requires an external capture tool");
    }
    Status debug_capture_end() override {
        return unsupported(
            "Vulkan debug capture requires an external capture tool");
    }

private:
    template <typename T>
    NativeId store(std::unordered_map<std::uint64_t, T> &objects, T value) {
        const std::uint64_t id = next_id_++;
        objects.emplace(id, std::move(value));
        return NativeId{id};
    }

    template <typename T>
    T *find(std::unordered_map<std::uint64_t, T> &objects, NativeId id) {
        const auto found = objects.find(id.value);
        return found == objects.end() ? nullptr : &found->second;
    }

    Status checked_validation() const {
        return context_->validation_status();
    }
    Status find_memory_type(std::uint32_t bits,
                            VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred,
                            std::uint32_t &index,
                            VkMemoryPropertyFlags &actual) const;
    Status create_buffer_allocation(VkDeviceSize size,
                                    VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags required,
                                    VkMemoryPropertyFlags preferred,
                                    BufferObject &output);
    Status create_image_view(TextureObject &texture,
                             VkImageViewType view_type,
                             VkFormat format,
                             std::uint32_t mip,
                             std::uint32_t mip_count,
                             std::uint32_t first_layer,
                             std::uint32_t layer_count,
                             VkImageView &output,
                             VkImageAspectFlags aspect = 0);
    Status begin_immediate(VkCommandBuffer &command);
    Status end_immediate(VkCommandBuffer command);
    Status transition_image_immediate(TextureObject &texture,
                                      vita_gpu_resource_state before,
                                      vita_gpu_resource_state after);
    Status upload_buffer(BufferObject &destination,
                         const std::uint8_t *data, VkDeviceSize size,
                         vita_gpu_resource_state final_state);
    Status upload_texture(TextureObject &destination,
                          const vita_gpu_texture_desc &desc);
    Status recreate_swapchain(SwapchainObject &swapchain,
                              std::uint32_t width, std::uint32_t height,
                              bool preserve_ids);
    void destroy_swapchain_native(SwapchainObject &swapchain,
                                  bool destroy_surface);
    Status encode_command_list(const RecordedCommandList &recorded,
                               VkCommandBuffer command,
                               std::set<std::uint64_t>
                                   &initialized_swapchain_textures,
                               std::vector<ReadbackLayoutUpdate>
                                   &readback_updates);
    Status cleanup_submissions();
    Status set_name(VkObjectType type, std::uint64_t object,
                    const char *name);
    VkDescriptorType descriptor_type(vita_gpu_binding_type type) const;

    std::unique_ptr<InstanceContext> context_;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceFeatures features_{};
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, SwapchainObject> swapchains_;
    std::unordered_map<std::uint64_t, BufferObject> buffers_;
    std::unordered_map<std::uint64_t, TextureObject> textures_;
    std::unordered_map<std::uint64_t, SamplerObject> samplers_;
    std::unordered_map<std::uint64_t, ViewObject> render_targets_;
    std::unordered_map<std::uint64_t, ViewObject> depth_targets_;
    std::unordered_map<std::uint64_t, ShaderObject> shaders_;
    std::unordered_map<std::uint64_t, BindingLayoutObject> binding_layouts_;
    std::unordered_map<std::uint64_t, BindingSetObject> binding_sets_;
    std::unordered_map<std::uint64_t, PipelineObject> pipelines_;
    std::unordered_map<std::uint64_t, FenceObject> fences_;
    std::unordered_map<std::uint64_t, ReadbackObject> readbacks_;
    std::vector<PendingSubmission> pending_submissions_;
};

VulkanDevice::~VulkanDevice() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    auto &vk = context_->dispatch();
    (void)vk.vkDeviceWaitIdle(device_);
    for (auto &[id, swapchain] : swapchains_) {
        (void)id;
        destroy_swapchain_native(swapchain, true);
    }
    swapchains_.clear();
    for (auto &[id, pipeline] : pipelines_) {
        (void)id;
        vk.vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
        vk.vkDestroyPipelineLayout(device_, pipeline.layout, nullptr);
    }
    for (auto &[id, shader] : shaders_) {
        (void)id;
        vk.vkDestroyShaderModule(device_, shader.module, nullptr);
    }
    for (auto &[id, view] : render_targets_) {
        (void)id;
        vk.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto &[id, view] : depth_targets_) {
        (void)id;
        vk.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto &[id, sampler] : samplers_) {
        (void)id;
        vk.vkDestroySampler(device_, sampler.sampler, nullptr);
    }
    for (auto &[id, texture] : textures_) {
        (void)id;
        if (texture.owned) {
            if (texture.default_view != VK_NULL_HANDLE) {
                vk.vkDestroyImageView(device_, texture.default_view, nullptr);
            }
            vk.vkDestroyImage(device_, texture.image, nullptr);
            vk.vkFreeMemory(device_, texture.memory, nullptr);
        }
    }
    for (auto &[id, buffer] : buffers_) {
        (void)id;
        vk.vkDestroyBuffer(device_, buffer.buffer, nullptr);
        vk.vkFreeMemory(device_, buffer.memory, nullptr);
    }
    for (auto &[id, readback] : readbacks_) {
        (void)id;
        if (readback.mapped != nullptr) {
            vk.vkUnmapMemory(device_, readback.memory);
        }
        vk.vkDestroyBuffer(device_, readback.buffer, nullptr);
        vk.vkFreeMemory(device_, readback.memory, nullptr);
    }
    for (auto &[id, fence] : fences_) {
        (void)id;
        vk.vkDestroySemaphore(device_, fence.semaphore, nullptr);
    }
    for (auto &[id, layout] : binding_layouts_) {
        (void)id;
        vk.vkDestroyDescriptorSetLayout(device_, layout.layout, nullptr);
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vk.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vk.vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
    vk.vkDestroyDevice(device_, nullptr);
}

Status VulkanDevice::initialize() {
    auto &vk = context_->dispatch();
    vk.init(vk::Device(device_));

    VkCommandPoolCreateInfo command_pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = queue_family_;
    VkResult result = vk.vkCreateCommandPool(
        device_, &command_pool_info, nullptr, &command_pool_);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateCommandPool failed");
    }

    constexpr std::uint32_t kDescriptorCount = 4096;
    const std::array<VkDescriptorPoolSize, 5> sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorCount},
    }};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 4096;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool_info.pPoolSizes = sizes.data();
    result = vk.vkCreateDescriptorPool(
        device_, &pool_info, nullptr, &descriptor_pool_);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateDescriptorPool failed");
    }
    return checked_validation();
}

Status VulkanDevice::capabilities(
    vita_gpu_device_capabilities &output) {
    const VkPhysicalDeviceLimits &limits = properties_.limits;
    output.flags =
        adapter_capabilities(context_->dispatch(), physical_) |
        (context_->debug_utils_available()
             ? VITA_GPU_CAPABILITY_DEBUG_MARKERS
             : UINT64_C(0));
    output.max_texture_1d = limits.maxImageDimension1D;
    output.max_texture_2d = limits.maxImageDimension2D;
    output.max_texture_3d = limits.maxImageDimension3D;
    output.max_texture_array_layers = limits.maxImageArrayLayers;
    output.max_color_targets =
        std::min(VITA_GPU_MAX_COLOR_TARGETS,
                 limits.maxColorAttachments);
    output.max_binding_sets = limits.maxBoundDescriptorSets;
    output.max_bindings_per_set =
        std::min({limits.maxPerStageDescriptorUniformBuffers,
                  limits.maxPerStageDescriptorStorageBuffers,
                  limits.maxPerStageDescriptorSampledImages,
                  limits.maxPerStageDescriptorStorageImages,
                  limits.maxPerStageDescriptorSamplers});
    output.max_push_constant_bytes = limits.maxPushConstantsSize;
    output.max_sampler_anisotropy =
        features_.samplerAnisotropy == VK_TRUE
            ? static_cast<std::uint32_t>(
                  std::floor(limits.maxSamplerAnisotropy))
            : 1;
    output.constant_buffer_alignment =
        static_cast<std::uint32_t>(
            std::min<VkDeviceSize>(
                limits.minUniformBufferOffsetAlignment,
                std::numeric_limits<std::uint32_t>::max()));
    output.storage_buffer_alignment =
        static_cast<std::uint32_t>(
            std::min<VkDeviceSize>(
                limits.minStorageBufferOffsetAlignment,
                std::numeric_limits<std::uint32_t>::max()));
    output.texture_row_pitch_alignment =
        static_cast<std::uint32_t>(
            std::min<VkDeviceSize>(
                limits.optimalBufferCopyRowPitchAlignment,
                std::numeric_limits<std::uint32_t>::max()));
    const float timestamp = limits.timestampPeriod;
    output.timestamp_period_denominator = 1000;
    output.timestamp_period_numerator =
        static_cast<std::uint32_t>(
            std::max(1.0F, std::round(timestamp * 1000.0F)));
    return checked_validation();
}

Status VulkanDevice::wait_idle() {
    const VkResult result =
        context_->dispatch().vkDeviceWaitIdle(device_);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkDeviceWaitIdle failed");
    }
    Status status = cleanup_submissions();
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::find_memory_type(
    std::uint32_t bits, VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred, std::uint32_t &index,
    VkMemoryPropertyFlags &actual) const {
    VkPhysicalDeviceMemoryProperties memory{};
    context_->dispatch().vkGetPhysicalDeviceMemoryProperties(
        physical_, &memory);
    std::optional<std::uint32_t> fallback;
    for (std::uint32_t candidate = 0;
         candidate < memory.memoryTypeCount; ++candidate) {
        if ((bits & (UINT32_C(1) << candidate)) == 0) {
            continue;
        }
        const VkMemoryPropertyFlags flags =
            memory.memoryTypes[candidate].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            index = candidate;
            actual = flags;
            return Status::success();
        }
        if (!fallback.has_value()) {
            fallback = candidate;
        }
    }
    if (fallback.has_value()) {
        index = *fallback;
        actual = memory.memoryTypes[*fallback].propertyFlags;
        return Status::success();
    }
    return unsupported(
        "physical device has no compatible Vulkan memory type");
}

Status VulkanDevice::create_buffer_allocation(
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    BufferObject &output) {
    auto &vk = context_->dispatch();
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result =
        vk.vkCreateBuffer(device_, &buffer_info, nullptr, &output.buffer);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateBuffer failed");
    }
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(
        device_, output.buffer, &requirements);
    std::uint32_t memory_type = 0;
    Status status = find_memory_type(
        requirements.memoryTypeBits, required, preferred, memory_type,
        output.memory_properties);
    if (!status.ok()) {
        vk.vkDestroyBuffer(device_, output.buffer, nullptr);
        output.buffer = VK_NULL_HANDLE;
        return status;
    }
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.buffer = output.buffer;
    allocation.pNext = &dedicated;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vk.vkAllocateMemory(
        device_, &allocation, nullptr, &output.memory);
    if (result != VK_SUCCESS) {
        vk.vkDestroyBuffer(device_, output.buffer, nullptr);
        output.buffer = VK_NULL_HANDLE;
        return vulkan_error(result, "vkAllocateMemory for buffer failed");
    }
    result = vk.vkBindBufferMemory(
        device_, output.buffer, output.memory, 0);
    if (result != VK_SUCCESS) {
        vk.vkDestroyBuffer(device_, output.buffer, nullptr);
        vk.vkFreeMemory(device_, output.memory, nullptr);
        output = {};
        return vulkan_error(result, "vkBindBufferMemory failed");
    }
    output.size = size;
    return Status::success();
}

Status VulkanDevice::create_image_view(
    TextureObject &texture, VkImageViewType view_type, VkFormat format,
    std::uint32_t mip, std::uint32_t mip_count,
    std::uint32_t first_layer, std::uint32_t layer_count,
    VkImageView &output, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = texture.image;
    info.viewType = view_type;
    info.format = format;
    info.subresourceRange.aspectMask =
        aspect == 0 ? texture.aspect : aspect;
    info.subresourceRange.baseMipLevel = mip;
    info.subresourceRange.levelCount = mip_count;
    info.subresourceRange.baseArrayLayer = first_layer;
    info.subresourceRange.layerCount = layer_count;
    const VkResult result =
        context_->dispatch().vkCreateImageView(
            device_, &info, nullptr, &output);
    return result == VK_SUCCESS
               ? Status::success()
               : vulkan_error(result, "vkCreateImageView failed");
}

Status VulkanDevice::set_name(VkObjectType type, std::uint64_t object,
                              const char *name) {
    if (name == nullptr || name[0] == '\0' ||
        context_->dispatch().vkSetDebugUtilsObjectNameEXT == nullptr) {
        return Status::success();
    }
    VkDebugUtilsObjectNameInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = object;
    info.pObjectName = name;
    const VkResult result =
        context_->dispatch().vkSetDebugUtilsObjectNameEXT(device_, &info);
    return result == VK_SUCCESS
               ? Status::success()
               : vulkan_error(result,
                               "vkSetDebugUtilsObjectNameEXT failed");
}

void VulkanDevice::destroy_swapchain_native(
    SwapchainObject &swapchain, bool destroy_surface) {
    auto &vk = context_->dispatch();
    for (SwapchainFrame &frame : swapchain.frames) {
        if (frame.present_command != VK_NULL_HANDLE) {
            vk.vkFreeCommandBuffers(
                device_, command_pool_, 1,
                &frame.present_command);
        }
        if (frame.image_available != VK_NULL_HANDLE) {
            vk.vkDestroySemaphore(
                device_, frame.image_available, nullptr);
        }
        if (frame.render_finished != VK_NULL_HANDLE) {
            vk.vkDestroySemaphore(
                device_, frame.render_finished, nullptr);
        }
        if (frame.in_flight != VK_NULL_HANDLE) {
            vk.vkDestroyFence(device_, frame.in_flight, nullptr);
        }
    }
    swapchain.frames.clear();
    for (const std::uint64_t id : swapchain.target_ids) {
        const auto target = render_targets_.find(id);
        if (target != render_targets_.end()) {
            vk.vkDestroyImageView(device_, target->second.view, nullptr);
            render_targets_.erase(target);
        }
    }
    for (const std::uint64_t id : swapchain.texture_ids) {
        textures_.erase(id);
    }
    swapchain.target_ids.clear();
    swapchain.texture_ids.clear();
    if (swapchain.swapchain != VK_NULL_HANDLE) {
        vk.vkDestroySwapchainKHR(
            device_, swapchain.swapchain, nullptr);
        swapchain.swapchain = VK_NULL_HANDLE;
    }
    if (destroy_surface && swapchain.surface != VK_NULL_HANDLE) {
        vk.vkDestroySurfaceKHR(
            context_->instance(), swapchain.surface, nullptr);
        swapchain.surface = VK_NULL_HANDLE;
    }
}

Status VulkanDevice::recreate_swapchain(
    SwapchainObject &swapchain, std::uint32_t width,
    std::uint32_t height, bool preserve_ids) {
    auto &vk = context_->dispatch();
    if (width == 0 || height == 0) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_STATE,
            "cannot create a Vulkan swapchain while the window is minimized",
            VITA_GPU_BACKEND_VULKAN);
    }
    VkBool32 present_supported = VK_FALSE;
    VkResult result = vk.vkGetPhysicalDeviceSurfaceSupportKHR(
        physical_, queue_family_, swapchain.surface, &present_supported);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkGetPhysicalDeviceSurfaceSupportKHR failed");
    }
    if (present_supported != VK_TRUE) {
        return unsupported(
            "selected Vulkan graphics queue cannot present to this window");
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    result = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physical_, swapchain.surface, &capabilities);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
    }
    std::uint32_t format_count = 0;
    result = vk.vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_, swapchain.surface, &format_count, nullptr);
    if (result != VK_SUCCESS || format_count == 0) {
        return result == VK_SUCCESS
                   ? unsupported("Vulkan surface exposes no formats")
                   : vulkan_error(
                         result,
                         "vkGetPhysicalDeviceSurfaceFormatsKHR failed");
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    result = vk.vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_, swapchain.surface, &format_count, formats.data());
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed");
    }
    const auto format = std::find_if(
        formats.begin(), formats.end(), [&swapchain](const auto &candidate) {
            return candidate.format == swapchain.format;
        });
    if (format == formats.end()) {
        if (formats.size() != 1 ||
            formats.front().format != VK_FORMAT_UNDEFINED) {
            return unsupported(
                "requested swapchain format is not supported by the surface");
        }
        swapchain.color_space = formats.front().colorSpace;
    } else {
        swapchain.color_space = format->colorSpace;
    }

    std::uint32_t mode_count = 0;
    result = vk.vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_, swapchain.surface, &mode_count, nullptr);
    if (result != VK_SUCCESS || mode_count == 0) {
        return result == VK_SUCCESS
                   ? unsupported("Vulkan surface exposes no present modes")
                   : vulkan_error(
                         result,
                         "vkGetPhysicalDeviceSurfacePresentModesKHR failed");
    }
    std::vector<VkPresentModeKHR> modes(mode_count);
    result = vk.vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_, swapchain.surface, &mode_count, modes.data());
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result,
            "vkGetPhysicalDeviceSurfacePresentModesKHR failed");
    }
    if (std::find(modes.begin(), modes.end(), swapchain.present_mode) ==
        modes.end()) {
        swapchain.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D extent{width, height};
    if (capabilities.currentExtent.width !=
        std::numeric_limits<std::uint32_t>::max()) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(
            extent.width, capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            extent.height, capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }
    std::uint32_t image_count = std::max(
        capabilities.minImageCount, swapchain.requested_image_count);
    if (capabilities.maxImageCount != 0) {
        image_count =
            std::min(image_count, capabilities.maxImageCount);
    }
    if ((capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        return unsupported(
            "Vulkan surface images cannot be used as color attachments");
    }
    VkImageUsageFlags image_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
        image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    VkCompositeAlphaFlagBitsKHR composite =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> alpha_modes{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (const auto candidate : alpha_modes) {
        if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
            composite = candidate;
            break;
        }
    }

    VkSwapchainCreateInfoKHR info{
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = swapchain.surface;
    info.minImageCount = image_count;
    info.imageFormat = swapchain.format;
    info.imageColorSpace = swapchain.color_space;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = image_usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = composite;
    info.presentMode = swapchain.present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain.swapchain;

    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    result = vk.vkCreateSwapchainKHR(
        device_, &info, nullptr, &replacement);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateSwapchainKHR failed");
    }
    std::uint32_t actual_count = 0;
    result = vk.vkGetSwapchainImagesKHR(
        device_, replacement, &actual_count, nullptr);
    if (result != VK_SUCCESS || actual_count == 0) {
        vk.vkDestroySwapchainKHR(device_, replacement, nullptr);
        return result == VK_SUCCESS
                   ? unsupported("Vulkan swapchain contains no images")
                   : vulkan_error(result,
                                  "vkGetSwapchainImagesKHR failed");
    }
    std::vector<VkImage> images(actual_count);
    result = vk.vkGetSwapchainImagesKHR(
        device_, replacement, &actual_count, images.data());
    if (result != VK_SUCCESS) {
        vk.vkDestroySwapchainKHR(device_, replacement, nullptr);
        return vulkan_error(result, "vkGetSwapchainImagesKHR failed");
    }

    std::vector<TextureObject> new_textures(actual_count);
    std::vector<VkImageView> new_views(actual_count, VK_NULL_HANDLE);
    for (std::uint32_t index = 0; index < actual_count; ++index) {
        TextureObject &texture = new_textures[index];
        texture.image = images[index];
        texture.format = swapchain.contract_format;
        texture.width = extent.width;
        texture.height = extent.height;
        texture.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        texture.dimension = VITA_GPU_TEXTURE_2D;
        texture.owned = false;
        texture.layout_initialized = false;
        texture.copy_source_supported =
            (image_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        Status status = create_image_view(
            texture, VK_IMAGE_VIEW_TYPE_2D, swapchain.format, 0, 1, 0, 1,
            new_views[index]);
        if (!status.ok()) {
            for (VkImageView view : new_views) {
                if (view != VK_NULL_HANDLE) {
                    vk.vkDestroyImageView(device_, view, nullptr);
                }
            }
            vk.vkDestroySwapchainKHR(device_, replacement, nullptr);
            return status;
        }
    }
    std::vector<SwapchainFrame> new_frames(actual_count);
    for (SwapchainFrame &frame : new_frames) {
        VkSemaphoreCreateInfo semaphore_info{
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vk.vkCreateSemaphore(
            device_, &semaphore_info, nullptr, &frame.image_available);
        if (result == VK_SUCCESS) {
            result = vk.vkCreateSemaphore(
                device_, &semaphore_info, nullptr, &frame.render_finished);
        }
        VkFenceCreateInfo fence_info{
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (result == VK_SUCCESS) {
            result = vk.vkCreateFence(
                device_, &fence_info, nullptr, &frame.in_flight);
        }
        if (result != VK_SUCCESS) {
            for (SwapchainFrame &created : new_frames) {
                if (created.image_available != VK_NULL_HANDLE) {
                    vk.vkDestroySemaphore(
                        device_, created.image_available, nullptr);
                }
                if (created.render_finished != VK_NULL_HANDLE) {
                    vk.vkDestroySemaphore(
                        device_, created.render_finished, nullptr);
                }
                if (created.in_flight != VK_NULL_HANDLE) {
                    vk.vkDestroyFence(
                        device_, created.in_flight, nullptr);
                }
            }
            for (VkImageView view : new_views) {
                vk.vkDestroyImageView(device_, view, nullptr);
            }
            vk.vkDestroySwapchainKHR(device_, replacement, nullptr);
            return vulkan_error(
                result,
                "creating Vulkan swapchain frame synchronization failed");
        }
    }

    result = vk.vkDeviceWaitIdle(device_);
    if (result != VK_SUCCESS) {
        for (SwapchainFrame &frame : new_frames) {
            vk.vkDestroySemaphore(
                device_, frame.image_available, nullptr);
            vk.vkDestroySemaphore(
                device_, frame.render_finished, nullptr);
            vk.vkDestroyFence(device_, frame.in_flight, nullptr);
        }
        for (VkImageView view : new_views) {
            vk.vkDestroyImageView(device_, view, nullptr);
        }
        vk.vkDestroySwapchainKHR(device_, replacement, nullptr);
        return vulkan_error(
            result, "vkDeviceWaitIdle before swapchain recreation failed");
    }
    const VkSwapchainKHR old_swapchain = swapchain.swapchain;
    const std::vector<std::uint64_t> old_textures =
        std::move(swapchain.texture_ids);
    const std::vector<std::uint64_t> old_targets =
        std::move(swapchain.target_ids);
    for (SwapchainFrame &frame : swapchain.frames) {
        if (frame.present_command != VK_NULL_HANDLE) {
            vk.vkFreeCommandBuffers(
                device_, command_pool_, 1,
                &frame.present_command);
        }
        vk.vkDestroySemaphore(device_, frame.image_available, nullptr);
        vk.vkDestroySemaphore(device_, frame.render_finished, nullptr);
        vk.vkDestroyFence(device_, frame.in_flight, nullptr);
    }
    swapchain.frames.clear();

    swapchain.texture_ids.reserve(actual_count);
    swapchain.target_ids.reserve(actual_count);
    for (std::uint32_t index = 0; index < actual_count; ++index) {
        std::uint64_t texture_id = 0;
        std::uint64_t target_id = 0;
        if (preserve_ids && index < old_textures.size() &&
            index < old_targets.size()) {
            texture_id = old_textures[index];
            target_id = old_targets[index];
            const auto old_target = render_targets_.find(target_id);
            if (old_target != render_targets_.end()) {
                vk.vkDestroyImageView(
                    device_, old_target->second.view, nullptr);
            }
            textures_[texture_id] = new_textures[index];
            render_targets_[target_id] =
                ViewObject{new_views[index], texture_id,
                           swapchain.contract_format,
                           extent.width, extent.height, 1, 1, false};
        } else {
            texture_id =
                store(textures_, new_textures[index]).value;
            target_id =
                store(render_targets_,
                      ViewObject{new_views[index], texture_id,
                                 swapchain.contract_format, extent.width,
                                 extent.height, 1, 1, false})
                    .value;
        }
        swapchain.texture_ids.push_back(texture_id);
        swapchain.target_ids.push_back(target_id);
    }
    for (std::size_t index = 0; index < old_targets.size(); ++index) {
        if (preserve_ids && index < actual_count) {
            continue;
        }
        const auto target = render_targets_.find(old_targets[index]);
        if (target != render_targets_.end()) {
            vk.vkDestroyImageView(device_, target->second.view, nullptr);
            render_targets_.erase(target);
        }
    }
    for (std::size_t index = 0; index < old_textures.size(); ++index) {
        if (!preserve_ids || index >= actual_count) {
            textures_.erase(old_textures[index]);
        }
    }
    if (old_swapchain != VK_NULL_HANDLE) {
        vk.vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    }
    swapchain.swapchain = replacement;
    swapchain.width = extent.width;
    swapchain.height = extent.height;
    swapchain.frames = std::move(new_frames);
    swapchain.frame_cursor = 0;
    swapchain.acquired = false;
    swapchain.submitted = false;
    swapchain.suboptimal = false;
    return checked_validation();
}

Status VulkanDevice::create_swapchain(
    const vita_gpu_swapchain_desc &desc, NativeId &output) {
    if (desc.window.system != VITA_GPU_WINDOW_WIN32 ||
        desc.window.native_window == nullptr) {
        return unsupported(
            "Vulkan backend requires a valid Win32 HWND");
    }
    SwapchainObject swapchain;
    swapchain.window = static_cast<HWND>(desc.window.native_window);
    swapchain.width = desc.width;
    swapchain.height = desc.height;
    swapchain.format = to_vk_format(desc.format);
    swapchain.contract_format = desc.format;
    swapchain.requested_image_count = desc.image_count;
    switch (desc.present_mode) {
    case VITA_GPU_PRESENT_IMMEDIATE:
        swapchain.present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    case VITA_GPU_PRESENT_MAILBOX:
        swapchain.present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        break;
    default:
        swapchain.present_mode = VK_PRESENT_MODE_FIFO_KHR;
        break;
    }
    VkWin32SurfaceCreateInfoKHR surface_info{
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance =
        reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(swapchain.window, GWLP_HINSTANCE));
    surface_info.hwnd = swapchain.window;
    VkResult result =
        context_->dispatch().vkCreateWin32SurfaceKHR(
            context_->instance(), &surface_info, nullptr,
            &swapchain.surface);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateWin32SurfaceKHR failed");
    }
    Status status = recreate_swapchain(
        swapchain, desc.width, desc.height, false);
    if (!status.ok()) {
        context_->dispatch().vkDestroySurfaceKHR(
            context_->instance(), swapchain.surface, nullptr);
        return status;
    }
    output = store(swapchains_, std::move(swapchain));
    return set_name(
        VK_OBJECT_TYPE_SWAPCHAIN_KHR,
        reinterpret_cast<std::uint64_t>(
            swapchains_[output.value].swapchain),
        desc.label);
}

Status VulkanDevice::resize_swapchain(
    NativeId id, std::uint32_t width, std::uint32_t height) {
    SwapchainObject *swapchain = find(swapchains_, id);
    if (swapchain == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE, "unknown Vulkan swapchain",
            VITA_GPU_BACKEND_VULKAN);
    }
    return recreate_swapchain(*swapchain, width, height, false);
}

Status VulkanDevice::acquire_swapchain(
    NativeId id, BackendSwapchainImage &output) {
    SwapchainObject *swapchain = find(swapchains_, id);
    if (swapchain == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE, "unknown Vulkan swapchain",
            VITA_GPU_BACKEND_VULKAN);
    }
    RECT client{};
    if (IsIconic(swapchain->window) != FALSE ||
        GetClientRect(swapchain->window, &client) == FALSE ||
        client.right <= client.left || client.bottom <= client.top) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_STATE,
            "cannot acquire a swapchain image while the window is minimized",
            VITA_GPU_BACKEND_VULKAN);
    }
    SwapchainFrame &frame =
        swapchain->frames[swapchain->frame_cursor %
                          swapchain->frames.size()];
    if (frame.submitted) {
        const VkResult wait_result =
            context_->dispatch().vkWaitForFences(
                device_, 1, &frame.in_flight, VK_TRUE,
                std::numeric_limits<std::uint64_t>::max());
        if (wait_result != VK_SUCCESS) {
            return vulkan_error(
                wait_result,
                "waiting for a Vulkan swapchain frame failed");
        }
        if (frame.present_command != VK_NULL_HANDLE) {
            context_->dispatch().vkFreeCommandBuffers(
                device_, command_pool_, 1,
                &frame.present_command);
            frame.present_command = VK_NULL_HANDLE;
        }
        frame.submitted = false;
    }
    std::uint32_t image_index = 0;
    VkResult result = context_->dispatch().vkAcquireNextImageKHR(
        device_, swapchain->swapchain,
        std::numeric_limits<std::uint64_t>::max(),
        frame.image_available, VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        const std::uint32_t current_width =
            static_cast<std::uint32_t>(client.right - client.left);
        const std::uint32_t current_height =
            static_cast<std::uint32_t>(client.bottom - client.top);
        Status status = recreate_swapchain(
            *swapchain, current_width, current_height, true);
        if (!status.ok()) {
            return status;
        }
        return acquire_swapchain(id, output);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return vulkan_error(result, "vkAcquireNextImageKHR failed");
    }
    if (image_index >= swapchain->texture_ids.size()) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "Vulkan returned an out-of-range swapchain image",
            VITA_GPU_BACKEND_VULKAN);
    }
    swapchain->acquired = true;
    swapchain->submitted = false;
    swapchain->suboptimal = result == VK_SUBOPTIMAL_KHR;
    swapchain->acquired_image = image_index;
    swapchain->acquired_frame =
        swapchain->frame_cursor %
        static_cast<std::uint32_t>(swapchain->frames.size());
    swapchain->frame_cursor =
        (swapchain->frame_cursor + 1U) %
        static_cast<std::uint32_t>(swapchain->frames.size());
    output.image_index = image_index;
    output.texture =
        NativeId{swapchain->texture_ids[image_index]};
    output.render_target =
        NativeId{swapchain->target_ids[image_index]};
    return checked_validation();
}

Status VulkanDevice::present_swapchain(
    NativeId id, const vita_gpu_present_desc &desc) {
    SwapchainObject *swapchain = find(swapchains_, id);
    if (swapchain == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE, "unknown Vulkan swapchain",
            VITA_GPU_BACKEND_VULKAN);
    }
    if (!swapchain->acquired ||
        desc.image_index != swapchain->acquired_image) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_STATE,
            "Vulkan present requires the acquired image",
            VITA_GPU_BACKEND_VULKAN);
    }
    if (desc.allow_tearing == VITA_GPU_TRUE &&
        swapchain->present_mode == VK_PRESENT_MODE_FIFO_KHR) {
        return unsupported(
            "tearing cannot be enabled per-present for a FIFO Vulkan swapchain");
    }
    SwapchainFrame &frame =
        swapchain->frames[swapchain->acquired_frame];
    auto &vk = context_->dispatch();
    if (!swapchain->submitted) {
        TextureObject *texture = find(
            textures_,
            NativeId{swapchain->texture_ids[
                swapchain->acquired_image]});
        if (texture == nullptr) {
            return Status::error(
                VITA_GPU_ERROR_INTERNAL,
                "acquired Vulkan swapchain texture is missing",
                VITA_GPU_BACKEND_VULKAN);
        }
        VkResult result =
            vk.vkResetFences(device_, 1, &frame.in_flight);
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result,
                "resetting the Vulkan frame fence before an unchanged present failed");
        }
        VkCommandBuffer transition = VK_NULL_HANDLE;
        if (!texture->layout_initialized) {
            VkCommandBufferAllocateInfo allocation{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = command_pool_;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            result = vk.vkAllocateCommandBuffers(
                device_, &allocation, &transition);
            if (result == VK_SUCCESS) {
                VkCommandBufferBeginInfo begin{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags =
                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                result = vk.vkBeginCommandBuffer(
                    transition, &begin);
            }
            if (result == VK_SUCCESS) {
                VkImageMemoryBarrier2 barrier{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask =
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                barrier.dstStageMask =
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout =
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.srcQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;
                barrier.image = texture->image;
                barrier.subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                VkDependencyInfo dependency{
                    VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &barrier;
                vk.vkCmdPipelineBarrier2(
                    transition, &dependency);
                result = vk.vkEndCommandBuffer(transition);
            }
            if (result != VK_SUCCESS) {
                if (transition != VK_NULL_HANDLE) {
                    vk.vkFreeCommandBuffers(
                        device_, command_pool_, 1,
                        &transition);
                }
                return vulkan_error(
                    result,
                    "recording an unchanged Vulkan present transition failed");
            }
        }
        const VkPipelineStageFlags wait_stage =
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.image_available;
        submit.pWaitDstStageMask = &wait_stage;
        if (transition != VK_NULL_HANDLE) {
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &transition;
        }
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &frame.render_finished;
        result = vk.vkQueueSubmit(
            queue_, 1, &submit, frame.in_flight);
        if (result != VK_SUCCESS) {
            if (transition != VK_NULL_HANDLE) {
                vk.vkFreeCommandBuffers(
                    device_, command_pool_, 1, &transition);
            }
            return vulkan_error(
                result,
                "submitting an unchanged Vulkan swapchain image failed");
        }
        frame.present_command = transition;
        texture->layout_initialized = true;
        frame.submitted = true;
        swapchain->submitted = true;
    }
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame.render_finished;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain->swapchain;
    info.pImageIndices = &swapchain->acquired_image;
    VkResult result = vk.vkQueuePresentKHR(queue_, &info);
    const bool recreate =
        result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_SUBOPTIMAL_KHR || swapchain->suboptimal;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR &&
        result != VK_ERROR_OUT_OF_DATE_KHR) {
        return vulkan_error(result, "vkQueuePresentKHR failed");
    }
    swapchain->acquired = false;
    swapchain->submitted = false;
    if (recreate) {
        RECT client{};
        if (IsIconic(swapchain->window) == FALSE &&
            GetClientRect(swapchain->window, &client) != FALSE &&
            client.right > client.left && client.bottom > client.top) {
            Status status = recreate_swapchain(
                *swapchain,
                static_cast<std::uint32_t>(
                    client.right - client.left),
                static_cast<std::uint32_t>(
                    client.bottom - client.top),
                true);
            if (!status.ok()) {
                return status;
            }
        }
    }
    return checked_validation();
}

Status VulkanDevice::begin_immediate(VkCommandBuffer &command) {
    VkCommandBufferAllocateInfo allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = command_pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkResult result =
        context_->dispatch().vkAllocateCommandBuffers(
            device_, &allocate, &command);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkAllocateCommandBuffers failed");
    }
    VkCommandBufferBeginInfo begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result =
        context_->dispatch().vkBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS) {
        context_->dispatch().vkFreeCommandBuffers(
            device_, command_pool_, 1, &command);
        command = VK_NULL_HANDLE;
        return vulkan_error(result, "vkBeginCommandBuffer failed");
    }
    return Status::success();
}

Status VulkanDevice::end_immediate(VkCommandBuffer command) {
    auto &vk = context_->dispatch();
    VkResult result = vk.vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) {
        vk.vkFreeCommandBuffers(device_, command_pool_, 1, &command);
        return vulkan_error(result, "vkEndCommandBuffer failed");
    }
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    result = vk.vkQueueSubmit(
        queue_, 1, &submit_info, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) {
        result = vk.vkQueueWaitIdle(queue_);
    }
    vk.vkFreeCommandBuffers(device_, command_pool_, 1, &command);
    return result == VK_SUCCESS
               ? checked_validation()
               : vulkan_error(result,
                               "immediate Vulkan submission failed");
}

Status VulkanDevice::transition_image_immediate(
    TextureObject &texture, vita_gpu_resource_state before,
    vita_gpu_resource_state after) {
    if (before == after) {
        return Status::success();
    }
    VkCommandBuffer command = VK_NULL_HANDLE;
    Status status = begin_immediate(command);
    if (!status.ok()) {
        return status;
    }
    const AccessState source = access_for(before);
    const AccessState destination = access_for(after);
    VkImageMemoryBarrier2 barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = source.stages;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destination.stages;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout =
        layout_for(before, is_depth_format(texture.format));
    barrier.newLayout =
        layout_for(after, is_depth_format(texture.format));
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = texture.aspect;
    barrier.subresourceRange.levelCount = texture.mip_levels;
    barrier.subresourceRange.layerCount = texture.layers;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    context_->dispatch().vkCmdPipelineBarrier2(command, &dependency);
    return end_immediate(command);
}

Status VulkanDevice::upload_buffer(
    BufferObject &destination, const std::uint8_t *data,
    VkDeviceSize size, vita_gpu_resource_state final_state) {
    BufferObject staging;
    Status status = create_buffer_allocation(
        size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
    if (!status.ok()) {
        return status;
    }
    auto &vk = context_->dispatch();
    void *mapped = nullptr;
    VkResult result =
        vk.vkMapMemory(device_, staging.memory, 0, size, 0, &mapped);
    if (result == VK_SUCCESS) {
        std::memcpy(mapped, data, static_cast<std::size_t>(size));
        if ((staging.memory_properties &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range{
                VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = staging.memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            result =
                vk.vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        vk.vkUnmapMemory(device_, staging.memory);
    }
    if (result != VK_SUCCESS) {
        vk.vkDestroyBuffer(device_, staging.buffer, nullptr);
        vk.vkFreeMemory(device_, staging.memory, nullptr);
        return vulkan_error(
            result, "mapping Vulkan upload memory failed");
    }
    VkCommandBuffer command = VK_NULL_HANDLE;
    status = begin_immediate(command);
    if (status.ok()) {
        VkBufferCopy copy{};
        copy.size = size;
        vk.vkCmdCopyBuffer(
            command, staging.buffer, destination.buffer, 1, &copy);
        const AccessState final_access = access_for(final_state);
        VkBufferMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = final_access.stages;
        barrier.dstAccessMask = final_access.access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = destination.buffer;
        barrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vk.vkCmdPipelineBarrier2(command, &dependency);
        status = end_immediate(command);
    }
    vk.vkDestroyBuffer(device_, staging.buffer, nullptr);
    vk.vkFreeMemory(device_, staging.memory, nullptr);
    return status;
}

Status VulkanDevice::create_buffer(
    const vita_gpu_buffer_desc &desc, NativeId &output) {
    VkBufferUsageFlags usage = 0;
    if ((desc.usage & VITA_GPU_BUFFER_VERTEX) != 0) {
        usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if ((desc.usage & VITA_GPU_BUFFER_INDEX) != 0) {
        usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if ((desc.usage & VITA_GPU_BUFFER_CONSTANT) != 0) {
        usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if ((desc.usage & VITA_GPU_BUFFER_STORAGE) != 0) {
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if ((desc.usage & VITA_GPU_BUFFER_INDIRECT) != 0) {
        usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if ((desc.usage & VITA_GPU_BUFFER_COPY_SOURCE) != 0) {
        usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if ((desc.usage & VITA_GPU_BUFFER_COPY_DEST) != 0) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (desc.initial_data != nullptr) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (desc.memory_type == VITA_GPU_MEMORY_CPU_UPLOAD) {
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        preferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else if (desc.memory_type == VITA_GPU_MEMORY_CPU_READBACK) {
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    BufferObject buffer;
    Status status = create_buffer_allocation(
        desc.byte_size, usage, required, preferred, buffer);
    if (!status.ok()) {
        return status;
    }
    auto &vk = context_->dispatch();
    if (desc.initial_data != nullptr) {
        if ((buffer.memory_properties &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            void *mapped = nullptr;
            VkResult result = vk.vkMapMemory(
                device_, buffer.memory, 0, desc.initial_data_size, 0,
                &mapped);
            if (result == VK_SUCCESS) {
                std::memcpy(
                    mapped, desc.initial_data,
                    static_cast<std::size_t>(desc.initial_data_size));
                if ((buffer.memory_properties &
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
                    VkMappedMemoryRange range{
                        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                    range.memory = buffer.memory;
                    range.size = VK_WHOLE_SIZE;
                    result = vk.vkFlushMappedMemoryRanges(
                        device_, 1, &range);
                }
                vk.vkUnmapMemory(device_, buffer.memory);
            }
            if (result != VK_SUCCESS) {
                vk.vkDestroyBuffer(device_, buffer.buffer, nullptr);
                vk.vkFreeMemory(device_, buffer.memory, nullptr);
                return vulkan_error(
                    result, "mapping Vulkan buffer upload failed");
            }
        } else {
            status = upload_buffer(
                buffer, desc.initial_data, desc.initial_data_size,
                desc.initial_state);
            if (!status.ok()) {
                vk.vkDestroyBuffer(device_, buffer.buffer, nullptr);
                vk.vkFreeMemory(device_, buffer.memory, nullptr);
                return status;
            }
        }
    }
    output = store(buffers_, std::move(buffer));
    status = set_name(
        VK_OBJECT_TYPE_BUFFER,
        reinterpret_cast<std::uint64_t>(
            buffers_[output.value].buffer),
        desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::upload_texture(
    TextureObject &destination,
    const vita_gpu_texture_desc &desc) {
    const FormatLayout layout = format_layout(desc.format);
    if (layout.bytes_per_block == 0) {
        return unsupported(
            "texture upload format has no Vulkan packing rule");
    }
    struct PackedSubresource {
        VkBufferImageCopy region{};
        const vita_gpu_subresource_data *source = nullptr;
        std::uint32_t tight_row = 0;
        std::uint32_t block_rows = 0;
        std::uint32_t depth = 1;
    };
    std::vector<PackedSubresource> packed;
    packed.reserve(desc.initial_data_count);
    VkDeviceSize total_size = 0;
    std::uint32_t source_index = 0;
    const std::uint32_t layers =
        desc.dimension == VITA_GPU_TEXTURE_3D
            ? 1U
            : desc.depth_or_layers;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t mip = 0; mip < desc.mip_levels; ++mip) {
            if (source_index == desc.initial_data_count) {
                break;
            }
            const std::uint32_t width =
                std::max(1U, desc.width >> mip);
            const std::uint32_t height =
                desc.dimension == VITA_GPU_TEXTURE_1D
                    ? 1U
                    : std::max(1U, desc.height >> mip);
            const std::uint32_t depth =
                desc.dimension == VITA_GPU_TEXTURE_3D
                    ? std::max(1U, desc.depth_or_layers >> mip)
                    : 1U;
            const std::uint32_t blocks_x =
                (width + layout.block_width - 1U) /
                layout.block_width;
            const std::uint32_t blocks_y =
                (height + layout.block_height - 1U) /
                layout.block_height;
            PackedSubresource item;
            item.source = &desc.initial_data[source_index++];
            item.tight_row = blocks_x * layout.bytes_per_block;
            item.block_rows = blocks_y;
            item.depth = depth;
            const VkDeviceSize offset_alignment =
                std::max<VkDeviceSize>(
                    4, layout.bytes_per_block);
            total_size =
                ((total_size + offset_alignment - 1) /
                 offset_alignment) *
                offset_alignment;
            item.region.bufferOffset = total_size;
            item.region.imageSubresource.aspectMask =
                destination.aspect;
            item.region.imageSubresource.mipLevel = mip;
            item.region.imageSubresource.baseArrayLayer = layer;
            item.region.imageSubresource.layerCount = 1;
            item.region.imageExtent = {width, height, depth};
            total_size +=
                static_cast<VkDeviceSize>(item.tight_row) *
                blocks_y * depth;
            packed.push_back(item);
        }
    }
    for (const PackedSubresource &item : packed) {
        const std::uint32_t source_row =
            item.source->row_pitch == 0
                ? item.tight_row
                : item.source->row_pitch;
        const std::uint32_t source_slice =
            item.source->slice_pitch == 0
                ? source_row * item.block_rows
                : item.source->slice_pitch;
        const std::uint64_t required_size =
            static_cast<std::uint64_t>(item.depth - 1U) *
                source_slice +
            static_cast<std::uint64_t>(item.block_rows - 1U) *
                source_row +
            item.tight_row;
        if (source_row < item.tight_row ||
            source_slice <
                static_cast<std::uint64_t>(source_row) *
                    item.block_rows ||
            item.source->data_size < required_size) {
            return invalid_argument(
                "texture upload pitches or data size are too small for the subresource");
        }
    }
    BufferObject staging;
    Status status = create_buffer_allocation(
        total_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
    if (!status.ok()) {
        return status;
    }
    auto &vk = context_->dispatch();
    void *mapped = nullptr;
    VkResult result = vk.vkMapMemory(
        device_, staging.memory, 0, total_size, 0, &mapped);
    if (result != VK_SUCCESS) {
        vk.vkDestroyBuffer(device_, staging.buffer, nullptr);
        vk.vkFreeMemory(device_, staging.memory, nullptr);
        return vulkan_error(
            result, "mapping Vulkan texture upload failed");
    }
    auto *destination_bytes = static_cast<std::uint8_t *>(mapped);
    for (const PackedSubresource &item : packed) {
        const std::uint32_t source_row =
            item.source->row_pitch == 0
                ? item.tight_row
                : item.source->row_pitch;
        const std::uint32_t source_slice =
            item.source->slice_pitch == 0
                ? source_row * item.block_rows
                : item.source->slice_pitch;
        auto *write =
            destination_bytes +
            static_cast<std::size_t>(item.region.bufferOffset);
        for (std::uint32_t z = 0; z < item.depth; ++z) {
            for (std::uint32_t row = 0;
                 row < item.block_rows; ++row) {
                const auto *read =
                    item.source->data +
                    static_cast<std::size_t>(z) * source_slice +
                    static_cast<std::size_t>(row) * source_row;
                std::memcpy(write, read, item.tight_row);
                write += item.tight_row;
            }
        }
    }
    if ((staging.memory_properties &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        VkMappedMemoryRange range{
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = staging.memory;
        range.size = VK_WHOLE_SIZE;
        result = vk.vkFlushMappedMemoryRanges(device_, 1, &range);
    }
    vk.vkUnmapMemory(device_, staging.memory);
    if (result != VK_SUCCESS) {
        vk.vkDestroyBuffer(device_, staging.buffer, nullptr);
        vk.vkFreeMemory(device_, staging.memory, nullptr);
        return vulkan_error(
            result, "flushing Vulkan texture upload failed");
    }

    VkCommandBuffer command = VK_NULL_HANDLE;
    status = begin_immediate(command);
    if (status.ok()) {
        const AccessState destination_access =
            access_for(VITA_GPU_RESOURCE_STATE_COPY_DEST);
        VkImageMemoryBarrier2 to_copy{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_copy.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_copy.dstStageMask = destination_access.stages;
        to_copy.dstAccessMask = destination_access.access;
        to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_copy.image = destination.image;
        to_copy.subresourceRange.aspectMask = destination.aspect;
        to_copy.subresourceRange.levelCount = destination.mip_levels;
        to_copy.subresourceRange.layerCount = destination.layers;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &to_copy;
        vk.vkCmdPipelineBarrier2(command, &dependency);

        std::vector<VkBufferImageCopy> regions;
        regions.reserve(packed.size());
        for (const auto &item : packed) {
            regions.push_back(item.region);
        }
        vk.vkCmdCopyBufferToImage(
            command, staging.buffer, destination.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<std::uint32_t>(regions.size()),
            regions.data());

        const AccessState final_access =
            access_for(desc.initial_state);
        VkImageMemoryBarrier2 to_final{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_final.srcStageMask = destination_access.stages;
        to_final.srcAccessMask = destination_access.access;
        to_final.dstStageMask = final_access.stages;
        to_final.dstAccessMask = final_access.access;
        to_final.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_final.newLayout =
            layout_for(desc.initial_state,
                       is_depth_format(desc.format));
        to_final.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_final.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_final.image = destination.image;
        to_final.subresourceRange = to_copy.subresourceRange;
        dependency.pImageMemoryBarriers = &to_final;
        vk.vkCmdPipelineBarrier2(command, &dependency);
        status = end_immediate(command);
    }
    vk.vkDestroyBuffer(device_, staging.buffer, nullptr);
    vk.vkFreeMemory(device_, staging.memory, nullptr);
    return status;
}

Status VulkanDevice::create_texture(
    const vita_gpu_texture_desc &desc, NativeId &output) {
    if (desc.memory_type != VITA_GPU_MEMORY_GPU_ONLY) {
        return unsupported(
            "Vulkan CPU-visible textures are unsupported; use upload or readback buffers");
    }
    if (has_stencil(desc.format) &&
        desc.initial_data_count != 0) {
        return unsupported(
            "Vulkan packed depth-stencil texture uploads are unsupported");
    }
    if (has_stencil(desc.format) &&
        (desc.usage & VITA_GPU_TEXTURE_SAMPLED) != 0) {
        return unsupported(
            "sampled Vulkan depth-stencil textures require aspect selection not represented by the contract");
    }
    if ((desc.usage & VITA_GPU_TEXTURE_STORAGE) != 0 &&
        desc.mip_levels != 1) {
        return unsupported(
            "Vulkan storage textures require one mip because the contract has no storage-image view selection");
    }
    if (desc.sample_count > 1 &&
        (desc.usage & VITA_GPU_TEXTURE_COPY_SOURCE) != 0) {
        return unsupported(
            "multisampled Vulkan textures require a resolve before readback");
    }
    TextureObject texture;
    texture.format = desc.format;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.depth =
        desc.dimension == VITA_GPU_TEXTURE_3D
            ? desc.depth_or_layers
            : 1;
    texture.layers =
        desc.dimension == VITA_GPU_TEXTURE_3D
            ? 1
            : desc.depth_or_layers;
    texture.mip_levels = desc.mip_levels;
    texture.sample_count = desc.sample_count;
    texture.dimension = desc.dimension;
    texture.aspect = aspect_for(desc.format);
    texture.copy_source_supported =
        (desc.usage & VITA_GPU_TEXTURE_COPY_SOURCE) != 0;
    VkImageUsageFlags usage = 0;
    if ((desc.usage & VITA_GPU_TEXTURE_SAMPLED) != 0) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_STORAGE) != 0) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_RENDER_TARGET) != 0) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_DEPTH_TARGET) != 0) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_COPY_SOURCE) != 0) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_COPY_DEST) != 0 ||
        desc.initial_data_count != 0) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType =
        desc.dimension == VITA_GPU_TEXTURE_1D
            ? VK_IMAGE_TYPE_1D
            : desc.dimension == VITA_GPU_TEXTURE_2D
                  ? VK_IMAGE_TYPE_2D
                  : VK_IMAGE_TYPE_3D;
    info.format = to_vk_format(desc.format);
    info.extent = {
        desc.width,
        desc.dimension == VITA_GPU_TEXTURE_1D ? 1U : desc.height,
        texture.depth};
    info.mipLevels = desc.mip_levels;
    info.arrayLayers = texture.layers;
    info.samples = to_vk_samples(desc.sample_count);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto &vk = context_->dispatch();
    VkImageFormatProperties supported{};
    VkResult result =
        vk.vkGetPhysicalDeviceImageFormatProperties(
            physical_, info.format, info.imageType, info.tiling,
            info.usage, info.flags, &supported);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        return unsupported(
            "texture format, usage, or dimension is unsupported by the selected Vulkan device");
    }
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result,
            "querying Vulkan texture format support failed");
    }
    if (info.extent.width > supported.maxExtent.width ||
        info.extent.height > supported.maxExtent.height ||
        info.extent.depth > supported.maxExtent.depth ||
        info.mipLevels > supported.maxMipLevels ||
        info.arrayLayers > supported.maxArrayLayers ||
        (supported.sampleCounts & info.samples) == 0) {
        return unsupported(
            "texture dimensions, mip count, layers, or samples exceed Vulkan format limits");
    }
    result =
        vk.vkCreateImage(device_, &info, nullptr, &texture.image);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateImage failed");
    }
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(
        device_, texture.image, &requirements);
    std::uint32_t memory_type = 0;
    VkMemoryPropertyFlags actual = 0;
    Status status = find_memory_type(
        requirements.memoryTypeBits, 0,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type, actual);
    if (!status.ok()) {
        vk.vkDestroyImage(device_, texture.image, nullptr);
        return status;
    }
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = texture.image;
    allocation.pNext = &dedicated;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vk.vkAllocateMemory(
        device_, &allocation, nullptr, &texture.memory);
    if (result == VK_SUCCESS) {
        result = vk.vkBindImageMemory(
            device_, texture.image, texture.memory, 0);
    }
    if (result != VK_SUCCESS) {
        vk.vkDestroyImage(device_, texture.image, nullptr);
        if (texture.memory != VK_NULL_HANDLE) {
            vk.vkFreeMemory(device_, texture.memory, nullptr);
        }
        return vulkan_error(
            result, "allocating Vulkan image memory failed");
    }
    const VkImageViewType view_type =
        desc.dimension == VITA_GPU_TEXTURE_1D
            ? (texture.layers > 1
                   ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                   : VK_IMAGE_VIEW_TYPE_1D)
            : desc.dimension == VITA_GPU_TEXTURE_2D
                  ? (texture.layers > 1
                         ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                         : VK_IMAGE_VIEW_TYPE_2D)
                  : VK_IMAGE_VIEW_TYPE_3D;
    status = create_image_view(
        texture, view_type, info.format, 0, texture.mip_levels, 0,
        texture.layers, texture.default_view,
        is_depth_format(desc.format)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : texture.aspect);
    if (!status.ok()) {
        vk.vkDestroyImage(device_, texture.image, nullptr);
        vk.vkFreeMemory(device_, texture.memory, nullptr);
        return status;
    }
    if (desc.initial_data_count != 0) {
        status = upload_texture(texture, desc);
    } else if (desc.initial_state !=
               VITA_GPU_RESOURCE_STATE_UNDEFINED) {
        status = transition_image_immediate(
            texture, VITA_GPU_RESOURCE_STATE_UNDEFINED,
            desc.initial_state);
    }
    if (!status.ok()) {
        vk.vkDestroyImageView(
            device_, texture.default_view, nullptr);
        vk.vkDestroyImage(device_, texture.image, nullptr);
        vk.vkFreeMemory(device_, texture.memory, nullptr);
        return status;
    }
    output = store(textures_, std::move(texture));
    status = set_name(
        VK_OBJECT_TYPE_IMAGE,
        reinterpret_cast<std::uint64_t>(
            textures_[output.value].image),
        desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_sampler(
    const vita_gpu_sampler_desc &desc, NativeId &output) {
    if (desc.max_anisotropy > 1 &&
        (features_.samplerAnisotropy != VK_TRUE ||
         static_cast<float>(desc.max_anisotropy) >
             properties_.limits.maxSamplerAnisotropy)) {
        return unsupported(
            "requested Vulkan sampler anisotropy exceeds device support");
    }
    if (std::abs(desc.mip_lod_bias) >
        properties_.limits.maxSamplerLodBias) {
        return unsupported(
            "requested Vulkan sampler LOD bias exceeds device support");
    }
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = desc.mag_filter == VITA_GPU_FILTER_LINEAR
                         ? VK_FILTER_LINEAR
                         : VK_FILTER_NEAREST;
    info.minFilter = desc.min_filter == VITA_GPU_FILTER_LINEAR
                         ? VK_FILTER_LINEAR
                         : VK_FILTER_NEAREST;
    info.mipmapMode =
        desc.mipmap_mode == VITA_GPU_MIPMAP_LINEAR
            ? VK_SAMPLER_MIPMAP_MODE_LINEAR
            : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const auto address_mode = [](vita_gpu_address_mode mode) {
        switch (mode) {
        case VITA_GPU_ADDRESS_REPEAT:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case VITA_GPU_ADDRESS_MIRROR_REPEAT:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case VITA_GPU_ADDRESS_CLAMP_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    };
    info.addressModeU = address_mode(desc.address_u);
    info.addressModeV = address_mode(desc.address_v);
    info.addressModeW = address_mode(desc.address_w);
    info.mipLodBias = desc.mip_lod_bias;
    info.anisotropyEnable = desc.max_anisotropy > 1 ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy =
        static_cast<float>(std::max(1U, desc.max_anisotropy));
    info.compareEnable = desc.compare_enable == VITA_GPU_TRUE
                             ? VK_TRUE
                             : VK_FALSE;
    info.compareOp = to_vk_compare(desc.compare_op);
    info.minLod = desc.min_lod;
    info.maxLod = desc.max_lod;
    const bool uses_border =
        desc.address_u == VITA_GPU_ADDRESS_CLAMP_BORDER ||
        desc.address_v == VITA_GPU_ADDRESS_CLAMP_BORDER ||
        desc.address_w == VITA_GPU_ADDRESS_CLAMP_BORDER;
    const bool transparent_black =
        desc.border_color[0] == 0.0F &&
        desc.border_color[1] == 0.0F &&
        desc.border_color[2] == 0.0F &&
        desc.border_color[3] == 0.0F;
    const bool opaque_black =
        desc.border_color[0] == 0.0F &&
        desc.border_color[1] == 0.0F &&
        desc.border_color[2] == 0.0F &&
        desc.border_color[3] == 1.0F;
    const bool opaque_white =
        desc.border_color[0] == 1.0F &&
        desc.border_color[1] == 1.0F &&
        desc.border_color[2] == 1.0F &&
        desc.border_color[3] == 1.0F;
    if (uses_border && !transparent_black && !opaque_black &&
        !opaque_white) {
        return unsupported(
            "Vulkan core samplers support only transparent black, opaque black, or opaque white borders");
    }
    if (opaque_white) {
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    } else if (opaque_black) {
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else {
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    SamplerObject sampler;
    const VkResult result =
        context_->dispatch().vkCreateSampler(
            device_, &info, nullptr, &sampler.sampler);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateSampler failed");
    }
    output = store(samplers_, sampler);
    Status status = set_name(
        VK_OBJECT_TYPE_SAMPLER,
        reinterpret_cast<std::uint64_t>(sampler.sampler),
        desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_render_target(
    const vita_gpu_render_target_desc &desc, NativeId texture_id,
    NativeId &output) {
    TextureObject *texture = find(textures_, texture_id);
    if (texture == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE, "unknown Vulkan texture",
            VITA_GPU_BACKEND_VULKAN);
    }
    if (texture->dimension != VITA_GPU_TEXTURE_2D) {
        return unsupported(
            "Vulkan render target views currently require 2D textures");
    }
    VkImageView view = VK_NULL_HANDLE;
    Status status = create_image_view(
        *texture,
        desc.array_layer_count > 1
            ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
            : VK_IMAGE_VIEW_TYPE_2D,
        to_vk_format(desc.format), desc.mip_level, 1,
        desc.first_array_layer, desc.array_layer_count, view);
    if (!status.ok()) {
        return status;
    }
    output = store(
        render_targets_,
        ViewObject{view, texture_id.value, desc.format,
                   std::max(1U, texture->width >> desc.mip_level),
                   std::max(1U, texture->height >> desc.mip_level),
                   texture->sample_count, desc.array_layer_count,
                   false});
    status = set_name(
        VK_OBJECT_TYPE_IMAGE_VIEW,
        reinterpret_cast<std::uint64_t>(view), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_depth_target(
    const vita_gpu_depth_target_desc &desc, NativeId texture_id,
    NativeId &output) {
    TextureObject *texture = find(textures_, texture_id);
    if (texture == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE, "unknown Vulkan texture",
            VITA_GPU_BACKEND_VULKAN);
    }
    if (texture->dimension != VITA_GPU_TEXTURE_2D) {
        return unsupported(
            "Vulkan depth target views currently require 2D textures");
    }
    if (has_stencil(texture->format) &&
        desc.read_only_depth != desc.read_only_stencil) {
        return unsupported(
            "Vulkan depth-stencil views require matching depth and stencil read-only states");
    }
    if (!has_stencil(texture->format) &&
        desc.read_only_stencil == VITA_GPU_TRUE) {
        return unsupported(
            "a Vulkan depth-only target has no stencil aspect to make read-only");
    }
    VkImageView view = VK_NULL_HANDLE;
    Status status = create_image_view(
        *texture,
        desc.array_layer_count > 1
            ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
            : VK_IMAGE_VIEW_TYPE_2D,
        to_vk_format(desc.format), desc.mip_level, 1,
        desc.first_array_layer, desc.array_layer_count, view);
    if (!status.ok()) {
        return status;
    }
    output = store(
        depth_targets_,
        ViewObject{view, texture_id.value, desc.format,
                   std::max(1U, texture->width >> desc.mip_level),
                   std::max(1U, texture->height >> desc.mip_level),
                   texture->sample_count,
                   desc.array_layer_count,
                   desc.read_only_depth == VITA_GPU_TRUE});
    status = set_name(
        VK_OBJECT_TYPE_IMAGE_VIEW,
        reinterpret_cast<std::uint64_t>(view), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_shader_module(
    const vita_gpu_shader_module_desc &desc, NativeId &output) {
    if (desc.input != VITA_GPU_SHADER_INPUT_BINARY ||
        desc.binary == nullptr) {
        return unsupported(
            "Vulkan HLSL modules must be compiled to SPIR-V by the pinned DXC contract");
    }
    if (desc.binary->target != VITA_GPU_SHADER_TARGET_SPIRV ||
        desc.binary->data_size < sizeof(std::uint32_t) ||
        (desc.binary->data_size % sizeof(std::uint32_t)) != 0) {
        return unsupported(
            "Vulkan shader module requires aligned SPIR-V binary data");
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, desc.binary->data, sizeof(magic));
    if (magic != UINT32_C(0x07230203)) {
        return Status::error(
            VITA_GPU_ERROR_SHADER_COMPILATION,
            "shader binary is not SPIR-V", VITA_GPU_BACKEND_VULKAN);
    }
    VkShaderModuleCreateInfo info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize =
        static_cast<std::size_t>(desc.binary->data_size);
    std::vector<std::uint32_t> words(
        info.codeSize / sizeof(std::uint32_t));
    std::memcpy(words.data(), desc.binary->data, info.codeSize);
    info.pCode = words.data();
    ShaderObject shader;
    shader.stage = desc.binary->stage;
    const VkResult result =
        context_->dispatch().vkCreateShaderModule(
            device_, &info, nullptr, &shader.module);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkCreateShaderModule failed");
    }
    output = store(shaders_, shader);
    Status status = set_name(
        VK_OBJECT_TYPE_SHADER_MODULE,
        reinterpret_cast<std::uint64_t>(shader.module), desc.label);
    return status.ok() ? checked_validation() : status;
}

VkDescriptorType VulkanDevice::descriptor_type(
    vita_gpu_binding_type type) const {
    switch (type) {
    case VITA_GPU_BINDING_CONSTANT_BUFFER:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case VITA_GPU_BINDING_STORAGE_BUFFER:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case VITA_GPU_BINDING_SAMPLED_TEXTURE:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case VITA_GPU_BINDING_STORAGE_TEXTURE:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case VITA_GPU_BINDING_SAMPLER:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    default:
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

Status VulkanDevice::create_binding_layout(
    const vita_gpu_binding_layout_desc &desc, NativeId &output) {
    if (desc.set_index >=
        properties_.limits.maxBoundDescriptorSets) {
        return unsupported(
            "binding-layout set index exceeds the selected Vulkan device limit");
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(desc.entry_count);
    std::array<std::uint64_t, 5> set_counts{};
    std::array<std::uint64_t, 5> vertex_counts{};
    std::array<std::uint64_t, 5> pixel_counts{};
    std::array<std::uint64_t, 5> compute_counts{};
    for (std::uint32_t index = 0; index < desc.entry_count; ++index) {
        const auto &entry = desc.entries[index];
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = entry.binding;
        binding.descriptorType = descriptor_type(entry.type);
        binding.descriptorCount = entry.array_count;
        binding.stageFlags = to_vk_stages(entry.visibility);
        bindings.push_back(binding);
        const std::size_t type_index =
            static_cast<std::size_t>(entry.type);
        set_counts[type_index] += entry.array_count;
        if ((entry.visibility &
             VITA_GPU_SHADER_STAGE_VERTEX_BIT) != 0) {
            vertex_counts[type_index] += entry.array_count;
        }
        if ((entry.visibility &
             VITA_GPU_SHADER_STAGE_PIXEL_BIT) != 0) {
            pixel_counts[type_index] += entry.array_count;
        }
        if ((entry.visibility &
             VITA_GPU_SHADER_STAGE_COMPUTE_BIT) != 0) {
            compute_counts[type_index] += entry.array_count;
        }
    }
    const auto stage_supported =
        [this](const std::array<std::uint64_t, 5> &counts) {
            const auto &limits = properties_.limits;
            const std::uint64_t resources =
                counts[0] + counts[1] + counts[2] +
                counts[3] + counts[4];
            return counts[0] <=
                       limits.maxPerStageDescriptorUniformBuffers &&
                   counts[1] <=
                       limits.maxPerStageDescriptorStorageBuffers &&
                   counts[2] <=
                       limits.maxPerStageDescriptorSampledImages &&
                   counts[3] <=
                       limits.maxPerStageDescriptorStorageImages &&
                   counts[4] <=
                       limits.maxPerStageDescriptorSamplers &&
                   resources <= limits.maxPerStageResources;
        };
    constexpr std::uint64_t kPoolDescriptorsPerType = 4096;
    const auto &limits = properties_.limits;
    if (set_counts[0] > limits.maxDescriptorSetUniformBuffers ||
        set_counts[1] > limits.maxDescriptorSetStorageBuffers ||
        set_counts[2] > limits.maxDescriptorSetSampledImages ||
        set_counts[3] > limits.maxDescriptorSetStorageImages ||
        set_counts[4] > limits.maxDescriptorSetSamplers ||
        std::any_of(
            set_counts.begin(), set_counts.end(),
            [](std::uint64_t count) {
                return count > kPoolDescriptorsPerType;
            }) ||
        !stage_supported(vertex_counts) ||
        !stage_supported(pixel_counts) ||
        !stage_supported(compute_counts)) {
        return unsupported(
            "binding layout exceeds selected Vulkan device or backend descriptor-pool limits");
    }
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount =
        static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    BindingLayoutObject layout;
    layout.set_index = desc.set_index;
    layout.set_counts = set_counts;
    layout.vertex_counts = vertex_counts;
    layout.pixel_counts = pixel_counts;
    layout.compute_counts = compute_counts;
    const VkResult result =
        context_->dispatch().vkCreateDescriptorSetLayout(
            device_, &info, nullptr, &layout.layout);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkCreateDescriptorSetLayout failed");
    }
    output = store(binding_layouts_, layout);
    Status status = set_name(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        reinterpret_cast<std::uint64_t>(layout.layout), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_binding_set(
    const vita_gpu_binding_set_desc &desc, NativeId layout_id,
    NativeId &output) {
    BindingLayoutObject *layout = find(binding_layouts_, layout_id);
    if (layout == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan binding layout",
            VITA_GPU_BACKEND_VULKAN);
    }
    VkDescriptorSetAllocateInfo allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = descriptor_pool_;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &layout->layout;
    BindingSetObject set;
    set.layout = layout_id.value;
    const VkResult result =
        context_->dispatch().vkAllocateDescriptorSets(
            device_, &allocation, &set.set);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkAllocateDescriptorSets failed");
    }
    output = store(binding_sets_, set);
    Status status = set_name(
        VK_OBJECT_TYPE_DESCRIPTOR_SET,
        reinterpret_cast<std::uint64_t>(set.set), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::update_binding_set(
    NativeId set_id,
    std::span<const BackendBindingWrite> writes) {
    BindingSetObject *set = find(binding_sets_, set_id);
    if (set == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan binding set", VITA_GPU_BACKEND_VULKAN);
    }
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> updates;
    buffers.reserve(writes.size());
    images.reserve(writes.size());
    updates.reserve(writes.size());
    for (const BackendBindingWrite &write : writes) {
        VkWriteDescriptorSet update{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        update.dstSet = set->set;
        update.dstBinding = write.binding;
        update.dstArrayElement = write.array_element;
        update.descriptorCount = 1;
        update.descriptorType = descriptor_type(write.type);
        if (write.type == VITA_GPU_BINDING_CONSTANT_BUFFER ||
            write.type == VITA_GPU_BINDING_STORAGE_BUFFER) {
            BufferObject *buffer = find(buffers_, write.buffer);
            if (buffer == nullptr) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_HANDLE,
                    "unknown Vulkan descriptor buffer",
                    VITA_GPU_BACKEND_VULKAN);
            }
            const bool uniform =
                write.type == VITA_GPU_BINDING_CONSTANT_BUFFER;
            const VkDeviceSize alignment =
                uniform
                    ? properties_.limits
                          .minUniformBufferOffsetAlignment
                    : properties_.limits
                          .minStorageBufferOffsetAlignment;
            const VkDeviceSize maximum =
                uniform
                    ? properties_.limits.maxUniformBufferRange
                    : properties_.limits.maxStorageBufferRange;
            if ((write.buffer_offset % alignment) != 0) {
                return invalid_argument(
                    "Vulkan descriptor buffer offset does not satisfy device alignment");
            }
            if (write.buffer_size > maximum) {
                return unsupported(
                    "Vulkan descriptor buffer range exceeds the selected device limit");
            }
            buffers.push_back(
                {buffer->buffer, write.buffer_offset,
                 write.buffer_size});
            update.pBufferInfo = &buffers.back();
        } else if (write.type ==
                       VITA_GPU_BINDING_SAMPLED_TEXTURE ||
                   write.type ==
                       VITA_GPU_BINDING_STORAGE_TEXTURE) {
            TextureObject *texture =
                find(textures_, write.texture);
            if (texture == nullptr) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_HANDLE,
                    "unknown Vulkan descriptor texture",
                    VITA_GPU_BACKEND_VULKAN);
            }
            VkDescriptorImageInfo image{};
            image.imageView = texture->default_view;
            image.imageLayout = layout_for(
                write.texture_state,
                is_depth_format(texture->format));
            images.push_back(image);
            update.pImageInfo = &images.back();
        } else {
            SamplerObject *sampler =
                find(samplers_, write.sampler);
            if (sampler == nullptr) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_HANDLE,
                    "unknown Vulkan descriptor sampler",
                    VITA_GPU_BACKEND_VULKAN);
            }
            VkDescriptorImageInfo image{};
            image.sampler = sampler->sampler;
            images.push_back(image);
            update.pImageInfo = &images.back();
        }
        updates.push_back(update);
    }
    context_->dispatch().vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(updates.size()),
        updates.data(), 0, nullptr);
    return checked_validation();
}

namespace {

VkBlendFactor to_vk_blend_factor(vita_gpu_blend_factor factor) {
    switch (factor) {
    case VITA_GPU_BLEND_ZERO:
        return VK_BLEND_FACTOR_ZERO;
    case VITA_GPU_BLEND_ONE:
        return VK_BLEND_FACTOR_ONE;
    case VITA_GPU_BLEND_SRC_COLOR:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case VITA_GPU_BLEND_INV_SRC_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case VITA_GPU_BLEND_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case VITA_GPU_BLEND_INV_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VITA_GPU_BLEND_DST_COLOR:
        return VK_BLEND_FACTOR_DST_COLOR;
    case VITA_GPU_BLEND_INV_DST_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case VITA_GPU_BLEND_DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case VITA_GPU_BLEND_INV_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:
        return VK_BLEND_FACTOR_ZERO;
    }
}

VkCullModeFlags to_vk_cull(vita_gpu_cull_mode mode) {
    switch (mode) {
    case VITA_GPU_CULL_FRONT:
        return VK_CULL_MODE_FRONT_BIT;
    case VITA_GPU_CULL_BACK:
        return VK_CULL_MODE_BACK_BIT;
    default:
        return VK_CULL_MODE_NONE;
    }
}

VkAttachmentLoadOp to_vk_load(vita_gpu_load_op operation) {
    switch (operation) {
    case VITA_GPU_LOAD_CLEAR:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case VITA_GPU_LOAD_DISCARD:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    default:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }
}

VkAttachmentStoreOp to_vk_store(vita_gpu_store_op operation) {
    return operation == VITA_GPU_STORE_DISCARD
               ? VK_ATTACHMENT_STORE_OP_DONT_CARE
               : VK_ATTACHMENT_STORE_OP_STORE;
}

} // namespace

Status VulkanDevice::create_graphics_pipeline(
    const BackendGraphicsPipelineDesc &translated,
    NativeId &output) {
    const auto &desc = *translated.source;
    ShaderObject *vertex = find(shaders_, translated.vertex_shader);
    ShaderObject *pixel = find(shaders_, translated.pixel_shader);
    if (vertex == nullptr || pixel == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan graphics shader",
            VITA_GPU_BACKEND_VULKAN);
    }
    if (vertex->stage != VITA_GPU_SHADER_VERTEX ||
        pixel->stage != VITA_GPU_SHADER_PIXEL) {
        return invalid_argument(
            "Vulkan graphics pipeline shader stages are mismatched");
    }
    if (desc.depth_format == VITA_GPU_FORMAT_UNKNOWN &&
        (desc.depth_stencil.depth_test_enable == VITA_GPU_TRUE ||
         desc.depth_stencil.depth_write_enable == VITA_GPU_TRUE)) {
        return invalid_argument(
            "a Vulkan pipeline with depth testing or writes requires a depth format");
    }
    const VkPhysicalDeviceLimits &limits = properties_.limits;
    const VkSampleCountFlagBits samples =
        to_vk_samples(desc.sample_count);
    if (desc.push_constant_bytes > limits.maxPushConstantsSize ||
        desc.color_target_count > limits.maxColorAttachments ||
        translated.binding_layouts.size() >
            limits.maxBoundDescriptorSets ||
        desc.vertex_binding_count > limits.maxVertexInputBindings ||
        desc.vertex_attribute_count >
            limits.maxVertexInputAttributes ||
        (limits.framebufferColorSampleCounts & samples) == 0 ||
        (desc.depth_format != VITA_GPU_FORMAT_UNKNOWN &&
         (limits.framebufferDepthSampleCounts & samples) == 0)) {
        return unsupported(
            "graphics pipeline exceeds selected Vulkan device limits");
    }
    for (std::uint32_t index = 0;
         index < desc.color_target_count; ++index) {
        VkFormatProperties format{};
        context_->dispatch().vkGetPhysicalDeviceFormatProperties(
            physical_, to_vk_format(desc.color_formats[index]),
            &format);
        if ((format.optimalTilingFeatures &
             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
            return unsupported(
                "graphics pipeline color format is unsupported by the selected Vulkan device");
        }
    }
    if (desc.color_target_count > 1 &&
        features_.independentBlend != VK_TRUE) {
        const auto equivalent_blend =
            [](const vita_gpu_blend_attachment_desc &left,
               const vita_gpu_blend_attachment_desc &right) {
                return left.blend_enable == right.blend_enable &&
                       left.source_color == right.source_color &&
                       left.destination_color ==
                           right.destination_color &&
                       left.color_op == right.color_op &&
                       left.source_alpha == right.source_alpha &&
                       left.destination_alpha ==
                           right.destination_alpha &&
                       left.alpha_op == right.alpha_op &&
                       left.color_write_mask ==
                           right.color_write_mask;
            };
        for (std::uint32_t index = 1;
             index < desc.color_target_count; ++index) {
            if (!equivalent_blend(
                    desc.blend[0], desc.blend[index])) {
                return unsupported(
                    "selected Vulkan device does not support independent blend state");
            }
        }
    }
    if (desc.depth_format != VITA_GPU_FORMAT_UNKNOWN) {
        VkFormatProperties format{};
        context_->dispatch().vkGetPhysicalDeviceFormatProperties(
            physical_, to_vk_format(desc.depth_format), &format);
        if ((format.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
            return unsupported(
                "graphics pipeline depth format is unsupported by the selected Vulkan device");
        }
    }
    std::set<std::uint32_t> declared_vertex_bindings;
    for (std::uint32_t index = 0;
         index < desc.vertex_binding_count; ++index) {
        const auto &binding = desc.vertex_bindings[index];
        if (binding.binding >= limits.maxVertexInputBindings ||
            binding.stride > limits.maxVertexInputBindingStride) {
            return unsupported(
                "vertex binding exceeds selected Vulkan device limits");
        }
        if (!declared_vertex_bindings.insert(
                binding.binding).second) {
            return invalid_argument(
                "vertex pipeline bindings must be unique");
        }
    }
    std::set<std::uint32_t> attribute_locations;
    std::set<std::uint32_t> required_vertex_bindings;
    for (std::uint32_t index = 0;
         index < desc.vertex_attribute_count; ++index) {
        const auto &attribute = desc.vertex_attributes[index];
        VkFormatProperties format{};
        context_->dispatch().vkGetPhysicalDeviceFormatProperties(
            physical_, to_vk_format(attribute.format), &format);
        if (attribute.location >= limits.maxVertexInputAttributes ||
            attribute.binding >= limits.maxVertexInputBindings ||
            attribute.offset > limits.maxVertexInputAttributeOffset ||
            (format.bufferFeatures &
             VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0) {
            return unsupported(
                "vertex attribute exceeds selected Vulkan device limits");
        }
        if (!declared_vertex_bindings.contains(
                attribute.binding) ||
            !attribute_locations.insert(
                attribute.location).second) {
            return invalid_argument(
                "vertex attributes must have unique locations and declared bindings");
        }
        required_vertex_bindings.insert(
            attribute.binding);
    }
    std::vector<std::pair<std::uint32_t, VkDescriptorSetLayout>>
        indexed_layouts;
    std::vector<const BindingLayoutObject *> limit_layouts;
    indexed_layouts.reserve(translated.binding_layouts.size());
    limit_layouts.reserve(translated.binding_layouts.size());
    for (NativeId layout_id : translated.binding_layouts) {
        BindingLayoutObject *layout =
            find(binding_layouts_, layout_id);
        if (layout == nullptr) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_HANDLE,
                "unknown Vulkan pipeline binding layout",
                VITA_GPU_BACKEND_VULKAN);
        }
        indexed_layouts.emplace_back(
            layout->set_index, layout->layout);
        limit_layouts.push_back(layout);
    }
    if (!descriptor_pipeline_supported(
            properties_.limits, limit_layouts,
            VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT)) {
        return unsupported(
            "graphics pipeline descriptor layouts exceed selected Vulkan device limits");
    }
    std::sort(
        indexed_layouts.begin(), indexed_layouts.end(),
        [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
    std::vector<VkDescriptorSetLayout> layouts;
    layouts.reserve(indexed_layouts.size());
    for (std::size_t index = 0;
         index < indexed_layouts.size(); ++index) {
        if (indexed_layouts[index].first != index) {
            return unsupported(
                "Vulkan pipeline binding set indices must be contiguous from zero");
        }
        layouts.push_back(indexed_layouts[index].second);
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                      VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = desc.push_constant_bytes;
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount =
        static_cast<std::uint32_t>(layouts.size());
    layout_info.pSetLayouts = layouts.data();
    if (push.size != 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
    }
    PipelineObject pipeline;
    VkResult result =
        context_->dispatch().vkCreatePipelineLayout(
            device_, &layout_info, nullptr, &pipeline.layout);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkCreatePipelineLayout failed");
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex->module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, pixel->module, "main", nullptr},
    }};
    std::vector<VkVertexInputBindingDescription> vertex_bindings;
    vertex_bindings.reserve(desc.vertex_binding_count);
    for (std::uint32_t index = 0;
         index < desc.vertex_binding_count; ++index) {
        const auto &source = desc.vertex_bindings[index];
        vertex_bindings.push_back(
            {source.binding, source.stride,
             source.step == VITA_GPU_VERTEX_PER_INSTANCE
                 ? VK_VERTEX_INPUT_RATE_INSTANCE
                 : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.vertex_attribute_count);
    for (std::uint32_t index = 0;
         index < desc.vertex_attribute_count; ++index) {
        const auto &source = desc.vertex_attributes[index];
        attributes.push_back(
            {source.location, source.binding,
             to_vk_format(source.format), source.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount =
        static_cast<std::uint32_t>(vertex_bindings.size());
    vertex_input.pVertexBindingDescriptions =
        vertex_bindings.data();
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology =
        static_cast<VkPrimitiveTopology>(
            static_cast<int>(desc.topology));

    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    if (desc.rasterizer.polygon_mode == VITA_GPU_POLYGON_LINE &&
        features_.fillModeNonSolid != VK_TRUE) {
        context_->dispatch().vkDestroyPipelineLayout(
            device_, pipeline.layout, nullptr);
        return unsupported(
            "selected Vulkan device does not support wireframe pipelines");
    }
    if (desc.rasterizer.depth_bias_clamp != 0.0F &&
        features_.depthBiasClamp != VK_TRUE) {
        context_->dispatch().vkDestroyPipelineLayout(
            device_, pipeline.layout, nullptr);
        return unsupported(
            "selected Vulkan device does not support depth-bias clamp");
    }
    VkPipelineRasterizationStateCreateInfo rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode =
        desc.rasterizer.polygon_mode == VITA_GPU_POLYGON_LINE
            ? VK_POLYGON_MODE_LINE
            : VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = to_vk_cull(desc.rasterizer.cull_mode);
    rasterizer.frontFace =
        desc.rasterizer.front_face ==
                VITA_GPU_FRONT_COUNTER_CLOCKWISE
            ? VK_FRONT_FACE_COUNTER_CLOCKWISE
            : VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable =
        desc.rasterizer.depth_bias != 0 ||
                desc.rasterizer.depth_bias_clamp != 0.0F ||
                desc.rasterizer.slope_scaled_depth_bias != 0.0F
            ? VK_TRUE
            : VK_FALSE;
    rasterizer.depthBiasConstantFactor =
        static_cast<float>(desc.rasterizer.depth_bias);
    rasterizer.depthBiasClamp =
        desc.rasterizer.depth_bias_clamp;
    rasterizer.depthBiasSlopeFactor =
        desc.rasterizer.slope_scaled_depth_bias;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;

    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable =
        desc.depth_stencil.depth_test_enable == VITA_GPU_TRUE
            ? VK_TRUE
            : VK_FALSE;
    depth.depthWriteEnable =
        desc.depth_stencil.depth_write_enable == VITA_GPU_TRUE
            ? VK_TRUE
            : VK_FALSE;
    depth.depthCompareOp =
        to_vk_compare(desc.depth_stencil.depth_compare);
    if (desc.depth_stencil.stencil_enable == VITA_GPU_TRUE) {
        context_->dispatch().vkDestroyPipelineLayout(
            device_, pipeline.layout, nullptr);
        return unsupported(
            "Vulkan stencil operations are not represented by the version 1 GPU contract");
    }

    std::vector<VkPipelineColorBlendAttachmentState> blend;
    blend.reserve(desc.color_target_count);
    for (std::uint32_t index = 0;
         index < desc.color_target_count; ++index) {
        const auto &source = desc.blend[index];
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable =
            source.blend_enable == VITA_GPU_TRUE ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor =
            to_vk_blend_factor(source.source_color);
        attachment.dstColorBlendFactor =
            to_vk_blend_factor(source.destination_color);
        attachment.colorBlendOp =
            static_cast<VkBlendOp>(source.color_op);
        attachment.srcAlphaBlendFactor =
            to_vk_blend_factor(source.source_alpha);
        attachment.dstAlphaBlendFactor =
            to_vk_blend_factor(source.destination_alpha);
        attachment.alphaBlendOp =
            static_cast<VkBlendOp>(source.alpha_op);
        attachment.colorWriteMask =
            static_cast<VkColorComponentFlags>(
                source.color_write_mask);
        blend.push_back(attachment);
    }
    VkPipelineColorBlendStateCreateInfo blend_state{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount =
        static_cast<std::uint32_t>(blend.size());
    blend_state.pAttachments = blend.data();

    const std::array<VkDynamicState, 2> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount =
        static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    std::array<VkFormat, VITA_GPU_MAX_COLOR_TARGETS> color_formats{};
    for (std::uint32_t index = 0;
         index < desc.color_target_count; ++index) {
        color_formats[index] =
            to_vk_format(desc.color_formats[index]);
    }
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = desc.color_target_count;
    rendering.pColorAttachmentFormats = color_formats.data();
    if (desc.depth_format != VITA_GPU_FORMAT_UNKNOWN) {
        rendering.depthAttachmentFormat =
            to_vk_format(desc.depth_format);
        if (has_stencil(desc.depth_format)) {
            rendering.stencilAttachmentFormat =
                rendering.depthAttachmentFormat;
        }
    }

    VkGraphicsPipelineCreateInfo info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = pipeline.layout;
    pipeline.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    result = context_->dispatch().vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &info, nullptr,
        &pipeline.pipeline);
    if (result != VK_SUCCESS) {
        context_->dispatch().vkDestroyPipelineLayout(
            device_, pipeline.layout, nullptr);
        return vulkan_error(
            result, "vkCreateGraphicsPipelines failed");
    }
    pipeline.color_formats.assign(
        desc.color_formats,
        desc.color_formats + desc.color_target_count);
    pipeline.depth_format = desc.depth_format;
    pipeline.sample_count = desc.sample_count;
    pipeline.depth_write =
        desc.depth_stencil.depth_write_enable == VITA_GPU_TRUE;
    pipeline.required_vertex_bindings =
        std::move(required_vertex_bindings);
    output = store(pipelines_, pipeline);
    Status status = set_name(
        VK_OBJECT_TYPE_PIPELINE,
        reinterpret_cast<std::uint64_t>(pipeline.pipeline), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_compute_pipeline(
    const BackendComputePipelineDesc &translated,
    NativeId &output) {
    const auto &desc = *translated.source;
    ShaderObject *shader = find(shaders_, translated.compute_shader);
    if (shader == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan compute shader",
            VITA_GPU_BACKEND_VULKAN);
    }
    if (shader->stage != VITA_GPU_SHADER_COMPUTE) {
        return invalid_argument(
            "Vulkan compute pipeline requires a compute shader");
    }
    if (desc.push_constant_bytes >
            properties_.limits.maxPushConstantsSize ||
        translated.binding_layouts.size() >
            properties_.limits.maxBoundDescriptorSets) {
        return unsupported(
            "compute pipeline exceeds selected Vulkan device limits");
    }
    std::vector<std::pair<std::uint32_t, VkDescriptorSetLayout>>
        indexed_layouts;
    std::vector<const BindingLayoutObject *> limit_layouts;
    limit_layouts.reserve(translated.binding_layouts.size());
    for (NativeId layout_id : translated.binding_layouts) {
        BindingLayoutObject *layout =
            find(binding_layouts_, layout_id);
        if (layout == nullptr) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_HANDLE,
                "unknown Vulkan pipeline binding layout",
                VITA_GPU_BACKEND_VULKAN);
        }
        indexed_layouts.emplace_back(
            layout->set_index, layout->layout);
        limit_layouts.push_back(layout);
    }
    if (!descriptor_pipeline_supported(
            properties_.limits, limit_layouts,
            VK_SHADER_STAGE_COMPUTE_BIT)) {
        return unsupported(
            "compute pipeline descriptor layouts exceed selected Vulkan device limits");
    }
    std::sort(
        indexed_layouts.begin(), indexed_layouts.end(),
        [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
    std::vector<VkDescriptorSetLayout> layouts;
    for (std::size_t index = 0;
         index < indexed_layouts.size(); ++index) {
        if (indexed_layouts[index].first != index) {
            return unsupported(
                "Vulkan pipeline binding set indices must be contiguous from zero");
        }
        layouts.push_back(indexed_layouts[index].second);
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = desc.push_constant_bytes;
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount =
        static_cast<std::uint32_t>(layouts.size());
    layout_info.pSetLayouts = layouts.data();
    if (push.size != 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
    }
    PipelineObject pipeline;
    VkResult result =
        context_->dispatch().vkCreatePipelineLayout(
            device_, &layout_info, nullptr, &pipeline.layout);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkCreatePipelineLayout failed");
    }
    VkComputePipelineCreateInfo info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader->module;
    info.stage.pName = "main";
    info.layout = pipeline.layout;
    pipeline.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    result = context_->dispatch().vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &info, nullptr,
        &pipeline.pipeline);
    if (result != VK_SUCCESS) {
        context_->dispatch().vkDestroyPipelineLayout(
            device_, pipeline.layout, nullptr);
        return vulkan_error(
            result, "vkCreateComputePipelines failed");
    }
    output = store(pipelines_, pipeline);
    Status status = set_name(
        VK_OBJECT_TYPE_PIPELINE,
        reinterpret_cast<std::uint64_t>(pipeline.pipeline), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_readback(
    const vita_gpu_readback_desc &desc, NativeId &output) {
    BufferObject allocation;
    Status status = create_buffer_allocation(
        desc.byte_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        allocation);
    if (!status.ok()) {
        return status;
    }
    ReadbackObject readback;
    readback.buffer = allocation.buffer;
    readback.memory = allocation.memory;
    readback.size = allocation.size;
    readback.memory_properties = allocation.memory_properties;
    output = store(readbacks_, readback);
    status = set_name(
        VK_OBJECT_TYPE_BUFFER,
        reinterpret_cast<std::uint64_t>(readback.buffer), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::create_fence(
    const vita_gpu_fence_desc &desc, NativeId &output) {
    VkSemaphoreTypeCreateInfo timeline{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline.initialValue = desc.initial_value;
    VkSemaphoreCreateInfo info{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &timeline;
    FenceObject fence;
    const VkResult result =
        context_->dispatch().vkCreateSemaphore(
            device_, &info, nullptr, &fence.semaphore);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "creating Vulkan timeline semaphore failed");
    }
    output = store(fences_, fence);
    Status status = set_name(
        VK_OBJECT_TYPE_SEMAPHORE,
        reinterpret_cast<std::uint64_t>(fence.semaphore), desc.label);
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::destroy_object(ObjectKind kind, NativeId object) {
    auto &vk = context_->dispatch();
    switch (kind) {
    case ObjectKind::swapchain: {
        const auto found = swapchains_.find(object.value);
        if (found == swapchains_.end()) {
            break;
        }
        const VkResult idle = vk.vkDeviceWaitIdle(device_);
        if (idle != VK_SUCCESS) {
            return vulkan_error(
                idle,
                "waiting before Vulkan swapchain destruction failed");
        }
        destroy_swapchain_native(found->second, true);
        swapchains_.erase(found);
        return checked_validation();
    }
    case ObjectKind::buffer: {
        const auto found = buffers_.find(object.value);
        if (found == buffers_.end()) {
            break;
        }
        vk.vkDestroyBuffer(device_, found->second.buffer, nullptr);
        vk.vkFreeMemory(device_, found->second.memory, nullptr);
        buffers_.erase(found);
        return checked_validation();
    }
    case ObjectKind::texture: {
        const auto found = textures_.find(object.value);
        if (found == textures_.end()) {
            break;
        }
        if (!found->second.owned) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_STATE,
                "swapchain textures are owned by their swapchain",
                VITA_GPU_BACKEND_VULKAN);
        }
        vk.vkDestroyImageView(
            device_, found->second.default_view, nullptr);
        vk.vkDestroyImage(device_, found->second.image, nullptr);
        vk.vkFreeMemory(device_, found->second.memory, nullptr);
        textures_.erase(found);
        return checked_validation();
    }
    case ObjectKind::sampler: {
        const auto found = samplers_.find(object.value);
        if (found == samplers_.end()) {
            break;
        }
        vk.vkDestroySampler(device_, found->second.sampler, nullptr);
        samplers_.erase(found);
        return checked_validation();
    }
    case ObjectKind::render_target: {
        const auto found = render_targets_.find(object.value);
        if (found == render_targets_.end()) {
            break;
        }
        vk.vkDestroyImageView(device_, found->second.view, nullptr);
        render_targets_.erase(found);
        return checked_validation();
    }
    case ObjectKind::depth_target: {
        const auto found = depth_targets_.find(object.value);
        if (found == depth_targets_.end()) {
            break;
        }
        vk.vkDestroyImageView(device_, found->second.view, nullptr);
        depth_targets_.erase(found);
        return checked_validation();
    }
    case ObjectKind::shader: {
        const auto found = shaders_.find(object.value);
        if (found == shaders_.end()) {
            break;
        }
        vk.vkDestroyShaderModule(device_, found->second.module, nullptr);
        shaders_.erase(found);
        return checked_validation();
    }
    case ObjectKind::binding_layout: {
        const auto found = binding_layouts_.find(object.value);
        if (found == binding_layouts_.end()) {
            break;
        }
        vk.vkDestroyDescriptorSetLayout(
            device_, found->second.layout, nullptr);
        binding_layouts_.erase(found);
        return checked_validation();
    }
    case ObjectKind::binding_set: {
        const auto found = binding_sets_.find(object.value);
        if (found == binding_sets_.end()) {
            break;
        }
        const VkResult result = vk.vkFreeDescriptorSets(
            device_, descriptor_pool_, 1, &found->second.set);
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result, "vkFreeDescriptorSets failed");
        }
        binding_sets_.erase(found);
        return checked_validation();
    }
    case ObjectKind::pipeline: {
        const auto found = pipelines_.find(object.value);
        if (found == pipelines_.end()) {
            break;
        }
        vk.vkDestroyPipeline(
            device_, found->second.pipeline, nullptr);
        vk.vkDestroyPipelineLayout(
            device_, found->second.layout, nullptr);
        pipelines_.erase(found);
        return checked_validation();
    }
    case ObjectKind::fence: {
        const auto found = fences_.find(object.value);
        if (found == fences_.end()) {
            break;
        }
        vk.vkDestroySemaphore(
            device_, found->second.semaphore, nullptr);
        fences_.erase(found);
        return checked_validation();
    }
    case ObjectKind::readback: {
        const auto found = readbacks_.find(object.value);
        if (found == readbacks_.end()) {
            break;
        }
        if (found->second.mapped != nullptr) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_STATE,
                "Vulkan readback is still mapped",
                VITA_GPU_BACKEND_VULKAN);
        }
        vk.vkDestroyBuffer(
            device_, found->second.buffer, nullptr);
        vk.vkFreeMemory(device_, found->second.memory, nullptr);
        readbacks_.erase(found);
        return checked_validation();
    }
    }
    return Status::error(
        VITA_GPU_ERROR_INVALID_HANDLE,
        "unknown Vulkan backend object",
        VITA_GPU_BACKEND_VULKAN);
}

Status VulkanDevice::fence_completed_value(
    NativeId id, std::uint64_t &value) {
    FenceObject *fence = find(fences_, id);
    if (fence == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan timeline fence",
            VITA_GPU_BACKEND_VULKAN);
    }
    const VkResult result =
        context_->dispatch().vkGetSemaphoreCounterValue(
            device_, fence->semaphore, &value);
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkGetSemaphoreCounterValue failed");
    }
    Status status = cleanup_submissions();
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::fence_wait(
    NativeId id, std::uint64_t value,
    std::uint64_t timeout_nanoseconds) {
    FenceObject *fence = find(fences_, id);
    if (fence == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan timeline fence",
            VITA_GPU_BACKEND_VULKAN);
    }
    VkSemaphoreWaitInfo info{
        VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &fence->semaphore;
    info.pValues = &value;
    const VkResult result =
        context_->dispatch().vkWaitSemaphores(
            device_, &info, timeout_nanoseconds);
    if (result != VK_SUCCESS) {
        return vulkan_error(result, "vkWaitSemaphores failed");
    }
    Status status = cleanup_submissions();
    return status.ok() ? checked_validation() : status;
}

Status VulkanDevice::map_readback(
    NativeId id, BackendReadbackMap &output) {
    ReadbackObject *readback = find(readbacks_, id);
    if (readback == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan readback", VITA_GPU_BACKEND_VULKAN);
    }
    VkResult result = context_->dispatch().vkMapMemory(
        device_, readback->memory, 0, readback->size, 0,
        &readback->mapped);
    if (result != VK_SUCCESS) {
        readback->mapped = nullptr;
        return vulkan_error(result, "vkMapMemory for readback failed");
    }
    if ((readback->memory_properties &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        VkMappedMemoryRange range{
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = readback->memory;
        range.size = VK_WHOLE_SIZE;
        result = context_->dispatch().vkInvalidateMappedMemoryRanges(
            device_, 1, &range);
        if (result != VK_SUCCESS) {
            context_->dispatch().vkUnmapMemory(
                device_, readback->memory);
            readback->mapped = nullptr;
            return vulkan_error(
                result, "invalidating Vulkan readback memory failed");
        }
    }
    output.data =
        static_cast<const std::uint8_t *>(readback->mapped);
    output.byte_size = readback->size;
    output.row_pitch = readback->row_pitch;
    output.slice_pitch = readback->slice_pitch;
    return checked_validation();
}

Status VulkanDevice::unmap_readback(NativeId id) {
    ReadbackObject *readback = find(readbacks_, id);
    if (readback == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "unknown Vulkan readback", VITA_GPU_BACKEND_VULKAN);
    }
    context_->dispatch().vkUnmapMemory(device_, readback->memory);
    readback->mapped = nullptr;
    return checked_validation();
}

Status VulkanDevice::encode_command_list(
    const RecordedCommandList &recorded, VkCommandBuffer command,
    std::set<std::uint64_t> &initialized_swapchain_textures,
    std::vector<ReadbackLayoutUpdate> &readback_updates) {
    auto &vk = context_->dispatch();
    PipelineObject *bound_pipeline = nullptr;
    bool rendering_active = false;
    std::vector<vita_gpu_format> active_color_formats;
    vita_gpu_format active_depth_format =
        VITA_GPU_FORMAT_UNKNOWN;
    std::uint32_t active_sample_count = 1;
    bool active_depth_read_only = false;
    bool viewport_set = false;
    bool scissor_set = false;
    bool index_buffer_set = false;
    std::set<std::uint32_t> bound_vertex_buffers;
    for (const CommandPayload &payload : recorded.commands) {
        Status status = std::visit(
            [&](const auto &item) -> Status {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, BarrierCommand>) {
                    const AccessState source = access_for(item.before);
                    const AccessState destination =
                        access_for(item.after);
                    VkDependencyInfo dependency{
                        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    if (item.kind == VITA_GPU_RESOURCE_BUFFER) {
                        BufferObject *buffer =
                            find(buffers_, item.resource);
                        if (buffer == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "barrier references an unknown Vulkan buffer",
                                VITA_GPU_BACKEND_VULKAN);
                        }
                        VkBufferMemoryBarrier2 barrier{
                            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                        barrier.srcStageMask = source.stages;
                        barrier.srcAccessMask = source.access;
                        barrier.dstStageMask = destination.stages;
                        barrier.dstAccessMask = destination.access;
                        barrier.srcQueueFamilyIndex =
                            VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex =
                            VK_QUEUE_FAMILY_IGNORED;
                        barrier.buffer = buffer->buffer;
                        barrier.size = VK_WHOLE_SIZE;
                        dependency.bufferMemoryBarrierCount = 1;
                        dependency.pBufferMemoryBarriers = &barrier;
                        vk.vkCmdPipelineBarrier2(command, &dependency);
                    } else {
                        TextureObject *texture =
                            find(textures_, item.resource);
                        if (texture == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "barrier references an unknown Vulkan texture",
                                VITA_GPU_BACKEND_VULKAN);
                        }
                        const auto combines_present =
                            [](vita_gpu_resource_state state) {
                                const vita_gpu_resource_state effective =
                                    state &
                                    ~VITA_GPU_RESOURCE_STATE_COMMON;
                                return (effective &
                                        VITA_GPU_RESOURCE_STATE_PRESENT) !=
                                           0 &&
                                       effective !=
                                           VITA_GPU_RESOURCE_STATE_PRESENT;
                            };
                        if (combines_present(item.before) ||
                            combines_present(item.after)) {
                            return unsupported(
                                "Vulkan PRESENT state cannot be combined with another image state");
                        }
                        if (texture->dimension ==
                                VITA_GPU_TEXTURE_3D &&
                            (item.first_array_layer != 0 ||
                             item.array_layer_count != 1)) {
                            return unsupported(
                                "Vulkan 3D image barriers address the single 3D array layer");
                        }
                        VkImageMemoryBarrier2 barrier{
                            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        const bool first_swapchain_use =
                            !texture->owned &&
                            !texture->layout_initialized &&
                            !initialized_swapchain_textures.contains(
                                item.resource.value) &&
                            item.before ==
                                VITA_GPU_RESOURCE_STATE_PRESENT;
                        barrier.srcStageMask =
                            first_swapchain_use
                                ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                : source.stages;
                        barrier.srcAccessMask =
                            first_swapchain_use ? 0 : source.access;
                        barrier.dstStageMask = destination.stages;
                        barrier.dstAccessMask = destination.access;
                        barrier.oldLayout =
                            first_swapchain_use
                                ? VK_IMAGE_LAYOUT_UNDEFINED
                                : layout_for(
                                      item.before,
                                      is_depth_format(texture->format));
                        barrier.newLayout = layout_for(
                            item.after,
                            is_depth_format(texture->format));
                        barrier.srcQueueFamilyIndex =
                            VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex =
                            VK_QUEUE_FAMILY_IGNORED;
                        barrier.image = texture->image;
                        barrier.subresourceRange.aspectMask =
                            texture->aspect;
                        barrier.subresourceRange.baseMipLevel =
                            item.first_mip;
                        barrier.subresourceRange.levelCount =
                            item.mip_count;
                        barrier.subresourceRange.baseArrayLayer =
                            item.first_array_layer;
                        barrier.subresourceRange.layerCount =
                            item.array_layer_count;
                        dependency.imageMemoryBarrierCount = 1;
                        dependency.pImageMemoryBarriers = &barrier;
                        vk.vkCmdPipelineBarrier2(command, &dependency);
                        if (!texture->owned) {
                            initialized_swapchain_textures.insert(
                                item.resource.value);
                        }
                    }
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, BeginRenderPassCommand>) {
                    std::vector<VkRenderingAttachmentInfo> colors;
                    colors.reserve(item.colors.size());
                    std::uint32_t width = 0;
                    std::uint32_t height = 0;
                    std::uint32_t layers = 0;
                    std::uint32_t samples = 0;
                    active_color_formats.clear();
                    active_depth_format =
                        VITA_GPU_FORMAT_UNKNOWN;
                    active_depth_read_only = false;
                    viewport_set = false;
                    scissor_set = false;
                    index_buffer_set = false;
                    bound_vertex_buffers.clear();
                    for (const BackendColorAttachment &source :
                         item.colors) {
                        ViewObject *target =
                            find(render_targets_, source.target);
                        if (target == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "render pass references an unknown Vulkan target",
                                VITA_GPU_BACKEND_VULKAN);
                        }
                        TextureObject *texture =
                            find(textures_, NativeId{target->texture});
                        if (texture == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "render target references an unknown Vulkan texture",
                                VITA_GPU_BACKEND_VULKAN);
                        }
                        width = width == 0
                                    ? target->width
                                    : std::min(width, target->width);
                        height = height == 0
                                     ? target->height
                                     : std::min(height, target->height);
                        layers = layers == 0
                                     ? target->layers
                                     : std::min(layers, target->layers);
                        if (samples != 0 &&
                            samples != target->sample_count) {
                            return invalid_argument(
                                "Vulkan rendering attachments have mismatched sample counts");
                        }
                        samples = target->sample_count;
                        active_color_formats.push_back(
                            target->format);
                        VkRenderingAttachmentInfo attachment{
                            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                        attachment.imageView = target->view;
                        attachment.imageLayout = layout_for(
                            source.state, false);
                        attachment.loadOp = to_vk_load(source.load_op);
                        attachment.storeOp = to_vk_store(source.store_op);
                        std::copy(
                            source.clear_color.begin(),
                            source.clear_color.end(),
                            attachment.clearValue.color.float32);
                        colors.push_back(attachment);
                    }
                    VkRenderingAttachmentInfo depth{
                        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    VkRenderingAttachmentInfo stencil{
                        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    bool depth_has_stencil = false;
                    if (item.depth.has_value()) {
                        ViewObject *target =
                            find(depth_targets_,
                                 item.depth->target);
                        if (target == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "render pass references an unknown Vulkan depth target",
                                VITA_GPU_BACKEND_VULKAN);
                        }
                        TextureObject *texture =
                            find(textures_, NativeId{target->texture});
                        if (texture == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "depth target references an unknown Vulkan texture",
                                VITA_GPU_BACKEND_VULKAN);
                        }
                        if (target->read_only &&
                            (item.depth->depth_load_op ==
                                 VITA_GPU_LOAD_CLEAR ||
                             item.depth->stencil_load_op ==
                                 VITA_GPU_LOAD_CLEAR)) {
                            return invalid_argument(
                                "a read-only Vulkan depth target cannot be cleared");
                        }
                        width = width == 0
                                    ? target->width
                                    : std::min(width, target->width);
                        height = height == 0
                                     ? target->height
                                     : std::min(height, target->height);
                        layers = layers == 0
                                     ? target->layers
                                     : std::min(layers, target->layers);
                        depth_has_stencil =
                            has_stencil(target->format);
                        if (samples != 0 &&
                            samples != target->sample_count) {
                            return invalid_argument(
                                "Vulkan rendering attachments have mismatched sample counts");
                        }
                        samples = target->sample_count;
                        active_depth_format = target->format;
                        active_depth_read_only =
                            target->read_only;
                        depth.imageView = target->view;
                        depth.imageLayout = layout_for(
                            item.depth->state, true);
                        depth.loadOp =
                            to_vk_load(item.depth->depth_load_op);
                        depth.storeOp =
                            to_vk_store(item.depth->depth_store_op);
                        depth.clearValue.depthStencil.depth =
                            item.depth->clear_depth;
                        depth.clearValue.depthStencil.stencil =
                            item.depth->clear_stencil;
                        stencil = depth;
                        stencil.loadOp =
                            to_vk_load(item.depth->stencil_load_op);
                        stencil.storeOp =
                            to_vk_store(item.depth->stencil_store_op);
                    }
                    VkRenderingInfo rendering{
                        VK_STRUCTURE_TYPE_RENDERING_INFO};
                    rendering.renderArea.extent = {width, height};
                    rendering.layerCount = std::max(1U, layers);
                    rendering.colorAttachmentCount =
                        static_cast<std::uint32_t>(colors.size());
                    rendering.pColorAttachments = colors.data();
                    if (item.depth.has_value()) {
                        rendering.pDepthAttachment = &depth;
                        if (depth_has_stencil) {
                            rendering.pStencilAttachment = &stencil;
                        }
                    }
                    vk.vkCmdBeginRendering(command, &rendering);
                    rendering_active = true;
                    active_sample_count = samples;
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, EndRenderPassCommand>) {
                    vk.vkCmdEndRendering(command);
                    rendering_active = false;
                    bound_pipeline = nullptr;
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, BindPipelineCommand>) {
                    bound_pipeline =
                        find(pipelines_, item.pipeline);
                    if (bound_pipeline == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "command references an unknown Vulkan pipeline",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    if (bound_pipeline->bind_point ==
                        VK_PIPELINE_BIND_POINT_GRAPHICS) {
                        if (!rendering_active ||
                            bound_pipeline->color_formats !=
                                active_color_formats ||
                            bound_pipeline->depth_format !=
                                active_depth_format ||
                            bound_pipeline->sample_count !=
                                active_sample_count ||
                            (active_depth_read_only &&
                             bound_pipeline->depth_write)) {
                            return invalid_argument(
                                "Vulkan graphics pipeline formats, samples, or depth writes do not match the active attachments");
                        }
                    } else if (rendering_active) {
                        return invalid_argument(
                            "a Vulkan compute pipeline cannot be bound during rendering");
                    }
                    vk.vkCmdBindPipeline(
                        command, bound_pipeline->bind_point,
                        bound_pipeline->pipeline);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, BindSetCommand>) {
                    if (bound_pipeline == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_STATE,
                            "descriptor set requires a bound Vulkan pipeline",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    BindingSetObject *set =
                        find(binding_sets_, item.set);
                    if (set == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "command references an unknown Vulkan binding set",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    vk.vkCmdBindDescriptorSets(
                        command, bound_pipeline->bind_point,
                        bound_pipeline->layout, item.set_index, 1,
                        &set->set, 0, nullptr);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, ViewportCommand>) {
                    const auto &limits = properties_.limits;
                    const float maximum_width =
                        static_cast<float>(
                            limits.maxViewportDimensions[0]);
                    const float maximum_height =
                        static_cast<float>(
                            limits.maxViewportDimensions[1]);
                    if (item.viewport.width > maximum_width ||
                        item.viewport.height > maximum_height ||
                        item.viewport.x <
                            limits.viewportBoundsRange[0] ||
                        item.viewport.y <
                            limits.viewportBoundsRange[0] ||
                        item.viewport.x + item.viewport.width >
                            limits.viewportBoundsRange[1] ||
                        item.viewport.y + item.viewport.height >
                            limits.viewportBoundsRange[1]) {
                        return unsupported(
                            "viewport exceeds selected Vulkan device limits");
                    }
                    VkViewport viewport{};
                    viewport.x = item.viewport.x;
                    viewport.y = item.viewport.y;
                    viewport.width = item.viewport.width;
                    viewport.height = item.viewport.height;
                    viewport.minDepth = item.viewport.min_depth;
                    viewport.maxDepth = item.viewport.max_depth;
                    vk.vkCmdSetViewport(command, 0, 1, &viewport);
                    viewport_set = true;
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, ScissorCommand>) {
                    if (item.scissor.x < 0 || item.scissor.y < 0 ||
                        static_cast<std::uint64_t>(item.scissor.x) +
                                item.scissor.width >
                            properties_.limits.maxFramebufferWidth ||
                        static_cast<std::uint64_t>(item.scissor.y) +
                                item.scissor.height >
                            properties_.limits.maxFramebufferHeight) {
                        return unsupported(
                            "scissor exceeds selected Vulkan device limits");
                    }
                    VkRect2D scissor{};
                    scissor.offset = {
                        item.scissor.x, item.scissor.y};
                    scissor.extent = {
                        item.scissor.width, item.scissor.height};
                    vk.vkCmdSetScissor(command, 0, 1, &scissor);
                    scissor_set = true;
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, VertexBufferCommand>) {
                    BufferObject *buffer =
                        find(buffers_, item.buffer);
                    if (buffer == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "command references an unknown Vulkan vertex buffer",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    if (item.binding >=
                        properties_.limits.maxVertexInputBindings) {
                        return unsupported(
                            "vertex-buffer binding exceeds selected Vulkan device limits");
                    }
                    const VkDeviceSize offset = item.offset;
                    vk.vkCmdBindVertexBuffers(
                        command, item.binding, 1, &buffer->buffer,
                        &offset);
                    bound_vertex_buffers.insert(item.binding);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, IndexBufferCommand>) {
                    BufferObject *buffer =
                        find(buffers_, item.buffer);
                    if (buffer == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "command references an unknown Vulkan index buffer",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    vk.vkCmdBindIndexBuffer(
                        command, buffer->buffer, item.offset,
                        item.format == VITA_GPU_INDEX_UINT32
                            ? VK_INDEX_TYPE_UINT32
                            : VK_INDEX_TYPE_UINT16);
                    index_buffer_set = true;
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, PushConstantsCommand>) {
                    if (bound_pipeline == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_STATE,
                            "push constants require a bound Vulkan pipeline",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    const vita_gpu_shader_stage_flags allowed =
                        bound_pipeline->bind_point ==
                                VK_PIPELINE_BIND_POINT_GRAPHICS
                            ? VITA_GPU_SHADER_STAGE_VERTEX_BIT |
                                  VITA_GPU_SHADER_STAGE_PIXEL_BIT
                            : VITA_GPU_SHADER_STAGE_COMPUTE_BIT;
                    if ((item.stages & ~allowed) != 0) {
                        return invalid_argument(
                            "push-constant stages do not match the bound Vulkan pipeline");
                    }
                    vk.vkCmdPushConstants(
                        command, bound_pipeline->layout,
                        to_vk_stages(item.stages), item.offset,
                        static_cast<std::uint32_t>(item.data.size()),
                        item.data.data());
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, DrawCommand>) {
                    if (bound_pipeline == nullptr ||
                        bound_pipeline->bind_point !=
                            VK_PIPELINE_BIND_POINT_GRAPHICS ||
                        !viewport_set || !scissor_set ||
                        !std::includes(
                            bound_vertex_buffers.begin(),
                            bound_vertex_buffers.end(),
                            bound_pipeline
                                ->required_vertex_bindings.begin(),
                            bound_pipeline
                                ->required_vertex_bindings.end())) {
                        return invalid_argument(
                            "Vulkan draw is missing graphics pipeline, dynamic state, or vertex buffers");
                    }
                    vk.vkCmdDraw(
                        command, item.draw.vertex_count,
                        item.draw.instance_count,
                        item.draw.first_vertex,
                        item.draw.first_instance);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, DrawIndexedCommand>) {
                    if (bound_pipeline == nullptr ||
                        bound_pipeline->bind_point !=
                            VK_PIPELINE_BIND_POINT_GRAPHICS ||
                        !viewport_set || !scissor_set ||
                        !index_buffer_set ||
                        !std::includes(
                            bound_vertex_buffers.begin(),
                            bound_vertex_buffers.end(),
                            bound_pipeline
                                ->required_vertex_bindings.begin(),
                            bound_pipeline
                                ->required_vertex_bindings.end())) {
                        return invalid_argument(
                            "Vulkan indexed draw is missing pipeline, dynamic state, or buffers");
                    }
                    vk.vkCmdDrawIndexed(
                        command, item.draw.index_count,
                        item.draw.instance_count,
                        item.draw.first_index,
                        item.draw.vertex_offset,
                        item.draw.first_instance);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, DispatchCommand>) {
                    if (bound_pipeline == nullptr ||
                        bound_pipeline->bind_point !=
                            VK_PIPELINE_BIND_POINT_COMPUTE) {
                        return invalid_argument(
                            "Vulkan dispatch requires a compute pipeline");
                    }
                    if (item.dispatch.group_count_x >
                            properties_.limits
                                .maxComputeWorkGroupCount[0] ||
                        item.dispatch.group_count_y >
                            properties_.limits
                                .maxComputeWorkGroupCount[1] ||
                        item.dispatch.group_count_z >
                            properties_.limits
                                .maxComputeWorkGroupCount[2]) {
                        return unsupported(
                            "dispatch dimensions exceed selected Vulkan device limits");
                    }
                    vk.vkCmdDispatch(
                        command, item.dispatch.group_count_x,
                        item.dispatch.group_count_y,
                        item.dispatch.group_count_z);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, CopyBufferCommand>) {
                    BufferObject *source =
                        find(buffers_, item.source);
                    BufferObject *destination =
                        find(buffers_, item.destination);
                    if (source == nullptr || destination == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "copy references an unknown Vulkan buffer",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    if ((item.source_offset % 4U) != 0 ||
                        (item.destination_offset % 4U) != 0) {
                        return unsupported(
                            "Vulkan buffer copies require four-byte-aligned offsets");
                    }
                    VkBufferCopy copy{
                        item.source_offset,
                        item.destination_offset,
                        item.byte_size};
                    vk.vkCmdCopyBuffer(
                        command, source->buffer,
                        destination->buffer, 1, &copy);
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T,
                                   CopyBufferReadbackCommand>) {
                    BufferObject *source =
                        find(buffers_, item.source);
                    ReadbackObject *destination =
                        find(readbacks_, item.destination);
                    if (source == nullptr || destination == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "readback copy references an unknown Vulkan object",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    if ((item.source_offset % 4U) != 0 ||
                        (item.destination_offset % 4U) != 0) {
                        return unsupported(
                            "Vulkan buffer readback copies require four-byte-aligned offsets");
                    }
                    VkBufferCopy copy{
                        item.source_offset,
                        item.destination_offset,
                        item.byte_size};
                    vk.vkCmdCopyBuffer(
                        command, source->buffer,
                        destination->buffer, 1, &copy);
                    readback_updates.push_back(
                        {item.destination, 0, 0});
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T,
                                   CopyTextureReadbackCommand>) {
                    TextureObject *source =
                        find(textures_, item.source);
                    ReadbackObject *destination =
                        find(readbacks_, item.destination);
                    if (source == nullptr || destination == nullptr) {
                        return Status::error(
                            VITA_GPU_ERROR_INVALID_HANDLE,
                            "texture readback references an unknown Vulkan object",
                            VITA_GPU_BACKEND_VULKAN);
                    }
                    if (source->dimension == VITA_GPU_TEXTURE_3D) {
                        return unsupported(
                            "Vulkan 3D texture readback is not represented unambiguously by the contract");
                    }
                    if (!source->copy_source_supported) {
                        return unsupported(
                            "this Vulkan image does not expose transfer-source usage");
                    }
                    if (has_stencil(source->format)) {
                        return unsupported(
                            "Vulkan packed depth-stencil readback is unsupported");
                    }
                    const FormatLayout format =
                        format_layout(source->format);
                    if (format.bytes_per_block == 0) {
                        return unsupported(
                            "texture readback format has no Vulkan packing rule");
                    }
                    const std::uint32_t width =
                        std::max(1U,
                                 source->width >> item.mip_level);
                    const std::uint32_t height =
                        std::max(1U,
                                 source->height >> item.mip_level);
                    const std::uint32_t blocks_x =
                        (width + format.block_width - 1U) /
                        format.block_width;
                    const std::uint32_t copy_block_rows =
                        (height + format.block_height - 1U) /
                        format.block_height;
                    const std::uint32_t minimum_row =
                        blocks_x * format.bytes_per_block;
                    if ((item.destination_offset % 4U) != 0 ||
                        (item.destination_offset %
                         format.bytes_per_block) != 0 ||
                        item.row_pitch < minimum_row ||
                        (item.row_pitch % format.bytes_per_block) != 0 ||
                        static_cast<std::uint64_t>(
                            item.slice_pitch) <
                            static_cast<std::uint64_t>(
                                item.row_pitch) *
                                copy_block_rows ||
                        (item.slice_pitch % item.row_pitch) != 0) {
                        return unsupported(
                            "Vulkan texture readback pitches or offset do not satisfy format alignment");
                    }
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = item.destination_offset;
                    if (item.row_pitch != 0) {
                        const std::uint32_t blocks =
                            item.row_pitch /
                            format.bytes_per_block;
                        copy.bufferRowLength =
                            blocks * format.block_width;
                    }
                    if (item.slice_pitch != 0 &&
                        item.row_pitch != 0) {
                        const std::uint32_t block_rows =
                            item.slice_pitch / item.row_pitch;
                        copy.bufferImageHeight =
                            block_rows * format.block_height;
                    }
                    copy.imageSubresource.aspectMask =
                        source->aspect;
                    copy.imageSubresource.mipLevel =
                        item.mip_level;
                    copy.imageSubresource.baseArrayLayer =
                        item.array_layer;
                    copy.imageSubresource.layerCount = 1;
                    copy.imageExtent.width = width;
                    copy.imageExtent.height = height;
                    copy.imageExtent.depth =
                        std::max(1U,
                                 source->depth >> item.mip_level);
                    vk.vkCmdCopyImageToBuffer(
                        command, source->image,
                        layout_for(
                            item.source_state,
                            is_depth_format(source->format)),
                        destination->buffer, 1, &copy);
                    readback_updates.push_back(
                        {item.destination, item.row_pitch,
                         item.slice_pitch});
                    return Status::success();
                } else if constexpr (
                    std::is_same_v<T, DebugMarkerCommand>) {
                    if (vk.vkCmdInsertDebugUtilsLabelEXT != nullptr) {
                        VkDebugUtilsLabelEXT label{
                            VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
                        label.pLabelName = item.label.c_str();
                        label.color[0] =
                            static_cast<float>(
                                item.rgba_color & UINT32_C(0xff)) /
                            255.0F;
                        label.color[1] =
                            static_cast<float>(
                                (item.rgba_color >> 8U) &
                                UINT32_C(0xff)) /
                            255.0F;
                        label.color[2] =
                            static_cast<float>(
                                (item.rgba_color >> 16U) &
                                UINT32_C(0xff)) /
                            255.0F;
                        label.color[3] =
                            static_cast<float>(
                                (item.rgba_color >> 24U) &
                                UINT32_C(0xff)) /
                            255.0F;
                        vk.vkCmdInsertDebugUtilsLabelEXT(
                            command, &label);
                    }
                    return Status::success();
                } else {
                    return unsupported(
                        "recorded command is not implemented by Vulkan");
                }
            },
            payload);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::success();
}

Status VulkanDevice::cleanup_submissions() {
    auto &vk = context_->dispatch();
    for (auto pending = pending_submissions_.begin();
         pending != pending_submissions_.end();) {
        std::uint64_t completed = 0;
        const VkResult result = vk.vkGetSemaphoreCounterValue(
            device_, pending->completion, &completed);
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result,
                "querying Vulkan submission completion failed");
        }
        if (completed >= pending->value) {
            if (!pending->command_buffers.empty()) {
                vk.vkFreeCommandBuffers(
                    device_, command_pool_,
                    static_cast<std::uint32_t>(
                        pending->command_buffers.size()),
                    pending->command_buffers.data());
            }
            pending = pending_submissions_.erase(pending);
        } else {
            ++pending;
        }
    }
    return Status::success();
}

Status VulkanDevice::submit(
    vita_gpu_queue_type,
    std::span<const RecordedCommandList *const> command_lists,
    std::span<const BackendFencePoint> waits,
    BackendFencePoint signal) {
    FenceObject *signal_fence = find(fences_, signal.fence);
    if (signal_fence == nullptr) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_HANDLE,
            "submission signal references an unknown Vulkan fence",
            VITA_GPU_BACKEND_VULKAN);
    }
    Status status = cleanup_submissions();
    if (!status.ok()) {
        return status;
    }
    std::vector<VkCommandBuffer> commands(command_lists.size());
    std::set<std::uint64_t> initialized_swapchain_textures;
    std::vector<ReadbackLayoutUpdate> readback_updates;
    VkCommandBufferAllocateInfo allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = command_pool_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount =
        static_cast<std::uint32_t>(commands.size());
    auto &vk = context_->dispatch();
    VkResult result = vk.vkAllocateCommandBuffers(
        device_, &allocation, commands.data());
    if (result != VK_SUCCESS) {
        return vulkan_error(
            result, "vkAllocateCommandBuffers failed");
    }
    std::size_t begun = 0;
    for (std::size_t index = 0;
         index < command_lists.size(); ++index) {
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vk.vkBeginCommandBuffer(commands[index], &begin);
        if (result != VK_SUCCESS) {
            status = vulkan_error(
                result, "vkBeginCommandBuffer failed");
            break;
        }
        ++begun;
        status = encode_command_list(
            *command_lists[index], commands[index],
            initialized_swapchain_textures, readback_updates);
        if (!status.ok()) {
            break;
        }
        result = vk.vkEndCommandBuffer(commands[index]);
        if (result != VK_SUCCESS) {
            status = vulkan_error(
                result, "vkEndCommandBuffer failed");
            break;
        }
    }
    if (!status.ok()) {
        for (std::size_t index = 0; index < begun; ++index) {
            (void)vk.vkResetCommandBuffer(commands[index], 0);
        }
        vk.vkFreeCommandBuffers(
            device_, command_pool_,
            static_cast<std::uint32_t>(commands.size()),
            commands.data());
        return status;
    }

    SwapchainObject *active_swapchain = nullptr;
    for (auto &[id, swapchain] : swapchains_) {
        (void)id;
        if (!swapchain.acquired ||
            swapchain.acquired_image >= swapchain.texture_ids.size()) {
            continue;
        }
        const std::uint64_t acquired_texture =
            swapchain.texture_ids[swapchain.acquired_image];
        const std::uint64_t acquired_target =
            swapchain.target_ids[swapchain.acquired_image];
        bool used = false;
        for (const RecordedCommandList *recorded : command_lists) {
            for (const CommandPayload &payload : recorded->commands) {
                if (const auto *barrier =
                        std::get_if<BarrierCommand>(&payload);
                    barrier != nullptr &&
                    barrier->kind == VITA_GPU_RESOURCE_TEXTURE &&
                    barrier->resource.value == acquired_texture) {
                    used = true;
                } else if (const auto *pass =
                               std::get_if<BeginRenderPassCommand>(
                                   &payload);
                           pass != nullptr &&
                           std::any_of(
                               pass->colors.begin(), pass->colors.end(),
                               [acquired_target](
                                   const BackendColorAttachment &color) {
                                   return color.target.value ==
                                          acquired_target;
                               })) {
                    used = true;
                } else if (const auto *copy =
                               std::get_if<CopyTextureReadbackCommand>(
                                   &payload);
                           copy != nullptr &&
                           copy->source.value == acquired_texture) {
                    used = true;
                }
            }
        }
        if (used) {
            if (swapchain.submitted) {
                vk.vkFreeCommandBuffers(
                    device_, command_pool_,
                    static_cast<std::uint32_t>(commands.size()),
                    commands.data());
                return Status::error(
                    VITA_GPU_ERROR_INVALID_STATE,
                    "an acquired Vulkan swapchain image supports exactly one submission before present",
                    VITA_GPU_BACKEND_VULKAN);
            }
            if (active_swapchain != nullptr) {
                vk.vkFreeCommandBuffers(
                    device_, command_pool_,
                    static_cast<std::uint32_t>(commands.size()),
                    commands.data());
                return unsupported(
                    "one Vulkan submission cannot target multiple acquired swapchains");
            }
            active_swapchain = &swapchain;
        }
    }

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<std::uint64_t> wait_values;
    std::vector<VkPipelineStageFlags> wait_stages;
    wait_semaphores.reserve(
        waits.size() + (active_swapchain != nullptr ? 1U : 0U));
    wait_values.reserve(wait_semaphores.capacity());
    wait_stages.reserve(wait_semaphores.capacity());
    SwapchainFrame *active_frame = nullptr;
    if (active_swapchain != nullptr) {
        active_frame =
            &active_swapchain->frames[
                active_swapchain->acquired_frame];
        result = vk.vkResetFences(
            device_, 1, &active_frame->in_flight);
        if (result != VK_SUCCESS) {
            vk.vkFreeCommandBuffers(
                device_, command_pool_,
                static_cast<std::uint32_t>(commands.size()),
                commands.data());
            return vulkan_error(
                result, "vkResetFences failed");
        }
        wait_semaphores.push_back(active_frame->image_available);
        wait_values.push_back(0);
        wait_stages.push_back(
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    for (const BackendFencePoint &wait : waits) {
        FenceObject *fence = find(fences_, wait.fence);
        if (fence == nullptr) {
            vk.vkFreeCommandBuffers(
                device_, command_pool_,
                static_cast<std::uint32_t>(commands.size()),
                commands.data());
            return Status::error(
                VITA_GPU_ERROR_INVALID_HANDLE,
                "submission wait references an unknown Vulkan fence",
                VITA_GPU_BACKEND_VULKAN);
        }
        wait_semaphores.push_back(fence->semaphore);
        wait_values.push_back(wait.value);
        wait_stages.push_back(
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    std::vector<VkSemaphore> signal_semaphores;
    std::vector<std::uint64_t> signal_values;
    if (active_frame != nullptr) {
        signal_semaphores.push_back(
            active_frame->render_finished);
        signal_values.push_back(0);
    }
    signal_semaphores.push_back(signal_fence->semaphore);
    signal_values.push_back(signal.value);
    VkTimelineSemaphoreSubmitInfo timeline{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount =
        static_cast<std::uint32_t>(wait_values.size());
    timeline.pWaitSemaphoreValues = wait_values.data();
    timeline.signalSemaphoreValueCount =
        static_cast<std::uint32_t>(signal_values.size());
    timeline.pSignalSemaphoreValues = signal_values.data();
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.pNext = &timeline;
    submit_info.waitSemaphoreCount =
        static_cast<std::uint32_t>(wait_semaphores.size());
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.commandBufferCount =
        static_cast<std::uint32_t>(commands.size());
    submit_info.pCommandBuffers = commands.data();
    submit_info.signalSemaphoreCount =
        static_cast<std::uint32_t>(signal_semaphores.size());
    submit_info.pSignalSemaphores = signal_semaphores.data();
    result = vk.vkQueueSubmit(
        queue_, 1, &submit_info,
        active_frame == nullptr ? VK_NULL_HANDLE
                                : active_frame->in_flight);
    if (result != VK_SUCCESS) {
        vk.vkFreeCommandBuffers(
            device_, command_pool_,
            static_cast<std::uint32_t>(commands.size()),
            commands.data());
        return vulkan_error(result, "vkQueueSubmit failed");
    }
    pending_submissions_.push_back(
        {signal_fence->semaphore, signal.value, std::move(commands)});
    for (std::uint64_t id : initialized_swapchain_textures) {
        TextureObject *texture = find(textures_, NativeId{id});
        if (texture != nullptr) {
            texture->layout_initialized = true;
        }
    }
    for (const ReadbackLayoutUpdate &update : readback_updates) {
        ReadbackObject *readback =
            find(readbacks_, update.readback);
        if (readback != nullptr) {
            readback->row_pitch = update.row_pitch;
            readback->slice_pitch = update.slice_pitch;
        }
    }
    if (active_frame != nullptr) {
        active_frame->submitted = true;
        active_swapchain->submitted = true;
    }
    return checked_validation();
}

Status enumerate_compatible_devices(
    InstanceContext &context,
    std::vector<VkPhysicalDevice> &compatible) {
    std::vector<VkPhysicalDevice> devices;
    Status status = context.enumerate_devices(devices);
    if (!status.ok()) {
        return status;
    }
    for (VkPhysicalDevice physical : devices) {
        VkPhysicalDeviceProperties properties{};
        context.dispatch().vkGetPhysicalDeviceProperties(
            physical, &properties);
        if (properties.apiVersion < kRequiredApiVersion ||
            !graphics_queue_family(context.dispatch(), physical)
                 .has_value()) {
            continue;
        }
        VkPhysicalDeviceFeatures2 features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features features12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features.pNext = &features12;
        features12.pNext = &features13;
        context.dispatch().vkGetPhysicalDeviceFeatures2(
            physical, &features);
        if (features12.timelineSemaphore != VK_TRUE ||
            features13.dynamicRendering != VK_TRUE ||
            features13.synchronization2 != VK_TRUE) {
            continue;
        }
        std::uint32_t extension_count = 0;
        VkResult result =
            context.dispatch().vkEnumerateDeviceExtensionProperties(
                physical, nullptr, &extension_count, nullptr);
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result,
                "vkEnumerateDeviceExtensionProperties failed");
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        result =
            context.dispatch().vkEnumerateDeviceExtensionProperties(
                physical, nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS) {
            return vulkan_error(
                result,
                "vkEnumerateDeviceExtensionProperties failed");
        }
        const bool has_swapchain = std::any_of(
            extensions.begin(), extensions.end(),
            [](const VkExtensionProperties &extension) {
                return std::string_view(extension.extensionName) ==
                       VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            });
        if (has_swapchain) {
            compatible.push_back(physical);
        }
    }
    return Status::success();
}

class VulkanFactory final : public BackendFactory {
public:
    [[nodiscard]] vita_gpu_backend backend() const noexcept override {
        return VITA_GPU_BACKEND_VULKAN;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "vulkan";
    }
    [[nodiscard]] bool test_only() const noexcept override {
        return false;
    }

    Status availability(bool &available, bool &validation_available,
                        std::string &reason) const override {
        available = false;
        validation_available = false;
        reason.clear();
        std::unique_ptr<InstanceContext> context;
        Status status =
            InstanceContext::create(false, "vitarecomp-probe", context);
        if (!status.ok()) {
            reason = status.message;
            return Status::success();
        }
        validation_available = context->validation_available();
        std::vector<VkPhysicalDevice> devices;
        status = enumerate_compatible_devices(*context, devices);
        if (!status.ok()) {
            reason = status.message;
            return Status::success();
        }
        if (devices.empty()) {
            reason =
                "Vulkan loader found no Vulkan 1.3 ICD with graphics, timeline semaphores, synchronization2, dynamic rendering, and swapchain support";
            return Status::success();
        }
        available = true;
        return Status::success();
    }

    Status adapter_count(std::uint32_t &count) const override {
        std::unique_ptr<InstanceContext> context;
        Status status =
            InstanceContext::create(false, "vitarecomp-enumeration",
                                    context);
        if (!status.ok()) {
            return status;
        }
        std::vector<VkPhysicalDevice> devices;
        status = enumerate_compatible_devices(*context, devices);
        if (!status.ok()) {
            return status;
        }
        count = static_cast<std::uint32_t>(devices.size());
        return Status::success();
    }

    Status adapter_info(std::uint32_t index,
                        vita_gpu_adapter_info &output) const override {
        std::unique_ptr<InstanceContext> context;
        Status status =
            InstanceContext::create(false, "vitarecomp-enumeration",
                                    context);
        if (!status.ok()) {
            return status;
        }
        std::vector<VkPhysicalDevice> devices;
        status = enumerate_compatible_devices(*context, devices);
        if (!status.ok()) {
            return status;
        }
        if (index >= devices.size()) {
            return Status::error(
                VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                "Vulkan adapter index is out of range",
                VITA_GPU_BACKEND_VULKAN);
        }
        return fill_adapter_info(
            *context, devices[index], index, output);
    }

    Status create_device(
        const vita_gpu_device_desc &desc,
        std::unique_ptr<BackendDevice> &output) const override {
        const bool validation =
            (desc.flags & VITA_GPU_DEVICE_ENABLE_VALIDATION) != 0;
        std::unique_ptr<InstanceContext> context;
        Status status = InstanceContext::create(
            validation,
            desc.application_name == nullptr
                ? std::string_view{}
                : std::string_view(desc.application_name),
            context);
        if (!status.ok()) {
            return status;
        }
        std::vector<VkPhysicalDevice> devices;
        status = enumerate_compatible_devices(*context, devices);
        if (!status.ok()) {
            return status;
        }
        if (desc.adapter_index >= devices.size()) {
            return Status::error(
                VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                "Vulkan adapter index is out of range",
                VITA_GPU_BACKEND_VULKAN);
        }
        const VkPhysicalDevice physical =
            devices[desc.adapter_index];
        const auto queue_family =
            graphics_queue_family(context->dispatch(), physical);
        if (!queue_family.has_value()) {
            return unsupported(
                "Vulkan adapter has no unified graphics/compute/transfer queue");
        }
        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = *queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        VkPhysicalDeviceFeatures2 available{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features available12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features available13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        available.pNext = &available12;
        available12.pNext = &available13;
        context->dispatch().vkGetPhysicalDeviceFeatures2(
            physical, &available);

        VkPhysicalDeviceFeatures enabled{};
        enabled.samplerAnisotropy =
            available.features.samplerAnisotropy;
        enabled.independentBlend =
            available.features.independentBlend;
        enabled.textureCompressionBC =
            available.features.textureCompressionBC;
        enabled.shaderInt64 = available.features.shaderInt64;
        enabled.fillModeNonSolid =
            available.features.fillModeNonSolid;
        enabled.depthBiasClamp =
            available.features.depthBiasClamp;
        VkPhysicalDeviceVulkan12Features enabled12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled12.timelineSemaphore = VK_TRUE;
        enabled12.shaderFloat16 = available12.shaderFloat16;
        VkPhysicalDeviceVulkan13Features enabled13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled13.dynamicRendering = VK_TRUE;
        enabled13.synchronization2 = VK_TRUE;
        enabled12.pNext = &enabled13;

        constexpr const char *extensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &enabled12;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = extensions;
        device_info.pEnabledFeatures = &enabled;
        VkDevice device = VK_NULL_HANDLE;
        VkResult result = context->dispatch().vkCreateDevice(
            physical, &device_info, nullptr, &device);
        if (result != VK_SUCCESS) {
            return vulkan_error(result, "vkCreateDevice failed");
        }
        context->dispatch().init(vk::Device(device));
        VkQueue queue = VK_NULL_HANDLE;
        context->dispatch().vkGetDeviceQueue(
            device, *queue_family, 0, &queue);
        VkPhysicalDeviceProperties properties{};
        context->dispatch().vkGetPhysicalDeviceProperties(
            physical, &properties);
        auto backend = std::make_unique<VulkanDevice>(
            std::move(context), physical, device, *queue_family, queue,
            properties, available.features);
        status = backend->initialize();
        if (!status.ok()) {
            return status;
        }
        output = std::move(backend);
        return Status::success();
    }
};

} // namespace

std::shared_ptr<BackendFactory> make_vulkan_backend_factory() {
    return std::make_shared<VulkanFactory>();
}

} // namespace vitarecomp::gpu
