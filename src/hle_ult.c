/* hle_ult.c — SceUlt: the user-level thread library's synchronisation
 * objects.
 *
 * SceUlt lets a program run its own fibres over a few kernel threads, with
 * mutexes, condition variables and semaphores whose storage the program
 * supplies (the objects are opaque blocks in its memory, sized by constants
 * in the SDK) and a resource pool of waiting queues it also supplies, sized
 * by a call. The leading-underscore entry points are what the SDK's inline
 * wrappers call, with the object's size as a trailing argument.
 *
 * Over the scheduler (sched.c): a mutex is its recursion count and owner, a
 * semaphore its count, a condition variable the mutex it pairs with; a wait
 * that cannot be satisfied sleeps until the matching signal. The storage the
 * program hands over holds exactly those words, and nothing in the program
 * reads it. Calling convention: see hle.h.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#include <limits.h>

#define SCE_ULT_ERROR_NULL        0x80810001u
#define SCE_ULT_ERROR_INVALID     0x80810003u
#define SCE_ULT_ERROR_BUSY        0x80810004u
#define ULT_WAIT_POOL_MAGIC       0x55574C54u

/* SceInt32 sceUltWaitingQueueResourcePoolGetWorkAreaSize(SceUInt32 numThreads, SceUInt32 numSyncObjects):
 * a size the program allocates for the pool; nothing here needs any of it. */
void vita_hle_sceUltWaitingQueueResourcePoolGetWorkAreaSize(void) {
    uint64_t size = UINT64_C(0x100) + UINT64_C(0x40) * (r0 + (uint64_t)r1);
    r0 = size <= INT32_MAX ? (uint32_t)size : SCE_ULT_ERROR_INVALID;
}

/* SceInt32 _sceUltWaitingQueueResourcePoolCreate(pool, name, numThreads, numSyncObjects, workArea, opt, size) */
void vita_hle__sceUltWaitingQueueResourcePoolCreate(void) {
    uint64_t required = UINT64_C(0x100) +
                        UINT64_C(0x40) * (r2 + (uint64_t)r3);
    uint32_t work = vita_read32(sp);
    if (!vita_mem_ptr(r0, 4) || required > UINT32_MAX ||
        !vita_mem_ptr(work, (uint32_t)required)) {
        r0 = SCE_ULT_ERROR_NULL;
        return;
    }
    vita_hle_trace("sceUltWaitingQueueResourcePoolCreate(0x%08X, threads %u, objects %u)", r0, r2, r3);
    vita_write32(r0, ULT_WAIT_POOL_MAGIC);
    r0 = 0;
}
void vita_hle_sceUltWaitingQueueResourcePoolDestroy(void) {
    if (!vita_mem_ptr(r0, 4) || vita_read32(r0) != ULT_WAIT_POOL_MAGIC) {
        r0 = SCE_ULT_ERROR_NULL;
        return;
    }
    vita_write32(r0, 0);
    r0 = 0;
}

/* SceInt32 sceUltQueueDataResourcePoolGetWorkAreaSize(numData, dataSize,
 * numQueueObjects). The 64-bit dataSize starts in r2; the final argument is
 * therefore passed on the stack. */
void vita_hle_sceUltQueueDataResourcePoolGetWorkAreaSize(void) {
    uint64_t data_size = (uint64_t)r2 | ((uint64_t)r3 << 32);
    uint32_t queues = vita_read32(sp);
    if (data_size > UINT32_MAX) {
        r0 = SCE_ULT_ERROR_INVALID;
        return;
    }
    uint64_t size = UINT64_C(0x100) + (uint64_t)r0 * data_size +
                    UINT64_C(0x40) * queues;
    r0 = size <= INT32_MAX ? (uint32_t)size : SCE_ULT_ERROR_INVALID;
}
void vita_hle__sceUltQueueDataResourcePoolCreate(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0, "SceUlt",
                                 "_sceUltQueueDataResourcePoolCreate");
}
void vita_hle_sceUltQueueDataResourcePoolDestroy(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0, "SceUlt",
                                 "sceUltQueueDataResourcePoolDestroy");
}

/* --- mutexes: the first word of the object is the recursion count --------------- */

/* SceInt32 _sceUltMutexOptParamInitialize(SceUltMutexOptParam *opt, SceSize size) */
void vita_hle__sceUltMutexOptParamInitialize(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, 0);
    r0 = 0;
}

/* SceInt32 _sceUltMutexCreate(SceUltMutex *mutex, const char *name, pool, const opt, SceSize size) */
void vita_hle__sceUltMutexCreate(void) {
    if (!vita_mem_ptr(r0, 8) || !vita_mem_ptr(r2, 4) ||
        vita_read32(r2) != ULT_WAIT_POOL_MAGIC) {
        r0 = SCE_ULT_ERROR_NULL;
        return;
    }
    vita_write32(r0, 0);
    vita_hle_trace("sceUltMutexCreate(0x%08X)", r0);
    r0 = 0;
}

static int ult_lock(uint32_t m, int try_only) {
    uint32_t me = vita_thread_self();
    for (;;) {
        uint32_t c = vita_read32(m), owner = vita_read32(m + 4);
        if (c == 0 || owner == me) {
            if (c == UINT32_MAX) return (int)SCE_ULT_ERROR_BUSY;
            vita_write32(m, c + 1);
            vita_write32(m + 4, me);
            return 0;
        }
        if (try_only) return (int)SCE_ULT_ERROR_BUSY;
        vita_sched_block(VW_ULT, m, 0);
    }
}

static void ult_unlock(uint32_t m) {
    uint32_t c = vita_read32(m);
    c = c ? c - 1 : 0;
    vita_write32(m, c);
    if (c == 0) { vita_write32(m + 4, 0); vita_sched_wake(VW_ULT, m, 1); }
}

/* SceInt32 sceUltMutexLock(SceUltMutex *mutex) */
void vita_hle_sceUltMutexLock(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    r0 = (uint32_t)ult_lock(r0, 0);
}
void vita_hle_sceUltMutexTryLock(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    r0 = (uint32_t)ult_lock(r0, 1);
}

/* SceInt32 sceUltMutexUnlock(SceUltMutex *mutex) */
void vita_hle_sceUltMutexUnlock(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    if (!vita_read32(r0) || vita_read32(r0 + 4) != vita_thread_self()) {
        r0 = SCE_ULT_ERROR_BUSY;
        return;
    }
    ult_unlock(r0);
    r0 = 0;
}
void vita_hle_sceUltMutexDestroy(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    if (vita_read32(r0)) { r0 = SCE_ULT_ERROR_BUSY; return; }
    vita_write32(r0, 0);
    vita_write32(r0 + 4, 0);
    r0 = 0;
}

/* --- condition variables ---------------------------------------------------------- */

/* SceInt32 _sceUltConditionVariableCreate(cond, name, SceUltMutex *mutex, opt, SceSize size) */
void vita_hle__sceUltConditionVariableCreate(void) {
    if (!vita_mem_ptr(r0, 8) || !vita_mem_ptr(r2, 8)) {
        r0 = SCE_ULT_ERROR_NULL;
        return;
    }
    vita_write32(r0, r2);                     /* the mutex it pairs with */
    r0 = 0;
}
void vita_hle__sceUltConditionVariableOptParamInitialize(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, 0);
    r0 = 0;
}

/* SceInt32 sceUltConditionVariableWait(SceUltConditionVariable *cond): give
 * the mutex up, sleep until a signal, take it back */
void vita_hle_sceUltConditionVariableWait(void) {
    uint32_t cv = r0;
    if (!vita_mem_ptr(cv, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    uint32_t m = vita_read32(cv);
    uint32_t held = (m && vita_mem_ptr(m, 8)) ? vita_read32(m) : 0;
    if (!held || vita_read32(m + 4) != vita_thread_self()) {
        r0 = SCE_ULT_ERROR_INVALID;
        return;
    }
    vita_write32(m, 0);
    vita_write32(m + 4, 0);
    vita_sched_wake(VW_ULT, m, 1);
    vita_sched_block(VW_ULT, cv, 0);
    (void)ult_lock(m, 0);
    vita_write32(m, held);
    r0 = 0;
}
void vita_hle_sceUltConditionVariableSignal(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_sched_wake(VW_ULT, r0, 0);
    r0 = 0;
}
void vita_hle_sceUltConditionVariableSignalAll(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_sched_wake(VW_ULT, r0, 1);
    r0 = 0;
}
void vita_hle_sceUltConditionVariableDestroy(void) {
    uint32_t cv = r0;
    if (!vita_mem_ptr(cv, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_sched_wake(VW_ULT, cv, 1);
    vita_write32(cv, 0);
    vita_write32(cv + 4, 0);
    r0 = 0;
}

/* --- semaphores: the first word is the count ---------------------------------------- */

/* SceInt32 _sceUltSemaphoreCreate(sema, name, SceInt32 numResource, pool, opt, SceSize size) */
void vita_hle__sceUltSemaphoreCreate(void) {
    if (!vita_mem_ptr(r0, 8) || !vita_mem_ptr(r3, 4) ||
        vita_read32(r3) != ULT_WAIT_POOL_MAGIC) {
        r0 = SCE_ULT_ERROR_NULL;
        return;
    }
    if ((int32_t)r2 < 0) { r0 = SCE_ULT_ERROR_INVALID; return; }
    vita_write32(r0, r2);
    vita_hle_trace("sceUltSemaphoreCreate(0x%08X, %d)", r0, (int)r2);
    r0 = 0;
}
void vita_hle__sceUltSemaphoreOptParamInitialize(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, 0);
    r0 = 0;
}

/* SceInt32 sceUltSemaphoreAcquire(sema, SceInt32 numResource) */
void vita_hle_sceUltSemaphoreAcquire(void) {
    uint32_t s = r0;
    int32_t n = (int32_t)r1;
    if (!vita_mem_ptr(s, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    if (n <= 0) { r0 = SCE_ULT_ERROR_INVALID; return; }
    for (;;) {
        int32_t c = (int32_t)vita_read32(s);
        if (c >= n) { vita_write32(s, (uint32_t)(c - n)); r0 = 0; return; }
        vita_sched_block(VW_ULT, s, 0);
    }
}

/* SceInt32 sceUltSemaphoreTryAcquire(sema, SceInt32 numResource) */
void vita_hle_sceUltSemaphoreTryAcquire(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    int32_t c = (int32_t)vita_read32(r0), n = (int32_t)r1;
    if (n <= 0) { r0 = SCE_ULT_ERROR_INVALID; return; }
    if (c >= n) { vita_write32(r0, (uint32_t)(c - n)); r0 = 0; }
    else r0 = SCE_ULT_ERROR_BUSY;
}

/* SceInt32 sceUltSemaphoreRelease(sema, SceInt32 numResource) */
void vita_hle_sceUltSemaphoreRelease(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    int32_t count = (int32_t)vita_read32(r0);
    int32_t amount = (int32_t)r1;
    if (amount <= 0 || count < 0 || amount > INT32_MAX - count) {
        r0 = SCE_ULT_ERROR_INVALID;
        return;
    }
    vita_write32(r0, (uint32_t)(count + amount));
    vita_sched_wake(VW_ULT, r0, 1);
    r0 = 0;
}
void vita_hle_sceUltSemaphoreDestroy(void) { r0 = vita_mem_ptr(r0, 4) ? 0 : SCE_ULT_ERROR_NULL; }
