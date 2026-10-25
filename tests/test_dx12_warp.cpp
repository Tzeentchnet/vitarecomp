#include "vitarecomp/gpu.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

bool check_result(vita_gpu_result result, vita_gpu_error &error,
                  std::string_view operation) {
    if (result == VITA_GPU_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed: " << vita_gpu_result_string(result)
              << " native=" << error.native_code << " function="
              << error.function_name << " message=" << error.message << '\n';
    return false;
}

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "REQUIRE failed at " << __FILE__ << ':'           \
                      << __LINE__ << ": " #condition "\n";                 \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (false)

#define GPU_REQUIRE(expression, operation)                                   \
    REQUIRE(check_result((expression), error, (operation)))

} // namespace

int main() {
    vita_gpu_error error = VITA_GPU_INIT(vita_gpu_error);
    vita_gpu_backend_info backend = VITA_GPU_INIT(vita_gpu_backend_info);
    GPU_REQUIRE(vita_gpu_get_backend_info(0, &backend, &error),
                "get DX12 backend info");
    REQUIRE(backend.backend == VITA_GPU_BACKEND_DX12);
    REQUIRE(std::strcmp(backend.name, "dx12") == 0);
    REQUIRE(backend.available == VITA_GPU_TRUE);

    std::uint32_t adapter_count = 0;
    GPU_REQUIRE(vita_gpu_adapter_count(
                    VITA_GPU_BACKEND_DX12, &adapter_count, &error),
                "enumerate DX12 adapters");
    REQUIRE(adapter_count != 0);

    std::uint32_t warp_index = std::numeric_limits<std::uint32_t>::max();
    bool saw_software = false;
    bool saw_hardware = false;
    for (std::uint32_t index = 0; index < adapter_count; ++index) {
        vita_gpu_adapter_info adapter =
            VITA_GPU_INIT(vita_gpu_adapter_info);
        GPU_REQUIRE(vita_gpu_get_adapter_info(
                        VITA_GPU_BACKEND_DX12, index, &adapter, &error),
                    "get DX12 adapter info");
        if (adapter.type == VITA_GPU_ADAPTER_SOFTWARE) {
            saw_software = true;
            warp_index = index;
            REQUIRE(std::strstr(adapter.name, "WARP") != nullptr);
            REQUIRE(std::strstr(adapter.name, "explicit selection") !=
                    nullptr);
        } else {
            REQUIRE(!saw_software);
            saw_hardware = true;
            REQUIRE((adapter.capabilities &
                     VITA_GPU_CAPABILITY_GRAPHICS) != 0);
        }
    }
    REQUIRE(warp_index != std::numeric_limits<std::uint32_t>::max());
    if (saw_hardware) {
        vita_gpu_adapter_info first =
            VITA_GPU_INIT(vita_gpu_adapter_info);
        GPU_REQUIRE(vita_gpu_get_adapter_info(
                        VITA_GPU_BACKEND_DX12, 0, &first, &error),
                    "get default DX12 adapter");
        REQUIRE(first.type != VITA_GPU_ADAPTER_SOFTWARE);
    }

    vita_gpu_device_desc device_desc =
        VITA_GPU_INIT(vita_gpu_device_desc);
    device_desc.backend = VITA_GPU_BACKEND_DX12;
    device_desc.adapter_index = warp_index;
    device_desc.flags =
        backend.validation_available != 0
            ? VITA_GPU_DEVICE_ENABLE_VALIDATION |
                  VITA_GPU_DEVICE_ENABLE_DEBUG_NAMES
            : VITA_GPU_DEVICE_ENABLE_DEBUG_NAMES;
    device_desc.application_name = "vitarecomp-dx12-warp-test";
    vita_gpu_device device{};
    GPU_REQUIRE(vita_gpu_create_device(&device_desc, &device, &error),
                "create explicit WARP device");

    vita_gpu_device_capabilities capabilities =
        VITA_GPU_INIT(vita_gpu_device_capabilities);
    GPU_REQUIRE(vita_gpu_device_get_capabilities(
                    device, &capabilities, &error),
                "get WARP capabilities");
    REQUIRE((capabilities.flags & VITA_GPU_CAPABILITY_GRAPHICS) != 0);
    REQUIRE((capabilities.flags &
             VITA_GPU_CAPABILITY_TIMELINE_FENCES) != 0);
    REQUIRE((capabilities.flags & VITA_GPU_CAPABILITY_DEBUG_CAPTURE) == 0);

    constexpr char hlsl[] =
        "float4 main(uint id : SV_VertexID) : SV_Position { "
        "return float4(0, 0, 0, 1); }\n";
    vita_gpu_shader_request_desc request =
        VITA_GPU_INIT(vita_gpu_shader_request_desc);
    request.source_name = "runtime-compile-check.hlsl";
    request.source = hlsl;
    request.source_size = sizeof(hlsl) - 1U;
    request.entry_point = "main";
    request.profile = "vs_6_0";
    request.stage = VITA_GPU_SHADER_VERTEX;
    request.target = VITA_GPU_SHADER_TARGET_DXIL;
    vita_gpu_shader_module_desc hlsl_module =
        VITA_GPU_INIT(vita_gpu_shader_module_desc);
    hlsl_module.input = VITA_GPU_SHADER_INPUT_HLSL;
    hlsl_module.hlsl = &request;
    vita_gpu_shader_module unsupported_shader{};
    REQUIRE(vita_gpu_create_shader_module(
                device, &hlsl_module, &unsupported_shader, &error) ==
            VITA_GPU_ERROR_SHADER_COMPILER_UNAVAILABLE);
    REQUIRE(unsupported_shader.value == 0);

    vita_gpu_debug_capture_desc capture =
        VITA_GPU_INIT(vita_gpu_debug_capture_desc);
    capture.flags = VITA_GPU_DEBUG_CAPTURE_FRAME;
    REQUIRE(vita_gpu_debug_capture_begin(device, &capture, &error) ==
            VITA_GPU_ERROR_UNSUPPORTED);

    vita_gpu_buffer_desc unsupported_storage_desc =
        VITA_GPU_INIT(vita_gpu_buffer_desc);
    unsupported_storage_desc.label = "unsupported-upload-storage";
    unsupported_storage_desc.byte_size = 256;
    unsupported_storage_desc.usage = VITA_GPU_BUFFER_STORAGE;
    unsupported_storage_desc.memory_type = VITA_GPU_MEMORY_CPU_UPLOAD;
    unsupported_storage_desc.initial_state =
        VITA_GPU_RESOURCE_STATE_SHADER_READ;
    vita_gpu_buffer unsupported_storage{};
    REQUIRE(vita_gpu_create_buffer(
                device, &unsupported_storage_desc, &unsupported_storage,
                &error) == VITA_GPU_ERROR_UNSUPPORTED);
    REQUIRE(unsupported_storage.value == 0);

    constexpr std::array<std::uint8_t, 64> source_data{
        0x42, 0x11, 0x9a, 0xef, 0x03, 0x71, 0x27, 0xcc,
        0x8d, 0x54, 0xb0, 0x16, 0x6e, 0x39, 0xd5, 0xa2,
        0x5b, 0xf1, 0x0c, 0x84, 0x2a, 0x96, 0x48, 0x73,
        0xbd, 0x07, 0xe4, 0x31, 0x69, 0xca, 0x1f, 0x58,
        0x90, 0x24, 0xde, 0x7a, 0x45, 0xb7, 0x0a, 0xf3,
        0x66, 0x18, 0x81, 0xcd, 0x3c, 0xa8, 0x52, 0x0f,
        0xd9, 0x74, 0x2d, 0x93, 0x4e, 0xe0, 0x15, 0x87,
        0x38, 0xab, 0x60, 0x1c, 0xf6, 0x49, 0x05, 0xbe};

    vita_gpu_buffer_desc upload_desc =
        VITA_GPU_INIT(vita_gpu_buffer_desc);
    upload_desc.label = "warp-upload";
    upload_desc.byte_size = source_data.size();
    upload_desc.usage = VITA_GPU_BUFFER_COPY_SOURCE;
    upload_desc.memory_type = VITA_GPU_MEMORY_CPU_UPLOAD;
    upload_desc.initial_state = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
    upload_desc.initial_data = source_data.data();
    upload_desc.initial_data_size = source_data.size();
    vita_gpu_buffer upload{};
    GPU_REQUIRE(vita_gpu_create_buffer(
                    device, &upload_desc, &upload, &error),
                "create upload buffer");

    vita_gpu_buffer_desc gpu_desc =
        VITA_GPU_INIT(vita_gpu_buffer_desc);
    gpu_desc.label = "warp-default";
    gpu_desc.byte_size = source_data.size();
    gpu_desc.usage =
        VITA_GPU_BUFFER_COPY_SOURCE | VITA_GPU_BUFFER_COPY_DEST;
    gpu_desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    gpu_desc.initial_state = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    vita_gpu_buffer gpu{};
    GPU_REQUIRE(vita_gpu_create_buffer(device, &gpu_desc, &gpu, &error),
                "create default buffer");

    vita_gpu_readback_desc readback_desc =
        VITA_GPU_INIT(vita_gpu_readback_desc);
    readback_desc.label = "warp-readback";
    readback_desc.byte_size = source_data.size();
    vita_gpu_readback readback{};
    GPU_REQUIRE(vita_gpu_create_readback(
                    device, &readback_desc, &readback, &error),
                "create readback");

    vita_gpu_command_list_desc command_desc =
        VITA_GPU_INIT(vita_gpu_command_list_desc);
    command_desc.label = "warp-copy";
    command_desc.queue = VITA_GPU_QUEUE_GRAPHICS;
    vita_gpu_command_list command{};
    GPU_REQUIRE(vita_gpu_create_command_list(
                    device, &command_desc, &command, &error),
                "create command list");

    vita_gpu_fence_desc fence_desc =
        VITA_GPU_INIT(vita_gpu_fence_desc);
    fence_desc.label = "warp-copy-fence";
    vita_gpu_fence fence{};
    GPU_REQUIRE(vita_gpu_create_fence(
                    device, &fence_desc, &fence, &error),
                "create fence");

    GPU_REQUIRE(vita_gpu_command_begin(device, command, &error),
                "begin command list");
    vita_gpu_buffer_copy_desc copy =
        VITA_GPU_INIT(vita_gpu_buffer_copy_desc);
    copy.source = upload;
    copy.destination = gpu;
    copy.byte_size = source_data.size();
    GPU_REQUIRE(vita_gpu_cmd_copy_buffer(
                    device, command, &copy, &error),
                "copy upload to default buffer");

    vita_gpu_resource_barrier barrier =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    barrier.kind = VITA_GPU_RESOURCE_BUFFER;
    barrier.buffer = gpu;
    barrier.before = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    barrier.after = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
    barrier.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    barrier.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    GPU_REQUIRE(vita_gpu_cmd_barriers(
                    device, command, &barrier, 1, &error),
                "transition default buffer");

    vita_gpu_buffer_readback_copy_desc readback_copy =
        VITA_GPU_INIT(vita_gpu_buffer_readback_copy_desc);
    readback_copy.source = gpu;
    readback_copy.destination = readback;
    readback_copy.byte_size = source_data.size();
    GPU_REQUIRE(vita_gpu_cmd_copy_buffer_to_readback(
                    device, command, &readback_copy, &error),
                "copy default buffer to readback");
    GPU_REQUIRE(vita_gpu_command_end(device, command, &error),
                "end command list");

    const std::array<vita_gpu_command_list, 1> commands{command};
    vita_gpu_submit_desc submit = VITA_GPU_INIT(vita_gpu_submit_desc);
    submit.command_lists = commands.data();
    submit.command_list_count = static_cast<std::uint32_t>(commands.size());
    submit.signal = vita_gpu_fence_point VITA_GPU_INIT(vita_gpu_fence_point);
    submit.signal.fence = fence;
    submit.signal.value = 1;
    GPU_REQUIRE(vita_gpu_submit(
                    device, VITA_GPU_QUEUE_GRAPHICS, &submit, &error),
                "submit copy");
    GPU_REQUIRE(vita_gpu_fence_wait(
                    device, fence, 1,
                    std::numeric_limits<std::uint64_t>::max(), &error),
                "wait for copy");
    GPU_REQUIRE(vita_gpu_command_reset(device, command, &error),
                "reset completed command list");

    vita_gpu_readback_map map = VITA_GPU_INIT(vita_gpu_readback_map);
    GPU_REQUIRE(vita_gpu_map_readback(
                    device, readback, &map, &error),
                "map readback");
    REQUIRE(map.byte_size == source_data.size());
    REQUIRE(std::memcmp(map.data, source_data.data(), source_data.size()) ==
            0);
    GPU_REQUIRE(vita_gpu_unmap_readback(device, readback, &error),
                "unmap readback");

    GPU_REQUIRE(vita_gpu_destroy_command_list(device, command, &error),
                "destroy command list");
    GPU_REQUIRE(vita_gpu_destroy_readback(device, readback, &error),
                "destroy readback");
    GPU_REQUIRE(vita_gpu_destroy_buffer(device, gpu, &error),
                "destroy default buffer");
    GPU_REQUIRE(vita_gpu_destroy_buffer(device, upload, &error),
                "destroy upload buffer");
    GPU_REQUIRE(vita_gpu_destroy_fence(device, fence, &error),
                "destroy fence");
    GPU_REQUIRE(vita_gpu_device_wait_idle(device, &error),
                "wait for WARP idle");
    GPU_REQUIRE(vita_gpu_destroy_device(device, &error),
                "destroy WARP device");

    std::cout << "DX12 WARP enumeration, upload, copy, readback, and "
                 "shutdown passed\n";
    return EXIT_SUCCESS;
}
