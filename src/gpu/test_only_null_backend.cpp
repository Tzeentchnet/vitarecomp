#include "test_only_null_backend.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace vitarecomp::gpu::test {

void TestOnlyNullState::record(std::string event) {
    std::scoped_lock lock(mutex_);
    events_.push_back(std::move(event));
}

std::vector<std::string> TestOnlyNullState::events() const {
    std::scoped_lock lock(mutex_);
    return events_;
}

void TestOnlyNullState::clear() {
    std::scoped_lock lock(mutex_);
    events_.clear();
}

namespace {

const char *kind_name(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::swapchain:
        return "swapchain";
    case ObjectKind::buffer:
        return "buffer";
    case ObjectKind::texture:
        return "texture";
    case ObjectKind::sampler:
        return "sampler";
    case ObjectKind::render_target:
        return "render-target";
    case ObjectKind::depth_target:
        return "depth-target";
    case ObjectKind::shader:
        return "shader";
    case ObjectKind::binding_layout:
        return "binding-layout";
    case ObjectKind::binding_set:
        return "binding-set";
    case ObjectKind::pipeline:
        return "pipeline";
    case ObjectKind::fence:
        return "fence";
    case ObjectKind::readback:
        return "readback";
    }
    return "unknown";
}

template <typename T>
constexpr const char *command_name() {
    if constexpr (std::is_same_v<T, BarrierCommand>) {
        return "barrier";
    } else if constexpr (std::is_same_v<T, BeginRenderPassCommand>) {
        return "begin-render-pass";
    } else if constexpr (std::is_same_v<T, EndRenderPassCommand>) {
        return "end-render-pass";
    } else if constexpr (std::is_same_v<T, BindPipelineCommand>) {
        return "bind-pipeline";
    } else if constexpr (std::is_same_v<T, BindSetCommand>) {
        return "bind-set";
    } else if constexpr (std::is_same_v<T, ViewportCommand>) {
        return "viewport";
    } else if constexpr (std::is_same_v<T, ScissorCommand>) {
        return "scissor";
    } else if constexpr (std::is_same_v<T, VertexBufferCommand>) {
        return "vertex-buffer";
    } else if constexpr (std::is_same_v<T, IndexBufferCommand>) {
        return "index-buffer";
    } else if constexpr (std::is_same_v<T, PushConstantsCommand>) {
        return "push-constants";
    } else if constexpr (std::is_same_v<T, DrawCommand>) {
        return "draw";
    } else if constexpr (std::is_same_v<T, DrawIndexedCommand>) {
        return "draw-indexed";
    } else if constexpr (std::is_same_v<T, DispatchCommand>) {
        return "dispatch";
    } else if constexpr (std::is_same_v<T, CopyBufferCommand>) {
        return "copy-buffer";
    } else if constexpr (std::is_same_v<T, CopyBufferReadbackCommand>) {
        return "copy-buffer-readback";
    } else if constexpr (std::is_same_v<T, CopyTextureReadbackCommand>) {
        return "copy-texture-readback";
    } else {
        return "marker";
    }
}

class TestOnlyNullDevice final : public BackendDevice {
public:
    TestOnlyNullDevice(std::shared_ptr<TestOnlyNullState> state,
                       vita_gpu_backend backend)
        : state_(std::move(state)), backend_(backend) {
        state_->record("device:create");
    }

    ~TestOnlyNullDevice() override {
        state_->record("device:destroy");
    }

    [[nodiscard]] vita_gpu_backend backend() const noexcept override {
        return backend_;
    }

    Status capabilities(vita_gpu_device_capabilities &output) override {
        output.flags =
            VITA_GPU_CAPABILITY_GRAPHICS | VITA_GPU_CAPABILITY_COMPUTE |
            VITA_GPU_CAPABILITY_TIMELINE_FENCES |
            VITA_GPU_CAPABILITY_DEBUG_MARKERS |
            VITA_GPU_CAPABILITY_DEBUG_CAPTURE |
            VITA_GPU_CAPABILITY_BC_TEXTURES |
            VITA_GPU_CAPABILITY_ANISOTROPY;
        output.max_texture_1d = 16384;
        output.max_texture_2d = 16384;
        output.max_texture_3d = 2048;
        output.max_texture_array_layers = 2048;
        output.max_color_targets = VITA_GPU_MAX_COLOR_TARGETS;
        output.max_binding_sets = 8;
        output.max_bindings_per_set = 256;
        output.max_push_constant_bytes = 256;
        output.max_sampler_anisotropy = 16;
        output.constant_buffer_alignment = 256;
        output.storage_buffer_alignment = 16;
        output.texture_row_pitch_alignment = 256;
        output.timestamp_period_numerator = 1;
        output.timestamp_period_denominator = 1;
        return Status::success();
    }

    Status wait_idle() override {
        state_->record("device:wait-idle");
        return Status::success();
    }

    Status create_swapchain(const vita_gpu_swapchain_desc &desc,
                            NativeId &output) override {
        output = allocate(ObjectKind::swapchain);
        Swapchain value;
        value.width = desc.width;
        value.height = desc.height;
        value.image_count = desc.image_count;
        regenerate_images(value);
        swapchains_.emplace(output.value, std::move(value));
        return Status::success();
    }

    Status resize_swapchain(NativeId swapchain, std::uint32_t width,
                            std::uint32_t height) override {
        const auto found = swapchains_.find(swapchain.value);
        if (found == swapchains_.end()) {
            return missing("swapchain");
        }
        found->second.width = width;
        found->second.height = height;
        regenerate_images(found->second);
        state_->record("swapchain:resize:" + std::to_string(width) + "x" +
                       std::to_string(height));
        return Status::success();
    }

    Status acquire_swapchain(NativeId swapchain,
                             BackendSwapchainImage &output) override {
        const auto found = swapchains_.find(swapchain.value);
        if (found == swapchains_.end()) {
            return missing("swapchain");
        }
        Swapchain &value = found->second;
        const std::uint32_t index = value.next_image++ % value.image_count;
        output = {index, value.textures[index], value.targets[index]};
        state_->record("swapchain:acquire:" + std::to_string(index));
        return Status::success();
    }

    Status present_swapchain(NativeId swapchain,
                             const vita_gpu_present_desc &desc) override {
        if (!objects_.contains(swapchain.value)) {
            return missing("swapchain");
        }
        state_->record("swapchain:present:" +
                       std::to_string(desc.image_index));
        return Status::success();
    }

    Status create_buffer(const vita_gpu_buffer_desc &,
                         NativeId &output) override {
        output = allocate(ObjectKind::buffer);
        return Status::success();
    }

    Status create_texture(const vita_gpu_texture_desc &,
                          NativeId &output) override {
        output = allocate(ObjectKind::texture);
        return Status::success();
    }

    Status create_sampler(const vita_gpu_sampler_desc &,
                          NativeId &output) override {
        output = allocate(ObjectKind::sampler);
        return Status::success();
    }

    Status create_render_target(const vita_gpu_render_target_desc &,
                                NativeId texture,
                                NativeId &output) override {
        if (!objects_.contains(texture.value)) {
            return missing("texture");
        }
        output = allocate(ObjectKind::render_target);
        return Status::success();
    }

    Status create_depth_target(const vita_gpu_depth_target_desc &,
                               NativeId texture,
                               NativeId &output) override {
        if (!objects_.contains(texture.value)) {
            return missing("texture");
        }
        output = allocate(ObjectKind::depth_target);
        return Status::success();
    }

    Status create_shader_module(const vita_gpu_shader_module_desc &,
                                NativeId &output) override {
        output = allocate(ObjectKind::shader);
        return Status::success();
    }

    Status create_binding_layout(const vita_gpu_binding_layout_desc &,
                                 NativeId &output) override {
        output = allocate(ObjectKind::binding_layout);
        return Status::success();
    }

    Status create_binding_set(const vita_gpu_binding_set_desc &,
                              NativeId layout, NativeId &output) override {
        if (!objects_.contains(layout.value)) {
            return missing("binding layout");
        }
        output = allocate(ObjectKind::binding_set);
        return Status::success();
    }

    Status update_binding_set(
        NativeId set, std::span<const BackendBindingWrite> writes) override {
        if (!objects_.contains(set.value)) {
            return missing("binding set");
        }
        state_->record("binding-set:update:" + std::to_string(writes.size()));
        return Status::success();
    }

    Status create_graphics_pipeline(const BackendGraphicsPipelineDesc &,
                                    NativeId &output) override {
        output = allocate(ObjectKind::pipeline);
        return Status::success();
    }

    Status create_compute_pipeline(const BackendComputePipelineDesc &,
                                   NativeId &output) override {
        output = allocate(ObjectKind::pipeline);
        return Status::success();
    }

    Status create_readback(const vita_gpu_readback_desc &desc,
                           NativeId &output) override {
        output = allocate(ObjectKind::readback);
        readbacks_.emplace(
            output.value,
            std::vector<std::uint8_t>(static_cast<std::size_t>(desc.byte_size)));
        return Status::success();
    }

    Status create_fence(const vita_gpu_fence_desc &desc,
                        NativeId &output) override {
        output = allocate(ObjectKind::fence);
        fences_.emplace(output.value, desc.initial_value);
        return Status::success();
    }

    Status destroy_object(ObjectKind kind, NativeId object) override {
        const auto found = objects_.find(object.value);
        if (found == objects_.end() || found->second != kind) {
            return missing(kind_name(kind));
        }
        state_->record("destroy:" + std::string(kind_name(kind)) + ":" +
                       std::to_string(object.value));
        objects_.erase(found);
        if (kind == ObjectKind::swapchain) {
            swapchains_.erase(object.value);
        } else if (kind == ObjectKind::readback) {
            readbacks_.erase(object.value);
        } else if (kind == ObjectKind::fence) {
            fences_.erase(object.value);
        }
        return Status::success();
    }

    Status submit(
        vita_gpu_queue_type,
        std::span<const RecordedCommandList *const> command_lists,
        std::span<const BackendFencePoint> waits,
        BackendFencePoint signal) override {
        for (const BackendFencePoint &wait : waits) {
            const auto fence = fences_.find(wait.fence.value);
            if (fence == fences_.end() || fence->second < wait.value) {
                return Status::error(VITA_GPU_ERROR_INVALID_STATE,
                                     "test-only wait fence is incomplete");
            }
        }
        for (const RecordedCommandList *command_list : command_lists) {
            for (const CommandPayload &payload : command_list->commands) {
                std::visit(
                    [this](const auto &command) {
                        using T = std::decay_t<decltype(command)>;
                        state_->record("cmd:" +
                                       std::string(command_name<T>()));
                    },
                    payload);
            }
        }
        const auto fence = fences_.find(signal.fence.value);
        if (fence == fences_.end()) {
            return missing("signal fence");
        }
        fence->second = signal.value;
        state_->record("submit:signal:" + std::to_string(signal.value));
        return Status::success();
    }

    Status fence_completed_value(NativeId fence,
                                 std::uint64_t &value) override {
        const auto found = fences_.find(fence.value);
        if (found == fences_.end()) {
            return missing("fence");
        }
        value = found->second;
        return Status::success();
    }

    Status fence_wait(NativeId fence, std::uint64_t value,
                      std::uint64_t) override {
        const auto found = fences_.find(fence.value);
        if (found == fences_.end()) {
            return missing("fence");
        }
        if (found->second < value) {
            return Status::error(VITA_GPU_ERROR_TIMEOUT,
                                 "test-only fence value is incomplete");
        }
        return Status::success();
    }

    Status map_readback(NativeId readback,
                        BackendReadbackMap &output) override {
        const auto found = readbacks_.find(readback.value);
        if (found == readbacks_.end()) {
            return missing("readback");
        }
        output.data = found->second.data();
        output.byte_size = found->second.size();
        output.row_pitch =
            static_cast<std::uint32_t>(std::min<std::size_t>(
                found->second.size(), UINT32_MAX));
        output.slice_pitch = output.row_pitch;
        state_->record("readback:map");
        return Status::success();
    }

    Status unmap_readback(NativeId readback) override {
        if (!readbacks_.contains(readback.value)) {
            return missing("readback");
        }
        state_->record("readback:unmap");
        return Status::success();
    }

    Status debug_capture_begin(
        const vita_gpu_debug_capture_desc &) override {
        state_->record("capture:begin");
        return Status::success();
    }

    Status debug_capture_end() override {
        state_->record("capture:end");
        return Status::success();
    }

private:
    struct Swapchain {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t image_count = 0;
        std::uint32_t next_image = 0;
        std::vector<NativeId> textures;
        std::vector<NativeId> targets;
    };

    NativeId allocate(ObjectKind kind) {
        const NativeId id{next_id_++};
        objects_.emplace(id.value, kind);
        state_->record("create:" + std::string(kind_name(kind)) + ":" +
                       std::to_string(id.value));
        return id;
    }

    void regenerate_images(Swapchain &swapchain) {
        swapchain.next_image = 0;
        swapchain.textures.clear();
        swapchain.targets.clear();
        for (std::uint32_t i = 0; i < swapchain.image_count; ++i) {
            /*
             * Swapchain image IDs belong to the swapchain and therefore are
             * not entered in objects_; the core never destroys them directly.
             */
            swapchain.textures.push_back(NativeId{next_id_++});
            swapchain.targets.push_back(NativeId{next_id_++});
        }
    }

    Status missing(std::string_view what) const {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "test-only backend cannot find " +
                                 std::string(what));
    }

    std::shared_ptr<TestOnlyNullState> state_;
    vita_gpu_backend backend_ = VITA_GPU_BACKEND_NONE;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, ObjectKind> objects_;
    std::unordered_map<std::uint64_t, Swapchain> swapchains_;
    std::unordered_map<std::uint64_t, std::uint64_t> fences_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> readbacks_;
};

class TestOnlyNullFactory final : public BackendFactory {
public:
    TestOnlyNullFactory(std::shared_ptr<TestOnlyNullState> state,
                        vita_gpu_backend backend)
        : state_(std::move(state)), backend_(backend),
          name_(backend == VITA_GPU_BACKEND_DX12
                    ? "test-only-null-dx12"
                    : "test-only-null-vulkan") {}

    [[nodiscard]] vita_gpu_backend backend() const noexcept override {
        return backend_;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }

    [[nodiscard]] bool test_only() const noexcept override {
        return true;
    }

    Status availability(bool &available, bool &validation_available,
                        std::string &reason) const override {
        available = true;
        validation_available = true;
        reason.clear();
        return Status::success();
    }

    Status adapter_count(std::uint32_t &count) const override {
        count = 1;
        return Status::success();
    }

    Status adapter_info(std::uint32_t index,
                        vita_gpu_adapter_info &output) const override {
        if (index != 0) {
            return Status::error(VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                                 "test-only adapter index is out of range");
        }
        output.adapter_index = 0;
        output.type = VITA_GPU_ADAPTER_SOFTWARE;
        output.vendor_id = 0;
        output.device_id = 0;
        output.dedicated_video_memory = 0;
        output.dedicated_system_memory = 0;
        output.shared_system_memory = UINT64_C(1024) * 1024U * 1024U;
        output.platform_id = UINT64_C(0x544553544e554c4c);
        std::fill(std::begin(output.device_uuid),
                  std::end(output.device_uuid), std::uint8_t{0});
        output.capabilities =
            VITA_GPU_CAPABILITY_GRAPHICS | VITA_GPU_CAPABILITY_COMPUTE |
            VITA_GPU_CAPABILITY_TIMELINE_FENCES;
        constexpr char name[] = "vitarecomp test-only null adapter";
        std::memset(output.name, 0, sizeof(output.name));
        std::memcpy(output.name, name, sizeof(name));
        return Status::success();
    }

    Status create_device(
        const vita_gpu_device_desc &,
        std::unique_ptr<BackendDevice> &output) const override {
        output = std::make_unique<TestOnlyNullDevice>(state_, backend_);
        return Status::success();
    }

private:
    std::shared_ptr<TestOnlyNullState> state_;
    vita_gpu_backend backend_ = VITA_GPU_BACKEND_NONE;
    std::string name_;
};

} // namespace

std::shared_ptr<BackendFactory> make_test_only_null_backend_factory(
    std::shared_ptr<TestOnlyNullState> state, vita_gpu_backend backend) {
    return std::make_shared<TestOnlyNullFactory>(std::move(state), backend);
}

} // namespace vitarecomp::gpu::test
