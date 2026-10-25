/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Canonical-C adaptations of independent vectors from the verified prior AOT
 * snapshot. Deferred vectors live in fixtures/prior-aot/migration-vectors.json.
 */

#include "vitarecomp/recomp_rt.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

typedef struct {
    uint32_t core[15];
    int nzcv[4];
    uint32_t vfp[32];
    int fpscr_nzcv[4];
} represented_state;

static uint32_t *const core_registers[15] = {
    &r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7,
    &r8, &r9, &r10, &r11, &r12, &sp, &lr,
};

static void capture_state(represented_state *out) {
    for (int i = 0; i < 15; i++) out->core[i] = *core_registers[i];
    out->nzcv[0] = flag_n;
    out->nzcv[1] = flag_z;
    out->nzcv[2] = flag_c;
    out->nzcv[3] = flag_v;
    for (int i = 0; i < 32; i++) out->vfp[i] = vfp_getu(i);
    out->fpscr_nzcv[0] = fpscr_n;
    out->fpscr_nzcv[1] = fpscr_z;
    out->fpscr_nzcv[2] = fpscr_c;
    out->fpscr_nzcv[3] = fpscr_v;
}

static void restore_state(const represented_state *in) {
    for (int i = 0; i < 15; i++) *core_registers[i] = in->core[i];
    flag_n = in->nzcv[0];
    flag_z = in->nzcv[1];
    flag_c = in->nzcv[2];
    flag_v = in->nzcv[3];
    for (int i = 0; i < 32; i++) vfp_setu(i, in->vfp[i]);
    fpscr_n = in->fpscr_nzcv[0];
    fpscr_z = in->fpscr_nzcv[1];
    fpscr_c = in->fpscr_nzcv[2];
    fpscr_v = in->fpscr_nzcv[3];
}

static void test_represented_state_round_trip(void) {
    represented_state expected, actual;

    for (int i = 0; i < 15; i++) *core_registers[i] = 0x11000000u + (uint32_t)i;
    flag_n = 1;
    flag_z = 0;
    flag_c = 1;
    flag_v = 0;
    for (int i = 0; i < 32; i++) vfp_setu(i, 0x22000000u + (uint32_t)i);
    vfp_setu(7, 0x7FC01234u);
    fpscr_n = 0;
    fpscr_z = 1;
    fpscr_c = 1;
    fpscr_v = 0;
    capture_state(&expected);

    memset(&actual, 0, sizeof(actual));
    for (int i = 0; i < 15; i++) *core_registers[i] = 0;
    flag_n = flag_z = flag_c = flag_v = 0;
    for (int i = 0; i < 32; i++) vfp_setu(i, 0);
    fpscr_n = fpscr_z = fpscr_c = fpscr_v = 0;

    restore_state(&expected);
    capture_state(&actual);
    assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
    assert(vfp_getu(7) == 0x7FC01234u);

    printf("  represented CPU/VFP state round-trip ok\n");
}

static void test_vfp_unordered_flags(void) {
    vfp_reg nan_value;
    nan_value.u = 0x7FC01234u;

    flag_n = flag_z = flag_c = flag_v = 0;
    vfp_cmp_f32(nan_value.f, 1.0f);
    assert(!fpscr_n && !fpscr_z && fpscr_c && fpscr_v);
    vfp_mrs_apsr();
    assert(!flag_n && !flag_z && flag_c && flag_v);

    printf("  VFP NaN payload and unordered flags ok\n");
}

static void test_checked_memory_edges(void) {
    const uint32_t base = 0x1000u;
    assert(vita_mem_init(base, 16) == 0);
    vita_mem_bad_access = 0;

    vita_write32(base + 1, 0x44332211u);
    assert(vita_read8(base + 1) == 0x11u);
    assert(vita_read16(base + 2) == 0x3322u);
    assert(vita_read32(base + 1) == 0x44332211u);
    assert(vita_mem_bad_access == 0);

    assert(vita_mem_avail(base) == 16);
    assert(vita_mem_avail(base + 15) == 1);
    assert(vita_mem_avail(base + 16) == 0);
    assert(vita_mem_bad_access == 0);

    assert(vita_read32(base + 14) == 0);
    assert(vita_mem_bad_access == 1);
    assert(vita_mem_ptr(base - 1, 1) == NULL);
    assert(vita_mem_bad_access == 2);
    assert(vita_mem_ptr(UINT32_MAX - 1u, UINT32_MAX) == NULL);
    assert(vita_mem_bad_access == 3);

    vita_mem_free();
    printf("  checked memory boundary vectors ok\n");
}

static void test_semantic_boundaries(void) {
    assert(vita_lsl(0xFFFFFFFFu, 32) == 0);
    assert(vita_lsr(0x80000000u, 33) == 0);
    assert(vita_asr(0x80000000u, 99) == 0xFFFFFFFFu);
    assert(vita_ror(0x12345678u, 32) == 0x12345678u);

    vita_flags_adc(5u, 0xFFFFFFFFu, 1u, 5u);
    assert(flag_c);
    vita_flags_sub(3u, 5u, (uint32_t)-2);
    assert(!flag_c);

    printf("  arithmetic semantic boundaries ok\n");
}

int main(void) {
    printf("prior AOT migration:\n");
    test_represented_state_round_trip();
    test_vfp_unordered_flags();
    test_checked_memory_edges();
    test_semantic_boundaries();
    printf("all executable prior AOT vectors passed\n");
    return 0;
}
