/* SPDX-License-Identifier: MIT */
#ifndef VITARECOMP_RUNTIME_H
#define VITARECOMP_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vita_result {
    VITA_OK = 0,
    VITA_ERROR_INVALID_ARGUMENT = -1,
    VITA_ERROR_NOT_INITIALIZED = -2,
    VITA_ERROR_ALREADY_INITIALIZED = -3,
    VITA_ERROR_NO_MEMORY = -4,
    VITA_ERROR_OUT_OF_BOUNDS = -5,
    VITA_ERROR_OVERFLOW = -6,
    VITA_ERROR_NOT_FOUND = -7,
    VITA_ERROR_READ_ONLY = -8,
    VITA_ERROR_TRAVERSAL = -9,
    VITA_ERROR_UNIMPLEMENTED = -10,
    VITA_ERROR_BUSY = -11,
    VITA_ERROR_IO = -12,
    VITA_ERROR_SHUTDOWN = -13,
    VITA_ERROR_LIMIT = -14,
    VITA_ERROR_NOT_TERMINATED = -15
} vita_result;

typedef enum vita_diag_severity {
    VITA_DIAG_TRACE,
    VITA_DIAG_INFO,
    VITA_DIAG_WARNING,
    VITA_DIAG_ERROR,
    VITA_DIAG_FATAL
} vita_diag_severity;

typedef enum vita_diag_code {
    VITA_DIAG_RUNTIME,
    VITA_DIAG_MEMORY,
    VITA_DIAG_IMPORT,
    VITA_DIAG_THREAD,
    VITA_DIAG_SYNC,
    VITA_DIAG_VFS,
    VITA_DIAG_PSARC,
    VITA_DIAG_SHUTDOWN
} vita_diag_code;

typedef struct vita_diagnostic {
    vita_diag_severity severity;
    vita_diag_code code;
    int32_t status;
    uint32_t guest_address;
    uint32_t library_nid;
    uint32_t function_nid;
    const char *library_name;
    const char *function_name;
    const char *message;
} vita_diagnostic;

typedef void (*vita_diagnostic_fn)(const vita_diagnostic *event, void *userdata);
typedef void (*vita_exit_fn)(int status, void *userdata);

typedef struct vita_runtime_config {
    uint32_t memory_base;
    size_t memory_size;

    uint32_t heap_base;
    uint32_t heap_size;
    uint32_t sysmem_base;
    uint32_t sysmem_size;
    uint32_t kernel_base;
    uint32_t kernel_size;

    int virtual_clock;
    uint64_t virtual_epoch_us;
    uint64_t virtual_read_step_us;

    vita_diagnostic_fn diagnostic;
    void *diagnostic_userdata;
    vita_exit_fn exit_requested;
    void *exit_userdata;
} vita_runtime_config;

typedef struct vita_runtime_counters {
    uint64_t memory_bad_accesses;
    uint64_t imports_dispatched;
    uint64_t imports_unimplemented;
    uint64_t threads_created;
    uint64_t threads_finished;
    uint64_t callbacks_run;
    uint64_t files_opened;
    uint64_t files_closed;
    uint64_t diagnostics_emitted;
    uint64_t shutdowns;
    uint32_t live_threads;
    uint32_t live_files;
    uint32_t live_sync_objects;
    uint32_t live_callbacks;
} vita_runtime_counters;

int vita_runtime_init(const vita_runtime_config *config);
void vita_runtime_shutdown(void);
int vita_runtime_is_initialized(void);
void vita_runtime_get_counters(vita_runtime_counters *out);

void vita_runtime_emit(const vita_diagnostic *event);
void vita_runtime_request_exit(int status);
int vita_runtime_exit_requested(int *status);

void vita_clock_configure(int virtual_clock, uint64_t epoch_us,
                          uint64_t read_step_us);
void vita_clock_reset(void);
uint64_t vita_clock_us(void);
uint64_t vita_clock_unix_us(void);
void vita_clock_wait(uint64_t us);
void vita_clock_advance(uint64_t us);
void vita_clock_vblank(void);
int vita_clock_is_virtual(void);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_RUNTIME_H */
