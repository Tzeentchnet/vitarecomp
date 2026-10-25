#include "vitarecomp/gpu.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"VitarecompDx12ConformanceWindow";
constexpr std::array<float, 4> kClearColor{
    0.0625F, 0.125F, 0.25F, 1.0F};

struct Options {
    bool warp = false;
    bool hidden = false;
    std::uint32_t iterations = 1;
    std::uint32_t frames = 4;
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool parse_u32(const char *text, std::uint32_t &value) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (text == end || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_options(int argc, char **argv, Options &options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--warp") {
            options.warp = true;
        } else if (argument == "--hidden") {
            options.hidden = true;
        } else if (argument == "--iterations" && index + 1 < argc) {
            if (!parse_u32(argv[++index], options.iterations)) {
                return false;
            }
        } else if (argument == "--frames" && index + 1 < argc) {
            if (!parse_u32(argv[++index], options.frames)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool gpu_call(vita_gpu_result result, const vita_gpu_error &error,
              std::string_view operation) {
    if (result == VITA_GPU_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed: " << vita_gpu_result_string(result)
              << " native=" << error.native_code << " function="
              << error.function_name << " message=" << error.message << '\n';
    return false;
}

#define GPU_CALL(expression, operation)                                      \
    do {                                                                     \
        if (!gpu_call((expression), error, (operation))) {                   \
            return false;                                                    \
        }                                                                    \
    } while (false)

std::vector<std::uint8_t> read_binary(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamoff length = stream.tellg();
    if (length <= 0 ||
        static_cast<std::uint64_t>(length) >
            std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char *>(bytes.data()), length);
    if (!stream) {
        return {};
    }
    return bytes;
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND create_window(std::uint32_t width, std::uint32_t height, bool hidden) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor =
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    RECT rectangle{
        0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    (void)AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(
        0, kWindowClass, L"vitarecomp DX12 conformance",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top, nullptr, nullptr, instance,
        nullptr);
    if (window != nullptr) {
        ShowWindow(window, hidden ? SW_HIDE : SW_SHOW);
        UpdateWindow(window);
        pump_messages();
    }
    return window;
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool select_adapter(bool warp, std::uint32_t &selected,
                    vita_gpu_adapter_info &selected_info) {
    vita_gpu_error error = VITA_GPU_INIT(vita_gpu_error);
    std::uint32_t count = 0;
    if (!gpu_call(vita_gpu_adapter_count(
                      VITA_GPU_BACKEND_DX12, &count, &error),
                  error, "enumerate DX12 adapters")) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        vita_gpu_adapter_info info =
            VITA_GPU_INIT(vita_gpu_adapter_info);
        if (!gpu_call(vita_gpu_get_adapter_info(
                          VITA_GPU_BACKEND_DX12, index, &info, &error),
                      error, "get DX12 adapter info")) {
            return false;
        }
        const bool software = info.type == VITA_GPU_ADAPTER_SOFTWARE;
        if (software == warp) {
            selected = index;
            selected_info = info;
            return true;
        }
    }
    std::cerr << (warp ? "WARP" : "hardware")
              << " DX12 adapter was not found"
              << (warp ? "" : "; pass --warp for explicit software mode")
              << '\n';
    return false;
}

struct RenderContext {
    vita_gpu_device device{};
    vita_gpu_swapchain swapchain{};
    vita_gpu_shader_module vertex_shader{};
    vita_gpu_shader_module pixel_shader{};
    vita_gpu_pipeline pipeline{};
    vita_gpu_command_list command{};
    vita_gpu_fence fence{};
    vita_gpu_readback readback{};
    std::uint64_t fence_value = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

bool render_frame(RenderContext &context, bool capture,
                  std::uint64_t &hash) {
    vita_gpu_error error = VITA_GPU_INIT(vita_gpu_error);
    vita_gpu_swapchain_image image =
        VITA_GPU_INIT(vita_gpu_swapchain_image);
    GPU_CALL(vita_gpu_swapchain_acquire(
                 context.device, context.swapchain, &image, &error),
             "acquire swapchain image");
    GPU_CALL(vita_gpu_command_begin(
                 context.device, context.command, &error),
             "begin graphics command list");

    vita_gpu_resource_barrier to_render =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    to_render.kind = VITA_GPU_RESOURCE_TEXTURE;
    to_render.texture = image.texture;
    to_render.before = VITA_GPU_RESOURCE_STATE_PRESENT;
    to_render.after = VITA_GPU_RESOURCE_STATE_RENDER_TARGET;
    to_render.mip_count = 1;
    to_render.array_layer_count = 1;
    to_render.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    to_render.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    GPU_CALL(vita_gpu_cmd_barriers(
                 context.device, context.command, &to_render, 1, &error),
             "transition back buffer to render target");

    vita_gpu_color_attachment color =
        VITA_GPU_INIT(vita_gpu_color_attachment);
    color.target = image.render_target;
    color.load_op = VITA_GPU_LOAD_CLEAR;
    color.store_op = VITA_GPU_STORE_PRESERVE;
    std::copy(kClearColor.begin(), kClearColor.end(), color.clear_color);
    vita_gpu_render_pass_desc render_pass =
        VITA_GPU_INIT(vita_gpu_render_pass_desc);
    render_pass.color_attachments = &color;
    render_pass.color_attachment_count = 1;
    GPU_CALL(vita_gpu_cmd_begin_render_pass(
                 context.device, context.command, &render_pass, &error),
             "begin render pass");
    GPU_CALL(vita_gpu_cmd_bind_pipeline(
                 context.device, context.command, context.pipeline, &error),
             "bind graphics pipeline");

    vita_gpu_viewport viewport = VITA_GPU_INIT(vita_gpu_viewport);
    viewport.width = static_cast<float>(context.width);
    viewport.height = static_cast<float>(context.height);
    viewport.max_depth = 1.0F;
    GPU_CALL(vita_gpu_cmd_set_viewport(
                 context.device, context.command, &viewport, &error),
             "set viewport");
    vita_gpu_scissor scissor = VITA_GPU_INIT(vita_gpu_scissor);
    scissor.width = context.width;
    scissor.height = context.height;
    GPU_CALL(vita_gpu_cmd_set_scissor(
                 context.device, context.command, &scissor, &error),
             "set scissor");
    vita_gpu_draw_desc draw = VITA_GPU_INIT(vita_gpu_draw_desc);
    draw.vertex_count = 3;
    draw.instance_count = 1;
    GPU_CALL(vita_gpu_cmd_draw(
                 context.device, context.command, &draw, &error),
             "draw deterministic triangle");
    GPU_CALL(vita_gpu_cmd_end_render_pass(
                 context.device, context.command, &error),
             "end render pass");

    if (capture) {
        vita_gpu_resource_barrier to_copy =
            VITA_GPU_INIT(vita_gpu_resource_barrier);
        to_copy.kind = VITA_GPU_RESOURCE_TEXTURE;
        to_copy.texture = image.texture;
        to_copy.before = VITA_GPU_RESOURCE_STATE_RENDER_TARGET;
        to_copy.after = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
        to_copy.mip_count = 1;
        to_copy.array_layer_count = 1;
        to_copy.source_queue = VITA_GPU_QUEUE_GRAPHICS;
        to_copy.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
        GPU_CALL(vita_gpu_cmd_barriers(
                     context.device, context.command, &to_copy, 1, &error),
                 "transition back buffer to copy source");

        const std::uint32_t row_pitch =
            align_up(context.width * 4U, 256U);
        vita_gpu_texture_readback_copy_desc copy =
            VITA_GPU_INIT(vita_gpu_texture_readback_copy_desc);
        copy.source = image.texture;
        copy.destination = context.readback;
        copy.row_pitch = row_pitch;
        copy.slice_pitch = row_pitch * context.height;
        GPU_CALL(vita_gpu_cmd_copy_texture_to_readback(
                     context.device, context.command, &copy, &error),
                 "copy back buffer to readback");

        vita_gpu_resource_barrier to_present =
            VITA_GPU_INIT(vita_gpu_resource_barrier);
        to_present.kind = VITA_GPU_RESOURCE_TEXTURE;
        to_present.texture = image.texture;
        to_present.before = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
        to_present.after = VITA_GPU_RESOURCE_STATE_PRESENT;
        to_present.mip_count = 1;
        to_present.array_layer_count = 1;
        to_present.source_queue = VITA_GPU_QUEUE_GRAPHICS;
        to_present.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
        GPU_CALL(vita_gpu_cmd_barriers(
                     context.device, context.command, &to_present, 1, &error),
                 "transition captured back buffer to present");
    } else {
        vita_gpu_resource_barrier to_present =
            VITA_GPU_INIT(vita_gpu_resource_barrier);
        to_present.kind = VITA_GPU_RESOURCE_TEXTURE;
        to_present.texture = image.texture;
        to_present.before = VITA_GPU_RESOURCE_STATE_RENDER_TARGET;
        to_present.after = VITA_GPU_RESOURCE_STATE_PRESENT;
        to_present.mip_count = 1;
        to_present.array_layer_count = 1;
        to_present.source_queue = VITA_GPU_QUEUE_GRAPHICS;
        to_present.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
        GPU_CALL(vita_gpu_cmd_barriers(
                     context.device, context.command, &to_present, 1, &error),
                 "transition back buffer to present");
    }

    GPU_CALL(vita_gpu_command_end(
                 context.device, context.command, &error),
             "end graphics command list");
    const std::array<vita_gpu_command_list, 1> commands{context.command};
    vita_gpu_submit_desc submit = VITA_GPU_INIT(vita_gpu_submit_desc);
    submit.command_lists = commands.data();
    submit.command_list_count = 1;
    submit.signal = vita_gpu_fence_point VITA_GPU_INIT(vita_gpu_fence_point);
    submit.signal.fence = context.fence;
    submit.signal.value = ++context.fence_value;
    GPU_CALL(vita_gpu_submit(
                 context.device, VITA_GPU_QUEUE_GRAPHICS, &submit, &error),
             "submit graphics work");
    GPU_CALL(vita_gpu_fence_wait(
                 context.device, context.fence, context.fence_value,
                 std::numeric_limits<std::uint64_t>::max(), &error),
             "wait for graphics work");
    GPU_CALL(vita_gpu_command_reset(
                 context.device, context.command, &error),
             "reset completed graphics command list");

    vita_gpu_present_desc present = VITA_GPU_INIT(vita_gpu_present_desc);
    present.image_index = image.image_index;
    GPU_CALL(vita_gpu_swapchain_present(
                 context.device, context.swapchain, &present, &error),
             "present swapchain image");

    if (capture) {
        vita_gpu_readback_map map = VITA_GPU_INIT(vita_gpu_readback_map);
        GPU_CALL(vita_gpu_map_readback(
                     context.device, context.readback, &map, &error),
                 "map captured back buffer");
        const std::uint32_t row_pitch =
            align_up(context.width * 4U, 256U);
        if (map.data == nullptr || map.row_pitch != row_pitch ||
            map.slice_pitch != row_pitch * context.height) {
            std::cerr << "captured back-buffer pitches are invalid\n";
            return false;
        }
        const std::uint8_t *corner = map.data;
        constexpr std::array<std::uint8_t, 4> expected{
            64, 32, 16, 255};
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            const int difference =
                static_cast<int>(corner[channel]) -
                static_cast<int>(expected[channel]);
            if (difference < -1 || difference > 1) {
                std::cerr << "deterministic clear pixel mismatch at channel "
                          << channel << ": got "
                          << static_cast<unsigned>(corner[channel])
                          << " expected "
                          << static_cast<unsigned>(expected[channel])
                          << '\n';
                return false;
            }
        }
        const std::uint8_t *center =
            map.data +
            static_cast<std::size_t>(context.height / 2U) * row_pitch +
            static_cast<std::size_t>(context.width / 2U) * 4U;
        if (std::memcmp(center, expected.data(), expected.size()) == 0) {
            std::cerr << "triangle did not modify the deterministic center "
                         "pixel\n";
            return false;
        }
        hash = UINT64_C(1469598103934665603);
        for (std::uint32_t y = 0; y < context.height; ++y) {
            const std::uint8_t *row =
                map.data + static_cast<std::size_t>(y) * row_pitch;
            for (std::uint32_t x = 0; x < context.width * 4U; ++x) {
                hash ^= row[x];
                hash *= UINT64_C(1099511628211);
            }
        }
        GPU_CALL(vita_gpu_unmap_readback(
                     context.device, context.readback, &error),
                 "unmap captured back buffer");
    }
    pump_messages();
    return true;
}

bool run_iteration(std::uint32_t adapter_index, bool hidden,
                   bool validation_available,
                   const std::vector<std::uint8_t> &vertex_shader,
                   const std::vector<std::uint8_t> &pixel_shader,
                   std::uint32_t frames, std::uint32_t iteration,
                   std::uint64_t &hash) {
    vita_gpu_error error = VITA_GPU_INIT(vita_gpu_error);
    constexpr std::uint32_t initial_width = 160;
    constexpr std::uint32_t initial_height = 120;
    constexpr std::uint32_t resized_width = 192;
    constexpr std::uint32_t resized_height = 128;
    HWND window = create_window(initial_width, initial_height, hidden);
    if (window == nullptr) {
        std::cerr << "CreateWindowExW failed: " << GetLastError() << '\n';
        return false;
    }

    RenderContext context;
    context.width = initial_width;
    context.height = initial_height;
    vita_gpu_device_desc device_desc =
        VITA_GPU_INIT(vita_gpu_device_desc);
    device_desc.backend = VITA_GPU_BACKEND_DX12;
    device_desc.adapter_index = adapter_index;
    device_desc.flags = VITA_GPU_DEVICE_ENABLE_DEBUG_NAMES;
    if (validation_available) {
        device_desc.flags |= VITA_GPU_DEVICE_ENABLE_VALIDATION;
    }
    device_desc.application_name = "vitarecomp-dx12-conformance";
    GPU_CALL(vita_gpu_create_device(
                 &device_desc, &context.device, &error),
             "create DX12 conformance device");

    vita_gpu_swapchain_desc swapchain_desc =
        VITA_GPU_INIT(vita_gpu_swapchain_desc);
    swapchain_desc.label = "conformance-swapchain";
    swapchain_desc.window =
        vita_gpu_window_handle VITA_GPU_INIT(vita_gpu_window_handle);
    swapchain_desc.window.system = VITA_GPU_WINDOW_WIN32;
    swapchain_desc.window.native_window = window;
    swapchain_desc.width = context.width;
    swapchain_desc.height = context.height;
    swapchain_desc.format = VITA_GPU_FORMAT_BGRA8_UNORM;
    swapchain_desc.image_count = 3;
    swapchain_desc.present_mode = VITA_GPU_PRESENT_IMMEDIATE;
    GPU_CALL(vita_gpu_create_swapchain(
                 context.device, &swapchain_desc, &context.swapchain, &error),
             "create Win32 swapchain");

    vita_gpu_shader_binary_desc vertex_binary =
        VITA_GPU_INIT(vita_gpu_shader_binary_desc);
    vertex_binary.stage = VITA_GPU_SHADER_VERTEX;
    vertex_binary.target = VITA_GPU_SHADER_TARGET_DXIL;
    vertex_binary.data = vertex_shader.data();
    vertex_binary.data_size = vertex_shader.size();
    vertex_binary.key = vita_gpu_shader_key VITA_GPU_INIT(vita_gpu_shader_key);
    vita_gpu_shader_module_desc vertex_desc =
        VITA_GPU_INIT(vita_gpu_shader_module_desc);
    vertex_desc.label = "conformance-vs";
    vertex_desc.input = VITA_GPU_SHADER_INPUT_BINARY;
    vertex_desc.binary = &vertex_binary;
    GPU_CALL(vita_gpu_create_shader_module(
                 context.device, &vertex_desc, &context.vertex_shader,
                 &error),
             "create vertex shader");

    vita_gpu_shader_binary_desc pixel_binary =
        VITA_GPU_INIT(vita_gpu_shader_binary_desc);
    pixel_binary.stage = VITA_GPU_SHADER_PIXEL;
    pixel_binary.target = VITA_GPU_SHADER_TARGET_DXIL;
    pixel_binary.data = pixel_shader.data();
    pixel_binary.data_size = pixel_shader.size();
    pixel_binary.key = vita_gpu_shader_key VITA_GPU_INIT(vita_gpu_shader_key);
    vita_gpu_shader_module_desc pixel_desc =
        VITA_GPU_INIT(vita_gpu_shader_module_desc);
    pixel_desc.label = "conformance-ps";
    pixel_desc.input = VITA_GPU_SHADER_INPUT_BINARY;
    pixel_desc.binary = &pixel_binary;
    GPU_CALL(vita_gpu_create_shader_module(
                 context.device, &pixel_desc, &context.pixel_shader, &error),
             "create pixel shader");

    vita_gpu_graphics_pipeline_desc pipeline_desc =
        VITA_GPU_INIT(vita_gpu_graphics_pipeline_desc);
    pipeline_desc.label = "conformance-pipeline";
    pipeline_desc.vertex_shader = context.vertex_shader;
    pipeline_desc.pixel_shader = context.pixel_shader;
    pipeline_desc.topology = VITA_GPU_TOPOLOGY_TRIANGLE_LIST;
    pipeline_desc.rasterizer =
        vita_gpu_rasterizer_desc VITA_GPU_INIT(vita_gpu_rasterizer_desc);
    pipeline_desc.rasterizer.polygon_mode = VITA_GPU_POLYGON_FILL;
    pipeline_desc.rasterizer.cull_mode = VITA_GPU_CULL_NONE;
    pipeline_desc.rasterizer.front_face =
        VITA_GPU_FRONT_COUNTER_CLOCKWISE;
    pipeline_desc.depth_stencil =
        vita_gpu_depth_stencil_desc
            VITA_GPU_INIT(vita_gpu_depth_stencil_desc);
    pipeline_desc.depth_stencil.depth_compare = VITA_GPU_COMPARE_ALWAYS;
    pipeline_desc.color_target_count = 1;
    pipeline_desc.color_formats[0] = VITA_GPU_FORMAT_BGRA8_UNORM;
    pipeline_desc.blend[0] =
        vita_gpu_blend_attachment_desc
            VITA_GPU_INIT(vita_gpu_blend_attachment_desc);
    pipeline_desc.blend[0].color_write_mask = 0x0f;
    pipeline_desc.sample_count = 1;
    GPU_CALL(vita_gpu_create_graphics_pipeline(
                 context.device, &pipeline_desc, &context.pipeline, &error),
             "create graphics pipeline");

    vita_gpu_command_list_desc command_desc =
        VITA_GPU_INIT(vita_gpu_command_list_desc);
    command_desc.label = "conformance-command-list";
    command_desc.queue = VITA_GPU_QUEUE_GRAPHICS;
    GPU_CALL(vita_gpu_create_command_list(
                 context.device, &command_desc, &context.command, &error),
             "create graphics command list");
    vita_gpu_fence_desc fence_desc =
        VITA_GPU_INIT(vita_gpu_fence_desc);
    fence_desc.label = "conformance-fence";
    GPU_CALL(vita_gpu_create_fence(
                 context.device, &fence_desc, &context.fence, &error),
             "create graphics fence");

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        if (!render_frame(context, false, hash)) {
            return false;
        }
    }

    ShowWindow(window, SW_MINIMIZE);
    pump_messages();
    Sleep(20);
    ShowWindow(window, SW_RESTORE);
    if (hidden) {
        ShowWindow(window, SW_HIDE);
    }
    SetWindowPos(window, nullptr, 0, 0,
                 static_cast<int>(resized_width),
                 static_cast<int>(resized_height),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump_messages();
    GPU_CALL(vita_gpu_device_wait_idle(context.device, &error),
             "wait before resize");
    GPU_CALL(vita_gpu_swapchain_resize(
                 context.device, context.swapchain, resized_width,
                 resized_height, &error),
             "resize swapchain after minimize/restore");
    context.width = resized_width;
    context.height = resized_height;

    vita_gpu_readback_desc readback_desc =
        VITA_GPU_INIT(vita_gpu_readback_desc);
    readback_desc.label = "conformance-backbuffer-readback";
    const std::uint32_t row_pitch =
        align_up(context.width * 4U, 256U);
    readback_desc.byte_size =
        static_cast<std::uint64_t>(row_pitch) * context.height;
    GPU_CALL(vita_gpu_create_readback(
                 context.device, &readback_desc, &context.readback, &error),
             "create back-buffer readback");
    if (!render_frame(context, true, hash)) {
        return false;
    }

    GPU_CALL(vita_gpu_destroy_command_list(
                 context.device, context.command, &error),
             "destroy graphics command list");
    GPU_CALL(vita_gpu_destroy_readback(
                 context.device, context.readback, &error),
             "destroy back-buffer readback");
    GPU_CALL(vita_gpu_destroy_pipeline(
                 context.device, context.pipeline, &error),
             "destroy graphics pipeline");
    GPU_CALL(vita_gpu_destroy_shader_module(
                 context.device, context.pixel_shader, &error),
             "destroy pixel shader");
    GPU_CALL(vita_gpu_destroy_shader_module(
                 context.device, context.vertex_shader, &error),
             "destroy vertex shader");
    GPU_CALL(vita_gpu_destroy_fence(
                 context.device, context.fence, &error),
             "destroy graphics fence");
    GPU_CALL(vita_gpu_destroy_swapchain(
                 context.device, context.swapchain, &error),
             "destroy swapchain");
    GPU_CALL(vita_gpu_device_wait_idle(context.device, &error),
             "wait before device shutdown");
    GPU_CALL(vita_gpu_destroy_device(context.device, &error),
             "destroy DX12 conformance device");
    DestroyWindow(window);
    pump_messages();
    std::cout << "iteration " << iteration
              << " deterministic FNV-1a=0x" << std::hex << hash
              << std::dec << " passed\n";
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: dx12_conformance [--warp] [--hidden] "
                     "[--iterations N] [--frames N]\n";
        return EXIT_FAILURE;
    }
    vita_gpu_error error = VITA_GPU_INIT(vita_gpu_error);
    vita_gpu_backend backend = VITA_GPU_BACKEND_NONE;
    if (!gpu_call(vita_gpu_backend_from_name(
                      "dx12", &backend, &error),
                  error, "select dx12 backend") ||
        backend != VITA_GPU_BACKEND_DX12) {
        return EXIT_FAILURE;
    }
    vita_gpu_backend_info info = VITA_GPU_INIT(vita_gpu_backend_info);
    if (!gpu_call(vita_gpu_get_backend_info(0, &info, &error),
                  error, "get dx12 backend info") ||
        info.available == VITA_GPU_FALSE) {
        std::cerr << "DX12 backend unavailable: "
                  << info.unavailable_reason << '\n';
        return EXIT_FAILURE;
    }
    if (info.validation_available == VITA_GPU_FALSE) {
        std::cerr << "warning: D3D12 debug layer is unavailable; install "
                     "Windows Graphics Tools to make high-severity info-queue "
                     "messages fail this run\n";
    }

    std::uint32_t adapter_index = 0;
    vita_gpu_adapter_info adapter =
        VITA_GPU_INIT(vita_gpu_adapter_info);
    if (!select_adapter(options.warp, adapter_index, adapter)) {
        return EXIT_FAILURE;
    }
    const std::filesystem::path directory = executable_directory();
    const std::vector<std::uint8_t> vertex_shader =
        read_binary(directory / "dx12_conformance_vs.dxil");
    const std::vector<std::uint8_t> pixel_shader =
        read_binary(directory / "dx12_conformance_ps.dxil");
    if (vertex_shader.empty() || pixel_shader.empty()) {
        std::cerr << "generated conformance DXIL is missing beside "
                     "the executable\n";
        return EXIT_FAILURE;
    }

    std::cout << "DX12 conformance adapter " << adapter_index << ": "
              << adapter.name
              << (options.warp ? " (explicit WARP mode)" :
                                 " (hardware-preferred mode)")
              << '\n';
    std::uint64_t expected_hash = 0;
    for (std::uint32_t iteration = 1;
         iteration <= options.iterations; ++iteration) {
        std::uint64_t hash = 0;
        if (!run_iteration(
                adapter_index, options.hidden,
                info.validation_available != VITA_GPU_FALSE,
                vertex_shader, pixel_shader, options.frames, iteration,
                hash)) {
            return EXIT_FAILURE;
        }
        if (iteration == 1) {
            expected_hash = hash;
        } else if (hash != expected_hash) {
            std::cerr << "readback hash changed across repeated startup/"
                         "resize/shutdown iterations\n";
            return EXIT_FAILURE;
        }
    }
    std::cout << "DX12 conformance passed " << options.iterations
              << " iteration(s)\n";
    return EXIT_SUCCESS;
}
