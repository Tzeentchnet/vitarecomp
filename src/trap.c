/* trap.c — the gaps, announcing themselves.
 *
 * Every one of these is a place the recompiler knew it did not know something.
 * The default is to abort, because a program that continues past an
 * untranslated instruction produces wrong results with full confidence, and
 * that is far more expensive to debug than a stop with an address on it.
 *
 * VITARECOMP_TRACE=1 changes that to log-and-continue. It is a DISCOVERY AID,
 * not a correctness mode: past the first unimplemented import the machine state
 * is wrong, and every subsequent trap is reached through a path that would not
 * have happened on hardware. What it is good for is one run naming twenty
 * firmware functions instead of one, which is far better prioritisation than a
 * static count of what the import table contains. Anything it reports should be
 * treated as "the module wanted this" and not as "the module wants these, in
 * this order, this many times".
 */

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/runtime.h"
#include "runtime_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

void vita_guest_thread_call_reset(void);

static int   g_trace = -1;
static FILE *g_out;

/* Distinct traps already reported, so a loop does not bury the log. */
#define SEEN_SLOTS 4096
static uint32_t g_seen[SEEN_SLOTS];
static uint32_t g_hits;

struct vita_guest_boundary_coordinator {
    mtx_t lock;
    thrd_t owner;
    unsigned int references;
    int active;
    int has_result;
    vita_guest_call_result result;
};

typedef struct guest_boundary_context {
    int active;
    int owner;
    jmp_buf *jump;
    vita_guest_boundary_coordinator *coordinator;
} guest_boundary_context;

static VITA_TLS guest_boundary_context g_boundary;

static int trace_on(void) {
    if (g_trace < 0) {
        const char *e = getenv("VITARECOMP_TRACE");
        g_trace = (e && *e && *e != '0') ? 1 : 0;
        g_out = stderr;
    }
    return g_trace;
}

/* Report each distinct key once. Returns 1 the first time a key is seen. */
static int first_time(uint32_t key) {
    uint32_t h = (key * 2654435761u) & (SEEN_SLOTS - 1);
    for (uint32_t i = 0; i < SEEN_SLOTS; i++) {
        uint32_t s = (h + i) & (SEEN_SLOTS - 1);
        if (g_seen[s] == key) return 0;
        if (g_seen[s] == 0)   { g_seen[s] = key; return 1; }
    }
    return 1;   /* table full: report rather than lose it */
}

uint32_t vita_trap_count(void) { return g_hits; }

static void boundary_context_reset(void) {
    g_boundary.active = 0;
    g_boundary.owner = 0;
    g_boundary.jump = NULL;
    g_boundary.coordinator = NULL;
}

static vita_guest_boundary_coordinator *boundary_coordinator_create(void) {
    vita_guest_boundary_coordinator *coordinator =
        (vita_guest_boundary_coordinator *)calloc(1, sizeof(*coordinator));
    if (!coordinator)
        return NULL;
    if (mtx_init(&coordinator->lock, mtx_plain) != thrd_success) {
        free(coordinator);
        return NULL;
    }
    coordinator->owner = thrd_current();
    coordinator->references = 1;
    coordinator->active = 1;
    coordinator->result.outcome = VITA_GUEST_CALL_COMPLETED;
    return coordinator;
}

static int boundary_record(vita_guest_boundary_coordinator *coordinator,
                           const vita_guest_call_result *result) {
    int active;
    mtx_lock(&coordinator->lock);
    active = coordinator->active;
    if (active && !coordinator->has_result) {
        coordinator->result = *result;
        coordinator->has_result = 1;
    }
    mtx_unlock(&coordinator->lock);
    return active;
}

static void boundary_close(vita_guest_boundary_coordinator *coordinator,
                           vita_guest_call_result *result) {
    mtx_lock(&coordinator->lock);
    coordinator->active = 0;
    *result = coordinator->result;
    mtx_unlock(&coordinator->lock);
}

vita_guest_boundary_coordinator *
vita_guest_boundary_retain_current(void) {
    vita_guest_boundary_coordinator *coordinator;
    if (!g_boundary.active || !g_boundary.coordinator)
        return NULL;
    coordinator = g_boundary.coordinator;
    mtx_lock(&coordinator->lock);
    if (!coordinator->active) {
        mtx_unlock(&coordinator->lock);
        return NULL;
    }
    coordinator->references++;
    mtx_unlock(&coordinator->lock);
    return coordinator;
}

void vita_guest_boundary_release(
    vita_guest_boundary_coordinator *coordinator) {
    int destroy = 0;
    if (!coordinator)
        return;
    mtx_lock(&coordinator->lock);
    if (--coordinator->references == 0)
        destroy = 1;
    mtx_unlock(&coordinator->lock);
    if (destroy) {
        mtx_destroy(&coordinator->lock);
        free(coordinator);
    }
}

int vita_guest_boundary_run_worker(
    vita_guest_boundary_coordinator *coordinator,
    vita_guest_worker_entry_fn entry, void *userdata) {
    jmp_buf jump;
    int active;

    if (!entry)
        return 0;
    if (!coordinator) {
        entry(userdata);
        return 0;
    }

    mtx_lock(&coordinator->lock);
    active = coordinator->active;
    mtx_unlock(&coordinator->lock);
    if (!active) {
        entry(userdata);
        return 0;
    }

    g_boundary.active = 1;
    g_boundary.owner = 0;
    g_boundary.jump = &jump;
    g_boundary.coordinator = coordinator;
    if (setjmp(jump) == 0) {
        entry(userdata);
        boundary_context_reset();
        return 0;
    }
    boundary_context_reset();
    return 1;
}

void vita_guest_boundary_poll(void) {
    vita_guest_boundary_coordinator *coordinator;
    int pending;
    if (!g_boundary.active || !g_boundary.owner ||
        !g_boundary.coordinator)
        return;
    coordinator = g_boundary.coordinator;
    if (!thrd_equal(coordinator->owner, thrd_current()))
        return;
    mtx_lock(&coordinator->lock);
    pending = coordinator->active && coordinator->has_result;
    mtx_unlock(&coordinator->lock);
    if (pending)
        longjmp(*g_boundary.jump, 1);
}

static void boundary_fail(vita_strict_failure_kind kind,
                          uint32_t source_address, uint32_t raw_instruction,
                          uint32_t target_address, uint32_t nid,
                          const char *name) {
    vita_guest_call_result result;
    if (!g_boundary.active || !g_boundary.coordinator)
        return;
    memset(&result, 0, sizeof(result));
    result.outcome = VITA_GUEST_CALL_STRICT_FAILURE;
    result.failure.kind = kind;
    result.failure.source_address = source_address;
    result.failure.raw_instruction = raw_instruction;
    result.failure.target_address = target_address;
    result.failure.nid = nid;
    result.failure.name = name;
    if (boundary_record(g_boundary.coordinator, &result))
        longjmp(*g_boundary.jump, 1);
}

void vita_guest_boundary_note_exit_requested(int status) {
    vita_guest_call_result result;
    if (!g_boundary.active || !g_boundary.coordinator)
        return;
    memset(&result, 0, sizeof(result));
    result.outcome = VITA_GUEST_CALL_EXIT_REQUESTED;
    result.exit_status = status;
    (void)boundary_record(g_boundary.coordinator, &result);
}

void vita_guest_boundary_exit_requested(int status) {
    vita_guest_call_result result;
    if (!g_boundary.active || !g_boundary.coordinator)
        return;
    memset(&result, 0, sizeof(result));
    result.outcome = VITA_GUEST_CALL_EXIT_REQUESTED;
    result.exit_status = status;
    if (boundary_record(g_boundary.coordinator, &result))
        longjmp(*g_boundary.jump, 1);
}

void vita_guest_boundary_unresolved_variable(uint32_t address, uint32_t nid,
                                             const char *name) {
    g_hits++;
    boundary_fail(VITA_STRICT_FAILURE_UNRESOLVED_VARIABLE, 0, 0, address, nid,
                  name);
    if (!name)
        name = "(unresolved NID)";
    if (trace_on()) {
        if (first_time(nid ? nid : address))
            fprintf(g_out,
                    "TRACE variable %s NID 0x%08X access 0x%08X\n",
                    name, nid, address);
        return;
    }
    fprintf(stderr,
            "vitarecomp: unresolved variable %s"
            " accessed at 0x%08X (NID 0x%08X)\n",
            name, address, nid);
    abort();
}

int vita_guest_call(vita_guest_entry_fn entry, vita_guest_call_result *result) {
    jmp_buf jump;
    int exit_status = 0;
    vita_guest_boundary_coordinator *coordinator;
    vita_guest_call_result exit_result;

    if (!entry || !result)
        return VITA_ERROR_INVALID_ARGUMENT;
    if (g_boundary.active)
        return VITA_ERROR_BUSY;

    boundary_context_reset();
    memset(result, 0, sizeof(*result));
    if (vita_runtime_exit_requested(&exit_status)) {
        result->outcome = VITA_GUEST_CALL_EXIT_REQUESTED;
        result->exit_status = exit_status;
        return VITA_OK;
    }

    coordinator = boundary_coordinator_create();
    if (!coordinator)
        return VITA_ERROR_NO_MEMORY;
    g_boundary.active = 1;
    g_boundary.owner = 1;
    g_boundary.jump = &jump;
    g_boundary.coordinator = coordinator;
    if (setjmp(jump) == 0) {
        entry();
        if (vita_runtime_exit_requested(&exit_status)) {
            memset(&exit_result, 0, sizeof(exit_result));
            exit_result.outcome = VITA_GUEST_CALL_EXIT_REQUESTED;
            exit_result.exit_status = exit_status;
            (void)boundary_record(coordinator, &exit_result);
        }
    }

    vita_guest_thread_call_reset();
    boundary_close(coordinator, result);
    boundary_context_reset();
    vita_guest_boundary_release(coordinator);
    return VITA_OK;
}

void vita_trap_unimpl(uint32_t addr, uint32_t raw, const char *what) {
    g_hits++;
    boundary_fail(VITA_STRICT_FAILURE_UNTRANSLATED_INSTRUCTION, addr, raw, 0, 0,
                  what);
    if (trace_on()) {
        if (first_time(addr))
            fprintf(g_out, "TRACE unimpl  0x%08X  raw 0x%08X  %s\n", addr, raw,
                    what ? what : "?");
        return;
    }
    fprintf(stderr,
            "vitarecomp: untranslated instruction at 0x%08X (raw 0x%08X): %s\n",
            addr, raw, what ? what : "?");
    abort();
}

void vita_trap_indirect(uint32_t addr, uint32_t target) {
    g_hits++;
    boundary_fail(VITA_STRICT_FAILURE_UNRESOLVED_TRANSFER, addr, 0, target, 0,
                  NULL);
    if (trace_on()) {
        if (first_time(addr))
            fprintf(g_out, "TRACE indirect 0x%08X -> 0x%08X\n", addr, target);
        return;
    }
    fprintf(stderr,
            "vitarecomp: unresolved indirect transfer at 0x%08X -> 0x%08X\n",
            addr, target);
    abort();
}

/* `name` is the resolved "Library::function", or NULL when the NID database did
 * not know it. Naming the function is the entire point of binding imports — a
 * NID is a hash, and "implement 0xBFE02B3A" is not a task anyone can start. */
void vita_trap_import(uint32_t addr, uint32_t nid, const char *name) {
    g_hits++;
    boundary_fail(VITA_STRICT_FAILURE_UNIMPLEMENTED_IMPORT, addr, 0, 0, nid,
                  name);
    if (!name)
        name = "(unresolved NID)";
    if (trace_on()) {
        /* Keyed on the NID, not the call site: the interesting question is
         * which firmware function is wanted, and the same one called from
         * three places is one thing to implement, not three. */
        if (first_time(nid ? nid : addr))
            fprintf(g_out, "TRACE import  %s  NID 0x%08X  (stub 0x%08X)\n",
                    name, nid, addr);
        return;
    }
    fprintf(stderr,
            "vitarecomp: unimplemented firmware import %s"
            " at 0x%08X (NID 0x%08X)\n",
            name, addr, nid);
    abort();
}
