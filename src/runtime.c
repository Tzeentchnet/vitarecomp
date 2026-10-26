/* SPDX-License-Identifier: MIT */
#include "vitarecomp/runtime.h"

#include "vitarecomp/cpu.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/vfs.h"
#include "runtime_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

void vita_fios_shutdown(void);
void vita_io_shutdown(void);
void vita_threadmgr_shutdown(void);
void vita_sched_quiesce(void);
void vita_sched_shutdown(void);
void vita_mspace_shutdown(void);
void vita_sysmem_shutdown(void);
void vita_hle_registry_reset(void);
void vita_libc_finalize(uint32_t dso);
void vita_libc_reset(void);
vita_result vita_tls_arena_init(uint32_t base, uint32_t size);

typedef struct runtime_state {
    mtx_t lock;
    int initialized;
    int initializing;
    int shutting_down;
    int exit_pending;
    int exit_status;
    vita_runtime_config config;
    vita_runtime_counters counters;
} runtime_state;

static once_flag g_once = ONCE_FLAG_INIT;
static runtime_state g_runtime;

static void runtime_once(void) {
    (void)mtx_init(&g_runtime.lock, mtx_plain);
}

static int range_inside(uint32_t outer_base, size_t outer_size,
                        uint32_t base, uint32_t size) {
    const uint64_t outer_end = (uint64_t)outer_base + outer_size;
    const uint64_t end = (uint64_t)base + size;
    return size == 0 || (base >= outer_base && end >= base && end <= outer_end);
}

static int ranges_overlap(uint32_t a, uint32_t as, uint32_t b, uint32_t bs) {
    if (!as || !bs) return 0;
    return (uint64_t)a < (uint64_t)b + bs && (uint64_t)b < (uint64_t)a + as;
}

int vita_runtime_init(const vita_runtime_config *config) {
    if (!config || !config->memory_size ||
        config->memory_size >
            UINT64_C(0x100000000) - config->memory_base)
        return VITA_ERROR_INVALID_ARGUMENT;
    if (!range_inside(config->memory_base, config->memory_size,
                      config->heap_base, config->heap_size) ||
        !range_inside(config->memory_base, config->memory_size,
                      config->sysmem_base, config->sysmem_size) ||
        !range_inside(config->memory_base, config->memory_size,
                      config->kernel_base, config->kernel_size) ||
        !range_inside(config->memory_base, config->memory_size,
                      config->tls_base, config->tls_size))
        return VITA_ERROR_OUT_OF_BOUNDS;
    if (((config->tls_size == 0u) != (config->tls_base == 0u)) ||
        (config->tls_size &&
         (((config->tls_base | config->tls_size) & 15u) != 0)))
        return VITA_ERROR_INVALID_ARGUMENT;
    if (ranges_overlap(config->heap_base, config->heap_size,
                       config->sysmem_base, config->sysmem_size) ||
        ranges_overlap(config->heap_base, config->heap_size,
                       config->kernel_base, config->kernel_size) ||
        ranges_overlap(config->heap_base, config->heap_size,
                       config->tls_base, config->tls_size) ||
        ranges_overlap(config->sysmem_base, config->sysmem_size,
                       config->kernel_base, config->kernel_size) ||
        ranges_overlap(config->sysmem_base, config->sysmem_size,
                       config->tls_base, config->tls_size) ||
        ranges_overlap(config->kernel_base, config->kernel_size,
                       config->tls_base, config->tls_size))
        return VITA_ERROR_INVALID_ARGUMENT;

    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    if (g_runtime.initialized || g_runtime.initializing ||
        g_runtime.shutting_down) {
        mtx_unlock(&g_runtime.lock);
        return VITA_ERROR_ALREADY_INITIALIZED;
    }
    g_runtime.initializing = 1;
    memset(&g_runtime.config, 0, sizeof(g_runtime.config));
    g_runtime.config = *config;
    memset(&g_runtime.counters, 0, sizeof(g_runtime.counters));
    g_runtime.exit_pending = 0;
    g_runtime.exit_status = 0;
    g_runtime.shutting_down = 0;
    mtx_unlock(&g_runtime.lock);

    vita_libc_reset();
    vita_cpu_reset();
    if (vita_mem_init(config->memory_base, config->memory_size) != 0) {
        mtx_lock(&g_runtime.lock);
        g_runtime.initializing = 0;
        mtx_unlock(&g_runtime.lock);
        return VITA_ERROR_NO_MEMORY;
    }
    const vita_result tls_result =
        vita_tls_arena_init(config->tls_base, config->tls_size);
    if (tls_result != VITA_OK) {
        vita_mem_free();
        mtx_lock(&g_runtime.lock);
        g_runtime.initializing = 0;
        mtx_unlock(&g_runtime.lock);
        return tls_result;
    }
    if (config->heap_size) vita_heap_init(config->heap_base, config->heap_size);
    if (config->sysmem_size)
        vita_memblock_init(config->sysmem_base, config->sysmem_size);
    if (config->kernel_size)
        vita_kernel_init(config->kernel_base, config->kernel_size);
    vita_clock_configure(config->virtual_clock, config->virtual_epoch_us,
                         config->virtual_read_step_us);

    mtx_lock(&g_runtime.lock);
    g_runtime.initialized = 1;
    g_runtime.initializing = 0;
    mtx_unlock(&g_runtime.lock);

    vita_diagnostic event = {
        VITA_DIAG_INFO, VITA_DIAG_RUNTIME, VITA_OK, 0, 0, 0,
        NULL, NULL, "runtime initialized"
    };
    vita_runtime_emit(&event);
    return VITA_OK;
}

void vita_runtime_shutdown(void) {
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    if (!g_runtime.initialized || g_runtime.shutting_down) {
        mtx_unlock(&g_runtime.lock);
        return;
    }
    g_runtime.shutting_down = 1;
    mtx_unlock(&g_runtime.lock);

    vita_sched_quiesce();
    vita_libc_finalize(0);
    vita_fios_shutdown();
    vita_io_shutdown();
    vita_vfs_unmount_all();
    vita_sched_shutdown();
    vita_threadmgr_shutdown();
    vita_mspace_shutdown();
    vita_sysmem_shutdown();
    vita_heap_shutdown();
    vita_libc_reset();
    vita_binding_runtime_reset();
    vita_hle_registry_reset();
    vita_mem_free();
    vita_clock_reset();
    vita_cpu_reset();

    mtx_lock(&g_runtime.lock);
    g_runtime.counters.shutdowns++;
    g_runtime.initialized = 0;
    g_runtime.shutting_down = 0;
    mtx_unlock(&g_runtime.lock);

    vita_diagnostic event = {
        VITA_DIAG_INFO, VITA_DIAG_SHUTDOWN, VITA_OK, 0, 0, 0,
        NULL, NULL, "runtime shutdown complete"
    };
    vita_runtime_emit(&event);
}

int vita_runtime_is_initialized(void) {
    int initialized;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    initialized = g_runtime.initialized;
    mtx_unlock(&g_runtime.lock);
    return initialized;
}

int vita_runtime_can_bind(void) {
    int ready;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    ready = g_runtime.initialized && !g_runtime.initializing &&
            !g_runtime.shutting_down;
    mtx_unlock(&g_runtime.lock);
    return ready;
}

void vita_runtime_get_counters(vita_runtime_counters *out) {
    if (!out) return;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    *out = g_runtime.counters;
    out->memory_bad_accesses = vita_mem_bad_access_count();
    mtx_unlock(&g_runtime.lock);
}

void vita_runtime_emit(const vita_diagnostic *event) {
    vita_diagnostic_fn callback = NULL;
    void *userdata = NULL;
    if (!event) return;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    g_runtime.counters.diagnostics_emitted++;
    callback = g_runtime.config.diagnostic;
    userdata = g_runtime.config.diagnostic_userdata;
    mtx_unlock(&g_runtime.lock);
    if (callback) callback(event, userdata);
}

void vita_runtime_count(vita_counter_id counter, int64_t amount) {
    uint64_t *wide = NULL;
    uint32_t *live = NULL;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    switch (counter) {
    case VITA_COUNTER_MEMORY_BAD: wide = &g_runtime.counters.memory_bad_accesses; break;
    case VITA_COUNTER_IMPORT_DISPATCHED: wide = &g_runtime.counters.imports_dispatched; break;
    case VITA_COUNTER_IMPORT_UNIMPLEMENTED: wide = &g_runtime.counters.imports_unimplemented; break;
    case VITA_COUNTER_THREAD_CREATED:
        wide = &g_runtime.counters.threads_created;
        live = &g_runtime.counters.live_threads;
        break;
    case VITA_COUNTER_THREAD_FINISHED:
        wide = &g_runtime.counters.threads_finished;
        live = &g_runtime.counters.live_threads;
        amount = -amount;
        break;
    case VITA_COUNTER_CALLBACK_RUN: wide = &g_runtime.counters.callbacks_run; break;
    case VITA_COUNTER_FILE_OPENED:
        wide = &g_runtime.counters.files_opened;
        live = &g_runtime.counters.live_files;
        break;
    case VITA_COUNTER_FILE_CLOSED:
        wide = &g_runtime.counters.files_closed;
        live = &g_runtime.counters.live_files;
        amount = -amount;
        break;
    case VITA_COUNTER_SYNC_CREATED: live = &g_runtime.counters.live_sync_objects; break;
    case VITA_COUNTER_SYNC_DESTROYED:
        live = &g_runtime.counters.live_sync_objects;
        amount = -amount;
        break;
    case VITA_COUNTER_CALLBACK_CREATED: live = &g_runtime.counters.live_callbacks; break;
    case VITA_COUNTER_CALLBACK_DESTROYED:
        live = &g_runtime.counters.live_callbacks;
        amount = -amount;
        break;
    }
    if (wide) {
        uint64_t increment = amount < 0
            ? (uint64_t)(-(amount + 1)) + 1u : (uint64_t)amount;
        *wide = increment > UINT64_MAX - *wide
              ? UINT64_MAX : *wide + increment;
    }
    if (live) {
        if (amount >= 0) *live += (uint32_t)amount;
        else {
            uint64_t magnitude = (uint64_t)(-(amount + 1)) + 1u;
            uint32_t n = magnitude > UINT32_MAX
                       ? UINT32_MAX : (uint32_t)magnitude;
            *live = *live > n ? *live - n : 0;
        }
    }
    mtx_unlock(&g_runtime.lock);
}

void vita_runtime_request_exit(int status) {
    vita_exit_fn callback;
    void *userdata;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    g_runtime.exit_pending = 1;
    g_runtime.exit_status = status;
    callback = g_runtime.config.exit_requested;
    userdata = g_runtime.config.exit_userdata;
    mtx_unlock(&g_runtime.lock);
    vita_guest_boundary_note_exit_requested(status);
    if (callback) callback(status, userdata);
    vita_guest_boundary_exit_requested(status);
}

int vita_runtime_exit_requested(int *status) {
    int pending;
    call_once(&g_once, runtime_once);
    mtx_lock(&g_runtime.lock);
    pending = g_runtime.exit_pending;
    if (pending && status) *status = g_runtime.exit_status;
    mtx_unlock(&g_runtime.lock);
    return pending;
}

void vita_host_exit(int status) {
    vita_runtime_request_exit(status);
}

void vita_hle_trace(const char *fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(message, sizeof(message), fmt ? fmt : "", args);
    va_end(args);
    vita_diagnostic event = {
        VITA_DIAG_TRACE, VITA_DIAG_RUNTIME, VITA_OK, 0, 0, 0,
        NULL, NULL, message
    };
    vita_runtime_emit(&event);
}
