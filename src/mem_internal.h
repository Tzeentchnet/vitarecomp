#ifndef VITARECOMP_MEM_INTERNAL_H
#define VITARECOMP_MEM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

void vita_mem_strict_check(uint32_t address, size_t size);
void *vita_mem_ptr_unchecked(uint32_t address, uint32_t size);

#endif
