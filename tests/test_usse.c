/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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
    {UINT64_C(0xB000000000000000), VITA_USSE_OPCODE_ILLEGAL22,
     "ILLEGAL22"},
    {UINT64_C(0xB800000000000000), VITA_USSE_OPCODE_ILLEGAL23,
     "ILLEGAL23"},
    {UINT64_C(0xC000000000000000), VITA_USSE_OPCODE_ILLEGAL24,
     "ILLEGAL24"},
    {UINT64_C(0xC800000000000000), VITA_USSE_OPCODE_I8MAD2, "I8MAD2"},
    {UINT64_C(0xD000000000000000), VITA_USSE_OPCODE_I32MAD2, "I32MAD2"},
    {UINT64_C(0xD800000000000000), VITA_USSE_OPCODE_ILLEGAL27,
     "ILLEGAL27"},
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

_Static_assert(
    sizeof(FAMILY_CASES) / sizeof(FAMILY_CASES[0]) ==
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
        assert(strcmp(
                   vita_usse_opcode_family_name(FAMILY_CASES[i].family),
                   FAMILY_CASES[i].name) == 0);
    }
    assert(strcmp(
               vita_usse_opcode_family_name(VITA_USSE_OPCODE_UNKNOWN),
               "UNKNOWN") == 0);
    assert(strcmp(
               vita_usse_opcode_family_name(VITA_USSE_OPCODE_FAMILY_COUNT),
               "INVALID") == 0);
}

static void test_broad_family_precedence(void) {
    assert_family(
        UINT64_C(0x4000000000000000), VITA_USSE_OPCODE_VPCK);
    assert_family(
        UINT64_C(0x4800000000000000), VITA_USSE_OPCODE_VTST);
    assert_family(
        UINT64_C(0x7800000000000000), VITA_USSE_OPCODE_VTSTMSK);
    assert_family(
        UINT64_C(0x5000000000000000), VITA_USSE_OPCODE_VBW);

    assert_family(
        UINT64_C(0xE000000000000000), VITA_USSE_OPCODE_SMP);
    assert_family(
        UINT64_C(0xFA40000000000000), VITA_USSE_OPCODE_PHAS);
    assert_family(
        UINT64_C(0xF800014000000000), VITA_USSE_OPCODE_NOP);
    assert_family(
        UINT64_C(0xF800000000000000), VITA_USSE_OPCODE_BR);
    assert_family(
        UINT64_C(0xFA10000000000000), VITA_USSE_OPCODE_SMLSI);
    assert_family(
        UINT64_C(0xFB10000000000000), VITA_USSE_OPCODE_SMBO);
    assert_family(
        UINT64_C(0xF9300006F0000000), VITA_USSE_OPCODE_KILL);
    assert_family(
        UINT64_C(0xFC20000000000000), VITA_USSE_OPCODE_LIMM);
    assert_family(
        UINT64_C(0xFB30000000000000), VITA_USSE_OPCODE_DEPTHF);
    /* The inactive legacy EMIT pattern deliberately falls through to SPEC. */
    assert_family(
        UINT64_C(0xFB20000000000000), VITA_USSE_OPCODE_SPEC);
    assert_family(
        UINT64_C(0xE800000000000000), VITA_USSE_OPCODE_VLDST);
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
    const vita_usse_stream_view primary = {
        primary_bytes, sizeof(primary_bytes), 2u
    };
    const vita_usse_stream_view secondary = {
        secondary_bytes, sizeof(secondary_bytes), 2u
    };
    vita_usse_census census;

    write_u64_le(
        primary_bytes, UINT64_C(0xA800000000000000) |
                           UINT64_C(0x0000800000000000));
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
    const vita_usse_stream_view primary = {
        primary_bytes, sizeof(primary_bytes), 1u
    };
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
    const uint8_t primary_bytes[8] = {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, UINT8_C(0x38)
    };
    const uint8_t secondary_bytes[8] = {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, UINT8_C(0xB0)
    };
    const vita_usse_stream_view primary = {
        primary_bytes, sizeof(primary_bytes), 1u
    };
    const vita_usse_stream_view secondary = {
        secondary_bytes, sizeof(secondary_bytes), 1u
    };
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
    const vita_usse_stream_view truncated_primary = {
        primary_bytes, 7u, 1u
    };
    const vita_usse_stream_view valid_primary = {
        primary_bytes, sizeof(primary_bytes), 1u
    };
    const vita_usse_stream_view truncated_secondary = {
        secondary_bytes, 7u, 1u
    };
    const vita_usse_stream_view empty = {NULL, 0u, 0u};
    vita_usse_census census;
    vita_usse_census original;

    memset(&census, 0xA5, sizeof(census));
    original = census;
    assert(vita_usse_census_program(
               &truncated_primary, &empty, &census) ==
           VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE);
    assert(memcmp(&census, &original, sizeof(census)) == 0);
    assert(vita_usse_census_program(
               &valid_primary, &truncated_secondary, &census) ==
           VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE);
    assert(memcmp(&census, &original, sizeof(census)) == 0);
    assert(vita_usse_census_program(NULL, &empty, &census) ==
           VITA_USSE_CENSUS_INVALID_ARGUMENT);
    assert(memcmp(&census, &original, sizeof(census)) == 0);
}

static void test_phase_names(void) {
    assert(strcmp(
               vita_usse_phase_name(VITA_USSE_PHASE_PRIMARY),
               "PRIMARY") == 0);
    assert(strcmp(
               vita_usse_phase_name(VITA_USSE_PHASE_SECONDARY),
               "SECONDARY") == 0);
    assert(strcmp(vita_usse_phase_name(VITA_USSE_PHASE_NONE), "NONE") == 0);
    assert(strcmp(
               vita_usse_phase_name((vita_usse_phase)UINT32_MAX),
               "INVALID") == 0);
}

int main(void) {
    test_all_family_values_and_names();
    test_broad_family_precedence();
    test_i32_reserved_bit_holes();
    test_census_semantics_and_independent_firsts();
    test_all_legal();
    test_little_endian_census_ingestion();
    test_bounded_failure_preserves_output();
    test_phase_names();
    puts("USSE classifier and census tests passed");
    return 0;
}
