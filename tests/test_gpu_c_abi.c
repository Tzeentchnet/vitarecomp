#include "vitarecomp/gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                   \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

int main(void) {
    vita_gpu_error error = VITA_GPU_INIT(vita_gpu_error);
    vita_gpu_backend backend = VITA_GPU_BACKEND_NONE;

    CHECK(vita_gpu_get_abi_version() == VITA_GPU_ABI_VERSION);
    CHECK(vita_gpu_backend_count() == 2);
    CHECK(vita_gpu_backend_from_name("dx12", &backend, &error) ==
          VITA_GPU_SUCCESS);
    CHECK(backend == VITA_GPU_BACKEND_DX12);
    CHECK(vita_gpu_backend_from_name("VULKAN", &backend, &error) ==
          VITA_GPU_SUCCESS);
    CHECK(backend == VITA_GPU_BACKEND_VULKAN);
    CHECK(vita_gpu_backend_from_name("auto", &backend, &error) ==
          VITA_GPU_ERROR_INVALID_ARGUMENT);
    CHECK(backend == VITA_GPU_BACKEND_NONE);
    CHECK(vita_gpu_backend_from_name("test-only-null", &backend, &error) ==
          VITA_GPU_ERROR_INVALID_ARGUMENT);

    for (uint32_t index = 0; index < vita_gpu_backend_count(); ++index) {
        vita_gpu_backend_info info = VITA_GPU_INIT(vita_gpu_backend_info);
        CHECK(vita_gpu_get_backend_info(index, &info, &error) ==
              VITA_GPU_SUCCESS);
        CHECK(info.backend == (index == 0 ? VITA_GPU_BACKEND_DX12
                                         : VITA_GPU_BACKEND_VULKAN));
        CHECK(strcmp(info.name, index == 0 ? "dx12" : "vulkan") == 0);
        if (index == 0) {
#if defined(_WIN32)
            CHECK(info.available == VITA_GPU_TRUE);
            CHECK(info.unavailable_reason[0] == '\0');
#else
            CHECK(info.available == VITA_GPU_FALSE);
            CHECK(info.unavailable_reason[0] != '\0');
#endif
        } else {
            CHECK(info.available == VITA_GPU_FALSE);
            CHECK(info.unavailable_reason[0] != '\0');
        }
    }

    {
        vita_gpu_device_desc desc = VITA_GPU_INIT(vita_gpu_device_desc);
        vita_gpu_device device = {0};
        desc.backend = VITA_GPU_BACKEND_DX12;
        desc.application_name = "gpu-c-abi-test";
#if defined(_WIN32)
        CHECK(vita_gpu_create_device(&desc, &device, &error) ==
              VITA_GPU_SUCCESS);
        CHECK(device.value != 0);
        CHECK(vita_gpu_destroy_device(device, &error) == VITA_GPU_SUCCESS);
        device.value = 0;
#else
        CHECK(vita_gpu_create_device(&desc, &device, &error) ==
              VITA_GPU_ERROR_BACKEND_UNAVAILABLE);
        CHECK(device.value == 0);
        CHECK(error.result == VITA_GPU_ERROR_BACKEND_UNAVAILABLE);
        CHECK(error.backend == VITA_GPU_BACKEND_DX12);
        CHECK(strcmp(error.function_name, "vita_gpu_create_device") == 0);
        CHECK(strstr(error.message, "no fallback") != NULL);
#endif

        desc.backend = VITA_GPU_BACKEND_VULKAN;
        CHECK(vita_gpu_create_device(&desc, &device, &error) ==
              VITA_GPU_ERROR_BACKEND_UNAVAILABLE);
        CHECK(error.backend == VITA_GPU_BACKEND_VULKAN);
        CHECK(strstr(error.message, "no fallback") != NULL);
    }

    {
        vita_gpu_device_desc desc = VITA_GPU_INIT(vita_gpu_device_desc);
        vita_gpu_device device = {0};
        desc.struct_size = sizeof(vita_gpu_struct_header);
        desc.backend = VITA_GPU_BACKEND_DX12;
        CHECK(vita_gpu_create_device(&desc, &device, &error) ==
              VITA_GPU_ERROR_INVALID_STRUCT);
    }

    puts("gpu C ABI tests passed");
    return EXIT_SUCCESS;
}
