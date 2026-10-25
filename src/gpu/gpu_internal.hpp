#ifndef VITARECOMP_GPU_INTERNAL_HPP
#define VITARECOMP_GPU_INTERNAL_HPP

#include "vitarecomp/gpu.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vitarecomp::gpu {

struct Status {
    vita_gpu_result code = VITA_GPU_SUCCESS;
    vita_gpu_backend backend = VITA_GPU_BACKEND_NONE;
    std::int64_t native_code = 0;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code == VITA_GPU_SUCCESS; }

    static Status success() { return {}; }
    static Status error(vita_gpu_result result, std::string text,
                        vita_gpu_backend source = VITA_GPU_BACKEND_NONE,
                        std::int64_t native = 0) {
        return Status{result, source, native, std::move(text)};
    }
};

template <typename T>
struct HandleLookup {
    T *value = nullptr;
    vita_gpu_result result = VITA_GPU_ERROR_INVALID_HANDLE;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value != nullptr;
    }
};

/*
 * Index and generation are both encoded in the public token. Destroying an
 * entry increments its generation before the slot can be reused, making stale
 * handles deterministic rather than dependent on allocator reuse.
 */
template <typename T>
class HandlePool {
public:
    explicit HandlePool(std::uint16_t owner_tag = 0)
        : owner_tag_(owner_tag) {}

    [[nodiscard]] std::uint64_t insert(T value) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            if (slots_.size() >= kIndexMask) {
                throw std::bad_alloc();
            }
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back({});
        }

        Slot &slot = slots_[index];
        slot.value.emplace(std::move(value));
        ++live_count_;
        return (static_cast<std::uint64_t>(owner_tag_) << 48U) |
               (static_cast<std::uint64_t>(slot.generation) << 24U) |
               (static_cast<std::uint64_t>(index) + 1U);
    }

    [[nodiscard]] HandleLookup<T> find(std::uint64_t handle) noexcept {
        if (handle == 0) {
            return {};
        }
        const std::uint32_t encoded_index =
            static_cast<std::uint32_t>(handle & kIndexMask);
        if (encoded_index == 0) {
            return {};
        }
        const std::uint32_t index = encoded_index - 1U;
        if (index >= slots_.size()) {
            return {};
        }

        Slot &slot = slots_[index];
        const std::uint16_t encoded_owner =
            static_cast<std::uint16_t>(handle >> 48U);
        const std::uint32_t generation =
            static_cast<std::uint32_t>((handle >> 24U) & kGenerationMask);
        if (owner_tag_ != 0 && encoded_owner != owner_tag_) {
            return {nullptr, VITA_GPU_ERROR_WRONG_OWNER};
        }
        if (generation != slot.generation || !slot.value.has_value()) {
            return {nullptr, VITA_GPU_ERROR_STALE_HANDLE};
        }
        return {&*slot.value, VITA_GPU_SUCCESS};
    }

    [[nodiscard]] HandleLookup<const T> find(std::uint64_t handle) const noexcept {
        const auto mutable_result = const_cast<HandlePool *>(this)->find(handle);
        return {mutable_result.value, mutable_result.result};
    }

    [[nodiscard]] std::pair<std::optional<T>, vita_gpu_result> remove(
        std::uint64_t handle) {
        auto lookup = find(handle);
        if (!lookup) {
            return {std::nullopt, lookup.result};
        }

        const std::uint32_t index =
            static_cast<std::uint32_t>(handle & kIndexMask) - 1U;
        Slot &slot = slots_[index];
        std::optional<T> value{std::move(slot.value)};
        slot.value.reset();
        ++slot.generation;
        if (slot.generation > kGenerationMask) {
            slot.generation = 1;
        } else {
            free_.push_back(index);
        }
        --live_count_;
        return {std::move(value), VITA_GPU_SUCCESS};
    }

    [[nodiscard]] std::size_t size() const noexcept { return live_count_; }

    template <typename F>
    void for_each(F &&function) {
        for (Slot &slot : slots_) {
            if (slot.value.has_value()) {
                function(*slot.value);
            }
        }
    }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t live_count_ = 0;
    std::uint16_t owner_tag_ = 0;

    static constexpr std::uint64_t kIndexMask = UINT64_C(0x00ffffff);
    static constexpr std::uint64_t kGenerationMask = UINT64_C(0x00ffffff);
};

struct NativeId {
    std::uint64_t value = 0;

    friend bool operator==(NativeId, NativeId) = default;
};

enum class ObjectKind : std::uint8_t {
    swapchain,
    buffer,
    texture,
    sampler,
    render_target,
    depth_target,
    shader,
    binding_layout,
    binding_set,
    pipeline,
    fence,
    readback
};

struct BackendSwapchainImage {
    std::uint32_t image_index = 0;
    NativeId texture;
    NativeId render_target;
};

struct BackendBindingWrite {
    std::uint32_t binding = 0;
    std::uint32_t array_element = 0;
    vita_gpu_binding_type type = VITA_GPU_BINDING_CONSTANT_BUFFER;
    NativeId buffer;
    std::uint64_t buffer_offset = 0;
    std::uint64_t buffer_size = 0;
    NativeId texture;
    vita_gpu_resource_state texture_state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    NativeId sampler;
};

struct BackendGraphicsPipelineDesc {
    const vita_gpu_graphics_pipeline_desc *source = nullptr;
    NativeId vertex_shader;
    NativeId pixel_shader;
    std::vector<NativeId> binding_layouts;
};

struct BackendComputePipelineDesc {
    const vita_gpu_compute_pipeline_desc *source = nullptr;
    NativeId compute_shader;
    std::vector<NativeId> binding_layouts;
};

struct BackendColorAttachment {
    NativeId target;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_load_op load_op = VITA_GPU_LOAD_PRESERVE;
    vita_gpu_store_op store_op = VITA_GPU_STORE_PRESERVE;
    std::array<float, 4> clear_color{};
};

struct BackendDepthAttachment {
    NativeId target;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_load_op depth_load_op = VITA_GPU_LOAD_PRESERVE;
    vita_gpu_store_op depth_store_op = VITA_GPU_STORE_PRESERVE;
    vita_gpu_load_op stencil_load_op = VITA_GPU_LOAD_PRESERVE;
    vita_gpu_store_op stencil_store_op = VITA_GPU_STORE_PRESERVE;
    float clear_depth = 1.0F;
    std::uint32_t clear_stencil = 0;
};

struct BarrierCommand {
    vita_gpu_resource_kind kind = VITA_GPU_RESOURCE_BUFFER;
    NativeId resource;
    std::uint64_t logical_handle = 0;
    vita_gpu_resource_state before = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_resource_state after = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    std::uint32_t first_mip = 0;
    std::uint32_t mip_count = 0;
    std::uint32_t first_array_layer = 0;
    std::uint32_t array_layer_count = 0;
    vita_gpu_queue_type source_queue = VITA_GPU_QUEUE_GRAPHICS;
    vita_gpu_queue_type destination_queue = VITA_GPU_QUEUE_GRAPHICS;
};

struct BeginRenderPassCommand {
    std::vector<BackendColorAttachment> colors;
    std::optional<BackendDepthAttachment> depth;
};

struct EndRenderPassCommand {};
struct BindPipelineCommand { NativeId pipeline; };
struct BindSetCommand {
    std::uint32_t set_index = 0;
    NativeId set;
};
struct ViewportCommand { vita_gpu_viewport viewport{}; };
struct ScissorCommand { vita_gpu_scissor scissor{}; };
struct VertexBufferCommand {
    std::uint32_t binding = 0;
    NativeId buffer;
    std::uint64_t offset = 0;
};
struct IndexBufferCommand {
    NativeId buffer;
    std::uint64_t offset = 0;
    vita_gpu_index_format format = VITA_GPU_INDEX_UINT16;
};
struct PushConstantsCommand {
    vita_gpu_shader_stage_flags stages = 0;
    std::uint32_t offset = 0;
    std::vector<std::uint8_t> data;
};
struct DrawCommand { vita_gpu_draw_desc draw{}; };
struct DrawIndexedCommand { vita_gpu_draw_indexed_desc draw{}; };
struct DispatchCommand { vita_gpu_dispatch_desc dispatch{}; };
struct CopyBufferCommand {
    NativeId source;
    std::uint64_t source_offset = 0;
    NativeId destination;
    std::uint64_t destination_offset = 0;
    std::uint64_t byte_size = 0;
};
struct CopyBufferReadbackCommand {
    NativeId source;
    std::uint64_t source_offset = 0;
    NativeId destination;
    std::uint64_t destination_offset = 0;
    std::uint64_t byte_size = 0;
};
struct CopyTextureReadbackCommand {
    NativeId source;
    vita_gpu_resource_state source_state =
        VITA_GPU_RESOURCE_STATE_UNDEFINED;
    std::uint32_t mip_level = 0;
    std::uint32_t array_layer = 0;
    NativeId destination;
    std::uint64_t destination_offset = 0;
    std::uint32_t row_pitch = 0;
    std::uint32_t slice_pitch = 0;
};
struct DebugMarkerCommand {
    std::string label;
    std::uint32_t rgba_color = 0;
};

using CommandPayload = std::variant<
    BarrierCommand, BeginRenderPassCommand, EndRenderPassCommand,
    BindPipelineCommand, BindSetCommand, ViewportCommand, ScissorCommand,
    VertexBufferCommand, IndexBufferCommand, PushConstantsCommand, DrawCommand,
    DrawIndexedCommand, DispatchCommand, CopyBufferCommand,
    CopyBufferReadbackCommand, CopyTextureReadbackCommand, DebugMarkerCommand>;

struct RecordedCommandList {
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
    std::vector<CommandPayload> commands;
};

struct BackendFencePoint {
    NativeId fence;
    std::uint64_t value = 0;
};

struct BackendReadbackMap {
    const std::uint8_t *data = nullptr;
    std::uint64_t byte_size = 0;
    std::uint32_t row_pitch = 0;
    std::uint32_t slice_pitch = 0;
};

class BackendDevice {
public:
    virtual ~BackendDevice() = default;

    [[nodiscard]] virtual vita_gpu_backend backend() const noexcept = 0;
    virtual Status capabilities(vita_gpu_device_capabilities &output) = 0;
    virtual Status wait_idle() = 0;

    virtual Status create_swapchain(const vita_gpu_swapchain_desc &desc,
                                    NativeId &output) = 0;
    virtual Status resize_swapchain(NativeId swapchain, std::uint32_t width,
                                    std::uint32_t height) = 0;
    virtual Status acquire_swapchain(NativeId swapchain,
                                     BackendSwapchainImage &output) = 0;
    virtual Status present_swapchain(NativeId swapchain,
                                     const vita_gpu_present_desc &desc) = 0;

    virtual Status create_buffer(const vita_gpu_buffer_desc &desc,
                                 NativeId &output) = 0;
    virtual Status create_texture(const vita_gpu_texture_desc &desc,
                                  NativeId &output) = 0;
    virtual Status create_sampler(const vita_gpu_sampler_desc &desc,
                                  NativeId &output) = 0;
    virtual Status create_render_target(
        const vita_gpu_render_target_desc &desc, NativeId texture,
        NativeId &output) = 0;
    virtual Status create_depth_target(
        const vita_gpu_depth_target_desc &desc, NativeId texture,
        NativeId &output) = 0;
    virtual Status create_shader_module(
        const vita_gpu_shader_module_desc &desc, NativeId &output) = 0;
    virtual Status create_binding_layout(
        const vita_gpu_binding_layout_desc &desc, NativeId &output) = 0;
    virtual Status create_binding_set(const vita_gpu_binding_set_desc &desc,
                                      NativeId layout, NativeId &output) = 0;
    virtual Status update_binding_set(
        NativeId set, std::span<const BackendBindingWrite> writes) = 0;
    virtual Status create_graphics_pipeline(
        const BackendGraphicsPipelineDesc &desc, NativeId &output) = 0;
    virtual Status create_compute_pipeline(
        const BackendComputePipelineDesc &desc, NativeId &output) = 0;
    virtual Status create_readback(const vita_gpu_readback_desc &desc,
                                   NativeId &output) = 0;
    virtual Status create_fence(const vita_gpu_fence_desc &desc,
                                NativeId &output) = 0;
    virtual Status destroy_object(ObjectKind kind, NativeId object) = 0;

    virtual Status submit(
        vita_gpu_queue_type queue,
        std::span<const RecordedCommandList *const> command_lists,
        std::span<const BackendFencePoint> waits,
        BackendFencePoint signal) = 0;
    virtual Status fence_completed_value(NativeId fence,
                                         std::uint64_t &value) = 0;
    virtual Status fence_wait(NativeId fence, std::uint64_t value,
                              std::uint64_t timeout_nanoseconds) = 0;

    virtual Status map_readback(NativeId readback,
                                BackendReadbackMap &output) = 0;
    virtual Status unmap_readback(NativeId readback) = 0;
    virtual Status debug_capture_begin(
        const vita_gpu_debug_capture_desc &desc) = 0;
    virtual Status debug_capture_end() = 0;
};

class BackendFactory {
public:
    virtual ~BackendFactory() = default;

    [[nodiscard]] virtual vita_gpu_backend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool test_only() const noexcept = 0;
    virtual Status availability(bool &available, bool &validation_available,
                                std::string &reason) const = 0;
    virtual Status adapter_count(std::uint32_t &count) const = 0;
    virtual Status adapter_info(std::uint32_t index,
                                vita_gpu_adapter_info &output) const = 0;
    virtual Status create_device(const vita_gpu_device_desc &desc,
                                 std::unique_ptr<BackendDevice> &output) const = 0;
};

class BackendRegistry {
public:
    explicit BackendRegistry(bool allow_test_backends = false)
        : allow_test_backends_(allow_test_backends) {}

    Status register_factory(std::shared_ptr<BackendFactory> factory);
    void seal() noexcept { sealed_ = true; }
    [[nodiscard]] std::shared_ptr<const BackendFactory> find(
        vita_gpu_backend backend) const;
    [[nodiscard]] bool allows_test_backends() const noexcept {
        return allow_test_backends_;
    }

private:
    static std::optional<std::size_t> slot(vita_gpu_backend backend) noexcept;

    bool allow_test_backends_ = false;
    bool sealed_ = false;
    std::array<std::shared_ptr<BackendFactory>, 2> factories_{};
};

class Runtime {
public:
    explicit Runtime(BackendRegistry registry);
    ~Runtime();
    Runtime(Runtime &&) noexcept;
    Runtime &operator=(Runtime &&) noexcept;
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    [[nodiscard]] std::uint32_t backend_count() const noexcept;
    Status backend_info(std::uint32_t index, vita_gpu_backend_info &output);
    Status adapter_count(vita_gpu_backend backend, std::uint32_t &count);
    Status adapter_info(vita_gpu_backend backend, std::uint32_t index,
                        vita_gpu_adapter_info &output);

    Status create_device(const vita_gpu_device_desc &desc,
                         vita_gpu_device &output);
    Status destroy_device(vita_gpu_device device);
    Status capabilities(vita_gpu_device device,
                        vita_gpu_device_capabilities &output);
    Status wait_idle(vita_gpu_device device);

    Status create_swapchain(vita_gpu_device device,
                            const vita_gpu_swapchain_desc &desc,
                            vita_gpu_swapchain &output);
    Status destroy_swapchain(vita_gpu_device device,
                             vita_gpu_swapchain swapchain);
    Status acquire_swapchain(vita_gpu_device device,
                             vita_gpu_swapchain swapchain,
                             vita_gpu_swapchain_image &output);
    Status resize_swapchain(vita_gpu_device device,
                            vita_gpu_swapchain swapchain, std::uint32_t width,
                            std::uint32_t height);
    Status present_swapchain(vita_gpu_device device,
                             vita_gpu_swapchain swapchain,
                             const vita_gpu_present_desc &desc);

    Status create_buffer(vita_gpu_device device,
                         const vita_gpu_buffer_desc &desc,
                         vita_gpu_buffer &output);
    Status destroy_buffer(vita_gpu_device device, vita_gpu_buffer buffer);
    Status create_texture(vita_gpu_device device,
                          const vita_gpu_texture_desc &desc,
                          vita_gpu_texture &output);
    Status destroy_texture(vita_gpu_device device, vita_gpu_texture texture);
    Status create_sampler(vita_gpu_device device,
                          const vita_gpu_sampler_desc &desc,
                          vita_gpu_sampler &output);
    Status destroy_sampler(vita_gpu_device device, vita_gpu_sampler sampler);
    Status create_render_target(vita_gpu_device device,
                                const vita_gpu_render_target_desc &desc,
                                vita_gpu_render_target &output);
    Status destroy_render_target(vita_gpu_device device,
                                 vita_gpu_render_target target);
    Status create_depth_target(vita_gpu_device device,
                               const vita_gpu_depth_target_desc &desc,
                               vita_gpu_depth_target &output);
    Status destroy_depth_target(vita_gpu_device device,
                                vita_gpu_depth_target target);
    Status create_shader(vita_gpu_device device,
                         const vita_gpu_shader_module_desc &desc,
                         vita_gpu_shader_module &output);
    Status destroy_shader(vita_gpu_device device,
                          vita_gpu_shader_module shader);
    Status create_binding_layout(vita_gpu_device device,
                                 const vita_gpu_binding_layout_desc &desc,
                                 vita_gpu_binding_layout &output);
    Status destroy_binding_layout(vita_gpu_device device,
                                  vita_gpu_binding_layout layout);
    Status create_binding_set(vita_gpu_device device,
                              const vita_gpu_binding_set_desc &desc,
                              vita_gpu_binding_set &output);
    Status update_binding_set(vita_gpu_device device, vita_gpu_binding_set set,
                              std::span<const vita_gpu_binding_write> writes);
    Status destroy_binding_set(vita_gpu_device device,
                               vita_gpu_binding_set set);
    Status create_graphics_pipeline(
        vita_gpu_device device, const vita_gpu_graphics_pipeline_desc &desc,
        vita_gpu_pipeline &output);
    Status create_compute_pipeline(
        vita_gpu_device device, const vita_gpu_compute_pipeline_desc &desc,
        vita_gpu_pipeline &output);
    Status destroy_pipeline(vita_gpu_device device, vita_gpu_pipeline pipeline);

    Status create_readback(vita_gpu_device device,
                           const vita_gpu_readback_desc &desc,
                           vita_gpu_readback &output);
    Status destroy_readback(vita_gpu_device device,
                            vita_gpu_readback readback);
    Status map_readback(vita_gpu_device device, vita_gpu_readback readback,
                        vita_gpu_readback_map &output);
    Status unmap_readback(vita_gpu_device device, vita_gpu_readback readback);

    Status create_command_list(vita_gpu_device device,
                               const vita_gpu_command_list_desc &desc,
                               vita_gpu_command_list &output);
    Status destroy_command_list(vita_gpu_device device,
                                vita_gpu_command_list command);
    Status command_begin(vita_gpu_device device, vita_gpu_command_list command);
    Status command_end(vita_gpu_device device, vita_gpu_command_list command);
    Status command_reset(vita_gpu_device device, vita_gpu_command_list command);
    Status command_barriers(
        vita_gpu_device device, vita_gpu_command_list command,
        std::span<const vita_gpu_resource_barrier> barriers);
    Status command_begin_render_pass(vita_gpu_device device,
                                     vita_gpu_command_list command,
                                     const vita_gpu_render_pass_desc &desc);
    Status command_end_render_pass(vita_gpu_device device,
                                   vita_gpu_command_list command);
    Status command_bind_pipeline(vita_gpu_device device,
                                 vita_gpu_command_list command,
                                 vita_gpu_pipeline pipeline);
    Status command_bind_set(vita_gpu_device device,
                            vita_gpu_command_list command,
                            std::uint32_t set_index, vita_gpu_binding_set set);
    Status command_set_viewport(vita_gpu_device device,
                                vita_gpu_command_list command,
                                const vita_gpu_viewport &viewport);
    Status command_set_scissor(vita_gpu_device device,
                               vita_gpu_command_list command,
                               const vita_gpu_scissor &scissor);
    Status command_set_vertex_buffers(
        vita_gpu_device device, vita_gpu_command_list command,
        std::span<const vita_gpu_vertex_buffer_binding> bindings);
    Status command_set_index_buffer(
        vita_gpu_device device, vita_gpu_command_list command,
        const vita_gpu_index_buffer_binding &binding);
    Status command_push_constants(vita_gpu_device device,
                                  vita_gpu_command_list command,
                                  const vita_gpu_push_constants_desc &desc);
    Status command_draw(vita_gpu_device device, vita_gpu_command_list command,
                        const vita_gpu_draw_desc &desc);
    Status command_draw_indexed(vita_gpu_device device,
                                vita_gpu_command_list command,
                                const vita_gpu_draw_indexed_desc &desc);
    Status command_dispatch(vita_gpu_device device,
                            vita_gpu_command_list command,
                            const vita_gpu_dispatch_desc &desc);
    Status command_copy_buffer(vita_gpu_device device,
                               vita_gpu_command_list command,
                               const vita_gpu_buffer_copy_desc &desc);
    Status command_copy_buffer_readback(
        vita_gpu_device device, vita_gpu_command_list command,
        const vita_gpu_buffer_readback_copy_desc &desc);
    Status command_copy_texture_readback(
        vita_gpu_device device, vita_gpu_command_list command,
        const vita_gpu_texture_readback_copy_desc &desc);
    Status command_debug_marker(vita_gpu_device device,
                                vita_gpu_command_list command,
                                std::string_view label,
                                std::uint32_t rgba_color);

    Status create_fence(vita_gpu_device device,
                        const vita_gpu_fence_desc &desc, vita_gpu_fence &output);
    Status destroy_fence(vita_gpu_device device, vita_gpu_fence fence);
    Status fence_completed(vita_gpu_device device, vita_gpu_fence fence,
                           std::uint64_t &output);
    Status fence_wait(vita_gpu_device device, vita_gpu_fence fence,
                      std::uint64_t value, std::uint64_t timeout_nanoseconds);
    Status submit(vita_gpu_device device, vita_gpu_queue_type queue,
                  const vita_gpu_submit_desc &desc);

    Status debug_capture_begin(vita_gpu_device device,
                               const vita_gpu_debug_capture_desc &desc);
    Status debug_capture_end(vita_gpu_device device);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

BackendRegistry make_production_registry();
Runtime &production_runtime();

Status validate_shader_request(const vita_gpu_shader_request_desc &request);
Status make_shader_key(const vita_gpu_shader_request_desc &request,
                       vita_gpu_shader_key &output);
Status validate_struct(const void *descriptor, std::size_t required_size,
                       std::string_view name);
Status validate_output_struct(const void *descriptor, std::size_t required_size,
                              std::string_view name);
Status validate_device_desc(const vita_gpu_device_desc &desc);
Status validate_swapchain_desc(const vita_gpu_swapchain_desc &desc);
Status validate_buffer_desc(const vita_gpu_buffer_desc &desc);
Status validate_texture_desc(const vita_gpu_texture_desc &desc);
Status validate_sampler_desc(const vita_gpu_sampler_desc &desc);
Status validate_render_target_desc(const vita_gpu_render_target_desc &desc);
Status validate_depth_target_desc(const vita_gpu_depth_target_desc &desc);
Status validate_shader_module_desc(const vita_gpu_shader_module_desc &desc);
Status validate_binding_layout_desc(const vita_gpu_binding_layout_desc &desc);
Status validate_binding_write(const vita_gpu_binding_write &write);
Status validate_graphics_pipeline_desc(
    const vita_gpu_graphics_pipeline_desc &desc);
Status validate_compute_pipeline_desc(
    const vita_gpu_compute_pipeline_desc &desc);

void write_error(vita_gpu_error *error, const Status &status,
                 std::string_view function_name) noexcept;

} // namespace vitarecomp::gpu

#endif
