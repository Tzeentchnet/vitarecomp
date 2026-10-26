/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <stdint.h>

/*
 * These family masks independently express two public SGX543 USSE bit-field
 * facts: VMOV has op1 00111 in bits 63:59, while PHAS has op1 11111 and
 * selector 100 in bits 54:52. No operand or semantic translation is claimed.
 * Factual references:
 *   https://wiki.henkaku.xyz/vita/SGX543
 *   https://github.com/VelocityRa/usse-decoder-gen/blob/3ce71c8f014dc7232a83991e6f8e616c268c568a/opcodes.yaml
 */
static uint64_t read_u64_le(const uint8_t *bytes) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8u; i++)
        value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

static vita_usse_opcode_family decode_family(const uint8_t *bytes) {
    const uint64_t instruction = read_u64_le(bytes);
    const uint32_t op1 = (uint32_t)(instruction >> 59u);
    if (op1 == UINT32_C(0x07))
        return VITA_USSE_OPCODE_VMOV;
    if (op1 == UINT32_C(0x1F) &&
        ((instruction >> 52u) & UINT64_C(0x07)) == UINT64_C(0x04))
        return VITA_USSE_OPCODE_PHAS;
    return VITA_USSE_OPCODE_UNKNOWN;
}

static vita_usse_census_result census_stream(
    const vita_usse_stream_view *stream, vita_usse_phase phase,
    vita_usse_census *census) {
    const uint64_t required_size =
        (uint64_t)stream->instruction_count * UINT64_C(8);
    if ((stream->instruction_count != 0u && !stream->bytes) ||
        required_size > SIZE_MAX || required_size > stream->byte_count)
        return VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE;

    for (uint32_t i = 0; i < stream->instruction_count; i++) {
        const vita_usse_opcode_family family =
            decode_family(stream->bytes + (size_t)i * 8u);
        census->total++;
        census->family_counts[family]++;
        if (family == VITA_USSE_OPCODE_UNKNOWN) {
            census->unsupported++;
            if (census->first_unsupported_phase == VITA_USSE_PHASE_NONE) {
                census->first_unsupported_phase = phase;
                census->first_unsupported_index = i;
                census->first_unsupported_family = family;
            }
        } else {
            census->decoded++;
        }
    }
    return VITA_USSE_CENSUS_OK;
}

vita_usse_census_result vita_usse_census_program(
    const vita_usse_stream_view *primary,
    const vita_usse_stream_view *secondary, vita_usse_census *census) {
    vita_usse_census measured = {0};
    vita_usse_census_result result;

    if (!primary || !secondary || !census)
        return VITA_USSE_CENSUS_INVALID_ARGUMENT;
    measured.first_unsupported_phase = VITA_USSE_PHASE_NONE;
    measured.first_unsupported_index = UINT32_MAX;
    measured.first_unsupported_family = VITA_USSE_OPCODE_UNKNOWN;

    result = census_stream(primary, VITA_USSE_PHASE_PRIMARY, &measured);
    if (result != VITA_USSE_CENSUS_OK)
        return result;
    result = census_stream(secondary, VITA_USSE_PHASE_SECONDARY, &measured);
    if (result != VITA_USSE_CENSUS_OK)
        return result;

    *census = measured;
    return VITA_USSE_CENSUS_OK;
}

const char *vita_usse_opcode_family_name(vita_usse_opcode_family family) {
    switch (family) {
    case VITA_USSE_OPCODE_UNKNOWN:
        return "UNKNOWN";
    case VITA_USSE_OPCODE_VMOV:
        return "VMOV";
    case VITA_USSE_OPCODE_PHAS:
        return "PHAS";
    case VITA_USSE_OPCODE_FAMILY_COUNT:
        break;
    }
    return "INVALID";
}

const char *vita_usse_phase_name(vita_usse_phase phase) {
    switch (phase) {
    case VITA_USSE_PHASE_NONE:
        return "NONE";
    case VITA_USSE_PHASE_PRIMARY:
        return "PRIMARY";
    case VITA_USSE_PHASE_SECONDARY:
        return "SECONDARY";
    }
    return "INVALID";
}
