/* SPDX-License-Identifier: MIT */
/* Page-granular SceSysmem blocks with first-fit reuse and coalescing. */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/runtime.h"
#include "hle_sysmem.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define SCE_KERNEL_ERROR_INVALID_ARGUMENT UINT32_C(0x80020005)
#define SCE_KERNEL_ERROR_INVALID_UID      UINT32_C(0x80024501)
#define SCE_KERNEL_ERROR_NO_MEMORY        UINT32_C(0x8002710B)

#define MAX_BLOCKS 256
#define UID_FIRST UINT32_C(0x41010000)

typedef struct block {
    uint32_t base;
    uint32_t size;
    uint32_t type;
    int used;
} block;

typedef struct arena_chunk {
    uint32_t base;
    uint32_t size;
    int owner; /* -1 free, -2 runtime reservation, otherwise block index */
} arena_chunk;

static once_flag g_once = ONCE_FLAG_INIT;
static mtx_t g_lock;
static block g_blocks[MAX_BLOCKS];
static arena_chunk *g_chunks;
static uint32_t g_chunk_count, g_chunk_capacity;
static uint32_t g_base, g_size;

static void sysmem_once(void) {
    (void)mtx_init(&g_lock, mtx_plain);
}

static int chunk_insert(uint32_t at, arena_chunk value) {
    if (g_chunk_count == g_chunk_capacity) {
        uint32_t capacity = g_chunk_capacity ? g_chunk_capacity * 2u : 64u;
        if (capacity < g_chunk_capacity ||
            capacity > UINT32_MAX / (uint32_t)sizeof(*g_chunks))
            return 0;
        arena_chunk *grown = (arena_chunk *)realloc(
            g_chunks, (size_t)capacity * sizeof(*grown));
        if (!grown) return 0;
        g_chunks = grown;
        g_chunk_capacity = capacity;
    }
    memmove(g_chunks + at + 1, g_chunks + at,
            (size_t)(g_chunk_count - at) * sizeof(*g_chunks));
    g_chunks[at] = value;
    g_chunk_count++;
    return 1;
}

static void chunk_erase(uint32_t at) {
    memmove(g_chunks + at, g_chunks + at + 1,
            (size_t)(g_chunk_count - at - 1) * sizeof(*g_chunks));
    g_chunk_count--;
}

void vita_memblock_init(uint32_t base, uint32_t size) {
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    memset(g_blocks, 0, sizeof(g_blocks));
    free(g_chunks);
    g_chunks = NULL;
    g_chunk_count = g_chunk_capacity = 0;
    g_base = g_size = 0;
    void *memory = NULL;
    if (size && vita_mem_range(base, size, &memory) == VITA_OK) {
        (void)memory;
        arena_chunk all = {base, size, -1};
        if (chunk_insert(0, all)) {
            g_base = base;
            g_size = size;
        }
    }
    mtx_unlock(&g_lock);
}

void vita_sysmem_shutdown(void) {
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    memset(g_blocks, 0, sizeof(g_blocks));
    free(g_chunks);
    g_chunks = NULL;
    g_chunk_count = g_chunk_capacity = 0;
    g_base = g_size = 0;
    mtx_unlock(&g_lock);
}

static int uid_index(uint32_t uid) {
    if (uid < UID_FIRST || uid >= UID_FIRST + MAX_BLOCKS) return -1;
    int index = (int)(uid - UID_FIRST);
    return g_blocks[index].used ? index : -1;
}

static uint32_t arena_alloc(uint32_t size, uint32_t alignment, int owner) {
    if (!size || !alignment || (alignment & (alignment - 1u))) return 0;
    for (uint32_t i = 0; i < g_chunk_count; i++) {
        arena_chunk *chunk = &g_chunks[i];
        if (chunk->owner != -1) continue;
        uint64_t start64 = ((uint64_t)chunk->base + alignment - 1u) &
                           ~(uint64_t)(alignment - 1u);
        if (start64 > UINT32_MAX) continue;
        uint32_t start = (uint32_t)start64;
        if (start < chunk->base || start - chunk->base > chunk->size)
            continue;
        uint32_t before = start - chunk->base;
        if (size > chunk->size - before) continue;
        uint32_t after = chunk->size - before - size;
        if (before) {
            arena_chunk head = {chunk->base, before, -1};
            if (!chunk_insert(i, head)) return 0;
            i++;
            chunk = &g_chunks[i];
        }
        if (after) {
            arena_chunk tail = {start + size, after, -1};
            if (!chunk_insert(i + 1, tail)) return 0;
            chunk = &g_chunks[i];
        }
        chunk->base = start;
        chunk->size = size;
        chunk->owner = owner;
        void *memory = NULL;
        if (vita_mem_range(start, size, &memory) != VITA_OK) {
            chunk->owner = -1;
            return 0;
        }
        (void)memory;
        (void)vita_mem_set_guest(start, 0, size);
        return start;
    }
    return 0;
}

static int arena_free(uint32_t address, int owner) {
    for (uint32_t i = 0; i < g_chunk_count; i++) {
        arena_chunk *chunk = &g_chunks[i];
        if (chunk->base != address || chunk->owner != owner) continue;
        chunk->owner = -1;
        if (i + 1 < g_chunk_count && g_chunks[i + 1].owner == -1) {
            chunk->size += g_chunks[i + 1].size;
            chunk_erase(i + 1);
        }
        if (i && g_chunks[i - 1].owner == -1) {
            g_chunks[i - 1].size += chunk->size;
            chunk_erase(i);
        }
        return 1;
    }
    return 0;
}

void vita_hle_sceKernelAllocMemBlock(void) {
    uint32_t type = r1, size = r2;
    uint32_t granularity = (type >> 24) == 0x09 ? 0x40000u : 0x1000u;
    if (!size || (size & (granularity - 1u))) {
        r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        return;
    }
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    int index;
    for (index = 0; index < MAX_BLOCKS && g_blocks[index].used; index++) {}
    if (index == MAX_BLOCKS) {
        mtx_unlock(&g_lock);
        r0 = SCE_KERNEL_ERROR_NO_MEMORY;
        return;
    }
    uint32_t base = arena_alloc(size, granularity, index);
    if (!base) {
        mtx_unlock(&g_lock);
        r0 = SCE_KERNEL_ERROR_NO_MEMORY;
        return;
    }
    g_blocks[index].base = base;
    g_blocks[index].size = size;
    g_blocks[index].type = type;
    g_blocks[index].used = 1;
    mtx_unlock(&g_lock);
    r0 = UID_FIRST + (uint32_t)index;
}

void vita_hle_sceKernelGetMemBlockBase(void) {
    uint32_t uid = r0, output = r1;
    if (!vita_mem_ptr(output, 4)) {
        r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        return;
    }
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    int index = uid_index(uid);
    uint32_t base = index >= 0 ? g_blocks[index].base : 0;
    mtx_unlock(&g_lock);
    if (index < 0) {
        r0 = SCE_KERNEL_ERROR_INVALID_UID;
        return;
    }
    vita_write32(output, base);
    r0 = 0;
}

void vita_hle_sceKernelFreeMemBlock(void) {
    uint32_t uid = r0;
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    int index = uid_index(uid);
    if (index < 0) {
        mtx_unlock(&g_lock);
        r0 = SCE_KERNEL_ERROR_INVALID_UID;
        return;
    }
    uint32_t base = g_blocks[index].base;
    (void)arena_free(base, index);
    memset(&g_blocks[index], 0, sizeof(g_blocks[index]));
    mtx_unlock(&g_lock);
    r0 = 0;
}

uint32_t vita_sysmem_reserve(uint32_t size) {
    if (!size || size > UINT32_MAX - 0xFFFu) return 0;
    size = (size + 0xFFFu) & ~0xFFFu;
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    uint32_t base = arena_alloc(size, 0x1000u, -2);
    mtx_unlock(&g_lock);
    return base;
}

void vita_sysmem_release(uint32_t base) {
    if (!base) return;
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    (void)arena_free(base, -2);
    mtx_unlock(&g_lock);
}

void vita_hle_sceKernelFindMemBlockByAddr(void) {
    uint32_t address = r0, length = r1;
    uint64_t end = (uint64_t)address + length;
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (!g_blocks[i].used || address < g_blocks[i].base) continue;
        uint64_t block_end = (uint64_t)g_blocks[i].base + g_blocks[i].size;
        if (end >= address && end <= block_end) {
            mtx_unlock(&g_lock);
            r0 = UID_FIRST + (uint32_t)i;
            return;
        }
    }
    mtx_unlock(&g_lock);
    r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT;
}

void vita_hle_sceKernelGetMemBlockInfoByAddr(void) {
    uint32_t address = r0, output = r1;
    if (!vita_mem_ptr(output, 24)) {
        r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        return;
    }
    call_once(&g_once, sysmem_once);
    mtx_lock(&g_lock);
    block found;
    memset(&found, 0, sizeof(found));
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (g_blocks[i].used && address >= g_blocks[i].base &&
            (uint64_t)address < (uint64_t)g_blocks[i].base + g_blocks[i].size) {
            found = g_blocks[i];
            break;
        }
    }
    mtx_unlock(&g_lock);
    if (!found.used) {
        r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        return;
    }
    vita_write32(output + 4, found.base);
    vita_write32(output + 8, found.size);
    vita_write32(output + 12, (found.type >> 24) == 0x09 ? 1u : 0u);
    vita_write32(output + 16, 6);
    vita_write32(output + 20, found.type);
    r0 = 0;
}

void vita_hle_sceKernelGetMemBlockInfoByRange(void) {
    vita_hle_sceKernelGetMemBlockInfoByAddr();
}
