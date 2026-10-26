/* sched.c — guest threads on the host. See sched.h.
 *
 * C11 threads: one host thread per guest thread, a single mutex, and one
 * condition variable per thread that it sleeps on while another holds the
 * baton. The baton is `g_cur`; the thread that holds it is the only one
 * touching the register globals or guest memory.
 */

#include "clock.h"
#include "sched.h"
#include "hle_sysmem.h"
#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/vfp.h"
#include "runtime_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define MAX_THREADS      64
#define THREAD_UID_FIRST 0x40010100u      /* the main thread's; the rest follow */

#define SCE_KERNEL_ERROR_INVALID_UID      0x80024501u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT     0x80028005u
#define SCE_KERNEL_ERROR_NO_MEMORY        0x8002710Bu
#define SCE_KERNEL_ERROR_NOT_DORMANT      0x80028013u

void vita_thread_tls_release(uint32_t *slot);
void vita_libc_thread_shutdown(void);

enum { T_FREE = 0, T_NEW, T_READY, T_RUNNING, T_WAITING, T_DONE };

typedef struct {
    uint32_t r[13], sp, lr;
    int      n, z, c, v, ge;
    vfp_reg  vfp[64];
    int      fn, fz, fc, fv;
    uint32_t fpscr_control;
    uint32_t last_pc;
    int      trace_insns;
    uint32_t trace_lo, trace_hi;
    int32_t  slice;
} regs_t;

typedef struct vthread_record {
    int      state;
    uint32_t uid;
    char     name[32];
    uint32_t entry, prio, stack_base, stack_size, tls;
    int      wait_kind;
    uint32_t wait_obj;
    uint64_t wait_until;         /* 0: no timeout                        */
    int      timed_out;
    int      exit_status;
    regs_t   regs;
    thrd_t   host;
    int      host_started;
    int      finished_counted;
    cnd_t    wake;
    int      go;
    int      parked;
    vita_guest_boundary_coordinator *boundary;
    struct vthread_record *boundary_owner;
} vthread;

static vthread  g_t[MAX_THREADS];
static mtx_t    g_mtx;
static mtx_t    g_boot_lock;
static cnd_t    g_boot_wake;
static once_flag g_boot_once = ONCE_FLAG_INIT;
static vthread *g_cur;
static int      g_init;
static int      g_shutdown;
static int      g_quiescing;
static int      g_quiesced;
static void   (*g_leave)(void);     /* what the running thread does before the baton leaves it (sched.h) */
static VITA_TLS vthread *g_host_thread;
static VITA_TLS jmp_buf *g_worker_stop;

void vita_sched_on_handover(void (*leave)(void)) { g_leave = leave; }

static uint64_t now_us(void) { return vita_clock_us(); }

/* A slice is a count of backward branches, whose host duration varies by
 * machine and instrumentation. With a real clock the count adapts toward a
 * bounded wall-clock slice so latency-sensitive workers get regular turns.
 * With a virtual clock computation advances no time, so the fixed count makes
 * scheduling deterministic. */
#define SLICE_US    3000u
#define SLICE_LEAST 250
static int32_t  g_slice_len = VITA_SLICE;
static uint64_t g_slice_began;

static void slice_begun(void) {
    vita_slice = g_slice_len;
    g_slice_began = now_us();
}

static void slice_measured(void) {
    uint64_t took = now_us() - g_slice_began;
    if (took > SLICE_US) {
        int64_t n = (int64_t)g_slice_len * SLICE_US / (int64_t)took;
        if (n < g_slice_len / 4) n = g_slice_len / 4;       /* one long call from the library is not the measure of a loop */
        g_slice_len = n < SLICE_LEAST ? SLICE_LEAST : (int32_t)n;
    } else if (took < SLICE_US / 4 && g_slice_len < VITA_SLICE) {
        g_slice_len = g_slice_len > VITA_SLICE / 2 ? VITA_SLICE : g_slice_len * 2;
    }
}

static void save_regs(vthread *t) {
    uint32_t *rr = t->regs.r;
    rr[0] = r0; rr[1] = r1; rr[2] = r2;  rr[3] = r3;  rr[4] = r4;  rr[5] = r5; rr[6] = r6;
    rr[7] = r7; rr[8] = r8; rr[9] = r9; rr[10] = r10; rr[11] = r11; rr[12] = r12;
    t->regs.sp = sp; t->regs.lr = lr;
    t->regs.n = flag_n; t->regs.z = flag_z; t->regs.c = flag_c; t->regs.v = flag_v; t->regs.ge = flag_ge;
    memcpy(t->regs.vfp, vfp_s, sizeof(vfp_s));
    t->regs.fn = fpscr_n; t->regs.fz = fpscr_z; t->regs.fc = fpscr_c; t->regs.fv = fpscr_v;
    t->regs.fpscr_control = fpscr_control;
    t->regs.last_pc = vita_last_pc;
    t->regs.trace_insns = vita_trace_insns;
    t->regs.trace_lo = vita_trace_lo;
    t->regs.trace_hi = vita_trace_hi;
    t->regs.slice = vita_slice;
}

static void load_regs(const vthread *t) {
    const uint32_t *rr = t->regs.r;
    r0 = rr[0]; r1 = rr[1]; r2 = rr[2];  r3 = rr[3];  r4 = rr[4];  r5 = rr[5]; r6 = rr[6];
    r7 = rr[7]; r8 = rr[8]; r9 = rr[9]; r10 = rr[10]; r11 = rr[11]; r12 = rr[12];
    sp = t->regs.sp; lr = t->regs.lr;
    flag_n = t->regs.n; flag_z = t->regs.z; flag_c = t->regs.c; flag_v = t->regs.v; flag_ge = t->regs.ge;
    memcpy(vfp_s, t->regs.vfp, sizeof(vfp_s));
    fpscr_n = t->regs.fn; fpscr_z = t->regs.fz; fpscr_c = t->regs.fc; fpscr_v = t->regs.fv;
    fpscr_control = t->regs.fpscr_control;
    vita_last_pc = t->regs.last_pc;
    vita_trace_insns = t->regs.trace_insns;
    vita_trace_lo = t->regs.trace_lo;
    vita_trace_hi = t->regs.trace_hi;
    vita_slice = t->regs.slice;
}

/* The main thread is the one that called first; it already runs. */
static void boot_once(void) {
    (void)mtx_init(&g_boot_lock, mtx_plain);
    (void)cnd_init(&g_boot_wake);
}

static void init_once(void) {
    call_once(&g_boot_once, boot_once);
    mtx_lock(&g_boot_lock);
    if (g_init) {
        mtx_unlock(&g_boot_lock);
        return;
    }
    (void)mtx_init(&g_mtx, mtx_plain);
    g_shutdown = 0;
    g_quiescing = 0;
    g_quiesced = 0;
    vthread *m = &g_t[0];
    memset(m, 0, sizeof(*m));
    m->state = T_RUNNING;
    m->uid = THREAD_UID_FIRST;
    snprintf(m->name, sizeof(m->name), "main");
    m->prio = 0x40;
    m->host = thrd_current();
    (void)cnd_init(&m->wake);
    g_cur = m;
    g_host_thread = m;
    g_init = 1;
    mtx_unlock(&g_boot_lock);
}

static vthread *by_uid(uint32_t uid) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_t[i].state != T_FREE && g_t[i].uid == uid) return &g_t[i];
    return NULL;
}

uint32_t vita_thread_self(void) { init_once(); return g_cur->uid; }

static int main_is_only_thread(void) {
    if (g_cur != &g_t[0] || g_t[0].state != T_RUNNING)
        return 0;
    for (int i = 1; i < MAX_THREADS; i++)
        if (g_t[i].state != T_FREE) return 0;
    return 1;
}

vita_result vita_main_stack_prepare(uint32_t base, uint32_t size,
                                    uint32_t *initial_stack_pointer) {
    if (!vita_runtime_is_initialized())
        return VITA_ERROR_NOT_INITIALIZED;
    if (!size || (base & 15u) != 0 || (size & 15u) != 0)
        return VITA_ERROR_INVALID_ARGUMENT;
    const uint64_t end = (uint64_t)base + size;
    if (end > UINT32_MAX)
        return VITA_ERROR_OVERFLOW;
    void *memory = NULL;
    if (vita_mem_range(base, size, &memory) != VITA_OK)
        return VITA_ERROR_OUT_OF_BOUNDS;
    (void)memory;

    init_once();
    mtx_lock(&g_mtx);
    if (!main_is_only_thread()) {
        mtx_unlock(&g_mtx);
        return VITA_ERROR_BUSY;
    }
    g_t[0].stack_base = base;
    g_t[0].stack_size = size;
    mtx_unlock(&g_mtx);
    if (initial_stack_pointer)
        *initial_stack_pointer = (uint32_t)end;
    return VITA_OK;
}

void vita_main_stack_init(uint32_t base, uint32_t size) {
    (void)vita_main_stack_prepare(base, size, NULL);
}

uint32_t *vita_thread_tls_slot(uint32_t uid) {
    init_once();
    vthread *t = by_uid(uid);
    return t ? &t->tls : NULL;
}

vita_result vita_sched_validate_main_stack_pointer(uint32_t stack_pointer) {
    init_once();
    mtx_lock(&g_mtx);
    if (!main_is_only_thread()) {
        mtx_unlock(&g_mtx);
        return VITA_ERROR_BUSY;
    }
    const uint64_t stack_end =
        (uint64_t)g_t[0].stack_base + g_t[0].stack_size;
    const int valid =
        g_t[0].stack_size >= 16u &&
        (uint64_t)stack_pointer >= (uint64_t)g_t[0].stack_base + 16u &&
        (uint64_t)stack_pointer <= stack_end;
    mtx_unlock(&g_mtx);
    return valid ? VITA_OK : VITA_ERROR_OUT_OF_BOUNDS;
}

vita_result vita_sched_restart_tls(void) {
    init_once();
    mtx_lock(&g_mtx);
    if (!main_is_only_thread()) {
        mtx_unlock(&g_mtx);
        return VITA_ERROR_BUSY;
    }
    uint32_t tls = g_t[0].tls;
    g_t[0].tls = 0;
    mtx_unlock(&g_mtx);
    vita_thread_tls_release(&tls);
    return VITA_OK;
}

vita_result vita_sched_prepare_main_execution(void) {
    init_once();
    mtx_lock(&g_mtx);
    if (!main_is_only_thread()) {
        mtx_unlock(&g_mtx);
        return VITA_ERROR_BUSY;
    }
    uint32_t tls = g_t[0].tls;
    g_t[0].tls = 0;
    memset(&g_t[0].regs, 0, sizeof(g_t[0].regs));
    g_t[0].wait_kind = VW_NONE;
    g_t[0].wait_obj = 0;
    g_t[0].wait_until = 0;
    g_t[0].timed_out = 0;
    g_t[0].exit_status = 0;
    g_t[0].go = 0;
    mtx_unlock(&g_mtx);
    vita_thread_tls_release(&tls);
    vita_libc_thread_shutdown();
    return VITA_OK;
}

uint32_t vita_sched_threads(uint32_t *runnable) {
    uint32_t n = 0, run = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_t[i].state == T_FREE) continue;
        n++;
        if (g_t[i].state == T_READY || g_t[i].state == T_RUNNING) run++;
    }
    if (runnable) *runnable = run;
    return n;
}

void vita_sched_dump(FILE *f) {
    static const char *const WAITS[] = { "nothing", "semaphore", "mutex", "condition", "the end of thread", "the clock", "SceUlt object",
                                         "event flag", "message pipe" };
    init_once();
    /* not under the lock: this runs on the thread that holds the baton, at exit */
    uint64_t now = now_us();
    for (int i = 0; i < MAX_THREADS; i++) {
        const vthread *t = &g_t[i];
        if (t->state == T_FREE) continue;
        uint32_t pc = t == g_cur ? vita_last_pc : t->regs.last_pc;
        fprintf(f, "  thread 0x%08X %-20s ", t->uid, t->name);
        if (t->state == T_WAITING) {
            int kind = t->wait_kind >= 0 && t->wait_kind <= VW_MSG_PIPE ? t->wait_kind : 0;
            fprintf(f, "waits on %s", WAITS[kind]);
            if (kind != VW_DELAY) fprintf(f, " 0x%08X", t->wait_obj);
            if (t->wait_until) fprintf(f, ", %llu ms more at most", (unsigned long long)(t->wait_until > now ? (t->wait_until - now) / 1000 : 0));
        } else {
            fprintf(f, "%s", t->state == T_RUNNING ? "running" : t->state == T_READY ? "ready" : t->state == T_NEW ? "not started" : "ended");
        }
        fprintf(f, ", last at 0x%08X\n", pc);
    }
}

uint32_t vita_sched_running(char *name, uint32_t cap) {
    init_once();
    mtx_lock(&g_mtx);
    const vthread *t = g_cur;
    uint32_t uid = t->state == T_RUNNING ? t->uid : 0;
    if (name && cap) snprintf(name, cap, "%s", uid ? t->name : "(idle)");
    mtx_unlock(&g_mtx);
    return uid;
}

/* Timed waiters whose time has come become runnable; returns the earliest
 * time still pending, or 0. */
static uint64_t expire(uint64_t now) {
    uint64_t earliest = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *t = &g_t[i];
        if (t->state != T_WAITING || !t->wait_until) continue;
        if (t->wait_until <= now) { t->state = T_READY; t->timed_out = 1; t->wait_until = 0; }
        else if (!earliest || t->wait_until < earliest) earliest = t->wait_until;
    }
    return earliest;
}

/* The next thread to run after `cur`, round-robin. When nothing is runnable
 * the host sleeps until the earliest timed wait, and when there is none of
 * those either the program has deadlocked, which is said rather than spun
 * on. Called with the mutex held. */
static vthread *pick_next(vthread *cur) {
    for (;;) {
        if (g_shutdown)
            return cur;
        uint64_t earliest = expire(now_us());
        int start = (int)(cur - g_t);
        for (int k = 1; k <= MAX_THREADS; k++) {
            vthread *t = &g_t[(start + k) % MAX_THREADS];
            if (t->state == T_READY) return t;
        }
        if (cur->state == T_READY || cur->state == T_RUNNING) return cur;
        if (!earliest) {
            uint32_t n = 0, run = 0;
            n = vita_sched_threads(&run);
            fprintf(stderr, "vitarecomp: every thread is waiting (%u threads); the program is deadlocked\n", n);
            for (int i = 0; i < MAX_THREADS; i++)
                if (g_t[i].state == T_WAITING)
                    fprintf(stderr, "  %s (0x%08X) waits on kind %d object 0x%08X\n",
                            g_t[i].name, g_t[i].uid, g_t[i].wait_kind, g_t[i].wait_obj);
            mtx_unlock(&g_mtx);
            vita_trap_unimpl(0, n, "deadlock: every thread is waiting");
            vita_host_exit(3);
            mtx_lock(&g_mtx);
            cur->state = T_RUNNING;
            cur->timed_out = 1;
            return cur;
        }
        uint64_t now = now_us();
        if (earliest > now) {
            uint64_t d = earliest - now;
            mtx_unlock(&g_mtx);
            vita_clock_wait(d);
            mtx_lock(&g_mtx);
        }
    }
}

/* Hand the baton from the running thread to `next` and sleep until it comes
 * back. Called with the mutex held; returns with it held and reports whether
 * normal guest execution may resume. */
static int switch_to(vthread *cur, vthread *next) {
    if (g_shutdown)
        return 0;
    if (next == cur) {
        cur->state = T_RUNNING;
        return 1;
    }
    if (g_leave) {
        void (*leave)(void) = g_leave;
        mtx_unlock(&g_mtx);
        leave();
        mtx_lock(&g_mtx);
        if (g_shutdown)
            return 0;
    }
    save_regs(cur);
    next->state = T_RUNNING;
    g_cur = next;
    next->go = 1;
    cnd_signal(&next->wake);
    cur->parked = 1;
    while (!cur->go && !g_shutdown)
        cnd_wait(&cur->wake, &g_mtx);
    cur->parked = 0;
    cnd_broadcast(&cur->wake);
    if (g_shutdown)
        return 0;
    cur->go = 0;
    cur->state = T_RUNNING;
    slice_begun();
    return 1;
}

static int mark_worker_finished_locked(vthread *thread, int status) {
    int count_finished = 0;
    thread->state = T_DONE;
    thread->exit_status = status;
    thread->wait_kind = VW_NONE;
    thread->wait_obj = 0;
    thread->wait_until = 0;
    if (!thread->finished_counted) {
        thread->finished_counted = 1;
        count_finished = 1;
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *waiter = &g_t[i];
        if (waiter->state == T_WAITING &&
            waiter->wait_kind == VW_THREAD_END &&
            waiter->wait_obj == thread->uid) {
            waiter->state = T_READY;
            waiter->wait_until = 0;
        }
    }
    return count_finished;
}

static void worker_host_release(vthread *thread, int count_finished) {
    vita_guest_boundary_coordinator *boundary = thread->boundary;
    thread->boundary = NULL;
    thread->boundary_owner = NULL;
    if (count_finished)
        vita_runtime_count(VITA_COUNTER_THREAD_FINISHED, 1);
    vita_guest_boundary_release(boundary);
    vita_libc_thread_shutdown();
    g_worker_stop = NULL;
    g_host_thread = NULL;
}

static int worker_cancelled(vthread *thread) {
    void (*leave)(void) = NULL;
    int count_finished;

    mtx_lock(&g_mtx);
    if (g_cur == thread) {
        save_regs(thread);
        leave = g_leave;
    }
    count_finished = mark_worker_finished_locked(thread,
                                                 VITA_ERROR_SHUTDOWN);
    mtx_unlock(&g_mtx);
    if (leave)
        leave();
    worker_host_release(thread, count_finished);
    return 0;
}

static void worker_finish(vthread *thread, int status,
                          vthread *preferred_next) {
    void (*leave)(void) = NULL;
    int count_finished;

    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return;
    }
    save_regs(thread);
    count_finished = mark_worker_finished_locked(thread, status);
    leave = g_leave;
    if (leave) {
        mtx_unlock(&g_mtx);
        leave();
        mtx_lock(&g_mtx);
    }
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        worker_host_release(thread, count_finished);
        thrd_exit(0);
    }

    vthread *next = preferred_next;
    if (!next || next == thread || next->state == T_FREE ||
        next->state == T_DONE)
        next = pick_next(thread);
    if (next == thread) {
        mtx_unlock(&g_mtx);
        worker_host_release(thread, count_finished);
        thrd_exit(0);
    }
    next->state = T_RUNNING;
    next->wait_until = 0;
    g_cur = next;
    next->go = 1;
    cnd_signal(&next->wake);
    mtx_unlock(&g_mtx);
    worker_host_release(thread, count_finished);
    thrd_exit(0);
}

static void worker_dispatch(void *userdata) {
    vthread *thread = (vthread *)userdata;
    vita_dispatch(0, thread->entry);
}

/* A new guest thread's host thread: waits for its first turn, then runs the
 * entry through the dispatch table with the registers switch_to loaded. The
 * outer jump is local to this host thread and is used only for scheduler
 * cancellation; strict failures use the worker boundary's own local jump. */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4702)
#endif
static int thread_main(void *arg) {
    vthread *t = (vthread *)arg;
    jmp_buf stop;

    g_host_thread = t;
    g_worker_stop = &stop;
    if (setjmp(stop) != 0)
        return worker_cancelled(t);

    mtx_lock(&g_mtx);
    while (!t->go && !g_shutdown)
        cnd_wait(&t->wake, &g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        return worker_cancelled(t);
    }
    t->go = 0;
    t->state = T_RUNNING;
    load_regs(t);
    slice_begun();
    mtx_unlock(&g_mtx);

    if (vita_guest_boundary_run_worker(t->boundary, worker_dispatch, t))
        worker_finish(t, 0, t->boundary_owner);
    vita_thread_exit((int)r0);
    return 0;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

uint32_t vita_thread_create(const char *name, uint32_t entry, uint32_t prio, uint32_t stack_size) {
    init_once();
    if (stack_size > UINT32_MAX - 0xFFFu) return 0;
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        return 0;
    }
    int i;
    for (i = 0; i < MAX_THREADS && g_t[i].state != T_FREE; i++) {}
    if (i == MAX_THREADS) {
        mtx_unlock(&g_mtx);
        return 0;
    }
    vthread *t = &g_t[i];
    memset(t, 0, sizeof(*t));
    t->state = T_NEW;
    t->uid = THREAD_UID_FIRST + (uint32_t)i;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "");
    t->entry = entry;
    t->prio = prio;
    t->stack_size = (stack_size + 0xFFFu) & ~0xFFFu;
    if (t->stack_size < 0x1000) t->stack_size = 0x1000;
    (void)cnd_init(&t->wake);
    mtx_unlock(&g_mtx);
    return t->uid;
}

int vita_thread_start(uint32_t uid, uint32_t arglen, uint32_t argp) {
    vita_guest_boundary_coordinator *boundary =
        vita_guest_boundary_retain_current();
    int error = 0;
    int switched;

    init_once();
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        error = VITA_ERROR_SHUTDOWN;
        goto fail;
    }
    vthread *t = by_uid(uid);
    if (!t) {
        error = (int)SCE_KERNEL_ERROR_INVALID_UID;
        goto fail;
    }
    if (t->state != T_NEW) {
        error = (int)SCE_KERNEL_ERROR_NOT_DORMANT;
        goto fail;
    }
    if (!t->stack_base) {
        t->stack_base = vita_sysmem_reserve(t->stack_size);
        if (!t->stack_base) {
            error = (int)SCE_KERNEL_ERROR_NO_MEMORY;
            goto fail;
        }
    }
    /* The arguments are copied onto the new stack, as the kernel does, and
     * the entry gets their length and the copy's address. */
    uint32_t top = t->stack_base + t->stack_size;
    uint32_t args = 0;
    if (arglen && argp) {
        if (arglen > t->stack_size - 16u ||
            !vita_mem_ptr(argp, arglen)) {
            error = (int)SCE_KERNEL_ERROR_NO_MEMORY;
            goto fail;
        }
        args = (top - arglen) & ~15u;
        void *d = vita_mem_ptr(args, arglen);
        const void *s = vita_mem_ptr(argp, arglen);
        if (d && s) {
            (void)vita_mem_move_guest(args, argp, arglen);
        } else {
            error = (int)SCE_KERNEL_ERROR_NO_MEMORY;
            goto fail;
        }
        top = args;
    }
    memset(&t->regs, 0, sizeof(t->regs));
    t->regs.r[0] = arglen;
    t->regs.r[1] = args;
    t->regs.sp = top - 16;
    t->regs.lr = 0;
    t->state = T_READY;
    t->boundary = boundary;
    t->boundary_owner =
        g_cur->boundary_owner ? g_cur->boundary_owner : g_cur;

    if (thrd_create(&t->host, thread_main, t) != thrd_success) {
        t->state = T_NEW;
        t->boundary = NULL;
        t->boundary_owner = NULL;
        error = (int)SCE_KERNEL_ERROR_NO_MEMORY;
        goto fail;
    }
    t->host_started = 1;
    vita_runtime_count(VITA_COUNTER_THREAD_CREATED, 1);
    mtx_unlock(&g_mtx);
    vita_hle_trace("thread \"%s\" (0x%08X) starts at 0x%08X, stack 0x%08X+0x%X", t->name, t->uid, t->entry, t->stack_base, t->stack_size);

    /* The new thread runs first, up to its first wait: that is what a
     * higher-priority thread would do, and it is what lets a worker set
     * itself up before the thread that made it goes on. */
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return VITA_ERROR_SHUTDOWN;
    }
    vthread *cur = g_cur;
    cur->state = T_READY;
    switched = switch_to(cur, t);
    mtx_unlock(&g_mtx);
    if (!switched) {
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return VITA_ERROR_SHUTDOWN;
    }
    vita_guest_boundary_poll();
    return 0;

fail:
    mtx_unlock(&g_mtx);
    vita_guest_boundary_release(boundary);
    return error;
}

void vita_thread_exit(int status) {
    init_once();
    vthread *cur = g_host_thread ? g_host_thread : g_cur;
    if (cur != &g_t[0])
        vita_hle_trace("thread \"%s\" (0x%08X) ends with %d",
                       cur->name, cur->uid, status);
    if (cur == &g_t[0]) {
        vita_host_exit(status);
        return;
    }
    worker_finish(cur, status, NULL);
}

int vita_thread_wait_end(uint32_t uid, uint32_t timeout_us, int *status) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    if (t->state != T_DONE) {
        if (vita_sched_block(VW_THREAD_END, uid, timeout_us) < 0) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        t = by_uid(uid);
        if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    }
    if (status) *status = t->exit_status;
    return 0;
}

int vita_thread_delete(uint32_t uid) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    if (t->state == T_NEW || t->state == T_DONE) {
        if (t->host_started) {
            int host_result = 0;
            (void)thrd_join(t->host, &host_result);
        }
        vita_thread_tls_release(&t->tls);
        vita_sysmem_release(t->stack_base);
        cnd_destroy(&t->wake);
        memset(t, 0, sizeof(*t));
        return 0;
    }
    return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
}

int vita_thread_info(uint32_t uid, char *name, uint32_t cap, uint32_t *entry,
                     uint32_t *stack, uint32_t *stack_size, uint32_t *prio) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return -1;
    if (name && cap) snprintf(name, cap, "%s", t->name);
    if (entry) *entry = t->entry;
    if (stack) *stack = t->stack_base;
    if (stack_size) *stack_size = t->stack_size;
    if (prio) *prio = t->prio;
    return 0;
}

int vita_sched_block(int kind, uint32_t obj, uint32_t timeout_us) {
    init_once();
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return -1;
    }
    vthread *cur = g_cur;
    cur->state = T_WAITING;
    cur->wait_kind = kind;
    cur->wait_obj = obj;
    if (timeout_us) {
        uint64_t now = now_us();
        cur->wait_until = timeout_us > UINT64_MAX - now
                        ? UINT64_MAX : now + timeout_us;
    } else {
        cur->wait_until = 0;
    }
    cur->timed_out = 0;
    vthread *next = pick_next(cur);
    int switched = switch_to(cur, next);
    if (!switched) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return -1;
    }
    int r = cur->timed_out ? -1 : 0;
    cur->wait_kind = VW_NONE;
    mtx_unlock(&g_mtx);
    vita_guest_boundary_poll();
    return r;
}

void vita_sched_wake(int kind, uint32_t obj, int all) {
    init_once();
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return;
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *t = &g_t[i];
        if (t->state != T_WAITING || t->wait_kind != kind || t->wait_obj != obj) continue;
        t->state = T_READY;
        t->wait_until = 0;
        t->timed_out = 0;
        if (!all) break;
    }
    mtx_unlock(&g_mtx);
}

void vita_sched_yield(void) {
    init_once();
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return;
    }
    vthread *cur = g_cur;
    cur->state = T_READY;
    vthread *next = pick_next(cur);
    int switched = switch_to(cur, next);
    mtx_unlock(&g_mtx);
    if (!switched) {
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return;
    }
    vita_guest_boundary_poll();
}

/* The running thread's slice ran out: the other runnable threads go, each
 * for a slice of its own, and this one resumes when the round comes back.
 * Timed waits that have come due count as runnable, so a thread that
 * computes without a firmware call no longer holds a sleeper past its time. */
void vita_sched_slice_end_impl(void) {
    slice_measured();
    slice_begun();
    if (!g_init) return;                   /* one thread, and no one to give way to */
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return;
    }
    vthread *cur = g_cur;
    (void)expire(now_us());
    int start = (int)(cur - g_t);
    for (int k = 1; k < MAX_THREADS; k++) {
        vthread *t = &g_t[(start + k) % MAX_THREADS];
        if (t->state != T_READY) continue;
        cur->state = T_READY;
        int switched = switch_to(cur, t);
        if (!switched) {
            mtx_unlock(&g_mtx);
            if (g_worker_stop)
                longjmp(*g_worker_stop, 1);
            return;
        }
        break;
    }
    mtx_unlock(&g_mtx);
    vita_guest_boundary_poll();
}

/* The running thread is in a loop that only another thread can end (see
 * recomp_rt.h). Another runnable thread goes at once. With none, a sleeper
 * on the clock may still end it, so the host sleeps a little and the loop
 * looks again; with none of those either nothing can ever change the word,
 * which is a deadlock and is said. */
void vita_sched_spin_wait_impl(uint32_t addr) {
    init_once();
    mtx_lock(&g_mtx);
    if (g_shutdown) {
        mtx_unlock(&g_mtx);
        if (g_worker_stop)
            longjmp(*g_worker_stop, 1);
        return;
    }
    vthread *cur = g_cur;
    uint64_t now = now_us(), earliest = expire(now);
    int start = (int)(cur - g_t);
    for (int k = 1; k < MAX_THREADS; k++) {
        vthread *t = &g_t[(start + k) % MAX_THREADS];
        if (t->state != T_READY) continue;
        cur->state = T_READY;
        int switched = switch_to(cur, t);
        mtx_unlock(&g_mtx);
        if (!switched) {
            if (g_worker_stop)
                longjmp(*g_worker_stop, 1);
            return;
        }
        vita_guest_boundary_poll();
        return;
    }
    if (!earliest) {
        uint32_t n = vita_sched_threads(NULL);
        fprintf(stderr, "vitarecomp: \"%s\" waits at 0x%08X on memory that no thread can change (%u threads, the rest waiting); the program is deadlocked\n",
                cur->name, addr, n);
        for (int i = 0; i < MAX_THREADS; i++)
            if (g_t[i].state == T_WAITING)
                fprintf(stderr, "  %s (0x%08X) waits on kind %d object 0x%08X\n",
                        g_t[i].name, g_t[i].uid, g_t[i].wait_kind, g_t[i].wait_obj);
        mtx_unlock(&g_mtx);
        vita_trap_unimpl(addr, n, "deadlock: a spin on memory no thread can change");
        vita_host_exit(3);
        return;
    }
    mtx_unlock(&g_mtx);
    uint64_t d = earliest > now ? earliest - now : 0;
    if (d > 1000) d = 1000;
    if (d) vita_clock_wait(d);
}

void vita_sched_delay(uint32_t us) {
    if (us == 0) { vita_sched_yield(); return; }
    (void)vita_sched_block(VW_DELAY, 0, us);
}

int vita_sched_is_shutting_down(void) {
    int shutting_down;
    call_once(&g_boot_once, boot_once);
    mtx_lock(&g_boot_lock);
    if (!g_init) {
        mtx_unlock(&g_boot_lock);
        return 1;
    }
    mtx_lock(&g_mtx);
    shutting_down = g_shutdown;
    mtx_unlock(&g_mtx);
    mtx_unlock(&g_boot_lock);
    return shutting_down;
}

void vita_sched_quiesce(void) {
    int caller_is_main;
    call_once(&g_boot_once, boot_once);
    mtx_lock(&g_boot_lock);
    while (g_quiescing)
        cnd_wait(&g_boot_wake, &g_boot_lock);
    if (!g_init) {
        mtx_unlock(&g_boot_lock);
        return;
    }
    if (g_quiesced) {
        mtx_unlock(&g_boot_lock);
        return;
    }
    g_quiescing = 1;
    caller_is_main = thrd_equal(g_t[0].host, thrd_current());
    mtx_unlock(&g_boot_lock);

    mtx_lock(&g_mtx);
    g_shutdown = 1;
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *thread = &g_t[i];
        if (thread->state == T_FREE)
            continue;
        thread->go = 1;
        cnd_broadcast(&thread->wake);
    }
    mtx_unlock(&g_mtx);

    for (int i = 1; i < MAX_THREADS; i++) {
        vthread *thread = &g_t[i];
        if (thread->host_started) {
            int host_result = 0;
            (void)thrd_join(thread->host, &host_result);
            thread->host_started = 0;
        }
    }

    mtx_lock(&g_mtx);
    while (!caller_is_main && g_t[0].parked)
        cnd_wait(&g_t[0].wake, &g_mtx);
    g_cur = &g_t[0];
    g_t[0].state = T_RUNNING;
    g_t[0].wait_kind = VW_NONE;
    g_t[0].wait_obj = 0;
    g_t[0].wait_until = 0;
    g_t[0].go = 0;
    g_t[0].parked = 0;
    mtx_unlock(&g_mtx);

    mtx_lock(&g_boot_lock);
    g_quiescing = 0;
    g_quiesced = 1;
    cnd_broadcast(&g_boot_wake);
    mtx_unlock(&g_boot_lock);
}

void vita_sched_shutdown(void) {
    vita_sched_quiesce();
    call_once(&g_boot_once, boot_once);
    mtx_lock(&g_boot_lock);
    while (g_quiescing)
        cnd_wait(&g_boot_wake, &g_boot_lock);
    if (!g_init) {
        mtx_unlock(&g_boot_lock);
        return;
    }

    for (int i = 1; i < MAX_THREADS; i++) {
        vthread *thread = &g_t[i];
        if (thread->state == T_FREE)
            continue;
        vita_guest_boundary_release(thread->boundary);
        thread->boundary = NULL;
        vita_thread_tls_release(&thread->tls);
        vita_sysmem_release(thread->stack_base);
        cnd_destroy(&thread->wake);
        memset(thread, 0, sizeof(*thread));
    }
    vita_thread_tls_release(&g_t[0].tls);
    cnd_destroy(&g_t[0].wake);
    memset(g_t, 0, sizeof(g_t));
    g_cur = NULL;
    g_leave = NULL;
    g_slice_len = VITA_SLICE;
    g_slice_began = 0;
    g_init = 0;
    g_shutdown = 0;
    g_quiescing = 0;
    g_quiesced = 0;
    g_host_thread = NULL;
    mtx_destroy(&g_mtx);
    mtx_unlock(&g_boot_lock);
}
