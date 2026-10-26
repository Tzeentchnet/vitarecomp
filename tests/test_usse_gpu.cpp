/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/gpu.h"
#include "vitarecomp/usse.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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
             std::string_view operation) {
    if (result != VITA_GPU_SUCCESS) {
        throw Failure(
            std::string(operation) + ": " + vita_gpu_result_string(result) +
            " native=" + std::to_string(error.native_code) +
            " function=" + error.function_name + " message=" + error.message);
    }
}

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw Failure(std::string(message));
    }
}

std::vector<std::uint8_t> read_binary(const char *path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw Failure(std::string("cannot open shader: ") + path);
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        throw Failure(std::string("shader is empty: ") + path);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!stream) {
        throw Failure(std::string("cannot read shader: ") + path);
    }
    return bytes;
}

std::uint32_t choose_adapter(vita_gpu_backend backend) {
    vita_gpu_error error = make_error();
    std::uint32_t count = 0U;
    require(vita_gpu_adapter_count(backend, &count, &error), error,
            "enumerate adapters");
    check(count != 0U, "backend exposed no adapters");
    if (backend != VITA_GPU_BACKEND_DX12) {
        return 0U;
    }
    for (std::uint32_t index = 0U; index < count; ++index) {
        vita_gpu_adapter_info info = VITA_GPU_INIT(vita_gpu_adapter_info);
        require(vita_gpu_get_adapter_info(backend, index, &info, &error), error,
                "query DX12 adapter");
        if (info.type == VITA_GPU_ADAPTER_SOFTWARE) {
            check(std::strstr(info.name, "WARP") != nullptr,
                  "software DX12 adapter is not WARP");
            return index;
        }
    }
    throw Failure("DX12 WARP adapter is unavailable");
}

float next_test_value(std::uint32_t &state) {
    static constexpr std::array<std::uint32_t, 16> edge_bits{
        0x00000000U, 0x80000000U, 0x3f800000U, 0xbf800000U,
        0x3f000000U, 0xbf000000U, 0x00800000U, 0x80800000U,
        0x00000001U, 0x80000001U, 0x7f7fffffU, 0xff7fffffU,
        0x7f800000U, 0xff800000U, 0x7fc12345U, 0xffc54321U,
    };
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    if ((state & 3U) != 0U) {
        return std::bit_cast<float>(
            edge_bits[(state >> 8U) % edge_bits.size()]);
    }
    const std::uint32_t sign = state & 0x80000000U;
    const std::uint32_t exponent = (((state >> 23U) % 254U) + 1U) << 23U;
    return std::bit_cast<float>(sign | exponent | (state & 0x007fffffU));
}

constexpr std::size_t kCaseCount = 96U;
constexpr std::size_t kWordsPerCase = VITA_USSE_HLSL_STATE_WORD_COUNT;

std::vector<float> make_input_states() {
    std::vector<float> states(kCaseCount * kWordsPerCase, 12345.25F);
    std::uint32_t random_state = 0x51f15e5dU;
    for (std::size_t test_case = 0U; test_case < kCaseCount; ++test_case) {
        float *state = states.data() + test_case * kWordsPerCase;
        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            state[VITA_USSE_HLSL_STATE_PRIMARY_OFFSET + 4U + lane] =
                next_test_value(random_state);
            state[VITA_USSE_HLSL_STATE_SECONDARY_OFFSET + 16U + lane] =
                next_test_value(random_state);
            state[VITA_USSE_HLSL_STATE_SECONDARY_OFFSET + 20U + lane] =
                next_test_value(random_state);
        }
    }
    float *basic = states.data();
    basic[VITA_USSE_HLSL_STATE_PRIMARY_OFFSET + 4U] = 1.0F;
    basic[VITA_USSE_HLSL_STATE_PRIMARY_OFFSET + 5U] = 2.0F;
    basic[VITA_USSE_HLSL_STATE_PRIMARY_OFFSET + 6U] = 3.0F;
    basic[VITA_USSE_HLSL_STATE_PRIMARY_OFFSET + 7U] = 4.0F;
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        basic[VITA_USSE_HLSL_STATE_SECONDARY_OFFSET + 16U + lane] =
            static_cast<float>(lane + 1U);
        basic[VITA_USSE_HLSL_STATE_SECONDARY_OFFSET + 20U + lane] =
            static_cast<float>(lane + 5U);
    }
    return states;
}

float unfused_mad(float left, float right, float addend) {
    volatile float product = left * right;
    return product + addend;
}

float flush_subnormal(float value) {
    return std::fpclassify(value) == FP_SUBNORMAL ? std::copysign(0.0F, value)
                                                  : value;
}

bool matches_float(float actual, float expected) {
    if (std::isnan(expected)) {
        return std::isnan(actual);
    }
    if (std::isinf(expected)) {
        return std::isinf(actual) &&
               std::signbit(actual) == std::signbit(expected);
    }
    if (actual == expected) {
        return true;
    }
    if (std::isnan(actual) || std::isinf(actual)) {
        return false;
    }
    const float difference = std::fabs(actual - expected);
    const float scale = std::max(std::fabs(actual), std::fabs(expected));
    return difference <= std::max(1.0e-6F, scale * 2.0e-5F);
}

float read_float(const std::uint8_t *bytes, std::size_t word) {
    float value;
    std::memcpy(&value, bytes + word * sizeof(float), sizeof(value));
    return value;
}

void expect_float(const std::uint8_t *bytes, std::size_t word, float expected,
                  std::string_view label, std::size_t test_case,
                  std::size_t lane) {
    const float actual = read_float(bytes, word);
    if (!matches_float(actual, expected)) {
        throw Failure(std::string(label) + " mismatch at case " +
                      std::to_string(test_case) + " lane " +
                      std::to_string(lane) +
                      ": actual=" + std::to_string(actual) +
                      " expected=" + std::to_string(expected));
    }
}

void validate_results(const std::vector<float> &input,
                      const vita_gpu_readback_map &mapping) {
    const std::size_t required_bytes = input.size() * sizeof(input[0]);
    check(mapping.byte_size >= required_bytes,
          "readback is smaller than USSE state");
    for (std::size_t test_case = 0U; test_case < kCaseCount; ++test_case) {
        const std::size_t base = test_case * kWordsPerCase;
        const float *source = input.data() + base;
        const std::size_t internal =
            base + VITA_USSE_HLSL_STATE_INTERNAL_OFFSET;
        std::array<float, 4> internal0{};
        std::array<float, 4> internal1{};

        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            internal0[lane] =
                source[VITA_USSE_HLSL_STATE_PRIMARY_OFFSET + 4U + lane];
            expect_float(mapping.data, internal + lane, internal0[lane],
                         "VPCK internal0", test_case, lane);
        }

        for (std::size_t lane = 0U; lane < 4U; ++lane) {
            const float secondary =
                source[VITA_USSE_HLSL_STATE_SECONDARY_OFFSET + 16U + lane];
            internal1[lane] = -std::fabs(flush_subnormal(secondary)) *
                              flush_subnormal(internal0[lane]);
            const float actual_internal1 =
                read_float(mapping.data, internal + 4U + lane);
            if (!matches_float(actual_internal1, internal1[lane])) {
                throw Failure("V32NMAD internal1 mismatch at case " +
                              std::to_string(test_case) + " lane " +
                              std::to_string(lane) +
                              ": actual=" + std::to_string(actual_internal1) +
                              " expected=" + std::to_string(internal1[lane]) +
                              " secondary=" + std::to_string(secondary) +
                              " primary=" + std::to_string(internal0[lane]));
            }

            const float multiplier = flush_subnormal(internal0[1]);
            const float mad_secondary = -std::fabs(flush_subnormal(
                source[VITA_USSE_HLSL_STATE_SECONDARY_OFFSET + 20U + lane]));
            float expected = 0.0F;
            float alternate = 0.0F;
            if ((UINT32_C(0xD) & (1U << lane)) != 0U) {
                expected = std::fma(multiplier, mad_secondary, internal1[lane]);
                alternate =
                    unfused_mad(multiplier, mad_secondary, internal1[lane]);
            }
            const float actual = read_float(mapping.data, internal + 8U + lane);
            if (!matches_float(actual, expected) &&
                !matches_float(actual, alternate)) {
                throw Failure("VMAD internal2 mismatch at case " +
                              std::to_string(test_case) + " lane " +
                              std::to_string(lane) +
                              ": actual=" + std::to_string(actual) +
                              " fused=" + std::to_string(expected) +
                              " unfused=" + std::to_string(alternate));
            }
            expect_float(mapping.data, internal + 12U + lane, 0.0F,
                         "initialized internal3", test_case, lane);
        }

        for (std::size_t word = 0U; word < 128U; ++word) {
            expect_float(mapping.data,
                         base + VITA_USSE_HLSL_STATE_TEMPORARY_OFFSET + word,
                         0.0F, "initialized temporary", test_case, word);
            expect_float(mapping.data,
                         base + VITA_USSE_HLSL_STATE_OUTPUT_OFFSET + word, 0.0F,
                         "initialized output", test_case, word);
        }
    }
}

int run(vita_gpu_backend backend, const char *shader_path) {
    const std::vector<std::uint8_t> shader_bytes = read_binary(shader_path);
    const std::vector<float> input_states = make_input_states();
    const std::uint64_t byte_size =
        input_states.size() * sizeof(input_states[0]);
    vita_gpu_error error = make_error();

    vita_gpu_device_desc device_desc = VITA_GPU_INIT(vita_gpu_device_desc);
    device_desc.backend = backend;
    device_desc.adapter_index = choose_adapter(backend);
    device_desc.flags = VITA_GPU_DEVICE_ENABLE_DEBUG_NAMES;
    device_desc.application_name = "vitarecomp-usse-conformance";
    vita_gpu_device device{};
    require(vita_gpu_create_device(&device_desc, &device, &error), error,
            "create device");

    vita_gpu_device_capabilities capabilities =
        VITA_GPU_INIT(vita_gpu_device_capabilities);
    require(vita_gpu_device_get_capabilities(device, &capabilities, &error),
            error, "query device capabilities");
    check((capabilities.flags & VITA_GPU_CAPABILITY_COMPUTE) != 0U,
          "adapter lacks compute capability");

    vita_gpu_shader_binary_desc binary =
        VITA_GPU_INIT(vita_gpu_shader_binary_desc);
    binary.stage = VITA_GPU_SHADER_COMPUTE;
    binary.target = backend == VITA_GPU_BACKEND_DX12
                        ? VITA_GPU_SHADER_TARGET_DXIL
                        : VITA_GPU_SHADER_TARGET_SPIRV;
    binary.data = shader_bytes.data();
    binary.data_size = shader_bytes.size();
    binary.key = vita_gpu_shader_key VITA_GPU_INIT(vita_gpu_shader_key);
    vita_gpu_shader_module_desc shader_desc =
        VITA_GPU_INIT(vita_gpu_shader_module_desc);
    shader_desc.label = "synthetic-usse-compute";
    shader_desc.input = VITA_GPU_SHADER_INPUT_BINARY;
    shader_desc.binary = &binary;
    vita_gpu_shader_module shader{};
    require(
        vita_gpu_create_shader_module(device, &shader_desc, &shader, &error),
        error, "create compute shader");

    vita_gpu_binding_layout_entry layout_entry =
        VITA_GPU_INIT(vita_gpu_binding_layout_entry);
    layout_entry.binding = 0U;
    layout_entry.array_count = 1U;
    layout_entry.type = VITA_GPU_BINDING_STORAGE_BUFFER;
    layout_entry.visibility = VITA_GPU_SHADER_STAGE_COMPUTE_BIT;
    vita_gpu_binding_layout_desc layout_desc =
        VITA_GPU_INIT(vita_gpu_binding_layout_desc);
    layout_desc.label = "synthetic-usse-layout";
    layout_desc.set_index = 0U;
    layout_desc.entries = &layout_entry;
    layout_desc.entry_count = 1U;
    vita_gpu_binding_layout layout{};
    require(
        vita_gpu_create_binding_layout(device, &layout_desc, &layout, &error),
        error, "create binding layout");

    vita_gpu_compute_pipeline_desc pipeline_desc =
        VITA_GPU_INIT(vita_gpu_compute_pipeline_desc);
    pipeline_desc.label = "synthetic-usse-pipeline";
    pipeline_desc.compute_shader = shader;
    pipeline_desc.binding_layouts = &layout;
    pipeline_desc.binding_layout_count = 1U;
    vita_gpu_pipeline pipeline{};
    require(vita_gpu_create_compute_pipeline(device, &pipeline_desc, &pipeline,
                                             &error),
            error, "create compute pipeline");

    vita_gpu_buffer_desc upload_desc = VITA_GPU_INIT(vita_gpu_buffer_desc);
    upload_desc.label = "synthetic-usse-upload";
    upload_desc.byte_size = byte_size;
    upload_desc.element_stride = sizeof(float);
    upload_desc.usage = VITA_GPU_BUFFER_COPY_SOURCE;
    upload_desc.memory_type = VITA_GPU_MEMORY_CPU_UPLOAD;
    upload_desc.initial_state = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
    upload_desc.initial_data =
        reinterpret_cast<const std::uint8_t *>(input_states.data());
    upload_desc.initial_data_size = byte_size;
    vita_gpu_buffer upload{};
    require(vita_gpu_create_buffer(device, &upload_desc, &upload, &error),
            error, "create upload buffer");

    vita_gpu_buffer_desc state_desc = VITA_GPU_INIT(vita_gpu_buffer_desc);
    state_desc.label = "synthetic-usse-state";
    state_desc.byte_size = byte_size;
    state_desc.element_stride = sizeof(float);
    state_desc.usage = VITA_GPU_BUFFER_STORAGE | VITA_GPU_BUFFER_COPY_SOURCE |
                       VITA_GPU_BUFFER_COPY_DEST;
    state_desc.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    state_desc.initial_state = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    vita_gpu_buffer state{};
    require(vita_gpu_create_buffer(device, &state_desc, &state, &error), error,
            "create state buffer");

    vita_gpu_binding_set_desc set_desc =
        VITA_GPU_INIT(vita_gpu_binding_set_desc);
    set_desc.label = "synthetic-usse-set";
    set_desc.layout = layout;
    vita_gpu_binding_set set{};
    require(vita_gpu_create_binding_set(device, &set_desc, &set, &error), error,
            "create binding set");
    vita_gpu_binding_write write = VITA_GPU_INIT(vita_gpu_binding_write);
    write.binding = 0U;
    write.type = VITA_GPU_BINDING_STORAGE_BUFFER;
    write.buffer = state;
    write.buffer_size = byte_size;
    require(vita_gpu_update_binding_set(device, set, &write, 1U, &error), error,
            "write storage binding");

    vita_gpu_readback_desc readback_desc =
        VITA_GPU_INIT(vita_gpu_readback_desc);
    readback_desc.label = "synthetic-usse-readback";
    readback_desc.byte_size = byte_size;
    vita_gpu_readback readback{};
    require(vita_gpu_create_readback(device, &readback_desc, &readback, &error),
            error, "create readback");

    vita_gpu_command_list_desc command_desc =
        VITA_GPU_INIT(vita_gpu_command_list_desc);
    command_desc.label = "synthetic-usse-command";
    command_desc.queue = VITA_GPU_QUEUE_GRAPHICS;
    vita_gpu_command_list command{};
    require(
        vita_gpu_create_command_list(device, &command_desc, &command, &error),
        error, "create command list");

    vita_gpu_fence_desc fence_desc = VITA_GPU_INIT(vita_gpu_fence_desc);
    fence_desc.label = "synthetic-usse-fence";
    vita_gpu_fence fence{};
    require(vita_gpu_create_fence(device, &fence_desc, &fence, &error), error,
            "create fence");

    require(vita_gpu_command_begin(device, command, &error), error,
            "begin command list");
    vita_gpu_buffer_copy_desc upload_copy =
        VITA_GPU_INIT(vita_gpu_buffer_copy_desc);
    upload_copy.source = upload;
    upload_copy.destination = state;
    upload_copy.byte_size = byte_size;
    require(vita_gpu_cmd_copy_buffer(device, command, &upload_copy, &error),
            error, "copy input state");

    vita_gpu_resource_barrier to_shader =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    to_shader.kind = VITA_GPU_RESOURCE_BUFFER;
    to_shader.buffer = state;
    to_shader.before = VITA_GPU_RESOURCE_STATE_COPY_DEST;
    to_shader.after = VITA_GPU_RESOURCE_STATE_SHADER_WRITE;
    to_shader.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    to_shader.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    require(vita_gpu_cmd_barriers(device, command, &to_shader, 1U, &error),
            error, "transition state to shader write");
    require(vita_gpu_cmd_bind_pipeline(device, command, pipeline, &error),
            error, "bind compute pipeline");
    require(vita_gpu_cmd_bind_set(device, command, 0U, set, &error), error,
            "bind storage set");
    vita_gpu_dispatch_desc dispatch = VITA_GPU_INIT(vita_gpu_dispatch_desc);
    dispatch.group_count_x = static_cast<std::uint32_t>(kCaseCount);
    dispatch.group_count_y = 1U;
    dispatch.group_count_z = 1U;
    require(vita_gpu_cmd_dispatch(device, command, &dispatch, &error), error,
            "dispatch semantic cases");

    vita_gpu_resource_barrier to_copy =
        VITA_GPU_INIT(vita_gpu_resource_barrier);
    to_copy.kind = VITA_GPU_RESOURCE_BUFFER;
    to_copy.buffer = state;
    to_copy.before = VITA_GPU_RESOURCE_STATE_SHADER_WRITE;
    to_copy.after = VITA_GPU_RESOURCE_STATE_COPY_SOURCE;
    to_copy.source_queue = VITA_GPU_QUEUE_GRAPHICS;
    to_copy.destination_queue = VITA_GPU_QUEUE_GRAPHICS;
    require(vita_gpu_cmd_barriers(device, command, &to_copy, 1U, &error), error,
            "transition state to copy source");
    vita_gpu_buffer_readback_copy_desc readback_copy =
        VITA_GPU_INIT(vita_gpu_buffer_readback_copy_desc);
    readback_copy.source = state;
    readback_copy.destination = readback;
    readback_copy.byte_size = byte_size;
    require(vita_gpu_cmd_copy_buffer_to_readback(device, command,
                                                 &readback_copy, &error),
            error, "copy state to readback");
    require(vita_gpu_command_end(device, command, &error), error,
            "end command list");

    vita_gpu_fence_point signal = VITA_GPU_INIT(vita_gpu_fence_point);
    signal.fence = fence;
    signal.value = 1U;
    vita_gpu_submit_desc submit = VITA_GPU_INIT(vita_gpu_submit_desc);
    submit.command_lists = &command;
    submit.command_list_count = 1U;
    submit.signal = signal;
    require(vita_gpu_submit(device, VITA_GPU_QUEUE_GRAPHICS, &submit, &error),
            error, "submit semantic dispatch");
    require(
        vita_gpu_fence_wait(device, fence, 1U, UINT64_C(10000000000), &error),
        error, "wait semantic dispatch");
    require(vita_gpu_command_reset(device, command, &error), error,
            "reset completed command list");

    vita_gpu_readback_map mapping = VITA_GPU_INIT(vita_gpu_readback_map);
    require(vita_gpu_map_readback(device, readback, &mapping, &error), error,
            "map semantic readback");
    validate_results(input_states, mapping);
    require(vita_gpu_unmap_readback(device, readback, &error), error,
            "unmap semantic readback");

    require(vita_gpu_destroy_fence(device, fence, &error), error,
            "destroy fence");
    require(vita_gpu_destroy_command_list(device, command, &error), error,
            "destroy command list");
    require(vita_gpu_destroy_readback(device, readback, &error), error,
            "destroy readback");
    require(vita_gpu_destroy_binding_set(device, set, &error), error,
            "destroy binding set");
    require(vita_gpu_destroy_buffer(device, state, &error), error,
            "destroy state buffer");
    require(vita_gpu_destroy_buffer(device, upload, &error), error,
            "destroy upload buffer");
    require(vita_gpu_destroy_pipeline(device, pipeline, &error), error,
            "destroy pipeline");
    require(vita_gpu_destroy_binding_layout(device, layout, &error), error,
            "destroy binding layout");
    require(vita_gpu_destroy_shader_module(device, shader, &error), error,
            "destroy shader");
    require(vita_gpu_destroy_device(device, &error), error, "destroy device");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: test_usse_gpu <dx12|vulkan> <shader>\n";
        return 2;
    }
    try {
        const std::string_view backend_name = argv[1];
        vita_gpu_backend backend;
        if (backend_name == "dx12") {
            backend = VITA_GPU_BACKEND_DX12;
        } else if (backend_name == "vulkan") {
            backend = VITA_GPU_BACKEND_VULKAN;
        } else {
            throw Failure("unknown backend");
        }
        const int result = run(backend, argv[2]);
        std::cout << "USSE semantic conformance passed on " << backend_name
                  << '\n';
        return result;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
