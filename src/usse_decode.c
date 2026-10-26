/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <stdint.h>
#include <string.h>

static uint32_t bits(uint64_t word, uint32_t offset, uint32_t width) {
    return (uint32_t)((word >> offset) &
                      ((UINT64_C(1) << width) - UINT64_C(1)));
}

static void identity_swizzle(vita_usse_operand *operand) {
    operand->swizzle[0] = VITA_USSE_SWIZZLE_X;
    operand->swizzle[1] = VITA_USSE_SWIZZLE_Y;
    operand->swizzle[2] = VITA_USSE_SWIZZLE_Z;
    operand->swizzle[3] = VITA_USSE_SWIZZLE_W;
}

static vita_usse_decode_result fail_decode(vita_usse_decode_failure *failure,
                                           vita_usse_decode_result result,
                                           vita_usse_phase phase,
                                           vita_usse_opcode_family family,
                                           const char *field) {
    if (failure) {
        failure->result = result;
        failure->phase = phase;
        failure->instruction_index = UINT32_MAX;
        failure->family = family;
        failure->field = field;
    }
    return result;
}

static vita_usse_register_bank decode_destination_bank(uint32_t selector,
                                                       uint32_t extended) {
    static const vita_usse_register_bank standard[4] = {
        VITA_USSE_REGISTER_TEMPORARY,
        VITA_USSE_REGISTER_OUTPUT,
        VITA_USSE_REGISTER_PRIMARY,
        VITA_USSE_REGISTER_INDEXED1,
    };
    static const vita_usse_register_bank extended_banks[4] = {
        VITA_USSE_REGISTER_SECONDARY,
        VITA_USSE_REGISTER_CONSTANT,
        VITA_USSE_REGISTER_INDEX,
        VITA_USSE_REGISTER_INDEXED2,
    };
    return extended ? extended_banks[selector] : standard[selector];
}

static vita_usse_register_bank decode_source_bank(uint32_t selector,
                                                  uint32_t extended) {
    static const vita_usse_register_bank standard[4] = {
        VITA_USSE_REGISTER_TEMPORARY,
        VITA_USSE_REGISTER_OUTPUT,
        VITA_USSE_REGISTER_PRIMARY,
        VITA_USSE_REGISTER_SECONDARY,
    };
    static const vita_usse_register_bank extended_banks[4] = {
        VITA_USSE_REGISTER_INDEXED1,
        VITA_USSE_REGISTER_CONSTANT,
        VITA_USSE_REGISTER_IMMEDIATE,
        VITA_USSE_REGISTER_INDEXED2,
    };
    return extended ? extended_banks[selector] : standard[selector];
}

static int bank_is_supported(vita_usse_register_bank bank) {
    return bank == VITA_USSE_REGISTER_TEMPORARY ||
           bank == VITA_USSE_REGISTER_PRIMARY ||
           bank == VITA_USSE_REGISTER_OUTPUT ||
           bank == VITA_USSE_REGISTER_SECONDARY ||
           bank == VITA_USSE_REGISTER_INTERNAL;
}

static vita_usse_decode_result
finalize_operand(vita_usse_operand *operand, uint32_t number,
                 int double_register, vita_usse_phase phase,
                 vita_usse_decode_failure *failure,
                 vita_usse_opcode_family family, const char *field) {
    if (double_register && operand->bank != VITA_USSE_REGISTER_CONSTANT &&
        operand->bank != VITA_USSE_REGISTER_IMMEDIATE)
        number = (number << 1u) & UINT32_C(0xFF);

    if (operand->bank == VITA_USSE_REGISTER_TEMPORARY) {
        const uint32_t internal_limit = double_register ? 120u : 124u;
        if (number >= internal_limit) {
            number -= internal_limit;
            if (double_register)
                number >>= 1u;
            operand->bank = VITA_USSE_REGISTER_INTERNAL;
        }
    } else if (operand->bank == VITA_USSE_REGISTER_CONSTANT) {
        if ((number & UINT32_C(0x40)) != 0u) {
            number &= ~UINT32_C(0x40);
            operand->bank = VITA_USSE_REGISTER_GLOBAL;
        }
    }

    if (phase == VITA_USSE_PHASE_SECONDARY &&
        operand->bank != VITA_USSE_REGISTER_INTERNAL &&
        operand->bank != VITA_USSE_REGISTER_CONSTANT &&
        operand->bank != VITA_USSE_REGISTER_IMMEDIATE)
        operand->bank = VITA_USSE_REGISTER_SECONDARY;

    operand->index = number;
    if (!bank_is_supported(operand->bank))
        return fail_decode(failure, VITA_USSE_DECODE_INVALID_REGISTER, phase,
                           family, field);
    if ((operand->bank == VITA_USSE_REGISTER_INTERNAL &&
         operand->index >= VITA_USSE_INTERNAL_REGISTER_COUNT) ||
        (operand->bank != VITA_USSE_REGISTER_INTERNAL &&
         operand->index >= VITA_USSE_REGISTER_WORD_COUNT))
        return fail_decode(failure, VITA_USSE_DECODE_INVALID_REGISTER, phase,
                           family, field);
    return VITA_USSE_DECODE_OK;
}

static vita_usse_decode_result
decode_destination(vita_usse_operand *operand, uint32_t number,
                   uint32_t selector, uint32_t extended, int double_register,
                   vita_usse_phase phase, vita_usse_decode_failure *failure,
                   vita_usse_opcode_family family) {
    operand->bank = decode_destination_bank(selector, extended);
    identity_swizzle(operand);
    return finalize_operand(operand, number, double_register, phase, failure,
                            family, "destination");
}

static vita_usse_decode_result
decode_source(vita_usse_operand *operand, uint32_t number, uint32_t selector,
              uint32_t extended, int double_register, vita_usse_phase phase,
              vita_usse_decode_failure *failure, vita_usse_opcode_family family,
              const char *field) {
    operand->bank = decode_source_bank(selector, extended);
    identity_swizzle(operand);
    return finalize_operand(operand, number, double_register, phase, failure,
                            family, field);
}

static int decode_vec4_swizzle(uint32_t encoded, uint32_t extended,
                               vita_usse_swizzle_channel output[4]) {
    static const vita_usse_swizzle_channel standard[16][4] = {
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_ONE},
    };
    static const vita_usse_swizzle_channel extended_table[16][4] = {
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_Y},
    };
    const vita_usse_swizzle_channel(*table)[4] =
        extended ? extended_table : standard;
    memcpy(output, table[encoded], sizeof(table[encoded]));
    return 1;
}

static int decode_vec3_swizzle(uint32_t encoded, uint32_t extended,
                               vita_usse_swizzle_channel output[4]) {
    static const vita_usse_swizzle_channel standard[16][4] = {
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_Y},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_Z},
        {VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_W, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_W},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_W,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Z, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_ONE,
         VITA_USSE_SWIZZLE_X},
    };
    static const vita_usse_swizzle_channel extended_table[11][4] = {
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Z,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_X,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_Y, VITA_USSE_SWIZZLE_ZERO,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_ONE, VITA_USSE_SWIZZLE_ZERO,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_ZERO, VITA_USSE_SWIZZLE_ZERO, VITA_USSE_SWIZZLE_ZERO,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_ONE, VITA_USSE_SWIZZLE_ONE, VITA_USSE_SWIZZLE_ONE,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_HALF, VITA_USSE_SWIZZLE_HALF, VITA_USSE_SWIZZLE_HALF,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_TWO, VITA_USSE_SWIZZLE_TWO, VITA_USSE_SWIZZLE_TWO,
         VITA_USSE_SWIZZLE_X},
        {VITA_USSE_SWIZZLE_X, VITA_USSE_SWIZZLE_ZERO, VITA_USSE_SWIZZLE_ZERO,
         VITA_USSE_SWIZZLE_X},
    };
    if (extended && encoded >= 11u)
        return 0;
    memcpy(output, extended ? extended_table[encoded] : standard[encoded],
           sizeof(standard[0]));
    return 1;
}

static vita_usse_decode_result
decode_phas(uint64_t word, vita_usse_phase phase,
            vita_usse_decoded_instruction *decoded,
            vita_usse_decode_failure *failure) {
    if (bits(word, 55u, 1u) != 0u || bits(word, 51u, 1u) != 0u ||
        bits(word, 50u, 1u) != 1u || bits(word, 48u, 2u) != 0u ||
        bits(word, 45u, 1u) != 0u || bits(word, 28u, 4u) != 0u ||
        bits(word, 14u, 6u) != 0u || bits(word, 7u, 7u) != 0u ||
        bits(word, 0u, 7u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_PHAS, "phase_control");

    decoded->operation = VITA_USSE_SEMANTIC_PHASE;
    decoded->phase_control.immediate = 1u;
    decoded->phase_control.wait_condition = (uint8_t)bits(word, 40u, 3u);
    decoded->phase_control.rate_high = (uint8_t)bits(word, 44u, 1u);
    decoded->phase_control.rate_low_or_no_schedule =
        (uint8_t)bits(word, 43u, 1u);
    decoded->phase_control.temporary_count = (uint8_t)bits(word, 32u, 8u);
    return VITA_USSE_DECODE_OK;
}

static vita_usse_decode_result
decode_nop(uint64_t word, vita_usse_phase phase,
           vita_usse_decoded_instruction *decoded,
           vita_usse_decode_failure *failure) {
    const uint64_t allowed = UINT64_C(0xF800014000000000) | UINT64_C(1) << 43u;
    if ((word & ~allowed) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_RESERVED_ENCODING, phase,
                           VITA_USSE_OPCODE_NOP, "reserved_bits");
    decoded->operation = VITA_USSE_SEMANTIC_NOP;
    return VITA_USSE_DECODE_OK;
}

static vita_usse_decode_result
decode_emit(uint64_t word, vita_usse_phase phase,
            vita_usse_decoded_instruction *decoded,
            vita_usse_decode_failure *failure) {
    if (bits(word, 56u, 3u) != 3u || bits(word, 54u, 2u) != 0u ||
        bits(word, 52u, 2u) != 2u || bits(word, 51u, 1u) != 0u ||
        bits(word, 50u, 1u) != 1u || bits(word, 49u, 1u) != 1u ||
        bits(word, 48u, 1u) != 1u || bits(word, 46u, 2u) != 1u ||
        bits(word, 45u, 1u) != 0u || bits(word, 44u, 1u) != 1u ||
        bits(word, 35u, 6u) != 0u || bits(word, 34u, 1u) != 0u ||
        bits(word, 32u, 2u) != 0u || bits(word, 30u, 2u) != 2u ||
        bits(word, 28u, 2u) != 2u || bits(word, 22u, 6u) != 0u ||
        bits(word, 21u, 1u) != 1u || bits(word, 14u, 7u) != 0u ||
        bits(word, 7u, 7u) != 0u || bits(word, 0u, 7u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_SPEC, "emit_control");

    decoded->operation = VITA_USSE_SEMANTIC_EMIT;
    decoded->emit_control.end = 1u;
    decoded->emit_control.target = 1u;
    decoded->emit_control.task_end = 1u;
    decoded->emit_control.free_partition = 1u;
    return VITA_USSE_DECODE_OK;
}

static vita_usse_decode_result
decode_vpck(uint64_t word, vita_usse_phase phase,
            vita_usse_decoded_instruction *decoded,
            vita_usse_decode_failure *failure) {
    vita_usse_decode_result result;
    const uint32_t source_format = bits(word, 41u, 3u);
    const uint32_t destination_format = bits(word, 38u, 3u);

    if (bits(word, 56u, 3u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_VPCK, "predicate");
    if (bits(word, 55u, 1u) != 1u || bits(word, 53u, 1u) != 0u ||
        bits(word, 52u, 1u) != 0u || bits(word, 50u, 1u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_VPCK, "control");
    if (bits(word, 44u, 4u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_VPCK, "repeat_count");
    if (source_format != 6u || destination_format != 6u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_FORMAT, phase,
                           VITA_USSE_OPCODE_VPCK, "format");
    if (bits(word, 18u, 1u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_FORMAT, phase,
                           VITA_USSE_OPCODE_VPCK, "scale");

    decoded->operation = VITA_USSE_SEMANTIC_PACK;
    decoded->repeat_count = 1u;
    decoded->write_mask = (uint8_t)bits(word, 34u, 4u);
    if (decoded->write_mask == 0u)
        return fail_decode(failure, VITA_USSE_DECODE_RESERVED_ENCODING, phase,
                           VITA_USSE_OPCODE_VPCK, "write_mask");
    decoded->destination.type = VITA_USSE_DATA_F32;
    result = decode_destination(&decoded->destination, bits(word, 21u, 7u),
                                bits(word, 32u, 2u), bits(word, 51u, 1u), 0,
                                phase, failure, VITA_USSE_OPCODE_VPCK);
    if (result != VITA_USSE_DECODE_OK)
        return result;

    decoded->source_count = 2u;
    decoded->sources[0].type = VITA_USSE_DATA_F32;
    result = decode_source(&decoded->sources[0], bits(word, 8u, 6u),
                           bits(word, 30u, 2u), bits(word, 49u, 1u), 1, phase,
                           failure, VITA_USSE_OPCODE_VPCK, "source1");
    if (result != VITA_USSE_DECODE_OK)
        return result;
    decoded->sources[1].type = VITA_USSE_DATA_F32;
    result = decode_source(&decoded->sources[1], bits(word, 1u, 6u),
                           bits(word, 28u, 2u), bits(word, 48u, 1u), 1, phase,
                           failure, VITA_USSE_OPCODE_VPCK, "source2");
    if (result != VITA_USSE_DECODE_OK)
        return result;

    decoded->source_select[0] =
        (uint8_t)(bits(word, 0u, 1u) | (bits(word, 7u, 1u) << 1u));
    decoded->source_select[1] = (uint8_t)bits(word, 16u, 2u);
    decoded->source_select[2] = (uint8_t)bits(word, 14u, 2u);
    decoded->source_select[3] = (uint8_t)bits(word, 19u, 2u);
    return VITA_USSE_DECODE_OK;
}

static vita_usse_decode_result
decode_v32nmad(uint64_t word, vita_usse_phase phase,
               vita_usse_decoded_instruction *decoded,
               vita_usse_decode_failure *failure) {
    static const vita_usse_semantic_operation operations[8] = {
        VITA_USSE_SEMANTIC_MUL,   VITA_USSE_SEMANTIC_ADD,
        VITA_USSE_SEMANTIC_FRACT, VITA_USSE_SEMANTIC_DDX,
        VITA_USSE_SEMANTIC_DDY,   VITA_USSE_SEMANTIC_MIN,
        VITA_USSE_SEMANTIC_MAX,   VITA_USSE_SEMANTIC_DOT,
    };
    vita_usse_decode_result result;
    uint32_t encoded_swizzle;

    if (bits(word, 56u, 3u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_V32NMAD, "predicate");
    if (bits(word, 55u, 1u) != 1u || bits(word, 52u, 1u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_V32NMAD, "control");

    decoded->operation = operations[bits(word, 12u, 3u)];
    decoded->repeat_count = 1u;
    decoded->write_mask = (uint8_t)bits(word, 39u, 4u);
    if (decoded->write_mask == 0u)
        return fail_decode(failure, VITA_USSE_DECODE_RESERVED_ENCODING, phase,
                           VITA_USSE_OPCODE_V32NMAD, "write_mask");
    if (decoded->operation == VITA_USSE_SEMANTIC_DOT &&
        (decoded->write_mask & (decoded->write_mask - 1u)) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_V32NMAD, "dot_write_mask");

    decoded->destination.type = VITA_USSE_DATA_F32;
    result = decode_destination(&decoded->destination, bits(word, 22u, 6u),
                                bits(word, 32u, 2u), bits(word, 51u, 1u), 1,
                                phase, failure, VITA_USSE_OPCODE_V32NMAD);
    if (result != VITA_USSE_DECODE_OK)
        return result;

    decoded->source_count =
        decoded->operation == VITA_USSE_SEMANTIC_FRACT ||
                decoded->operation == VITA_USSE_SEMANTIC_DDX ||
                decoded->operation == VITA_USSE_SEMANTIC_DDY
            ? 1u
            : 2u;
    decoded->sources[0].type = VITA_USSE_DATA_F32;
    result = decode_source(&decoded->sources[0], bits(word, 6u, 6u),
                           bits(word, 30u, 2u), bits(word, 49u, 1u), 1, phase,
                           failure, VITA_USSE_OPCODE_V32NMAD, "source1");
    if (result != VITA_USSE_DECODE_OK)
        return result;
    encoded_swizzle = bits(word, 15u, 7u) | (bits(word, 34u, 2u) << 7u) |
                      (bits(word, 50u, 1u) << 9u) |
                      (bits(word, 53u, 2u) << 10u);
    for (uint32_t lane = 0u; lane < 4u; lane++)
        decoded->sources[0].swizzle[lane] =
            (vita_usse_swizzle_channel)bits(encoded_swizzle, lane * 3u, 3u);
    switch (bits(word, 37u, 2u)) {
    case 1u:
        decoded->sources[0].flags = VITA_USSE_OPERAND_NEGATE;
        break;
    case 2u:
        decoded->sources[0].flags = VITA_USSE_OPERAND_ABSOLUTE;
        break;
    case 3u:
        decoded->sources[0].flags =
            VITA_USSE_OPERAND_ABSOLUTE | VITA_USSE_OPERAND_NEGATE;
        break;
    default:
        break;
    }

    decoded->sources[1].type = VITA_USSE_DATA_F32;
    result = decode_source(&decoded->sources[1], bits(word, 0u, 6u),
                           bits(word, 28u, 2u), bits(word, 48u, 1u), 1, phase,
                           failure, VITA_USSE_OPCODE_V32NMAD, "source2");
    if (result != VITA_USSE_DECODE_OK)
        return result;
    (void)decode_vec4_swizzle(bits(word, 44u, 4u), 0u,
                              decoded->sources[1].swizzle);
    if (bits(word, 36u, 1u) != 0u)
        decoded->sources[1].flags |= VITA_USSE_OPERAND_ABSOLUTE;
    return VITA_USSE_DECODE_OK;
}

static vita_usse_decode_result
decode_vmad(uint64_t word, vita_usse_phase phase,
            vita_usse_decoded_instruction *decoded,
            vita_usse_decode_failure *failure) {
    const uint32_t vector_size = bits(word, 52u, 1u) ? 4u : 3u;
    vita_usse_decode_result result;

    if (bits(word, 56u, 3u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_VMAD, "predicate");
    if (bits(word, 55u, 1u) != 1u || bits(word, 50u, 1u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_VMAD, "control");
    if (bits(word, 44u, 2u) != 0u)
        return fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_CONTROL, phase,
                           VITA_USSE_OPCODE_VMAD, "repeat_count");

    decoded->operation = VITA_USSE_SEMANTIC_FMA;
    decoded->repeat_mode = (uint8_t)bits(word, 47u, 2u);
    decoded->repeat_count = 1u;
    decoded->write_mask = (uint8_t)bits(word, 39u, 4u);
    if (decoded->write_mask == 0u ||
        (vector_size == 3u && (decoded->write_mask & 8u) != 0u))
        return fail_decode(failure, VITA_USSE_DECODE_RESERVED_ENCODING, phase,
                           VITA_USSE_OPCODE_VMAD, "write_mask");

    decoded->destination.type = VITA_USSE_DATA_F32;
    result = decode_destination(&decoded->destination, bits(word, 22u, 6u),
                                bits(word, 32u, 2u), bits(word, 51u, 1u), 1,
                                phase, failure, VITA_USSE_OPCODE_VMAD);
    if (result != VITA_USSE_DECODE_OK)
        return result;

    decoded->source_count = 3u;
    decoded->sources[0].bank = VITA_USSE_REGISTER_INTERNAL;
    decoded->sources[0].type = VITA_USSE_DATA_F32;
    decoded->sources[0].index = bits(word, 28u, 2u);
    if (!(vector_size == 4u
              ? decode_vec4_swizzle(bits(word, 18u, 4u), bits(word, 34u, 1u),
                                    decoded->sources[0].swizzle)
              : decode_vec3_swizzle(bits(word, 18u, 4u), bits(word, 34u, 1u),
                                    decoded->sources[0].swizzle)))
        return fail_decode(failure, VITA_USSE_DECODE_INVALID_SWIZZLE, phase,
                           VITA_USSE_OPCODE_VMAD, "source0_swizzle");
    if (bits(word, 46u, 1u) != 0u)
        decoded->sources[0].flags |= VITA_USSE_OPERAND_ABSOLUTE;
    if (bits(word, 11u, 1u) != 0u)
        decoded->sources[0].flags |= VITA_USSE_OPERAND_NEGATE;

    decoded->sources[1].type = VITA_USSE_DATA_F32;
    result = decode_source(&decoded->sources[1], bits(word, 0u, 6u),
                           bits(word, 30u, 2u), bits(word, 49u, 1u), 1, phase,
                           failure, VITA_USSE_OPCODE_VMAD, "source1");
    if (result != VITA_USSE_DECODE_OK)
        return result;
    if (!(vector_size == 4u
              ? decode_vec4_swizzle(bits(word, 6u, 4u), bits(word, 10u, 1u),
                                    decoded->sources[1].swizzle)
              : decode_vec3_swizzle(bits(word, 6u, 4u), bits(word, 10u, 1u),
                                    decoded->sources[1].swizzle)))
        return fail_decode(failure, VITA_USSE_DECODE_INVALID_SWIZZLE, phase,
                           VITA_USSE_OPCODE_VMAD, "source1_swizzle");
    if (bits(word, 37u, 1u) != 0u)
        decoded->sources[1].flags |= VITA_USSE_OPERAND_ABSOLUTE;
    if (bits(word, 38u, 1u) != 0u)
        decoded->sources[1].flags |= VITA_USSE_OPERAND_NEGATE;

    decoded->sources[2].bank = VITA_USSE_REGISTER_INTERNAL;
    decoded->sources[2].type = VITA_USSE_DATA_F32;
    decoded->sources[2].index = bits(word, 12u, 2u);
    if (!(vector_size == 4u
              ? decode_vec4_swizzle(bits(word, 14u, 4u), bits(word, 54u, 1u),
                                    decoded->sources[2].swizzle)
              : decode_vec3_swizzle(bits(word, 14u, 4u), bits(word, 54u, 1u),
                                    decoded->sources[2].swizzle)))
        return fail_decode(failure, VITA_USSE_DECODE_INVALID_SWIZZLE, phase,
                           VITA_USSE_OPCODE_VMAD, "source2_swizzle");
    if (bits(word, 35u, 1u) != 0u)
        decoded->sources[2].flags |= VITA_USSE_OPERAND_ABSOLUTE;
    if (bits(word, 36u, 1u) != 0u)
        decoded->sources[2].flags |= VITA_USSE_OPERAND_NEGATE;
    return VITA_USSE_DECODE_OK;
}

vita_usse_decode_result
vita_usse_decode_word(uint64_t instruction, vita_usse_phase phase,
                      vita_usse_decoded_instruction *decoded,
                      vita_usse_decode_failure *failure) {
    vita_usse_decoded_instruction parsed = {0};
    vita_usse_decode_result result;

    if (!decoded || (phase != VITA_USSE_PHASE_PRIMARY &&
                     phase != VITA_USSE_PHASE_SECONDARY))
        return fail_decode(failure, VITA_USSE_DECODE_INVALID_ARGUMENT, phase,
                           VITA_USSE_OPCODE_UNKNOWN, "argument");

    parsed.family = vita_usse_classify_word(instruction);
    parsed.phase = phase;
    parsed.repeat_count = 1u;
    switch (parsed.family) {
    case VITA_USSE_OPCODE_V32NMAD:
        result = decode_v32nmad(instruction, phase, &parsed, failure);
        break;
    case VITA_USSE_OPCODE_VMAD:
        result = decode_vmad(instruction, phase, &parsed, failure);
        break;
    case VITA_USSE_OPCODE_VPCK:
        result = decode_vpck(instruction, phase, &parsed, failure);
        break;
    case VITA_USSE_OPCODE_PHAS:
        result = decode_phas(instruction, phase, &parsed, failure);
        break;
    case VITA_USSE_OPCODE_NOP:
        result = decode_nop(instruction, phase, &parsed, failure);
        break;
    case VITA_USSE_OPCODE_SPEC:
        result = decode_emit(instruction, phase, &parsed, failure);
        break;
    case VITA_USSE_OPCODE_UNKNOWN:
        result = fail_decode(failure, VITA_USSE_DECODE_UNKNOWN_FAMILY, phase,
                             parsed.family, "family");
        break;
    case VITA_USSE_OPCODE_ILLEGAL22:
    case VITA_USSE_OPCODE_ILLEGAL23:
    case VITA_USSE_OPCODE_ILLEGAL24:
    case VITA_USSE_OPCODE_ILLEGAL27:
        result = fail_decode(failure, VITA_USSE_DECODE_ILLEGAL_FAMILY, phase,
                             parsed.family, "family");
        break;
    default:
        result = fail_decode(failure, VITA_USSE_DECODE_UNSUPPORTED_FAMILY,
                             phase, parsed.family, "family");
        break;
    }

    if (result != VITA_USSE_DECODE_OK)
        return result;
    if (failure) {
        failure->result = VITA_USSE_DECODE_OK;
        failure->phase = phase;
        failure->instruction_index = UINT32_MAX;
        failure->family = parsed.family;
        failure->field = NULL;
    }
    *decoded = parsed;
    return VITA_USSE_DECODE_OK;
}
