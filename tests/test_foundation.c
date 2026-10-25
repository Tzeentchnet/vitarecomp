/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/runtime.h"
#include "vitarecomp/vfs.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#define make_dir(path) _mkdir(path)
#define remove_dir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(path) mkdir(path, 0777)
#define remove_dir(path) rmdir(path)
#endif

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

#define MEMORY_BASE UINT32_C(0x10000000)
#define MEMORY_SIZE UINT32_C(0x01000000)
#define HEAP_BASE   UINT32_C(0x10000000)
#define HEAP_SIZE   UINT32_C(0x00200000)
#define MSPACE_BASE UINT32_C(0x10300000)
#define SYSMEM_BASE UINT32_C(0x10800000)
#define SYSMEM_SIZE UINT32_C(0x00400000)
#define KERNEL_BASE UINT32_C(0x10C00000)
#define KERNEL_SIZE UINT32_C(0x00200000)
#define MAIN_STACK  UINT32_C(0x106F0000)

static void configure_noninteractive_failures(void) {
#ifdef _WIN32
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER) && defined(_DEBUG)
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#endif
}

void vita_hle_memcpy(void);
void vita_hle_strlen(void);
void vita_hle_calloc(void);
void vita_hle_free(void);
void vita_hle_sinf(void);
void vita_hle__sceLibcErrnoLoc(void);
void vita_hle___cxa_atexit(void);
void vita_hle___cxa_guard_acquire(void);
void vita_hle___cxa_guard_release(void);
void vita_hle___cxa_guard_abort(void);

void vita_hle_sceClibMspaceCreate(void);
void vita_hle_sceClibMspaceDestroy(void);
void vita_hle_sceClibMspaceMalloc(void);
void vita_hle_sceClibMspaceFree(void);

void vita_hle_sceKernelAllocMemBlock(void);
void vita_hle_sceKernelGetMemBlockBase(void);
void vita_hle_sceKernelFreeMemBlock(void);

void vita_hle_sceKernelCreateSema(void);
void vita_hle_sceKernelWaitSema(void);
void vita_hle_sceKernelSignalSema(void);
void vita_hle_sceKernelDeleteSema(void);
void vita_hle_sceKernelCreateThread(void);
void vita_hle_sceKernelStartThread(void);
void vita_hle_sceKernelWaitThreadEnd(void);
void vita_hle_sceKernelDeleteThread(void);
void vita_sched_on_handover(void (*leave)(void));
void vita_sched_wake(int kind, uint32_t object, int all);

void vita_hle_sceFiosInitialize(void);
void vita_hle_sceFiosTerminate(void);
void vita_hle_sceFiosFHOpenSync(void);
void vita_hle_sceFiosFHOpen(void);
void vita_hle_sceFiosFHReadSync(void);
void vita_hle_sceFiosFHCloseSync(void);
void vita_hle_sceFiosIsValidHandle(void);
void vita_hle_sceFiosArchiveMountSync(void);
void vita_hle_sceFiosArchiveUnmountSync(void);
void vita_hle_sceFiosOpIsDone(void);
void vita_hle_sceFiosOpGetError(void);
void vita_hle_sceFiosOpDelete(void);

void vita_hle_sceUltWaitingQueueResourcePoolGetWorkAreaSize(void);
void vita_hle__sceUltWaitingQueueResourcePoolCreate(void);
void vita_hle_sceUltWaitingQueueResourcePoolDestroy(void);
void vita_hle__sceUltMutexCreate(void);
void vita_hle_sceUltMutexLock(void);
void vita_hle_sceUltMutexTryLock(void);
void vita_hle_sceUltMutexUnlock(void);
void vita_hle_sceUltMutexDestroy(void);
void vita_hle__sceUltSemaphoreCreate(void);
void vita_hle_sceUltSemaphoreAcquire(void);
void vita_hle_sceUltSemaphoreTryAcquire(void);
void vita_hle_sceUltSemaphoreRelease(void);
void vita_hle_sceUltSemaphoreDestroy(void);

void vita_hle_sceKernelCreateMutex(void);
void vita_hle_sceKernelLockMutex(void);
void vita_hle_sceKernelUnlockMutex(void);
void vita_hle_sceKernelDeleteMutex(void);
void vita_hle_sceKernelCreateEventFlag(void);
void vita_hle_sceKernelSetEventFlag(void);
void vita_hle_sceKernelPollEventFlag(void);
void vita_hle_sceKernelDeleteEventFlag(void);
void vita_hle_sceKernelCreateMsgPipe(void);
void vita_hle_sceKernelTrySendMsgPipe(void);
void vita_hle_sceKernelTryReceiveMsgPipe(void);
void vita_hle_sceKernelDeleteMsgPipe(void);
void vita_hle_sceKernelCreateCallback(void);
void vita_hle_sceKernelNotifyCallback(void);
void vita_hle_sceKernelCheckCallback(void);
void vita_hle_sceKernelDeleteCallback(void);

typedef struct diagnostics {
    uint32_t total;
    uint32_t import_errors;
    char library[64];
    char function[64];
} diagnostics;

static diagnostics g_diagnostics;
static uint32_t g_worker_marker = MEMORY_BASE + UINT32_C(0x00601000);
static uint32_t g_worker_initial_r4;
static uint32_t g_callback_count;
static uint32_t g_callback_arg;
static uint32_t g_callback_userdata;
static uint32_t g_import_calls;
static uint32_t g_worker_tls;
static uint32_t g_worker_errno;
static uint32_t g_destructor_argument;
static uint32_t g_handover_calls;

static void handover_regression_callback(void) {
    g_handover_calls++;
    vita_sched_wake(0x7FFF, UINT32_C(0xA55A), 1);
}

static void diagnostic(const vita_diagnostic *event, void *userdata) {
    diagnostics *state = (diagnostics *)userdata;
    state->total++;
    if (event->code == VITA_DIAG_IMPORT &&
        event->severity >= VITA_DIAG_ERROR) {
        state->import_errors++;
        snprintf(state->library, sizeof(state->library), "%s",
                 event->library_name ? event->library_name : "");
        snprintf(state->function, sizeof(state->function), "%s",
                 event->function_name ? event->function_name : "");
    }
}

static void put_guest(uint32_t address, const void *data, size_t size) {
    assert(vita_mem_copy_to_guest(address, data, size) == VITA_OK);
}

static void put_string(uint32_t address, const char *value) {
    put_guest(address, value, strlen(value) + 1);
}

static void worker_entry(void) {
    g_worker_initial_r4 = r4;
    g_worker_tls = vita_tls_base();
    vita_hle__sceLibcErrnoLoc();
    g_worker_errno = r0;
    r4 = UINT32_C(0x22222222);
    uint32_t argument = r1;
    uint32_t semaphore = vita_read32(argument);
    uint32_t marker = vita_read32(argument + 4);
    r0 = semaphore;
    r1 = 1;
    r2 = 0;
    vita_hle_sceKernelWaitSema();
    assert(r0 == 0);
    vita_write32(marker, UINT32_C(0xC0FFEE));
    r0 = 77;
}

static void callback_entry(void) {
    g_callback_count = r1;
    g_callback_arg = r2;
    g_callback_userdata = r3;
    r0 = 123;
}

static void registered_import(void) {
    g_import_calls++;
    r0 = UINT32_C(0x12345678);
}

static void destructor_entry(void) {
    g_destructor_argument = r0;
}

static const vita_dispatch_entry DISPATCH[] = {
    { UINT32_C(0x1001), worker_entry },
    { UINT32_C(0x2001), callback_entry },
    { UINT32_C(0x3001), destructor_entry },
};

static void test_memory(void) {
    void *pointer = NULL;
    assert(vita_mem_range(MEMORY_BASE, 0, &pointer) == VITA_OK);
    assert(pointer != NULL);
    assert(vita_mem_range(MEMORY_BASE + MEMORY_SIZE, 0, &pointer) == VITA_OK);
    assert(vita_mem_range(UINT32_C(0xFFFFFFF0), 64, &pointer) ==
           VITA_ERROR_OUT_OF_BOUNDS);

    const char text[] = "abcdef";
    put_guest(MEMORY_BASE + 0x1000, text, sizeof(text));
    assert(vita_mem_move_guest(MEMORY_BASE + 0x1001,
                               MEMORY_BASE + 0x1000, 5) == VITA_OK);
    char moved[7] = {0};
    assert(vita_mem_copy_from_guest(moved, MEMORY_BASE + 0x1000, 6) ==
           VITA_OK);
    assert(memcmp(moved, "aabcde", 6) == 0);

    size_t length = 0;
    char string[8];
    put_string(MEMORY_BASE + 0x1100, "guest");
    assert(vita_mem_read_cstr(MEMORY_BASE + 0x1100, string,
                              sizeof(string), &length) == VITA_OK);
    assert(length == 5 && strcmp(string, "guest") == 0);
    assert(vita_mem_read_cstr(MEMORY_BASE + 0x1100, string, 4, &length) ==
           VITA_ERROR_LIMIT);
    assert(length == 5 && strcmp(string, "gue") == 0);
    const char unterminated[4] = {'n', 'o', 'p', 'e'};
    put_guest(MEMORY_BASE + MEMORY_SIZE - 4, unterminated,
              sizeof(unterminated));
    assert(vita_mem_read_cstr(MEMORY_BASE + MEMORY_SIZE - 4, string,
                              sizeof(string), NULL) ==
           VITA_ERROR_NOT_TERMINATED);

    uint32_t atomic = MEMORY_BASE + 0x1200;
    vita_write32(atomic, 10);
    assert(vita_ldrex(atomic, 4) == 10);
    vita_write32(atomic, 11);
    assert(vita_strex(atomic, 12, 4) == 1);
    uint32_t observed = 0;
    assert(vita_mem_compare_exchange32(atomic, 99, 20, &observed) ==
           VITA_ERROR_BUSY);
    assert(observed == 11);
    assert(vita_ldrex(atomic, 4) == 11);
    assert(vita_mem_compare_exchange32(atomic, 11, 20, &observed) == VITA_OK);
    assert(vita_strex(atomic, 21, 4) == 1);
    assert(vita_ldrex(atomic, 4) == 20);
    assert(vita_strex(atomic, 21, 4) == 0);
    assert(vita_read32(atomic) == 21);

    uint32_t source = MEMORY_BASE + 0x1210;
    vita_write32(source, 33);
    (void)vita_ldrex(atomic, 4);
    r0 = atomic;
    r1 = source;
    r2 = 4;
    vita_hle_memcpy();
    assert(vita_strex(atomic, 44, 4) == 1);
    assert(vita_read32(atomic) == 33);

    uint64_t bad_before = vita_mem_bad_access_count();
    (void)vita_read32(UINT32_C(0xFFFFFFFF));
    assert(vita_mem_bad_access_count() == bad_before + 1);
}

static void test_allocators_and_libc(void) {
    uint32_t first = vita_heap_alloc(64, 16);
    uint32_t second = vita_heap_alloc(64, 16);
    assert(first && second && first != second);
    vita_write32(first, UINT32_C(0xAABBCCDD));
    assert(vita_heap_free(first) == VITA_OK);
    uint32_t reused = vita_heap_alloc(32, 16);
    assert(reused == first);
    uint32_t grown = vita_heap_realloc(reused, 128);
    assert(grown && vita_read32(grown) == UINT32_C(0xAABBCCDD));
    assert(vita_heap_free(grown) == VITA_OK);
    assert(vita_heap_free(second) == VITA_OK);
    assert(vita_heap_used() == 0);
    assert(vita_heap_peak() >= 128);

    r0 = 4;
    r1 = 8;
    vita_hle_calloc();
    uint32_t zeroed = r0;
    assert(zeroed && vita_read32(zeroed) == 0);
    r0 = zeroed;
    vita_hle_free();

    put_string(MEMORY_BASE + 0x1300, "bounded");
    r0 = MEMORY_BASE + 0x1300;
    vita_hle_strlen();
    assert(r0 == 7);

    vfp_setf(0, 0.5f);
    vita_hle_sinf();
    assert(fabsf(vfp_getf(0) - sinf(0.5f)) < 0.000001f);

    r0 = MSPACE_BASE;
    r1 = 0x10000;
    vita_hle_sceClibMspaceCreate();
    uint32_t mspace = r0;
    assert(mspace == MSPACE_BASE);
    r0 = mspace;
    r1 = 128;
    vita_hle_sceClibMspaceMalloc();
    uint32_t ma = r0;
    r0 = mspace;
    r1 = 128;
    vita_hle_sceClibMspaceMalloc();
    uint32_t mb = r0;
    assert(ma && mb && ma != mb);
    r0 = mspace;
    r1 = ma;
    vita_hle_sceClibMspaceFree();
    r0 = mspace;
    r1 = 64;
    vita_hle_sceClibMspaceMalloc();
    assert(r0 == ma);
    r0 = mspace;
    vita_hle_sceClibMspaceDestroy();
    assert(r0 == 0);

    r0 = 0;
    r1 = 0;
    r2 = 0x1000;
    vita_hle_sceKernelAllocMemBlock();
    uint32_t block_a = r0;
    assert((block_a & UINT32_C(0x80000000)) == 0);
    uint32_t base_output = MEMORY_BASE + 0x1400;
    r0 = block_a;
    r1 = base_output;
    vita_hle_sceKernelGetMemBlockBase();
    assert(r0 == 0);
    uint32_t block_base = vita_read32(base_output);
    r0 = block_a;
    vita_hle_sceKernelFreeMemBlock();
    assert(r0 == 0);
    r0 = 0;
    r1 = 0;
    r2 = 0x1000;
    vita_hle_sceKernelAllocMemBlock();
    uint32_t block_b = r0;
    r0 = block_b;
    r1 = base_output;
    vita_hle_sceKernelGetMemBlockBase();
    assert(vita_read32(base_output) == block_base);
    r0 = block_b;
    vita_hle_sceKernelFreeMemBlock();

    uint32_t guard = MEMORY_BASE + 0x1410;
    vita_write32(guard, 0);
    r0 = guard;
    vita_hle___cxa_guard_acquire();
    assert(r0 == 1);
    r0 = guard;
    vita_hle___cxa_guard_abort();
    r0 = guard;
    vita_hle___cxa_guard_acquire();
    assert(r0 == 1);
    r0 = guard;
    vita_hle___cxa_guard_release();
    r0 = guard;
    vita_hle___cxa_guard_acquire();
    assert(r0 == 0);
}

static void test_imports(void) {
    const vita_hle_import imports[] = {
        { 0x10, 0x20, "FixtureLibrary", "fixtureFunction",
          registered_import },
        { 0, 0, "FixtureLibrary", "fixtureByName", registered_import },
    };
    assert(vita_hle_register(imports, 2) == VITA_OK);
    r0 = 0;
    assert(vita_hle_dispatch_import(0x5000, 0x10, 0x20, NULL, NULL) ==
           VITA_OK);
    assert(r0 == UINT32_C(0x12345678) && g_import_calls == 1);
    assert(vita_hle_dispatch_import(0x5004, 0, 0, "FixtureLibrary",
                                    "fixtureByName") == VITA_OK);
    assert(g_import_calls == 2);

    const vita_hle_import duplicates[] = {
        { 1, 2, "Duplicate", "same", registered_import },
        { 1, 2, "Duplicate", "same", registered_import },
    };
    assert(vita_hle_register(duplicates, 2) ==
           VITA_ERROR_ALREADY_INITIALIZED);

    assert(vita_hle_dispatch_import(0x5008, 0xAA, 0xBB,
                                    "MissingLibrary", "missingFunction") ==
           VITA_ERROR_UNIMPLEMENTED);
    assert(r0 == UINT32_C(0x80020001));
    assert(g_diagnostics.import_errors == 1);
    assert(strcmp(g_diagnostics.library, "MissingLibrary") == 0);
    assert(strcmp(g_diagnostics.function, "missingFunction") == 0);
}

static void test_threads_sync_and_callbacks(void) {
    uint32_t name = MEMORY_BASE + 0x1500;
    put_string(name, "fixture");
    vita_dispatch_init(DISPATCH,
                       (uint32_t)(sizeof(DISPATCH) / sizeof(DISPATCH[0])));
    vita_main_stack_init(MAIN_STACK, 0x10000);
    sp = MAIN_STACK + 0xF000;
    vita_sched_on_handover(handover_regression_callback);
    uint32_t main_tls = vita_tls_base();
    vita_hle__sceLibcErrnoLoc();
    uint32_t main_errno = r0;
    assert(main_tls && main_errno);

    r0 = name;
    r1 = 0;
    r2 = 0;
    r3 = 1;
    vita_hle_sceKernelCreateSema();
    uint32_t semaphore = r0;

    uint32_t arguments = MEMORY_BASE + 0x1600;
    vita_write32(arguments, semaphore);
    vita_write32(arguments + 4, g_worker_marker);
    r0 = name;
    r1 = 0x1001;
    r2 = 0x40;
    r3 = 0x4000;
    vita_hle_sceKernelCreateThread();
    uint32_t thread = r0;
    assert((thread & UINT32_C(0x80000000)) == 0);

    r4 = UINT32_C(0x11111111);
    r0 = thread;
    r1 = 8;
    r2 = arguments;
    vita_hle_sceKernelStartThread();
    assert(r0 == 0);
    assert(r4 == UINT32_C(0x11111111));
    assert(g_worker_initial_r4 == 0);
    assert(vita_read32(g_worker_marker) == 0);

    r0 = semaphore;
    r1 = 1;
    vita_hle_sceKernelSignalSema();
    assert(r0 == 0);
    uint32_t status = MEMORY_BASE + 0x1610;
    r0 = thread;
    r1 = status;
    r2 = 0;
    vita_hle_sceKernelWaitThreadEnd();
    assert(r0 == 0);
    assert(vita_read32(status) == 77);
    assert(vita_read32(g_worker_marker) == UINT32_C(0xC0FFEE));
    assert(g_worker_tls && g_worker_tls != main_tls);
    assert(g_worker_errno && g_worker_errno != main_errno);
    assert(r4 == UINT32_C(0x11111111));
    r0 = thread;
    vita_hle_sceKernelDeleteThread();
    assert(r0 == 0);
    r0 = semaphore;
    vita_hle_sceKernelDeleteSema();
    assert(r0 == 0);

    r0 = name;
    r1 = 0;
    r2 = 0;
    r3 = 0;
    vita_hle_sceKernelCreateMutex();
    uint32_t mutex = r0;
    r0 = mutex;
    r1 = 1;
    r2 = 0;
    vita_hle_sceKernelLockMutex();
    assert(r0 == 0);
    r0 = mutex;
    r1 = 1;
    vita_hle_sceKernelUnlockMutex();
    assert(r0 == 0);
    r0 = mutex;
    vita_hle_sceKernelDeleteMutex();
    assert(r0 == 0);

    r0 = name;
    r1 = 0;
    r2 = 0;
    r3 = 0;
    vita_hle_sceKernelCreateEventFlag();
    uint32_t event = r0;
    r0 = event;
    r1 = 0x4;
    vita_hle_sceKernelSetEventFlag();
    uint32_t event_out = MEMORY_BASE + 0x1620;
    r0 = event;
    r1 = 0x4;
    r2 = 0;
    r3 = event_out;
    vita_hle_sceKernelPollEventFlag();
    assert(r0 == 0 && (vita_read32(event_out) & 0x4));
    r0 = event;
    vita_hle_sceKernelDeleteEventFlag();
    assert(r0 == 0);

    r0 = name;
    r1 = 0;
    r2 = 0;
    r3 = 8;
    vita_hle_sceKernelCreateMsgPipe();
    uint32_t pipe = r0;
    uint32_t message = MEMORY_BASE + 0x1630;
    uint32_t received = MEMORY_BASE + 0x1640;
    uint32_t transferred = MEMORY_BASE + 0x1650;
    put_guest(message, "ping", 4);
    vita_write32(sp, transferred);
    vita_write32(sp + 4, 0);
    r0 = pipe;
    r1 = message;
    r2 = 4;
    r3 = 1;
    vita_hle_sceKernelTrySendMsgPipe();
    assert(r0 == 0 && vita_read32(transferred) == 4);
    vita_write32(transferred, 0);
    r0 = pipe;
    r1 = received;
    r2 = 4;
    r3 = 1;
    vita_hle_sceKernelTryReceiveMsgPipe();
    char received_host[4];
    assert(r0 == 0 && vita_read32(transferred) == 4);
    assert(vita_mem_copy_from_guest(received_host, received, 4) == VITA_OK);
    assert(memcmp(received_host, "ping", 4) == 0);
    r0 = pipe;
    vita_hle_sceKernelDeleteMsgPipe();
    assert(r0 == 0);

    r0 = name;
    r1 = 0;
    r2 = 0x2001;
    r3 = UINT32_C(0xABCDEF01);
    vita_hle_sceKernelCreateCallback();
    uint32_t callback = r0;
    r0 = callback;
    r1 = 7;
    vita_hle_sceKernelNotifyCallback();
    assert(r0 == 0);
    r0 = callback;
    r1 = 9;
    vita_hle_sceKernelNotifyCallback();
    assert(r0 == 0);
    vita_hle_sceKernelCheckCallback();
    assert(r0 == 0);
    assert(g_callback_count == 2);
    assert(g_callback_arg == 9);
    assert(g_callback_userdata == UINT32_C(0xABCDEF01));
    r0 = callback;
    vita_hle_sceKernelDeleteCallback();
    assert(r0 == 0);
    vita_sched_on_handover(NULL);
    assert(g_handover_calls >= 2);
}

static void test_ult(void) {
    uint32_t pool = MEMORY_BASE + 0x1800;
    uint32_t mutex = MEMORY_BASE + 0x1820;
    uint32_t semaphore = MEMORY_BASE + 0x1840;
    uint32_t work_area = MEMORY_BASE + 0x1A00;

    r0 = 2;
    r1 = 4;
    vita_hle_sceUltWaitingQueueResourcePoolGetWorkAreaSize();
    assert(r0 == 0x280);
    r0 = pool;
    r1 = 0;
    r2 = 2;
    r3 = 4;
    vita_write32(sp, work_area);
    vita_hle__sceUltWaitingQueueResourcePoolCreate();
    assert(r0 == 0);

    r0 = mutex;
    r1 = 0;
    r2 = pool;
    r3 = 0;
    vita_hle__sceUltMutexCreate();
    assert(r0 == 0);
    r0 = mutex;
    vita_hle_sceUltMutexLock();
    assert(r0 == 0);
    r0 = mutex;
    vita_hle_sceUltMutexTryLock();
    assert(r0 == 0);
    r0 = mutex;
    vita_hle_sceUltMutexUnlock();
    assert(r0 == 0);
    r0 = mutex;
    vita_hle_sceUltMutexUnlock();
    assert(r0 == 0);
    r0 = mutex;
    vita_hle_sceUltMutexUnlock();
    assert(r0 != 0);
    r0 = mutex;
    vita_hle_sceUltMutexDestroy();
    assert(r0 == 0);

    r0 = semaphore;
    r1 = 0;
    r2 = 2;
    r3 = pool;
    vita_hle__sceUltSemaphoreCreate();
    assert(r0 == 0);
    r0 = semaphore;
    r1 = 1;
    vita_hle_sceUltSemaphoreAcquire();
    assert(r0 == 0);
    r0 = semaphore;
    r1 = 2;
    vita_hle_sceUltSemaphoreTryAcquire();
    assert(r0 != 0);
    r0 = semaphore;
    r1 = 2;
    vita_hle_sceUltSemaphoreRelease();
    assert(r0 == 0);
    r0 = semaphore;
    r1 = 2;
    vita_hle_sceUltSemaphoreTryAcquire();
    assert(r0 == 0);
    r0 = semaphore;
    vita_hle_sceUltSemaphoreDestroy();
    assert(r0 == 0);

    r0 = pool;
    vita_hle_sceUltWaitingQueueResourcePoolDestroy();
    assert(r0 == 0);
}

static void put_be32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put_be40(uint8_t *out, uint64_t value) {
    out[0] = (uint8_t)(value >> 32);
    put_be32(out + 1, (uint32_t)value);
}

static void write_psarc_entry(uint8_t *entry, uint32_t first,
                              uint64_t size, uint64_t offset) {
    put_be32(entry + 16, first);
    put_be40(entry + 20, size);
    put_be40(entry + 25, offset);
}

static void create_psarc(const char *path, int malformed) {
    static const char manifest[] = "dir/file.txt\nroot.bin\n";
    static const char first_file[] = "hello";
    static const char second_file[] = "ROOT";
    enum { ENTRY_COUNT = 3, TOC_LENGTH = 32 + ENTRY_COUNT * 30 + 3 };
    uint8_t archive[TOC_LENGTH + sizeof(manifest) - 1 +
                    sizeof(first_file) - 1 + sizeof(second_file) - 1];
    memset(archive, 0, sizeof(archive));
    memcpy(archive, "PSAR", 4);
    put_be32(archive + 4, UINT32_C(0x00010004));
    memcpy(archive + 8, "zlib", 4);
    put_be32(archive + 12, TOC_LENGTH);
    put_be32(archive + 16, 30);
    put_be32(archive + 20, ENTRY_COUNT);
    put_be32(archive + 24, 256);
    uint64_t manifest_offset = TOC_LENGTH;
    uint64_t first_offset = manifest_offset + sizeof(manifest) - 1;
    uint64_t second_offset = first_offset + sizeof(first_file) - 1;
    write_psarc_entry(archive + 32, 0, sizeof(manifest) - 1,
                      manifest_offset);
    write_psarc_entry(archive + 62, 1, sizeof(first_file) - 1,
                      malformed ? UINT64_C(0xFFFFFFFFFF) : first_offset);
    write_psarc_entry(archive + 92, 2, sizeof(second_file) - 1,
                      second_offset);
    memcpy(archive + TOC_LENGTH, manifest, sizeof(manifest) - 1);
    memcpy(archive + first_offset, first_file, sizeof(first_file) - 1);
    memcpy(archive + second_offset, second_file, sizeof(second_file) - 1);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(archive, 1, sizeof(archive), file) == sizeof(archive));
    assert(fclose(file) == 0);
}

static void prepare_fixture(void) {
    (void)remove("runtime-vfs-fixture/source.txt");
    (void)remove("runtime-vfs-fixture/new.bin");
    (void)remove("runtime-vfs-fixture/moved.bin");
    (void)remove("runtime-vfs-fixture/archive.psarc");
    (void)remove("runtime-vfs-fixture/malformed.psarc");
    (void)remove("runtime-vfs-fixture/escape");
    (void)remove_dir("runtime-vfs-fixture/sub");
    (void)remove_dir("runtime-vfs-fixture");
    assert(make_dir("runtime-vfs-fixture") == 0 || errno == EEXIST);
    assert(make_dir("runtime-vfs-fixture/sub") == 0 || errno == EEXIST);
    FILE *file = fopen("runtime-vfs-fixture/source.txt", "wb");
    assert(file);
    assert(fwrite("source", 1, 6, file) == 6);
    assert(fclose(file) == 0);
    create_psarc("runtime-vfs-fixture/archive.psarc", 0);
    create_psarc("runtime-vfs-fixture/malformed.psarc", 1);
}

static void test_fios(void) {
    const uint32_t path = MEMORY_BASE + 0x1900;
    const uint32_t out_handle = MEMORY_BASE + 0x1940;
    const uint32_t buffer = MEMORY_BASE + 0x1950;
    put_string(path, "data:source.txt");

    r0 = 0;
    vita_hle_sceFiosInitialize();
    assert(r0 == 0);
    r0 = 0;
    r1 = out_handle;
    r2 = path;
    r3 = 0;
    vita_hle_sceFiosFHOpenSync();
    assert(r0 == 0);
    uint32_t handle = vita_read32(out_handle);
    assert(handle != 0);

    vita_write32(sp, 6);
    vita_write32(sp + 4, 0);
    r0 = 0;
    r1 = handle;
    r2 = buffer;
    vita_hle_sceFiosFHReadSync();
    assert(r0 == 6 && r1 == 0);
    char contents[7] = {0};
    assert(vita_mem_copy_from_guest(contents, buffer, 6) == VITA_OK);
    assert(strcmp(contents, "source") == 0);
    r0 = 0;
    r1 = handle;
    vita_hle_sceFiosFHCloseSync();
    assert(r0 == 0);

    r0 = 7;
    r1 = out_handle;
    r2 = path;
    r3 = 0;
    vita_hle_sceFiosFHOpen();
    uint32_t operation = r0;
    assert(operation != 0);
    r0 = operation;
    vita_hle_sceFiosOpIsDone();
    assert(r0 == 1);
    r0 = operation;
    vita_hle_sceFiosOpGetError();
    assert(r0 == 0);
    handle = vita_read32(out_handle);
    r0 = 0;
    r1 = handle;
    vita_hle_sceFiosFHCloseSync();
    assert(r0 == 0);
    r0 = operation;
    vita_hle_sceFiosOpDelete();

    put_string(path, "data:../outside");
    r0 = 0;
    r1 = out_handle;
    r2 = path;
    r3 = 0;
    vita_hle_sceFiosFHOpenSync();
    assert((int32_t)r0 < 0);

    const uint32_t mount_point = MEMORY_BASE + 0x19A0;
    put_string(path, "data:archive.psarc");
    put_string(mount_point, "pack:");
    r0 = 0;
    r1 = out_handle;
    r2 = path;
    r3 = mount_point;
    vita_hle_sceFiosArchiveMountSync();
    assert(r0 == 0);
    uint32_t mount_handle = vita_read32(out_handle);
    put_string(path, "pack:dir/file.txt");
    r0 = 0;
    r1 = out_handle;
    r2 = path;
    r3 = 0;
    vita_hle_sceFiosFHOpenSync();
    assert(r0 == 0);
    handle = vita_read32(out_handle);
    vita_write32(sp, 5);
    vita_write32(sp + 4, 0);
    r0 = 0;
    r1 = handle;
    r2 = buffer;
    vita_hle_sceFiosFHReadSync();
    assert(r0 == 5 && r1 == 0);
    memset(contents, 0, sizeof(contents));
    assert(vita_mem_copy_from_guest(contents, buffer, 5) == VITA_OK);
    assert(strcmp(contents, "hello") == 0);

    r0 = 0;
    r1 = mount_handle;
    vita_hle_sceFiosArchiveUnmountSync();
    assert(r0 == 0);
    r0 = handle;
    vita_hle_sceFiosIsValidHandle();
    assert(r0 == 0);
    r0 = mount_handle;
    vita_hle_sceFiosIsValidHandle();
    assert(r0 == 0);
    vita_hle_sceFiosTerminate();
}

static void test_vfs_and_psarc(void) {
    prepare_fixture();
    assert(vita_vfs_mount("app0:", "runtime-vfs-fixture", 0) == VITA_OK);
    assert(vita_vfs_mount("data", "runtime-vfs-fixture", 0) == VITA_OK);
    char resolved[1024];
    assert(vita_vfs_resolve("data:../outside", VITA_VFS_OPEN_READ,
                            resolved, sizeof(resolved)) ==
           VITA_ERROR_TRAVERSAL);
    assert(vita_vfs_resolve("data:sub/../../outside", VITA_VFS_OPEN_READ,
                            resolved, sizeof(resolved)) ==
           VITA_ERROR_TRAVERSAL);
    assert(vita_vfs_resolve("data:C:/outside", VITA_VFS_OPEN_READ,
                            resolved, sizeof(resolved)) ==
           VITA_ERROR_INVALID_ARGUMENT);
    test_fios();

    vita_vfs_file *file = NULL;
    assert(vita_vfs_open("app0:source.txt", VITA_VFS_OPEN_WRITE,
                         &file) == VITA_ERROR_READ_ONLY);
    assert(vita_vfs_open("data:new.bin",
                         VITA_VFS_OPEN_READ | VITA_VFS_OPEN_WRITE |
                         VITA_VFS_OPEN_CREATE, &file) == VITA_OK);
    assert(vita_vfs_write(file, "abc", 3) == 3);
    uint64_t position = 99;
    assert(vita_vfs_seek(file, 0, SEEK_SET, &position) == VITA_OK);
    assert(position == 0);
    char contents[8] = {0};
    assert(vita_vfs_read(file, contents, 3) == 3);
    assert(strcmp(contents, "abc") == 0);
    vita_vfs_stat stat_value;
    assert(vita_vfs_file_stat(file, &stat_value) == VITA_OK);
    assert(stat_value.type == VITA_VFS_NODE_FILE && stat_value.size == 3);
    vita_vfs_close(file);

    assert(vita_vfs_open("data:new.bin",
                         VITA_VFS_OPEN_READ | VITA_VFS_OPEN_WRITE |
                         VITA_VFS_OPEN_CREATE, &file) == VITA_OK);
    assert(vita_vfs_write(file, "Z", 1) == 1);
    assert(vita_vfs_file_stat(file, &stat_value) == VITA_OK);
    assert(stat_value.size == 3);
    vita_vfs_close(file);
    assert(vita_vfs_rename("data:new.bin", "data:moved.bin") == VITA_OK);
    assert(vita_vfs_stat_path("data:moved.bin", &stat_value) == VITA_OK);

    vita_vfs_dir *dir = NULL;
    assert(vita_vfs_opendir("data:", &dir) == VITA_OK);
    int found_source = 0, found_moved = 0, found_sub = 0;
    char name[128];
    while (vita_vfs_readdir(dir, name, sizeof(name), &stat_value) == VITA_OK) {
        found_source |= strcmp(name, "source.txt") == 0;
        found_moved |= strcmp(name, "moved.bin") == 0;
        found_sub |= strcmp(name, "sub") == 0 &&
                     stat_value.type == VITA_VFS_NODE_DIRECTORY;
    }
    assert(found_source && found_moved && found_sub);
    vita_vfs_closedir(dir);

#ifndef _WIN32
    FILE *outside = fopen("runtime-vfs-outside.txt", "wb");
    assert(outside);
    assert(fwrite("outside", 1, 7, outside) == 7);
    assert(fclose(outside) == 0);
    assert(symlink("../runtime-vfs-outside.txt",
                   "runtime-vfs-fixture/escape") == 0);
    assert(vita_vfs_open("data:escape", VITA_VFS_OPEN_READ, &file) ==
           VITA_ERROR_TRAVERSAL);
    assert(remove("runtime-vfs-fixture/escape") == 0);
    assert(remove("runtime-vfs-outside.txt") == 0);
#endif

    assert(vita_vfs_mount_psarc("arc",
                                "runtime-vfs-fixture/archive.psarc") ==
           VITA_OK);
    assert(vita_vfs_mount_psarc("bad",
                                "runtime-vfs-fixture/malformed.psarc") ==
           VITA_ERROR_IO);
    assert(vita_vfs_open("arc:dir/file.txt", VITA_VFS_OPEN_READ,
                         &file) == VITA_OK);
    memset(contents, 0, sizeof(contents));
    assert(vita_vfs_read(file, contents, 2) == 2);
    assert(vita_vfs_read(file, contents + 2, 3) == 3);
    assert(strcmp(contents, "hello") == 0);
    assert(vita_vfs_seek(file, -2, SEEK_END, &position) == VITA_OK);
    char tail[3] = {0};
    assert(vita_vfs_read(file, tail, 2) == 2);
    assert(strcmp(tail, "lo") == 0);
    vita_vfs_close(file);
    assert(vita_vfs_stat_path("arc:dir", &stat_value) == VITA_OK);
    assert(stat_value.type == VITA_VFS_NODE_DIRECTORY);
    assert(vita_vfs_opendir("arc:", &dir) == VITA_OK);
    int found_archive_dir = 0, found_archive_file = 0;
    while (vita_vfs_readdir(dir, name, sizeof(name), &stat_value) == VITA_OK) {
        found_archive_dir |= strcmp(name, "dir") == 0 &&
                             stat_value.type == VITA_VFS_NODE_DIRECTORY;
        found_archive_file |= strcmp(name, "root.bin") == 0 &&
                              stat_value.size == 4;
    }
    assert(found_archive_dir && found_archive_file);
    vita_vfs_closedir(dir);
    assert(vita_vfs_opendir("arc:dir", &dir) == VITA_OK);
    assert(vita_vfs_readdir(dir, name, sizeof(name), &stat_value) == VITA_OK);
    assert(strcmp(name, "file.txt") == 0 && stat_value.size == 5);
    vita_vfs_closedir(dir);

    assert(vita_vfs_remove("data:moved.bin") == VITA_OK);
    vita_vfs_unmount_all();
    assert(remove("runtime-vfs-fixture/source.txt") == 0);
    assert(remove("runtime-vfs-fixture/archive.psarc") == 0);
    assert(remove("runtime-vfs-fixture/malformed.psarc") == 0);
    assert(remove_dir("runtime-vfs-fixture/sub") == 0);
    assert(remove_dir("runtime-vfs-fixture") == 0);
}

static void test_clock(void) {
    vita_clock_configure(1, 1000, 5);
    assert(vita_clock_is_virtual());
    assert(vita_clock_us() == 0);
    assert(vita_clock_us() == 5);
    vita_clock_advance(10);
    assert(vita_clock_us() == 20);
    assert(vita_clock_unix_us() == 1025);
    vita_clock_wait(10);
    assert(vita_clock_unix_us() == 1035);
    vita_clock_vblank();
    assert(vita_clock_unix_us() == 1000 + 16667);
}

static void test_tls_shutdown_regression(const vita_runtime_config *config) {
    assert(vita_runtime_init(config) == VITA_OK);
    vita_main_stack_init(MAIN_STACK, 0x10000);
    assert(vita_tls_base() != 0);
    vita_runtime_shutdown();
    assert(!vita_runtime_is_initialized());
}

int main(void) {
    configure_noninteractive_failures();
    vita_runtime_config config;
    memset(&config, 0, sizeof(config));
    config.memory_base = MEMORY_BASE;
    config.memory_size = MEMORY_SIZE;
    config.heap_base = HEAP_BASE;
    config.heap_size = HEAP_SIZE;
    config.sysmem_base = SYSMEM_BASE;
    config.sysmem_size = SYSMEM_SIZE;
    config.kernel_base = KERNEL_BASE;
    config.kernel_size = KERNEL_SIZE;
    config.virtual_clock = 1;
    config.virtual_epoch_us = 1000;
    config.virtual_read_step_us = 5;
    config.diagnostic = diagnostic;
    config.diagnostic_userdata = &g_diagnostics;

    assert(vita_runtime_init(&config) == VITA_OK);
    assert(vita_runtime_init(&config) == VITA_ERROR_ALREADY_INITIALIZED);
    test_memory();
    test_allocators_and_libc();
    test_imports();
    test_threads_sync_and_callbacks();
    test_ult();
    test_vfs_and_psarc();
    test_clock();

    vita_runtime_counters counters;
    vita_runtime_get_counters(&counters);
    assert(counters.imports_dispatched == 2);
    assert(counters.imports_unimplemented == 1);
    assert(counters.threads_created == 1);
    assert(counters.threads_finished == 1);
    assert(counters.callbacks_run == 1);
    assert(counters.live_threads == 0);
    assert(counters.live_files == 0);
    assert(counters.live_sync_objects == 0);
    assert(counters.live_callbacks == 0);

    r0 = UINT32_C(0x3001);
    r1 = UINT32_C(0xD35A0001);
    r2 = 1;
    vita_hle___cxa_atexit();
    assert(r0 == 0);
    vita_runtime_shutdown();
    assert(g_destructor_argument == UINT32_C(0xD35A0001));
    assert(!vita_runtime_is_initialized());
    vita_runtime_get_counters(&counters);
    assert(counters.shutdowns == 1);
    assert(counters.live_threads == 0 && counters.live_files == 0 &&
           counters.live_sync_objects == 0 && counters.live_callbacks == 0);
    vita_runtime_shutdown();
    vita_runtime_get_counters(&counters);
    assert(counters.shutdowns == 1);

    test_tls_shutdown_regression(&config);
    puts("runtime/HLE foundation tests passed");
    return 0;
}
