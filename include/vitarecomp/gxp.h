/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITARECOMP_GXP_H
#define VITARECOMP_GXP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vita_gxp_stage {
    VITA_GXP_STAGE_VERTEX = 0,
    VITA_GXP_STAGE_FRAGMENT = 1,
    VITA_GXP_STAGE_ANY = 2
} vita_gxp_stage;

typedef enum vita_gxp_parse_result {
    VITA_GXP_PARSE_OK = 0,
    VITA_GXP_PARSE_INVALID_ARGUMENT,
    VITA_GXP_PARSE_TRUNCATED,
    VITA_GXP_PARSE_BAD_MAGIC,
    VITA_GXP_PARSE_BAD_DECLARED_SIZE,
    VITA_GXP_PARSE_WRONG_STAGE,
    VITA_GXP_PARSE_INSTRUCTION_COUNT_OVERFLOW,
    VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE
} vita_gxp_parse_result;

typedef struct vita_gxp_instruction_stream {
    const uint8_t *bytes;
    size_t byte_count;
    uint32_t instruction_count;
} vita_gxp_instruction_stream;

typedef struct vita_gxp_program_view {
    const uint8_t *bytes;
    size_t declared_size;
    uint32_t flags;
    vita_gxp_stage stage;
    vita_gxp_instruction_stream primary;
    vita_gxp_instruction_stream secondary;
} vita_gxp_program_view;

#define VITA_GXP_TRANSLATION_KEY_SIZE 32u
#define VITA_GXP_TRANSLATOR_ABI_VERSION 1u

typedef enum vita_gxp_register_initialization {
    VITA_GXP_REGISTER_INITIALIZATION_ZERO = 1
} vita_gxp_register_initialization;

typedef struct vita_gxp_translation_request {
    const vita_gxp_program_view *program;
    uint32_t translator_abi_version;
    uint32_t hlsl_abi_version;
    vita_gxp_register_initialization register_initialization;
    uint64_t feature_mask;
    const uint8_t *vertex_attribute_bytes;
    size_t vertex_attribute_byte_count;
    const uint8_t *vertex_stream_bytes;
    size_t vertex_stream_byte_count;
} vita_gxp_translation_request;

typedef struct vita_gxp_translation_key {
    uint8_t bytes[VITA_GXP_TRANSLATION_KEY_SIZE];
} vita_gxp_translation_key;

typedef enum vita_gxp_translation_key_result {
    VITA_GXP_TRANSLATION_KEY_OK = 0,
    VITA_GXP_TRANSLATION_KEY_INVALID_ARGUMENT,
    VITA_GXP_TRANSLATION_KEY_INVALID_PROGRAM_VIEW,
    VITA_GXP_TRANSLATION_KEY_INVALID_LAYOUT
} vita_gxp_translation_key_result;

/*
 * Parse one caller-owned byte span. The returned views borrow that span and
 * remain valid only while the caller keeps it alive and unchanged.
 */
vita_gxp_parse_result vita_gxp_parse(const uint8_t *bytes, size_t byte_count,
                                     vita_gxp_stage required_stage,
                                     vita_gxp_program_view *program);

/*
 * Builds the target-neutral translation identity. Backend target, compiler
 * profile, flags, and defines remain part of vita_gpu_shader_request_key and
 * are intentionally not duplicated here.
 */
vita_gxp_translation_key_result
vita_gxp_make_translation_key(const vita_gxp_translation_request *request,
                              vita_gxp_translation_key *key);

const char *
vita_gxp_translation_key_result_name(vita_gxp_translation_key_result result);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_GXP_H */
