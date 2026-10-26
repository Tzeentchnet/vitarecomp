/* SPDX-License-Identifier: MIT */
#ifndef VITARECOMP_RUNTIME_INTERNAL_H
#define VITARECOMP_RUNTIME_INTERNAL_H

#include "vitarecomp/runtime.h"

typedef struct vita_guest_boundary_coordinator
    vita_guest_boundary_coordinator;
typedef void (*vita_guest_worker_entry_fn)(void *userdata);

typedef enum vita_counter_id {
    VITA_COUNTER_MEMORY_BAD,
    VITA_COUNTER_IMPORT_DISPATCHED,
    VITA_COUNTER_IMPORT_UNIMPLEMENTED,
    VITA_COUNTER_THREAD_CREATED,
    VITA_COUNTER_THREAD_FINISHED,
    VITA_COUNTER_CALLBACK_RUN,
    VITA_COUNTER_FILE_OPENED,
    VITA_COUNTER_FILE_CLOSED,
    VITA_COUNTER_SYNC_CREATED,
    VITA_COUNTER_SYNC_DESTROYED,
    VITA_COUNTER_CALLBACK_CREATED,
    VITA_COUNTER_CALLBACK_DESTROYED
} vita_counter_id;

void vita_runtime_count(vita_counter_id counter, int64_t amount);
void vita_guest_boundary_note_exit_requested(int status);
void vita_guest_boundary_exit_requested(int status);
void vita_guest_boundary_unresolved_variable(uint32_t address, uint32_t nid,
                                             const char *name);
vita_guest_boundary_coordinator *
vita_guest_boundary_retain_current(void);
void vita_guest_boundary_release(
    vita_guest_boundary_coordinator *coordinator);
int vita_guest_boundary_run_worker(
    vita_guest_boundary_coordinator *coordinator,
    vita_guest_worker_entry_fn entry, void *userdata);
void vita_guest_boundary_poll(void);
int vita_runtime_can_bind(void);
void vita_binding_runtime_reset(void);

#endif /* VITARECOMP_RUNTIME_INTERNAL_H */
