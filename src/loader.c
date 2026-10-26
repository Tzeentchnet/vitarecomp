/* loader.c — ELF32 segment loading into guest memory. */

#include "vitarecomp/loader.h"
#include "vitarecomp/mem.h"
#include "excl_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_LOAD 1

#define ELF32_HEADER_SIZE 0x34u
#define ELF32_PROGRAM_HEADER_SIZE 0x20u
#define ELF32_SECTION_HEADER_SIZE 0x28u

#define ET_EXEC 2u
#define ET_SCE_EXEC 0xFE00u
#define ET_SCE_RELEXEC 0xFE04u
#define EM_ARM 40u

#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_SCE_RELA 0x60000000u

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define PF_MASK (PF_X | PF_W | PF_R)

#define VITA_LOADER_VALIDATION_MASK                                      \
    (VITA_LOADER_VALIDATE_MODULE_INFO | VITA_LOADER_VALIDATE_RELOCATIONS)

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

/* Walk the program headers, calling back for each PT_LOAD. `load` selects
 * whether the bytes are copied or merely measured, so probing and loading
 * cannot disagree about which segments count. */
static int walk(const char *path, vita_load_info *info, int load) {
    size_t size = 0;
    uint8_t *buf = slurp(path, &size);
    if (!buf) return 1;

    memset(info, 0, sizeof(*info));
    info->lowest = 0xFFFFFFFFu;

    if (size < 0x34 || buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        free(buf); return 1;
    }

    uint32_t phoff     = r32(buf + 0x1C);
    uint16_t phentsize = r16(buf + 0x2A);
    uint16_t phnum     = r16(buf + 0x2C);
    if (phentsize == 0) { free(buf); return 1; }

    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t p = (uint64_t)phoff + (uint64_t)i * phentsize;
        if (p + 0x20 > size) break;

        const uint8_t *ph = buf + p;
        if (r32(ph + 0x00) != PT_LOAD) continue;

        uint32_t offset = r32(ph + 0x04);
        uint32_t vaddr  = r32(ph + 0x08);
        uint32_t filesz = r32(ph + 0x10);
        uint32_t memsz  = r32(ph + 0x14);

        if ((uint64_t)offset + filesz > size) continue;

        if (vaddr < info->lowest) info->lowest = vaddr;
        if (vaddr + memsz > info->highest) info->highest = vaddr + memsz;
        info->segments++;

        if (!load) continue;

        void *dst = vita_mem_ptr(vaddr, memsz);
        if (!dst) {
            fprintf(stderr,
                    "vitarecomp: segment %u (0x%08X, %u bytes) is outside guest memory\n",
                    i, vaddr, memsz);
            free(buf);
            return 1;
        }

        /* memsz exceeds filesz for .bss, and the difference must be zeroed
         * rather than left as whatever the allocation held. A program whose
         * .bss starts non-zero fails in ways that look like logic bugs. */
        memcpy(dst, buf + offset, filesz);
        if (memsz > filesz) memset((uint8_t *)dst + filesz, 0, memsz - filesz);
    }

    free(buf);
    return info->segments ? 0 : 1;
}

int vita_probe_elf(const char *path, vita_load_info *info) {
    return walk(path, info, 0);
}

int vita_load_elf(const char *path, vita_load_info *info) {
    return walk(path, info, 1);
}

/* --- strict in-memory loader ------------------------------------------------ */

typedef struct {
    uint16_t type;
    uint16_t machine;
    uint32_t entry;
    uint32_t phoff;
    uint32_t flags;
    uint32_t shoff;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} strict_elf_header;

typedef struct {
    strict_elf_header header;
    vita_loader_module_metadata metadata;
    vita_load_info info;
    size_t first_segment;
    uint32_t automatic_validation;
} strict_module_plan;

typedef struct {
    uint32_t module_index;
    uint32_t program_header_index;
    uint32_t virtual_address;
    uint32_t end_address;
    uint32_t file_size;
    uint32_t memory_size;
    const uint8_t *source;
    void *destination;
} strict_load_segment;

typedef struct {
    const vita_loader_module *modules;
    size_t module_count;
    strict_module_plan *module_plans;
    vita_loader_segment_metadata *segments;
    strict_load_segment *loads;
    size_t load_count;
} strict_load_plan;

static void clear_loader_error(vita_loader_error *error) {
    if (!error) return;
    memset(error, 0, sizeof(*error));
    error->status = VITA_LOADER_OK;
    error->module_index = VITA_LOADER_INDEX_NONE;
    error->program_header_index = VITA_LOADER_INDEX_NONE;
    error->record_index = VITA_LOADER_INDEX_NONE;
    error->related_module_index = VITA_LOADER_INDEX_NONE;
    error->related_program_header_index = VITA_LOADER_INDEX_NONE;
}

static vita_loader_status loader_fail(vita_loader_error *error,
                                      vita_loader_status status,
                                      uint32_t module_index,
                                      uint32_t program_header_index,
                                      uint32_t record_index) {
    clear_loader_error(error);
    if (error) {
        error->status = status;
        error->module_index = module_index;
        error->program_header_index = program_header_index;
        error->record_index = record_index;
    }
    return status;
}

static int supported_elf_type(uint16_t type) {
    return type == ET_EXEC || type == ET_SCE_EXEC ||
           type == ET_SCE_RELEXEC;
}

static int power_of_two(uint32_t value) {
    return value && !(value & (value - 1u));
}

static vita_loader_status parse_strict_header(
    const vita_loader_module *module,
    uint32_t module_index,
    strict_elf_header *header,
    vita_loader_error *error) {
    const uint8_t *bytes = module->bytes;
    const size_t size = module->size;

    if (!bytes)
        return loader_fail(error, VITA_LOADER_ERROR_INVALID_ARGUMENT,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (size < ELF32_HEADER_SIZE)
        return loader_fail(error, VITA_LOADER_ERROR_TRUNCATED,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (bytes[0] != 0x7Fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F')
        return loader_fail(error, VITA_LOADER_ERROR_BAD_MAGIC,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (bytes[4] != 1u)
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_CLASS,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (bytes[5] != 1u)
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_ENDIAN,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (bytes[6] != 1u || r32(bytes + 0x14) != 1u)
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_VERSION,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);

    memset(header, 0, sizeof(*header));
    header->type = r16(bytes + 0x10);
    header->machine = r16(bytes + 0x12);
    header->entry = r32(bytes + 0x18);
    header->phoff = r32(bytes + 0x1C);
    header->shoff = r32(bytes + 0x20);
    header->flags = r32(bytes + 0x24);
    header->phnum = r16(bytes + 0x2C);
    header->shentsize = r16(bytes + 0x2E);
    header->shnum = r16(bytes + 0x30);
    header->shstrndx = r16(bytes + 0x32);

    if (!supported_elf_type(header->type))
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_TYPE,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (header->machine != EM_ARM)
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_MACHINE,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (r16(bytes + 0x28) != ELF32_HEADER_SIZE)
        return loader_fail(error, VITA_LOADER_ERROR_HEADER_SIZE,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (r16(bytes + 0x2A) != ELF32_PROGRAM_HEADER_SIZE)
        return loader_fail(error, VITA_LOADER_ERROR_PROGRAM_HEADER_SIZE,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);

    if (header->phnum == UINT16_MAX)
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_LAYOUT,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (header->phnum) {
        uint64_t table_end =
            (uint64_t)header->phoff +
            (uint64_t)header->phnum * ELF32_PROGRAM_HEADER_SIZE;
        if (header->phoff < ELF32_HEADER_SIZE ||
            table_end > UINT32_MAX || table_end > size)
            return loader_fail(error,
                               VITA_LOADER_ERROR_PROGRAM_HEADER_TABLE,
                               module_index, VITA_LOADER_INDEX_NONE,
                               VITA_LOADER_INDEX_NONE);
    }

    if (header->shnum) {
        uint64_t table_end =
            (uint64_t)header->shoff +
            (uint64_t)header->shnum * ELF32_SECTION_HEADER_SIZE;
        if (header->shentsize != ELF32_SECTION_HEADER_SIZE ||
            header->shoff < ELF32_HEADER_SIZE ||
            table_end > UINT32_MAX || table_end > size ||
            (header->shstrndx != 0u &&
             header->shstrndx >= header->shnum))
            return loader_fail(error,
                               VITA_LOADER_ERROR_SECTION_HEADER_TABLE,
                               module_index, VITA_LOADER_INDEX_NONE,
                               VITA_LOADER_INDEX_NONE);
    } else if (header->shoff != 0u || header->shstrndx != 0u ||
               (header->shentsize != 0u &&
                header->shentsize != ELF32_SECTION_HEADER_SIZE)) {
        /* Extended section numbering needs section-header parsing, which the
         * runtime loader intentionally does not implement. */
        return loader_fail(error, VITA_LOADER_ERROR_UNSUPPORTED_LAYOUT,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    }

    return VITA_LOADER_OK;
}

static void free_strict_plan(strict_load_plan *plan) {
    if (!plan) return;
    free(plan->loads);
    free(plan->segments);
    free(plan->module_plans);
    memset(plan, 0, sizeof(*plan));
}

static int load_segment_order(const void *left, const void *right) {
    const strict_load_segment *a = (const strict_load_segment *)left;
    const strict_load_segment *b = (const strict_load_segment *)right;
    if (a->virtual_address < b->virtual_address) return -1;
    if (a->virtual_address > b->virtual_address) return 1;
    if (a->module_index < b->module_index) return -1;
    if (a->module_index > b->module_index) return 1;
    if (a->program_header_index < b->program_header_index) return -1;
    if (a->program_header_index > b->program_header_index) return 1;
    return 0;
}

static int same_segment_metadata(
    const vita_loader_segment_metadata *actual,
    const vita_loader_segment_metadata *expected) {
    return actual->program_header_index == expected->program_header_index &&
           actual->file_offset == expected->file_offset &&
           actual->virtual_address == expected->virtual_address &&
           actual->physical_address == expected->physical_address &&
           actual->file_size == expected->file_size &&
           actual->memory_size == expected->memory_size &&
           actual->flags == expected->flags &&
           actual->alignment == expected->alignment;
}

static vita_loader_status compare_expected_metadata(
    const strict_module_plan *module_plan,
    const vita_loader_module_metadata *expected,
    uint32_t module_index,
    vita_loader_error *error) {
    const vita_loader_module_metadata *actual = &module_plan->metadata;
    if (!expected) return VITA_LOADER_OK;
    if (expected->segment_count && !expected->segments)
        return loader_fail(error, VITA_LOADER_ERROR_INVALID_ARGUMENT,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (actual->elf_type != expected->elf_type ||
        actual->machine != expected->machine ||
        actual->entry != expected->entry ||
        actual->elf_flags != expected->elf_flags ||
        actual->program_header_count != expected->program_header_count)
        return loader_fail(error, VITA_LOADER_ERROR_METADATA_MISMATCH,
                           module_index, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (actual->segment_count != expected->segment_count) {
        uint32_t record = actual->segment_count < expected->segment_count
                        ? actual->segment_count : expected->segment_count;
        return loader_fail(error, VITA_LOADER_ERROR_METADATA_MISMATCH,
                           module_index, VITA_LOADER_INDEX_NONE, record);
    }
    for (uint32_t i = 0; i < actual->segment_count; i++) {
        if (!same_segment_metadata(&actual->segments[i],
                                   &expected->segments[i]))
            return loader_fail(
                error, VITA_LOADER_ERROR_METADATA_MISMATCH, module_index,
                actual->segments[i].program_header_index, i);
    }
    return VITA_LOADER_OK;
}

static vita_loader_status run_validation_extension(
    const vita_loader_module *module,
    const strict_module_plan *module_plan,
    uint32_t module_index,
    vita_loader_error *error) {
    const uint32_t requested =
        module->required_validation | module_plan->automatic_validation;
    if (!requested) return VITA_LOADER_OK;

    if (!module->validate) {
        vita_loader_status status =
            loader_fail(error, VITA_LOADER_ERROR_VALIDATION_REQUIRED,
                        module_index, VITA_LOADER_INDEX_NONE,
                        VITA_LOADER_INDEX_NONE);
        if (error) error->required_validation = requested;
        return status;
    }

    uint32_t completed = 0;
    vita_loader_error extension_error;
    clear_loader_error(&extension_error);
    extension_error.required_validation = requested;
    vita_loader_status status = module->validate(
        module->bytes, module->size, &module_plan->metadata, requested,
        &completed, &extension_error, module->validation_userdata);
    if (status != VITA_LOADER_OK) {
        if (error) {
            *error = extension_error;
            error->status = status;
            error->module_index = module_index;
            error->required_validation = requested;
            error->completed_validation = completed;
        }
        return status;
    }
    if ((completed & requested) != requested) {
        status = loader_fail(error, VITA_LOADER_ERROR_VALIDATION_REQUIRED,
                             module_index, VITA_LOADER_INDEX_NONE,
                             VITA_LOADER_INDEX_NONE);
        if (error) {
            error->required_validation = requested;
            error->completed_validation = completed;
        }
        return status;
    }
    return VITA_LOADER_OK;
}

static vita_loader_status build_strict_plan(
    const vita_loader_module *modules,
    size_t module_count,
    int require_guest_memory,
    strict_load_plan *plan,
    vita_loader_error *error) {
    size_t maximum_segments = 0;
    size_t load_cursor = 0;
    vita_loader_status status;

    clear_loader_error(error);
    memset(plan, 0, sizeof(*plan));
    if (!modules || !module_count || module_count > UINT32_MAX)
        return loader_fail(error, VITA_LOADER_ERROR_INVALID_ARGUMENT,
                           VITA_LOADER_INDEX_NONE, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);
    if (module_count > SIZE_MAX / sizeof(*plan->module_plans))
        return loader_fail(error, VITA_LOADER_ERROR_NO_MEMORY,
                           VITA_LOADER_INDEX_NONE, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);

    plan->modules = modules;
    plan->module_count = module_count;
    plan->module_plans = (strict_module_plan *)calloc(
        module_count, sizeof(*plan->module_plans));
    if (!plan->module_plans)
        return loader_fail(error, VITA_LOADER_ERROR_NO_MEMORY,
                           VITA_LOADER_INDEX_NONE, VITA_LOADER_INDEX_NONE,
                           VITA_LOADER_INDEX_NONE);

    for (size_t i = 0; i < module_count; i++) {
        if (modules[i].required_validation &
            ~((uint32_t)VITA_LOADER_VALIDATION_MASK)) {
            status = loader_fail(error,
                                 VITA_LOADER_ERROR_INVALID_ARGUMENT,
                                 (uint32_t)i, VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE);
            goto fail;
        }
        status = parse_strict_header(&modules[i], (uint32_t)i,
                                     &plan->module_plans[i].header, error);
        if (status != VITA_LOADER_OK) goto fail;
        if (maximum_segments >
            SIZE_MAX - plan->module_plans[i].header.phnum) {
            status = loader_fail(error, VITA_LOADER_ERROR_NO_MEMORY,
                                 (uint32_t)i, VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE);
            goto fail;
        }
        maximum_segments += plan->module_plans[i].header.phnum;
    }

    if (maximum_segments) {
        if (maximum_segments > SIZE_MAX / sizeof(*plan->segments) ||
            maximum_segments > SIZE_MAX / sizeof(*plan->loads)) {
            status = loader_fail(error, VITA_LOADER_ERROR_NO_MEMORY,
                                 VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE);
            goto fail;
        }
        plan->segments = (vita_loader_segment_metadata *)calloc(
            maximum_segments, sizeof(*plan->segments));
        plan->loads = (strict_load_segment *)calloc(
            maximum_segments, sizeof(*plan->loads));
        if (!plan->segments || !plan->loads) {
            status = loader_fail(error, VITA_LOADER_ERROR_NO_MEMORY,
                                 VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE);
            goto fail;
        }
    }

    for (size_t module_index = 0;
         module_index < module_count; module_index++) {
        const vita_loader_module *module = &modules[module_index];
        strict_module_plan *module_plan =
            &plan->module_plans[module_index];
        const strict_elf_header *header = &module_plan->header;
        module_plan->first_segment = load_cursor;
        module_plan->info.lowest = UINT32_MAX;
        module_plan->metadata.elf_type = header->type;
        module_plan->metadata.machine = header->machine;
        module_plan->metadata.entry = header->entry;
        module_plan->metadata.elf_flags = header->flags;
        module_plan->metadata.program_header_count = header->phnum;

        if (header->type == ET_SCE_EXEC ||
            header->type == ET_SCE_RELEXEC)
            module_plan->automatic_validation |=
                VITA_LOADER_VALIDATE_MODULE_INFO;
        if (header->type == ET_SCE_RELEXEC)
            module_plan->automatic_validation |=
                VITA_LOADER_VALIDATE_RELOCATIONS;

        for (uint32_t ph_index = 0; ph_index < header->phnum; ph_index++) {
            const uint8_t *ph =
                module->bytes + header->phoff +
                (size_t)ph_index * ELF32_PROGRAM_HEADER_SIZE;
            uint32_t type = r32(ph + 0x00);
            uint32_t offset = r32(ph + 0x04);
            uint32_t vaddr = r32(ph + 0x08);
            uint32_t paddr = r32(ph + 0x0C);
            uint32_t filesz = r32(ph + 0x10);
            uint32_t memsz = r32(ph + 0x14);
            uint32_t flags = r32(ph + 0x18);
            uint32_t align = r32(ph + 0x1C);
            uint64_t file_end = (uint64_t)offset + filesz;

            if (align > 1u && !power_of_two(align)) {
                status = loader_fail(error, VITA_LOADER_ERROR_ALIGNMENT,
                                     (uint32_t)module_index, ph_index,
                                     VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if (file_end > UINT32_MAX) {
                status = loader_fail(
                    error, VITA_LOADER_ERROR_FILE_RANGE_OVERFLOW,
                    (uint32_t)module_index, ph_index,
                    VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if (file_end > module->size) {
                status = loader_fail(error, VITA_LOADER_ERROR_FILE_RANGE,
                                     (uint32_t)module_index, ph_index,
                                     VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if (type == PT_DYNAMIC || type == PT_INTERP) {
                status = loader_fail(
                    error, VITA_LOADER_ERROR_UNSUPPORTED_LAYOUT,
                    (uint32_t)module_index, ph_index,
                    VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if (type == PT_SCE_RELA)
                module_plan->automatic_validation |=
                    VITA_LOADER_VALIDATE_RELOCATIONS;
            if (type != PT_LOAD) continue;

            if (filesz > memsz) {
                status = loader_fail(error, VITA_LOADER_ERROR_FILE_SIZE,
                                     (uint32_t)module_index, ph_index,
                                     VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if (!memsz) {
                status = loader_fail(
                    error, VITA_LOADER_ERROR_ZERO_LOAD_RANGE,
                    (uint32_t)module_index, ph_index,
                    VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            uint64_t memory_end = (uint64_t)vaddr + memsz;
            if (memory_end > UINT32_MAX) {
                status = loader_fail(
                    error, VITA_LOADER_ERROR_VIRTUAL_RANGE_OVERFLOW,
                    (uint32_t)module_index, ph_index,
                    VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if ((flags & ~PF_MASK) != 0u || (flags & PF_R) == 0u ||
                (flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
                status = loader_fail(error, VITA_LOADER_ERROR_LOAD_FLAGS,
                                     (uint32_t)module_index, ph_index,
                                     VITA_LOADER_INDEX_NONE);
                goto fail;
            }
            if (align > 1u &&
                (vaddr & (align - 1u)) !=
                (offset & (align - 1u))) {
                status = loader_fail(error, VITA_LOADER_ERROR_ALIGNMENT,
                                     (uint32_t)module_index, ph_index,
                                     VITA_LOADER_INDEX_NONE);
                goto fail;
            }

            vita_loader_segment_metadata *segment =
                &plan->segments[load_cursor];
            segment->program_header_index = ph_index;
            segment->file_offset = offset;
            segment->virtual_address = vaddr;
            segment->physical_address = paddr;
            segment->file_size = filesz;
            segment->memory_size = memsz;
            segment->flags = flags;
            segment->alignment = align;

            strict_load_segment *load = &plan->loads[load_cursor];
            load->module_index = (uint32_t)module_index;
            load->program_header_index = ph_index;
            load->virtual_address = vaddr;
            load->end_address = (uint32_t)memory_end;
            load->file_size = filesz;
            load->memory_size = memsz;
            load->source = module->bytes + offset;

            if (vaddr < module_plan->info.lowest)
                module_plan->info.lowest = vaddr;
            if ((uint32_t)memory_end > module_plan->info.highest)
                module_plan->info.highest = (uint32_t)memory_end;
            module_plan->info.segments++;
            load_cursor++;
        }

        if (!module_plan->info.segments) {
            status = loader_fail(
                error, VITA_LOADER_ERROR_NO_LOAD_SEGMENTS,
                (uint32_t)module_index, VITA_LOADER_INDEX_NONE,
                VITA_LOADER_INDEX_NONE);
            goto fail;
        }
        module_plan->metadata.segment_count =
            (uint32_t)module_plan->info.segments;
        module_plan->metadata.segments =
            &plan->segments[module_plan->first_segment];
    }
    plan->load_count = load_cursor;

    qsort(plan->loads, plan->load_count, sizeof(*plan->loads),
          load_segment_order);
    if (plan->load_count) {
        strict_load_segment *active = &plan->loads[0];
        for (size_t i = 1; i < plan->load_count; i++) {
            strict_load_segment *current = &plan->loads[i];
            if (current->virtual_address < active->end_address) {
                vita_loader_status overlap =
                    current->module_index == active->module_index
                    ? VITA_LOADER_ERROR_LOAD_RANGE_OVERLAP
                    : VITA_LOADER_ERROR_CROSS_MODULE_OVERLAP;
                status = loader_fail(
                    error, overlap, current->module_index,
                    current->program_header_index,
                    VITA_LOADER_INDEX_NONE);
                if (error) {
                    error->related_module_index = active->module_index;
                    error->related_program_header_index =
                        active->program_header_index;
                }
                goto fail;
            }
            if (current->end_address > active->end_address)
                active = current;
        }
    }

    for (size_t i = 0; i < module_count; i++) {
        status = compare_expected_metadata(
            &plan->module_plans[i], modules[i].expected, (uint32_t)i,
            error);
        if (status != VITA_LOADER_OK) goto fail;
        status = run_validation_extension(
            &modules[i], &plan->module_plans[i], (uint32_t)i, error);
        if (status != VITA_LOADER_OK) goto fail;
    }

    if (require_guest_memory) {
        uint32_t guest_base = vita_mem_base();
        size_t guest_size = vita_mem_size();
        uint64_t guest_end = (uint64_t)guest_base + guest_size;
        if (!guest_size || guest_end > UINT64_C(0x100000000)) {
            status = loader_fail(error, VITA_LOADER_ERROR_GUEST_RANGE,
                                 VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE,
                                 VITA_LOADER_INDEX_NONE);
            goto fail;
        }
        for (size_t i = 0; i < plan->load_count; i++) {
            strict_load_segment *load = &plan->loads[i];
            if (load->virtual_address < guest_base ||
                load->end_address > guest_end) {
                status = loader_fail(
                    error, VITA_LOADER_ERROR_GUEST_RANGE,
                    load->module_index, load->program_header_index,
                    VITA_LOADER_INDEX_NONE);
                goto fail;
            }
        }
    }

    return VITA_LOADER_OK;

fail:
    free_strict_plan(plan);
    return status;
}

static void copy_load_infos(const strict_load_plan *plan,
                            vita_load_info *infos) {
    if (!infos) return;
    for (size_t i = 0; i < plan->module_count; i++)
        infos[i] = plan->module_plans[i].info;
}

static vita_loader_status commit_strict_plan(
    strict_load_plan *plan,
    vita_loader_error *error) {
    vita_memory_lock();
    for (size_t i = 0; i < plan->load_count; i++) {
        strict_load_segment *load = &plan->loads[i];
        load->destination =
            vita_mem_ptr(load->virtual_address, load->memory_size);
        if (!load->destination) {
            vita_memory_unlock();
            return loader_fail(
                error, VITA_LOADER_ERROR_GUEST_RANGE,
                load->module_index, load->program_header_index,
                VITA_LOADER_INDEX_NONE);
        }
    }

    for (size_t i = 0; i < plan->load_count; i++) {
        strict_load_segment *load = &plan->loads[i];
        if (load->file_size)
            memmove(load->destination, load->source, load->file_size);
        if (load->memory_size > load->file_size)
            memset((uint8_t *)load->destination + load->file_size, 0,
                   load->memory_size - load->file_size);
    }
    vita_exclusive_note_write();
    vita_memory_unlock();
    return VITA_LOADER_OK;
}

vita_loader_status vita_preflight_module_set(
    const vita_loader_module *modules,
    size_t module_count,
    vita_load_info *infos,
    vita_loader_error *error) {
    strict_load_plan plan;
    vita_loader_status status =
        build_strict_plan(modules, module_count, 0, &plan, error);
    if (status != VITA_LOADER_OK) return status;
    copy_load_infos(&plan, infos);
    free_strict_plan(&plan);
    return VITA_LOADER_OK;
}

vita_loader_status vita_load_module_set(
    const vita_loader_module *modules,
    size_t module_count,
    vita_load_info *infos,
    vita_loader_error *error) {
    strict_load_plan plan;
    vita_loader_status status =
        build_strict_plan(modules, module_count, 1, &plan, error);
    if (status != VITA_LOADER_OK) return status;
    status = commit_strict_plan(&plan, error);
    if (status == VITA_LOADER_OK) copy_load_infos(&plan, infos);
    free_strict_plan(&plan);
    return status;
}

vita_loader_status vita_preflight_module(
    const vita_loader_module *module,
    vita_load_info *info,
    vita_loader_error *error) {
    return vita_preflight_module_set(module, 1, info, error);
}

vita_loader_status vita_load_module(
    const vita_loader_module *module,
    vita_load_info *info,
    vita_loader_error *error) {
    return vita_load_module_set(module, 1, info, error);
}

const char *vita_loader_status_string(vita_loader_status status) {
    switch (status) {
        case VITA_LOADER_OK: return "ok";
        case VITA_LOADER_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case VITA_LOADER_ERROR_NO_MEMORY: return "out of memory";
        case VITA_LOADER_ERROR_TRUNCATED: return "truncated ELF header";
        case VITA_LOADER_ERROR_BAD_MAGIC: return "bad ELF magic";
        case VITA_LOADER_ERROR_UNSUPPORTED_CLASS:
            return "ELF is not 32-bit";
        case VITA_LOADER_ERROR_UNSUPPORTED_ENDIAN:
            return "ELF is not little-endian";
        case VITA_LOADER_ERROR_UNSUPPORTED_VERSION:
            return "unsupported ELF version";
        case VITA_LOADER_ERROR_UNSUPPORTED_TYPE:
            return "unsupported ELF type";
        case VITA_LOADER_ERROR_UNSUPPORTED_MACHINE:
            return "ELF is not ARM";
        case VITA_LOADER_ERROR_HEADER_SIZE:
            return "ELF header size mismatch";
        case VITA_LOADER_ERROR_PROGRAM_HEADER_SIZE:
            return "program-header size mismatch";
        case VITA_LOADER_ERROR_PROGRAM_HEADER_TABLE:
            return "invalid program-header table";
        case VITA_LOADER_ERROR_SECTION_HEADER_TABLE:
            return "invalid section-header table";
        case VITA_LOADER_ERROR_FILE_RANGE_OVERFLOW:
            return "program-header file range overflows";
        case VITA_LOADER_ERROR_FILE_RANGE:
            return "program-header file range is outside the image";
        case VITA_LOADER_ERROR_FILE_SIZE:
            return "PT_LOAD file size exceeds memory size";
        case VITA_LOADER_ERROR_VIRTUAL_RANGE_OVERFLOW:
            return "PT_LOAD virtual range overflows";
        case VITA_LOADER_ERROR_ZERO_LOAD_RANGE:
            return "PT_LOAD memory range is empty";
        case VITA_LOADER_ERROR_LOAD_RANGE_OVERLAP:
            return "PT_LOAD ranges overlap";
        case VITA_LOADER_ERROR_CROSS_MODULE_OVERLAP:
            return "module load ranges overlap";
        case VITA_LOADER_ERROR_LOAD_FLAGS:
            return "unsafe or unsupported PT_LOAD flags";
        case VITA_LOADER_ERROR_ALIGNMENT:
            return "invalid program-header alignment";
        case VITA_LOADER_ERROR_UNSUPPORTED_LAYOUT:
            return "unsupported ELF load layout";
        case VITA_LOADER_ERROR_NO_LOAD_SEGMENTS:
            return "ELF has no PT_LOAD segments";
        case VITA_LOADER_ERROR_METADATA_MISMATCH:
            return "module metadata mismatch";
        case VITA_LOADER_ERROR_GUEST_RANGE:
            return "PT_LOAD range is outside guest memory";
        case VITA_LOADER_ERROR_VALIDATION_REQUIRED:
            return "module validation extension is required";
        case VITA_LOADER_ERROR_MODULE_INFO:
            return "module-info validation failed";
        case VITA_LOADER_ERROR_RELOCATION:
            return "relocation validation failed";
        case VITA_LOADER_ERROR_EXTENSION:
            return "module validation extension failed";
        default: return "unknown loader status";
    }
}
