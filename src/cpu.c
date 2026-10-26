/* cpu.c — the machine state the generated C operates on. */

#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

VITA_TLS uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
VITA_TLS uint32_t r8, r9, r10, r11, r12;
VITA_TLS uint32_t sp, lr;

VITA_TLS int flag_n, flag_z, flag_c, flag_v;
VITA_TLS int flag_ge;

VITA_TLS uint32_t vita_last_pc;
VITA_TLS int vita_trace_insns;
VITA_TLS uint32_t vita_trace_lo, vita_trace_hi = 0xFFFFFFFFu;

VITA_TLS int32_t vita_slice = VITA_SLICE;

static vita_slice_hook g_slice_hook;
static vita_spin_hook g_spin_hook;
static vita_tls_hook g_tls_hook;

#define FUNCTION_PROFILE_TABLE_SIZE 16384u

typedef struct function_profile_slot {
    vita_function_profile_entry entry;
    int occupied;
} function_profile_slot;

static once_flag g_profile_once = ONCE_FLAG_INIT;
static mtx_t g_profile_lock;
static int g_profile_ready;
#if defined(_MSC_VER)
static volatile long g_profile_enabled;
#else
static int g_profile_enabled;
#endif
static function_profile_slot g_profile_slots[FUNCTION_PROFILE_TABLE_SIZE];
static uint32_t g_profile_slot_count;
static uint64_t g_profile_sequence;
static uint64_t g_profile_dropped_events;
static uint32_t g_profile_recent_addresses[VITA_FUNCTION_PROFILE_RECENT_LIMIT];
static uint32_t g_profile_recent_head;
static uint32_t g_profile_recent_count;
static vita_function_profile_trap g_profile_first_trap;

void vita_sched_slice_end_impl(void);
void vita_sched_spin_wait_impl(uint32_t addr);
uint32_t vita_thread_tls_base_impl(void);

static void function_profile_initialize(void) {
    if (mtx_init(&g_profile_lock, mtx_plain) == thrd_success)
        g_profile_ready = 1;
}

static void function_profile_store_enabled(int enabled) {
#if defined(_MSC_VER)
    (void)_InterlockedExchange(&g_profile_enabled, enabled ? 1L : 0L);
#else
    __atomic_store_n(&g_profile_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
#endif
}

static int function_profile_load_enabled(void) {
#if defined(_MSC_VER)
    return _InterlockedCompareExchange(&g_profile_enabled, 0L, 0L) != 0L;
#else
    return __atomic_load_n(&g_profile_enabled, __ATOMIC_ACQUIRE);
#endif
}

static function_profile_slot *function_profile_find(uint32_t address,
                                                    int create) {
    const uint32_t hash =
        (address * UINT32_C(2654435761)) & (FUNCTION_PROFILE_TABLE_SIZE - 1u);
    for (uint32_t probe = 0u; probe < FUNCTION_PROFILE_TABLE_SIZE; probe++) {
        function_profile_slot *slot =
            &g_profile_slots[(hash + probe) &
                             (FUNCTION_PROFILE_TABLE_SIZE - 1u)];
        if (slot->occupied) {
            if (slot->entry.address == address)
                return slot;
            continue;
        }
        if (!create)
            return NULL;
        slot->occupied = 1;
        slot->entry.address = address;
        slot->entry.first_trap_distance = UINT32_MAX;
        g_profile_slot_count++;
        return slot;
    }
    return NULL;
}

void vita_function_profile_set_enabled(int enabled) {
    call_once(&g_profile_once, function_profile_initialize);
    function_profile_store_enabled(enabled && g_profile_ready);
}

int vita_function_profile_is_enabled(void) {
    return function_profile_load_enabled();
}

void vita_function_profile_reset(void) {
    call_once(&g_profile_once, function_profile_initialize);
    if (!g_profile_ready)
        return;
    mtx_lock(&g_profile_lock);
    memset(g_profile_slots, 0, sizeof(g_profile_slots));
    memset(g_profile_recent_addresses, 0, sizeof(g_profile_recent_addresses));
    memset(&g_profile_first_trap, 0, sizeof(g_profile_first_trap));
    g_profile_slot_count = 0u;
    g_profile_sequence = 0u;
    g_profile_dropped_events = 0u;
    g_profile_recent_head = 0u;
    g_profile_recent_count = 0u;
    mtx_unlock(&g_profile_lock);
}

static int compare_profile_entries(const void *left_pointer,
                                   const void *right_pointer) {
    const vita_function_profile_entry *left =
        (const vita_function_profile_entry *)left_pointer;
    const vita_function_profile_entry *right =
        (const vita_function_profile_entry *)right_pointer;
    const int left_has_distance = left->first_trap_distance != UINT32_MAX;
    const int right_has_distance = right->first_trap_distance != UINT32_MAX;
    if (left_has_distance != right_has_distance)
        return left_has_distance ? -1 : 1;
    if (left_has_distance &&
        left->first_trap_distance != right->first_trap_distance)
        return left->first_trap_distance < right->first_trap_distance ? -1 : 1;
    if (left->hit_count != right->hit_count)
        return left->hit_count > right->hit_count ? -1 : 1;
    if (left->first_sequence != right->first_sequence)
        return left->first_sequence < right->first_sequence ? -1 : 1;
    if (left->address == right->address)
        return 0;
    return left->address < right->address ? -1 : 1;
}

vita_result
vita_function_profile_snapshot(vita_function_profile_entry *entries,
                               size_t entry_capacity, size_t *entry_count,
                               vita_function_profile_summary *summary) {
    size_t copied = 0u;

    if (!entry_count || !summary || (!entries && entry_capacity != 0u))
        return VITA_ERROR_INVALID_ARGUMENT;
    call_once(&g_profile_once, function_profile_initialize);
    if (!g_profile_ready)
        return VITA_ERROR_NO_MEMORY;

    mtx_lock(&g_profile_lock);
    *entry_count = g_profile_slot_count;
    summary->event_count = g_profile_sequence;
    summary->dropped_events = g_profile_dropped_events;
    summary->tracked_function_count = g_profile_slot_count;
    summary->first_trap = g_profile_first_trap;
    if (!entries && entry_capacity == 0u) {
        mtx_unlock(&g_profile_lock);
        return VITA_OK;
    }
    if (entry_capacity < g_profile_slot_count) {
        mtx_unlock(&g_profile_lock);
        return VITA_ERROR_LIMIT;
    }
    for (uint32_t index = 0u; index < FUNCTION_PROFILE_TABLE_SIZE; index++) {
        if (g_profile_slots[index].occupied)
            entries[copied++] = g_profile_slots[index].entry;
    }
    mtx_unlock(&g_profile_lock);
    qsort(entries, copied, sizeof(entries[0]), compare_profile_entries);
    return VITA_OK;
}

void vita_function_profile_note_strict_failure(vita_strict_failure_kind kind,
                                               uint32_t source_address,
                                               uint32_t raw_instruction,
                                               uint32_t target_address,
                                               uint32_t nid) {
    if (!vita_function_profile_is_enabled())
        return;
    call_once(&g_profile_once, function_profile_initialize);
    if (!g_profile_ready)
        return;

    mtx_lock(&g_profile_lock);
    if (!g_profile_first_trap.captured) {
        g_profile_first_trap.captured = 1u;
        g_profile_first_trap.kind = kind;
        g_profile_first_trap.source_address = source_address;
        g_profile_first_trap.raw_instruction = raw_instruction;
        g_profile_first_trap.target_address = target_address;
        g_profile_first_trap.nid = nid;
        g_profile_first_trap.sequence = g_profile_sequence;
        g_profile_first_trap.recent_count = g_profile_recent_count;
        for (uint32_t distance = 0u; distance < g_profile_recent_count;
             distance++) {
            const uint32_t ring_index =
                (g_profile_recent_head + VITA_FUNCTION_PROFILE_RECENT_LIMIT -
                 1u - distance) %
                VITA_FUNCTION_PROFILE_RECENT_LIMIT;
            const uint32_t address = g_profile_recent_addresses[ring_index];
            function_profile_slot *slot = function_profile_find(address, 0);
            g_profile_first_trap.recent_addresses[distance] = address;
            if (slot && slot->entry.first_trap_distance > distance)
                slot->entry.first_trap_distance = distance;
        }
    }
    mtx_unlock(&g_profile_lock);
}

void vita_cpu_reset(void) {
    r0 = r1 = r2 = r3 = r4 = r5 = r6 = r7 = 0;
    r8 = r9 = r10 = r11 = r12 = sp = lr = 0;
    flag_n = flag_z = flag_c = flag_v = flag_ge = 0;
    memset(vfp_s, 0, sizeof(vfp_s));
    vfp_set_fpscr(0);
    vita_last_pc = 0;
    vita_trace_insns = 0;
    vita_trace_lo = 0;
    vita_trace_hi = 0;
    vita_slice = VITA_SLICE;
    vita_clrex();
}

vita_result
vita_cpu_prepare_module_start(const vita_module_start_state *state) {
    if (!state)
        return VITA_ERROR_INVALID_ARGUMENT;
    if (!vita_runtime_is_initialized())
        return VITA_ERROR_NOT_INITIALIZED;
    if ((state->stack_pointer & 15u) != 0)
        return VITA_ERROR_INVALID_ARGUMENT;
    if (state->argument_size) {
        void *arguments = NULL;
        const uint64_t end =
            (uint64_t)state->argument_pointer + state->argument_size;
        if (end > UINT64_C(0x100000000))
            return VITA_ERROR_OVERFLOW;
        if (vita_mem_range(state->argument_pointer, state->argument_size,
                           &arguments) != VITA_OK)
            return VITA_ERROR_OUT_OF_BOUNDS;
        (void)arguments;
    }
    vita_result result =
        vita_sched_validate_main_stack_pointer(state->stack_pointer);
    if (result != VITA_OK)
        return result;
    result = vita_sched_prepare_main_execution();
    if (result != VITA_OK)
        return result;

    vita_cpu_reset();
    r0 = state->argument_size;
    r1 = state->argument_pointer;
    sp = state->stack_pointer;
    lr = state->return_address;
    return VITA_OK;
}

void vita_runtime_set_hooks(vita_slice_hook slice_hook,
                            vita_spin_hook spin_hook, vita_tls_hook tls_hook) {
    g_slice_hook = slice_hook;
    g_spin_hook = spin_hook;
    g_tls_hook = tls_hook;
}

void vita_slice_end(void) {
    if (g_slice_hook) {
        vita_slice = VITA_SLICE;
        g_slice_hook();
        return;
    }
    vita_sched_slice_end_impl();
}

void vita_spin_wait(uint32_t addr) {
    if (g_spin_hook) {
        g_spin_hook(addr);
        return;
    }
    vita_sched_spin_wait_impl(addr);
}

uint32_t vita_tls_base(void) {
    if (g_tls_hook)
        return g_tls_hook();
    return vita_thread_tls_base_impl();
}

void vita_trace_fn(uint32_t addr) {
    function_profile_slot *slot;
    if (!vita_function_profile_is_enabled())
        return;
    call_once(&g_profile_once, function_profile_initialize);
    if (!g_profile_ready)
        return;

    mtx_lock(&g_profile_lock);
    if (g_profile_sequence != UINT64_MAX)
        g_profile_sequence++;
    slot = function_profile_find(addr, 1);
    if (slot) {
        if (slot->entry.hit_count == 0u)
            slot->entry.first_sequence = g_profile_sequence;
        if (slot->entry.hit_count != UINT64_MAX)
            slot->entry.hit_count++;
        slot->entry.last_sequence = g_profile_sequence;
    } else if (g_profile_dropped_events != UINT64_MAX) {
        g_profile_dropped_events++;
    }
    g_profile_recent_addresses[g_profile_recent_head] = addr;
    g_profile_recent_head =
        (g_profile_recent_head + 1u) % VITA_FUNCTION_PROFILE_RECENT_LIMIT;
    if (g_profile_recent_count < VITA_FUNCTION_PROFILE_RECENT_LIMIT)
        g_profile_recent_count++;
    mtx_unlock(&g_profile_lock);
}

void vita_trace_insn(uint32_t addr) {
    fprintf(stderr,
            "TRACE pc  0x%08X  r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X "
            "r6=%08X r7=%08X"
            " r8=%08X ip=%08X sp=%08X lr=%08X %c%c%c%c\n",
            addr, r0, r1, r2, r3, r4, r5, r6, r7, r8, r12, sp, lr,
            flag_n ? 'N' : 'n', flag_z ? 'Z' : 'z', flag_c ? 'C' : 'c',
            flag_v ? 'V' : 'v');
}
