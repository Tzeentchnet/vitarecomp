#include "gpu_internal.hpp"
#include "../sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace vitarecomp::gpu {
namespace {

constexpr vita_gpu_shader_flags kKnownShaderFlags =
    VITA_GPU_SHADER_DEBUG | VITA_GPU_SHADER_DISABLE_OPTIMIZE |
    VITA_GPU_SHADER_WARNINGS_AS_ERROR;

Status invalid(std::string message) {
    return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT, std::move(message));
}

bool bounded_string(const char *text, std::size_t maximum,
                    bool allow_empty = false) {
    if (text == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < maximum; ++i) {
        if (text[i] == '\0') {
            return allow_empty || i != 0;
        }
    }
    return false;
}

bool identifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) != 0 || first == '_')) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || value == '_';
    });
}

bool profile_matches(vita_gpu_shader_stage stage, std::string_view profile) {
    constexpr std::array<std::string_view, 3> prefixes{"vs_", "ps_", "cs_"};
    const auto stage_index = static_cast<std::size_t>(stage);
    if (stage_index >= prefixes.size() ||
        !profile.starts_with(prefixes[stage_index])) {
        return false;
    }
    const std::string_view version = profile.substr(3);
    if (version.size() != 3 || version[0] != '6' || version[1] != '_' ||
        version[2] < '0' || version[2] > '9') {
        return false;
    }
    return true;
}

class Sha256 {
  public:
    Sha256() { vita_sha256_init(&context_); }

    void update(std::span<const std::uint8_t> bytes) {
        vita_sha256_update(&context_, bytes.data(), bytes.size());
    }

    void update(std::string_view text) {
        update(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
    }

    void update_u32(std::uint32_t value) {
        vita_sha256_update_u32(&context_, value);
    }

    void update_u64(std::uint64_t value) {
        vita_sha256_update_u64(&context_, value);
    }

    void update_field(std::string_view value) {
        vita_sha256_update_field(&context_, value.data(), value.size());
    }

    std::array<std::uint8_t, 32> finish() {
        std::array<std::uint8_t, 32> digest{};
        vita_sha256_finish(&context_, digest.data());
        return digest;
    }

  private:
    vita_sha256_context context_{};
};

} // namespace

Status validate_shader_request(const vita_gpu_shader_request_desc &request) {
    Status status =
        validate_struct(&request, sizeof(request), "shader_request_desc");
    if (!status.ok()) {
        return status;
    }
    if (request.source == nullptr || request.source_size == 0 ||
        request.source_size > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::size_t>::max())) {
        return invalid(
            "shader request requires non-empty addressable HLSL source");
    }
    if (!bounded_string(request.source_name, 1024) ||
        !bounded_string(request.entry_point, 256) ||
        !bounded_string(request.profile, 32)) {
        return invalid(
            "shader source name, entry point, or profile is invalid");
    }
    if (request.stage < VITA_GPU_SHADER_VERTEX ||
        request.stage > VITA_GPU_SHADER_COMPUTE ||
        request.target < VITA_GPU_SHADER_TARGET_DXIL ||
        request.target > VITA_GPU_SHADER_TARGET_SPIRV) {
        return invalid("shader stage or target is invalid");
    }
    if (!profile_matches(request.stage, request.profile)) {
        return invalid("shader profile does not match stage or Shader Model 6");
    }
    if ((request.flags & ~kKnownShaderFlags) != 0) {
        return invalid("shader flags contain unknown bits");
    }
    if ((request.defines == nullptr) != (request.define_count == 0) ||
        request.define_count > 256) {
        return invalid("shader defines pointer or count is invalid");
    }
    const auto source_size = static_cast<std::size_t>(request.source_size);
    if (std::find(request.source, request.source + source_size, '\0') !=
        request.source + source_size) {
        return invalid("shader source may not contain embedded NUL bytes");
    }

    std::vector<std::string_view> names;
    names.reserve(request.define_count);
    for (std::uint32_t i = 0; i < request.define_count; ++i) {
        const vita_gpu_shader_define &define = request.defines[i];
        status = validate_struct(&define, sizeof(define), "shader_define");
        if (!status.ok()) {
            return status;
        }
        if (!bounded_string(define.name, 256) ||
            !bounded_string(define.value, 1024, true) ||
            !identifier(define.name)) {
            return invalid("shader define has an invalid name or value");
        }
        names.emplace_back(define.name);
    }
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
        return invalid("shader define names must be unique");
    }
    return Status::success();
}

Status make_shader_key(const vita_gpu_shader_request_desc &request,
                       vita_gpu_shader_key &output) {
    Status status = validate_shader_request(request);
    if (!status.ok()) {
        return status;
    }
    status = validate_output_struct(&output, sizeof(output), "shader_key");
    if (!status.ok()) {
        return status;
    }

    std::vector<const vita_gpu_shader_define *> defines;
    defines.reserve(request.define_count);
    for (std::uint32_t i = 0; i < request.define_count; ++i) {
        defines.push_back(&request.defines[i]);
    }
    std::sort(defines.begin(), defines.end(),
              [](const auto *left, const auto *right) {
                  const int by_name = std::strcmp(left->name, right->name);
                  if (by_name != 0) {
                      return by_name < 0;
                  }
                  return std::strcmp(left->value, right->value) < 0;
              });

    Sha256 hash;
    hash.update("vitarecomp.shader-request.v1");
    hash.update_u32(static_cast<std::uint32_t>(request.stage));
    hash.update_u32(static_cast<std::uint32_t>(request.target));
    hash.update_u64(request.flags);
    hash.update_field(request.source_name);
    hash.update_field(request.entry_point);
    hash.update_field(request.profile);
    hash.update_u64(request.source_size);
    hash.update(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(request.source),
        static_cast<std::size_t>(request.source_size)));
    hash.update_u32(static_cast<std::uint32_t>(defines.size()));
    for (const vita_gpu_shader_define *define : defines) {
        hash.update_field(define->name);
        hash.update_field(define->value);
    }
    const auto digest = hash.finish();
    std::copy(digest.begin(), digest.end(), std::begin(output.bytes));
    return Status::success();
}

} // namespace vitarecomp::gpu
