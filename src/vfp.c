/* vfp.c — floating-point state and the comparison semantics. */

#include "vitarecomp/vfp.h"
#include "vitarecomp/cpu.h"

VITA_TLS vfp_reg vfp_s[64];
VITA_TLS int fpscr_n, fpscr_z, fpscr_c, fpscr_v;
VITA_TLS uint32_t fpscr_control;

#define FPSCR_NZCV 0xF0000000u
#define FPSCR_FZ   0x01000000u

void vfp_set_fpscr(uint32_t value) {
    fpscr_n = (value >> 31) & 1;
    fpscr_z = (value >> 30) & 1;
    fpscr_c = (value >> 29) & 1;
    fpscr_v = (value >> 28) & 1;
    fpscr_control = value & ~FPSCR_NZCV;
}

uint32_t vfp_get_fpscr(void) {
    return fpscr_control
         | ((uint32_t)fpscr_n << 31)
         | ((uint32_t)fpscr_z << 30)
         | ((uint32_t)fpscr_c << 29)
         | ((uint32_t)fpscr_v << 28);
}

static float flush_f32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if ((bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0) {
        bits &= 0x80000000u;
        memcpy(&value, &bits, sizeof(value));
    }
    return value;
}

static double flush_f64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if ((bits & UINT64_C(0x7FF0000000000000)) == 0 &&
        (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0) {
        bits &= UINT64_C(0x8000000000000000);
        memcpy(&value, &bits, sizeof(value));
    }
    return value;
}

/* ARM's four comparison outcomes, written as the architecture defines them
 * rather than derived from C's operators — C has no way to express "unordered"
 * and would silently collapse it into "not equal". */
static void set_flags(int lt, int eq, int unordered) {
    if (unordered) { fpscr_n = 0; fpscr_z = 0; fpscr_c = 1; fpscr_v = 1; return; }
    if (eq)        { fpscr_n = 0; fpscr_z = 1; fpscr_c = 1; fpscr_v = 0; return; }
    if (lt)        { fpscr_n = 1; fpscr_z = 0; fpscr_c = 0; fpscr_v = 0; return; }
    /* greater than */ fpscr_n = 0; fpscr_z = 0; fpscr_c = 1; fpscr_v = 0;
}

void vfp_cmp_f32(float a, float b) {
    if (fpscr_control & FPSCR_FZ) {
        a = flush_f32(a);
        b = flush_f32(b);
    }
    /* A NaN compares false against everything including itself, which is what
     * `a != a` detects. */
    int unordered = (a != a) || (b != b);
    set_flags(a < b, a == b, unordered);
}

void vfp_cmp_f64(double a, double b) {
    if (fpscr_control & FPSCR_FZ) {
        a = flush_f64(a);
        b = flush_f64(b);
    }
    int unordered = (a != a) || (b != b);
    set_flags(a < b, a == b, unordered);
}

void vfp_mrs_apsr(void) {
    flag_n = fpscr_n;
    flag_z = fpscr_z;
    flag_c = fpscr_c;
    flag_v = fpscr_v;
}
