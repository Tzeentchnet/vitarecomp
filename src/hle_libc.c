/* hle_libc.c — SceLibc, forwarded to the host C library.
 *
 * These are the easy ones, and they are easy for a specific reason: SceLibc is
 * the standard C library, so the semantics are already specified and already
 * implemented on the host. What is NOT free is the boundary — every pointer
 * argument is a guest address into a flat allocation, not a host pointer, and
 * every one has to be bounds-checked before it is touched. A forwarded
 * `memcpy` that skips that step is a guest-controlled write anywhere in the
 * host process.
 *
 * See hle.h for the calling convention and the link-order requirement.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/runtime.h"
#include "sched.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

/* --- the guest/host boundary ------------------------------------------------ */

/* Translate a guest pointer, or trap. Returning NULL and continuing is not an
 * option here: the caller asked for a string to be copied, and a copy that did
 * not happen leaves the program running with the work undone — the same reason
 * a dispatch miss stops rather than returns. */
static void *gp(uint32_t addr, uint32_t len, const char *who) {
    if (len == 0) return NULL;              /* a zero-length op touches nothing */
    void *p = vita_mem_ptr(addr, len);
    if (!p) vita_trap_unimpl(addr, len, who);
    return p;
}

/* The length of a guest C string, bounded by what is actually backed.
 *
 * A host strlen on guest memory runs off the end of the allocation the moment
 * the guest passes something that is not a string, and it does so reading host
 * memory it has no business in. Scanning within the region turns that into a
 * named trap. */
static uint32_t gstrlen(uint32_t addr, const char *who) {
    uint32_t avail = vita_mem_avail(addr);
    if (avail == 0) { vita_trap_unimpl(addr, 0, who); return 0; }
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(addr, 1);
    if (!p) { vita_trap_unimpl(addr, 0, who); return 0; }
    const void *nul = memchr(p, 0, avail);
    if (!nul) { vita_trap_unimpl(addr, avail, who); return avail; }
    return (uint32_t)((const uint8_t *)nul - p);
}

/* --- C++ runtime and CRT init ------------------------------------------------ */

typedef struct atexit_record {
    uint32_t function;
    uint32_t argument;
    uint32_t dso;
    int active;
} atexit_record;

static once_flag g_atexit_once = ONCE_FLAG_INIT;
static mtx_t g_atexit_lock;
static atexit_record *g_atexit_records;
static size_t g_atexit_count;
static size_t g_atexit_capacity;

static void atexit_once(void) {
    (void)mtx_init(&g_atexit_lock, mtx_plain);
}

static int register_atexit(uint32_t function, uint32_t argument,
                           uint32_t dso) {
    const atexit_record record = { function, argument, dso, 1 };
    if (!function) return -1;
    call_once(&g_atexit_once, atexit_once);
    mtx_lock(&g_atexit_lock);
    if (g_atexit_count == g_atexit_capacity) {
        if (g_atexit_capacity >
            SIZE_MAX / 2 / sizeof(*g_atexit_records)) {
            mtx_unlock(&g_atexit_lock);
            return -1;
        }
        size_t capacity = g_atexit_capacity ? g_atexit_capacity * 2 : 16;
        atexit_record *records =
            (atexit_record *)realloc(g_atexit_records,
                                     capacity * sizeof(*records));
        if (!records) {
            mtx_unlock(&g_atexit_lock);
            return -1;
        }
        g_atexit_records = records;
        g_atexit_capacity = capacity;
    }
    g_atexit_records[g_atexit_count++] = record;
    mtx_unlock(&g_atexit_lock);
    return 0;
}

void vita_libc_finalize(uint32_t dso) {
    call_once(&g_atexit_once, atexit_once);
    for (;;) {
        atexit_record record = { 0 };
        mtx_lock(&g_atexit_lock);
        for (size_t i = g_atexit_count; i; --i) {
            atexit_record *candidate = &g_atexit_records[i - 1];
            if (!candidate->active || (dso && candidate->dso != dso))
                continue;
            record = *candidate;
            candidate->active = 0;
            break;
        }
        mtx_unlock(&g_atexit_lock);
        if (!record.active) break;
        r0 = record.argument;
        vita_dispatch(vita_last_pc, record.function);
    }
}

void vita_libc_reset(void) {
    call_once(&g_atexit_once, atexit_once);
    mtx_lock(&g_atexit_lock);
    free(g_atexit_records);
    g_atexit_records = NULL;
    g_atexit_count = 0;
    g_atexit_capacity = 0;
    mtx_unlock(&g_atexit_lock);
}

/* Records the DSO handle used to key static destructors. There is one module
 * and it never unloads, so there is nothing to key. */
void vita_hle___cxa_set_dso_handle_main(void) { }

void vita_hle___cxa_atexit(void) {
    r0 = (uint32_t)register_atexit(r0, r1, r2);
}
void vita_hle___aeabi_atexit(void) {
    r0 = (uint32_t)register_atexit(r1, r0, r2);
}
void vita_hle___cxa_finalize(void) {
    vita_libc_finalize(r0);
}

/* Guards for function-local static initialisation. Word value 2 is the
 * in-progress state; bit zero is the ABI's completed byte. */
void vita_hle___cxa_guard_acquire(void) {
    uint32_t address = r0;
    if (!vita_mem_ptr(address, 4)) {
        vita_trap_unimpl(address, 4, "__cxa_guard_acquire: bad guard");
        r0 = 0;
        return;
    }
    for (;;) {
        uint32_t state = vita_read32(address);
        if (state & 1u) {
            r0 = 0;
            return;
        }
        if (state == 0) {
            uint32_t observed = 0;
            if (vita_mem_compare_exchange32(address, 0, 2, &observed) ==
                VITA_OK) {
                r0 = 1;
                return;
            }
        }
        (void)vita_sched_block(VW_MUTEX, address, 0);
    }
}
void vita_hle___cxa_guard_release(void) {
    uint32_t address = r0;
    if (!vita_mem_ptr(address, 4)) {
        vita_trap_unimpl(address, 4, "__cxa_guard_release: bad guard");
        return;
    }
    vita_write32(address, 1);
    vita_sched_wake(VW_MUTEX, address, 1);
}
void vita_hle___cxa_guard_abort(void) {
    uint32_t address = r0;
    if (!vita_mem_ptr(address, 4)) {
        vita_trap_unimpl(address, 4, "__cxa_guard_abort: bad guard");
        return;
    }
    vita_write32(address, 0);
    vita_sched_wake(VW_MUTEX, address, 1);
}

/* Static module TLS is configured by the native host through vita_tls_init.
 * The loader-private registration structure is not interpreted here. */
void vita_hle__sceLdTlsRegisterModuleInfo(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "SceLibc", "_sceLdTlsRegisterModuleInfo");
}

/* --- errno -------------------------------------------------------------------
 *
 * `errno` is a macro for *__errno_loc(), so the guest needs an address it can
 * write through, which means the slot has to live in GUEST memory rather than
 * be a host int. It is carved out of the heap on first use for that reason.
 */

static VITA_TLS uint32_t g_errno_slot;

void vita_hle__sceLibcErrnoLoc(void) {
    if (!g_errno_slot) {
        g_errno_slot = vita_heap_alloc(4, 4);
        if (g_errno_slot) vita_write32(g_errno_slot, 0);
    }
    r0 = g_errno_slot;
}

/* --- the guest heap ---------------------------------------------------------- */

typedef struct heap_chunk {
    uint32_t address;
    uint32_t size;
    int used;
} heap_chunk;

static once_flag g_heap_once = ONCE_FLAG_INIT;
static mtx_t g_heap_lock;
static heap_chunk *g_heap_chunks;
static uint32_t g_heap_chunk_count, g_heap_chunk_capacity;
static uint32_t g_heap_base, g_heap_size, g_heap_used, g_heap_peak;

static void heap_once(void) {
    (void)mtx_init(&g_heap_lock, mtx_plain);
}

static int heap_insert(uint32_t at, heap_chunk value) {
    if (g_heap_chunk_count == g_heap_chunk_capacity) {
        uint32_t capacity = g_heap_chunk_capacity
                          ? g_heap_chunk_capacity * 2u : 64u;
        if (capacity < g_heap_chunk_capacity ||
            capacity > UINT32_MAX / sizeof(heap_chunk))
            return 0;
        heap_chunk *chunks = (heap_chunk *)realloc(
            g_heap_chunks, (size_t)capacity * sizeof(heap_chunk));
        if (!chunks) return 0;
        g_heap_chunks = chunks;
        g_heap_chunk_capacity = capacity;
    }
    memmove(g_heap_chunks + at + 1, g_heap_chunks + at,
            (size_t)(g_heap_chunk_count - at) * sizeof(heap_chunk));
    g_heap_chunks[at] = value;
    g_heap_chunk_count++;
    return 1;
}

static void heap_erase(uint32_t at) {
    memmove(g_heap_chunks + at, g_heap_chunks + at + 1,
            (size_t)(g_heap_chunk_count - at - 1) * sizeof(heap_chunk));
    g_heap_chunk_count--;
}

void vita_heap_init(uint32_t base, uint32_t size) {
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    free(g_heap_chunks);
    g_heap_chunks = NULL;
    g_heap_chunk_count = 0;
    g_heap_chunk_capacity = 0;
    g_heap_base = 0;
    g_heap_size = 0;
    g_heap_used = 0;
    g_heap_peak = 0;
    g_errno_slot = 0;

    void *region = NULL;
    if (size >= 16 && vita_mem_range(base, size, &region) == VITA_OK) {
        (void)region;
        g_heap_base = base;
        g_heap_size = size;
        g_errno_slot = base;
        vita_write32(base, 0);
        heap_chunk available = { base + 8u, size - 8u, 0 };
        (void)heap_insert(0, available);
    }
    mtx_unlock(&g_heap_lock);
}

void vita_heap_shutdown(void) {
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    free(g_heap_chunks);
    g_heap_chunks = NULL;
    g_heap_chunk_count = 0;
    g_heap_chunk_capacity = 0;
    g_heap_base = g_heap_size = g_heap_used = g_heap_peak = 0;
    g_errno_slot = 0;
    mtx_unlock(&g_heap_lock);
}

uint32_t vita_heap_used(void) {
    uint32_t value;
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    value = g_heap_used;
    mtx_unlock(&g_heap_lock);
    return value;
}

uint32_t vita_heap_size(void) {
    uint32_t value;
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    value = g_heap_size;
    mtx_unlock(&g_heap_lock);
    return value;
}

uint32_t vita_heap_peak(void) {
    uint32_t value;
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    value = g_heap_peak;
    mtx_unlock(&g_heap_lock);
    return value;
}

uint32_t vita_heap_alloc(uint32_t n, uint32_t alignment) {
    if (!n) return 0;
    if (alignment < 8) alignment = 8;
    if ((alignment & (alignment - 1u)) != 0) return 0;
    if (n > UINT32_MAX - 7u) return 0;
    uint32_t size = (n + 7u) & ~7u;

    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    if (!g_heap_size) {
        mtx_unlock(&g_heap_lock);
        return 0;
    }
    for (uint32_t i = 0; i < g_heap_chunk_count; i++) {
        heap_chunk *chunk = &g_heap_chunks[i];
        if (chunk->used) continue;
        uint64_t aligned64 =
            ((uint64_t)chunk->address + alignment - 1u) & ~(uint64_t)(alignment - 1u);
        if (aligned64 > UINT32_MAX) continue;
        uint32_t aligned = (uint32_t)aligned64;
        if (aligned < chunk->address ||
            aligned - chunk->address > chunk->size)
            continue;
        uint32_t before = aligned - chunk->address;
        if (size > chunk->size - before) continue;
        uint32_t after = chunk->size - before - size;

        if (before) {
            heap_chunk head = { chunk->address, before, 0 };
            if (!heap_insert(i, head)) break;
            i++;
            chunk = &g_heap_chunks[i];
        }
        if (after) {
            heap_chunk tail = { aligned + size, after, 0 };
            if (!heap_insert(i + 1, tail)) break;
            chunk = &g_heap_chunks[i];
        }
        chunk->address = aligned;
        chunk->size = size;
        chunk->used = 1;
        g_heap_used += size;
        if (g_heap_used > g_heap_peak) g_heap_peak = g_heap_used;
        mtx_unlock(&g_heap_lock);
        return aligned;
    }
    mtx_unlock(&g_heap_lock);
    return 0;
}

int vita_heap_free(uint32_t address) {
    if (!address) return VITA_OK;
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    for (uint32_t i = 0; i < g_heap_chunk_count; i++) {
        heap_chunk *chunk = &g_heap_chunks[i];
        if (chunk->address != address) continue;
        if (!chunk->used) {
            mtx_unlock(&g_heap_lock);
            return VITA_ERROR_INVALID_ARGUMENT;
        }
        chunk->used = 0;
        g_heap_used -= chunk->size;
        if (i + 1 < g_heap_chunk_count && !g_heap_chunks[i + 1].used) {
            chunk->size += g_heap_chunks[i + 1].size;
            heap_erase(i + 1);
        }
        if (i && !g_heap_chunks[i - 1].used) {
            g_heap_chunks[i - 1].size += chunk->size;
            heap_erase(i);
        }
        mtx_unlock(&g_heap_lock);
        return VITA_OK;
    }

    mtx_unlock(&g_heap_lock);
    return VITA_ERROR_INVALID_ARGUMENT;
}

void vita_libc_thread_shutdown(void) {
    if (g_errno_slot && g_errno_slot != g_heap_base)
        (void)vita_heap_free(g_errno_slot);
    g_errno_slot = 0;
}

static uint32_t heap_allocation_size(uint32_t address) {
    for (uint32_t i = 0; i < g_heap_chunk_count; i++)
        if (g_heap_chunks[i].used && g_heap_chunks[i].address == address)
            return g_heap_chunks[i].size;
    return 0;
}

uint32_t vita_heap_realloc(uint32_t address, uint32_t n) {
    if (!address) return vita_heap_alloc(n, 8);
    if (!n) {
        (void)vita_heap_free(address);
        return 0;
    }
    call_once(&g_heap_once, heap_once);
    mtx_lock(&g_heap_lock);
    uint32_t old_size = heap_allocation_size(address);
    mtx_unlock(&g_heap_lock);
    if (!old_size) return 0;
    if (old_size >= n) return address;

    uint32_t replacement = vita_heap_alloc(n, 8);
    if (!replacement) return 0;
    void *dst = NULL;
    void *src = NULL;
    if (vita_mem_range(replacement, old_size, &dst) != VITA_OK ||
        vita_mem_range(address, old_size, &src) != VITA_OK) {
        (void)vita_heap_free(replacement);
        return 0;
    }
    (void)dst;
    (void)src;
    (void)vita_mem_move_guest(replacement, address, old_size);
    (void)vita_heap_free(address);
    return replacement;
}

void vita_hle_malloc(void) {
    uint32_t n = r0;
    r0 = vita_heap_alloc(n, 8);
}

void vita_hle_free(void) {
    uint32_t address = r0;
    (void)vita_heap_free(address);
}

void vita_hle_realloc(void) {
    r0 = vita_heap_realloc(r0, r1);
}

void vita_hle_calloc(void) {
    uint32_t count = r0, size = r1;
    if (count && size > UINT32_MAX / count) {
        r0 = 0;
        return;
    }
    uint32_t bytes = count * size;
    uint32_t address = vita_heap_alloc(bytes, 8);
    if (address && bytes) {
        (void)vita_mem_set_guest(address, 0, bytes);
    }
    r0 = address;
}

/* --- memory ------------------------------------------------------------------
 *
 * Sizes are known up front, so these are a bounds check and a host call.
 */

void vita_hle_memcpy(void) {
    void *d = gp(r0, r2, "memcpy dst"), *s = gp(r1, r2, "memcpy src");
    /* memmove, not memcpy: the guest may hand over overlapping regions, and
     * while ARM's memcpy is equally undefined there, the host's would corrupt
     * rather than trap and the bug would surface far away. */
    if (d && s) (void)vita_mem_move_guest(r0, r1, r2);
    /* memcpy returns its destination. */
}

void vita_hle_memmove(void) {
    void *d = gp(r0, r2, "memmove dst"), *s = gp(r1, r2, "memmove src");
    if (d && s) (void)vita_mem_move_guest(r0, r1, r2);
}

void vita_hle_memset(void) {
    void *d = gp(r0, r2, "memset");
    if (d) (void)vita_mem_set_guest(r0, (uint8_t)r1, r2);
}

void vita_hle_memcmp(void) {
    void *a = gp(r0, r2, "memcmp a"), *b = gp(r1, r2, "memcmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)memcmp(a, b, r2) : 0u;
}

void vita_hle_memchr(void) {
    uint8_t *p = (uint8_t *)gp(r0, r2, "memchr");
    if (!p) { r0 = 0; return; }
    uint8_t *hit = (uint8_t *)memchr(p, (int)(r1 & 0xFF), r2);
    /* A guest pointer back, not a host one: the offset is what carries over. */
    r0 = hit ? r0 + (uint32_t)(hit - p) : 0u;
}

/* --- strings -----------------------------------------------------------------
 *
 * Every one of these has to measure before it can act, because the length is
 * in the data. The measurement is the bounds check.
 */

void vita_hle_strlen(void) { r0 = gstrlen(r0, "strlen"); }

void vita_hle_strcmp(void) {
    uint32_t la = gstrlen(r0, "strcmp a"), lb = gstrlen(r1, "strcmp b");
    void *a = gp(r0, la + 1, "strcmp a"), *b = gp(r1, lb + 1, "strcmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)strcmp((char *)a, (char *)b) : 0u;
}

void vita_hle_strncmp(void) {
    uint32_t n = r2;
    uint32_t la = gstrlen(r0, "strncmp a"), lb = gstrlen(r1, "strncmp b");
    uint32_t ca = la + 1 < n ? la + 1 : n, cb = lb + 1 < n ? lb + 1 : n;
    void *a = gp(r0, ca, "strncmp a"), *b = gp(r1, cb, "strncmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)strncmp((char *)a, (char *)b, n) : 0u;
}

void vita_hle_strcpy(void) {
    uint32_t n = gstrlen(r1, "strcpy src") + 1;
    void *d = gp(r0, n, "strcpy dst"), *s = gp(r1, n, "strcpy src");
    if (d && s) (void)vita_mem_move_guest(r0, r1, n);
}

void vita_hle_strncpy(void) {
    uint32_t n = r2, l = gstrlen(r1, "strncpy src");
    void *d = gp(r0, n, "strncpy dst");
    if (!d) return;
    uint32_t copy = l < n ? l : n;
    void *s = gp(r1, copy, "strncpy src");
    if (s) (void)vita_mem_move_guest(r0, r1, copy);
    /* strncpy pads the remainder with NULs — not just one, all of them. */
    if (copy < n) (void)vita_mem_set_guest(r0 + copy, 0, n - copy);
}

void vita_hle_strcat(void) {
    uint32_t ld = gstrlen(r0, "strcat dst"), ls = gstrlen(r1, "strcat src");
    void *d = gp(r0 + ld, ls + 1, "strcat dst"), *s = gp(r1, ls + 1, "strcat src");
    if (d && s) (void)vita_mem_move_guest(r0 + ld, r1, ls + 1);
}

void vita_hle_strchr(void) {
    uint32_t base = r0, l = gstrlen(base, "strchr");
    /* The terminator is findable: strchr(s, 0) returns the end of the string. */
    uint8_t *p = (uint8_t *)gp(base, l + 1, "strchr");
    if (!p) { r0 = 0; return; }
    uint8_t *hit = (uint8_t *)memchr(p, (int)(r1 & 0xFF), l + 1);
    r0 = hit ? base + (uint32_t)(hit - p) : 0u;
}

void vita_hle_strrchr(void) {
    uint32_t base = r0, l = gstrlen(base, "strrchr");
    uint8_t *p = (uint8_t *)gp(base, l + 1, "strrchr");
    if (!p) { r0 = 0; return; }
    uint8_t c = (uint8_t)(r1 & 0xFF);
    for (uint32_t i = l + 1; i-- > 0; ) {
        if (p[i] == c) { r0 = base + i; return; }
    }
    r0 = 0;
}

void vita_hle_strstr(void) {
    uint32_t base = r0;
    uint32_t lh = gstrlen(base, "strstr haystack"), ln = gstrlen(r1, "strstr needle");
    char *h = (char *)gp(base, lh + 1, "strstr haystack");
    char *n = (char *)gp(r1, ln + 1, "strstr needle");
    if (!h || !n) { r0 = 0; return; }
    char *hit = strstr(h, n);
    r0 = hit ? base + (uint32_t)(hit - h) : 0u;
}
