/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exact missing-target diagnostic adapted from the prior strict dispatcher.
 */

#include "vitarecomp/dispatch.h"
#include "vitarecomp/recomp_rt.h"

#include <assert.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

int main(void) {
    vita_dispatch_init(NULL, 0);
    vita_dispatch(0x00001000u, 0x00002001u);
    assert(vita_dispatch_hits() == 0);
    assert(vita_dispatch_misses() == 1);
    assert(vita_trap_count() == 1);
    return 0;
}
