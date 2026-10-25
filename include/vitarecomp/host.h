/* SPDX-License-Identifier: MIT */
#ifndef VITARECOMP_HOST_H
#define VITARECOMP_HOST_H

#include "runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal title-neutral host seam. Rendering, input, media and audio belong to
 * separate host modules and are deliberately absent from this contract. */
void vita_host_exit(int status);
void vita_hle_trace(const char *fmt, ...);
void vita_clock_virtual(void);

void vita_main_stack_init(uint32_t base, uint32_t size);
void vita_memblock_init(uint32_t base, uint32_t size);
void vita_kernel_init(uint32_t base, uint32_t size);
void vita_tls_init(uint32_t image, uint32_t filesz, uint32_t memsz);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_HOST_H */
