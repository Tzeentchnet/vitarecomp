/* dispatch.c — guest address to recompiled function. */

#include "vitarecomp/dispatch.h"
#include "vitarecomp/recomp_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

static once_flag g_once = ONCE_FLAG_INIT;
static mtx_t g_lock;
static vita_dispatch_entry *g_table;
static uint32_t g_count;
static uint32_t g_hits, g_misses;

static void dispatch_once(void) {
    (void)mtx_init(&g_lock, mtx_plain);
}

vita_dispatch_status vita_dispatch_init_checked(
    const vita_dispatch_entry *table,
    uint32_t count) {
    vita_dispatch_entry *copy = NULL;
    if ((!table && count) || (table && !count))
        return VITA_DISPATCH_ERROR_NULL_COUNT;
    for (uint32_t i = 0; i < count; i++) {
        if (!table[i].fn) return VITA_DISPATCH_ERROR_NULL_FUNCTION;
        if (!i) continue;
        if (table[i - 1].addr == table[i].addr)
            return VITA_DISPATCH_ERROR_DUPLICATE;
        if (table[i - 1].addr > table[i].addr)
            return VITA_DISPATCH_ERROR_UNSORTED;
    }
    if (count) {
        if ((size_t)count > SIZE_MAX / sizeof(*copy))
            return VITA_DISPATCH_ERROR_NO_MEMORY;
        copy = (vita_dispatch_entry *)malloc((size_t)count * sizeof(*copy));
        if (!copy) return VITA_DISPATCH_ERROR_NO_MEMORY;
        memcpy(copy, table, (size_t)count * sizeof(*copy));
    }

    call_once(&g_once, dispatch_once);
    mtx_lock(&g_lock);
    free(g_table);
    g_table = copy;
    g_count = count;
    g_hits = 0;
    g_misses = 0;
    mtx_unlock(&g_lock);
    return VITA_DISPATCH_OK;
}

void vita_dispatch_init(const vita_dispatch_entry *table, uint32_t count) {
    /* ABI-v1 emitters materialized a one-entry dummy array for an empty table.
     * Normalize that historical representation for the strict checked API. */
    if (!count) table = NULL;
    vita_dispatch_status status = vita_dispatch_init_checked(table, count);
    if (status == VITA_DISPATCH_OK) return;
    (void)vita_dispatch_init_checked(NULL, 0);
    fprintf(stderr, "vitarecomp: dispatch table rejected: %s\n",
            vita_dispatch_status_string(status));
}

void vita_dispatch_reset(void) {
    (void)vita_dispatch_init_checked(NULL, 0);
}

uint32_t vita_dispatch_floor(uint32_t addr) {
    call_once(&g_once, dispatch_once);
    mtx_lock(&g_lock);
    uint32_t key = addr, lo = 0, hi = g_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].addr <= key) lo = mid + 1; else hi = mid;
    }
    uint32_t result = lo ? g_table[lo - 1].addr : 0;
    mtx_unlock(&g_lock);
    return result;
}

uint32_t vita_dispatch_hits(void) {
    uint32_t result;
    call_once(&g_once, dispatch_once);
    mtx_lock(&g_lock);
    result = g_hits;
    mtx_unlock(&g_lock);
    return result;
}

uint32_t vita_dispatch_misses(void) {
    uint32_t result;
    call_once(&g_once, dispatch_once);
    mtx_lock(&g_lock);
    result = g_misses;
    mtx_unlock(&g_lock);
    return result;
}

void vita_call(vita_fn fn) {
    fn();
}

void vita_dispatch(uint32_t from, uint32_t addr) {
    /* Bit 0 is the ARM/Thumb discriminator and therefore part of the key. */
    uint32_t key = addr;
    vita_fn function = NULL;

    call_once(&g_once, dispatch_once);
    mtx_lock(&g_lock);
    uint32_t lo = 0, hi = g_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].addr == key) {
            g_hits++;
            function = g_table[mid].fn;
            break;
        }
        if (g_table[mid].addr < key) lo = mid + 1; else hi = mid;
    }
    if (!function) g_misses++;
    mtx_unlock(&g_lock);

    if (function) {
        function();
        return;
    }
    /* A miss is not recoverable by returning. The caller expected the callee to
     * do something — set a return value, mutate memory, never come back — and
     * pretending it ran is how a recompiled program produces confident garbage.
     * vita_trap_indirect stops unless tracing is on. */
    vita_trap_indirect(from, addr);
}

const char *vita_dispatch_status_string(vita_dispatch_status status) {
    switch (status) {
    case VITA_DISPATCH_OK: return "ok";
    case VITA_DISPATCH_ERROR_NULL_COUNT:
        return "table/count mismatch";
    case VITA_DISPATCH_ERROR_NULL_FUNCTION:
        return "null dispatch function";
    case VITA_DISPATCH_ERROR_UNSORTED:
        return "dispatch keys are unsorted";
    case VITA_DISPATCH_ERROR_DUPLICATE:
        return "duplicate dispatch key";
    case VITA_DISPATCH_ERROR_NO_MEMORY:
        return "out of memory";
    default:
        return "unknown dispatch status";
    }
}
