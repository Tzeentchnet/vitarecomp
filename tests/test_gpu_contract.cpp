#include "gpu_internal.hpp"
#include "test_only_null_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gpu = vitarecomp::gpu;
namespace test_backend = vitarecomp::gpu::test;

namespace {

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw TestFailure(std::string("CHECK failed at ") + __FILE__ +    \
                              ":" + std::to_string(__LINE__) + ": " +         \
                              #condition);                                     \
        }                                                                      \
    } while (false)

void expect(const gpu::Status &status, vita_gpu_result result,
            std::string_view context) {
    if (status.code != result) {
        throw TestFailure(std::string(context) + ": expected " +
                          vita_gpu_result_string(result) + ", got " +
                          vita_gpu_result_string(status.code) + " (" +
                          status.message + ")");
    }
}

struct Fixture {
    Fixture() : state(std::make_shared<test_backend::TestOnlyNullState>()) {
        gpu::BackendRegistry registry(true);
        expect(registry.register_factory(
                   test_backend::make_test_only_null_backend_factory(state)),
               VITA_GPU_SUCCESS, "register test-only backend");
        runtime = std::make_unique<gpu::Runtime>(std::move(registry));

        vita_gpu_device_desc desc = VITA_GPU_INIT(vita_gpu_device_desc);
        desc.backend = VITA_GPU_BACKEND_DX12;
        desc.adapter_index = 0;
        desc.flags = VITA_GPU_DEVICE_ENABLE_VALIDATION;
        desc.application_name = "gpu-contract-test";
        expect(runtime->create_device(desc, device), VITA_GPU_SUCCESS,
               "create test device");
    }

    void close() {
        expect(runtime->destroy_device(device), VITA_GPU_SUCCESS,
               "destroy test device");
        device = {};
    }

    std::shared_ptr<test_backend::TestOnlyNullState> state;
    std::unique_ptr<gpu::Runtime> runtime;
    vita_gpu_device device{};
};

vita_gpu_buffer_desc buffer_desc(vita_gpu_resource_state state,
                                 vita_gpu_buffer_usage usage,
                                 std::uint64_t size = 256) {
    vita_gpu_buffer_desc desc = VITA_GPU_INIT(vita_gpu_buffer_desc);
    desc.label = "test-buffer";
    desc.byte_size = size;
    desc.element_stride = 0;
    desc.element_format = VITA_GPU_FORMAT_UNKNOWN;
    desc.usage = usage;
    desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    desc.initial_state = state;
    return desc;
}

vita_gpu_texture_desc color_texture_desc(
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_RENDER_TARGET) {
    vita_gpu_texture_desc desc = VITA_GPU_INIT(vita_gpu_texture_desc);
    desc.label = "test-color";
    desc.dimension = VITA_GPU_TEXTURE_2D;
    desc.width = 64;
    desc.height = 64;
    desc.depth_or_layers = 1;
    desc.mip_levels = 1;
    desc.sample_count = 1;
    desc.format = VITA_GPU_FORMAT_RGBA8_UNORM;
    desc.usage = VITA_GPU_TEXTURE_RENDER_TARGET |
                 VITA_GPU_TEXTURE_COPY_SOURCE |
                 VITA_GPU_TEXTURE_COPY_DEST |
                 VITA_GPU_TEXTURE_SAMPLED;
    desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    desc.initial_state = state;
    return desc;
}

vita_gpu_render_target make_render_target(Fixture &fixture,
                                          vita_gpu_texture texture) {
    vita_gpu_render_target_desc desc =
        VITA_GPU_INIT(vita_gpu_render_target_desc);
    desc.label = "test-rt";
    desc.texture = texture;
    desc.format = VITA_GPU_FORMAT_RGBA8_UNORM;
    desc.mip_level = 0;
    desc.first_array_layer = 0;
    desc.array_layer_count = 1;
    vita_gpu_render_target target{};
    expect(fixture.runtime->create_render_target(fixture.device, desc, target),
           VITA_GPU_SUCCESS, "create render target");
    return target;
}

vita_gpu_command_list make_command(Fixture &fixture,
                                   vita_gpu_queue_type queue =
                                       VITA_GPU_QUEUE_GRAPHICS) {
    vita_gpu_command_list_desc desc =
        VITA_GPU_INIT(vita_gpu_command_list_desc);
    desc.label = "test-command";
    desc.queue = queue;
    vita_gpu_command_list command{};
    expect(fixture.runtime->create_command_list(fixture.device, desc, command),
           VITA_GPU_SUCCESS, "create command list");
    return command;
}

vita_gpu_fence make_fence(Fixture &fixture, std::uint64_t initial = 0) {
    vita_gpu_fence_desc desc = VITA_GPU_INIT(vita_gpu_fence_desc);
    desc.label = "test-fence";
    desc.initial_value = initial;
    vita_gpu_fence fence{};
    expect(fixture.runtime->create_fence(fixture.device, desc, fence),
           VITA_GPU_SUCCESS, "create fence");
    return fence;
}

void submit(Fixture &fixture, vita_gpu_command_list command,
            vita_gpu_fence fence, std::uint64_t value,
            vita_gpu_result expected = VITA_GPU_SUCCESS) {
    vita_gpu_fence_point signal = VITA_GPU_INIT(vita_gpu_fence_point);
    signal.fence = fence;
    signal.value = value;
    vita_gpu_submit_desc desc = VITA_GPU_INIT(vita_gpu_submit_desc);
    desc.command_lists = &command;
    desc.command_list_count = 1;
    desc.signal = signal;
    expect(fixture.runtime->submit(fixture.device, VITA_GPU_QUEUE_GRAPHICS,
                                   desc),
           expected, "submit command list");
}

vita_gpu_shader_module make_binary_shader(Fixture &fixture,
                                          vita_gpu_shader_stage stage) {
    static constexpr std::array<std::uint8_t, 4> bytecode{
        UINT8_C(0x44), UINT8_C(0x58), UINT8_C(0x49), UINT8_C(0x4c)};
    vita_gpu_shader_binary_desc binary =
        VITA_GPU_INIT(vita_gpu_shader_binary_desc);
    binary.stage = stage;
    binary.target = VITA_GPU_SHADER_TARGET_DXIL;
    binary.data = bytecode.data();
    binary.data_size = bytecode.size();
    binary.key = vita_gpu_shader_key VITA_GPU_INIT(vita_gpu_shader_key);

    vita_gpu_shader_module_desc desc =
        VITA_GPU_INIT(vita_gpu_shader_module_desc);
    desc.label = "test-shader";
    desc.input = VITA_GPU_SHADER_INPUT_BINARY;
    desc.binary = &binary;
    vita_gpu_shader_module shader{};
    expect(fixture.runtime->create_shader(fixture.device, desc, shader),
           VITA_GPU_SUCCESS, "create shader");
    return shader;
}

vita_gpu_pipeline make_graphics_pipeline(Fixture &fixture,
                                         vita_gpu_shader_module vertex,
                                         vita_gpu_shader_module pixel) {
    vita_gpu_graphics_pipeline_desc desc =
        VITA_GPU_INIT(vita_gpu_graphics_pipeline_desc);
    desc.label = "test-pipeline";
    desc.vertex_shader = vertex;
    desc.pixel_shader = pixel;
    desc.topology = VITA_GPU_TOPOLOGY_TRIANGLE_LIST;
    desc.rasterizer =
        vita_gpu_rasterizer_desc VITA_GPU_INIT(vita_gpu_rasterizer_desc);
    desc.rasterizer.polygon_mode = VITA_GPU_POLYGON_FILL;
    desc.rasterizer.cull_mode = VITA_GPU_CULL_NONE;
    desc.rasterizer.front_face = VITA_GPU_FRONT_COUNTER_CLOCKWISE;
    desc.depth_stencil =
        vita_gpu_depth_stencil_desc VITA_GPU_INIT(vita_gpu_depth_stencil_desc);
    desc.depth_stencil.depth_compare = VITA_GPU_COMPARE_ALWAYS;
    desc.color_target_count = 1;
    desc.color_formats[0] = VITA_GPU_FORMAT_RGBA8_UNORM;
    desc.blend[0] =
        vita_gpu_blend_attachment_desc
            VITA_GPU_INIT(vita_gpu_blend_attachment_desc);
    desc.blend[0].source_color = VITA_GPU_BLEND_ONE;
    desc.blend[0].destination_color = VITA_GPU_BLEND_ZERO;
    desc.blend[0].color_op = VITA_GPU_BLEND_OP_ADD;
    desc.blend[0].source_alpha = VITA_GPU_BLEND_ONE;
    desc.blend[0].destination_alpha = VITA_GPU_BLEND_ZERO;
    desc.blend[0].alpha_op = VITA_GPU_BLEND_OP_ADD;
    desc.blend[0].color_write_mask = 0x0f;
    desc.depth_format = VITA_GPU_FORMAT_UNKNOWN;
    desc.sample_count = 1;
    vita_gpu_pipeline pipeline{};
    expect(fixture.runtime->create_graphics_pipeline(fixture.device, desc,
                                                      pipeline),
           VITA_GPU_SUCCESS, "create graphics pipeline");
    return pipeline;
}

void test_descriptor_validation() {
    Fixture fixture;
    vita_gpu_buffer buffer{};

    auto desc = buffer_desc(VITA_GPU_RESOURCE_STATE_COPY_DEST,
                            VITA_GPU_BUFFER_COPY_DEST);
    desc.struct_size = sizeof(vita_gpu_struct_header);
    expect(fixture.runtime->create_buffer(fixture.device, desc, buffer),
           VITA_GPU_ERROR_INVALID_STRUCT, "short buffer descriptor");

    desc = buffer_desc(VITA_GPU_RESOURCE_STATE_COPY_DEST,
                       VITA_GPU_BUFFER_COPY_DEST);
    desc.byte_size = 0;
    expect(fixture.runtime->create_buffer(fixture.device, desc, buffer),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "zero-sized buffer");

    desc = buffer_desc(VITA_GPU_RESOURCE_STATE_COPY_DEST,
                       VITA_GPU_BUFFER_COPY_DEST);
    desc.initial_data_size = 4;
    expect(fixture.runtime->create_buffer(fixture.device, desc, buffer),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "missing initial data pointer");

    auto texture_desc = color_texture_desc();
    texture_desc.sample_count = 3;
    vita_gpu_texture texture{};
    expect(fixture.runtime->create_texture(fixture.device, texture_desc,
                                            texture),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "invalid sample count");

    vita_gpu_device_capabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    capabilities.struct_version = 99;
    expect(fixture.runtime->capabilities(fixture.device, capabilities),
           VITA_GPU_ERROR_UNSUPPORTED_ABI_VERSION,
           "capabilities output version");

    capabilities = vita_gpu_device_capabilities
        VITA_GPU_INIT(vita_gpu_device_capabilities);
    expect(fixture.runtime->capabilities(fixture.device, capabilities),
           VITA_GPU_SUCCESS, "capabilities");
    CHECK((capabilities.flags & VITA_GPU_CAPABILITY_GRAPHICS) != 0);

    std::uint32_t adapter_count = 0;
    expect(fixture.runtime->adapter_count(VITA_GPU_BACKEND_DX12,
                                          adapter_count),
           VITA_GPU_SUCCESS, "adapter count");
    CHECK(adapter_count == 1);
    vita_gpu_adapter_info adapter = VITA_GPU_INIT(vita_gpu_adapter_info);
    expect(fixture.runtime->adapter_info(VITA_GPU_BACKEND_DX12, 0, adapter),
           VITA_GPU_SUCCESS, "adapter info");
    CHECK(adapter.type == VITA_GPU_ADAPTER_SOFTWARE);

    const auto constant_desc =
        buffer_desc(VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER,
                    VITA_GPU_BUFFER_CONSTANT);
    vita_gpu_buffer constant_buffer{};
    expect(fixture.runtime->create_buffer(fixture.device, constant_desc,
                                           constant_buffer),
           VITA_GPU_SUCCESS, "create binding buffer");
    vita_gpu_binding_layout_entry entry =
        VITA_GPU_INIT(vita_gpu_binding_layout_entry);
    entry.binding = 0;
    entry.array_count = 1;
    entry.type = VITA_GPU_BINDING_CONSTANT_BUFFER;
    entry.visibility = VITA_GPU_SHADER_STAGE_VERTEX_BIT;
    vita_gpu_binding_layout_desc layout_desc =
        VITA_GPU_INIT(vita_gpu_binding_layout_desc);
    layout_desc.label = "test-layout";
    layout_desc.set_index = 0;
    layout_desc.entries = &entry;
    layout_desc.entry_count = 1;
    vita_gpu_binding_layout layout{};
    expect(fixture.runtime->create_binding_layout(fixture.device, layout_desc,
                                                   layout),
           VITA_GPU_SUCCESS, "create binding layout");
    vita_gpu_binding_set_desc set_desc =
        VITA_GPU_INIT(vita_gpu_binding_set_desc);
    set_desc.label = "test-set";
    set_desc.layout = layout;
    vita_gpu_binding_set set{};
    expect(fixture.runtime->create_binding_set(fixture.device, set_desc, set),
           VITA_GPU_SUCCESS, "create binding set");
    vita_gpu_binding_write write = VITA_GPU_INIT(vita_gpu_binding_write);
    write.binding = 0;
    write.type = VITA_GPU_BINDING_SAMPLER;
    expect(fixture.runtime->update_binding_set(fixture.device, set,
                                                std::span(&write, 1)),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "reject mismatched binding write");
    write.type = VITA_GPU_BINDING_CONSTANT_BUFFER;
    write.buffer = constant_buffer;
    write.buffer_size = constant_desc.byte_size;
    expect(fixture.runtime->update_binding_set(fixture.device, set,
                                                std::span(&write, 1)),
           VITA_GPU_SUCCESS, "update binding set");
    expect(fixture.runtime->destroy_buffer(fixture.device, constant_buffer),
           VITA_GPU_ERROR_RESOURCE_IN_USE, "binding retains buffer");
    expect(fixture.runtime->destroy_binding_set(fixture.device, set),
           VITA_GPU_SUCCESS, "destroy binding set");
    expect(fixture.runtime->destroy_binding_layout(fixture.device, layout),
           VITA_GPU_SUCCESS, "destroy binding layout");
    expect(fixture.runtime->destroy_buffer(fixture.device, constant_buffer),
           VITA_GPU_SUCCESS, "destroy binding buffer");

    fixture.close();
}

void test_handle_generation_and_owner() {
    gpu::HandlePool<int> pool(7);
    const std::uint64_t first = pool.insert(10);
    CHECK(pool.find(first));
    const auto removed = pool.remove(first);
    CHECK(removed.second == VITA_GPU_SUCCESS);
    CHECK(removed.first == 10);
    CHECK(pool.find(first).result == VITA_GPU_ERROR_STALE_HANDLE);
    const std::uint64_t second = pool.insert(20);
    CHECK((first & UINT64_C(0x00ffffff)) ==
          (second & UINT64_C(0x00ffffff)));
    CHECK(first != second);

    gpu::HandlePool<int> other_pool(8);
    (void)other_pool.insert(30);
    CHECK(other_pool.find(second).result == VITA_GPU_ERROR_WRONG_OWNER);

    Fixture fixture;
    const auto desc = buffer_desc(VITA_GPU_RESOURCE_STATE_COPY_DEST,
                                  VITA_GPU_BUFFER_COPY_DEST);
    vita_gpu_buffer old_buffer{};
    expect(fixture.runtime->create_buffer(fixture.device, desc, old_buffer),
           VITA_GPU_SUCCESS, "create old buffer");
    expect(fixture.runtime->destroy_buffer(fixture.device, old_buffer),
           VITA_GPU_SUCCESS, "destroy old buffer");
    vita_gpu_buffer new_buffer{};
    expect(fixture.runtime->create_buffer(fixture.device, desc, new_buffer),
           VITA_GPU_SUCCESS, "reuse buffer slot");
    CHECK(old_buffer.value != new_buffer.value);
    expect(fixture.runtime->destroy_buffer(fixture.device, old_buffer),
           VITA_GPU_ERROR_STALE_HANDLE, "reject stale buffer");
    expect(fixture.runtime->destroy_buffer(fixture.device, new_buffer),
           VITA_GPU_SUCCESS, "destroy new buffer");
    fixture.close();
}

void test_lifecycle_order() {
    Fixture fixture;
    vita_gpu_texture texture{};
    const auto texture_desc = color_texture_desc();
    expect(fixture.runtime->create_texture(fixture.device, texture_desc,
                                            texture),
           VITA_GPU_SUCCESS, "create texture");
    const vita_gpu_render_target target = make_render_target(fixture, texture);

    expect(fixture.runtime->destroy_texture(fixture.device, texture),
           VITA_GPU_ERROR_RESOURCE_IN_USE,
           "texture outlives its render target");
    expect(fixture.runtime->destroy_device(fixture.device),
           VITA_GPU_ERROR_RESOURCE_IN_USE,
           "device rejects live children");
    expect(fixture.runtime->destroy_render_target(fixture.device, target),
           VITA_GPU_SUCCESS, "destroy render target");
    expect(fixture.runtime->destroy_texture(fixture.device, texture),
           VITA_GPU_SUCCESS, "destroy texture");
    fixture.close();

    const auto events = fixture.state->events();
    const auto target_destroy = std::find_if(
        events.begin(), events.end(), [](const std::string &event) {
            return event.starts_with("destroy:render-target:");
        });
    const auto texture_destroy = std::find_if(
        events.begin(), events.end(), [](const std::string &event) {
            return event.starts_with("destroy:texture:");
        });
    const auto device_destroy =
        std::find(events.begin(), events.end(), "device:destroy");
    CHECK(target_destroy < texture_destroy);
    CHECK(texture_destroy < device_destroy);
}

void test_state_transitions() {
    Fixture fixture;
    vita_gpu_buffer buffer{};
    const auto desc = buffer_desc(
        VITA_GPU_RESOURCE_STATE_COPY_DEST,
        VITA_GPU_BUFFER_COPY_SOURCE | VITA_GPU_BUFFER_COPY_DEST |
            VITA_GPU_BUFFER_VERTEX);
    expect(fixture.runtime->create_buffer(fixture.device, desc, buffer),
           VITA_GPU_SUCCESS, "create transition buffer");
    const vita_gpu_command_list command = make_command(fixture);
    expect(fixture.runtime->command_begin(fixture.device, command),
           VITA_GPU_SUCCESS, "begin transition command");

    vita_gpu_resource_barrier barrier =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    barrier.kind = VITA_GPU_RESOURCE_BUFFER;
    barrier.buffer = buffer;
    barrier.before = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
    barrier.after = VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER;
    barrier.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    barrier.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    expect(fixture.runtime->command_barriers(fixture.device, command,
                                              std::span(&barrier, 1)),
           VITA_GPU_ERROR_INVALID_STATE, "wrong before state");

    barrier.before = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    expect(fixture.runtime->command_barriers(fixture.device, command,
                                              std::span(&barrier, 1)),
           VITA_GPU_SUCCESS, "valid transition");
    expect(fixture.runtime->command_barriers(fixture.device, command,
                                              std::span(&barrier, 1)),
           VITA_GPU_ERROR_INVALID_STATE, "stale recorded before state");
    expect(fixture.runtime->command_end(fixture.device, command),
           VITA_GPU_SUCCESS, "end transition command");

    const vita_gpu_fence fence = make_fence(fixture);
    submit(fixture, command, fence, 1);
    std::uint64_t completed = 0;
    expect(fixture.runtime->fence_completed(fixture.device, fence, completed),
           VITA_GPU_SUCCESS, "completed transition fence");
    CHECK(completed == 1);
    expect(fixture.runtime->command_reset(fixture.device, command),
           VITA_GPU_SUCCESS, "reset transition command");
    expect(fixture.runtime->command_begin(fixture.device, command),
           VITA_GPU_SUCCESS, "begin second transition command");
    barrier.before = VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER;
    barrier.after = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    expect(fixture.runtime->command_barriers(fixture.device, command,
                                              std::span(&barrier, 1)),
           VITA_GPU_SUCCESS, "global state updated after submit");
    expect(fixture.runtime->command_end(fixture.device, command),
           VITA_GPU_SUCCESS, "end second transition command");
    submit(fixture, command, fence, 2);
    expect(fixture.runtime->command_reset(fixture.device, command),
           VITA_GPU_SUCCESS, "reset second transition");
    expect(fixture.runtime->destroy_command_list(fixture.device, command),
           VITA_GPU_SUCCESS, "destroy transition command");

    const vita_gpu_command_list compute =
        make_command(fixture, VITA_GPU_QUEUE_COMPUTE);
    expect(fixture.runtime->command_begin(fixture.device, compute),
           VITA_GPU_SUCCESS, "begin queue transfer");
    barrier.before = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    barrier.after = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    barrier.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    barrier.destination_queue = VITA_GPU_QUEUE_COMPUTE;
    expect(fixture.runtime->command_barriers(fixture.device, compute,
                                              std::span(&barrier, 1)),
           VITA_GPU_SUCCESS, "queue ownership transition");
    expect(fixture.runtime->command_end(fixture.device, compute),
           VITA_GPU_SUCCESS, "end queue transfer");
    vita_gpu_fence_point signal = VITA_GPU_INIT(vita_gpu_fence_point);
    signal.fence = fence;
    signal.value = 3;
    vita_gpu_submit_desc compute_submit = VITA_GPU_INIT(vita_gpu_submit_desc);
    compute_submit.command_lists = &compute;
    compute_submit.command_list_count = 1;
    compute_submit.signal = signal;
    expect(fixture.runtime->submit(fixture.device, VITA_GPU_QUEUE_COMPUTE,
                                   compute_submit),
           VITA_GPU_SUCCESS, "submit queue transfer");
    expect(fixture.runtime->command_reset(fixture.device, compute),
           VITA_GPU_SUCCESS, "reset queue transfer");
    expect(fixture.runtime->destroy_command_list(fixture.device, compute),
           VITA_GPU_SUCCESS, "destroy queue transfer");

    expect(fixture.runtime->destroy_buffer(fixture.device, buffer),
           VITA_GPU_SUCCESS, "destroy transition buffer");
    expect(fixture.runtime->destroy_fence(fixture.device, fence),
           VITA_GPU_SUCCESS, "destroy transition fence");
    fixture.close();
}

void test_command_ordering() {
    Fixture fixture;
    vita_gpu_texture texture{};
    auto texture_desc = color_texture_desc();
    expect(fixture.runtime->create_texture(fixture.device, texture_desc,
                                            texture),
           VITA_GPU_SUCCESS, "create color texture");
    const vita_gpu_render_target target = make_render_target(fixture, texture);
    const vita_gpu_shader_module vertex =
        make_binary_shader(fixture, VITA_GPU_SHADER_VERTEX);
    const vita_gpu_shader_module pixel =
        make_binary_shader(fixture, VITA_GPU_SHADER_PIXEL);
    const vita_gpu_pipeline pipeline =
        make_graphics_pipeline(fixture, vertex, pixel);
    const vita_gpu_command_list command = make_command(fixture);

    expect(fixture.runtime->command_end(fixture.device, command),
           VITA_GPU_ERROR_INVALID_STATE, "end before begin");
    expect(fixture.runtime->command_begin(fixture.device, command),
           VITA_GPU_SUCCESS, "begin ordered command");
    vita_gpu_draw_desc draw = VITA_GPU_INIT(vita_gpu_draw_desc);
    draw.vertex_count = 3;
    draw.instance_count = 1;
    expect(fixture.runtime->command_draw(fixture.device, command, draw),
           VITA_GPU_ERROR_INVALID_STATE, "draw outside pass");

    vita_gpu_color_attachment color =
        VITA_GPU_INIT(vita_gpu_color_attachment);
    color.target = target;
    color.load_op = VITA_GPU_LOAD_CLEAR;
    color.store_op = VITA_GPU_STORE_PRESERVE;
    color.clear_color[3] = 1.0F;
    vita_gpu_render_pass_desc pass =
        VITA_GPU_INIT(vita_gpu_render_pass_desc);
    pass.color_attachments = &color;
    pass.color_attachment_count = 1;
    expect(fixture.runtime->command_begin_render_pass(fixture.device, command,
                                                       pass),
           VITA_GPU_SUCCESS, "begin render pass");
    expect(fixture.runtime->command_end(fixture.device, command),
           VITA_GPU_ERROR_INVALID_STATE, "end command with open pass");

    vita_gpu_resource_barrier barrier =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    barrier.kind = VITA_GPU_RESOURCE_TEXTURE;
    barrier.texture = texture;
    barrier.before = VITA_GPU_RESOURCE_STATE_RENDER_TARGET;
    barrier.after = VITA_GPU_RESOURCE_STATE_PRESENT;
    barrier.mip_count = 1;
    barrier.array_layer_count = 1;
    barrier.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    barrier.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    expect(fixture.runtime->command_barriers(fixture.device, command,
                                              std::span(&barrier, 1)),
           VITA_GPU_ERROR_INVALID_STATE, "barrier inside pass");

    expect(fixture.runtime->command_bind_pipeline(fixture.device, command,
                                                   pipeline),
           VITA_GPU_SUCCESS, "bind graphics pipeline");
    vita_gpu_viewport viewport = VITA_GPU_INIT(vita_gpu_viewport);
    viewport.width = 64.0F;
    viewport.height = 64.0F;
    viewport.max_depth = 1.0F;
    expect(fixture.runtime->command_set_viewport(fixture.device, command,
                                                  viewport),
           VITA_GPU_SUCCESS, "set viewport");
    expect(fixture.runtime->command_draw(fixture.device, command, draw),
           VITA_GPU_SUCCESS, "ordered draw");
    expect(fixture.runtime->command_end_render_pass(fixture.device, command),
           VITA_GPU_SUCCESS, "end render pass");
    expect(fixture.runtime->command_end(fixture.device, command),
           VITA_GPU_SUCCESS, "end ordered command");
    const vita_gpu_fence fence = make_fence(fixture);
    fixture.state->clear();
    submit(fixture, command, fence, 1);

    const std::vector<std::string> expected{
        "cmd:begin-render-pass", "cmd:bind-pipeline", "cmd:viewport",
        "cmd:draw", "cmd:end-render-pass", "submit:signal:1"};
    CHECK(fixture.state->events() == expected);

    expect(fixture.runtime->command_reset(fixture.device, command),
           VITA_GPU_SUCCESS, "reset ordered command");
    expect(fixture.runtime->destroy_command_list(fixture.device, command),
           VITA_GPU_SUCCESS, "destroy ordered command");
    expect(fixture.runtime->destroy_pipeline(fixture.device, pipeline),
           VITA_GPU_SUCCESS, "destroy pipeline");
    expect(fixture.runtime->destroy_shader(fixture.device, pixel),
           VITA_GPU_SUCCESS, "destroy pixel shader");
    expect(fixture.runtime->destroy_shader(fixture.device, vertex),
           VITA_GPU_SUCCESS, "destroy vertex shader");
    expect(fixture.runtime->destroy_render_target(fixture.device, target),
           VITA_GPU_SUCCESS, "destroy target");
    expect(fixture.runtime->destroy_texture(fixture.device, texture),
           VITA_GPU_SUCCESS, "destroy color texture");
    expect(fixture.runtime->destroy_fence(fixture.device, fence),
           VITA_GPU_SUCCESS, "destroy fence");
    fixture.close();
}

void test_fence_monotonicity() {
    Fixture fixture;
    const vita_gpu_fence fence = make_fence(fixture, 3);
    const vita_gpu_command_list first = make_command(fixture);
    expect(fixture.runtime->command_begin(fixture.device, first),
           VITA_GPU_SUCCESS, "begin first fence command");
    expect(fixture.runtime->command_end(fixture.device, first),
           VITA_GPU_SUCCESS, "end first fence command");
    submit(fixture, first, fence, 4);
    expect(fixture.runtime->command_reset(fixture.device, first),
           VITA_GPU_SUCCESS, "reset first fence command");

    expect(fixture.runtime->command_begin(fixture.device, first),
           VITA_GPU_SUCCESS, "begin reused fence command");
    expect(fixture.runtime->command_end(fixture.device, first),
           VITA_GPU_SUCCESS, "end reused fence command");
    submit(fixture, first, fence, 4, VITA_GPU_ERROR_INVALID_ARGUMENT);
    submit(fixture, first, fence, 5);

    std::uint64_t completed = 0;
    expect(fixture.runtime->fence_completed(fixture.device, fence, completed),
           VITA_GPU_SUCCESS, "query fence");
    CHECK(completed == 5);
    expect(fixture.runtime->fence_wait(fixture.device, fence, 6, 0),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "reject future CPU wait");
    expect(fixture.runtime->fence_wait(fixture.device, fence, 5, 0),
           VITA_GPU_SUCCESS, "wait completed fence");
    expect(fixture.runtime->destroy_fence(fixture.device, fence),
           VITA_GPU_ERROR_RESOURCE_IN_USE,
           "submitted command retains fence");
    expect(fixture.runtime->command_reset(fixture.device, first),
           VITA_GPU_SUCCESS, "reset submitted fence command");
    expect(fixture.runtime->destroy_command_list(fixture.device, first),
           VITA_GPU_SUCCESS, "destroy fence command");
    expect(fixture.runtime->destroy_fence(fixture.device, fence),
           VITA_GPU_SUCCESS, "destroy monotonic fence");
    fixture.close();
}

void test_resize() {
    Fixture fixture;
    vita_gpu_swapchain_desc desc = VITA_GPU_INIT(vita_gpu_swapchain_desc);
    desc.label = "test-swapchain";
    desc.window = vita_gpu_window_handle VITA_GPU_INIT(vita_gpu_window_handle);
    desc.window.system = VITA_GPU_WINDOW_WIN32;
    desc.window.native_window =
        reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
    desc.width = 640;
    desc.height = 480;
    desc.format = VITA_GPU_FORMAT_RGBA8_UNORM;
    desc.image_count = 2;
    desc.present_mode = VITA_GPU_PRESENT_FIFO;
    vita_gpu_swapchain swapchain{};
    expect(fixture.runtime->create_swapchain(fixture.device, desc, swapchain),
           VITA_GPU_SUCCESS, "create swapchain");

    vita_gpu_swapchain_image image =
        VITA_GPU_INIT(vita_gpu_swapchain_image);
    expect(fixture.runtime->acquire_swapchain(fixture.device, swapchain, image),
           VITA_GPU_SUCCESS, "acquire before resize");
    const vita_gpu_texture old_texture = image.texture;
    expect(fixture.runtime->resize_swapchain(fixture.device, swapchain, 800,
                                              600),
           VITA_GPU_ERROR_INVALID_STATE, "resize acquired swapchain");
    vita_gpu_present_desc present = VITA_GPU_INIT(vita_gpu_present_desc);
    present.image_index = image.image_index;
    expect(fixture.runtime->present_swapchain(fixture.device, swapchain,
                                               present),
           VITA_GPU_SUCCESS, "present before resize");
    expect(fixture.runtime->resize_swapchain(fixture.device, swapchain, 800,
                                              600),
           VITA_GPU_SUCCESS, "resize swapchain");
    expect(fixture.runtime->destroy_texture(fixture.device, old_texture),
           VITA_GPU_ERROR_STALE_HANDLE, "old swapchain image is stale");

    image = vita_gpu_swapchain_image VITA_GPU_INIT(vita_gpu_swapchain_image);
    expect(fixture.runtime->acquire_swapchain(fixture.device, swapchain, image),
           VITA_GPU_SUCCESS, "acquire after resize");
    CHECK(image.texture.value != old_texture.value);
    present.image_index = image.image_index;
    expect(fixture.runtime->present_swapchain(fixture.device, swapchain,
                                               present),
           VITA_GPU_SUCCESS, "present after resize");
    expect(fixture.runtime->destroy_swapchain(fixture.device, swapchain),
           VITA_GPU_SUCCESS, "destroy swapchain");
    fixture.close();

    const auto events = fixture.state->events();
    CHECK(std::find(events.begin(), events.end(), "swapchain:resize:800x600") !=
          events.end());
}

void test_readback_and_capture() {
    Fixture fixture;
    const auto source_desc =
        buffer_desc(VITA_GPU_RESOURCE_STATE_COPY_SOURCE,
                    VITA_GPU_BUFFER_COPY_SOURCE, 64);
    vita_gpu_buffer source{};
    expect(fixture.runtime->create_buffer(fixture.device, source_desc, source),
           VITA_GPU_SUCCESS, "create readback source");
    vita_gpu_readback_desc readback_desc =
        VITA_GPU_INIT(vita_gpu_readback_desc);
    readback_desc.label = "test-readback";
    readback_desc.byte_size = 64;
    vita_gpu_readback readback{};
    expect(fixture.runtime->create_readback(fixture.device, readback_desc,
                                             readback),
           VITA_GPU_SUCCESS, "create readback");
    const vita_gpu_command_list command = make_command(fixture);
    expect(fixture.runtime->command_begin(fixture.device, command),
           VITA_GPU_SUCCESS, "begin readback command");
    vita_gpu_buffer_readback_copy_desc copy =
        VITA_GPU_INIT(vita_gpu_buffer_readback_copy_desc);
    copy.source = source;
    copy.destination = readback;
    copy.byte_size = 64;
    expect(fixture.runtime->command_copy_buffer_readback(
               fixture.device, command, copy),
           VITA_GPU_SUCCESS, "record readback copy");
    expect(fixture.runtime->command_end(fixture.device, command),
           VITA_GPU_SUCCESS, "end readback command");
    const vita_gpu_fence fence = make_fence(fixture);
    submit(fixture, command, fence, 1);

    vita_gpu_readback_map map = VITA_GPU_INIT(vita_gpu_readback_map);
    expect(fixture.runtime->map_readback(fixture.device, readback, map),
           VITA_GPU_ERROR_RESOURCE_IN_USE,
           "mapping requires completed command reset");
    expect(fixture.runtime->command_reset(fixture.device, command),
           VITA_GPU_SUCCESS, "reset readback command");
    expect(fixture.runtime->map_readback(fixture.device, readback, map),
           VITA_GPU_SUCCESS, "map readback");
    CHECK(map.data != nullptr);
    CHECK(map.byte_size == 64);
    expect(fixture.runtime->unmap_readback(fixture.device, readback),
           VITA_GPU_SUCCESS, "unmap readback");

    vita_gpu_debug_capture_desc capture =
        VITA_GPU_INIT(vita_gpu_debug_capture_desc);
    capture.flags = VITA_GPU_DEBUG_CAPTURE_FRAME;
    expect(fixture.runtime->debug_capture_begin(fixture.device, capture),
           VITA_GPU_SUCCESS, "begin debug capture");
    expect(fixture.runtime->debug_capture_begin(fixture.device, capture),
           VITA_GPU_ERROR_INVALID_STATE, "reject nested debug capture");
    expect(fixture.runtime->debug_capture_end(fixture.device),
           VITA_GPU_SUCCESS, "end debug capture");

    expect(fixture.runtime->destroy_command_list(fixture.device, command),
           VITA_GPU_SUCCESS, "destroy readback command");
    expect(fixture.runtime->destroy_readback(fixture.device, readback),
           VITA_GPU_SUCCESS, "destroy readback");
    expect(fixture.runtime->destroy_buffer(fixture.device, source),
           VITA_GPU_SUCCESS, "destroy readback source");
    expect(fixture.runtime->destroy_fence(fixture.device, fence),
           VITA_GPU_SUCCESS, "destroy readback fence");
    fixture.close();
}

void test_backend_availability_policy() {
    auto state = std::make_shared<test_backend::TestOnlyNullState>();
    gpu::BackendRegistry production_registry;
    expect(production_registry.register_factory(
               test_backend::make_test_only_null_backend_factory(state)),
           VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
           "production registry rejects test backend");
    gpu::Runtime runtime(std::move(production_registry));
    for (const vita_gpu_backend backend :
         {VITA_GPU_BACKEND_DX12, VITA_GPU_BACKEND_VULKAN}) {
        vita_gpu_device_desc desc = VITA_GPU_INIT(vita_gpu_device_desc);
        desc.backend = backend;
        desc.application_name = "unavailable-test";
        vita_gpu_device device{};
        const gpu::Status status = runtime.create_device(desc, device);
        expect(status, VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
               "requested unavailable backend");
        CHECK(status.backend == backend);
        CHECK(status.message.find("fallback") != std::string::npos);
        CHECK(device.value == 0);
    }

    constexpr std::array<std::pair<vita_gpu_backend, vita_gpu_backend>, 2>
        cross_backend_requests{{
            {VITA_GPU_BACKEND_DX12, VITA_GPU_BACKEND_VULKAN},
            {VITA_GPU_BACKEND_VULKAN, VITA_GPU_BACKEND_DX12},
        }};
    for (const auto &[registered, requested] : cross_backend_requests) {
        auto isolated_state =
            std::make_shared<test_backend::TestOnlyNullState>();
        gpu::BackendRegistry one_factory_registry(true);
        expect(one_factory_registry.register_factory(
                   test_backend::make_test_only_null_backend_factory(
                       isolated_state, registered)),
               VITA_GPU_SUCCESS, "register one backend factory");
        gpu::Runtime one_factory_runtime(std::move(one_factory_registry));
        vita_gpu_device_desc desc = VITA_GPU_INIT(vita_gpu_device_desc);
        desc.backend = requested;
        desc.application_name = "cross-backend-fallback-test";
        vita_gpu_device device{};
        const gpu::Status status =
            one_factory_runtime.create_device(desc, device);
        expect(status, VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
               "do not fall back to another registered backend");
        CHECK(status.backend == requested);
        CHECK(status.message.find("fallback") != std::string::npos);
        CHECK(device.value == 0);
        CHECK(isolated_state->events().empty());
    }

    vita_gpu_backend parsed = VITA_GPU_BACKEND_NONE;
    CHECK(vita_gpu_backend_from_name("Dx12", &parsed, nullptr) ==
          VITA_GPU_SUCCESS);
    CHECK(parsed == VITA_GPU_BACKEND_DX12);
    CHECK(vita_gpu_backend_from_name("VULKAN", &parsed, nullptr) ==
          VITA_GPU_SUCCESS);
    CHECK(parsed == VITA_GPU_BACKEND_VULKAN);
    CHECK(vita_gpu_backend_from_name(" vulkan", &parsed, nullptr) ==
          VITA_GPU_ERROR_INVALID_ARGUMENT);
    CHECK(parsed == VITA_GPU_BACKEND_NONE);
    CHECK(vita_gpu_backend_from_name("null", &parsed, nullptr) ==
          VITA_GPU_ERROR_INVALID_ARGUMENT);
}

void test_backend_coexistence_and_namespaces() {
    auto dx12_state =
        std::make_shared<test_backend::TestOnlyNullState>();
    auto vulkan_state =
        std::make_shared<test_backend::TestOnlyNullState>();
    gpu::BackendRegistry registry(true);
    expect(registry.register_factory(
               test_backend::make_test_only_null_backend_factory(
                   dx12_state, VITA_GPU_BACKEND_DX12)),
           VITA_GPU_SUCCESS, "register DX12 test factory");
    expect(registry.register_factory(
               test_backend::make_test_only_null_backend_factory(
                   vulkan_state, VITA_GPU_BACKEND_VULKAN)),
           VITA_GPU_SUCCESS, "register Vulkan test factory");
    expect(registry.register_factory(
               test_backend::make_test_only_null_backend_factory(
                   dx12_state, VITA_GPU_BACKEND_DX12)),
           VITA_GPU_ERROR_INVALID_STATE, "reject duplicate DX12 factory");

    gpu::Runtime runtime(std::move(registry));
    CHECK(runtime.backend_count() == 2);
    for (std::uint32_t index = 0; index < runtime.backend_count(); ++index) {
        vita_gpu_backend_info info = VITA_GPU_INIT(vita_gpu_backend_info);
        expect(runtime.backend_info(index, info), VITA_GPU_SUCCESS,
               "query coexisting backend");
        const vita_gpu_backend expected =
            index == 0 ? VITA_GPU_BACKEND_DX12 : VITA_GPU_BACKEND_VULKAN;
        CHECK(info.backend == expected);
        CHECK(info.available == VITA_GPU_TRUE);
        CHECK(std::string_view(info.name) ==
              (expected == VITA_GPU_BACKEND_DX12 ? "dx12" : "vulkan"));
        std::uint32_t adapter_count = 0;
        expect(runtime.adapter_count(expected, adapter_count),
               VITA_GPU_SUCCESS, "enumerate coexisting backend");
        CHECK(adapter_count == 1);
    }

    vita_gpu_device previous_device{};
    vita_gpu_buffer previous_buffer{};
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (const vita_gpu_backend backend :
             {VITA_GPU_BACKEND_DX12, VITA_GPU_BACKEND_VULKAN}) {
            vita_gpu_device_desc device_desc =
                VITA_GPU_INIT(vita_gpu_device_desc);
            device_desc.backend = backend;
            device_desc.flags = VITA_GPU_DEVICE_ENABLE_VALIDATION;
            device_desc.application_name = "backend-coexistence-test";
            vita_gpu_device device{};
            expect(runtime.create_device(device_desc, device),
                   VITA_GPU_SUCCESS, "create sequential backend device");
            CHECK(device.value != 0);
            CHECK(device.value != previous_device.value);

            vita_gpu_buffer buffer{};
            const auto desc =
                buffer_desc(VITA_GPU_RESOURCE_STATE_COPY_DEST,
                            VITA_GPU_BUFFER_COPY_DEST);
            expect(runtime.create_buffer(device, desc, buffer),
                   VITA_GPU_SUCCESS, "create sequential backend buffer");
            CHECK(buffer.value != 0);
            CHECK(buffer.value != previous_buffer.value);
            if (previous_buffer.value != 0) {
                expect(runtime.destroy_buffer(device, previous_buffer),
                       VITA_GPU_ERROR_WRONG_OWNER,
                       "reject cross-backend resource handle");
            }
            expect(runtime.destroy_buffer(device, buffer), VITA_GPU_SUCCESS,
                   "destroy sequential backend buffer");
            expect(runtime.destroy_device(device), VITA_GPU_SUCCESS,
                   "destroy sequential backend device");
            previous_device = device;
            previous_buffer = buffer;
        }
    }
}

void test_shader_request_determinism() {
    constexpr char source[] =
        "float4 main(float4 p : POSITION) : SV_Position { return p; }\n";
    vita_gpu_shader_define defines_a[2] = {
        vita_gpu_shader_define VITA_GPU_INIT(vita_gpu_shader_define),
        vita_gpu_shader_define VITA_GPU_INIT(vita_gpu_shader_define)};
    defines_a[0].name = "ALPHA";
    defines_a[0].value = "1";
    defines_a[1].name = "BETA";
    defines_a[1].value = "2";
    vita_gpu_shader_define defines_b[2] = {defines_a[1], defines_a[0]};

    vita_gpu_shader_request_desc request =
        VITA_GPU_INIT(vita_gpu_shader_request_desc);
    request.source_name = "contract.hlsl";
    request.source = source;
    request.source_size = sizeof(source) - 1;
    request.entry_point = "main";
    request.profile = "vs_6_0";
    request.defines = defines_a;
    request.define_count = 2;
    request.stage = VITA_GPU_SHADER_VERTEX;
    request.target = VITA_GPU_SHADER_TARGET_DXIL;
    request.flags = VITA_GPU_SHADER_WARNINGS_AS_ERROR;

    vita_gpu_shader_key first = VITA_GPU_INIT(vita_gpu_shader_key);
    vita_gpu_shader_key second = VITA_GPU_INIT(vita_gpu_shader_key);
    expect(gpu::make_shader_key(request, first), VITA_GPU_SUCCESS,
           "first shader key");
    expect(gpu::make_shader_key(request, second), VITA_GPU_SUCCESS,
           "second shader key");
    CHECK(std::memcmp(first.bytes, second.bytes, sizeof(first.bytes)) == 0);
    constexpr std::array<std::uint8_t, VITA_GPU_SHADER_KEY_SIZE> golden{
        0x25, 0xcf, 0x0e, 0xa6, 0x93, 0x7f, 0x0f, 0xcb,
        0x2d, 0x81, 0xdc, 0xd0, 0x53, 0xaa, 0x11, 0x53,
        0xf0, 0x86, 0xba, 0xfa, 0xcf, 0x80, 0xcb, 0xdc,
        0xc4, 0x6b, 0x71, 0xae, 0x22, 0x81, 0x83, 0x71};
    CHECK(std::equal(golden.begin(), golden.end(), std::begin(first.bytes)));

    request.defines = defines_b;
    vita_gpu_shader_key reordered = VITA_GPU_INIT(vita_gpu_shader_key);
    expect(gpu::make_shader_key(request, reordered), VITA_GPU_SUCCESS,
           "reordered shader key");
    CHECK(std::memcmp(first.bytes, reordered.bytes, sizeof(first.bytes)) == 0);

    request.target = VITA_GPU_SHADER_TARGET_SPIRV;
    vita_gpu_shader_key spirv = VITA_GPU_INIT(vita_gpu_shader_key);
    expect(gpu::make_shader_key(request, spirv), VITA_GPU_SUCCESS,
           "SPIR-V shader key");
    CHECK(std::memcmp(first.bytes, spirv.bytes, sizeof(first.bytes)) != 0);

    defines_b[1] = defines_b[0];
    request.defines = defines_b;
    request.target = VITA_GPU_SHADER_TARGET_DXIL;
    expect(gpu::make_shader_key(request, spirv),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "duplicate shader define");
    request.defines = defines_a;
    request.profile = "ps_6_0";
    expect(gpu::make_shader_key(request, spirv),
           VITA_GPU_ERROR_INVALID_ARGUMENT, "profile/stage mismatch");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"descriptor validation", test_descriptor_validation},
        {"handle generation and owner", test_handle_generation_and_owner},
        {"lifecycle order", test_lifecycle_order},
        {"state transitions", test_state_transitions},
        {"command ordering", test_command_ordering},
        {"fence monotonicity", test_fence_monotonicity},
        {"resize", test_resize},
        {"readback and capture", test_readback_and_capture},
        {"backend availability policy", test_backend_availability_policy},
        {"backend coexistence and namespaces",
         test_backend_coexistence_and_namespaces},
        {"shader request determinism", test_shader_request_determinism},
    };

    std::size_t failures = 0;
    for (const auto &[name, function] : tests) {
        try {
            function();
            std::cout << "  " << name << " ok\n";
        } catch (const std::exception &exception) {
            ++failures;
            std::cerr << "  " << name << " FAILED: " << exception.what()
                      << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " GPU contract test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all GPU contract tests passed\n";
    return EXIT_SUCCESS;
}
