#include "gpu_internal.hpp"

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

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     std::uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

class Sha256 {
public:
    Sha256()
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(std::span<const std::uint8_t> bytes) {
        total_bytes_ += bytes.size();
        for (const std::uint8_t byte : bytes) {
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0;
            }
        }
    }

    void update(std::string_view text) {
        update(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
    }

    void update_u32(std::uint32_t value) {
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 24U)};
        update(bytes);
    }

    void update_u64(std::uint64_t value) {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(value >> (i * 8U));
        }
        update(bytes);
    }

    void update_field(std::string_view value) {
        update_u64(value.size());
        update(value);
    }

    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bit_count =
            static_cast<std::uint64_t>(total_bytes_) * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            while (buffer_size_ < buffer_.size()) {
                buffer_[buffer_size_++] = 0;
            }
            transform(buffer_);
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56) {
            buffer_[buffer_size_++] = 0;
        }
        for (std::size_t i = 0; i < 8; ++i) {
            buffer_[63U - i] =
                static_cast<std::uint8_t>(bit_count >> (i * 8U));
        }
        transform(buffer_);

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4U] = static_cast<std::uint8_t>(state_[i] >> 24U);
            digest[i * 4U + 1U] =
                static_cast<std::uint8_t>(state_[i] >> 16U);
            digest[i * 4U + 2U] =
                static_cast<std::uint8_t>(state_[i] >> 8U);
            digest[i * 4U + 3U] = static_cast<std::uint8_t>(state_[i]);
        }
        return digest;
    }

private:
    void transform(const std::array<std::uint8_t, 64> &block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t offset = i * 4U;
            words[i] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                       (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                       static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const std::uint32_t s0 =
                rotate_right(words[i - 15U], 7U) ^
                rotate_right(words[i - 15U], 18U) ^
                (words[i - 15U] >> 3U);
            const std::uint32_t s1 =
                rotate_right(words[i - 2U], 17U) ^
                rotate_right(words[i - 2U], 19U) ^
                (words[i - 2U] >> 10U);
            words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
        }

        std::array<std::uint32_t, 8> working = state_;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t sum1 =
                rotate_right(working[4], 6U) ^
                rotate_right(working[4], 11U) ^
                rotate_right(working[4], 25U);
            const std::uint32_t choose =
                (working[4] & working[5]) ^
                ((~working[4]) & working[6]);
            const std::uint32_t temporary1 =
                working[7] + sum1 + choose + kRoundConstants[i] + words[i];
            const std::uint32_t sum0 =
                rotate_right(working[0], 2U) ^
                rotate_right(working[0], 13U) ^
                rotate_right(working[0], 22U);
            const std::uint32_t majority =
                (working[0] & working[1]) ^
                (working[0] & working[2]) ^
                (working[1] & working[2]);
            const std::uint32_t temporary2 = sum0 + majority;

            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temporary1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temporary1 + temporary2;
        }
        for (std::size_t i = 0; i < state_.size(); ++i) {
            state_[i] += working[i];
        }
    }

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::size_t total_bytes_ = 0;
};

} // namespace

Status validate_shader_request(const vita_gpu_shader_request_desc &request) {
    Status status =
        validate_struct(&request, sizeof(request), "shader_request_desc");
    if (!status.ok()) {
        return status;
    }
    if (request.source == nullptr || request.source_size == 0 ||
        request.source_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
        return invalid("shader request requires non-empty addressable HLSL source");
    }
    if (!bounded_string(request.source_name, 1024) ||
        !bounded_string(request.entry_point, 256) ||
        !bounded_string(request.profile, 32)) {
        return invalid("shader source name, entry point, or profile is invalid");
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
    std::sort(defines.begin(), defines.end(), [](const auto *left,
                                                 const auto *right) {
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
