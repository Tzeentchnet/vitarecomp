#include "gpu_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace vitarecomp::gpu {
namespace {

Status invalid(std::string message) {
    return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT, std::move(message));
}

Status state_error(std::string message) {
    return Status::error(VITA_GPU_ERROR_INVALID_STATE, std::move(message));
}

Status handle_error(vita_gpu_result result, std::string_view object_name) {
    return Status::error(result, std::string(object_name) +
                                     (result == VITA_GPU_ERROR_WRONG_OWNER
                                          ? " belongs to another device"
                                          : result == VITA_GPU_ERROR_STALE_HANDLE
                                                ? " is stale"
                                                : " is invalid"));
}

Status validate_native_handle(NativeId handle, std::string_view object_name,
                              vita_gpu_backend backend) {
    return handle.value != 0
               ? Status::success()
               : Status::error(
                     VITA_GPU_ERROR_INTERNAL,
                     "backend returned an invalid " +
                         std::string(object_name) + " handle",
                     backend);
}

bool valid_queue(vita_gpu_queue_type queue) {
    return queue >= VITA_GPU_QUEUE_GRAPHICS && queue <= VITA_GPU_QUEUE_COPY;
}

bool depth_format(vita_gpu_format format) {
    return format >= VITA_GPU_FORMAT_D16_UNORM &&
           format <= VITA_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

bool has_state(vita_gpu_resource_state current,
               vita_gpu_resource_state required) {
    return (current & required) == required;
}

struct UseState {
    std::uint32_t dependencies = 0;
    std::uint32_t command_lists = 0;
    bool borrowed = false;

    [[nodiscard]] bool busy() const noexcept {
        return dependencies != 0 || command_lists != 0;
    }
};

struct ResourceRef {
    ObjectKind kind = ObjectKind::buffer;
    std::uint64_t handle = 0;

    friend bool operator==(const ResourceRef &, const ResourceRef &) = default;
};

struct SwapchainImageRecord {
    std::uint32_t image_index = 0;
    vita_gpu_texture texture{};
    vita_gpu_render_target target{};
};

struct SwapchainRecord {
    NativeId native;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    bool acquired = false;
    std::uint32_t acquired_index = 0;
    std::vector<SwapchainImageRecord> images;
    UseState use;
};

struct BufferRecord {
    NativeId native;
    std::uint64_t size = 0;
    vita_gpu_buffer_usage usage = 0;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
    UseState use;
};

struct TextureRecord {
    NativeId native;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth_or_layers = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t sample_count = 1;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    vita_gpu_texture_usage usage = 0;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
    UseState use;
};

struct SamplerRecord {
    NativeId native;
    UseState use;
};

struct RenderTargetRecord {
    NativeId native;
    std::uint64_t texture = 0;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    UseState use;
};

struct DepthTargetRecord {
    NativeId native;
    std::uint64_t texture = 0;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    bool read_only_depth = false;
    UseState use;
};

struct ShaderRecord {
    NativeId native;
    vita_gpu_shader_stage stage = VITA_GPU_SHADER_VERTEX;
    vita_gpu_shader_target target = VITA_GPU_SHADER_TARGET_DXIL;
    UseState use;
};

struct BindingLayoutRecord {
    NativeId native;
    std::uint32_t set_index = 0;
    std::vector<vita_gpu_binding_layout_entry> entries;
    UseState use;
};

struct BoundSlot {
    std::uint32_t binding = 0;
    std::uint32_t array_element = 0;
    ResourceRef resource;
    vita_gpu_resource_state required_state =
        VITA_GPU_RESOURCE_STATE_UNDEFINED;
};

struct BindingSetRecord {
    NativeId native;
    std::uint64_t layout = 0;
    std::vector<BoundSlot> slots;
    UseState use;
};

struct PipelineRecord {
    NativeId native;
    bool graphics = true;
    std::uint32_t push_constant_bytes = 0;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> layouts;
    std::vector<ResourceRef> dependencies;
    UseState use;
};

enum class CommandState : std::uint8_t {
    initial,
    recording,
    render_pass,
    executable,
    submitted
};

struct PredictedState {
    ResourceRef resource;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
};

struct CommandListRecord {
    RecordedCommandList recorded;
    CommandState state = CommandState::initial;
    std::vector<ResourceRef> references;
    std::vector<PredictedState> predicted_states;
    std::optional<std::uint64_t> bound_pipeline;
    bool index_buffer_bound = false;
    std::uint64_t submitted_fence = 0;
    std::uint64_t submitted_value = 0;
};

struct FenceRecord {
    NativeId native;
    std::uint64_t last_signaled = 0;
    std::uint64_t last_completed = 0;
    UseState use;
};

struct ReadbackRecord {
    NativeId native;
    std::uint64_t size = 0;
    bool mapped = false;
    UseState use;
};

template <typename T>
Status resolve(HandlePool<T> &pool, std::uint64_t handle, T *&output,
               std::string_view object_name) {
    const auto found = pool.find(handle);
    if (!found) {
        return handle_error(found.result, object_name);
    }
    output = found.value;
    return Status::success();
}

template <typename T>
Status ensure_destroyable(const T &record, std::string_view object_name) {
    if (record.use.borrowed) {
        return state_error(std::string(object_name) +
                           " is owned by its swapchain");
    }
    if (record.use.busy()) {
        return Status::error(VITA_GPU_ERROR_RESOURCE_IN_USE,
                             std::string(object_name) + " is still referenced");
    }
    return Status::success();
}

template <typename Handle>
Handle public_handle(std::uint64_t value) {
    return Handle{value};
}

class DeviceCore {
public:
    DeviceCore(std::uint16_t owner_tag,
               std::unique_ptr<BackendDevice> backend_device)
        : backend_(std::move(backend_device)),
          swapchains_(owner_tag),
          buffers_(owner_tag),
          textures_(owner_tag),
          samplers_(owner_tag),
          render_targets_(owner_tag),
          depth_targets_(owner_tag),
          shaders_(owner_tag),
          binding_layouts_(owner_tag),
          binding_sets_(owner_tag),
          pipelines_(owner_tag),
          commands_(owner_tag),
          fences_(owner_tag),
          readbacks_(owner_tag) {}

    Status can_destroy();
    Status capabilities(vita_gpu_device_capabilities &output);
    Status wait_idle();

    Status create_swapchain(const vita_gpu_swapchain_desc &desc,
                            vita_gpu_swapchain &output);
    Status destroy_swapchain(vita_gpu_swapchain handle);
    Status acquire_swapchain(vita_gpu_swapchain handle,
                             vita_gpu_swapchain_image &output);
    Status resize_swapchain(vita_gpu_swapchain handle, std::uint32_t width,
                            std::uint32_t height);
    Status present_swapchain(vita_gpu_swapchain handle,
                             const vita_gpu_present_desc &desc);

    Status create_buffer(const vita_gpu_buffer_desc &desc,
                         vita_gpu_buffer &output);
    Status destroy_buffer(vita_gpu_buffer handle);
    Status create_texture(const vita_gpu_texture_desc &desc,
                          vita_gpu_texture &output);
    Status destroy_texture(vita_gpu_texture handle);
    Status create_sampler(const vita_gpu_sampler_desc &desc,
                          vita_gpu_sampler &output);
    Status destroy_sampler(vita_gpu_sampler handle);
    Status create_render_target(const vita_gpu_render_target_desc &desc,
                                vita_gpu_render_target &output);
    Status destroy_render_target(vita_gpu_render_target handle);
    Status create_depth_target(const vita_gpu_depth_target_desc &desc,
                               vita_gpu_depth_target &output);
    Status destroy_depth_target(vita_gpu_depth_target handle);
    Status create_shader(const vita_gpu_shader_module_desc &desc,
                         vita_gpu_shader_module &output);
    Status destroy_shader(vita_gpu_shader_module handle);
    Status create_binding_layout(const vita_gpu_binding_layout_desc &desc,
                                 vita_gpu_binding_layout &output);
    Status destroy_binding_layout(vita_gpu_binding_layout handle);
    Status create_binding_set(const vita_gpu_binding_set_desc &desc,
                              vita_gpu_binding_set &output);
    Status update_binding_set(vita_gpu_binding_set handle,
                              std::span<const vita_gpu_binding_write> writes);
    Status destroy_binding_set(vita_gpu_binding_set handle);
    Status create_graphics_pipeline(
        const vita_gpu_graphics_pipeline_desc &desc, vita_gpu_pipeline &output);
    Status create_compute_pipeline(const vita_gpu_compute_pipeline_desc &desc,
                                   vita_gpu_pipeline &output);
    Status destroy_pipeline(vita_gpu_pipeline handle);
    Status create_readback(const vita_gpu_readback_desc &desc,
                           vita_gpu_readback &output);
    Status destroy_readback(vita_gpu_readback handle);
    Status map_readback(vita_gpu_readback handle,
                        vita_gpu_readback_map &output);
    Status unmap_readback(vita_gpu_readback handle);

    Status create_command_list(const vita_gpu_command_list_desc &desc,
                               vita_gpu_command_list &output);
    Status destroy_command_list(vita_gpu_command_list handle);
    Status command_begin(vita_gpu_command_list handle);
    Status command_end(vita_gpu_command_list handle);
    Status command_reset(vita_gpu_command_list handle);
    Status command_barriers(
        vita_gpu_command_list handle,
        std::span<const vita_gpu_resource_barrier> barriers);
    Status command_begin_render_pass(vita_gpu_command_list handle,
                                     const vita_gpu_render_pass_desc &desc);
    Status command_end_render_pass(vita_gpu_command_list handle);
    Status command_bind_pipeline(vita_gpu_command_list handle,
                                 vita_gpu_pipeline pipeline);
    Status command_bind_set(vita_gpu_command_list handle,
                            std::uint32_t set_index, vita_gpu_binding_set set);
    Status command_set_viewport(vita_gpu_command_list handle,
                                const vita_gpu_viewport &viewport);
    Status command_set_scissor(vita_gpu_command_list handle,
                               const vita_gpu_scissor &scissor);
    Status command_set_vertex_buffers(
        vita_gpu_command_list handle,
        std::span<const vita_gpu_vertex_buffer_binding> bindings);
    Status command_set_index_buffer(
        vita_gpu_command_list handle,
        const vita_gpu_index_buffer_binding &binding);
    Status command_push_constants(vita_gpu_command_list handle,
                                  const vita_gpu_push_constants_desc &desc);
    Status command_draw(vita_gpu_command_list handle,
                        const vita_gpu_draw_desc &desc);
    Status command_draw_indexed(vita_gpu_command_list handle,
                                const vita_gpu_draw_indexed_desc &desc);
    Status command_dispatch(vita_gpu_command_list handle,
                            const vita_gpu_dispatch_desc &desc);
    Status command_copy_buffer(vita_gpu_command_list handle,
                               const vita_gpu_buffer_copy_desc &desc);
    Status command_copy_buffer_readback(
        vita_gpu_command_list handle,
        const vita_gpu_buffer_readback_copy_desc &desc);
    Status command_copy_texture_readback(
        vita_gpu_command_list handle,
        const vita_gpu_texture_readback_copy_desc &desc);
    Status command_debug_marker(vita_gpu_command_list handle,
                                std::string_view label,
                                std::uint32_t rgba_color);

    Status create_fence(const vita_gpu_fence_desc &desc,
                        vita_gpu_fence &output);
    Status destroy_fence(vita_gpu_fence handle);
    Status fence_completed(vita_gpu_fence handle, std::uint64_t &output);
    Status fence_wait(vita_gpu_fence handle, std::uint64_t value,
                      std::uint64_t timeout_nanoseconds);
    Status submit(vita_gpu_queue_type queue, const vita_gpu_submit_desc &desc);
    Status debug_capture_begin(const vita_gpu_debug_capture_desc &desc);
    Status debug_capture_end();

private:
    Status add_reference(CommandListRecord &command, ResourceRef reference);
    void release_command_references(CommandListRecord &command);
    Status add_dependency(ResourceRef reference);
    void release_dependency(ResourceRef reference);
    Status resource_state(ResourceRef reference,
                          vita_gpu_resource_state *&state);
    Status resource_queue(ResourceRef reference, vita_gpu_queue_type *&queue);
    Status command_state(CommandListRecord &command, ResourceRef reference,
                         vita_gpu_resource_state &state);
    Status command_queue(CommandListRecord &command, ResourceRef reference,
                         vita_gpu_queue_type &queue);
    Status validate_command_resource_queue(CommandListRecord &command,
                                           ResourceRef reference);
    Status validate_submitted_state(
        std::span<CommandListRecord *const> command_lists,
        std::vector<PredictedState> &updates);
    Status reset_submitted(CommandListRecord &command);
    bool swapchain_images_busy(const SwapchainRecord &swapchain);
    void clear_swapchain_images(SwapchainRecord &swapchain);
    std::size_t child_count() const;

    std::unique_ptr<BackendDevice> backend_;
    std::mutex mutex_;
    HandlePool<SwapchainRecord> swapchains_;
    HandlePool<BufferRecord> buffers_;
    HandlePool<TextureRecord> textures_;
    HandlePool<SamplerRecord> samplers_;
    HandlePool<RenderTargetRecord> render_targets_;
    HandlePool<DepthTargetRecord> depth_targets_;
    HandlePool<ShaderRecord> shaders_;
    HandlePool<BindingLayoutRecord> binding_layouts_;
    HandlePool<BindingSetRecord> binding_sets_;
    HandlePool<PipelineRecord> pipelines_;
    HandlePool<CommandListRecord> commands_;
    HandlePool<FenceRecord> fences_;
    HandlePool<ReadbackRecord> readbacks_;
    bool capture_active_ = false;
};

std::size_t DeviceCore::child_count() const {
    return swapchains_.size() + buffers_.size() + textures_.size() +
           samplers_.size() + render_targets_.size() + depth_targets_.size() +
           shaders_.size() + binding_layouts_.size() + binding_sets_.size() +
           pipelines_.size() + commands_.size() + fences_.size() +
           readbacks_.size();
}

Status DeviceCore::can_destroy() {
    std::scoped_lock lock(mutex_);
    if (capture_active_) {
        return Status::error(VITA_GPU_ERROR_RESOURCE_IN_USE,
                             "debug capture is still active",
                             backend_->backend());
    }
    if (child_count() != 0) {
        return Status::error(
            VITA_GPU_ERROR_RESOURCE_IN_USE,
            "device still owns " + std::to_string(child_count()) +
                " object(s); destroy children in reverse creation order",
            backend_->backend());
    }
    return backend_->wait_idle();
}

Status DeviceCore::capabilities(vita_gpu_device_capabilities &output) {
    Status status =
        validate_output_struct(&output, sizeof(output), "device_capabilities");
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    return backend_->capabilities(output);
}

Status DeviceCore::wait_idle() {
    std::scoped_lock lock(mutex_);
    return backend_->wait_idle();
}

Status DeviceCore::create_buffer(const vita_gpu_buffer_desc &desc,
                                 vita_gpu_buffer &output) {
    Status status = validate_buffer_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_buffer(desc, native);
    if (!status.ok()) {
        return status;
    }
    if (native.value == 0) {
        return Status::error(VITA_GPU_ERROR_INTERNAL,
                             "backend returned an invalid buffer handle",
                             backend_->backend());
    }
    output = public_handle<vita_gpu_buffer>(buffers_.insert(
        BufferRecord{native, desc.byte_size, desc.usage, desc.initial_state,
                     VITA_GPU_QUEUE_GRAPHICS, {}}));
    return Status::success();
}

Status DeviceCore::destroy_buffer(vita_gpu_buffer handle) {
    std::scoped_lock lock(mutex_);
    BufferRecord *record = nullptr;
    Status status = resolve(buffers_, handle.value, record, "buffer");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "buffer");
    if (!status.ok()) {
        return status;
    }
    status = backend_->destroy_object(ObjectKind::buffer, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)buffers_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::create_texture(const vita_gpu_texture_desc &desc,
                                  vita_gpu_texture &output) {
    Status status = validate_texture_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_texture(desc, native);
    if (!status.ok()) {
        return status;
    }
    if (native.value == 0) {
        return Status::error(VITA_GPU_ERROR_INTERNAL,
                             "backend returned an invalid texture handle",
                             backend_->backend());
    }
    output = public_handle<vita_gpu_texture>(textures_.insert(TextureRecord{
        native, desc.width, desc.height, desc.depth_or_layers, desc.mip_levels,
        desc.sample_count, desc.format, desc.usage, desc.initial_state,
        VITA_GPU_QUEUE_GRAPHICS, {}}));
    return Status::success();
}

Status DeviceCore::destroy_texture(vita_gpu_texture handle) {
    std::scoped_lock lock(mutex_);
    TextureRecord *record = nullptr;
    Status status = resolve(textures_, handle.value, record, "texture");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "texture");
    if (!status.ok()) {
        return status;
    }
    status = backend_->destroy_object(ObjectKind::texture, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)textures_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::create_sampler(const vita_gpu_sampler_desc &desc,
                                  vita_gpu_sampler &output) {
    Status status = validate_sampler_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_sampler(desc, native);
    if (!status.ok()) {
        return status;
    }
    status =
        validate_native_handle(native, "sampler", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    output = public_handle<vita_gpu_sampler>(
        samplers_.insert(SamplerRecord{native, {}}));
    return Status::success();
}

Status DeviceCore::destroy_sampler(vita_gpu_sampler handle) {
    std::scoped_lock lock(mutex_);
    SamplerRecord *record = nullptr;
    Status status = resolve(samplers_, handle.value, record, "sampler");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "sampler");
    if (!status.ok()) {
        return status;
    }
    status = backend_->destroy_object(ObjectKind::sampler, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)samplers_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::create_render_target(
    const vita_gpu_render_target_desc &desc, vita_gpu_render_target &output) {
    Status status = validate_render_target_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    TextureRecord *texture = nullptr;
    status = resolve(textures_, desc.texture.value, texture, "texture");
    if (!status.ok()) {
        return status;
    }
    if ((texture->usage & VITA_GPU_TEXTURE_RENDER_TARGET) == 0 ||
        depth_format(texture->format) || desc.format != texture->format ||
        desc.mip_level >= texture->mip_levels ||
        desc.first_array_layer >= texture->depth_or_layers ||
        desc.array_layer_count >
            texture->depth_or_layers - desc.first_array_layer) {
        return invalid("render target is incompatible with its texture");
    }
    NativeId native;
    status = backend_->create_render_target(desc, texture->native, native);
    if (!status.ok()) {
        return status;
    }
    status = validate_native_handle(native, "render target",
                                    backend_->backend());
    if (!status.ok()) {
        return status;
    }
    ++texture->use.dependencies;
    output = public_handle<vita_gpu_render_target>(
        render_targets_.insert(RenderTargetRecord{
            native, desc.texture.value, desc.format, {}}));
    return Status::success();
}

Status DeviceCore::destroy_render_target(vita_gpu_render_target handle) {
    std::scoped_lock lock(mutex_);
    RenderTargetRecord *record = nullptr;
    Status status =
        resolve(render_targets_, handle.value, record, "render target");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "render target");
    if (!status.ok()) {
        return status;
    }
    const std::uint64_t texture = record->texture;
    status =
        backend_->destroy_object(ObjectKind::render_target, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)render_targets_.remove(handle.value);
    release_dependency({ObjectKind::texture, texture});
    return Status::success();
}

Status DeviceCore::create_depth_target(const vita_gpu_depth_target_desc &desc,
                                       vita_gpu_depth_target &output) {
    Status status = validate_depth_target_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    TextureRecord *texture = nullptr;
    status = resolve(textures_, desc.texture.value, texture, "texture");
    if (!status.ok()) {
        return status;
    }
    if ((texture->usage & VITA_GPU_TEXTURE_DEPTH_TARGET) == 0 ||
        !depth_format(texture->format) || desc.format != texture->format ||
        desc.mip_level >= texture->mip_levels ||
        desc.first_array_layer >= texture->depth_or_layers ||
        desc.array_layer_count >
            texture->depth_or_layers - desc.first_array_layer) {
        return invalid("depth target is incompatible with its texture");
    }
    NativeId native;
    status = backend_->create_depth_target(desc, texture->native, native);
    if (!status.ok()) {
        return status;
    }
    status =
        validate_native_handle(native, "depth target", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    ++texture->use.dependencies;
    output = public_handle<vita_gpu_depth_target>(
        depth_targets_.insert(DepthTargetRecord{
            native, desc.texture.value, desc.format,
            desc.read_only_depth == VITA_GPU_TRUE, {}}));
    return Status::success();
}

Status DeviceCore::destroy_depth_target(vita_gpu_depth_target handle) {
    std::scoped_lock lock(mutex_);
    DepthTargetRecord *record = nullptr;
    Status status =
        resolve(depth_targets_, handle.value, record, "depth target");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "depth target");
    if (!status.ok()) {
        return status;
    }
    const std::uint64_t texture = record->texture;
    status = backend_->destroy_object(ObjectKind::depth_target, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)depth_targets_.remove(handle.value);
    release_dependency({ObjectKind::texture, texture});
    return Status::success();
}

Status DeviceCore::create_shader(const vita_gpu_shader_module_desc &desc,
                                 vita_gpu_shader_module &output) {
    Status status = validate_shader_module_desc(desc);
    if (!status.ok()) {
        return status;
    }
    const vita_gpu_shader_stage stage =
        desc.input == VITA_GPU_SHADER_INPUT_HLSL ? desc.hlsl->stage
                                                : desc.binary->stage;
    const vita_gpu_shader_target target =
        desc.input == VITA_GPU_SHADER_INPUT_HLSL ? desc.hlsl->target
                                                : desc.binary->target;
    if ((backend_->backend() == VITA_GPU_BACKEND_DX12 &&
         target != VITA_GPU_SHADER_TARGET_DXIL) ||
        (backend_->backend() == VITA_GPU_BACKEND_VULKAN &&
         target != VITA_GPU_SHADER_TARGET_SPIRV)) {
        return invalid("shader target does not match the selected backend");
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_shader_module(desc, native);
    if (!status.ok()) {
        return status;
    }
    status =
        validate_native_handle(native, "shader module", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    output = public_handle<vita_gpu_shader_module>(
        shaders_.insert(ShaderRecord{native, stage, target, {}}));
    return Status::success();
}

Status DeviceCore::destroy_shader(vita_gpu_shader_module handle) {
    std::scoped_lock lock(mutex_);
    ShaderRecord *record = nullptr;
    Status status = resolve(shaders_, handle.value, record, "shader module");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "shader module");
    if (!status.ok()) {
        return status;
    }
    status = backend_->destroy_object(ObjectKind::shader, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)shaders_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::create_binding_layout(
    const vita_gpu_binding_layout_desc &desc, vita_gpu_binding_layout &output) {
    Status status = validate_binding_layout_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_binding_layout(desc, native);
    if (!status.ok()) {
        return status;
    }
    status = validate_native_handle(native, "binding layout",
                                    backend_->backend());
    if (!status.ok()) {
        return status;
    }
    std::vector<vita_gpu_binding_layout_entry> entries;
    if (desc.entry_count != 0) {
        entries.assign(desc.entries, desc.entries + desc.entry_count);
    }
    output = public_handle<vita_gpu_binding_layout>(
        binding_layouts_.insert(BindingLayoutRecord{
            native, desc.set_index, std::move(entries), {}}));
    return Status::success();
}

Status DeviceCore::destroy_binding_layout(vita_gpu_binding_layout handle) {
    std::scoped_lock lock(mutex_);
    BindingLayoutRecord *record = nullptr;
    Status status =
        resolve(binding_layouts_, handle.value, record, "binding layout");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "binding layout");
    if (!status.ok()) {
        return status;
    }
    status =
        backend_->destroy_object(ObjectKind::binding_layout, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)binding_layouts_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::create_binding_set(const vita_gpu_binding_set_desc &desc,
                                      vita_gpu_binding_set &output) {
    Status status = validate_struct(&desc, sizeof(desc), "binding_set_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.layout.value == 0) {
        return invalid("binding set requires a layout");
    }
    std::scoped_lock lock(mutex_);
    BindingLayoutRecord *layout = nullptr;
    status =
        resolve(binding_layouts_, desc.layout.value, layout, "binding layout");
    if (!status.ok()) {
        return status;
    }
    NativeId native;
    status = backend_->create_binding_set(desc, layout->native, native);
    if (!status.ok()) {
        return status;
    }
    status =
        validate_native_handle(native, "binding set", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    ++layout->use.dependencies;
    output = public_handle<vita_gpu_binding_set>(
        binding_sets_.insert(BindingSetRecord{
            native, desc.layout.value, {}, {}}));
    return Status::success();
}

Status DeviceCore::update_binding_set(
    vita_gpu_binding_set handle,
    std::span<const vita_gpu_binding_write> writes) {
    if (writes.empty()) {
        return invalid("binding update requires at least one write");
    }
    std::scoped_lock lock(mutex_);
    BindingSetRecord *set = nullptr;
    Status status = resolve(binding_sets_, handle.value, set, "binding set");
    if (!status.ok()) {
        return status;
    }
    if (set->use.command_lists != 0) {
        return Status::error(VITA_GPU_ERROR_RESOURCE_IN_USE,
                             "binding set is referenced by a command list");
    }
    BindingLayoutRecord *layout = nullptr;
    status =
        resolve(binding_layouts_, set->layout, layout, "binding layout");
    if (!status.ok()) {
        return status;
    }

    std::vector<BackendBindingWrite> backend_writes;
    std::vector<BoundSlot> replacements;
    backend_writes.reserve(writes.size());
    replacements.reserve(writes.size());
    std::set<std::pair<std::uint32_t, std::uint32_t>> destinations;
    for (const auto &write : writes) {
        status = validate_binding_write(write);
        if (!status.ok()) {
            return status;
        }
        const auto layout_entry = std::find_if(
            layout->entries.begin(), layout->entries.end(),
            [&write](const auto &entry) { return entry.binding == write.binding; });
        if (layout_entry == layout->entries.end() ||
            layout_entry->type != write.type ||
            write.array_element >= layout_entry->array_count ||
            !destinations.emplace(write.binding, write.array_element).second) {
            return invalid(
                "binding write does not match the layout or is duplicated");
        }

        BackendBindingWrite translated;
        translated.binding = write.binding;
        translated.array_element = write.array_element;
        translated.type = write.type;
        translated.buffer_offset = write.buffer_offset;
        translated.buffer_size = write.buffer_size;
        translated.texture_state = write.texture_state;
        ResourceRef reference;
        vita_gpu_resource_state required_state =
            VITA_GPU_RESOURCE_STATE_UNDEFINED;
        if (write.type == VITA_GPU_BINDING_CONSTANT_BUFFER ||
            write.type == VITA_GPU_BINDING_STORAGE_BUFFER) {
            BufferRecord *buffer = nullptr;
            status = resolve(buffers_, write.buffer.value, buffer, "buffer");
            if (!status.ok()) {
                return status;
            }
            if (write.buffer_offset > buffer->size ||
                write.buffer_size > buffer->size - write.buffer_offset ||
                (write.type == VITA_GPU_BINDING_CONSTANT_BUFFER &&
                 (buffer->usage & VITA_GPU_BUFFER_CONSTANT) == 0) ||
                (write.type == VITA_GPU_BINDING_STORAGE_BUFFER &&
                 (buffer->usage & VITA_GPU_BUFFER_STORAGE) == 0)) {
                return invalid("buffer binding range or usage is invalid");
            }
            translated.buffer = buffer->native;
            reference = {ObjectKind::buffer, write.buffer.value};
            required_state =
                write.type == VITA_GPU_BINDING_CONSTANT_BUFFER
                    ? VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER
                    : VITA_GPU_RESOURCE_STATE_SHADER_WRITE;
        } else if (write.type == VITA_GPU_BINDING_SAMPLED_TEXTURE ||
                   write.type == VITA_GPU_BINDING_STORAGE_TEXTURE) {
            TextureRecord *texture = nullptr;
            status =
                resolve(textures_, write.texture.value, texture, "texture");
            if (!status.ok()) {
                return status;
            }
            const vita_gpu_texture_usage required =
                write.type == VITA_GPU_BINDING_SAMPLED_TEXTURE
                    ? VITA_GPU_TEXTURE_SAMPLED
                    : VITA_GPU_TEXTURE_STORAGE;
            const vita_gpu_resource_state required_texture_state =
                write.type == VITA_GPU_BINDING_SAMPLED_TEXTURE
                    ? VITA_GPU_RESOURCE_STATE_SHADER_READ
                    : VITA_GPU_RESOURCE_STATE_SHADER_WRITE;
            if ((texture->usage & required) == 0 ||
                !has_state(write.texture_state, required_texture_state)) {
                return invalid("texture binding usage or state is invalid");
            }
            translated.texture = texture->native;
            reference = {ObjectKind::texture, write.texture.value};
            required_state = required_texture_state;
        } else {
            SamplerRecord *sampler = nullptr;
            status =
                resolve(samplers_, write.sampler.value, sampler, "sampler");
            if (!status.ok()) {
                return status;
            }
            translated.sampler = sampler->native;
            reference = {ObjectKind::sampler, write.sampler.value};
        }
        backend_writes.push_back(translated);
        replacements.push_back(
            BoundSlot{write.binding, write.array_element, reference,
                      required_state});
    }

    status = backend_->update_binding_set(set->native, backend_writes);
    if (!status.ok()) {
        return status;
    }
    for (const BoundSlot &replacement : replacements) {
        auto existing = std::find_if(
            set->slots.begin(), set->slots.end(),
            [&replacement](const BoundSlot &slot) {
                return slot.binding == replacement.binding &&
                       slot.array_element == replacement.array_element;
            });
        if (existing != set->slots.end()) {
            release_dependency(existing->resource);
            *existing = replacement;
        } else {
            set->slots.push_back(replacement);
        }
        status = add_dependency(replacement.resource);
        if (!status.ok()) {
            return Status::error(
                VITA_GPU_ERROR_INTERNAL,
                "binding dependency disappeared while the device was locked",
                backend_->backend());
        }
    }
    return Status::success();
}

Status DeviceCore::destroy_binding_set(vita_gpu_binding_set handle) {
    std::scoped_lock lock(mutex_);
    BindingSetRecord *record = nullptr;
    Status status =
        resolve(binding_sets_, handle.value, record, "binding set");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "binding set");
    if (!status.ok()) {
        return status;
    }
    const std::uint64_t layout = record->layout;
    const std::vector<BoundSlot> slots = record->slots;
    status =
        backend_->destroy_object(ObjectKind::binding_set, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)binding_sets_.remove(handle.value);
    for (const BoundSlot &slot : slots) {
        release_dependency(slot.resource);
    }
    release_dependency({ObjectKind::binding_layout, layout});
    return Status::success();
}

Status DeviceCore::create_graphics_pipeline(
    const vita_gpu_graphics_pipeline_desc &desc, vita_gpu_pipeline &output) {
    Status status = validate_graphics_pipeline_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    ShaderRecord *vertex = nullptr;
    ShaderRecord *pixel = nullptr;
    status = resolve(shaders_, desc.vertex_shader.value, vertex, "vertex shader");
    if (!status.ok()) {
        return status;
    }
    status = resolve(shaders_, desc.pixel_shader.value, pixel, "pixel shader");
    if (!status.ok()) {
        return status;
    }
    if (vertex->stage != VITA_GPU_SHADER_VERTEX ||
        pixel->stage != VITA_GPU_SHADER_PIXEL) {
        return invalid("graphics pipeline shader stages are incorrect");
    }

    BackendGraphicsPipelineDesc translated;
    translated.source = &desc;
    translated.vertex_shader = vertex->native;
    translated.pixel_shader = pixel->native;
    std::vector<ResourceRef> dependencies{
        {ObjectKind::shader, desc.vertex_shader.value},
        {ObjectKind::shader, desc.pixel_shader.value}};
    std::set<std::uint32_t> layout_sets;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pipeline_layouts;
    for (std::uint32_t i = 0; i < desc.binding_layout_count; ++i) {
        BindingLayoutRecord *layout = nullptr;
        status = resolve(binding_layouts_, desc.binding_layouts[i].value,
                         layout, "binding layout");
        if (!status.ok()) {
            return status;
        }
        if (!layout_sets.insert(layout->set_index).second) {
            return invalid(
                "graphics pipeline binding layout set indices must be unique");
        }
        translated.binding_layouts.push_back(layout->native);
        pipeline_layouts.emplace_back(layout->set_index,
                                      desc.binding_layouts[i].value);
        dependencies.push_back(
            {ObjectKind::binding_layout, desc.binding_layouts[i].value});
    }

    NativeId native;
    status = backend_->create_graphics_pipeline(translated, native);
    if (!status.ok()) {
        return status;
    }
    status = validate_native_handle(native, "graphics pipeline",
                                    backend_->backend());
    if (!status.ok()) {
        return status;
    }
    for (const ResourceRef dependency : dependencies) {
        (void)add_dependency(dependency);
    }
    output = public_handle<vita_gpu_pipeline>(
        pipelines_.insert(PipelineRecord{
            native, true, desc.push_constant_bytes, std::move(pipeline_layouts),
            std::move(dependencies), {}}));
    return Status::success();
}

Status DeviceCore::create_compute_pipeline(
    const vita_gpu_compute_pipeline_desc &desc, vita_gpu_pipeline &output) {
    Status status = validate_compute_pipeline_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    ShaderRecord *compute = nullptr;
    status =
        resolve(shaders_, desc.compute_shader.value, compute, "compute shader");
    if (!status.ok()) {
        return status;
    }
    if (compute->stage != VITA_GPU_SHADER_COMPUTE) {
        return invalid("compute pipeline requires a compute shader");
    }
    BackendComputePipelineDesc translated;
    translated.source = &desc;
    translated.compute_shader = compute->native;
    std::vector<ResourceRef> dependencies{
        {ObjectKind::shader, desc.compute_shader.value}};
    std::set<std::uint32_t> layout_sets;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pipeline_layouts;
    for (std::uint32_t i = 0; i < desc.binding_layout_count; ++i) {
        BindingLayoutRecord *layout = nullptr;
        status = resolve(binding_layouts_, desc.binding_layouts[i].value,
                         layout, "binding layout");
        if (!status.ok()) {
            return status;
        }
        if (!layout_sets.insert(layout->set_index).second) {
            return invalid(
                "compute pipeline binding layout set indices must be unique");
        }
        translated.binding_layouts.push_back(layout->native);
        pipeline_layouts.emplace_back(layout->set_index,
                                      desc.binding_layouts[i].value);
        dependencies.push_back(
            {ObjectKind::binding_layout, desc.binding_layouts[i].value});
    }
    NativeId native;
    status = backend_->create_compute_pipeline(translated, native);
    if (!status.ok()) {
        return status;
    }
    status = validate_native_handle(native, "compute pipeline",
                                    backend_->backend());
    if (!status.ok()) {
        return status;
    }
    for (const ResourceRef dependency : dependencies) {
        (void)add_dependency(dependency);
    }
    output = public_handle<vita_gpu_pipeline>(
        pipelines_.insert(PipelineRecord{
            native, false, desc.push_constant_bytes, std::move(pipeline_layouts),
            std::move(dependencies), {}}));
    return Status::success();
}

Status DeviceCore::destroy_pipeline(vita_gpu_pipeline handle) {
    std::scoped_lock lock(mutex_);
    PipelineRecord *record = nullptr;
    Status status = resolve(pipelines_, handle.value, record, "pipeline");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "pipeline");
    if (!status.ok()) {
        return status;
    }
    const std::vector<ResourceRef> dependencies = record->dependencies;
    status = backend_->destroy_object(ObjectKind::pipeline, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)pipelines_.remove(handle.value);
    for (const ResourceRef dependency : dependencies) {
        release_dependency(dependency);
    }
    return Status::success();
}

Status DeviceCore::create_readback(const vita_gpu_readback_desc &desc,
                                   vita_gpu_readback &output) {
    Status status = validate_struct(&desc, sizeof(desc), "readback_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.byte_size == 0) {
        return invalid("readback byte_size must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_readback(desc, native);
    if (!status.ok()) {
        return status;
    }
    status =
        validate_native_handle(native, "readback", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    output = public_handle<vita_gpu_readback>(
        readbacks_.insert(ReadbackRecord{native, desc.byte_size, false, {}}));
    return Status::success();
}

Status DeviceCore::destroy_readback(vita_gpu_readback handle) {
    std::scoped_lock lock(mutex_);
    ReadbackRecord *record = nullptr;
    Status status = resolve(readbacks_, handle.value, record, "readback");
    if (!status.ok()) {
        return status;
    }
    if (record->mapped) {
        return state_error("readback must be unmapped before destruction");
    }
    status = ensure_destroyable(*record, "readback");
    if (!status.ok()) {
        return status;
    }
    status = backend_->destroy_object(ObjectKind::readback, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)readbacks_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::map_readback(vita_gpu_readback handle,
                                vita_gpu_readback_map &output) {
    Status status =
        validate_output_struct(&output, sizeof(output), "readback_map");
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    ReadbackRecord *record = nullptr;
    status = resolve(readbacks_, handle.value, record, "readback");
    if (!status.ok()) {
        return status;
    }
    if (record->mapped) {
        return state_error("readback is already mapped");
    }
    if (record->use.command_lists != 0) {
        return Status::error(
            VITA_GPU_ERROR_RESOURCE_IN_USE,
            "readback is still referenced by a command list; wait and reset it before mapping");
    }
    BackendReadbackMap map;
    status = backend_->map_readback(record->native, map);
    if (!status.ok()) {
        return status;
    }
    if (map.data == nullptr || map.byte_size > record->size) {
        (void)backend_->unmap_readback(record->native);
        return Status::error(VITA_GPU_ERROR_INTERNAL,
                             "backend returned an invalid readback mapping",
                             backend_->backend());
    }
    output.data = map.data;
    output.byte_size = map.byte_size;
    output.row_pitch = map.row_pitch;
    output.slice_pitch = map.slice_pitch;
    record->mapped = true;
    return Status::success();
}

Status DeviceCore::unmap_readback(vita_gpu_readback handle) {
    std::scoped_lock lock(mutex_);
    ReadbackRecord *record = nullptr;
    Status status = resolve(readbacks_, handle.value, record, "readback");
    if (!status.ok()) {
        return status;
    }
    if (!record->mapped) {
        return state_error("readback is not mapped");
    }
    status = backend_->unmap_readback(record->native);
    if (status.ok()) {
        record->mapped = false;
    }
    return status;
}

Status DeviceCore::add_dependency(ResourceRef reference) {
    switch (reference.kind) {
    case ObjectKind::swapchain: {
        SwapchainRecord *record = nullptr;
        Status status =
            resolve(swapchains_, reference.handle, record, "swapchain");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::buffer: {
        BufferRecord *record = nullptr;
        Status status = resolve(buffers_, reference.handle, record, "buffer");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::texture: {
        TextureRecord *record = nullptr;
        Status status = resolve(textures_, reference.handle, record, "texture");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::sampler: {
        SamplerRecord *record = nullptr;
        Status status = resolve(samplers_, reference.handle, record, "sampler");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::render_target: {
        RenderTargetRecord *record = nullptr;
        Status status = resolve(render_targets_, reference.handle, record,
                                "render target");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::depth_target: {
        DepthTargetRecord *record = nullptr;
        Status status =
            resolve(depth_targets_, reference.handle, record, "depth target");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::shader: {
        ShaderRecord *record = nullptr;
        Status status =
            resolve(shaders_, reference.handle, record, "shader module");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::binding_layout: {
        BindingLayoutRecord *record = nullptr;
        Status status = resolve(binding_layouts_, reference.handle, record,
                                "binding layout");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::binding_set: {
        BindingSetRecord *record = nullptr;
        Status status =
            resolve(binding_sets_, reference.handle, record, "binding set");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::pipeline: {
        PipelineRecord *record = nullptr;
        Status status =
            resolve(pipelines_, reference.handle, record, "pipeline");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::fence: {
        FenceRecord *record = nullptr;
        Status status = resolve(fences_, reference.handle, record, "fence");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    case ObjectKind::readback: {
        ReadbackRecord *record = nullptr;
        Status status =
            resolve(readbacks_, reference.handle, record, "readback");
        if (status.ok()) {
            ++record->use.dependencies;
        }
        return status;
    }
    }
    return Status::error(VITA_GPU_ERROR_INTERNAL,
                         "unknown dependency object kind");
}

void DeviceCore::release_dependency(ResourceRef reference) {
    const auto decrement = [](UseState &use) {
        if (use.dependencies != 0) {
            --use.dependencies;
        }
    };
    switch (reference.kind) {
    case ObjectKind::swapchain: {
        const auto found = swapchains_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::buffer: {
        const auto found = buffers_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::texture: {
        const auto found = textures_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::sampler: {
        const auto found = samplers_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::render_target: {
        const auto found = render_targets_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::depth_target: {
        const auto found = depth_targets_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::shader: {
        const auto found = shaders_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::binding_layout: {
        const auto found = binding_layouts_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::binding_set: {
        const auto found = binding_sets_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::pipeline: {
        const auto found = pipelines_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::fence: {
        const auto found = fences_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    case ObjectKind::readback: {
        const auto found = readbacks_.find(reference.handle);
        if (found) {
            decrement(found.value->use);
        }
        break;
    }
    }
}

Status DeviceCore::add_reference(CommandListRecord &command,
                                 ResourceRef reference) {
    if (std::find(command.references.begin(), command.references.end(),
                  reference) != command.references.end()) {
        return Status::success();
    }
    Status status = add_dependency(reference);
    if (!status.ok()) {
        return status;
    }
    /*
     * Command references have their own counter so diagnostics can distinguish
     * recorded work from long-lived object dependencies.
     */
    release_dependency(reference);
    switch (reference.kind) {
    case ObjectKind::buffer:
        ++buffers_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::texture:
        ++textures_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::sampler:
        ++samplers_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::render_target:
        ++render_targets_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::depth_target:
        ++depth_targets_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::shader:
        ++shaders_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::binding_layout:
        ++binding_layouts_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::binding_set:
        ++binding_sets_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::pipeline:
        ++pipelines_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::readback:
        ++readbacks_.find(reference.handle).value->use.command_lists;
        break;
    case ObjectKind::fence:
    case ObjectKind::swapchain:
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "fences and swapchains cannot be command resource references");
    }
    command.references.push_back(reference);
    return Status::success();
}

void DeviceCore::release_command_references(CommandListRecord &command) {
    const auto decrement = [](UseState &use) {
        if (use.command_lists != 0) {
            --use.command_lists;
        }
    };
    for (const ResourceRef reference : command.references) {
        switch (reference.kind) {
        case ObjectKind::buffer: {
            const auto found = buffers_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::texture: {
            const auto found = textures_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::sampler: {
            const auto found = samplers_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::render_target: {
            const auto found = render_targets_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::depth_target: {
            const auto found = depth_targets_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::shader: {
            const auto found = shaders_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::binding_layout: {
            const auto found = binding_layouts_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::binding_set: {
            const auto found = binding_sets_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::pipeline: {
            const auto found = pipelines_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::readback: {
            const auto found = readbacks_.find(reference.handle);
            if (found) {
                decrement(found.value->use);
            }
            break;
        }
        case ObjectKind::fence:
        case ObjectKind::swapchain:
            break;
        }
    }
    command.references.clear();
    command.predicted_states.clear();
}

Status DeviceCore::resource_state(ResourceRef reference,
                                  vita_gpu_resource_state *&state) {
    if (reference.kind == ObjectKind::buffer) {
        BufferRecord *record = nullptr;
        Status status =
            resolve(buffers_, reference.handle, record, "buffer");
        if (status.ok()) {
            state = &record->state;
        }
        return status;
    }
    if (reference.kind == ObjectKind::texture) {
        TextureRecord *record = nullptr;
        Status status =
            resolve(textures_, reference.handle, record, "texture");
        if (status.ok()) {
            state = &record->state;
        }
        return status;
    }
    return Status::error(VITA_GPU_ERROR_INTERNAL,
                         "object does not carry resource state");
}

Status DeviceCore::resource_queue(ResourceRef reference,
                                  vita_gpu_queue_type *&queue) {
    if (reference.kind == ObjectKind::buffer) {
        BufferRecord *record = nullptr;
        Status status =
            resolve(buffers_, reference.handle, record, "buffer");
        if (status.ok()) {
            queue = &record->queue;
        }
        return status;
    }
    if (reference.kind == ObjectKind::texture) {
        TextureRecord *record = nullptr;
        Status status =
            resolve(textures_, reference.handle, record, "texture");
        if (status.ok()) {
            queue = &record->queue;
        }
        return status;
    }
    return Status::error(VITA_GPU_ERROR_INTERNAL,
                         "object does not carry queue ownership");
}

Status DeviceCore::command_state(CommandListRecord &command,
                                 ResourceRef reference,
                                 vita_gpu_resource_state &state) {
    const auto predicted = std::find_if(
        command.predicted_states.begin(), command.predicted_states.end(),
        [reference](const PredictedState &entry) {
            return entry.resource == reference;
        });
    if (predicted != command.predicted_states.end()) {
        state = predicted->state;
        return Status::success();
    }
    vita_gpu_resource_state *global = nullptr;
    Status status = resource_state(reference, global);
    if (status.ok()) {
        state = *global;
    }
    return status;
}

Status DeviceCore::command_queue(CommandListRecord &command,
                                 ResourceRef reference,
                                 vita_gpu_queue_type &queue) {
    const auto predicted = std::find_if(
        command.predicted_states.begin(), command.predicted_states.end(),
        [reference](const PredictedState &entry) {
            return entry.resource == reference;
        });
    if (predicted != command.predicted_states.end()) {
        queue = predicted->queue;
        return Status::success();
    }
    vita_gpu_queue_type *global = nullptr;
    Status status = resource_queue(reference, global);
    if (status.ok()) {
        queue = *global;
    }
    return status;
}

Status DeviceCore::validate_command_resource_queue(
    CommandListRecord &command, ResourceRef reference) {
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
    Status status = command_queue(command, reference, queue);
    if (!status.ok()) {
        return status;
    }
    if (queue != command.recorded.queue) {
        return state_error(
            "resource is owned by a different queue; record an ownership transition");
    }
    return Status::success();
}

Status DeviceCore::create_swapchain(const vita_gpu_swapchain_desc &desc,
                                    vita_gpu_swapchain &output) {
    Status status = validate_swapchain_desc(desc);
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_swapchain(desc, native);
    if (!status.ok()) {
        return status;
    }
    status =
        validate_native_handle(native, "swapchain", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    output = public_handle<vita_gpu_swapchain>(
        swapchains_.insert(SwapchainRecord{
            native, desc.width, desc.height, desc.format, false, 0, {}, {}}));
    return Status::success();
}

bool DeviceCore::swapchain_images_busy(const SwapchainRecord &swapchain) {
    for (const SwapchainImageRecord &image : swapchain.images) {
        const auto target = render_targets_.find(image.target.value);
        const auto texture = textures_.find(image.texture.value);
        if (!target || !texture || target.value->use.command_lists != 0 ||
            target.value->use.dependencies != 0 ||
            texture.value->use.command_lists != 0 ||
            texture.value->use.dependencies > 1) {
            return true;
        }
    }
    return false;
}

void DeviceCore::clear_swapchain_images(SwapchainRecord &swapchain) {
    for (const SwapchainImageRecord &image : swapchain.images) {
        const auto target = render_targets_.remove(image.target.value);
        if (target.first.has_value()) {
            release_dependency(
                {ObjectKind::texture, target.first->texture});
        }
        (void)textures_.remove(image.texture.value);
    }
    swapchain.images.clear();
}

Status DeviceCore::destroy_swapchain(vita_gpu_swapchain handle) {
    std::scoped_lock lock(mutex_);
    SwapchainRecord *record = nullptr;
    Status status =
        resolve(swapchains_, handle.value, record, "swapchain");
    if (!status.ok()) {
        return status;
    }
    if (record->acquired) {
        return state_error("swapchain image must be presented before destruction");
    }
    if (record->use.busy() || swapchain_images_busy(*record)) {
        return Status::error(VITA_GPU_ERROR_RESOURCE_IN_USE,
                             "swapchain or one of its images is still referenced");
    }
    status = backend_->destroy_object(ObjectKind::swapchain, record->native);
    if (!status.ok()) {
        return status;
    }
    clear_swapchain_images(*record);
    (void)swapchains_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::acquire_swapchain(vita_gpu_swapchain handle,
                                     vita_gpu_swapchain_image &output) {
    Status status =
        validate_output_struct(&output, sizeof(output), "swapchain_image");
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    SwapchainRecord *record = nullptr;
    status = resolve(swapchains_, handle.value, record, "swapchain");
    if (!status.ok()) {
        return status;
    }
    if (record->acquired) {
        return state_error("swapchain already has an acquired image");
    }
    BackendSwapchainImage backend_image;
    status = backend_->acquire_swapchain(record->native, backend_image);
    if (!status.ok()) {
        return status;
    }
    if (backend_image.texture.value == 0 ||
        backend_image.render_target.value == 0) {
        return Status::error(VITA_GPU_ERROR_INTERNAL,
                             "backend returned invalid swapchain image handles",
                             backend_->backend());
    }
    auto image = std::find_if(
        record->images.begin(), record->images.end(),
        [&backend_image](const SwapchainImageRecord &entry) {
            return entry.image_index == backend_image.image_index;
        });
    if (image == record->images.end()) {
        TextureRecord texture{
            backend_image.texture,
            record->width,
            record->height,
            1,
            1,
            1,
            record->format,
            VITA_GPU_TEXTURE_RENDER_TARGET | VITA_GPU_TEXTURE_COPY_SOURCE,
            VITA_GPU_RESOURCE_STATE_PRESENT,
            VITA_GPU_QUEUE_GRAPHICS,
            {0, 0, true}};
        const vita_gpu_texture texture_handle =
            public_handle<vita_gpu_texture>(textures_.insert(texture));
        RenderTargetRecord target{
            backend_image.render_target, texture_handle.value, record->format,
            {0, 0, true}};
        const vita_gpu_render_target target_handle =
            public_handle<vita_gpu_render_target>(
                render_targets_.insert(target));
        ++textures_.find(texture_handle.value).value->use.dependencies;
        record->images.push_back(
            {backend_image.image_index, texture_handle, target_handle});
        image = std::prev(record->images.end());
    } else {
        const auto texture = textures_.find(image->texture.value);
        const auto target = render_targets_.find(image->target.value);
        if (!texture || !target ||
            texture.value->native != backend_image.texture ||
            target.value->native != backend_image.render_target) {
            return Status::error(
                VITA_GPU_ERROR_INTERNAL,
                "backend changed swapchain image identity without resize",
                backend_->backend());
        }
    }
    record->acquired = true;
    record->acquired_index = backend_image.image_index;
    output.image_index = image->image_index;
    output.texture = image->texture;
    output.render_target = image->target;
    return Status::success();
}

Status DeviceCore::resize_swapchain(vita_gpu_swapchain handle,
                                    std::uint32_t width,
                                    std::uint32_t height) {
    if (width == 0 || height == 0) {
        return invalid("swapchain resize extent must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    SwapchainRecord *record = nullptr;
    Status status =
        resolve(swapchains_, handle.value, record, "swapchain");
    if (!status.ok()) {
        return status;
    }
    if (record->acquired) {
        return state_error("cannot resize while an image is acquired");
    }
    if (swapchain_images_busy(*record)) {
        return Status::error(
            VITA_GPU_ERROR_RESOURCE_IN_USE,
            "cannot resize while swapchain images are referenced");
    }
    status = backend_->resize_swapchain(record->native, width, height);
    if (!status.ok()) {
        return status;
    }
    clear_swapchain_images(*record);
    record->width = width;
    record->height = height;
    return Status::success();
}

Status DeviceCore::present_swapchain(vita_gpu_swapchain handle,
                                     const vita_gpu_present_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc), "present_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.allow_tearing > VITA_GPU_TRUE) {
        return invalid("present allow_tearing must be a boolean");
    }
    std::scoped_lock lock(mutex_);
    SwapchainRecord *record = nullptr;
    status = resolve(swapchains_, handle.value, record, "swapchain");
    if (!status.ok()) {
        return status;
    }
    if (!record->acquired || desc.image_index != record->acquired_index) {
        return state_error("present image does not match the acquired image");
    }
    const auto image = std::find_if(
        record->images.begin(), record->images.end(),
        [&desc](const SwapchainImageRecord &entry) {
            return entry.image_index == desc.image_index;
        });
    if (image == record->images.end()) {
        return Status::error(VITA_GPU_ERROR_INTERNAL,
                             "acquired swapchain image is missing");
    }
    const auto texture = textures_.find(image->texture.value);
    if (!texture ||
        texture.value->state != VITA_GPU_RESOURCE_STATE_PRESENT) {
        return state_error(
            "swapchain image must be transitioned to PRESENT before present");
    }
    status = backend_->present_swapchain(record->native, desc);
    if (status.ok()) {
        record->acquired = false;
    }
    return status;
}

Status DeviceCore::create_command_list(
    const vita_gpu_command_list_desc &desc, vita_gpu_command_list &output) {
    Status status = validate_struct(&desc, sizeof(desc), "command_list_desc");
    if (!status.ok()) {
        return status;
    }
    if (!valid_queue(desc.queue)) {
        return invalid("command list queue is invalid");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord record;
    record.recorded.queue = desc.queue;
    output = public_handle<vita_gpu_command_list>(
        commands_.insert(std::move(record)));
    return Status::success();
}

Status DeviceCore::reset_submitted(CommandListRecord &command) {
    if (command.state != CommandState::submitted) {
        return Status::success();
    }
    FenceRecord *fence = nullptr;
    Status status =
        resolve(fences_, command.submitted_fence, fence, "submission fence");
    if (!status.ok()) {
        return status;
    }
    std::uint64_t completed = 0;
    status = backend_->fence_completed_value(fence->native, completed);
    if (!status.ok()) {
        return status;
    }
    if (completed < fence->last_completed || completed > fence->last_signaled) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "backend returned a non-monotonic or unsignaled fence value",
            backend_->backend());
    }
    fence->last_completed = completed;
    if (completed < command.submitted_value) {
        return Status::error(
            VITA_GPU_ERROR_RESOURCE_IN_USE,
            "command list submission has not reached its fence value");
    }
    release_dependency({ObjectKind::fence, command.submitted_fence});
    command.submitted_fence = 0;
    command.submitted_value = 0;
    command.state = CommandState::executable;
    return Status::success();
}

Status DeviceCore::destroy_command_list(vita_gpu_command_list handle) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state == CommandState::recording ||
        record->state == CommandState::render_pass) {
        return state_error("cannot destroy a recording command list");
    }
    status = reset_submitted(*record);
    if (!status.ok()) {
        return status;
    }
    release_command_references(*record);
    (void)commands_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::command_begin(vita_gpu_command_list handle) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::initial) {
        return state_error("command_begin requires an initial command list");
    }
    record->state = CommandState::recording;
    return Status::success();
}

Status DeviceCore::command_end(vita_gpu_command_list handle) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording) {
        return state_error(
            "command_end requires recording state and no open render pass");
    }
    record->state = CommandState::executable;
    return Status::success();
}

Status DeviceCore::command_reset(vita_gpu_command_list handle) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state == CommandState::recording ||
        record->state == CommandState::render_pass ||
        record->state == CommandState::initial) {
        return state_error(
            "command_reset requires an executable or completed command list");
    }
    status = reset_submitted(*record);
    if (!status.ok()) {
        return status;
    }
    release_command_references(*record);
    record->recorded.commands.clear();
    record->bound_pipeline.reset();
    record->index_buffer_bound = false;
    record->state = CommandState::initial;
    return Status::success();
}

Status DeviceCore::command_barriers(
    vita_gpu_command_list handle,
    std::span<const vita_gpu_resource_barrier> barriers) {
    if (barriers.empty()) {
        return invalid("barrier batch must not be empty");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording) {
        return state_error("resource barriers are only valid outside a render pass");
    }

    std::vector<BarrierCommand> translated;
    translated.reserve(barriers.size());
    std::vector<PredictedState> predictions = record->predicted_states;
    std::vector<ResourceRef> references;
    for (const auto &barrier : barriers) {
        status = validate_struct(&barrier, sizeof(barrier), "resource_barrier");
        if (!status.ok()) {
            return status;
        }
        constexpr vita_gpu_resource_state known_states =
            VITA_GPU_RESOURCE_STATE_COMMON |
            VITA_GPU_RESOURCE_STATE_COPY_SOURCE |
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
        const auto structurally_valid_state =
            [known_states](vita_gpu_resource_state state,
                           bool allow_undefined) {
                if ((state & ~known_states) != 0 ||
                    (state == VITA_GPU_RESOURCE_STATE_UNDEFINED &&
                     !allow_undefined)) {
                    return false;
                }
                const vita_gpu_resource_state writes =
                    state & (VITA_GPU_RESOURCE_STATE_COPY_DEST |
                             VITA_GPU_RESOURCE_STATE_SHADER_WRITE |
                             VITA_GPU_RESOURCE_STATE_RENDER_TARGET |
                             VITA_GPU_RESOURCE_STATE_DEPTH_WRITE |
                             VITA_GPU_RESOURCE_STATE_PRESENT);
                return writes == 0 || (writes & (writes - 1U)) == 0;
            };
        if ((barrier.before & ~known_states) != 0 ||
            !structurally_valid_state(barrier.before, true) ||
            !structurally_valid_state(barrier.after, false) ||
            (barrier.before == barrier.after &&
             barrier.source_queue == barrier.destination_queue) ||
            !valid_queue(barrier.source_queue) ||
            !valid_queue(barrier.destination_queue)) {
            return invalid("resource barrier states or queues are invalid");
        }

        ResourceRef reference;
        NativeId native;
        vita_gpu_resource_state current = VITA_GPU_RESOURCE_STATE_UNDEFINED;
        vita_gpu_queue_type current_queue = VITA_GPU_QUEUE_GRAPHICS;
        if (barrier.kind == VITA_GPU_RESOURCE_BUFFER) {
            if (barrier.buffer.value == 0 || barrier.texture.value != 0 ||
                barrier.first_mip != 0 || barrier.mip_count != 0 ||
                barrier.first_array_layer != 0 ||
                barrier.array_layer_count != 0) {
                return invalid("buffer barrier contains texture-only fields");
            }
            BufferRecord *buffer = nullptr;
            status =
                resolve(buffers_, barrier.buffer.value, buffer, "buffer");
            if (!status.ok()) {
                return status;
            }
            const auto supported = [buffer](vita_gpu_resource_state state) {
                const vita_gpu_resource_state forbidden =
                    VITA_GPU_RESOURCE_STATE_RENDER_TARGET |
                    VITA_GPU_RESOURCE_STATE_DEPTH_READ |
                    VITA_GPU_RESOURCE_STATE_DEPTH_WRITE |
                    VITA_GPU_RESOURCE_STATE_PRESENT;
                return (state & forbidden) == 0 &&
                       ((state & VITA_GPU_RESOURCE_STATE_COPY_SOURCE) == 0 ||
                        (buffer->usage & VITA_GPU_BUFFER_COPY_SOURCE) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_COPY_DEST) == 0 ||
                        (buffer->usage & VITA_GPU_BUFFER_COPY_DEST) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER) == 0 ||
                        (buffer->usage & VITA_GPU_BUFFER_VERTEX) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_INDEX_BUFFER) == 0 ||
                        (buffer->usage & VITA_GPU_BUFFER_INDEX) != 0) &&
                       ((state &
                         VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER) == 0 ||
                        (buffer->usage & VITA_GPU_BUFFER_CONSTANT) != 0) &&
                       ((state & (VITA_GPU_RESOURCE_STATE_SHADER_READ |
                                  VITA_GPU_RESOURCE_STATE_SHADER_WRITE)) == 0 ||
                        (buffer->usage & VITA_GPU_BUFFER_STORAGE) != 0);
            };
            if (!supported(barrier.before) || !supported(barrier.after)) {
                return invalid(
                    "buffer barrier state is incompatible with buffer usage");
            }
            reference = {ObjectKind::buffer, barrier.buffer.value};
            native = buffer->native;
            auto predicted = std::find_if(
                predictions.begin(), predictions.end(),
                [reference](const PredictedState &entry) {
                    return entry.resource == reference;
                });
            current = predicted == predictions.end() ? buffer->state
                                                     : predicted->state;
            current_queue = predicted == predictions.end()
                                ? buffer->queue
                                : predicted->queue;
        } else if (barrier.kind == VITA_GPU_RESOURCE_TEXTURE) {
            if (barrier.texture.value == 0 || barrier.buffer.value != 0 ||
                barrier.mip_count == 0 || barrier.array_layer_count == 0) {
                return invalid("texture barrier subresource range is invalid");
            }
            TextureRecord *texture = nullptr;
            status =
                resolve(textures_, barrier.texture.value, texture, "texture");
            if (!status.ok()) {
                return status;
            }
            if (barrier.first_mip >= texture->mip_levels ||
                barrier.mip_count >
                    texture->mip_levels - barrier.first_mip ||
                barrier.first_array_layer >= texture->depth_or_layers ||
                barrier.array_layer_count >
                    texture->depth_or_layers - barrier.first_array_layer) {
                return invalid("texture barrier subresource range is out of bounds");
            }
            const auto supported = [texture](vita_gpu_resource_state state) {
                const vita_gpu_resource_state forbidden =
                    VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER |
                    VITA_GPU_RESOURCE_STATE_INDEX_BUFFER |
                    VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER;
                return (state & forbidden) == 0 &&
                       ((state & VITA_GPU_RESOURCE_STATE_COPY_SOURCE) == 0 ||
                        (texture->usage & VITA_GPU_TEXTURE_COPY_SOURCE) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_COPY_DEST) == 0 ||
                        (texture->usage & VITA_GPU_TEXTURE_COPY_DEST) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_SHADER_READ) == 0 ||
                        (texture->usage &
                         (VITA_GPU_TEXTURE_SAMPLED |
                          VITA_GPU_TEXTURE_STORAGE)) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_SHADER_WRITE) == 0 ||
                        (texture->usage & VITA_GPU_TEXTURE_STORAGE) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_RENDER_TARGET) == 0 ||
                        (texture->usage &
                         VITA_GPU_TEXTURE_RENDER_TARGET) != 0) &&
                       ((state & (VITA_GPU_RESOURCE_STATE_DEPTH_READ |
                                  VITA_GPU_RESOURCE_STATE_DEPTH_WRITE)) == 0 ||
                        (texture->usage & VITA_GPU_TEXTURE_DEPTH_TARGET) != 0) &&
                       ((state & VITA_GPU_RESOURCE_STATE_PRESENT) == 0 ||
                        texture->use.borrowed);
            };
            if (!supported(barrier.before) || !supported(barrier.after)) {
                return invalid(
                    "texture barrier state is incompatible with texture usage");
            }
            reference = {ObjectKind::texture, barrier.texture.value};
            native = texture->native;
            auto predicted = std::find_if(
                predictions.begin(), predictions.end(),
                [reference](const PredictedState &entry) {
                    return entry.resource == reference;
                });
            current = predicted == predictions.end() ? texture->state
                                                     : predicted->state;
            current_queue = predicted == predictions.end()
                                ? texture->queue
                                : predicted->queue;
        } else {
            return invalid("resource barrier kind is invalid");
        }
        if (current != barrier.before) {
            return state_error(
                "resource barrier before-state does not match tracked state");
        }
        if (current_queue != barrier.source_queue ||
            barrier.destination_queue != record->recorded.queue) {
            return state_error(
                "resource barrier queue ownership does not match tracked or recording queue");
        }
        auto prediction = std::find_if(
            predictions.begin(), predictions.end(),
            [reference](const PredictedState &entry) {
                return entry.resource == reference;
            });
        if (prediction == predictions.end()) {
            predictions.push_back(
                {reference, barrier.after, barrier.destination_queue});
        } else {
            prediction->state = barrier.after;
            prediction->queue = barrier.destination_queue;
        }
        references.push_back(reference);
        translated.push_back(BarrierCommand{
            barrier.kind,
            native,
            reference.handle,
            barrier.before,
            barrier.after,
            barrier.first_mip,
            barrier.mip_count,
            barrier.first_array_layer,
            barrier.array_layer_count,
            barrier.source_queue,
            barrier.destination_queue});
    }

    for (const ResourceRef reference : references) {
        status = add_reference(*record, reference);
        if (!status.ok()) {
            return status;
        }
    }
    for (const BarrierCommand &barrier : translated) {
        record->recorded.commands.emplace_back(barrier);
    }
    record->predicted_states = std::move(predictions);
    return Status::success();
}

Status DeviceCore::command_begin_render_pass(
    vita_gpu_command_list handle, const vita_gpu_render_pass_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc), "render_pass_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.color_attachment_count > VITA_GPU_MAX_COLOR_TARGETS ||
        (desc.color_attachments == nullptr) !=
            (desc.color_attachment_count == 0) ||
        (desc.color_attachment_count == 0 &&
         desc.depth_attachment == nullptr)) {
        return invalid("render pass attachment list is invalid");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording ||
        record->recorded.queue != VITA_GPU_QUEUE_GRAPHICS) {
        return state_error(
            "render pass requires a recording graphics command list");
    }

    BeginRenderPassCommand command;
    std::vector<ResourceRef> references;
    std::set<std::uint64_t> attachment_handles;
    for (std::uint32_t i = 0; i < desc.color_attachment_count; ++i) {
        const auto &attachment = desc.color_attachments[i];
        status =
            validate_struct(&attachment, sizeof(attachment), "color_attachment");
        if (!status.ok()) {
            return status;
        }
        if (attachment.load_op < VITA_GPU_LOAD_PRESERVE ||
            attachment.load_op > VITA_GPU_LOAD_DISCARD ||
            attachment.store_op < VITA_GPU_STORE_PRESERVE ||
            attachment.store_op > VITA_GPU_STORE_DISCARD ||
            !std::all_of(std::begin(attachment.clear_color),
                         std::end(attachment.clear_color),
                         [](float value) { return std::isfinite(value); }) ||
            !attachment_handles.insert(attachment.target.value).second) {
            return invalid("render pass has an invalid or duplicate color attachment");
        }
        RenderTargetRecord *target = nullptr;
        status = resolve(render_targets_, attachment.target.value, target,
                         "render target");
        if (!status.ok()) {
            return status;
        }
        vita_gpu_resource_state texture_state =
            VITA_GPU_RESOURCE_STATE_UNDEFINED;
        status = command_state(
            *record, {ObjectKind::texture, target->texture}, texture_state);
        if (!status.ok()) {
            return status;
        }
        status = validate_command_resource_queue(
            *record, {ObjectKind::texture, target->texture});
        if (!status.ok()) {
            return status;
        }
        if (!has_state(texture_state, VITA_GPU_RESOURCE_STATE_RENDER_TARGET)) {
            return state_error(
                "color attachment texture is not in RENDER_TARGET state");
        }
        BackendColorAttachment translated;
        translated.target = target->native;
        translated.state = texture_state;
        translated.load_op = attachment.load_op;
        translated.store_op = attachment.store_op;
        std::copy(std::begin(attachment.clear_color),
                  std::end(attachment.clear_color),
                  translated.clear_color.begin());
        command.colors.push_back(translated);
        references.push_back(
            {ObjectKind::render_target, attachment.target.value});
    }
    if (desc.depth_attachment != nullptr) {
        const auto &attachment = *desc.depth_attachment;
        status =
            validate_struct(&attachment, sizeof(attachment), "depth_attachment");
        if (!status.ok()) {
            return status;
        }
        if (attachment.depth_load_op < VITA_GPU_LOAD_PRESERVE ||
            attachment.depth_load_op > VITA_GPU_LOAD_DISCARD ||
            attachment.depth_store_op < VITA_GPU_STORE_PRESERVE ||
            attachment.depth_store_op > VITA_GPU_STORE_DISCARD ||
            attachment.stencil_load_op < VITA_GPU_LOAD_PRESERVE ||
            attachment.stencil_load_op > VITA_GPU_LOAD_DISCARD ||
            attachment.stencil_store_op < VITA_GPU_STORE_PRESERVE ||
            attachment.stencil_store_op > VITA_GPU_STORE_DISCARD ||
            !std::isfinite(attachment.clear_depth) ||
            attachment.clear_depth < 0.0F ||
            attachment.clear_depth > 1.0F) {
            return invalid("render pass depth attachment is invalid");
        }
        DepthTargetRecord *target = nullptr;
        status = resolve(depth_targets_, attachment.target.value, target,
                         "depth target");
        if (!status.ok()) {
            return status;
        }
        vita_gpu_resource_state texture_state =
            VITA_GPU_RESOURCE_STATE_UNDEFINED;
        status = command_state(
            *record, {ObjectKind::texture, target->texture}, texture_state);
        if (!status.ok()) {
            return status;
        }
        status = validate_command_resource_queue(
            *record, {ObjectKind::texture, target->texture});
        if (!status.ok()) {
            return status;
        }
        const vita_gpu_resource_state expected =
            target->read_only_depth ? VITA_GPU_RESOURCE_STATE_DEPTH_READ
                                    : VITA_GPU_RESOURCE_STATE_DEPTH_WRITE;
        if (!has_state(texture_state, expected)) {
            return state_error(
                "depth attachment texture is not in the required depth state");
        }
        command.depth = BackendDepthAttachment{
            target->native,
            texture_state,
            attachment.depth_load_op,
            attachment.depth_store_op,
            attachment.stencil_load_op,
            attachment.stencil_store_op,
            attachment.clear_depth,
            attachment.clear_stencil};
        references.push_back(
            {ObjectKind::depth_target, attachment.target.value});
    }
    for (const ResourceRef reference : references) {
        status = add_reference(*record, reference);
        if (!status.ok()) {
            return status;
        }
    }
    record->recorded.commands.emplace_back(std::move(command));
    record->state = CommandState::render_pass;
    return Status::success();
}

Status DeviceCore::command_end_render_pass(vita_gpu_command_list handle) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass) {
        return state_error("no render pass is open");
    }
    record->recorded.commands.emplace_back(EndRenderPassCommand{});
    record->state = CommandState::recording;
    record->bound_pipeline.reset();
    record->index_buffer_bound = false;
    return Status::success();
}

Status DeviceCore::command_bind_pipeline(vita_gpu_command_list handle,
                                         vita_gpu_pipeline pipeline) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording &&
        record->state != CommandState::render_pass) {
        return state_error("pipeline binding requires recording state");
    }
    PipelineRecord *pipeline_record = nullptr;
    status =
        resolve(pipelines_, pipeline.value, pipeline_record, "pipeline");
    if (!status.ok()) {
        return status;
    }
    if ((record->state == CommandState::render_pass &&
         !pipeline_record->graphics) ||
        (record->state == CommandState::recording &&
         pipeline_record->graphics)) {
        return state_error(
            "graphics pipelines bind inside render passes; compute pipelines bind outside");
    }
    status = add_reference(*record, {ObjectKind::pipeline, pipeline.value});
    if (!status.ok()) {
        return status;
    }
    record->recorded.commands.emplace_back(
        BindPipelineCommand{pipeline_record->native});
    record->bound_pipeline = pipeline.value;
    return Status::success();
}

Status DeviceCore::command_bind_set(vita_gpu_command_list handle,
                                    std::uint32_t set_index,
                                    vita_gpu_binding_set set) {
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording &&
        record->state != CommandState::render_pass) {
        return state_error("binding set requires recording state");
    }
    if (!record->bound_pipeline.has_value()) {
        return state_error("binding set requires a bound pipeline");
    }
    BindingSetRecord *set_record = nullptr;
    status =
        resolve(binding_sets_, set.value, set_record, "binding set");
    if (!status.ok()) {
        return status;
    }
    BindingLayoutRecord *layout = nullptr;
    status =
        resolve(binding_layouts_, set_record->layout, layout, "binding layout");
    if (!status.ok()) {
        return status;
    }
    if (layout->set_index != set_index) {
        return invalid("binding set index does not match its layout");
    }
    PipelineRecord *pipeline = nullptr;
    status =
        resolve(pipelines_, *record->bound_pipeline, pipeline, "pipeline");
    if (!status.ok()) {
        return status;
    }
    const auto expected_layout = std::find_if(
        pipeline->layouts.begin(), pipeline->layouts.end(),
        [set_index](const auto &entry) { return entry.first == set_index; });
    if (expected_layout == pipeline->layouts.end() ||
        expected_layout->second != set_record->layout) {
        return invalid("binding set layout does not match the bound pipeline");
    }
    for (const BoundSlot &slot : set_record->slots) {
        if (slot.required_state == VITA_GPU_RESOURCE_STATE_UNDEFINED) {
            continue;
        }
        vita_gpu_resource_state tracked = VITA_GPU_RESOURCE_STATE_UNDEFINED;
        status = command_state(*record, slot.resource, tracked);
        if (!status.ok()) {
            return status;
        }
        status = validate_command_resource_queue(*record, slot.resource);
        if (!status.ok()) {
            return status;
        }
        if (!has_state(tracked, slot.required_state)) {
            return state_error(
                "bound resource is not in the state declared by the binding write");
        }
    }
    status = add_reference(*record, {ObjectKind::binding_set, set.value});
    if (!status.ok()) {
        return status;
    }
    record->recorded.commands.emplace_back(
        BindSetCommand{set_index, set_record->native});
    return Status::success();
}

Status DeviceCore::command_set_viewport(vita_gpu_command_list handle,
                                        const vita_gpu_viewport &viewport) {
    Status status = validate_struct(&viewport, sizeof(viewport), "viewport");
    if (!status.ok()) {
        return status;
    }
    if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
        !std::isfinite(viewport.min_depth) ||
        !std::isfinite(viewport.max_depth) ||
        viewport.width <= 0.0F || viewport.height <= 0.0F ||
        viewport.min_depth < 0.0F || viewport.max_depth > 1.0F ||
        viewport.min_depth > viewport.max_depth) {
        return invalid("viewport dimensions or depth range are invalid");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass) {
        return state_error("viewport can only be set inside a render pass");
    }
    record->recorded.commands.emplace_back(ViewportCommand{viewport});
    return Status::success();
}

Status DeviceCore::command_set_scissor(vita_gpu_command_list handle,
                                       const vita_gpu_scissor &scissor) {
    Status status = validate_struct(&scissor, sizeof(scissor), "scissor");
    if (!status.ok()) {
        return status;
    }
    if (scissor.width == 0 || scissor.height == 0) {
        return invalid("scissor extent must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass) {
        return state_error("scissor can only be set inside a render pass");
    }
    record->recorded.commands.emplace_back(ScissorCommand{scissor});
    return Status::success();
}

Status DeviceCore::command_set_vertex_buffers(
    vita_gpu_command_list handle,
    std::span<const vita_gpu_vertex_buffer_binding> bindings) {
    if (bindings.empty()) {
        return invalid("vertex buffer binding list must not be empty");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass) {
        return state_error("vertex buffers bind inside a render pass");
    }
    std::set<std::uint32_t> slots;
    std::vector<std::pair<ResourceRef, VertexBufferCommand>> translated;
    translated.reserve(bindings.size());
    for (const auto &binding : bindings) {
        status =
            validate_struct(&binding, sizeof(binding), "vertex_buffer_binding");
        if (!status.ok()) {
            return status;
        }
        if (!slots.insert(binding.binding).second) {
            return invalid("vertex buffer binding slots must be unique");
        }
        BufferRecord *buffer = nullptr;
        status =
            resolve(buffers_, binding.buffer.value, buffer, "vertex buffer");
        if (!status.ok()) {
            return status;
        }
        vita_gpu_resource_state tracked = VITA_GPU_RESOURCE_STATE_UNDEFINED;
        const ResourceRef reference{ObjectKind::buffer, binding.buffer.value};
        status = command_state(*record, reference, tracked);
        if (!status.ok()) {
            return status;
        }
        status = validate_command_resource_queue(*record, reference);
        if (!status.ok()) {
            return status;
        }
        if ((buffer->usage & VITA_GPU_BUFFER_VERTEX) == 0 ||
            binding.offset >= buffer->size ||
            !has_state(tracked, VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER)) {
            return invalid("vertex buffer usage, offset, or state is invalid");
        }
        translated.push_back(
            {reference,
             VertexBufferCommand{binding.binding, buffer->native,
                                 binding.offset}});
    }
    for (const auto &[reference, command] : translated) {
        status = add_reference(*record, reference);
        if (!status.ok()) {
            return status;
        }
        record->recorded.commands.emplace_back(command);
    }
    return Status::success();
}

Status DeviceCore::command_set_index_buffer(
    vita_gpu_command_list handle,
    const vita_gpu_index_buffer_binding &binding) {
    Status status =
        validate_struct(&binding, sizeof(binding), "index_buffer_binding");
    if (!status.ok()) {
        return status;
    }
    if (binding.format != VITA_GPU_INDEX_UINT16 &&
        binding.format != VITA_GPU_INDEX_UINT32) {
        return invalid("index buffer format is invalid");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass) {
        return state_error("index buffer binds inside a render pass");
    }
    BufferRecord *buffer = nullptr;
    status = resolve(buffers_, binding.buffer.value, buffer, "index buffer");
    if (!status.ok()) {
        return status;
    }
    const ResourceRef reference{ObjectKind::buffer, binding.buffer.value};
    vita_gpu_resource_state tracked = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    status = command_state(*record, reference, tracked);
    if (!status.ok()) {
        return status;
    }
    status = validate_command_resource_queue(*record, reference);
    if (!status.ok()) {
        return status;
    }
    const std::uint64_t alignment =
        binding.format == VITA_GPU_INDEX_UINT16 ? 2U : 4U;
    if ((buffer->usage & VITA_GPU_BUFFER_INDEX) == 0 ||
        binding.offset >= buffer->size ||
        (binding.offset % alignment) != 0 ||
        !has_state(tracked, VITA_GPU_RESOURCE_STATE_INDEX_BUFFER)) {
        return invalid("index buffer usage, offset, alignment, or state is invalid");
    }
    status = add_reference(*record, reference);
    if (!status.ok()) {
        return status;
    }
    record->recorded.commands.emplace_back(
        IndexBufferCommand{buffer->native, binding.offset, binding.format});
    record->index_buffer_bound = true;
    return Status::success();
}

Status DeviceCore::command_push_constants(
    vita_gpu_command_list handle, const vita_gpu_push_constants_desc &desc) {
    Status status =
        validate_struct(&desc, sizeof(desc), "push_constants_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.stages == 0 ||
        (desc.stages & ~VITA_GPU_SHADER_STAGE_ALL) != 0 ||
        desc.data == nullptr || desc.data_size == 0 ||
        (desc.offset % 4U) != 0 || (desc.data_size % 4U) != 0) {
        return invalid("push constants stages, data, size, or alignment is invalid");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if ((record->state != CommandState::recording &&
         record->state != CommandState::render_pass) ||
        !record->bound_pipeline.has_value()) {
        return state_error("push constants require a bound pipeline");
    }
    PipelineRecord *pipeline = nullptr;
    status = resolve(pipelines_, *record->bound_pipeline, pipeline, "pipeline");
    if (!status.ok()) {
        return status;
    }
    if (desc.offset > pipeline->push_constant_bytes ||
        desc.data_size > pipeline->push_constant_bytes - desc.offset) {
        return invalid("push constant range exceeds the pipeline declaration");
    }
    PushConstantsCommand command;
    command.stages = desc.stages;
    command.offset = desc.offset;
    command.data.assign(desc.data, desc.data + desc.data_size);
    record->recorded.commands.emplace_back(std::move(command));
    return Status::success();
}

Status DeviceCore::command_draw(vita_gpu_command_list handle,
                                const vita_gpu_draw_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc), "draw_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.vertex_count == 0 || desc.instance_count == 0) {
        return invalid("draw counts must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass ||
        !record->bound_pipeline.has_value()) {
        return state_error("draw requires a render pass and graphics pipeline");
    }
    PipelineRecord *pipeline = nullptr;
    status = resolve(pipelines_, *record->bound_pipeline, pipeline, "pipeline");
    if (!status.ok()) {
        return status;
    }
    if (!pipeline->graphics) {
        return state_error("draw cannot use a compute pipeline");
    }
    record->recorded.commands.emplace_back(DrawCommand{desc});
    return Status::success();
}

Status DeviceCore::command_draw_indexed(
    vita_gpu_command_list handle, const vita_gpu_draw_indexed_desc &desc) {
    Status status =
        validate_struct(&desc, sizeof(desc), "draw_indexed_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.index_count == 0 || desc.instance_count == 0) {
        return invalid("indexed draw counts must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::render_pass ||
        !record->bound_pipeline.has_value() || !record->index_buffer_bound) {
        return state_error(
            "indexed draw requires a render pass, pipeline, and index buffer");
    }
    PipelineRecord *pipeline = nullptr;
    status = resolve(pipelines_, *record->bound_pipeline, pipeline, "pipeline");
    if (!status.ok()) {
        return status;
    }
    if (!pipeline->graphics) {
        return state_error("indexed draw cannot use a compute pipeline");
    }
    record->recorded.commands.emplace_back(DrawIndexedCommand{desc});
    return Status::success();
}

Status DeviceCore::command_dispatch(vita_gpu_command_list handle,
                                    const vita_gpu_dispatch_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc), "dispatch_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.group_count_x == 0 || desc.group_count_y == 0 ||
        desc.group_count_z == 0) {
        return invalid("dispatch group counts must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording ||
        !record->bound_pipeline.has_value()) {
        return state_error(
            "dispatch requires a bound compute pipeline outside a render pass");
    }
    PipelineRecord *pipeline = nullptr;
    status = resolve(pipelines_, *record->bound_pipeline, pipeline, "pipeline");
    if (!status.ok()) {
        return status;
    }
    if (pipeline->graphics) {
        return state_error("dispatch cannot use a graphics pipeline");
    }
    record->recorded.commands.emplace_back(DispatchCommand{desc});
    return Status::success();
}

Status DeviceCore::command_copy_buffer(
    vita_gpu_command_list handle, const vita_gpu_buffer_copy_desc &desc) {
    Status status =
        validate_struct(&desc, sizeof(desc), "buffer_copy_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.byte_size == 0 || desc.source.value == desc.destination.value) {
        return invalid("buffer copy size is zero or resources alias");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording) {
        return state_error("buffer copies are only valid outside render passes");
    }
    BufferRecord *source = nullptr;
    BufferRecord *destination = nullptr;
    status = resolve(buffers_, desc.source.value, source, "source buffer");
    if (!status.ok()) {
        return status;
    }
    status =
        resolve(buffers_, desc.destination.value, destination, "destination buffer");
    if (!status.ok()) {
        return status;
    }
    const ResourceRef source_ref{ObjectKind::buffer, desc.source.value};
    const ResourceRef destination_ref{ObjectKind::buffer, desc.destination.value};
    vita_gpu_resource_state source_state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_resource_state destination_state =
        VITA_GPU_RESOURCE_STATE_UNDEFINED;
    status = command_state(*record, source_ref, source_state);
    if (!status.ok()) {
        return status;
    }
    status = validate_command_resource_queue(*record, source_ref);
    if (!status.ok()) {
        return status;
    }
    status = command_state(*record, destination_ref, destination_state);
    if (!status.ok()) {
        return status;
    }
    status = validate_command_resource_queue(*record, destination_ref);
    if (!status.ok()) {
        return status;
    }
    if (desc.source_offset > source->size ||
        desc.byte_size > source->size - desc.source_offset ||
        desc.destination_offset > destination->size ||
        desc.byte_size > destination->size - desc.destination_offset ||
        (source->usage & VITA_GPU_BUFFER_COPY_SOURCE) == 0 ||
        (destination->usage & VITA_GPU_BUFFER_COPY_DEST) == 0 ||
        !has_state(source_state, VITA_GPU_RESOURCE_STATE_COPY_SOURCE) ||
        !has_state(destination_state, VITA_GPU_RESOURCE_STATE_COPY_DEST)) {
        return invalid("buffer copy range, usage, or state is invalid");
    }
    status = add_reference(*record, source_ref);
    if (!status.ok()) {
        return status;
    }
    status = add_reference(*record, destination_ref);
    if (!status.ok()) {
        return status;
    }
    record->recorded.commands.emplace_back(CopyBufferCommand{
        source->native, desc.source_offset, destination->native,
        desc.destination_offset, desc.byte_size});
    return Status::success();
}

Status DeviceCore::command_copy_buffer_readback(
    vita_gpu_command_list handle,
    const vita_gpu_buffer_readback_copy_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc),
                                    "buffer_readback_copy_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.byte_size == 0) {
        return invalid("buffer readback copy size must be non-zero");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording) {
        return state_error("readback copies are invalid inside render passes");
    }
    BufferRecord *source = nullptr;
    ReadbackRecord *destination = nullptr;
    status = resolve(buffers_, desc.source.value, source, "source buffer");
    if (!status.ok()) {
        return status;
    }
    status = resolve(readbacks_, desc.destination.value, destination, "readback");
    if (!status.ok()) {
        return status;
    }
    const ResourceRef source_ref{ObjectKind::buffer, desc.source.value};
    vita_gpu_resource_state source_state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    status = command_state(*record, source_ref, source_state);
    if (!status.ok()) {
        return status;
    }
    status = validate_command_resource_queue(*record, source_ref);
    if (!status.ok()) {
        return status;
    }
    if (desc.source_offset > source->size ||
        desc.byte_size > source->size - desc.source_offset ||
        desc.destination_offset > destination->size ||
        desc.byte_size > destination->size - desc.destination_offset ||
        (source->usage & VITA_GPU_BUFFER_COPY_SOURCE) == 0 ||
        !has_state(source_state, VITA_GPU_RESOURCE_STATE_COPY_SOURCE)) {
        return invalid("buffer readback range, usage, or state is invalid");
    }
    status = add_reference(*record, source_ref);
    if (!status.ok()) {
        return status;
    }
    status = add_reference(
        *record, {ObjectKind::readback, desc.destination.value});
    if (!status.ok()) {
        return status;
    }
    record->recorded.commands.emplace_back(CopyBufferReadbackCommand{
        source->native, desc.source_offset, destination->native,
        desc.destination_offset, desc.byte_size});
    return Status::success();
}

Status DeviceCore::command_copy_texture_readback(
    vita_gpu_command_list handle,
    const vita_gpu_texture_readback_copy_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc),
                                    "texture_readback_copy_desc");
    if (!status.ok()) {
        return status;
    }
    if (desc.row_pitch == 0 || desc.slice_pitch < desc.row_pitch) {
        return invalid("texture readback pitches are invalid");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    status = resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording) {
        return state_error("readback copies are invalid inside render passes");
    }
    TextureRecord *source = nullptr;
    ReadbackRecord *destination = nullptr;
    status = resolve(textures_, desc.source.value, source, "source texture");
    if (!status.ok()) {
        return status;
    }
    status = resolve(readbacks_, desc.destination.value, destination, "readback");
    if (!status.ok()) {
        return status;
    }
    const ResourceRef source_ref{ObjectKind::texture, desc.source.value};
    vita_gpu_resource_state source_state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    status = command_state(*record, source_ref, source_state);
    if (!status.ok()) {
        return status;
    }
    status = validate_command_resource_queue(*record, source_ref);
    if (!status.ok()) {
        return status;
    }
    if (desc.mip_level >= source->mip_levels ||
        desc.array_layer >= source->depth_or_layers ||
        desc.destination_offset > destination->size ||
        desc.slice_pitch > destination->size - desc.destination_offset ||
        (source->usage & VITA_GPU_TEXTURE_COPY_SOURCE) == 0 ||
        !has_state(source_state, VITA_GPU_RESOURCE_STATE_COPY_SOURCE)) {
        return invalid("texture readback range, usage, or state is invalid");
    }
    status = add_reference(*record, source_ref);
    if (!status.ok()) {
        return status;
    }
    status = add_reference(
        *record, {ObjectKind::readback, desc.destination.value});
    if (!status.ok()) {
        return status;
    }
    record->recorded.commands.emplace_back(CopyTextureReadbackCommand{
        source->native, source_state, desc.mip_level, desc.array_layer,
        destination->native,
        desc.destination_offset, desc.row_pitch, desc.slice_pitch});
    return Status::success();
}

Status DeviceCore::command_debug_marker(vita_gpu_command_list handle,
                                        std::string_view label,
                                        std::uint32_t rgba_color) {
    if (label.empty() || label.size() > 255) {
        return invalid("debug marker label must contain 1 to 255 bytes");
    }
    std::scoped_lock lock(mutex_);
    CommandListRecord *record = nullptr;
    Status status =
        resolve(commands_, handle.value, record, "command list");
    if (!status.ok()) {
        return status;
    }
    if (record->state != CommandState::recording &&
        record->state != CommandState::render_pass) {
        return state_error("debug markers require recording state");
    }
    record->recorded.commands.emplace_back(
        DebugMarkerCommand{std::string(label), rgba_color});
    return Status::success();
}

Status DeviceCore::create_fence(const vita_gpu_fence_desc &desc,
                                vita_gpu_fence &output) {
    Status status = validate_struct(&desc, sizeof(desc), "fence_desc");
    if (!status.ok()) {
        return status;
    }
    std::scoped_lock lock(mutex_);
    NativeId native;
    status = backend_->create_fence(desc, native);
    if (!status.ok()) {
        return status;
    }
    status = validate_native_handle(native, "fence", backend_->backend());
    if (!status.ok()) {
        return status;
    }
    output = public_handle<vita_gpu_fence>(
        fences_.insert(FenceRecord{
            native, desc.initial_value, desc.initial_value, {}}));
    return Status::success();
}

Status DeviceCore::destroy_fence(vita_gpu_fence handle) {
    std::scoped_lock lock(mutex_);
    FenceRecord *record = nullptr;
    Status status = resolve(fences_, handle.value, record, "fence");
    if (!status.ok()) {
        return status;
    }
    status = ensure_destroyable(*record, "fence");
    if (!status.ok()) {
        return status;
    }
    status = backend_->destroy_object(ObjectKind::fence, record->native);
    if (!status.ok()) {
        return status;
    }
    (void)fences_.remove(handle.value);
    return Status::success();
}

Status DeviceCore::fence_completed(vita_gpu_fence handle,
                                   std::uint64_t &output) {
    std::scoped_lock lock(mutex_);
    FenceRecord *record = nullptr;
    Status status = resolve(fences_, handle.value, record, "fence");
    if (!status.ok()) {
        return status;
    }
    status = backend_->fence_completed_value(record->native, output);
    if (!status.ok()) {
        return status;
    }
    if (output < record->last_completed || output > record->last_signaled) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "backend returned a non-monotonic or unsignaled fence value",
            backend_->backend());
    }
    record->last_completed = output;
    return Status::success();
}

Status DeviceCore::fence_wait(vita_gpu_fence handle, std::uint64_t value,
                              std::uint64_t timeout_nanoseconds) {
    std::scoped_lock lock(mutex_);
    FenceRecord *record = nullptr;
    Status status = resolve(fences_, handle.value, record, "fence");
    if (!status.ok()) {
        return status;
    }
    if (value > record->last_signaled) {
        return invalid("cannot wait for a fence value that has not been signaled");
    }
    status =
        backend_->fence_wait(record->native, value, timeout_nanoseconds);
    if (!status.ok()) {
        return status;
    }
    std::uint64_t completed = 0;
    status = backend_->fence_completed_value(record->native, completed);
    if (!status.ok()) {
        return status;
    }
    if (completed < value || completed < record->last_completed ||
        completed > record->last_signaled) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "backend fence wait completed without the requested monotonic value",
            backend_->backend());
    }
    record->last_completed = completed;
    return Status::success();
}

Status DeviceCore::validate_submitted_state(
    std::span<CommandListRecord *const> command_lists,
    std::vector<PredictedState> &updates) {
    using StateAndQueue =
        std::pair<vita_gpu_resource_state, vita_gpu_queue_type>;
    std::map<std::pair<ObjectKind, std::uint64_t>, StateAndQueue> states;
    for (CommandListRecord *command : command_lists) {
        for (const CommandPayload &payload : command->recorded.commands) {
            const auto *barrier = std::get_if<BarrierCommand>(&payload);
            if (barrier == nullptr) {
                continue;
            }
            const ResourceRef reference{
                barrier->kind == VITA_GPU_RESOURCE_BUFFER
                    ? ObjectKind::buffer
                    : ObjectKind::texture,
                barrier->logical_handle};
            const auto key = std::make_pair(reference.kind, reference.handle);
            auto state = states.find(key);
            if (state == states.end()) {
                vita_gpu_resource_state *global = nullptr;
                vita_gpu_queue_type *global_queue = nullptr;
                Status status = resource_state(reference, global);
                if (!status.ok()) {
                    return status;
                }
                status = resource_queue(reference, global_queue);
                if (!status.ok()) {
                    return status;
                }
                state = states.emplace(
                    key, StateAndQueue{*global, *global_queue}).first;
            }
            if (state->second.first != barrier->before ||
                state->second.second != barrier->source_queue ||
                barrier->destination_queue != command->recorded.queue) {
                return state_error(
                    "submitted barrier state or queue no longer matches global ordering");
            }
            state->second =
                StateAndQueue{barrier->after, barrier->destination_queue};
        }
    }
    updates.reserve(states.size());
    for (const auto &[key, value] : states) {
        updates.push_back({ResourceRef{key.first, key.second}, value.first,
                           value.second});
    }
    return Status::success();
}

Status DeviceCore::submit(vita_gpu_queue_type queue,
                          const vita_gpu_submit_desc &desc) {
    Status status = validate_struct(&desc, sizeof(desc), "submit_desc");
    if (!status.ok()) {
        return status;
    }
    if (!valid_queue(queue) || desc.command_lists == nullptr ||
        desc.command_list_count == 0 ||
        (desc.waits == nullptr) != (desc.wait_count == 0)) {
        return invalid("submit queue, command list, or wait list is invalid");
    }
    status =
        validate_struct(&desc.signal, sizeof(desc.signal), "signal_fence_point");
    if (!status.ok()) {
        return status;
    }
    if (desc.signal.fence.value == 0) {
        return invalid("every submission requires a signal fence");
    }

    std::scoped_lock lock(mutex_);
    std::vector<CommandListRecord *> command_records;
    std::vector<const RecordedCommandList *> backend_commands;
    std::set<std::uint64_t> unique_commands;
    command_records.reserve(desc.command_list_count);
    backend_commands.reserve(desc.command_list_count);
    for (std::uint32_t i = 0; i < desc.command_list_count; ++i) {
        if (!unique_commands.insert(desc.command_lists[i].value).second) {
            return invalid("submission contains a duplicate command list");
        }
        CommandListRecord *command = nullptr;
        status = resolve(commands_, desc.command_lists[i].value, command,
                         "command list");
        if (!status.ok()) {
            return status;
        }
        if (command->state != CommandState::executable ||
            command->recorded.queue != queue) {
            return state_error(
                "submitted command list is not executable on the selected queue");
        }
        command_records.push_back(command);
        backend_commands.push_back(&command->recorded);
    }

    std::vector<BackendFencePoint> waits;
    waits.reserve(desc.wait_count);
    for (std::uint32_t i = 0; i < desc.wait_count; ++i) {
        const vita_gpu_fence_point &point = desc.waits[i];
        status = validate_struct(&point, sizeof(point), "wait_fence_point");
        if (!status.ok()) {
            return status;
        }
        FenceRecord *fence = nullptr;
        status = resolve(fences_, point.fence.value, fence, "wait fence");
        if (!status.ok()) {
            return status;
        }
        if (point.value > fence->last_signaled) {
            return invalid("submission waits on an unsignaled fence value");
        }
        waits.push_back({fence->native, point.value});
    }

    FenceRecord *signal = nullptr;
    status =
        resolve(fences_, desc.signal.fence.value, signal, "signal fence");
    if (!status.ok()) {
        return status;
    }
    if (desc.signal.value <= signal->last_signaled) {
        return invalid("fence signal values must increase monotonically");
    }

    std::vector<PredictedState> state_updates;
    status = validate_submitted_state(command_records, state_updates);
    if (!status.ok()) {
        return status;
    }
    status = backend_->submit(
        queue, backend_commands, waits,
        BackendFencePoint{signal->native, desc.signal.value});
    if (!status.ok()) {
        return status;
    }

    for (const PredictedState &update : state_updates) {
        vita_gpu_resource_state *global = nullptr;
        vita_gpu_queue_type *global_queue = nullptr;
        status = resource_state(update.resource, global);
        if (!status.ok()) {
            return Status::error(
                VITA_GPU_ERROR_INTERNAL,
                "submitted resource disappeared while device was locked",
                backend_->backend());
        }
        status = resource_queue(update.resource, global_queue);
        if (!status.ok()) {
            return Status::error(
                VITA_GPU_ERROR_INTERNAL,
                "submitted resource queue disappeared while device was locked",
                backend_->backend());
        }
        *global = update.state;
        *global_queue = update.queue;
    }
    signal->last_signaled = desc.signal.value;
    for (CommandListRecord *command : command_records) {
        command->state = CommandState::submitted;
        command->submitted_fence = desc.signal.fence.value;
        command->submitted_value = desc.signal.value;
        (void)add_dependency(
            {ObjectKind::fence, desc.signal.fence.value});
    }
    return Status::success();
}

Status DeviceCore::debug_capture_begin(
    const vita_gpu_debug_capture_desc &desc) {
    Status status =
        validate_struct(&desc, sizeof(desc), "debug_capture_desc");
    if (!status.ok()) {
        return status;
    }
    constexpr vita_gpu_debug_capture_flags known_flags =
        VITA_GPU_DEBUG_CAPTURE_API | VITA_GPU_DEBUG_CAPTURE_FRAME;
    if (desc.flags == 0 || (desc.flags & ~known_flags) != 0) {
        return invalid("debug capture flags are invalid");
    }
    std::scoped_lock lock(mutex_);
    if (capture_active_) {
        return state_error("debug capture is already active");
    }
    status = backend_->debug_capture_begin(desc);
    if (status.ok()) {
        capture_active_ = true;
    }
    return status;
}

Status DeviceCore::debug_capture_end() {
    std::scoped_lock lock(mutex_);
    if (!capture_active_) {
        return state_error("debug capture is not active");
    }
    Status status = backend_->debug_capture_end();
    if (status.ok()) {
        capture_active_ = false;
    }
    return status;
}

} // namespace

class Runtime::Impl {
public:
    explicit Impl(BackendRegistry source) : registry(std::move(source)) {
        registry.seal();
    }

    Status find_device(vita_gpu_device handle,
                       std::shared_ptr<DeviceCore> &output) {
        std::scoped_lock lock(mutex);
        const auto found = devices.find(handle.value);
        if (!found) {
            return handle_error(found.result, "device");
        }
        output = *found.value;
        return Status::success();
    }

    template <typename Function>
    Status call(vita_gpu_device handle, Function &&function) {
        std::shared_ptr<DeviceCore> device;
        Status status = find_device(handle, device);
        return status.ok() ? std::invoke(std::forward<Function>(function),
                                         *device)
                           : status;
    }

    BackendRegistry registry;
    std::mutex mutex;
    HandlePool<std::shared_ptr<DeviceCore>> devices;
    std::uint32_t next_owner_tag = 1;
};

Runtime::Runtime(BackendRegistry registry)
    : impl_(std::make_unique<Impl>(std::move(registry))) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime &&) noexcept = default;
Runtime &Runtime::operator=(Runtime &&) noexcept = default;

std::uint32_t Runtime::backend_count() const noexcept {
    return 2;
}

Status Runtime::backend_info(std::uint32_t index,
                             vita_gpu_backend_info &output) {
    Status status =
        validate_output_struct(&output, sizeof(output), "backend_info");
    if (!status.ok()) {
        return status;
    }
    if (index >= backend_count()) {
        return invalid("backend index is out of range");
    }
    const vita_gpu_backend backend =
        index == 0 ? VITA_GPU_BACKEND_DX12 : VITA_GPU_BACKEND_VULKAN;
    output.backend = backend;
    output.available = VITA_GPU_FALSE;
    output.validation_available = VITA_GPU_FALSE;
    const char *name =
        backend == VITA_GPU_BACKEND_DX12 ? "dx12" : "vulkan";
    std::memset(output.name, 0, sizeof(output.name));
    std::memset(output.unavailable_reason, 0,
                sizeof(output.unavailable_reason));
    std::memcpy(output.name, name, std::strlen(name));

    const auto factory = impl_->registry.find(backend);
    std::string reason = std::string(name) +
                         " backend is not built into this library";
    if (factory) {
        bool available = false;
        bool validation = false;
        status = factory->availability(available, validation, reason);
        if (!status.ok()) {
            return status;
        }
        output.available = available ? VITA_GPU_TRUE : VITA_GPU_FALSE;
        output.validation_available =
            validation ? VITA_GPU_TRUE : VITA_GPU_FALSE;
    }
    const std::size_t reason_size =
        std::min(reason.size(), sizeof(output.unavailable_reason) - 1U);
    if (output.available == VITA_GPU_FALSE && reason_size != 0) {
        std::memcpy(output.unavailable_reason, reason.data(), reason_size);
    }
    return Status::success();
}

Status Runtime::adapter_count(vita_gpu_backend backend,
                              std::uint32_t &count) {
    if (backend != VITA_GPU_BACKEND_DX12 &&
        backend != VITA_GPU_BACKEND_VULKAN) {
        return invalid("backend must be dx12 or vulkan");
    }
    const auto factory = impl_->registry.find(backend);
    if (!factory) {
        return Status::error(
            VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
            std::string(backend == VITA_GPU_BACKEND_DX12 ? "dx12" : "vulkan") +
                " backend is unavailable",
            backend);
    }
    bool available = false;
    bool validation = false;
    std::string reason;
    Status status = factory->availability(available, validation, reason);
    (void)validation;
    if (!status.ok()) {
        return status;
    }
    if (!available) {
        return Status::error(VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                             reason.empty() ? "backend is unavailable" : reason,
                             backend);
    }
    return factory->adapter_count(count);
}

Status Runtime::adapter_info(vita_gpu_backend backend, std::uint32_t index,
                             vita_gpu_adapter_info &output) {
    Status status =
        validate_output_struct(&output, sizeof(output), "adapter_info");
    if (!status.ok()) {
        return status;
    }
    std::uint32_t count = 0;
    status = adapter_count(backend, count);
    if (!status.ok()) {
        return status;
    }
    if (index >= count) {
        return Status::error(VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                             "adapter index is out of range", backend);
    }
    const auto factory = impl_->registry.find(backend);
    status = factory->adapter_info(index, output);
    if (!status.ok()) {
        return status;
    }
    if (output.adapter_index != index) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "backend returned adapter information for the wrong index",
            backend);
    }
    return Status::success();
}

Status Runtime::create_device(const vita_gpu_device_desc &desc,
                              vita_gpu_device &output) {
    Status status = validate_device_desc(desc);
    if (!status.ok()) {
        return status;
    }
    const auto factory = impl_->registry.find(desc.backend);
    if (!factory) {
        return Status::error(
            VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
            std::string(desc.backend == VITA_GPU_BACKEND_DX12 ? "dx12"
                                                              : "vulkan") +
                " backend is unavailable; no fallback was attempted",
            desc.backend);
    }
    bool available = false;
    bool validation = false;
    std::string reason;
    status = factory->availability(available, validation, reason);
    if (!status.ok()) {
        return status;
    }
    if (!available) {
        return Status::error(VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                             (reason.empty() ? "backend is unavailable"
                                             : reason) +
                                 std::string("; no fallback was attempted"),
                             desc.backend);
    }
    if ((desc.flags & VITA_GPU_DEVICE_ENABLE_VALIDATION) != 0 && !validation) {
        return Status::error(VITA_GPU_ERROR_UNSUPPORTED,
                             "requested validation is unavailable",
                             desc.backend);
    }
    std::uint32_t count = 0;
    status = factory->adapter_count(count);
    if (!status.ok()) {
        return status;
    }
    if (desc.adapter_index >= count) {
        return Status::error(VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                             "requested adapter index is unavailable",
                             desc.backend);
    }
    std::unique_ptr<BackendDevice> backend_device;
    status = factory->create_device(desc, backend_device);
    if (!status.ok()) {
        return status;
    }
    if (!backend_device || backend_device->backend() != desc.backend) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "backend factory returned no device or the wrong backend",
            desc.backend);
    }

    std::scoped_lock lock(impl_->mutex);
    if (impl_->next_owner_tag >
        std::numeric_limits<std::uint16_t>::max()) {
        return Status::error(VITA_GPU_ERROR_OUT_OF_MEMORY,
                             "device handle owner namespace is exhausted");
    }
    const auto owner_tag =
        static_cast<std::uint16_t>(impl_->next_owner_tag++);
    auto device =
        std::make_shared<DeviceCore>(owner_tag, std::move(backend_device));
    output = public_handle<vita_gpu_device>(
        impl_->devices.insert(std::move(device)));
    return Status::success();
}

Status Runtime::destroy_device(vita_gpu_device handle) {
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->devices.find(handle.value);
    if (!found) {
        return handle_error(found.result, "device");
    }
    Status status = (*found.value)->can_destroy();
    if (!status.ok()) {
        return status;
    }
    (void)impl_->devices.remove(handle.value);
    return Status::success();
}

Status Runtime::capabilities(vita_gpu_device device,
                             vita_gpu_device_capabilities &output) {
    return impl_->call(device, [&output](DeviceCore &core) {
        return core.capabilities(output);
    });
}

Status Runtime::wait_idle(vita_gpu_device device) {
    return impl_->call(device,
                       [](DeviceCore &core) { return core.wait_idle(); });
}

#define VITA_GPU_FORWARD(method, signature, invocation) \
    Status Runtime::method signature {                  \
        return impl_->call(device, [&](DeviceCore &core) { return core.invocation; }); \
    }

VITA_GPU_FORWARD(create_swapchain,
                 (vita_gpu_device device,
                  const vita_gpu_swapchain_desc &desc,
                  vita_gpu_swapchain &output),
                 create_swapchain(desc, output))
VITA_GPU_FORWARD(destroy_swapchain,
                 (vita_gpu_device device, vita_gpu_swapchain swapchain),
                 destroy_swapchain(swapchain))
VITA_GPU_FORWARD(acquire_swapchain,
                 (vita_gpu_device device, vita_gpu_swapchain swapchain,
                  vita_gpu_swapchain_image &output),
                 acquire_swapchain(swapchain, output))
VITA_GPU_FORWARD(resize_swapchain,
                 (vita_gpu_device device, vita_gpu_swapchain swapchain,
                  std::uint32_t width, std::uint32_t height),
                 resize_swapchain(swapchain, width, height))
VITA_GPU_FORWARD(present_swapchain,
                 (vita_gpu_device device, vita_gpu_swapchain swapchain,
                  const vita_gpu_present_desc &desc),
                 present_swapchain(swapchain, desc))
VITA_GPU_FORWARD(create_buffer,
                 (vita_gpu_device device, const vita_gpu_buffer_desc &desc,
                  vita_gpu_buffer &output),
                 create_buffer(desc, output))
VITA_GPU_FORWARD(destroy_buffer,
                 (vita_gpu_device device, vita_gpu_buffer buffer),
                 destroy_buffer(buffer))
VITA_GPU_FORWARD(create_texture,
                 (vita_gpu_device device, const vita_gpu_texture_desc &desc,
                  vita_gpu_texture &output),
                 create_texture(desc, output))
VITA_GPU_FORWARD(destroy_texture,
                 (vita_gpu_device device, vita_gpu_texture texture),
                 destroy_texture(texture))
VITA_GPU_FORWARD(create_sampler,
                 (vita_gpu_device device, const vita_gpu_sampler_desc &desc,
                  vita_gpu_sampler &output),
                 create_sampler(desc, output))
VITA_GPU_FORWARD(destroy_sampler,
                 (vita_gpu_device device, vita_gpu_sampler sampler),
                 destroy_sampler(sampler))
VITA_GPU_FORWARD(create_render_target,
                 (vita_gpu_device device,
                  const vita_gpu_render_target_desc &desc,
                  vita_gpu_render_target &output),
                 create_render_target(desc, output))
VITA_GPU_FORWARD(destroy_render_target,
                 (vita_gpu_device device, vita_gpu_render_target target),
                 destroy_render_target(target))
VITA_GPU_FORWARD(create_depth_target,
                 (vita_gpu_device device,
                  const vita_gpu_depth_target_desc &desc,
                  vita_gpu_depth_target &output),
                 create_depth_target(desc, output))
VITA_GPU_FORWARD(destroy_depth_target,
                 (vita_gpu_device device, vita_gpu_depth_target target),
                 destroy_depth_target(target))
VITA_GPU_FORWARD(create_shader,
                 (vita_gpu_device device,
                  const vita_gpu_shader_module_desc &desc,
                  vita_gpu_shader_module &output),
                 create_shader(desc, output))
VITA_GPU_FORWARD(destroy_shader,
                 (vita_gpu_device device, vita_gpu_shader_module shader),
                 destroy_shader(shader))
VITA_GPU_FORWARD(create_binding_layout,
                 (vita_gpu_device device,
                  const vita_gpu_binding_layout_desc &desc,
                  vita_gpu_binding_layout &output),
                 create_binding_layout(desc, output))
VITA_GPU_FORWARD(destroy_binding_layout,
                 (vita_gpu_device device, vita_gpu_binding_layout layout),
                 destroy_binding_layout(layout))
VITA_GPU_FORWARD(create_binding_set,
                 (vita_gpu_device device,
                  const vita_gpu_binding_set_desc &desc,
                  vita_gpu_binding_set &output),
                 create_binding_set(desc, output))
VITA_GPU_FORWARD(update_binding_set,
                 (vita_gpu_device device, vita_gpu_binding_set set,
                  std::span<const vita_gpu_binding_write> writes),
                 update_binding_set(set, writes))
VITA_GPU_FORWARD(destroy_binding_set,
                 (vita_gpu_device device, vita_gpu_binding_set set),
                 destroy_binding_set(set))
VITA_GPU_FORWARD(create_graphics_pipeline,
                 (vita_gpu_device device,
                  const vita_gpu_graphics_pipeline_desc &desc,
                  vita_gpu_pipeline &output),
                 create_graphics_pipeline(desc, output))
VITA_GPU_FORWARD(create_compute_pipeline,
                 (vita_gpu_device device,
                  const vita_gpu_compute_pipeline_desc &desc,
                  vita_gpu_pipeline &output),
                 create_compute_pipeline(desc, output))
VITA_GPU_FORWARD(destroy_pipeline,
                 (vita_gpu_device device, vita_gpu_pipeline pipeline),
                 destroy_pipeline(pipeline))
VITA_GPU_FORWARD(create_readback,
                 (vita_gpu_device device,
                  const vita_gpu_readback_desc &desc,
                  vita_gpu_readback &output),
                 create_readback(desc, output))
VITA_GPU_FORWARD(destroy_readback,
                 (vita_gpu_device device, vita_gpu_readback readback),
                 destroy_readback(readback))
VITA_GPU_FORWARD(map_readback,
                 (vita_gpu_device device, vita_gpu_readback readback,
                  vita_gpu_readback_map &output),
                 map_readback(readback, output))
VITA_GPU_FORWARD(unmap_readback,
                 (vita_gpu_device device, vita_gpu_readback readback),
                 unmap_readback(readback))
VITA_GPU_FORWARD(create_command_list,
                 (vita_gpu_device device,
                  const vita_gpu_command_list_desc &desc,
                  vita_gpu_command_list &output),
                 create_command_list(desc, output))
VITA_GPU_FORWARD(destroy_command_list,
                 (vita_gpu_device device, vita_gpu_command_list command),
                 destroy_command_list(command))
VITA_GPU_FORWARD(command_begin,
                 (vita_gpu_device device, vita_gpu_command_list command),
                 command_begin(command))
VITA_GPU_FORWARD(command_end,
                 (vita_gpu_device device, vita_gpu_command_list command),
                 command_end(command))
VITA_GPU_FORWARD(command_reset,
                 (vita_gpu_device device, vita_gpu_command_list command),
                 command_reset(command))
VITA_GPU_FORWARD(command_barriers,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  std::span<const vita_gpu_resource_barrier> barriers),
                 command_barriers(command, barriers))
VITA_GPU_FORWARD(command_begin_render_pass,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_render_pass_desc &desc),
                 command_begin_render_pass(command, desc))
VITA_GPU_FORWARD(command_end_render_pass,
                 (vita_gpu_device device, vita_gpu_command_list command),
                 command_end_render_pass(command))
VITA_GPU_FORWARD(command_bind_pipeline,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  vita_gpu_pipeline pipeline),
                 command_bind_pipeline(command, pipeline))
VITA_GPU_FORWARD(command_bind_set,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  std::uint32_t set_index, vita_gpu_binding_set set),
                 command_bind_set(command, set_index, set))
VITA_GPU_FORWARD(command_set_viewport,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_viewport &viewport),
                 command_set_viewport(command, viewport))
VITA_GPU_FORWARD(command_set_scissor,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_scissor &scissor),
                 command_set_scissor(command, scissor))
VITA_GPU_FORWARD(command_set_vertex_buffers,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  std::span<const vita_gpu_vertex_buffer_binding> bindings),
                 command_set_vertex_buffers(command, bindings))
VITA_GPU_FORWARD(command_set_index_buffer,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_index_buffer_binding &binding),
                 command_set_index_buffer(command, binding))
VITA_GPU_FORWARD(command_push_constants,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_push_constants_desc &desc),
                 command_push_constants(command, desc))
VITA_GPU_FORWARD(command_draw,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_draw_desc &desc),
                 command_draw(command, desc))
VITA_GPU_FORWARD(command_draw_indexed,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_draw_indexed_desc &desc),
                 command_draw_indexed(command, desc))
VITA_GPU_FORWARD(command_dispatch,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_dispatch_desc &desc),
                 command_dispatch(command, desc))
VITA_GPU_FORWARD(command_copy_buffer,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_buffer_copy_desc &desc),
                 command_copy_buffer(command, desc))
VITA_GPU_FORWARD(command_copy_buffer_readback,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_buffer_readback_copy_desc &desc),
                 command_copy_buffer_readback(command, desc))
VITA_GPU_FORWARD(command_copy_texture_readback,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  const vita_gpu_texture_readback_copy_desc &desc),
                 command_copy_texture_readback(command, desc))
VITA_GPU_FORWARD(command_debug_marker,
                 (vita_gpu_device device, vita_gpu_command_list command,
                  std::string_view label, std::uint32_t rgba_color),
                 command_debug_marker(command, label, rgba_color))
VITA_GPU_FORWARD(create_fence,
                 (vita_gpu_device device, const vita_gpu_fence_desc &desc,
                  vita_gpu_fence &output),
                 create_fence(desc, output))
VITA_GPU_FORWARD(destroy_fence,
                 (vita_gpu_device device, vita_gpu_fence fence),
                 destroy_fence(fence))
VITA_GPU_FORWARD(fence_completed,
                 (vita_gpu_device device, vita_gpu_fence fence,
                  std::uint64_t &output),
                 fence_completed(fence, output))
VITA_GPU_FORWARD(fence_wait,
                 (vita_gpu_device device, vita_gpu_fence fence,
                  std::uint64_t value, std::uint64_t timeout_nanoseconds),
                 fence_wait(fence, value, timeout_nanoseconds))
VITA_GPU_FORWARD(submit,
                 (vita_gpu_device device, vita_gpu_queue_type queue,
                  const vita_gpu_submit_desc &desc),
                 submit(queue, desc))
VITA_GPU_FORWARD(debug_capture_begin,
                 (vita_gpu_device device,
                  const vita_gpu_debug_capture_desc &desc),
                 debug_capture_begin(desc))
VITA_GPU_FORWARD(debug_capture_end,
                 (vita_gpu_device device),
                 debug_capture_end())

#undef VITA_GPU_FORWARD

} // namespace vitarecomp::gpu
