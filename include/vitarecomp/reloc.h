/* reloc.h -- shared PT_SCE_RELA decoding and identity validation. */

#ifndef VITARECOMP_RELOC_H
#define VITARECOMP_RELOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_RELOC_PT_LOAD 0x00000001u
#define VITA_RELOC_PT_SCE_RELA 0x60000000u
#define VITA_RELOC_SEGMENT_ABSOLUTE 0x0Fu
#define VITA_RELOC_INDEX_NONE UINT32_MAX

typedef enum vita_reloc_code {
    VITA_RELOC_NONE = 0,
    VITA_RELOC_ABS32 = 2,
    VITA_RELOC_REL32 = 3,
    VITA_RELOC_ABS8 = 8,
    VITA_RELOC_THM_CALL = 10,
    VITA_RELOC_CALL = 28,
    VITA_RELOC_JUMP24 = 29,
    VITA_RELOC_THM_JUMP24 = 30,
    VITA_RELOC_TARGET1 = 38,
    VITA_RELOC_V4BX = 40,
    VITA_RELOC_TARGET2 = 41,
    VITA_RELOC_PREL31 = 42,
    VITA_RELOC_MOVW_ABS_NC = 43,
    VITA_RELOC_MOVT_ABS = 44,
    VITA_RELOC_THM_MOVW_ABS_NC = 47,
    VITA_RELOC_THM_MOVT_ABS = 48,
    VITA_RELOC_THM_JUMP11 = 102,
    VITA_RELOC_THM_JUMP8 = 103,
    VITA_RELOC_RBASE = 255
} vita_reloc_code;

typedef struct vita_reloc_program_header {
    uint32_t type;
    uint32_t file_offset;
    uint32_t virtual_address;
    uint32_t file_size;
    uint32_t memory_size;
} vita_reloc_program_header;

/* Program headers are in their original ELF order because relocation segment
 * selectors are program-header indexes, not indexes into the PT_LOAD subset. */
typedef struct vita_reloc_module {
    const vita_reloc_program_header *program_headers;
    uint32_t program_header_count;
} vita_reloc_module;

typedef struct vita_reloc_entry {
    uint32_t patch;
    uint32_t value;
    uint8_t format;
    uint8_t code;
    uint8_t patch_seg;
    uint8_t sym_seg;
} vita_reloc_entry;

typedef struct vita_reloc_table {
    vita_reloc_entry *v;
    uint32_t n;
    uint32_t cap;
    uint32_t streams;
    uint32_t stream_bytes;
    uint32_t by_format[10];
    uint32_t by_code[256];
    uint32_t bad_segment;
    uint32_t rebase_unmatched;
    uint32_t trailing;
} vita_reloc_table;

typedef struct vita_var_reloc_entry {
    uint32_t offset;
    uint32_t addend;
    uint8_t format;
    uint8_t code;
    uint8_t patch_seg;
} vita_var_reloc_entry;

typedef struct vita_var_reloc_table {
    vita_var_reloc_entry *v;
    uint32_t n;
    uint32_t declared_bytes;
    uint32_t by_format[3];
} vita_var_reloc_table;

typedef enum vita_reloc_status {
    VITA_RELOC_OK = 0,
    VITA_RELOC_ERROR_INVALID_ARGUMENT = -1,
    VITA_RELOC_ERROR_NO_MEMORY = -2,
    VITA_RELOC_ERROR_ELF_HEADER = -3,
    VITA_RELOC_ERROR_PROGRAM_HEADER_TABLE = -4,
    VITA_RELOC_ERROR_FILE_RANGE = -5,
    VITA_RELOC_ERROR_VIRTUAL_RANGE = -6,
    VITA_RELOC_ERROR_UNKNOWN_FORMAT = -7,
    VITA_RELOC_ERROR_TRUNCATED_RECORD = -8,
    VITA_RELOC_ERROR_MISSING_STATE = -9,
    VITA_RELOC_ERROR_ADDRESS_OVERFLOW = -10,
    VITA_RELOC_ERROR_BAD_SEGMENT = -11,
    VITA_RELOC_ERROR_REBASE_TARGET = -12,
    VITA_RELOC_ERROR_UNSUPPORTED_CODE = -13,
    VITA_RELOC_ERROR_PATCH_OUTSIDE_FILE = -14,
    VITA_RELOC_ERROR_NOT_IDENTITY = -15
} vita_reloc_status;

typedef struct vita_reloc_error {
    vita_reloc_status status;
    uint32_t program_header_index;
    uint32_t record_index;
    uint32_t stream_offset;
    char message[192];
} vita_reloc_error;

typedef struct vita_reloc_identity {
    uint32_t changed;
    uint32_t preserved;
    uint32_t unsupported;
    uint32_t outside;
    uint32_t first_changed;
    uint32_t first_preserved;
    uint32_t first_unsupported;
    uint32_t first_outside;
} vita_reloc_identity;

/* Decode one variable-import relocation block. Format 1 carries a 16-bit
 * addend in its attribute word; format 2 carries a full trailing word. */
vita_reloc_status vita_var_reloc_parse_block(
    const uint8_t *bytes,
    size_t size,
    vita_var_reloc_table *out,
    vita_reloc_error *error);

void vita_var_reloc_free(vita_var_reloc_table *table);

/* Decode all PT_SCE_RELA streams. Partial entries remain available after an
 * error for diagnostic tools, but a strict runtime must accept only OK. */
vita_reloc_status vita_reloc_parse(
    const uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    vita_reloc_table *out,
    vita_reloc_error *error);

/* Extract program headers from a plain ELF32 image and use the same decoder. */
vita_reloc_status vita_reloc_parse_elf(
    const uint8_t *bytes,
    size_t size,
    vita_reloc_table *out,
    vita_reloc_error *error);

/* Strict runtime contract: decode every record and prove that applying every
 * supported relocation at the identity load addresses would change nothing.
 * The source bytes are never modified. */
vita_reloc_status vita_reloc_validate_elf_identity(
    const uint8_t *bytes,
    size_t size,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error);

/* Variable-import relocations legitimately replace identity-relocated bytes
 * during post-link binding. The exception list must be strictly increasing;
 * matching records are structurally validated but neither compared nor
 * materialized. */
vita_reloc_status vita_reloc_validate_elf_identity_except(
    const uint8_t *bytes,
    size_t size,
    const uint32_t *post_link_patches,
    uint32_t post_link_patch_count,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error);

/* Materialize the identity-load result in a writable ELF32 image. Validation
 * completes before bytes are changed. On success, identity reports the state
 * before normalization, and a second call reports zero changed records. */
vita_reloc_status vita_reloc_normalize_elf_identity(
    uint8_t *bytes,
    size_t size,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error);

vita_reloc_status vita_reloc_normalize_elf_identity_except(
    uint8_t *bytes,
    size_t size,
    const uint32_t *post_link_patches,
    uint32_t post_link_patch_count,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error);

/* Compare the declared post-link bytes with the identity-load result without
 * modifying them. Strict runtime preflight rejects every non-zero counter. */
vita_reloc_status vita_reloc_verify_identity(
    const uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    const vita_reloc_table *table,
    vita_reloc_identity *result);

/* Translation tools may deliberately materialize the identity-load result. */
vita_reloc_status vita_reloc_apply(
    uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    const vita_reloc_table *table,
    vita_reloc_identity *result);

void vita_reloc_free(vita_reloc_table *table);
const char *vita_reloc_code_name(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_RELOC_H */
