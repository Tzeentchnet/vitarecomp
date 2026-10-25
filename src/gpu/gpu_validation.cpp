#include "gpu_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace vitarecomp::gpu {
namespace {

constexpr vita_gpu_device_flags kDeviceFlags =
    VITA_GPU_DEVICE_ENABLE_VALIDATION | VITA_GPU_DEVICE_ENABLE_DEBUG_NAMES;
constexpr vita_gpu_buffer_usage kBufferUsage =
    VITA_GPU_BUFFER_VERTEX | VITA_GPU_BUFFER_INDEX | VITA_GPU_BUFFER_CONSTANT |
    VITA_GPU_BUFFER_STORAGE | VITA_GPU_BUFFER_INDIRECT |
    VITA_GPU_BUFFER_COPY_SOURCE | VITA_GPU_BUFFER_COPY_DEST;
constexpr vita_gpu_texture_usage kTextureUsage =
    VITA_GPU_TEXTURE_SAMPLED | VITA_GPU_TEXTURE_STORAGE |
    VITA_GPU_TEXTURE_RENDER_TARGET | VITA_GPU_TEXTURE_DEPTH_TARGET |
    VITA_GPU_TEXTURE_COPY_SOURCE | VITA_GPU_TEXTURE_COPY_DEST;
constexpr vita_gpu_resource_state kResourceStates =
    VITA_GPU_RESOURCE_STATE_COMMON | VITA_GPU_RESOURCE_STATE_COPY_SOURCE |
    VITA_GPU_RESOURCE_STATE_COPY_DEST |
    VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER |
    VITA_GPU_RESOURCE_STATE_INDEX_BUFFER |
    VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER |
    VITA_GPU_RESOURCE_STATE_SHADER_READ |
    VITA_GPU_RESOURCE_STATE_SHADER_WRITE |
    VITA_GPU_RESOURCE_STATE_RENDER_TARGET |
    VITA_GPU_RESOURCE_STATE_DEPTH_READ |
    VITA_GPU_RESOURCE_STATE_DEPTH_WRITE |
    VITA_GPU_RESOURCE_STATE_PRESENT;
constexpr vita_gpu_shader_flags kShaderFlags =
    VITA_GPU_SHADER_DEBUG | VITA_GPU_SHADER_DISABLE_OPTIMIZE |
    VITA_GPU_SHADER_WARNINGS_AS_ERROR;
constexpr vita_gpu_shader_stage_flags kShaderStages = VITA_GPU_SHADER_STAGE_ALL;

Status invalid(std::string message) {
    return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT, std::move(message));
}

bool valid_backend(vita_gpu_backend value) {
    return value == VITA_GPU_BACKEND_DX12 || value == VITA_GPU_BACKEND_VULKAN;
}

bool valid_queue(vita_gpu_queue_type value) {
    return value >= VITA_GPU_QUEUE_GRAPHICS && value <= VITA_GPU_QUEUE_COPY;
}

bool valid_format(vita_gpu_format value) {
    return value > VITA_GPU_FORMAT_UNKNOWN &&
           value <= VITA_GPU_FORMAT_BC7_SRGB;
}

bool depth_format(vita_gpu_format value) {
    return value >= VITA_GPU_FORMAT_D16_UNORM &&
           value <= VITA_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

bool valid_resource_state(vita_gpu_resource_state state,
                          bool allow_undefined = false) {
    if ((state & ~kResourceStates) != 0) {
        return false;
    }
    if (state == VITA_GPU_RESOURCE_STATE_UNDEFINED) {
        return allow_undefined;
    }
    const vita_gpu_resource_state write_states =
        state & (VITA_GPU_RESOURCE_STATE_COPY_DEST |
                 VITA_GPU_RESOURCE_STATE_SHADER_WRITE |
                 VITA_GPU_RESOURCE_STATE_RENDER_TARGET |
                 VITA_GPU_RESOURCE_STATE_DEPTH_WRITE |
                 VITA_GPU_RESOURCE_STATE_PRESENT);
    return write_states == 0 || (write_states & (write_states - 1U)) == 0;
}

bool valid_shader_stage(vita_gpu_shader_stage value) {
    return value >= VITA_GPU_SHADER_VERTEX && value <= VITA_GPU_SHADER_COMPUTE;
}

bool valid_shader_target(vita_gpu_shader_target value) {
    return value == VITA_GPU_SHADER_TARGET_DXIL ||
           value == VITA_GPU_SHADER_TARGET_SPIRV;
}

bool valid_compare(vita_gpu_compare_op value) {
    return value >= VITA_GPU_COMPARE_NEVER && value <= VITA_GPU_COMPARE_ALWAYS;
}

bool valid_sample_count(std::uint32_t value) {
    return value == 1 || value == 2 || value == 4 || value == 8 ||
           value == 16;
}

bool valid_optional_string(const char *text, std::size_t maximum = 4096) {
    if (text == nullptr) {
        return true;
    }
    for (std::size_t i = 0; i < maximum; ++i) {
        if (text[i] == '\0') {
            return true;
        }
    }
    return false;
}

bool valid_required_string(const char *text, std::size_t maximum = 4096) {
    return text != nullptr && text[0] != '\0' &&
           valid_optional_string(text, maximum);
}

template <typename T>
Status validate_nested(const T &descriptor, std::string_view name) {
    return validate_struct(&descriptor, sizeof(T), name);
}

Status validate_rasterizer(const vita_gpu_rasterizer_desc &desc) {
    Status status = validate_nested(desc, "rasterizer");
    if (!status.ok()) {
        return status;
    }
    if (desc.polygon_mode < VITA_GPU_POLYGON_FILL ||
        desc.polygon_mode > VITA_GPU_POLYGON_LINE ||
        desc.cull_mode < VITA_GPU_CULL_NONE ||
        desc.cull_mode > VITA_GPU_CULL_BACK ||
        desc.front_face < VITA_GPU_FRONT_COUNTER_CLOCKWISE ||
        desc.front_face > VITA_GPU_FRONT_CLOCKWISE ||
        !std::isfinite(desc.depth_bias_clamp) ||
        !std::isfinite(desc.slope_scaled_depth_bias)) {
        return invalid("graphics pipeline has invalid rasterizer state");
    }
    return Status::success();
}

Status validate_depth_stencil(const vita_gpu_depth_stencil_desc &desc) {
    Status status = validate_nested(desc, "depth_stencil");
    if (!status.ok()) {
        return status;
    }
    if (desc.depth_test_enable > VITA_GPU_TRUE ||
        desc.depth_write_enable > VITA_GPU_TRUE ||
        desc.stencil_enable > VITA_GPU_TRUE ||
        !valid_compare(desc.depth_compare)) {
        return invalid("graphics pipeline has invalid depth/stencil state");
    }
    return Status::success();
}

Status validate_blend(const vita_gpu_blend_attachment_desc &desc) {
    Status status = validate_nested(desc, "blend_attachment");
    if (!status.ok()) {
        return status;
    }
    if (desc.blend_enable > VITA_GPU_TRUE ||
        desc.source_color < VITA_GPU_BLEND_ZERO ||
        desc.source_color > VITA_GPU_BLEND_INV_DST_ALPHA ||
        desc.destination_color < VITA_GPU_BLEND_ZERO ||
        desc.destination_color > VITA_GPU_BLEND_INV_DST_ALPHA ||
        desc.source_alpha < VITA_GPU_BLEND_ZERO ||
        desc.source_alpha > VITA_GPU_BLEND_INV_DST_ALPHA ||
        desc.destination_alpha < VITA_GPU_BLEND_ZERO ||
        desc.destination_alpha > VITA_GPU_BLEND_INV_DST_ALPHA ||
        desc.color_op < VITA_GPU_BLEND_OP_ADD ||
        desc.color_op > VITA_GPU_BLEND_OP_MAX ||
        desc.alpha_op < VITA_GPU_BLEND_OP_ADD ||
        desc.alpha_op > VITA_GPU_BLEND_OP_MAX ||
        (desc.color_write_mask & ~UINT32_C(0x0f)) != 0) {
        return invalid("graphics pipeline has invalid blend state");
    }
    return Status::success();
}

} // namespace

Status validate_struct(const void *descriptor, std::size_t required_size,
                       std::string_view name) {
    if (descriptor == nullptr) {
        return invalid(std::string(name) + " is null");
    }
    const auto *header =
        static_cast<const vita_gpu_struct_header *>(descriptor);
    if (header->struct_version != VITA_GPU_STRUCT_VERSION_1) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED_ABI_VERSION,
            std::string(name) + " has unsupported struct_version " +
                std::to_string(header->struct_version));
    }
    if (header->struct_size < required_size) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_STRUCT,
            std::string(name) + " struct_size is " +
                std::to_string(header->struct_size) + ", expected at least " +
                std::to_string(required_size));
    }
    return Status::success();
}

Status validate_output_struct(const void *descriptor,
                              std::size_t required_size,
                              std::string_view name) {
    return validate_struct(descriptor, required_size, name);
}

Status validate_device_desc(const vita_gpu_device_desc &desc) {
    Status status = validate_nested(desc, "device_desc");
    if (!status.ok()) {
        return status;
    }
    if (!valid_backend(desc.backend)) {
        return invalid("device_desc.backend must be dx12 or vulkan");
    }
    if ((desc.flags & ~kDeviceFlags) != 0) {
        return invalid("device_desc.flags contains unknown bits");
    }
    if (!valid_optional_string(desc.application_name, 256)) {
        return invalid("device_desc.application_name is not a bounded string");
    }
    return Status::success();
}

Status validate_swapchain_desc(const vita_gpu_swapchain_desc &desc) {
    Status status = validate_nested(desc, "swapchain_desc");
    if (!status.ok()) {
        return status;
    }
    status = validate_nested(desc.window, "window_handle");
    if (!status.ok()) {
        return status;
    }
    if (desc.window.system != VITA_GPU_WINDOW_WIN32 ||
        desc.window.native_window == nullptr) {
        return invalid("swapchain requires a non-null Win32 window handle");
    }
    if (desc.width == 0 || desc.height == 0) {
        return invalid("swapchain extent must be non-zero");
    }
    if (!valid_format(desc.format) || depth_format(desc.format)) {
        return invalid("swapchain format must be a color format");
    }
    if (desc.image_count < 2 || desc.image_count > 8) {
        return invalid("swapchain image_count must be between 2 and 8");
    }
    if (desc.present_mode < VITA_GPU_PRESENT_FIFO ||
        desc.present_mode > VITA_GPU_PRESENT_MAILBOX) {
        return invalid("swapchain has invalid present mode");
    }
    if (!valid_optional_string(desc.label, 256)) {
        return invalid("swapchain label is not a bounded string");
    }
    return Status::success();
}

Status validate_buffer_desc(const vita_gpu_buffer_desc &desc) {
    Status status = validate_nested(desc, "buffer_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.byte_size == 0) {
        return invalid("buffer byte_size must be non-zero");
    }
    if (desc.usage == 0 || (desc.usage & ~kBufferUsage) != 0) {
        return invalid("buffer usage is empty or contains unknown bits");
    }
    if (desc.memory_type < VITA_GPU_MEMORY_GPU_ONLY ||
        desc.memory_type > VITA_GPU_MEMORY_CPU_READBACK) {
        return invalid("buffer memory_type is invalid");
    }
    if (!valid_resource_state(desc.initial_state)) {
        return invalid("buffer initial_state is invalid");
    }
    const vita_gpu_resource_state forbidden_buffer_states =
        VITA_GPU_RESOURCE_STATE_RENDER_TARGET |
        VITA_GPU_RESOURCE_STATE_DEPTH_READ |
        VITA_GPU_RESOURCE_STATE_DEPTH_WRITE |
        VITA_GPU_RESOURCE_STATE_PRESENT;
    if ((desc.initial_state & forbidden_buffer_states) != 0 ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_COPY_SOURCE) != 0 &&
         (desc.usage & VITA_GPU_BUFFER_COPY_SOURCE) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_COPY_DEST) != 0 &&
         (desc.usage & VITA_GPU_BUFFER_COPY_DEST) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER) != 0 &&
         (desc.usage & VITA_GPU_BUFFER_VERTEX) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_INDEX_BUFFER) != 0 &&
         (desc.usage & VITA_GPU_BUFFER_INDEX) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER) != 0 &&
         (desc.usage & VITA_GPU_BUFFER_CONSTANT) == 0) ||
        ((desc.initial_state & (VITA_GPU_RESOURCE_STATE_SHADER_READ |
                                VITA_GPU_RESOURCE_STATE_SHADER_WRITE)) != 0 &&
         (desc.usage & VITA_GPU_BUFFER_STORAGE) == 0)) {
        return invalid("buffer initial_state is incompatible with usage");
    }
    if (desc.element_format != VITA_GPU_FORMAT_UNKNOWN &&
        !valid_format(desc.element_format)) {
        return invalid("buffer element_format is invalid");
    }
    if (desc.element_format != VITA_GPU_FORMAT_UNKNOWN &&
        desc.element_stride == 0) {
        return invalid("typed buffer requires a non-zero element_stride");
    }
    if ((desc.initial_data == nullptr) != (desc.initial_data_size == 0)) {
        return invalid("buffer initial_data and initial_data_size disagree");
    }
    if (desc.initial_data_size > desc.byte_size) {
        return invalid("buffer initial_data exceeds byte_size");
    }
    if (desc.memory_type == VITA_GPU_MEMORY_CPU_READBACK &&
        desc.initial_data != nullptr) {
        return invalid("readback buffer cannot have initial data");
    }
    if (!valid_optional_string(desc.label, 256)) {
        return invalid("buffer label is not a bounded string");
    }
    return Status::success();
}

Status validate_texture_desc(const vita_gpu_texture_desc &desc) {
    Status status = validate_nested(desc, "texture_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.dimension < VITA_GPU_TEXTURE_1D ||
        desc.dimension > VITA_GPU_TEXTURE_3D || desc.width == 0 ||
        desc.height == 0 || desc.depth_or_layers == 0 ||
        desc.mip_levels == 0 || !valid_sample_count(desc.sample_count) ||
        !valid_format(desc.format)) {
        return invalid("texture dimensions, format, mip count, or samples are invalid");
    }
    if (desc.dimension == VITA_GPU_TEXTURE_1D && desc.height != 1) {
        return invalid("1D texture height must be one");
    }
    if (desc.dimension == VITA_GPU_TEXTURE_3D &&
        (desc.usage & VITA_GPU_TEXTURE_DEPTH_TARGET) != 0) {
        return invalid("3D textures cannot be depth targets");
    }
    if (desc.sample_count > 1 &&
        (desc.mip_levels != 1 || desc.initial_data_count != 0)) {
        return invalid("multisampled textures require one mip and no initial data");
    }
    if (desc.usage == 0 || (desc.usage & ~kTextureUsage) != 0) {
        return invalid("texture usage is empty or contains unknown bits");
    }
    if (depth_format(desc.format) &&
        (desc.usage & VITA_GPU_TEXTURE_DEPTH_TARGET) == 0) {
        return invalid("depth format requires depth-target usage");
    }
    if (!depth_format(desc.format) &&
        (desc.usage & VITA_GPU_TEXTURE_DEPTH_TARGET) != 0) {
        return invalid("depth-target usage requires a depth format");
    }
    if (desc.memory_type < VITA_GPU_MEMORY_GPU_ONLY ||
        desc.memory_type > VITA_GPU_MEMORY_CPU_READBACK ||
        desc.memory_type == VITA_GPU_MEMORY_CPU_READBACK) {
        return invalid("textures support GPU-only or CPU-upload memory");
    }
    if (!valid_resource_state(desc.initial_state, true)) {
        return invalid("texture initial_state is invalid");
    }
    const vita_gpu_resource_state forbidden_texture_states =
        VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER |
        VITA_GPU_RESOURCE_STATE_INDEX_BUFFER |
        VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER |
        VITA_GPU_RESOURCE_STATE_PRESENT;
    if ((desc.initial_state & forbidden_texture_states) != 0 ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_COPY_SOURCE) != 0 &&
         (desc.usage & VITA_GPU_TEXTURE_COPY_SOURCE) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_COPY_DEST) != 0 &&
         (desc.usage & VITA_GPU_TEXTURE_COPY_DEST) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_SHADER_READ) != 0 &&
         (desc.usage &
          (VITA_GPU_TEXTURE_SAMPLED | VITA_GPU_TEXTURE_STORAGE)) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_SHADER_WRITE) != 0 &&
         (desc.usage & VITA_GPU_TEXTURE_STORAGE) == 0) ||
        ((desc.initial_state & VITA_GPU_RESOURCE_STATE_RENDER_TARGET) != 0 &&
         (desc.usage & VITA_GPU_TEXTURE_RENDER_TARGET) == 0) ||
        ((desc.initial_state & (VITA_GPU_RESOURCE_STATE_DEPTH_READ |
                                VITA_GPU_RESOURCE_STATE_DEPTH_WRITE)) != 0 &&
         (desc.usage & VITA_GPU_TEXTURE_DEPTH_TARGET) == 0)) {
        return invalid("texture initial_state is incompatible with usage");
    }
    if ((desc.initial_data == nullptr) != (desc.initial_data_count == 0)) {
        return invalid("texture initial_data and initial_data_count disagree");
    }
    const std::uint64_t layers =
        desc.dimension == VITA_GPU_TEXTURE_3D ? 1U : desc.depth_or_layers;
    const std::uint64_t maximum_subresources =
        static_cast<std::uint64_t>(desc.mip_levels) * layers;
    if (desc.initial_data_count > maximum_subresources) {
        return invalid("texture has more initial subresources than declared");
    }
    for (std::uint32_t i = 0; i < desc.initial_data_count; ++i) {
        status = validate_nested(desc.initial_data[i], "subresource_data");
        if (!status.ok()) {
            return status;
        }
        if (desc.initial_data[i].data == nullptr ||
            desc.initial_data[i].data_size == 0 ||
            desc.initial_data[i].row_pitch == 0 ||
            desc.initial_data[i].slice_pitch <
                desc.initial_data[i].row_pitch) {
            return invalid("texture subresource data is incomplete");
        }
    }
    if (!valid_optional_string(desc.label, 256)) {
        return invalid("texture label is not a bounded string");
    }
    return Status::success();
}

Status validate_sampler_desc(const vita_gpu_sampler_desc &desc) {
    Status status = validate_nested(desc, "sampler_desc");
    if (!status.ok()) {
        return status;
    }
    const auto valid_address = [](vita_gpu_address_mode value) {
        return value >= VITA_GPU_ADDRESS_REPEAT &&
               value <= VITA_GPU_ADDRESS_CLAMP_BORDER;
    };
    if (desc.min_filter < VITA_GPU_FILTER_NEAREST ||
        desc.min_filter > VITA_GPU_FILTER_LINEAR ||
        desc.mag_filter < VITA_GPU_FILTER_NEAREST ||
        desc.mag_filter > VITA_GPU_FILTER_LINEAR ||
        desc.mipmap_mode < VITA_GPU_MIPMAP_NEAREST ||
        desc.mipmap_mode > VITA_GPU_MIPMAP_LINEAR ||
        !valid_address(desc.address_u) || !valid_address(desc.address_v) ||
        !valid_address(desc.address_w) || desc.max_anisotropy == 0 ||
        desc.compare_enable > VITA_GPU_TRUE ||
        !valid_compare(desc.compare_op) || !std::isfinite(desc.mip_lod_bias) ||
        !std::isfinite(desc.min_lod) || !std::isfinite(desc.max_lod) ||
        desc.min_lod > desc.max_lod) {
        return invalid("sampler state is invalid");
    }
    if (!std::all_of(std::begin(desc.border_color),
                     std::end(desc.border_color),
                     [](float value) { return std::isfinite(value); })) {
        return invalid("sampler border color must be finite");
    }
    if (!valid_optional_string(desc.label, 256)) {
        return invalid("sampler label is not a bounded string");
    }
    return Status::success();
}

Status validate_render_target_desc(const vita_gpu_render_target_desc &desc) {
    Status status = validate_nested(desc, "render_target_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.texture.value == 0 || !valid_format(desc.format) ||
        depth_format(desc.format) || desc.array_layer_count == 0) {
        return invalid("render target texture, format, or layer range is invalid");
    }
    return Status::success();
}

Status validate_depth_target_desc(const vita_gpu_depth_target_desc &desc) {
    Status status = validate_nested(desc, "depth_target_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.texture.value == 0 || !depth_format(desc.format) ||
        desc.array_layer_count == 0 ||
        desc.read_only_depth > VITA_GPU_TRUE ||
        desc.read_only_stencil > VITA_GPU_TRUE) {
        return invalid("depth target texture, format, flags, or layer range is invalid");
    }
    return Status::success();
}

Status validate_shader_module_desc(const vita_gpu_shader_module_desc &desc) {
    Status status = validate_nested(desc, "shader_module_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.input == VITA_GPU_SHADER_INPUT_HLSL) {
        if (desc.hlsl == nullptr || desc.binary != nullptr) {
            return invalid("HLSL shader module requires only hlsl input");
        }
        return validate_shader_request(*desc.hlsl);
    }
    if (desc.input == VITA_GPU_SHADER_INPUT_BINARY) {
        if (desc.binary == nullptr || desc.hlsl != nullptr) {
            return invalid("binary shader module requires only binary input");
        }
        status = validate_nested(*desc.binary, "shader_binary_desc");
        if (!status.ok()) {
            return status;
        }
        status = validate_nested(desc.binary->key, "shader_key");
        if (!status.ok()) {
            return status;
        }
        if (!valid_shader_stage(desc.binary->stage) ||
            !valid_shader_target(desc.binary->target) ||
            desc.binary->data == nullptr || desc.binary->data_size == 0) {
            return invalid("shader binary is incomplete");
        }
        return Status::success();
    }
    return invalid("shader module input kind is invalid");
}

Status validate_binding_layout_desc(
    const vita_gpu_binding_layout_desc &desc) {
    Status status = validate_nested(desc, "binding_layout_desc");
    if (!status.ok()) {
        return status;
    }
    if ((desc.entries == nullptr) != (desc.entry_count == 0) ||
        desc.entry_count > 256 || desc.set_index >= 8) {
        return invalid("binding layout entries are invalid");
    }
    std::unordered_set<std::uint32_t> bindings;
    for (std::uint32_t i = 0; i < desc.entry_count; ++i) {
        const auto &entry = desc.entries[i];
        status = validate_nested(entry, "binding_layout_entry");
        if (!status.ok()) {
            return status;
        }
        if (entry.array_count == 0 ||
            entry.type < VITA_GPU_BINDING_CONSTANT_BUFFER ||
            entry.type > VITA_GPU_BINDING_SAMPLER || entry.visibility == 0 ||
            (entry.visibility & ~kShaderStages) != 0 ||
            !bindings.insert(entry.binding).second) {
            return invalid("binding layout has an invalid or duplicate entry");
        }
    }
    return Status::success();
}

Status validate_binding_write(const vita_gpu_binding_write &write) {
    Status status = validate_nested(write, "binding_write");
    if (!status.ok()) {
        return status;
    }
    if (write.type < VITA_GPU_BINDING_CONSTANT_BUFFER ||
        write.type > VITA_GPU_BINDING_SAMPLER) {
        return invalid("binding write type is invalid");
    }
    const bool needs_buffer =
        write.type == VITA_GPU_BINDING_CONSTANT_BUFFER ||
        write.type == VITA_GPU_BINDING_STORAGE_BUFFER;
    const bool needs_texture =
        write.type == VITA_GPU_BINDING_SAMPLED_TEXTURE ||
        write.type == VITA_GPU_BINDING_STORAGE_TEXTURE;
    const bool needs_sampler = write.type == VITA_GPU_BINDING_SAMPLER;
    if ((write.buffer.value != 0) != needs_buffer ||
        (write.texture.value != 0) != needs_texture ||
        (write.sampler.value != 0) != needs_sampler) {
        return invalid("binding write contains the wrong resource type");
    }
    if (needs_buffer && write.buffer_size == 0) {
        return invalid("buffer binding size must be non-zero");
    }
    if (needs_texture && !valid_resource_state(write.texture_state)) {
        return invalid("texture binding state is invalid");
    }
    return Status::success();
}

Status validate_graphics_pipeline_desc(
    const vita_gpu_graphics_pipeline_desc &desc) {
    Status status = validate_nested(desc, "graphics_pipeline_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.vertex_shader.value == 0 || desc.pixel_shader.value == 0 ||
        desc.topology < VITA_GPU_TOPOLOGY_POINT_LIST ||
        desc.topology > VITA_GPU_TOPOLOGY_TRIANGLE_STRIP ||
        desc.color_target_count == 0 ||
        desc.color_target_count > VITA_GPU_MAX_COLOR_TARGETS ||
        desc.binding_layout_count > 8 ||
        desc.vertex_binding_count > 32 ||
        desc.vertex_attribute_count > 64 ||
        !valid_sample_count(desc.sample_count) ||
        desc.push_constant_bytes > 4096 ||
        (desc.binding_layouts == nullptr) !=
            (desc.binding_layout_count == 0) ||
        (desc.vertex_bindings == nullptr) !=
            (desc.vertex_binding_count == 0) ||
        (desc.vertex_attributes == nullptr) !=
            (desc.vertex_attribute_count == 0)) {
        return invalid("graphics pipeline has invalid required fields");
    }
    status = validate_rasterizer(desc.rasterizer);
    if (!status.ok()) {
        return status;
    }
    status = validate_depth_stencil(desc.depth_stencil);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < desc.color_target_count; ++i) {
        if (!valid_format(desc.color_formats[i]) ||
            depth_format(desc.color_formats[i])) {
            return invalid("graphics pipeline color format is invalid");
        }
        status = validate_blend(desc.blend[i]);
        if (!status.ok()) {
            return status;
        }
    }
    if (desc.depth_format != VITA_GPU_FORMAT_UNKNOWN &&
        !depth_format(desc.depth_format)) {
        return invalid("graphics pipeline depth format is invalid");
    }
    for (std::uint32_t i = 0; i < desc.vertex_binding_count; ++i) {
        const auto &binding = desc.vertex_bindings[i];
        status = validate_nested(binding, "vertex_binding_desc");
        if (!status.ok()) {
            return status;
        }
        if (binding.stride == 0 ||
            binding.step < VITA_GPU_VERTEX_PER_VERTEX ||
            binding.step > VITA_GPU_VERTEX_PER_INSTANCE) {
            return invalid("vertex binding is invalid");
        }
    }
    for (std::uint32_t i = 0; i < desc.vertex_attribute_count; ++i) {
        const auto &attribute = desc.vertex_attributes[i];
        status = validate_nested(attribute, "vertex_attribute_desc");
        if (!status.ok()) {
            return status;
        }
        if (!valid_format(attribute.format) || depth_format(attribute.format)) {
            return invalid("vertex attribute format is invalid");
        }
    }
    return Status::success();
}

Status validate_compute_pipeline_desc(
    const vita_gpu_compute_pipeline_desc &desc) {
    Status status = validate_nested(desc, "compute_pipeline_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.compute_shader.value == 0 ||
        (desc.binding_layouts == nullptr) !=
            (desc.binding_layout_count == 0) ||
        desc.binding_layout_count > 8 ||
        desc.push_constant_bytes > 4096) {
        return invalid("compute pipeline has invalid required fields");
    }
    return Status::success();
}

} // namespace vitarecomp::gpu
