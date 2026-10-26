/* SPDX-License-Identifier: MIT */
#ifndef VITARECOMP_HOST_H
#define VITARECOMP_HOST_H

#include "generated_module.h"
#include "runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal title-neutral host seam. Rendering, input, media and audio belong to
 * separate host modules and are deliberately absent from this contract. */
void vita_host_exit(int status);
void vita_hle_trace(const char *fmt, ...);
void vita_clock_virtual(void);

/* Configure the main guest stack and return its aligned initial SP. The whole
 * range must be 16-byte aligned, backed by the initialized runtime, and free
 * of uint32_t overflow. The output is optional and changes only on success. */
vita_result vita_main_stack_prepare(
    uint32_t base,
    uint32_t size,
    uint32_t *initial_stack_pointer);

/* Compatibility wrapper for older hosts. New code must use the checked form
 * above so a rejected stack cannot be mistaken for successful preparation. */
void vita_main_stack_init(uint32_t base, uint32_t size);

void vita_memblock_init(uint32_t base, uint32_t size);
void vita_kernel_init(uint32_t base, uint32_t size);

/* TLS templates are laid out in array order. Each module begins at the first
 * offset satisfying its power-of-two alignment. Initialized bytes are copied
 * from guest memory and the rest of every module is zero-filled for each
 * thread. The caller's arrays are borrowed only for the duration of the call. */
typedef struct vita_tls_template {
    uint32_t image_address;
    uint32_t initialized_size;
    uint32_t memory_size;
    uint32_t alignment;
} vita_tls_template;

typedef struct vita_tls_module_layout {
    uint32_t offset;
    uint32_t initialized_size;
    uint32_t memory_size;
    uint32_t alignment;
} vita_tls_module_layout;

typedef struct vita_tls_layout {
    uint32_t module_count;
    uint32_t data_size;
    uint32_t data_alignment;
    uint32_t allocation_size;
} vita_tls_layout;

vita_result vita_tls_prepare(
    const vita_tls_template *templates,
    size_t template_count,
    vita_tls_module_layout *module_layouts,
    size_t module_layout_capacity,
    vita_tls_layout *layout);

/* Discard every currently materialized TLS block while retaining the template
 * configuration. Blocks are rebuilt lazily on their owning threads. */
vita_result vita_tls_restart(void);

/* Prepare every TLS-bearing descriptor in composed registry order. The
 * aggregate layout is mandatory and is checked against the descriptor set,
 * explicit alignments, recomputed offsets, and aggregate size/alignment. */
vita_result vita_tls_prepare_generated_modules(
    const vita_generated_module_descriptor *const *modules,
    size_t module_count,
    const vita_generated_tls_layout *generated_layout,
    vita_tls_module_layout *module_layouts,
    size_t module_layout_capacity,
    vita_tls_layout *layout);

/* Materialize the calling guest thread's configured TLS block and return its
 * TPIDRURO-visible data base. On failure, the output is unchanged. Hosts must
 * call this for the main thread before making generated entry callable. */
vita_result vita_tls_materialize_current(uint32_t *tls_base);

/* Compatibility wrapper for the former single-template API. */
void vita_tls_init(uint32_t image, uint32_t filesz, uint32_t memsz);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_HOST_H */
