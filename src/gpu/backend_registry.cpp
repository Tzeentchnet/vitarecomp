#include "gpu_internal.hpp"

#include <algorithm>
#include <cstring>

namespace vitarecomp::gpu {

std::optional<std::size_t> BackendRegistry::slot(
    vita_gpu_backend backend) noexcept {
    switch (backend) {
    case VITA_GPU_BACKEND_DX12:
        return 0;
    case VITA_GPU_BACKEND_VULKAN:
        return 1;
    default:
        return std::nullopt;
    }
}

Status BackendRegistry::register_factory(
    std::shared_ptr<BackendFactory> factory) {
    if (sealed_) {
        return Status::error(VITA_GPU_ERROR_INVALID_STATE,
                             "backend registry is sealed");
    }
    if (!factory) {
        return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                             "backend factory is null");
    }
    const auto index = slot(factory->backend());
    if (!index.has_value()) {
        return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                             "backend factory identifies an unknown backend");
    }
    if (factory->test_only() && !allow_test_backends_) {
        return Status::error(
            VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
            "test-only backend factories cannot enter the production registry");
    }
    if (factories_[*index]) {
        return Status::error(VITA_GPU_ERROR_INVALID_STATE,
                             "backend factory is already registered",
                             factory->backend());
    }
    factories_[*index] = std::move(factory);
    return Status::success();
}

std::shared_ptr<const BackendFactory> BackendRegistry::find(
    vita_gpu_backend backend) const {
    const auto index = slot(backend);
    return index.has_value() ? factories_[*index] : nullptr;
}

BackendRegistry make_production_registry() {
    BackendRegistry registry;

    /*
     * Concrete backends register here under build-time feature macros. There
     * is intentionally no null/default factory: absence is reported to the
     * caller as VITA_GPU_ERROR_BACKEND_UNAVAILABLE.
     */
#if defined(VITA_GPU_HAS_DX12_BACKEND)
    extern std::shared_ptr<BackendFactory> make_dx12_backend_factory();
    (void)registry.register_factory(make_dx12_backend_factory());
#endif
#if defined(VITA_GPU_HAS_VULKAN_BACKEND)
    extern std::shared_ptr<BackendFactory> make_vulkan_backend_factory();
    (void)registry.register_factory(make_vulkan_backend_factory());
#endif

    registry.seal();
    return registry;
}

Runtime &production_runtime() {
    static Runtime runtime(make_production_registry());
    return runtime;
}

namespace {

void copy_text(char *destination, std::size_t destination_size,
               std::string_view text) noexcept {
    if (destination_size == 0) {
        return;
    }
    const std::size_t count =
        std::min(destination_size - 1U, text.size());
    if (count != 0) {
        std::memcpy(destination, text.data(), count);
    }
    destination[count] = '\0';
}

} // namespace

void write_error(vita_gpu_error *error, const Status &status,
                 std::string_view function_name) noexcept {
    if (error == nullptr || error->struct_version != VITA_GPU_STRUCT_VERSION_1 ||
        error->struct_size < sizeof(vita_gpu_error)) {
        return;
    }
    error->result = status.code;
    error->backend = status.backend;
    error->native_code = status.native_code;
    copy_text(error->function_name, sizeof(error->function_name),
              function_name);
    copy_text(error->message, sizeof(error->message), status.message);
}

} // namespace vitarecomp::gpu
