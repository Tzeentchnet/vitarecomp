/* mem.h — the recompiled program's address space.
 *
 * The Vita has an MMU, so unlike the PSP there is no small fixed map to fold
 * onto a backing store with a mask. What there is instead is simpler in
 * practice: a module is loaded at a known base (0x81000000 for the executable
 * segment) and the interesting addresses are all within a bounded window above
 * it. A flat host allocation plus a subtraction covers it.
 *
 * Accesses outside the window are COUNTED rather than silently returning zero.
 * A recompiled program should reach a steady state with that counter flat;
 * when it climbs, discovery or translation missed something.
 */

#ifndef VITARECOMP_MEM_H
#define VITARECOMP_MEM_H

#include <stdint.h>
#include <stddef.h>

extern uint32_t vita_mem_bad_access;

typedef struct vita_mem_strict_range {
    uint32_t address;
    uint32_t size;
    uint32_t nid;
    const char *name;
} vita_mem_strict_range;

/* Reserve `size` bytes of guest address space starting at `base`. */
int  vita_mem_init(uint32_t base, size_t size);
void vita_mem_free(void);

/* Mark unresolved guest objects that may be bound for bring-up but must fail
 * by name on their first access. Configuration is initialization-only and is
 * cleared with the guest address space. */
int vita_mem_set_strict_ranges(const vita_mem_strict_range *ranges,
                               size_t count);
void vita_mem_clear_strict_ranges(void);

/* Returns NULL rather than a wild pointer when an access would straddle the
 * end of the region, so a bad address is a diagnosable NULL dereference at the
 * point of failure instead of silent corruption elsewhere. */
void *vita_mem_ptr(uint32_t addr, uint32_t len);

/* Checked forms for host and HLE code. They distinguish an invalid guest
 * range from a valid zero-length one and never truncate a host size_t. */
int vita_mem_range(uint32_t addr, size_t len, void **out);
int vita_mem_copy_from_guest(void *dst, uint32_t src, size_t len);
int vita_mem_copy_to_guest(uint32_t dst, const void *src, size_t len);
int vita_mem_move_guest(uint32_t dst, uint32_t src, size_t len);
int vita_mem_set_guest(uint32_t dst, uint8_t value, size_t len);

/* Copies a NUL-terminated guest string. A missing terminator, an undersized
 * destination and an unbacked address are distinct failures. */
int vita_mem_read_cstr(uint32_t addr, char *dst, size_t capacity,
                       size_t *length);

uint32_t vita_mem_base(void);
size_t vita_mem_size(void);
uint64_t vita_mem_bad_access_count(void);

/* Bytes backed from `addr` to the end of the region, or 0 if it is outside.
 *
 * The HLE needs this and `vita_mem_ptr` cannot answer it: a C string has no
 * length until you find its terminator, so forwarding `strlen` to the host
 * means scanning guest memory, and a scan with no bound runs off the
 * allocation when the guest hands over a pointer to something that is not a
 * string. Unlike a bad read this does not count as a bad access — asking how
 * much is there is a legitimate question. */
uint32_t vita_mem_avail(uint32_t addr);

uint32_t vita_read32(uint32_t addr);
uint16_t vita_read16(uint32_t addr);
uint8_t  vita_read8 (uint32_t addr);

void vita_write32(uint32_t addr, uint32_t v);
void vita_write16(uint32_t addr, uint16_t v);
void vita_write8 (uint32_t addr, uint8_t  v);

/* Checked host-side atomic compare/exchange. `observed` receives the value
 * read even when it differs from `expected`. */
int vita_mem_compare_exchange32(uint32_t addr, uint32_t expected,
                                uint32_t desired, uint32_t *observed);
int vita_mem_mark_written(uint32_t addr, size_t len);

/* Exclusive accesses use byte sizes (1, 2, 4, or 8). Any successful ordinary
 * write invalidates outstanding monitors before a later STREX can succeed. */
uint64_t vita_ldrex(uint32_t addr, uint32_t size);
uint32_t vita_strex(uint32_t addr, uint64_t value, uint32_t size);
void vita_clrex(void);
void vita_barrier(void);

#endif /* VITARECOMP_MEM_H */
