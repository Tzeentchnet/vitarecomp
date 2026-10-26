/* Executes direct and indirect calls emitted from a synthetic module pair. */

#include "binding_a.h"
#include "binding_b.h"

#include "vitarecomp/binding.h"
#include "vitarecomp/cpu.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

#define MEMORY_BASE UINT32_C(0x10000000)
#define MEMORY_SIZE UINT32_C(0x00010000)
#define A_BASE      UINT32_C(0x10000000)
#define B_BASE      UINT32_C(0x10001000)
#define A_FUNCTION  A_BASE
#define B_CROSS     (B_BASE + UINT32_C(0x81))
#define B_CROSS_DUP (B_BASE + UINT32_C(0x91))
#define B_HLE       (B_BASE + UINT32_C(0xA1))
#define B_UNSUP     (B_BASE + UINT32_C(0xB1))
#define B_TARGET_0  (B_BASE + UINT32_C(0x200))
#define B_TARGET_1  (B_BASE + UINT32_C(0x204))
#define B_TARGET_2  (B_BASE + UINT32_C(0x208))
#define B_TARGET_3  (B_BASE + UINT32_C(0x20C))

#define CROSS_LIBRARY UINT32_C(0x11110000)
#define CROSS_NID     UINT32_C(0x11110001)
#define HLE_LIBRARY   UINT32_C(0x22220000)
#define HLE_NID       UINT32_C(0x22220001)
#define UNSUP_LIBRARY UINT32_C(0x33330000)
#define UNSUP_NID     UINT32_C(0x33330001)

static uint32_t g_hle_calls;

static void hle_override(void) {
    g_hle_calls++;
    r0 += 10u;
}

static vita_runtime_config runtime_config(void) {
    vita_runtime_config config;
    memset(&config, 0, sizeof(config));
    config.memory_base = MEMORY_BASE;
    config.memory_size = MEMORY_SIZE;
    return config;
}

int main(void) {
    vita_loader_segment_metadata segments[2] = {
        {0, 0, A_BASE, A_BASE, 0x400u, 0x400u, 7, 4},
        {0, 0, B_BASE, B_BASE, 0x400u, 0x400u, 7, 4},
    };
    vita_loader_module_metadata loaders[2] = {
        {2, 40, A_BASE, 0, 1, 0, 1, &segments[0]},
        {2, 40, B_BASE, 0, 1, 0, 1, &segments[1]},
    };
    vita_binding_export_metadata exports[] = {
        {VITA_BINDING_EXPORT_FUNCTION, CROSS_LIBRARY, CROSS_NID, A_FUNCTION}
    };
    vita_binding_function_import_metadata imports[] = {
        {CROSS_LIBRARY, CROSS_NID, B_TARGET_0, A_FUNCTION},
        {CROSS_LIBRARY, CROSS_NID, B_TARGET_1, A_FUNCTION},
        {HLE_LIBRARY, HLE_NID, B_TARGET_2, UINT32_C(0xF0000001)},
        {UNSUP_LIBRARY, UNSUP_NID, B_TARGET_3, UINT32_C(0xF0000002)},
    };
    vita_binding_module_metadata post_link[2] = {
        {exports, 1, NULL, 0, NULL, 0},
        {NULL, 0, imports, 4, NULL, 0},
    };
    vita_binding_module modules[2] = {
        {&loaders[0], &vita_generated_module_binding_a, &post_link[0]},
        {&loaders[1], &vita_generated_module_binding_b, &post_link[1]},
    };
    const vita_hle_import hle = {
        HLE_LIBRARY, HLE_NID, "FixtureHle", "hleOverride", hle_override
    };
    vita_binding_view view = {0};
    vita_binding_error error;
    vita_guest_call_result call_result;
    vita_fn cross_slot_before;
    vita_fn duplicate_slot_before;
    vita_runtime_config config = runtime_config();

    assert(vita_runtime_init(&config) == VITA_OK);
    assert(vita_hle_register(&hle, 1) == VITA_OK);
    vita_write32(B_TARGET_0, A_FUNCTION);
    vita_write32(B_TARGET_1, A_FUNCTION);
    vita_write32(B_TARGET_2, UINT32_C(0xF0000001));
    vita_write32(B_TARGET_3, UINT32_C(0xF0000002));
    assert(vita_binding_install_module_set(
               modules, 2, NULL, 0, &view, &error) == VITA_BINDING_OK);
    assert(view.function_import_count == 4u);

    r0 = 0u;
    vita_call(vita_func_binding_b_10001000);
    assert(r0 == 1u);
    vita_dispatch(B_BASE, B_CROSS);
    assert(r0 == 2u);
    vita_call(vita_func_binding_b_10001010);
    assert(r0 == 3u);
    vita_dispatch(B_BASE, B_CROSS_DUP);
    assert(r0 == 4u);

    g_hle_calls = 0u;
    vita_call(vita_func_binding_b_10001020);
    vita_dispatch(B_BASE, B_HLE);
    assert(g_hle_calls == 2u);
    assert(r0 == 24u);

    memset(&call_result, 0, sizeof(call_result));
    assert(vita_guest_call(
               vita_func_binding_b_10001030, &call_result) == VITA_OK);
    assert(call_result.outcome == VITA_GUEST_CALL_STRICT_FAILURE);
    assert(call_result.failure.kind ==
           VITA_STRICT_FAILURE_UNIMPLEMENTED_IMPORT);
    assert(call_result.failure.nid == UNSUP_NID);
    assert(call_result.failure.name != NULL);
    assert(strcmp(call_result.failure.name,
                  "FixtureUnsupported::namedUnsupported") == 0);

    vita_hle_registry_reset();
    {
        vita_binding_view rebound = {0};
        assert(vita_binding_install_module_set(
                   modules, 2, NULL, 0, &rebound, &error) ==
               VITA_BINDING_OK);
        memset(&call_result, 0, sizeof(call_result));
        assert(vita_guest_call(
                   vita_func_binding_b_10001020, &call_result) == VITA_OK);
        assert(call_result.outcome == VITA_GUEST_CALL_STRICT_FAILURE);
        assert(call_result.failure.nid == HLE_NID);
        assert(call_result.failure.name != NULL);
        assert(strcmp(call_result.failure.name,
                      "FixtureHle::hleOverride") == 0);
        vita_binding_view_destroy(&rebound);
    }

    cross_slot_before = *vita_generated_module_binding_b
                             .function_imports[0].target_slot;
    duplicate_slot_before = *vita_generated_module_binding_b
                                 .function_imports[1].target_slot;
    vita_write32(B_TARGET_0, A_FUNCTION ^ UINT32_C(0x100));
    {
        vita_binding_view failed_view = {0};
        assert(vita_binding_install_module_set(
                   modules, 2, NULL, 0, &failed_view, &error) ==
               VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH);
        assert(failed_view.function_import_count == 0u);
    }
    assert(*vita_generated_module_binding_b
                .function_imports[0].target_slot == cross_slot_before);
    assert(*vita_generated_module_binding_b
                .function_imports[1].target_slot == duplicate_slot_before);

    vita_binding_view_destroy(&view);
    vita_runtime_shutdown();
    puts("emitted cross-module binding tests passed");
    return 0;
}
