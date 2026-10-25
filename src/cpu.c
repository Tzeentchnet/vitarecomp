/* cpu.c — the machine state the generated C operates on. */

#include "vitarecomp/recomp_rt.h"

#include <stdio.h>

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

void vita_runtime_set_hooks(vita_slice_hook slice_hook,
                            vita_spin_hook spin_hook,
                            vita_tls_hook tls_hook) {
    g_slice_hook = slice_hook;
    g_spin_hook = spin_hook;
    g_tls_hook = tls_hook;
}

void vita_slice_end(void) {
    vita_slice = VITA_SLICE;
    if (g_slice_hook) g_slice_hook();
}

void vita_spin_wait(uint32_t addr) {
    if (g_spin_hook) {
        g_spin_hook(addr);
        return;
    }
    vita_trap_unimpl(addr, 0, "spin wait requires a scheduler hook");
}

uint32_t vita_tls_base(void) {
    if (g_tls_hook) return g_tls_hook();
    vita_trap_unimpl(vita_last_pc, 0, "TLS base requires a runtime hook");
    return 0;
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
