/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITARECOMP_USSE_H
#define VITARECOMP_USSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vita_usse_opcode_family {
    VITA_USSE_OPCODE_UNKNOWN = 0,
    VITA_USSE_OPCODE_VMAD2,
    VITA_USSE_OPCODE_V32NMAD,
    VITA_USSE_OPCODE_V16NMAD,
    VITA_USSE_OPCODE_VMAD,
    VITA_USSE_OPCODE_VDP,
    VITA_USSE_OPCODE_VDUAL,
    VITA_USSE_OPCODE_VCOMP,
    VITA_USSE_OPCODE_VMOV,
    VITA_USSE_OPCODE_VPCK,
    VITA_USSE_OPCODE_VTST,
    VITA_USSE_OPCODE_VTSTMSK,
    VITA_USSE_OPCODE_VBW,
    VITA_USSE_OPCODE_SOP2,
    VITA_USSE_OPCODE_SOP2M,
    VITA_USSE_OPCODE_SOP3,
    VITA_USSE_OPCODE_I8MAD,
    VITA_USSE_OPCODE_I16MAD,
    VITA_USSE_OPCODE_I32MAD,
    VITA_USSE_OPCODE_ILLEGAL22,
    VITA_USSE_OPCODE_ILLEGAL23,
    VITA_USSE_OPCODE_ILLEGAL24,
    VITA_USSE_OPCODE_I8MAD2,
    VITA_USSE_OPCODE_I32MAD2,
    VITA_USSE_OPCODE_ILLEGAL27,
    VITA_USSE_OPCODE_SMP,
    VITA_USSE_OPCODE_PHAS,
    VITA_USSE_OPCODE_NOP,
    VITA_USSE_OPCODE_BR,
    VITA_USSE_OPCODE_SMLSI,
    VITA_USSE_OPCODE_SMBO,
    VITA_USSE_OPCODE_KILL,
    VITA_USSE_OPCODE_LIMM,
    VITA_USSE_OPCODE_DEPTHF,
    VITA_USSE_OPCODE_SPEC,
    VITA_USSE_OPCODE_VLDST,
    VITA_USSE_OPCODE_FAMILY_COUNT
} vita_usse_opcode_family;

typedef enum vita_usse_phase {
    VITA_USSE_PHASE_NONE = 0,
    VITA_USSE_PHASE_PRIMARY,
    VITA_USSE_PHASE_SECONDARY
} vita_usse_phase;

typedef enum vita_usse_census_result {
    VITA_USSE_CENSUS_OK = 0,
    VITA_USSE_CENSUS_INVALID_ARGUMENT,
    VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE
} vita_usse_census_result;

typedef struct vita_usse_stream_view {
    const uint8_t *bytes;
    size_t byte_count;
    uint32_t instruction_count;
} vita_usse_stream_view;

typedef struct vita_usse_census {
    uint64_t total;
    uint64_t classified;
    uint64_t unclassified;
    uint64_t illegal;
    uint64_t family_counts[VITA_USSE_OPCODE_FAMILY_COUNT];
    vita_usse_phase first_unclassified_phase;
    uint32_t first_unclassified_index;
    vita_usse_opcode_family first_unclassified_family;
    vita_usse_phase first_illegal_phase;
    uint32_t first_illegal_index;
    vita_usse_opcode_family first_illegal_family;
} vita_usse_census;

#define VITA_USSE_REGISTER_WORD_COUNT 128u
#define VITA_USSE_INTERNAL_REGISTER_COUNT 4u
#define VITA_USSE_HLSL_ABI_VERSION 1u

#define VITA_USSE_HLSL_STATE_PRIMARY_OFFSET 0u
#define VITA_USSE_HLSL_STATE_SECONDARY_OFFSET 128u
#define VITA_USSE_HLSL_STATE_TEMPORARY_OFFSET 256u
#define VITA_USSE_HLSL_STATE_OUTPUT_OFFSET 384u
#define VITA_USSE_HLSL_STATE_INTERNAL_OFFSET 512u
#define VITA_USSE_HLSL_STATE_WORD_COUNT 528u

typedef enum vita_usse_data_type {
    VITA_USSE_DATA_INVALID = 0,
    VITA_USSE_DATA_F32,
    VITA_USSE_DATA_F16,
    VITA_USSE_DATA_U8,
    VITA_USSE_DATA_S8,
    VITA_USSE_DATA_O8,
    VITA_USSE_DATA_U16,
    VITA_USSE_DATA_S16,
    VITA_USSE_DATA_C10
} vita_usse_data_type;

typedef enum vita_usse_register_bank {
    VITA_USSE_REGISTER_INVALID = 0,
    VITA_USSE_REGISTER_TEMPORARY,
    VITA_USSE_REGISTER_PRIMARY,
    VITA_USSE_REGISTER_OUTPUT,
    VITA_USSE_REGISTER_SECONDARY,
    VITA_USSE_REGISTER_INTERNAL,
    VITA_USSE_REGISTER_CONSTANT,
    VITA_USSE_REGISTER_IMMEDIATE,
    VITA_USSE_REGISTER_INDEX,
    VITA_USSE_REGISTER_INDEXED1,
    VITA_USSE_REGISTER_INDEXED2,
    VITA_USSE_REGISTER_GLOBAL
} vita_usse_register_bank;

typedef enum vita_usse_swizzle_channel {
    VITA_USSE_SWIZZLE_X = 0,
    VITA_USSE_SWIZZLE_Y,
    VITA_USSE_SWIZZLE_Z,
    VITA_USSE_SWIZZLE_W,
    VITA_USSE_SWIZZLE_ZERO,
    VITA_USSE_SWIZZLE_ONE,
    VITA_USSE_SWIZZLE_TWO,
    VITA_USSE_SWIZZLE_HALF,
    VITA_USSE_SWIZZLE_INVALID
} vita_usse_swizzle_channel;

typedef enum vita_usse_semantic_operation {
    VITA_USSE_SEMANTIC_INVALID = 0,
    VITA_USSE_SEMANTIC_FMA,
    VITA_USSE_SEMANTIC_MUL,
    VITA_USSE_SEMANTIC_ADD,
    VITA_USSE_SEMANTIC_FRACT,
    VITA_USSE_SEMANTIC_DDX,
    VITA_USSE_SEMANTIC_DDY,
    VITA_USSE_SEMANTIC_MIN,
    VITA_USSE_SEMANTIC_MAX,
    VITA_USSE_SEMANTIC_DOT,
    VITA_USSE_SEMANTIC_PACK,
    VITA_USSE_SEMANTIC_PHASE,
    VITA_USSE_SEMANTIC_NOP,
    VITA_USSE_SEMANTIC_EMIT
} vita_usse_semantic_operation;

typedef enum vita_usse_operand_flags {
    VITA_USSE_OPERAND_NONE = 0,
    VITA_USSE_OPERAND_ABSOLUTE = 1u << 0u,
    VITA_USSE_OPERAND_NEGATE = 1u << 1u
} vita_usse_operand_flags;

typedef struct vita_usse_operand {
    vita_usse_register_bank bank;
    vita_usse_data_type type;
    uint32_t index;
    vita_usse_swizzle_channel swizzle[4];
    uint32_t flags;
} vita_usse_operand;

typedef struct vita_usse_decoded_instruction {
    vita_usse_opcode_family family;
    vita_usse_phase phase;
    vita_usse_semantic_operation operation;
    uint8_t predicate;
    uint8_t write_mask;
    uint8_t repeat_mode;
    uint8_t repeat_count;
    uint8_t source_count;
    uint8_t source_select[4];
    vita_usse_operand destination;
    vita_usse_operand sources[3];
    struct {
        uint8_t immediate;
        uint8_t wait_condition;
        uint8_t rate_high;
        uint8_t rate_low_or_no_schedule;
        uint8_t temporary_count;
    } phase_control;
    struct {
        uint8_t end;
        uint8_t target;
        uint8_t task_end;
        uint8_t free_partition;
    } emit_control;
} vita_usse_decoded_instruction;

typedef enum vita_usse_decode_result {
    VITA_USSE_DECODE_OK = 0,
    VITA_USSE_DECODE_INVALID_ARGUMENT,
    VITA_USSE_DECODE_UNKNOWN_FAMILY,
    VITA_USSE_DECODE_ILLEGAL_FAMILY,
    VITA_USSE_DECODE_UNSUPPORTED_FAMILY,
    VITA_USSE_DECODE_RESERVED_ENCODING,
    VITA_USSE_DECODE_INVALID_REGISTER,
    VITA_USSE_DECODE_INVALID_SWIZZLE,
    VITA_USSE_DECODE_UNSUPPORTED_FORMAT,
    VITA_USSE_DECODE_UNSUPPORTED_CONTROL
} vita_usse_decode_result;

typedef struct vita_usse_decode_failure {
    vita_usse_decode_result result;
    vita_usse_phase phase;
    uint32_t instruction_index;
    vita_usse_opcode_family family;
    const char *field;
} vita_usse_decode_failure;

typedef enum vita_usse_hlsl_mode {
    VITA_USSE_HLSL_VERTEX = 1,
    VITA_USSE_HLSL_COMPUTE_CONFORMANCE
} vita_usse_hlsl_mode;

typedef struct vita_usse_hlsl_options {
    vita_usse_hlsl_mode mode;
    uint32_t abi_version;
    uint64_t feature_mask;
} vita_usse_hlsl_options;

typedef enum vita_usse_translate_result {
    VITA_USSE_TRANSLATE_OK = 0,
    VITA_USSE_TRANSLATE_INVALID_ARGUMENT,
    VITA_USSE_TRANSLATE_STREAM_OUT_OF_RANGE,
    VITA_USSE_TRANSLATE_DECODE_ERROR,
    VITA_USSE_TRANSLATE_UNSUPPORTED_SEMANTIC,
    VITA_USSE_TRANSLATE_INVALID_PROGRAM_ORDER,
    VITA_USSE_TRANSLATE_OUTPUT_TOO_SMALL,
    VITA_USSE_TRANSLATE_INTERNAL_ERROR
} vita_usse_translate_result;

typedef struct vita_usse_translate_failure {
    vita_usse_translate_result result;
    vita_usse_decode_failure decode;
    const char *field;
} vita_usse_translate_failure;

/*
 * Census order is primary instruction order followed by secondary instruction
 * order. Top-level family classification is not semantic translation. The
 * result records classifications and positions, never instruction words or
 * decoded operands.
 */
vita_usse_census_result
vita_usse_census_program(const vita_usse_stream_view *primary,
                         const vita_usse_stream_view *secondary,
                         vita_usse_census *census);

/* Classify one normalized numeric instruction word. Serialized instruction
 * bytes must be normalized by the caller before using this API. */
vita_usse_opcode_family vita_usse_classify_word(uint64_t instruction);

vita_usse_decode_result
vita_usse_decode_word(uint64_t instruction, vita_usse_phase phase,
                      vita_usse_decoded_instruction *decoded,
                      vita_usse_decode_failure *failure);

/*
 * The returned required size includes the terminating NUL. Passing a NULL
 * output with zero capacity measures the source without writing it. On every
 * failure, output is left unchanged.
 */
vita_usse_translate_result
vita_usse_translate_hlsl(const vita_usse_stream_view *primary,
                         const vita_usse_stream_view *secondary,
                         const vita_usse_hlsl_options *options, char *output,
                         size_t output_capacity, size_t *required_size,
                         vita_usse_translate_failure *failure);

const char *vita_usse_opcode_family_name(vita_usse_opcode_family family);
const char *vita_usse_phase_name(vita_usse_phase phase);
const char *
vita_usse_semantic_operation_name(vita_usse_semantic_operation operation);
const char *vita_usse_register_bank_name(vita_usse_register_bank bank);
const char *vita_usse_decode_result_name(vita_usse_decode_result result);
const char *vita_usse_translate_result_name(vita_usse_translate_result result);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_USSE_H */
