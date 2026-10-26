/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

typedef struct family_case {
    uint64_t word;
    vita_usse_opcode_family family;
    const char *name;
} family_case;

static const family_case FAMILY_CASES[] = {
    {UINT64_C(0x0000000000000000), VITA_USSE_OPCODE_VMAD2, "VMAD2"},
    {UINT64_C(0x0800000000000000), VITA_USSE_OPCODE_V32NMAD, "V32NMAD"},
    {UINT64_C(0x1000000000000000), VITA_USSE_OPCODE_V16NMAD, "V16NMAD"},
    {UINT64_C(0x1820000000000000), VITA_USSE_OPCODE_VMAD, "VMAD"},
    {UINT64_C(0x1800000000000000), VITA_USSE_OPCODE_VDP, "VDP"},
    {UINT64_C(0x2000000000000000), VITA_USSE_OPCODE_VDUAL, "VDUAL"},
    {UINT64_C(0x3000000000000000), VITA_USSE_OPCODE_VCOMP, "VCOMP"},
    {UINT64_C(0x3800000000000000), VITA_USSE_OPCODE_VMOV, "VMOV"},
    {UINT64_C(0x4000000000000000), VITA_USSE_OPCODE_VPCK, "VPCK"},
    {UINT64_C(0x4800000000000000), VITA_USSE_OPCODE_VTST, "VTST"},
    {UINT64_C(0x7800000000000000), VITA_USSE_OPCODE_VTSTMSK, "VTSTMSK"},
    {UINT64_C(0x5000000000000000), VITA_USSE_OPCODE_VBW, "VBW"},
    {UINT64_C(0x8000000000000000), VITA_USSE_OPCODE_SOP2, "SOP2"},
    {UINT64_C(0x9000000000000000), VITA_USSE_OPCODE_SOP2M, "SOP2M"},
    {UINT64_C(0x8800000000000000), VITA_USSE_OPCODE_SOP3, "SOP3"},
    {UINT64_C(0x9800000000000000), VITA_USSE_OPCODE_I8MAD, "I8MAD"},
    {UINT64_C(0xA000000000000000), VITA_USSE_OPCODE_I16MAD, "I16MAD"},
    {UINT64_C(0xA800000000000000), VITA_USSE_OPCODE_I32MAD, "I32MAD"},
    {UINT64_C(0xB000000000000000), VITA_USSE_OPCODE_ILLEGAL22, "ILLEGAL22"},
    {UINT64_C(0xB800000000000000), VITA_USSE_OPCODE_ILLEGAL23, "ILLEGAL23"},
    {UINT64_C(0xC000000000000000), VITA_USSE_OPCODE_ILLEGAL24, "ILLEGAL24"},
    {UINT64_C(0xC800000000000000), VITA_USSE_OPCODE_I8MAD2, "I8MAD2"},
    {UINT64_C(0xD000000000000000), VITA_USSE_OPCODE_I32MAD2, "I32MAD2"},
    {UINT64_C(0xD800000000000000), VITA_USSE_OPCODE_ILLEGAL27, "ILLEGAL27"},
    {UINT64_C(0xE000000000000000), VITA_USSE_OPCODE_SMP, "SMP"},
    {UINT64_C(0xFA40000000000000), VITA_USSE_OPCODE_PHAS, "PHAS"},
    {UINT64_C(0xF800014000000000), VITA_USSE_OPCODE_NOP, "NOP"},
    {UINT64_C(0xF800000000000000), VITA_USSE_OPCODE_BR, "BR"},
    {UINT64_C(0xFA10000000000000), VITA_USSE_OPCODE_SMLSI, "SMLSI"},
    {UINT64_C(0xFB10000000000000), VITA_USSE_OPCODE_SMBO, "SMBO"},
    {UINT64_C(0xF9300006F0000000), VITA_USSE_OPCODE_KILL, "KILL"},
    {UINT64_C(0xFC20000000000000), VITA_USSE_OPCODE_LIMM, "LIMM"},
    {UINT64_C(0xFB30000000000000), VITA_USSE_OPCODE_DEPTHF, "DEPTHF"},
    {UINT64_C(0xFB20000000000000), VITA_USSE_OPCODE_SPEC, "SPEC"},
    {UINT64_C(0xE800000000000000), VITA_USSE_OPCODE_VLDST, "VLDST"},
};

_Static_assert(sizeof(FAMILY_CASES) / sizeof(FAMILY_CASES[0]) ==
                   (size_t)VITA_USSE_OPCODE_FAMILY_COUNT - 1u,
               "tests need one representative for every classified family");

static void write_u64_le(uint8_t *bytes, uint64_t value) {
    for (uint32_t i = 0; i < 8u; i++)
        bytes[i] = (uint8_t)(value >> (i * 8u));
}

static void assert_family(uint64_t word, vita_usse_opcode_family family) {
    assert(vita_usse_classify_word(word) == family);
    assert(vita_usse_classify_word(word) == family);
}

static void test_all_family_values_and_names(void) {
    for (size_t i = 0; i < sizeof(FAMILY_CASES) / sizeof(FAMILY_CASES[0]);
         i++) {
        assert_family(FAMILY_CASES[i].word, FAMILY_CASES[i].family);
        assert(strcmp(vita_usse_opcode_family_name(FAMILY_CASES[i].family),
                      FAMILY_CASES[i].name) == 0);
    }
    assert(strcmp(vita_usse_opcode_family_name(VITA_USSE_OPCODE_UNKNOWN),
                  "UNKNOWN") == 0);
    assert(strcmp(vita_usse_opcode_family_name(VITA_USSE_OPCODE_FAMILY_COUNT),
                  "INVALID") == 0);
}

static void test_broad_family_precedence(void) {
    assert_family(UINT64_C(0x4000000000000000), VITA_USSE_OPCODE_VPCK);
    assert_family(UINT64_C(0x4800000000000000), VITA_USSE_OPCODE_VTST);
    assert_family(UINT64_C(0x7800000000000000), VITA_USSE_OPCODE_VTSTMSK);
    assert_family(UINT64_C(0x5000000000000000), VITA_USSE_OPCODE_VBW);

    assert_family(UINT64_C(0xE000000000000000), VITA_USSE_OPCODE_SMP);
    assert_family(UINT64_C(0xFA40000000000000), VITA_USSE_OPCODE_PHAS);
    assert_family(UINT64_C(0xF800014000000000), VITA_USSE_OPCODE_NOP);
    assert_family(UINT64_C(0xF800000000000000), VITA_USSE_OPCODE_BR);
    assert_family(UINT64_C(0xFA10000000000000), VITA_USSE_OPCODE_SMLSI);
    assert_family(UINT64_C(0xFB10000000000000), VITA_USSE_OPCODE_SMBO);
    assert_family(UINT64_C(0xF9300006F0000000), VITA_USSE_OPCODE_KILL);
    assert_family(UINT64_C(0xFC20000000000000), VITA_USSE_OPCODE_LIMM);
    assert_family(UINT64_C(0xFB30000000000000), VITA_USSE_OPCODE_DEPTHF);
    /* The inactive legacy EMIT pattern deliberately falls through to SPEC. */
    assert_family(UINT64_C(0xFB20000000000000), VITA_USSE_OPCODE_SPEC);
    assert_family(UINT64_C(0xE800000000000000), VITA_USSE_OPCODE_VLDST);
}

static void test_i32_reserved_bit_holes(void) {
    static const uint64_t holes[] = {
        UINT64_C(0xA800000000000000) | UINT64_C(0x0000800000000000),
        UINT64_C(0xA800000000000000) | UINT64_C(0x0000010000000000),
        UINT64_C(0xA800000000000000) | UINT64_C(0x0000000800000000),
        UINT64_C(0xD000000000000000) | UINT64_C(0x0000040000000000),
        UINT64_C(0xD000000000000000) | UINT64_C(0x0000000800000000),
    };

    for (size_t i = 0; i < sizeof(holes) / sizeof(holes[0]); i++)
        assert_family(holes[i], VITA_USSE_OPCODE_UNKNOWN);
}

static void test_census_semantics_and_independent_firsts(void) {
    uint8_t primary_bytes[16];
    uint8_t secondary_bytes[16];
    const vita_usse_stream_view primary = {primary_bytes, sizeof(primary_bytes),
                                           2u};
    const vita_usse_stream_view secondary = {secondary_bytes,
                                             sizeof(secondary_bytes), 2u};
    vita_usse_census census;

    write_u64_le(primary_bytes,
                 UINT64_C(0xA800000000000000) | UINT64_C(0x0000800000000000));
    write_u64_le(primary_bytes + 8u, UINT64_C(0x3800000000000000));
    write_u64_le(secondary_bytes, UINT64_C(0xD800000000000000));
    write_u64_le(secondary_bytes + 8u, UINT64_C(0xFB20000000000000));

    assert(vita_usse_census_program(&primary, &secondary, &census) ==
           VITA_USSE_CENSUS_OK);
    assert(census.total == 4u);
    assert(census.classified == 3u);
    assert(census.unclassified == 1u);
    assert(census.illegal == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_UNKNOWN] == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_VMOV] == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_ILLEGAL27] == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_SPEC] == 1u);
    assert(census.first_unclassified_phase == VITA_USSE_PHASE_PRIMARY);
    assert(census.first_unclassified_index == 0u);
    assert(census.first_unclassified_family == VITA_USSE_OPCODE_UNKNOWN);
    assert(census.first_illegal_phase == VITA_USSE_PHASE_SECONDARY);
    assert(census.first_illegal_index == 0u);
    assert(census.first_illegal_family == VITA_USSE_OPCODE_ILLEGAL27);
}

static void test_all_legal(void) {
    uint8_t primary_bytes[8];
    const vita_usse_stream_view primary = {primary_bytes, sizeof(primary_bytes),
                                           1u};
    const vita_usse_stream_view empty = {NULL, 0u, 0u};
    vita_usse_census census;

    write_u64_le(primary_bytes, UINT64_C(0x3800000000000000));
    assert(vita_usse_census_program(&primary, &empty, &census) ==
           VITA_USSE_CENSUS_OK);
    assert(census.total == 1u);
    assert(census.classified == 1u);
    assert(census.unclassified == 0u);
    assert(census.illegal == 0u);
    assert(census.first_unclassified_phase == VITA_USSE_PHASE_NONE);
    assert(census.first_unclassified_index == UINT32_MAX);
    assert(census.first_illegal_phase == VITA_USSE_PHASE_NONE);
    assert(census.first_illegal_index == UINT32_MAX);
}

static void test_little_endian_census_ingestion(void) {
    const uint8_t primary_bytes[8] = {0u, 0u, 0u, 0u,
                                      0u, 0u, 0u, UINT8_C(0x38)};
    const uint8_t secondary_bytes[8] = {0u, 0u, 0u, 0u,
                                        0u, 0u, 0u, UINT8_C(0xB0)};
    const vita_usse_stream_view primary = {primary_bytes, sizeof(primary_bytes),
                                           1u};
    const vita_usse_stream_view secondary = {secondary_bytes,
                                             sizeof(secondary_bytes), 1u};
    vita_usse_census census;

    assert(vita_usse_census_program(&primary, &secondary, &census) ==
           VITA_USSE_CENSUS_OK);
    assert(census.total == 2u);
    assert(census.classified == 2u);
    assert(census.unclassified == 0u);
    assert(census.illegal == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_VMOV] == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_ILLEGAL22] == 1u);
}

static void test_bounded_failure_preserves_output(void) {
    uint8_t primary_bytes[8] = {0};
    uint8_t secondary_bytes[8] = {0};
    const vita_usse_stream_view truncated_primary = {primary_bytes, 7u, 1u};
    const vita_usse_stream_view valid_primary = {primary_bytes,
                                                 sizeof(primary_bytes), 1u};
    const vita_usse_stream_view truncated_secondary = {secondary_bytes, 7u, 1u};
    const vita_usse_stream_view empty = {NULL, 0u, 0u};
    vita_usse_census census;
    vita_usse_census original;

    memset(&census, 0xA5, sizeof(census));
    original = census;
    assert(vita_usse_census_program(&truncated_primary, &empty, &census) ==
           VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE);
    assert(memcmp(&census, &original, sizeof(census)) == 0);
    assert(vita_usse_census_program(&valid_primary, &truncated_secondary,
                                    &census) ==
           VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE);
    assert(memcmp(&census, &original, sizeof(census)) == 0);
    assert(vita_usse_census_program(NULL, &empty, &census) ==
           VITA_USSE_CENSUS_INVALID_ARGUMENT);
    assert(memcmp(&census, &original, sizeof(census)) == 0);
}

static void test_phase_names(void) {
    assert(strcmp(vita_usse_phase_name(VITA_USSE_PHASE_PRIMARY), "PRIMARY") ==
           0);
    assert(strcmp(vita_usse_phase_name(VITA_USSE_PHASE_SECONDARY),
                  "SECONDARY") == 0);
    assert(strcmp(vita_usse_phase_name(VITA_USSE_PHASE_NONE), "NONE") == 0);
    assert(strcmp(vita_usse_phase_name((vita_usse_phase)UINT32_MAX),
                  "INVALID") == 0);
}

static uint64_t field(uint64_t value, uint32_t offset, uint32_t width) {
    assert(width != 0u && width < 64u);
    assert(value < (UINT64_C(1) << width));
    return value << offset;
}

static uint64_t make_phase(uint32_t wait_condition, uint32_t temporary_count,
                           uint32_t rate_high, uint32_t rate_low) {
    return field(31u, 59u, 5u) | field(2u, 56u, 3u) | field(4u, 52u, 3u) |
           field(1u, 50u, 1u) | field(rate_high, 44u, 1u) |
           field(rate_low, 43u, 1u) | field(wait_condition, 40u, 3u) |
           field(temporary_count, 32u, 8u);
}

static uint64_t make_nop(uint32_t no_schedule) {
    return UINT64_C(0xF800014000000000) | field(no_schedule, 43u, 1u);
}

static uint64_t make_pack(void) {
    return field(8u, 59u, 5u) | field(1u, 55u, 1u) | field(6u, 41u, 3u) |
           field(6u, 38u, 3u) | field(15u, 34u, 4u) | field(2u, 30u, 2u) |
           field(2u, 28u, 2u) | field(124u, 21u, 7u) | field(3u, 19u, 2u) |
           field(1u, 16u, 2u) | field(2u, 14u, 2u) | field(2u, 8u, 6u) |
           field(3u, 1u, 6u);
}

static uint64_t make_v32_mul(uint32_t source1_modifier) {
    const uint32_t identity_swizzle = UINT32_C(0x688);
    return field(1u, 59u, 5u) | field(1u, 55u, 1u) |
           field((identity_swizzle >> 10u) & 3u, 53u, 2u) |
           field((identity_swizzle >> 9u) & 1u, 50u, 1u) | field(4u, 44u, 4u) |
           field(15u, 39u, 4u) | field(source1_modifier, 37u, 2u) |
           field((identity_swizzle >> 7u) & 3u, 34u, 2u) | field(3u, 30u, 2u) |
           field(61u, 22u, 6u) | field(identity_swizzle & 127u, 15u, 7u) |
           field(8u, 6u, 6u) | field(60u, 0u, 6u);
}

static uint64_t make_vmad(uint32_t vector4, uint32_t destination_number,
                          uint32_t destination_mask) {
    return field(3u, 59u, 5u) | field(1u, 55u, 1u) | field(1u, 53u, 1u) |
           field(vector4, 52u, 1u) | field(3u, 47u, 2u) |
           field(destination_mask, 39u, 4u) | field(1u, 38u, 1u) |
           field(1u, 37u, 1u) | field(3u, 30u, 2u) |
           field(destination_number, 22u, 6u) | field(1u, 18u, 4u) |
           field(4u, 14u, 4u) | field(1u, 12u, 2u) | field(4u, 6u, 4u) |
           field(10u, 0u, 6u);
}

static uint64_t make_emit(uint32_t no_schedule) {
    return field(31u, 59u, 5u) | field(3u, 56u, 3u) | field(2u, 52u, 2u) |
           field(1u, 50u, 1u) | field(1u, 49u, 1u) | field(1u, 48u, 1u) |
           field(1u, 46u, 2u) | field(1u, 44u, 1u) |
           field(no_schedule, 43u, 1u) | field(2u, 30u, 2u) |
           field(2u, 28u, 2u) | field(1u, 21u, 1u);
}

static size_t make_synthetic_program(uint8_t bytes[48]) {
    const uint64_t instructions[6] = {
        make_phase(3u, 9u, 1u, 0u),
        make_nop(0u),
        make_pack(),
        make_v32_mul(3u),
        make_vmad(1u, 62u, 13u),
        make_emit(1u),
    };
    for (size_t index = 0u; index < 6u; index++)
        write_u64_le(bytes + index * 8u, instructions[index]);
    return sizeof(instructions);
}

static void test_typed_decode_and_register_aliasing(void) {
    vita_usse_decoded_instruction decoded;
    vita_usse_decode_failure failure;

    assert(vita_usse_decode_word(make_pack(), VITA_USSE_PHASE_PRIMARY, &decoded,
                                 &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.operation == VITA_USSE_SEMANTIC_PACK);
    assert(decoded.destination.bank == VITA_USSE_REGISTER_INTERNAL);
    assert(decoded.destination.index == 0u);
    assert(decoded.sources[0].bank == VITA_USSE_REGISTER_PRIMARY);
    assert(decoded.sources[0].index == 4u);
    assert(decoded.sources[1].bank == VITA_USSE_REGISTER_PRIMARY);
    assert(decoded.sources[1].index == 6u);
    assert(decoded.source_select[0] == 0u);
    assert(decoded.source_select[1] == 1u);
    assert(decoded.source_select[2] == 2u);
    assert(decoded.source_select[3] == 3u);

    assert(vita_usse_decode_word(make_v32_mul(3u), VITA_USSE_PHASE_PRIMARY,
                                 &decoded, &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.operation == VITA_USSE_SEMANTIC_MUL);
    assert(decoded.destination.bank == VITA_USSE_REGISTER_INTERNAL);
    assert(decoded.destination.index == 1u);
    assert(decoded.sources[0].bank == VITA_USSE_REGISTER_SECONDARY);
    assert(decoded.sources[0].index == 16u);
    assert(decoded.sources[0].flags ==
           (VITA_USSE_OPERAND_ABSOLUTE | VITA_USSE_OPERAND_NEGATE));
    assert(decoded.sources[1].bank == VITA_USSE_REGISTER_INTERNAL);
    assert(decoded.sources[1].index == 0u);
    assert(decoded.sources[1].swizzle[0] == VITA_USSE_SWIZZLE_X);
    assert(decoded.sources[1].swizzle[3] == VITA_USSE_SWIZZLE_W);

    assert(vita_usse_decode_word(make_vmad(1u, 62u, 15u),
                                 VITA_USSE_PHASE_PRIMARY, &decoded,
                                 &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.operation == VITA_USSE_SEMANTIC_FMA);
    assert(decoded.destination.bank == VITA_USSE_REGISTER_INTERNAL);
    assert(decoded.destination.index == 2u);
    assert(decoded.sources[0].bank == VITA_USSE_REGISTER_INTERNAL);
    assert(decoded.sources[0].swizzle[0] == VITA_USSE_SWIZZLE_Y);
    assert(decoded.sources[1].bank == VITA_USSE_REGISTER_SECONDARY);
    assert(decoded.sources[1].index == 20u);
    assert(decoded.sources[2].bank == VITA_USSE_REGISTER_INTERNAL);
    assert(decoded.sources[2].index == 1u);

    assert(vita_usse_decode_word(make_vmad(0u, 62u, 7u),
                                 VITA_USSE_PHASE_PRIMARY, &decoded,
                                 &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.write_mask == 7u);
    assert(decoded.sources[0].swizzle[0] == VITA_USSE_SWIZZLE_Y);
}

static void test_strict_control_decode(void) {
    vita_usse_decoded_instruction decoded;
    vita_usse_decode_failure failure;
    uint64_t word = make_phase(3u, 9u, 1u, 0u);

    assert(vita_usse_decode_word(word, VITA_USSE_PHASE_PRIMARY, &decoded,
                                 &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.operation == VITA_USSE_SEMANTIC_PHASE);
    assert(decoded.phase_control.wait_condition == 3u);
    assert(decoded.phase_control.temporary_count == 9u);
    assert(decoded.phase_control.rate_high == 1u);
    assert(vita_usse_decode_word(word | field(1u, 45u, 1u),
                                 VITA_USSE_PHASE_PRIMARY, &decoded, &failure) ==
           VITA_USSE_DECODE_UNSUPPORTED_CONTROL);
    assert(strcmp(failure.field, "phase_control") == 0);

    assert(vita_usse_decode_word(make_nop(0u), VITA_USSE_PHASE_PRIMARY,
                                 &decoded, &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.operation == VITA_USSE_SEMANTIC_NOP);
    assert(vita_usse_decode_word(make_nop(0u) | 1u, VITA_USSE_PHASE_PRIMARY,
                                 &decoded, &failure) ==
           VITA_USSE_DECODE_RESERVED_ENCODING);
    assert(strcmp(failure.field, "reserved_bits") == 0);

    assert(vita_usse_decode_word(make_emit(1u), VITA_USSE_PHASE_PRIMARY,
                                 &decoded, &failure) == VITA_USSE_DECODE_OK);
    assert(decoded.operation == VITA_USSE_SEMANTIC_EMIT);
    assert(decoded.emit_control.target == 1u);
    word = make_emit(1u);
    word &= ~(UINT64_C(3) << 46u);
    word |= field(2u, 46u, 2u);
    assert(vita_usse_decode_word(word, VITA_USSE_PHASE_PRIMARY, &decoded,
                                 &failure) ==
           VITA_USSE_DECODE_UNSUPPORTED_CONTROL);
    assert(strcmp(failure.field, "emit_control") == 0);

    assert(vita_usse_decode_word(UINT64_C(0xFB20000000000000),
                                 VITA_USSE_PHASE_PRIMARY, &decoded, &failure) ==
           VITA_USSE_DECODE_UNSUPPORTED_CONTROL);
    assert(vita_usse_decode_word(UINT64_C(0x3800000000000000),
                                 VITA_USSE_PHASE_PRIMARY, &decoded, &failure) ==
           VITA_USSE_DECODE_UNSUPPORTED_FAMILY);
    assert(vita_usse_decode_word(UINT64_C(0xB000000000000000),
                                 VITA_USSE_PHASE_PRIMARY, &decoded,
                                 &failure) == VITA_USSE_DECODE_ILLEGAL_FAMILY);
}

static char *translate_synthetic(vita_usse_hlsl_mode mode,
                                 size_t *source_size) {
    uint8_t bytes[48];
    const size_t byte_count = make_synthetic_program(bytes);
    const vita_usse_stream_view primary = {bytes, byte_count,
                                           (uint32_t)(byte_count / 8u)};
    const vita_usse_stream_view empty = {NULL, 0u, 0u};
    const vita_usse_hlsl_options options = {mode, VITA_USSE_HLSL_ABI_VERSION,
                                            0u};
    vita_usse_translate_failure failure;
    size_t required = 0u;
    char *source;

    assert(vita_usse_translate_hlsl(&primary, &empty, &options, NULL, 0u,
                                    &required,
                                    &failure) == VITA_USSE_TRANSLATE_OK);
    assert(required > 1u);
    source = (char *)malloc(required);
    assert(source != NULL);
    assert(vita_usse_translate_hlsl(&primary, &empty, &options, source,
                                    required, &required,
                                    &failure) == VITA_USSE_TRANSLATE_OK);
    assert(strlen(source) + 1u == required);
    *source_size = required;
    return source;
}

static void test_deterministic_hlsl_generation(void) {
    size_t compute_size;
    size_t second_size;
    size_t vertex_size;
    char *compute =
        translate_synthetic(VITA_USSE_HLSL_COMPUTE_CONFORMANCE, &compute_size);
    char *second =
        translate_synthetic(VITA_USSE_HLSL_COMPUTE_CONFORMANCE, &second_size);
    char *vertex = translate_synthetic(VITA_USSE_HLSL_VERTEX, &vertex_size);

    assert(compute_size == second_size);
    assert(memcmp(compute, second, compute_size) == 0);
    assert(strstr(compute, "vita_temporary[vita_i] = 0.0f;") != NULL);
    assert(strstr(compute, "vita_output[vita_i] = 0.0f;") != NULL);
    assert(strstr(compute, "vita_internal[vita_i] = 0.0f;") != NULL);
    assert(strstr(compute, "RWStructuredBuffer<float> vita_state") != NULL);
    assert(strstr(compute, "mad(") != NULL);
    assert(strstr(compute, "// EMIT target=1") != NULL);
    assert(strstr(vertex, "VitaVertexOutput vita_main") != NULL);
    assert(strstr(vertex, "result.position") != NULL);
    assert(vertex_size != compute_size);

    free(vertex);
    free(second);
    free(compute);
}

static void test_translation_failures_preserve_output(void) {
    uint8_t bytes[48];
    uint8_t secondary_bytes[8];
    const size_t byte_count = make_synthetic_program(bytes);
    vita_usse_stream_view primary = {bytes, byte_count,
                                     (uint32_t)(byte_count / 8u)};
    vita_usse_stream_view secondary = {NULL, 0u, 0u};
    const vita_usse_hlsl_options options = {VITA_USSE_HLSL_COMPUTE_CONFORMANCE,
                                            VITA_USSE_HLSL_ABI_VERSION, 0u};
    vita_usse_translate_failure failure;
    char output[32];
    char original[32];
    size_t required = 0u;

    memset(output, 0xA5, sizeof(output));
    memcpy(original, output, sizeof(output));
    assert(vita_usse_translate_hlsl(&primary, &secondary, &options, output,
                                    sizeof(output), &required, &failure) ==
           VITA_USSE_TRANSLATE_OUTPUT_TOO_SMALL);
    assert(memcmp(output, original, sizeof(output)) == 0);
    assert(required > sizeof(output));

    write_u64_le(bytes + 2u * 8u, make_pack() | field(1u, 56u, 3u));
    assert(vita_usse_translate_hlsl(&primary, &secondary, &options, output,
                                    sizeof(output), &required, &failure) ==
           VITA_USSE_TRANSLATE_DECODE_ERROR);
    assert(failure.decode.instruction_index == 2u);
    assert(strcmp(failure.field, "predicate") == 0);
    assert(memcmp(output, original, sizeof(output)) == 0);

    make_synthetic_program(bytes);
    primary.instruction_count--;
    primary.byte_count -= 8u;
    assert(vita_usse_translate_hlsl(&primary, &secondary, &options, NULL, 0u,
                                    &required, &failure) ==
           VITA_USSE_TRANSLATE_INVALID_PROGRAM_ORDER);
    assert(strcmp(failure.field, "missing_emit") == 0);

    make_synthetic_program(bytes);
    primary.instruction_count = 6u;
    primary.byte_count = sizeof(bytes);
    write_u64_le(bytes + 4u * 8u, make_emit(1u));
    assert(vita_usse_translate_hlsl(&primary, &secondary, &options, NULL, 0u,
                                    &required, &failure) ==
           VITA_USSE_TRANSLATE_INVALID_PROGRAM_ORDER);
    assert(strcmp(failure.field, "instruction_after_emit") == 0);

    write_u64_le(secondary_bytes, make_nop(0u));
    secondary.bytes = secondary_bytes;
    secondary.byte_count = sizeof(secondary_bytes);
    secondary.instruction_count = 1u;
    assert(vita_usse_translate_hlsl(&primary, &secondary, &options, NULL, 0u,
                                    &required, &failure) ==
           VITA_USSE_TRANSLATE_UNSUPPORTED_SEMANTIC);
    assert(strcmp(failure.field, "secondary_program") == 0);
}

static int write_synthetic_hlsl(const char *mode_name, const char *path) {
    vita_usse_hlsl_mode mode;
    size_t source_size;
    char *source;
    FILE *file;
    size_t written;

    if (strcmp(mode_name, "compute") == 0)
        mode = VITA_USSE_HLSL_COMPUTE_CONFORMANCE;
    else if (strcmp(mode_name, "vertex") == 0)
        mode = VITA_USSE_HLSL_VERTEX;
    else
        return 2;
    source = translate_synthetic(mode, &source_size);
    file = fopen(path, "wb");
    if (!file) {
        free(source);
        return 3;
    }
    written = fwrite(source, 1u, source_size - 1u, file);
    if (fclose(file) != 0 || written != source_size - 1u) {
        free(source);
        return 4;
    }
    free(source);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--write-hlsl") == 0)
        return write_synthetic_hlsl(argv[2], argv[3]);
    assert(argc == 1);
    test_all_family_values_and_names();
    test_broad_family_precedence();
    test_i32_reserved_bit_holes();
    test_census_semantics_and_independent_firsts();
    test_all_legal();
    test_little_endian_census_ingestion();
    test_bounded_failure_preserves_output();
    test_phase_names();
    test_typed_decode_and_register_aliasing();
    test_strict_control_decode();
    test_deterministic_hlsl_generation();
    test_translation_failures_preserve_output();
    puts("USSE classifier, decode, and HLSL tests passed");
    return 0;
}
