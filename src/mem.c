/* mem.c — a flat guest address space.
 *
 * One allocation and a subtraction. The Vita's MMU means there is no fixed map
 * to model; what matters for a recompiled module is that the addresses it
 * actually touches are backed and that the ones it should not touch are
 * noticed rather than silently absorbed.
 */

#include "vitarecomp/mem.h"
#include "vitarecomp/runtime.h"
#include "excl_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

uint32_t vita_mem_bad_access;

static uint8_t *g_mem;
static uint32_t g_base;
static size_t   g_size;

static void note_bad_access(void) {
#if defined(_MSC_VER)
    (void)_InterlockedIncrement((volatile long *)&vita_mem_bad_access);
#else
    (void)__atomic_add_fetch(&vita_mem_bad_access, 1u, __ATOMIC_RELAXED);
#endif
}

static int range_ptr(uint32_t addr, size_t len, void **out, int count_bad) {
    if (out) *out = NULL;
    if (!g_mem || addr < g_base) {
        if (count_bad) note_bad_access();
        return VITA_ERROR_OUT_OF_BOUNDS;
    }
    const uint64_t off = (uint64_t)addr - g_base;
    if (off > g_size || len > g_size - (size_t)off) {
        if (count_bad) note_bad_access();
        return VITA_ERROR_OUT_OF_BOUNDS;
    }
    if (out) *out = g_mem + (size_t)off;
    return VITA_OK;
}

int vita_mem_init(uint32_t base, size_t size) {
    if (!size || (uint64_t)base + size > UINT64_C(0x100000000))
        return 1;
    vita_memory_lock();
    free(g_mem);
    g_mem = (uint8_t *)calloc(1, size);
    if (!g_mem) {
        g_base = 0;
        g_size = 0;
        vita_memory_unlock();
        return 1;
    }
    g_base = base;
    g_size = size;
    vita_mem_bad_access = 0;
    vita_memory_unlock();
    return 0;
}

void vita_mem_free(void) {
    vita_memory_lock();
    free(g_mem);
    g_mem = NULL;
    g_base = 0;
    g_size = 0;
    vita_memory_unlock();
}

void *vita_mem_ptr(uint32_t addr, uint32_t len) {
    void *p = NULL;
    (void)range_ptr(addr, len, &p, 1);
    return p;
}

int vita_mem_range(uint32_t addr, size_t len, void **out) {
    if (!out) return VITA_ERROR_INVALID_ARGUMENT;
    return range_ptr(addr, len, out, 1);
}

int vita_mem_copy_from_guest(void *dst, uint32_t src, size_t len) {
    if (!dst && len) return VITA_ERROR_INVALID_ARGUMENT;
    vita_memory_lock();
    void *p = NULL;
    int result = range_ptr(src, len, &p, 1);
    if (result == VITA_OK && len) memcpy(dst, p, len);
    vita_memory_unlock();
    return result;
}

int vita_mem_copy_to_guest(uint32_t dst, const void *src, size_t len) {
    if (!src && len) return VITA_ERROR_INVALID_ARGUMENT;
    vita_memory_lock();
    void *p = NULL;
    int result = range_ptr(dst, len, &p, 1);
    if (result == VITA_OK && len) {
        memcpy(p, src, len);
        vita_exclusive_note_write();
    }
    vita_memory_unlock();
    return result;
}

int vita_mem_move_guest(uint32_t dst, uint32_t src, size_t len) {
    vita_memory_lock();
    void *destination = NULL;
    void *source = NULL;
    int result = range_ptr(dst, len, &destination, 1);
    if (result == VITA_OK)
        result = range_ptr(src, len, &source, 1);
    if (result == VITA_OK && len) {
        memmove(destination, source, len);
        vita_exclusive_note_write();
    }
    vita_memory_unlock();
    return result;
}

int vita_mem_set_guest(uint32_t dst, uint8_t value, size_t len) {
    vita_memory_lock();
    void *destination = NULL;
    int result = range_ptr(dst, len, &destination, 1);
    if (result == VITA_OK && len) {
        memset(destination, value, len);
        vita_exclusive_note_write();
    }
    vita_memory_unlock();
    return result;
}

int vita_mem_read_cstr(uint32_t addr, char *dst, size_t capacity,
                       size_t *length) {
    if (!dst || !capacity) return VITA_ERROR_INVALID_ARGUMENT;
    dst[0] = '\0';
    if (length) *length = 0;

    vita_memory_lock();
    if (!g_mem || addr < g_base) {
        note_bad_access();
        vita_memory_unlock();
        return VITA_ERROR_OUT_OF_BOUNDS;
    }
    uint64_t off64 = (uint64_t)addr - g_base;
    if (off64 >= g_size) {
        note_bad_access();
        vita_memory_unlock();
        return VITA_ERROR_OUT_OF_BOUNDS;
    }
    size_t available = g_size - (size_t)off64;
    const char *source = (const char *)g_mem + (size_t)off64;
    const char *nul = (const char *)memchr(source, '\0', available);
    if (!nul) {
        vita_memory_unlock();
        return VITA_ERROR_NOT_TERMINATED;
    }
    size_t n = (size_t)(nul - source);
    if (length) *length = n;
    if (n >= capacity) {
        memcpy(dst, source, capacity - 1);
        dst[capacity - 1] = '\0';
        vita_memory_unlock();
        return VITA_ERROR_LIMIT;
    }
    memcpy(dst, source, n + 1);
    vita_memory_unlock();
    return VITA_OK;
}

uint32_t vita_mem_avail(uint32_t addr) {
    if (!g_mem || addr < g_base) return 0;
    uint64_t off = (uint64_t)addr - g_base;
    if (off >= g_size) return 0;
    size_t available = g_size - (size_t)off;
    return available > UINT32_MAX ? UINT32_MAX : (uint32_t)available;
}

uint32_t vita_mem_base(void) { return g_base; }
size_t vita_mem_size(void) { return g_size; }

uint64_t vita_mem_bad_access_count(void) {
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long *)&vita_mem_bad_access, 0, 0);
#else
    return __atomic_load_n(&vita_mem_bad_access, __ATOMIC_RELAXED);
#endif
}

/* Unaligned access is legal on ARMv7 for ordinary loads and stores, so these
 * go through memcpy rather than a cast. A cast would be undefined behaviour on
 * an unaligned address and would fault outright on some hosts. */

uint32_t vita_read32(uint32_t a) {
    vita_memory_lock();
    void *p = NULL;
    uint32_t v = 0;
    if (range_ptr(a, 4, &p, 1) == VITA_OK) memcpy(&v, p, 4);
    vita_memory_unlock();
    return v;
}

uint16_t vita_read16(uint32_t a) {
    vita_memory_lock();
    void *p = NULL;
    uint16_t v = 0;
    if (range_ptr(a, 2, &p, 1) == VITA_OK) memcpy(&v, p, 2);
    vita_memory_unlock();
    return v;
}

uint8_t vita_read8(uint32_t a) {
    vita_memory_lock();
    void *p = NULL;
    uint8_t v = 0;
    if (range_ptr(a, 1, &p, 1) == VITA_OK) v = *(uint8_t *)p;
    vita_memory_unlock();
    return v;
}

void vita_write32(uint32_t a, uint32_t v) {
    vita_memory_lock();
    void *p = NULL;
    if (range_ptr(a, 4, &p, 1) == VITA_OK) {
        memcpy(p, &v, 4);
        vita_exclusive_note_write();
    }
    vita_memory_unlock();
}

void vita_write16(uint32_t a, uint16_t v) {
    vita_memory_lock();
    void *p = NULL;
    if (range_ptr(a, 2, &p, 1) == VITA_OK) {
        memcpy(p, &v, 2);
        vita_exclusive_note_write();
    }
    vita_memory_unlock();
}

void vita_write8(uint32_t a, uint8_t v) {
    vita_memory_lock();
    void *p = NULL;
    if (range_ptr(a, 1, &p, 1) == VITA_OK) {
        *(uint8_t *)p = v;
        vita_exclusive_note_write();
    }
    vita_memory_unlock();
}

int vita_mem_compare_exchange32(uint32_t addr, uint32_t expected,
                                uint32_t desired, uint32_t *observed) {
    if (!observed || (addr & 3u)) {
        note_bad_access();
        return VITA_ERROR_INVALID_ARGUMENT;
    }
    vita_memory_lock();
    void *p = NULL;
    int result = range_ptr(addr, 4, &p, 1);
    if (result == VITA_OK) {
        uint32_t current;
        memcpy(&current, p, sizeof(current));
        *observed = current;
        if (current == expected) {
            memcpy(p, &desired, sizeof(desired));
            vita_exclusive_note_write();
        } else {
            result = VITA_ERROR_BUSY;
        }
    }
    vita_memory_unlock();
    return result;
}

int vita_mem_mark_written(uint32_t addr, size_t len) {
    vita_memory_lock();
    void *p = NULL;
    int result = range_ptr(addr, len, &p, 1);
    (void)p;
    if (result == VITA_OK && len) vita_exclusive_note_write();
    vita_memory_unlock();
    return result;
}
