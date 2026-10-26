/* cpu.c — the machine state the generated C operates on. */

#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#include <stdio.h>
#include <string.h>

VITA_TLS uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
VITA_TLS uint32_t r8, r9, r10, r11, r12;
VITA_TLS uint32_t sp, lr;

VITA_TLS int flag_n, flag_z, flag_c, flag_v;
VITA_TLS int flag_ge;

VITA_TLS uint32_t vita_last_pc;
VITA_TLS int      vita_trace_insns;
VITA_TLS uint32_t vita_trace_lo, vita_trace_hi = 0xFFFFFFFFu;

VITA_TLS int32_t vita_slice = VITA_SLICE;

static vita_slice_hook g_slice_hook;
static vita_spin_hook g_spin_hook;
static vita_tls_hook g_tls_hook;

void vita_sched_slice_end_impl(void);
void vita_sched_spin_wait_impl(uint32_t addr);
uint32_t vita_thread_tls_base_impl(void);

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

vita_result vita_cpu_prepare_module_start(
    const vita_module_start_state *state) {
    if (!state) return VITA_ERROR_INVALID_ARGUMENT;
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
    if (result != VITA_OK) return result;
    result = vita_sched_prepare_main_execution();
    if (result != VITA_OK) return result;

    vita_cpu_reset();
    r0 = state->argument_size;
    r1 = state->argument_pointer;
    sp = state->stack_pointer;
    lr = state->return_address;
    return VITA_OK;
}

void vita_runtime_set_hooks(vita_slice_hook slice_hook,
                            vita_spin_hook spin_hook,
                            vita_tls_hook tls_hook) {
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
    if (g_tls_hook) return g_tls_hook();
    return vita_thread_tls_base_impl();
}

void vita_trace_fn(uint32_t addr) {
    (void)addr;
}

void vita_trace_insn(uint32_t addr) {
    fprintf(stderr,
            "TRACE pc  0x%08X  r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X"
            " r8=%08X ip=%08X sp=%08X lr=%08X %c%c%c%c\n",
            addr, r0, r1, r2, r3, r4, r5, r6, r7, r8, r12, sp, lr,
            flag_n ? 'N' : 'n', flag_z ? 'Z' : 'z', flag_c ? 'C' : 'c', flag_v ? 'V' : 'v');
}
