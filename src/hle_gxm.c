/* hle_gxm.c - SceGxm guest ABI state and resource descriptors. */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_sysmem.h"

#include <stdint.h>
#include <string.h>

#define SCE_GXM_ERROR_ALREADY_INITIALIZED UINT32_C(0x805B0001)
#define SCE_GXM_ERROR_OUT_OF_MEMORY       UINT32_C(0x805B0002)
#define SCE_GXM_ERROR_INVALID_VALUE   UINT32_C(0x805B0003)
#define SCE_GXM_ERROR_INVALID_POINTER UINT32_C(0x805B0004)
#define SCE_GXM_ERROR_NULL_PROGRAM    UINT32_C(0x805B0008)

#define SCE_GXM_TEXTURE_LINEAR UINT32_C(0x60000000)
#define GXM_MEMORY_MAP_LIMIT 256u
#define GXM_USSE_MAP_LIMIT 64u
#define GXM_CONTEXT_LIMIT 8u
#define GXM_SHADER_PATCHER_LIMIT 8u
#define GXM_REGISTERED_PROGRAM_LIMIT 256u
#define GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE UINT32_C(0x800)
#define GXM_SHADER_PATCHER_OBJECT_SIZE UINT32_C(0x100)
#define GXM_REGISTERED_PROGRAM_SIZE UINT32_C(0x4)
#define GXM_NOTIFICATION_REGION_SIZE UINT32_C(0x100000)

typedef struct gxm_memory_map {
    uint32_t base;
    uint32_t size;
    uint32_t attributes;
} gxm_memory_map;

typedef struct gxm_usse_map {
    uint32_t base;
    uint32_t size;
    uint32_t offset;
} gxm_usse_map;

typedef struct gxm_context {
    uint32_t address;
    uint32_t host_mem;
    uint32_t host_mem_size;
    uint32_t vdm_ring_buffer;
    uint32_t vdm_ring_buffer_size;
    uint32_t vertex_ring_buffer;
    uint32_t vertex_ring_buffer_size;
    uint32_t fragment_ring_buffer;
    uint32_t fragment_ring_buffer_size;
    uint32_t fragment_usse_ring_buffer;
    uint32_t fragment_usse_ring_buffer_size;
    uint32_t fragment_usse_ring_buffer_offset;
    uint32_t finish_count;
    uint32_t vertex_program;
    uint32_t fragment_program;
} gxm_context;

typedef struct gxm_shader_patcher {
    uint32_t address;
    uint32_t params[17];
} gxm_shader_patcher;

typedef struct gxm_registered_program {
    uint32_t address;
    uint32_t patcher;
    uint32_t program;
} gxm_registered_program;

typedef struct gxm_state {
    uint32_t initialized;
    uint32_t flags;
    uint32_t display_queue_max_pending_count;
    uint32_t display_queue_callback;
    uint32_t display_queue_callback_data_size;
    uint32_t parameter_buffer_size;
    uint32_t notification_region;
    uint32_t memory_map_count;
    gxm_memory_map memory_maps[GXM_MEMORY_MAP_LIMIT];
    uint32_t fragment_usse_map_count;
    gxm_usse_map fragment_usse_maps[GXM_USSE_MAP_LIMIT];
    uint32_t vertex_usse_map_count;
    gxm_usse_map vertex_usse_maps[GXM_USSE_MAP_LIMIT];
    uint32_t context_count;
    gxm_context contexts[GXM_CONTEXT_LIMIT];
    uint32_t shader_patcher_count;
    gxm_shader_patcher shader_patchers[GXM_SHADER_PATCHER_LIMIT];
    uint32_t registered_program_count;
    gxm_registered_program
        registered_programs[GXM_REGISTERED_PROGRAM_LIMIT];
} gxm_state;

static gxm_state g_gxm;

void vita_gxm_reset(void) {
    g_gxm = (gxm_state){0};
    /* Keep this HLE object linked ahead of generated strict import defaults. */
}

void vita_hle_sceGxmInitialize(void) {
    const uint32_t params = r0;
    if (!vita_mem_ptr(params, 20u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }

    const uint32_t flags = vita_read32(params);
    const uint32_t max_pending_count = vita_read32(params + 4u);
    const uint32_t callback = vita_read32(params + 8u);
    const uint32_t callback_data_size = vita_read32(params + 12u);
    const uint32_t parameter_buffer_size = vita_read32(params + 16u);
    if ((flags != 0u && flags != UINT32_C(0x00010000) &&
         flags != UINT32_C(0x00020000)) ||
        parameter_buffer_size < UINT32_C(0x40000) ||
        (parameter_buffer_size & UINT32_C(0x3FFFF)) != 0u ||
        (uint64_t)max_pending_count * callback_data_size >
            UINT64_C(0x200)) {
        r0 = SCE_GXM_ERROR_INVALID_VALUE;
        return;
    }
    if (g_gxm.initialized) {
        r0 = SCE_GXM_ERROR_ALREADY_INITIALIZED;
        return;
    }

    const uint32_t notification_region =
        vita_sysmem_reserve(GXM_NOTIFICATION_REGION_SIZE);
    if (!notification_region) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }
    (void)vita_mem_set_guest(
        notification_region, 0, GXM_NOTIFICATION_REGION_SIZE);
    g_gxm.initialized = 1u;
    g_gxm.flags = flags;
    g_gxm.display_queue_max_pending_count = max_pending_count;
    g_gxm.display_queue_callback = callback;
    g_gxm.display_queue_callback_data_size = callback_data_size;
    g_gxm.parameter_buffer_size = parameter_buffer_size;
    g_gxm.notification_region = notification_region;
    vita_hle_trace(
        "sceGxmInitialize(flags 0x%08X, pending %u, callback 0x%08X, "
        "callback data %u, parameter buffer 0x%X)",
        flags, max_pending_count, callback, callback_data_size,
        parameter_buffer_size);
    r0 = 0;
}

void vita_hle_sceGxmGetNotificationRegion(void) {
    r0 = g_gxm.initialized ? g_gxm.notification_region : 0u;
    vita_hle_trace("sceGxmGetNotificationRegion() -> 0x%08X", r0);
}

void vita_hle_sceGxmMapMemory(void) {
    const uint32_t base = r0;
    const uint32_t size = r1;
    const uint32_t attributes = r2;
    const uint64_t end = (uint64_t)base + size;
    if (!base || end > UINT64_C(0x100000000) ||
        !vita_mem_ptr(base, size)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    if (attributes == 0u || attributes > 3u) {
        r0 = SCE_GXM_ERROR_INVALID_VALUE;
        return;
    }

    const uint32_t aligned_base = base & UINT32_C(0xFFFFF000);
    const uint64_t aligned_end =
        (end + UINT64_C(0xFFF)) & ~UINT64_C(0xFFF);
    const uint64_t aligned_size = aligned_end - aligned_base;
    if (aligned_size > UINT32_MAX ||
        (aligned_size != 0u &&
         !vita_mem_ptr(aligned_base, (uint32_t)aligned_size))) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    for (uint32_t i = 0; i < g_gxm.memory_map_count; i++) {
        if (g_gxm.memory_maps[i].base == aligned_base) {
            r0 = SCE_GXM_ERROR_INVALID_POINTER;
            return;
        }
    }
    if (g_gxm.memory_map_count == GXM_MEMORY_MAP_LIMIT) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }

    gxm_memory_map *mapping =
        &g_gxm.memory_maps[g_gxm.memory_map_count++];
    mapping->base = aligned_base;
    mapping->size = (uint32_t)aligned_size;
    mapping->attributes = attributes;
    vita_hle_trace(
        "sceGxmMapMemory(base 0x%08X, size 0x%X, attributes 0x%X) "
        "mapped [0x%08X, +0x%X)",
        base, size, attributes, mapping->base, mapping->size);
    r0 = 0;
}

static void map_usse_memory(gxm_usse_map *mappings, uint32_t *map_count,
                            const char *name) {
    const uint32_t base = r0;
    const uint32_t size = r1;
    const uint32_t offset_address = r2;
    if (!base || !size || !vita_mem_ptr(base, size) ||
        !vita_mem_ptr(offset_address, 4u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    for (uint32_t i = 0; i < *map_count; i++) {
        if (mappings[i].base == base) {
            r0 = SCE_GXM_ERROR_INVALID_POINTER;
            return;
        }
    }
    if (*map_count == GXM_USSE_MAP_LIMIT) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }

    gxm_usse_map *mapping = &mappings[(*map_count)++];
    mapping->base = base;
    mapping->size = size;
    mapping->offset = base;
    vita_write32(offset_address, mapping->offset);
    vita_hle_trace(
        "%s(base 0x%08X, size 0x%X, "
        "offset 0x%08X)",
        name, base, size, mapping->offset);
    r0 = 0;
}

void vita_hle_sceGxmMapFragmentUsseMemory(void) {
    map_usse_memory(
        g_gxm.fragment_usse_maps, &g_gxm.fragment_usse_map_count,
        "sceGxmMapFragmentUsseMemory");
}

void vita_hle_sceGxmMapVertexUsseMemory(void) {
    map_usse_memory(
        g_gxm.vertex_usse_maps, &g_gxm.vertex_usse_map_count,
        "sceGxmMapVertexUsseMemory");
}

void vita_hle_sceGxmCreateContext(void) {
    const uint32_t params = r0;
    const uint32_t context_address = r1;
    if (!vita_mem_ptr(params, 44u) ||
        !vita_mem_ptr(context_address, 4u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }

    const uint32_t host_mem = vita_read32(params);
    const uint32_t host_mem_size = vita_read32(params + 4u);
    const uint32_t vdm_ring_buffer = vita_read32(params + 8u);
    const uint32_t vdm_ring_buffer_size = vita_read32(params + 12u);
    const uint32_t vertex_ring_buffer = vita_read32(params + 16u);
    const uint32_t vertex_ring_buffer_size = vita_read32(params + 20u);
    const uint32_t fragment_ring_buffer = vita_read32(params + 24u);
    const uint32_t fragment_ring_buffer_size = vita_read32(params + 28u);
    const uint32_t fragment_usse_ring_buffer =
        vita_read32(params + 32u);
    const uint32_t fragment_usse_ring_buffer_size =
        vita_read32(params + 36u);
    const uint32_t fragment_usse_ring_buffer_offset =
        vita_read32(params + 40u);
    const uint32_t context = (host_mem + 7u) & ~UINT32_C(7);
    const uint32_t host_padding = context - host_mem;

    if (host_mem_size < GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE ||
        host_padding > host_mem_size ||
        host_mem_size - host_padding <
            GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE) {
        r0 = SCE_GXM_ERROR_INVALID_VALUE;
        return;
    }
    if (!vita_mem_ptr(host_mem, host_mem_size) ||
        !vdm_ring_buffer || !vita_mem_ptr(vdm_ring_buffer,
                                          vdm_ring_buffer_size) ||
        !vertex_ring_buffer || !vita_mem_ptr(vertex_ring_buffer,
                                             vertex_ring_buffer_size) ||
        !fragment_ring_buffer ||
        !vita_mem_ptr(fragment_ring_buffer, fragment_ring_buffer_size) ||
        !fragment_usse_ring_buffer ||
        !vita_mem_ptr(fragment_usse_ring_buffer,
                      fragment_usse_ring_buffer_size)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    if (g_gxm.context_count == GXM_CONTEXT_LIMIT) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }
    for (uint32_t i = 0; i < g_gxm.context_count; i++) {
        if (g_gxm.contexts[i].address == context) {
            r0 = SCE_GXM_ERROR_INVALID_VALUE;
            return;
        }
    }

    gxm_context *created = &g_gxm.contexts[g_gxm.context_count++];
    *created = (gxm_context){
        context,
        host_mem,
        host_mem_size,
        vdm_ring_buffer,
        vdm_ring_buffer_size,
        vertex_ring_buffer,
        vertex_ring_buffer_size,
        fragment_ring_buffer,
        fragment_ring_buffer_size,
        fragment_usse_ring_buffer,
        fragment_usse_ring_buffer_size,
        fragment_usse_ring_buffer_offset
    };
    (void)vita_mem_set_guest(context, 0,
                             GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    vita_write32(context_address, context);
    vita_hle_trace(
        "sceGxmCreateContext(params 0x%08X) -> context 0x%08X "
        "(host size 0x%X)",
        params, context, host_mem_size);
    r0 = 0;
}

static uint32_t call_guest_callback_2(uint32_t callback, uint32_t arg0,
                                      uint32_t arg1) {
    const uint32_t saved_sp = sp;
    const uint32_t saved_lr = lr;
    r0 = arg0;
    r1 = arg1;
    vita_dispatch(0u, callback);
    const uint32_t result = r0;
    sp = saved_sp;
    lr = saved_lr;
    return result;
}

static gxm_context *find_context(uint32_t address) {
    for (uint32_t i = 0; i < g_gxm.context_count; i++) {
        if (g_gxm.contexts[i].address == address)
            return &g_gxm.contexts[i];
    }
    return NULL;
}

void vita_hle_sceGxmFinish(void) {
    gxm_context *context = find_context(r0);
    if (!context) {
        vita_hle_trace("sceGxmFinish(invalid context 0x%08X)", r0);
        return;
    }
    context->finish_count++;
    vita_hle_trace(
        "sceGxmFinish(context 0x%08X) completed empty queue #%u",
        context->address, context->finish_count);
}

void vita_hle_sceGxmSetVertexProgram(void) {
    gxm_context *context = find_context(r0);
    if (!context || !r1) {
        vita_hle_trace(
            "sceGxmSetVertexProgram(context 0x%08X, program 0x%08X) "
            "ignored invalid pointer",
            r0, r1);
        return;
    }
    context->vertex_program = r1;
    vita_hle_trace(
        "sceGxmSetVertexProgram(context 0x%08X, program 0x%08X)",
        context->address, context->vertex_program);
}

void vita_hle_sceGxmSetFragmentProgram(void) {
    gxm_context *context = find_context(r0);
    if (!context || !r1) {
        vita_hle_trace(
            "sceGxmSetFragmentProgram(context 0x%08X, program 0x%08X) "
            "ignored invalid pointer",
            r0, r1);
        return;
    }
    context->fragment_program = r1;
    vita_hle_trace(
        "sceGxmSetFragmentProgram(context 0x%08X, program 0x%08X)",
        context->address, context->fragment_program);
}

void vita_hle_sceGxmProgramCheck(void) {
    const uint32_t program = r0;
    if (!vita_mem_ptr(program, 4u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    if (vita_read32(program) != UINT32_C(0x00505847)) {
        r0 = SCE_GXM_ERROR_NULL_PROGRAM;
        return;
    }
    vita_hle_trace("sceGxmProgramCheck(program 0x%08X) -> valid GXP",
                   program);
    r0 = 0;
}

static int gxp_program_size(uint32_t program, uint32_t *size) {
    if (!size || !vita_mem_ptr(program, 12u) ||
        vita_read32(program) != UINT32_C(0x00505847))
        return 0;
    const uint32_t declared_size = vita_read32(program + 8u);
    if (declared_size < 12u || !vita_mem_ptr(program, declared_size))
        return 0;
    *size = declared_size;
    return 1;
}

static int gxp_program_u32(uint32_t program, uint32_t offset,
                           uint32_t *value) {
    uint32_t size = 0u;
    if (!value || !gxp_program_size(program, &size) ||
        offset > size || size - offset < 4u)
        return 0;
    *value = vita_read32(program + offset);
    return 1;
}

void vita_hle_sceGxmProgramGetSize(void) {
    const uint32_t program = r0;
    uint32_t size = 0u;
    if (!gxp_program_size(program, &size)) {
        vita_hle_trace(
            "sceGxmProgramGetSize(invalid program 0x%08X) -> 0",
            program);
        r0 = 0u;
        return;
    }
    vita_hle_trace("sceGxmProgramGetSize(program 0x%08X) -> 0x%X",
                   program, size);
    r0 = size;
}

void vita_hle_sceGxmProgramGetType(void) {
    const uint32_t program = r0;
    uint32_t flags = 0u;
    if (!gxp_program_u32(program, 20u, &flags)) {
        vita_hle_trace(
            "sceGxmProgramGetType(invalid program 0x%08X) -> 0",
            program);
        r0 = 0u;
        return;
    }
    r0 = flags & 1u;
    vita_hle_trace(
        "sceGxmProgramGetType(program 0x%08X) -> %u",
        program, r0);
}

void vita_hle_sceGxmProgramIsNativeColorUsed(void) {
    const uint32_t program = r0;
    uint32_t flags = 0u;
    if (!gxp_program_u32(program, 20u, &flags)) {
        r0 = 0u;
        return;
    }
    r0 = (flags & UINT32_C(0x40)) != 0u;
    vita_hle_trace(
        "sceGxmProgramIsNativeColorUsed(program 0x%08X) -> %u",
        program, r0);
}

void vita_hle_sceGxmProgramGetParameterCount(void) {
    const uint32_t program = r0;
    uint32_t count = 0u;
    if (!gxp_program_u32(program, 36u, &count)) {
        vita_hle_trace(
            "sceGxmProgramGetParameterCount(invalid program 0x%08X) -> 0",
            program);
        r0 = 0u;
        return;
    }
    vita_hle_trace(
        "sceGxmProgramGetParameterCount(program 0x%08X) -> %u",
        program, count);
    r0 = count;
}

void vita_hle_sceGxmProgramGetDefaultUniformBufferSize(void) {
    const uint32_t program = r0;
    uint32_t count = 0u;
    if (!gxp_program_u32(program, 100u, &count)) {
        vita_hle_trace(
            "sceGxmProgramGetDefaultUniformBufferSize(invalid program "
            "0x%08X) -> 0",
            program);
        r0 = 0u;
        return;
    }
    r0 = count * 4u;
    vita_hle_trace(
        "sceGxmProgramGetDefaultUniformBufferSize(program 0x%08X) "
        "-> 0x%X",
        program, r0);
}

static uint32_t gxp_program_parameter(uint32_t program, uint32_t index) {
    uint32_t size = 0u;
    uint32_t count = 0u;
    uint32_t relative_offset = 0u;
    if (!gxp_program_size(program, &size) ||
        !gxp_program_u32(program, 36u, &count) ||
        !gxp_program_u32(program, 40u, &relative_offset) ||
        index >= count)
        return 0u;

    const uint64_t table =
        (uint64_t)program + 40u + relative_offset;
    const uint64_t table_size = (uint64_t)count * 16u;
    const uint64_t end = (uint64_t)program + size;
    if (table < program || table > end || table_size > end - table)
        return 0u;
    return (uint32_t)(table + (uint64_t)index * 16u);
}

void vita_hle_sceGxmProgramGetParameter(void) {
    const uint32_t program = r0;
    const uint32_t index = r1;
    r0 = gxp_program_parameter(program, index);
    vita_hle_trace(
        "sceGxmProgramGetParameter(program 0x%08X, index %u) -> "
        "0x%08X",
        program, index, r0);
}

static const char *gxp_guest_cstr(uint32_t address) {
    const uint32_t available = vita_mem_avail(address);
    const char *text = available
        ? (const char *)vita_mem_ptr(address, 1u)
        : NULL;
    return text && memchr(text, '\0', available) ? text : NULL;
}

static uint32_t gxp_parameter_name(uint32_t parameter) {
    if (!vita_mem_ptr(parameter, 16u))
        return 0u;
    const uint32_t encoded_offset = vita_read32(parameter);
    const int64_t relative_offset =
        encoded_offset <= INT32_MAX
            ? (int64_t)encoded_offset
            : -((int64_t)(UINT32_MAX - encoded_offset) + 1);
    const int64_t name_address =
        (int64_t)parameter + relative_offset;
    if (name_address <= 0 || name_address > UINT32_MAX)
        return 0u;
    const uint32_t name = (uint32_t)name_address;
    return gxp_guest_cstr(name) ? name : 0u;
}

void vita_hle_sceGxmProgramFindParameterByName(void) {
    const uint32_t program = r0;
    const uint32_t name = r1;
    uint32_t count = 0u;
    const char *query = gxp_guest_cstr(name);
    if (!query || !gxp_program_u32(program, 36u, &count)) {
        r0 = 0u;
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t parameter =
            gxp_program_parameter(program, i);
        const uint32_t parameter_name =
            gxp_parameter_name(parameter);
        const char *candidate =
            gxp_guest_cstr(parameter_name);
        if (candidate && strcmp(candidate, query) == 0) {
            r0 = parameter;
            vita_hle_trace(
                "sceGxmProgramFindParameterByName(program 0x%08X, "
                "name 0x%08X) -> 0x%08X",
                program, name, parameter);
            return;
        }
    }
    r0 = 0u;
    vita_hle_trace(
        "sceGxmProgramFindParameterByName(program 0x%08X, "
        "name 0x%08X) -> 0",
        program, name);
}

void vita_hle_sceGxmProgramParameterGetCategory(void) {
    const uint32_t parameter = r0;
    if (!vita_mem_ptr(parameter, 16u)) {
        vita_hle_trace(
            "sceGxmProgramParameterGetCategory(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }
    r0 = vita_read8(parameter + 4u) & 0xFu;
    vita_hle_trace(
        "sceGxmProgramParameterGetCategory(parameter 0x%08X) -> %u",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterGetArraySize(void) {
    const uint32_t parameter = r0;
    if (!vita_mem_ptr(parameter, 16u)) {
        vita_hle_trace(
            "sceGxmProgramParameterGetArraySize(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }
    r0 = vita_read32(parameter + 8u);
    vita_hle_trace(
        "sceGxmProgramParameterGetArraySize(parameter 0x%08X) -> %u",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterGetType(void) {
    const uint32_t parameter = r0;
    if (!vita_mem_ptr(parameter, 16u)) {
        vita_hle_trace(
            "sceGxmProgramParameterGetType(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }
    r0 = vita_read8(parameter + 4u) >> 4u;
    vita_hle_trace(
        "sceGxmProgramParameterGetType(parameter 0x%08X) -> %u",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterGetContainerIndex(void) {
    const uint32_t parameter = r0;
    if (!vita_mem_ptr(parameter, 16u)) {
        vita_hle_trace(
            "sceGxmProgramParameterGetContainerIndex(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }
    r0 = vita_read8(parameter + 5u) >> 4u;
    vita_hle_trace(
        "sceGxmProgramParameterGetContainerIndex(parameter 0x%08X) "
        "-> %u",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterGetComponentCount(void) {
    const uint32_t parameter = r0;
    if (!vita_mem_ptr(parameter, 16u)) {
        vita_hle_trace(
            "sceGxmProgramParameterGetComponentCount(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }
    r0 = vita_read8(parameter + 5u) & 0xFu;
    vita_hle_trace(
        "sceGxmProgramParameterGetComponentCount(parameter 0x%08X) "
        "-> %u",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterGetResourceIndex(void) {
    const uint32_t parameter = r0;
    if (!vita_mem_ptr(parameter, 16u)) {
        vita_hle_trace(
            "sceGxmProgramParameterGetResourceIndex(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }
    r0 = vita_read32(parameter + 12u);
    vita_hle_trace(
        "sceGxmProgramParameterGetResourceIndex(parameter 0x%08X) "
        "-> %u",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterGetName(void) {
    const uint32_t parameter = r0;
    const uint32_t name = gxp_parameter_name(parameter);
    if (!name) {
        vita_hle_trace(
            "sceGxmProgramParameterGetName(invalid parameter "
            "0x%08X) -> 0",
            parameter);
        r0 = 0u;
        return;
    }

    r0 = name;
    vita_hle_trace(
        "sceGxmProgramParameterGetName(parameter 0x%08X) -> "
        "0x%08X",
        parameter, r0);
}

void vita_hle_sceGxmProgramParameterIsRegFormat(void) {
    const uint32_t program = r0;
    const uint32_t parameter = r1;
    uint32_t size = 0u;
    uint32_t flags = 0u;
    uint32_t varyings_offset = 0u;
    if (!gxp_program_size(program, &size) ||
        !gxp_program_u32(program, 20u, &flags) ||
        !gxp_program_u32(program, 44u, &varyings_offset) ||
        !vita_mem_ptr(parameter, 16u)) {
        r0 = 0u;
        return;
    }
    const uint32_t category = vita_read8(parameter + 4u) & 0xFu;
    const uint32_t resource_index = vita_read32(parameter + 12u);
    if ((flags & 1u) || category != 0u || resource_index >= 64u) {
        r0 = 0u;
        return;
    }

    const uint64_t varyings =
        (uint64_t)program + 44u + varyings_offset;
    const uint64_t end = (uint64_t)program + size;
    if (varyings < program || varyings > end ||
        end - varyings < 16u) {
        r0 = 0u;
        return;
    }
    const uint32_t register_bits = vita_read32(
        (uint32_t)varyings + 8u +
        (resource_index >= 32u ? 4u : 0u));
    r0 = (register_bits >> (resource_index & 31u)) & 1u;
    vita_hle_trace(
        "sceGxmProgramParameterIsRegFormat(program 0x%08X, "
        "parameter 0x%08X, resource %u) -> %u",
        program, parameter, resource_index, r0);
}

void vita_hle_sceGxmShaderPatcherCreate(void) {
    const uint32_t params = r0;
    const uint32_t output = r1;
    if (!vita_mem_ptr(params, 68u) || !vita_mem_ptr(output, 4u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }

    const uint32_t user_data = vita_read32(params);
    const uint32_t host_alloc_callback = vita_read32(params + 4u);
    const uint32_t buffer_mem = vita_read32(params + 20u);
    const uint32_t buffer_mem_size = vita_read32(params + 24u);
    const uint32_t vertex_usse_mem = vita_read32(params + 36u);
    const uint32_t vertex_usse_mem_size = vita_read32(params + 40u);
    const uint32_t fragment_usse_mem = vita_read32(params + 56u);
    const uint32_t fragment_usse_mem_size = vita_read32(params + 60u);
    if (!host_alloc_callback ||
        (buffer_mem_size && !vita_mem_ptr(buffer_mem, buffer_mem_size)) ||
        (vertex_usse_mem_size &&
         !vita_mem_ptr(vertex_usse_mem, vertex_usse_mem_size)) ||
        (fragment_usse_mem_size &&
         !vita_mem_ptr(fragment_usse_mem, fragment_usse_mem_size))) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    if (g_gxm.shader_patcher_count == GXM_SHADER_PATCHER_LIMIT) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }

    const uint32_t patcher = call_guest_callback_2(
        host_alloc_callback, user_data, GXM_SHADER_PATCHER_OBJECT_SIZE);
    if (!patcher ||
        !vita_mem_ptr(patcher, GXM_SHADER_PATCHER_OBJECT_SIZE)) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }
    for (uint32_t i = 0; i < g_gxm.shader_patcher_count; i++) {
        if (g_gxm.shader_patchers[i].address == patcher) {
            r0 = SCE_GXM_ERROR_INVALID_VALUE;
            return;
        }
    }

    gxm_shader_patcher *created =
        &g_gxm.shader_patchers[g_gxm.shader_patcher_count++];
    created->address = patcher;
    for (uint32_t i = 0; i < 17u; i++)
        created->params[i] = vita_read32(params + i * 4u);
    (void)vita_mem_set_guest(
        patcher, 0, GXM_SHADER_PATCHER_OBJECT_SIZE);
    vita_write32(output, patcher);
    vita_hle_trace(
        "sceGxmShaderPatcherCreate(params 0x%08X) -> 0x%08X",
        params, patcher);
    r0 = 0;
}

static gxm_shader_patcher *find_shader_patcher(uint32_t address) {
    for (uint32_t i = 0; i < g_gxm.shader_patcher_count; i++) {
        if (g_gxm.shader_patchers[i].address == address)
            return &g_gxm.shader_patchers[i];
    }
    return NULL;
}

void vita_hle_sceGxmShaderPatcherRegisterProgram(void) {
    const uint32_t patcher_address = r0;
    const uint32_t program = r1;
    const uint32_t output = r2;
    gxm_shader_patcher *patcher =
        find_shader_patcher(patcher_address);
    uint32_t program_size = 0u;
    if (!patcher || !gxp_program_size(program, &program_size) ||
        !vita_mem_ptr(output, 4u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    if (g_gxm.registered_program_count ==
        GXM_REGISTERED_PROGRAM_LIMIT) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }

    const uint32_t registration = call_guest_callback_2(
        patcher->params[1], patcher->params[0],
        GXM_REGISTERED_PROGRAM_SIZE);
    if (!registration ||
        !vita_mem_ptr(registration, GXM_REGISTERED_PROGRAM_SIZE)) {
        r0 = SCE_GXM_ERROR_OUT_OF_MEMORY;
        return;
    }
    for (uint32_t i = 0; i < g_gxm.registered_program_count; i++) {
        if (g_gxm.registered_programs[i].address == registration) {
            r0 = SCE_GXM_ERROR_INVALID_VALUE;
            return;
        }
    }

    gxm_registered_program *registered =
        &g_gxm.registered_programs[g_gxm.registered_program_count++];
    registered->address = registration;
    registered->patcher = patcher_address;
    registered->program = program;
    vita_write32(registration, program);
    vita_write32(output, registration);
    vita_hle_trace(
        "sceGxmShaderPatcherRegisterProgram(patcher 0x%08X, "
        "program 0x%08X, size 0x%X) -> 0x%08X",
        patcher_address, program, program_size, registration);
    r0 = 0u;
}

void vita_hle_sceGxmTextureInitLinear(void) {
    const uint32_t texture = r0;
    const uint32_t data = r1;
    const uint32_t format = r2;
    const uint32_t width = r3;
    const uint32_t height = vita_read32(sp);
    const uint32_t mip_count = vita_read32(sp + 4u);

    if (!vita_mem_ptr(texture, 16u)) {
        r0 = SCE_GXM_ERROR_INVALID_POINTER;
        return;
    }
    if (width > 4096u || height > 4096u || mip_count > 13u) {
        r0 = SCE_GXM_ERROR_INVALID_VALUE;
        return;
    }

    const uint32_t control0 =
        UINT32_C(0x03E00090) |
        ((mip_count - 1u < 15u ? mip_count - 1u : 15u) << 17u) |
        (format & UINT32_C(0x80000000));
    const uint32_t control1 =
        ((height - 1u) & UINT32_C(0xFFF)) |
        (((width - 1u) & UINT32_C(0xFFF)) << 12u) |
        (format & UINT32_C(0x1F000000)) |
        SCE_GXM_TEXTURE_LINEAR;
    const uint32_t control2 = data & UINT32_C(0xFFFFFFFC);
    const uint32_t control3 =
        UINT32_C(0x80000000) | ((format & UINT32_C(0x7000)) << 16u);

    vita_write32(texture, control0);
    vita_write32(texture + 4u, control1);
    vita_write32(texture + 8u, control2);
    vita_write32(texture + 12u, control3);
    vita_hle_trace(
        "sceGxmTextureInitLinear(texture 0x%08X, data 0x%08X, "
        "format 0x%08X, %ux%u, mips %u)",
        texture, data, format, width, height, mip_count);
    r0 = 0;
}
