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
    VITA_USSE_OPCODE_VMOV,
    VITA_USSE_OPCODE_PHAS,
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
    uint64_t decoded;
    uint64_t unsupported;
    uint64_t family_counts[VITA_USSE_OPCODE_FAMILY_COUNT];
    vita_usse_phase first_unsupported_phase;
    uint32_t first_unsupported_index;
    vita_usse_opcode_family first_unsupported_family;
} vita_usse_census;

/*
 * Census order is primary instruction order followed by secondary instruction
 * order. The result records classifications and positions, never instruction
 * words or decoded operands.
 */
vita_usse_census_result vita_usse_census_program(
    const vita_usse_stream_view *primary,
    const vita_usse_stream_view *secondary, vita_usse_census *census);

const char *vita_usse_opcode_family_name(vita_usse_opcode_family family);
const char *vita_usse_phase_name(vita_usse_phase phase);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_USSE_H */
