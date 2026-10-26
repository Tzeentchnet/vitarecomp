/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/gxp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

static void write_u32_le(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1u] = (uint8_t)(value >> 8u);
    bytes[offset + 2u] = (uint8_t)(value >> 16u);
    bytes[offset + 3u] = (uint8_t)(value >> 24u);
}

static void make_valid_vertex_program(uint8_t *bytes, size_t size) {
    memset(bytes, 0, size);
    write_u32_le(bytes, 0u, UINT32_C(0x00505847));
    write_u32_le(bytes, 8u, (uint32_t)size);
    write_u32_le(bytes, 20u, 0u);
    write_u32_le(bytes, 60u, 2u);
    write_u32_le(bytes, 64u, 16u);
    write_u32_le(bytes, 68u, 1u);
    write_u32_le(bytes, 72u, 32u);
}

static void test_valid_program(void) {
    uint8_t bytes[128];
    vita_gxp_program_view program;
    make_valid_vertex_program(bytes, sizeof(bytes));

    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_OK);
    assert(program.bytes == bytes);
    assert(program.declared_size == sizeof(bytes));
    assert(program.flags == 0u);
    assert(program.stage == VITA_GXP_STAGE_VERTEX);
    assert(program.primary.bytes == bytes + 80u);
    assert(program.primary.byte_count == 16u);
    assert(program.primary.instruction_count == 2u);
    assert(program.secondary.bytes == bytes + 104u);
    assert(program.secondary.byte_count == 8u);
    assert(program.secondary.instruction_count == 1u);
}

static void test_truncated_and_declared_sizes(void) {
    uint8_t bytes[128];
    vita_gxp_program_view program;
    make_valid_vertex_program(bytes, sizeof(bytes));

    write_u32_le(bytes, 0u, UINT32_C(0x00505846));
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_BAD_MAGIC);
    write_u32_le(bytes, 0u, UINT32_C(0x00505847));
    assert(vita_gxp_parse(
               bytes, 75u, VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_TRUNCATED);
    write_u32_le(bytes, 8u, 75u);
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_BAD_DECLARED_SIZE);
    write_u32_le(bytes, 8u, 129u);
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_BAD_DECLARED_SIZE);
}

static void test_count_and_relative_bounds(void) {
    uint8_t bytes[128];
    vita_gxp_program_view program;
    make_valid_vertex_program(bytes, sizeof(bytes));

    write_u32_le(bytes, 60u, UINT32_C(0x20000000));
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_INSTRUCTION_COUNT_OVERFLOW);

    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 64u, UINT32_C(0x7FFFFFFF));
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE);

    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 64u, UINT32_C(0x80000000));
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE);

    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 64u, 56u);
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE);
}

static void test_stage_requirement(void) {
    uint8_t bytes[128];
    vita_gxp_program_view program;
    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 20u, 1u);

    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_WRONG_STAGE);
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_FRAGMENT, &program) ==
           VITA_GXP_PARSE_OK);
    assert(program.stage == VITA_GXP_STAGE_FRAGMENT);
    assert(vita_gxp_parse(
               bytes, sizeof(bytes), VITA_GXP_STAGE_ANY, &program) ==
           VITA_GXP_PARSE_OK);
}

int main(void) {
    test_valid_program();
    test_truncated_and_declared_sizes();
    test_count_and_relative_bounds();
    test_stage_requirement();
    puts("bounded GXP parser tests passed");
    return 0;
}
