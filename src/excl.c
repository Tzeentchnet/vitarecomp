/* Exclusive-monitor and memory-order helpers for generated LDREX/STREX code. */

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "excl_internal.h"

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
static volatile long g_memory_lock;
#else
static int g_memory_lock;
#endif

static uint64_t g_write_epoch;
static VITA_TLS uint32_t g_monitor_addr;
static VITA_TLS uint64_t g_monitor_value;
static VITA_TLS uint64_t g_monitor_epoch;
static VITA_TLS uint8_t g_monitor_size;

void vita_memory_lock(void) {
#if defined(_MSC_VER)
    while (_InterlockedCompareExchange(&g_memory_lock, 1, 0) != 0) {
        _mm_pause();
    }
#else
    while (__atomic_exchange_n(&g_memory_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
#endif
}

void vita_memory_unlock(void) {
#if defined(_MSC_VER)
    _InterlockedExchange(&g_memory_lock, 0);
#else
    __atomic_store_n(&g_memory_lock, 0, __ATOMIC_RELEASE);
#endif
}

void vita_exclusive_note_write(void) {
    g_write_epoch++;
    g_monitor_size = 0;
}

static int valid_size(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static void note_bad_exclusive(void) {
#if defined(_MSC_VER)
    (void)_InterlockedIncrement((volatile long *)&vita_mem_bad_access);
#else
    (void)__atomic_add_fetch(&vita_mem_bad_access, 1u, __ATOMIC_RELAXED);
#endif
}

uint64_t vita_ldrex(uint32_t addr, uint32_t size) {
    uint64_t value = 0;
    if (!valid_size(size) || (addr & (size - 1u)) != 0) {
        note_bad_exclusive();
        g_monitor_size = 0;
        return 0;
    }

    vita_memory_lock();
    const void *p = vita_mem_ptr(addr, size);
    if (p) {
        memcpy(&value, p, size);
        g_monitor_addr = addr;
        g_monitor_value = value;
        g_monitor_epoch = g_write_epoch;
        g_monitor_size = (uint8_t)size;
    } else {
        g_monitor_size = 0;
    }
    vita_memory_unlock();
    return value;
}

uint32_t vita_strex(uint32_t addr, uint64_t value, uint32_t size) {
    const int monitor_matches =
        valid_size(size) &&
        g_monitor_size == size &&
        g_monitor_addr == addr;
    g_monitor_size = 0;
    if (!monitor_matches || (addr & (size - 1u)) != 0) return 1;

    uint32_t failed = 1;
    vita_memory_lock();
    void *p = vita_mem_ptr(addr, size);
    if (p && g_monitor_epoch == g_write_epoch) {
        uint64_t current = 0;
        memcpy(&current, p, size);
        if (current == g_monitor_value) {
            memcpy(p, &value, size);
            g_write_epoch++;
            failed = 0;
        }
    }
    vita_memory_unlock();
    return failed;
}

void vita_clrex(void) {
    g_monitor_size = 0;
}

void vita_barrier(void) {
#if defined(_MSC_VER)
    __faststorefence();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
