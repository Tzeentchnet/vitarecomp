/* SPDX-License-Identifier: MIT */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/host.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/runtime.h"
#include "vitarecomp/vfp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MEMORY_BASE UINT32_C(0x20000000)
#define MEMORY_SIZE UINT32_C(0x00800000)
#define MAIN_STACK  UINT32_C(0x20080000)
#define MAIN_STACK_SIZE UINT32_C(0x00010000)
#define HEAP_BASE   UINT32_C(0x20100000)
#define HEAP_SIZE   UINT32_C(0x00080000)
#define SYSMEM_BASE UINT32_C(0x20200000)
#define SYSMEM_SIZE UINT32_C(0x00200000)
#define KERNEL_BASE UINT32_C(0x20500000)
#define KERNEL_SIZE UINT32_C(0x00200000)
#define TLS_BASE    UINT32_C(0x20700010)
#define TLS_SIZE    UINT32_C(0x000FFFE0)

#define ARGUMENTS   UINT32_C(0x20001000)
#define IMAGE_A     UINT32_C(0x20002000)
#define IMAGE_B     UINT32_C(0x20003000)
#define IMAGE_C     UINT32_C(0x20003800)
#define THREAD_NAME UINT32_C(0x20004000)
#define THREAD_STATUS UINT32_C(0x20005000)

void vita_hle_sceKernelCreateThread(void);
void vita_hle_sceKernelStartThread(void);
void vita_hle_sceKernelWaitThreadEnd(void);
void vita_hle_sceKernelDeleteThread(void);

static vita_tls_module_layout g_tls_modules[3];
static uint32_t g_main_tls;
static uint32_t g_worker_tls;

static const uint8_t IMAGE_A_BYTES[] = {0x11, 0x22, 0x33};
static const uint8_t IMAGE_B_BYTES[] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5};
static const uint8_t IMAGE_C_BYTES[] = {0x51, 0x62, 0x73, 0x84};

static void assert_tls_image(uint32_t base) {
    for (uint32_t i = 0; i < sizeof(IMAGE_A_BYTES); i++)
        assert(vita_read8(base + g_tls_modules[0].offset + i) ==
               IMAGE_A_BYTES[i]);
    for (uint32_t i = (uint32_t)sizeof(IMAGE_A_BYTES);
         i < g_tls_modules[0].memory_size; i++)
        assert(vita_read8(base + g_tls_modules[0].offset + i) == 0);

    for (uint32_t i = 0; i < sizeof(IMAGE_B_BYTES); i++)
        assert(vita_read8(base + g_tls_modules[1].offset + i) ==
               IMAGE_B_BYTES[i]);
    for (uint32_t i = (uint32_t)sizeof(IMAGE_B_BYTES);
         i < g_tls_modules[1].memory_size; i++)
        assert(vita_read8(base + g_tls_modules[1].offset + i) == 0);

    for (uint32_t i = 0; i < sizeof(IMAGE_C_BYTES); i++)
        assert(vita_read8(base + g_tls_modules[2].offset + i) ==
               IMAGE_C_BYTES[i]);
    for (uint32_t i = (uint32_t)sizeof(IMAGE_C_BYTES);
         i < g_tls_modules[2].memory_size; i++)
        assert(vita_read8(base + g_tls_modules[2].offset + i) == 0);
}

static void tls_worker(void) {
    g_worker_tls = vita_tls_base();
    assert(g_worker_tls != 0);
    assert(g_worker_tls != g_main_tls);
    assert((g_worker_tls & 63u) == 0);
    assert(g_worker_tls >= TLS_BASE &&
           g_worker_tls < TLS_BASE + TLS_SIZE);
    assert_tls_image(g_worker_tls);
    r0 = 19;
}

static const vita_dispatch_entry DISPATCH[] = {
    {UINT32_C(0x1001), tls_worker},
};

static vita_runtime_config runtime_config(void) {
    vita_runtime_config config;
    memset(&config, 0, sizeof(config));
    config.memory_base = MEMORY_BASE;
    config.memory_size = MEMORY_SIZE;
    config.heap_base = HEAP_BASE;
    config.heap_size = HEAP_SIZE;
    config.sysmem_base = SYSMEM_BASE;
    config.sysmem_size = SYSMEM_SIZE;
    config.kernel_base = KERNEL_BASE;
    config.kernel_size = KERNEL_SIZE;
    config.tls_base = TLS_BASE;
    config.tls_size = TLS_SIZE;
    config.virtual_clock = 1;
    return config;
}

static void test_checked_stack_and_cpu_reset(void) {
    uint32_t initial_sp = UINT32_C(0xDEADBEEF);
    vita_module_start_state entry = {
        4u, ARGUMENTS, MAIN_STACK + MAIN_STACK_SIZE, UINT32_C(0x12345679)
    };
    assert(vita_main_stack_prepare(
               MAIN_STACK, MAIN_STACK_SIZE, &initial_sp) ==
           VITA_ERROR_NOT_INITIALIZED);
    assert(initial_sp == UINT32_C(0xDEADBEEF));
    assert(vita_cpu_prepare_module_start(&entry) ==
           VITA_ERROR_NOT_INITIALIZED);

    vita_runtime_config config = runtime_config();
    vita_runtime_config invalid = config;
    invalid.memory_size = SIZE_MAX;
    assert(vita_runtime_init(&invalid) == VITA_ERROR_INVALID_ARGUMENT);
    invalid = config;
    invalid.tls_base = KERNEL_BASE;
    invalid.tls_size = UINT32_C(0x1000);
    assert(vita_runtime_init(&invalid) == VITA_ERROR_INVALID_ARGUMENT);
    invalid = config;
    invalid.tls_base = MEMORY_BASE + MEMORY_SIZE - UINT32_C(0x1000);
    invalid.tls_size = UINT32_C(0x2000);
    assert(vita_runtime_init(&invalid) == VITA_ERROR_OUT_OF_BOUNDS);
    assert(vita_runtime_init(&config) == VITA_OK);
    assert(vita_main_stack_prepare(
               MAIN_STACK + 1u, MAIN_STACK_SIZE, &initial_sp) ==
           VITA_ERROR_INVALID_ARGUMENT);
    assert(vita_main_stack_prepare(
               MAIN_STACK, MAIN_STACK_SIZE - 1u, &initial_sp) ==
           VITA_ERROR_INVALID_ARGUMENT);
    assert(vita_main_stack_prepare(
               UINT32_C(0xFFFFFFF0), 0x20u, &initial_sp) ==
           VITA_ERROR_OVERFLOW);
    assert(vita_main_stack_prepare(
               MEMORY_BASE + MEMORY_SIZE, 0x1000u, &initial_sp) ==
           VITA_ERROR_OUT_OF_BOUNDS);
    assert(vita_main_stack_prepare(
               MAIN_STACK, MAIN_STACK_SIZE, &initial_sp) == VITA_OK);
    assert(initial_sp == MAIN_STACK + MAIN_STACK_SIZE);
    assert((initial_sp & 15u) == 0);

    entry.stack_pointer = initial_sp - 4u;
    assert(vita_cpu_prepare_module_start(&entry) ==
           VITA_ERROR_INVALID_ARGUMENT);
    entry.stack_pointer = MAIN_STACK - 16u;
    assert(vita_cpu_prepare_module_start(&entry) ==
           VITA_ERROR_OUT_OF_BOUNDS);
    entry.stack_pointer = initial_sp;
    entry.argument_pointer = MEMORY_BASE + MEMORY_SIZE;
    assert(vita_cpu_prepare_module_start(&entry) ==
           VITA_ERROR_OUT_OF_BOUNDS);
    entry.argument_pointer = ARGUMENTS;
    vita_write32(ARGUMENTS, UINT32_C(0xCAFEBABE));

    uint32_t *const gprs[] = {
        &r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7,
        &r8, &r9, &r10, &r11, &r12, &sp, &lr
    };
    for (size_t i = 0; i < sizeof(gprs) / sizeof(gprs[0]); i++)
        *gprs[i] = UINT32_C(0xA5000000) + (uint32_t)i;
    flag_n = flag_z = flag_c = flag_v = flag_ge = 1;
    for (size_t i = 0; i < sizeof(vfp_s) / sizeof(vfp_s[0]); i++)
        vfp_s[i].u = UINT32_C(0xF0000000) + (uint32_t)i;
    vfp_set_fpscr(UINT32_C(0xF900009F));
    vita_last_pc = UINT32_C(0x81234567);
    vita_trace_insns = 1;
    vita_trace_lo = UINT32_C(0x81000000);
    vita_trace_hi = UINT32_C(0x81FFFFFF);
    vita_slice = -7;
    (void)vita_ldrex(ARGUMENTS, 4);

    assert(vita_cpu_prepare_module_start(&entry) == VITA_OK);
    assert(r0 == entry.argument_size);
    assert(r1 == entry.argument_pointer);
    assert(r2 == 0 && r3 == 0 && r4 == 0 && r5 == 0 && r6 == 0);
    assert(r7 == 0 && r8 == 0 && r9 == 0 && r10 == 0);
    assert(r11 == 0 && r12 == 0);
    assert(sp == entry.stack_pointer && lr == entry.return_address);
    assert(!flag_n && !flag_z && !flag_c && !flag_v && !flag_ge);
    for (size_t i = 0; i < sizeof(vfp_s) / sizeof(vfp_s[0]); i++)
        assert(vfp_s[i].u == 0);
    assert(vfp_get_fpscr() == 0);
    assert(vita_last_pc == 0 && vita_trace_insns == 0);
    assert(vita_trace_lo == 0 && vita_trace_hi == 0);
    assert(vita_slice == VITA_SLICE);
    assert(vita_strex(ARGUMENTS, UINT32_C(0xDEADBEEF), 4) == 1);
}

static void test_tls_configuration(void) {
    const vita_tls_template invalid_size = {
        IMAGE_A, 5u, 4u, 16u
    };
    const vita_tls_template invalid_alignment = {
        IMAGE_A, 3u, 8u, 3u
    };
    const vita_tls_template overflow = {
        UINT32_C(0xFFFFFFF0), 32u, 32u, 16u
    };
    const vita_tls_template outside = {
        MEMORY_BASE + MEMORY_SIZE, 1u, 16u, 16u
    };
    assert(vita_tls_prepare(
               &invalid_size, 1, NULL, 0, NULL) ==
           VITA_ERROR_INVALID_ARGUMENT);
    assert(vita_tls_prepare(
               &invalid_alignment, 1, NULL, 0, NULL) ==
           VITA_ERROR_INVALID_ARGUMENT);
    assert(vita_tls_prepare(&overflow, 1, NULL, 0, NULL) ==
           VITA_ERROR_OVERFLOW);
    assert(vita_tls_prepare(&outside, 1, NULL, 0, NULL) ==
           VITA_ERROR_OUT_OF_BOUNDS);

    assert(vita_mem_copy_to_guest(
               IMAGE_A, IMAGE_A_BYTES, sizeof(IMAGE_A_BYTES)) == VITA_OK);
    assert(vita_mem_copy_to_guest(
               IMAGE_B, IMAGE_B_BYTES, sizeof(IMAGE_B_BYTES)) == VITA_OK);
    assert(vita_mem_copy_to_guest(
               IMAGE_C, IMAGE_C_BYTES, sizeof(IMAGE_C_BYTES)) == VITA_OK);
    const vita_tls_template templates[] = {
        {IMAGE_A, (uint32_t)sizeof(IMAGE_A_BYTES), 11u, 16u},
        {IMAGE_B, (uint32_t)sizeof(IMAGE_B_BYTES), 13u, 64u},
        {IMAGE_C, (uint32_t)sizeof(IMAGE_C_BYTES), 9u, 32u},
    };
    vita_tls_layout layout;
    memset(&layout, 0, sizeof(layout));
    assert(vita_tls_prepare(
               templates, 3, g_tls_modules, 3, &layout) == VITA_OK);
    assert(layout.module_count == 3);
    assert(layout.data_alignment == 64);
    assert(g_tls_modules[0].offset == 0);
    assert(g_tls_modules[1].offset == 64);
    assert(g_tls_modules[2].offset == 96);
    assert(layout.data_size == 105);
    assert(layout.allocation_size >= UINT32_C(0x800) + layout.data_size);

    g_main_tls = 0;
    assert(vita_tls_materialize_current(&g_main_tls) == VITA_OK);
    assert(g_main_tls != 0 && (g_main_tls & 63u) == 0);
    assert(g_main_tls >= TLS_BASE && g_main_tls < TLS_BASE + TLS_SIZE);
    assert_tls_image(g_main_tls);

    vita_write8(g_main_tls + g_tls_modules[0].offset, 0xFF);
    vita_write8(
        g_main_tls + g_tls_modules[1].offset +
            (uint32_t)sizeof(IMAGE_B_BYTES),
        0xFF);
    vita_write8(IMAGE_A, 0x77);
    assert(vita_tls_restart() == VITA_OK);
    g_main_tls = 0;
    assert(vita_tls_materialize_current(&g_main_tls) == VITA_OK);
    assert_tls_image(g_main_tls);
    assert(vita_mem_copy_to_guest(
               IMAGE_A, IMAGE_A_BYTES, sizeof(IMAGE_A_BYTES)) == VITA_OK);

    vita_dispatch_init(
        DISPATCH, (uint32_t)(sizeof(DISPATCH) / sizeof(DISPATCH[0])));
    static const char name[] = "tls-worker";
    assert(vita_mem_copy_to_guest(
               THREAD_NAME, name, sizeof(name)) == VITA_OK);
    r0 = THREAD_NAME;
    r1 = UINT32_C(0x1001);
    r2 = 0x40u;
    r3 = 0x4000u;
    vita_hle_sceKernelCreateThread();
    const uint32_t thread = r0;
    assert(thread != 0 && (thread & UINT32_C(0x80000000)) == 0);
    r0 = thread;
    r1 = 0;
    r2 = 0;
    vita_hle_sceKernelStartThread();
    assert(r0 == 0);
    r0 = thread;
    r1 = THREAD_STATUS;
    r2 = 0;
    vita_hle_sceKernelWaitThreadEnd();
    assert(r0 == 0);
    assert(vita_read32(THREAD_STATUS) == 19);
    r0 = thread;
    vita_hle_sceKernelDeleteThread();
    assert(r0 == 0);
    assert(g_worker_tls != 0 && g_worker_tls != g_main_tls);

    vita_generated_module_descriptor first;
    vita_generated_module_descriptor second;
    vita_generated_module_descriptor third;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&third, 0, sizeof(third));
    first.abi_version = VITA_GENERATED_MODULE_ABI_VERSION;
    first.struct_size = (uint32_t)sizeof(first);
    first.module_id = "first";
    first.tls = (vita_generated_module_tls){
        1u, 1u, IMAGE_A, (uint32_t)sizeof(IMAGE_A_BYTES), 11u, 16u
    };
    second.abi_version = VITA_GENERATED_MODULE_ABI_VERSION;
    second.struct_size = (uint32_t)sizeof(second);
    second.module_id = "second";
    second.tls = (vita_generated_module_tls){
        1u, 1u, IMAGE_B, (uint32_t)sizeof(IMAGE_B_BYTES), 13u, 64u
    };
    third.abi_version = VITA_GENERATED_MODULE_ABI_VERSION;
    third.struct_size = (uint32_t)sizeof(third);
    third.module_id = "third";
    third.tls = (vita_generated_module_tls){
        1u, 1u, IMAGE_C, (uint32_t)sizeof(IMAGE_C_BYTES), 9u, 32u
    };
    const vita_generated_module_descriptor *generated[] = {
        &first, &second, &third
    };
    vita_generated_tls_layout_entry generated_entries[] = {
        {&first, 0u, 0u, 0u, 16u},
        {&second, 1u, 1u, 64u, 64u},
        {&third, 2u, 2u, 96u, 32u},
    };
    vita_generated_tls_layout generated_layout = {
        VITA_GENERATED_MODULE_ABI_VERSION,
        (uint32_t)sizeof(vita_generated_tls_layout),
        generated_entries,
        3u,
        3u,
        105u,
        64u,
    };
    vita_generated_tls_layout two_module_layout = generated_layout;
    two_module_layout.descriptor_count = 2u;
    two_module_layout.tls_module_count = 2u;
    two_module_layout.data_size = 77u;
    assert(vita_tls_prepare_generated_modules(
               generated, 3, NULL, NULL, 0, NULL) ==
           VITA_ERROR_INVALID_ARGUMENT);
    assert(vita_tls_prepare_generated_modules(
               generated, 2, &two_module_layout, g_tls_modules, 3,
               &layout) == VITA_OK);
    assert(layout.module_count == 2 &&
           g_tls_modules[0].offset == 0 &&
           g_tls_modules[1].offset == 64);
    {
        uint32_t two_module_tls = 0;
        assert(vita_tls_materialize_current(&two_module_tls) == VITA_OK);
        for (uint32_t i = 0; i < sizeof(IMAGE_A_BYTES); i++)
            assert(vita_read8(two_module_tls + i) == IMAGE_A_BYTES[i]);
        for (uint32_t i = 0; i < sizeof(IMAGE_B_BYTES); i++)
            assert(vita_read8(two_module_tls + 64u + i) ==
                   IMAGE_B_BYTES[i]);
    }
    assert(vita_tls_prepare_generated_modules(
               generated, 3, &generated_layout, g_tls_modules, 3,
               &layout) == VITA_OK);
    assert(layout.module_count == 3 &&
           g_tls_modules[0].offset == 0 &&
           g_tls_modules[1].offset == 64 &&
           g_tls_modules[2].offset == 96);
    {
        uint32_t three_module_tls = 0;
        assert(vita_tls_materialize_current(&three_module_tls) == VITA_OK);
        assert_tls_image(three_module_tls);
    }
    generated_entries[1].expected_offset = 63u;
    assert(vita_tls_prepare_generated_modules(
               generated, 3, &generated_layout, g_tls_modules, 3,
               &layout) == VITA_ERROR_INVALID_ARGUMENT);
    generated_entries[1].expected_offset = 64u;
    second.tls.alignment = 32u;
    assert(vita_tls_prepare_generated_modules(
               generated, 3, &generated_layout, g_tls_modules, 3,
               &layout) == VITA_ERROR_INVALID_ARGUMENT);
    second.tls.alignment = 64u;
    generated_layout.data_alignment = 32u;
    assert(vita_tls_prepare_generated_modules(
               generated, 3, &generated_layout, g_tls_modules, 3,
               &layout) == VITA_ERROR_INVALID_ARGUMENT);
    generated_layout.data_alignment = 64u;

    assert(vita_tls_prepare(
               templates, 3, g_tls_modules, 3, &layout) == VITA_OK);
    assert(g_tls_modules[0].offset == 0 &&
           g_tls_modules[1].offset == 64 &&
           g_tls_modules[2].offset == 96);
}

static void test_runtime_restart(void) {
    const vita_tls_template templates[] = {
        {IMAGE_A, (uint32_t)sizeof(IMAGE_A_BYTES), 11u, 16u},
        {IMAGE_B, (uint32_t)sizeof(IMAGE_B_BYTES), 13u, 64u},
        {IMAGE_C, (uint32_t)sizeof(IMAGE_C_BYTES), 9u, 32u},
    };
    vita_runtime_shutdown();
    assert(vita_tls_restart() == VITA_ERROR_NOT_INITIALIZED);

    vita_runtime_config config = runtime_config();
    assert(vita_runtime_init(&config) == VITA_OK);
    uint32_t initial_sp = 0;
    assert(vita_main_stack_prepare(
               MAIN_STACK, MAIN_STACK_SIZE, &initial_sp) == VITA_OK);
    assert((initial_sp & 15u) == 0);
    assert(vita_mem_copy_to_guest(
               IMAGE_A, IMAGE_A_BYTES, sizeof(IMAGE_A_BYTES)) == VITA_OK);
    assert(vita_mem_copy_to_guest(
               IMAGE_B, IMAGE_B_BYTES, sizeof(IMAGE_B_BYTES)) == VITA_OK);
    assert(vita_mem_copy_to_guest(
               IMAGE_C, IMAGE_C_BYTES, sizeof(IMAGE_C_BYTES)) == VITA_OK);
    vita_tls_layout layout;
    assert(vita_tls_prepare(
               templates, 3, g_tls_modules, 3, &layout) == VITA_OK);
    g_main_tls = 0;
    assert(vita_tls_materialize_current(&g_main_tls) == VITA_OK);
    assert_tls_image(g_main_tls);
    vita_runtime_shutdown();
}

static void copy_tls_images(void) {
    assert(vita_mem_copy_to_guest(
               IMAGE_A, IMAGE_A_BYTES, sizeof(IMAGE_A_BYTES)) == VITA_OK);
    assert(vita_mem_copy_to_guest(
               IMAGE_B, IMAGE_B_BYTES, sizeof(IMAGE_B_BYTES)) == VITA_OK);
    assert(vita_mem_copy_to_guest(
               IMAGE_C, IMAGE_C_BYTES, sizeof(IMAGE_C_BYTES)) == VITA_OK);
}

static void test_tls_arena_capacity_and_reinitialization(void) {
    const vita_tls_template templates[] = {
        {IMAGE_A, (uint32_t)sizeof(IMAGE_A_BYTES), 11u, 16u},
        {IMAGE_B, (uint32_t)sizeof(IMAGE_B_BYTES), 13u, 64u},
        {IMAGE_C, (uint32_t)sizeof(IMAGE_C_BYTES), 9u, 32u},
    };
    const uint32_t exact_usable_size = UINT32_C(0x8A0);
    const uint32_t allocation_size = UINT32_C(0x870);
    vita_runtime_config config = runtime_config();
    vita_tls_layout layout;

    config.tls_size = allocation_size - 16u;
    assert(vita_runtime_init(&config) == VITA_OK);
    copy_tls_images();
    assert(vita_tls_prepare(
               templates, 3, g_tls_modules, 3, &layout) ==
           VITA_ERROR_NO_MEMORY);
    vita_runtime_shutdown();

    config.tls_size = allocation_size;
    assert(vita_runtime_init(&config) == VITA_OK);
    copy_tls_images();
    assert(vita_tls_prepare(
               templates, 3, g_tls_modules, 3, &layout) ==
           VITA_ERROR_NO_MEMORY);
    vita_runtime_shutdown();

    config.tls_size = exact_usable_size - 16u;
    assert(vita_runtime_init(&config) == VITA_OK);
    copy_tls_images();
    assert(vita_tls_prepare(
               templates, 3, g_tls_modules, 3, &layout) ==
           VITA_ERROR_NO_MEMORY);
    vita_runtime_shutdown();

    config.tls_size = exact_usable_size;
    for (uint32_t iteration = 0; iteration < 3u; iteration++) {
        assert(vita_runtime_init(&config) == VITA_OK);
        copy_tls_images();
        assert(vita_tls_prepare(
                   templates, 3, g_tls_modules, 3, &layout) == VITA_OK);
        assert(layout.allocation_size == allocation_size);
        uint32_t tls_base = 0;
        assert(vita_tls_materialize_current(&tls_base) == VITA_OK);
        assert(tls_base != 0 && (tls_base & 63u) == 0);
        assert_tls_image(tls_base);
        assert(vita_tls_restart() == VITA_OK);
        assert(vita_tls_materialize_current(&tls_base) == VITA_OK);
        assert_tls_image(tls_base);
        vita_runtime_shutdown();
        vita_runtime_shutdown();
        assert(!vita_runtime_is_initialized());
    }
}

int main(void) {
    test_checked_stack_and_cpu_reset();
    test_tls_configuration();
    test_runtime_restart();
    test_tls_arena_capacity_and_reinitialization();
    puts("entry-state/TLS tests passed");
    return 0;
}
