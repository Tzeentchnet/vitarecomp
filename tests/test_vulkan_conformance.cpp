#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "vitarecomp/gpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Failure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

vita_gpu_error make_error() {
    return vita_gpu_error VITA_GPU_INIT(vita_gpu_error);
}

void require(vita_gpu_result result, vita_gpu_error &error,
             const char *operation) {
    if (result != VITA_GPU_SUCCESS) {
        throw Failure(
            std::string(operation) + ": " +
            vita_gpu_result_string(result) + " (" + error.message + ")");
    }
}

std::vector<std::uint8_t> read_binary(const char *path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw Failure(std::string("cannot open shader: ") + path);
    }
    const std::streamsize length = stream.tellg();
    if (length <= 0) {
        throw Failure(std::string("empty shader: ") + path);
    }
    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(
        reinterpret_cast<char *>(data.data()), length);
    if (!stream) {
        throw Failure(std::string("cannot read shader: ") + path);
    }
    return data;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND create_window(HINSTANCE instance, std::uint32_t width,
                   std::uint32_t height) {
    RECT rectangle{
        0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (AdjustWindowRect(
            &rectangle, WS_OVERLAPPEDWINDOW, FALSE) == FALSE) {
        throw Failure("AdjustWindowRect failed");
    }
    HWND window = CreateWindowExW(
        0, L"VitarecompVulkanConformance",
        L"vitarecomp Vulkan conformance", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top, nullptr, nullptr, instance,
        nullptr);
    if (window == nullptr) {
        throw Failure("CreateWindowExW failed");
    }
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    pump_messages();
    return window;
}

void resize_window(HWND window, std::uint32_t width,
                   std::uint32_t height) {
    RECT rectangle{
        0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (AdjustWindowRect(
            &rectangle, WS_OVERLAPPEDWINDOW, FALSE) == FALSE ||
        SetWindowPos(
            window, nullptr, 0, 0,
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        throw Failure("resizing Win32 conformance window failed");
    }
    pump_messages();
}

vita_gpu_shader_module create_shader(
    vita_gpu_device device, vita_gpu_shader_stage stage,
    const std::vector<std::uint8_t> &data) {
    vita_gpu_shader_binary_desc binary =
        VITA_GPU_INIT(vita_gpu_shader_binary_desc);
    binary.stage = stage;
    binary.target = VITA_GPU_SHADER_TARGET_SPIRV;
    binary.data = data.data();
    binary.data_size = data.size();
    binary.key = vita_gpu_shader_key VITA_GPU_INIT(vita_gpu_shader_key);
    vita_gpu_shader_module_desc descriptor =
        VITA_GPU_INIT(vita_gpu_shader_module_desc);
    descriptor.label =
        stage == VITA_GPU_SHADER_VERTEX ? "triangle-vs" : "triangle-ps";
    descriptor.input = VITA_GPU_SHADER_INPUT_BINARY;
    descriptor.binary = &binary;
    vita_gpu_shader_module shader{};
    vita_gpu_error error = make_error();
    require(vita_gpu_create_shader_module(
                device, &descriptor, &shader, &error),
            error, "create SPIR-V shader");
    return shader;
}

vita_gpu_command_list create_command(vita_gpu_device device) {
    vita_gpu_command_list_desc descriptor =
        VITA_GPU_INIT(vita_gpu_command_list_desc);
    descriptor.label = "vulkan-conformance-command";
    descriptor.queue = VITA_GPU_QUEUE_GRAPHICS;
    vita_gpu_command_list command{};
    vita_gpu_error error = make_error();
    require(vita_gpu_create_command_list(
                device, &descriptor, &command, &error),
            error, "create command list");
    return command;
}

void wait_and_reset(vita_gpu_device device, vita_gpu_command_list command,
                    vita_gpu_fence fence, std::uint64_t value) {
    vita_gpu_error error = make_error();
    require(vita_gpu_fence_wait(
                device, fence, value, UINT64_C(5000000000), &error),
            error, "wait timeline fence");
    require(vita_gpu_command_reset(device, command, &error),
            error, "reset command list");
}

void submit(vita_gpu_device device, vita_gpu_command_list command,
            vita_gpu_fence fence, std::uint64_t value) {
    vita_gpu_fence_point signal =
        VITA_GPU_INIT(vita_gpu_fence_point);
    signal.fence = fence;
    signal.value = value;
    vita_gpu_submit_desc descriptor =
        VITA_GPU_INIT(vita_gpu_submit_desc);
    descriptor.command_lists = &command;
    descriptor.command_list_count = 1;
    descriptor.signal = signal;
    vita_gpu_error error = make_error();
    require(vita_gpu_submit(
                device, VITA_GPU_QUEUE_GRAPHICS, &descriptor, &error),
            error, "submit Vulkan command list");
}

void exercise_resources(vita_gpu_device device, vita_gpu_fence fence,
                        std::uint64_t &fence_value) {
    constexpr std::array<std::uint8_t, 16> source{
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
    vita_gpu_buffer_desc buffer_desc =
        VITA_GPU_INIT(vita_gpu_buffer_desc);
    buffer_desc.label = "staged-gpu-buffer";
    buffer_desc.byte_size = source.size();
    buffer_desc.usage =
        VITA_GPU_BUFFER_COPY_SOURCE | VITA_GPU_BUFFER_COPY_DEST;
    buffer_desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    buffer_desc.initial_state = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
    buffer_desc.initial_data = source.data();
    buffer_desc.initial_data_size = source.size();
    vita_gpu_buffer buffer{};
    vita_gpu_error error = make_error();
    require(vita_gpu_create_buffer(
                device, &buffer_desc, &buffer, &error),
            error, "create staged Vulkan buffer");

    vita_gpu_readback_desc readback_desc =
        VITA_GPU_INIT(vita_gpu_readback_desc);
    readback_desc.label = "buffer-readback";
    readback_desc.byte_size = source.size();
    vita_gpu_readback readback{};
    require(vita_gpu_create_readback(
                device, &readback_desc, &readback, &error),
            error, "create buffer readback");
    const vita_gpu_command_list command = create_command(device);
    require(vita_gpu_command_begin(device, command, &error),
            error, "begin buffer copy");
    vita_gpu_buffer_readback_copy_desc copy =
        VITA_GPU_INIT(vita_gpu_buffer_readback_copy_desc);
    copy.source = buffer;
    copy.destination = readback;
    copy.byte_size = source.size();
    require(vita_gpu_cmd_copy_buffer_to_readback(
                device, command, &copy, &error),
            error, "record buffer readback");
    require(vita_gpu_command_end(device, command, &error),
            error, "end buffer copy");
    submit(device, command, fence, ++fence_value);
    wait_and_reset(device, command, fence, fence_value);

    vita_gpu_readback_map mapping =
        VITA_GPU_INIT(vita_gpu_readback_map);
    require(vita_gpu_map_readback(
                device, readback, &mapping, &error),
            error, "map buffer readback");
    if (mapping.byte_size < source.size() ||
        std::memcmp(mapping.data, source.data(), source.size()) != 0) {
        throw Failure("Vulkan buffer upload/readback mismatch");
    }
    require(vita_gpu_unmap_readback(device, readback, &error),
            error, "unmap buffer readback");
    require(vita_gpu_destroy_command_list(device, command, &error),
            error, "destroy buffer copy command");
    require(vita_gpu_destroy_readback(device, readback, &error),
            error, "destroy buffer readback");
    require(vita_gpu_destroy_buffer(device, buffer, &error),
            error, "destroy staged Vulkan buffer");
}

struct SampledFixture {
    vita_gpu_texture texture{};
    vita_gpu_sampler sampler{};
    vita_gpu_binding_layout layout{};
    vita_gpu_binding_set set{};
    vita_gpu_texture depth_texture{};
    vita_gpu_depth_target depth_target{};
};

SampledFixture create_sampled_fixture(vita_gpu_device device) {
    constexpr std::array<std::uint8_t, 16> texels{
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255};
    vita_gpu_subresource_data data =
        VITA_GPU_INIT(vita_gpu_subresource_data);
    data.data = texels.data();
    data.data_size = texels.size();
    data.row_pitch = 8;
    data.slice_pitch = 16;
    vita_gpu_texture_desc texture_desc =
        VITA_GPU_INIT(vita_gpu_texture_desc);
    texture_desc.label = "sampled-upload-texture";
    texture_desc.dimension = VITA_GPU_TEXTURE_2D;
    texture_desc.width = 2;
    texture_desc.height = 2;
    texture_desc.depth_or_layers = 1;
    texture_desc.mip_levels = 1;
    texture_desc.sample_count = 1;
    texture_desc.format = VITA_GPU_FORMAT_RGBA8_UNORM;
    texture_desc.usage =
        VITA_GPU_TEXTURE_SAMPLED | VITA_GPU_TEXTURE_COPY_DEST;
    texture_desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    texture_desc.initial_state = VITA_GPU_RESOURCE_STATE_SHADER_READ;
    texture_desc.initial_data = &data;
    texture_desc.initial_data_count = 1;
    SampledFixture fixture;
    vita_gpu_error error = make_error();
    require(vita_gpu_create_texture(
                device, &texture_desc, &fixture.texture, &error),
            error, "create sampled Vulkan texture");

    vita_gpu_sampler_desc sampler_desc =
        VITA_GPU_INIT(vita_gpu_sampler_desc);
    sampler_desc.label = "linear-sampler";
    sampler_desc.min_filter = VITA_GPU_FILTER_LINEAR;
    sampler_desc.mag_filter = VITA_GPU_FILTER_LINEAR;
    sampler_desc.mipmap_mode = VITA_GPU_MIPMAP_LINEAR;
    sampler_desc.address_u = VITA_GPU_ADDRESS_CLAMP_EDGE;
    sampler_desc.address_v = VITA_GPU_ADDRESS_CLAMP_EDGE;
    sampler_desc.address_w = VITA_GPU_ADDRESS_CLAMP_EDGE;
    sampler_desc.max_lod = 1.0F;
    sampler_desc.max_anisotropy = 1;
    sampler_desc.compare_op = VITA_GPU_COMPARE_ALWAYS;
    require(vita_gpu_create_sampler(
                device, &sampler_desc, &fixture.sampler, &error),
            error, "create Vulkan sampler");

    std::array<vita_gpu_binding_layout_entry, 2> entries{
        vita_gpu_binding_layout_entry
            VITA_GPU_INIT(vita_gpu_binding_layout_entry),
        vita_gpu_binding_layout_entry
            VITA_GPU_INIT(vita_gpu_binding_layout_entry)};
    entries[0].binding = 0;
    entries[0].array_count = 1;
    entries[0].type = VITA_GPU_BINDING_SAMPLED_TEXTURE;
    entries[0].visibility = VITA_GPU_SHADER_STAGE_PIXEL_BIT;
    entries[1].binding = 1;
    entries[1].array_count = 1;
    entries[1].type = VITA_GPU_BINDING_SAMPLER;
    entries[1].visibility = VITA_GPU_SHADER_STAGE_PIXEL_BIT;
    vita_gpu_binding_layout_desc layout_desc =
        VITA_GPU_INIT(vita_gpu_binding_layout_desc);
    layout_desc.label = "sampled-layout";
    layout_desc.entries = entries.data();
    layout_desc.entry_count =
        static_cast<std::uint32_t>(entries.size());
    require(vita_gpu_create_binding_layout(
                device, &layout_desc, &fixture.layout, &error),
            error, "create Vulkan binding layout");
    vita_gpu_binding_set_desc set_desc =
        VITA_GPU_INIT(vita_gpu_binding_set_desc);
    set_desc.label = "sampled-set";
    set_desc.layout = fixture.layout;
    require(vita_gpu_create_binding_set(
                device, &set_desc, &fixture.set, &error),
            error, "create Vulkan binding set");
    std::array<vita_gpu_binding_write, 2> writes{
        vita_gpu_binding_write VITA_GPU_INIT(vita_gpu_binding_write),
        vita_gpu_binding_write VITA_GPU_INIT(vita_gpu_binding_write)};
    writes[0].binding = 0;
    writes[0].type = VITA_GPU_BINDING_SAMPLED_TEXTURE;
    writes[0].texture = fixture.texture;
    writes[0].texture_state = VITA_GPU_RESOURCE_STATE_SHADER_READ;
    writes[1].binding = 1;
    writes[1].type = VITA_GPU_BINDING_SAMPLER;
    writes[1].sampler = fixture.sampler;
    require(vita_gpu_update_binding_set(
                device, fixture.set, writes.data(),
                static_cast<std::uint32_t>(writes.size()), &error),
            error, "update Vulkan binding set");

    vita_gpu_texture_desc depth_desc =
        VITA_GPU_INIT(vita_gpu_texture_desc);
    depth_desc.label = "depth-texture";
    depth_desc.dimension = VITA_GPU_TEXTURE_2D;
    depth_desc.width = 800;
    depth_desc.height = 450;
    depth_desc.depth_or_layers = 1;
    depth_desc.mip_levels = 1;
    depth_desc.sample_count = 1;
    depth_desc.format = VITA_GPU_FORMAT_D32_FLOAT;
    depth_desc.usage = VITA_GPU_TEXTURE_DEPTH_TARGET;
    depth_desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    depth_desc.initial_state = VITA_GPU_RESOURCE_STATE_DEPTH_WRITE;
    require(vita_gpu_create_texture(
                device, &depth_desc, &fixture.depth_texture, &error),
            error, "create Vulkan depth texture");
    vita_gpu_depth_target_desc target_desc =
        VITA_GPU_INIT(vita_gpu_depth_target_desc);
    target_desc.label = "depth-target";
    target_desc.texture = fixture.depth_texture;
    target_desc.format = VITA_GPU_FORMAT_D32_FLOAT;
    target_desc.array_layer_count = 1;
    require(vita_gpu_create_depth_target(
                device, &target_desc, &fixture.depth_target, &error),
            error, "create Vulkan depth target");
    return fixture;
}

void destroy_sampled_fixture(vita_gpu_device device,
                             const SampledFixture &fixture) {
    vita_gpu_error error = make_error();
    require(vita_gpu_destroy_depth_target(
                device, fixture.depth_target, &error),
            error, "destroy Vulkan depth target");
    require(vita_gpu_destroy_texture(
                device, fixture.depth_texture, &error),
            error, "destroy Vulkan depth texture");
    require(vita_gpu_destroy_binding_set(
                device, fixture.set, &error),
            error, "destroy Vulkan binding set");
    require(vita_gpu_destroy_binding_layout(
                device, fixture.layout, &error),
            error, "destroy Vulkan binding layout");
    require(vita_gpu_destroy_sampler(
                device, fixture.sampler, &error),
            error, "destroy Vulkan sampler");
    require(vita_gpu_destroy_texture(
                device, fixture.texture, &error),
            error, "destroy sampled Vulkan texture");
}

vita_gpu_pipeline create_pipeline(
    vita_gpu_device device, vita_gpu_shader_module vertex,
    vita_gpu_shader_module pixel, vita_gpu_binding_layout layout) {
    vita_gpu_graphics_pipeline_desc descriptor =
        VITA_GPU_INIT(vita_gpu_graphics_pipeline_desc);
    descriptor.label = "conformance-triangle-pipeline";
    descriptor.vertex_shader = vertex;
    descriptor.pixel_shader = pixel;
    descriptor.binding_layouts = &layout;
    descriptor.binding_layout_count = 1;
    descriptor.topology = VITA_GPU_TOPOLOGY_TRIANGLE_LIST;
    descriptor.rasterizer =
        vita_gpu_rasterizer_desc VITA_GPU_INIT(vita_gpu_rasterizer_desc);
    descriptor.rasterizer.polygon_mode = VITA_GPU_POLYGON_FILL;
    descriptor.rasterizer.cull_mode = VITA_GPU_CULL_NONE;
    descriptor.rasterizer.front_face =
        VITA_GPU_FRONT_COUNTER_CLOCKWISE;
    descriptor.depth_stencil =
        vita_gpu_depth_stencil_desc
            VITA_GPU_INIT(vita_gpu_depth_stencil_desc);
    descriptor.depth_stencil.depth_test_enable = VITA_GPU_TRUE;
    descriptor.depth_stencil.depth_write_enable = VITA_GPU_TRUE;
    descriptor.depth_stencil.depth_compare = VITA_GPU_COMPARE_ALWAYS;
    descriptor.depth_format = VITA_GPU_FORMAT_D32_FLOAT;
    descriptor.color_target_count = 1;
    descriptor.color_formats[0] = VITA_GPU_FORMAT_BGRA8_UNORM;
    descriptor.blend[0] =
        vita_gpu_blend_attachment_desc
            VITA_GPU_INIT(vita_gpu_blend_attachment_desc);
    descriptor.blend[0].source_color = VITA_GPU_BLEND_ONE;
    descriptor.blend[0].source_alpha = VITA_GPU_BLEND_ONE;
    descriptor.blend[0].color_write_mask = 0x0f;
    descriptor.sample_count = 1;
    vita_gpu_pipeline pipeline{};
    vita_gpu_error error = make_error();
    require(vita_gpu_create_graphics_pipeline(
                device, &descriptor, &pipeline, &error),
            error, "create Vulkan graphics pipeline");
    return pipeline;
}

void record_barrier(vita_gpu_device device, vita_gpu_command_list command,
                    vita_gpu_texture texture,
                    vita_gpu_resource_state before,
                    vita_gpu_resource_state after) {
    vita_gpu_resource_barrier barrier =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    barrier.kind = VITA_GPU_RESOURCE_TEXTURE;
    barrier.texture = texture;
    barrier.before = before;
    barrier.after = after;
    barrier.mip_count = 1;
    barrier.array_layer_count = 1;
    barrier.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    barrier.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    vita_gpu_error error = make_error();
    require(vita_gpu_cmd_barriers(
                device, command, &barrier, 1, &error),
            error, "record Vulkan image barrier");
}

void render_frame(
    vita_gpu_device device, vita_gpu_swapchain swapchain,
    vita_gpu_pipeline pipeline, vita_gpu_binding_set binding_set,
    vita_gpu_depth_target depth_target,
    vita_gpu_fence fence, std::uint64_t &fence_value,
    std::uint32_t width, std::uint32_t height, bool capture) {
    vita_gpu_swapchain_image image =
        VITA_GPU_INIT(vita_gpu_swapchain_image);
    vita_gpu_error error = make_error();
    require(vita_gpu_swapchain_acquire(
                device, swapchain, &image, &error),
            error, "acquire Vulkan swapchain image");
    const vita_gpu_command_list command = create_command(device);
    require(vita_gpu_command_begin(device, command, &error),
            error, "begin render command");
    record_barrier(
        device, command, image.texture,
        VITA_GPU_RESOURCE_STATE_PRESENT,
        VITA_GPU_RESOURCE_STATE_RENDER_TARGET);
    vita_gpu_color_attachment color =
        VITA_GPU_INIT(vita_gpu_color_attachment);
    color.target = image.render_target;
    color.load_op = VITA_GPU_LOAD_CLEAR;
    color.store_op = VITA_GPU_STORE_PRESERVE;
    color.clear_color[0] = 0.125F;
    color.clear_color[1] = 0.25F;
    color.clear_color[2] = 0.5F;
    color.clear_color[3] = 1.0F;
    vita_gpu_render_pass_desc pass =
        VITA_GPU_INIT(vita_gpu_render_pass_desc);
    pass.color_attachments = &color;
    pass.color_attachment_count = 1;
    vita_gpu_depth_attachment depth =
        VITA_GPU_INIT(vita_gpu_depth_attachment);
    depth.target = depth_target;
    depth.depth_load_op = VITA_GPU_LOAD_CLEAR;
    depth.depth_store_op = VITA_GPU_STORE_DISCARD;
    depth.clear_depth = 1.0F;
    pass.depth_attachment = &depth;
    require(vita_gpu_cmd_begin_render_pass(
                device, command, &pass, &error),
            error, "begin Vulkan dynamic rendering");
    require(vita_gpu_cmd_bind_pipeline(
                device, command, pipeline, &error),
            error, "bind Vulkan pipeline");
    require(vita_gpu_cmd_bind_set(
                device, command, 0, binding_set, &error),
            error, "bind Vulkan descriptor set");
    vita_gpu_viewport viewport = VITA_GPU_INIT(vita_gpu_viewport);
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.max_depth = 1.0F;
    require(vita_gpu_cmd_set_viewport(
                device, command, &viewport, &error),
            error, "set Vulkan viewport");
    vita_gpu_scissor scissor = VITA_GPU_INIT(vita_gpu_scissor);
    scissor.width = width;
    scissor.height = height;
    require(vita_gpu_cmd_set_scissor(
                device, command, &scissor, &error),
            error, "set Vulkan scissor");
    vita_gpu_draw_desc draw = VITA_GPU_INIT(vita_gpu_draw_desc);
    draw.vertex_count = 3;
    draw.instance_count = 1;
    require(vita_gpu_cmd_draw(
                device, command, &draw, &error),
            error, "record Vulkan triangle");
    require(vita_gpu_cmd_end_render_pass(device, command, &error),
            error, "end Vulkan dynamic rendering");

    vita_gpu_readback readback{};
    if (capture) {
        record_barrier(
            device, command, image.texture,
            VITA_GPU_RESOURCE_STATE_RENDER_TARGET,
            VITA_GPU_RESOURCE_STATE_COPY_SOURCE);
        vita_gpu_readback_desc readback_desc =
            VITA_GPU_INIT(vita_gpu_readback_desc);
        readback_desc.label = "swapchain-readback";
        readback_desc.byte_size =
            static_cast<std::uint64_t>(width) * height * 4U;
        require(vita_gpu_create_readback(
                    device, &readback_desc, &readback, &error),
                error, "create swapchain readback");
        vita_gpu_texture_readback_copy_desc copy =
            VITA_GPU_INIT(vita_gpu_texture_readback_copy_desc);
        copy.source = image.texture;
        copy.destination = readback;
        copy.row_pitch = width * 4U;
        copy.slice_pitch = copy.row_pitch * height;
        require(vita_gpu_cmd_copy_texture_to_readback(
                    device, command, &copy, &error),
                error, "record swapchain readback");
        record_barrier(
            device, command, image.texture,
            VITA_GPU_RESOURCE_STATE_COPY_SOURCE,
            VITA_GPU_RESOURCE_STATE_PRESENT);
    } else {
        record_barrier(
            device, command, image.texture,
            VITA_GPU_RESOURCE_STATE_RENDER_TARGET,
            VITA_GPU_RESOURCE_STATE_PRESENT);
    }
    require(vita_gpu_command_end(device, command, &error),
            error, "end render command");
    submit(device, command, fence, ++fence_value);
    wait_and_reset(device, command, fence, fence_value);
    if (capture) {
        vita_gpu_readback_map mapping =
            VITA_GPU_INIT(vita_gpu_readback_map);
        require(vita_gpu_map_readback(
                    device, readback, &mapping, &error),
                error, "map swapchain readback");
        const std::array<int, 4> expected{128, 64, 32, 255};
        for (std::size_t channel = 0; channel < expected.size();
             ++channel) {
            const int actual = mapping.data[channel];
            if (std::abs(actual - expected[channel]) > 2) {
                throw Failure(
                    "deterministic Vulkan clear readback mismatch");
            }
        }
        require(vita_gpu_unmap_readback(device, readback, &error),
                error, "unmap swapchain readback");
    }
    vita_gpu_present_desc present =
        VITA_GPU_INIT(vita_gpu_present_desc);
    present.image_index = image.image_index;
    require(vita_gpu_swapchain_present(
                device, swapchain, &present, &error),
            error, "present Vulkan swapchain");
    require(vita_gpu_destroy_command_list(device, command, &error),
            error, "destroy render command");
    if (capture) {
        require(vita_gpu_destroy_readback(device, readback, &error),
                error, "destroy swapchain readback");
    }
    pump_messages();
}

void run_cycle(
    HINSTANCE instance, const std::vector<std::uint8_t> &vertex_data,
    const std::vector<std::uint8_t> &pixel_data,
    std::uint32_t adapter_index, bool validation, int cycle) {
    constexpr std::uint32_t kInitialWidth = 640;
    constexpr std::uint32_t kInitialHeight = 360;
    HWND window = create_window(
        instance, kInitialWidth, kInitialHeight);
    vita_gpu_device_desc device_desc =
        VITA_GPU_INIT(vita_gpu_device_desc);
    device_desc.backend = VITA_GPU_BACKEND_VULKAN;
    device_desc.adapter_index = adapter_index;
    device_desc.flags =
        validation ? VITA_GPU_DEVICE_ENABLE_VALIDATION : 0;
    device_desc.application_name = "vitarecomp-vulkan-conformance";
    vita_gpu_device device{};
    vita_gpu_error error = make_error();
    require(vita_gpu_create_device(
                &device_desc, &device, &error),
            error, "create Vulkan device");

    vita_gpu_device_capabilities capabilities =
        VITA_GPU_INIT(vita_gpu_device_capabilities);
    require(vita_gpu_device_get_capabilities(
                device, &capabilities, &error),
            error, "query Vulkan capabilities");
    if ((capabilities.flags & VITA_GPU_CAPABILITY_GRAPHICS) == 0 ||
        (capabilities.flags &
         VITA_GPU_CAPABILITY_TIMELINE_FENCES) == 0) {
        throw Failure("Vulkan backend omitted required capabilities");
    }
    vita_gpu_swapchain_desc swapchain_desc =
        VITA_GPU_INIT(vita_gpu_swapchain_desc);
    swapchain_desc.label = "vulkan-conformance-swapchain";
    swapchain_desc.window =
        vita_gpu_window_handle VITA_GPU_INIT(vita_gpu_window_handle);
    swapchain_desc.window.system = VITA_GPU_WINDOW_WIN32;
    swapchain_desc.window.native_window = window;
    swapchain_desc.width = kInitialWidth;
    swapchain_desc.height = kInitialHeight;
    swapchain_desc.format = VITA_GPU_FORMAT_BGRA8_UNORM;
    swapchain_desc.image_count = 3;
    swapchain_desc.present_mode = VITA_GPU_PRESENT_FIFO;
    vita_gpu_swapchain swapchain{};
    require(vita_gpu_create_swapchain(
                device, &swapchain_desc, &swapchain, &error),
            error, "create Vulkan swapchain");
    const vita_gpu_shader_module vertex =
        create_shader(device, VITA_GPU_SHADER_VERTEX, vertex_data);
    const vita_gpu_shader_module pixel =
        create_shader(device, VITA_GPU_SHADER_PIXEL, pixel_data);
    const SampledFixture fixture = create_sampled_fixture(device);
    const vita_gpu_pipeline pipeline =
        create_pipeline(device, vertex, pixel, fixture.layout);
    vita_gpu_fence_desc fence_desc =
        VITA_GPU_INIT(vita_gpu_fence_desc);
    fence_desc.label = "conformance-timeline";
    vita_gpu_fence fence{};
    require(vita_gpu_create_fence(
                device, &fence_desc, &fence, &error),
            error, "create Vulkan timeline fence");
    std::uint64_t fence_value = 0;
    exercise_resources(device, fence, fence_value);

    for (int frame = 0; frame < 3; ++frame) {
        render_frame(
            device, swapchain, pipeline, fixture.set,
            fixture.depth_target, fence,
            fence_value, kInitialWidth, kInitialHeight, frame == 2);
    }
    vita_gpu_swapchain_image unchanged =
        VITA_GPU_INIT(vita_gpu_swapchain_image);
    require(vita_gpu_swapchain_acquire(
                device, swapchain, &unchanged, &error),
            error, "acquire unchanged Vulkan image");
    vita_gpu_present_desc unchanged_present =
        VITA_GPU_INIT(vita_gpu_present_desc);
    unchanged_present.image_index = unchanged.image_index;
    require(vita_gpu_swapchain_present(
                device, swapchain, &unchanged_present, &error),
            error, "present unchanged Vulkan image");

    constexpr std::uint32_t kImplicitWidth = 704;
    constexpr std::uint32_t kImplicitHeight = 396;
    resize_window(window, kImplicitWidth, kImplicitHeight);
    render_frame(
        device, swapchain, pipeline, fixture.set,
        fixture.depth_target, fence, fence_value,
        kImplicitWidth, kImplicitHeight, false);

    ShowWindow(window, SW_MINIMIZE);
    pump_messages();
    vita_gpu_swapchain_image minimized =
        VITA_GPU_INIT(vita_gpu_swapchain_image);
    const vita_gpu_result minimized_result =
        vita_gpu_swapchain_acquire(
            device, swapchain, &minimized, &error);
    if (minimized_result != VITA_GPU_ERROR_INVALID_STATE) {
        throw Failure(
            "minimized Vulkan acquire did not report INVALID_STATE");
    }
    ShowWindow(window, SW_RESTORE);
    constexpr std::uint32_t kResizedWidth = 800;
    constexpr std::uint32_t kResizedHeight = 450;
    resize_window(window, kResizedWidth, kResizedHeight);
    require(vita_gpu_swapchain_resize(
                device, swapchain, kResizedWidth, kResizedHeight,
                &error),
            error, "resize Vulkan swapchain");
    for (int frame = 0; frame < 3; ++frame) {
        render_frame(
            device, swapchain, pipeline, fixture.set,
            fixture.depth_target, fence,
            fence_value, kResizedWidth, kResizedHeight, frame == 2);
    }

    vita_gpu_debug_capture_desc capture =
        VITA_GPU_INIT(vita_gpu_debug_capture_desc);
    capture.output_path = "unsupported";
    capture.flags = VITA_GPU_DEBUG_CAPTURE_FRAME;
    const vita_gpu_result capture_result =
        vita_gpu_debug_capture_begin(device, &capture, &error);
    if (capture_result != VITA_GPU_ERROR_UNSUPPORTED) {
        throw Failure(
            "unsupported Vulkan debug capture did not report unsupported");
    }

    require(vita_gpu_device_wait_idle(device, &error),
            error, "wait Vulkan device idle");
    require(vita_gpu_destroy_fence(device, fence, &error),
            error, "destroy Vulkan timeline fence");
    require(vita_gpu_destroy_pipeline(device, pipeline, &error),
            error, "destroy Vulkan pipeline");
    destroy_sampled_fixture(device, fixture);
    require(vita_gpu_destroy_shader_module(device, pixel, &error),
            error, "destroy Vulkan pixel shader");
    require(vita_gpu_destroy_shader_module(device, vertex, &error),
            error, "destroy Vulkan vertex shader");
    require(vita_gpu_destroy_swapchain(device, swapchain, &error),
            error, "destroy Vulkan swapchain");
    require(vita_gpu_destroy_device(device, &error),
            error, "destroy Vulkan device");
    DestroyWindow(window);
    pump_messages();
    std::cout << "cycle " << cycle << " passed\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            throw Failure(
                "usage: test_vulkan_conformance <vertex.spv> <pixel.spv>");
        }
        vita_gpu_backend selected = VITA_GPU_BACKEND_NONE;
        vita_gpu_error error = make_error();
        require(vita_gpu_backend_from_name(
                    "vulkan", &selected, &error),
                error, "select Vulkan backend");
        if (selected != VITA_GPU_BACKEND_VULKAN ||
            std::string(vita_gpu_backend_name(selected)) != "vulkan") {
            throw Failure("Vulkan backend registration is incorrect");
        }
        vita_gpu_backend_info backend =
            VITA_GPU_INIT(vita_gpu_backend_info);
        require(vita_gpu_get_backend_info(1, &backend, &error),
                error, "query Vulkan backend");
        if (backend.available != VITA_GPU_TRUE) {
            std::cout << "SKIP: "
                      << (backend.unavailable_reason[0] == '\0'
                              ? "Vulkan loader/ICD unavailable"
                              : backend.unavailable_reason)
                      << '\n';
            return 77;
        }
        std::uint32_t adapter_count = 0;
        require(vita_gpu_adapter_count(
                    VITA_GPU_BACKEND_VULKAN, &adapter_count, &error),
                error, "enumerate Vulkan adapters");
        if (adapter_count == 0) {
            std::cout << "SKIP: Vulkan loader found no compatible ICD\n";
            return 77;
        }
        std::uint32_t selected_adapter = 0;
        bool selected_discrete = false;
        for (std::uint32_t index = 0; index < adapter_count; ++index) {
            vita_gpu_adapter_info adapter =
                VITA_GPU_INIT(vita_gpu_adapter_info);
            require(vita_gpu_get_adapter_info(
                        VITA_GPU_BACKEND_VULKAN, index, &adapter,
                        &error),
                    error, "query Vulkan adapter");
            std::cout << "adapter " << index << ": " << adapter.name
                      << " vendor=0x" << std::hex << adapter.vendor_id
                      << " device=0x" << adapter.device_id << std::dec
                      << '\n';
            if (!selected_discrete &&
                adapter.type == VITA_GPU_ADAPTER_DISCRETE) {
                selected_adapter = index;
                selected_discrete = true;
            }
        }
        const auto vertex = read_binary(argv[1]);
        const auto pixel = read_binary(argv[2]);
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = L"VitarecompVulkanConformance";
        if (RegisterClassW(&window_class) == 0) {
            throw Failure("RegisterClassW failed");
        }
        const bool validation =
            backend.validation_available == VITA_GPU_TRUE;
        std::cout << "validation=" << (validation ? "enabled" : "unavailable")
                  << '\n';
        run_cycle(
            instance, vertex, pixel, selected_adapter, validation, 1);
        run_cycle(
            instance, vertex, pixel, selected_adapter, validation, 2);
        UnregisterClassW(window_class.lpszClassName, instance);
        std::cout << "Vulkan conformance passed\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "Vulkan conformance failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
