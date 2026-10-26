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

/*
 * Census order is primary instruction order followed by secondary instruction
 * order. Top-level family classification is not semantic translation. The
 * result records classifications and positions, never instruction words or
 * decoded operands.
 */
vita_usse_census_result vita_usse_census_program(
    const vita_usse_stream_view *primary,
    const vita_usse_stream_view *secondary, vita_usse_census *census);

/* Classify one normalized numeric instruction word. Serialized instruction
 * bytes must be normalized by the caller before using this API. */
vita_usse_opcode_family vita_usse_classify_word(uint64_t instruction);

const char *vita_usse_opcode_family_name(vita_usse_opcode_family family);
const char *vita_usse_phase_name(vita_usse_phase phase);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_USSE_H */
