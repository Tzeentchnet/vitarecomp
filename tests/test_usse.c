/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

static void write_u64_le(uint8_t *bytes, uint64_t value) {
    for (uint32_t i = 0; i < 8u; i++)
        bytes[i] = (uint8_t)(value >> (i * 8u));
}

static void test_recognized_and_unknown_families(void) {
    uint8_t primary_bytes[16];
    uint8_t secondary_bytes[8];
    const vita_usse_stream_view primary = {
        primary_bytes, sizeof(primary_bytes), 2u
    };
    const vita_usse_stream_view secondary = {
        secondary_bytes, sizeof(secondary_bytes), 1u
    };
    vita_usse_census census;

    write_u64_le(primary_bytes, UINT64_C(0x07) << 59u);
    write_u64_le(primary_bytes + 8u, UINT64_C(0x01) << 59u);
    write_u64_le(
        secondary_bytes,
        (UINT64_C(0x1F) << 59u) | (UINT64_C(0x04) << 52u));

    assert(vita_usse_census_program(&primary, &secondary, &census) ==
           VITA_USSE_CENSUS_OK);
    assert(census.total == 3u);
    assert(census.decoded == 2u);
    assert(census.unsupported == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_UNKNOWN] == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_VMOV] == 1u);
    assert(census.family_counts[VITA_USSE_OPCODE_PHAS] == 1u);
    assert(census.first_unsupported_phase == VITA_USSE_PHASE_PRIMARY);
    assert(census.first_unsupported_index == 1u);
    assert(census.first_unsupported_family == VITA_USSE_OPCODE_UNKNOWN);
}

static void test_all_recognized(void) {
    uint8_t primary_bytes[8];
    uint8_t secondary_bytes[8];
    const vita_usse_stream_view primary = {
        primary_bytes, sizeof(primary_bytes), 1u
    };
    const vita_usse_stream_view secondary = {
        secondary_bytes, sizeof(secondary_bytes), 1u
    };
    vita_usse_census census;

    write_u64_le(primary_bytes, UINT64_C(0x07) << 59u);
    write_u64_le(
        secondary_bytes,
        (UINT64_C(0x1F) << 59u) | (UINT64_C(0x04) << 52u));
    assert(vita_usse_census_program(&primary, &secondary, &census) ==
           VITA_USSE_CENSUS_OK);
    assert(census.total == 2u);
    assert(census.decoded == 2u);
    assert(census.unsupported == 0u);
    assert(census.first_unsupported_phase == VITA_USSE_PHASE_NONE);
    assert(census.first_unsupported_index == UINT32_MAX);
}

static void test_secondary_first_unsupported(void) {
    uint8_t primary_bytes[8];
    uint8_t secondary_bytes[16];
    const vita_usse_stream_view primary = {
        primary_bytes, sizeof(primary_bytes), 1u
    };
    const vita_usse_stream_view secondary = {
        secondary_bytes, sizeof(secondary_bytes), 2u
    };
    vita_usse_census census;

    write_u64_le(primary_bytes, UINT64_C(0x07) << 59u);
    write_u64_le(
        secondary_bytes,
        (UINT64_C(0x1F) << 59u) | (UINT64_C(0x04) << 52u));
    write_u64_le(secondary_bytes + 8u, UINT64_C(0x02) << 59u);
    assert(vita_usse_census_program(&primary, &secondary, &census) ==
           VITA_USSE_CENSUS_OK);
    assert(census.first_unsupported_phase == VITA_USSE_PHASE_SECONDARY);
    assert(census.first_unsupported_index == 1u);
    assert(census.first_unsupported_family == VITA_USSE_OPCODE_UNKNOWN);
}

static void test_bounded_fetch(void) {
    uint8_t bytes[8] = {0};
    const vita_usse_stream_view truncated = {bytes, 7u, 1u};
    const vita_usse_stream_view empty = {NULL, 0u, 0u};
    vita_usse_census census;
    memset(&census, 0xA5, sizeof(census));

    assert(vita_usse_census_program(&truncated, &empty, &census) ==
           VITA_USSE_CENSUS_STREAM_OUT_OF_RANGE);
    assert(census.total == UINT64_C(0xA5A5A5A5A5A5A5A5));
    assert(vita_usse_census_program(NULL, &empty, &census) ==
           VITA_USSE_CENSUS_INVALID_ARGUMENT);
}

static void test_static_names(void) {
    assert(strcmp(
               vita_usse_opcode_family_name(VITA_USSE_OPCODE_UNKNOWN),
               "UNKNOWN") == 0);
    assert(strcmp(
               vita_usse_opcode_family_name(VITA_USSE_OPCODE_VMOV),
               "VMOV") == 0);
    assert(strcmp(
               vita_usse_opcode_family_name(VITA_USSE_OPCODE_PHAS),
               "PHAS") == 0);
    assert(strcmp(
               vita_usse_phase_name(VITA_USSE_PHASE_SECONDARY),
               "SECONDARY") == 0);
}

int main(void) {
    test_recognized_and_unknown_families();
    test_all_recognized();
    test_secondary_first_unsupported();
    test_bounded_fetch();
    test_static_names();
    puts("USSE census tests passed");
    return 0;
}
