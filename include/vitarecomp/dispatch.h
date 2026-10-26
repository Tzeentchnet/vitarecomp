/* dispatch.h — indirect transfers, resolved at run time.
 *
 * A recompiled program has no program counter, so `BX r3` cannot be translated
 * into anything static: the destination is a value, and it is only known while
 * the program is running. Every recompiler needs this layer, and on Vita it
 * carries more traffic than most — the module has ~18,000 indirect call sites,
 * because `module_start` itself passes the real entry point as a POINTER rather
 * than calling it, and C++ virtual dispatch does the rest.
 *
 * The table maps a guest address to the C function that was recompiled from it.
 * A hit is a real transfer; a miss is a named trap, never a silent return —
 * jumping to an address we never translated has to stop the program, because
 * continuing means running the caller's code with the callee's job undone.
 */

#ifndef VITARECOMP_DISPATCH_H
#define VITARECOMP_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vita_fn)(void);

typedef struct {
    uint32_t addr; /* guest address with ARM/Thumb bit retained */
    vita_fn fn;
} vita_dispatch_entry;

typedef enum vita_dispatch_status {
    VITA_DISPATCH_OK = 0,
    VITA_DISPATCH_ERROR_NULL_COUNT = -1,
    VITA_DISPATCH_ERROR_NULL_FUNCTION = -2,
    VITA_DISPATCH_ERROR_UNSORTED = -3,
    VITA_DISPATCH_ERROR_DUPLICATE = -4,
    VITA_DISPATCH_ERROR_NO_MEMORY = -5
} vita_dispatch_status;

/* Install a private copy of a strictly key-sorted table. NULL is valid only
 * with count zero. Equal adjacent keys are reported separately from descending
 * keys because either condition would make binary-search results ambiguous. */
vita_dispatch_status
vita_dispatch_init_checked(const vita_dispatch_entry *table, uint32_t count);

/* Compatibility entry point retained for generated ABI-v1 code. Invalid
 * tables are refused and leave an empty dispatcher; new code should use the
 * checked form so initialization failure cannot be mistaken for success. */
void vita_dispatch_init(const vita_dispatch_entry *table, uint32_t count);
void vita_dispatch_reset(void);
const char *vita_dispatch_status_string(vita_dispatch_status status);

/* Call a statically resolved generated function through the runtime boundary.
 * Keeping this out of the generated translation unit prevents the host
 * compiler from deciding a guest callee never returns and deleting valid
 * fallthrough code. */
void vita_call(vita_fn fn);

/* Call the function recompiled from `addr`. A pointer to Thumb code carries
 * bit 0; it is retained so ARM and Thumb entries at one aligned address remain
 * distinct.
 *
 * `from` is the guest address of the transfer itself. A miss reports both, and
 * the destination alone is not enough to act on: "something jumped to
 * 0x814B9590" does not say which function to translate next, and that is the
 * only question a miss ever raises. */
void vita_dispatch(uint32_t from, uint32_t addr);

/* How many distinct addresses were dispatched to, and how many missed. Useful
 * for telling "the table is incomplete" from "the program went somewhere it
 * should not have". */
/* The start of the function an address falls in: the greatest table entry
 * not above it, or 0. For naming where a sample or a fault was. */
uint32_t vita_dispatch_floor(uint32_t addr);

uint32_t vita_dispatch_hits(void);
uint32_t vita_dispatch_misses(void);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_DISPATCH_H */
