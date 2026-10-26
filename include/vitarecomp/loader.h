/* loader.h — putting the module's data where the recompiled code expects it.
 *
 * Recompiling the code is only half of it. The generated C reads its own
 * constants, tables and strings through vita_read32() at the ORIGINAL virtual
 * addresses, so those bytes have to be present in guest memory before anything
 * runs. Without this, every literal-pool address and every rodata lookup reads
 * zero — and a program that reads zero for its jump tables does not crash
 * usefully, it wanders.
 *
 * So the ELF is still needed at run time, and not as a formality: the
 * recompiler extracted the instructions from it, and the loader supplies
 * everything else that was in it.
 */

#ifndef VITARECOMP_LOADER_H
#define VITARECOMP_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t lowest;      /* lowest p_vaddr seen           */
    uint32_t highest;     /* highest p_vaddr + p_memsz     */
    int      segments;
} vita_load_info;

/* The path APIs are retained for compatibility. New hosts should prefer the
 * strict in-memory APIs below: the legacy parser predates structured errors
 * and transactional module-set loading. */

/* Load every PT_LOAD segment of an ELF32 into guest memory at its own vaddr.
 * `vita_mem_init` must already cover the range. Returns 0 on success. */
int vita_load_elf(const char *path, vita_load_info *info);

/* Determine the address range an ELF needs, without loading it. Lets a host
 * size its guest memory from the file rather than from a guess. */
int vita_probe_elf(const char *path, vita_load_info *info);

/* --- strict in-memory loader ------------------------------------------------
 *
 * A strict load owns no input bytes and never reopens a path. The caller must
 * keep every module buffer and expected-metadata record immutable for the
 * duration of the call. All modules, extensions, cross-module ranges, and
 * guest destinations are preflighted before any guest byte is changed.
 */

#define VITA_LOADER_INDEX_NONE UINT32_MAX

typedef enum vita_loader_status {
    VITA_LOADER_OK = 0,
    VITA_LOADER_ERROR_INVALID_ARGUMENT = -1,
    VITA_LOADER_ERROR_NO_MEMORY = -2,
    VITA_LOADER_ERROR_TRUNCATED = -3,
    VITA_LOADER_ERROR_BAD_MAGIC = -4,
    VITA_LOADER_ERROR_UNSUPPORTED_CLASS = -5,
    VITA_LOADER_ERROR_UNSUPPORTED_ENDIAN = -6,
    VITA_LOADER_ERROR_UNSUPPORTED_VERSION = -7,
    VITA_LOADER_ERROR_UNSUPPORTED_TYPE = -8,
    VITA_LOADER_ERROR_UNSUPPORTED_MACHINE = -9,
    VITA_LOADER_ERROR_HEADER_SIZE = -10,
    VITA_LOADER_ERROR_PROGRAM_HEADER_SIZE = -11,
    VITA_LOADER_ERROR_PROGRAM_HEADER_TABLE = -12,
    VITA_LOADER_ERROR_SECTION_HEADER_TABLE = -13,
    VITA_LOADER_ERROR_FILE_RANGE_OVERFLOW = -14,
    VITA_LOADER_ERROR_FILE_RANGE = -15,
    VITA_LOADER_ERROR_FILE_SIZE = -16,
    VITA_LOADER_ERROR_VIRTUAL_RANGE_OVERFLOW = -17,
    VITA_LOADER_ERROR_ZERO_LOAD_RANGE = -18,
    VITA_LOADER_ERROR_LOAD_RANGE_OVERLAP = -19,
    VITA_LOADER_ERROR_CROSS_MODULE_OVERLAP = -20,
    VITA_LOADER_ERROR_LOAD_FLAGS = -21,
    VITA_LOADER_ERROR_ALIGNMENT = -22,
    VITA_LOADER_ERROR_UNSUPPORTED_LAYOUT = -23,
    VITA_LOADER_ERROR_NO_LOAD_SEGMENTS = -24,
    VITA_LOADER_ERROR_METADATA_MISMATCH = -25,
    VITA_LOADER_ERROR_GUEST_RANGE = -26,
    VITA_LOADER_ERROR_VALIDATION_REQUIRED = -27,
    VITA_LOADER_ERROR_MODULE_INFO = -28,
    VITA_LOADER_ERROR_RELOCATION = -29,
    VITA_LOADER_ERROR_EXTENSION = -30
} vita_loader_status;

typedef enum vita_loader_validation {
    VITA_LOADER_VALIDATE_NONE = 0,
    VITA_LOADER_VALIDATE_MODULE_INFO = 1u << 0,
    VITA_LOADER_VALIDATE_RELOCATIONS = 1u << 1
} vita_loader_validation;

typedef struct vita_loader_error {
    vita_loader_status status;
    uint32_t module_index;
    uint32_t program_header_index;
    uint32_t record_index;
    uint32_t related_module_index;
    uint32_t related_program_header_index;
    uint32_t required_validation;
    uint32_t completed_validation;
} vita_loader_error;

typedef struct vita_loader_segment_metadata {
    uint32_t program_header_index;
    uint32_t file_offset;
    uint32_t virtual_address;
    uint32_t physical_address;
    uint32_t file_size;
    uint32_t memory_size;
    uint32_t flags;
    uint32_t alignment;
} vita_loader_segment_metadata;

/* ELF-level metadata, not .sce_module_info. `segments` contains PT_LOAD
 * records in program-header order. An expected record compares every field. */
typedef struct vita_loader_module_metadata {
    uint16_t elf_type;
    uint16_t machine;
    uint32_t entry;
    uint32_t elf_flags;
    uint16_t program_header_count;
    uint16_t reserved;
    uint32_t segment_count;
    const vita_loader_segment_metadata *segments;
} vita_loader_module_metadata;

/* Optional validation that is deliberately outside the generic loader.
 *
 * The callback runs after complete loader-level validation and before any
 * guest write. It must be side-effect free. It receives loader-validated ELF
 * metadata and reports which requested checks it completed. On failure it may
 * set program_header_index and record_index in `error`; the loader supplies
 * the module index. Returning OK without completing every requested bit is
 * reported as VITA_LOADER_ERROR_VALIDATION_REQUIRED.
 *
 * SCE executable types automatically request module-info validation, and
 * relocatable SCE types or PT_SCE_RELA records automatically request
 * relocation validation. The loader never applies or claims relocations.
 */
typedef vita_loader_status (*vita_loader_validation_fn)(
    const uint8_t *bytes,
    size_t size,
    const vita_loader_module_metadata *metadata,
    uint32_t requested_validation,
    uint32_t *completed_validation,
    vita_loader_error *error,
    void *userdata);

typedef struct vita_loader_module {
    const uint8_t *bytes;
    size_t size;
    const vita_loader_module_metadata *expected;
    uint32_t required_validation;
    vita_loader_validation_fn validate;
    void *validation_userdata;
} vita_loader_module;

/* Validate one module or a whole module set without modifying or requiring
 * guest memory. `infos`, when non-NULL, points to one output record per module
 * and is only changed on success, so it can be used to size guest memory. */
vita_loader_status vita_preflight_module(
    const vita_loader_module *module,
    vita_load_info *info,
    vita_loader_error *error);
vita_loader_status vita_preflight_module_set(
    const vita_loader_module *modules,
    size_t module_count,
    vita_load_info *infos,
    vita_loader_error *error);

/* Transactional strict loads. Validation, including the whole module set, is
 * completed first. Destinations are then rechecked under the memory lock and
 * all segments are copied and all BSS tails zeroed as one commit. */
vita_loader_status vita_load_module(
    const vita_loader_module *module,
    vita_load_info *info,
    vita_loader_error *error);
vita_loader_status vita_load_module_set(
    const vita_loader_module *modules,
    size_t module_count,
    vita_load_info *infos,
    vita_loader_error *error);

const char *vita_loader_status_string(vita_loader_status status);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_LOADER_H */
